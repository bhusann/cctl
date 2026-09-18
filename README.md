# CCTL — ColorControl

Linux CLI alternative to the Windows-only Colorful Laptop Control Center. Fast, single-binary tool for Colorful Evol P15 laptops (Clevo/TUXEDO chassis) — controls power profiles, fans, keyboard backlight, display, battery, and NVIDIA GPU. Pure C, no GUI, no daemon.

## Tested Hardware

| Laptop | CPU | GPU |
|---|---|---|
| Colorful Evol P15 | Intel Core i7-13620H | RTX 4060 Mobile 100W |
| Colorful Evol P15 | Intel Core i5-12500H | RTX 4050 Mobile 100W |

Built and tested for the Colorful Evol P15 series. Other Clevo/TUXEDO variants may have different EC register layouts, fan byte orders, or GPU profile slots — use at your own risk.

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

`set <profile>` configures CPU governor, EPP, turbo boost, and the Clevo EC GPU profile slot.
`setR <profile>` does the same, plus clamps CPU package power via RAPL.

```
Profile     Turbo  Governor     EPP                TDP (CPU + GPU)        RAPL (with setR)
─────────── ────── ──────────── ────────────────── ────────────────────── ────────────────────
max         ON     performance  performance        90/115W + GPU 100W     PL1 45 / PL2 90W
cpuperf     ON     performance  performance        45/115W + GPU 70W      PL2 70W
balanced    ON     powersave    balance_performance 45/115W + GPU 70W     PL1 35 / PL2 40W
powersave   OFF    powersave    balance_power      15/30W  + GPU 70W      (no RAPL change)
eco         OFF    powersave    power              15/30W  + GPU 70W      PL1 9 / PL2 10W
```

> **`set max` vs `setR max`**: Plain `set max` leaves RAPL untouched, running at OEM platform limits (PL1 90W / PL2 115W CPU, 100W GPU). `setR max` caps sustained CPU draw to 45W (burst to 90W) to leave thermal headroom for the GPU.

---

## Commands

### Fan Control
```bash
cctl fan auto|max|silent        # Both fans: EC automatic / 100% / quiet
cctl fan <pct>                  # Set both fans to duty cycle (21-100%)
cctl fan cpu|gpu <pct>          # Set individual fan duty
```

### Display
```bash
cctl rr [1|2|<rate>]            # Refresh rate (1=max, 2=min, or e.g. 60)
cctl scale <factor|WxH|off>    # GPU scaling (0.75, 1920x1080, or off to reset)
```

### Keyboard Backlight
```bash
cctl kbc <R G B>                # Set RGB color (e.g. 255 0 128)
cctl kbc <#hex>                 # Set hex color (e.g. #00ffff)
cctl kbc <preset>               # Named preset (see below)
cctl kbb <pct>                  # Brightness (0-100%)
```

Presets: `blue` `chocolate` `coral` `cyan` `gold` `gray` `green` `indigo` `lime` `magenta` `maroon` `navy` `off` `olive` `orange` `pink` `purple` `red` `salmon` `silver` `teal` `turquoise` `violet` `white` `yellow`

### Power & System
```bash
cctl turbo on|off               # Toggle Intel turbo boost
cctl gov powersave|performance  # CPU scaling governor
cctl epp <preference>           # performance, balance_performance, balance_power, power
cctl rapl <pl1> <pl2>           # Set PL1/PL2 in watts (use 'skip' to omit one)
cctl mic [on|off]               # Toggle or set microphone (internal + headphone)
cctl webcam [on|off]            # Toggle or set webcam
cctl fn lock|unlock             # Fn Lock toggle
cctl status                     # Print all current settings
cctl monitor                    # Live CPU/power/fan/memory monitor (color-coded)
```

### Battery
```bash
cctl bat                        # Show charge thresholds and battery health
cctl bat <start> <stop>         # Set charge thresholds (e.g. 40 80)
cctl bat off                    # Top-up mode (charge to max)
```

### NVIDIA GPU
```bash
# Available in all builds:
cctl nvidia power [on|off]              # PCI power state (D0/D3cold); no arg shows state
cctl nvidia clock <min,max>|reset       # Lock GPU clocks; auto-enables persistence mode
cctl nvidia memclock <min,max>|reset    # Lock VRAM clocks

# Requires make cctl-nvidia:
cctl nvidia load                        # Load compute modules (nvidia, nvidia_uvm)
cctl nvidia loadgame                    # Load all modules (+ modeset, drm)
cctl nvidia unload                      # Unload all modules + power off (D3cold)
cctl nvidia status                      # Telemetry, loaded modules, active GPU PIDs
cctl nvidia on|off                      # Persistent boot toggle (blacklist + initramfs)
```

> ⚠️ **`nvidia off`** permanently blacklists the driver and rebuilds initramfs. Use `nvidia unload` for session-only GPU power off.

---

## Build

```bash
make                # Standard build (profiles, fans, display, battery, nvidia clock/power)
make cctl-nvidia    # Full build with NVIDIA module management (on/off/load/unload/status)
make all            # Build cctl + in-tree kernel drivers
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

## Hardware Quirks

- **RAPL 0.4 GHz Throttle** — Only package-0 (`intel-rapl:0`) is safe to write. Touching sub-zones (`intel-rapl:0:X`) or platform `psys` triggers an EC conflict that hard-throttles the CPU to 400 MHz.

- **EC Fan Byte Order** — Clevo's EC expects reversed byte order depending on command context. Auto-restore uses `{0xFF, fan_idx}`, duty cycle uses `{fan_idx, duty}`.

- **GPU Fan Duty Register** — ACPI `FANINFO1` byte 2 is stuck at ~15% on this model. `cctl` reads GPU fan duty from `FANINFO2` (`0x64`, byte 0) for accurate readings.

- **Xorg Holding GPU on Hotplug** — When using `nvidia loadgame` under Xorg, it may grab the hotplugged card, preventing `nvidia unload`. Fix by adding to `/etc/X11/xorg.conf.d/10-no-gpu-hotplug.conf`:
  ```
  Section "ServerFlags"
      Option "AutoAddGPU" "false"
  EndSection
  ```
