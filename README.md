# CCTL — ColorControl

Linux CLI alternative to the Windows-only Colorful Laptop Control Center. Fast, single-binary tool for Colorful Evol P15 laptops (Clevo/TUXEDO chassis) — controls power profiles, fans, keyboard backlight, display, battery, and NVIDIA GPU. Pure C, no GUI, no daemon.

## Hardware Compatibility

Specifically designed for the **Colorful Evol P15** series (Clevo/TUXEDO chassis).

### Tested Hardware
| Laptop | CPU | GPU | Keyboard |
|---|---|---|---|
| Colorful Evol P15 | Intel Core i7-13620H | RTX 4060 Mobile 100W | Single-zone RGB |
| Colorful Evol P15 | Intel Core i5-12500H | RTX 4050 Mobile 100W | Single-zone RGB |

### Supported Configurations
* **Series:** Colorful Evol P15 series
* **GPUs:** NVIDIA GeForce RTX 4060 Mobile or RTX 4050 Mobile (both 100W and 140W variants supported)
* **CPUs:** Intel Core i7-13620H, i5-12500H, i7-12650H, and i5-12450H
* **Keyboard:** Single-zone RGB keyboard (tested)

> **Note:** Built specifically for Colorful Evol P15 series laptops. Other laptop models or other Clevo/TUXEDO variants may have different EC register layouts, fan byte orders, or GPU profile slots — use at your own risk.

---

## Quick Start

Pre-built binaries and driver sources are available on the [Releases](https://github.com/bhusann/cctl/releases) page.

```bash
# 1. Install cctl system-wide (adds to PATH, sets up passwordless sudo + shell alias)
sudo ./cctl install

# 2. Install kernel drivers via DKMS (required for fans, keyboard backlight, battery)
sudo ./cctl drivers-install

# 3. Restart terminal, then use:
cctl set balanced          # apply a power profile
cctl fan auto              # set fans to automatic
cctl status                # view all current settings
```

---

## Power Profiles

`set <profile>` applies a preset (changes turbo, governor, EPP and CPU & GPU TDP according to laptop EC defaults).
`setR <profile>` applies a preset + custom pre-configured CPU TDP change (changes turbo, governor, EPP and only GPU TDP according to laptop EC defaults, CPU TDP according to SetR table values using RAPL).

```
Profile     Turbo  Governor     EPP                EC default CPU & GPU TDP (set)  RAPL CPU TDP overide (setR only)
─────────── ────── ──────────── ────────────────── ────────────────────────────── ────────────────────────────────
max         ON     performance  performance        90/115W + GPU 100W              PL1 45 / PL2 90W
cpuperf     ON     performance  performance        45/115W + GPU 70W               PL2 70W
balanced    ON     powersave    balance_performance 45/115W + GPU 70W              PL1 35 / PL2 40W
powersave   OFF    powersave    balance_power      15/30W  + GPU 70W               (no RAPL change)
eco         OFF    powersave    power              15/30W  + GPU 70W               PL1 9 / PL2 10W
```

> **`set max` vs `setR max`**: Plain `set max` leaves RAPL untouched, running at OEM platform limits (PL1 90W / PL2 115W CPU, 100W GPU). `setR max` caps sustained CPU draw to 45W (burst to 90W) to leave thermal headroom for the GPU.

---

## Commands

### Keyboard Backlight
```bash
cctl kbc <color>                # Set keyboard color: R G B (0-255), #hex, or preset name
cctl kbb <pct>                  # Brightness (0-100%)
cctl fn lock|unlock             # Fn Lock toggle (Fn key behavior)
```

Presets: `blue` `chocolate` `coral` `cyan` `gold` `gray` `green` `indigo` `lime` `magenta` `maroon` `navy` `off` `olive` `orange` `pink` `purple` `red` `salmon` `silver` `teal` `turquoise` `violet` `white` `yellow`

### Fan Control
```bash
cctl fan auto|max|silent        # Both fans: EC automatic / 100% / quiet
cctl fan <pct>                  # Set both fans to duty cycle (21-100%)
cctl fan cpu|gpu <pct>          # Set individual fan duty
```

### Privacy
```bash
cctl webcam [on|off]            # Toggle or set webcam
cctl mic [on|off]               # Toggle or set internal microphone (laptop mic only, needs alsa/amixer)
```

### Battery
```bash
cctl bat                        # Show current thresholds and battery health
cctl bat <start> <stop>         # Set charge thresholds (custom, e.g. 40 80)
cctl bat max                    # Standard mode (change to max - 100%)
```

### Info
```bash
cctl status                     # Print all current settings
cctl monitor                    # Live CPU/power/fan/memory monitor (color-coded)
```

### NVIDIA GPU
```bash
# Available in all builds:
cctl nvidia power [on|off]              # Hardware D0/D3cold control; no arg shows state
cctl nvidia clock <min,max> | reset     # Lock/unlock GPU clocks (auto persistence)
cctl nvidia memclock <min,max> | reset  # Lock/unlock memory clocks (auto persistence)

# Requires make cctl-nvidia:
cctl nvidia <on|off>                    # Persistent boot toggle (blacklist + initramfs)
cctl nvidia load                        # Load compute modules (nvidia, nvidia_uvm)
cctl nvidia loadgame                    # Load all modules (+ modeset, drm)
cctl nvidia unload                      # Unload all modules + power off (D3cold)
cctl nvidia status                      # Telemetry, loaded modules, active GPU PIDs
```

> ⚠️ **`nvidia off`** permanently blacklists the driver and rebuilds initramfs. Use `nvidia unload` for session-only GPU power off.

### Display
> **Note:** Display commands rely on `xrandr` and are only supported on native **X11** sessions. They are **not** supported under Wayland or XWayland.
>
> Displays only support their specific predefined hardware/EDID refresh rates — arbitrary in-between refresh rates cannot be set. Run `cctl rr` without arguments to list all available/supported refresh rates for your screen, and choose only from those listed.

```bash
cctl rr                         # List all supported refresh rates
cctl rr [1|2|<rate>]            # Set refresh rate (1=highest, 2=lowest, or explicit value like 60 or 144)
cctl scale <factor|WxH|off>    # GPU scaling (0.75, 1920x1080, or off to reset)
```

### Profile Individual Overrides
```bash
cctl turbo on|off               # Toggle Intel turbo boost
cctl gov powersave|performance  # CPU scaling governor
cctl epp <preference>           # performance, balance_performance, balance_power, power
cctl rapl <pl1> <pl2>           # Set PL1/PL2 in watts (use 'skip' to omit one)
```

---

## Build

```bash
make                # Standard build (profiles, fans, display, battery, nvidia clock/power)
make cctl-nvidia    # Full build with NVIDIA module management (on/off/load/unload/status)
make drivers        # Install kernel drivers via DKMS (runs driverinstall.sh --install)
make test-drivers   # Compile kernel drivers locally in-tree for testing (no install)
```

---

## Kernel Drivers

The Clevo/TUXEDO driver stack (`clevo_acpi`, `tuxedo_keyboard`, `tuxedo_io`) is required for fan control, keyboard backlight, battery thresholds, and EC GPU profile slots. The installer handles DKMS registration, build, and modprobe config.

```bash
sudo ./cctl drivers-install                  # Interactive: detects state, installs/uninstalls
sudo ./drivers/driverinstall.sh --install    # Direct install
sudo ./drivers/driverinstall.sh --uninstall  # Direct uninstall
./drivers/driverinstall.sh --status          # Check state (no root needed)
```

### Supported Distros

Auto-installs `dkms` + kernel headers if missing, with confirmation prompt:

| Package Manager | Distros | Tested |
|---|---|---|
| `pacman` | Arch, Manjaro, EndeavourOS, CachyOS, Garuda | ✅ Arch, CachyOS |
| `apt` | Debian, Ubuntu, Mint, Pop!_OS, Zorin | ✅ Debian |
| `dnf` | Fedora, RHEL, Rocky, Alma | — |
| `zypper` | openSUSE Tumbleweed/Leap | — |
| `xbps` | Void Linux | — |
| `emerge` | Gentoo | — |
| `eopkg` | Solus | — |

On immutable distros (Bazzite, Silverblue, etc.) the installer explains that DKMS won't persist across image updates and directs you to your distro's documentation. On unrecognized distros, it points to the driver source at `drivers/` for manual installation.

---

## Hardware Quirks & Developer Notes

- **RAPL 0.4 GHz Throttle** — Only package-0 (`intel-rapl:0`) is safe to write. Touching sub-zones (`intel-rapl:0:X`) or platform `psys` triggers an EC conflict that hard-throttles the CPU to 400 MHz.

- **EC Fan Byte Order** — Clevo's EC expects reversed byte order depending on command context. Auto-restore uses `{0xFF, fan_idx}`, duty cycle uses `{fan_idx, duty}`.

- **GPU Fan Duty Register** — ACPI `FANINFO1` byte 2 is stuck at ~15% on this model. `cctl` reads GPU fan duty from `FANINFO2` (`0x64`, byte 0) for accurate readings.

- **Keyboard Backlight Type Override (`force_backlight_type=6`)** — Why the driver installer forces type 6:
  - In `drivers/src/clevo_leds.h:clevo_leds_init()`:
    1. The driver calls `clevo_evaluate_method2(0x0D, 0)` to read the ACPI `_DSM` buffer. `buffer[0x0f]` holds the keyboard type ID. Known IDs include: `0x00` (NONE), `0x01` (FIXED), `0x02` (3ZONE), `0x06` (1ZONE), `0xF3` (PERKEY).
    2. The Colorful Evol P15 EC returns `0x26`. Because this ID is unknown to the driver, probe switches fail to match, registering no `led_classdev` — resulting in no `/sys/class/leds/*::kbd_backlight` and leaving brightness/color ioctls non-functional.
    3. `0x26` (`0b00100110`) vs `0x06` (`0b00000110`): the low nibble is identical, while the high bits likely signify a newer revision or flag. The wire protocol (`0x67` + `0xF4000000 | brightness`, zone color) remains standard single-zone RGB.
    4. Passing module option `force_backlight_type=6` (`tuxedo_keyboard`) bypasses ACPI buffer evaluation directly:
       ```c
       if (force_backlight_type >= 0) type = force; else { /* SPECS retry x3 -> 0x52/0x7A fallback */ }
       ```
    5. Upstream driver fix would be handling `case 0x26: type = 1ZONE;` or matching `(type & 0x0F) == 0x06`. Until upstreamed, forcing type 6 via modprobe is required.
  - **Customizing your keyboard type:**
    If you have a different variant and need to force another keyboard backlight mode, edit `/etc/modprobe.d/tuxedo_keyboard.conf`:
    ```
    options tuxedo_keyboard force_backlight_type=<value>
    ```
    Known values:
    * `1` — FIXED white backlight (`0x01`)
    * `2` — 3-zone RGB (`0x02`)
    * `6` — 1-zone RGB (`0x06`) *(default configured by this project)*
    * `243` — Per-key RGB (`0xF3`)

  - **Sysfs LED Exposure (`/sys/class/leds`):**
    All keyboard backlight controls registered by the driver are exposed under `/sys/class/leds`:
    * For **1-zone RGB (`force_backlight_type=6`)**: A single device entry is created:
      ```
      /sys/class/leds/rgb:kbd_backlight/
      ```
      (Contains controls like `brightness`, `multi_intensity`, `color`, etc.)
    * For **3-zone RGB (`force_backlight_type=2`)**: The kernel driver registers 3 separate LED device entries under `/sys/class/leds` corresponding to each keyboard zone (e.g., `rgb:kbd_backlight`, `rgb:kbd_backlight_1`, `rgb:kbd_backlight_2` or separate zone descriptors), allowing each zone's color and brightness to be tuned individually via sysfs.
    * For **Fixed White (`force_backlight_type=1`)**: A `white:kbd_backlight` directory is exposed for brightness level control.

    > **Note:** `cctl`'s command implementation currently only supports **1-zone RGB** (using type 6). If your hardware uses 3-zone RGB or per-key RGB, you can manage the zones directly through their respective `/sys/class/leds` folders or adapt `cctl`'s source code (`cctl.c`) to control individual zones.

- **Xorg Holding GPU on Hotplug** — When using `nvidia loadgame` under Xorg, it may grab the hotplugged card, preventing `nvidia unload`. Fix by adding to `/etc/X11/xorg.conf.d/10-no-gpu-hotplug.conf`:
  ```
  Section "ServerFlags"
      Option "AutoAddGPU" "false"
  EndSection
  ```

---

## License

`cctl` own code is licensed under the MIT License — see [LICENSE](LICENSE).

The driver code under `drivers/` is **not** MIT. It is derived from the TUXEDO Linux driver project and remains under **GPL-2.0-or-later** — see [drivers/LICENSE](drivers/LICENSE) and [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES).

## Credits

`cctl` uses driver code from the TUXEDO Linux driver project:

https://github.com/tuxedocomputers/tuxedo-drivers

The driver code is licensed under GPL-2.0-or-later. Copyright belongs to the respective original authors (TUXEDO Computers GmbH and contributors).
