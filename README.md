# cctl

Lightweight CLI tool for Clevo P15 laptop performance management. Sets CPU power profiles (governor, turbo boost, EPP, RAPL limits), controls fan speeds via direct EC port I/O, adjusts keyboard backlight color/brightness, toggles webcam/microphone, and sets display refresh rate. No GUI, no TUI, no dependencies beyond libc.

## Build

```bash
gcc -o cctl cctl.c -Os -s
```

## ASCII art (generated with)

```bash
curl "https://asciified.thelicato.io/api/v2/ascii?text=cctl&font=standard"
```

## Usage

```
            _                           _             _ 
   ___ ___ | | ___  _ __ ___ ___  _ __ | |_ _ __ ___ | |
  / __/ _ \| |/ _ \| '__/ __/ _ \| '_ \| __| '__/ _ \| |
 | (_| (_) | | (_) | | | (_| (_) | | | | |_| | | (_) | |
  \___\___/|_|\___/|_|  \___\___/|_| |_|\__|_|  \___/|_|
```

```bash
sudo ./cctl set <profile>       # max | cpuperf | balanced | powersave | eco
sudo ./cctl setR <profile>      # same but with RAPL power limits
sudo ./cctl fan <mode> [value]  # auto | max | silent | cpu <pct> | gpu <pct>
sudo ./cctl turbo <on|off>      # standalone turbo override
sudo ./cctl gov <governor>      # standalone governor override (powersave|performance)
sudo ./cctl epp <value>         # standalone EPP override (performance|balance_performance|balance_power|power)
sudo ./cctl kbc R G B           # keyboard RGB (0-255 per channel)
sudo ./cctl kbcp <name|hex>   # keyboard color preset (blue, red, cyan, etc.) or raw hex code (#RRGGBB)
sudo ./cctl kbb <pct>           # keyboard brightness (0-100%)
sudo ./cctl rapl <pl1> <pl2>    # set RAPL power limits in watts
sudo ./cctl mic [on|off]        # toggle/set microphone (no root needed)
sudo ./cctl rr [rate]           # list/set display refresh rate (no root needed)
sudo ./cctl webcam [on|off]     # toggle/set webcam (needs root)
sudo ./cctl nvidia <on|off|status> # Nvidia GPU module switcher & telemetry (on/off needs root)
sudo ./cctl status              # show current settings, CPU temp, fans (no root needed)
sudo ./cctl monitor             # live CPU freq, temp, power, fans, usage monitor (needs root)
```

Profiles set all CPU/GPU parameters at once. `setR` variant also applies RAPL power limits (package-0 only — writing to psys/sub-zones causes EC conflict and 0.4GHz throttle on Clevo). Turbo and governor overrides apply immediately but get replaced by the next profile change. The tool dynamically groups hybrid P/E-cores based on CPU topology/max frequency and pins itself to E-cores to minimize game performance impact.

## Profiles

| Profile    | GPU  | Turbo | Governor  | EPP                | RAPL (setR)  |
|------------|------|-------|-----------|--------------------|---------------|
| max        | 2    | ON    | perf      | performance        | 45W / 90W    |
| cpuperf    | 3    | ON    | perf      | performance        | 70W          |
| balanced   | 3    | ON    | powersave | balance_performance| 35W / 40W    |
| powersave  | 1    | OFF   | powersave | balance_power      | —            |
| eco        | 0    | OFF   | powersave | power              | 9W / 10W     |

## Fan modes

| Mode   | Description                  |
|--------|------------------------------|
| auto   | Both fans to EC-controlled   |
| max    | Both fans to max speed       |
| silent | Both fans to silent mode     |
| cpu    | CPU fan to 21-100%           |
| gpu    | GPU fan to 21-100%           |

> [!NOTE]
> **EC Byte-Order Quirk:** The Clevo EC interprets the byte order of the two-byte fan speed commands (`cmd 0x99`) contextually. When restoring `auto` mode, cmd `0x99` expects `{ 0xFF, fan_idx }`. When setting a specific duty cycle (`fan_set_duty`, `fan_max_all`, `fan_silent_all`), cmd `0x99` expects `{ fan_idx, duty_value }`. Do not attempt to "normalize" these argument layouts, as this contextual byte-swapping is verified correct on the Clevo P15 hardware.
>
> **GPU Duty Persistence:** The GPU fan duty you set with `fan gpu <pct>` is saved to `/tmp/.cctl-gpu-duty` (tmpfs / RAM). This is necessary because the Clevo EC doesn't expose a readable GPU duty register — so on the next `cctl status` invocation, the tool reads back whatever it last wrote. The file is lost on reboot, which is fine since the EC resets to its own defaults at power-on anyway.


## Keyboard presets

blue, chocolate, coral, cyan, gold, gray, green, indigo, lime, magenta, maroon, navy, off, olive, orange, pink, purple, red, salmon, silver, teal, turquoise, violet, white, yellow

---

## legacygpu (kernel module)

One-shot ACPI _DSM GPU profile setter. Sets Clevo performance profile via the EC's `_DSM` interface. No dependency on tuxedo_io.

### Build

```bash
make        # compiles legacygpu.ko for running kernel
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

## Additional Scripts

### `toggle-nvidia`

Standalone NVIDIA GPU module switcher with colored output, confirmation prompts, and full telemetry display. An independent alternative to `cctl nvidia` — no compilation needed.

```bash
sudo ./toggle-nvidia off           # blacklist NVIDIA (rebuilds initramfs)
sudo ./toggle-nvidia on            # unblacklist NVIDIA (rebuilds initramfs)
sudo ./toggle-nvidia status        # show boot config, module state, GPU telemetry
sudo ./toggle-nvidia off --force   # skip confirmation prompts
```

### `nvidia.sh`

Minimal helper to load/unload NVIDIA kernel modules at runtime (no initramfs changes). Useful for quick testing:

```bash
sudo ./nvidia.sh off    # unload: nvidia_drm → nvidia_modeset → nvidia_uvm → nvidia
sudo ./nvidia.sh on     # load:   nvidia → nvidia_uvm → nvidia_modeset → nvidia_drm
sudo ./nvidia.sh status # show loaded state of each module
```

### `setup.sh`

Auto-compiles `legacygpu.ko` when the kernel version changes. Runs silently if the module is already up to date. Called automatically — no user action needed.

---

## drivers/

Full [tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers) kernel module source tree (keyboard backlight, Clevo WMI, Tuxedo IO interface). Not required for `cctl` to work — the tool falls back to direct EC I/O and sysfs if the hardware drivers are absent. Build with `make -C drivers/`.

---

## Manual RAPL (for reference)

```bash
echo 35000000 | sudo tee /sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw
echo 41000000 | sudo tee /sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw
```

---

## Architecture & Safety Improvements

Following a comprehensive code audit, several critical optimizations and architectural rewrites were implemented:
- **CLI Command Dispatch Table**: Replaced the large nested `if-else` routing sequence in `main()` with a clean, modular `struct command` dispatch table mapping command names to safe handler function pointers.
- **Security & Input Sanitization**: Swapped direct shell spawns (`system()`) with secure child processes (`fork`/`execvp`/`waitpid`) for display actions, and introduced a validated `safe_atoi()` parser to verify arguments and prevent bounds overflows.
- **Dynamic Hybrid core classification**: The system now dynamically detects and separates hybrid P-cores/E-cores based on core frequency topology rather than relying on hardcoded core counts (e.g. CPU 12 split).
- **Dynamic Core Pinning**: Thread affinity routines query the core classifier and pin the program's background tasks specifically to real system E-cores, preventing performance interference on P-cores.
- **Descriptor & IOCTL Handle Caching**: The `monitor` loops now cache sysfs and `tuxedo_io` handles once before starting, reading updates using fast `pread()` offset reads to completely eliminate repetitive open/close syscall overhead.
- **Clean Compilation**: Re-dimensioned path buffers to resolve potential string truncation warnings, allowing compilation with `-Wall -Wextra` and zero warnings.
- **Robustness & Cleanups (July 2026 Audit)**:
  - Silenced `amixer` stdout output leakage when modifying microphone capture states.
  - Hardened the `nvidia-smi` parser using specific CSV group parsing in `sscanf()` to handle process names containing spaces.
  - Added safety checks in `webcam_toggle()` to handle driver or detection failures gracefully.
  - Eliminated shell invocation overheads by swapping `system("command -v ...")` with direct `access()` checks for initramfs tools (`mkinitcpio`, `dracut`, `update-initramfs`) and `nvidia-smi`.
  - Fixed terminal rendering glitches in the CPU monitor by performing explicit cursor and terminal buffer clears.
  - Removed dead clamps from `fan_set_duty()` and simplified RAPL watts resolution using pre-existing sysfs helpers.
