/*
 * OmniVision OV2675 sensor driver
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or
 *modify it under the terms of the GNU General Public License as
 *published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 *kind, whether express or implied; without even the implied warranty
 *of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/videodev2.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/log2.h>
#include <linux/delay.h>
#include <linux/module.h>

#include <media/v4l2-subdev.h>
#include <media/v4l2-chip-ident.h>
#include <media/soc_camera.h>
#include <linux/videodev2_brcm.h>
#include "ov2675.h"

#ifdef CONFIG_VIDEO_ADP1653
#include "adp1653.h"
#endif

#ifdef CONFIG_VIDEO_AS3643
#include "as3643.h"
#endif


/* #define OV2675_DEBUG */

#define iprintk(format, arg...)	\
	printk(KERN_INFO"[%s]: "format"\n", __func__, ##arg)

/* OV2675 has only one fixed colorspace per pixelcode */
struct ov2675_datafmt {
	enum v4l2_mbus_pixelcode code;
	enum v4l2_colorspace colorspace;
};

struct ov2675_timing_cfg {
	u16 x_addr_start;
	u16 y_addr_start;
	u16 x_addr_end;
	u16 y_addr_end;
	u16 h_output_size;
	u16 v_output_size;
	u16 h_total_size;
	u16 v_total_size;
	u16 isp_h_offset;
	u16 isp_v_offset;
	u8 h_odd_ss_inc;
	u8 h_even_ss_inc;
	u8 v_odd_ss_inc;
	u8 v_even_ss_inc;
	u8 out_mode_sel;
	u8 sclk_dividers;
	u8 sys_mipi_clk;

};

static const struct ov2675_datafmt ov2675_fmts[] = {
	/*
	 * Order important: first natively supported,
	 *second supported with a GPIO extender
	 */
	{V4L2_MBUS_FMT_UYVY8_2X8, V4L2_COLORSPACE_JPEG},
	{V4L2_MBUS_FMT_YUYV8_2X8, V4L2_COLORSPACE_JPEG},
/*	{V4L2_MBUS_FMT_JPEG_1X8, V4L2_COLORSPACE_JPEG}, */

};

enum ov2675_size {
	OV2675_SIZE_QVGA,	/*  320 x 240 */
	OV2675_SIZE_VGA,	/*  640 x 480 */
	OV2675_SIZE_720P,
	OV2675_SIZE_1280x960,	/*  1280 x 960 (1.2M) */
	OV2675_SIZE_UXGA,	/*  1600 x 1200 (2M) */
	OV2675_SIZE_LAST,
	OV2675_SIZE_MAX
};

enum  cam_running_mode {
	CAM_RUNNING_MODE_NOTREADY,
	CAM_RUNNING_MODE_PREVIEW,
	CAM_RUNNING_MODE_CAPTURE,
	CAM_RUNNING_MODE_CAPTURE_DONE,
	CAM_RUNNING_MODE_RECORDING,
};
enum  cam_running_mode runmode;

static const struct v4l2_frmsize_discrete ov2675_frmsizes[OV2675_SIZE_LAST] = {
	{320, 240},
	{640, 480},
	{1280, 720},
	{1280, 960},
	{1600, 1200},
};

/* Find a data format by a pixel code in an array */
static int ov2675_find_datafmt(enum v4l2_mbus_pixelcode code)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ov2675_fmts); i++)
		if (ov2675_fmts[i].code == code)
			break;

	/* If not found, select latest */
	if (i >= ARRAY_SIZE(ov2675_fmts))
		i = ARRAY_SIZE(ov2675_fmts) - 1;

	return i;
}

/* Find a frame size in an array */
static int ov2675_find_framesize(u32 width, u32 height)
{
	int i;

	for (i = 0; i < OV2675_SIZE_LAST; i++) {
		if ((ov2675_frmsizes[i].width >= width) &&
		    (ov2675_frmsizes[i].height >= height))
			break;
	}

	/* If not found, select biggest */
	if (i >= OV2675_SIZE_LAST)
		i = OV2675_SIZE_LAST - 1;

	return i;
}

struct ov2675 {
	struct v4l2_subdev subdev;
	struct v4l2_subdev_sensor_interface_parms *plat_parms;
	int i_size;
	int i_fmt;
	int brightness;
	int contrast;
	int colorlevel;
	int sharpness;
	int saturation;
	int antibanding;
	int whitebalance;
	int framerate;
	int flashmode;
};

static int set_flash_mode(int, struct ov2675 *);
static int flash_gpio_strobe(int);

static struct ov2675 *to_ov2675(const struct i2c_client *client)
{
	return container_of(i2c_get_clientdata(client), struct ov2675, subdev);
}


static const struct ov2675_timing_cfg timing_cfg_yuv[OV2675_SIZE_LAST] = {
	[OV2675_SIZE_QVGA] = {
			/*  Timing control  2624 x 1952 --> 2592 x 1944 */
			      .x_addr_start = 16,
			      .y_addr_start = 4,
			      .x_addr_end = 2607,
			      .y_addr_end = 1947,
			/*  Output image size */
			      .h_output_size = 320,
			      .v_output_size = 240,
			/*  ISP Windowing size 1296 x 972 --> 1280 x 960 */
			      .isp_h_offset = 8,
			      .isp_v_offset = 6,
			/*  Total size (+blanking) */
			      .h_total_size = 2200,
			      .v_total_size = 1280,
			/*  Sensor Read Binning Enabled */
			      .h_odd_ss_inc = 3,
			      .h_even_ss_inc = 1,
			      .v_odd_ss_inc = 3,
			      .v_even_ss_inc = 1,
#ifdef CONFIG_MACH_HAWAII_GARNET
				  .out_mode_sel = 0x01,
#else
			      .out_mode_sel = 0x07,
#endif
			      .sclk_dividers = 0x01,
			      .sys_mipi_clk = 0x11,
			       },
	[OV2675_SIZE_VGA] = {
			/*  Timing control  2624 x 1952 --> 2592 x 1944 */
			      .x_addr_start = 16,
			      .y_addr_start = 4,
			      .x_addr_end = 2607,
			      .y_addr_end = 1947,
			/*  Output image size */
			      .h_output_size = 640,
			      .v_output_size = 480,
			/*  ISP Windowing size  1296 x 972 --> 1280 x 960 */
			      .isp_h_offset = 8,
			      .isp_v_offset = 6,
			/*  Total size (+blanking) */
			      .h_total_size = 2200,
			      .v_total_size = 1280,
			/*  Sensor Read Binning Enabled */
			      .h_odd_ss_inc = 3,
			      .h_even_ss_inc = 1,
			      .v_odd_ss_inc = 3,
			      .v_even_ss_inc = 1,
#ifdef CONFIG_MACH_HAWAII_GARNET
				  .out_mode_sel = 0x01,
#else
			      .out_mode_sel = 0x07,
#endif
			      .sclk_dividers = 0x01,
			      .sys_mipi_clk = 0x11,
			       },
	[OV2675_SIZE_720P] = {
			/*  Timing control  2624 x 1952 --> 2592 x 1944 */
			      .x_addr_start = 16,
			      .y_addr_start = 4,
			      .x_addr_end = 2607,
			      .y_addr_end = 1947,
			/*  Output image size */
			      .h_output_size = 1280,
			      .v_output_size = 720,
			/*  ISP Windowing size  1296 x 972 --> 1280 x 960 */
			      .isp_h_offset = 8,
			      .isp_v_offset = 6,
			/*  Total size (+blanking) */
			      .h_total_size = 2200,
			      .v_total_size = 1280,
			/*  Sensor Read Binning Enabled */
			      .h_odd_ss_inc = 3,
			      .h_even_ss_inc = 1,
			      .v_odd_ss_inc = 3,
			      .v_even_ss_inc = 1,
#ifdef CONFIG_MACH_HAWAII_GARNET
				  .out_mode_sel = 0x01,
#else
				  .out_mode_sel = 0x07,
#endif
			      .sclk_dividers = 0x01,
			      .sys_mipi_clk = 0x11,
			      },
	[OV2675_SIZE_1280x960] = {
			/*  Timing control  2624 x 1952 --> 2592 x 1944 */
			      .x_addr_start = 16,
			      .y_addr_start = 4,
			      .x_addr_end = 2607,
			      .y_addr_end = 1947,
			/*  Output image size */
			      .h_output_size = 1280,
			      .v_output_size = 960,
			/*  ISP Windowing size  1296 x 972 --> 1280 x 960 */
			      .isp_h_offset = 8,
			      .isp_v_offset = 6,
			/*  Total size (+blanking) */
			      .h_total_size = 2200,
			      .v_total_size = 1280,
			/*  Sensor Read Binning Enabled */
			      .h_odd_ss_inc = 3,
			      .h_even_ss_inc = 1,
			      .v_odd_ss_inc = 3,
			      .v_even_ss_inc = 1,
#ifdef CONFIG_MACH_HAWAII_GARNET
				  .out_mode_sel = 0x01,
#else
			      .out_mode_sel = 0x07,
#endif
			      .sclk_dividers = 0x01,
			      .sys_mipi_clk = 0x11,
			       },
	[OV2675_SIZE_UXGA] = {
			/*  Timing control  2624 x 1952 --> 2592 x 1944 */
			      .x_addr_start = 16,
			      .y_addr_start = 4,
			      .x_addr_end = 2607,
			      .y_addr_end = 1947,
			/*  Output image size */
			      .h_output_size = 1600,
			      .v_output_size = 1200,
			/*  ISP Windowing size	2592 x 1944 --> 2560 x 1920 */
			      .isp_h_offset = 16,
			      .isp_v_offset = 12,
			/*  Total size (+blanking) */
			      .h_total_size = 2844,
			      .v_total_size = 1968,
			/*  Sensor Read Binning Disabled */
			      .h_odd_ss_inc = 1,
			      .h_even_ss_inc = 1,
			      .v_odd_ss_inc = 1,
			      .v_even_ss_inc = 1,
#ifdef CONFIG_MACH_HAWAII_GARNET
				  .out_mode_sel = 0x00,
#else
			      .out_mode_sel = 0x06,
#endif
			      .sclk_dividers = 0x01,
			      .sys_mipi_clk = 0x12,
			      },

};

static const struct ov2675_timing_cfg timing_cfg_jpeg[OV2675_SIZE_LAST] = {
	[OV2675_SIZE_QVGA] = {
			/*  Timing control  2624 x 1952 --> 2592 x 1944 */
			      .x_addr_start = 16,
			      .y_addr_start = 4,
			      .x_addr_end = 2607,
			      .y_addr_end = 1947,
			/*  Output image size */
			      .h_output_size = 320,
			      .v_output_size = 240,
			/*  ISP Windowing size	2592 x 1944 --> 2560 x 1920 */
			      .isp_h_offset = 16,
			      .isp_v_offset = 12,
			/*  Total size (+blanking) */
			      .h_total_size = 2844,
			      .v_total_size = 1968,
			/*  Sensor Read Binning Disabled */
			      .h_odd_ss_inc = 1,
			      .h_even_ss_inc = 1,
			      .v_odd_ss_inc = 1,
			      .v_even_ss_inc = 1,
			      .out_mode_sel = 0x26,
			      .sclk_dividers = 0x01,
			      .sys_mipi_clk = 0x12,
			       },
	[OV2675_SIZE_VGA] = {
			/*  Timing control  2624 x 1952 --> 2592 x 1944 */
			      .x_addr_start = 16,
			      .y_addr_start = 4,
			      .x_addr_end = 2607,
			      .y_addr_end = 1947,
			/*  Output image size */
			      .h_output_size = 640,
			      .v_output_size = 480,
			/*  ISP Windowing size	2592 x 1944 --> 2560 x 1920 */
			      .isp_h_offset = 16,
			      .isp_v_offset = 12,
			/*  Total size (+blanking) */
			      .h_total_size = 2844,
			      .v_total_size = 1968,
			/*  Sensor Read Binning Disabled */
			      .h_odd_ss_inc = 1,
			      .h_even_ss_inc = 1,
			      .v_odd_ss_inc = 1,
			      .v_even_ss_inc = 1,
			      .out_mode_sel = 0x26,
			      .sclk_dividers = 0x01,
			      .sys_mipi_clk = 0x12,
			       },
	[OV2675_SIZE_720P] = {
			/*  Timing control  2624 x 1952 --> 2592 x 1944 */
			      .x_addr_start = 16,
			      .y_addr_start = 4,
			      .x_addr_end = 2607,
			      .y_addr_end = 1947,
			/*  Output image size */
			      .h_output_size = 1280,
			      .v_output_size = 720,
			/*  ISP Windowing size	2592 x 1944 --> 2560 x 1920 */
			      .isp_h_offset = 16,
			      .isp_v_offset = 12,
			/*  Total size (+blanking) */
			      .h_total_size = 2844,
			      .v_total_size = 1968,
			/*  Sensor Read Binning Disabled */
			      .h_odd_ss_inc = 1,
			      .h_even_ss_inc = 1,
			      .v_odd_ss_inc = 1,
			      .v_even_ss_inc = 1,
			      .out_mode_sel = 0x26,
			      .sclk_dividers = 0x01,
			      .sys_mipi_clk = 0x12,
			       },
	[OV2675_SIZE_1280x960] = {
			/*  Timing control  2624 x 1952 --> 2592 x 1944 */
			      .x_addr_start = 16,
			      .y_addr_start = 4,
			      .x_addr_end = 2607,
			      .y_addr_end = 1947,
			/*  Output image size */
			      .h_output_size = 1280,
			      .v_output_size = 960,
			/*  ISP Windowing size	2592 x 1944 --> 2560 x 1920 */
			      .isp_h_offset = 16,
			      .isp_v_offset = 12,
			/*  Total size (+blanking) */
			      .h_total_size = 2844,
			      .v_total_size = 1968,
			/*  Sensor Read Binning Disabled */
			      .h_odd_ss_inc = 1,
			      .h_even_ss_inc = 1,
			      .v_odd_ss_inc = 1,
			      .v_even_ss_inc = 1,
			      .out_mode_sel = 0x26,
			      .sclk_dividers = 0x01,
			      .sys_mipi_clk = 0x12,
			       },
	[OV2675_SIZE_UXGA] = {
			/*  Timing control  2624 x 1952 --> 2592 x 1944 */
			      .x_addr_start = 16,
			      .y_addr_start = 4,
			      .x_addr_end = 2607,
			      .y_addr_end = 1947,
			/*  Output image size */
			      .h_output_size = 1600,
			      .v_output_size = 1200,
			/*  ISP Windowing size	2592 x 1944 --> 2560 x 1920 */
			      .isp_h_offset = 16,
			      .isp_v_offset = 12,
			/*  Total size (+blanking) */
			      .h_total_size = 2844,
			      .v_total_size = 1968,
			/*  Sensor Read Binning Disabled */
			      .h_odd_ss_inc = 1,
			      .h_even_ss_inc = 1,
			      .v_odd_ss_inc = 1,
			      .v_even_ss_inc = 1,
			      .out_mode_sel = 0x26,
			      .sclk_dividers = 0x01,
			      .sys_mipi_clk = 0x12,
			       },

};


/**
 *ov2675_reg_read - Read a value from a register in an ov2675 sensor device
 *@client: i2c driver client structure
 *@reg: register address / offset
 *@val: stores the value that gets read
 *
 * Read a value from a register in an ov2675 sensor device.
 * The value is returned in 'val'.
 * Returns zero if successful, or non-zero otherwise.
 */
static int ov2675_reg_read(struct i2c_client *client, u16 reg, u8 *val)
{
	int ret;
	u8 data[2] = { 0 };
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = 0,
		.len = 2,
		.buf = data,
	};

	data[0] = (u8) (reg >> 8);
	data[1] = (u8) (reg & 0xff);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		goto err;

	msg.flags = I2C_M_RD;
	msg.len = 1;
	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		goto err;

	*val = data[0];
	return 0;

err:
	dev_err(&client->dev, "Failed reading register 0x%02x!\n", reg);
	return ret;
}

/**
 * Write a value to a register in ov2675 sensor device.
 *@client: i2c driver client structure.
 *@reg: Address of the register to read value from.
 *@val: Value to be written to a specific register.
 * Returns zero if successful, or non-zero otherwise.
 */
static int ov2675_reg_write(struct i2c_client *client, u16 reg, u8 val)
{
	int ret;
	unsigned char data[3] = { (u8) (reg >> 8), (u8) (reg & 0xff), val };
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = 0,
		.len = 3,
		.buf = data,
	};

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "Failed writing register 0x%02x!\n", reg);
		return ret;
	}

	return 0;
}

static const struct v4l2_queryctrl ov2675_controls[] = {
	{
	 .id = V4L2_CID_CAMERA_BRIGHTNESS,
	 .type = V4L2_CTRL_TYPE_INTEGER,
	 .name = "Brightness",
	 .minimum = EV_MINUS_1,
	 .maximum = EV_PLUS_1,
	 .step = 1,
	 .default_value = EV_DEFAULT,
	 },

	{
	 .id = V4L2_CID_CAMERA_EFFECT,
	 .type = V4L2_CTRL_TYPE_INTEGER,
	 .name = "Color Effects",
	 .minimum = IMAGE_EFFECT_NONE,
	 .maximum = (1 << IMAGE_EFFECT_NONE | 1 << IMAGE_EFFECT_SEPIA |
			 1 << IMAGE_EFFECT_BNW | 1 << IMAGE_EFFECT_NEGATIVE),
	 .step = 1,
	 .default_value = IMAGE_EFFECT_NONE,
	 },
	{
	 .id = V4L2_CID_CAMERA_ANTI_BANDING,
	 .type = V4L2_CTRL_TYPE_INTEGER,
	 .name = "Anti Banding",
	 .minimum = ANTI_BANDING_AUTO,
	 .maximum = ANTI_BANDING_60HZ,
	 .step = 1,
	 .default_value = ANTI_BANDING_AUTO,
	 },
	 {
	 .id = V4L2_CID_CAMERA_WHITE_BALANCE,
	 .type = V4L2_CTRL_TYPE_INTEGER,
	 .name = "White Balance",
	 .minimum = WHITE_BALANCE_AUTO,
	 .maximum = WHITE_BALANCE_FLUORESCENT,
	 .step = 1,
	 .default_value = WHITE_BALANCE_AUTO,
	 },
	 {
	 .id = V4L2_CID_CAMERA_FRAME_RATE,
	 .type = V4L2_CTRL_TYPE_INTEGER,
	 .name = "Framerate control",
	 .minimum = FRAME_RATE_AUTO,
	 .maximum = (1 << FRAME_RATE_AUTO | 1 << FRAME_RATE_15 |
				1 << FRAME_RATE_30),
	 .step = 1,
	 .default_value = FRAME_RATE_AUTO,
	 },

};

/**
 * Initialize a list of ov2675 registers.
 * The list of registers is terminated by the pair of values
 *@client: i2c driver client structure.
 *@reglist[]: List of address of the registers to write data.
 * Returns zero if successful, or non-zero otherwise.
 */
static int ov2675_reg_writes(struct i2c_client *client,
			const struct ov2675_reg reglist[])
{
	int err = 0, index;

	for (index = 0; ((reglist[index].reg != 0xFFFF) && (err == 0));
								index++) {
		err |=
			ov2675_reg_write(client, reglist[index].reg,
				     reglist[index].val);
		/*  Check for Pause condition */
		if ((reglist[index + 1].reg == 0xFFFF)
			&& (reglist[index + 1].val != 0)) {
			msleep(reglist[index + 1].val);
			index += 1;
		}
	}
	return 0;
}

static int ov2675_config_timing(struct i2c_client *client)
{
	struct ov2675 *ov2675 = to_ov2675(client);
	int ret, i = ov2675->i_size;
	const struct ov2675_timing_cfg *timing_cfg;

	if (ov2675_fmts[ov2675->i_fmt].code == V4L2_MBUS_FMT_JPEG_1X8)
		timing_cfg = &timing_cfg_jpeg[i];
	else
		timing_cfg = &timing_cfg_yuv[i];

	msleep(50);

	return ret;
}

static int ov2675_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov2675 *ov2675 = to_ov2675(client);
	int ret = 0;

	if (enable) {
		if ((ov2675->flashmode == FLASH_MODE_ON)
			|| (ov2675->flashmode == FLASH_MODE_AUTO))
			flash_gpio_strobe(1);
		/* Power Up, Start Streaming */
		ret = ov2675_reg_writes(client, ov2675_stream);
		if ((ov2675->flashmode == FLASH_MODE_ON)
			|| (ov2675->flashmode == FLASH_MODE_AUTO))
			flash_gpio_strobe(0);

		msleep(50);

	} else {
		/* Stop Streaming, Power Down*/
		ret = ov2675_reg_writes(client, ov2675_power_down);
	}

	return ret;
}

static int ov2675_set_bus_param(struct soc_camera_device *icd,
				unsigned long flags)
{
	/* TODO: Do the right thing here, and validate bus params */
	return 0;
}

static unsigned long ov2675_query_bus_param(struct soc_camera_device *icd)
{
	unsigned long flags = SOCAM_PCLK_SAMPLE_FALLING |
		SOCAM_HSYNC_ACTIVE_HIGH | SOCAM_VSYNC_ACTIVE_HIGH |
		SOCAM_DATA_ACTIVE_HIGH | SOCAM_MASTER;

	/* TODO: Do the right thing here, and validate bus params */

	flags |= SOCAM_DATAWIDTH_10;

	return flags;
}

static int ov2675_enum_input(struct soc_camera_device *icd,
			     struct v4l2_input *inp)
{
	struct soc_camera_link *icl = to_soc_camera_link(icd);
	struct v4l2_subdev_sensor_interface_parms *plat_parms;

	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->std = V4L2_STD_UNKNOWN;
	strcpy(inp->name, "ov2675");

	if (icl && icl->priv) {

		plat_parms = icl->priv;
		inp->status = 0;

		if (plat_parms->orientation == V4L2_SUBDEV_SENSOR_PORTRAIT)
			inp->status |= V4L2_IN_ST_HFLIP;

		if (plat_parms->facing == V4L2_SUBDEV_SENSOR_BACK)
			inp->status |= V4L2_IN_ST_BACK;

	}
	return 0;
}

static int ov2675_g_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov2675 *ov2675 = to_ov2675(client);

	mf->width = ov2675_frmsizes[ov2675->i_size].width;
	mf->height = ov2675_frmsizes[ov2675->i_size].height;
	mf->code = ov2675_fmts[ov2675->i_fmt].code;
	mf->colorspace = ov2675_fmts[ov2675->i_fmt].colorspace;
	mf->field = V4L2_FIELD_NONE;

	return 0;
}

static int ov2675_try_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{
	int i_fmt;
	int i_size;

	i_fmt = ov2675_find_datafmt(mf->code);

	mf->code = ov2675_fmts[i_fmt].code;
	mf->colorspace = ov2675_fmts[i_fmt].colorspace;
	mf->field = V4L2_FIELD_NONE;

	i_size = ov2675_find_framesize(mf->width, mf->height);

	mf->width = ov2675_frmsizes[i_size].width;
	mf->height = ov2675_frmsizes[i_size].height;

	return 0;
}

static int ov2675_s_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov2675 *ov2675 = to_ov2675(client);
	int ret = 0;

	ret = ov2675_try_fmt(sd, mf);
	if (ret < 0)
		return ret;

	ov2675->i_size = ov2675_find_framesize(mf->width, mf->height);
	ov2675->i_fmt = ov2675_find_datafmt(mf->code);

	/*To avoide reentry init sensor, remove from here	*/
	/*ret =  ov2675_reg_writes(client,configscript_common1);*/

	switch ((u32) ov2675_fmts[ov2675->i_fmt].code) {
	case V4L2_MBUS_FMT_UYVY8_2X8:

		break;
	case V4L2_MBUS_FMT_YUYV8_2X8:

		break;
	case V4L2_MBUS_FMT_JPEG_1X8:
		ret = ov2675_reg_writes(client, jpeg_init_common);
		if (ret)
			return ret;
		break;
	default:
		/* This shouldn't happen */
		ret = -EINVAL;
		return ret;
	}
	msleep(50);

	return ret;
}

static int ov2675_g_chip_ident(struct v4l2_subdev *sd,
			       struct v4l2_dbg_chip_ident *id)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (id->match.type != V4L2_CHIP_MATCH_I2C_ADDR)
		return -EINVAL;

	if (id->match.addr != client->addr)
		return -ENODEV;

	id->ident = V4L2_IDENT_OV5640;
	id->revision = 0;

	return 0;
}

static int ov2675_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov2675 *ov2675 = to_ov2675(client);

	dev_dbg(&client->dev, "ov2675_g_ctrl\n");

	switch (ctrl->id) {
	case V4L2_CID_CAMERA_BRIGHTNESS:
		ctrl->value = ov2675->brightness;
		break;
	case V4L2_CID_CAMERA_CONTRAST:
		ctrl->value = ov2675->contrast;
		break;
	case V4L2_CID_CAMERA_EFFECT:
		ctrl->value = ov2675->colorlevel;
		break;
	case V4L2_CID_SATURATION:
		ctrl->value = ov2675->saturation;
		break;
	case V4L2_CID_SHARPNESS:
		ctrl->value = ov2675->sharpness;
		break;
	case V4L2_CID_CAMERA_ANTI_BANDING:
		ctrl->value = ov2675->antibanding;
		break;
	case V4L2_CID_CAMERA_WHITE_BALANCE:
		ctrl->value = ov2675->whitebalance;
		break;
	case V4L2_CID_CAMERA_FRAME_RATE:
		ctrl->value = ov2675->framerate;
		break;
	case V4L2_CID_CAMERA_FLASH_MODE:
		ctrl->value = ov2675->flashmode;
		break;
	}

	return 0;
}

static int ov2675_preview_start(struct i2c_client *client)
{
	int ret = 0;
	printk(KERN_INFO "ov2675_preview_start!");
	ret = ov2675_reg_writes(client, configscript_common1);
	return ret;
}

static int ov2675_s_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov2675 *ov2675 = to_ov2675(client);
	u8 ov_reg;
	int ret = 0;

	dev_dbg(&client->dev, "ov2675_s_ctrl\n");

	switch (ctrl->id) {
	case V4L2_CID_CAMERA_BRIGHTNESS:

		if (ctrl->value > EV_PLUS_1)
			return -EINVAL;

		ov2675->brightness = ctrl->value;
		switch (ov2675->brightness) {
		case EV_MINUS_1:
			ret = ov2675_reg_writes(client,
					ov2675_brightness_lv4_tbl);
			break;
		case EV_PLUS_1:
			ret = ov2675_reg_writes(client,
					ov2675_brightness_lv0_tbl);
			break;
		default:
			ret = ov2675_reg_writes(client,
					ov2675_brightness_lv2_default_tbl);
			break;
		}
		if (ret)
			return ret;
		break;
	case V4L2_CID_CAMERA_CONTRAST:

		if (ctrl->value > CONTRAST_PLUS_1)
			return -EINVAL;

		ov2675->contrast = ctrl->value;
		switch (ov2675->contrast) {
		case CONTRAST_MINUS_1:
			ret = ov2675_reg_writes(client,
					ov2675_contrast_lv5_tbl);
			break;
		case CONTRAST_PLUS_1:
			ret = ov2675_reg_writes(client,
					ov2675_contrast_lv0_tbl);
			break;
		default:
			ret = ov2675_reg_writes(client,
					ov2675_contrast_default_lv3_tbl);
			break;
		}
		if (ret)
			return ret;
		break;
	case V4L2_CID_CAMERA_EFFECT:

		if (ctrl->value > IMAGE_EFFECT_BNW)
			return -EINVAL;

		ov2675->colorlevel = ctrl->value;

		switch (ov2675->colorlevel) {
		case IMAGE_EFFECT_BNW:
			ret = ov2675_reg_writes(client,
					ov2675_effect_bw_tbl);
			break;
		case IMAGE_EFFECT_SEPIA:
			ret = ov2675_reg_writes(client,
					ov2675_effect_sepia_tbl);
			break;
		case IMAGE_EFFECT_NEGATIVE:
			ret = ov2675_reg_writes(client,
					ov2675_effect_negative_tbl);
			break;
		default:
			ret = ov2675_reg_writes(client,
					ov2675_effect_normal_tbl);
			break;
		}
		if (ret)
			return ret;
		break;
	case V4L2_CID_SATURATION:

		if (ctrl->value > OV2675_SATURATION_MAX)
			return -EINVAL;

		ov2675->saturation = ctrl->value;
		switch (ov2675->saturation) {
		case OV2675_SATURATION_MIN:
			ret = ov2675_reg_writes(client,
					ov2675_saturation_lv0_tbl);
			break;
		case OV2675_SATURATION_MAX:
			ret = ov2675_reg_writes(client,
					ov2675_saturation_lv5_tbl);
			break;
		default:
			ret = ov2675_reg_writes(client,
					ov2675_saturation_default_lv3_tbl);
			break;
		}
		if (ret)
			return ret;
		break;
	case V4L2_CID_SHARPNESS:

		if (ctrl->value > OV2675_SHARPNESS_MAX)
			return -EINVAL;

		ov2675->sharpness = ctrl->value;
		switch (ov2675->sharpness) {
		case OV2675_SHARPNESS_MIN:
			ret = ov2675_reg_writes(client,
					ov2675_sharpness_lv0_tbl);
			break;
		case OV2675_SHARPNESS_MAX:
			ret = ov2675_reg_writes(client,
					ov2675_sharpness_lv3_tbl);
			break;
		default:
			ret = ov2675_reg_writes(client,
					ov2675_sharpness_default_lv2_tbl);
			break;
		}
		if (ret)
			return ret;
		break;

	case V4L2_CID_CAMERA_ANTI_BANDING:

		if (ctrl->value > ANTI_BANDING_60HZ)
			return -EINVAL;

		ov2675->antibanding = ctrl->value;

		switch (ov2675->antibanding) {
		case ANTI_BANDING_50HZ:
			ret = ov2675_reg_writes(client,
					ov2675_antibanding_50z_tbl);
			break;
		case ANTI_BANDING_60HZ:
			ret = ov2675_reg_writes(client,
					ov2675_antibanding_60z_tbl);
			break;
		default:
			ret = ov2675_reg_writes(client,
					ov2675_antibanding_auto_tbl);
			break;
		}
		if (ret)
			return ret;
		break;

	case V4L2_CID_CAMERA_WHITE_BALANCE:

		if (ctrl->value > WHITE_BALANCE_FLUORESCENT)
			return -EINVAL;

		ov2675->whitebalance = ctrl->value;
		break;

	case V4L2_CID_CAMERA_FRAME_RATE:

		if (ctrl->value > FRAME_RATE_30)
			return -EINVAL;

		ov2675->framerate = ctrl->value;

		switch (ov2675->framerate) {
		case FRAME_RATE_5:
			ret = ov2675_reg_writes(client,
					ov2675_fps_5);
			break;
		case FRAME_RATE_7:
			ret = ov2675_reg_writes(client,
					ov2675_fps_7);
			break;
		case FRAME_RATE_10:
			ret = ov2675_reg_writes(client,
					ov2675_fps_10);
			break;
		case FRAME_RATE_15:
			ret = ov2675_reg_writes(client,
					ov2675_fps_15);
			break;
		case FRAME_RATE_20:
			ret = ov2675_reg_writes(client,
					ov2675_fps_20);
			break;
		case FRAME_RATE_25:
			ret = ov2675_reg_writes(client,
					ov2675_fps_25);
			break;
		case FRAME_RATE_30:
		case FRAME_RATE_AUTO:
		default:
			ret = ov2675_reg_writes(client,
					ov2675_fps_30);
			break;
		}
		if (ret)
			return ret;
		break;

	case V4L2_CID_CAMERA_FLASH_MODE:
		set_flash_mode(ctrl->value, ov2675);
		break;

	case V4L2_CID_CAM_PREVIEW_ONOFF:
	{
		if (ctrl->value) {
			printk(KERN_INFO "ov2675 runmode = %d", runmode);
			if (runmode == CAM_RUNNING_MODE_NOTREADY)
				ov2675_preview_start(client);

			runmode = CAM_RUNNING_MODE_PREVIEW;
		} else
			runmode = CAM_RUNNING_MODE_NOTREADY;

		break;
	}

	case V4L2_CID_CAM_CAPTURE:
		runmode = CAM_RUNNING_MODE_CAPTURE;
		break;

	case V4L2_CID_CAM_CAPTURE_DONE:
		runmode = CAM_RUNNING_MODE_CAPTURE_DONE;
		break;

	}

	return ret;
}

int set_flash_mode(int mode, struct ov2675 *ov2675)
{
	if (ov2675->flashmode == mode)
		return 0;

#ifdef CONFIG_VIDEO_ADP1653
	if ((mode == FLASH_MODE_OFF) || (mode == FLASH_MODE_TORCH_OFF)) {
		if (ov2675->flashmode != FLASH_MODE_OFF) {
			adp1653_clear_all();
			adp1653_gpio_toggle(0);
			mode = FLASH_MODE_OFF;
		}
	} else if (mode == FLASH_MODE_TORCH_ON) {
		if ((ov2675->flashmode == FLASH_MODE_ON)
			|| (ov2675->flashmode == FLASH_MODE_AUTO))
			set_flash_mode(FLASH_MODE_OFF, ov2675);
		adp1653_gpio_toggle(1);
		adp1653_gpio_strobe(0);
		usleep_range(30, 31);
		adp1653_set_timer(1, 0);
		adp1653_set_ind_led(1);
		/* Torch current no indicator LED */
		adp1653_set_torch_flash(10);
		adp1653_sw_strobe(1);
	} else if (mode == FLASH_MODE_ON) {
		if ((ov2675->flashmode == FLASH_MODE_TORCH_ON)
			|| (ov2675->flashmode == FLASH_MODE_AUTO))
			set_flash_mode(FLASH_MODE_OFF, ov2675);
		adp1653_gpio_strobe(0);
		adp1653_gpio_toggle(1);
		usleep_range(30, 31);
		adp1653_set_timer(1, 0x5);
		adp1653_set_ind_led(1);
		/* Flash current indicator LED ON */
		adp1653_set_torch_flash(28);
		/* Strobing should hapen later */
	} else if (mode == FLASH_MODE_AUTO) {
		if ((ov2675->flashmode == FLASH_MODE_TORCH_ON)
			|| (ov2675->flashmode == FLASH_MODE_ON))
			set_flash_mode(FLASH_MODE_OFF, ov2675);
		adp1653_gpio_strobe(0);
		adp1653_gpio_toggle(1);
		usleep_range(30, 31);
		adp1653_set_timer(1, 0x5);
		adp1653_set_ind_led(1);
		/* Flash current indicator LED ON */
		adp1653_set_torch_flash(28);
		/* Camera sensor will strobe if required */
	} else {
		return -EINVAL;
	}
	ov2675->flashmode = mode;
#endif

#ifdef CONFIG_VIDEO_AS3643
	if ((mode == FLASH_MODE_OFF) || (mode == FLASH_MODE_TORCH_OFF)) {
		if (ov2675->flashmode != FLASH_MODE_OFF) {
			as3643_clear_all();
			as3643_gpio_toggle(0);
		}
	} else if (mode == FLASH_MODE_TORCH_ON) {
		as3643_gpio_toggle(1);
		usleep_range(25, 30);
		as3643_set_torch_flash(0x80);
	} else if (mode == FLASH_MODE_ON) {
		as3643_gpio_toggle(1);
		usleep_range(25, 30);
		as3643_set_ind_led(0x80);
	} else if (mode == FLASH_MODE_AUTO) {
		/* Not yet implemented */
	} else {
		return -EINVAL;
	}
	ov2675->flashmode = mode;
#endif
	return 0;
}

static int flash_gpio_strobe(int on)
{
#ifdef CONFIG_VIDEO_ADP1653
	return adp1653_gpio_strobe(on);
#endif
#ifdef CONFIG_VIDEO_AS3643
	return 0;
#endif
}

static long ov2675_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	int ret = 0;

	switch (cmd) {
	case VIDIOC_THUMB_SUPPORTED:
		{
			int *p = arg;
			*p = 0;	/* no we don't support thumbnail */
			break;
		}
	case VIDIOC_JPEG_G_PACKET_INFO:
		{
			struct v4l2_jpeg_packet_info *p =
				(struct v4l2_jpeg_packet_info *)arg;
			p->padded = 0;
			p->packet_size = 0x400;
			break;
		}

	case VIDIOC_SENSOR_G_OPTICAL_INFO:
		{
			struct v4l2_sensor_optical_info *p =
				(struct v4l2_sensor_optical_info *)arg;
			/* assuming 67.5 degree diagonal viewing angle */
			p->hor_angle.numerator = 5401;
			p->hor_angle.denominator = 100;
			p->ver_angle.numerator = 3608;
			p->ver_angle.denominator = 100;
			p->focus_distance[0] = 10; /* near focus in cm */
			p->focus_distance[1] = 100; /* optimal focus in cm */
			p->focus_distance[2] = -1; /* infinity */
			p->focal_length.numerator = 342;
			p->focal_length.denominator = 100;
			break;
		}
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int ov2675_g_register(struct v4l2_subdev *sd,
			     struct v4l2_dbg_register *reg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (reg->match.type != V4L2_CHIP_MATCH_I2C_ADDR || reg->size > 2)
		return -EINVAL;

	if (reg->match.addr != client->addr)
		return -ENODEV;

	reg->size = 2;
	if (ov2675_reg_read(client, reg->reg, &reg->val))
		return -EIO return 0;
}

static int ov2675_s_register(struct v4l2_subdev *sd,
			     struct v4l2_dbg_register *reg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (reg->match.type != V4L2_CHIP_MATCH_I2C_ADDR || reg->size > 2)
		return -EINVAL;

	if (reg->match.addr != client->addr)
		return -ENODEV;

	if (ov2675_reg_write(client, reg->reg, reg->val))
		return -EIO;

	return 0;
}
#endif

static struct soc_camera_ops ov2675_ops = {
	.set_bus_param = ov2675_set_bus_param,
	.query_bus_param = ov2675_query_bus_param,
	.enum_input = ov2675_enum_input,
	.controls = ov2675_controls,
	.num_controls = ARRAY_SIZE(ov2675_controls),
};

static int ov2675_init(struct i2c_client *client)
{
	struct ov2675 *ov2675 = to_ov2675(client);
	int ret = 0;
	printk(KERN_ERR "ov2675_init\n");
	ret = ov2675_reg_writes(client, configscript_common1);
	if (ret)
		goto out;

	/* Power Up, Start Streaming for AF Init*/
	ret = ov2675_reg_writes(client, ov2675_stream);
	if (ret)
		goto out;
	/* Delay for sensor streaming*/
	msleep(20);


	/* Stop Streaming, Power Down*/
	ret = ov2675_reg_writes(client, ov2675_power_down);

	/* default brightness and contrast */
	ov2675->brightness = EV_DEFAULT;
	ov2675->contrast = CONTRAST_DEFAULT;
	ov2675->colorlevel = IMAGE_EFFECT_NONE;
	ov2675->antibanding = ANTI_BANDING_AUTO;
	ov2675->whitebalance = WHITE_BALANCE_AUTO;
	ov2675->framerate = FRAME_RATE_AUTO;

	dev_dbg(&client->dev, "Sensor initialized\n");

out:
	return ret;
}

/*
 * Interface active, can use i2c. If it fails, it can indeed mean, that
 *this wasn't our capture interface, so, we wait for the right one
 */
static int ov2675_video_probe(struct soc_camera_device *icd,
			      struct i2c_client *client)
{
	unsigned long flags;
	int ret = 0;
	u8 revision = 0;
	printk(KERN_ERR "ov2675_video_probe\n");
	/*
	 * We must have a parent by now. And it cannot be a wrong one.
	 * So this entire test is completely redundant.
	 */
	client->addr = 0x30;

	if (!icd->dev.parent ||
	    to_soc_camera_host(icd->dev.parent)->nr != icd->iface)
		return -ENODEV;
/*
	ret = ov2675_reg_read(client, 0x302A, &revision);
	if (ret) {
		dev_err(&client->dev, "Failure to detect OV2675 chip\n");
		goto out;
	}
	printk(KERN_ERR "OV2675 value read=%x\n", revision);

	revision &= 0xF;
*/
	flags = SOCAM_DATAWIDTH_8;

	dev_info(&client->dev, "Detected a OV2675 chip, revision %x\n",
		 revision);

	/* TODO: Do something like ov2675_init */

out:
	return ret;
}

static void ov2675_video_remove(struct soc_camera_device *icd)
{
	dev_dbg(&icd->dev, "Video removed: %p, %p\n",
		icd->dev.parent, icd->vdev);
}

static struct v4l2_subdev_core_ops ov2675_subdev_core_ops = {
	.g_chip_ident = ov2675_g_chip_ident,
	.g_ctrl = ov2675_g_ctrl,
	.s_ctrl = ov2675_s_ctrl,
	.ioctl = ov2675_ioctl,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register = ov2675_g_register,
	.s_register = ov2675_s_register,
#endif
};

static int ov2675_enum_fmt(struct v4l2_subdev *sd, unsigned int index,
			   enum v4l2_mbus_pixelcode *code)
{
	if (index >= ARRAY_SIZE(ov2675_fmts))
		return -EINVAL;

	*code = ov2675_fmts[index].code;
	return 0;
}

static int ov2675_enum_framesizes(struct v4l2_subdev *sd,
				  struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index >= OV2675_SIZE_LAST)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->pixel_format = V4L2_PIX_FMT_UYVY;

	fsize->discrete = ov2675_frmsizes[fsize->index];

	return 0;
}

/* we only support fixed frame rate */
static int ov2675_enum_frameintervals(struct v4l2_subdev *sd,
				      struct v4l2_frmivalenum *interval)
{
	int size;

	if (interval->index >= 1)
		return -EINVAL;

	interval->type = V4L2_FRMIVAL_TYPE_DISCRETE;

	size = ov2675_find_framesize(interval->width, interval->height);

	switch (size) {
	case OV2675_SIZE_UXGA:
		interval->discrete.numerator = 1;
		interval->discrete.denominator = 15;
		break;

	case OV2675_SIZE_VGA:
	default:
		interval->discrete.numerator = 1;
		interval->discrete.denominator = 24;
		break;
	}
/*	printk(KERN_ERR"%s: width=%d height=%d fi=%d/%d\n", __func__,
			interval->width,
			interval->height, interval->discrete.numerator,
			interval->discrete.denominator);
			*/
	return 0;
}

static int ov2675_g_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov2675 *ov2675 = to_ov2675(client);
	struct v4l2_captureparm *cparm;

	if (param->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	cparm = &param->parm.capture;

	memset(param, 0, sizeof(*param));
	param->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	cparm->capability = V4L2_CAP_TIMEPERFRAME;

	switch (ov2675->i_size) {
	case OV2675_SIZE_UXGA:
		cparm->timeperframe.numerator = 1;
		cparm->timeperframe.denominator = 15;
		break;
	case OV2675_SIZE_VGA:
	default:
		cparm->timeperframe.numerator = 1;
		cparm->timeperframe.denominator = 24;
		break;
	}

	return 0;
}
static int ov2675_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{
	/*
	 * FIXME: This just enforces the hardcoded framerates until this is
	 *flexible enough.
	 */
	return ov2675_g_parm(sd, param);
}

static struct v4l2_subdev_video_ops ov2675_subdev_video_ops = {
	.s_stream = ov2675_s_stream,
	.s_mbus_fmt = ov2675_s_fmt,
	.g_mbus_fmt = ov2675_g_fmt,
	.try_mbus_fmt = ov2675_try_fmt,
	.enum_mbus_fmt = ov2675_enum_fmt,
	.enum_mbus_fsizes = ov2675_enum_framesizes,
	.enum_framesizes = ov2675_enum_framesizes,
	.enum_frameintervals = ov2675_enum_frameintervals,
	.g_parm = ov2675_g_parm,
	.s_parm = ov2675_s_parm,
};
static int ov2675_g_skip_frames(struct v4l2_subdev *sd, u32 *frames)
{
	/* Quantity of initial bad frames to skip. Revisit. */
	/*Waitting for AWB stability,  avoid green color issue*/
	*frames = 5;

	return 0;
}

static int ov2675_g_interface_parms(struct v4l2_subdev *sd,
				    struct v4l2_subdev_sensor_interface_parms
				    *parms)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov2675 *ov2675 = to_ov2675(client);
	u8 sclk_dividers;

	if (!parms)
		return -EINVAL;

	parms->if_type = ov2675->plat_parms->if_type;
	parms->if_mode = ov2675->plat_parms->if_mode;
	parms->parms = ov2675->plat_parms->parms;

	/* set the hs term time */
	if (ov2675_fmts[ov2675->i_fmt].code == V4L2_MBUS_FMT_JPEG_1X8)
		sclk_dividers  = timing_cfg_jpeg[ov2675->i_size].sclk_dividers;
	else
		sclk_dividers = timing_cfg_yuv[ov2675->i_size].sclk_dividers;

	if (sclk_dividers == 0x01)
		parms->parms.serial.hs_term_time = 0x01;
	else
		parms->parms.serial.hs_term_time = 0x08;

	switch (ov2675->framerate) {
	case FRAME_RATE_5:
		parms->parms.serial.hs_settle_time = 9;
		break;
	case FRAME_RATE_7:
		parms->parms.serial.hs_settle_time = 6;
		break;
	case FRAME_RATE_10:
	case FRAME_RATE_15:
	case FRAME_RATE_25:
	case FRAME_RATE_30:
	case FRAME_RATE_AUTO:
	default:
		parms->parms.serial.hs_settle_time = 2;
		break;
	}

	return 0;
}



static struct v4l2_subdev_sensor_ops ov2675_subdev_sensor_ops = {
	.g_skip_frames = ov2675_g_skip_frames,
	.g_interface_parms = ov2675_g_interface_parms,
};

static struct v4l2_subdev_ops ov2675_subdev_ops = {
	.core = &ov2675_subdev_core_ops,
	.video = &ov2675_subdev_video_ops,
	.sensor = &ov2675_subdev_sensor_ops,
};

static int ov2675_probe(struct i2c_client *client,
			const struct i2c_device_id *did)
{
	struct ov2675 *ov2675;
	struct soc_camera_device *icd = client->dev.platform_data;
	struct soc_camera_link *icl;
	int ret;

	client->addr = 0x30;

	printk(KERN_ERR "OV2675 probe\n");
	if (!icd) {
		dev_err(&client->dev, "OV2675: missing soc-camera data!\n");
		return -EINVAL;
	}

	icl = to_soc_camera_link(icd);
	if (!icl) {
		dev_err(&client->dev, "OV2675 driver needs platform data\n");
		return -EINVAL;
	}

	if (!icl->priv) {
		dev_err(&client->dev,
			"OV2675 driver needs i/f platform data\n");
		return -EINVAL;
	}

	ov2675 = kzalloc(sizeof(struct ov2675), GFP_KERNEL);
	if (!ov2675)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&ov2675->subdev, client, &ov2675_subdev_ops);

	/* Second stage probe - when a capture adapter is there */
	icd->ops = &ov2675_ops;

	ov2675->i_size = OV2675_SIZE_VGA;
	ov2675->i_fmt = 0;	/* First format in the list */
	ov2675->plat_parms = icl->priv;

	ret = ov2675_video_probe(icd, client);
	if (ret) {
		icd->ops = NULL;
		kfree(ov2675);
		return ret;
	}

	/* init the sensor here */
	ret = ov2675_init(client);
	if (ret) {
		dev_err(&client->dev, "Failed to initialize sensor\n");
		ret = -EINVAL;
	}

	return ret;
}

static int ov2675_remove(struct i2c_client *client)
{
	struct ov2675 *ov2675 = to_ov2675(client);
	struct soc_camera_device *icd = client->dev.platform_data;

	icd->ops = NULL;
	ov2675_video_remove(icd);
	client->driver = NULL;
	kfree(ov2675);

	return 0;
}

static const struct i2c_device_id ov2675_id[] = {
	{"ov2675", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, ov2675_id);

static struct i2c_driver ov2675_i2c_driver = {
	.driver = {
		   .name = "ov2675",
		   },
	.probe = ov2675_probe,
	.remove = ov2675_remove,
	.id_table = ov2675_id,
};

static int __init ov2675_mod_init(void)
{
	return i2c_add_driver(&ov2675_i2c_driver);
}

static void __exit ov2675_mod_exit(void)
{
	i2c_del_driver(&ov2675_i2c_driver);
}

module_init(ov2675_mod_init);
module_exit(ov2675_mod_exit);

MODULE_DESCRIPTION("OmniVision OV2675 Camera driver");
MODULE_AUTHOR("Sergio Aguirre <saaguirre@ti.com>");
MODULE_LICENSE("GPL v2");
