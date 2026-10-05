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
#include <linux/hwmon.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/wmi.h>
#include <linux/workqueue.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#include <linux/platform_profile.h>
#endif
#include <acpi/battery.h>

#define HWMI_BUFF_SIZE 0x100
#define HONOR_KBBL_OFFSET 0x41
#define HONOR_KBBL_OFF 0x04
#define HONOR_KBBL_LOW 0x02
#define HONOR_KBBL_HIGH 0x03
#define HONOR_KBBL_LATCH 0x01

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
#define sysfs_emit(buf, fmt, ...) sprintf(buf, fmt, ##__VA_ARGS__)
#endif

#ifndef kzalloc_obj
#define kzalloc_obj(P) kzalloc(sizeof(P), GFP_KERNEL)
#endif

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
	BATTERY_THRESH_GET	      = 0x00001103, /* \GBTT */
	BATTERY_THRESH_SET	      = 0x00001003, /* \SBTT */
	FN_LOCK_GET		      = 0x00000604, /* \GFRS */
	FN_LOCK_SET		      = 0x00000704, /* \SFRS */
	KBDLIGHT_GET		      = 0x00000602, /* \GLIV */
	KBDLIGHT_SET		      = 0x00000702, /* \SLIV */
	MICMUTE_LED_SET		      = 0x00000b04, /* \SMLS */
	KBDLIGHT_TIMEOUT_SET	      = 0x00001106, /* \SKBT */
	KBDLIGHT_TIMEOUT_GET	      = 0x00001206, /* \GKBT */
	KBDLIGHT_MODE_GET	      = 0x00001306, /* \GKBM */
	KBDLIGHT_MODE_SET	      = 0x00001406, /* \SKBM */
	KBDLIGHT_SET_AUTO	      = 0x00001506, /* \SKBL */
	POWER_UNLOCK_SET	      = 0x00000F04, /* \STUB */
	POWER_UNLOCK_GET	      = 0x00000E04, /* \STUB */
	FAN_SPEED_GET		      = 0x00000802, /* \GFNS */
	TEMP_GET		      = 0x00000202, /* \GTMP */
	TOUCHPAD_GET		      = 0x00000F02, /* \GTPS */
	TOUCHPAD_SET		      = 0x00001002, /* \STPS */
	BATTERY_CHARGE_MODE_GET	      = 0x00001603, /* \GBCM */
	BATTERY_CHARGE_MODE_SET	      = 0x00001503, /* \SBCM */
	BATTERY_CHARGE_MODE_PARAM_GET = 0x00001303, /* \GBAC */
	BATTERY_CHARGE_MODE_PARAM_SET = 0x00001203, /* \SBAC */
};

enum {
	PROFILE_BALANCED    = 0,
	PROFILE_PERFORMANCE = 1,
	PROFILE_HUNTER	    = 3,
};

union hwmi_arg {
	u64 cmd;
	u8  args[8];
};

struct quirk_entry {
	bool battery_reset;
	bool ec_micmute;
	bool report_brightness;
	bool report_volume;
	bool handle_kbdlight;
	bool kbdlight_auto;
	bool kbdlight_ec;
	bool fan_ec;
	bool platform_profile;
	bool platform_profile_hunter;
};

static struct quirk_entry *quirks;

struct huawei_wmi_debug {
	struct dentry *root;
	u64	       arg;
};

struct huawei_wmi {
	bool battery_available;
	bool fn_lock_available;
	bool kbdlight_available;
	bool kbdlight_quirk_input;
	bool kbdlight_steady;
	u8   kbdlight_ec_level;
	bool kbdlight_timeout_available;
	bool power_unlock_available;
	bool fan_speed_available;
	bool temp_available;
	bool smart_charge_available;
	bool smart_charge_param_available;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	bool		      platform_profile_available;
	bool		      acpi_notifier_registered;
	bool		      power_supply_notifier_registered;
	struct mutex	      platform_profile_lock;
	struct notifier_block acpi_nb;
	struct notifier_block power_supply_nb;
	struct delayed_work   platform_profile_work;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	struct device *platform_profile_device;
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
	struct platform_profile_handler profile_handler;
#endif
#endif

	struct delayed_work	kbdlight_latch_work;
	struct mutex		kbdlight_lock;
	struct huawei_wmi_debug debug;
	struct input_dev       *idev[2];
	struct led_classdev	micmute_cdev;
	struct led_classdev	kbdlight_cdev;
	struct device	       *dev;
	struct device	       *hwmon;

	struct mutex wmi_lock;
};

static struct huawei_wmi *huawei_wmi;

enum {
	KBDLIGHT_KEY_0 = 0x293,
	KBDLIGHT_KEY_1 = 0x294,
	KBDLIGHT_KEY_2 = 0x295,
};

enum {
	KBDLIGHT_MODE_OFF  = 0x02,
	KBDLIGHT_MODE_LOW  = 0x03,
	KBDLIGHT_MODE_HIGH = 0x04,
	KBDLIGHT_MODE_AUTO = 0x10,
};

static const struct key_entry huawei_wmi_keymap[] = {
	{ KE_KEY, 0x109, { KEY_BATTERY } },
	{ KE_KEY, 0x281, { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY, 0x282, { KEY_BRIGHTNESSUP } },
	{ KE_KEY, 0x283, { KEY_TOUCHPAD_ON } },
	{ KE_KEY, 0x2a3, { KEY_TOUCHPAD_OFF } },
	{ KE_KEY, 0x284, { KEY_MUTE } },
	{ KE_KEY, 0x285, { KEY_VOLUMEDOWN } },
	{ KE_KEY, 0x286, { KEY_VOLUMEUP } },
	{ KE_KEY, 0x287, { KEY_MICMUTE } },
	{ KE_KEY, 0x288, { KEY_CAMERA_ACCESS_TOGGLE } },
	{ KE_KEY, 0x289, { KEY_WLAN } },
	// Huawei |M| key
	{ KE_KEY, 0x28a, { KEY_CONTROLPANEL } },
	// Sidebar (notifications) key
	{ KE_KEY, 0x28b, { KEY_NOTIFICATION_CENTER } },
	{ KE_KEY, 0x28e, { KEY_SELECTIVE_SCREENSHOT } },
	// Keyboard backlight (F-keys)
	{ KE_IGNORE, KBDLIGHT_KEY_0, { KEY_KBDILLUMTOGGLE } },
	{ KE_IGNORE, KBDLIGHT_KEY_1, { KEY_KBDILLUMDOWN } },
	{ KE_IGNORE, KBDLIGHT_KEY_2, { KEY_KBDILLUMUP } },
	// Power unlock (Fn+P)
	{ KE_KEY, 0x2a0, { KEY_PROG1 } },
	{ KE_KEY, 0x2a1, { KEY_PROG1 } },
	{ KE_KEY, 0x2a6, { KEY_PROG1 } },
	// Refresh rate (Fn+R)
	{ KE_KEY, 0x2a7, { KEY_REFRESH_RATE_TOGGLE } },
	{ KE_KEY, 0x2b1, { KEY_KBDILLUMDOWN } },
	{ KE_KEY, 0x2b2, { KEY_KBDILLUMDOWN } },
	{ KE_KEY, 0x2b3, { KEY_KBDILLUMUP } },
	{ KE_KEY, 0x2b4, { KEY_KBDILLUMTOGGLE } },
	{ KE_KEY, 0x2b5, { KEY_VOICECOMMAND } },
	{ KE_KEY, 0x2bc, { KEY_CAMERA_ACCESS_ENABLE } },
	{ KE_KEY, 0x2bd, { KEY_CAMERA_ACCESS_DISABLE } },
	// Ignore Ambient Light Sensoring
	{ KE_IGNORE, 0x2c1, { KEY_RESERVED } },
	{ KE_KEY, 0x2c3, { KEY_FIND } },
	// Camera module slot
	{ KE_KEY, 0x2e0, { KEY_CAMERA_ACCESS_ENABLE } },
	{ KE_KEY, 0x2e1, { KEY_CAMERA_ACCESS_DISABLE } },
	{ KE_KEY, 0x2e2, { KEY_PROG2 } },
	{ KE_KEY, 0x2e3, { KEY_PROG3 } },
	{ KE_IGNORE, 0x2e5, { KEY_RESERVED } },
	{ KE_IGNORE, 0x2e6, { KEY_RESERVED } },
	{ KE_END, 0 }
};

static int battery_reset     = -1;
static int report_brightness = -1;
static int report_volume     = -1;
static int handle_kbdlight   = -1;
static int kbdlight_auto     = -1;

module_param(battery_reset, bint, 0444);
MODULE_PARM_DESC(
    battery_reset,
    "Reset battery charge values to (0-0) before disabling it using (0-100)");
module_param(report_brightness, bint, 0444);
MODULE_PARM_DESC(report_brightness, "Report brightness keys.");
module_param(report_volume, bint, 0444);
MODULE_PARM_DESC(report_volume, "Report volume keys.");
module_param(handle_kbdlight, bint, 0444);
MODULE_PARM_DESC(handle_kbdlight, "Handle keyboard backlight events.");
module_param(kbdlight_auto, bint, 0444);
MODULE_PARM_DESC(kbdlight_auto, "Keyboard backlight supports the auto mode.");

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

static struct quirk_entry quirk_kbdlight_auto = {
	.handle_kbdlight  = false,
	.kbdlight_auto	  = true,
	.platform_profile = true,
};

static struct quirk_entry quirk_honor_platform_profile = {
	.handle_kbdlight  = true,
	.platform_profile = true,
};

static struct quirk_entry quirk_honor_zqcp_m1010 = {
	.handle_kbdlight  = true,
	.kbdlight_ec	  = true,
	.fan_ec		  = true,
	.platform_profile = true,
};

static struct quirk_entry quirk_honor_fmbp = {
	.handle_kbdlight  = true,
	.kbdlight_ec	  = true,
	.platform_profile = true,
};

static struct quirk_entry quirk_honor_drbp = {
	.platform_profile = true,
};

static struct quirk_entry quirk_honor_drbp_hunter = {
	.platform_profile	 = true,
	.platform_profile_hunter = true,
};

static struct quirk_entry quirk_honor_dra_hunter = {
	.handle_kbdlight	 = true,
	.platform_profile	 = true,
	.platform_profile_hunter = true,
};

static struct quirk_entry quirk_mach_wx9 = {
	.battery_reset	 = true,
	.handle_kbdlight = false,
};

static struct quirk_entry quirk_matebook_x = {
	.ec_micmute	   = true,
	.report_brightness = true,
	.handle_kbdlight   = false,
};

static struct quirk_entry quirk_matebook_d = {
	.report_brightness = false,
	.report_volume	   = false,
	.handle_kbdlight   = false,
};

static const struct dmi_system_id huawei_quirks[] = {
	{.callback = dmi_matched,
	.ident = "Huawei MACH-WX9",
	.matches =
	    {
		DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"),
		DMI_MATCH(DMI_PRODUCT_NAME, "MACH-WX9"),
	    },
	.driver_data = &quirk_mach_wx9},
    {.callback	     = dmi_matched,
	.ident	     = "Huawei MateBook X",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"),
	    DMI_MATCH(DMI_PRODUCT_NAME, "HUAWEI MateBook X")},
	.driver_data = &quirk_matebook_x},
    {.callback	 = dmi_matched,
	.ident	 = "Huawei MCLF-XX",
	.matches = {DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"), DMI_MATCH(DMI_PRODUCT_NAME, "MCLF-XX")},
	.driver_data = &quirk_matebook_d},
    {.callback	 = dmi_matched,
	.ident	 = "Huawei KPL-W0X",
	.matches = {DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"), DMI_MATCH(DMI_PRODUCT_NAME, "KPL-W0X")},
	.driver_data = &quirk_skip_kbdlight},
    {.callback	 = dmi_matched,
	.ident	 = "Huawei MACHC-WAX9",
	.matches = {DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"), DMI_MATCH(DMI_PRODUCT_NAME, "MACHC-WAX9")},
	.driver_data = &quirk_unknown},
    {.callback	 = dmi_matched,
	.ident	 = "Huawei NBLK-WAX9X",
	.matches = {DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"), DMI_MATCH(DMI_PRODUCT_NAME, "NBLK-WAX9X")},
	.driver_data = &quirk_skip_kbdlight},
    {.callback	 = dmi_matched,
	.ident	 = "Huawei HLYL-WXX9",
	.matches = {DMI_MATCH(DMI_SYS_VENDOR, "HUAWEI"), DMI_MATCH(DMI_PRODUCT_NAME, "HLYL-WXX9")},
	.driver_data = &quirk_skip_kbdlight},
    {.callback	     = dmi_matched,
	.ident	     = "Honor MRA-XXX",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "MRA-XXX")},
	.driver_data = &quirk_kbdlight_auto},
    /* Keep board-specific entries before product-wide matches: dmi_matched()
     * returns nonzero, which stops dmi_check_system() after its first match.
     */
    {.callback	     = dmi_matched,
	.ident	     = "HONOR ZQC-P M1010 EC keyboard backlight",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "ZQC-P"),
	    DMI_MATCH(DMI_BOARD_NAME, "ZQC-P-PCB"), DMI_MATCH(DMI_BOARD_VERSION, "M1010")},
	.driver_data = &quirk_honor_zqcp_m1010},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR DRA-XX HUNTER M1030",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "DRA-XX"),
	    DMI_MATCH(DMI_BOARD_VERSION, "M1030")},
	.driver_data = &quirk_honor_dra_hunter},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR DRA-XX HUNTER M1040",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "DRA-XX"),
	    DMI_MATCH(DMI_BOARD_VERSION, "M1040")},
	.driver_data = &quirk_honor_dra_hunter},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR DRB-P HUNTER M1020",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "DRB-P"),
	    DMI_MATCH(DMI_BOARD_NAME, "DRB-P-PCB"), DMI_MATCH(DMI_BOARD_VERSION, "M1020")},
	.driver_data = &quirk_honor_drbp_hunter},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR DRB-P HUNTER M1100",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "DRB-P"),
	    DMI_MATCH(DMI_BOARD_NAME, "DRB-P-PCB"), DMI_MATCH(DMI_BOARD_VERSION, "M1100")},
	.driver_data = &quirk_honor_drbp_hunter},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR ZQC-P",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "ZQC-P")},
	.driver_data = &quirk_honor_platform_profile},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR XWC-P",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "XWC-P")},
	.driver_data = &quirk_honor_platform_profile},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR FMB-P",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "FMB-P"),
	    DMI_MATCH(DMI_BOARD_NAME, "FMB-P-PCB")},
	.driver_data = &quirk_honor_fmbp},
    {
	.callback	     = dmi_matched,
	.ident	     = "HONOR FMB-PM",
	.matches     = {
		DMI_MATCH(DMI_SYS_VENDOR, "HONOR"),
		DMI_MATCH(DMI_PRODUCT_NAME, "FMB-PM")
	},
	.driver_data = &quirk_honor_platform_profile
	},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR BCC-N",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "BCC-N")},
	.driver_data = &quirk_honor_platform_profile},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR MRB-XXX",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "MRB-XXX")},
	.driver_data = &quirk_honor_platform_profile},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR FRB-X",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "FRB-X")},
	.driver_data = &quirk_honor_platform_profile},
    {.callback	 = dmi_matched,
	.ident	 = "HONOR GLO-GXXX",
	.matches = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "GLO-GXXX")},
	.driver_data = &quirk_honor_platform_profile},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR FMI-XX",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "FMI-XX")},
	.driver_data = &quirk_honor_platform_profile},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR DRA-XX",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "DRA-XX")},
	.driver_data = &quirk_honor_platform_profile},
    {.callback	     = dmi_matched,
	.ident	     = "HONOR DRB-P",
	.matches     = {DMI_MATCH(DMI_SYS_VENDOR, "HONOR"), DMI_MATCH(DMI_PRODUCT_NAME, "DRB-P")},
	.driver_data = &quirk_honor_drbp},
    {}};

/* Utils */

static int huawei_wmi_call(struct huawei_wmi  *huawei,
			   struct acpi_buffer *in,
			   struct acpi_buffer *out)
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
	struct acpi_buffer out	  = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in;
	union acpi_object *obj;
	size_t		   len;
	int		   err, i;

	in.length  = sizeof(arg);
	in.pointer = &arg;

	/* Some models require calling HWMI twice to execute a command. We
	 * evaluate HWMI and if we get a non-zero return status we evaluate it
	 * again.
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
		/* Models that implement both "legacy" and HWMI tend to return a
		 * 0x104 sized buffer instead of a package of 0x4 and 0x100
		 * buffers.
		 */
		case ACPI_TYPE_BUFFER:
			if (obj->buffer.length == 0x104) {
				// Skip the first 4 bytes.
				obj->buffer.pointer += 4;
				len		     = HWMI_BUFF_SIZE;
			} else {
				dev_err(huawei->dev,
					"Bad buffer length, got %d\n",
					obj->buffer.length);
				err = -EIO;
				goto fail_cmd;
			}

			break;
		/* HWMI returns a package with 2 buffer elements, one of 4 bytes
		 * and the other is 256 bytes.
		 */
		case ACPI_TYPE_PACKAGE:
			if (obj->package.count != 2) {
				dev_err(huawei->dev,
					"Bad package count, got %d\n",
					obj->package.count);
				err = -EIO;
				goto fail_cmd;
			}

			obj = &obj->package.elements[1];
			if (obj->type != ACPI_TYPE_BUFFER) {
				dev_err(huawei->dev,
					"Bad package element type, got %d\n",
					obj->type);
				err = -EIO;
				goto fail_cmd;
			}
			len = obj->buffer.length;

			break;
		/* Shouldn't get here! */
		default:
			dev_err(huawei->dev,
				"Unexpected obj type, got: %d\n",
				obj->type);
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
				      enum led_brightness  brightness)
{
	/* This is a workaround until the "legacy" interface is implemented. */
	if (quirks && quirks->ec_micmute) {
		char		       *acpi_method;
		acpi_handle		handle;
		acpi_status		status;
		union acpi_object	args[3];
		struct acpi_object_list arg_list = {
			.pointer = args,
			.count	 = ARRAY_SIZE(args),
		};

		handle = ec_get_handle();
		if (!handle)
			return -ENODEV;

		args[0].type = args[1].type = args[2].type = ACPI_TYPE_INTEGER;
		args[1].integer.value			   = 0x04;

		if (acpi_has_method(handle, "SPIN")) {
			acpi_method	      = "SPIN";
			args[0].integer.value = 0;
			args[2].integer.value = brightness ? 1 : 0;
		} else if (acpi_has_method(handle, "WPIN")) {
			acpi_method	      = "WPIN";
			args[0].integer.value = 1;
			args[2].integer.value = brightness ? 0 : 1;
		} else {
			return -ENODEV;
		}

		status =
		    acpi_evaluate_object(handle, acpi_method, &arg_list, NULL);
		if (ACPI_FAILURE(status))
			return -ENODEV;

		return 0;
	} else {
		union hwmi_arg arg;

		arg.cmd	    = MICMUTE_LED_SET;
		arg.args[2] = brightness;

		return huawei_wmi_cmd(arg.cmd, NULL, 0);
	}
}

static int huawei_wmi_kbdlight_set_auto(int level);
static int huawei_wmi_kbdlight_ec_set(int level);

static int huawei_wmi_kbdlight_led_set(struct led_classdev *led_cdev,
				       enum led_brightness  brightness)
{
	if (quirks && quirks->kbdlight_ec)
		return huawei_wmi_kbdlight_ec_set(brightness);

	return huawei_wmi_kbdlight_set_auto(brightness);
}

static void huawei_wmi_leds_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	huawei->micmute_cdev.name	    = "huawei::micmute";
	huawei->micmute_cdev.max_brightness = 1;
	huawei->micmute_cdev.brightness_set_blocking =
	    &huawei_wmi_micmute_led_set;
	huawei->micmute_cdev.default_trigger = "audio-micmute";
	huawei->micmute_cdev.dev	     = dev;
	huawei->micmute_cdev.flags	     = LED_CORE_SUSPENDRESUME;

	if (acpi_has_method(NULL, "\\SMLS") || (quirks && quirks->ec_micmute))
		devm_led_classdev_register(dev, &huawei->micmute_cdev);

	huawei->kbdlight_cdev.name = "huawei::kbd_backlight";
	huawei->kbdlight_cdev.max_brightness =
	    quirks && quirks->kbdlight_ec ? 2 : 100;
	if (quirks && quirks->kbdlight_ec)
		huawei->kbdlight_cdev.brightness = 1;
	huawei->kbdlight_cdev.brightness_set_blocking =
	    &huawei_wmi_kbdlight_led_set;
	huawei->kbdlight_cdev.dev = dev;

	if (acpi_has_method(NULL, "\\SKBL") ||
	    (quirks && (quirks->kbdlight_auto || quirks->kbdlight_ec)))
		devm_led_classdev_register(dev, &huawei->kbdlight_cdev);
}

/* Battery protection */

static int huawei_wmi_battery_get(int *start, int *end)
{
	u8  ret[HWMI_BUFF_SIZE];
	int err, i;

	err = huawei_wmi_cmd(BATTERY_THRESH_GET, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	/* Find the last two non-zero values. Return status is ignored. */
	i = ARRAY_SIZE(ret) - 1;
	do {
		if (start)
			*start = ret[i - 1];
		if (end)
			*end = ret[i];
	} while (i > 2 && !ret[i--]);

	return 0;
}

static int huawei_wmi_battery_set(int start, int end)
{
	union hwmi_arg arg;
	int	       err;

	if (start < 0 || end < 0 || start > 100 || end > 100)
		return -EINVAL;

	arg.cmd	    = BATTERY_THRESH_SET;
	arg.args[2] = start;
	arg.args[3] = end;

	/* This is an edge case were some models turn battery protection
	 * off without changing their thresholds values. We clear the
	 * values before turning off protection. Sometimes we need a sleep delay
	 * to make sure these values make their way to EC memory.
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

static ssize_t charge_control_start_threshold_show(
    struct device *dev, struct device_attribute *attr, char *buf)
{
	int err, start;

	err = huawei_wmi_battery_get(&start, NULL);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", start);
}

static ssize_t charge_control_end_threshold_show(struct device		 *dev,
						 struct device_attribute *attr,
						 char			 *buf)
{
	int err, end;

	err = huawei_wmi_battery_get(NULL, &end);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", end);
}

static ssize_t charge_control_thresholds_show(struct device	      *dev,
					      struct device_attribute *attr,
					      char		      *buf)
{
	int err, start, end;

	err = huawei_wmi_battery_get(&start, &end);
	if (err)
		return err;

	return sysfs_emit(buf, "%d %d\n", start, end);
}

static ssize_t
charge_control_start_threshold_store(struct device	     *dev,
				     struct device_attribute *attr,
				     const char		     *buf,
				     size_t		      size)
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

static ssize_t charge_control_end_threshold_store(struct device		  *dev,
						  struct device_attribute *attr,
						  const char		  *buf,
						  size_t		   size)
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

static ssize_t charge_control_thresholds_store(struct device	       *dev,
					       struct device_attribute *attr,
					       const char	       *buf,
					       size_t			size)
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
static int huawei_wmi_battery_add(struct power_supply	   *battery,
				  struct acpi_battery_hook *hook)
#else
static int huawei_wmi_battery_add(struct power_supply *battery)
#endif
{
	int err = 0;

	err = device_create_file(&battery->dev,
				 &dev_attr_charge_control_start_threshold);
	if (err)
		return err;

	err = device_create_file(&battery->dev,
				 &dev_attr_charge_control_end_threshold);
	if (err)
		device_remove_file(&battery->dev,
				   &dev_attr_charge_control_start_threshold);

	return err;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
static int huawei_wmi_battery_remove(struct power_supply      *battery,
				     struct acpi_battery_hook *hook)
#else
static int huawei_wmi_battery_remove(struct power_supply *battery)
#endif
{
	device_remove_file(&battery->dev,
			   &dev_attr_charge_control_start_threshold);
	device_remove_file(&battery->dev,
			   &dev_attr_charge_control_end_threshold);

	return 0;
}

static struct acpi_battery_hook huawei_wmi_battery_hook = {
	.add_battery	= huawei_wmi_battery_add,
	.remove_battery = huawei_wmi_battery_remove,
	.name		= "Huawei Battery Extension"
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

/* Smart charge param*/

static int huawei_wmi_smart_charge_param_get(int *value)
{
	u8  ret[HWMI_BUFF_SIZE];
	int err;

	err =
	    huawei_wmi_cmd(BATTERY_CHARGE_MODE_PARAM_GET, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	if (value)
		*value = ret[1];

	return 0;
}

static int huawei_wmi_smart_charge_param_set(int value)
{
	union hwmi_arg arg;
	int	       err;

	if (value < 0 || value > 2)
		return -EINVAL;

	arg.cmd	    = BATTERY_CHARGE_MODE_PARAM_SET;
	arg.args[2] = (u8)value;

	err = huawei_wmi_cmd(arg.cmd, NULL, 0);
	return err;
}

static ssize_t smart_charge_param_show(struct device	       *dev,
				       struct device_attribute *attr,
				       char		       *buf)
{
	int err, value;

	err = huawei_wmi_smart_charge_param_get(&value);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t smart_charge_param_store(struct device		*dev,
					struct device_attribute *attr,
					const char		*buf,
					size_t			 size)
{
	int err, value;

	if (sscanf(buf, "%d", &value) != 1)
		return -EINVAL;

	err = huawei_wmi_smart_charge_param_set(value);
	if (err)
		return err;

	return size;
}

static DEVICE_ATTR_RW(smart_charge_param);

static void huawei_wmi_smart_charge_param_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	huawei->smart_charge_param_available = true;
	if (huawei_wmi_smart_charge_param_get(NULL)) {
		huawei->smart_charge_param_available = false;
		return;
	}

	device_create_file(dev, &dev_attr_smart_charge_param);
}

static void huawei_wmi_smart_charge_param_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->smart_charge_param_available) {
		device_remove_file(dev, &dev_attr_smart_charge_param);
	}
}

/* Smart charge */

static int
huawei_wmi_smart_charge_get(int *mode, int *unknow, int *start, int *end)
{
	u8  ret[HWMI_BUFF_SIZE];
	int err;

	err = huawei_wmi_cmd(BATTERY_CHARGE_MODE_GET, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	if (mode)
		*mode = ret[1];
	if (unknow)
		*unknow = ret[2];
	if (start)
		*start = ret[3];
	if (end)
		*end = ret[4];

	return 0;
}

static int huawei_wmi_smart_charge_set(int mode, int unknow, int start, int end)
{
	union hwmi_arg arg;
	int	       err;

	if (start < 0 || end < 0 || start > 100 || end > 100 || mode < 0 ||
	    mode > 0xff || unknow < 0 || unknow > 0xff)
		return -EINVAL;

	arg.cmd	    = BATTERY_CHARGE_MODE_SET;
	arg.args[2] = (u8)mode;
	arg.args[3] = (u8)unknow;
	arg.args[4] = (u8)start;
	arg.args[5] = (u8)end;

	err = huawei_wmi_cmd(arg.cmd, NULL, 0);
	return err;
}

static ssize_t
smart_charge_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int err, start, end, mode, unknow;

	err = huawei_wmi_smart_charge_get(&mode, &unknow, &start, &end);
	if (err)
		return err;

	return sysfs_emit(buf, "%d %d %d %d\n", mode, unknow, start, end);
}

static ssize_t smart_charge_store(struct device		  *dev,
				  struct device_attribute *attr,
				  const char		  *buf,
				  size_t		   size)
{
	int err, start, end, mode, unknow;

	if (sscanf(buf, "%d %d %d %d", &mode, &unknow, &start, &end) != 4)
		return -EINVAL;

	err = huawei_wmi_smart_charge_set(mode, unknow, start, end);
	if (err)
		return err;

	return size;
}

static DEVICE_ATTR_RW(smart_charge);

static void huawei_wmi_smart_charge_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	huawei->smart_charge_available = true;
	if (huawei_wmi_smart_charge_get(NULL, NULL, NULL, NULL)) {
		huawei->smart_charge_available = false;
		return;
	}

	device_create_file(dev, &dev_attr_smart_charge);
}

static void huawei_wmi_smart_charge_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->smart_charge_available) {
		device_remove_file(dev, &dev_attr_smart_charge);
	}
}

/* Fn lock */

static int huawei_wmi_fn_lock_get(int *on)
{
	u8  ret[HWMI_BUFF_SIZE] = { 0 };
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

	arg.cmd	    = FN_LOCK_SET;
	arg.args[2] = on + 1; // 0 undefined, 1 off, 2 on.

	return huawei_wmi_cmd(arg.cmd, NULL, 0);
}

static ssize_t
fn_lock_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int err, on;

	err = huawei_wmi_fn_lock_get(&on);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", on);
}

static ssize_t fn_lock_state_store(struct device	   *dev,
				   struct device_attribute *attr,
				   const char		   *buf,
				   size_t		    size)
{
	int on, err;

	if (kstrtoint(buf, 10, &on) || on < 0 || on > 1)
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
	u8  ret[HWMI_BUFF_SIZE] = { 0 };
	int err;
	u8  ec_level;

	if (quirks && quirks->kbdlight_ec) {
		mutex_lock(&huawei_wmi->kbdlight_lock);
		err = ec_read(HONOR_KBBL_OFFSET, &ec_level);
		if (err) {
			mutex_unlock(&huawei_wmi->kbdlight_lock);
			return err;
		}

		switch (ec_level) {
		case HONOR_KBBL_OFF:
			huawei_wmi->kbdlight_ec_level = 0;
			break;
		case HONOR_KBBL_LOW:
			huawei_wmi->kbdlight_ec_level = 1;
			break;
		case HONOR_KBBL_HIGH:
			huawei_wmi->kbdlight_ec_level = 2;
			break;
		case HONOR_KBBL_LATCH:
			break;
		default:
			mutex_unlock(&huawei_wmi->kbdlight_lock);
			return -ENODEV;
		}
		if (level)
			*level = huawei_wmi->kbdlight_ec_level;
		mutex_unlock(&huawei_wmi->kbdlight_lock);
		return 0;
	}

	err = huawei_wmi_cmd(KBDLIGHT_GET, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;
	if (!ret[2])
		return -ENODEV;

	/* Some models like the MACH-WX9 use 0x01, 0x02, and 0x04 for off, level
	 * 1, and level 2 respectively rather than 0x04, 0x08, and 0x10.
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

static void huawei_wmi_kbdlight_latch_work(struct work_struct *work)
{
	struct huawei_wmi *huawei = container_of(
	    to_delayed_work(work), struct huawei_wmi, kbdlight_latch_work);

	mutex_lock(&huawei->kbdlight_lock);
	if (huawei->kbdlight_steady && huawei->kbdlight_ec_level)
		ec_write(HONOR_KBBL_OFFSET, HONOR_KBBL_LATCH);
	mutex_unlock(&huawei->kbdlight_lock);
}

static int huawei_wmi_kbdlight_ec_set(int level)
{
	struct huawei_wmi *huawei = huawei_wmi;
	u8		   ec_level;
	bool		   latch;
	int		   err;

	if (level < 0 || level > 2)
		return -EINVAL;

	cancel_delayed_work_sync(&huawei->kbdlight_latch_work);
	switch (level) {
	case 0:
		ec_level = HONOR_KBBL_OFF;
		break;
	case 1:
		ec_level = HONOR_KBBL_LOW;
		break;
	default:
		ec_level = HONOR_KBBL_HIGH;
		break;
	}

	mutex_lock(&huawei->kbdlight_lock);
	err = ec_write(HONOR_KBBL_OFFSET, ec_level);
	if (!err)
		huawei->kbdlight_ec_level = level;
	latch = !err && huawei->kbdlight_steady && level;
	mutex_unlock(&huawei->kbdlight_lock);

	if (latch)
		mod_delayed_work(system_wq,
				 &huawei->kbdlight_latch_work,
				 msecs_to_jiffies(1500));

	return err;
}

static int huawei_wmi_kbdlight_set(int level)
{
	union hwmi_arg arg;

	if (quirks && quirks->kbdlight_ec)
		return huawei_wmi_kbdlight_ec_set(level);

	// Huawei laptops only support 3 kbdlight levels
	if (level < 0 || level > 2)
		return -EINVAL;
	if (!huawei_wmi->kbdlight_quirk_input)
		level += 2;

	arg.cmd	    = KBDLIGHT_SET;
	arg.args[2] = 1 << level;

	return huawei_wmi_cmd(arg.cmd, NULL, 0);
}

static int huawei_wmi_kbdlight_set_auto(int level)
{
	union hwmi_arg arg;
	int	       err;

	if (level < 0 || level > 255)
		return -EINVAL;

	arg.cmd	    = KBDLIGHT_MODE_SET;
	arg.args[2] = KBDLIGHT_MODE_AUTO;
	err	    = huawei_wmi_cmd(arg.cmd, NULL, 0);
	if (err)
		return err;

	msleep(10);

	arg.cmd	    = KBDLIGHT_SET_AUTO;
	arg.args[2] = level;

	return huawei_wmi_cmd(arg.cmd, NULL, 0);
}

static ssize_t
kbdlight_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	bool		   steady;

	mutex_lock(&huawei->kbdlight_lock);
	steady = huawei->kbdlight_steady;
	mutex_unlock(&huawei->kbdlight_lock);

	return sysfs_emit(buf, "%s\n", steady ? "steady" : "reactive");
}

static ssize_t kbdlight_mode_store(struct device	   *dev,
				   struct device_attribute *attr,
				   const char		   *buf,
				   size_t		    size)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	bool		   old_steady, steady;
	int		   level, err;

	if (sysfs_streq(buf, "steady"))
		steady = true;
	else if (sysfs_streq(buf, "reactive"))
		steady = false;
	else
		return -EINVAL;

	cancel_delayed_work_sync(&huawei->kbdlight_latch_work);
	mutex_lock(&huawei->kbdlight_lock);
	old_steady		= huawei->kbdlight_steady;
	huawei->kbdlight_steady = steady;
	level			= huawei->kbdlight_ec_level;
	mutex_unlock(&huawei->kbdlight_lock);

	err = huawei_wmi_kbdlight_ec_set(level);
	if (err) {
		mutex_lock(&huawei->kbdlight_lock);
		huawei->kbdlight_steady = old_steady;
		mutex_unlock(&huawei->kbdlight_lock);
		return err;
	}

	return size;
}

static DEVICE_ATTR_RW(kbdlight_mode);

static ssize_t
kbdlight_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int err, level;

	err = huawei_wmi_kbdlight_get(&level);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", level);
}

static ssize_t kbdlight_store(struct device	      *dev,
			      struct device_attribute *attr,
			      const char	      *buf,
			      size_t		       size)
{
	int level, err;

	if (kstrtoint(buf, 10, &level))
		return -EINVAL;

	if (quirks && quirks->kbdlight_ec)
		err = huawei_wmi_kbdlight_ec_set(level);
	else if (quirks && quirks->kbdlight_auto)
		err = huawei_wmi_kbdlight_set_auto(level);
	else
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
	if (quirks && quirks->kbdlight_ec) {
		mutex_init(&huawei->kbdlight_lock);
		INIT_DELAYED_WORK(&huawei->kbdlight_latch_work,
				  huawei_wmi_kbdlight_latch_work);
		huawei->kbdlight_ec_level = 1;
		if (huawei_wmi_kbdlight_get(NULL)) {
			huawei->kbdlight_available = false;
			return;
		}
	} else if (!(acpi_has_method(NULL, "\\SKBL") ||
		     (quirks && quirks->kbdlight_auto)) &&
		   huawei_wmi_kbdlight_get(NULL)) {
		huawei->kbdlight_available = false;
		return;
	}

	device_create_file(dev, &dev_attr_kbdlight);
	if (quirks && quirks->kbdlight_ec)
		device_create_file(dev, &dev_attr_kbdlight_mode);
}

static void huawei_wmi_kbdlight_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (quirks && quirks->kbdlight_ec) {
		cancel_delayed_work_sync(&huawei->kbdlight_latch_work);
		device_remove_file(dev, &dev_attr_kbdlight_mode);
	}
	if (huawei->kbdlight_available)
		device_remove_file(dev, &dev_attr_kbdlight);
}

/* Keyboard backlight timeout */

static int huawei_wmi_kbdlight_timeout_get(int *seconds)
{
	u8  ret[HWMI_BUFF_SIZE] = { 0 };
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

	arg.cmd	    = KBDLIGHT_TIMEOUT_SET;
	arg.args[2] = (seconds & 0xff);
	arg.args[3] = (seconds >> 8);

	return huawei_wmi_cmd(arg.cmd, NULL, 0);
}

static ssize_t kbdlight_timeout_show(struct device	     *dev,
				     struct device_attribute *attr,
				     char		     *buf)
{
	int err, seconds;

	err = huawei_wmi_kbdlight_timeout_get(&seconds);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", seconds);
}

static ssize_t kbdlight_timeout_store(struct device	      *dev,
				      struct device_attribute *attr,
				      const char	      *buf,
				      size_t		       size)
{
	int seconds, err;

	if (kstrtoint(buf, 10, &seconds) || seconds < 0 || seconds > 0xffff)
		return -EINVAL;

	err = huawei_wmi_kbdlight_timeout_set(seconds);
	if (err)
		return err;

	return size;
}

static DEVICE_ATTR_RW(kbdlight_timeout);

static void huawei_wmi_kbdlight_timeout_setup(struct device *dev)
{
	struct huawei_wmi *huawei	   = dev_get_drvdata(dev);
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

/* Power unlock */

static int huawei_wmi_power_unlock_get(int *on)
{
	u8  ret[HWMI_BUFF_SIZE] = { 0 };
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
	int	       actual_on, err;

	arg.cmd	    = POWER_UNLOCK_SET;
	arg.args[2] = on;

	err = huawei_wmi_cmd(arg.cmd, NULL, 0);
	if (err)
		return err;

	err = huawei_wmi_power_unlock_get(&actual_on);
	if (err)
		return err;

	return actual_on == on ? 0 : -EBUSY;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
static int huawei_wmi_power_supply_check_battery(struct power_supply *psy,
						 void		     *data)
{
	bool			  *battery_discharging = data;
	union power_supply_propval value;

	if (psy->desc->type != POWER_SUPPLY_TYPE_BATTERY)
		return 0;

	if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_STATUS, &value) &&
	    value.intval == POWER_SUPPLY_STATUS_DISCHARGING)
		*battery_discharging = true;

	return 0;
}

static bool huawei_wmi_platform_profile_battery_limited(void)
{
	bool battery_discharging = false;

	power_supply_for_each_psy(&battery_discharging,
				  huawei_wmi_power_supply_check_battery);

	return !power_supply_is_system_supplied() || battery_discharging;
}
#endif

static ssize_t
power_unlock_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int err, on;

	err = huawei_wmi_power_unlock_get(&on);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", on);
}

static ssize_t power_unlock_store(struct device		  *dev,
				  struct device_attribute *attr,
				  const char		  *buf,
				  size_t		   size)
{
	int mode, err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	struct huawei_wmi *huawei	   = dev_get_drvdata(dev);
	bool		   profile_control = quirks && quirks->platform_profile;

	if (profile_control)
		mutex_lock(&huawei->platform_profile_lock);
#endif

	if (kstrtoint(buf, 10, &mode) ||
	    (mode != PROFILE_BALANCED && mode != PROFILE_PERFORMANCE &&
	     (mode != PROFILE_HUNTER || !quirks ||
	      !quirks->platform_profile_hunter))) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		if (profile_control)
			mutex_unlock(&huawei->platform_profile_lock);
#endif
		return -EINVAL;
	}
	if (quirks && quirks->platform_profile && mode != PROFILE_BALANCED &&
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	    huawei_wmi_platform_profile_battery_limited()) {
#else
	    !power_supply_is_system_supplied()) {
#endif
		err = huawei_wmi_power_unlock_set(PROFILE_BALANCED);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		if (profile_control)
			mutex_unlock(&huawei->platform_profile_lock);
#endif
		return err ? err : -EBUSY;
	}

	err = huawei_wmi_power_unlock_set(mode);
	if (err) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		if (profile_control)
			mutex_unlock(&huawei->platform_profile_lock);
#endif
		return err;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	if (profile_control)
		mutex_unlock(&huawei->platform_profile_lock);
#endif

	return size;
}

static DEVICE_ATTR_RW(power_unlock);

static void huawei_wmi_power_unlock_setup(struct device *dev)
{
	struct huawei_wmi *huawei      = dev_get_drvdata(dev);
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
static int
huawei_wmi_platform_profile_force_mode_locked(struct huawei_wmi *huawei,
					      int		 mode)
{
	int actual_mode, err;

	err = huawei_wmi_power_unlock_get(&actual_mode);
	if (err || actual_mode == mode)
		return err;

	return huawei_wmi_power_unlock_set(mode);
}

static int
huawei_wmi_platform_profile_apply(struct huawei_wmi	      *huawei,
				  enum platform_profile_option profile)
{
	const char *profile_name;
	int	    mode, err;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		if (!quirks || !quirks->platform_profile_hunter)
			return -EOPNOTSUPP;
		mode	     = PROFILE_BALANCED;
		profile_name = "low-power";
		break;
	case PLATFORM_PROFILE_BALANCED:
		mode	     = quirks && quirks->platform_profile_hunter
				   ? PROFILE_PERFORMANCE
				   : PROFILE_BALANCED;
		profile_name = "balanced";
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		mode	     = quirks && quirks->platform_profile_hunter
				   ? PROFILE_HUNTER
				   : PROFILE_PERFORMANCE;
		profile_name = "performance";
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&huawei->platform_profile_lock);
	if (huawei_wmi_platform_profile_battery_limited() &&
	    mode != PROFILE_BALANCED) {
		err = huawei_wmi_platform_profile_force_mode_locked(
		    huawei, PROFILE_BALANCED);
		if (!err)
			err = -EBUSY;
		goto out_unlock;
	}

	err = huawei_wmi_power_unlock_set(mode);
	if (!err && mode != PROFILE_BALANCED &&
	    huawei_wmi_platform_profile_battery_limited()) {
		err = huawei_wmi_platform_profile_force_mode_locked(
		    huawei, PROFILE_BALANCED);
		if (!err)
			err = -EBUSY;
		goto out_unlock;
	}
	if (err && quirks && quirks->platform_profile_hunter &&
	    profile == PLATFORM_PROFILE_PERFORMANCE &&
	    !huawei_wmi_platform_profile_battery_limited())
		goto fallback_balanced;
	if (err) {
		if (err == -EBUSY)
			dev_warn(huawei->dev,
				 "Firmware did not apply platform profile %s\n",
				 profile_name);
		goto out_unlock;
	}

	goto out_unlock;

fallback_balanced:
	dev_warn(
	    huawei->dev,
	    "Firmware rejected HUNTER; falling back to balanced profile\n");
	err = huawei_wmi_platform_profile_force_mode_locked(
	    huawei, PROFILE_PERFORMANCE);
	if (!err)
		err = -EBUSY;

out_unlock:
	mutex_unlock(&huawei->platform_profile_lock);
	return err;
}

static int
huawei_wmi_platform_profile_get_firmware(struct huawei_wmi	      *huawei,
					 enum platform_profile_option *profile)
{
	int on, err;

	err = huawei_wmi_power_unlock_get(&on);
	if (err)
		return err;

	switch (on) {
	case PROFILE_BALANCED:
		*profile = quirks && quirks->platform_profile_hunter
			       ? PLATFORM_PROFILE_LOW_POWER
			       : PLATFORM_PROFILE_BALANCED;
		break;
	case PROFILE_PERFORMANCE:
		*profile = quirks && quirks->platform_profile_hunter
			       ? PLATFORM_PROFILE_BALANCED
			       : PLATFORM_PROFILE_PERFORMANCE;
		break;
	case PROFILE_HUNTER:
		if (quirks && quirks->platform_profile_hunter) {
			*profile = PLATFORM_PROFILE_PERFORMANCE;
			break;
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}

	return 0;
}

static int
huawei_wmi_platform_profile_get_current(struct huawei_wmi	     *huawei,
					enum platform_profile_option *profile)
{
	return huawei_wmi_platform_profile_get_firmware(huawei, profile);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static int huawei_wmi_platform_profile_probe(void	   *drvdata,
					     unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	if (quirks && quirks->platform_profile_hunter)
		set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	return 0;
}

static int
huawei_wmi_platform_profile_get(struct device		     *dev,
				enum platform_profile_option *profile)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	return huawei_wmi_platform_profile_get_current(huawei, profile);
}

static int huawei_wmi_platform_profile_set(struct device	       *dev,
					   enum platform_profile_option profile)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	return huawei_wmi_platform_profile_apply(huawei, profile);
}

static const struct platform_profile_ops huawei_wmi_platform_profile_ops = {
	.probe	     = huawei_wmi_platform_profile_probe,
	.profile_get = huawei_wmi_platform_profile_get,
	.profile_set = huawei_wmi_platform_profile_set,
};
#else
static int
huawei_wmi_platform_profile_get(struct platform_profile_handler *handler,
				enum platform_profile_option	*profile)
{
	struct huawei_wmi *huawei =
	    container_of(handler, struct huawei_wmi, profile_handler);

	return huawei_wmi_platform_profile_get_current(huawei, profile);
}

static int
huawei_wmi_platform_profile_set(struct platform_profile_handler *handler,
				enum platform_profile_option	 profile)
{
	struct huawei_wmi *huawei =
	    container_of(handler, struct huawei_wmi, profile_handler);

	return huawei_wmi_platform_profile_apply(huawei, profile);
}
#endif

static void huawei_wmi_platform_profile_work(struct work_struct *work)
{
	struct huawei_wmi *huawei = container_of(
	    to_delayed_work(work), struct huawei_wmi, platform_profile_work);
	int err;

	mutex_lock(&huawei->platform_profile_lock);
	if (huawei_wmi_platform_profile_battery_limited()) {
		err = huawei_wmi_platform_profile_force_mode_locked(
		    huawei, PROFILE_BALANCED);

		if (err)
			dev_warn(huawei->dev,
				 "Failed to switch to battery-safe platform "
				 "profile: %d\n",
				 err);
	}
	mutex_unlock(&huawei->platform_profile_lock);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	platform_profile_notify(huawei->platform_profile_device);
#else
	platform_profile_notify();
#endif
}

static int huawei_wmi_acpi_notifier(struct notifier_block *nb,
				    unsigned long	   event,
				    void		  *data)
{
	struct huawei_wmi *huawei =
	    container_of(nb, struct huawei_wmi, acpi_nb);
	struct acpi_bus_event *acpi_event = data;

	if (!acpi_event || strcmp(acpi_event->device_class, "ac_adapter"))
		return NOTIFY_DONE;

	mod_delayed_work(
	    system_wq, &huawei->platform_profile_work, msecs_to_jiffies(500));
	return NOTIFY_OK;
}

static int huawei_wmi_power_supply_notifier(struct notifier_block *nb,
					    unsigned long	   event,
					    void		  *data)
{
	struct huawei_wmi *huawei =
	    container_of(nb, struct huawei_wmi, power_supply_nb);
	struct power_supply *psy = data;

	if (event != PSY_EVENT_PROP_CHANGED || !psy ||
	    psy->desc->type != POWER_SUPPLY_TYPE_BATTERY)
		return NOTIFY_DONE;

	mod_delayed_work(
	    system_wq, &huawei->platform_profile_work, msecs_to_jiffies(500));
	return NOTIFY_OK;
}

static void huawei_wmi_platform_profile_setup(struct device *dev)
{
	struct huawei_wmi	    *huawei = dev_get_drvdata(dev);
	enum platform_profile_option profile;
	int			     err;

	if (!quirks || !quirks->platform_profile)
		return;

	if (huawei_wmi_platform_profile_battery_limited()) {
		mutex_lock(&huawei->platform_profile_lock);
		err = huawei_wmi_platform_profile_force_mode_locked(
		    huawei, PROFILE_BALANCED);
		mutex_unlock(&huawei->platform_profile_lock);
		if (err)
			dev_warn(dev,
				 "Failed to set battery-safe platform profile "
				 "during "
				 "probe: %d\n",
				 err);
	}

	/* Some firmware lacks this command or exposes additional modes such as
	 * HUNTER. */
	err = huawei_wmi_platform_profile_get_firmware(huawei, &profile);
	if (err) {
		dev_dbg(dev, "Platform profile WMI command is not supported\n");
		return;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	huawei->platform_profile_device = devm_platform_profile_register(
	    dev, "huawei-wmi", huawei, &huawei_wmi_platform_profile_ops);
	if (IS_ERR(huawei->platform_profile_device)) {
		dev_warn(dev, "Failed to register platform profile provider\n");
		return;
	}
#else
	int err;

	set_bit(PLATFORM_PROFILE_BALANCED, huawei->profile_handler.choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, huawei->profile_handler.choices);
	if (quirks->platform_profile_hunter)
		set_bit(PLATFORM_PROFILE_LOW_POWER,
			huawei->profile_handler.choices);
	huawei->profile_handler.profile_get = huawei_wmi_platform_profile_get;
	huawei->profile_handler.profile_set = huawei_wmi_platform_profile_set;
	err = platform_profile_register(&huawei->profile_handler);
	if (err) {
		dev_warn(dev,
			 "Failed to register platform profile provider: %d\n",
			 err);
		return;
	}
#endif
	huawei->platform_profile_available = true;
	INIT_DELAYED_WORK(&huawei->platform_profile_work,
			  huawei_wmi_platform_profile_work);
	huawei->power_supply_nb.notifier_call =
	    huawei_wmi_power_supply_notifier;
	if (power_supply_reg_notifier(&huawei->power_supply_nb))
		dev_warn(dev, "Failed to register power supply notifier\n");
	else
		huawei->power_supply_notifier_registered = true;
	huawei->acpi_nb.notifier_call = huawei_wmi_acpi_notifier;
	if (register_acpi_notifier(&huawei->acpi_nb)) {
		dev_warn(dev, "Failed to register AC adapter notifier\n");
		return;
	}
	huawei->acpi_notifier_registered = true;
}

static void huawei_wmi_platform_profile_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->acpi_notifier_registered)
		unregister_acpi_notifier(&huawei->acpi_nb);
	if (huawei->power_supply_notifier_registered)
		power_supply_unreg_notifier(&huawei->power_supply_nb);
	if (huawei->platform_profile_available)
		cancel_delayed_work_sync(&huawei->platform_profile_work);
	huawei->acpi_notifier_registered = false;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
	if (huawei->platform_profile_available)
		platform_profile_remove();
#endif
	huawei->platform_profile_available = false;
}
#endif

/* Hwmon subdriver */

/* Fan speed */

static int huawei_wmi_fan_speed_get(u8 num, int *rpm)
{
	u8  ret[HWMI_BUFF_SIZE] = { 0 };
	int err, fan_rpm;
	u8  low, high;
	u8  offset;

	if (quirks && quirks->fan_ec) {
		if (num > 1)
			return -EINVAL;

		offset = num ? 0x2e : 0x2c;
		err    = ec_read(offset, &low);
		if (!err)
			err = ec_read(offset + 1, &high);
		if (!err) {
			fan_rpm = low | (high << 8);
			if (fan_rpm <= 20000) {
				if (rpm)
					*rpm = fan_rpm;
				return 0;
			}
		}
	}

	union hwmi_arg arg;
	arg.cmd	    = FAN_SPEED_GET;
	arg.args[2] = num;

	err = huawei_wmi_cmd(arg.cmd, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	if (rpm)
		*rpm = ret[1] | (ret[2] << 8);

	return 0;
}

static ssize_t
fan1_input_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int err, rpm;

	err = huawei_wmi_fan_speed_get(0, &rpm);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", rpm);
}

static ssize_t
fan2_input_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int err, rpm;

	err = huawei_wmi_fan_speed_get(1, &rpm);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", rpm);
}

static DEVICE_ATTR_RO(fan1_input);
static DEVICE_ATTR_RO(fan2_input);

static void huawei_wmi_fan_speed_setup(struct device *dev)
{
	struct huawei_wmi *huawei   = dev_get_drvdata(dev);
	huawei->fan_speed_available = true;
	if (huawei_wmi_fan_speed_get(0, NULL)) {
		huawei->fan_speed_available = false;
		return;
	}

	device_create_file(huawei->hwmon, &dev_attr_fan1_input);
	device_create_file(huawei->hwmon, &dev_attr_fan2_input);
}

static void huawei_wmi_fan_speed_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->fan_speed_available) {
		device_remove_file(huawei->hwmon, &dev_attr_fan1_input);
		device_remove_file(huawei->hwmon, &dev_attr_fan2_input);
	}
}

/* Temp */
/*
 * HVY-WXX9 and WRT-WX9 have more temp zone
 *
 * 0x00 CTMP cpu     TP00
 * 0x01              TP01
 * 0x05 TSLO         TP08
 * 0x06              TP06
 * 0x07 TNTC         TP02
 * 0x08 CNTC         TP03
 * 0x0B DNTC         TP05
 * 0x0E BTMP battery BTEM
 * 0x0F              TP0C
 * 0x15              TP07
 * 0x16              TP04
 */

static int huawei_wmi_temp_get(u8 num, int *temp)
{
	u8  ret[HWMI_BUFF_SIZE] = { 0 };
	int err;

	union hwmi_arg arg;
	arg.cmd	    = TEMP_GET;
	arg.args[2] = num;

	err = huawei_wmi_cmd(arg.cmd, ret, HWMI_BUFF_SIZE);
	if (err)
		return err;

	if (temp)
		*temp = ret[2];

	return 0;
}

#define CREATE_TEMP_ATTR(_idxA, _idxB, _idxC)                                  \
	static ssize_t temp##_idxA##_input_show(                               \
	    struct device *dev, struct device_attribute *attr, char *buf)      \
	{                                                                      \
		int err, temp;                                                 \
		err = huawei_wmi_temp_get(_idxB, &temp);                       \
		if (err)                                                       \
			return err;                                            \
                                                                               \
		return sysfs_emit(buf, "%d000\n", temp);                       \
	}                                                                      \
                                                                               \
	static DEVICE_ATTR_RO(temp##_idxA##_input);                            \
                                                                               \
	static ssize_t temp##_idxA##_label_show(                               \
	    struct device *dev, struct device_attribute *attr, char *buf)      \
	{                                                                      \
		return sysfs_emit(buf, _idxC);                                 \
	}                                                                      \
                                                                               \
	static DEVICE_ATTR_RO(temp##_idxA##_label);

CREATE_TEMP_ATTR(1, 0x00, "cpu\n")
CREATE_TEMP_ATTR(2, 0x01, "TP01\n")
CREATE_TEMP_ATTR(3, 0x05, "TSLO\n")
CREATE_TEMP_ATTR(4, 0x06, "TP06\n")
CREATE_TEMP_ATTR(5, 0x07, "TNTC\n")
CREATE_TEMP_ATTR(6, 0x08, "CNTC\n")
CREATE_TEMP_ATTR(7, 0x0B, "DNTC\n")
CREATE_TEMP_ATTR(8, 0x0E, "battery\n")
CREATE_TEMP_ATTR(9, 0x0F, "TP0C\n")
CREATE_TEMP_ATTR(10, 0x15, "TP07\n")
CREATE_TEMP_ATTR(11, 0x16, "TP04\n")

#define CREATE_TEMP_FILE(_idxA)                                                \
	device_create_file(huawei->hwmon, &dev_attr_temp##_idxA##_input);      \
	device_create_file(huawei->hwmon, &dev_attr_temp##_idxA##_label);

#define REMOVE_TEMP_FILE(_idxA)                                                \
	device_remove_file(huawei->hwmon, &dev_attr_temp##_idxA##_label);      \
	device_remove_file(huawei->hwmon, &dev_attr_temp##_idxA##_input);

static void huawei_wmi_temp_setup(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);
	huawei->temp_available	  = true;
	if (huawei_wmi_temp_get(0, NULL)) {
		huawei->temp_available = false;
		return;
	}

	CREATE_TEMP_FILE(1)
	CREATE_TEMP_FILE(2)
	CREATE_TEMP_FILE(3)
	CREATE_TEMP_FILE(4)
	CREATE_TEMP_FILE(5)
	CREATE_TEMP_FILE(6)
	CREATE_TEMP_FILE(7)
	CREATE_TEMP_FILE(8)
	CREATE_TEMP_FILE(9)
	CREATE_TEMP_FILE(10)
	CREATE_TEMP_FILE(11)
}

static void huawei_wmi_temp_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	if (huawei->temp_available) {
		REMOVE_TEMP_FILE(1)
		REMOVE_TEMP_FILE(2)
		REMOVE_TEMP_FILE(3)
		REMOVE_TEMP_FILE(4)
		REMOVE_TEMP_FILE(5)
		REMOVE_TEMP_FILE(6)
		REMOVE_TEMP_FILE(7)
		REMOVE_TEMP_FILE(8)
		REMOVE_TEMP_FILE(9)
		REMOVE_TEMP_FILE(10)
		REMOVE_TEMP_FILE(11)
	}
}

/* debugfs */

static void huawei_wmi_debugfs_call_dump(struct seq_file   *m,
					 void		   *data,
					 union acpi_object *obj)
{
	struct huawei_wmi *huawei = m->private;
	int		   i;

	switch (obj->type) {
	case ACPI_TYPE_INTEGER:
		seq_printf(m, "0x%llx", obj->integer.value);
		break;
	case ACPI_TYPE_STRING:
		seq_printf(
		    m, "\"%.*s\"", obj->string.length, obj->string.pointer);
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
			huawei_wmi_debugfs_call_dump(
			    m, huawei, &obj->package.elements[i]);
			if (i < obj->package.count - 1)
				seq_puts(m, ",");
		}
		seq_puts(m, "]");
		break;
	default:
		dev_err(
		    huawei->dev, "Unexpected obj type, got %d\n", obj->type);
		return;
	}
}

static int huawei_wmi_debugfs_call_show(struct seq_file *m, void *data)
{
	struct huawei_wmi *huawei = m->private;
	struct acpi_buffer out	  = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in;
	union acpi_object *obj;
	int		   err;

	in.length  = sizeof(u64);
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

	debugfs_create_x64("arg", 0644, huawei->debug.root, &huawei->debug.arg);
	debugfs_create_file("call",
			    0400,
			    huawei->debug.root,
			    huawei,
			    &huawei_wmi_debugfs_call_fops);
}

static void huawei_wmi_debugfs_exit(struct device *dev)
{
	struct huawei_wmi *huawei = dev_get_drvdata(dev);

	debugfs_remove_recursive(huawei->debug.root);
}

/* Input */

static void huawei_wmi_process_key(struct input_dev *idev, int code)
{
	struct huawei_wmi      *huawei = dev_get_drvdata(idev->dev.parent);
	const struct key_entry *key;

	/*
	 * WMI0 uses code 0x80 to indicate a hotkey event.
	 * The actual key is fetched from the method WQ00
	 * using WMI0_EXPENSIVE_GUID.
	 */
	if (code == 0x80) {
		struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
		union acpi_object *obj;
		acpi_status	   status;

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
		dev_info(
		    &idev->dev, "Unknown key pressed, code: 0x%04x\n", code);
		return;
	}

	if (quirks && !quirks->report_brightness &&
	    (key->sw.code == KEY_BRIGHTNESSDOWN ||
	     key->sw.code == KEY_BRIGHTNESSUP))
		return;

	if (quirks && !quirks->report_volume &&
	    (key->sw.code == KEY_VOLUMEUP || key->sw.code == KEY_VOLUMEDOWN ||
	     key->sw.code == KEY_MUTE))
		return;

	if (quirks && quirks->handle_kbdlight && huawei->kbdlight_available &&
	    (key->code == KBDLIGHT_KEY_0 || key->code == KBDLIGHT_KEY_1 ||
	     key->code == KBDLIGHT_KEY_2)) {
		huawei_wmi_kbdlight_set(key->code - KBDLIGHT_KEY_0);
	}

	sparse_keymap_report_entry(idev, key, 1, true);
}

static void huawei_wmi_input_notify(union acpi_object *obj, void *context)
{
	struct input_dev *idev = (struct input_dev *)context;

	if (obj && obj->type == ACPI_TYPE_INTEGER)
		huawei_wmi_process_key(idev, obj->integer.value);
	else
		dev_err(&idev->dev, "Bad response type\n");
}

static int huawei_wmi_input_setup(struct device	    *dev,
				  const char	    *guid,
				  struct input_dev **idev)
{
	acpi_status status;
	int	    err;

	*idev = devm_input_allocate_device(dev);
	if (!*idev)
		return -ENOMEM;

	(*idev)->name	    = "Huawei WMI hotkeys";
	(*idev)->phys	    = "wmi/input0";
	(*idev)->id.bustype = BUS_HOST;
	(*idev)->dev.parent = dev;

	err = sparse_keymap_setup(*idev, huawei_wmi_keymap, NULL);
	if (err)
		return err;

	err = input_register_device(*idev);
	if (err)
		return err;

	status =
	    wmi_install_notify_handler(guid, huawei_wmi_input_notify, *idev);
	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static void huawei_wmi_input_exit(struct device *dev, const char *guid)
{
	wmi_remove_notify_handler(guid);
}

/* Huawei driver */

static const struct wmi_device_id huawei_wmi_events_id_table[] = {
	{ .guid_string = WMI0_EVENT_GUID },
	{ .guid_string = HWMI_EVENT_GUID },
	{}
};

static int huawei_wmi_probe(struct platform_device *pdev)
{
	const struct wmi_device_id *guid = huawei_wmi_events_id_table;
	struct input_dev	   *idev = *huawei_wmi->idev;
	int			    err;

	platform_set_drvdata(pdev, huawei_wmi);
	huawei_wmi->dev = &pdev->dev;

	while (*guid->guid_string) {
		if (wmi_has_guid(guid->guid_string)) {
			err = huawei_wmi_input_setup(
			    &pdev->dev, guid->guid_string, &idev);
			if (err) {
				dev_err(&pdev->dev,
					"Failed to setup input on %s\n",
					guid->guid_string);
				return err;
			}
		}

		idev++;
		guid++;
	}

	if (wmi_has_guid(HWMI_METHOD_GUID)) {
		mutex_init(&huawei_wmi->wmi_lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		mutex_init(&huawei_wmi->platform_profile_lock);
#endif

		huawei_wmi->hwmon = hwmon_device_register_with_groups(
		    &pdev->dev, "huawei_wmi", NULL, NULL);
		if (IS_ERR(huawei_wmi->hwmon)) {
			huawei_wmi->hwmon = NULL;
		} else {
			huawei_wmi_fan_speed_setup(&pdev->dev);
			huawei_wmi_temp_setup(&pdev->dev);
		}
		huawei_wmi_smart_charge_setup(&pdev->dev);
		huawei_wmi_smart_charge_param_setup(&pdev->dev);
		huawei_wmi_power_unlock_setup(&pdev->dev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		huawei_wmi_platform_profile_setup(&pdev->dev);
#endif
		huawei_wmi_kbdlight_timeout_setup(&pdev->dev);
		huawei_wmi_kbdlight_setup(&pdev->dev);
		huawei_wmi_leds_setup(&pdev->dev);
		huawei_wmi_fn_lock_setup(&pdev->dev);
		huawei_wmi_battery_setup(&pdev->dev);
		huawei_wmi_debugfs_setup(&pdev->dev);
	}

	return 0;
}

static void huawei_wmi_remove(struct platform_device *pdev)
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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		huawei_wmi_platform_profile_exit(&pdev->dev);
#endif
		huawei_wmi_smart_charge_exit(&pdev->dev);
		huawei_wmi_smart_charge_param_exit(&pdev->dev);
		if (huawei_wmi->hwmon) {
			huawei_wmi_temp_exit(&pdev->dev);
			huawei_wmi_fan_speed_exit(&pdev->dev);
			hwmon_device_unregister(huawei_wmi->hwmon);
		}
	}
}

static struct platform_driver huawei_wmi_driver = {
    .driver =
	{
	    .name = "huawei-wmi",
	},
    .probe  = huawei_wmi_probe,
    .remove = huawei_wmi_remove,
};

static __init int huawei_wmi_init(void)
{
	struct platform_device *pdev;
	int			err;

	huawei_wmi = kzalloc_obj(struct huawei_wmi);
	if (!huawei_wmi)
		return -ENOMEM;

	quirks = &quirk_unknown;
	dmi_check_system(huawei_quirks);
	if (battery_reset != -1)
		quirks->battery_reset = battery_reset;
	if (report_brightness != -1)
		quirks->report_brightness = report_brightness;
	if (report_volume != -1)
		quirks->report_volume = report_volume;
	if (handle_kbdlight != -1)
		quirks->handle_kbdlight = handle_kbdlight;
	if (kbdlight_auto != -1)
		quirks->kbdlight_auto = kbdlight_auto;

	err = platform_driver_register(&huawei_wmi_driver);
	if (err)
		goto pdrv_err;

	pdev = platform_device_register_simple(
	    "huawei-wmi", PLATFORM_DEVID_NONE, NULL, 0);
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

MODULE_ALIAS("wmi:" HWMI_METHOD_GUID);
MODULE_DEVICE_TABLE(wmi, huawei_wmi_events_id_table);
MODULE_AUTHOR("Ayman Bagabas <ayman.bagabas@gmail.com>");
MODULE_DESCRIPTION("Huawei WMI laptop extras driver");
MODULE_LICENSE("GPL v2");
