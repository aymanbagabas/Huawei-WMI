# Huawei WMI laptop extras linux driver

**NOTE: Version v2.0 is the one in mainline kernel >= 5.0, this repository is used for
testing and development purposes. v3.3 has been merged in kernel 5.5**

This driver adds support for some of the missing features found on Huawei
laptops running linux. It implements Windows Management Instrumentation (WMI)
device mapping to kernel. Supported features are:

* Function hotkeys, implemented in v1.0
* Micmute LED, implemented in v2.0. Updated in v3.0 to work with newer laptops.
* Battery protection, implemented in v3.0. Updated in v3.3 to use battery charge API.
* Fn-lock, implemented v3.0.

Battery protection can accessed from either `/sys/class/power_supply/BAT0/charge_control_{start,end}_threshold` or `/sys/devices/platform/huawei-wmi/charge_control_thresholds`

Fn-lock can be accessed from `/sys/devices/platform/huawei-wmi/fn_lock_state`

This driver requires kernel >= 5.1. If you're on kernel <= 5.0, please refer to
tag [v1.0](https://github.com/aymanbagabas/Huawei-WMI/tree/v1.0) for kernel < 5.0 or tag [v3.2](https://github.com/aymanbagabas/Huawei-WMI/tree/v3.2) if you're running version 5.0.

Check out [matebook-applet](https://github.com/nekr0z/matebook-applet) for a GUI
to control Fn-lock and battery protection.

## Installation

Make sure you're using kernel >= 5.0.
You can get this driver from
[here](https://github.com/aymanbagabas/Huawei-WMI/releases) if you want to use
DKMS modules for easy installation.

### Use RPM package for Fedora

Install the RPM package provided [here](https://github.com/aymanbagabas/Huawei-WMI/releases).

### Use dkms tarball installation

Note: change `VER` to the desired module version.

1. Grab `huawei-wmi-VER-source-only.dkms.tar.gz` from [here](https://github.com/aymanbagabas/Huawei-WMI/releases)
2. Add dkms tarball and install module

```sh
sudo dkms ldtarball --archive=huawei-wmi-VER-source-only.dkms.tar.gz
# For autoinstallation
sudo dkms autoinstall -m huawei-wmi/VER
# For one-time installation
sudo dkms install huawei-wmi/VER
```
3. Reboot

### Build from source

1. Make sure you have your kernel headers. In Fedora that would be:

```sh
sudo dnf install kernel-headers kernel-devel
```

Should be similar in other distributions.
2. Clone and *update* / *install* the module.

```sh
git clone https://github.com/aymanbagabas/Huawei-WMI
cd Huawei-WMI
make
# To update use:
sudo cp huawei-wmi.ko /lib/modules/$(uname -r)/updates/
sudo depmod
# To install use:
sudo make install
reboot
```

This method overwrites the exsiting version of `huawei-wmi` that comes with
kernel 5.0. You have to redo it everytime the kernel gets updated.

## Keyboard

**NOTE: Ignore this if you're running `systemd-udev` > 240.**

One of the keys, `micmute`, wouldn't work after inserting the module and that is
due to an issue with X.Org. The solution would be to remap it to using `udev`
hwdb tables.
Copy `99-Huawei.hwdb` to `/etc/udev/hwdb.d/` then update the hwdb tables:

```sh
sudo udevadm --debug hwdb --update; sudo udevadm trigger
```

## TODO

* ~~Merge driver into upstream~~ Merged in Linux > 4.20. [Commit log](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/drivers/platform/x86/huawei-wmi.c)
* ~~Getting device LEDs to work~~ See
[this](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/sound/pci/hda/patch_realtek.c?id=e2744fd7097dd06b751b15395256ec7b7bb62124) and [this](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/sound/pci/hda/patch_realtek.c?id=0fbf21c3b36a9921467aa7525d2768b07f9f8fbb)
* Support more devices
* ACPI driver?

## Contribution

Fork, modify, and pull request.

## Credits

* Thanks to Daniel Vogelbacher [@cytrinox](https://github.com/cytrinox) and Jan
Baer [@janbaer](https://github.com/janbaer) for testing the module on the
Matebook X (2017).
* Big thanks to [@nekr0z](https://github.com/nekr0z) for testing this driver on his Matebook 13 (2019)
`WRT-WX9` and for his awesome project [matebook-applet](https://github.com/nekr0z/matebook-applet).
* Thanks to [@wasakakero](https://github.com/wasakakero) for testing this driver on the Matebook D 14-AMD `KPL-W0X`.
