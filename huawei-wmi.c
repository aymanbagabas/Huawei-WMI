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
	BATTERY_GET, /* \GBTT */
	BATTERY_SET, /* \SBTT */
	FN_LOCK_GET, /* \GFRS */
	FN_LOCK_SET, /* \SFRS */
	MICMUTE_LED, /* \SMLS */
};

struct amw0_arg {
	u8 arg0;
	u8 arg1;
	u8 arg2;
	u8 arg3;
};

struct amw0_ret {
	u8 arg0;
	u8 arg1;
	u8 arg2;
};

	/*BATTERY_PROT_SET = 0x00001003,*/
	/*BATTERY_PROT_GET = 0x00001103,*/
	/*FNLK_GET = 0x00000604,*/
	/*FNLK_SET = 0x00000704,*/
	/*MICMUTE_LED_SET = 0x00000b04,*/

enum thresh {
	THRESHOLD_START,
	THRESHOLD_STOP,
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

static int huawei_wmi_exec(enum wmaa_cmd cmd, struct amw0_arg *arg, struct amw0_ret *ret)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in;
	union acpi_object *obj;
	acpi_status status;

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
			pr_err("Invalid command\n");
			return -EINVAL;
	}
	pr_debug("wmi exec 0x%x 0x%x 0x%x 0x%x\n", arg->arg0, arg->arg1, arg->arg2, arg->arg3);
	pr_debug("wmi exec 0x%08x\n", *arg);

	in.length = sizeof(struct amw0_arg);
	in.pointer = (u32 *)arg;
	status = wmi_evaluate_method(AMW0_METHOD_GUID, 0, 1, &in, &out);
	if (ACPI_FAILURE(status)) {
		dev_err(&huawei->pdev->dev, "Failed to execute wmi method\n");
		return -ENODEV;
	}

	obj = out.pointer;
	if (!obj || obj->type != ACPI_TYPE_PACKAGE) {
		dev_err(&huawei->pdev->dev, "Invalid acpi_object: expected 0x%x got 0x%x\n",
				ACPI_TYPE_BUFFER, obj->type);
		return -EINVAL;
	}

	// TODO: cleanup
	if (ret && obj->package.count >= 2) {
		union acpi_object *elem = &(obj->package.elements[1]);
		if (elem->type != ACPI_TYPE_BUFFER) {
			return -EINVAL;
		}
		memcpy(ret, elem->buffer.pointer, sizeof(struct amw0_ret));
	}

	kfree(obj);

	return 0;
}

/* LEDs */

static int huawei_wmi_micmute_led_set(struct led_classdev *led_cdev,
		enum led_brightness brightness)
{
	struct amw0_arg arg = { 0, 0, brightness, 0 };
	return huawei_wmi_exec(MICMUTE_LED, &arg, NULL);

}

static int huawei_wmi_leds_setup(struct huawei_wmi_priv *priv)
{
	pr_debug("leds setup\n");

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
	pr_debug("wmi probe\n");

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

static int huawei_wmi_battery_get(enum thresh thresh, int *val)
{
	struct amw0_arg arg = { 0, 0, 0, 0 };
	struct amw0_ret ret = { 0, 0, 0 };
	int err;

	err = huawei_wmi_exec(BATTERY_GET, &arg, &ret);
	if (err)
		return err;
	pr_debug("bat get %d %d %d\n", ret.arg0, ret.arg1, ret.arg2);
	pr_debug("bat get 0x%06x\n", ret);
	pr_debug("bat get 0x%08x\n", arg);

	switch (thresh) {
		case THRESHOLD_START:
			*val = ret.arg1;
			break;
		case THRESHOLD_STOP:
			*val = ret.arg2;
			break;
		default:
			pr_err("Invalid arguments\n");
			return -EINVAL;
	}

	return ret.arg0;
}

static int huawei_wmi_battery_set(enum thresh thresh, int *val)
{
	struct amw0_arg arg = { 0, 0, 0, 0 };
	struct amw0_ret ret = { 0, 0, 0 };
	int err, tmp;

	// TODO: cleanup
	switch (thresh) {
		case THRESHOLD_START:
			err = huawei_wmi_battery_get(THRESHOLD_STOP, &tmp);
			if (err)
				return -EINVAL;

			arg.arg2 = *val;
			arg.arg3 = tmp;
			break;
		case THRESHOLD_STOP:
			err = huawei_wmi_battery_get(THRESHOLD_START, &tmp);
			if (err)
				return -EINVAL;

			arg.arg2 = tmp;
			arg.arg3 = *val;
			break;
		default:
			pr_err("Invalid argument\n");
			return -EINVAL;
	}
	err = huawei_wmi_exec(BATTERY_SET, &arg, &ret);
	if (err)
		return err;

	pr_debug("bat set %d %d %d %d\n", arg.arg0, arg.arg1, arg.arg2, arg.arg3);
	pr_debug("bat set 0x%08x\n", arg);

	return ret.arg0;
}

static int huawei_wmi_fn_lock_get(int *val)
{
	struct amw0_arg arg = { 0, 0, 0, 0 };
	struct amw0_ret ret = { 0, 0, 0 };
	int err;

	err = huawei_wmi_exec(FN_LOCK_GET, &arg, &ret);
	if (err)
		return err;

	*val = (ret.arg1 == FN_LOCK_OFF) ? 0 : 1;

	pr_debug("fn get %d %d %d %d\n", arg.arg0, arg.arg1, arg.arg2, arg.arg3);
	pr_debug("fn get 0x%08x\n", arg);

	return ret.arg0;
}

static int huawei_wmi_fn_lock_set(int *val)
{
	struct amw0_arg arg = { 0, 0, 0, 0 };
	struct amw0_ret ret = { 0, 0, 0 };
	int err;

	arg.arg2 = (*val) ? FN_LOCK_ON : FN_LOCK_OFF;
	err = huawei_wmi_exec(FN_LOCK_SET, &arg, &ret);
	if (err)
		return err;
	pr_debug("fn set %d %d %d %d\n", arg.arg0, arg.arg1, arg.arg2, arg.arg3);
	pr_debug("fn set 0x%08x\n", arg);
	
	return ret.arg0;
}

/* sysfs */

static ssize_t charge_start_threshold_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size) {
	int err, val;

	err = kstrtoint(buf, 10, &val);
	if (err)
		return err;
	if (val < 0 || val > 99 ||
			huawei_wmi_battery_set(THRESHOLD_START, &val))
		return -EINVAL;

	return size;
}

static ssize_t charge_stop_threshold_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size) {
	int err, val;

	err = kstrtoint(buf, 10, &val);
	if (err)
		return err;
	if (val < 1 || val > 100 ||
			huawei_wmi_battery_set(THRESHOLD_STOP, &val))
		return -EINVAL;

	return size;
}

static ssize_t fn_lock_state_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size) {
	int err, val;

	err = kstrtoint(buf, 10, &val);
	if (err)
		return err;
	pr_debug("fn store %d\n", val);
	if (val < 0 || val > 1 ||
			huawei_wmi_fn_lock_set(&val))
		return -EINVAL;

	return size;
}

static ssize_t charge_start_threshold_show(struct device *dev,
		struct device_attribute *attr,
		char *buf) {
	int err, val;

	err = huawei_wmi_battery_get(THRESHOLD_START, &val);
	if (err)
		return err;

	return sprintf(buf, "%d\n", val);
}

static ssize_t charge_stop_threshold_show(struct device *dev,
		struct device_attribute *attr,
		char *buf) {
	int err, val;

	err = huawei_wmi_battery_get(THRESHOLD_STOP, &val);
	if (err)
		return err;

	return sprintf(buf, "%d\n", val);
}

static ssize_t fn_lock_state_show(struct device *dev,
		struct device_attribute *attr,
		char *buf) {
	int err, val;

	err = huawei_wmi_fn_lock_get(&val);
	if (err)
		return err;

	return sprintf(buf, "%d\n", val);
}

static DEVICE_ATTR_RW(charge_start_threshold);
static DEVICE_ATTR_RW(charge_stop_threshold);
static DEVICE_ATTR_RW(fn_lock_state);

static struct attribute *huawei_wmi_attrs[] = {
	&dev_attr_charge_start_threshold.attr,
	&dev_attr_charge_stop_threshold.attr,
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

	pr_debug("start init\n");
	err = platform_driver_register(&huawei_wmi_driver);
	if (err) {
		pr_err("Failed to register platform driver\n");
		return err;
	}
	pr_debug("done pf driver\n");

	huawei->pdev = platform_device_register_simple("huawei-wmi", -1, NULL, 0);
	if (IS_ERR(huawei->pdev)) {
		pr_err("Failed to register platform device\n");
		err = PTR_ERR(huawei->pdev);
		huawei->pdev = NULL;
		return err;
	}
	pr_debug("done pf dev\n");

	platform_set_drvdata(huawei->pdev, huawei);

	err = wmi_driver_register(&huawei_wmi_input_driver);
	if (err) {
		pr_err("Failed to register wmi driver\n");
		return err;
	}
	pr_debug("done wmi dri\n");
	
	// TODO: check laptop capabilities and features
	err = sysfs_create_group(&huawei->pdev->dev.kobj, &huawei_wmi_group);
	if (err)
		return err;
	err = huawei_wmi_leds_setup(huawei);

	if (err) {
		pr_err("Failed to register leds\n");
		return err;
	}
	pr_debug("finish init\n");

	return 0;
}

static __exit void huawei_wmi_exit(void)
{
	pr_debug("start exit\n");
	sysfs_remove_group(&huawei->pdev->dev.kobj, &huawei_wmi_group);
	wmi_driver_unregister(&huawei_wmi_input_driver);
	platform_device_unregister(huawei->pdev);
	platform_driver_unregister(&huawei_wmi_driver);
	kfree(huawei);
	pr_debug("finish exit\n");
}

module_init(huawei_wmi_init);
module_exit(huawei_wmi_exit);

MODULE_DEVICE_TABLE(wmi, huawei_wmi_id_table);
MODULE_AUTHOR("Ayman Bagabas <ayman.bagabas@gmail.com>");
MODULE_DESCRIPTION("Huawei WMI laptop driver");
MODULE_LICENSE("GPL v2");
