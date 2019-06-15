// SPDX-License-Identifier: GPL-2.0
/*
 *  Huawei WMI laptop extras driver
 *
 *  Copyright (C) 2018	      Ayman Bagabas <ayman.bagabas@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/wmi.h>

/*
 * Huawei WMI GUIDs
 */
#define AMW0_METHOD_GUID "ABBC0F5B-8EA1-11D1-A000-C90629100000"
#define AMW0_EVENT_GUID "ABBC0F5C-8EA1-11D1-A000-C90629100000"

/* Legacy GUIDs */
#define WMI0_EXPENSIVE_GUID "39142400-C6A3-40fa-BADB-8A2652834100"
#define WMI0_EVENT_GUID "59142400-C6A3-40fa-BADB-8A2652834100"

/* AMW0_commands */

enum wmaa_cmd {
	BATTERY_GET, /* \GBTT 0x00001103 */
	BATTERY_SET, /* \SBTT 0xXXYY1003 */
	FN_LOCK_GET, /* \GFRS 0x00000604 */
	FN_LOCK_SET, /* \SFRS 0x000X0704 */
	MICMUTE_LED, /* \SMLS 0x000X0b04 */
};

enum fn_state {
	FN_LOCK_OFF = 0x01,
	FN_LOCK_ON = 0x02,
};

struct quirk_entry {
	bool battery_sleep;
	bool ec_micmute;
	bool report_brightness;
};

static struct quirk_entry *quirks;

struct huawei_wmi {
	struct led_classdev cdev;
	struct mutex wmi_lock;
	struct mutex battery_lock;
	struct input_dev *idev[2];
	struct platform_device *pdev;
};

struct platform_device *huawei_wmi_pdev;

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

static bool battery_sleep;
static bool report_brightness;

module_param(battery_sleep, bool, 0444);
MODULE_PARM_DESC(battery_sleep,
		"Delay after setting battery charging thresholds.");
module_param(report_brightness, bool, 0444);
MODULE_PARM_DESC(report_brightness,
		"Report brightness key events.");

/* Quirks */

static int __init dmi_matched(const struct dmi_system_id *dmi)
{
	quirks = dmi->driver_data;
	return 1;
}

static struct quirk_entry quirk_unknown = {
};

static struct quirk_entry quirk_battery_sleep = {
	.battery_sleep = true,
};

static struct quirk_entry quirk_matebook_x = {
	.ec_micmute = true,
	.report_brightness = true,
};

static const struct dmi_system_id huawei_quirks[] = {
	{
		.callback = dmi_matched,
		.ident = "Huawei MACH-WX9",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "MACH-WX9"),
		},
		.driver_data = &quirk_battery_sleep
	},
	{
		.callback = dmi_matched,
		.ident = "Huawei MateBook X",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "HUAWEI MateBook X")
		},
		.driver_data = &quirk_matebook_x
	},
	{  }
};

/* Utils */

static int huawei_wmi_eval(struct device *dev, u8 *arg,
		u8 *buf, size_t buflen)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in;
	union acpi_object *obj;
	acpi_status status;
	size_t len;
	int err = -ENODEV;

	in.length = sizeof(u8) * 4;
	in.pointer = (u32 *)arg;
	mutex_lock(&huawei->wmi_lock);
	status = wmi_evaluate_method(AMW0_METHOD_GUID, 0, 1, &in, &out);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Failed to evaluate wmi method\n");
		goto wmi_eval_fail;
	}

	/* WMAA takes a 4 bytes buffer as an input. It returns a package
	 * with two buffer elements. The first buffer is 4 bytes long and
	 * the second is 0x100 (256) bytes long. The first buffer is always
	 * zeros. The second stores the output from every call. The first
	 * byte of the second buffer always have the return status of the
	 * called command.
	 */
	obj = out.pointer;
	if (!obj)
		goto wmi_eval_fail;
	if (obj->type != ACPI_TYPE_PACKAGE || obj->package.count != 2) {
		dev_err(dev, "Unknown response type %d\n", obj->type);
		goto wmi_eval_fail;
	}

	obj = &(obj->package.elements[1]);
	if (!obj || obj->type != ACPI_TYPE_BUFFER)
		goto wmi_eval_fail;

	if (buf) {
		len = min(buflen, obj->buffer.length);
		memcpy(buf, obj->buffer.pointer, len);
	}
	err = 0;

wmi_eval_fail:
	mutex_unlock(&huawei->wmi_lock);
	kfree(out.pointer);
	return err;
}

static int huawei_wmi_cmd(struct device *dev, enum wmaa_cmd cmd, u8 *arg,
		u8 *out, size_t outlen)
{
	u8 parm[4] = { 0 };
	u8 buf[0x100] = { 0xff };
	int err;

	if (!arg)
		arg = parm;

	switch (cmd) {
	case BATTERY_SET:
		arg[0] = 0x03;
		arg[1] = 0x10;
		break;
	case BATTERY_GET:
		arg[0] = 0x03;
		arg[1] = 0x11;
		break;
	case FN_LOCK_GET:
		arg[0] = 0x04;
		arg[1] = 0x06;
		break;
	case FN_LOCK_SET:
		arg[0] = 0x04;
		arg[1] = 0x07;
		break;
	case MICMUTE_LED:
		arg[0] = 0x04;
		arg[1] = 0x0b;
		break;
	default:
		dev_err(dev, "Unsupported command, got: 0x%08x\n", *(u32 *)arg);
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
	if (buf[0]) {
		err = huawei_wmi_eval(dev, arg, buf, 0x100);
		if (err)
			return err;
		if (buf[0]) {
			dev_err(dev, "Invalid response, got: %d\n", buf[0]);
			return -ENXIO;
		}
	}
	if (out)
		memcpy(out, buf, outlen);

	return 0;
}

/* Input */

static void huawei_wmi_process_key(struct input_dev *idev, int code)
{
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
		dev_info(&idev->dev, "Unknown key pressed, code: 0x%04x\n", code);
		return;
	}

	if (quirks && !quirks->report_brightness &&
			(key->sw.code == KEY_BRIGHTNESSDOWN ||
			key->sw.code == KEY_BRIGHTNESSUP))
		return;

	sparse_keymap_report_entry(idev, key, 1, true);
}

static void huawei_wmi_input_notify(u32 value, void *context)
{
	struct input_dev *idev = (struct input_dev *)context;
	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = wmi_get_event_data(value, &response);
	if (ACPI_FAILURE(status)) {
		dev_err(&idev->dev, "Unable to get event data\n");
		return;
	}

	obj = (union acpi_object *)response.pointer;
	if (obj && obj->type == ACPI_TYPE_INTEGER)
		huawei_wmi_process_key(idev, obj->integer.value);
	else
		dev_err(&idev->dev, "Bad response type\n");

	kfree(response.pointer);
}

static int huawei_wmi_input_setup(struct platform_device *pdev,
		struct input_dev **idev)
{
	int err;

	*idev = devm_input_allocate_device(&pdev->dev);
	if (!*idev)
		return -ENOMEM;

	(*idev)->name = "Huawei WMI hotkeys";
	(*idev)->phys = "wmi/input0";
	(*idev)->id.bustype = BUS_HOST;
	(*idev)->dev.parent = &pdev->dev;

	err = sparse_keymap_setup(*idev, huawei_wmi_keymap, NULL);
	if (err)
		return err;

	return input_register_device(*idev);
}

/* LEDs */

static void huawei_wmi_micmute_led_set(struct led_classdev *led_cdev,
		enum led_brightness brightness)
{
	/* This is a workaround until the "legacy" interface is implemented. */
	if (quirks && quirks->ec_micmute) {
		char *acpi_method;
		acpi_handle handle;
		union acpi_object args[3];
		struct acpi_object_list arg_list = {
			.pointer = args,
			.count = ARRAY_SIZE(args),
		};

		handle = ec_get_handle();
		if (!handle) {
			dev_err(led_cdev->dev->parent, "Failed to get EC handle\n");
			return;
		}

		args[0].type = args[1].type = args[2].type = ACPI_TYPE_INTEGER;
		args[1].integer.value = 0x04;

		if (acpi_has_method(handle, "SPIN")) {
			acpi_method = "SPIN";
			args[0].integer.value = 0;
			args[2].integer.value = brightness ? 1 : 0;
		} else if (acpi_has_method(handle, "WPIN")) {
			acpi_method = "WPIN";
			args[0].integer.value = 1;
			args[2].integer.value = brightness ? 0 : 1;
		} else {
			return;
		}

		acpi_evaluate_object(handle, acpi_method, &arg_list, NULL);
	} else {
		u8 arg[] = { 0, 0, brightness, 0 };

		huawei_wmi_cmd(led_cdev->dev->parent, MICMUTE_LED, arg, NULL, NULL);
	}
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
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	u8 ret[0x100] = { 0 };
	int err, i = 0x100;

	mutex_lock(&huawei->battery_lock);
	err = huawei_wmi_cmd(dev, BATTERY_GET, NULL, ret, 0x100);
	mutex_unlock(&huawei->battery_lock);
	if (err)
		return err;

	/* Returned buffer positions battery thresholds either in index
	 * 3 and 2 or in 2 and 1. 0 reserved for return status.
	 */
	while (i > 0 && !ret[i--]);
	*low = ret[i];
	*high = ret[i+1];

	return 0;
}

static int huawei_wmi_battery_set(struct device *dev, int low, int high)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	u8 arg[] = { 0, 0, low, high };
	int err;

	/* This is an edge case were some models turn battery protection
	 * off without changing their thresholds values. We clear the
	 * values before turning off protection. We need wait blocking to
	 * make sure these values make its way to EC.
	 */
	if (low == 0 && high == 100)
		huawei_wmi_battery_set(dev, 0, 0);

	mutex_lock(&huawei->battery_lock);
	err = huawei_wmi_cmd(dev, BATTERY_SET, arg, NULL, NULL);
	if (quirks && quirks->battery_sleep)
		msleep(jiffies_to_msecs(0.5 * HZ));
	mutex_unlock(&huawei->battery_lock);
	if (err)
		return err;

	return 0;
}

/* Fn lock */

static int huawei_wmi_fn_lock_get(struct device *dev, int *on)
{
	u8 ret[0x100] = { 0 };
	int err, i = 0;

	err = huawei_wmi_cmd(dev, FN_LOCK_GET, NULL, ret, 0x100);
	if (err)
		return err;

	while (i <= 0x100 && !ret[i++]);
	*on = (ret[i-1] == FN_LOCK_OFF) ? 0 : 1;

	return 0;
}

static int huawei_wmi_fn_lock_set(struct device *dev, int on)
{
	u8 arg[] = { 0, 0, (on) ? FN_LOCK_ON : FN_LOCK_OFF, 0 };

	return huawei_wmi_cmd(dev, FN_LOCK_SET, arg, NULL, NULL);
}

/* sysfs */

static ssize_t charge_thresholds_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
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
		const char *buf, size_t size)
{
	int on;

	if (kstrtoint(buf, 10, &on) ||
			on < 0 || on > 1 ||
			huawei_wmi_fn_lock_set(dev, on))
		return -EINVAL;

	return size;
}

static ssize_t charge_thresholds_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, low, high;

	low = high = 0;
	err = huawei_wmi_battery_get(dev, &low, &high);
	if (err)
		return -EINVAL;

	return sprintf(buf, "%d %d\n", low, high);
}

static ssize_t fn_lock_state_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
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

/* Huawei driver */

static int huawei_wmi_probe(struct platform_device *pdev)
{
	struct huawei_wmi *huawei;
	int err;

	huawei = devm_kzalloc(&pdev->dev, sizeof(struct huawei_wmi), GFP_KERNEL);
	if (!huawei)
		return -ENOMEM;

	huawei->pdev = pdev;
	dev_set_drvdata(&pdev->dev, huawei);

	if (wmi_has_guid(WMI0_EVENT_GUID)) {
		err = huawei_wmi_input_setup(pdev, &huawei->idev[0]);
		if (err)
			pr_err("Failed to setup input\n");
		err = wmi_install_notify_handler(WMI0_EVENT_GUID,
				huawei_wmi_input_notify, huawei->idev[0]);
		if (err)
			pr_err("Failed to install notify\n");
	}
	
	if (wmi_has_guid(AMW0_EVENT_GUID)) {
		err = huawei_wmi_input_setup(pdev, &huawei->idev[1]);
		if (err)
			pr_err("Failed to setup input\n");
		err = wmi_install_notify_handler(AMW0_EVENT_GUID,
				huawei_wmi_input_notify, huawei->idev[1]);
		if (err)
			pr_err("Failed to install notify\n");
	}

	if (wmi_has_guid(AMW0_METHOD_GUID)) {

		mutex_init(&huawei->wmi_lock);
		mutex_init(&huawei->battery_lock);

		err = sysfs_create_group(&pdev->dev.kobj, &huawei_wmi_group);
		if (err)
			pr_err("Failed to create sysfs interface\n");

		err = huawei_wmi_leds_setup(&pdev->dev);
		if (err)
			pr_err("Failed to setup leds\n");
	}

	return 0;
}

static int huawei_wmi_remove(struct platform_device *pdev)
{
	if (wmi_has_guid(WMI0_EVENT_GUID))
		wmi_remove_notify_handler(WMI0_EVENT_GUID);
	
	if (wmi_has_guid(AMW0_EVENT_GUID))
		wmi_remove_notify_handler(AMW0_EVENT_GUID);

	if (wmi_has_guid(AMW0_METHOD_GUID))
		sysfs_remove_group(&pdev->dev.kobj, &huawei_wmi_group);

	return 0;
}

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

	quirks = &quirk_unknown;
	dmi_check_system(huawei_quirks);
	quirks->battery_sleep |= battery_sleep;
	quirks->report_brightness |= report_brightness;

	err = platform_driver_register(&huawei_wmi_driver);
	if (err) {
		pr_err("Failed to register platform driver\n");
		return err;
	}

	huawei_wmi_pdev = platform_device_register_simple("huawei-wmi", -1, NULL, 0);
	if (IS_ERR(huawei_wmi_pdev)) {
		pr_err("Failed to register platform device\n");
		platform_driver_unregister(&huawei_wmi_driver);
		return PTR_ERR(huawei_wmi_pdev);
	}

	return 0;
}

static __exit void huawei_wmi_exit(void)
{
	platform_device_unregister(huawei_wmi_pdev);
	platform_driver_unregister(&huawei_wmi_driver);
}

module_init(huawei_wmi_init);
module_exit(huawei_wmi_exit);

MODULE_ALIAS("wmi:"WMI0_EVENT_GUID);
MODULE_ALIAS("wmi:"AMW0_EVENT_GUID);
MODULE_ALIAS("wmi:"AMW0_METHOD_GUID);
MODULE_AUTHOR("Ayman Bagabas <ayman.bagabas@gmail.com>");
MODULE_DESCRIPTION("Huawei WMI laptop extras driver");
MODULE_LICENSE("GPL v2");
