## Commands

### Keyboard Backlight
```bash
cctl kbc <color>                # Set keyboard color: R G B (0-255) or preset name
cctl kbb <pct>                  # Brightness (0-100%)
cctl kbe [effect|stop]          # Start an effect, stop & restore original color/brightness, or show status & list effects
cctl fn [lock|unlock]          # Toggle or set Fn Lock (no arg toggles, or lock/unlock)
```

Color presets: `blue` `chocolate` `coral` `cyan` `gold` `gray` `green` `indigo` `lime` `magenta` `maroon` `navy` `olive` `orange` `pink` `purple` `red` `salmon` `silver` `teal` `turquoise` `violet` `white` `yellow` `off`

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
cctl snap delete <name>          # Delete a saved snapshot (alias: del)
```

Use `--mask` to omit fields while saving or skip them while restoring. Separate
field names with commas. For example: `cctl snap s mypcwork --mask epp,pl1,turbo`.
Snapshot names can contain letters, numbers, hyphens, and underscores, but no
spaces or other symbols. `snap` is a shorter alias for `snapshot`.
Add `--dry-run` to `snap restore` to preview the settings and skipped
fields without applying changes. Restoring EC code `2`/`3` sets fans to AUTO;
restoring Turbo ON does so only when the active EC code is `2`/`3`. Add
`nosafe` to the restore mask to bypass the automatic fan change (for example,
`cctl snap r mypcwork --mask nosafe`).
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
> Only the display panel's preconfigured refresh rates are supported — run `cctl rr` to list the valid ones for your screen and pick from that list.

```bash
cctl rr                         # List all supported refresh rates
cctl rr [1|2|<rate>]            # Set refresh rate (1=highest, 2=lowest, or explicit value like 60 or 144)
cctl scale <factor|WxH|off>    # X11 display scaling (0.75, 1920x1080, or off to reset)
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
cctl options streamtext <on|off>        # Enable or disable character/line-streamed terminal output
cctl options default <help|status>      # Action when running bare 'cctl' without arguments (default: help)
```

- **`streamtext`**: When `on`, cctl animates terminal text with character- or line-by-line streaming. Set to `off` for instantaneous plain output.
- **`default`**: By default (`help`), typing bare `cctl` prints the full help menu. Setting this to `status` makes typing bare `cctl` execute `cctl status` directly. The full help menu can always still be accessed via `cctl --help` or `cctl help`.
