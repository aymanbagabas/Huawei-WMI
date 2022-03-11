// SPDX-License-Identifier: GPL-2.0
/*
 *  Huawei WMI laptop extras driver
 *
 *  Copyright (C) 2018	      Ayman Bagabas <ayman.bagabas@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/sysfs.h>
#include <linux/wmi.h>
#include <linux/hwmon.h>
#include <acpi/battery.h>

#define HWMI_BUFF_SIZE 0x100

/*
 * Huawei WMI GUIDs
 */
#define HWMI_METHOD_GUID "ABBC0F5B-8EA1-11D1-A000-C90629100000"
#define HWMI_EVENT_GUID "ABBC0F5C-8EA1-11D1-A000-C90629100000"

/* Legacy GUIDs */
#define WMI0_EXPENSIVE_GUID "39142400-C6A3-40fa-BADB-8A2652834100"
#define WMI0_EVENT_GUID "59142400-C6A3-40fa-BADB-8A2652834100"

/* HWMI commands */

enum {
	BATTERY_THRESH_GET = 0x00001103, /* \GBTT */
	BATTERY_THRESH_SET = 0x00001003, /* \SBTT */
	FN_LOCK_GET        = 0x00000604, /* \GFRS */
	FN_LOCK_SET        = 0x00000704, /* \SFRS */
	KBDLIGHT_GET       = 0x00000602, /* \GLIV */
	KBDLIGHT_SET       = 0x00000702, /* \SLIV */
	MICMUTE_LED_SET    = 0x00000b04, /* \SMLS */
	KBDLIGHT_TIMEOUT_SET = 0x00001106, /* \SKBT */
	KBDLIGHT_TIMEOUT_GET = 0x00001206, /* \GKBT */
	POWER_UNLOCK_SET   = 0x00000F04, /* \STUB */
	POWER_UNLOCK_GET   = 0x00000E04, /* \STUB */
	FAN_SPEED_GET      = 0x00000802, /* \GFNS */
	TEMP_GET           = 0x00000202, /* \GTMP */
};

union hwmi_arg {
	u64 cmd;
	u8 args[8];
};

struct quirk_entry {
	bool battery_reset;
	bool ec_micmute;
	bool report_brightness;
	bool handle_kbdlight;
};

static struct quirk_entry *quirks;

struct huawei_wmi_debug {
	struct dentry *root;
	u64 arg;
};

struct huawei_wmi {
	bool battery_available;
	bool fn_lock_available;
	bool kbdlight_available;
	bool kbdlight_quirk_input;
	bool kbdlight_timeout_available;
	bool power_unlock_available;
	bool fan_speed_available;
	bool temp_available;

	struct huawei_wmi_debug debug;
	struct input_dev *idev[2];
	struct led_classdev cdev;
	struct device *dev;
	struct device *hwmon;

	struct mutex wmi_lock;
};

static struct huawei_wmi *huawei_wmi;

enum {
	KBDLIGHT_KEY_0 = 0x293,
	KBDLIGHT_KEY_1 = 0x294,
	KBDLIGHT_KEY_2 = 0x295
};

static const struct key_entry huawei_wmi_keymap[] = {
	{ KE_KEY,    0x281,          { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY,    0x282,          { KEY_BRIGHTNESSUP } },
	{ KE_KEY,    0x284,          { KEY_MUTE } },
	{ KE_KEY,    0x285,          { KEY_VOLUMEDOWN } },
	{ KE_KEY,    0x286,          { KEY_VOLUMEUP } },
	{ KE_KEY,    0x287,          { KEY_MICMUTE } },
	{ KE_KEY,    0x289,          { KEY_WLAN } },
	// Huawei |M| key
	{ KE_KEY,    0x28a,          { KEY_CONFIG } },
	// Power unlock (Fn+P)
	{ KE_KEY,    0x2a0,          { KEY_BATTERY } },
 	{ KE_KEY,    0x2a1,          { KEY_BATTERY } },
 	{ KE_KEY,    0x2a6,          { KEY_BATTERY } },
	// Keyboard backlit
	{ KE_IGNORE, KBDLIGHT_KEY_0, { KEY_KBDILLUMTOGGLE } },
	{ KE_IGNORE, KBDLIGHT_KEY_1, { KEY_KBDILLUMDOWN } },
	{ KE_IGNORE, KBDLIGHT_KEY_2, { KEY_KBDILLUMUP } },
	{ KE_END,    0 }
};

static int battery_reset = -1;
static int report_brightness = -1;
static int handle_kbdlight = -1;

module_param(battery_reset, bint, 0444);
MODULE_PARM_DESC(battery_reset,
		"Reset battery charge values to (0-0) before disabling it using (0-100)");
module_param(report_brightness, bint, 0444);
MODULE_PARM_DESC(report_brightness,
		"Report brightness keys.");
module_param(handle_kbdlight, bint, 0444);
MODULE_PARM_DESC(handle_kbdlight,
		"Handle keyboard backlight events.");

/* Quirks */

static int __init dmi_matched(const struct dmi_system_id *dmi)
{
	quirks = dmi->driver_data;
	return 1;
}

static struct quirk_entry quirk_unknown = {
	.handle_kbdlight = true,
};

static struct quirk_entry quirk_skip_kbdlight = {
	.handle_kbdlight = false,
};

static struct quirk_entry quirk_mach_wx9 = {
	.battery_reset = true,
	.handle_kbdlight = false,
};

static struct quirk_entry quirk_matebook_x = {
	.ec_micmute = true,
	.report_brightness = true,
	.handle_kbdlight = false,
};

static const struct dmi_system_id huawei_quirks[] = {
	{
		.callback = dmi_matched,
		.ident = "Huawei MACH-WX9",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "MACH-WX9"),
		},
		.driver_data = &quirk_mach_wx9
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
	{
		.callback = dmi_matched,
		.ident = "Huawei KPL-W0X",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "KPL-W0X")
		},
		.driver_data = &quirk_skip_kbdlight
	},
	{
		.callback = dmi_matched,
		.ident = "Huawei MACHC-WAX9",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "MACHC-WAX9")
		},
		.driver_data = &quirk_unknown
	},
	{
		.callback = dmi_matched,
		.ident = "Huawei NBLK-WAX9X",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "NBLK-WAX9X")
		},
		.driver_data = &quirk_unknown
	},
	{
		.callback = dmi_matched,
		.ident = "Huawei HLYL-WXX9",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "HLYL-WXX9")
		},
		.driver_data = &quirk_skip_kbdlight
	},
	{  }
};

/* Utils */

static int huawei_wmi_call(struct huawei_wmi *huawei,
			   struct acpi_buffer *in, struct acpi_buffer *out)
{
	acpi_status status;

	mutex_lock(&huawei->wmi_lock);
	status = wmi_evaluate_method(HWMI_METHOD_GUID, 0, 1, in, out);
	mutex_unlock(&huawei->wmi_lock);
	if (ACPI_FAILURE(status)) {
		dev_err(huawei->dev, "Failed to evaluate wmi method\n");
		return -ENODEV;
	}

	return 0;
}

/* HWMI takes a 64 bit input and returns either a package with 2 buffers, one of
 * 4 bytes and the other of 256 bytes, or one buffer of size 0x104 (260) bytes.
 * The first 4 bytes are ignored, we ignore the first 4 bytes buffer if we got a
 * package, or skip the first 4 if a buffer of 0x104 is used. The first byte of
 * the remaining 0x100 sized buffer has the return status of every call. In case
 * the return status is non-zero, we return -ENODEV but still copy the returned
 * buffer to the given buffer parameter (buf).
 */
static int huawei_wmi_cmd(u64 arg, u8 *buf, size_t buflen)
{
	struct huawei_wmi *huawei = huawei_wmi;
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in;
	union acpi_object *obj;
	size_t len;
	int err, i;

	in.length = sizeof(arg);
	in.pointer = &arg;

	/* Some models require calling HWMI twice to execute a command. We evaluate
	 * HWMI and if we get a non-zero return status we evaluate it again.
	 */
	for (i = 0; i < 2; i++) {
		err = huawei_wmi_call(huawei, &in, &out);
		if (err)
			goto fail_cmd;

		obj = out.pointer;
		if (!obj) {
			err = -EIO;
			goto fail_cmd;
		}

		switch (obj->type) {
		/* Models that implement both "legacy" and HWMI tend to return a 0x104
		 * sized buffer instead of a package of 0x4 and 0x100 buffers.
		 */
		case ACPI_TYPE_BUFFER:
			if (obj->buffer.length == 0x104) {
				// Skip the first 4 bytes.
				obj->buffer.pointer += 4;
				len = HWMI_BUFF_SIZE;
			} else {
				dev_err(huawei->dev, "Bad buffer length, got %d\n", obj->buffer.length);
				err = -EIO;
				goto fail_cmd;
			}

			break;
		/* HWMI returns a package with 2 buffer elements, one of 4 bytes and the
		 * other is 256 bytes.
		 */
		case ACPI_TYPE_PACKAGE:
			if (obj->package.count != 2) {
				dev_err(huawei->dev, "Bad package count, got %d\n", obj->package.count);
				err = -EIO;
				goto fail_cmd;
			}

			obj = &obj->package.elements[1];
			if (obj->type != ACPI_TYPE_BUFFER) {
				dev_err(huawei->dev, "Bad package element type, got %d\n", obj->type);
				err = -EIO;
				goto fail_cmd;
			}
			len = obj->buffer.length;

			break;
		/* Shouldn't get here! */
		default:
			dev_err(huawei->dev, "Unexpected obj type, got: %d\n", obj->type);
			err = -EIO;
			goto fail_cmd;
		}

		if (!*obj->buffer.pointer)
			break;
	}

	err = (*obj->buffer.pointer) ? -ENODEV : 0;

	if (buf) {
		len = min(buflen, len);
		memcpy(buf, obj->buffer.pointer, len);
	}

fail_cmd:
	kfree(out.pointer);
	return err;
}

/* LEDs */

static int huawei_wmi_micmute_led_set(struct led_classdev *led_cdev,
		enum led_brightness brightness)
{
	/* This is a workaround until the "legacy" interface is implemented. */
	if (quirks && quirks->ec_micmute) {
		char *acpi_method;
		acpi_handle handle;
		acpi_status status;
		union acpi_object args[3];
		struct acpi_object_list arg_list = {
			.pointer = args,
			.count = ARRAY_SIZE(args),
		};

		handle = ec_get_handle();
		if (!handle)
			return -ENODEV;

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
			return -ENODEV;
		}

		status = acpi_evaluate_object(handle, acpi_method, &arg_list, NULL);
		if (ACPI_FAILURE(status))
			return -ENODEV;

		return 0;
	} else {
		union hwmi_arg arg;

		arg.cmd = MICMUTE_LED_SET;
		arg.args[2] = brightness;

		return huawei_wmi_cmd(arg.cmd, NULL, 0);
	}
}

static void huawei_wmi_leds_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	huawei->cdev.name = "platform::micmute";
	huawei->cdev.max_brightness = 1;
	huawei->cdev.brightness_set_blocking = &huawei_wmi_micmute_led_set;
	huawei->cdev.default_trigger = "audio-micmute";
	huawei->cdev.brightness = ledtrig_audio_get(LED_AUDIO_MICMUTE);
	huawei->cdev.dev = dev;
	huawei->cdev.flags = LED_CORE_SUSPENDRESUME;

	devm_led_classdev_register(dev, &huawei->cdev);
}

/* Battery protection */

static int huawei_wmi_battery_get(int *start, int *end)
{
	u8 ret[HWMI_BUFF_SIZE];
	int err, i;

	err = huawei_wmi_cmd(BATTERY_THRESH_GET, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	/* Find the last two non-zero values. Return status is ignored. */
	i = 0xff;
	do {
		if (start)
			*start = ret[i-1];
		if (end)
			*end = ret[i];
	} while (i > 2 && !ret[i--]);

	return 0;
}

static int huawei_wmi_battery_set(int start, int end)
{
	union hwmi_arg arg;
	int err;

	if (start < 0 || end < 0 || start > 100 || end > 100)
		return -EINVAL;

	arg.cmd = BATTERY_THRESH_SET;
	arg.args[2] = start;
	arg.args[3] = end;

	/* This is an edge case were some models turn battery protection
	 * off without changing their thresholds values. We clear the
	 * values before turning off protection. Sometimes we need a sleep delay to
	 * make sure these values make their way to EC memory.
	 */
	if (quirks && quirks->battery_reset && start == 0 && end == 100) {
		err = huawei_wmi_battery_set(0, 0);
		if (err)
			return err;

		msleep(1000);
	}

	err = huawei_wmi_cmd(arg.cmd, NULL, 0);

	return err;
}

static ssize_t charge_control_start_threshold_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, start;

	err = huawei_wmi_battery_get(&start, NULL);
	if (err)
		return err;

	return sprintf(buf, "%d\n", start);
}

static ssize_t charge_control_end_threshold_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, end;

	err = huawei_wmi_battery_get(NULL, &end);
	if (err)
		return err;

	return sprintf(buf, "%d\n", end);
}

static ssize_t charge_control_thresholds_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, start, end;

	err = huawei_wmi_battery_get(&start, &end);
	if (err)
		return err;

	return sprintf(buf, "%d %d\n", start, end);
}

static ssize_t charge_control_start_threshold_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int err, start, end;

	err = huawei_wmi_battery_get(NULL, &end);
	if (err)
		return err;

	if (sscanf(buf, "%d", &start) != 1)
		return -EINVAL;

	err = huawei_wmi_battery_set(start, end);
	if (err)
		return err;

	return size;
}

static ssize_t charge_control_end_threshold_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int err, start, end;

	err = huawei_wmi_battery_get(&start, NULL);
	if (err)
		return err;

	if (sscanf(buf, "%d", &end) != 1)
		return -EINVAL;

	err = huawei_wmi_battery_set(start, end);
	if (err)
		return err;

	return size;
}

static ssize_t charge_control_thresholds_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int err, start, end;

	if (sscanf(buf, "%d %d", &start, &end) != 2)
		return -EINVAL;

	err = huawei_wmi_battery_set(start, end);
	if (err)
		return err;

	return size;
}

static DEVICE_ATTR_RW(charge_control_start_threshold);
static DEVICE_ATTR_RW(charge_control_end_threshold);
static DEVICE_ATTR_RW(charge_control_thresholds);

static int huawei_wmi_battery_add(struct power_supply *battery)
{
	device_create_file(&battery->dev, &dev_attr_charge_control_start_threshold);
	device_create_file(&battery->dev, &dev_attr_charge_control_end_threshold);

	return 0;
}

static int huawei_wmi_battery_remove(struct power_supply *battery)
{
	device_remove_file(&battery->dev, &dev_attr_charge_control_start_threshold);
	device_remove_file(&battery->dev, &dev_attr_charge_control_end_threshold);

	return 0;
}

static struct acpi_battery_hook huawei_wmi_battery_hook = {
	.add_battery = huawei_wmi_battery_add,
	.remove_battery = huawei_wmi_battery_remove,
	.name = "Huawei Battery Extension"
};

static void huawei_wmi_battery_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	huawei->battery_available = true;
	if (huawei_wmi_battery_get(NULL, NULL)) {
		huawei->battery_available = false;
		return;
	}

	battery_hook_register(&huawei_wmi_battery_hook);
	device_create_file(dev, &dev_attr_charge_control_thresholds);
}

static void huawei_wmi_battery_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->battery_available) {
		battery_hook_unregister(&huawei_wmi_battery_hook);
		device_remove_file(dev, &dev_attr_charge_control_thresholds);
	}
}

/* Fn lock */

static int huawei_wmi_fn_lock_get(int *on)
{
	u8 ret[HWMI_BUFF_SIZE] = { 0 };
	int err, i;

	err = huawei_wmi_cmd(FN_LOCK_GET, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	/* Find the first non-zero value. Return status is ignored. */
	i = 1;
	do {
		if (on)
			*on = ret[i] - 1; // -1 undefined, 0 off, 1 on.
	} while (i < 0xff && !ret[i++]);

	return 0;
}

static int huawei_wmi_fn_lock_set(int on)
{
	union hwmi_arg arg;

	arg.cmd = FN_LOCK_SET;
	arg.args[2] = on + 1; // 0 undefined, 1 off, 2 on.

	return huawei_wmi_cmd(arg.cmd, NULL, 0);
}

static ssize_t fn_lock_state_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, on;

	err = huawei_wmi_fn_lock_get(&on);
	if (err)
		return err;

	return sprintf(buf, "%d\n", on);
}

static ssize_t fn_lock_state_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int on, err;

	if (kstrtoint(buf, 10, &on) ||
			on < 0 || on > 1)
		return -EINVAL;

	err = huawei_wmi_fn_lock_set(on);
	if (err)
		return err;

	return size;
}

static DEVICE_ATTR_RW(fn_lock_state);

static void huawei_wmi_fn_lock_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	huawei->fn_lock_available = true;
	if (huawei_wmi_fn_lock_get(NULL)) {
		huawei->fn_lock_available = false;
		return;
	}

	device_create_file(dev, &dev_attr_fn_lock_state);
}

static void huawei_wmi_fn_lock_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->fn_lock_available)
		device_remove_file(dev, &dev_attr_fn_lock_state);
}

/* Keyboard backlight */

static int huawei_wmi_kbdlight_get(int *level)
{
	u8 ret[HWMI_BUFF_SIZE] = { 0 };
	int err;

	err = huawei_wmi_cmd(KBDLIGHT_GET, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;
	if (!ret[2])
		return -ENODEV;

	/* Some models like the MACH-WX9 use 0x01, 0x02, and 0x04 for off, level 1,
	 * and level 2 respectively rather than 0x04, 0x08, and 0x10.
	 */
	huawei_wmi->kbdlight_quirk_input = ret[1] == 0xff;

	if (level) {
		*level = 0;
		while (ret[2] >>= 1) {
			*level += 1;
		}

		if (ret[1] != 0xff)
			*level -= 2;
	}

	return 0;
}

static int huawei_wmi_kbdlight_set(int level)
{
	union hwmi_arg arg;

	// Huawei laptops only support 3 kbdlight levels
	if (level < 0 || level > 2)
		return -EINVAL;
	if (!huawei_wmi->kbdlight_quirk_input)
		level += 2;

	arg.cmd = KBDLIGHT_SET;
	arg.args[2] = 1 << level;

	return huawei_wmi_cmd(arg.cmd, NULL, 0);
}

static ssize_t kbdlight_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, level;

	err = huawei_wmi_kbdlight_get(&level);
	if (err)
		return err;

	return sprintf(buf, "%d\n", level);
}

static ssize_t kbdlight_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int level, err;

	if (kstrtoint(buf, 10, &level))
		return -EINVAL;

	err = huawei_wmi_kbdlight_set(level);
	if (err)
		return err;

	return size;
}

static DEVICE_ATTR_RW(kbdlight);

static void huawei_wmi_kbdlight_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	huawei->kbdlight_available = true;
	if (huawei_wmi_kbdlight_get(NULL)) {
		huawei->kbdlight_available = false;
		return;
	}

	device_create_file(dev, &dev_attr_kbdlight);
}

static void huawei_wmi_kbdlight_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->kbdlight_available)
		device_remove_file(dev, &dev_attr_kbdlight);
}

/*Keyboard backlight timeout*/

static int huawei_wmi_kbdlight_timeout_get(int *seconds)
{
	u8 ret[HWMI_BUFF_SIZE] = { 0 };
	int err;

	err = huawei_wmi_cmd(KBDLIGHT_TIMEOUT_GET, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	if (seconds)
		*seconds = ret[1] | (ret[2] << 8);

	return 0;
}

static int huawei_wmi_kbdlight_timeout_set(int seconds)
{
	union hwmi_arg arg;

	arg.cmd = KBDLIGHT_TIMEOUT_SET;
	arg.args[2] = (seconds & 0xff);
	arg.args[3] = (seconds >> 8);

	return huawei_wmi_cmd(arg.cmd, NULL, 0);

}

static ssize_t kbdlight_timeout_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, seconds;

	err = huawei_wmi_kbdlight_timeout_get(&seconds);
	if (err)
		return err;

	return sprintf(buf, "%d\n", seconds);
}

static ssize_t kbdlight_timeout_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int seconds, err;

	if (kstrtoint(buf, 10, &seconds) ||
			seconds < 0 || seconds > 0xffff)
		return -EINVAL;

	err = huawei_wmi_kbdlight_timeout_set(seconds);
	if (err)
		return err;

	return size;
}

static DEVICE_ATTR_RW(kbdlight_timeout);

static void huawei_wmi_kbdlight_timeout_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	huawei->kbdlight_timeout_available = true;
	if (huawei_wmi_kbdlight_timeout_get(NULL)) {
		huawei->kbdlight_timeout_available = false;
		return;
	}

	device_create_file(dev, &dev_attr_kbdlight_timeout);
}

static void huawei_wmi_kbdlight_timeout_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->kbdlight_timeout_available)
		device_remove_file(dev, &dev_attr_kbdlight_timeout);
}

/*Power unlock*/

static int huawei_wmi_power_unlock_get(int *on)
{
	u8 ret[HWMI_BUFF_SIZE] = { 0 };
	int err;

	err = huawei_wmi_cmd(POWER_UNLOCK_GET, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	if (on)
		*on = ret[1];

	return 0;
}

static int huawei_wmi_power_unlock_set(int on)
{
	union hwmi_arg arg;

	arg.cmd = POWER_UNLOCK_SET;
	arg.args[2] = on;

	return huawei_wmi_cmd(arg.cmd, NULL, 0);

}

static ssize_t power_unlock_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, on;

	err = huawei_wmi_power_unlock_get(&on);
	if (err)
		return err;

	return sprintf(buf, "%d\n", on);
}

static ssize_t power_unlock_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int on, err;

	if (kstrtoint(buf, 10, &on) ||
			on < 0 || on > 1)
		return -EINVAL;

	err = huawei_wmi_power_unlock_set(on);
	if (err)
		return err;

	return size;
}

static DEVICE_ATTR_RW(power_unlock);

static void huawei_wmi_power_unlock_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	huawei->power_unlock_available = true;
	if (huawei_wmi_power_unlock_get(NULL)) {
		huawei->power_unlock_available = false;
		return;
	}

	device_create_file(dev, &dev_attr_power_unlock);
}

static void huawei_wmi_power_unlock_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->power_unlock_available)
		device_remove_file(dev, &dev_attr_power_unlock);
}

/*Hwmon subdriver*/

/*Fan speed*/

static int huawei_wmi_fan_speed_get(u8 num, int *rpm)
{
	u8 ret[HWMI_BUFF_SIZE] = { 0 };
	int err;
	
	union hwmi_arg arg;
	arg.cmd = FAN_SPEED_GET;
	arg.args[2] = num;

	err = huawei_wmi_cmd(arg.cmd, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	if (rpm)
		*rpm = ret[1] | (ret[2] << 8);

	return 0;
}

static ssize_t fan1_input_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, rpm;

	err = huawei_wmi_fan_speed_get(0, &rpm);
	if (err)
		return err;

	return sprintf(buf, "%d\n", rpm);
}

static ssize_t fan2_input_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, rpm;

	err = huawei_wmi_fan_speed_get(1, &rpm);
	if (err)
		return err;

	return sprintf(buf, "%d\n", rpm);
}

static DEVICE_ATTR_RO(fan1_input);
static DEVICE_ATTR_RO(fan2_input);

static void huawei_wmi_fan_speed_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	huawei->fan_speed_available = true;
	if (huawei_wmi_fan_speed_get(0, NULL)) 
	{
		huawei->fan_speed_available = false;
		return;
	}

	device_create_file(huawei->hwmon, &dev_attr_fan1_input);
	device_create_file(huawei->hwmon, &dev_attr_fan2_input);
}

static void huawei_wmi_fan_speed_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->fan_speed_available)
	{
		device_remove_file(huawei->hwmon, &dev_attr_fan1_input);
		device_remove_file(huawei->hwmon, &dev_attr_fan2_input);
	}
}

/*Temp*/
/*
 *HVY-WXX9_1.8 and WRT-WX9_? have more temp zone
 *
 *0x00 CTMP cpu     TP00
 *0x01              TP01
 *0x05 TSLO         TP08
 *0x06              TP06
 *0x07 TNTC         TP02
 *0x08 CNTC         TP03
 *0x0B DNTC         TP05
 *0x0E BTMP battery BTEM
 *0x0F			    TP0C
 *0x15              TP07
 *0x16              TP04
 */

static int huawei_wmi_temp_get(u8 num, int *temp)
{
	u8 ret[HWMI_BUFF_SIZE] = { 0 };
	int err;
	
	union hwmi_arg arg;
	arg.cmd = TEMP_GET;
	arg.args[2] = num;

	err = huawei_wmi_cmd(arg.cmd, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	if (temp)
		*temp = ret[2];

	return 0;
}

static ssize_t temp1_input_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int err, temp;

	err = huawei_wmi_temp_get(0x0E, &temp);
	if (err)
		return err;

	return sprintf(buf, "%d000\n", rpm);
}

static DEVICE_ATTR_RO(temp1_input);

static ssize_t temp1_label_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return sprintf(buf, "battery\n");
}

static DEVICE_ATTR_RO(temp1_label);

static void huawei_wmi_temp_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	huawei->temp_available = true;
	if (huawei_wmi_temp_get(0, NULL)) 
	{
		huawei->temp_available = false;
		return;
	}

	device_create_file(huawei->hwmon, &dev_attr_temp1_input);
	device_create_file(huawei->hwmon, &dev_attr_temp1_label);
}

static void huawei_wmi_temp_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->temp_available)
	{
		device_remove_file(huawei->hwmon, &dev_attr_temp1_label);
		device_remove_file(huawei->hwmon, &dev_attr_temp1_input);
	}
}

/* debugfs */

static void huawei_wmi_debugfs_call_dump(struct seq_file *m, void *data,
		union acpi_object *obj)
{
	struct huawei_wmi *huawei = m->private;
	int i;

	switch (obj->type) {
	case ACPI_TYPE_INTEGER:
		seq_printf(m, "0x%llx", obj->integer.value);
		break;
	case ACPI_TYPE_STRING:
		seq_printf(m, "\"%.*s\"", obj->string.length, obj->string.pointer);
		break;
	case ACPI_TYPE_BUFFER:
		seq_puts(m, "{");
		for (i = 0; i < obj->buffer.length; i++) {
			seq_printf(m, "0x%02x", obj->buffer.pointer[i]);
			if (i < obj->buffer.length - 1)
				seq_puts(m, ",");
		}
		seq_puts(m, "}");
		break;
	case ACPI_TYPE_PACKAGE:
		seq_puts(m, "[");
		for (i = 0; i < obj->package.count; i++) {
			huawei_wmi_debugfs_call_dump(m, huawei, &obj->package.elements[i]);
			if (i < obj->package.count - 1)
				seq_puts(m, ",");
		}
		seq_puts(m, "]");
		break;
	default:
		dev_err(huawei->dev, "Unexpected obj type, got %d\n", obj->type);
		return;
	}
}

static int huawei_wmi_debugfs_call_show(struct seq_file *m, void *data)
{
	struct huawei_wmi *huawei = m->private;
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in;
	union acpi_object *obj;
	int err;

	in.length = sizeof(u64);
	in.pointer = &huawei->debug.arg;

	err = huawei_wmi_call(huawei, &in, &out);
	if (err)
		return err;

	obj = out.pointer;
	if (!obj) {
		err = -EIO;
		goto fail_debugfs_call;
	}

	huawei_wmi_debugfs_call_dump(m, huawei, obj);

fail_debugfs_call:
	kfree(out.pointer);
	return err;
}

DEFINE_SHOW_ATTRIBUTE(huawei_wmi_debugfs_call);

static void huawei_wmi_debugfs_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	huawei->debug.root = debugfs_create_dir("huawei-wmi", NULL);

	debugfs_create_x64("arg", 0644, huawei->debug.root,
		&huawei->debug.arg);
	debugfs_create_file("call", 0400,
		huawei->debug.root, huawei, &huawei_wmi_debugfs_call_fops);
}

static void huawei_wmi_debugfs_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	debugfs_remove_recursive(huawei->debug.root);
}

/* Input */

static void huawei_wmi_process_key(struct input_dev *idev, int code)
{
	struct huawei_wmi *huawei = dev_get_drvdata(idev->dev.parent);
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

	if (quirks && quirks->handle_kbdlight && huawei->kbdlight_available &&
			(key->code == KBDLIGHT_KEY_0 ||
			key->code == KBDLIGHT_KEY_1 ||
			key->code == KBDLIGHT_KEY_2)) {
		huawei_wmi_kbdlight_set(key->code - KBDLIGHT_KEY_0);
	}

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

static int huawei_wmi_input_setup(struct device *dev,
		const char *guid,
		struct input_dev **idev)
{
	*idev = devm_input_allocate_device(dev);
	if (!*idev)
		return -ENOMEM;

	(*idev)->name = "Huawei WMI hotkeys";
	(*idev)->phys = "wmi/input0";
	(*idev)->id.bustype = BUS_HOST;
	(*idev)->dev.parent = dev;

	return sparse_keymap_setup(*idev, huawei_wmi_keymap, NULL) ||
		input_register_device(*idev) ||
		wmi_install_notify_handler(guid, huawei_wmi_input_notify,
				*idev);
}

static void huawei_wmi_input_exit(struct device *dev, const char *guid)
{
	wmi_remove_notify_handler(guid);
}

/* Huawei driver */

static const struct wmi_device_id huawei_wmi_events_id_table[] = {
	{ .guid_string = WMI0_EVENT_GUID },
	{ .guid_string = HWMI_EVENT_GUID },
	{  }
};

static int huawei_wmi_probe(struct platform_device *pdev)
{
	const struct wmi_device_id *guid = huawei_wmi_events_id_table;
	struct input_dev *idev = *huawei_wmi->idev;
	int err;

	platform_set_drvdata(pdev, huawei_wmi);
	huawei_wmi->dev = &pdev->dev;

	while (*guid->guid_string) {
		if (wmi_has_guid(guid->guid_string)) {
			err = huawei_wmi_input_setup(&pdev->dev, guid->guid_string, &idev);
			if (err) {
				dev_err(&pdev->dev, "Failed to setup input on %s\n", guid->guid_string);
				return err;
			}
		}

		idev++;
		guid++;
	}

	if (wmi_has_guid(HWMI_METHOD_GUID)) {
		mutex_init(&huawei_wmi->wmi_lock);

		huawei_wmi->hwmon = hwmon_device_register_with_groups(&pdev->dev, "huawei_wmi_sensors", NULL, NULL);
		if (IS_ERR(huawei_wmi->hwmon)) 
		{
			huawei_wmi->hwmon = NULL;
		}
		else
		{
			huawei_wmi_fan_speed_setup(&pdev->dev);
			huawei_wmi_temp_setup(&pdev->dev);
		}
		huawei_wmi_power_unlock_setup(&pdev->dev);
		huawei_wmi_kbdlight_timeout_setup(&pdev->dev);
		huawei_wmi_kbdlight_setup(&pdev->dev);
		huawei_wmi_leds_setup(&pdev->dev);
		huawei_wmi_fn_lock_setup(&pdev->dev);
		huawei_wmi_battery_setup(&pdev->dev);
		huawei_wmi_debugfs_setup(&pdev->dev);
	}

	return 0;
}

static int huawei_wmi_remove(struct platform_device *pdev)
{
	const struct wmi_device_id *guid = huawei_wmi_events_id_table;

	while (*guid->guid_string) {
		if (wmi_has_guid(guid->guid_string))
			huawei_wmi_input_exit(&pdev->dev, guid->guid_string);

		guid++;
	}

	if (wmi_has_guid(HWMI_METHOD_GUID)) {
		huawei_wmi_debugfs_exit(&pdev->dev);
		huawei_wmi_battery_exit(&pdev->dev);
		huawei_wmi_fn_lock_exit(&pdev->dev);
		huawei_wmi_kbdlight_exit(&pdev->dev);
		huawei_wmi_kbdlight_timeout_exit(&pdev->dev);
		huawei_wmi_power_unlock_exit(&pdev->dev);
		if (huawei_wmi->hwmon)
		{
			huawei_wmi_temp_exit(&pdev->dev);
			huawei_wmi_fan_speed_exit(&pdev->dev);
			hwmon_device_unregister(huawei_wmi->hwmon);
		}
	}

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
	struct platform_device *pdev;
	int err;

	huawei_wmi = kzalloc(sizeof(struct huawei_wmi), GFP_KERNEL);
	if (!huawei_wmi)
		return -ENOMEM;

	quirks = &quirk_unknown;
	dmi_check_system(huawei_quirks);
	if (battery_reset != -1)
		quirks->battery_reset = battery_reset;
	if (report_brightness != -1)
		quirks->report_brightness = report_brightness;
	if (handle_kbdlight != -1)
		quirks->handle_kbdlight = handle_kbdlight;

	err = platform_driver_register(&huawei_wmi_driver);
	if (err)
		goto pdrv_err;

	pdev = platform_device_register_simple("huawei-wmi", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		err = PTR_ERR(pdev);
		goto pdev_err;
	}
	return 0;

pdev_err:
	platform_driver_unregister(&huawei_wmi_driver);
pdrv_err:
	kfree(huawei_wmi);
	return err;
}

static __exit void huawei_wmi_exit(void)
{
	struct platform_device *pdev = to_platform_device(huawei_wmi->dev);

	platform_device_unregister(pdev);
	platform_driver_unregister(&huawei_wmi_driver);

	kfree(huawei_wmi);
}

module_init(huawei_wmi_init);
module_exit(huawei_wmi_exit);

MODULE_ALIAS("wmi:"HWMI_METHOD_GUID);
MODULE_DEVICE_TABLE(wmi, huawei_wmi_events_id_table);
MODULE_AUTHOR("Ayman Bagabas <ayman.bagabas@gmail.com>");
MODULE_DESCRIPTION("Huawei WMI laptop extras driver");
MODULE_LICENSE("GPL v2");
