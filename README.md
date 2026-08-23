# CCTL

CCTL is a small command-line tool for managing performance on Clevo P15 laptops. It sets CPU power profiles (governor, turbo, EPP, RAPL limits), controls fans through direct EC I/O, adjusts the keyboard backlight, toggles webcam/microphone, sets the display refresh rate and GPU scaling — all with no GUI and only libc as a dependency.

## Build

```bash
make            # build just the cctl binary
make all        # build cctl + kernel drivers
```

## Installation

Pre-built binaries and the drivers tarball (`cctl` and `cctl-drivers.tar.gz`) are attached to each GitHub **Releases** page — download those to skip building from source, then use the commands below. The `cctl` binary there is already built and ready to copy into place; `cctl-drivers.tar.gz` is what `drivers-install` looks for.

**`install` — install the `cctl` binary**

From a built `cctl`, run:

```bash
sudo ./cctl install
```

It copies the running binary to `/usr/local/bin/cctl` (mode 0755), writes
`/etc/sudoers.d/cctl` granting the installing user passwordless `sudo cctl`,
detects your shell (bash/zsh/fish) and adds `alias cctl='sudo cctl'` to the
matching rc file, then reminds you to restart your terminal. The `cctl` help
only shows the install hint when it is not already installed at
`/usr/local/bin/cctl`.

**`drivers-install` — install the kernel drivers**

With the bundled `cctl-drivers.tar.gz` (from a GitHub release or the repo root), run:

```bash
./cctl drivers-install
```

It searches your home directory for `cctl-drivers.tar.gz`, extracts it to a
temporary directory, and launches the interactive `driverinstall.sh` to
install/check the TUXEDO/Clevo drivers via DKMS. If the tarball isn't found
automatically you're prompted for its full path.

## Drivers

This repo ships kernel drivers for TUXEDO/Clevo hardware:

| Module | Description |
|---|---|
| `tuxedo_keyboard` | Keyboard backlight |
| `clevo_acpi` | Clevo ACPI interface |
| `tuxedo_io` | Hardware I/O (fan/PWM/EC) |
| `legacygpu` | Legacy GPU power control (optional) |

Install via DKMS (recommended):

```bash
sudo ./drivers/driverinstall.sh --install
```

The script auto-detects state and prompts to install or uninstall, and drivers rebuild automatically on kernel updates.

Make targets:

```bash
make drivers           # build drivers in-tree (no DKMS)
make drivers-install   # run driverinstall.sh --install
make drivers-uninstall # run driverinstall.sh --uninstall
make drivers-status    # check driver state
```

The keyboard module is installed with `force_backlight_type=6`.

## Usage

Most commands need `sudo`.

```
   ____ ___  _     ___  ____   ____ ___  _   _ _____ ____   ___  _     
  / ___/ _ \| |   / _ \|  _ \ / ___/ _ \| \ | |_   _|  _ \ / _ \| |    
 | |  | | | | |  | | | | |_) | |  | | | |  \| | | | | |_) | | | | |    
 | |__| |_| | |__| |_| |  _ <| |__| |_| | |\  | | | |  _ <| |_| | |___ 
  \____\___/|_____\___/|_| \_\\____\___/|_| \_| |_| |_| \_\\___/|_____|
```

**Profiles**
| Command | Description |
|---|---|
| `set <profile>` | Apply a profile (no RAPL) |
| `setR <profile>` | Apply a profile and also set RAPL limits |

Profiles: `max`, `cpuperf`, `balanced`, `powersave`, `eco`
`setR` limits: max 45/90W · cpuperf 70W · balanced 35/40W · eco 9/10W

**Fan**
| Command | Description |
|---|---|
| `fan auto\|max\|silent` | Both fans: EC-controlled / full / quiet |
| `fan <pct>` | Both fans to a duty cycle (21–100%) |
| `fan cpu\|gpu <pct>` | One fan to a duty cycle (21–100%) |

**Display** (no root)
| Command | Description |
|---|---|
| `rr [rate]` | List or set refresh rate (1 = high, 2 = low) |
| `scale <value>` | GPU scaling: factor 0.01–1.0 (e.g. `0.5`), resolution `WxH` (e.g. `1920x1080`), or `off`/`reset` for native |

**Keyboard**
| Command | Description |
|---|---|
| `kbc R G B` | Color by RGB (0–255 each, e.g. `kbc 255 0 128`) |
| `kbc #hex` | Color by hex (`kbc #ff0080`) |
| `kbc <name>` | Color by preset name (`kbc cyan`) |
| `kbb <pct>` | Brightness (0–100%) |

`kbcp` is still accepted as an alias for `kbc`.

**System**
| Command | Description |
|---|---|
| `turbo <on\|off>` | Turbo boost override |
| `gov <governor>` | CPU governor (`powersave`, `performance`) |
| `epp <value>` | EPP (`performance`, `balance_performance`, `balance_power`, `power`) |
| `rapl <pl1> <pl2>` | RAPL limits in watts; use `skip` to omit one |
| `rapl skip <pl2>` | PL2 only |
| `rapl <pl1> skip` | PL1 only |
| `rapl <pl2>` | One arg = PL2 only (shorthand) |
| `mic [on\|off]` | Toggle or set microphone |
| `fn <lock\|unlock>` | Fn Lock toggle |
| `webcam [on\|off]` | Toggle or set webcam |

**Battery**
| Command | Description |
|---|---|
| `bat` | Show charge thresholds |
| `bat <start> <stop>` | Set start/stop charge % (sudo) |
| `bat off` | Widest range (min/max) |

**NVIDIA** (needs root)
| Command | Description |
|---|---|
| `nvidia <on\|off>` | Persistent toggle + initramfs rebuild |
| `nvidia load` | Load compute modules for this session |
| `nvidia loadgame` | Load all modules (incl. drm) for this session |
| `nvidia unload` | Unload modules + power off GPU for this session |
| `nvidia status` | Show boot config, module state, telemetry |
| `nvidia clock <min,max>` | Lock GPU clocks (`reset` to unlock) |
| `nvidia memclock <min,max>` | Lock GPU memory clocks (`reset` to unlock) |
| `nvidia pm <on\|off>` | Toggle persistence mode |
| `nvidia pmclock <min,max>` | Persistence on + clock lock, one step |
| `nvidia power [on\|off]` | Direct PCI power (D0/D3cold); no arg shows state |

> [!WARNING]
> **`nvidia off` is experimental — use it only as a last resort.** It permanently disables the discrete GPU at boot (blacklist + initramfs rebuild) and can break games and other dGPU software. Try **`nvidia unload`** first to switch the GPU off for the current session only (reverts after reboot). Confirming `nvidia off` requires typing `yes` twice.

**Info** (no root)
| Command | Description |
|---|---|
| `status` | Show all current settings |
| `monitor` | Live CPU/power/fan monitor |

Profiles apply every CPU/GPU parameter at once. The `setR` variant also sets RAPL (package-0 only — writing to `psys` or sub-zones causes an EC conflict that throttles the CPU to 0.4 GHz on Clevo). Turbo and governor overrides take effect immediately but are overwritten by the next profile change. CCTL detects the P/E-core layout from CPU topology and pins itself to E-cores so it doesn't disturb games.

### RAPL examples

```bash
sudo cctl rapl 45 90     # PL1=45W, PL2=90W
sudo cctl rapl skip 50   # PL2 only (50W)
sudo cctl rapl 40 skip   # PL1 only (40W)
sudo cctl rapl 50        # one arg = PL2 only
```

> [!NOTE]
> `skip` cleanly omits a limit — no placeholder value is ever written to the omitted power limit.

## Profiles

| Profile | GPU | Turbo | Governor | EPP | RAPL (setR) |
|---------|-----|-------|----------|-----|-------------|
| max | 2 | ON | perf | performance | 45W / 90W |
| cpuperf | 3 | ON | perf | performance | 70W |
| balanced | 3 | ON | powersave | balance_performance | 35W / 40W |
| powersave | 1 | OFF | powersave | balance_power | — |
| eco | 0 | OFF | powersave | power | 9W / 10W |

## Fan notes

> [!NOTE]
> **EC byte-order quirk.** The Clevo EC reads the two-byte fan commands (`cmd 0x99`) in different orders depending on context: when restoring `auto` it expects `{ 0xFF, fan_idx }`, but when setting a duty (`fan_set_duty`, `fan_max_all`, `fan_silent_all`) it expects `{ fan_idx, duty }`. Don't "normalize" these — the swapping is verified correct on the P15.

> [!NOTE]
> **GPU fan telemetry uses a different register.** The ACPI `FANINFO1` register (`0x63`, byte 2) does not report GPU fan duty correctly on this model (it's stuck around 13–19%). CCTL reads GPU duty from `FANINFO2` (`0x64`, byte 0) instead, which is accurate. The CPU fan reads fine from `FANINFO1`.

## Keyboard presets

blue, chocolate, coral, cyan, gold, gray, green, indigo, lime, magenta, maroon, navy, off, olive, orange, pink, purple, red, salmon, silver, teal, turquoise, violet, white, yellow

## NVIDIA GPU

**Recommended workflow:** run `nvidia off` once to blacklist the GPU and rebuild initramfs — that makes nvidia-off the boot default — then use `nvidia load`, `nvidia loadgame`, or `nvidia unload` for instant session-only toggling. The blacklist stays in place, so the GPU remains off after reboot. Only run `nvidia on` when you want nvidia back at boot.

`load`/`loadgame`/`unload` require blacklist mode (i.e. you must have run `nvidia off` first). `load` and `loadgame` temporarily move the blacklist aside for `modprobe`, then restore it. `unload` uses `rmmod` to remove the loaded modules and powers the GPU off via PCI runtime PM (D3cold); it warns first if the GPU is still driving a display.

`nvidia power` talks to the GPU hardware directly through PCI runtime PM. That's useful when no nvidia driver is loaded — otherwise the GPU sits in D0 drawing ~5–15 W of idle power. With the driver loaded it manages its own power state.

`nvidia clock <min,max>` locks the clock range via `nvidia-smi -lgc` (e.g. `cctl nvidia clock 210,1600`); `nvidia clock reset` unlocks with `-rgc`. `nvidia memclock` does the same for memory clocks (`-lmc`/`-rmc`). On laptops where power-limit (`-pl`) and temperature-target (`-gtt`) controls are unsupported, clock capping is the practical way to cut GPU power and heat. `nvidia pm on|off` toggles persistence mode (`-pm`), and `nvidia pmclock <min,max>` turns on persistence and locks clocks in one step. Clock ranges are validated (`min <= max`, both > 0).

> [!NOTE]
> For display or gaming (Vulkan, PRIME offload, external monitors), use `nvidia loadgame` to load all four modules at once. If you already ran `nvidia load`, you can still load the DRM module manually with `sudo modprobe nvidia_drm`.

> [!TIP]
> **Stop Xorg from locking the GPU (i3 / tiling WM users).** After `nvidia loadgame`, the display modules load and `/dev/dri/card1` appears. Xorg may auto-detect the new GPU and grab it, which then blocks `nvidia unload` with `Module is in use` errors.
>
> Disable `AutoAddGPU` so Xorg leaves the hotplugged GPU alone:
> ```
> # /etc/X11/xorg.conf.d/10-no-gpu-hotplug.conf
> Section "ServerFlags"
>     Option "AutoAddGPU" "false"
> EndSection
> ```
> With that set, `nvidia unload` works immediately whenever you're done, without ending your session.
