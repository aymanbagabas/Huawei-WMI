**NOTE: Since this driver, v2.0, made it to mainline kernel, this repository will be used for testing and development purposes.**

# Huawei WMI laptop extras linux driver
This driver adds support for some of the missing features found on Huawei laptops running linux. It implements Windows Management Instrumentation (WMI) device mapping to kernel. Supported features are:
* Function hotkeys, implemented in v1.0
* Micmute LED, implemented in v2.0 & v3.0 (all models should work)
* Battery protection, implemented in v3.0
* Fn-lock, implemented v3.0

Battery protection and Fn-lock can be accessed from `/sys/devices/platform/huawei-wmi/{charge_thresholds,fn_lock_state}`

This driver requires kernel >= 5.0. If you're on kernel < 5.0, please refer to tag [v1.0](https://github.com/aymanbagabas/Huawei-WMI/tree/v1.0).

Check out [matebook-applet](https://github.com/nekr0z/matebook-applet) for a GUI
to control Fn-look and battery protection.

## Installation
Make sure you're using kernel >= 5.0.
You can get this driver from [here](https://github.com/aymanbagabas/Huawei-WMI/releases) if you want to use DKMS modules for easy installation.

Or build it from source.

1. Make sure you have your kernel headers. In Fedora that would be:

```
$ sudo dnf install kernel-headers kernel-devel
```
Should be similar in other distributions.

2. Clone and *update*/*install* the module.

```
$ git clone https://github.com/aymanbagabas/Huawei-WMI
$ cd Huawei-WMI
$ make
$ # To update use:
$ sudo cp huawei-wmi.ko /lib/modules/$(uname -r)/updates/
$ sudo depmod
$ # To install use:
$ sudo make install
$ reboot
```

This method overwrites the exsiting version of `huawei-wmi` that comes with kernel 5.0. You have to redo it everytime the kernel gets updated.

## Keyboard
**NOTE: Ignore this if you're running `systemd-udev` > 240.**

One of the keys, `micmute`, wouldn't work after inserting the module and that is due to an issue with X.Org. The solution would be to remap it to using `udev` hwdb tables.
Copy `99-Huawei.hwdb` to `/etc/udev/hwdb.d/` then update the hwdb tables:
```
sudo udevadm --debug hwdb --update; sudo udevadm trigger
```

## TODO
* ~~Merge driver into upstream~~ Merged in Linux > 4.20. [Commit log](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/drivers/platform/x86/huawei-wmi.c?id=bf4fb28c6e74495de9e1e4ad359cd2272ac12c53)
* ~~Getting device LEDs to work~~ See `0003-ALSA-hda-add-support-for-Huawei-WMI-micmute-LED.patch`
* Support more devices
* ACPI driver?

## Contribution
Fork, modify, and create a pull request.

## Credits
* Thanks to Daniel Vogelbacher [@cytrinox](https://github.com/cytrinox) and Jan Baer [@janbaer](https://github.com/janbaer) for testing the module on the Matebook X (2017).
* Big thanks to @nekr0z for testing this driver on his Matebook 13 (2019) `WRT-WX9` and for his awesome project [matebook-applet](https://github.com/nekr0z/matebook-applet).
* Thanks to @wasakakero for testing this driver on the Matebook D 14-AMD `KPL-W0X`.
