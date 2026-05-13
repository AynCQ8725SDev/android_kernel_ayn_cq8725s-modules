#include <linux/device.h>
#include <linux/irq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/completion.h>
#include <linux/timer.h>
#include <linux/pm_wakeup.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>
#include <linux/of_irq.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/led-class-multicolor.h>
#include <linux/soc/qcom/panel_event_notifier.h>

#define SN3112_SHUTDOWN  	0x00
#define 	SN3112_SHUTDOWN_OFF		0
#define 	SN3112_SHUTDOWN_ON		1
#define SN3112_PWM(idx) 	(0x04 + idx)
#define SN3112_CTRL0		0x13
#define 	SN3112_CTRL0_CH0_ENABLE (1 << 3)
#define 	SN3112_CTRL0_CH1_ENABLE (1 << 4)
#define 	SN3112_CTRL0_CH2_ENABLE (1 << 5)
#define SN3112_CTRL1		0x14
#define 	SN3112_CTRL1_CH3_ENABLE (1 << 0)
#define 	SN3112_CTRL1_CH4_ENABLE (1 << 1)
#define 	SN3112_CTRL1_CH5_ENABLE (1 << 2)
#define 	SN3112_CTRL1_CH6_ENABLE (1 << 3)
#define 	SN3112_CTRL1_CH7_ENABLE (1 << 4)
#define 	SN3112_CTRL1_CH8_ENABLE (1 << 5)
#define SN3112_CTRL2		0x15
#define 	SN3112_CTRL2_CH9_ENABLE (1 << 0)
#define 	SN3112_CTRL2_CH10_ENABLE (1 << 1)
#define 	SN3112_CTRL2_CH11_ENABLE (1 << 2)
#define SN3112_PWM_UPDATE	0x16
#define SN3112_RESET		0x17

#define SN3112_DRV_NAME "SN3112"

struct sn3112_status;

struct sn3112_led_group {
	struct sn3112_status *sn3112;
	struct led_classdev_mc mcled_cdev;
};

struct sn3112_status {
	struct i2c_client *client;
	struct regulator *vdd_reg;
	int group_count;
	struct sn3112_led_group *led_groups;
	void *notifier_cookie;
	bool shutdown;
};

static int sn3112_write_reg(struct i2c_client *client, u8 addr, u8 value)
{
	int ret = 0;
	struct i2c_msg msgs;
	u8 buf[] = {addr, value};

	memset(&msgs, 0, sizeof(struct i2c_msg));
	msgs.addr = client->addr;
	msgs.flags = 0;
	msgs.buf = buf;
	msgs.len = sizeof(buf);

	ret = i2c_transfer(client->adapter, &msgs, 1);
	if (ret < 0)
		dev_err(&client->dev, "i2c_transfer(write) fail %d", ret);

	return ret;
}

static int sn3112_brightness_set(struct led_classdev *cdev,
				  enum led_brightness brightness)
{
	struct led_classdev_mc *mcled_cdev = lcdev_to_mccdev(cdev);
	struct sn3112_led_group *group =
		container_of(mcled_cdev, struct sn3112_led_group, mcled_cdev);
	struct sn3112_status *sn3112 = group->sn3112;
	struct i2c_client *client = sn3112->client;
	int i;

	led_mc_calc_color_components(mcled_cdev, brightness);

	for (i = 0; i < mcled_cdev->num_colors; i++) {
		struct mc_subled *subled_info = &mcled_cdev->subled_info[i];

		sn3112_write_reg(client,
				  SN3112_PWM(subled_info->channel),
				  subled_info->brightness);
	}

	sn3112_write_reg(client, SN3112_PWM_UPDATE, 0x00);

	return 0;
};

static void sn3112_panel_event_notifier_callback(
	enum panel_event_notifier_tag tag,
	struct panel_event_notification *notification, void *data)
{
	struct sn3112_status *sn3112 = data;

	if (!notification) {
		dev_err(&sn3112->client->dev, "Invalid panel notification\n");
		return;
	}

	dev_dbg(&sn3112->client->dev, "panel event received, type: %d\n", notification->notif_type);
	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_BLANK:
		if (regulator_is_enabled(sn3112->vdd_reg))
			sn3112_write_reg(sn3112->client, SN3112_SHUTDOWN,
					  SN3112_SHUTDOWN_OFF);

		sn3112->shutdown = true;
		break;
	case DRM_PANEL_EVENT_UNBLANK:
		if (regulator_is_enabled(sn3112->vdd_reg))
			sn3112_write_reg(sn3112->client, SN3112_SHUTDOWN,
					  SN3112_SHUTDOWN_ON);

		sn3112->shutdown = false;
		break;
	default:
		dev_dbg(&sn3112->client->dev, "Ignore panel event: %d\n", notification->notif_type);
		break;
	}
}

static int sn3112_register_panel_notifier(struct sn3112_status *sn3112)
{
	struct device_node *np = sn3112->client->dev.of_node;
	struct device_node *pnode;
	struct drm_panel *panel = NULL;
	void *cookie = NULL;
	int i, count, rc;

	count = of_count_phandle_with_args(np, "panel", NULL);
	if (count <= 0)
		return 0;

	for (i = 0; i < count; i++) {
		pnode = of_parse_phandle(np, "panel", i);
		if (!pnode)
			return -ENODEV;

		panel = of_drm_find_panel(pnode);
		of_node_put(pnode);
		if (!IS_ERR(panel)) {
			break;
		}
	}

	if (!panel || IS_ERR(panel)) {
		rc = PTR_ERR(panel);
		if (rc != -EPROBE_DEFER)
			dev_err(&sn3112->client->dev, "failed to find active panel, rc=%d\n", rc);

		return rc;
	}

	cookie = panel_event_notifier_register(
		PANEL_EVENT_NOTIFICATION_PRIMARY,
		PANEL_EVENT_NOTIFIER_CLIENT_LIGHTS_LEFT, panel,
		sn3112_panel_event_notifier_callback, (void *)sn3112);

	if (IS_ERR(cookie) && PTR_ERR(cookie) == -EEXIST) {
		cookie = panel_event_notifier_register(
			PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_LIGHTS_RIGHT, panel,
			sn3112_panel_event_notifier_callback, (void *)sn3112);
	}

	if (IS_ERR(cookie)) {
		rc = PTR_ERR(cookie);
		dev_err(&sn3112->client->dev, "failed to register panel event notifier, rc=%d\n", rc);
		return rc;
	}

	dev_dbg(&sn3112->client->dev, "register panel notifier successfully\n");
	sn3112->notifier_cookie = cookie;
	return 0;
}

static int sn3112_probe(struct i2c_client *client)
{
	int rc = 0, group_count = 0, group_idx = 0, i;
	struct sn3112_status *sn3112;
	struct fwnode_handle *child = NULL, *led_node = NULL;
	struct led_init_data init_data = {};
	u32 sn3112_ctrl0 = 0, sn3112_ctrl1 = 0, sn3112_ctrl2 = 0;

	sn3112 = kzalloc(sizeof(struct sn3112_status), GFP_KERNEL);
	if (sn3112 == NULL) {
		rc = -ENOMEM;
		printk(KERN_ERR "failed to allocate sn3112\n");
		goto exit;
	}

	dev_set_drvdata(&client->dev, sn3112);

	sn3112->client = client;

	if (sn3112_register_panel_notifier(sn3112) == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	sn3112->vdd_reg = devm_regulator_get(&client->dev, "vdd");
	if (IS_ERR(sn3112->vdd_reg)) {
		rc = PTR_ERR(sn3112->vdd_reg);
		dev_err(&client->dev, "Failed to get vdd regulator\n");
		goto exit;
	}

	device_for_each_child_node (&client->dev, child)
		++group_count;

	sn3112->led_groups =
		devm_kcalloc(&client->dev, group_count,
			     sizeof(struct sn3112_led_group), GFP_KERNEL);
	sn3112->group_count = group_count;

	device_for_each_child_node (&client->dev, child) {
		struct sn3112_led_group *group =
			&sn3112->led_groups[group_idx];
		struct led_classdev_mc *mcled_cdev = &group->mcled_cdev;
		int led_count = 0, led_idx = 0;

		group->sn3112 = sn3112;

		fwnode_for_each_child_node (child, led_node)
			++led_count;

		mcled_cdev->num_colors = led_count;
		mcled_cdev->subled_info =
			devm_kcalloc(&client->dev, led_count,
				     sizeof(struct mc_subled), GFP_KERNEL);

		fwnode_for_each_child_node (child, led_node) {
			struct mc_subled *subled_info =
				&mcled_cdev->subled_info[led_idx];

			rc = fwnode_property_read_u32(led_node, "reg",
						      &subled_info->channel);
			if (rc) {
				dev_err(&client->dev, "Cannot read channel\n");
				goto led_out;
			}

			rc = fwnode_property_read_u32(
				led_node, "color", &subled_info->color_index);
			if (rc) {
				dev_err(&client->dev, "Cannot read color\n");
				goto led_out;
			}

			++led_idx;
		};

		mcled_cdev->led_cdev.brightness_set_blocking =
			sn3112_brightness_set;
		init_data.fwnode = child;

		rc = devm_led_classdev_multicolor_register_ext(
			&client->dev, mcled_cdev, &init_data);
		if (rc) {
			dev_err(&client->dev, "led register err: %d\n", rc);
			goto child_out;
		}
		++group_idx;
	};

	rc = regulator_enable(sn3112->vdd_reg);
	if (rc) {
		dev_err(&client->dev, "Failed to enable vdd regulator\n");
		goto exit;
	}

	msleep(20);

	for (i = 0; i < sn3112->group_count; i++) {
		struct sn3112_led_group *group = &sn3112->led_groups[i];
		struct led_classdev_mc *mcled_cdev = &group->mcled_cdev;
		int j;

		for (j = 0; j < mcled_cdev->num_colors; j++) {
			struct mc_subled *subled_info = &mcled_cdev->subled_info[j];

			switch (subled_info->channel) {
				case 0: sn3112_ctrl0 |= SN3112_CTRL0_CH0_ENABLE; break;
				case 1: sn3112_ctrl0 |= SN3112_CTRL0_CH1_ENABLE; break;
				case 2: sn3112_ctrl0 |= SN3112_CTRL0_CH2_ENABLE; break;
				case 3: sn3112_ctrl1 |= SN3112_CTRL1_CH3_ENABLE; break;
				case 4: sn3112_ctrl1 |= SN3112_CTRL1_CH4_ENABLE; break;
				case 5: sn3112_ctrl1 |= SN3112_CTRL1_CH5_ENABLE; break;
				case 6: sn3112_ctrl1 |= SN3112_CTRL1_CH6_ENABLE; break;
				case 7: sn3112_ctrl1 |= SN3112_CTRL1_CH7_ENABLE; break;
				case 8: sn3112_ctrl1 |= SN3112_CTRL1_CH8_ENABLE; break;
				case 9: sn3112_ctrl2 |= SN3112_CTRL2_CH9_ENABLE; break;
				case 10: sn3112_ctrl2 |= SN3112_CTRL2_CH10_ENABLE; break;
				case 11: sn3112_ctrl2 |= SN3112_CTRL2_CH11_ENABLE; break;
			}
		}
	}

	sn3112_write_reg(sn3112->client, SN3112_CTRL0, sn3112_ctrl0);
	sn3112_write_reg(sn3112->client, SN3112_CTRL1, sn3112_ctrl1);
	sn3112_write_reg(sn3112->client, SN3112_CTRL1, sn3112_ctrl2);

	sn3112_write_reg(sn3112->client, SN3112_PWM_UPDATE, 0x00);
	sn3112_write_reg(sn3112->client, SN3112_SHUTDOWN,
			  SN3112_SHUTDOWN_ON);

	sn3112->shutdown = false;

	return 0;

led_out:
	fwnode_handle_put(led_node);
child_out:
	fwnode_handle_put(child);
exit:
	return rc;
}

static void sn3112_remove(struct i2c_client *client)
{
	struct sn3112_status *sn3112 = dev_get_drvdata(&client->dev);

	sn3112_write_reg(sn3112->client, SN3112_SHUTDOWN,
			  SN3112_SHUTDOWN_OFF);

	if (!IS_ERR_OR_NULL(sn3112->vdd_reg) && regulator_is_enabled(sn3112->vdd_reg))
		regulator_disable(sn3112->vdd_reg);

	if (sn3112->notifier_cookie)
		panel_event_notifier_unregister(sn3112->notifier_cookie);

	kfree(sn3112);
}

static int __maybe_unused sn3112_suspend(struct device *dev)
{
	struct sn3112_status *sn3112 = dev_get_drvdata(dev);

	if (regulator_is_enabled(sn3112->vdd_reg))
		regulator_disable(sn3112->vdd_reg);

	return 0;
}

static int __maybe_unused sn3112_resume(struct device *dev)
{
	struct sn3112_status *sn3112 = dev_get_drvdata(dev);
	int rc, i;
	u32 sn3112_ctrl0 = 0, sn3112_ctrl1 = 0, sn3112_ctrl2 = 0;

	if (!regulator_is_enabled(sn3112->vdd_reg)) {
		rc = regulator_enable(sn3112->vdd_reg);
		if (rc) {
			dev_err(dev, "Failed to enable vdd regulator\n");
			return rc;
		}
	}

	msleep(20);

	for (i = 0; i < sn3112->group_count; i++) {
		struct sn3112_led_group *group = &sn3112->led_groups[i];
		struct led_classdev_mc *mcled_cdev = &group->mcled_cdev;
		int j;

		for (j = 0; j < mcled_cdev->num_colors; j++) {
			struct mc_subled *subled_info = &mcled_cdev->subled_info[j];

			switch (subled_info->channel) {
				case 0: sn3112_ctrl0 |= SN3112_CTRL0_CH0_ENABLE; break;
				case 1: sn3112_ctrl0 |= SN3112_CTRL0_CH1_ENABLE; break;
				case 2: sn3112_ctrl0 |= SN3112_CTRL0_CH2_ENABLE; break;
				case 3: sn3112_ctrl1 |= SN3112_CTRL1_CH3_ENABLE; break;
				case 4: sn3112_ctrl1 |= SN3112_CTRL1_CH4_ENABLE; break;
				case 5: sn3112_ctrl1 |= SN3112_CTRL1_CH5_ENABLE; break;
				case 6: sn3112_ctrl1 |= SN3112_CTRL1_CH6_ENABLE; break;
				case 7: sn3112_ctrl1 |= SN3112_CTRL1_CH7_ENABLE; break;
				case 8: sn3112_ctrl1 |= SN3112_CTRL1_CH8_ENABLE; break;
				case 9: sn3112_ctrl2 |= SN3112_CTRL2_CH9_ENABLE; break;
				case 10: sn3112_ctrl2 |= SN3112_CTRL2_CH10_ENABLE; break;
				case 11: sn3112_ctrl2 |= SN3112_CTRL2_CH11_ENABLE; break;
			}

			sn3112_write_reg(sn3112->client,
					  SN3112_PWM(subled_info->channel),
					  subled_info->brightness);
		}
	}

	sn3112_write_reg(sn3112->client, SN3112_CTRL0, sn3112_ctrl0);
	sn3112_write_reg(sn3112->client, SN3112_CTRL1, sn3112_ctrl1);
	sn3112_write_reg(sn3112->client, SN3112_CTRL1, sn3112_ctrl2);

	sn3112_write_reg(sn3112->client, SN3112_PWM_UPDATE, 0x00);
	sn3112_write_reg(sn3112->client, SN3112_SHUTDOWN,
			  sn3112->shutdown ? SN3112_SHUTDOWN_OFF : SN3112_SHUTDOWN_ON);

	return 0;
}

static const struct of_device_id sn3112_of_match_table[] = {
	{.compatible = "si-en,sn3112",},
	{},
};

static const struct i2c_device_id sn3112_device_id[] = {
	{SN3112_DRV_NAME, 0},
	{}
};

static const struct dev_pm_ops sn3112_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sn3112_suspend, sn3112_resume)
};

static struct i2c_driver sn3112_i2c_driver = {
	.driver = {
		.name = SN3112_DRV_NAME,
		.of_match_table = sn3112_of_match_table,
		.pm = &sn3112_pm_ops,
	},
	.probe = sn3112_probe,
	.remove = sn3112_remove,
	.id_table = sn3112_device_id,
};

static int __init sn3112_driver_init(void)
{
	return i2c_add_driver(&sn3112_i2c_driver);
}

static void __exit sn3112_driver_exit(void)
{
	i2c_del_driver(&sn3112_i2c_driver);
}

module_init(sn3112_driver_init);
module_exit(sn3112_driver_exit);

MODULE_DESCRIPTION("SN3112 Driver");
MODULE_LICENSE("GPL v2");
