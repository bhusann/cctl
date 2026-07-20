# legacymethod/ — Legacy tools and supplementary components

## ASCII art (generated with)

```bash
curl "https://asciified.thelicato.io/api/v2/ascii?text=cctl&font=standard"
```

---

## `toggle-nvidia`

Standalone NVIDIA GPU module switcher with colored output, confirmation prompts, and full telemetry display. An independent alternative to `cctl nvidia` — no compilation needed.

```bash
sudo ./toggle-nvidia off           # blacklist NVIDIA (rebuilds initramfs)
sudo ./toggle-nvidia on            # unblacklist NVIDIA (rebuilds initramfs)
sudo ./toggle-nvidia status        # show boot config, module state, GPU telemetry
sudo ./toggle-nvidia off --force   # skip confirmation prompts
```

---

## `nvidia.sh`

Minimal helper to load/unload NVIDIA kernel modules at runtime (no initramfs changes). Useful for quick testing:

```bash
sudo ./nvidia.sh off    # unload: nvidia_drm → nvidia_modeset → nvidia_uvm → nvidia
sudo ./nvidia.sh on     # load:   nvidia → nvidia_uvm → nvidia_modeset → nvidia_drm
sudo ./nvidia.sh status # show loaded state of each module
```

---

## `setup.sh`

Auto-compiles `legacygpu.ko` when the kernel version changes. Runs silently if the module is already up to date. Called automatically — no user action needed.

---

## legacygpu (kernel module)

One-shot ACPI _DSM GPU profile setter. Sets Clevo performance profile via the EC's `_DSM` interface. No dependency on tuxedo_io.

### Build

```bash
make        # from project root — compiles legacygpu.ko for running kernel
```

Requires `kernel-headers` for the running kernel.

### Usage

```bash
sudo insmod legacygpu.ko profile=2   # set profile, then rmmod
sudo rmmod legacygpu                 # must remove before changing profile

# Profiles: 0=quiet  1=standard  2=performance  3=turbo
```

Check dmesg for result: `dmesg | tail -3`

The module does one ACPI call and stays loaded — `rmmod` cleanly unloads it. Must rmmod before insmod with a different profile value.

---

## drivers/

Full [tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers) kernel module source tree (keyboard backlight, Clevo WMI, Tuxedo IO interface). Not required for `cctl` to work — the tool falls back to direct EC I/O and sysfs if the hardware drivers are absent. Build with `make -C drivers/`.

---

## Manual RAPL (for reference)

```bash
echo 35000000 | sudo tee /sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw
echo 41000000 | sudo tee /sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw
```
