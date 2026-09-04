# cctl

Fast, lightweight CLI tool to control power profiles, fans, keyboard backlight, display, and GPU settings on Clevo P15 laptops. Pure C, libc only, no GUI daemon.

## Tested Laptops

* **Colorful Evol P15** (Intel Core i7-13620H, RTX 4060 Mobile 100W)
* **Colorful Evol P15** (Intel Core i5-12500H, RTX 4050 Mobile 100W)

Compatible with most Clevo-chassis variants that support the TUXEDO/Clevo driver stack.

## Installation

Pre-built binaries and kernel drivers are available on the [Releases](https://github.com/bhusann/cctl/releases) page. The release binary is the default no-NVIDIA build.

```bash
# 1. Install binary to /usr/local/bin, set up passwordless sudoers, and shell alias
sudo ./cctl install

# 2. Install kernel drivers via DKMS (clevo_acpi, tuxedo_keyboard, tuxedo_io)
./cctl drivers-install
```

Restart your terminal after running `cctl install`.

## Build

```bash
make             # Standard build (power/fans/display/battery + nvidia-smi clock control)
make cctl-nvidia # Full build with module toggles (on/off/load/unload/status/power)
make all         # Build cctl + in-tree kernel drivers
```

## Quick Reference

Most hardware-write commands require `sudo` (or the alias configured by `cctl install`).

### Performance Profiles

`set <profile>` changes CPU governor, EPP, turbo boost, and Clevo EC GPU profile slot.  
`setR <profile>` applies the same profile and also clamps package power limits via RAPL.

| Profile | GPU Slot | Turbo | Governor | EPP | `setR` Power Limits | Best For |
|---|---|---|---|---|---|---|
| `max` | performance (2) | ON | performance | performance | PL1: 45W, PL2: 90W | Gaming, heavy compute |
| `cpuperf` | turbo (3) | ON | performance | performance | PL2: 70W only | Compiles, CPU benchmarks |
| `balanced` | turbo (3) | ON | powersave | balance_perf | PL1: 35W, PL2: 40W | Daily use |
| `powersave` | standard (1) | OFF | powersave | balance_power | (no cap) | Quiet battery use |
| `eco` | quiet (0) | OFF | powersave | power | PL1: 9W, PL2: 10W | Maximum battery savings |

> **Note on `set max` vs `setR max`**: Plain `set max` leaves RAPL untouched, running OEM platform limits (PL1 90W / PL2 115W CPU, 100W GPU). `setR max` holds sustained CPU package draw to 45W (boost to 90W) to leave thermal headroom for the GPU.

### Commands

#### Fan Control
```bash
cctl fan auto|max|silent      # Both fans: EC automatic, 100% full, or quiet
cctl fan <pct>                # Set both fans to duty cycle (21-100%)
cctl fan cpu|gpu <pct>        # Set CPU or GPU fan duty cycle individually
```

#### Display
```bash
cctl rr [1|2|<rate>]          # Set refresh rate (1 = max, 2 = min, or e.g. 60)
cctl scale <factor|WxH|off>   # GPU scaling (e.g. 0.75, 1920x1080, or off to reset)
```

#### Keyboard Backlight
```bash
cctl kbc <R G B>              # Set RGB color (e.g. 255 0 128)
cctl kbc <#hex>               # Set hex color (e.g. #00ffff)
cctl kbc <preset>             # Set named preset
cctl kbb <pct>                # Brightness (0-100%)
```
*Presets*: `blue`, `chocolate`, `coral`, `cyan`, `gold`, `gray`, `green`, `indigo`, `lime`, `magenta`, `maroon`, `navy`, `off`, `olive`, `orange`, `pink`, `purple`, `red`, `salmon`, `silver`, `teal`, `turquoise`, `violet`, `white`, `yellow`.

#### Power & System
```bash
cctl turbo on|off             # Toggle Intel turbo boost
cctl gov powersave|performance# CPU scaling governor
cctl epp <preference>         # performance, balance_performance, balance_power, power
cctl rapl <pl1> <pl2>         # Set PL1/PL2 in watts (use 'skip' to omit one, e.g. rapl skip 50)
cctl bat                      # Show charge thresholds and battery health
cctl bat <start> <stop>       # Set charge thresholds (e.g. 40 80)
cctl bat off                  # Top-up mode (e.g. 95 100)
cctl mic [on|off]             # Toggle or set microphone
cctl webcam [on|off]          # Toggle or set webcam
cctl fn lock|unlock           # Toggle Fn Lock
cctl status                   # Print all current settings
cctl monitor                  # Live curses-free CPU/power/fan monitor
```

#### NVIDIA GPU
```bash
# Clock management (available in all builds, auto-enables persistence mode):
cctl nvidia clock <min,max>|reset    # Lock GPU clocks (-lgc), reset unlocks (-rgc)
cctl nvidia memclock <min,max>|reset # Lock VRAM clocks (-lmc), reset unlocks (-rmc)

# Module & hardware controls (requires make cctl-nvidia):
cctl nvidia load                     # Session load: compute modules (nvidia, nvidia_uvm)
cctl nvidia loadgame                 # Session load: all modules (+ modeset, drm)
cctl nvidia unload                   # Session unload: rmmod all modules + cut power (D3cold)
cctl nvidia power [on|off]           # PCI power state (D0 / D3cold); no arg checks state
cctl nvidia status                   # Telemetry, loaded modules, and active GPU PIDs
cctl nvidia on|off                   # Persistent boot toggle (modprobe blacklist + initramfs)
```

> **Warning (`nvidia off`)**: `nvidia off` permanently blacklists the driver and rebuilds your initramfs. Use `nvidia unload` instead if you only want to power off the GPU for the current session.

## Important Hardware Quirks

* **RAPL 0.4 GHz Throttle**: Only package-0 (`/sys/class/powercap/intel-rapl:0`) is safe to write. Touching sub-zones (`intel-rapl:0:X`) or platform `psys` triggers an EC conflict that hard-throttles the CPU to 400 MHz.
* **EC Fan Byte Order**: Clevo's EC expects reversed byte order depending on command context. For `auto` restore it expects `{0xFF, fan_idx}`, while setting duty cycle expects `{fan_idx, duty}`.
* **GPU Fan Duty Register**: ACPI `FANINFO1` byte 2 is stuck at ~15% on this model. `cctl` reads GPU fan duty from `FANINFO2` (`0x64`, byte 0), which reports accurate percentages.
* **Xorg Holding GPU on Hotplug**: When using `nvidia loadgame` under Xorg/i3, Xorg may grab the hotplugged card, causing `nvidia unload` to fail with `Module is in use`. Add this snippet to `/etc/X11/xorg.conf.d/10-no-gpu-hotplug.conf`:
  ```xorg
  Section "ServerFlags"
      Option "AutoAddGPU" "false"
  EndSection
  ```
