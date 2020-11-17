---
name: Bug report
about: Create a report to help us improve
title: ''
labels: ''
assignees: ''

---

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:
1. Go to '...'
2. Click on '....'
3. Scroll down to '....'
4. See error

**Expected behavior**
A clear and concise description of what you expected to happen.

**Screenshots**
If applicable, add screenshots to help explain your problem.

**(please complete the following information):**
 - acpidump `sudo acpidump > acpidump.out`
 - dmidecode `dmidecode > dmidecode`
 - Kernel [e.g. 5.2.9-200.fc30.x86_64] `uname -a`
 - Distro [e.g. Fedora 30] `
 - Loaded WMI modules `lsmod | grep wmi`
 - Dmesg log `dmesg > dmesg.txt`
 - ALSA log if applicable `alsa-info.sh`
 - `evtest` if it's keyboard related. Run `sudo evtest` then choose "Huawei WMI hotkeys", test all hotkeys and submit output.
 - `acpi_listen` if it's keyboard related. Run `sudo acpi_listen` and test all hotkeys. Submit output.

**Additional context**
If any
