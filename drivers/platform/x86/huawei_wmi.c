/*
 *  Huawei WMI Hotkeys Driver
 *
 *  Copyright (C) 2018          Ayman Bagabas <ayman.bagabas@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/acpi.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/miscdevice.h>

MODULE_AUTHOR("Ayman Bagabas <ayman.bagabas@gmail.com>");
MODULE_DESCRIPTION("Huawei WMI Hotkeys Driver");
MODULE_LICENSE("GPL");

#define DEVICE_NAME "huawei"
#define MODULE_NAME DEVICE_NAME"_wmi"

/*
 * Huawei WMI Events GUIDs
 */
#define EVENT_GUID "ABBC0F5C-8EA1-11D1-A000-C90629100000"

MODULE_ALIAS("wmi:"EVENT_GUID);

static const struct key_entry huawei_wmi_keymap[] __initconst = {
        { KE_IGNORE, 0x281, { KEY_BRIGHTNESSDOWN } },
        { KE_IGNORE, 0x282, { KEY_BRIGHTNESSUP } },
        { KE_IGNORE, 0x283, { KEY_KBDILLUMTOGGLE } },
        { KE_IGNORE, 0x284, { KEY_MUTE } },
        { KE_IGNORE, 0x285, { KEY_VOLUMEDOWN } },
        { KE_IGNORE, 0x286, { KEY_VOLUMEUP } },
        { KE_KEY,    0x287, { KEY_MICMUTE } },
        { KE_KEY,    0x289, { KEY_WLAN } },
        { KE_KEY,    0x28a, { KEY_PROG1 } },          // Huawei |M| button
        { KE_END,    0 }
};

struct huawei_wmi_device {
    struct input_dev *inputdev;
};
static struct huawei_wmi_device *wmi_device;

static void huawei_wmi_process_key(struct input_dev *input_dev, int code)
{
    const struct key_entry *key;
    
    key = sparse_keymap_entry_from_scancode(input_dev, code);

    if (!key) {
        pr_info("%s: Unknown key pressed, code: 0x%04x\n", MODULE_NAME, code);
        return;
    }

    sparse_keymap_report_entry(input_dev, key, 1, true);
}

static void huawei_wmi_notify(u32 value, void *context)
{
    struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
    union acpi_object *obj;
    acpi_status status;

    status = wmi_get_event_data(value, &response);
    if (ACPI_FAILURE(status)) {
        pr_err("%s: Bad event status 0x%x\n", MODULE_NAME, status);
        return;
    }

    obj = (union acpi_object *)response.pointer;

    if (obj && obj->type == ACPI_TYPE_INTEGER) {
        huawei_wmi_process_key(wmi_device->inputdev, obj->integer.value);
    }

    kfree(response.pointer);
}

static int huawei_input_init(void)
{
    acpi_status status;
    int err;

    wmi_device->inputdev = input_allocate_device();
    if (!wmi_device->inputdev)
        return -ENOMEM;

    wmi_device->inputdev->name = "Huawei WMI Hotkeys Driver";
    wmi_device->inputdev->phys = "wmi/input0";
    wmi_device->inputdev->id.bustype = BUS_HOST;

    err = sparse_keymap_setup(wmi_device->inputdev, huawei_wmi_keymap, NULL);
    if (err)
        goto err_free_dev;

    status = wmi_install_notify_handler(EVENT_GUID,
                                        huawei_wmi_notify,
                                        NULL);

    if (ACPI_FAILURE(status)) {
        err = -EIO;
        goto err_free_dev;
    }

    err = input_register_device(wmi_device->inputdev);
    if (err)
        goto err_remove_notifier;

    return 0;


err_remove_notifier:
    wmi_remove_notify_handler(EVENT_GUID);
err_free_dev:
    input_free_device(wmi_device->inputdev);
    return err;
}

static void huawei_input_exit(void)
{
    wmi_remove_notify_handler(EVENT_GUID);
    input_unregister_device(wmi_device->inputdev);
}

static int __init huawei_wmi_setup(void)
{
    int err;

    wmi_device = kmalloc(sizeof(struct huawei_wmi_device), GFP_KERNEL);
    if (!wmi_device)
        return -ENOMEM;

    err = huawei_input_init();
    if (err)
        goto err_input;

    return 0;

err_input:
    return err;
}

static void huawei_wmi_destroy(void)
{
    huawei_input_exit();
}

static int __init huawei_wmi_init(void)
{
    int err;

    if (!wmi_has_guid(EVENT_GUID)) {
        pr_warning("%s: No known WMI Events found\n", MODULE_NAME);
        return -ENODEV;
    }

    err = huawei_wmi_setup();
    if (err) {
        pr_err("%s: Failed to setup device\n", MODULE_NAME);
        return err;
    }

    return 0;
}

static void __exit huawei_wmi_exit(void)
{
    huawei_wmi_destroy();
    kfree(wmi_device);
    pr_debug("%s: Driver unloaded successfully\n", MODULE_NAME);
}

module_init(huawei_wmi_init);
module_exit(huawei_wmi_exit);
