# Huawei WMI Hotkeys Driver
This Linux driver enables the extra keys on Huawei laptops. So far, it has been tested on these models:
* Matebook X
* Matebook X Pro

## Installation
1. Make sure you have your kernel headers. In Fedora it would be:
```
# dnf install kernel-headers
```
Should be similar in other distributions.

2. Clone and install the module.

```
$ git clone https://github.com/aymanbagabas/Huawei-WMI
$ cd Huawei-WMI
$ make
$ sudo make install
```

## Keyboard
One of the keys, `micmute`, wouldn't work after inserting the module and that is due to an issue with X.Org. The solution would be to remap it to using `udev` hwdb tables.
Copy `99-Huawei.hwdb` to `/etc/udev/hwdb.d/` then update the hwdb tables:
```
sudo udevadm --debug hwdb --update; sudo udevadm trigger
```

## TODO
* Merge driver into upstream
* ~~Getting device LEDs to work~~ See `0003-ALSA-hda-add-support-for-Huawei-WMI-micmute-LED.patch`
* Support more devices
* ACPI driver?

## Contribution
Fork, modify, and create a pull request.

## Credits
* Thanks to Daniel Vogelbacher [@cytrinox](https://github.com/cytrinox) for testing the module on the MBX.
