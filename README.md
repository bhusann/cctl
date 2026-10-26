<p align="center">
  <a href="https://github.com/bhusann/cctl/releases/latest"><img src="https://img.shields.io/github/v/release/bhusann/cctl?label=release&color=7C5CFF&logo=github" alt="Latest release"></a>
  <a href="https://github.com/bhusann/cctl/releases"><img src="https://img.shields.io/github/downloads/bhusann/cctl/total?label=downloads&color=22D3EE" alt="Total downloads"></a>
</p>

<p align="center">
  <img src="docs/cctl-icon.svg" width="112" alt="CCTL icon">
</p>

<h1 align="center">CCTL — Linux Control Center for Colorful Laptops</h1>

CCTL (ColorControl) is a lightweight command-line control center for Colorful
Evol P15 laptops built on Clevo/TUXEDO chassis. It provides direct control over
EC power profiles, CPU and GPU fans, keyboard RGB colors and effects, battery
charge modes and thresholds, display refresh rate and scaling, GPU MUX
switching, NVIDIA clocks, webcam, and microphone. Live CPU and GPU monitors are
included, and the DKMS drivers that back these features can be managed from the
same tool.

CCTL is written in C, runs without a GUI, and needs no always-on service —
keyboard effects and custom fan curves use a small background worker only
while they are active.

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

> **Note:** Built specifically for Colorful Evol P15 series laptops. Other laptop models or other Clevo/TUXEDO variants may have different EC register layouts, fan byte orders, or EC profile codes — use at your own risk.

---

## Quick Start

1. Install the latest release in one command (downloads the latest binary and runs the installer):

   ```bash
   curl -fsSL https://github.com/bhusann/cctl/releases/latest/download/cctl -o /tmp/cctl && chmod +x /tmp/cctl && sudo /tmp/cctl install
   ```

2. Or download the pre-built `cctl` binary from the **[Releases page](https://github.com/bhusann/cctl/releases)** and install it manually:

   ```bash
   chmod +x ./cctl && sudo ./cctl install
   ```

3. Install kernel drivers via DKMS (required for fans, keyboard backlight, battery):

   ```bash
   cctl drivers-manage
   ```

4. Ready to use immediately (privileged commands auto-elevate seamlessly, no manual sudo or aliases needed):

   ```bash
   cctl set balanced          # apply a power profile (auto-elevates via sudo)
   cctl fan auto              # set fans to automatic (auto-elevates via sudo)
   cctl fancurve start quiet  # run a saved custom fan curve (after creating one)
   cctl status                # view all current settings
   ```

## Updating cctl

1. Download the latest `cctl` binary from the [official Releases page](https://github.com/bhusann/cctl/releases).
2. In the folder where you downloaded it, run:

   ```bash
   chmod +x ./cctl
   sudo ./cctl install
   ```

3. When asked to upgrade or reinstall cctl, type `y` to confirm. The installer updates the installed binary and sudo rule.

---

## Screenshots

**`cctl` — full command reference**

![cctl help](docs/screenshots/help.png)

**`cctl status` — all current settings at a glance**

![cctl status](docs/screenshots/status.png)

**`cctl cpumon` — live CPU, memory, fan, and power readings**

![cctl CPU monitor](docs/screenshots/cpumon.png)

**`cctl gpumon` — live NVIDIA GPU readings**

![cctl GPU monitor](docs/screenshots/gpumon.png)

**`cctl kbe` — keyboard backlight effects**

![cctl keyboard effects](docs/screenshots/kbe-effects.png)

**`cctl kbc` — keyboard color presets**

![cctl keyboard color presets](docs/screenshots/kbc-presets.png)

---

## Power Profiles

`set <profile> [--nosafe]` applies preset (EC defaults + table values below).
Built-in profiles are quick-use presets, not official or universal
recommendations. Choose what suits your workload, or save a preferred
combination as a [custom profile](#custom-profiles).

```
Profile    EC code Turbo  Governor     EPP                 EC default TDP      Refresh
────────── ─────── ────── ──────────── ─────────────────── ─────────────────── ───────
max        2       ON     powersave    performance         90/115W + GPU 100W  --
cpuperf    3       ON     powersave    performance         45/115W             --
balanced   3       ON     powersave    balance_performance 45/115W             --
powersave  1       OFF    powersave    balance_power       15/30W              lowest
silent/eco 0       OFF    powersave    power               15/30W              --
```

> [!NOTE]
> The built-in `powersave` profile and `silent/eco` leave fan control unchanged. `powersave` also selects the lowest supported internal display refresh rate under X11 (e.g. 40 Hz) to maximize battery runtime. You can adjust the refresh rate anytime using `cctl rr`.

> [!CAUTION]
> Silent fan mode and manual fan duty may keep fans quiet or fixed regardless of
> CPU temperature, even under heavy load. To protect the hardware, applying EC
> profile code `2` or `3`, or enabling Turbo while EC code `2` or `3` is
> active, sets fans to **AUTO** by default.
> `--nosafe` skips that automatic change and keeps the current fan setting.
> Use manual fan duty only for experimental purposes.

### EC Profiles and Governor/EPP Load Test Results

Full-load measurements and comparisons are documented in the [EC profile test results](docs/ec-profile-test-results.md).

### Custom Profiles

Create extra power presets with `cctl editconf`, then apply one with
`cctl set <name>`. The [Custom Profiles guide](docs/custom-profiles.md) shows
how to choose profile settings and includes examples and safety limits.

---

## Commands

Command names:
- Profiles and setup: `set`, `editconf`, `install`, `uninstall`, `drivers-manage`
- Keyboard and fans: `kbc`, `kbb`, `kbe`, `kbn`, `fn`, `fan`, `fancurve`
- GPU and privacy: `mux`, `nvidia clock`, `nvidia memclock`, `webcam`, `mic`
- Battery and system info: `bat`, `status`, `--version`
- Monitors: `cpumon`, `gpumon`
- Display: `rr`, `scale`
- CPU controls: `turbo`, `gov`, `epp`, `rapl`
- Snapshots: `snap` (alias `snapshot`), actions `save`/`s`, `restore`/`r`, `view`/`v`, `delete`/`del`
- Application settings: `options`
- Help: `--help`

Full usage, options, and hardware quirk notes: [commands.md](docs/commands.md).

---

## Persistence and Services

Most `cctl` commands apply a setting and exit; cctl has no tray app or general
startup service. `cctl fancurve start <name>` is an exception: it starts a
detached worker that runs the curve until stopped or the system shuts down.
When a compatible login manager provides sleep notifications, the worker
returns fan control to the laptop before sleep and resumes the curve after wake.
If the wake notification is missed, it can resume curve control when a monitored
temperature rises above 85 °C. See the [Fan Curves guide](docs/commands.md#fan-curves)
for details.

To restore saved settings at startup, save them with `cctl snap save <name>` and
configure your service manager (for example, systemd or runit) to run
`cctl snap restore <name>`. Snapshots do not include fan settings or active fan
curves; fan curves do not start automatically at boot.

> **Caution:** cctl writes the EC directly. Don't run TCC (`tccd`), NBFC, or
> another Clevo fan daemon at the same time — they will fight over fan speed and
> profile state. If TCC is installed, stop it first:
> `sudo systemctl stop tccd` (re-enable later with `sudo systemctl enable --now tccd`).

---

## Build

```bash
make                # Standard build (profiles, fans, display, battery, nvidia clock/power)
make install        # Build and install through cctl's installer
```

The Makefile respects `CC`, `CFLAGS`, and `LDFLAGS`. Hardening flags are always applied. Pass `STRIP=0` to keep debug symbols.

---

## Kernel Drivers

The Clevo/TUXEDO driver stack (`clevo_acpi`, `tuxedo_keyboard`, `tuxedo_io`) is required for fan control, keyboard backlight, battery thresholds, and EC profile codes. The installer handles DKMS registration, build, and modprobe config.

**Driver sources live only in the mirror repo:** https://github.com/bhusann/tuxedo-drivers-cctl-mirror — a readable `drivers/` folder plus the pre-packed `drivers.tar.gz`. Every GitHub **release** of cctl also attaches that *identical* tarball.

Install or uninstall directly with the script (it lives in the mirror repo):

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

Drivers first, then the cctl binary — in that order, so cctl is still around for the clean driver removal.

### Automated Uninstallation (Recommended)

1. **Kernel drivers** — `sudo cctl drivers-manage` and select the uninstall option (it cleanly unloads the modules and deregisters DKMS):

```bash
sudo cctl drivers-manage   # select the uninstall option
```

2. **cctl and its data** — run the uninstall command:

```bash
sudo cctl uninstall
```

Type `yes` in full to confirm. This removes the cctl binary, sudoers rule and
backup, custom profiles, and saved cctl data under `/var/lib/cctl`. If cctl
detects loaded TUXEDO/Clevo modules, it recommends removing them first with
`cctl drivers-manage`; you can confirm to remove cctl while leaving the drivers
installed. It is recommended to uninstall the drivers first, before uninstalling
the cctl binary.

### Manual Uninstallation

For manual driver and cctl cleanup, follow the [manual uninstallation guide](docs/manual_uninstall.md).

---

## Developer Notes

- [Developer notes](docs/developer-notes.md)

---

## License

`cctl` own code is licensed under the MIT License — see [LICENSE](LICENSE).

The driver code (in the [mirror repo](https://github.com/bhusann/tuxedo-drivers-cctl-mirror)'s `drivers/` folder) is **not** MIT. It is derived from the TUXEDO Linux driver project and remains under **GPL-2.0-or-later** — see [drivers/LICENSE](https://github.com/bhusann/tuxedo-drivers-cctl-mirror/blob/main/drivers/LICENSE) and [THIRD-PARTY-NOTICES](docs/THIRD-PARTY-NOTICES).

## Credits

> [!IMPORTANT]
> Created with AI assistance under human direction, and thoroughly tested and
> evaluated by a human.

`cctl` uses driver code from the TUXEDO Linux driver project:

https://github.com/tuxedocomputers/tuxedo-drivers

The driver code is licensed under GPL-2.0-or-later. Copyright belongs to the respective original authors (TUXEDO Computers GmbH and contributors).

GPU MUX reverse-engineered findings are referred from this repo:

https://github.com/arbitrary-string/clevo-control-panel
