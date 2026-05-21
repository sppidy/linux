// SPDX-License-Identifier: GPL-2.0
/*
 * Himax HM1092 image sensor driver draft.
 *
 * Register tables were extracted from Qualcomm Chromatix sensor module
 * com.qti.sensormodule.hm1092.bin. Keep hm1092_regs.h next to this file, or
 * fold the generated tables into this source before upstream submission.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#include "hm1092_regs.h"

#define HM1092_LINK_FREQ_400MHZ		400000000ULL
#define HM1092_MCLK			24000000
#define HM1092_BITS_PER_SAMPLE		10

#define HM1092_REG_STREAM		CCI_REG8(0x0100)

struct hm1092_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts;
};

static const struct hm1092_mode hm1092_mode_1280x720 = {
	.width = 1280,
	.height = 720,
	.hts = 0x0650,
	.vts = 0x02ee,
};

static const char * const hm1092_supply_names[] = {
	"dovdd",
	"avdd",
	"dvdd",
};

static const char * const hm1092_test_pattern_menu[] = {
	"Disabled",
	"Mode 1",
	"Mode 2",
	"Mode 3",
	"Mode 4",
};

static const s64 hm1092_link_freq_menu[] = {
	HM1092_LINK_FREQ_400MHZ,
};

struct hm1092 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct regmap *regmap;
	struct clk *img_clk;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[ARRAY_SIZE(hm1092_supply_names)];
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	u8 mipi_lanes;
};

static inline struct hm1092 *to_hm1092(struct v4l2_subdev *sd)
{
	return container_of(sd, struct hm1092, sd);
}

static int hm1092_write_regs(struct hm1092 *hm1092,
			     const struct hm1092_reg *regs, unsigned int len)
{
	int ret = 0;
	unsigned int i;

	for (i = 0; i < len; i++) {
		cci_write(hm1092->regmap, CCI_REG8(regs[i].address),
			  regs[i].val, &ret);
		if (ret)
			return ret;
	}

	return 0;
}

static int hm1092_set_test_pattern(struct hm1092 *hm1092, int pattern)
{
	switch (pattern) {
	case 0:
		return hm1092_write_regs(hm1092, hm1092_test_pattern_mode0,
					 ARRAY_SIZE(hm1092_test_pattern_mode0));
	case 1:
		return hm1092_write_regs(hm1092, hm1092_test_pattern_mode1,
					 ARRAY_SIZE(hm1092_test_pattern_mode1));
	case 2:
		return hm1092_write_regs(hm1092, hm1092_test_pattern_mode2,
					 ARRAY_SIZE(hm1092_test_pattern_mode2));
	case 3:
		return hm1092_write_regs(hm1092, hm1092_test_pattern_mode3,
					 ARRAY_SIZE(hm1092_test_pattern_mode3));
	case 4:
		return hm1092_write_regs(hm1092, hm1092_test_pattern_mode4,
					 ARRAY_SIZE(hm1092_test_pattern_mode4));
	default:
		return -EINVAL;
	}
}

static int hm1092_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct hm1092 *hm1092 = container_of(ctrl->handler, struct hm1092,
					     ctrl_handler);
	int ret = 0;

	if (!pm_runtime_get_if_in_use(hm1092->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		ret = hm1092_set_test_pattern(hm1092, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
	case V4L2_CID_EXPOSURE:
		/* TODO: write to the sensor's exposure/gain registers once
		 * we know which Chromatix middle*Addr fields point at them.
		 */
		ret = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(hm1092->dev);

	return ret;
}

static const struct v4l2_ctrl_ops hm1092_ctrl_ops = {
	.s_ctrl = hm1092_set_ctrl,
};

static int hm1092_init_controls(struct hm1092 *hm1092)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &hm1092->ctrl_handler;
	const struct hm1092_mode *mode = &hm1092_mode_1280x720;
	struct v4l2_fwnode_device_properties props;
	s64 hblank, pixel_rate;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 6);

	hm1092->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr,
						   &hm1092_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   0, 0,
						   hm1092_link_freq_menu);
	if (hm1092->link_freq)
		hm1092->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = div_u64(HM1092_LINK_FREQ_400MHZ * 2 * hm1092->mipi_lanes,
			     HM1092_BITS_PER_SAMPLE);
	hm1092->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &hm1092_ctrl_ops,
					       V4L2_CID_PIXEL_RATE, 0,
					       pixel_rate, 1, pixel_rate);

	hblank = mode->hts - mode->width;
	hm1092->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &hm1092_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank, 1,
					   hblank);
	if (hm1092->hblank)
		hm1092->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	hm1092->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &hm1092_ctrl_ops,
					   V4L2_CID_VBLANK,
					   mode->vts - mode->height,
					   0xffff - mode->height, 1,
					   mode->vts - mode->height);
	if (hm1092->vblank)
		hm1092->vblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* Mandatory controls for libcamera. Conservative defaults until we
	 * RE the exposure/gain register address layout from the Chromatix
	 * sensormodule (middleCoarseIntgTimeAddr / shortGlobalGainAddr).
	 */
	v4l2_ctrl_new_std(ctrl_hdlr, &hm1092_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  0x10, 0xff, 1, 0x10);
	v4l2_ctrl_new_std(ctrl_hdlr, &hm1092_ctrl_ops, V4L2_CID_EXPOSURE,
			  1, mode->vts - 4, 1, mode->vts - 4);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &hm1092_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(hm1092_test_pattern_menu) - 1,
				     0, 0, hm1092_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(hm1092->dev, &props);
	if (ret)
		return ret;

	v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &hm1092_ctrl_ops, &props);

	if (ctrl_hdlr->error)
		return ctrl_hdlr->error;

	hm1092->sd.ctrl_handler = ctrl_hdlr;

	return 0;
}

static void hm1092_update_pad_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = hm1092_mode_1280x720.width;
	fmt->height = hm1092_mode_1280x720.height;
	fmt->code = MEDIA_BUS_FMT_Y10_1X10;
	fmt->field = V4L2_FIELD_NONE;
}

static int hm1092_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct hm1092 *hm1092 = to_hm1092(sd);
	int ret;

	ret = pm_runtime_resume_and_get(hm1092->dev);
	if (ret)
		return ret;

	ret = hm1092_write_regs(hm1092, hm1092_init_regs,
				ARRAY_SIZE(hm1092_init_regs));
	if (ret) {
		dev_err(hm1092->dev, "failed to write init registers\n");
		goto out;
	}

	ret = __v4l2_ctrl_handler_setup(hm1092->sd.ctrl_handler);
	if (ret)
		goto out;

	ret = hm1092_write_regs(hm1092, hm1092_start_streaming,
				ARRAY_SIZE(hm1092_start_streaming));
	if (ret)
		dev_err(hm1092->dev, "failed to start streaming\n");

out:
	if (ret)
		pm_runtime_put(hm1092->dev);

	return ret;
}

static int hm1092_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct hm1092 *hm1092 = to_hm1092(sd);
	int ret = 0;

	cci_write(hm1092->regmap, HM1092_REG_STREAM, 0, &ret);
	pm_runtime_put(hm1092->dev);

	return ret;
}

static int hm1092_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	hm1092_update_pad_format(&fmt->format);
	*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;

	return 0;
}

static int hm1092_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_Y10_1X10;

	return 0;
}

static int hm1092_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index)
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_Y10_1X10)
		return -EINVAL;

	fse->min_width = hm1092_mode_1280x720.width;
	fse->max_width = hm1092_mode_1280x720.width;
	fse->min_height = hm1092_mode_1280x720.height;
	fse->max_height = hm1092_mode_1280x720.height;

	return 0;
}

static int hm1092_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	hm1092_update_pad_format(v4l2_subdev_state_get_format(state, 0));

	return 0;
}

static const struct v4l2_subdev_video_ops hm1092_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops hm1092_pad_ops = {
	.set_fmt = hm1092_set_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.enum_mbus_code = hm1092_enum_mbus_code,
	.enum_frame_size = hm1092_enum_frame_size,
	.enable_streams = hm1092_enable_streams,
	.disable_streams = hm1092_disable_streams,
};

static const struct v4l2_subdev_ops hm1092_subdev_ops = {
	.video = &hm1092_video_ops,
	.pad = &hm1092_pad_ops,
};

static const struct media_entity_operations hm1092_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops hm1092_internal_ops = {
	.init_state = hm1092_init_state,
};

static int hm1092_check_hwcfg(struct hm1092 *hm1092)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct device *dev = hm1092->dev;
	struct fwnode_handle *ep, *fwnode = dev_fwnode(dev);
	unsigned long link_freq_bitmap;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, 0, 0, 0);
	if (!ep)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "waiting for fwnode graph endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(dev, ret, "parsing endpoint failed\n");

	ret = v4l2_link_freq_to_bitmap(dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       hm1092_link_freq_menu,
				       ARRAY_SIZE(hm1092_link_freq_menu),
				       &link_freq_bitmap);
	if (ret)
		goto out;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != 1 &&
	    bus_cfg.bus.mipi_csi2.num_data_lanes != 2) {
		ret = dev_err_probe(dev, -EINVAL,
				    "unsupported CSI2 data lanes: %u\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
		goto out;
	}

	hm1092->mipi_lanes = bus_cfg.bus.mipi_csi2.num_data_lanes;

out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int hm1092_get_pm_resources(struct hm1092 *hm1092)
{
	unsigned int i;

	hm1092->reset = devm_gpiod_get_optional(hm1092->dev, "reset",
						GPIOD_OUT_HIGH);
	if (IS_ERR(hm1092->reset))
		return dev_err_probe(hm1092->dev, PTR_ERR(hm1092->reset),
				     "failed to get reset gpio\n");

	for (i = 0; i < ARRAY_SIZE(hm1092_supply_names); i++)
		hm1092->supplies[i].supply = hm1092_supply_names[i];

	return devm_regulator_bulk_get(hm1092->dev,
				       ARRAY_SIZE(hm1092_supply_names),
				       hm1092->supplies);
}

static int hm1092_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hm1092 *hm1092 = to_hm1092(sd);

	gpiod_set_value_cansleep(hm1092->reset, 1);
	regulator_bulk_disable(ARRAY_SIZE(hm1092_supply_names),
			       hm1092->supplies);
	clk_disable_unprepare(hm1092->img_clk);

	return 0;
}

static int hm1092_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hm1092 *hm1092 = to_hm1092(sd);
	int ret;

	/*
	 * Sequence reverse-engineered from the Chromatix AeoB powerSetting:
	 *   1. enable all rails (~1 ms ramp per supply)
	 *   2. hold reset asserted
	 *   3. start MCLK and let the sensor clock for ~1 ms
	 *   4. release reset and wait 18 ms for the sensor to come up
	 */
	ret = regulator_bulk_enable(ARRAY_SIZE(hm1092_supply_names),
				    hm1092->supplies);
	if (ret)
		return ret;
	usleep_range(3000, 3500);

	if (hm1092->reset)
		gpiod_set_value_cansleep(hm1092->reset, 1);

	ret = clk_prepare_enable(hm1092->img_clk);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(hm1092_supply_names),
				       hm1092->supplies);
		return ret;
	}
	usleep_range(1000, 1200);

	if (hm1092->reset)
		gpiod_set_value_cansleep(hm1092->reset, 0);
	msleep(18);

	return 0;
}

static void hm1092_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct hm1092 *hm1092 = to_hm1092(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(hm1092->dev);
	if (!pm_runtime_status_suspended(hm1092->dev)) {
		hm1092_power_off(hm1092->dev);
		pm_runtime_set_suspended(hm1092->dev);
	}
}

static int hm1092_probe(struct i2c_client *client)
{
	struct hm1092 *hm1092;
	unsigned long freq;
	int ret;

	hm1092 = devm_kzalloc(&client->dev, sizeof(*hm1092), GFP_KERNEL);
	if (!hm1092)
		return -ENOMEM;

	hm1092->dev = &client->dev;

	hm1092->img_clk = devm_v4l2_sensor_clk_get(hm1092->dev, NULL);
	if (IS_ERR(hm1092->img_clk))
		return dev_err_probe(hm1092->dev, PTR_ERR(hm1092->img_clk),
				     "failed to get imaging clock\n");

	freq = clk_get_rate(hm1092->img_clk);
	if (freq != HM1092_MCLK)
		return dev_err_probe(hm1092->dev, -EINVAL,
				     "external clock %lu is not supported\n",
				     freq);

	v4l2_i2c_subdev_init(&hm1092->sd, client, &hm1092_subdev_ops);

	ret = hm1092_check_hwcfg(hm1092);
	if (ret)
		return ret;

	ret = hm1092_get_pm_resources(hm1092);
	if (ret)
		return ret;

	hm1092->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(hm1092->regmap))
		return PTR_ERR(hm1092->regmap);

	ret = hm1092_power_on(hm1092->dev);
	if (ret)
		return dev_err_probe(hm1092->dev, ret, "failed to power on\n");

	ret = hm1092_init_controls(hm1092);
	if (ret)
		goto err_power_off;

	hm1092->sd.internal_ops = &hm1092_internal_ops;
	hm1092->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	hm1092->sd.entity.ops = &hm1092_entity_ops;
	hm1092->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	hm1092->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&hm1092->sd.entity, 1, &hm1092->pad);
	if (ret)
		goto err_ctrls;

	hm1092->sd.state_lock = hm1092->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&hm1092->sd);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(hm1092->dev);
	pm_runtime_enable(hm1092->dev);

	ret = v4l2_async_register_subdev_sensor(&hm1092->sd);
	if (ret)
		goto err_subdev;

	pm_runtime_idle(hm1092->dev);
	return 0;

err_subdev:
	pm_runtime_disable(hm1092->dev);
	pm_runtime_set_suspended(hm1092->dev);
	v4l2_subdev_cleanup(&hm1092->sd);
err_entity:
	media_entity_cleanup(&hm1092->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(hm1092->sd.ctrl_handler);
err_power_off:
	hm1092_power_off(hm1092->dev);

	return ret;
}

static DEFINE_RUNTIME_DEV_PM_OPS(hm1092_pm_ops, hm1092_power_off,
				 hm1092_power_on, NULL);

static const struct of_device_id hm1092_of_match[] = {
	{ .compatible = "himax,hm1092" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hm1092_of_match);

static struct i2c_driver hm1092_i2c_driver = {
	.driver = {
		.name = "hm1092",
		.pm = pm_sleep_ptr(&hm1092_pm_ops),
		.of_match_table = hm1092_of_match,
	},
	.probe = hm1092_probe,
	.remove = hm1092_remove,
};

module_i2c_driver(hm1092_i2c_driver);

MODULE_DESCRIPTION("Himax HM1092 sensor driver draft");
MODULE_LICENSE("GPL");
