# **CCTL**

Lightweight CLI tool for Clevo P15 laptop performance management. Sets CPU power profiles (governor, turbo boost, EPP, RAPL limits), controls fan speeds via direct EC port I/O, adjusts keyboard backlight color/brightness, toggles webcam/microphone, sets display refresh rate, and GPU-side display scaling. No GUI, no TUI, no dependencies beyond libc.

## Build

```bash
# cctl binary only (default)
make

# everything (cctl + kernel modules)
make all
```

## Drivers

This repo includes kernel drivers for TUXEDO/Clevo laptop hardware:

| Module | Description |
|---|---|
| `tuxedo_keyboard` | Keyboard backlight control |
| `clevo_acpi` | Clevo ACPI interface |
| `tuxedo_io` | Hardware I/O (fan/PWM/EC access) |
| `legacygpu` | Legacy GPU power control (optional) |

### Install drivers via DKMS (recommended)

```bash
sudo ./drivers/driverinstall.sh --install
```

Or interactively:

```bash
sudo ./drivers/driverinstall.sh
```

The script auto-detects current state and prompts install or uninstall. Drivers auto-rebuild on kernel updates.

### Make targets

```bash
make drivers           # build drivers in-tree (no DKMS)
make drivers-install   # run driverinstall.sh --install
make drivers-uninstall # run driverinstall.sh --uninstall
make drivers-status    # check driver state
```

The keyboard backlight module uses `force_backlight_type=6` option — set automatically on install.

## Usage

```
   ____ ___  _     ___  ____   ____ ___  _   _ _____ ____   ___  _     
  / ___/ _ \| |   / _ \|  _ \ / ___/ _ \| \ | |_   _|  _ \ / _ \| |    
 | |  | | | | |  | | | | |_) | |  | | | |  \| | | | | |_) | | | | |    
 | |__| |_| | |__| |_| |  _ <| |__| |_| | |\  | | | |  _ <| |_| | |___ 
  \____\___/|_____\___/|_| \_\\____\___/|_| \_| |_| |_| \_\\___/|_____|
```

**Usage:** `cctl <command> [options]`  *(most commands need sudo)*

**PROFILES**
| Command | Description |
|---|---|
| `set <profile>` | Apply profile (no RAPL) |
| `setR <profile>` | Apply profile (with RAPL) |

Profiles: `max`, `cpuperf`, `balanced`, `powersave`, `eco`
setR limits: max(45/90W) · cpuperf(70W) · balanced(35/40W) · eco(9/10W)

**FAN**
| Command | Description |
|---|---|
| `fan auto\|max\|silent` | Set both fans (EC-controlled / full / quiet) |
| `fan <pct>` | Set both fans to duty (21-100%) |
| `fan cpu\|gpu <pct>` | Set individual fan duty (21-100%) |

**DISPLAY** *(no root needed)*
| Command | Description |
|---|---|
| `rr [rate]` | List/set refresh rate (1=high, 2=low) |
| `scale <value>` | GPU-side scaling: factor 0.01-1.0 (e.g. `0.5`=half, `0.75`=1080p), resolution `WxH` (e.g. `1920x1080`), or `off`/`reset` for native |

**KEYBOARD**
| Command | Description |
|---|---|
| `kbc R G B` | Set color via RGB (0-255 per channel, e.g. `kbc 255 0 128`) |
| `kbc #hex` | Set color via hex (e.g. `kbc #ff0080` or `kbc ff0080`) |
| `kbc <name>` | Set color from preset (e.g. `kbc cyan`) |
| `kbb <pct>` | Set brightness (0-100%) |

> `kbcp` still works as an alias for `kbc`.

**SYSTEM**
| Command | Description |
|---|---|
| `turbo <on\|off>` | Turbo boost override |
| `gov <governor>` | CPU governor (`powersave`, `performance`) |
| `epp <value>` | EPP (`performance`, `balance_performance`, `balance_power`, `power`) |
| `rapl <pl1> <pl2>` | RAPL power limits (watts). Use `skip` to omit one limit |
| `rapl skip <pl2>` | Set PL2 only (skip PL1) |
| `rapl <pl1> skip` | Set PL1 only (skip PL2) |
| `rapl <pl2>` | Single arg = PL2 only (shorthand) |
| `mic [on\|off]` | Toggle/set microphone |
| `fnlock <on\|off>` | Fn Lock toggle (Fn key behavior) |
| `webcam [on\|off]` | Toggle/set webcam |

**BATTERY**
| Command | Description |
|---|---|
| `bat` | Show current charge thresholds |
| `bat <start> <stop>` | Set start/stop charge % (sudo) |
| `bat off` | Widest range (start=min, stop=max) |

**NVIDIA** *(needs root)*
| Command | Description |
|---|---|
| `nvidia <on\|off>` | Persistent toggle + initramfs rebuild (`--force` to skip checks) |
| `nvidia load` | Session load (compute modules) |
| `nvidia loadgame` | Session load (all modules incl. drm) |
| `nvidia unload` | Session unload + power off |
| `nvidia status` | Show GPU status & telemetry |
| `nvidia clock <min,max>` | Lock GPU clocks to a range (`reset` to unlock) |
| `nvidia memclock <min,max>` | Lock GPU memory clocks to a range (`reset` to unlock) |
| `nvidia pm <on\|off>` | Toggle persistence mode |
| `nvidia pmclock <min,max>` | Persistence on + lock GPU clocks in one step |
| `nvidia-power [on\|off]` | Hardware D0/D3cold control |

**INFO** *(no root needed)*
| Command | Description |
|---|---|
| `status` | Show all current settings |
| `monitor` | Live CPU/power/fan monitor |

Profiles set all CPU/GPU parameters at once. `setR` variant also applies RAPL power limits (package-0 only — writing to psys/sub-zones causes EC conflict and 0.4GHz throttle on Clevo). Turbo and governor overrides apply immediately but get replaced by the next profile change. The tool dynamically groups hybrid P/E-cores based on CPU topology/max frequency and pins itself to E-cores to minimize game performance impact.

### RAPL examples

```bash
sudo cctl rapl 45 90       # set both PL1=45W, PL2=90W
sudo cctl rapl skip 50     # PL2 only (50W)
sudo cctl rapl 40 skip     # PL1 only (40W)
sudo cctl rapl 50          # one arg = PL2 only (shorthand)
sudo cctl rapl - 50        # skip via dash
sudo cctl rapl none 50     # skip via "none"
```

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
| <pct>  | Both fans to 21-100%         |
| cpu    | CPU fan to 21-100%           |
| gpu    | GPU fan to 21-100%           |

> [!NOTE]
> **EC Byte-Order Quirk:** The Clevo EC interprets the byte order of the two-byte fan speed commands (`cmd 0x99`) contextually. When restoring `auto` mode, cmd `0x99` expects `{ 0xFF, fan_idx }`. When setting a specific duty cycle (`fan_set_duty`, `fan_max_all`, `fan_silent_all`), cmd `0x99` expects `{ fan_idx, duty_value }`. Do not attempt to "normalize" these argument layouts, as this contextual byte-swapping is verified correct on the Clevo P15 hardware.
>
> **GPU Fan Telemetry — Broken ACPI Register:**
> The ACPI FANINFO1 register (`0x63` byte 2) does **not** report GPU fan duty correctly on this Clevo model. It is essentially stuck in a narrow range (~13-19%) regardless of actual GPU fan speed. cctl reads GPU duty from FANINFO2 (`0x64` byte 0) instead, which is accurate. The CPU fan works fine through FANINFO1.


## Keyboard presets

blue, chocolate, coral, cyan, gold, gray, green, indigo, lime, magenta, maroon, navy, off, olive, orange, pink, purple, red, salmon, silver, teal, turquoise, violet, white, yellow


## Nvidia GPU

| Command | Action | Speed |
|---------|--------|-------|
| `nvidia off` | Blacklist + initramfs rebuild + rmmod | ~60s |
| `nvidia on` | Remove blacklist + initramfs rebuild + modprobe | ~60s |
| `nvidia load` | Session-only: wake GPU, load nvidia + nvidia_uvm (2 modules) | Instant |
| `nvidia loadgame` | Session-only: wake GPU, load nvidia + uvm + modeset + drm (4 modules) | Instant |
| `nvidia unload` | Session-only: rmmod all nvidia modules (detects/removes 2 or 4 modules), power off GPU | Instant |
| `nvidia status` | Show boot config, module state, GPU telemetry | — |
| `nvidia clock <min,max>` | Lock GPU clocks to [min,max] via `nvidia-smi -lgc` (`clock reset` unlocks via `-rgc`) | Instant |
| `nvidia memclock <min,max>` | Lock GPU memory clocks via `nvidia-smi -lmc` (`memclock reset` unlocks via `-rmc`) | Instant |
| `nvidia pm <on\|off>` | Toggle persistence mode via `nvidia-smi -pm` | Instant |
| `nvidia pmclock <min,max>` | Enable persistence + lock GPU clocks in one step | Instant |
| `nvidia-power off` | Set GPU hardware to D3cold (powered off) | Instant |
| `nvidia-power on` | Set GPU hardware to D0 (powered on) | Instant |

**Recommended workflow:** Run `nvidia off` once to blacklist and rebuild initramfs (sets boot default to nvidia-off). Then use `nvidia load`, `nvidia loadgame` or `nvidia unload` for instant session-only toggling — the blacklist stays intact, so nvidia remains off on next reboot. Only run `nvidia on` when you want to permanently re-enable nvidia at boot.

`load`/`loadgame`/`unload` require blacklist mode (i.e., `nvidia off` must have been run first). `load` and `loadgame` temporarily move the blacklist aside for modprobe, then restore it. `unload` uses `rmmod` directly to clean up any loaded modules and powers off the GPU via PCI runtime PM (D3cold).

`nvidia-power` controls the GPU hardware power state directly via PCI runtime PM. Useful when no nvidia driver is loaded — without it, the GPU sits in D0 drawing idle power (~5-15W). When the nvidia driver is loaded, it manages its own power state automatically.

`nvidia clock <min,max>` locks the GPU clock range via `nvidia-smi -lgc` (e.g. `cctl nvidia clock 210,1600`), and `nvidia clock reset` unlocks it (`-rgc`). `nvidia memclock <min,max>` does the same for memory clocks (`-lmc`/`-rmc`). On laptops where power limit (`-pl`) and target temperature (`-gtt`) are unsupported, clock capping is the practical way to cut GPU power draw and temperature. `nvidia pm on|off` toggles persistence mode (`-pm`), and `nvidia pmclock <min,max>` combines both (persistence on + clock lock) in one command. Clock ranges are validated (`min <= max`, both > 0); the driver reports unsupported values.

> [!NOTE]
> For display/gaming (Vulkan, PRIME offload, external monitors via nvidia), you can use `nvidia loadgame` directly to load all 4 modules. If you already ran `nvidia load`, you can still manually run `sudo modprobe nvidia_drm`.
>
> [!TIP]
> **Preventing Xorg GPU Lock (Important for i3/WM users):**
> When you run `nvidia loadgame`, the display driver modules are loaded and `/dev/dri/card1` is created. By default, Xorg might automatically detect the new GPU and lock it, which prevents `nvidia unload` from working (producing `Module is in use` errors).
>
> To stop Xorg from claiming the newly loaded GPU, disable `AutoAddGPU` by creating or editing `/etc/X11/xorg.conf.d/10-no-gpu-hotplug.conf`:
> ```
> Section "ServerFlags"
>     Option "AutoAddGPU" "false"
> EndSection
> ```
> With this flag set, Xorg won't lock the GPU, enabling `nvidia unload` to work instantly and flawlessly whenever you're done gaming, without having to close your session.

## Architecture

- **Command dispatch table** — modular `struct command` array instead of nested if-else routing.
- **No shell execution** — uses `fork`/`execvp`/`waitpid` for all subprocesses; no shell injection surface.
- **Hybrid CPU aware** — dynamically detects P-cores vs E-cores by frequency topology, no hardcoded core counts.
- **E-core pinning** — pins itself to E-cores at startup to avoid stealing P-core cycles from foreground workloads.
