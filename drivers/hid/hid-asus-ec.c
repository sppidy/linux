// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ASUS EC HID driver for Zenbook A14 (UX3407QA)
 *
 * EC I2C HID driver for keyboard backlight and Fn hotkeys.
 *
 * Copyright (c) 2025 Alexandru Marc Serdeliuc <serdeliuk@yahoo.com>
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define ASUS_VENDOR_ID		0x0B05
#define ASUS_PRODUCT_ID		0x0220

#define A14_EC_REPORT_ID	0x5A
#define A14_EC_REPORT_SIZE	64
#define A14_EC_MAX_BACKLIGHT	3

#define A14_EC_EVT_KEY_FN_ESC	0x4E
#define A14_EC_EVT_KEY_FN_F4	0xC7
#define A14_EC_EVT_KEY_FN_F5	0x10
#define A14_EC_EVT_KEY_FN_F6	0x20
#define A14_EC_EVT_KEY_FN_F8	0x7E
#define A14_EC_EVT_KEY_FN_F9	0x7C
#define A14_EC_EVT_KEY_FN_F10	0x85
#define A14_EC_EVT_KEY_FN_F11	0x6B
#define A14_EC_EVT_KEY_FN_F12	0x86
#define A14_EC_EVT_KEY_FN_F	0x9d

/*
 * The Copilot key is sent as three keyboard reports: Left GUI, then Left GUI +
 * Left Shift, then Left GUI + Left Shift + F23. A lone Left GUI press from an
 * idle keyboard is held back briefly to see whether that sequence follows.
 */
#define A14_KBD_REPORT_ID	0x02
#define A14_KBD_REPORT_SIZE	9
#define A14_KBD_MODS		1
#define A14_KBD_KEYS		3
#define A14_KBD_NKEYS		6
#define A14_KBD_MOD_LSHIFT	BIT(1)
#define A14_KBD_MOD_LGUI	BIT(3)
#define A14_KBD_COPILOT_MODS	(A14_KBD_MOD_LGUI | A14_KBD_MOD_LSHIFT)
#define A14_KBD_USAGE_F23	0x72
#define A14_KBD_HOLD_MS		30

enum asus_copilot_state {
	ASUS_COPILOT_IDLE,
	ASUS_COPILOT_DOWN,
	ASUS_COPILOT_RELEASING,
};

struct asus_hid_data {
	struct hid_device *hdev;
	struct led_classdev kbd_led_cdev;
	struct input_dev *hotkey_input_dev;
	enum led_brightness saved_brightness;
	bool interfaces_ready;

	/* Copilot key detection on the keyboard report, under kbd_lock. */
	spinlock_t kbd_lock;
	struct delayed_work kbd_hold_work;
	struct input_dev *kbd_input;
	u8 held[A14_KBD_REPORT_SIZE];
	bool holding;
	u8 injected_mods;
	bool kbd_idle;
	enum asus_copilot_state copilot;
};

static int asus_send_ec_command(struct hid_device *hdev, u8 *cmd_buf)
{
	int ret;
	u8 report_id = cmd_buf[0];

	ret = hid_hw_raw_request(hdev, report_id, cmd_buf, A14_EC_REPORT_SIZE,
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

	if (ret < 0)
		dev_err(&hdev->dev, "hid_hw_raw_request failed with status: %d\n", ret);

	return ret;
}

static int asus_kbd_led_init(struct hid_device *hdev)
{
	u8 ec_init_cmd[A14_EC_REPORT_SIZE] = { A14_EC_REPORT_ID, 0xD0, 0x8F, 0x01 };
	int ret;

	ret = asus_send_ec_command(hdev, ec_init_cmd);

	if (ret < 0)
		return ret;

	u8 brightness_cmd[A14_EC_REPORT_SIZE] = { A14_EC_REPORT_ID, 0xBA, 0xC5,
				    0xC4, A14_EC_MAX_BACKLIGHT };

	ret = asus_send_ec_command(hdev, brightness_cmd);
	if (ret < 0)
		dev_warn(&hdev->dev, "Brightness init failed: %d\n", ret);

	return ret;
}

static void asus_kbd_set_brightness(struct led_classdev *led_cdev,
				    enum led_brightness brightness)
{
	struct asus_hid_data *data = container_of(led_cdev, struct asus_hid_data, kbd_led_cdev);

	u8 level = (u8)brightness;

	if (level > A14_EC_MAX_BACKLIGHT)
		level = A14_EC_MAX_BACKLIGHT;

	u8 buf[A14_EC_REPORT_SIZE] = { A14_EC_REPORT_ID, 0xBA, 0xC5, 0xC4, level };

	asus_send_ec_command(data->hdev, buf);
	msleep(20);
	data->saved_brightness = (enum led_brightness)level;
}

/*
 * The keyboard key array is declared with Logical Maximum 0x65, but the
 * firmware sends usages above it: the Copilot key reports F23 (0x72). The HID
 * core drops array values beyond the logical maximum, so raise it to 0x7f
 * (Usage Maximum is 0xdd, but a one-byte Logical Maximum is signed).
 */
static const u8 asus_kbd_keys_desc[] = {
	0x95, 0x06,		/* Report Count (6)		*/
	0x75, 0x08,		/* Report Size (8)		*/
	0x15, 0x00,		/* Logical Minimum (0)		*/
	0x25, 0x65,		/* Logical Maximum (101)	*/
	0x05, 0x07,		/* Usage Page (Keyboard)	*/
	0x19, 0x00,		/* Usage Minimum (0)		*/
	0x29, 0xdd,		/* Usage Maximum (221)		*/
	0x81, 0x00,		/* Input (Data,Arr,Abs)		*/
};
#define ASUS_KBD_KEYS_LMAX_OFFSET	7

static const __u8 *asus_report_fixup(struct hid_device *hdev, __u8 *rdesc,
				     unsigned int *rsize)
{
	unsigned int i;

	for (i = 0; i + sizeof(asus_kbd_keys_desc) <= *rsize; i++) {
		if (memcmp(rdesc + i, asus_kbd_keys_desc,
			   sizeof(asus_kbd_keys_desc)))
			continue;
		hid_info(hdev, "fixing up keyboard key array logical maximum\n");
		rdesc[i + ASUS_KBD_KEYS_LMAX_OFFSET] = 0x7f;
		break;
	}

	return rdesc;
}

static bool asus_kbd_has_key(const u8 *raw, u8 usage)
{
	return memchr(raw + A14_KBD_KEYS, usage, A14_KBD_NKEYS);
}

static bool asus_kbd_no_keys(const u8 *raw)
{
	return !memchr_inv(raw + A14_KBD_KEYS, 0, A14_KBD_NKEYS);
}

static bool asus_kbd_only_key(const u8 *raw, u8 usage)
{
	int i, n = 0;

	for (i = 0; i < A14_KBD_NKEYS; i++) {
		if (!raw[A14_KBD_KEYS + i])
			continue;
		if (raw[A14_KBD_KEYS + i] != usage)
			return false;
		n++;
	}
	return n == 1;
}

/* Report modifier changes straight to the keyboard input device. */
static void asus_kbd_report_mods(struct asus_hid_data *data, u8 mods, int value)
{
	if (!data->kbd_input || !mods)
		return;
	if (mods & A14_KBD_MOD_LGUI)
		input_report_key(data->kbd_input, KEY_LEFTMETA, value);
	if (mods & A14_KBD_MOD_LSHIFT)
		input_report_key(data->kbd_input, KEY_LEFTSHIFT, value);
	input_sync(data->kbd_input);
}

static void asus_copilot_report(struct asus_hid_data *data, int value)
{
	input_report_key(data->hotkey_input_dev, KEY_ASSISTANT, value);
	input_sync(data->hotkey_input_dev);
}

/*
 * A held Left GUI press timed out without the Copilot sequence following.
 * Report the held modifiers now so that a key held on its own (e.g. Super
 * for a pointer drag) still works; the held report itself is replayed to the
 * HID core when the next keyboard report arrives.
 */
static void asus_kbd_hold_timeout(struct work_struct *work)
{
	struct asus_hid_data *data = container_of(to_delayed_work(work),
						  struct asus_hid_data,
						  kbd_hold_work);
	unsigned long flags;
	u8 mods;

	spin_lock_irqsave(&data->kbd_lock, flags);
	if (data->holding) {
		mods = data->held[A14_KBD_MODS] & ~data->injected_mods;
		asus_kbd_report_mods(data, mods, 1);
		data->injected_mods |= mods;
	}
	spin_unlock_irqrestore(&data->kbd_lock, flags);
}

/*
 * Returns < 0 to swallow the report, 0 to let the HID core process it.
 * Called with the HID core's input lock held.
 */
static int asus_kbd_raw_event(struct hid_device *hdev,
			      struct asus_hid_data *data, u8 *raw)
{
	u8 mods = raw[A14_KBD_MODS];
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&data->kbd_lock, flags);

	switch (data->copilot) {
	case ASUS_COPILOT_DOWN:
		if (asus_kbd_has_key(raw, A14_KBD_USAGE_F23)) {
			ret = -EINPROGRESS;
			goto out;
		}
		asus_copilot_report(data, 0);
		data->copilot = ASUS_COPILOT_RELEASING;
		fallthrough;
	case ASUS_COPILOT_RELEASING:
		/* Swallow the firmware's trailing Shift and GUI releases. */
		if (asus_kbd_no_keys(raw) && !(mods & ~A14_KBD_COPILOT_MODS)) {
			if (!mods) {
				data->copilot = ASUS_COPILOT_IDLE;
				data->kbd_idle = true;
			}
			ret = -EINPROGRESS;
			goto out;
		}
		data->copilot = ASUS_COPILOT_IDLE;
		break;
	case ASUS_COPILOT_IDLE:
		break;
	}

	if (data->holding) {
		if (mods == A14_KBD_COPILOT_MODS &&
		    asus_kbd_only_key(raw, A14_KBD_USAGE_F23)) {
			data->holding = false;
			cancel_delayed_work(&data->kbd_hold_work);
			asus_kbd_report_mods(data, data->injected_mods, 0);
			data->injected_mods = 0;
			asus_copilot_report(data, 1);
			data->copilot = ASUS_COPILOT_DOWN;
			ret = -EINPROGRESS;
			goto out;
		}
		if (mods == A14_KBD_COPILOT_MODS && asus_kbd_no_keys(raw) &&
		    data->held[A14_KBD_MODS] == A14_KBD_MOD_LGUI) {
			memcpy(data->held, raw, A14_KBD_REPORT_SIZE);
			mod_delayed_work(system_wq, &data->kbd_hold_work,
					 msecs_to_jiffies(A14_KBD_HOLD_MS));
			ret = -EINPROGRESS;
			goto out;
		}
		/* Not the Copilot key: replay the held report, then this one. */
		data->holding = false;
		cancel_delayed_work(&data->kbd_hold_work);
		data->injected_mods = 0;
		hid_report_raw_event(hdev, HID_INPUT_REPORT, data->held,
				     A14_KBD_REPORT_SIZE, A14_KBD_REPORT_SIZE, 1);
	} else if (data->kbd_idle && mods == A14_KBD_MOD_LGUI &&
		   asus_kbd_no_keys(raw)) {
		memcpy(data->held, raw, A14_KBD_REPORT_SIZE);
		data->holding = true;
		data->injected_mods = 0;
		mod_delayed_work(system_wq, &data->kbd_hold_work,
				 msecs_to_jiffies(A14_KBD_HOLD_MS));
		ret = -EINPROGRESS;
		goto out;
	}

	data->kbd_idle = !mods && asus_kbd_no_keys(raw);
out:
	spin_unlock_irqrestore(&data->kbd_lock, flags);
	return ret;
}

static int asus_raw_event(struct hid_device *hdev, struct hid_report *report,
			  u8 *raw_data, int size)
{
	struct asus_hid_data *data = hid_get_drvdata(hdev);
	struct input_dev *input_dev;

	if (!data || !READ_ONCE(data->interfaces_ready))
		return 0;

	input_dev = data->hotkey_input_dev;

	if (report->id == A14_KBD_REPORT_ID && size == A14_KBD_REPORT_SIZE) {
		if (!data->kbd_input && report->maxfield && report->field[0]->hidinput)
			data->kbd_input = report->field[0]->hidinput->input;
		return asus_kbd_raw_event(hdev, data, raw_data);
	}

	if (report->id == A14_EC_REPORT_ID && size >= 2) {
		u8 usage_code = raw_data[1];

		switch (usage_code) {
		case A14_EC_EVT_KEY_FN_ESC:
			input_event(input_dev, EV_KEY, KEY_FN_ESC, 1);
			input_sync(input_dev);
			input_event(input_dev, EV_KEY, KEY_FN_ESC, 0);
			input_sync(input_dev);
			return 1;

		/* FN + F1, F2 and F3 are not managed by EC*/

		case A14_EC_EVT_KEY_FN_F4: {
			u8 next_level = (data->saved_brightness + 1) %
					(A14_EC_MAX_BACKLIGHT + 1);
			asus_kbd_set_brightness(&data->kbd_led_cdev,
						(enum led_brightness)next_level);
			led_classdev_notify_brightness_hw_changed(&data->kbd_led_cdev, next_level);
			return 1;
		}
		case A14_EC_EVT_KEY_FN_F5:
			input_event(input_dev, EV_KEY, KEY_BRIGHTNESSDOWN, 1);
			input_sync(input_dev);
			input_event(input_dev, EV_KEY, KEY_BRIGHTNESSDOWN, 0);
			input_sync(input_dev);
			return 1;
		case A14_EC_EVT_KEY_FN_F6:
			input_event(input_dev, EV_KEY, KEY_BRIGHTNESSUP, 1);
			input_sync(input_dev);
			input_event(input_dev, EV_KEY, KEY_BRIGHTNESSUP, 0);
			input_sync(input_dev);
			return 1;

		/* FN + F7 is not managed by the EC*/

		case A14_EC_EVT_KEY_FN_F8:
			input_event(input_dev, EV_KEY, KEY_EMOJI_PICKER, 1);
			input_sync(input_dev);
			input_event(input_dev, EV_KEY, KEY_EMOJI_PICKER, 0);
			input_sync(input_dev);
			return 1;
		case A14_EC_EVT_KEY_FN_F9:
			input_event(input_dev, EV_KEY, KEY_MICMUTE, 1);
			input_sync(input_dev);
			input_event(input_dev, EV_KEY, KEY_MICMUTE, 0);
			input_sync(input_dev);
			return 1;
		case A14_EC_EVT_KEY_FN_F10:
			input_event(input_dev, EV_KEY, KEY_CAMERA_ACCESS_TOGGLE, 1);
			input_sync(input_dev);
			input_event(input_dev, EV_KEY, KEY_CAMERA_ACCESS_TOGGLE, 0);
			input_sync(input_dev);
			return 1;
		case A14_EC_EVT_KEY_FN_F11:
			input_event(input_dev, EV_KEY, KEY_TOUCHPAD_TOGGLE, 1);
			input_sync(input_dev);
			input_event(input_dev, EV_KEY, KEY_TOUCHPAD_TOGGLE, 0);
			input_sync(input_dev);
			return 1;
		case A14_EC_EVT_KEY_FN_F12:
			input_event(input_dev, EV_KEY, KEY_PROG1, 1);
			input_sync(input_dev);
			input_event(input_dev, EV_KEY, KEY_PROG1, 0);
			input_sync(input_dev);
			return 1;
		case A14_EC_EVT_KEY_FN_F:
			input_event(input_dev, EV_KEY, KEY_PERFORMANCE, 1);
			input_sync(input_dev);
			input_event(input_dev, EV_KEY, KEY_PERFORMANCE, 0);
			input_sync(input_dev);
			return 1;
		default:
			return 0;
		}
	}
	return 0;
}

static int asus_hid_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct asus_hid_data *data = hid_get_drvdata(hdev);
	enum led_brightness saved_brightness = data->saved_brightness;

	cancel_delayed_work_sync(&data->kbd_hold_work);

	if (message.event == PM_EVENT_SUSPEND || message.event == PM_EVENT_HIBERNATE) {
		asus_kbd_set_brightness(&data->kbd_led_cdev, LED_OFF);
		data->saved_brightness = saved_brightness;
		return 0;
	}

	dev_dbg(&hdev->dev, "Runtime suspend: turning off keyboard backlight\n");
	asus_kbd_set_brightness(&data->kbd_led_cdev, LED_OFF);
	data->saved_brightness = saved_brightness;
	return 0;
}

static int asus_hid_resume(struct hid_device *hdev)
{
	struct asus_hid_data *data = hid_get_drvdata(hdev);
	int ret;
	int retry_count;

	msleep(40);

	dev_dbg(&hdev->dev, "Re-initialising EC backlight communication\n");
	retry_count = 0;
	do {
		ret = asus_kbd_led_init(hdev);
		if (ret >= 0) {
			dev_dbg(&hdev->dev,
				"EC init successful on attempt %d\n",
				retry_count + 1);
			break;
		}
		retry_count++;
		dev_warn(&hdev->dev,
			 "EC init attempt %d failed: %d, retrying...\n",
			 retry_count, ret);
		msleep(300 * retry_count);
	} while (retry_count < 5);

	if (ret < 0) {
		dev_err(&hdev->dev,
			"EC init on resume failed after %d attempts: %d\n",
			retry_count, ret);
		dev_err(&hdev->dev,
			"Keyboard backlight may not work; try reloading the driver\n");
	} else {
		dev_dbg(&hdev->dev, "EC backlight communication restored\n");
	}

	asus_kbd_set_brightness(&data->kbd_led_cdev, data->saved_brightness);

	dev_info(&hdev->dev, "Resume complete\n");
	return 0;
}

static const struct hid_device_id asus_hid_devices[] = {
	/* Tested on ASUS Zenbook A14 (UX3407QA) only. */
	{ HID_DEVICE(BUS_I2C, 0x00, ASUS_VENDOR_ID, ASUS_PRODUCT_ID) },
	{ } /* Terminating entry */
};
MODULE_DEVICE_TABLE(hid, asus_hid_devices);

static int asus_hid_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct asus_hid_data *data;

	data = devm_kzalloc(&hdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->hdev = hdev;
	hid_set_drvdata(hdev, data);
	data->saved_brightness = A14_EC_MAX_BACKLIGHT;
	spin_lock_init(&data->kbd_lock);
	INIT_DELAYED_WORK(&data->kbd_hold_work, asus_kbd_hold_timeout);
	data->kbd_idle = true;
	ret = hid_parse(hdev);
	if (ret)
		return ret;
	ret = hid_hw_start(hdev, HID_INPUT_REPORT | HID_OUTPUT_REPORT | HID_FEATURE_REPORT);
	if (ret)
		return ret;
	data->hotkey_input_dev = input_allocate_device();
	if (!data->hotkey_input_dev) {
		dev_err(&hdev->dev, "Failed to allocate hotkey input device\n");
		hid_hw_stop(hdev); return -ENOMEM;
	}
	data->hotkey_input_dev->name = "ASUS EC I2C HID hotkeys";
	data->hotkey_input_dev->phys = "i2c-hid/input1/hotkeys";
	data->hotkey_input_dev->id.bustype = hdev->bus;
	data->hotkey_input_dev->id.vendor = hdev->vendor;
	data->hotkey_input_dev->id.product = hdev->product;
	data->hotkey_input_dev->dev.parent = &hdev->dev;
	set_bit(EV_KEY, data->hotkey_input_dev->evbit);
	set_bit(KEY_FN_ESC, data->hotkey_input_dev->keybit);
	set_bit(KEY_KBDILLUMUP, data->hotkey_input_dev->keybit);
	set_bit(KEY_KBDILLUMDOWN, data->hotkey_input_dev->keybit);
	set_bit(KEY_BRIGHTNESSDOWN, data->hotkey_input_dev->keybit);
	set_bit(KEY_BRIGHTNESSUP, data->hotkey_input_dev->keybit);
	set_bit(KEY_SWITCHVIDEOMODE, data->hotkey_input_dev->keybit);
	set_bit(KEY_EMOJI_PICKER, data->hotkey_input_dev->keybit);
	set_bit(KEY_MICMUTE, data->hotkey_input_dev->keybit);
	set_bit(KEY_CAMERA_ACCESS_TOGGLE, data->hotkey_input_dev->keybit);
	set_bit(KEY_TOUCHPAD_TOGGLE, data->hotkey_input_dev->keybit);
	set_bit(KEY_PROG1, data->hotkey_input_dev->keybit);
	set_bit(KEY_PERFORMANCE, data->hotkey_input_dev->keybit);
	set_bit(KEY_ASSISTANT, data->hotkey_input_dev->keybit);

	ret = input_register_device(data->hotkey_input_dev);
	if (ret) {
		dev_err(&hdev->dev, "Failed to register hotkey input device\n");
		input_free_device(data->hotkey_input_dev); hid_hw_stop(hdev);
		return ret;
	}
	asus_kbd_led_init(hdev);
	data->kbd_led_cdev.name = "asus::kbd_backlight";
	data->kbd_led_cdev.brightness_set = asus_kbd_set_brightness;
	data->kbd_led_cdev.max_brightness = A14_EC_MAX_BACKLIGHT;
	data->kbd_led_cdev.flags = LED_BRIGHT_HW_CHANGED;
	ret = led_classdev_register(&hdev->dev, &data->kbd_led_cdev);
	if (ret) {
		input_unregister_device(data->hotkey_input_dev);
		hid_hw_stop(hdev);
		return ret;
	}
	WRITE_ONCE(data->interfaces_ready, true);
	dev_info(&hdev->dev,
		 "ASUS EC HID driver for Zenbook A14 loaded for 0x%04x:0x%04x\n",
		 ASUS_VENDOR_ID, ASUS_PRODUCT_ID);
	return 0;
}

static void asus_hid_remove(struct hid_device *hdev)
{
	struct asus_hid_data *data = hid_get_drvdata(hdev);

	WRITE_ONCE(data->interfaces_ready, false);
	cancel_delayed_work_sync(&data->kbd_hold_work);
	led_classdev_unregister(&data->kbd_led_cdev);
	input_unregister_device(data->hotkey_input_dev);
	hid_hw_stop(hdev);
}

static struct hid_driver hid_asus_ec_driver = {
	.name       = "hid-asus-ec",
	.id_table   = asus_hid_devices,
	.probe      = asus_hid_probe,
	.remove     = asus_hid_remove,
	.report_fixup = asus_report_fixup,
	.raw_event  = asus_raw_event,
	.suspend    = asus_hid_suspend,
	.resume     = asus_hid_resume,
};
module_hid_driver(hid_asus_ec_driver);
MODULE_AUTHOR("Alexandru Marc Serdeliuc <serdeliuk@yahoo.com>");
MODULE_DESCRIPTION("ASUS EC HID driver for Zenbook A14 (UX3407QA)");
MODULE_LICENSE("GPL");
