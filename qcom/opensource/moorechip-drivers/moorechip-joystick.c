#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#include <uapi/linux/sched/types.h>
#include <linux/firmware.h>
#include <linux/version.h>

#define USB_VENDOR_ID_MICROSOFT 0x045e
#define USB_DEVICE_ID_MICROSOFT_XBOX_360_PAD 0x028e

#define USB_VENDOR_ID_NINTENDO 0x057e
#define USB_DEVICE_ID_NINTENDO_PROCON 0x2009

/* The raw analog joystick values will be mapped in terms of this magnitude */
#define MOORECHIP_MAX_STICK_MAG		32767
#define MOORECHIP_STICK_FUZZ		250
#define MOORECHIP_STICK_FLAT		500

#define MOORECHIP_MAGIC 0x3D5AD3A5

enum moorechip_type {
	MOORECHIP_TYPE_CMD = 1,
	MOORECHIP_TYPE_STATUS,
	MOORECHIP_TYPE_UPGRADE_DATA,
};

enum moorechip_cmd {
	MOORECHIP_CMD_NOP = 1, // returns 1
	MOORECHIP_CMD_GET_VERSION,
	// 3 and 4 are not supported in the firmware
	MOORECHIP_CMD_SET_PARAMETER = 5,
	MOORECHIP_CMD_GET_APP_SIZE,
	MOORECHIP_CMD_SET_PROCESSING_STATE,
	MOORECHIP_CMD_TEST_DATA,
	MOORECHIP_CMD_SET_TRANSMIT_STATE,
	MOORECHIP_CMD_UPGRADE_START = 0xE9,
	MOORECHIP_CMD_UPGRADE_STATE_REPORT,
	MOORECHIP_CMD_UPGRADE_SET_PARAMETER,
	MOORECHIP_CMD_UPGRADE_SUCCESS,
	MOORECHIP_CMD_UPGRADE_SAVE_LAST_FRAME,
	MOORECHIP_CMD_UPGRADE_SET_APP_FLAG
};

struct moorechip_header {
	u32 magic;
	u8 seq;
	u8 type;
	u16 datalen;
} __packed;

struct moorechip_command {
	u8 cmd;
} __packed;

struct moorechip_parameter {
	u8 cmd;
	u8 data1;
	u32 debounce;
	u32 frame_rate;
} __packed;

struct moorechip_enable {
	u8 cmd;
	u8 enable;
} __packed;

struct moorechip_upgrade_parameter {
	u8 cmd;
	u32 size;
	u32 crc;
} __packed;

enum moorechip_keys {
	MOORECHIP_BTN_UP = (1 << 0),
	MOORECHIP_BTN_DOWN = (1 << 1),
	MOORECHIP_BTN_LEFT = (1 << 2),
	MOORECHIP_BTN_RIGHT = (1 << 3),
	MOORECHIP_BTN_X = (1 << 4),
	MOORECHIP_BTN_Y = (1 << 5),
	MOORECHIP_BTN_A = (1 << 6),
	MOORECHIP_BTN_B = (1 << 7),
	MOORECHIP_BTN_TL = (1 << 8),
	MOORECHIP_BTN_TR = (1 << 9),
	MOORECHIP_BTN_SELECT = (1 << 10),
	MOORECHIP_BTN_START = (1 << 11),
	MOORECHIP_BTN_THUMBL = (1 << 12),
	MOORECHIP_BTN_THUMBR = (1 << 13),
	MOORECHIP_BTN_HOME = (1 << 14),
	MOORECHIP_BTN_BACK = (1 << 15)
};

enum moorechip_ignore_extra {
	MOORECHIP_IGNORE_HATY = (1 << 16),
	MOORECHIP_IGNORE_HATX = (1 << 17),
	MOORECHIP_IGNORE_LEFT_STICK = (1 << 18),
	MOORECHIP_IGNORE_RIGHT_STICK = (1 << 19),
};

struct moorechip_key_data {
	u16 keys;
	u16 hat2y;
	u16 hat2x;
	s16 left_joystick_x;
	s16 left_joystick_y;
	s16 right_joystick_x;
	s16 right_joystick_y;
} __packed;

struct moorechip_abs_calib {
	int min;
	int max;
	int center;
};

struct moorechip_stick_calib {
	struct moorechip_abs_calib x;
	struct moorechip_abs_calib y;
};

struct moorechip_hat_calib {
	int min;
	int max;
};

struct moorechip_driver {
	struct serdev_device *serdev;
	struct input_dev *input;
	struct regulator *vdd_reg;
	struct regulator *levelshifter_reg;
	int boot_gpio;
	int reset_gpio;
	bool layout_xbox;
	bool digital_triggers;
	u8 seq;
	struct moorechip_key_data last_keys;
	struct moorechip_stick_calib calib_stick_left;
	struct moorechip_stick_calib calib_stick_right;
	struct moorechip_hat_calib calib_hat_left;
	struct moorechip_hat_calib calib_hat_right;
	struct class *class;
	u32 ignore_mask;
	char firmware_version[58];
	const struct firmware *fw;
	u32 fw_sent;
	struct gpio_desc *m0_key;
	int m0_irq;
	u32 m0_code;
	struct gpio_desc *m1_key;
	int m1_irq;
	u32 m1_code;
};

static int moorechip_send_cmd(struct moorechip_driver *moorechip, u8 type, const void *data, u16 datalen)
{
	struct device *dev = &moorechip->serdev->dev;
	int packetlen = sizeof(struct moorechip_header) + datalen + 1;
	u8 *buf = devm_kzalloc(dev, packetlen, GFP_KERNEL);
	struct moorechip_header *hdr = (struct moorechip_header *)buf;
	u8 checksum = 0;
	int rc, i;

	hdr->magic = MOORECHIP_MAGIC;
	hdr->seq = ++moorechip->seq;
	hdr->type = type;
	hdr->datalen = datalen;

	memcpy(&buf[sizeof(struct moorechip_header)], data, datalen);

	for (i = 4; i < packetlen - 1; i++)
		checksum ^= buf[i];

	buf[packetlen - 1] = checksum;

	rc = serdev_device_write_buf(moorechip->serdev, buf, packetlen);
	if (rc < 0) {
		dev_err(dev, "Failed to send serial data; rc=%d\n", rc);
		return rc;
	}

	devm_kfree(dev, buf);

	return 0;
}

static int moorechip_nop(struct moorechip_driver *moorechip)
{
	struct moorechip_command cmd = {
		.cmd = MOORECHIP_CMD_NOP
	};

	return moorechip_send_cmd(moorechip, MOORECHIP_TYPE_CMD, &cmd, sizeof(cmd));
}

static int moorechip_get_firmware_version(struct moorechip_driver *moorechip)
{
	struct moorechip_command cmd = {
		.cmd = MOORECHIP_CMD_GET_VERSION
	};

	return moorechip_send_cmd(moorechip, MOORECHIP_TYPE_CMD, &cmd, sizeof(cmd));
}

static int moorechip_set_parameter(struct moorechip_driver *moorechip, u32 debounce, u32 frame_rate)
{
	struct moorechip_parameter param = {
		.cmd = MOORECHIP_CMD_SET_PARAMETER,
		.data1 = 1, // must be 1, hardcoded in the firmware
		.debounce = __cpu_to_be32(debounce),
		.frame_rate = __cpu_to_be32(frame_rate)
	};

	return moorechip_send_cmd(moorechip, MOORECHIP_TYPE_CMD, &param, sizeof(param));
}

static int moorechip_set_input_processing_enabled(struct moorechip_driver *moorechip, bool enable)
{
	struct moorechip_enable en = {
		.cmd = MOORECHIP_CMD_SET_PROCESSING_STATE,
		.enable = enable ? 1 : 2
	};

	return moorechip_send_cmd(moorechip, MOORECHIP_TYPE_CMD, &en, sizeof(en));
}

static int moorechip_set_input_transmit_enabled(struct moorechip_driver *moorechip, bool enable)
{
	struct moorechip_enable en = {
		.cmd = MOORECHIP_CMD_SET_TRANSMIT_STATE,
		.enable = enable ? 2 : 1
	};

	return moorechip_send_cmd(moorechip, MOORECHIP_TYPE_CMD, &en, sizeof(en));
}

static int moorechip_upgrade_start(struct moorechip_driver *moorechip)
{
	struct moorechip_command cmd = {
		.cmd = MOORECHIP_CMD_UPGRADE_START
	};

	return moorechip_send_cmd(moorechip, MOORECHIP_TYPE_CMD, &cmd, sizeof(cmd));
}

static int moorechip_upgrade_set_parameter(struct moorechip_driver *moorechip, u32 size)
{
	struct moorechip_upgrade_parameter param = {
		.cmd = MOORECHIP_CMD_UPGRADE_SET_PARAMETER,
		.size = __cpu_to_be32(size),
		.crc = __cpu_to_be32(0)};

	return moorechip_send_cmd(moorechip, MOORECHIP_TYPE_CMD, &param, sizeof(param));
}

static int moorechip_upgrade_send_chunk(struct moorechip_driver *moorechip, const u8 *data, u32 len)
{
	return moorechip_send_cmd(moorechip, MOORECHIP_TYPE_UPGRADE_DATA, data, len);
}

static int moorechip_upgrade_save_last_frame(struct moorechip_driver *moorechip)
{
	struct moorechip_command cmd = {
		.cmd = MOORECHIP_CMD_UPGRADE_SAVE_LAST_FRAME
	};

	return moorechip_send_cmd(moorechip, MOORECHIP_TYPE_CMD, &cmd, sizeof(cmd));
}

static int moorechip_upgrade_done(struct moorechip_driver *moorechip)
{
	moorechip_set_input_transmit_enabled(moorechip, true);
	msleep(100);

	moorechip_set_parameter(moorechip, 40, 7);
	msleep(100);

	moorechip_set_input_processing_enabled(moorechip, true);

	return 0;
}

static s32 moorechip_map_stick_val(struct moorechip_abs_calib *cal, s32 val)
{
	s32 center = cal->center;
	s32 min = cal->min;
	s32 max = cal->max;
	s32 new_val;

	if (val > center) {
		new_val = (val - center) * -MOORECHIP_MAX_STICK_MAG;
		new_val /= (max - center);
	} else {
		new_val = (center - val) * MOORECHIP_MAX_STICK_MAG;
		new_val /= (center - min);
	}
	new_val = clamp(new_val, (s32)-MOORECHIP_MAX_STICK_MAG, (s32)MOORECHIP_MAX_STICK_MAG);
	return new_val;
}

static int moorechip_joystick_receive_buf(struct serdev_device *serdev,
				     const unsigned char *buf, size_t len)
{
	struct moorechip_driver *moorechip = serdev_device_get_drvdata(serdev);
	struct device *dev = &moorechip->serdev->dev;
	struct moorechip_header *hdr = (struct moorechip_header *) buf;
	u8 checksum = 0;
	int packetlen, i;

	if (len < sizeof(struct moorechip_header) + 1) {
		dev_dbg(dev, "Too small packet!\n");
		return -EINVAL;
	}

	if (hdr->magic != MOORECHIP_MAGIC) {
		dev_err(dev, "Invalid magic!\n");
		return sizeof(hdr->magic); // return sizeof(hdr->magic) here, to flush out the buffer
	}

	packetlen = sizeof(struct moorechip_header) + hdr->datalen + 1;

	if (len < packetlen) {
		dev_dbg(dev, "packet len mismatch!\n");
		return -EINVAL;
	}

	for (i = 4; i < packetlen - 1; i++)
		checksum ^= buf[i];

	if (buf[packetlen - 1] != checksum) {
		dev_err(dev, "Invalid checksum!\n");
		return packetlen; // return packetlen here, to flush out the buffer
	}

	switch (hdr->type) {
		case MOORECHIP_TYPE_CMD:
		{
			const u8 *data = &buf[sizeof(struct moorechip_header)];
			switch (data[0]) {
				case MOORECHIP_CMD_NOP:
				{
					if (hdr->datalen != 1)
						dev_err(dev, "Unexpected nop response!\n");

					// We sould only receive a NOP when the fw upgrade is done
					if (moorechip->reset_gpio >= 0) {
						gpio_direction_output(moorechip->reset_gpio, 0);
						msleep(100);
						gpio_direction_output(moorechip->reset_gpio, 1);
						msleep(100);
					} else {
						int rc;

						regulator_disable(moorechip->vdd_reg);
						regulator_disable(moorechip->levelshifter_reg);
						msleep(100);

						rc = regulator_enable(moorechip->vdd_reg);
						if (rc)
						{
							dev_err(dev, "Failed to enable joystick vdd regulator\n");
							return rc;
						}

						rc = regulator_enable(moorechip->levelshifter_reg);
						if (rc)
						{
							dev_err(dev, "Failed to enable joystick levelshifter regulator\n");
							return rc;
						}
						msleep(100);
					}

					moorechip_upgrade_done(moorechip);
					break;
				}
				case MOORECHIP_CMD_GET_VERSION:
				{
					int rc;
					const struct firmware *ver;
					if (hdr->datalen > sizeof(moorechip->firmware_version)) {
						dev_err(dev, "Unexpected version response!\n");
						break;
					}
					memset(moorechip->firmware_version, 0, sizeof(moorechip->firmware_version));
					memcpy(moorechip->firmware_version, &data[1], hdr->datalen - 1);
					dev_info(dev, "Firmware version: %s\n", moorechip->firmware_version);

					// check if we are in bootloader
					if (moorechip->firmware_version[0] != 'B') {
						rc = request_firmware(&ver, "mcuapp_firmware.txt", dev);
						if (rc < 0) {
							dev_err(dev, "Failed to get firmware version: %d\n", rc);
							moorechip_upgrade_done(moorechip);
							break;
						}

						if (!strncmp(ver->data, &moorechip->firmware_version[4], ver->size)) {
							dev_dbg(dev, "Version matches.\n");
							release_firmware(ver);
							moorechip_upgrade_done(moorechip);
							break;
						}
						release_firmware(ver);

						dev_info(dev, "Version mismatch. Upgrading the firmware.\n");

						moorechip_upgrade_start(moorechip);
					}

					rc = request_firmware(&moorechip->fw, "mcuapp_firmware.bin", dev);
					if (rc < 0) {
						dev_err(dev, "Failed to get firmware: %d\n", rc);
						moorechip_upgrade_done(moorechip);
						break;
					}
					moorechip->fw_sent = 0;

					break;
				}
				case MOORECHIP_CMD_SET_PARAMETER:
				{
					if (hdr->datalen != 1) {
						dev_err(dev, "Unexpected set parameter response!\n");
						break;
					}
					dev_dbg(dev, "ACK: set parameter\n");
					break;
				}
				case MOORECHIP_CMD_GET_APP_SIZE:
				{
					if (hdr->datalen != 3) {
						dev_err(dev, "Unexpected get app size response!\n");
						break;
					}
					dev_dbg(dev, "APP SIZE: 0x%x\n", *(u16 *)&data[1]);
					break;
				}
				case MOORECHIP_CMD_SET_PROCESSING_STATE:
				{
					dev_dbg(dev, "ACK: set parameter\n");
					break;
				}
				case MOORECHIP_CMD_TEST_DATA:
				{
					dev_dbg(dev, "test data response\n");
					break;
				}
				case MOORECHIP_CMD_SET_TRANSMIT_STATE:
				{
					dev_dbg(dev, "ACK: set parameter\n");
					break;
				}
				case MOORECHIP_CMD_UPGRADE_START:
				{
					dev_dbg(dev, "ACK: upgrade start\n");
					break;
				}
				case MOORECHIP_CMD_UPGRADE_STATE_REPORT:
				{
					dev_dbg(dev, "ACK: upgrade state report\n");
					if (moorechip->fw)
						moorechip_upgrade_set_parameter(moorechip, moorechip->fw->size);
					break;
				}
				case MOORECHIP_CMD_UPGRADE_SET_PARAMETER:
				{
					dev_dbg(dev, "ACK: upgrade set parameter\n");
					fallthrough;
				}
				case MOORECHIP_CMD_UPGRADE_SUCCESS:
				{
					dev_dbg(dev, "ACK: upgrade success\n");

					if (moorechip->fw) {
						if (moorechip->fw_sent != moorechip->fw->size) {
							int len = moorechip->fw->size - moorechip->fw_sent;
							if (len > 256)
								len = 256;

							moorechip_upgrade_send_chunk(moorechip, &moorechip->fw->data[moorechip->fw_sent], len);
							moorechip->fw_sent += len;
						} else {
							release_firmware(moorechip->fw);
							moorechip->fw = NULL;
							moorechip_upgrade_save_last_frame(moorechip);
						}
					}
					break;
				}
				case MOORECHIP_CMD_UPGRADE_SAVE_LAST_FRAME:
				{
					dev_dbg(dev, "ACK: upgrade save last frame\n");
					break;
				}
				case MOORECHIP_CMD_UPGRADE_SET_APP_FLAG:
				{
					dev_dbg(dev, "ACK: upgrade set app flag\n");

					moorechip_nop(moorechip);
					break;
				}
			}
			break;
		}
		case MOORECHIP_TYPE_STATUS:
		{
			struct moorechip_key_data *last_keys = &moorechip->last_keys;
			const struct moorechip_key_data *key_data = (const struct moorechip_key_data *) &buf[sizeof(struct moorechip_header)];
			u16 changed_keys;
			u16 keys;

			if (hdr->datalen != sizeof(*key_data)) {
				dev_err(dev, "Unexpected packet length for status!\n");
				return packetlen; // return packetlen here, to flush out the buffer
			}

			keys = key_data->keys;
			changed_keys = (last_keys->keys ^ keys) & ~moorechip->ignore_mask;

			if (changed_keys & MOORECHIP_BTN_UP)
				input_report_key(moorechip->input, BTN_DPAD_UP, !!(keys & MOORECHIP_BTN_UP));
			else if (changed_keys & MOORECHIP_BTN_DOWN)
				input_report_key(moorechip->input, BTN_DPAD_DOWN, !!(keys & MOORECHIP_BTN_DOWN));
			else if (changed_keys & MOORECHIP_BTN_LEFT)
				input_report_key(moorechip->input, BTN_DPAD_LEFT, !!(keys & MOORECHIP_BTN_LEFT));
			else if (changed_keys & MOORECHIP_BTN_RIGHT)
				input_report_key(moorechip->input, BTN_DPAD_RIGHT, !!(keys & MOORECHIP_BTN_RIGHT));
			else if (changed_keys & MOORECHIP_BTN_X)
				input_report_key(moorechip->input, BTN_WEST, !!(keys & MOORECHIP_BTN_X));
			else if (changed_keys & MOORECHIP_BTN_Y)
				input_report_key(moorechip->input, BTN_NORTH, !!(keys & MOORECHIP_BTN_Y));
			else if (changed_keys & MOORECHIP_BTN_A)
				input_report_key(moorechip->input, moorechip->layout_xbox ? BTN_EAST: BTN_SOUTH, !!(keys & MOORECHIP_BTN_A));
			else if (changed_keys & MOORECHIP_BTN_B)
				input_report_key(moorechip->input, moorechip->layout_xbox ? BTN_SOUTH: BTN_EAST, !!(keys & MOORECHIP_BTN_B));
			else if (changed_keys & MOORECHIP_BTN_TL)
				input_report_key(moorechip->input, BTN_TL, !!(keys & MOORECHIP_BTN_TL));
			else if (changed_keys & MOORECHIP_BTN_TR)
				input_report_key(moorechip->input, BTN_TR, !!(keys & MOORECHIP_BTN_TR));
			else if (changed_keys & MOORECHIP_BTN_SELECT)
				input_report_key(moorechip->input, BTN_SELECT, !!(keys & MOORECHIP_BTN_SELECT));
			else if (changed_keys & MOORECHIP_BTN_START)
				input_report_key(moorechip->input, BTN_START, !!(keys & MOORECHIP_BTN_START));
			else if (changed_keys & MOORECHIP_BTN_THUMBL)
				input_report_key(moorechip->input, BTN_THUMBL, !!(keys & MOORECHIP_BTN_THUMBL));
			else if (changed_keys & MOORECHIP_BTN_THUMBR)
				input_report_key(moorechip->input, BTN_THUMBR, !!(keys & MOORECHIP_BTN_THUMBR));
			else if (changed_keys & MOORECHIP_BTN_HOME)
				input_report_key(moorechip->input, moorechip->layout_xbox ? BTN_MODE : KEY_HOME, !!(keys & MOORECHIP_BTN_HOME));
			else if (changed_keys & MOORECHIP_BTN_BACK)
				input_report_key(moorechip->input, KEY_BACK, !!(keys & MOORECHIP_BTN_BACK));

			if (!(moorechip->ignore_mask & MOORECHIP_IGNORE_HATY)) {
				if (moorechip->digital_triggers) {
					int trigger_point = (moorechip->calib_hat_left.max - moorechip->calib_hat_left.min) / 2;
					bool last_active = last_keys->hat2y < trigger_point;
					bool now_active = key_data->hat2y < trigger_point;
					if (last_active != now_active)
						input_report_key(moorechip->input, BTN_TL2, now_active);
				} else {
					if (last_keys->hat2y != key_data->hat2y)
						input_report_abs(moorechip->input, ABS_HAT2Y,
							clamp(key_data->hat2y, moorechip->calib_hat_left.min, moorechip->calib_hat_left.max));
				}
			}

			if (!(moorechip->ignore_mask & MOORECHIP_IGNORE_HATX)) {
				if (moorechip->digital_triggers) {
					int trigger_point = (moorechip->calib_hat_right.max - moorechip->calib_hat_right.min) / 2;
					bool last_active = last_keys->hat2x < trigger_point;
					bool now_active = key_data->hat2x < trigger_point;
					if (last_active != now_active)
						input_report_key(moorechip->input, BTN_TR2, now_active);
				} else {
					if (last_keys->hat2x != key_data->hat2x)
						input_report_abs(moorechip->input, ABS_HAT2X,
							clamp(key_data->hat2x, moorechip->calib_hat_right.min, moorechip->calib_hat_right.max));
				}
			}

			if (!(moorechip->ignore_mask & MOORECHIP_IGNORE_LEFT_STICK)) {
				if (last_keys->left_joystick_x != key_data->left_joystick_x)
					input_report_abs(moorechip->input, ABS_X, moorechip_map_stick_val(&moorechip->calib_stick_left.x, key_data->left_joystick_x));
				if (last_keys->left_joystick_y != key_data->left_joystick_y)
					input_report_abs(moorechip->input, ABS_Y, moorechip_map_stick_val(&moorechip->calib_stick_left.y, key_data->left_joystick_y));
			}

			if (!(moorechip->ignore_mask & MOORECHIP_IGNORE_RIGHT_STICK)) {
				if (last_keys->right_joystick_x != key_data->right_joystick_x)
					input_report_abs(moorechip->input, ABS_RX, moorechip_map_stick_val(&moorechip->calib_stick_right.x, key_data->right_joystick_x));
				if (last_keys->right_joystick_y != key_data->right_joystick_y)
					input_report_abs(moorechip->input, ABS_RY, moorechip_map_stick_val(&moorechip->calib_stick_right.y, key_data->right_joystick_y));
			}

			input_sync(moorechip->input);

			memcpy(last_keys, key_data, sizeof(*key_data));
		}
	}

	return packetlen;
};

static irqreturn_t moorechip_gpio_isr(int irq, void *dev_id)
{
	struct moorechip_driver *moorechip = dev_id;

	if (irq == moorechip->m0_irq) {
		u32 code = moorechip->m0_code;
		if (code) {
			if (code == KEY_HOME)
				code = moorechip->layout_xbox ? BTN_MODE : KEY_HOME;
			else if (code == BTN_SOUTH)
				code = moorechip->layout_xbox ? BTN_EAST : BTN_SOUTH;
			else if (code == BTN_EAST)
				code = moorechip->layout_xbox ? BTN_SOUTH : BTN_EAST;
			input_report_key(moorechip->input, code, gpiod_get_value_cansleep(moorechip->m0_key));
			input_sync(moorechip->input);
		}
	} else {
		u32 code = moorechip->m1_code;
		if (code) {
			if (code == KEY_HOME)
				code = moorechip->layout_xbox ? BTN_MODE : KEY_HOME;
			else if (code == BTN_SOUTH)
				code = moorechip->layout_xbox ? BTN_EAST : BTN_SOUTH;
			else if (code == BTN_EAST)
				code = moorechip->layout_xbox ? BTN_SOUTH : BTN_EAST;
			input_report_key(moorechip->input, code, gpiod_get_value_cansleep(moorechip->m1_key));
			input_sync(moorechip->input);
		}
	}

	return IRQ_HANDLED;
}

static struct serdev_device_ops moorechip_joystick_ops = {
	.receive_buf = moorechip_joystick_receive_buf,
};

static int moorechip_joystick_register_input(struct moorechip_driver *moorechip)
{
	moorechip->input = devm_input_allocate_device(&moorechip->serdev->dev);
	if (!moorechip->input)
		return -ENOMEM;
	moorechip->input->id.bustype = BUS_VIRTUAL;

	if (moorechip->layout_xbox) {
		// Xbox 360 controller
		moorechip->input->id.vendor = USB_VENDOR_ID_MICROSOFT;
		moorechip->input->id.product = USB_DEVICE_ID_MICROSOFT_XBOX_360_PAD;
		moorechip->input->name = "moorechip-xbox";
	} else {
		// Nintendo Switch Pro controller
		moorechip->input->id.vendor = USB_VENDOR_ID_NINTENDO;
		moorechip->input->id.product = USB_DEVICE_ID_NINTENDO_PROCON;
		moorechip->input->name = "moorechip-nintendo";
	}

	moorechip->input->id.version = 0;
	input_set_drvdata(moorechip->input, moorechip);

	input_set_capability(moorechip->input, EV_KEY, BTN_DPAD_UP);
	input_set_capability(moorechip->input, EV_KEY, BTN_DPAD_DOWN);
	input_set_capability(moorechip->input, EV_KEY, BTN_DPAD_LEFT);
	input_set_capability(moorechip->input, EV_KEY, BTN_DPAD_RIGHT);
	input_set_capability(moorechip->input, EV_KEY, BTN_NORTH);
	input_set_capability(moorechip->input, EV_KEY, BTN_WEST);
	input_set_capability(moorechip->input, EV_KEY, BTN_EAST);
	input_set_capability(moorechip->input, EV_KEY, BTN_SOUTH);
	input_set_capability(moorechip->input, EV_KEY, BTN_TL);
	input_set_capability(moorechip->input, EV_KEY, BTN_TR);
	input_set_capability(moorechip->input, EV_KEY, BTN_SELECT);
	input_set_capability(moorechip->input, EV_KEY, BTN_START);
	input_set_capability(moorechip->input, EV_KEY, BTN_THUMBL);
	input_set_capability(moorechip->input, EV_KEY, BTN_THUMBR);
	input_set_capability(moorechip->input, EV_KEY, moorechip->layout_xbox ? BTN_MODE : KEY_HOME);
	input_set_capability(moorechip->input, EV_KEY, KEY_BACK);
	input_set_capability(moorechip->input, EV_KEY, BTN_TL2);
	input_set_capability(moorechip->input, EV_KEY, BTN_TR2);

	input_set_abs_params(moorechip->input, ABS_HAT2Y, moorechip->calib_hat_left.max,
		moorechip->calib_hat_left.min, 0, 30);
	input_set_abs_params(moorechip->input, ABS_HAT2X, moorechip->calib_hat_right.max,
		moorechip->calib_hat_right.min, 0, 30);

	input_set_abs_params(moorechip->input, ABS_X,
			     -MOORECHIP_MAX_STICK_MAG, MOORECHIP_MAX_STICK_MAG,
			     MOORECHIP_STICK_FUZZ, MOORECHIP_STICK_FLAT);
	input_set_abs_params(moorechip->input, ABS_Y,
			     -MOORECHIP_MAX_STICK_MAG, MOORECHIP_MAX_STICK_MAG,
			     MOORECHIP_STICK_FUZZ, MOORECHIP_STICK_FLAT);

	input_set_abs_params(moorechip->input, ABS_RX,
			     -MOORECHIP_MAX_STICK_MAG, MOORECHIP_MAX_STICK_MAG,
			     MOORECHIP_STICK_FUZZ, MOORECHIP_STICK_FLAT);
	input_set_abs_params(moorechip->input, ABS_RY,
			     -MOORECHIP_MAX_STICK_MAG, MOORECHIP_MAX_STICK_MAG,
			     MOORECHIP_STICK_FUZZ, MOORECHIP_STICK_FLAT);

	return input_register_device(moorechip->input);
}

static ssize_t set_calibration(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);
	char *temp;

	char *next = (char *)buf;
	if (next == NULL)
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_left.x.min))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_left.x.max))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_left.x.center))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_left.y.min))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_left.y.max))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_left.y.center))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_right.x.min))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_right.x.max))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_right.x.center))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_right.y.min))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_right.y.max))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_stick_right.y.center))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_hat_left.min))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_hat_left.max))
		return -EINVAL;

	temp = strsep(&next, ":");
	if (next == NULL)
		return -EINVAL;

	if (kstrtoint(temp, 10, &moorechip->calib_hat_right.min))
		return -EINVAL;

	if (kstrtoint(next, 10, &moorechip->calib_hat_right.max))
		return -EINVAL;

	input_unregister_device(moorechip->input);
	moorechip_joystick_register_input(moorechip);

	return count;
}

static ssize_t get_calibration(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE,
			"%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d",
			moorechip->calib_stick_left.x.min,
			moorechip->calib_stick_left.x.max,
			moorechip->calib_stick_left.x.center,
			moorechip->calib_stick_left.y.min,
			moorechip->calib_stick_left.y.max,
			moorechip->calib_stick_left.y.center,
			moorechip->calib_stick_right.x.min,
			moorechip->calib_stick_right.x.max,
			moorechip->calib_stick_right.x.center,
			moorechip->calib_stick_right.y.min,
			moorechip->calib_stick_right.y.max,
			moorechip->calib_stick_right.y.center,
			moorechip->calib_hat_left.min,
			moorechip->calib_hat_left.max,
			moorechip->calib_hat_right.min,
			moorechip->calib_hat_right.max);
}
static DEVICE_ATTR(calibration, 0644, get_calibration, set_calibration);

static ssize_t get_raw(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);
	struct moorechip_key_data *last_keys = &moorechip->last_keys;

	return snprintf(buf, PAGE_SIZE, "%04x:%d:%d:%d:%d:%d:%d",
			last_keys->keys,
			last_keys->left_joystick_x, last_keys->left_joystick_y,
			last_keys->right_joystick_x, last_keys->right_joystick_y,
			last_keys->hat2y, last_keys->hat2x);
}
static DEVICE_ATTR(raw, 0444, get_raw, NULL);

static ssize_t set_layout(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);

	if (!strcmp("xbox", buf))
		moorechip->layout_xbox = true;
	else if (!strcmp("nintendo", buf))
		moorechip->layout_xbox = false;
	else
		return -EINVAL;

	input_unregister_device(moorechip->input);
	moorechip_joystick_register_input(moorechip);

	return count;
}

static ssize_t get_layout(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);
	if (moorechip->layout_xbox)
		return snprintf(buf, PAGE_SIZE, "xbox");
	else
		return snprintf(buf, PAGE_SIZE, "nintendo");
}
static DEVICE_ATTR(layout, 0644, get_layout, set_layout);

static ssize_t set_triggers(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);

	if (!strcmp("digital", buf))
		moorechip->digital_triggers = true;
	else if (!strcmp("analog", buf))
		moorechip->digital_triggers = false;
	else
		return -EINVAL;

	input_unregister_device(moorechip->input);
	moorechip_joystick_register_input(moorechip);

	return count;
}

static ssize_t get_triggers(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);
	if (moorechip->digital_triggers)
		return snprintf(buf, PAGE_SIZE, "digital");
	else
		return snprintf(buf, PAGE_SIZE, "analog");
}
static DEVICE_ATTR(triggers, 0644, get_triggers, set_triggers);

static ssize_t set_ignore_mask(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);

	if (kstrtou32(buf, 16, &moorechip->ignore_mask))
		return -EINVAL;

	return count;
}

static ssize_t get_ignore_mask(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%x", moorechip->ignore_mask);
}
static DEVICE_ATTR(ignore_mask, 0644, get_ignore_mask, set_ignore_mask);

static ssize_t get_firmware_version(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%s", moorechip->firmware_version);
}
static DEVICE_ATTR(firmware_version, 0444, get_firmware_version, NULL);

static ssize_t set_m0_function(struct device *dev, struct device_attribute *attr,
						  const char *buf, size_t count)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);

	if (!strcmp("none", buf))
		moorechip->m0_code = 0;
	else if (!strcmp("home", buf))
		moorechip->m0_code = KEY_HOME;
	else if (!strcmp("select", buf))
		moorechip->m0_code = BTN_SELECT;
	else if (!strcmp("start", buf))
		moorechip->m0_code = BTN_START;
	else if (!strcmp("back", buf))
		moorechip->m0_code = KEY_BACK;
	else if (!strcmp("a", buf))
		moorechip->m0_code = BTN_SOUTH;
	else if (!strcmp("b", buf))
		moorechip->m0_code = BTN_EAST;
	else if (!strcmp("x", buf))
		moorechip->m0_code = BTN_WEST;
	else if (!strcmp("y", buf))
		moorechip->m0_code = BTN_NORTH;
	else if (!strcmp("l1", buf))
		moorechip->m0_code = BTN_TL;
	else if (!strcmp("l2", buf))
		moorechip->m0_code = BTN_TL2;
	else if (!strcmp("l3", buf))
		moorechip->m0_code = BTN_THUMBL;
	else if (!strcmp("r1", buf))
		moorechip->m0_code = BTN_TR;
	else if (!strcmp("r2", buf))
		moorechip->m0_code = BTN_TR2;
	else if (!strcmp("r3", buf))
		moorechip->m0_code = BTN_THUMBR;
	else if (!strcmp("down", buf))
		moorechip->m0_code = BTN_DPAD_DOWN;
	else if (!strcmp("up", buf))
		moorechip->m0_code = BTN_DPAD_UP;
	else if (!strcmp("left", buf))
		moorechip->m0_code = BTN_DPAD_LEFT;
	else if (!strcmp("right", buf))
		moorechip->m0_code = BTN_DPAD_RIGHT;
	else
		return -EINVAL;

	return count;
}

static ssize_t get_m0_function(struct device *dev, struct device_attribute *attr,
						  char *buf)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);

	switch (moorechip->m0_code)
	{
	case 0: return snprintf(buf, PAGE_SIZE, "none");
	case KEY_HOME: return snprintf(buf, PAGE_SIZE, "home");
	case BTN_SELECT: return snprintf(buf, PAGE_SIZE, "select");
	case BTN_START: return snprintf(buf, PAGE_SIZE, "start");
	case KEY_BACK: return snprintf(buf, PAGE_SIZE, "back");
	case BTN_SOUTH: return snprintf(buf, PAGE_SIZE, "a");
	case BTN_EAST: return snprintf(buf, PAGE_SIZE, "b");
	case BTN_WEST: return snprintf(buf, PAGE_SIZE, "x");
	case BTN_NORTH: return snprintf(buf, PAGE_SIZE, "y");
	case BTN_TL: return snprintf(buf, PAGE_SIZE, "l1");
	case BTN_TL2: return snprintf(buf, PAGE_SIZE, "l2");
	case BTN_THUMBL: return snprintf(buf, PAGE_SIZE, "l3");
	case BTN_TR: return snprintf(buf, PAGE_SIZE, "r1");
	case BTN_TR2: return snprintf(buf, PAGE_SIZE, "r2");
	case BTN_THUMBR: return snprintf(buf, PAGE_SIZE, "r3");
	case BTN_DPAD_DOWN: return snprintf(buf, PAGE_SIZE, "down");
	case BTN_DPAD_UP: return snprintf(buf, PAGE_SIZE, "up");
	case BTN_DPAD_LEFT: return snprintf(buf, PAGE_SIZE, "left");
	case BTN_DPAD_RIGHT: return snprintf(buf, PAGE_SIZE, "right");
	}

	return -EINVAL;
}
static DEVICE_ATTR(m0_function, 0644, get_m0_function, set_m0_function);

static ssize_t set_m1_function(struct device *dev, struct device_attribute *attr,
						  const char *buf, size_t count)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);

	if (!strcmp("none", buf))
		moorechip->m1_code = 0;
	else if (!strcmp("home", buf))
		moorechip->m1_code = KEY_HOME;
	else if (!strcmp("select", buf))
		moorechip->m1_code = BTN_SELECT;
	else if (!strcmp("start", buf))
		moorechip->m1_code = BTN_START;
	else if (!strcmp("back", buf))
		moorechip->m1_code = KEY_BACK;
	else if (!strcmp("a", buf))
		moorechip->m1_code = BTN_SOUTH;
	else if (!strcmp("b", buf))
		moorechip->m1_code = BTN_EAST;
	else if (!strcmp("x", buf))
		moorechip->m1_code = BTN_WEST;
	else if (!strcmp("y", buf))
		moorechip->m1_code = BTN_NORTH;
	else if (!strcmp("l1", buf))
		moorechip->m1_code = BTN_TL;
	else if (!strcmp("l2", buf))
		moorechip->m1_code = BTN_TL2;
	else if (!strcmp("l3", buf))
		moorechip->m1_code = BTN_THUMBL;
	else if (!strcmp("r1", buf))
		moorechip->m1_code = BTN_TR;
	else if (!strcmp("r2", buf))
		moorechip->m1_code = BTN_TR2;
	else if (!strcmp("r3", buf))
		moorechip->m1_code = BTN_THUMBR;
	else if (!strcmp("down", buf))
		moorechip->m1_code = BTN_DPAD_DOWN;
	else if (!strcmp("up", buf))
		moorechip->m1_code = BTN_DPAD_UP;
	else if (!strcmp("left", buf))
		moorechip->m1_code = BTN_DPAD_LEFT;
	else if (!strcmp("right", buf))
		moorechip->m1_code = BTN_DPAD_RIGHT;
	else
		return -EINVAL;

	return count;
}

static ssize_t get_m1_function(struct device *dev, struct device_attribute *attr,
						  char *buf)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);

	switch (moorechip->m1_code)
	{
	case 0: return snprintf(buf, PAGE_SIZE, "none");
	case KEY_HOME: return snprintf(buf, PAGE_SIZE, "home");
	case BTN_SELECT: return snprintf(buf, PAGE_SIZE, "select");
	case BTN_START: return snprintf(buf, PAGE_SIZE, "start");
	case KEY_BACK: return snprintf(buf, PAGE_SIZE, "back");
	case BTN_SOUTH: return snprintf(buf, PAGE_SIZE, "a");
	case BTN_EAST: return snprintf(buf, PAGE_SIZE, "b");
	case BTN_WEST: return snprintf(buf, PAGE_SIZE, "x");
	case BTN_NORTH: return snprintf(buf, PAGE_SIZE, "y");
	case BTN_TL: return snprintf(buf, PAGE_SIZE, "l1");
	case BTN_TL2: return snprintf(buf, PAGE_SIZE, "l2");
	case BTN_THUMBL: return snprintf(buf, PAGE_SIZE, "l3");
	case BTN_TR: return snprintf(buf, PAGE_SIZE, "r1");
	case BTN_TR2: return snprintf(buf, PAGE_SIZE, "r2");
	case BTN_THUMBR: return snprintf(buf, PAGE_SIZE, "r3");
	case BTN_DPAD_DOWN: return snprintf(buf, PAGE_SIZE, "down");
	case BTN_DPAD_UP: return snprintf(buf, PAGE_SIZE, "up");
	case BTN_DPAD_LEFT: return snprintf(buf, PAGE_SIZE, "left");
	case BTN_DPAD_RIGHT: return snprintf(buf, PAGE_SIZE, "right");
	}

	return -EINVAL;
}
static DEVICE_ATTR(m1_function, 0644, get_m1_function, set_m1_function);

static int moorechip_joystick_probe(struct serdev_device *serdev)
{
	struct moorechip_driver *moorechip;
	struct device *dev = &serdev->dev;
	int ret = 0;

	moorechip = devm_kzalloc(dev, sizeof(*moorechip), GFP_KERNEL);
	if (!moorechip)
		return -ENOMEM;

	moorechip->serdev = serdev;
	moorechip->seq = 0;
	memset(&moorechip->last_keys, 0, sizeof(moorechip->last_keys));
	moorechip->calib_stick_left.x.min = -1350;
	moorechip->calib_stick_left.x.max = 1350;
	moorechip->calib_stick_left.x.center = 0;
	moorechip->calib_stick_left.y.min = -1350;
	moorechip->calib_stick_left.y.max = 1350;
	moorechip->calib_stick_left.y.center = 0;
	moorechip->calib_stick_right.x.min = -1350;
	moorechip->calib_stick_right.x.max = 1350;
	moorechip->calib_stick_right.x.center = 0;
	moorechip->calib_stick_right.y.min = -1350;
	moorechip->calib_stick_right.y.max = 1350;
	moorechip->calib_stick_right.y.center = 0;
	moorechip->calib_hat_left.min = 0;
	moorechip->calib_hat_left.max = 1550;
	moorechip->calib_hat_right.min = 0;
	moorechip->calib_hat_right.max = 1550;
	moorechip->digital_triggers = false;
	moorechip->ignore_mask = 0;
	moorechip->fw = NULL;

	moorechip->vdd_reg = devm_regulator_get(dev, "vdd");
	if (IS_ERR(moorechip->vdd_reg)) {
		ret = PTR_ERR(moorechip->vdd_reg);
		dev_err(dev, "Failed to get joystick vdd regulator\n");
		return ret;
	}

	moorechip->levelshifter_reg = devm_regulator_get(dev, "levelshifter");
	if (IS_ERR(moorechip->levelshifter_reg)) {
		ret = PTR_ERR(moorechip->levelshifter_reg);
		dev_err(dev, "Failed to get joystick levelshifter regulator\n");
		return ret;
	}

	moorechip->boot_gpio = of_get_named_gpio(dev->of_node, "moorechip,boot-gpio", 0);
	if (moorechip->boot_gpio >= 0) {
		ret = devm_gpio_request(dev, moorechip->boot_gpio, "joystick-boot");
		if (ret)
		{
			dev_err(dev, "Failed to request gpio joystick-boot\n");
			return ret;
		}
	}

	moorechip->reset_gpio = of_get_named_gpio(dev->of_node, "moorechip,reset-gpio", 0);
	if (moorechip->reset_gpio >= 0) {
		ret = devm_gpio_request(dev, moorechip->reset_gpio, "joystick-reset");
		if (ret)
		{
			dev_err(dev, "Failed to request gpio joystick-reset\n");
			return ret;
		}
	}

	serdev_device_set_drvdata(serdev, moorechip);
	moorechip->input = NULL;

	if (of_property_read_bool(dev->of_node, "moorechip,layout-xbox")) {
		moorechip->layout_xbox = true;
	} else if (of_property_read_bool(dev->of_node, "moorechip,layout-nintendo")) {
		moorechip->layout_xbox = false;
	} else {
		dev_err(dev, "Layout not defined!\n");
		return -EINVAL;
	}

	moorechip->m0_key = devm_gpiod_get(dev, "m0_key", GPIOD_IN);
	if (PTR_ERR(moorechip->m0_key) == -ENOENT) {
		moorechip->m0_key = NULL;
	} else if (IS_ERR(moorechip->m0_key)) {
		ret = PTR_ERR(moorechip->m0_key);
		dev_err(dev, "failed to get m0 key: %d\n", ret);
		goto err;
	} else {
		ret = gpiod_to_irq(moorechip->m0_key);
		if (ret < 0) {
			dev_err(dev, "Unable to get irq number for m0 key: %d\n", ret);
			goto err;
		}
		moorechip->m0_irq = ret;

		ret = devm_request_any_context_irq(dev, moorechip->m0_irq,
			moorechip_gpio_isr, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"m0_key", moorechip);
		if (ret < 0) {
			dev_err(dev, "Unable to claim m0 key irq: %d\n", ret);
			goto err;
		}

		moorechip->m0_code = 0;
	}

	moorechip->m1_key = devm_gpiod_get(dev, "m1_key", GPIOD_IN);
	if (PTR_ERR(moorechip->m1_key) == -ENOENT) {
		moorechip->m1_key = NULL;
	} else if (IS_ERR(moorechip->m1_key)) {
		ret = PTR_ERR(moorechip->m1_key);
		dev_err(dev, "failed to get m1 key: %d\n", ret);
		goto err;
	} else {
		ret = gpiod_to_irq(moorechip->m1_key);
		if (ret < 0) {
			dev_err(dev, "Unable to get irq number for m1 key: %d\n", ret);
			goto err;
		}
		moorechip->m1_irq = ret;

		ret = devm_request_any_context_irq(dev, moorechip->m1_irq,
			moorechip_gpio_isr, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"m1_key", moorechip);
		if (ret < 0) {
			dev_err(dev, "Unable to claim m1 key irq: %d\n", ret);
			goto err;
		}

		moorechip->m1_code = 0;
	}

	ret = serdev_device_open(serdev);
	if (ret) {
		dev_err(dev, "Failed to open serdev device; ret=%d\n", ret);
		goto err;
	}
	serdev_device_set_client_ops(serdev, &moorechip_joystick_ops);
	serdev_device_set_flow_control(serdev, false);
	serdev_device_set_baudrate(serdev, 115200);

	ret = moorechip_joystick_register_input(moorechip);
	if (ret)
		goto err_sdev_close;

#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
	moorechip->class = class_create("moorechip-joystick");
#else
	moorechip->class = class_create(THIS_MODULE, "moorechip-joystick");
#endif
	if (moorechip->class != NULL) {
		struct device *joystick = device_create(moorechip->class, NULL, 0, NULL, "joystick");
		dev_set_drvdata(joystick, moorechip);
		device_create_file(joystick, &dev_attr_calibration);
		device_create_file(joystick, &dev_attr_raw);
		device_create_file(joystick, &dev_attr_layout);
		device_create_file(joystick, &dev_attr_triggers);
		device_create_file(joystick, &dev_attr_ignore_mask);
		device_create_file(joystick, &dev_attr_firmware_version);
		if (moorechip->m0_key)
			device_create_file(joystick, &dev_attr_m0_function);
		if (moorechip->m1_key)
			device_create_file(joystick, &dev_attr_m1_function);
	}

	if (moorechip->boot_gpio >= 0)
		gpio_direction_output(moorechip->boot_gpio, 0);
	if (moorechip->reset_gpio >= 0)
		gpio_direction_output(moorechip->reset_gpio, 0);

	ret = regulator_enable(moorechip->vdd_reg);
	if (ret) {
		dev_err(dev, "Failed to enable joystick vdd regulator\n");
		goto err_sdev_close;
	}

	ret = regulator_enable(moorechip->levelshifter_reg);
	if (ret) {
		dev_err(dev,
			"Failed to enable joystick levelshifter regulator\n");
		goto err_sdev_close;
	}

	msleep(40);

	if (moorechip->reset_gpio >= 0) {
		gpio_direction_output(moorechip->reset_gpio, 1);
		msleep(100);
	}

	moorechip_set_input_transmit_enabled(moorechip, true);
	msleep(100);

	moorechip_get_firmware_version(moorechip);

	return 0;

err_sdev_close:
	serdev_device_close(serdev);

err:
	return ret;
}

static void moorechip_joystick_remove(struct serdev_device *serdev)
{
	struct moorechip_driver *moorechip = serdev_device_get_drvdata(serdev);

	moorechip_set_input_processing_enabled(moorechip, false);

	moorechip_set_input_transmit_enabled(moorechip, false);

	serdev_device_close(serdev);

	if(moorechip->reset_gpio >= 0)
		gpio_direction_output(moorechip->reset_gpio, 0);
	if (moorechip->boot_gpio >= 0)
		gpio_direction_output(moorechip->boot_gpio, 0);

	if (!IS_ERR_OR_NULL(moorechip->vdd_reg) && regulator_is_enabled(moorechip->vdd_reg))
		regulator_disable(moorechip->vdd_reg);

	if (!IS_ERR_OR_NULL(moorechip->levelshifter_reg) && regulator_is_enabled(moorechip->levelshifter_reg))
		regulator_disable(moorechip->levelshifter_reg);
}

static int __maybe_unused moorechip_joystick_suspend(struct device *dev)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);

	if(moorechip->reset_gpio >= 0)
			gpio_direction_output(moorechip->reset_gpio, 0);
	if (moorechip->boot_gpio >= 0)
		gpio_direction_output(moorechip->boot_gpio, 0);

	if (regulator_is_enabled(moorechip->vdd_reg))
		regulator_disable(moorechip->vdd_reg);

	if (regulator_is_enabled(moorechip->levelshifter_reg))
		regulator_disable(moorechip->levelshifter_reg);

	return 0;
}

static int __maybe_unused moorechip_joystick_resume(struct device *dev)
{
	struct moorechip_driver *moorechip = dev_get_drvdata(dev);
	int ret;

	device_destroy(moorechip->class, 0);
	class_destroy(moorechip->class);

	if (moorechip->boot_gpio >= 0)
		gpio_direction_output(moorechip->boot_gpio, 0);
	if (moorechip->reset_gpio >= 0)
		gpio_direction_output(moorechip->reset_gpio, 0);

	if (!regulator_is_enabled(moorechip->vdd_reg)) {
		ret = regulator_enable(moorechip->vdd_reg);
		if (ret) {
			dev_err(dev, "Failed to enable joystick vdd regulator\n");
			return ret;
		}
	}

	if (!regulator_is_enabled(moorechip->levelshifter_reg)) {
		ret = regulator_enable(moorechip->levelshifter_reg);
		if (ret) {
			dev_err(dev,
				"Failed to enable joystick levelshifter regulator\n");
			return ret;
		}
	}

	msleep(40);

	if(moorechip->reset_gpio >= 0) {
		gpio_direction_output(moorechip->reset_gpio, 1);
		msleep(100);
	}

	moorechip_set_input_transmit_enabled(moorechip, true);
	msleep(100);

	moorechip_set_parameter(moorechip, 40, 7);
	msleep(100);

	moorechip_set_input_processing_enabled(moorechip, true);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id moorechip_joystick_of_match[] = {
	{ .compatible = "moorechip,joystick" },
	{},
};
MODULE_DEVICE_TABLE(of, moorechip_joystick_of_match);
#endif

static const struct dev_pm_ops moorechip_joystick_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(moorechip_joystick_suspend, moorechip_joystick_resume)
};

static struct serdev_device_driver moorechip_joystick_driver = {
	.probe = moorechip_joystick_probe,
	.remove = moorechip_joystick_remove,
	.driver = {
		.name = "moorechip-joystick",
		.of_match_table = of_match_ptr(moorechip_joystick_of_match),
		.pm = &moorechip_joystick_pm_ops,
	},
};

int __init moorechip_joystick_init(void)
{
	return serdev_device_driver_register(&moorechip_joystick_driver);
}

void __exit moorechip_joystick_deinit(void)
{
	serdev_device_driver_unregister(&moorechip_joystick_driver);
}

module_init(moorechip_joystick_init);
module_exit(moorechip_joystick_deinit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Balázs Triszka <info@balika011.hu>");
MODULE_DESCRIPTION("Driver for Moorechip controller over UART");
