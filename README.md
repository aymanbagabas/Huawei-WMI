# Huawei WMI laptop extras linux driver

[![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=7TMEEVMB4S4G8&currency_code=USD&source=url)
[![Donate](https://img.shields.io/badge/Donate-Buy%20Me%20A%20Coffee-green)](https://www.buymeacoffee.com/aymanbagabas)

## Contents

- [Platform profile](#platform-profile)
- [Keyboard backlight and fan sensors](#honor-keyboard-backlight-and-fan-sensors)
- [Installation](#installation)
- [Keyboard](#keyboard)
- [TODO](#todo)
- [Contribution](#contribution)
- [Credits](#credits)

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

## Platform profile

On supported HONOR laptops, the driver exposes the firmware profiles through
the Linux platform profile interface (power-profile-daemon's `low-power`, `balanced`, `performance` modes). The DMI allowlist covers ZQC-P, XWC-P,
FMB-P (including FMB-PM), BCC-N, DRA-XX, DRB-P, MRA-XXX, MRB-XXX, FRB-X,
GLO-GXXX, and FMI-XX. The available profiles depend on the DMI revision:

| Models | Linux profiles | Firmware `power_unlock` modes |
| --- | --- | --- |
| HONOR models without HUNTER | `balanced`, `performance` | `0`, `1` |
| Confirmed HUNTER revisions | `low-power`, `balanced`, `performance` | `0`, `1`, `3` |

HUNTER mode (`3`) is enabled only for HONOR DRA-XX board versions M1030/M1040
and DRB-P-PCB board versions M1020/M1100. Other revisions do not get HUNTER
support. On those confirmed revisions, `low-power` maps to firmware mode `0`,
`balanced` to mode `1`, and `performance` to HUNTER mode `3`. Other supported
models keep the firmware's two-profile mapping: `balanced` to mode `0` and
`performance` to mode `1`.

On Linux 6.9 and newer, you can find the `huawei-wmi` provider and select one of its
advertised profiles directly:

```sh
sh -c '
  PROFILE_DIR=$(
      for d in /sys/class/platform-profile/platform-profile-*; do
          [ "$(cat "$d/name")" = "huawei-wmi" ] && {
              echo "$d"
              break
          }
      done
  )

  cat "$PROFILE_DIR/choices"
  echo performance | sudo tee "$PROFILE_DIR/profile"
'
```

On Linux 5.11 through 6.8,
the driver uses the legacy system-wide interface instead:

```sh
cat /sys/firmware/acpi/platform_profile_choices
echo performance | sudo tee /sys/firmware/acpi/platform_profile
```

While running on battery, or when an attached power source is insufficient and
the battery is discharging, only the profile mapped to firmware mode `0` is
allowed: `low-power` on HUNTER revisions and `balanced` on other supported
models. Attempts to select another profile are rejected, and the driver returns
the firmware to mode `0` if it was already in another mode. AC-adapter and
battery-status changes trigger a firmware-state check and a platform-profile
notification. When the battery is not discharging, all profiles supported by
the model can be selected. If the firmware rejects HUNTER/performance with a
low-wattage adapter, the driver tries firmware mode `1` (`balanced`) instead
and returns an error for the rejected request; the reported platform profile
reflects the mode the firmware actually accepted.

`powerprofilesctl` controls the system-wide profile and may also adjust CPU
energy-performance preferences. Whether it changes this driver's profile
depends on the kernel interface and the installed power-profiles-daemon (PPD)
backend. PPD may still adjust CPU preferences even when the firmware rejects a
platform profile request, so `powerprofilesctl` may show `performance` while
the Huawei platform profile reports `balanced`. The raw firmware mode is
available through `/sys/devices/platform/huawei-wmi/power_unlock`; it follows
the same battery restriction on models with platform-profile support.

## HONOR keyboard backlight and fan sensors

On HONOR ZQC-P M1010 and FMB-P, the driver controls the keyboard backlight via
the EC `KBBL` field because the firmware WMI writes are no-ops. The LED class
brightness has three levels (`0` off, `1` low, `2` high); the additional
`/sys/devices/platform/huawei-wmi/kbdlight_mode` attribute selects `reactive`
(firmware timeout) or `steady` (latch the selected level).

On ZQC-P M1010, fan RPM is read directly from the measured EC tachometer words
at `0x2c-0x2d` and `0x2e-0x2f`. Other models continue to use the firmware WMI
`GFNS` method when available. This is read-only; fan speed control is not
provided.

The driver maps the HONOR performance key to `KEY_PROG1` and ignores EC
keyboard-backlight notifications (`0x2e5`/`0x2e6`). Cycling profiles in
response to `KEY_PROG1`, and the camera-key USB authorization action, are
userspace behavior provided separately by [Linux on the HONOR MagicBook](https://github.com/rs0x29a/Linux-on-HONOR-MagicBook-14-Pro-2026-AI_ZQC-P_M1010) repos's
`hotkey-actions` service.

This driver requires kernel >= 5.1. If you're on kernel <= 5.0, please refer to
tag [v1.0](https://github.com/aymanbagabas/Huawei-WMI/tree/v1.0) for kernel < 5.0 or tag [v3.2](https://github.com/aymanbagabas/Huawei-WMI/tree/v3.2) if you're running version 5.0.

Check out [matebook-applet](https://github.com/nekr0z/matebook-applet) for a GUI
to control Fn-lock and battery protection.

## Installation

Make sure you're using kernel >= 5.1.
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
