// SPDX-License-Identifier: GPL-2.0
/*
 *  Huawei WMI laptop extras driver
 *
 *  Copyright (C) 2018	      Ayman Bagabas <ayman.bagabas@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/wmi.h>

/*
 * Huawei WMI GUIDs
 */
#define AMW0_METHOD_GUID "ABBC0F5B-8EA1-11D1-A000-C90629100000"

#define WMI0_EVENT_GUID "59142400-C6A3-40fa-BADB-8A2652834100"
#define AMW0_EVENT_GUID "ABBC0F5C-8EA1-11D1-A000-C90629100000"

#define WMI0_EXPENSIVE_GUID "39142400-C6A3-40fa-BADB-8A2652834100"

/* AMW0_commands */

enum wmaa_cmd {
	BATTERY_GET, /* \GBTT 0x00001103 */
	BATTERY_SET, /* \SBTT 0xXXYY1003 */
	FN_LOCK_GET, /* \GFRS 0x00000604 */
	FN_LOCK_SET, /* \SFRS 0x000X0704 */
	MICMUTE_LED, /* \SMLS 0x000X0b04 */
};

struct amw0_arg {
	u8 arg0;
	u8 arg1;
	u8 arg2;
	u8 arg3;
};

enum fn_state {
	FN_LOCK_OFF = 0x01,
	FN_LOCK_ON = 0x02,
};

struct huawei_wmi {
	struct led_classdev cdev;
	struct mutex wmi_mutex;
	struct platform_device *pdev;
	struct wmi_device *wdev;
};

static struct platform_device *pdev;

static const struct key_entry huawei_wmi_keymap[] = {
	{ KE_KEY,    0x281, { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY,    0x282, { KEY_BRIGHTNESSUP } },
	{ KE_KEY,    0x284, { KEY_MUTE } },
	{ KE_KEY,    0x285, { KEY_VOLUMEDOWN } },
	{ KE_KEY,    0x286, { KEY_VOLUMEUP } },
	{ KE_KEY,    0x287, { KEY_MICMUTE } },
	{ KE_KEY,    0x289, { KEY_WLAN } },
	// Huawei |M| key
	{ KE_KEY,    0x28a, { KEY_CONFIG } },
	// Keyboard backlit
	{ KE_IGNORE, 0x293, { KEY_KBDILLUMTOGGLE } },
	{ KE_IGNORE, 0x294, { KEY_KBDILLUMUP } },
	{ KE_IGNORE, 0x295, { KEY_KBDILLUMUP } },
	{ KE_END,	 0 }
};

/* Utils */

static int huawei_wmi_eval(struct device *dev, struct amw0_arg *arg, void *buf, size_t buflen)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in;
	union acpi_object *obj;
	acpi_status status;
	size_t len;
	int err = 0;

	in.length = sizeof(struct amw0_arg);
	in.pointer = (u32 *)arg;
	mutex_lock(&huawei->wmi_mutex);
	status = wmi_evaluate_method(AMW0_METHOD_GUID, 0, 1, &in, &out);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Failed to evaluate wmi method\n");
		err = -ENODEV;
		goto wmi_eval_fail;
	}

	/* WMAA returns a package with two buffer elements. The first buffer
	 * is 4 bytes long and the second is 0x100 (256) bytes long. The
	 * first buffer is always zeros. The second stores the output
	 * from every call. The first byte of the second buffer always
	 * have the return status of the called command.
	 */
	obj = out.pointer;
	if (!obj) {
		err = -ENODEV;
		goto wmi_eval_fail;
	}
	if (obj->type != ACPI_TYPE_PACKAGE) {
		dev_err(dev, "Unknown response type %d\n", obj->type);
		err = -ENODEV;
		goto wmi_eval_fail;
	}
	if (obj->package.count != 2) {
		dev_err(dev, "Unknown package count %d\n", obj->package.count);
		err = -ENODEV;
		goto wmi_eval_fail;
	}

	obj = &(obj->package.elements[1]);
	if (!obj || obj->type != ACPI_TYPE_BUFFER) {
		dev_err(dev, "Unknown response type %d\n", obj->type);
		err = -ENODEV;
		goto wmi_eval_fail;
	}

	if (buf) {
		len = min(buflen, obj->buffer.length);
		memcpy(buf, obj->buffer.pointer, len);
	}

wmi_eval_fail:
	mutex_unlock(&huawei->wmi_mutex);
	kfree(out.pointer);
	return err;
}

static int huawei_wmi_cmd(struct device *dev, enum wmaa_cmd cmd, struct amw0_arg *arg, void *out, size_t outlen)
{
	struct amw0_arg parm;
	u8 buf[0x100] = { 0xff };
	int err;

	if (!arg) {
		parm.arg0 = parm.arg1 = parm.arg2 = parm.arg3 = 0;
		arg = &parm;
	}

	switch (cmd) {
		case BATTERY_SET:
			arg->arg0 = 0x03;
			arg->arg1 = 0x10;
			break;
		case BATTERY_GET:
			arg->arg0 = 0x03;
			arg->arg1 = 0x11;
			break;
		case FN_LOCK_GET:
			arg->arg0 = 0x04;
			arg->arg1 = 0x06;
			break;
		case FN_LOCK_SET:
			arg->arg0 = 0x04;
			arg->arg1 = 0x07;
			break;
		case MICMUTE_LED:
			arg->arg0 = 0x04;
			arg->arg1 = 0x0b;
			break;
		default:
			dev_err(dev, "Command not supported\n");
			return -EINVAL;
	}

	/* Some models require calling WMAA twice to execute
	 * a command. We call WMAA and if we get a non-zero return
	 * status we evaluate WMAA again. If we get another non-zero
	 * return, we return -ENXIO. This way we don't need to 
	 * check for return status anywhere we call huawei_wmi_cmd.
	 */
	err = huawei_wmi_eval(dev, arg, buf, 0x100);
	if (err)
		return err;
	if (*buf) {
		err = huawei_wmi_eval(dev, arg, buf, 0x100);
		if (err) {
			return err;
		}
		if (*buf) {
			dev_err(dev, "Invalid command, got: %d\n", *buf);
			return -ENXIO;
		}
	}
	if (out)
		memcpy(out, buf, outlen);

	return 0;
}

/* Input */

static void huawei_wmi_process_key(struct wmi_device *wdev, int code)
{
	struct input_dev *idev = dev_get_drvdata(&wdev->dev);
	const struct key_entry *key;

	/*
	 * WMI0 uses code 0x80 to indicate a hotkey event.
	 * The actual key is fetched from the method WQ00
	 * using WMI0_EXPENSIVE_GUID.
	 */
	if (code == 0x80) {
		struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
		union acpi_object *obj;
		acpi_status status;

		status = wmi_query_block(WMI0_EXPENSIVE_GUID, 0, &response);
		if (ACPI_FAILURE(status))
			return;

		obj = (union acpi_object *)response.pointer;
		if (obj && obj->type == ACPI_TYPE_INTEGER)
			code = obj->integer.value;

		kfree(response.pointer);
	}

	key = sparse_keymap_entry_from_scancode(idev, code);
	if (!key) {
		dev_info(&wdev->dev, "Unknown key pressed, code: 0x%04x\n", code);
		return;
	}

	sparse_keymap_report_entry(idev, key, 1, true);
}

static void huawei_wmi_input_notify(struct wmi_device *wdev,
		union acpi_object *obj)
{
	if (obj->type == ACPI_TYPE_INTEGER)
		huawei_wmi_process_key(wdev, obj->integer.value);
	else
		dev_info(&wdev->dev, "Bad response type %d\n", obj->type);
}

static int huawei_wmi_input_setup(struct wmi_device *wdev)
{
	struct input_dev *idev;
	int err;

	idev = devm_input_allocate_device(&wdev->dev);
	if (!idev)
		return -ENOMEM;

	dev_set_drvdata(&wdev->dev, idev);

	idev->name = "Huawei WMI hotkeys";
	idev->phys = "wmi/input0";
	idev->id.bustype = BUS_HOST;
	idev->dev.parent = &wdev->dev;

	err = sparse_keymap_setup(idev, huawei_wmi_keymap, NULL);
	if (err)
		return err;

	return input_register_device(idev);
}

static int huawei_wmi_input_destroy(struct wmi_device *wdev)
{
	struct input_dev *idev = dev_get_drvdata(&wdev->dev);
	input_unregister_device(idev);
	return 0;
}

static const struct wmi_device_id huawei_wmi_input_id_table[] = {
	{ .guid_string = WMI0_EVENT_GUID },
	{ .guid_string = AMW0_EVENT_GUID },
	{  }
};

static struct wmi_driver huawei_wmi_input_driver = {
	.driver = {
		.name = "huawei-wmi",
	},
	.id_table = huawei_wmi_input_id_table,
	.probe = huawei_wmi_input_setup,
	.remove = huawei_wmi_input_destroy,
	.notify = huawei_wmi_input_notify,
};

/* LEDs */

static void huawei_wmi_micmute_led_set(struct led_classdev *led_cdev,
		enum led_brightness brightness)
{
	struct amw0_arg arg = { 0, 0, brightness, 0 };
	huawei_wmi_cmd(led_cdev->dev->parent, MICMUTE_LED, &arg, NULL, NULL);
}

static int huawei_wmi_leds_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	huawei->cdev.name = "platform::micmute";
	huawei->cdev.max_brightness = 1;
	huawei->cdev.brightness_set = huawei_wmi_micmute_led_set;
	huawei->cdev.default_trigger = "audio-micmute";
	huawei->cdev.brightness = ledtrig_audio_get(LED_AUDIO_MICMUTE);
	huawei->cdev.dev = dev->parent;
	huawei->cdev.flags = LED_CORE_SUSPENDRESUME;

	return devm_led_classdev_register(dev, &huawei->cdev);
}

/* Battery protection */

static int huawei_wmi_battery_get(struct device *dev, int *low, int *high)
{
	char ret[0x100] = { 0 };
	int err, i;

	err = huawei_wmi_cmd(dev, BATTERY_GET, NULL, &ret, sizeof(struct amw0_arg));
	if (err)
		return -EINVAL;

	/* Returned buffer positions are either 0x03 and 0x02
	 * or 0x02 and 0x01. 0x00 reserved for return status.
	 */
	for(i = 0x100 -1; i > 0; i--) {
		if (ret[i]) {
			*high = ret[i];
			*low = ret[i-1];
			break;
		}
	}

	return 0;
}

static int huawei_wmi_battery_set(struct device *dev, int low, int high)
{
	struct amw0_arg arg = { 0, 0, low, high };

	/* This is an edge case were some models turn battery protection
	 * off without changing their thresholds values. We clear the
	 * values before turning off protection. Since this function
	 * writes to EC memory, we wait before calling it again.
	 */
	// FIXME: this call gets ignored
	if (low == 0 && high == 100)
		huawei_wmi_battery_set(dev, 0, 0);

	return huawei_wmi_cmd(dev, BATTERY_SET, &arg, NULL, NULL);
}

/* Fn lock */

static int huawei_wmi_fn_lock_get(struct device *dev, int *on)
{
	char ret[0x100] = { 0 };
	int err, i;

	err = huawei_wmi_cmd(dev, FN_LOCK_GET, NULL, &ret, 0x100);
	if (err)
		return -EINVAL;

	for(i = 0x100 -1; i > 0; i--) {
		if (ret[i]) {
			*on = (ret[i] == FN_LOCK_OFF) ? 0 : 1;
			break;
		}
	}

	return 0;
}

static int huawei_wmi_fn_lock_set(struct device *dev, int on)
{
	struct amw0_arg arg = { 0, 0, (on) ? FN_LOCK_ON : FN_LOCK_OFF, 0 };
	return huawei_wmi_cmd(dev, FN_LOCK_SET, &arg, NULL, NULL);
}

/* sysfs */

static ssize_t charge_thresholds_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size) {
	int low, high;

	if (sscanf(buf, "%d %d", &low, &high) != 2 ||
			low < 0 || high > 100 ||
			low > high ||
			huawei_wmi_battery_set(dev, low, high))
		return -EINVAL;

	return size;
}

static ssize_t fn_lock_state_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size) {
	int on;

	if (kstrtoint(buf, 10, &on) ||
			on < 0 || on > 1 ||
			huawei_wmi_fn_lock_set(dev, on))
		return -EINVAL;

	return size;
}

static ssize_t charge_thresholds_show(struct device *dev,
		struct device_attribute *attr,
		char *buf) {
	int err, low, high;

	low = high = 0;
	err = huawei_wmi_battery_get(dev, &low, &high);
	if (err)
		return -EINVAL;

	return sprintf(buf, "%d %d\n", low, high);
}

static ssize_t fn_lock_state_show(struct device *dev,
		struct device_attribute *attr,
		char *buf) {
	int err, on;

	on = 0;
	err = huawei_wmi_fn_lock_get(dev, &on);
	if (err)
		return -EINVAL;

	return sprintf(buf, "%d\n", on);
}

static DEVICE_ATTR_RW(charge_thresholds);
static DEVICE_ATTR_RW(fn_lock_state);

static struct attribute *huawei_wmi_attrs[] = {
	&dev_attr_charge_thresholds.attr,
	&dev_attr_fn_lock_state.attr,
	NULL
};

static const struct attribute_group huawei_wmi_group = {
	.attrs = huawei_wmi_attrs
};

static int huawei_wmi_probe(struct platform_device *pdev)
{
	struct huawei_wmi *huawei;
	int err;

	huawei = devm_kzalloc(&pdev->dev, sizeof(struct huawei_wmi), GFP_KERNEL);
	if (!huawei)
		return -ENOMEM;

	// TODO: add quirks?

	huawei->pdev = pdev;
	dev_set_drvdata(&pdev->dev, huawei);
	mutex_init(&huawei->wmi_mutex);
	
	// TODO: check laptop capabilities and features
	err = sysfs_create_group(&pdev->dev.kobj, &huawei_wmi_group);
	if (err)
		goto fail_sysfs;

	err = huawei_wmi_leds_setup(&pdev->dev);
	if (err)
		goto fail_leds;

	return 0;

fail_leds:
	sysfs_remove_group(&pdev->dev.kobj, &huawei_wmi_group);
fail_sysfs:
	return err;
}

static int huawei_wmi_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &huawei_wmi_group);
	return 0;
}

/* Huawei driver */

static struct platform_driver huawei_wmi_driver = {
	.driver = {
		.name = "huawei-wmi",
	},
	.probe = huawei_wmi_probe,
	.remove = huawei_wmi_remove,
};

static __init int huawei_wmi_init(void)
{
	int err;

	err = wmi_driver_register(&huawei_wmi_input_driver);
	if (err)
		pr_err("Unable to register wmi input driver\n");

	if (wmi_has_guid(AMW0_METHOD_GUID)) {
		err = platform_driver_register(&huawei_wmi_driver);
		if (err) {
			pr_err("Failed to register platform driver\n");
			return 0;
		}

		pdev = platform_device_register_simple("huawei-wmi", -1, NULL, 0);
		if (IS_ERR(pdev)) {
			pr_err("Failed to register platform device\n");
			err = PTR_ERR(pdev);
			return err;
		}
	}

	return 0;
}

static __exit void huawei_wmi_exit(void)
{
	wmi_driver_unregister(&huawei_wmi_input_driver);
	if (wmi_has_guid(AMW0_METHOD_GUID)) {
		platform_device_unregister(pdev);
		platform_driver_unregister(&huawei_wmi_driver);
	}
}

module_init(huawei_wmi_init);
module_exit(huawei_wmi_exit);

MODULE_ALIAS("wmi:"AMW0_METHOD_GUID);
MODULE_ALIAS("wmi:"AMW0_EVENT_GUID);
MODULE_ALIAS("wmi:"WMI0_EVENT_GUID);
MODULE_AUTHOR("Ayman Bagabas <ayman.bagabas@gmail.com>");
MODULE_DESCRIPTION("Huawei WMI laptop driver");
MODULE_LICENSE("GPL v2");
