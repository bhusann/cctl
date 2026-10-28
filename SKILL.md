---
name: cctl
description: Operate the installed cctl command-line application to inspect or change laptop power, fan, keyboard, display, battery, and GPU settings.
---

# Use the cctl application

Use this skill when a user asks an agent to inspect or operate the installed `cctl` application. It is a command-use guide, not a guide for developing cctl.

## Start with the installed command

Use the installed `cctl` executable. Check `cctl --version`, `cctl --help`, and `cctl status` when useful; `cctl --am` prints this guide. Run `cctl` as the user without `sudo`; it requests elevation when needed. This guide matches 5.0+; defer to the installed help for exact syntax on other builds.

When inspecting, prefer read-only commands such as `status`, `options`, `gov`, `epp`, `fancurve`, `snap view`, and monitor one-shots. After a requested change, verify the result with a read-only command; don't claim success without confirming output.

If the user asks to create a fan curve or custom profile but the request is vague, and you have a question tool available, use it to ask for the values you need before creating anything. Collect the required curve points, temperatures, fan speeds, or profile settings; do not guess values that affect thermals or power.

## Power profiles and CPU controls

- `cctl set <profile>` applies a built-in or custom profile; `cctl set` lists available names.
- `cctl gov` and `cctl epp` show the current value and supported choices. Use only values cctl reports.
- `cctl turbo on|off` controls Turbo. `cctl rapl <pl1> <pl2>` changes CPU power limits.
- EC codes `2` and `3` trigger fan-auto safety when applied. `--nosafe` bypasses it; use only on explicit request.

EC profile codes are firmware modes managed by the laptop's built-in embedded controller (EC), not Linux governor or EPP settings.

| EC code | Mode | EC default PL1 | EC default PL2 | GPU power cap |
| ---: | --- | ---: | ---: | ---: |
| `0` | Silent | 15 W | 30 W | 70 W |
| `1` | Power saving | 15 W | 30 W | 70 W |
| `2` | High performance | 90 W | 115 W | 100 W (unlocked) |
| `3` | Standard | 45 W | 115 W | 70 W |

All modes use the EC's TCC thermal target. The table shows EC defaults; after applying a code you can adjust CPU power with `cctl rapl` and limit NVIDIA GPU clocks with `cctl nvidia clock`/`memclock` on supported builds. The EC may still enforce lower limits.

To raise the GPU to 100 W, use EC code `2`; cctl has no separate GPU-wattage command. Code `2` also changes CPU PL1/PL2 and TCC, and fan-auto safety applies. Explain and confirm if the request is ambiguous. Use `cctl set max`, or a custom profile with EC code `2` and the other fields `skip` when only the EC mode should change. NVIDIA clock commands cap clocks, not wattage.

Built-in mapping: `max` uses 2; `cpuperf`/`balanced` use 3; `powersave` uses 1; `silent` uses 0. In a custom profile, the EC code is the first numeric field; use `skip` unless intentionally changing the EC mode.

### Measured load behavior

Full-load tests on the tested 10-core, 16-thread laptop (results vary with cooling and workload): EPP determined performance; the same EPP measured the same under both governors, and the `performance` governor locks EPP to `performance`. Code `2` with Turbo + `performance` EPP peaked at 85-87 W and sustained 75-76 W near 98°C; `balance_performance` sustained 63-64 W near 91°C. Code `3` peaked at 67-70 W at 87°C and sustained 45 W after its 28-second TAU; `balance_performance` peaked at 63 W and sustained 45 W. Codes `0` and `1` measured the same: 30 W peak, 15 W after the 8-second TAU, no PROCHOT; Turbo off was 24-25 W. `balance_power` kept codes `2`/`3` near 22 W while codes `0`/`1` stayed at 15 W; `power` measured about 12 W across all codes.

## Fan control and curves

Ordinary fan commands:

```sh
cctl fan auto       # Return both fans to EC automatic control
cctl fan max        # Request full fan speed
```

Manual fixed-duty fan control is experimental and hidden until `cctl options experimental-fan on`. It ignores temperature and can overheat; use it only on explicit request and stop any active curve first. After opting in:

```sh
cctl fan silent [--nosafe]          # Quiet mode; applies silent/eco unless --nosafe
cctl fan <pct> --nosafe             # Both fans, 25-100%
cctl fan cpu|gpu <pct> --nosafe     # One fan, 25-100%
cctl fan cpu|gpu auto               # Restore one fan to EC auto
```

For temperature-based fan control, use a saved curve:

```sh
cctl fancurve                    # Show saved curves and active curve
cctl fancurve create <name>      # Create and edit a curve
cctl fancurve view [name]        # List profiles, or validate and display one
cctl fancurve edit|e <name>      # Edit a saved curve
cctl fancurve start|s <name>     # Run it for this boot
cctl fancurve log                # Follow diagnostics, waiting across curve restarts; Ctrl-C exits
cctl fancurve stop|st            # Stop and return fans to EC auto
cctl fancurve del <name>         # Delete an inactive curve (--noconfirm skips prompt)
```

Aliases: `c` create, `v` view, `s` start, `st` stop, `e` edit; `fanc` is shorter. Curves own both fans while active; stop one before manual fan changes or driver maintenance. Curves do not auto-start at boot, but snapshots can restore one.

`cctl fancurve log` is continuous: it waits for a curve, follows restarts, and runs until interrupted with Ctrl-C. In non-interactive use, bound it with `timeout`, for example `timeout 5s cctl fancurve log`; exit status 124 is expected when the timeout stops it.

### Non-interactive fan curves

`fancurve create` is interactive unless `--am` is supplied. Print the rules with `cctl fancurve create --am`, then create a validated curve:

```sh
cctl fancurve create --am quiet 3 3 2000 45 25 60 45 75 a
```

Arguments are `<name> <points> <hysteresis> <ms> <temp1> <fan1> ...`. `--am` may also appear after the name. Use 2-16 points with temperatures 0-95 in strictly increasing order, fan 25-100, and `a`/`auto` only on the final point. `hysteresis` is 0-20 and `ms` is 250-60000. Invalid input prints `Invalid fan curve: <reason>` and saves nothing. `fancurve edit` is always interactive; to change a curve, view it, delete it, then recreate it with `--am`:

```sh
cctl fancurve view quiet
cctl fancurve del quiet --noconfirm
cctl fancurve create --am quiet ...
```

### Non-interactive custom profiles

`editconf` is interactive unless `--am` is supplied; `cctl editconf --am` prints the rules. Create with eight validated values:

```sh
cctl editconf --am --create quietcpu skip skip powersave balance_power skip skip skip
```

Arguments are `<name> <ec> <turbo> <gov> <epp> <pl1> <pl2> <refresh>`; use `skip` to leave a value unchanged. Delete by name, or pass the same fields:

```sh
cctl editconf --am --delete quietcpu
```

`--create` appends a row and fails on duplicate names. To edit, preview with `cctl set <name> --dry-run`, delete, then recreate. Do not use `editconf` non-interactively without `--am`.

Each non-comment row uses 5, 7, or 8 whitespace-separated fields:

```text
name ec_code turbo governor epp [pl1_watts pl2_watts [refresh_rate]]
```

- `name`: max 31 characters from `A-Za-z0-9_-`; unique and not a built-in or `eco`. At most 32 custom profiles.
- `ec_code`: 0-3 or `skip`; `turbo`: 0/1 or `skip`.
- `governor` and `epp`: values from `cctl gov` and `cctl epp`, or `skip`.
- `pl1_watts`: 1-45W, or up to 90W with EC code 2 or EC `skip`; `pl2_watts`: 1-115W and greater than PL1. Use `skip` to leave limits unchanged.
- `refresh_rate`: a rate such as `165`, `lowest`, or `highest`, or `skip`.

Custom profiles are stored in `/var/lib/cctl/profiles.conf` (root-owned, world-readable). `cctl set` lists them; `cctl set <name> --dry-run` previews one. Manage them with `cctl editconf`; the older `/etc/cctl/profiles.conf` is not read. Invalid input prints `Invalid custom profile: line N: <reason>` and saves nothing.

## Keyboard lighting

- `cctl kbc <color>` sets the keyboard color; `cctl kbc` lists presets. Colors include names such as `red`, `cyan`, or `white`, and hex values such as `#ff3040`.
- `cctl kbb <percent>` sets brightness from 0-100.
- `cctl kbe` lists effects and status; `cctl kbe <effect>` starts one and `cctl kbe stop` stops it. Effects include `breathe`, `cycle`, `flash`, `candle`, `pulse`, `police`, `fire`, `aurora`, `storm`, `temp`, `ram`, `sos`, and `firecrackers`.
- `cctl kbn flash|pulse|sos <color> [times]` plays a temporary notification and restores the previous keyboard state. `cctl kbn police [<color1> <color2> [times]]` alternates colors.

Keyboard notification colors use the same preset names or hex values; `times` is 1-20. For exact arguments and build-specific effects, use `cctl kbe`, `cctl kbn`, and `cctl --help`.

## Monitoring and status

```sh
cctl status [--json]
cctl cpumon --json       # One JSON sample
cctl gpumon --json       # One JSON sample
timeout 3s cctl cpumon --live --interval 1   # Live JSON, every 1 second
timeout 3s cctl gpumon --live
```

`--json` without `--live` prints once. `--interval SEC` changes the live CPU
polling interval (default 0.5 seconds). `--live` implies `--json` and keeps
running until interrupted, so it holds the terminal. In non-interactive use,
always bound it with `timeout` (for example `timeout 3s`); exit status 124 is
expected when the timeout stops it. The GPU monitor may report limited NVIDIA
telemetry when the dGPU is asleep; do not force the GPU awake solely to obtain
a sample unless the user asks.

## Snapshots

Snapshots save and restore supported settings. They include the active fan-curve
configuration but not manual fan duty:

```sh
cctl snap view
cctl snap save <name>
cctl snap view <name>
cctl snap restore <name> --dry-run   # Preview before applying
cctl snap restore <name>
cctl snap delete <name>
```

Restore applies the saved curve state last: a snapshot with no saved curve stops
an active curve, while a snapshot with a saved curve replaces it. Restore first
sets both fans to AUTO even when a curve is active; `--mask nosafe` skips that
step. `--mask fancurve` preserves the current curve state, and `--mask bat` masks both
battery charge mode and thresholds. Use `--dry-run` when the user asks to review
what a restore would change. Deletion normally asks for confirmation; add
`--noconfirm` to skip it when the user explicitly requests that.

Available mask fields are:

```text
nvidia_clock nvidia_memclock ec_code turbo gov epp pl1 pl2
webcam mic rr scale bat kbe kbc kbb fn_lock fancurve nosafe
```

For `save`, a masked field is recorded as skipped. For `restore`, it leaves
that current setting unchanged. `nosafe` is restore-only and bypasses
the initial fan-auto safety step. `--dry-run` is restore-only. Snapshot names
use letters, numbers, `-`, and `_`, up to 64 characters.

## Display, MUX, battery, and privacy

- `cctl rr` lists displays; `cctl rr <rate>` changes the primary display rate. `cctl rr toggle` switches between available rates; `cctl rr <rate> --2` targets display #2. Rates are matched to the closest supported value. `cctl scale <factor|WxH>` scales the internal panel; `cctl scale off` restores native 1:1. These commands require native X11; they may be unavailable under Wayland.
- `cctl mux` shows the GPU MUX mode. `cctl mux switch` stages the other mode in firmware and requires a reboot; it does not switch immediately. If the user requests a switch, explain that reboot is required and follow cctl's confirmation prompt. Do not reboot unless the user asks.
- `cctl bat` shows battery charge mode, thresholds, and available values. `cctl bat <start> <stop>` selects Custom mode and sets thresholds; `cctl bat standard` returns to the standard charge mode.
- To check webcam, microphone, or Fn Lock status, use `cctl status`. `cctl webcam` and `cctl mic` without an argument toggle the device, and `cctl fn` without an argument toggles Fn Lock. Use `cctl webcam on|off`, `cctl mic on|off`, or `cctl fn lock|unlock` to set the state explicitly; otherwise the bare command reverses the current state.
- NVIDIA clock commands and `cctl gpumon` are available only when supported by the installed cctl build and hardware. NVIDIA-enabled builds may also provide `nvidia on|off|load|loadgame|unload|status`; check `cctl --help`.

## Application preferences and drivers

- `cctl options` shows persistent cctl preferences:
  - `cctl options streamtext <on|off>`
  - `cctl options default <help|status>`
  - `cctl options experimental-fan <on|off>`

  Change only the option the user requests.
- `cctl drivers-manage` manages the supported driver package. Driver install/uninstall can affect loaded kernel modules; first stop any active fan curve. Follow cctl's prompts and report any module-in-use or reboot notices.
- `cctl install [--force]` installs or upgrades cctl system-wide; `cctl uninstall` removes cctl, its saved data, and its sudo rule. Run these only when the user requests installation or removal.

## Files and runtime state

Persistent cctl data lives under `/var/lib/cctl`:

- `profiles.conf`: custom profiles; manage with `cctl editconf`.
- `settings`: saved `cctl options` values.
- `fancurves/*.json`: saved curves; inspect with `cctl fancurve`.
- `snapshots/<name>.conf`: named snapshots. The directory is root-only (0700); use `cctl snap view`.
- `drivers.tar.gz`: verified driver cache for `drivers-manage`.

Runtime state under `/run` is cleared on reboot:

- `/run/cctl/mode`: NVIDIA clock locks.
- `/run/cctl_curve.state`: active curve PID, start time, name, and config (0600); snapshots read it.
- `/run/cctl_curve.lock`: single fan-curve worker lock.
- `/run/cctl_curve.fifo`: diagnostic FIFO for `fancurve log`.
- `/run/cctl_kbe.state` / `.lock`: active keyboard effect and worker lock.

These files are cctl-owned; do not edit them manually. Read them only for diagnosis, and use the matching cctl command instead.

## Keep actions clear

Explain the expected effect in plain language for lasting or disruptive changes such as a fixed fan percentage, firmware MUX switch, driver removal, or snapshot restore. Do not trigger a reboot, bypass a safety check, or use a confirmation-skipping option unless the user asked for it. For settings changes the user explicitly requested, run the matching cctl command without asking them to reconfirm the request.
