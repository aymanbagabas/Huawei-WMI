// SPDX-License-Identifier: GPL-2.0
/*
 *  Huawei WMI hotkeys
 *
 *  Copyright (C) 2018	      Ayman Bagabas <ayman.bagabas@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/module.h>
//#include <linux/platform_data/x86/huawei_wmi.h>

MODULE_AUTHOR("Ayman Bagabas <ayman.bagabas@gmail.com>");
MODULE_DESCRIPTION("Huawei WMI hotkeys");
MODULE_LICENSE("GPL");

/*
 * Huawei WMI Events GUIDs
 */
#define MBX_EVENT_GUID "59142400-C6A3-40fa-BADB-8A2652834100"
#define MBXP_EVENT_GUID "ABBC0F5C-8EA1-11D1-A000-C90629100000"

MODULE_ALIAS("wmi:"MBX_EVENT_GUID);
MODULE_ALIAS("wmi:"MBXP_EVENT_GUID);

static const struct key_entry huawei_wmi_keymap[] __initconst = {
		{ KE_KEY,    0x281, { KEY_BRIGHTNESSDOWN } },
		{ KE_KEY,    0x282, { KEY_BRIGHTNESSUP } },
		{ KE_KEY,    0x284, { KEY_MUTE } },
		{ KE_KEY,    0x285, { KEY_VOLUMEDOWN } },
		{ KE_KEY,    0x286, { KEY_VOLUMEUP } },
		{ KE_KEY,	 0x287, { KEY_MICMUTE } },
		{ KE_KEY,	 0x289, { KEY_WLAN } },
		// Huawei |M| button
		{ KE_KEY,	 0x28a, { KEY_PROG1 } },
		// Keyboard light
		{ KE_IGNORE, 0x293, { KEY_KBDILLUMTOGGLE } },
		{ KE_IGNORE, 0x294, { KEY_KBDILLUMUP } },
		{ KE_IGNORE, 0x295, { KEY_KBDILLUMUP } },
		{ KE_END,	 0 }
};

static char *event_guid;
static struct input_dev *inputdev;

int huawei_wmi_micmute_led_set(bool on)
{
	acpi_handle handle;
	char *method;
	union acpi_object args[3];
	struct acpi_object_list arg_list = {
		.pointer = args,
		.count = ARRAY_SIZE(args),
	};

	handle = ACPI_HANDLE(&inputdev->dev);
	args[0].type = args[1].type = args[2].type = ACPI_TYPE_INTEGER;
	args[1].integer.value = 0x04;

	if (acpi_has_method(handle, method = "\\_SB.PCI0.LPCB.EC0.SPIN")) {
		args[0].integer.value = 0;
		args[2].integer.value = on ? 1 : 0;
	} else if (acpi_has_method(handle, method = "\\_SB.PCI0.LPCB.EC0.WPIN")) {
		args[0].integer.value = 1;
		args[2].integer.value = on ? 0 : 1;
	} else {
		dev_err(&inputdev->dev, "Unable to find ACPI method\n");
		return -ENOSYS;
	}

	acpi_evaluate_object(handle, method, &arg_list, NULL);

	return 0;
}
EXPORT_SYMBOL_GPL(huawei_wmi_micmute_led_set);

static void huawei_wmi_process_key(struct input_dev *inputdev, int code)
{
	const struct key_entry *key;

	/*
	 * MBX uses code 0x80 to indicate a hotkey event.
	 * The actual key is fetched from the method WQ00
	 */
	if (code == 0x80) {
		acpi_status status;
		unsigned long long result;
		const char *method = "\\WMI0.WQ00";
		union acpi_object args[1];
		struct acpi_object_list arg_list = {
			.pointer = args,
			.count = ARRAY_SIZE(args),
		};

		args[0].type = ACPI_TYPE_INTEGER;
		args[0].integer.value = 0;

		status = acpi_evaluate_integer(ACPI_HANDLE(&inputdev->dev), (char *)method, &arg_list, &result);
		if (ACPI_FAILURE(status)) {
			dev_err(&inputdev->dev, "Unable to evaluate ACPI method %s\n", method);
			return;
		}

		code = result;
	}

	key = sparse_keymap_entry_from_scancode(inputdev, code);
	if (!key) {
		dev_info(&inputdev->dev, "Unknown key pressed, code: 0x%04x\n", code);
		return;
	}

	/*
	 * The MBXP handles backlight natively using ACPI,
	 * but not the MBX. If MBXP is being used, skip reporting event.
	 */
	if ((key->sw.code == KEY_BRIGHTNESSUP || key->sw.code == KEY_BRIGHTNESSDOWN)
			&& strcmp(event_guid, MBXP_EVENT_GUID) == 0)
		return;

	sparse_keymap_report_entry(inputdev, key, 1, true);
}

static void huawei_wmi_notify(u32 value, void *context)
{
	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	struct input_dev *inputdev = (struct input_dev*)context;

	status = wmi_get_event_data(value, &response);
	if (ACPI_FAILURE(status)) {
		dev_err(&inputdev->dev, "Bad event status 0x%x\n", status);
		return;
	}

	obj = (union acpi_object *)response.pointer;
	if (!obj)
		return;

	if (obj->type == ACPI_TYPE_INTEGER)
		huawei_wmi_process_key(inputdev, obj->integer.value);
	else
		dev_info(&inputdev->dev, "Unknown response received %d\n", obj->type);

	kfree(response.pointer);
}

static int huawei_wmi_input_init(void)
{
	acpi_status status;
	int err;

	inputdev = input_allocate_device();
	if (!inputdev)
		return -ENOMEM;

	inputdev->name = "Huawei WMI hotkeys";
	inputdev->phys = "wmi/input0";
	inputdev->id.bustype = BUS_HOST;

	err = sparse_keymap_setup(inputdev,
			huawei_wmi_keymap, NULL);
	if (err)
		goto err_free_dev;

	status = wmi_install_notify_handler(event_guid,
			huawei_wmi_notify,
			inputdev);
	if (ACPI_FAILURE(status)) {
		err = -EIO;
		goto err_free_dev;
	}

	err = input_register_device(inputdev);
	if (err)
		goto err_remove_notifier;

	return 0;

err_remove_notifier:
	wmi_remove_notify_handler(event_guid);
err_free_dev:
	input_free_device(inputdev);
	return err;
}

static void huawei_wmi_input_exit(void)
{
	wmi_remove_notify_handler(event_guid);
	input_unregister_device(inputdev);
}

static int __init huawei_wmi_init(void)
{
	if (wmi_has_guid(MBX_EVENT_GUID)) {
		event_guid = MBX_EVENT_GUID;
	} else if (wmi_has_guid(MBXP_EVENT_GUID)) {
		event_guid = MBXP_EVENT_GUID;
	} else {
		pr_warn("Compatible WMI GUID not found\n");
		return -ENODEV;
	}

	return huawei_wmi_input_init();
}

static void __exit huawei_wmi_exit(void)
{
	huawei_wmi_input_exit();
}

module_init(huawei_wmi_init);
module_exit(huawei_wmi_exit);
