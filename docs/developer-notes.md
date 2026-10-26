# Developer Notes

This document records code structure, hardware-specific behavior, and
implementation constraints that are useful when changing cctl. Unless noted
otherwise, hardware observations refer to the tested Colorful Evol P15.

## Source Layout and Build

- `cctl.c` includes subsystem fragments from `src/` in this order: core,
  controls, platform, keyboard, fan curves, telemetry, runtime, NVIDIA,
  snapshots, commands, and driver management. They form one translation unit
  so private hardware helpers can be shared without exporting symbols. Keep
  subsystem changes in their owning fragment.
- The Makefile builds `cctl` from `cctl.c`, `src/cctl_cpumon.c`, and
  `src/cctl_gpumon.c`. The separate monitor programs share code through
  `src/cctl_monitor_common.h` and `src/cctl_shared.h`. The `experimental`
  target enables the optional NVIDIA management commands with `CCTL_NVIDIA`.
- `src/cctl_fancurve.inc` is included before snapshot and command handlers;
  its private worker and state helpers are available to those subsystems.

## EC Profiles, CPU Power, and Thermal Reporting

### Live EC profile detection

`cctl_live_ec_state()` reads the recognized TCC offset from processor thermal
sysfs and falls back to the temperature-target MSR when needed. On the tested
machine, the offsets map as follows:

| TCC offset | EC code | Profile | PROCHOT temperature |
| ---: | ---: | --- | ---: |
| 2°C | 2 | High performance | 98°C |
| 13°C | 3 | Standard | 87°C |
| 15°C | 1 | Powersave | 85°C |
| 10°C | 0 | Silent | 90°C |

`cctl status` prefers a recognized live TCC offset. It falls back to the
`tuxedo_io` profile cache only when the offset cannot identify a profile. The
cache records profile writes made through `tuxedo_io` since module load; it
does not observe Fn-key, firmware, or other driver changes. A cache value of
`-1` means unknown and is omitted from the status detail. Cache reads are
version-gated because older drivers silently accept unknown ioctls.

The mode label in `cctl status` is inferred from the EC code and current CPU
governor/EPP; it is not a separately stored EC mode. The runtime state file
does not contain an EC profile code.

The profile names, TCC offsets, and trip temperatures above are specific to
the tested EC and CPU. The load-test README uses the live
`tcc_offset_degree_celsius` value; P-core and E-core frequencies there are in
MHz.

### `cpumon` thermal state

The `PROCHOT` value in JSON is inferred from the live EC/TCC state. The
terminal `STATE` label does not classify temperature against those profile
thresholds. It uses live hardware throttle flags when available and treats
power readings near RAPL limits as estimates. Current classification order in
`src/cctl_cpumon.c` is:

1. `THERMAL THROTTLE` when a live thermal-throttle flag is set.
2. `RECENT THERMAL EVENT` when the package throttle counter increased within
   the last two seconds.
3. `POWER LIMIT ACTIVE` when the hardware power-limit flag is set.
4. `NEAR PL2 (est.)` at 98% of the reported PL2 value or higher.
5. `ABOVE PL1 (est.)` above 102% of PL1.
6. `NEAR PL1 (est.)` at 90% of PL1 or higher while total CPU utilization
   exceeds 60%.
7. `IDLE` below 10% utilization. Otherwise, `NORMAL` appears only when the
   hardware status read succeeded; if it failed, the display says `STATUS N/A`.

The monitor reads the RAPL TAU from
`/sys/class/powercap/intel-rapl:0/constraint_0_time_window_us`; it does not
assume a fixed time window. Hardware throttle flags come from
`IA32_PACKAGE_THERM_STATUS`, with per-core `IA32_THERM_STATUS` as fallback.
The package power-limit status is bit 10; the current thermal status is bit 0.
Threshold estimates compare sampled package power with the configured RAPL
limits and do not prove that a limit caused throttling.

### Governor, EPP, and snapshots

- With the tested Intel `intel_pstate` setup, the `performance` governor locks
  EPP to `performance`. The `powersave` governor allows the configured EPP to
  guide clock scaling. Built-in profiles therefore use `powersave` and vary
  EPP.
- During snapshot restore, a saved EPP that cannot be applied under the live
  `performance` governor causes cctl to switch to `powersave` when the
  governor was not itself saved or masked. Masking EPP avoids that adjustment.
- EC profile codes 2 and 3 require fan-auto safety when applied through
  `cctl set`; Turbo ON requires it when the current EC code is 2 or 3.
  `--nosafe` bypasses this. If a fan curve is active, profile and Turbo safety
  steps are skipped so the curve retains fan control.
- Snapshot restore follows the same fan-safety policy. It skips the AUTO step
  while a curve is active; `--dry-run` reports this. Code is in
  `cctl_snapshot_apply_fan_safety()`.

### RAPL limits

- Writes are restricted to package 0 (`/sys/class/powercap/intel-rapl:0`),
  after verifying that constraint 0 is `long_term`; PL1 goes to constraint 0
  and PL2 to constraint 1. Do not write sub-zones or `psys`. On the tested
  laptop, writing those zones caused an EC conflict and a 0.4 GHz CPU throttle;
  treat this as a hardware-specific observation.
- PL1 is limited to 45 W normally and 90 W while EC code 2 is active. The EC
  may enforce lower defaults in codes 0, 1, and 3. PL2 is capped at 115 W and
  must remain greater than PL1. The invariant is enforced by the command,
  custom profile loading, snapshot restore, and `set_rapl_limits()`.
- If a snapshot requests PL1 above 45 W without applying EC code 2, restore
  skips PL1 and can still apply PL2.
- `cctl set max` prints a model-specific advisory that sustained CPU power
  can reach 60–90 W and temperatures can approach 98°C under heavy load. It
  recommends `cpuperf` or `balanced` for lighter use.

## GPU Monitoring

- `gpumon` runs `nvidia-smi` telemetry queries only while the dGPU is in PCI
  power state D0. The `p` key pauses or resumes NVIDIA polling; cctl fan
  readings continue while polling is paused or the GPU is in D3cold.
- The effective GPU power limit uses `enforced.power.limit`, then
  `power.limit`, then `power.max_limit` as fallbacks. `power.max_limit` is
  retained separately for the gauge scale and JSON output. Avoid hardcoding
  example wattages; limits vary by profile and hardware.
- On the tested EC, the GPU fan duty in `FANINFO1` is stuck near 15%. Read GPU
  duty from the low byte of `FANINFO2` instead.

## Fan Control

Fan control uses two paths:

- `tuxedo_io` handles both-fan duty writes (`W_CL_FANSPEED`) and automatic
  control (`W_CL_FANAUTO`). The all-fan automatic-control mask is `0x0f`.
  `W_CL_FANSPEED` sets both channels to manual duty together.
- Direct EC port I/O handles individual fan duty and the MAX/SILENT modes. The
  protocol uses command port `0x66`, data port `0x62`, and waits for the input
  buffer to clear before each write. Individual fan duty uses command `0x99`
  with `{ fan_index, raw_duty }`, where CPU is 1, GPU is 2, and
  `raw_duty = percent * 255 / 100`.
- MAX and SILENT send mode command `0x98` (`0x40` and `0x20` respectively),
  followed by `0x99` for each fan. In this sequence the arguments are
  `{ fan_index, value }`. Standalone automatic restore has a firmware quirk:
  command `0x99` must instead receive `{ 0xff, fan_index }`.

## Fan Curve Worker

Implementation lives in `src/cctl_fancurve.inc`; profiles are stored as JSON
under `/var/lib/cctl/fancurves/`.

### Profile and temperature behavior

- Profiles require 2–16 points, strictly increasing temperatures from 0–95°C,
  fan speeds from 25–100%, hysteresis from 0–20°C, and a polling interval
  from 250–60000 ms. Only the final point may use `"a"` or `"auto"`.
- Numeric points are interpolated linearly. Below the first point the first
  speed is used; above the final numeric point the final speed is held. A
  final AUTO point switches to EC control at its temperature and resumes the
  curve below `auto_temp - hysteresis`.
- `interval_ms` sets the worker polling delay. A new duty is written only when
  the target changes and the temperature has moved by at least `hysteresis`
  since the previous duty write.
- The worker controls both fans using the hotter valid CPU/GPU fan-sensor
  temperature. It reads the third byte of `R_CL_FANINFO1/2`; CPU `hwmon` and
  NVIDIA `nvidia-smi` temperatures are fallbacks.
- Above 95°C, after three consecutive temperature-read failures, or after
  three consecutive fan-duty write failures, the worker attempts to restore EC
  auto. It retries ioctls, can reload `clevo_acpi` and `tuxedo_io`, and finally
  falls back to direct EC auto. Following a successful safety handoff, the
  curve resumes below 90°C. The thermal condition is strictly above 95°C.
- On this EC, duties 13–24% clamp to 25%, and duties below 13% turn the fans
  off. The profile minimum is therefore 25%.

### Worker lifecycle and safety

- Starting a curve requires `tuxedo_io` 0.3.10 or newer, both driver modules,
  and `/dev/tuxedo_io`. The worker arms the driver's per-file close recovery
  before writing manual duty. Closing the worker's descriptor restores EC
  automatic control, including after `SIGKILL`; unrelated cctl commands use
  their own descriptors and do not trigger it.
- The open descriptor holds a module reference, so unloading `tuxedo_io`
  fails while the worker is active. Stop the worker before replacing drivers.
  The current `driverinstall.sh` retries module unload, warns if a module is
  still in use, then continues with DKMS cleanup/install; it does not stop the
  fan-curve worker itself.
- The worker is detached and takes an exclusive `flock` on
  `/run/cctl_curve.lock`. `/run/cctl_curve.state` records PID, process start
  time, profile name, and start time. Matching the process start time prevents
  a reused PID from being mistaken for the worker. `stop` waits for that
  worker to exit before returning. `set` and `off` remain accepted as aliases
  for `start` and `stop` for compatibility, though help shows the primary
  command names.
- `create` and `edit` open the configured editor as the invoking user. When
  the profile directory is not writable, a restricted `fancurve __install`
  helper validates the temporary file and profile before installing it.
  Replacing an existing profile requires confirmation. Editing or deleting the
  currently active profile is refused.
- While active, the curve owns both fans. `cctl fan auto` stops it before
  restoring EC auto; other fan overrides are refused. `cctl set`, Turbo, and
  snapshot fan-safety steps skip changes that would take control away from the
  curve.
- Fan curves are not included in snapshots and are not started at boot.

### EC profile cache

`R_CL_PERF_PROFILE` reports the last performance profile written through
`tuxedo_io` since module load, or `-1` when unknown. Fn-key, firmware, and
other driver writes are outside this cache. `cctl status` uses a recognized
live TCC-derived code first and the cache as a fallback; it omits cache data
when the value is `-1`.

## Keyboard Backlight Driver

The installer sets `force_backlight_type=6` for `tuxedo_keyboard`. On the
tested Colorful Evol P15 the EC reports keyboard type `0x26`, which the driver
does not recognize. Its low nibble matches the driver's 1-zone RGB type `0x06`;
the idea that the high bits indicate a newer revision is a hypothesis. The
override bypasses detection and makes the supported 1-zone controls available.

The driver can also expose fixed-white (`1`), 3-zone RGB (`2`), and per-key
RGB (`243`) types, but cctl's keyboard commands currently support only 1-zone
RGB. LEDs are exposed under `/sys/class/leds`; the 1-zone device is
`rgb:kbd_backlight`.

## Display MUX and Refresh Rate

- The tested Insyde H2O firmware uses UEFI NVRAM for MUX mode. `cctl mux
  switch` changes `Setup-a04a27f4-df00-4d42-b552-39511302113d` at file offset
  430 (`0x03` = MSHybrid, `0x02` = dGPU only). The variable is 1204 bytes and
  the code rejects an unexpected size or mode. It stages the change for the
  next boot; it cannot switch the physical display MUX during the current
  session. Other firmware-synchronized bytes are left untouched. When on
  battery below 10%, switching is refused to reduce the risk of power loss
  during the NVRAM write.
- In dGPU mode on this laptop, X11 starts the internal panel at 40 Hz on each
  boot. The panel EDID lists 40 Hz as its preferred timing and 165 Hz in the
  DisplayID extension. The NVIDIA X driver starts at 40 Hz; `xrandr` still
  lists 165 Hz, and `cctl rr 165` selects it. This is a model/driver
  observation, not a general dGPU behavior.

## Driver Archive and Release Workflow

- The expected archive SHA-256 is `DRIVERS_SHA256` in
  `src/cctl_drivers_cli.inc`. Keep it synchronized with the verified archive
  stored under the matching SHA-256 directory in
  `tuxedo-drivers-cctl-mirror`.
- The release workflow reads that constant, downloads
  `<sha256>/drivers.tar.gz` from the mirror without rebuilding it, verifies
  the checksum, and attaches the verified archive to the release.
- `drivers-manage` searches for a valid archive in this order: verified
  `/var/lib/cctl/drivers.tar.gz` cache, a matching archive beside the cctl
  binary, a fresh mirror download, then a user-provided full path. Every
  candidate is checked against the embedded hash before extraction. A
  verified candidate is cached before extraction so later reinstall or
  uninstall can work offline.
