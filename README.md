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

## TODO
* Merge driver into upstream
* ~~Getting device LEDs to work~~ See `huawei-wmi-micmute-led-and-mbxp-pins-fixup.patch`
* Support new devices
* ACPI driver?

## Contribution
Fork and create a pull request.
