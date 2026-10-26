## Commands

### Keyboard Backlight
```bash
cctl kbc <color>                # Set keyboard color: R G B (0-255) or preset name
cctl kbb <pct>                  # Brightness (0-100%)
cctl kbe [effect|stop]          # Start an effect, stop & restore original color/brightness, or show status & list effects
cctl fn [lock|unlock]          # Toggle or set Fn Lock (no arg toggles, or lock/unlock)
```

Color presets: `azure` `blue` `coral` `cyan` `green` `lime` `orange` `pink` `purple` `red` `white` `yellow` `off`

Effect presets (single zone): `breathe` `breathe-cycle` `cycle` `flash` `flash-cycle` `candle` `pulse` `pulse-cycle` `police` `fire` `aurora` `storm` `temp` `ram` `sos` `firecrackers`

- `temp`: CPU temperature is cyan below 40°C. From 40°C it shifts gradually
  from light green to dark green, then through yellow and orange to red as it
  warms. At 90°C or higher, it flashes a red SOS signal.
- `ram`: RAM usage shifts gradually from cyan (up to 25%) through green,
  yellow, and orange to red (above 85%). Red pulses at 95% or higher.

*Manual Status Check*: Run `cctl kbe` to view the current active effect.

### Fan Control
```bash
cctl fan auto|max               # Both fans: EC automatic / 100% full speed
cctl fan silent [--nosafe]      # Quiet mode (forces silent/eco profile first; bypass with --nosafe)
cctl fan <pct> --nosafe         # Set both fans to duty cycle (25-100%, requires --nosafe)
cctl fan cpu|gpu <pct> --nosafe # Set individual fan duty (requires --nosafe)
cctl fan cpu|gpu auto           # Restore individual fan to automatic EC control (independent)
```

### Fan Curves

Fan curves adjust both fans as the laptop heats up. Create a curve, fill in its
temperature and fan-speed points, then start it:

```bash
cctl fancurve                         # Show saved curves and the active curve
cctl fancurve create <name>           # Create a curve and open it for editing
cctl fancurve view <name>             # Check and display a curve
cctl fancurve edit <name>             # Edit a saved curve
cctl fancurve start <name>            # Start or restart a curve
cctl fancurve stop                    # Stop the curve and return fans to automatic control
cctl fancurve del <name> [--noconfirm] # Delete a saved curve
```

Short forms: `s` for `start`, `c` for `create`, `v` for `view`, and `e` for
`edit`.

`create` asks how many points to add, then opens an example for you to edit.
Each point pairs a temperature with a fan speed. For example:

```json
{
  "name": "quiet",
  "hysteresis": 3,
  "interval_ms": 2000,
  "points": [
    { "temp": 40, "fan": 25 },
    { "temp": 60, "fan": 35 },
    { "temp": 80, "fan": 70 },
    { "temp": 95, "fan": 100 }
  ]
}
```

Set temperatures in Celsius from `0` to `95`, in increasing order. Fan speeds
can be `25` to `100` percent. Use 2–16 points. The curve changes fan speed
smoothly between points; below the first point it uses that point's speed, and
above the last point it keeps that point's speed.

`hysteresis` helps prevent frequent speed changes when the temperature
fluctuates. `interval_ms` controls how often the temperature is checked. The
final point can use `"a"` or `"auto"` instead of a percentage to return the
fans to automatic control at that temperature. The curve resumes after the
temperature drops below that point by the hysteresis amount.

For safety, cctl returns the fans to automatic control if the temperature
goes above 95 °C, then resumes the curve after it cools below 90 °C. If the curve
stops unexpectedly, the fans also return to automatic control. Curves require
an up-to-date driver; run `cctl drivers-manage` if cctl asks you to update it.

While a curve is running, it keeps control of both fans. Other manual fan
changes are unavailable until the curve stops. Applying a profile or restoring
a snapshot leaves fan control with the curve. `cctl fan auto` stops the curve
and returns the fans to automatic control.

You cannot edit or delete the curve that is currently running; stop it first.
Deleting a saved curve asks for confirmation; add `--noconfirm` to skip the
prompt. Curves run only for the current boot and are not included in snapshots.

### GPU MUX Switching
Switches between **MSHybrid** (iGPU + dGPU) and **dGPU** (NVIDIA GPU only). A reboot is required to apply the change.

```bash
cctl mux                        # Show current MUX mode (MSHybrid / dGPU) & pending status
cctl mux switch                 # Toggle MUX mode (stages in UEFI NVRAM, reboot to apply)
```

> **X11 hardware quirk:** In **dGPU** mode, the internal display starts at **40 Hz on every boot**, whether dGPU mode was selected in BIOS or with `cctl mux switch`. Run `cctl rr 165` to use the full 165 Hz refresh rate. To apply it automatically, add `cctl rr 165` to your desktop environment's autostart/startup applications. *(X11 sessions only; the autostart method depends on your desktop environment.)*

### Privacy
```bash
cctl webcam [on|off]            # Toggle or set webcam
cctl mic [on|off]               # Toggle or set internal microphone (laptop mic only, needs alsa/amixer)
```

### Battery
```bash
cctl bat                        # Show charge mode and current thresholds
cctl bat <start> <stop>         # Select Custom mode, then set thresholds (e.g. 40 80)
cctl bat standard               # Select Standard charge mode
```

### Info
```bash
cctl status [--json]            # Print all current settings; --json emits a JSON object
cctl cpumon [--json]            # Live CPU, RAM, temperature, power, and fan monitor; JSON streams one sample per line
cctl gpumon [--json]            # Live NVIDIA GPU monitor; --json emits one sample per line
cctl capabilities [--json]      # Show controls detected on this system
cctl --version                  # Print version and exit (also -V)
```

### Snapshots
```bash
cctl snap view                   # List saved snapshot names
cctl snap save <name> [--mask fields]      # Save current settings (alias: s)
cctl snap view <name>            # Show a saved snapshot (alias: v)
cctl snap restore <name> [--mask fields] [--dry-run] # Apply or preview (alias: r)
cctl snap delete <name> [--noconfirm] # Delete a saved snapshot (alias: del)
```

Use `--mask` to omit fields while saving or skip them while restoring. Separate
field names with commas. For example: `cctl snap s mypcwork --mask epp,pl1,turbo`.
Snapshot names can contain letters, numbers, hyphens, and underscores, but no
spaces or other symbols. `snap` is a shorter alias for `snapshot`.
`snap delete` asks for confirmation before removing a snapshot; add
`--noconfirm` to skip the prompt (for example, `cctl snap del mypcwork --noconfirm`).
Add `--dry-run` to `snap restore` to preview the settings and skipped
fields without applying changes. Restoring EC code `2`/`3` sets fans to AUTO;
restoring Turbo ON does so only when the active EC code is `2`/`3`. If a fan
curve is active, restore leaves fan control with the curve and skips the AUTO
change. Add `nosafe` to the restore mask to bypass automatic fan changes
(for example, `cctl snap r mypcwork --mask nosafe`).
Available names are `nvidia_clock`, `nvidia_memclock`,
`ec_code`, `turbo`, `gov`, `epp`, `pl1`, `pl2`, `kbe`, `kbc`, `kbb`,
`fn_lock`, `webcam`, `mic`, `bat`, `rr`, and `scale`; `nosafe` is restore-only.
`kbc` masks keyboard
color, `kbb` brightness, `kbe` effect, and `bat` both battery thresholds.

Snapshots save system settings, CPU power settings, keyboard settings, Fn Lock,
webcam and microphone states, battery thresholds, GPU clock settings, and
display settings. Fan settings are not saved or restored.

### NVIDIA GPU
```bash
cctl gpumon [--json]                     # Live GPU telemetry and power state
cctl nvidia clock [min] <max> | reset   # Lock/unlock GPU clocks (no arg shows max clock; min defaults to 0)
cctl nvidia memclock [min] <max> | reset # Lock/unlock memory clocks (no arg shows max clock; min defaults to 0)
```

The GPU monitor reads PCI power state without waking a suspended GPU. It only
queries NVIDIA GPU telemetry while the dGPU is in D0. Press `p` to pause
NVIDIA polling while waiting for D3cold; cctl fan readings continue during the
wait and while in D3cold. Polling resumes if the GPU later returns to D0.
Press `p` again before D3cold to cancel the wait; press `q` or Ctrl-C to exit.

### Display
> **Note:** Display commands need a native **X11** session — if your desktop doesn't support one (e.g. Wayland), the `DISPLAY` section is hidden from `cctl --help` and the refresh rate from `cctl status`.
>
> `cctl rr` lists every active display and numbers it: the internal panel (`eDP-*`, or the first `DP-*` when the MUX is in dGPU mode) is always **#1** while it is on, other outputs follow as **#2**, **#3**, … When the panel is off, the first active monitor becomes **#1**. `cctl rr <rate>` changes display 1; `cctl rr <rate> --2` changes display 2. Any numeric value is accepted: `xrandr` snaps it to the closest rate that display supports, and cctl prints what was actually applied.
>
> Scaling, profiles and the `powersave` refresh drop target the **internal laptop panel** only. Connected but inactive outputs are listed separately and cannot be targeted.

```bash
cctl rr                         # List displays: number, resolution, current & available rates
cctl rr [<rate>|toggle] [#N]    # Set/toggle a rate; #N defaults to display 1
cctl scale <factor|WxH|off>    # X11 display scaling (0.75, 1920x1080, or off to reset)
```

Example — set the second monitor to 60 Hz without touching the laptop panel:

```bash
cctl rr 60 --2
```

### Profile Individual Overrides
```bash
cctl turbo on|off [--nosafe]    # Turbo ON sets fans to auto only in EC code 2/3
cctl gov powersave|performance  # CPU scaling governor
cctl epp <preference>           # performance, balance_performance, balance_power, power
cctl rapl <pl1> <pl2>           # Set PL1/PL2 in watts (use 'skip' to omit one)
```
>
> **RAPL limits:** PL2 ≤ 115 W always. PL1 ≤ 45 W normally, up to **90 W while EC profile code 2 is active**.

### Application Settings (Options)
Application settings persist across reboots in `/var/lib/cctl/settings`:

```bash
cctl options                            # Show current cctl settings
cctl options streamtext <on|off>        # Show output as it appears, or print it immediately
cctl options default <help|status>      # Action when running bare 'cctl' without arguments (default: help)
```

- **`streamtext`**: When `on`, cctl shows command output as it appears, character-by-character or line-by-line depending on the command. Set it to `off` to print output immediately.
- **`default`**: By default (`help`), typing bare `cctl` prints the full help menu. Setting this to `status` makes typing bare `cctl` execute `cctl status` directly. The full help menu can always still be accessed via `cctl --help` or `cctl help`.
