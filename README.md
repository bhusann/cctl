# **CCTL**

Lightweight CLI tool for Clevo P15 laptop performance management. Sets CPU power profiles (governor, turbo boost, EPP, RAPL limits), controls fan speeds via direct EC port I/O, adjusts keyboard backlight color/brightness, toggles webcam/microphone, and sets display refresh rate. No GUI, no TUI, no dependencies beyond libc.

## Build

```bash
gcc -o cctl cctl.c -Os -s
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
sudo ./cctl nvidia <on|off>     # Nvidia persistent toggle: blacklist + initramfs rebuild (needs root)
sudo ./cctl nvidia <load|loadgame|unload> # Nvidia session-only: instant modprobe/rmmod (blacklist mode, needs root)
sudo ./cctl nvidia status       # Nvidia GPU status & telemetry (no root needed)
sudo ./cctl nvidia-power [on|off] # Nvidia GPU hardware power: D0 (on) / D3cold (off) (needs root)
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


## Nvidia GPU

| Command | Action | Speed |
|---------|--------|-------|
| `nvidia off` | Blacklist + initramfs rebuild + rmmod | ~60s |
| `nvidia on` | Remove blacklist + initramfs rebuild + modprobe | ~60s |
| `nvidia load` | Session-only: wake GPU, load nvidia + nvidia_uvm (2 modules) | Instant |
| `nvidia loadgame` | Session-only: wake GPU, load nvidia + uvm + modeset + drm (4 modules) | Instant |
| `nvidia unload` | Session-only: rmmod all nvidia modules (detects/removes 2 or 4 modules), power off GPU | Instant |
| `nvidia status` | Show boot config, module state, GPU telemetry | — |
| `nvidia-power off` | Set GPU hardware to D3cold (powered off) | Instant |
| `nvidia-power on` | Set GPU hardware to D0 (powered on) | Instant |

**Recommended workflow:** Run `nvidia off` once to blacklist and rebuild initramfs (sets boot default to nvidia-off). Then use `nvidia load`, `nvidia loadgame` or `nvidia unload` for instant session-only toggling — the blacklist stays intact, so nvidia remains off on next reboot. Only run `nvidia on` when you want to permanently re-enable nvidia at boot.

`load`/`loadgame`/`unload` require blacklist mode (i.e., `nvidia off` must have been run first). `load` and `loadgame` temporarily move the blacklist aside for modprobe, then restore it. `unload` uses `rmmod` directly to clean up any loaded modules and powers off the GPU via PCI runtime PM (D3cold).

`nvidia-power` controls the GPU hardware power state directly via PCI runtime PM. Useful when no nvidia driver is loaded — without it, the GPU sits in D0 drawing idle power (~5-15W). When the nvidia driver is loaded, it manages its own power state automatically.

> [!NOTE]
> For display/gaming (Vulkan, PRIME offload, external monitors via nvidia), you can use `nvidia loadgame` directly to load all 4 modules. If you already ran `nvidia load`, you can still manually run `sudo modprobe nvidia_drm`.
>
> [!TIP]
> **Preventing Xorg GPU Lock (Important for i3/WM users):**
> When you run `nvidia loadgame`, the display driver modules are loaded and `/dev/dri/card1` is created. By default, Xorg might automatically detect the new GPU and lock it, which prevents `nvidia unload` from working (producing `Module is in use` errors).
>
> To stop Xorg from claiming the newly loaded GPU, disable `AutoAddGPU` by creating or editing `/etc/X11/xorg.conf.d/10-no-gpu-hotplug.conf`:
> ```xorg
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
