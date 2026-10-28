---
name: cctl
description: Operate the installed cctl command-line application to inspect or change laptop power, fan, keyboard, display, battery, and GPU settings.
---

# Use the cctl application

Use this skill when a user asks an agent to inspect or operate the installed `cctl` application. It is a command-use guide, not a guide for developing cctl.

## Start with the installed command

Use the user's installed `cctl` executable. Check its available commands and current state when useful:

```sh
cctl --version
cctl --help
cctl --am            # Print this agent skill
cctl status
```

Use the help output for exact command syntax supported by that installation. For supported hardware, `cctl` commands generally request privilege elevation when needed; run `cctl ...` as the user and do not add `sudo` by default. If cctl reports that a command needs root, follow its instruction. This guide is embedded in cctl and printed by `cctl --am`; it reflects cctl 4.9+ behavior. Check `cctl --version` and defer to the installed help on older builds.

When asked to inspect, prefer read-only commands such as `status`, `options`, `gov`, `epp`, `fancurve`, `snap view`, and monitor one-shots. When asked to make a change, carry out that requested change, then use a suitable read-only command to report the resulting state if one is available. Don't imply a setting was applied unless command output confirms it.

## Power profiles and CPU controls

- `cctl set <profile>` applies a built-in or custom power profile. Use `cctl set` to see built-in names; custom profiles can be managed with `cctl editconf`.
- `cctl gov` and `cctl epp` show the current value and the values available on this machine. Choose only from the values cctl reports; supported choices depend on the active CPU driver.
- `cctl turbo on|off` controls Turbo. `cctl rapl <pl1> <pl2>` controls CPU power limits; consult help for units and accepted `skip` syntax.
- EC profile codes 2 and 3 normally trigger fan-auto safety when applied. `--nosafe` bypasses safety where supported; use it only when the user explicitly asks for that behavior.

EC profile codes select the laptop's embedded-controller power mode:

- `0`: silent
- `1`: power saving
- `2`: high performance
- `3`: standard

Built-in mapping: `max` uses 2; `cpuperf` and `balanced` use 3; `powersave`
uses 1; `silent` uses 0.

For a custom profile, the EC code is the first numeric field after the name.
Codes `2` and `3` also affect fan/TDP behavior, not just the Linux governor or
EPP. Under normal use, choose `skip` unless the user explicitly wants to change
the EC mode.

## Fan control and curves

Ordinary fan commands:

```sh
cctl fan auto       # Return both fans to EC automatic control
cctl fan max        # Request full fan speed
```

Manual fixed-duty fan controls are experimental. They are hidden and unavailable until the user enables them with:

```sh
cctl options experimental-fan on
```

A fixed percentage does not rise automatically with temperature. Do not enable experimental fan controls or set fixed duty unless the user requests it. Prefer `cctl fan auto` for normal thermal control.

After opting in, the experimental forms are:

```sh
cctl fan silent [--nosafe]          # Quiet mode; applies silent/eco unless --nosafe
cctl fan <pct> --nosafe             # Both fans, 25-100%
cctl fan cpu|gpu <pct> --nosafe     # One fan, 25-100%
cctl fan cpu|gpu auto               # Restore one fan to EC auto
```

Fixed duty ignores temperature and can overheat the laptop. Stop any active fan
curve first, and use these forms only when the user explicitly requests them.

For temperature-based fan control, use a saved curve:

```sh
cctl fancurve                    # Show saved curves and active curve
cctl fancurve create <name>      # Create and edit a curve
cctl fancurve view [name]        # List profiles, or validate and display one
cctl fancurve edit|e <name>      # Edit a saved curve
cctl fancurve start|s <name>     # Run it for this boot
cctl fancurve log                # Follow live diagnostics; Ctrl-C exits the log view
cctl fancurve stop|st            # Stop and return fans to EC auto
cctl fancurve del <name>         # Delete an inactive curve (--noconfirm skips prompt)
```

Aliases `c`, `v`, `st`, `s`, and `e` mean create, view, stop, start, and edit respectively; `fanc` is a shorter command name. Curves control both fans while active; stop the curve before manual fan changes or driver maintenance. cctl does not auto-start curves at boot, but a snapshot can include the active curve configuration and start it during restore. The user can edit curve points in the editor opened by `create` or `edit`. Read `cctl fancurve` help for details and use `cctl fancurve log` when diagnosing a running curve.

### Non-interactive fan curves

`fancurve create` is interactive unless `--am` is supplied. Print the
authoritative rules first:

```sh
cctl fancurve create --am
```

Create a curve in one validated command:

```sh
cctl fancurve create --am quiet 3 3 2000 45 25 60 45 75 a
```

Arguments are `<name> <points> <hysteresis> <ms> <temp1> <fan1> ...`.
`--am` may also appear after the name. Use 2-16 points with temperatures 0-95
in strictly increasing order, fan 25-100, and `a`/`auto` only on the final
point. `hysteresis` is 0-20 and `ms` is 250-60000. On invalid input cctl prints
`Invalid fan curve: <reason>` and saves nothing.

To edit a curve, view it, delete it, then create it again:

```sh
cctl fancurve view quiet
cctl fancurve del quiet --noconfirm
cctl fancurve create --am quiet ...
```

Do not use `fancurve create` without `--am` when acting non-interactively.
`fancurve edit` is always interactive; to change a curve, view it, delete it,
then create it again with `--am`.

### Non-interactive custom profiles

`editconf` is interactive unless `--am` is supplied. Print the authoritative
rules first:

```sh
cctl editconf --am
```

Create a profile with the eight validated values:

```sh
cctl editconf --am --create quietcpu skip skip powersave balance_power skip skip skip
```

Arguments are `<name> <ec> <turbo> <gov> <epp> <pl1> <pl2> <refresh>`. Use
`skip` for any value that should remain unchanged.

Delete a profile with the same eight values, or with just its name:

```sh
cctl editconf --am --delete quietcpu skip skip powersave balance_power skip skip skip
cctl editconf --am --delete quietcpu
```

`--create` appends a new row and fails if the name already exists. To edit a
custom profile, view it with `cctl set`, delete it, then create it again. Do not
use `editconf` without `--am` when acting non-interactively.

Each non-comment row uses 5, 7, or 8 whitespace-separated fields:

```text
name ec_code turbo governor epp [pl1_watts pl2_watts [refresh_rate]]
```

- `name`: max 31 characters from `A-Za-z0-9_-`; must be unique and not a
  built-in or `eco`. At most 32 custom profiles are allowed.
- `ec_code`: 0-3 or `skip`; `turbo`: 0/1 or `skip`.
- `governor` and `epp`: values from `cctl gov` and `cctl epp`, or `skip`.
- `pl1_watts`: 1-45W, or up to 90W with EC code 2 or EC `skip`; `pl2_watts`:
  1-115W and greater than PL1. Use `skip` to leave limits unchanged.
- `refresh_rate`: a rate such as `165`, `lowest`, or `highest`, or `skip`.

On invalid input cctl prints the same error as the editor, for example
`Invalid custom profile: line 1: EC profile code must be 0-3 or 'skip'`, and
saves nothing. Fix the arguments and retry.

## Keyboard lighting

- `cctl kbc <color>` sets the keyboard color; `cctl kbc` lists presets. Colors include names such as `red`, `cyan`, or `white`, and hex values such as `#ff3040`.
- `cctl kbb <percent>` sets brightness from 0-100.
- `cctl kbe` lists effects and status; `cctl kbe <effect>` starts one and `cctl kbe stop` stops it. Effects include `breathe`, `cycle`, `flash`, `candle`, `pulse`, `police`, `fire`, `aurora`, `storm`, `temp`, `ram`, `sos`, and `firecrackers`.
- `cctl kbn flash|pulse|sos <color> [times]` plays a temporary notification and restores the previous keyboard state. `cctl kbn police [<color1> <color2> [times]]` alternates colors.
- `cctl fn [lock|unlock]` toggles or sets Fn Lock.

Keyboard notification colors use the same preset names or hex values; `times` is 1-20. For exact accepted arguments and any build-specific effects, use `cctl kbe`, `cctl kbn`, and `cctl --help`.

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
an active curve, while a snapshot with a saved curve replaces it.
`--mask fancurve` preserves the current curve state, and `--mask bat` masks both
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
- `cctl webcam` and `cctl mic` toggle the device when called without an
  argument; use `cctl status` to inspect state, or `on`/`off` to set it
  explicitly.
- NVIDIA clock commands and `cctl gpumon` are available only when supported by the installed cctl build and hardware. NVIDIA-enabled builds may also provide `nvidia on|off|load|loadgame|unload|status`; check `cctl --help`.

## Application preferences and drivers

- `cctl options` shows persistent cctl preferences:
  - `cctl options streamtext <on|off>`
  - `cctl options default <help|status>`
  - `cctl options experimental-fan <on|off>`

  Change only the option the user requests.
- `cctl drivers-manage` manages the supported driver package. Driver install/uninstall can affect loaded kernel modules; first stop any active fan curve. Follow cctl's prompts and report any module-in-use or reboot notices.
- `cctl install [--force]` installs or upgrades cctl system-wide; `cctl uninstall` removes cctl, its saved data, and its sudo rule. Run these only when the user requests installation or removal.

## Keep actions clear

Explain the expected effect in plain language for lasting or disruptive changes such as a fixed fan percentage, firmware MUX switch, driver removal, or snapshot restore. Do not trigger a reboot, bypass a safety check, or use a confirmation-skipping option unless the user asked for it. For settings changes the user explicitly requested, run the matching cctl command without asking them to reconfirm the request.
