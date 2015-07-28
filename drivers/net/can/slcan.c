/*
 * slcan.c - serial line CAN interface driver (using tty line discipline)
 *
 * This file is derived from linux/drivers/net/slip/slip.c
 *
 * slip.c Authors  : Laurence Culhane <loz@holmes.demon.co.uk>
 *                   Fred N. van Kempen <waltje@uwalt.nl.mugnet.org>
 * slcan.c Author  : Oliver Hartkopp <socketcan@hartkopp.net>
 *
 * SLCAN channel muxing (XSLCAN) is Copyright (C) 2015 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307. You can also get it
 * at http://www.gnu.org/licenses/gpl.html
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/uaccess.h>
#include <linux/bitops.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/can.h>

static __initconst const char banner[] =
	KERN_INFO "slcan: serial line CAN interface driver\n";

MODULE_ALIAS_LDISC(N_SLCAN);
MODULE_DESCRIPTION("serial line CAN interface");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Oliver Hartkopp <socketcan@hartkopp.net>");

#define SLCAN_MAGIC 0x53CA
#define MUX_NETDEV_MAX 10

static int maxchannel = 10;	/* MAX number of SLCAN channels;
				   This can be overridden with
				   insmod slcan.ko maxcahnnel=nnn   */
module_param(maxchannel, int, 0);
MODULE_PARM_DESC(maxchannel, "Maximum number of slcan channels");

static int muxnetdevs = 2;	/*	MAX number of net devices multiplexed
					per SLCAN channel;
					This can be overridden with
					insmod slcan.ko muxnetdevs=nnn   */
module_param(muxnetdevs, int, 0);
MODULE_PARM_DESC(muxnetdevs, "Max number of netdevs muxed per slcan channel");

/* maximum rx buffer len: extended CAN frame with timestamp */
#define SLC_MTU (sizeof("0T1111222281122334455667788EA5F\r")+1)

struct slcan_channel {
	int			magic;

	/* Various fields. */
	struct tty_struct	*tty;		/* ptr to TTY structure	     */
	struct net_device	*dev[MUX_NETDEV_MAX];
	/* easy for intr handling    */
	spinlock_t		lock;

	/* These are pointers to the malloc()ed frame buffers. */
	unsigned char		rbuff[SLC_MTU];	/* receiver buffer	     */
	int			rcount;         /* received chars counter    */

	unsigned long		flags;		/* Flag values/ mode etc     */
#define SLF_INUSE		0		/* Channel in use            */
#define SLF_ERROR		1               /* Parity, etc. error        */
};

struct slcan_dev {
	struct slcan_channel	*channel;	/* parent slcan channel      */
	int			addr;

	/* These are pointers to the malloc()ed frame buffers. */
	unsigned char		xbuff[SLC_MTU];	/* transmitter buffer	     */
	unsigned char		*xhead;         /* pointer to next XMIT byte */
	int			xleft;          /* bytes left in XMIT queue  */
};

static struct slcan_channel **slcan_channels;

/************************************************************************
 *			SLCAN ENCAPSULATION FORMAT			 *
 ************************************************************************/

/*
 * A CAN frame has a can_id (11 bit standard frame format OR 29 bit extended
 * frame format) a data length code (can_dlc) which can be from 0 to 8
 * and up to <can_dlc> data bytes as payload.
 * Additionally a CAN frame may become a remote transmission frame if the
 * RTR-bit is set. This causes another ECU to send a CAN frame with the
 * given can_id.
 *
 * The SLCAN ASCII representation of these different frame types is:
 * <type> <id> <dlc> <data>*
 *
 * Extended frames (29 bit) are defined by capital characters in the type.
 * RTR frames are defined as 'r' types - normal frames have 't' type:
 * t => 11 bit data frame
 * r => 11 bit RTR frame
 * T => 29 bit data frame
 * R => 29 bit RTR frame
 *
 * The <id> is 3 (standard) or 8 (extended) bytes in ASCII Hex (base64).
 * The <dlc> is a one byte ASCII number ('0' - '8')
 * The <data> section has at much ASCII Hex bytes as defined by the <dlc>
 *
 * Examples:
 *
 * t1230 : can_id 0x123, can_dlc 0, no data
 * t4563112233 : can_id 0x456, can_dlc 3, data 0x11 0x22 0x33
 * T12ABCDEF2AA55 : extended can_id 0x12ABCDEF, can_dlc 2, data 0xAA 0x55
 * r1230 : can_id 0x123, can_dlc 0, no data, remote transmission request
 *
 */

/************************************************************************
 *			STANDARD SLCAN DECAPSULATION			 *
 ************************************************************************/

/* Send one completely decapsulated can_frame to the network layer */
static void slc_bump(struct slcan_channel *sl)
{
	struct sk_buff *skb;
	struct can_frame cf;
	int i, dlc_pos, tmp;
	unsigned long ultmp;
	int ext_frame, dev_idx;

	char cmd;

	ext_frame = (sl->rbuff[0] >= '0' && sl->rbuff[0] <= '9') ? 1 : 0;

	cmd = sl->rbuff[ext_frame];

	if ((cmd != 't') && (cmd != 'T') && (cmd != 'r') && (cmd != 'R'))
		return;

	if (cmd & 0x20) /* tiny chars 'r' 't' => standard frame format */
		dlc_pos = 4 + ext_frame; /* dlc position tiiid */
	else
		dlc_pos = 9 + ext_frame; /* dlc position Tiiiiiiiid */

	if (!((sl->rbuff[dlc_pos] >= '0') && (sl->rbuff[dlc_pos] < '9')))
		return;

	cf.can_dlc = sl->rbuff[dlc_pos] - '0'; /* get can_dlc from ASCII val */

	sl->rbuff[dlc_pos] = 0; /* terminate can_id string */

	if (strict_strtoul(sl->rbuff + 1 + ext_frame, 16, &ultmp))
		return;

	cf.can_id = ultmp;

	if (!(cmd & 0x20)) /* NO tiny chars => extended frame format */
		cf.can_id |= CAN_EFF_FLAG;

	if ((cmd | 0x20) == 'r') /* RTR frame */
		cf.can_id |= CAN_RTR_FLAG;

	*(u64 *) (&cf.data) = 0; /* clear payload */

	for (i = 0, dlc_pos++; i < cf.can_dlc; i++) {
		tmp = hex_to_bin(sl->rbuff[dlc_pos++]);
		if (tmp < 0)
			return;
		cf.data[i] = (tmp << 4);
		tmp = hex_to_bin(sl->rbuff[dlc_pos++]);
		if (tmp < 0)
			return;
		cf.data[i] |= tmp;
	}

	skb = dev_alloc_skb(sizeof(struct can_frame));
	if (!skb)
		return;

	dev_idx = ext_frame ? sl->rbuff[0] - '0' : 0;

	if (sl->dev[dev_idx] == NULL)
		return;

	skb->dev = sl->dev[dev_idx];
	skb->protocol = htons(ETH_P_CAN);
	skb->pkt_type = PACKET_BROADCAST;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	memcpy(skb_put(skb, sizeof(struct can_frame)),
	       &cf, sizeof(struct can_frame));
	netif_rx_ni(skb);

	sl->dev[dev_idx]->stats.rx_packets++;
	sl->dev[dev_idx]->stats.rx_bytes += cf.can_dlc;
}

/* parse tty input stream */
static void slcan_unesc(struct slcan_channel *sl, unsigned char s)
{

	if ((s == '\r') || (s == '\a')) { /* CR or BEL ends the pdu */
		if (!test_and_clear_bit(SLF_ERROR, &sl->flags) &&
		    (sl->rcount > 4))
			slc_bump(sl);
		sl->rcount = 0;
	} else {
		if (!test_bit(SLF_ERROR, &sl->flags))  {
			if (sl->rcount < SLC_MTU)  {
				sl->rbuff[sl->rcount++] = s;
				return;
			} else {
				sl->dev[0]->stats.rx_over_errors++;
				set_bit(SLF_ERROR, &sl->flags);
			}
		}
	}
}

/************************************************************************
 *			STANDARD SLCAN ENCAPSULATION			 *
 ************************************************************************/

/* Encapsulate one can_frame and stuff into a TTY queue. */
static void slc_encaps(struct slcan_dev *sl_dev, struct can_frame *cf,
		       int dev_idx)
{
	int actual, idx, i;
	char cmd, if_idx;

	if (cf->can_id & CAN_RTR_FLAG)
		cmd = 'R'; /* becomes 'r' in standard frame format */
	else
		cmd = 'T'; /* becomes 't' in standard frame format */

	if (muxnetdevs < 2) {
		if (cf->can_id & CAN_EFF_FLAG)
			sprintf(sl_dev->xbuff, "%c%08X%d", cmd,
				cf->can_id & CAN_EFF_MASK, cf->can_dlc);
		else
			sprintf(sl_dev->xbuff, "%c%03X%d", cmd | 0x20,
				cf->can_id & CAN_SFF_MASK, cf->can_dlc);
	} else {
		if_idx = dev_idx + '0';
		if (cf->can_id & CAN_EFF_FLAG)
			sprintf(sl_dev->xbuff, "%c%c%08X%d", if_idx, cmd,
				cf->can_id & CAN_EFF_MASK, cf->can_dlc);
		else
			sprintf(sl_dev->xbuff, "%c%c%03X%d", if_idx,
				cmd | 0x20,
				cf->can_id & CAN_SFF_MASK, cf->can_dlc);
	}

	idx = strlen(sl_dev->xbuff);

	for (i = 0; i < cf->can_dlc; i++)
		sprintf(&sl_dev->xbuff[idx + 2 * i], "%02X", cf->data[i]);

	strcat(sl_dev->xbuff, "\r"); /* add terminating character */

	/* Order of next two lines is *very* important.
	 * When we are sending a little amount of data,
	 * the transfer may be completed inside the ops->write()
	 * routine, because it's running with interrupts enabled.
	 * In this case we *never* got WRITE_WAKEUP event,
	 * if we did not request it before write operation.
	 *       14 Oct 1994  Dmitry Gorodchanin.
	 */
	set_bit(TTY_DO_WRITE_WAKEUP, &sl_dev->channel->tty->flags);
	actual = sl_dev->channel->tty->ops->write(sl_dev->channel->tty,
			sl_dev->xbuff,
			strlen(sl_dev->xbuff));

	sl_dev->xleft = strlen(sl_dev->xbuff) - actual;
	sl_dev->xhead = sl_dev->xbuff + actual;
	sl_dev->channel->dev[dev_idx]->stats.tx_bytes += cf->can_dlc;

}

/*
 * Called by the driver when there's room for more data.  If we have
 * more packets to send, we send them here.
 */
static void slcan_write_wakeup(struct tty_struct *tty)
{
	int actual, i;
	struct slcan_channel *sl = (struct slcan_channel *) tty->disc_data;

	struct slcan_dev *sl_dev;

	/* First make sure we're connected. */
	if (!sl || sl->magic != SLCAN_MAGIC)
		return;

	for (i = 0; i < muxnetdevs; i++) {

		if (!netif_running(sl->dev[i]))
			continue;

		sl_dev = netdev_priv(sl->dev[i]);

		if (sl_dev->xleft <= 0)  {
			/* Now serial buffer is almost free & we can start
			 * transmission of another packet */
			sl->dev[i]->stats.tx_packets++;
			clear_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
			netif_wake_queue(sl->dev[i]);
			continue;
		}

		actual = tty->ops->write(tty, sl_dev->xhead, sl_dev->xleft);

		sl_dev->xleft -= actual;
		sl_dev->xhead += actual;
	}
}

/* Send a can_frame to a TTY queue. */
static netdev_tx_t slc_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct slcan_dev *sl_dev = netdev_priv(dev);

	struct slcan_channel *sl = sl_dev->channel;

	if (skb->len != sizeof(struct can_frame))
		goto out;

	spin_lock(&sl->lock);
	if (!netif_running(dev))  {
		spin_unlock(&sl->lock);
		printk(KERN_WARNING "%s: xmit: iface is down\n", dev->name);
		goto out;
	}
	if (sl->tty == NULL) {
		spin_unlock(&sl->lock);
		goto out;
	}

	netif_stop_queue(sl->dev[sl_dev->addr]);
	slc_encaps(sl_dev, (struct can_frame *) skb->data,
		   sl_dev->addr); /* encaps & send */
	spin_unlock(&sl->lock);

out:
	kfree_skb(skb);
	return NETDEV_TX_OK;
}


/******************************************
 *   Routines looking at netdevice side.
 ******************************************/

/* Netdevice UP -> DOWN routine */
static int slc_close(struct net_device *dev)
{
	struct slcan_dev *sl_dev = netdev_priv(dev);

	struct slcan_channel *sl = sl_dev->channel;

	spin_lock_bh(&sl->lock);
	if (sl->tty) {
		/* TTY discipline is running. */
		clear_bit(TTY_DO_WRITE_WAKEUP, &sl->tty->flags);
	}
	netif_stop_queue(dev);
	sl->rcount   = 0;
	sl_dev->xleft    = 0;
	spin_unlock_bh(&sl->lock);

	return 0;
}

/* Netdevice DOWN -> UP routine */
static int slc_open(struct net_device *dev)
{
	struct slcan_dev *sl_dev = netdev_priv(dev);

	struct slcan_channel *sl = sl_dev->channel;

	if (sl->tty == NULL)
		return -ENODEV;

	sl->flags &= (1 << SLF_INUSE);
	netif_start_queue(dev);
	return 0;
}

/* Hook the destructor so we can free slcan devs at the right point in time */
static void slc_free_netdev(struct net_device *dev)
{
	int dev_idx, devs_in_use, i;

	struct slcan_dev *sl_dev = netdev_priv(dev);

	struct slcan_channel *sl = sl_dev->channel;

	dev_idx = sl_dev->addr;

	free_netdev(dev);

	sl->dev[dev_idx] = NULL;

	devs_in_use = 0;

	for (i = 0; i < muxnetdevs; i++) {
		if (sl->dev[i] != 0)
			devs_in_use++;
	}

	/* Free slcan_channel when not referencing any netdev. */
	if (devs_in_use == 0) {
		for (i = 0; i < maxchannel; i++) {
			if (sl == slcan_channels[i])
				slcan_channels[i] = NULL;
		}
		kfree(sl);
	}
}

static const struct net_device_ops slc_netdev_ops = {
	.ndo_open               = slc_open,
	.ndo_stop               = slc_close,
	.ndo_start_xmit         = slc_xmit,
};

static void slc_setup(struct net_device *dev)
{
	dev->netdev_ops		= &slc_netdev_ops;
	dev->destructor		= slc_free_netdev;

	dev->hard_header_len	= 0;
	dev->addr_len		= 0;
	dev->tx_queue_len	= 10;

	dev->mtu		= sizeof(struct can_frame);
	dev->type		= ARPHRD_CAN;

	/* New-style flags. */
	dev->flags		= IFF_NOARP;
	dev->features           = NETIF_F_HW_CSUM;
}

/******************************************
  Routines looking at TTY side.
 ******************************************/

/*
 * Handle the 'receiver data ready' interrupt.
 * This function is called by the 'tty_io' module in the kernel when
 * a block of SLCAN data has been received, which can now be decapsulated
 * and sent on to some IP layer for further processing. This will not
 * be re-entered while running but other ldisc functions may be called
 * in parallel
 */

static void slcan_receive_buf(struct tty_struct *tty,
			      const unsigned char *cp, char *fp, int count)
{
	struct slcan_channel *sl = (struct slcan_channel *) tty->disc_data;

	if (!sl || sl->magic != SLCAN_MAGIC)
		return;

	if (!netif_running(sl->dev[0]))
		return;

	/* Read the characters out of the buffer */
	while (count--) {
		if (fp && *fp++) {
			if (!test_and_set_bit(SLF_ERROR, &sl->flags))
				sl->dev[0]->stats.rx_errors++;
			cp++;
			continue;
		}
		slcan_unesc(sl, *cp++);
	}
}

/************************************
 *  slcan_open helper routines.
 ************************************/

/* Collect hanged up channels */
static void slc_sync(void)
{
	int i, j;
	struct slcan_channel *sl;

	for (i = 0; i < maxchannel; i++) {
		sl = slcan_channels[i];
		if (sl == NULL)
			break;

		if (sl->tty)
			continue;

		for (j = 0; j < muxnetdevs; j++) {

			if (sl->dev[i] == NULL)
				continue;

			if (sl->dev[i]->flags & IFF_UP)
				dev_close(sl->dev[i]);
		}
	}
}

/* Find a free SLCAN channel, and link in this `tty' line. */
static struct slcan_channel *slc_alloc(dev_t line)
{
	int i, j;
	char name[IFNAMSIZ];
	struct net_device *dev = NULL;
	struct slcan_channel *sl;
	struct slcan_dev *sl_dev;

	for (i = 0; i < maxchannel; i++) {
		sl = slcan_channels[i];
		if (sl == NULL)
			break;

	}

	/* Sorry, too many, all slots in use */
	if (i >= maxchannel)
		return NULL;

	sl = kzalloc(sizeof(struct slcan_channel), GFP_KERNEL);

	if (!sl)
		return NULL;

	for (j = 0; j < MUX_NETDEV_MAX; j++)
		sl->dev[j] = NULL;

	for (j = 0; j < muxnetdevs; j++) {
		sprintf(name, "slcan%d", i * muxnetdevs + j);

		dev = alloc_netdev(sizeof(*sl_dev), name, slc_setup);
		if (!dev) {
			kfree(sl);
			return NULL;
		}

		dev->base_addr  = i * muxnetdevs + j;
		sl_dev = netdev_priv(dev);

		sl_dev->channel = sl;
		sl_dev->addr = j;
		sl_dev->xleft = 0;

		sl->dev[j] = dev;
	}

	/* Initialize channel control data */
	sl->magic = SLCAN_MAGIC;

	spin_lock_init(&sl->lock);
	slcan_channels[i] = sl;

	return sl;
}

/*
 * Open the high-level part of the SLCAN channel.
 * This function is called by the TTY module when the
 * SLCAN line discipline is called for.  Because we are
 * sure the tty line exists, we only have to link it to
 * a free SLCAN channel...
 *
 * Called in process context serialized from other ldisc calls.
 */

static int slcan_open(struct tty_struct *tty)
{
	struct slcan_channel *sl;
	int err, i;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (tty->ops->write == NULL)
		return -EOPNOTSUPP;

	/* RTnetlink lock is misused here to serialize concurrent
	   opens of slcan channels. There are better ways, but it is
	   the simplest one.
	 */
	rtnl_lock();

	/* Collect hanged up channels. */
	slc_sync();

	sl = tty->disc_data;

	err = -EEXIST;
	/* First make sure we're not already connected. */
	if (sl && sl->magic == SLCAN_MAGIC)
		goto err_exit;


	/* OK.  Find a free SLCAN channel to use. */
	err = -ENFILE;
	sl = slc_alloc(tty_devnum(tty));
	if (sl == NULL)
		goto err_exit;

	sl->tty = tty;
	tty->disc_data = sl;

	if (!test_bit(SLF_INUSE, &sl->flags)) {
		/* Perform the low-level SLCAN initialization. */
		sl->rcount   = 0;

		set_bit(SLF_INUSE, &sl->flags);

		for (i = 0; i < muxnetdevs; i++) {

			err = register_netdevice(sl->dev[i]);
			if (err)
				goto err_free_chan;
		}
	}

	/* Done.  We have linked the TTY line to a channel. */
	rtnl_unlock();
	tty->receive_room = 65536;	/* We don't flow control */

	/* TTY layer expects 0 on success */
	return 0;

err_free_chan:
	sl->tty = NULL;
	tty->disc_data = NULL;
	clear_bit(SLF_INUSE, &sl->flags);

err_exit:
	rtnl_unlock();

	/* Count references from TTY module */
	return err;
}

/*
 * Close down a SLCAN channel.
 * This means flushing out any pending queues, and then returning. This
 * call is serialized against other ldisc functions.
 *
 * We also use this method for a hangup event.
 */

static void slcan_close(struct tty_struct *tty)
{
	int i;

	struct slcan_channel *sl = (struct slcan_channel *) tty->disc_data;

	/* First make sure we're connected. */
	if (!sl || sl->magic != SLCAN_MAGIC || sl->tty != tty)
		return;

	tty->disc_data = NULL;
	sl->tty = NULL;

	/* Flush network side */
	for (i = 0; i < muxnetdevs; i++)
		unregister_netdev(sl->dev[i]);
	/* This will complete via sl_free_netdev */
}

static int slcan_hangup(struct tty_struct *tty)
{
	slcan_close(tty);
	return 0;
}

/* Perform I/O control on an active SLCAN channel. */
static int slcan_ioctl(struct tty_struct *tty, struct file *file,
		       unsigned int cmd, unsigned long arg)
{
	struct slcan_channel *sl = (struct slcan_channel *) tty->disc_data;
	unsigned int tmp;

	/* First make sure we're connected. */
	if (!sl || sl->magic != SLCAN_MAGIC)
		return -EINVAL;

	switch (cmd) {
	case SIOCGIFNAME:
		tmp = strlen(sl->dev[0]->name) + 1;
		if (copy_to_user((void __user *)arg, sl->dev[0]->name, tmp))
			return -EFAULT;
		return 0;

	case SIOCSIFHWADDR:
		return -EINVAL;

	default:
		return tty_mode_ioctl(tty, file, cmd, arg);
	}
}

static struct tty_ldisc_ops slc_ldisc = {
	.owner		= THIS_MODULE,
	.magic		= TTY_LDISC_MAGIC,
	.name		= "slcan",
	.open		= slcan_open,
	.close		= slcan_close,
	.hangup		= slcan_hangup,
	.ioctl		= slcan_ioctl,
	.receive_buf	= slcan_receive_buf,
	.write_wakeup	= slcan_write_wakeup,
};

static int __init slcan_init(void)
{
	int status;

	if (maxchannel < 4)
		maxchannel = 4; /* Sanity */

	if (muxnetdevs < 1)
		muxnetdevs = 1;

	if (muxnetdevs > MUX_NETDEV_MAX)
		muxnetdevs = MUX_NETDEV_MAX;

	printk(banner);
	printk(KERN_INFO "slcan: %d dynamic interface channels.\n", maxchannel);

	if (muxnetdevs > 1)
		printk(KERN_INFO "slcan: xslcan multiplexer enabled ratio %d:1.\n",
		       muxnetdevs);

	slcan_channels = kzalloc(sizeof(struct slcan_channel *)*maxchannel,
					GFP_KERNEL);
	if (!slcan_channels)
		return -ENOMEM;

	/* Fill in our line protocol discipline, and register it */
	status = tty_register_ldisc(N_SLCAN, &slc_ldisc);
	if (status)  {
		printk(KERN_ERR "slcan: can't register line discipline\n");
		kfree(slcan_channels);
	}
	return status;
}

static void __exit slcan_exit(void)
{
	int i;
	struct slcan_channel *sl;
	unsigned long timeout = jiffies + HZ;
	int busy = 0;

	if (slcan_channels == NULL)
		return;

	/* First of all: check for active disciplines and hangup them.
	 */
	do {
		if (busy)
			msleep_interruptible(100);

		busy = 0;
		for (i = 0; i < maxchannel; i++) {

			sl = slcan_channels[i];
			if (!sl)
				continue;
			spin_lock_bh(&sl->lock);
			if (sl->tty) {
				busy++;
				tty_hangup(sl->tty);
			}
			spin_unlock_bh(&sl->lock);
		}
	} while (busy && time_before(jiffies, timeout));

	/* FIXME: hangup is async so we should wait when doing this second
	   phase */

	for (i = 0; i < maxchannel; i++) {
		sl = slcan_channels[i];
		if (!sl)
			continue;
		slcan_channels[i] = NULL;

		if (sl->tty) {
			printk(KERN_ERR "%s: tty discipline still running\n",
			       sl->dev[i]->name);
			/* Intentionally leak the control block. */
			sl->dev[i]->destructor = NULL;
		}

		if (sl->dev[i] == NULL)
			continue;

		for (i = 0; i < muxnetdevs; i++)
			unregister_netdev(sl->dev[i]);
	}

	kfree(slcan_channels);
	slcan_channels = NULL;

	i = tty_unregister_ldisc(N_SLCAN);
	if (i)
		printk(KERN_ERR "slcan: can't unregister ldisc (err %d)\n", i);
}

module_init(slcan_init);
module_exit(slcan_exit);
