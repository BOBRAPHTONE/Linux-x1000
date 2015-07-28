/*
 * max78m6610+lmu SPI protocol driver
 *
 * Copyright(c) 2013-2015 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * This SPI protocol driver is developed for the Maxim 78M6610+LMU (eADC).
 * The driver is developed as a part of the Quark BSP where integrated into
 * Quark Evaluation Boards Cross Hill Industrial-E.
 *
 * The Maxim 78M6610+LMU is an energy measurement processor (EMP) for
 * load monitoring on single or spilt-phase AC loads. It supports varies
 * interface configuration protocols through I/O pins.
 *
 * With 3 wire serial input/output interfaces provided by Quark SoC,
 * the 78M6610+LMU can be connected directly as SPI slave device.
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/types.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/events.h>
#include <linux/platform_data/max78m6610_lmu.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/version.h>

/* Calibration registers */
#define COMMAND         0x00 /* Command Register */
#define SAMPLES         0x03 /* High-rate samples per low-rate */
#define CALCYCS         0x04 /* Number of Calibration Cycles to Average */
#define PHASECOMP1      0x05 /* Phase compensation for S1 input */
#define PHASECOMP3      0x06 /* Phase compensation for S3 input */
#define S1_GAIN         0x07 /* Input S1 Gain Calibration */
#define S0_GAIN         0x08 /* Input S0 Gain Calibration */
#define S3_GAIN         0x09 /* Input S3 Gain Calibration */
#define S2_GAIN         0x0A /* Input S2 Gain Calibration */
#define S1_OFFSET       0x0D /* Input S1 Offset Calibration */
#define S0_OFFSET       0x0B /* Input S0 Offset Calibration */
#define S3_OFFSET       0x0E /* Input S3 Offset Calibration */
#define S2_OFFSET       0x0C /* Input S2 Offset Calibration */
#define VTARGET         0x12 /* Voltage Calibration Target */
#define ITARGET         0x39 /* Current Calibration Target */

#define CALCMD_S0_GAIN  0xCA2030 /* Calibrate Voltage Gain for Input S0 */
#define CALCMD_S1_GAIN  0xCA0830 /* Calibrate Current Gain for Input S1 */
#define CALCMD_S2_GAIN  0xCA4030 /* Calibrate Voltage Gain for Input S2 */
#define CALCMD_S3_GAIN  0xCA1030 /* Calibrate Current Gain for Input S3 */
#define CALCMD_S0_OFFS  0xCA2210 /* Calibrate Voltage Offset for Input S0 */
#define CALCMD_S1_OFFS  0xCA0A10 /* Calibrate Current Offset for Input S1 */
#define CALCMD_S2_OFFS  0xCA4210 /* Calibrate Voltage Offset for Input S2 */
#define CALCMD_S3_OFFS  0xCA1210 /* Calibrate Current Offset for Input S3 */
#define FLASHSAVE_CMD   0xACC210 /* Save calibration coefficients to flash */

/* Interrupt status registers */
#define MASK0           0x02 /* Status bit mask for MP0 pin */
#define STATUS          0x0F /* Status of device and alarms */
#define STATUS_RESET    0x11 /* Used to Reset Status bits */
#define STATUS_MASK_DRDY     (1 << 23)
#define STATUS_MASK_MMUPD    (1 << 22)
#define STATUS_MASK_VA_SAG   (1 << 21)
#define STATUS_MASK_VB_SAG   (1 << 20)
#define STATUS_MASK_SIGN_VA  (1 << 19)
#define STATUS_MASK_SIGN_VB  (1 << 18)
#define STATUS_MASK_OV_TEMP  (1 << 17)
#define STATUS_MASK_UN_TEMP  (1 << 16)
#define STATUS_MASK_OV_FREQ  (1 << 15)
#define STATUS_MASK_UN_FREQ  (1 << 14)
#define STATUS_MASK_OV_VRMSA (1 << 13)
#define STATUS_MASK_UN_VRMSA (1 << 12)
#define STATUS_MASK_OV_VRMSB (1 << 11)
#define STATUS_MASK_UN_VRMSB (1 << 10)
#define STATUS_MASK_VA_SURGE (1 << 9)
#define STATUS_MASK_VB_SURGE (1 << 8)
#define STATUS_MASK_OV_WATT1 (1 << 7)
#define STATUS_MASK_OV_WATT2 (1 << 6)
#define STATUS_MASK_OV_AMP1  (1 << 5)
#define STATUS_MASK_OV_AMP2  (1 << 4)
#define STATUS_MASK_XSTATE   (1 << 3)
#define STATUS_MASK_RELAY1   (1 << 2)
#define STATUS_MASK_RELAY2   (1 << 1)
#define STATUS_MASK_RESET    (1)
#define STATUS_MASK_STICKY   (0x73FFF1)
#define STATUS_MASK_IGNORE   (0x00000E)

#define VSURG_VAL	0x13 /* Voltage surge alarm threshold */
#define VSAG_VAL	0x14 /* Voltage sag alarm threshold */
#define VRMS_MIN	0x15 /* Voltage lower alarm limit */
#define VRMS_MAX	0x16 /* Voltage upper alarm limit */
#define IRMS_MAX	0x27 /* Over Current alarm limit */
#define WATT_MAX	0x32 /* Power alarm limit */

#define INSTAN_VA       0x1D /* instaneous Voltage for VA source */
#define INSTAN_IA       0x25 /* instaneous Current for IA source */
#define INSTAN_PA       0x2E /* instaneous Active Power for source A*/
#define INSTAN_PQA      0x30 /* instaneous Reactive Power for source A*/
#define VA_RMS          0x17 /* RMS voltage for VA source */
#define IA_RMS          0x1F /* RMS current for VA source */
#define WATT_A          0x28 /* Active Power for source A */
#define VAR_A           0x2C /* Reactive power for source A */
#define VA_A            0x2A /* Volt-Amperes for source A */
#define PFA             0x33 /* Source A Power Factor */

#define INSTAN_VB       0x1E /* instaneous Voltage for VB source */
#define INSTAN_IB       0x26 /* instaneous Current for IB source */
#define INSTAN_PB       0x2F /* instaneous Active Power for source B*/
#define INSTAN_PQB      0x31 /* instaneous Voltage for VB source */
#define VB_RMS          0x18 /* RMS voltage for VB source */
#define IB_RMS          0x20 /* RMS current for VB source */
#define WATT_B          0x29 /* Active Power for source B */
#define VAR_B           0x2D /* Reactive power for source B */
#define VA_B            0x2B /* Volt-amperes for source B */
#define PFB             0x34 /* Source B Power Factor */

/* Addr bit 6-7: ADDR6, ADDR7 */
#define SPI_CB_ADDR_MASK_7_6(x)	(((x) & 0xC0) >> 6)
/* Addr bit 0 - 5 */
#define SPI_TB_ADDR_MASK_5_0(x)	((x) & 0x3F)

#define SPI_CB_NBR_ACC	0x00	/* number register of accesss, limit to 1 */
#define SPI_CB_CMD	0x01	/* SPI command flag */
#define SPI_OP_READ	0x00	/* bit 1: Read/Write RD:0 W:1 */
#define SPI_OP_WRITE	0x02	/* bit 1: Read/Write RD:0 W:1 */
/* Positive / negative conversion */
#define SIGN_CONVERT	0xFFFFFFFFFFFFFFFF
#define DATA_BIT_MASK	0x00FFFFFF
#define SIGN_BIT_NUM	23
#define SPI_MSG_LEN	5
#define RX_OFFSET	1
#define SPI_BBUFFER_LEN 4096
/* All registers on the device are 24-bit */
#define REG_WIDTH	24
#define SAMPLE_INTERVAL_USEC 250 /* High-rate sample interval (microseconds) */
#define RESET_DELAY_MSEC 100
#define INTR_GPIO        2

/* SPI message Control byte */
#define SPI_CB(x)	((SPI_CB_NBR_ACC << 4)\
			| (SPI_CB_ADDR_MASK_7_6(x) << 2)\
			| SPI_CB_CMD)
/* SPI message Transaction byte */
#define SPI_TB_READ(x)	((SPI_TB_ADDR_MASK_5_0(x) << 2)\
			| SPI_OP_READ)
#define SPI_TB_WRITE(x)	((SPI_TB_ADDR_MASK_5_0(x) << 2)\
			| SPI_OP_WRITE)

#define TIMER_PERIOD_MS	90
#define TIMER_PERIOD	msecs_to_jiffies(TIMER_PERIOD_MS)

#define MASK0_INT	(STATUS_MASK_OV_AMP2 \
			| STATUS_MASK_OV_AMP1 \
			| STATUS_MASK_OV_WATT2 \
			| STATUS_MASK_OV_WATT1 \
			| STATUS_MASK_VB_SURGE \
			| STATUS_MASK_VA_SURGE \
			| STATUS_MASK_UN_VRMSB \
			| STATUS_MASK_OV_VRMSB \
			| STATUS_MASK_UN_VRMSA \
			| STATUS_MASK_OV_VRMSA \
			| STATUS_MASK_VB_SAG \
			| STATUS_MASK_VA_SAG)


/**
 * max78m6610_lmu_channels structure maps eADC measurement features to
 * IIO channels on the IIO sysfs user interface
 */
static const struct iio_chan_spec max78m6610_lmu_channels[] = {
	/* IIO Channels for source A */
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 0,
		.extend_name = "inst",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = INSTAN_VA,
		.scan_index = 0,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 0,
		.extend_name = "rms",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = IA_RMS,
		.scan_index = 1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 0,
		.extend_name = "inst_act",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = INSTAN_PA,
		.scan_index = 2,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 0,
		.extend_name = "inst_react",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = INSTAN_PQA,
		.scan_index = 3,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 0,
		.extend_name = "avg_act",
		/* IIO_CHAN_INFO_AVERAGE_RAW is not used here,
		 * this average value is provide by HW register,
		 */
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = WATT_A,
		.scan_index = 4,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 0,
		.extend_name = "avg_react",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = VAR_A,
		.scan_index = 5,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 0,
		.extend_name = "apparent",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = VA_A,
		.scan_index = 6,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 0,
		.extend_name = "factor",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = PFA,
		.scan_index = 7,
		.scan_type = {
			.sign = 's',
			.realbits = 32, /* data type S.22 */
			.storagebits = 32,
			.shift = 22,
		},
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 0,
		.extend_name = "rms",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = VA_RMS,
		.scan_index = 8,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},

	/* IIO channels for source B */
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 1,
		.extend_name = "inst",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = INSTAN_VB,
		.scan_index = 9,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 1,
		.extend_name = "rms",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = IB_RMS,
		.scan_index = 10,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 1,
		.extend_name = "inst_act",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = INSTAN_PB,
		.scan_index = 11,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 1,
		.extend_name = "inst_react",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = INSTAN_PQB,
		.scan_index = 12,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 1,
		.extend_name = "avg_act",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = WATT_B,
		.scan_index = 13,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 1,
		.extend_name = "avg_react",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = VAR_B,
		.scan_index = 14,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 1,
		.extend_name = "apparent",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = VA_B,
		.scan_index = 15,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_POWER,
		.indexed = 1,
		.channel = 1,
		.extend_name = "factor",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = PFB,
		.scan_index = 16,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 22, /* data type S.22 */
		},
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 1,
		.extend_name = "rms",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = VB_RMS,
		.scan_index = 17,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 0,
		.extend_name = "inst",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = INSTAN_IA,
		.scan_index = 18,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 1,
		.extend_name = "inst",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT |
				IIO_CHAN_INFO_SCALE_SHARED_BIT,
		.address = INSTAN_IB,
		.scan_index = 19,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 0,
		.extend_name = "phasecomp",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = PHASECOMP1,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 21,
		},
		.output = 1,
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 1,
		.extend_name = "phasecomp",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = PHASECOMP3,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 21,
		},
		.output = 1,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 0,
		.extend_name = "calib_target_rms",
		.info_mask = IIO_CHAN_INFO_SHARED_BIT(IIO_CHAN_INFO_RAW),
		.address = VTARGET,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 0,
		.extend_name = "calib_target_rms",
		.info_mask = IIO_CHAN_INFO_SHARED_BIT(IIO_CHAN_INFO_RAW),
		.address = ITARGET,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 0,
		.extend_name = "calib_gain",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = S0_GAIN,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 21,
		},
		.output = 1,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 1,
		.extend_name = "calib_gain",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = S2_GAIN,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 21,
		},
		.output = 1,
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 0,
		.extend_name = "calib_gain",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = S1_GAIN,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 21,
		},
		.output = 1,
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 1,
		.extend_name = "calib_gain",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = S3_GAIN,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 21,
		},
		.output = 1,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 0,
		.extend_name = "calib_offset",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = S0_OFFSET,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 1,
		.extend_name = "calib_offset",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = S2_OFFSET,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 0,
		.extend_name = "calib_offset",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = S1_OFFSET,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_CURRENT,
		.indexed = 1,
		.channel = 1,
		.extend_name = "calib_offset",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = S3_OFFSET,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 0,
		.channel = 0,
		.extend_name = "surge_threshold",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = VSURG_VAL,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 0,
		.channel = 0,
		.extend_name = "sag_threshold",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = VSAG_VAL,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 0,
		.channel = 0,
		.extend_name = "rms_min_threshold",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = VRMS_MIN,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_VOLTAGE,
		.indexed = 0,
		.channel = 0,
		.extend_name = "rms_max_threshold",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = VRMS_MAX,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_CURRENT,
		.indexed = 0,
		.channel = 0,
		.extend_name = "rms_max_threshold",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = IRMS_MAX,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},
	{
		.type = IIO_POWER,
		.indexed = 0,
		.channel = 0,
		.extend_name = "active_max_threshold",
		.info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT,
		.address = WATT_MAX,
		.scan_index = -1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.shift = 23,
		},
		.output = 1,
	},

	IIO_CHAN_SOFT_TIMESTAMP(20),
};

/* max number of iio channels */
#define MAX_CHAN_NUM		ARRAY_SIZE(max78m6610_lmu_channels)

/* eADC state structure */
struct max78m6610_lmu_state {
	struct spi_device	*spi;
	struct iio_dev_attr	*iio_attr;
	struct iio_trigger      *trig;
	struct spi_transfer	ring_xfer[MAX_CHAN_NUM];
	struct spi_transfer	scan_single_xfer;
	struct spi_message	ring_msg;
	struct spi_message	scan_single_msg;

	u8	tx_buf[SPI_MSG_LEN * MAX_CHAN_NUM];
	u8	rx_buf[SPI_MSG_LEN * MAX_CHAN_NUM + sizeof(s64)];

	int reset_gpio;

	/* Char dev to provide ioctl interface for f/w upgrade
	 * or low-level register access */
	struct cdev cdev;
	dev_t cdev_no;
	struct class *cl;
	u8	*bbuffer;
	struct timer_list	max78m6610_timer;
};

/**
 * ret_fraction_log2
 *
 * @param val: pointer to val
 * @param val2: pointer to val2
 * @return: no returns
 *
 * this function to re-implement of IIO_VAL_FRACTIONAL_LOG2 marco in IIO
 * because of the do_div() function is not correctly handle the negative
 * input value.
 */
static void ret_fraction_log2(int *val, int *val2)
{
	s64 tmp;

	tmp = *val;

	if (*val < 0) {
		/* the do_div function will return trash if the value
		 * of input is negative. We need to treat tmp as
		 * a positive number for calculation.
		 * 1. XOR tmp with 0xFFFFFFFFFFFFFFFF.
		 * 2. add on the differential
		 */
		tmp = (tmp ^ SIGN_CONVERT) + 1;
		tmp = tmp * 1000000000LL >> (*val2);
		*val2 = do_div(tmp, 1000000000LL);
		*val = tmp;
		/* the IIO_VAL_INT_PLUS_NANO marco is used in the later stage
		 * to return the proper format of output.
		 * The IIO use the value of val2 to determinate the sign
		 * of the output.
		 * Convert val2 from positive to negative to fool IIO to
		 * display the
		 * correct output format.
		 */
		*val2 = *val2 ^ SIGN_CONVERT;
	} else {

		tmp = tmp * 1000000000LL >> (*val2);
		*val2 = do_div(tmp, 1000000000LL);
		*val = tmp;
	}
}

/**
 * intplusnano_to_regval
 *
 * @param val_int:  Integer part of floating point value
 * @param val_nano: Fractional part of floating point value
 * @param frac_bits: The number of fractional bits to produce for this register
 * @param regval: The resulting 24-bit signed fixed-point register value
 * @return: 0 on success, non-zero on error
 *
 * As the kernel doesn't allow floating point numbers, IIO
 * will split them into separate integer and fractional parts.  This function
 * then converts them into fixed-point signed register values for the MAX78M6610
 */
static int intplusnano_to_regval(int val_int, int val_nano,
				 int fract_bits, u32 *regval)
{
	int i, max_int, negative = 0;

	/* Maximum integer value must be 24 bits minus sign and fraction_bits */
	max_int = 1 << (REG_WIDTH - fract_bits - 1);

	if ((val_int >= max_int) ||
	    (val_int < -max_int) ||
	    ((val_int == -max_int) && (val_nano != 0))) {
		pr_err("Input value exceeds maximum allowed range\n");
		return -EINVAL;
	}

	*regval = abs(val_int) << fract_bits;

	/* Set the sign-bit, if input is negative */
	if ((val_int < 0) || (val_nano < 0))
		negative = 1;

	val_nano = abs(val_nano);

	/* Divide the fractional part down by negative powers of 2*/
	for (i = fract_bits-1; i >= 0 && val_nano; i--) {
		val_nano = val_nano << 1;
		if (val_nano >= 1000000000LL) {
			*regval |= (1 << i);
			val_nano -= 1000000000LL;
		}
	}

	/* Get 2s complement of number if negative */
	if (negative)
		*regval = (~(*regval) + 1) & ((1 << REG_WIDTH) - 1);

	return 0;
}

/**
 * __max78m6610_lmu_spi_reg_read
 *
 * @param st: Driver state data
 * @param regaddr: The register word address to read
 * @param regval: The 24-bit register value obtained by the read operation
 * @return: 0 on success, non-zero on error
 *
 * Issues a SPI transaction to read a single register on the device.
 * Performs endian byte swap before returning the register data.
 */
static inline
int __max78m6610_lmu_spi_reg_read(struct max78m6610_lmu_state *st,
				  u8 regaddr,
				  u32 *regval)
{
	int ret;

	st->tx_buf[0] = SPI_CB(regaddr);
	st->tx_buf[1] = SPI_TB_READ(regaddr);
	ret = spi_sync(st->spi, &st->scan_single_msg);
	if (ret) {
		pr_err("spi_sync return error: %d\n", ret);
		return -EIO;
	}

	*regval = (st->rx_buf[2] << 16) | (st->rx_buf[3] << 8) | st->rx_buf[4];

	return 0;
}

/**
 * __max78m6610_lmu_spi_reg_write
 *
 * @param st: Driver state data
 * @param regaddr: The register word address to write
 * @param regval: The 24-bit value to write to the register
 * @return: 0 on success, non-zero on error
 *
 * Issues a SPI transaction to write a single register on the device.
 * Performs endian byte swap before writing the register data.
 */
static inline
int __max78m6610_lmu_spi_reg_write(struct max78m6610_lmu_state *st,
				   u8 regaddr,
				   u32 regval)
{
	int ret;

	st->tx_buf[0] = SPI_CB(regaddr);
	st->tx_buf[1] = SPI_TB_WRITE(regaddr);
	st->tx_buf[2] = regval >> 16;
	st->tx_buf[3] = regval >> 8;
	st->tx_buf[4] = regval & 0xFF;

	ret = spi_sync(st->spi, &st->scan_single_msg);
	if (ret) {
		pr_err("spi_sync return non-zero value\n");
		ret = -EIO;
	}

	return ret;
}

/**
 * max78m6610_lmu_update_scan_mode
 *
 * @param indio_dev: iio_dev pointer.
 * @param active_scan_mask: pointer to scan mask.
 * @return 0 on success or standard errnos on failure
 *
 * setup the spi transfer buffer for the actived scan mask
 **/
static int max78m6610_lmu_update_scan_mode(struct iio_dev *indio_dev,
	const unsigned long *active_scan_mask)
{
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);
	int i, tx = 0, k = 0;
	unsigned addr;

	spi_message_init(&st->ring_msg);

	/* scan through all the channels */
	for (i = 0; i < MAX_CHAN_NUM; i++) {
		/* we build the the spi message here that support
		 * multiple register access request on the selected channel */
		if (test_bit(i, active_scan_mask)) {
			addr = max78m6610_lmu_channels[i].address;
			/* first two bytes are the contol bytes */
			st->tx_buf[tx] = SPI_CB(addr);
			st->tx_buf[tx+1] = SPI_TB_READ(addr);

			st->ring_xfer[k].cs_change = 0;
			st->ring_xfer[k].tx_buf = &st->tx_buf[tx];
			/* rx buffer */
			/* All the HW registers in the HW are designed as 24 bit
			 * size, so we skip the first byte in the rx_buf when
			 * constructing the ring_xfer.
			 */
			st->ring_xfer[k].rx_buf = &st->rx_buf[tx];
			st->ring_xfer[k].len = SPI_MSG_LEN;
			st->ring_xfer[k].cs_change = 1;

			spi_message_add_tail(&st->ring_xfer[k],
					&st->ring_msg);
			/* update in bytes number */
			tx += SPI_MSG_LEN;
			k++;
		}
	}

	return 0;
}

/**
 * max78m6610_lmu_trigger_handle
 *
 * @param irq: irq indicator
 * @parma p: iio pull funciton pointer
 * @return IRQ_HANDLED
 *
 * bh handler of trigger launched polling to ring buffer
 *
 **/
static irqreturn_t max78m6610_lmu_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);

	u32 scan_buf[((sizeof(u32)*MAX_CHAN_NUM)+sizeof(s64))/sizeof(u32)];
	s64 time_ns = 0;
	int b_sent;
	int i = 0, rx_bit = 0;
	int scan_count;

	b_sent = spi_sync(st->spi, &st->ring_msg);
	if (b_sent) {
		pr_err("spi_sync failed.\n");
		goto done;
	}

	scan_count = bitmap_weight(indio_dev->active_scan_mask,
				   indio_dev->masklength);

	if (indio_dev->scan_timestamp) {
		time_ns = iio_get_time_ns();
		memcpy((u8 *)scan_buf + indio_dev->scan_bytes - sizeof(s64),
			&time_ns, sizeof(time_ns));
	}

	for (i = 0; i < scan_count; i++) {
		u32 *rx_buf_32 = NULL;
		rx_bit = i*SPI_MSG_LEN + RX_OFFSET;
		rx_buf_32 = (u32 *)&(st->rx_buf[rx_bit]);
		*rx_buf_32 = be32_to_cpu(*rx_buf_32) & DATA_BIT_MASK;
		scan_buf[i] = sign_extend32(*rx_buf_32,
				SIGN_BIT_NUM);
	}

	iio_push_to_buffers(indio_dev, (u8 *)scan_buf);
done:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

/**
 * max78m6610_lmu_read_raw
 *
 * @param indio_dev: iio_dev pointer
 * @param chan: pointer to iio channel spec struct
 * @param val: return value pointer
 * @param val2: return value 2 ponter
 * @parma m: read mask
 * @return: IIO value type
 *
 * This function will be invoked when request a value form the device.
 * Read mask specifies which value, return value will specify the type of
 * value returned from device, val and val2 will contains the elements
 * making up the return value.
 */
static int max78m6610_lmu_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val,
			   int *val2,
			   long m)
{
	int ret;
	u32 regval;
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);

	switch (m) {

	case IIO_CHAN_INFO_RAW:
		mutex_lock(&indio_dev->mlock);
		if (indio_dev->currentmode == INDIO_BUFFER_TRIGGERED) {
			mutex_unlock(&indio_dev->mlock);
			return -EBUSY;
		}

		ret = __max78m6610_lmu_spi_reg_read(st, chan->address, &regval);
		mutex_unlock(&indio_dev->mlock);
		if (ret)
			return ret;

		*val = sign_extend32(regval, SIGN_BIT_NUM);
		*val2 = chan->scan_type.shift;

		ret_fraction_log2(val, val2);
		return IIO_VAL_INT_PLUS_NANO;

		/* the full scale units : -1.0 to 1-LSB (0x7FFFFF)
		 * As an example, if 230V-peak at the input to the voltage
		 * divider gives 250mV-peak at the chip input, one would get a
		 * full scale register reading of 1 - LSB (0x7FFFFF) for
		 * instaneous voltage.
		 * Similarly, if 30Apk at the sensor input provides 250mV-peak
		 * to the chip input, a full scale register value of 1 - LSB
		 * (0x7FFFFF) for instanteous current would correspond to
		 * 30 amps.
		 * Full scale watts correspond to the result of full scale
		 * current and voltage so, in this example, full scale watts
		 * is 230 x 30 or 6900 watts.
		 */

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_CURRENT:
			*val = 250; /* unit mV */
			return IIO_VAL_INT;

		case IIO_VOLTAGE:
			*val = 250; /* unit: mV */
			return IIO_VAL_INT;

		case IIO_POWER:
			*val = 250*250; /* uV */
			return IIO_VAL_INT;

		default:
			return -EINVAL;
		}

	}
	return -EINVAL;
}

/**
 * max78m6610_lmu_write_raw
 *
 * @param indio_dev: iio_dev pointer
 * @param chan: pointer to iio channel spec struct
 * @param val: input value pointer
 * @param val2: input value 2 ponter
 * @parma m: write mask indicating IIO info type
 * @return: status indicating success (zero) or fail (non-zero)
 *
 * This function will be invoked on a request to write a value to the device.
 * Write mask specifies an IIO value type, val and val2 contain the integer and
 * fractional elements of the floating point input value (INT+NANO).
 */
static int max78m6610_lmu_write_raw(struct iio_dev *indio_dev,
				    struct iio_chan_spec const *chan,
				    int val,
				    int val2,
				    long m)
{
	int ret;
	u32 regval;
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);

	mutex_lock(&indio_dev->mlock);
	if (indio_dev->currentmode == INDIO_BUFFER_TRIGGERED) {
		ret = -EBUSY;
		goto exit_unlock;
	}

	switch (m) {

	case IIO_CHAN_INFO_RAW:
		ret = intplusnano_to_regval(val, val2,
					    chan->scan_type.shift, &regval);
		if (ret)
			goto exit_unlock;

		ret = __max78m6610_lmu_spi_reg_write(st, chan->address, regval);
		break;

	default:
		pr_err("Invalid channel selected for writing\n");
		ret = -EINVAL;
		goto exit_unlock;
	}

exit_unlock:
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

/**
 * max78m6610_lmu_write_raw_get_fmt
 *
 * @param indio_dev: iio_dev pointer
 * @param chan: pointer to iio channel spec struct
 * @param mask: specifies which value to be written
 * @return: the format specifier for the channel value to be written
 *
 * IIO will query the expected format of the input value, and will then
 * interpret and format it correctly before passing it to
 * max78m6610_lmu_write_raw().  In all cases, we expect floating point
 * numbers as input, which IIO will convert into integer and fractional parts
 */
static int max78m6610_lmu_write_raw_get_fmt(struct iio_dev *indio_dev,
					    struct iio_chan_spec const *chan,
					    long mask)
{
	return IIO_VAL_INT_PLUS_NANO;
}


/**
 * max78m6610_lmu_reg_access
 *
 * @param indio_dev: iio_dev pointer
 * @param reg: register address
 * @param writeval: register value to write (ignored if readval is set)
 * @parma readval: pointer to return register read result (set NULL for write)
 * @return: status indicating success (zero) or fail (non-zero)
 *
 * This function allows direct read/write access MAX78M6610+LMU registers
 * for debug only
 */
static int max78m6610_lmu_reg_access(struct iio_dev *indio_dev,
				     unsigned reg, unsigned writeval,
				     unsigned *readval)
{
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);
	int ret = 0;

	mutex_lock(&indio_dev->mlock);
	if (indio_dev->currentmode == INDIO_BUFFER_TRIGGERED) {
		ret = -EBUSY;
		goto exit_unlock;
	}

	if (readval)
		ret = __max78m6610_lmu_spi_reg_read(st, reg, readval);
	else
		ret = __max78m6610_lmu_spi_reg_write(st, reg, writeval);

exit_unlock:
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

/**
 * max78m6610_lmu_reset
 *
 * @param indio_dev: iio_dev pointer
 *
 * Executes a reset of the MAX78M6610+LMU by briefly asserting the
 * hardware reset signal for the device.  Volatile register values
 * will revert to power-on default values.
 */
static int max78m6610_lmu_reset(struct iio_dev *indio_dev)
{
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);
	int ret = 0;
	int gpio = st->reset_gpio;

	struct gpio device_reset_gpio = {
		gpio,
		GPIOF_OUT_INIT_HIGH,
		"max78m6610_lmu_reset"
	};

	if (gpio < 0) {
		pr_err("Reset GPIO has not been configured\n");
		return -ENXIO;
	}

	mutex_lock(&indio_dev->mlock);
	if (indio_dev->currentmode == INDIO_BUFFER_TRIGGERED) {
		ret = -EBUSY;
		goto exit_unlock;
	}

	ret = gpio_request_array(&device_reset_gpio, 1);
	if (ret) {
		pr_err("%s: Failed to allocate Device Reset GPIO pin\n",
				__func__);
		goto exit_unlock;
	}
	gpio_set_value(gpio, 0);
	msleep(RESET_DELAY_MSEC);
	gpio_set_value(gpio, 1);
	msleep(RESET_DELAY_MSEC);

	gpio_free_array(&device_reset_gpio, 1);

exit_unlock:
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

/**
 * max78m6610_lmu_write_reset
 *
 * @param dev: device descriptor associated with sysfs attribute node
 * @param attr: device sysfs attribute descriptor
 * @param buf: data written by user to the attribute node
 * @param len: length in bytes of data written by user
 *
 * This handles a write to this sysfs node from user-space, and invokes a
 * reset of the MAX78M6610+LMU if an appropriate value is written.
 * Valid input character values are 1, y and Y
 */
static ssize_t max78m6610_lmu_write_reset(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	int ret = 0;

	if (len < 1)
		return -1;
	switch (buf[0]) {
	case '1':
	case 'y':
	case 'Y':
		ret = max78m6610_lmu_reset(indio_dev);
		return ret ? ret : len;
	}
	return -1;
}

/**
 * max78m6610_lmu_calib_cmd
 *
 * @param indio_dev: iio_dev pointer
 * @param calib_command: 24-bit calibration command value
 *
 * Executes a specified calibration command on the MAX78M6610+LMU
 * The calib_command input value is written directly to the COMMAND
 * register on the device, to invoke a selected automatic calibration
 * routine.  The driver waits until the calibration completes, and then
 * checks the status (depending on the specific command)
 */
static int max78m6610_lmu_calib_cmd(struct iio_dev *indio_dev,
				    u32 calib_command)
{
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);
	u32 samples, calcycs;
	unsigned delay_ms;
	int max_retries = 5;
	int ret = 0;

	mutex_lock(&indio_dev->mlock);
	if (indio_dev->currentmode == INDIO_BUFFER_TRIGGERED) {
		ret = -EBUSY;
		goto exit_unlock;
	}

	/* Calculate the delay required for calibration to complete */
	ret = __max78m6610_lmu_spi_reg_read(st, SAMPLES, &samples);
	if (ret)
		goto exit_unlock;
	ret = __max78m6610_lmu_spi_reg_read(st, CALCYCS, &calcycs);
	if (ret)
		goto exit_unlock;
	delay_ms = (samples * calcycs * SAMPLE_INTERVAL_USEC)/1000;

	ret = __max78m6610_lmu_spi_reg_write(st, COMMAND, calib_command);
	if (ret)
		goto exit_unlock;

	do {
		/* Wait for the calibration to complete */
		mdelay(delay_ms);

		ret = __max78m6610_lmu_spi_reg_read(st, COMMAND,
						    &calib_command);
		if (ret)
			goto exit_unlock;
	} while ((calib_command & 0xFF0000) && (max_retries--));

	if (max_retries <= 0) {
		pr_err("Timed out waiting for calibration to complete\n");
		ret = -EIO;
		goto exit_unlock;
	}

	/* Gain calibration commands (bit 9 unset) can be checked for failure */
	if ((!(calib_command & 0x000200)) && (calib_command & 0x007800)) {
		pr_err("Calibration failed: COMMAND=0x%06X\n", calib_command);
		ret = -EFAULT;
	}

exit_unlock:
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

/**
 * max78m6610_lmu_write_calib
 *
 * @param dev: device descriptor associated with sysfs attribute node
 * @param attr: device sysfs attribute descriptor
 * @param buf: data written by user to the attribute node
 * @param len: length in bytes of data written by user
 *
 * This handles a write to this sysfs node from user-space, and invokes a
 * calibration command on the MAX78M6610+LMU if an appropriate value is written.
 * Valid input character values are 1, y and Y.
 * The handler is re-used for multiple calibration commands, so the command
 * value is passed transparently via the attribute address field
 */
static ssize_t max78m6610_lmu_write_calib(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	int ret = 0;

	if (len < 1)
		return -1;
	switch (buf[0]) {
	case '1':
	case 'y':
	case 'Y':
		ret = max78m6610_lmu_calib_cmd(indio_dev, this_attr->address);
		return ret ? ret : len;
	}
	return -1;
}

/**
 * max78m6610_lmu_flash_save_cmd
 *
 * @param indio_dev: iio_dev pointer
 *
 * Executes a flash-save command on the MAX78M6610+LMU.
 * This saves all current volatile register values to flash on the device,
 * making them persistent across device resets or power cycles.
 */
static int max78m6610_lmu_flash_save_cmd(struct iio_dev *indio_dev)
{
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);
	int ret = 0;

	mutex_lock(&indio_dev->mlock);
	if (indio_dev->currentmode == INDIO_BUFFER_TRIGGERED) {
		ret = -EBUSY;
		goto exit_unlock;
	}

	ret = __max78m6610_lmu_spi_reg_write(st, COMMAND, FLASHSAVE_CMD);

exit_unlock:
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

/**
 * max78m6610_lmu_write_flash
 *
 * @param dev: device descriptor associated with sysfs attribute node
 * @param attr: device sysfs attribute descriptor
 * @param buf: data written by user to the attribute node
 * @param len: length in bytes of data written by user
 *
 * This handles a write to this sysfs node from user-space, and invokes a
 * flash-save command on the MAX78M6610+LMU if an appropriate value is written.
 * Valid input character values are 1, y and Y.
 */
static ssize_t max78m6610_lmu_write_flash(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	int ret = 0;

	if (len < 1)
		return -1;
	switch (buf[0]) {
	case '1':
	case 'y':
	case 'Y':
		ret = max78m6610_lmu_flash_save_cmd(indio_dev);
		return ret ? ret : len;
	}
	return -1;
}

static inline int __max78m6610_lmu_mask0_set(struct max78m6610_lmu_state *st);
static inline int __max78m6610_lmu_mask0_reset(struct max78m6610_lmu_state *st);

/**
 * max78m6610_lmu_status_scan
 *
 * @param indio_dev: iio_dev pointer
 *
 * Executes a read of the STATUS register on the MAX78M6610+LMU.
 * Event status bits are checked, and event notifications are raised for
 * user-space applications if any events are asserted. Event status bits are
 * sticky and are cleared by setting the corresponding bit in the STATUS_RESET
 * register to allow further occurances of the same event to be detected
 */
static int max78m6610_lmu_status_scan(struct iio_dev *indio_dev)
{
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);
	int ret;
	u64 timestamp_ns = iio_get_time_ns();
	static unsigned old_status;
	unsigned new_status = 0x00;
	u16 event_active;


	mutex_lock(&indio_dev->mlock);
	if (indio_dev->currentmode == INDIO_BUFFER_TRIGGERED) {
		ret = -EBUSY;
		goto exit_unlock;
	}

	/* Disable eADC interrupts - special-reset MASK0 */
	ret = __max78m6610_lmu_mask0_reset(st);
	if (ret) {
		pr_err("Failed to disable interrupts from MASK0!\n");
		goto exit_unlock;
	}

	ret = __max78m6610_lmu_spi_reg_read(st, STATUS, &new_status);
	if (ret) {
		pr_err("Failed to read STATUS register\n");
		goto exit_unlock;
	}
	new_status &= ~STATUS_MASK_IGNORE;

	/* Not all of the event types used below are ideal, but there is a
	 * limited set available and we want to use different event types for
	 * the different events (e.g sag vs. min-threshold) to allow user
	 * applications to distinguish them
	 */
	if ((new_status & STATUS_MASK_VA_SAG) ^
			(old_status & STATUS_MASK_VA_SAG)) {
		event_active = !!(new_status & STATUS_MASK_VA_SAG);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_VOLTAGE,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_FALLING,
					IIO_EV_TYPE_MAG,
					0 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_VB_SAG) ^
			(old_status & STATUS_MASK_VB_SAG)) {
		event_active = !!(new_status & STATUS_MASK_VB_SAG);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_VOLTAGE,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_FALLING,
					IIO_EV_TYPE_MAG,
					1 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_OV_VRMSA) ^
			(old_status & STATUS_MASK_OV_VRMSA)) {
		event_active = !!(new_status & STATUS_MASK_OV_VRMSA);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_VOLTAGE,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_RISING,
					IIO_EV_TYPE_THRESH,
					0 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_UN_VRMSA) ^
			(old_status & STATUS_MASK_UN_VRMSA)) {
		event_active = !!(new_status & STATUS_MASK_UN_VRMSA);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_VOLTAGE,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_FALLING,
					IIO_EV_TYPE_THRESH,
					0 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_OV_VRMSB) ^
			(old_status & STATUS_MASK_OV_VRMSB)) {
		event_active = !!(new_status & STATUS_MASK_OV_VRMSB);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_VOLTAGE,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_RISING,
					IIO_EV_TYPE_THRESH,
					1 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_UN_VRMSB) ^
			(old_status & STATUS_MASK_UN_VRMSB)) {
		event_active = !!(new_status & STATUS_MASK_UN_VRMSB);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_VOLTAGE,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_FALLING,
					IIO_EV_TYPE_THRESH,
					1 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_VA_SURGE) ^
			(old_status & STATUS_MASK_VA_SURGE)) {
		event_active = !!(new_status & STATUS_MASK_VA_SURGE);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_VOLTAGE,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_RISING,
					IIO_EV_TYPE_MAG,
					0 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_VB_SURGE) ^
			(old_status & STATUS_MASK_VB_SURGE)) {
		event_active = !!(new_status & STATUS_MASK_VB_SURGE);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_VOLTAGE,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_RISING,
					IIO_EV_TYPE_MAG,
					1 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_OV_WATT1) ^
			(old_status & STATUS_MASK_OV_WATT1)) {
		event_active = !!(new_status & STATUS_MASK_OV_WATT1);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_POWER,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_RISING,
					IIO_EV_TYPE_THRESH,
					0 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_OV_WATT2) ^
			(old_status & STATUS_MASK_OV_WATT2)) {
		event_active = !!(new_status & STATUS_MASK_OV_WATT2);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_POWER,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_RISING,
					IIO_EV_TYPE_THRESH,
					1 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_OV_AMP1) ^
			(old_status & STATUS_MASK_OV_AMP1)) {
		event_active = !!(new_status & STATUS_MASK_OV_AMP1);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_CURRENT,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_RISING,
					IIO_EV_TYPE_THRESH,
					0 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}
	if ((new_status & STATUS_MASK_OV_AMP2) ^
			(old_status & STATUS_MASK_OV_AMP2)) {
		event_active = !!(new_status & STATUS_MASK_OV_AMP2);
		iio_push_event(indio_dev,
				IIO_EVENT_CODE(IIO_CURRENT,
					0 /* diff */,
					IIO_NO_MOD,
					IIO_EV_DIR_RISING,
					IIO_EV_TYPE_THRESH,
					1 /* chan */,
					0 /* chan1 */,
					event_active /* chan2 */),
				timestamp_ns);
	}

	/* Write reset register, clearing only bits that we've processed and
	 * RESET bit if it was set at the time of the last read of STATUS */
	ret = __max78m6610_lmu_spi_reg_write(st, STATUS_RESET,
			new_status & STATUS_MASK_STICKY);
	if (ret) {
		pr_err("Failed to write STATUS_RESET register\n");
		goto exit_unlock;
	}

	/* Save the current state of STATUS to be used next time as reference*/
	old_status = new_status;
	if (new_status & STATUS_MASK_STICKY) {
		mod_timer(&st->max78m6610_timer, jiffies + TIMER_PERIOD);
	} else {
		del_timer(&st->max78m6610_timer);
		/* Re-enable eADC interrupts by restoring the content
		 * of MASK0 register */
		ret = __max78m6610_lmu_mask0_set(st);
		if (ret) {
			pr_err("Failed to restore MASK0 register!\n");
			goto exit_unlock;
		}
	}
	mutex_unlock(&indio_dev->mlock);
	return ret;

exit_unlock:
	/* if something failed setup the timer to fire again no matter what */
	mod_timer(&st->max78m6610_timer, jiffies + TIMER_PERIOD);
	mutex_unlock(&indio_dev->mlock);

	return ret;
}


/**
 * max78m6610_lmu_write_int
 *
 * @param dev: device descriptor associated with sysfs attribute node
 * @param attr: device sysfs attribute descriptor
 * @param buf: data written by user to the attribute node
 * @param len: length in bytes of data written by user
 *
 * This is a generic handler to write integer values to any integer registers
 * which are exposed as device sysfs attributes on the user interface.
 * The attribute address field specifies which register to write.
 */

static IIO_DEVICE_ATTR(do_reset, S_IWUSR, NULL,
		       max78m6610_lmu_write_reset, 0);
static IIO_DEVICE_ATTR(do_voltage0_gain_calib, S_IWUSR, NULL,
		       max78m6610_lmu_write_calib, CALCMD_S0_GAIN);
static IIO_DEVICE_ATTR(do_current0_gain_calib, S_IWUSR, NULL,
		       max78m6610_lmu_write_calib, CALCMD_S1_GAIN);
static IIO_DEVICE_ATTR(do_voltage1_gain_calib, S_IWUSR, NULL,
		       max78m6610_lmu_write_calib, CALCMD_S2_GAIN);
static IIO_DEVICE_ATTR(do_current1_gain_calib, S_IWUSR, NULL,
		       max78m6610_lmu_write_calib, CALCMD_S3_GAIN);
static IIO_DEVICE_ATTR(do_voltage0_offset_calib, S_IWUSR, NULL,
		       max78m6610_lmu_write_calib, CALCMD_S0_OFFS);
static IIO_DEVICE_ATTR(do_current0_offset_calib, S_IWUSR, NULL,
		       max78m6610_lmu_write_calib, CALCMD_S1_OFFS);
static IIO_DEVICE_ATTR(do_voltage1_offset_calib, S_IWUSR, NULL,
		       max78m6610_lmu_write_calib, CALCMD_S2_OFFS);
static IIO_DEVICE_ATTR(do_current1_offset_calib, S_IWUSR, NULL,
		       max78m6610_lmu_write_calib, CALCMD_S3_OFFS);
static IIO_DEVICE_ATTR(do_save_to_flash, S_IWUSR, NULL,
		       max78m6610_lmu_write_flash, 0);

static struct attribute *max78m6610_lmu_attributes[] = {
	&iio_dev_attr_do_reset.dev_attr.attr,
	&iio_dev_attr_do_voltage0_gain_calib.dev_attr.attr,
	&iio_dev_attr_do_current0_gain_calib.dev_attr.attr,
	&iio_dev_attr_do_voltage1_gain_calib.dev_attr.attr,
	&iio_dev_attr_do_current1_gain_calib.dev_attr.attr,
	&iio_dev_attr_do_voltage0_offset_calib.dev_attr.attr,
	&iio_dev_attr_do_current0_offset_calib.dev_attr.attr,
	&iio_dev_attr_do_voltage1_offset_calib.dev_attr.attr,
	&iio_dev_attr_do_current1_offset_calib.dev_attr.attr,
	&iio_dev_attr_do_save_to_flash.dev_attr.attr,
	NULL,
};

static const struct attribute_group max78m6610_lmu_attribute_group = {
	.attrs = max78m6610_lmu_attributes,
};

/* Provides an option to poll for events (useful if interrupts unavailable)
 * The option to poll for events is no longer supported, but we need to have at
 * least 1 event attribute to enable the IIO events.
 * That's why this attribute is created with no _show and no _store handlers.
 * */
static IIO_DEVICE_ATTR(null, S_IWUSR, NULL, NULL, 0);


/* Need to have at least 1 event attribute to enable IIO events.
 * Purposely not setting .event_mask for the channels because that would
 * enable the IIO events sysfs entries which are not suitable for this driver
 */
static struct attribute *max78m6610_lmu_event_attributes[] = {
	&iio_dev_attr_null.dev_attr.attr,
	NULL,
};

static struct attribute_group max78m6610_lmu_event_attribute_group = {
	.attrs = max78m6610_lmu_event_attributes,
};


/* Driver specific iio info structure */
static const struct iio_info max78m6610_lmu_info = {
	.read_raw = max78m6610_lmu_read_raw,
	.write_raw = max78m6610_lmu_write_raw,
	.write_raw_get_fmt = max78m6610_lmu_write_raw_get_fmt,
	.debugfs_reg_access = max78m6610_lmu_reg_access,
	.update_scan_mode = max78m6610_lmu_update_scan_mode,
	.event_attrs = &max78m6610_lmu_event_attribute_group,
	.attrs = &max78m6610_lmu_attribute_group,
	.driver_module = THIS_MODULE,
};

/**
 * max78m6610_lmu_open
 *
 * @param inode: inode descriptor associated with char device
 * @param filp: file object pointer
 * @return 0 on success, non-zero errno otherwise
 *
 * This handles an open syscall on the character device node
 */
static int
max78m6610_lmu_open(struct inode *inode, struct file *filp)
{
	struct max78m6610_lmu_state *st;
	int ret = 0;

	st = container_of(inode->i_cdev,
			    struct max78m6610_lmu_state,
			    cdev);
	filp->private_data = st;

	if (!st->bbuffer) {
		st->bbuffer = kmalloc(SPI_BBUFFER_LEN, GFP_KERNEL);
		if (!st->bbuffer) {
			dev_dbg(&st->spi->dev, "open/ENOMEM\n");
			ret = -ENOMEM;
		}
	}

	return ret;
}

/**
 * max78m6610_lmu_release
 *
 * @param inode: inode descriptor associated with char device
 * @param filp: file object pointer
 * @return 0 on success, non-zero errno otherwise
 *
 * This handles a close syscall on the character device node
 */
static int
max78m6610_lmu_release(struct inode *inode, struct file *filp)
{
	struct max78m6610_lmu_state *st =
		(struct max78m6610_lmu_state *)filp->private_data;

	kfree(st->bbuffer);
	st->bbuffer = NULL;

	return 0;
}

/**
 * spidev_message
 *
 * @param st: driver state information
 * @param u_xfers: spi transfer descriptor array
 * @param n_xfers: number of spi transfer descriptors
 * @return 0 on success, non-zero errno otherwise
 *
 * Translates a set of user-space SPI transfer requests to kernel-space
 * equivalent, using bounce-buffers for the data, and invokes spi_sync()
 * to execute the bi-directional SPI transfers
 *
 * The implementation below was borrowed directly from the spidev kernel driver
 * with minor modifications to fit it in here
 */
static int spidev_message(struct max78m6610_lmu_state *st,
			  struct spi_ioc_transfer *u_xfers,
			  unsigned n_xfers)
{
	struct spi_message	msg;
	struct spi_transfer	*k_xfers;
	struct spi_transfer	*k_tmp;
	struct spi_ioc_transfer *u_tmp;
	unsigned		n, total;
	u8			*buf;
	int			status = -EFAULT;

	spi_message_init(&msg);
	k_xfers = kcalloc(n_xfers, sizeof(*k_tmp), GFP_KERNEL);
	if (k_xfers == NULL)
		return -ENOMEM;

	/* Construct spi_message, copying any tx data to bounce buffer.
	 * We walk the array of user-provided transfers, using each one
	 * to initialize a kernel version of the same transfer.
	 */
	buf = st->bbuffer;
	total = 0;
	for (n = n_xfers, k_tmp = k_xfers, u_tmp = u_xfers;
			n;
			n--, k_tmp++, u_tmp++) {
		k_tmp->len = u_tmp->len;

		total += k_tmp->len;
		if (total > SPI_BBUFFER_LEN) {
			status = -EMSGSIZE;
			goto done;
		}

		if (u_tmp->rx_buf) {
			k_tmp->rx_buf = buf;
			if (!access_ok(VERIFY_WRITE, (u8 __user *)
						(uintptr_t) u_tmp->rx_buf,
						u_tmp->len))
				goto done;
		}
		if (u_tmp->tx_buf) {
			k_tmp->tx_buf = buf;
			if (copy_from_user(buf, (const u8 __user *)
						(uintptr_t) u_tmp->tx_buf,
					u_tmp->len))
				goto done;
		}
		buf += k_tmp->len;

		k_tmp->cs_change = !!u_tmp->cs_change;
		k_tmp->bits_per_word = u_tmp->bits_per_word;
		k_tmp->delay_usecs = u_tmp->delay_usecs;
		k_tmp->speed_hz = u_tmp->speed_hz;
#ifdef VERBOSE
		dev_dbg(&st->spi->dev,
			"  xfer len %zd %s%s%s%dbits %u usec %uHz\n",
			u_tmp->len,
			u_tmp->rx_buf ? "rx " : "",
			u_tmp->tx_buf ? "tx " : "",
			u_tmp->cs_change ? "cs " : "",
			u_tmp->bits_per_word ? : st->spi->bits_per_word,
			u_tmp->delay_usecs,
			u_tmp->speed_hz ? : st->spi->max_speed_hz);
#endif
		spi_message_add_tail(k_tmp, &msg);
	}

	status = spi_sync(st->spi, &msg);
	if (status < 0)
		goto done;

	/* copy any rx data out of bounce buffer */
	buf = st->bbuffer;
	for (n = n_xfers, u_tmp = u_xfers; n; n--, u_tmp++) {
		if (u_tmp->rx_buf) {
			if (__copy_to_user((u8 __user *)
					(uintptr_t) u_tmp->rx_buf, buf,
					u_tmp->len)) {
				status = -EFAULT;
				goto done;
			}
		}
		buf += u_tmp->len;
	}
	status = total;

done:
	kfree(k_xfers);
	return status;
}

/**
 * max78m6610_lmu_ioctl
 *
 * @param filp: file object pointer
 * @param cmd: ioctl command
 * @param arg: optional data argument
 * @return 0 on success, non-zero errno otherwise
 *
 * This handles an ioctl syscall on the character device node.  This handler
 * supports only the SPI_IOC_MESSAGE ioctl command defined in spidev.h
 * The implementation below was borrowed from the spidev driver, with some minor
 * modifications to remove support for other ioctl commands not needed here
 */
static long
max78m6610_lmu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct max78m6610_lmu_state *st = filp->private_data;
	struct iio_dev *indio_dev = spi_get_drvdata(st->spi);
	u32			tmp;
	unsigned		n_ioc;
	struct spi_ioc_transfer	*ioc;
	int ret = 0;

	/* Check type and command number */
	if (_IOC_TYPE(cmd) != SPI_IOC_MAGIC)
		return -ENOTTY;

	/* Check access direction once here; don't repeat below.
	 * IOC_DIR is from the user perspective, while access_ok is
	 * from the kernel perspective; so they look reversed.
	 */
	if (_IOC_DIR(cmd) & _IOC_READ)
		ret = !access_ok(VERIFY_WRITE,
				(void __user *)arg, _IOC_SIZE(cmd));
	if (ret == 0 && _IOC_DIR(cmd) & _IOC_WRITE)
		ret = !access_ok(VERIFY_READ,
				(void __user *)arg, _IOC_SIZE(cmd));
	if (ret)
		return -EFAULT;

	ret = mutex_lock_interruptible(&indio_dev->mlock);
	if (ret)
		return ret;

	/* segmented and/or full-duplex I/O request */
	if (_IOC_NR(cmd) != _IOC_NR(SPI_IOC_MESSAGE(0))
	    || _IOC_DIR(cmd) != _IOC_WRITE) {
		ret = -ENOTTY;
		goto exit;
	}

	tmp = _IOC_SIZE(cmd);
	if ((tmp % sizeof(struct spi_ioc_transfer)) != 0) {
		ret = -EINVAL;
		goto exit;
	}
	n_ioc = tmp / sizeof(struct spi_ioc_transfer);
	if (n_ioc == 0)
		goto exit;

	/* copy into scratch area */
	ioc = kmalloc(tmp, GFP_KERNEL);
	if (!ioc) {
		ret = -ENOMEM;
		goto exit;
	}
	if (__copy_from_user(ioc, (void __user *)arg, tmp)) {
		kfree(ioc);
		ret = -EFAULT;
		goto exit;
	}

	/* translate to spi_message, execute */
	ret = spidev_message(st, ioc, n_ioc);
	kfree(ioc);

exit:
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

static const struct file_operations max78m6610_lmu_fops = {
	.owner = THIS_MODULE,
	.open = max78m6610_lmu_open,
	.release = max78m6610_lmu_release,
	.unlocked_ioctl = max78m6610_lmu_ioctl,
};

/**
 * max78m6610_lmu_chrdev_init
 *
 * @param st: driver state information
 * @return 0 on success, non-zero errno otherwise
 *
 * Creates a character device to implement a subset of the spidev user-interface
 * API, namely full-duplex SPI transfers via the ioctl() interface.
 * The intention is to provide user-space applications with direct access to the
 * underlying SPI device if required.  The user-space application is, in this
 * mode of operation, responsible for directly constructing the SPI messages
 * required by the MAX78M6610 and those messages are passed transparently
 * through this driver.  This is needed, for example, to facilitate binary-only
 * firmware update applications, but may also be used by user-space applications
 * to access any device registers which have not been exposed by this driver.
 *
 * The device node created will appear in the filesystem as /dev/max78m6610_lmu
 */
static int
max78m6610_lmu_chrdev_init(struct max78m6610_lmu_state *st)
{
	int ret;
	struct device *dev;

	ret = alloc_chrdev_region(&st->cdev_no, 0, 1,
				  "max78m6610_lmu");
	if (ret) {
		pr_err("Failed to alloc chrdev: %d", ret);
		return ret;
	}

	cdev_init(&st->cdev, &max78m6610_lmu_fops);

	ret = cdev_add(&st->cdev, st->cdev_no, 1);
	if (ret) {
		pr_err("Failed to add cdev: %d", ret);
		unregister_chrdev_region(st->cdev_no, 1);
		return ret;
	}

	st->cl = class_create(THIS_MODULE, "char");
	if (IS_ERR(st->cl)) {
		pr_err("Failed to create device class: %ld",
		       PTR_ERR(st->cl));
		cdev_del(&st->cdev);
		unregister_chrdev_region(st->cdev_no, 1);
		return PTR_ERR(st->cl);
	}

	dev = device_create(st->cl, NULL, st->cdev_no, NULL,
			    "max78m6610_lmu");
	if (IS_ERR(dev)) {
		pr_err("Failed to create device: %ld",
		       PTR_ERR(st->cl));
		class_destroy(st->cl);
		cdev_del(&st->cdev);
		unregister_chrdev_region(st->cdev_no, 1);
		return PTR_ERR(dev);
	}

	return 0;
}

/**
 * max78m6610_lmu_chrdev_remove
 *
 * @param st: driver state information
 * @return 0 on success, non-zero errno otherwise
 *
 * Remove the character device created by max78m6610_lmu_chrdev_init()
 */
static int
max78m6610_lmu_chrdev_remove(struct max78m6610_lmu_state *st)
{
	device_destroy(st->cl, st->cdev_no);
	class_destroy(st->cl);
	cdev_del(&st->cdev);
	unregister_chrdev_region(st->cdev_no, 1);

	return 0;
}


/* Spinlock used to lock the external and timer interrupt handlers */
static DEFINE_SPINLOCK(max78m6610_spinlock);

/* Pointer used to pass iio_dev from top-half to bottom-half handler */
static void *wq_indio_dev;

/* Workqueue used for deffering the work in the bottom-half handler */
static void max78m6610_lmu_irq_do_work(struct work_struct *max78m6610_lmu_wq);
static DECLARE_WORK(max78m6610_lmu_wq, max78m6610_lmu_irq_do_work);


/* max78m6610_lmu_irq_do_work
 *
 * @param max78m6610_lmu_wq: working item
 *
 * Worker function of the work queue which does the bottom-half processing of
 * MAX78M6610 IRQ.
 */
static void max78m6610_lmu_irq_do_work(struct work_struct *max78m6610_lmu_wq)
{
	int ret = 0x00;

	ret = max78m6610_lmu_status_scan((struct iio_dev *)wq_indio_dev);

	if (ret)
		pr_err("MAX78M6610 status scan failed; return code: %d\n", ret);
}



/* max78m6610_lmu_irq_handler
 *
 * @param irq: IRQ number
 * @param private: The dev_id cookie passed to request_irq()
 *
 * @return:
 *	IRQ_NONE	interrupt was not from this device
 *	IRQ_HANDLED	interrupt was handled by this device
 *	IRQ_WAKE_THREAD	handler requests to wake the handler thread
 *
 * Interrupt handler for eADC IRQ.
 */
static irqreturn_t max78m6610_lmu_irq_handler(int irq, void *private)
{
	spin_lock(&max78m6610_spinlock);

	wq_indio_dev = private;
	schedule_work(&max78m6610_lmu_wq);

	spin_unlock(&max78m6610_spinlock);
	return IRQ_HANDLED;
}


/* __max78m6610_lmu_mask0_reset
 *
 * @param indio_dev: iio_dev pointer
 * @return 0 on success, non-zero errno otherwise
 *
 * Clears all bits of MASK0 register except RELAY2 bit.
 *
 * If MASK0 register is completely cleared (write 0x00 to it) while the MP0
 * bit is already active, the MP0 bit is not de-activated.
 * If MASK0 register's value != 0x00 and MASK0 & STATUS == 0, the MP0 bit is
 * de-activated.
 *
 * (MP0 pin == 0) && (MASK0 & STATUS == 0) && (MASK0 != 0) => MP0 = 1
 * (interrupt line is de-asserted)
 */
static inline int __max78m6610_lmu_mask0_reset(struct max78m6610_lmu_state *st)
{
	return __max78m6610_lmu_spi_reg_write(st, MASK0, STATUS_MASK_RELAY2);
}


/* __max78m6610_lmu_mask0_set
 *
 * @param st: eADC state structure
 * @return 0 on success, non-zero errno otherwise
 *
 * Sets on eADC chip the MASK0 bits corresponding to the events we want to
 * receive an interrupt for.
 * If one wants to modify the events which the driver receives interrupt for, he
 * must modify MASK0_INT macro
 */
static inline int __max78m6610_lmu_mask0_set(struct max78m6610_lmu_state *st)
{
	return __max78m6610_lmu_spi_reg_write(st, MASK0, MASK0_INT);
}


/* max78m6610_lmu_mask0_set_default
 *
 * @param indio_dev: IIO device
 * @return 0 on success, non-zero errno otherwise
 *
 * Read MASK0 register, check if it's default value is already MASK0_INT,
 * otherwise set MASK0 = MASK0_INT and save defaults into flash in order to
 * change MASK0 default value.
 */
static int max78m6610_lmu_mask0_set_default(struct iio_dev *indio_dev)
{
	int ret = 0;
	unsigned mask0 = 0x00;
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);

	/* Read MASK0 value */
	mutex_lock(&indio_dev->mlock);
	ret = __max78m6610_lmu_spi_reg_read(st, MASK0, &mask0);
	if (ret) {
		pr_err("Failed to read MASK0 register! ret: %d\n", ret);
		goto error_unlock;
	}
	mutex_unlock(&indio_dev->mlock);
	if (mask0 != MASK0_INT) {
		/* Tell eADC what events to generate interrupt for */
		mutex_lock(&indio_dev->mlock);
		ret = __max78m6610_lmu_mask0_set(st);
		if (ret) {
			pr_err("Failed to enable interrupts on eADC side!\n");
			goto error_unlock;
		}
		mutex_unlock(&indio_dev->mlock);

		/* Save MASK0 default to flash */
		ret = max78m6610_lmu_flash_save_cmd(indio_dev);
		if (ret) {
			pr_err("Failed to save MASK0 default to flash!\n");
			goto error_ret;
		}
	}
	return 0;

error_unlock:
	mutex_unlock(&indio_dev->mlock);
error_ret:
	return ret;
}


/*
 * max78m6610_lmu_irq_init
 *
 * @param indio_dev: IIO device
 * @return 0 on success, non-zero errno otherwise
 *
 * Allocate memory for IIO triger, request the IRQ for eADC, set IIO triger
 * parent and operations; register IIO trigger; set this trigger as default
 * trigger; configure MASK0 default value.
 */
static int max78m6610_lmu_irq_init(struct iio_dev *indio_dev)
{
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);
	int ret = 0;

	if (st->spi->irq < 0) {
		pr_warn("MAX78M6610+LMU IRQ not set. spi->irq: %d\n",
				st->spi->irq);
		return 0;
	}

	ret = request_irq(st->spi->irq, max78m6610_lmu_irq_handler,
			IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND, spi_get_device_id(st->spi)->name,
			indio_dev);
	if (ret) {
		pr_err("Failed to request IRQ %d: request_irg returned %d.\n",
				st->spi->irq, ret);
		goto error_ret;
	}

	/* Check and set MASK0 default */
	ret = max78m6610_lmu_mask0_set_default(indio_dev);
	if (ret) {
		pr_err("Failed to set MASK0 default!\n");
		goto error_free_irq;
	}

	return 0;

error_free_irq:
	free_irq(st->spi->irq, indio_dev);
error_ret:
	return ret;
}


/* max78m6610_lmu_irq_remove
 *
 * @param indio_dev: IIO device
 * @return N/A
 *
 * Unregister IIO trigger, release the IRQ and free IIO triger memory
 */
static void max78m6610_lmu_irq_remove(struct iio_dev *indio_dev)
{
	int ret;
	u32 mask0 = 0x00;
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);

	if (st->spi->irq < 0)
		return;
	/* Instruct MAX78M6610+LMU chip to stop generating interrupts on MP0 */
	mutex_lock(&indio_dev->mlock);
	ret = __max78m6610_lmu_spi_reg_write(st, MASK0, mask0);
	if (ret)
		pr_warn("Failed to write MASK0 register.\n");
	mutex_unlock(&indio_dev->mlock);

	free_irq(st->spi->irq, indio_dev);
}

/* max78m6610_lmu_timer_handler
 *
 * @param data: unused
 * @reutnr N/A
 *
 * max78m6610_timer interrupt handler
 */
static void max78m6610_lmu_timer_handler(unsigned long data)
{
	spin_lock(&max78m6610_spinlock);
	if (NULL != wq_indio_dev)
		schedule_work(&max78m6610_lmu_wq);
	spin_unlock(&max78m6610_spinlock);
}


/**
 * max78m6610_lmu_probe
 *
 * @param spi: spi device pointer
 * @return: return 0 or standard error ids if failure
 *
 * device driver probe funciton for iio_dev struct initialisation.
 */
static int max78m6610_lmu_probe(struct spi_device *spi)
{
	struct max78m6610_lmu_state *st;
	struct iio_dev *indio_dev = iio_device_alloc(sizeof(*st));
	struct max78m6610_lmu_platform_data *pdata = spi->dev.platform_data;
	int ret;

	if (indio_dev == NULL)
		return -ENOMEM;
	st = iio_priv(indio_dev);

	spi_set_drvdata(spi, indio_dev);
	st->spi = spi;

	if (pdata)
		st->reset_gpio = pdata->reset_gpio;
	else
		st->reset_gpio = -1;

	indio_dev->name = spi_get_device_id(spi)->name;
	indio_dev->dev.parent = &spi->dev;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = max78m6610_lmu_channels;
	indio_dev->num_channels = ARRAY_SIZE(max78m6610_lmu_channels);
	indio_dev->info = &max78m6610_lmu_info;

	/* Setup default message */
	st->scan_single_xfer.tx_buf = &st->tx_buf[0];
	st->scan_single_xfer.rx_buf = &st->rx_buf[0];
	st->scan_single_xfer.len = SPI_MSG_LEN;

	spi_message_init(&st->scan_single_msg);
	spi_message_add_tail(&st->scan_single_xfer, &st->scan_single_msg);

	ret = iio_triggered_buffer_setup(indio_dev, NULL,
			&max78m6610_lmu_trigger_handler, NULL);
	if (ret) {
		pr_err("triger buffer setup failed !\n");
		goto error_free;
	}

	pr_debug("%s: alloc dev id: %d\n", __func__, indio_dev->id);
	ret = iio_device_register(indio_dev);
	if (ret)
		goto error_cleanup_ring;

	ret = max78m6610_lmu_chrdev_init(st);
	if (ret)
		goto error_cleanup_ring;
	/* Init the external GPIO interrupt */
	ret = max78m6610_lmu_irq_init(indio_dev);
	if (ret)
		goto error_cleanup_chrdev;
	/* Initialise the timer */
	setup_timer(&st->max78m6610_timer, max78m6610_lmu_timer_handler, 0);

	return 0;

error_cleanup_chrdev:
	max78m6610_lmu_chrdev_remove(st);
error_cleanup_ring:
	iio_triggered_buffer_cleanup(indio_dev);
error_free:
	iio_device_free(indio_dev);

	return ret;
}

/**
 * max78m6610_lmu_remove
 *
 * @param spi: spi device pointer
 * @return: return 0
 *
 * iio device unregister & cleanup
 */
static int max78m6610_lmu_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct max78m6610_lmu_state *st = iio_priv(indio_dev);

	del_timer(&st->max78m6610_timer);

	max78m6610_lmu_irq_remove(indio_dev);

	max78m6610_lmu_chrdev_remove(st);

	iio_device_unregister(indio_dev);

	iio_triggered_buffer_cleanup(indio_dev);
	iio_device_free(indio_dev);

	return 0;
}

static const struct spi_device_id max78m6610_lmu_id[] = {
	{"max78m6610_lmu", 0},
	{}
};
MODULE_DEVICE_TABLE(spi, max78m6610_lmu_id);

static struct spi_driver max78m6610_lmu_driver = {
	.driver = {
		.name	= "max78m6610_lmu",
		.owner	= THIS_MODULE,
	},
	.probe		= max78m6610_lmu_probe,
	.remove		= max78m6610_lmu_remove,
	.id_table	= max78m6610_lmu_id,
};

/**
 * max78m6610_lmu_init
 *
 * device driver module init
 */
static __init int max78m6610_lmu_init(void)
{
	int ret;

	ret = spi_register_driver(&max78m6610_lmu_driver);
	if (ret < 0)
		return ret;

	return 0;
}
module_init(max78m6610_lmu_init);

/**
 * max78m6610_lmu_exit
 *
 * device driver module exit
 */
static __exit void max78m6610_lmu_exit(void)
{
	spi_unregister_driver(&max78m6610_lmu_driver);
}
module_exit(max78m6610_lmu_exit);


MODULE_AUTHOR("Kai Ji <kai.ji@emutex.com>");
MODULE_DESCRIPTION("Maxim 78M6610+LMU eADC");
MODULE_LICENSE("GPL v2");
