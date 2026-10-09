# Manual Uninstallation

Remove the TUXEDO/Clevo drivers first, then remove cctl. The driver installer
can be run without cctl and handles module unloading, DKMS cleanup, stale
module files, the modprobe configuration, and `depmod` updates.

## Remove the Drivers

Clone the mirror and run its uninstall script:

```bash
git clone https://github.com/bhusann/tuxedo-drivers-cctl-mirror
cd tuxedo-drivers-cctl-mirror
sudo ./drivers/driverinstall.sh --uninstall
```

If you need to remove the drivers manually, use the same module order as the
installer, then clean their DKMS registration and files:

```bash
sudo modprobe -r clevo_acpi
sudo modprobe -r tuxedo_io
sudo modprobe -r tuxedo_keyboard

sudo dkms remove tuxedo-drivers/1.0 --all
sudo rm -rf /var/lib/dkms/tuxedo-drivers
sudo rm -rf /usr/src/tuxedo-drivers-1.0
sudo rm -f /etc/modprobe.d/tuxedo_keyboard.conf
```

Remove any remaining copies of these modules from kernel update directories,
then rebuild the module dependency cache:

```bash
sudo find /lib/modules -path '*/updates/*' -type f \
  \( -name 'clevo_acpi.ko*' -o -name 'tuxedo_keyboard.ko*' -o -name 'tuxedo_io.ko*' \) -delete
sudo find /lib/modules -path '*/updates/*' -type d -empty -delete
sudo depmod -a
```

## Remove cctl

Remove the installed binary, sudo rule and its backup or temporary file, your
optional custom profile, and cctl's saved settings:

```bash
sudo rm -f /usr/local/bin/cctl
sudo rm -f /etc/sudoers.d/cctl /etc/sudoers.d/cctl.bak /etc/sudoers.d/.cctl.tmp
sudo rm -f /var/lib/cctl/profiles.conf /var/lib/cctl/settings
sudo rm -f /etc/cctl/profiles.conf   # only if left from an older cctl version
sudo rmdir /etc/cctl                 # removes it only if empty
```

The settings file stores cctl-specific preferences such as streamed terminal
output. These commands leave snapshots and the cached driver archive in place.

Optionally remove cctl's persistent data:

```bash
sudo rm -rf /var/lib/cctl
```

This removes saved snapshots **and** the cached `drivers.tar.gz` archive.
