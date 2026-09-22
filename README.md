# CCTL — ColorControl

Linux CLI alternative to the Windows-only Colorful Laptop Control Center. Fast, single-binary tool for Colorful Evol P15 laptops (Clevo/TUXEDO chassis) — controls power profiles, fans, keyboard backlight, display, battery, GPU MUX switching, and NVIDIA GPU. Pure C, no GUI, no daemon.

## Hardware Compatibility

Specifically designed for the **Colorful Evol P15** series (Clevo/TUXEDO chassis).

### Tested Hardware
| Laptop | CPU | GPU | Keyboard |
|---|---|---|---|
| Colorful Evol P15 | Intel Core i7-13620H | RTX 4060 Mobile 100W | Single-zone RGB |
| Colorful Evol P15 | Intel Core i5-12500H | RTX 4050 Mobile 100W | Single-zone RGB |

### Supported Configurations
* **Series:** Colorful Evol P15 series
* **GPUs:** NVIDIA GeForce RTX 40 Series Mobile
* **CPUs:** Intel CPUs
* **Keyboard:** Single-zone RGB keyboard (tested)

> **Note:** Built specifically for Colorful Evol P15 series laptops. Other laptop models or other Clevo/TUXEDO variants may have different EC register layouts, fan byte orders, or GPU profile slots — use at your own risk.

---

## Quick Start

Pre-built binaries and driver sources are available on the [Releases](https://github.com/bhusann/cctl/releases) page.

```bash
# 1. Install cctl system-wide (installs to /usr/local/bin, sets up passwordless sudo & auto-elevation)
sudo ./cctl install

# 2. Install kernel drivers via DKMS (required for fans, keyboard backlight, battery)
sudo ./cctl drivers-install

# 3. Ready to use immediately (privileged commands auto-elevate seamlessly, no manual sudo or aliases needed):
cctl set balanced          # apply a power profile (auto-elevates via sudo)
cctl fan auto              # set fans to automatic (auto-elevates via sudo)
cctl status                # view all current settings
```

---

## Power Profiles

`set <profile> [--nosafe]` applies preset (EC defaults + table values below).

`setR <profile> [--nosafe]` applies preset (EC defaults + preconfigured CPU TDP override).

```
Profile     Turbo  Governor     EPP                EC default CPU & GPU TDP (set)  RAPL CPU TDP override (setR only)
─────────── ────── ──────────── ────────────────── ────────────────────────────── ────────────────────────────────
max         ON     performance  performance        90/115W + GPU 100W              PL1 45 / PL2 90W
cpuperf     ON     performance  performance        45/115W + GPU 70W               (no RAPL change)
balanced    ON     powersave    balance_performance 45/115W + GPU 70W              PL1 35 / PL2 40W
powersave   OFF    powersave    balance_power      15/30W  + GPU 70W               (no RAPL change)
eco         OFF    powersave    power              15/30W  + GPU 70W               PL1 9 / PL2 10W
```

> **Fan Safety Defaults & Safety Disclaimer**: If fans were previously locked to `silent`, the EC suppresses fan speeds even under high heat. To prevent overheating and thermal throttling, `cctl` automatically resets fans to **`AUTO`** when activating high-power profiles (`max`, `cpuperf`, `balanced`) or enabling `turbo on`. Pass `--nosafe` (e.g. `cctl set max --nosafe` or `cctl turbo on --nosafe`) to bypass this and keep your current fan state. Setting manual fan duty (`cctl fan <pct> --nosafe` or `cctl fan cpu|gpu <pct> --nosafe`) strictly requires `--nosafe`.
>
> ⚠️ **Safety Notice & Disclaimer**: Safety defaults (running without `--nosafe`) are strongly recommended for daily use to protect your hardware. The `--nosafe` flag is intended strictly for experimenting or one-time use for a specific purpose — **not for daily or regular use**. Overriding safety mechanisms can lead to severe overheating, thermal throttling, or hardware stress; the author is not responsible for any damage or instability caused by using this flag.
>
> **`set max` vs `setR max`**: Plain `set max` leaves RAPL untouched, running at OEM platform limits (PL1 90W / PL2 115W CPU, 100W GPU). `setR max` caps sustained CPU draw to 45W (burst to 90W) to leave thermal headroom for the GPU.
>
> **Profile Change Visual Indicator**: Whenever switching profiles (`cctl set` or `cctl setR`), the keyboard backlight performs a snappy 375ms breathing pulse in the profile's signature color to provide instant visual feedback without blocking the shell, then immediately restores your previous state:
> * **`max`**: Red (`#ff0000`)
> * **`cpuperf`**: Orange (`#ff6e00`)
> * **`balanced`**: Violet (`#c832ff`)
> * **`powersave`**: Green (`#00ff00`)
> * **`eco`**: Light Blue (`#50b4ff`)
>
> *Non-destructive state handling*: If the keyboard was set to a solid color (or off), it returns to that exact color and brightness. If a background animation was active (such as `fire` or `aurora`), the pulse temporarily overrides it and automatically resumes your animation afterward without losing your original solid state.

---

## Commands

### Keyboard Backlight
```bash
cctl kbc <color>                # Set keyboard color: R G B (0-255), #RRGGBB, or preset name
cctl kbb <pct>                  # Brightness (0-100%)
cctl kbe <effect>               # Start background keyboard effect
cctl kbe stop                   # Stop active effect and restore original color & brightness
cctl kbe                        # Show active effect status or list available effects
cctl fn [lock|unlock]          # Toggle or set Fn Lock (no arg toggles, or lock/unlock)
```

Presets: `blue` `chocolate` `coral` `cyan` `gold` `gray` `green` `indigo` `lime` `magenta` `maroon` `navy` `olive` `orange` `pink` `purple` `red` `salmon` `silver` `teal` `turquoise` `violet` `white` `yellow` `off`

**Keyboard Effects (`kbe`)**: Runs background animations on single-zone RGB keyboards without holding the terminal. Before starting, it records your current keyboard color and brightness; when stopped (`cctl kbe stop`), it restores the keyboard to that exact original state. Single-color effects automatically use your current keyboard color:
* `breathe` — Smooth fade in/out breathing (uses current color)
* `breathe-cycle` — Smooth breathing while cycling through the spectrum
* `cycle` — Smooth continuous rainbow color cycle
* `flash` — Strobe flash bursts (uses current color)
* `flash-cycle` — Strobe flash bursts changing color on each burst
* `candle` — Realistic flickering candlelight flame
* `pulse` — Heartbeat double-pulse rhythm (uses current color)
* `police` — Alternating emergency red & blue strobe bursts
* `fire` — Dynamic warm campfire flame with turbulent embers
* `aurora` — Hypnotic Northern Lights emerald, cyan, and violet drift
* `storm` — Dark moody sky with sudden electric lightning strikes
* `starlight` — Deep midnight sky with gentle twinkling star shimmers
* `temp` — Live CPU thermal heatmap (cyan $\to$ green $\to$ yellow $\to$ red, pulses $>90^\circ\text{C}$)

*Manual Status Check*: Run `cctl kbe` to view the active effect, PID, and preserved base state (it re-elevates automatically — the `/run/cctl_kbe.state` file is root-only; `sudo cat` it directly if you prefer).

### Fan Control
```bash
cctl fan auto|max               # Both fans: EC automatic / 100% full speed
cctl fan silent [--nosafe]      # Quiet mode (forces eco profile first; bypass with --nosafe)
cctl fan <pct> --nosafe         # Set both fans to duty cycle (21-100%, requires --nosafe)
cctl fan cpu|gpu <pct> --nosafe # Set individual fan duty (requires --nosafe)
```

### GPU MUX Switching
Toggle the internal display hardware multiplexer between **MSHybrid** (panel driven by iGPU, both GPUs enumerated) and **dGPU** (panel driven directly by NVIDIA discrete GPU, iGPU removed from PCI display class).

```bash
cctl mux                        # Show current MUX mode (MSHybrid / dGPU) & pending status
cctl mux switch                 # Toggle MUX mode (stages in UEFI NVRAM, reboot to apply)
```

> **How it works:** `cctl mux switch` writes to the UEFI Setup NVRAM variable (`Setup-a04a27f4-df00-4d42-b552-39511302113d` offset 430). The setting is committed to SPI flash and latched by firmware during POST on the next boot. It includes safety guards: blob length verification (1204 B) and unknown value refusal.

### Privacy
```bash
cctl webcam [on|off]            # Toggle or set webcam
cctl mic [on|off]               # Toggle or set internal microphone (laptop mic only, needs alsa/amixer)
```

### Battery
```bash
cctl bat                        # Show current thresholds and battery health
cctl bat <start> <stop>         # Set charge thresholds (custom, e.g. 40 80)
cctl bat max                    # Standard: charge to 100%, resume at 95%
```

### Info
```bash
cctl status                     # Print all current settings (profiles, GPU MUX, telemetry)
cctl monitor                    # Live CPU/power/fan/memory monitor (color-coded)
cctl update                     # Update cctl itself from the latest GitHub release
cctl --version                  # Print version and exit (also -V)
```

### NVIDIA GPU
```bash
cctl nvidia power [on|off]              # Hardware D0/D3cold control; no arg shows state
cctl nvidia clock <min,max> | reset     # Lock/unlock GPU clocks (auto persistence)
cctl nvidia memclock <min,max> | reset  # Lock/unlock memory clocks (auto persistence)
```

### Display
> **Note:** Display commands rely on `xrandr` and are only supported on native **X11** sessions. They are **not** supported under Wayland or XWayland. `cctl` automatically checks for an active X11 session with `xrandr`: if not present, the `DISPLAY` section is hidden from `cctl --help` and the refresh rate is omitted from `cctl status`.
>
> Displays only support their specific predefined hardware/EDID refresh rates — arbitrary in-between refresh rates cannot be set. Run `cctl rr` without arguments to list all available/supported refresh rates for your screen, and choose only from those listed.

```bash
cctl rr                         # List all supported refresh rates
cctl rr [1|2|<rate>]            # Set refresh rate (1=highest, 2=lowest, or explicit value like 60 or 144)
cctl scale <factor|WxH|off>    # GPU scaling (0.75, 1920x1080, or off to reset)
```

### Profile Individual Overrides
```bash
cctl turbo on|off [--nosafe]    # Toggle Intel turbo boost (on sets fans to auto; bypass with --nosafe)
cctl gov powersave|performance  # CPU scaling governor
cctl epp <preference>           # performance, balance_performance, balance_power, power
cctl rapl <pl1> <pl2>           # Set PL1/PL2 in watts (use 'skip' to omit one)
```
>
> **RAPL limits:** PL2 ≤ 115 W always. PL1 ≤ 45 W — with one exception: up to **90 W PL1 while max mode is active** (recorded by the last `cctl set max` / `cctl setR max`; shown as `Mode:` in `cctl status`, `EC default` when no profile has been applied yet).

---

## Build

```bash
make                # Standard build (profiles, fans, display, battery, nvidia clock/power)
```

---

## Kernel Drivers

The Clevo/TUXEDO driver stack (`clevo_acpi`, `tuxedo_keyboard`, `tuxedo_io`) is required for fan control, keyboard backlight, battery thresholds, and EC GPU profile slots. The installer handles DKMS registration, build, and modprobe config.

**Driver sources live only in the mirror repo:** https://github.com/bhusann/tuxedo-drivers-cctl-mirror — a readable `drivers/` folder plus the pre-packed `drivers.tar.gz`. Every GitHub **release** of cctl also attaches that *identical* tarball (fetched straight from the mirror — never rebuilt; the release workflow refuses to publish if its sha256 drifts from the constant baked into `cctl.c`).

`sudo cctl drivers-install` resolves the sources itself, trying in this order:

1. `drivers.tar.gz` next to the cctl binary — sha256-checked on the spot (a bad file is rejected immediately, before anything is copied)
2. download `drivers.tar.gz` from the mirror repo (curl/wget) into a private temp dir — on failure it tells you exactly where to place a manual copy
3. offline / download failed: accepts the full path to a `drivers.tar.gz` you already have (checked in place first; staged to `/tmp` only if valid)

Every candidate is verified against the sha256 **baked into the cctl binary before extraction** — spoofed or stale files are refused.

```bash
sudo ./cctl drivers-install
```

Direct install/uninstall (bypass cctl) — the script lives in the mirror repo:

```bash
git clone https://github.com/bhusann/tuxedo-drivers-cctl-mirror
sudo tuxedo-drivers-cctl-mirror/drivers/driverinstall.sh --install
sudo tuxedo-drivers-cctl-mirror/drivers/driverinstall.sh --uninstall
tuxedo-drivers-cctl-mirror/drivers/driverinstall.sh --status   # no root needed
```

### Supported Distros

Auto-installs `dkms` + kernel headers if missing, with confirmation prompt:

| Package Manager | Distros | Tested |
|---|---|---|
| `pacman` | Arch, Manjaro, EndeavourOS, CachyOS, Garuda | ✅ Arch, CachyOS |
| `apt` | Debian, Ubuntu, Mint, Pop!_OS, Zorin | — |
| `dnf` | Fedora (standard Workstation/Spins), RHEL, Rocky, Alma | — |
| `zypper` | openSUSE Tumbleweed/Leap | — |
| `xbps` | Void Linux | — |
| `emerge` | Gentoo | — |
| `eopkg` | Solus | — |

> **Immutable / Atomic OS note:** Atomic and immutable editions of Fedora (Silverblue, Kinoite, Atomic Desktops, Bazzite, etc. using `rpm-ostree`) are **not supported** because the root filesystem is read-only and DKMS modules cannot persist across image updates. Only standard, non-atomic Fedora (Workstation, KDE Spin, etc. using regular `dnf`) is supported. On unrecognized distros, the installer points to the driver source in the mirror repo for manual installation.

---

## Uninstallation

### 1. Remove `cctl` Binary & Sudoers Entry
`cctl install` places the binary in `/usr/local/bin/cctl` and configures a passwordless sudo rule in `/etc/sudoers.d/cctl` for auto-elevation (validated with `visudo -c` on install; the previous rule is backed up as `/etc/sudoers.d/cctl.bak`). To remove them:

```bash
# Remove installed binary
sudo rm -f /usr/local/bin/cctl

# Remove passwordless sudo rule (+ backup)
sudo rm -f /etc/sudoers.d/cctl /etc/sudoers.d/cctl.bak
```

### 2. Remove Kernel Drivers

#### Method A: Automated (Recommended)
Use the driver installation script to cleanly unload modules and deregister DKMS:

```bash
git clone https://github.com/bhusann/tuxedo-drivers-cctl-mirror
sudo tuxedo-drivers-cctl-mirror/drivers/driverinstall.sh --uninstall
# or if cctl is still installed:
sudo cctl drivers-install   # select the uninstall option
```

#### Method B: Manual Uninstallation
To remove the driver stack manually without using the script:

1. **Unload active kernel modules** (in reverse dependency order):
   ```bash
   sudo modprobe -r clevo_acpi
   sudo modprobe -r tuxedo_io
   sudo modprobe -r tuxedo_keyboard
   ```

2. **Unregister and remove DKMS module**:
   ```bash
   sudo dkms remove tuxedo-drivers/1.0 --all
   sudo rm -rf /var/lib/dkms/tuxedo-drivers
   ```

3. **Remove driver source files and modprobe options**:
   ```bash
   sudo rm -rf /usr/src/tuxedo-drivers-1.0
   sudo rm -f /etc/modprobe.d/tuxedo_keyboard.conf
   ```

4. **Update module dependency cache**:
   ```bash
   sudo depmod -a
   ```

---

## Hardware Quirks & Developer Notes

- **RAPL 0.4 GHz Throttle** — Only package-0 (`intel-rapl:0`) is safe to write. Touching sub-zones (`intel-rapl:0:X`) or platform `psys` triggers an EC conflict that hard-throttles the CPU to 400 MHz.

- **EC Fan Byte Order** — Clevo's EC expects reversed byte order depending on command context. Auto-restore uses `{0xFF, fan_idx}`, duty cycle uses `{fan_idx, duty}`.

- **GPU Fan Duty Register** — ACPI `FANINFO1` byte 2 is stuck at ~15% on this model. `cctl` reads GPU fan duty from `FANINFO2` (`0x64`, byte 0) for accurate readings.

- **Keyboard Backlight Type Override (`force_backlight_type=6`)** — Why the driver installer forces type 6:
  - In `drivers/src/clevo_leds.h:clevo_leds_init()` (path within the mirror repo):
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

- **GPU Display MUX via UEFI NVRAM (Insyde H2O)** — The laptop features a physical display multiplexer with two BIOS modes:
  - `MSHybrid` (panel driven by Intel iGPU; NVIDIA dGPU provides render offload).
  - `dGPU` (panel wired directly to NVIDIA GeForce RTX card; Intel iGPU is unmapped from PCI display class).
  - The hardware MUX state cannot be flipped on-the-fly inside an active OS session (ACPI `_DSM` methods on this Insyde board hang the display subsystem). Instead, switching is staged via the UEFI NVRAM variable `Setup-a04a27f4-df00-4d42-b552-39511302113d` at file offset 430 (`0x03` = MSHybrid, `0x02` = dGPU). The new mode is latched during POST upon reboot.

---

## License

`cctl` own code is licensed under the MIT License — see [LICENSE](LICENSE).

The driver code (in the [mirror repo](https://github.com/bhusann/tuxedo-drivers-cctl-mirror)'s `drivers/` folder) is **not** MIT. It is derived from the TUXEDO Linux driver project and remains under **GPL-2.0-or-later** — see [drivers/LICENSE](https://github.com/bhusann/tuxedo-drivers-cctl-mirror/blob/main/drivers/LICENSE) and [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES).

## Credits

`cctl` uses driver code from the TUXEDO Linux driver project:

https://github.com/tuxedocomputers/tuxedo-drivers

The driver code is licensed under GPL-2.0-or-later. Copyright belongs to the respective original authors (TUXEDO Computers GmbH and contributors).
