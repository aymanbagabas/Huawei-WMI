// SPDX-License-Identifier: GPL-2.0
/*
 *  Huawei WMI laptop extras driver
 *
 *  Copyright (C) 2018	      Ayman Bagabas <ayman.bagabas@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
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

static struct huawei_wmi_priv {
	struct led_classdev cdev;
	struct platform_device *pdev;
} *huawei;

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
	// Keyboard backlight
	{ KE_IGNORE, 0x293, { KEY_KBDILLUMTOGGLE } },
	{ KE_IGNORE, 0x294, { KEY_KBDILLUMUP } },
	{ KE_IGNORE, 0x295, { KEY_KBDILLUMUP } },
	{ KE_END,	 0 }
};

/* Utils */

static int huawei_wmi_eval(struct amw0_arg *arg, void *buf, size_t buflen)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in;
	union acpi_object *obj;
	acpi_status status;
	size_t len;

	in.length = sizeof(struct amw0_arg);
	in.pointer = (u32 *)arg;
	status = wmi_evaluate_method(AMW0_METHOD_GUID, 0, 1, &in, &out);
	if (ACPI_FAILURE(status)) {
		dev_err(&huawei->pdev->dev, "Failed to evaluate wmi method\n");
		return -ENODEV;
	}

	/* WMAA returns a package with two buffer elements. The first buffer
	 * is 4 bytes long and the second is 0x100 (256) bytes long. The
	 * first buffer is always zeros. The second stores the output
	 * from every call. The first byte of the second buffer always
	 * have the return status of the called command.
	 */
	obj = out.pointer;
	if (!obj)
		return -ENODEV;
	if (obj->type != ACPI_TYPE_PACKAGE) {
		dev_err(&huawei->pdev->dev, "Unknown response type %d\n", obj->type);
		kfree(obj);
		return -ENODEV;
	}
	if (obj->package.count != 2) {
		dev_err(&huawei->pdev->dev, "Unknown package count %d\n", obj->package.count);
		kfree(obj);
		return -ENODEV;
	}

	obj = &(obj->package.elements[1]);
	if (!obj || obj->type != ACPI_TYPE_BUFFER) {
		dev_err(&huawei->pdev->dev, "Unknown response type %d\n", obj->type);
		kfree(out.pointer);
		return -ENODEV;
	}

	if (buf) {
		len = min(buflen, obj->buffer.length);
		memcpy(buf, obj->buffer.pointer, len);
	}
	kfree(out.pointer);

	return 0;
}

static int huawei_wmi_cmd(enum wmaa_cmd cmd, struct amw0_arg *arg, void *out, size_t outlen)
{
	u8 buf[0x100] = { 0 };
	int err;

	if (!arg) {
		struct amw0_arg parm;
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
			pr_err("Command not supported\n");
			return -EINVAL;
	}
	pr_info("wmi exec 0x%x 0x%x 0x%x 0x%x\n", arg->arg0, arg->arg1, arg->arg2, arg->arg3);
	pr_info("wmi exec 0x%08x\n", *arg);

	/* Some models require calling WMAA twice to execute
	 * a command. We call WMAA and if we get a non-zero return
	 * status we evaluate WMAA again. If we get another non-zero
	 * return, we return -ENXIO. This way we don't need to 
	 * check for return status anywhere we call huawei_wmi_cmd.
	 */
	err = huawei_wmi_eval(arg, buf, 0x100);
	if (err)
		return err;
	if (*buf) {
		err = huawei_wmi_eval(arg, buf, 0x100);
		if (err) {
			return err;
		}
		if (*buf) {
			dev_err(&huawei->pdev->dev, "Invalid command, got: %d\n", *buf);
			return -ENXIO;
		}
	}
	if (out)
		memcpy(out, buf, outlen);
	pr_info(" Buffer:\n");
	int i;
	for(i = 0; i < 10; i++)
		pr_info(" 0x%x\n", buf[i]);
	return 0;
}

/* LEDs */

static int huawei_wmi_micmute_led_set(struct led_classdev *led_cdev,
		enum led_brightness brightness)
{
	struct amw0_arg arg = { 0, 0, brightness, 0 };
	return huawei_wmi_cmd(MICMUTE_LED, &arg, NULL, NULL);
}

static int huawei_wmi_leds_setup(struct huawei_wmi_priv *priv)
{
	priv->cdev.name = "platform::micmute";
	priv->cdev.max_brightness = 1;
	priv->cdev.brightness_set_blocking = huawei_wmi_micmute_led_set;
	priv->cdev.default_trigger = "audio-micmute";
	priv->cdev.brightness = ledtrig_audio_get(LED_AUDIO_MICMUTE);
	priv->cdev.dev = &priv->pdev->dev;
	priv->cdev.flags = LED_CORE_SUSPENDRESUME;

	return devm_led_classdev_register(&priv->pdev->dev, &priv->cdev);
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

static int huawei_wmi_input_probe(struct wmi_device *wdev)
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

static int huawei_wmi_input_remove(struct wmi_device *wdev)
{
	struct input_dev *idev = dev_get_drvdata(&wdev->dev);

	input_unregister_device(idev);

	return 0;
}

static const struct wmi_device_id huawei_wmi_id_table[] = {
	{ .guid_string = WMI0_EVENT_GUID },
	{ .guid_string = AMW0_EVENT_GUID },
	{  }
};

static struct wmi_driver huawei_wmi_input_driver = {
	.driver = {
		.name = "huawei-wmi",
	},
	.id_table = huawei_wmi_id_table,
	.probe = huawei_wmi_input_probe,
	.remove = huawei_wmi_input_remove,
	.notify = huawei_wmi_input_notify,
};

/* Battery protection */

static int huawei_wmi_battery_get(int *low, int *high)
{
	struct amw0_arg ret;
	long err;

	err = huawei_wmi_cmd(BATTERY_GET, NULL, &ret, sizeof(struct amw0_arg));
	if (err)
		return -EINVAL;

	/* Returned buffer positions are either 0x03 and 0x02
	 * or 0x02 and 0x01. 0x00 reserved for return status.
	 */
	if (ret.arg3) {
		*high = ret.arg3;
		*low = ret.arg2;
	} else if (ret.arg2) {
		*high = ret.arg2;
		*low = ret.arg1;
	} else if (ret.arg1) {
		*low = ret.arg1;
	}

	return 0;
}

static int huawei_wmi_battery_set(int low, int high)
{
	struct amw0_arg arg = { 0, 0, low, high };
	return huawei_wmi_cmd(BATTERY_SET, &arg, NULL, NULL);
}

static int huawei_wmi_fn_lock_get(int *on)
{
	struct amw0_arg ret;
	int err;

	err = huawei_wmi_cmd(FN_LOCK_GET, NULL, &ret, sizeof(struct amw0_arg));
	if (err)
		return -EINVAL;

	if (ret.arg3)
		*on = (ret.arg3 == FN_LOCK_OFF) ? 0 : 1;
	else if (ret.arg2)
		*on = (ret.arg2 == FN_LOCK_OFF) ? 0 : 1;
	else if (ret.arg1)
		*on = (ret.arg1 == FN_LOCK_OFF) ? 0 : 1;

	return 0;
}

static int huawei_wmi_fn_lock_set(int on)
{
	struct amw0_arg arg = { 0, 0, (on) ? FN_LOCK_ON : FN_LOCK_OFF, 0 };
	return huawei_wmi_cmd(FN_LOCK_SET, &arg, NULL, NULL);
}

/* sysfs */

static ssize_t charge_thresholds_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size) {
	int low, high;

	if (sscanf(buf, "%d %d", &low, &high) != 2 ||
			low < 0 || low > 100 ||
			high < 0 || high > 100 ||
			huawei_wmi_battery_set(low, high))
		return -EINVAL;

	return size;
}

static ssize_t fn_lock_state_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size) {
	int on;

	if (kstrtoint(buf, 10, &on) ||
			on < 0 || on > 1 ||
			huawei_wmi_fn_lock_set(on))
		return -EINVAL;

	return size;
}

static ssize_t charge_thresholds_show(struct device *dev,
		struct device_attribute *attr,
		char *buf) {
	int err, low, high;

	low = high = 0;
	err = huawei_wmi_battery_get(&low, &high);
	if (err)
		return -EINVAL;

	return sprintf(buf, "%d %d\n", low, high);
}

static ssize_t fn_lock_state_show(struct device *dev,
		struct device_attribute *attr,
		char *buf) {
	int err, on;

	on = 0;
	err = huawei_wmi_fn_lock_get(&on);
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

/* Huawei driver */

static struct platform_driver huawei_wmi_driver = {
	.driver = {
		.name = "huawei-wmi",
	},
};

static __init int huawei_wmi_init(void)
{
	int err;

	huawei = kzalloc(sizeof(struct huawei_wmi_priv), GFP_KERNEL);
	if (!huawei)
		return -ENOMEM;

	pr_info("start init\n");
	err = platform_driver_register(&huawei_wmi_driver);
	if (err) {
		pr_err("Failed to register platform driver\n");
		goto fail_pfdrv;
	}
	pr_info("done pf driver\n");

	huawei->pdev = platform_device_register_simple("huawei-wmi", -1, NULL, 0);
	if (IS_ERR(huawei->pdev)) {
		pr_err("Failed to register platform device\n");
		err = PTR_ERR(huawei->pdev);
		goto fail_pfdev;
	}
	pr_info("done pf dev\n");

	// TODO: add quirks

	platform_set_drvdata(huawei->pdev, huawei);

	err = wmi_driver_register(&huawei_wmi_input_driver);
	if (err) {
		pr_err("Failed to register wmi driver\n");
		goto fail_input;
	}
	pr_info("done wmi dri\n");
	
	// TODO: check laptop capabilities and features
	err = sysfs_create_group(&huawei->pdev->dev.kobj, &huawei_wmi_group);
	if (err)
		goto fail_sysfs;

	err = huawei_wmi_leds_setup(huawei);
	if (err) {
		pr_err("Failed to register leds\n");
		goto fail_leds;
	}
	pr_info("finish init\n");

	return 0;

fail_leds:
	sysfs_remove_group(&huawei->pdev->dev.kobj, &huawei_wmi_group);
fail_sysfs:
	wmi_driver_unregister(&huawei_wmi_input_driver);
fail_input:
	platform_device_unregister(huawei->pdev);
fail_pfdev:
	huawei->pdev = NULL;
	platform_driver_unregister(&huawei_wmi_driver);
fail_pfdrv:
	kfree(huawei);
	return err;
}

static __exit void huawei_wmi_exit(void)
{
	pr_info("start exit\n");
	sysfs_remove_group(&huawei->pdev->dev.kobj, &huawei_wmi_group);
	wmi_driver_unregister(&huawei_wmi_input_driver);
	platform_device_unregister(huawei->pdev);
	platform_driver_unregister(&huawei_wmi_driver);
	kfree(huawei);
	pr_info("finish exit\n");
}

module_init(huawei_wmi_init);
module_exit(huawei_wmi_exit);

MODULE_DEVICE_TABLE(wmi, huawei_wmi_id_table);
MODULE_AUTHOR("Ayman Bagabas <ayman.bagabas@gmail.com>");
MODULE_DESCRIPTION("Huawei WMI laptop driver");
MODULE_LICENSE("GPL v2");
