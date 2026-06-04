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

#define HTR3212_SHUTDOWN	0x00
#define 	HTR3212_SHUTDOWN_OFF		0
#define 	HTR3212_SHUTDOWN_ON			1
#define HTR3212_PWM(idx)	(0x0D + idx)
#define HTR3212_PWM_UPDATE	0x25
#define HTR3212_CTRL(idx)	(0x32 + idx)
#define 	HTR3212_CTRL_LED_OFF		0
#define 	HTR3212_CTRL_LED_ON			1
#define 	HTR3212_CTRL_CURRENT_MAX	(0 << 1)
#define 	HTR3212_CTRL_CURRENT_HALF	(1 << 1)
#define 	HTR3212_CTRL_CURRENT_THIRD	(2 << 1)
#define 	HTR3212_CTRL_CURRENT_FORTH	(3 << 1)
#define HTR3212_GLOBAL_CTRL 0x4A
#define 	HTR3212_GLOBAL_CTRL_ENABLE	0
#define 	HTR3212_GLOBAL_CTRL_DISABLE	1
#define HTR3212_OUT_FREQ 	0x4B
#define 	HTR3212_OUT_FREQ_3KHZ 		0
#define 	HTR3212_OUT_FREQ_22KHZ 		1
#define HTR3212_RESET	 	0x4F

#define HTR3212_DRV_NAME "HTR3212"

struct htr3212_status;

struct htr3212_led_group {
	struct htr3212_status *htr3212;
	struct led_classdev_mc mcled_cdev;
};

struct htr3212_status {
	struct i2c_client *client;
	struct regulator *vdd_reg;
	int group_count;
	struct htr3212_led_group *led_groups;
	void *notifier_cookie;
	bool shutdown;
};

static int htr3212_write_reg(struct i2c_client *client, u8 addr, u8 value)
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

static int htr3212_brightness_set(struct led_classdev *cdev,
				  enum led_brightness brightness)
{
	struct led_classdev_mc *mcled_cdev = lcdev_to_mccdev(cdev);
	struct htr3212_led_group *group =
		container_of(mcled_cdev, struct htr3212_led_group, mcled_cdev);
	struct htr3212_status *htr3212 = group->htr3212;
	struct i2c_client *client = htr3212->client;
	int i;

	led_mc_calc_color_components(mcled_cdev, brightness);

	for (i = 0; i < mcled_cdev->num_colors; i++) {
		struct mc_subled *subled_info = &mcled_cdev->subled_info[i];

		htr3212_write_reg(client,
				  HTR3212_PWM(subled_info->channel),
				  subled_info->brightness);
	}

	htr3212_write_reg(client, HTR3212_PWM_UPDATE, 0x00);

	return 0;
};

static void htr3212_panel_event_notifier_callback(
	enum panel_event_notifier_tag tag,
	struct panel_event_notification *notification, void *data)
{
	struct htr3212_status *htr3212 = data;

	if (!notification) {
		dev_err(&htr3212->client->dev, "Invalid panel notification\n");
		return;
	}

	dev_dbg(&htr3212->client->dev, "panel event received, type: %d\n", notification->notif_type);
	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_BLANK:
		if (regulator_is_enabled(htr3212->vdd_reg))
			htr3212_write_reg(htr3212->client, HTR3212_SHUTDOWN,
					  HTR3212_SHUTDOWN_OFF);

		htr3212->shutdown = true;
		break;
	case DRM_PANEL_EVENT_UNBLANK:
		if (regulator_is_enabled(htr3212->vdd_reg))
			htr3212_write_reg(htr3212->client, HTR3212_SHUTDOWN,
					  HTR3212_SHUTDOWN_ON);

		htr3212->shutdown = false;
		break;
	default:
		dev_dbg(&htr3212->client->dev, "Ignore panel event: %d\n", notification->notif_type);
		break;
	}
}

static int htr3212_register_panel_notifier(struct htr3212_status *htr3212)
{
	struct device_node *np = htr3212->client->dev.of_node;
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
			dev_err(&htr3212->client->dev, "failed to find active panel, rc=%d\n", rc);

		return rc;
	}

	cookie = panel_event_notifier_register(
		PANEL_EVENT_NOTIFICATION_PRIMARY,
		PANEL_EVENT_NOTIFIER_CLIENT_LIGHTS_LEFT, panel,
		htr3212_panel_event_notifier_callback, (void *)htr3212);

	if (IS_ERR(cookie) && PTR_ERR(cookie) == -EEXIST) {
		cookie = panel_event_notifier_register(
			PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_LIGHTS_RIGHT, panel,
			htr3212_panel_event_notifier_callback, (void *)htr3212);
	}

	if (IS_ERR(cookie)) {
		rc = PTR_ERR(cookie);
		dev_err(&htr3212->client->dev, "failed to register panel event notifier, rc=%d\n", rc);
		return rc;
	}

	dev_dbg(&htr3212->client->dev, "register panel notifier successfully\n");
	htr3212->notifier_cookie = cookie;
	return 0;
}

static int htr3212_probe(struct i2c_client *client)
{
	int rc = 0, group_count = 0, group_idx = 0, i;
	struct htr3212_status *htr3212;
	struct fwnode_handle *child = NULL, *led_node = NULL;
	struct led_init_data init_data = {};

	htr3212 = kzalloc(sizeof(struct htr3212_status), GFP_KERNEL);
	if (htr3212 == NULL) {
		rc = -ENOMEM;
		printk(KERN_ERR "failed to allocate htr3212\n");
		goto exit;
	}

	dev_set_drvdata(&client->dev, htr3212);

	htr3212->client = client;

	if (htr3212_register_panel_notifier(htr3212) == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	htr3212->vdd_reg = devm_regulator_get(&client->dev, "vdd");
	if (IS_ERR(htr3212->vdd_reg)) {
		rc = PTR_ERR(htr3212->vdd_reg);
		dev_err(&client->dev, "Failed to get vdd regulator\n");
		goto exit;
	}

	device_for_each_child_node (&client->dev, child)
		++group_count;

	htr3212->led_groups =
		devm_kcalloc(&client->dev, group_count,
			     sizeof(struct htr3212_led_group), GFP_KERNEL);
	htr3212->group_count = group_count;

	device_for_each_child_node (&client->dev, child) {
		struct htr3212_led_group *group =
			&htr3212->led_groups[group_idx];
		struct led_classdev_mc *mcled_cdev = &group->mcled_cdev;
		int led_count = 0, led_idx = 0;

		group->htr3212 = htr3212;

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
			htr3212_brightness_set;
		init_data.fwnode = child;

		rc = devm_led_classdev_multicolor_register_ext(
			&client->dev, mcled_cdev, &init_data);
		if (rc) {
			dev_err(&client->dev, "led register err: %d\n", rc);
			goto child_out;
		}
		++group_idx;
	};

	rc = regulator_enable(htr3212->vdd_reg);
	if (rc) {
		dev_err(&client->dev, "Failed to enable vdd regulator\n");
		goto exit;
	}

	msleep(20);

	htr3212_write_reg(htr3212->client, HTR3212_GLOBAL_CTRL,
			  HTR3212_GLOBAL_CTRL_ENABLE);
	htr3212_write_reg(htr3212->client, HTR3212_OUT_FREQ,
			  HTR3212_OUT_FREQ_22KHZ);

	for (i = 0; i < htr3212->group_count; i++) {
		struct htr3212_led_group *group = &htr3212->led_groups[i];
		struct led_classdev_mc *mcled_cdev = &group->mcled_cdev;
		int j;

		for (j = 0; j < mcled_cdev->num_colors; j++) {
			struct mc_subled *subled_info = &mcled_cdev->subled_info[j];

			htr3212_write_reg(htr3212->client,
					  HTR3212_CTRL(subled_info->channel),
					  HTR3212_CTRL_LED_ON |  HTR3212_CTRL_CURRENT_MAX);
		}
	}

	htr3212_write_reg(htr3212->client, HTR3212_PWM_UPDATE, 0x00);
	htr3212_write_reg(htr3212->client, HTR3212_SHUTDOWN,
			  HTR3212_SHUTDOWN_ON);

	htr3212->shutdown = false;

	return 0;

led_out:
	fwnode_handle_put(led_node);
child_out:
	fwnode_handle_put(child);
exit:
	return rc;
}

static void htr3212_remove(struct i2c_client *client)
{
	struct htr3212_status *htr3212 = dev_get_drvdata(&client->dev);

	htr3212_write_reg(htr3212->client, HTR3212_SHUTDOWN,
			  HTR3212_SHUTDOWN_OFF);

	if (!IS_ERR_OR_NULL(htr3212->vdd_reg) && regulator_is_enabled(htr3212->vdd_reg))
		regulator_disable(htr3212->vdd_reg);

	if (htr3212->notifier_cookie)
		panel_event_notifier_unregister(htr3212->notifier_cookie);

	kfree(htr3212);
}

static int __maybe_unused htr3212_suspend(struct device *dev)
{
	struct htr3212_status *htr3212 = dev_get_drvdata(dev);

	if (regulator_is_enabled(htr3212->vdd_reg))
		regulator_disable(htr3212->vdd_reg);

	return 0;
}

static int __maybe_unused htr3212_resume(struct device *dev)
{
	struct htr3212_status *htr3212 = dev_get_drvdata(dev);
	int rc, i;

	if (!regulator_is_enabled(htr3212->vdd_reg)) {
		rc = regulator_enable(htr3212->vdd_reg);
		if (rc) {
			dev_err(dev, "Failed to enable vdd regulator\n");
			return rc;
		}
	}

	msleep(20);

	htr3212_write_reg(htr3212->client, HTR3212_GLOBAL_CTRL,
			  HTR3212_GLOBAL_CTRL_ENABLE);
	htr3212_write_reg(htr3212->client, HTR3212_OUT_FREQ,
			  HTR3212_OUT_FREQ_22KHZ);

	for (i = 0; i < htr3212->group_count; i++) {
		struct htr3212_led_group *group = &htr3212->led_groups[i];
		struct led_classdev_mc *mcled_cdev = &group->mcled_cdev;
		int j;

		for (j = 0; j < mcled_cdev->num_colors; j++) {
			struct mc_subled *subled_info = &mcled_cdev->subled_info[j];

			htr3212_write_reg(htr3212->client,
					  HTR3212_CTRL(subled_info->channel),
					  HTR3212_CTRL_LED_ON |  HTR3212_CTRL_CURRENT_MAX);

			htr3212_write_reg(htr3212->client,
					  HTR3212_PWM(subled_info->channel),
					  subled_info->brightness);
		}
	}

	htr3212_write_reg(htr3212->client, HTR3212_PWM_UPDATE, 0x00);
	htr3212_write_reg(htr3212->client, HTR3212_SHUTDOWN,
			  htr3212->shutdown ? HTR3212_SHUTDOWN_OFF : HTR3212_SHUTDOWN_ON);

	return 0;
}

static const struct of_device_id htr3212_of_match_table[] = {
	{ .compatible = "yongfukang,htr3212", },
	{},
};

static const struct i2c_device_id htr3212_device_id[] = {
	{HTR3212_DRV_NAME, 0},
	{}
};

static const struct dev_pm_ops htr3212_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(htr3212_suspend, htr3212_resume)
};

static struct i2c_driver htr3212_i2c_driver = {
	.driver = {
		.name = HTR3212_DRV_NAME,
		.of_match_table = htr3212_of_match_table,
		.pm = &htr3212_pm_ops,
	},
	.probe = htr3212_probe,
	.remove = htr3212_remove,
	.id_table = htr3212_device_id,
};

static int __init htr3212_driver_init(void)
{
	return i2c_add_driver(&htr3212_i2c_driver);
}

static void __exit htr3212_driver_exit(void)
{
	i2c_del_driver(&htr3212_i2c_driver);
}

module_init(htr3212_driver_init);
module_exit(htr3212_driver_exit);

MODULE_DESCRIPTION("HTR3212 Driver");
MODULE_LICENSE("GPL v2");
