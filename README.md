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
| max        | 3    | ON    | perf      | performance        | 45W / 90W    |
| cpuperf    | 2    | ON    | perf      | performance        | 70W          |
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
