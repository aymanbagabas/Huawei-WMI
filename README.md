# Huawei WMI Hotkeys Driver
This Linux driver enables the extra keys on Huawei laptops. Right now the driver only works for the **Matebook X Pro**, contributers are welcome.

## Installation
First, make sure that you install kernel headers. In Fedora it would be:
```
# dnf install kernel-headers
```
It should be similar in other distributions.

Then clone and install the module. Note you have to do that for every kernel update.

```
$ git clone https://github.com/aymanbagabas/Huawei-WMI
$ cd Huawei-WMI
$ make
$ sudo make install
```

## Keyboard remaps
Copy `99-Huawei-MBXP.hwdb` to `/etc/udev/hwdb.d/` to get the micmute to work and define missing keys within the atkbd driver.
Then update hwdb tables:
```
sudo udevadm --debug hwdb --update; sudo udevadm trigger
```

## TODO
* Merge driver into upstream
* ~~Getting device LEDs to work~~ See `0003-ALSA-hda-add-support-for-Huawei-WMI-MicMute-LED.patch`
* Support new devices
* ACPI driver?

## Contribution
Fork and create a pull request.
