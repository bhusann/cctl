# Developer Notes

- **Source layout** — `cctl.c` includes subsystem fragments in a fixed order: core, power controls, platform/privacy/status, keyboard, monitor, runtime helpers, NVIDIA, command handlers, and driver/install commands. They remain one translation unit so hardware helpers stay private and link behavior is unchanged; edit the owning fragment instead of growing the main file.

- **RAPL 0.4 GHz Throttle** — Only package-0 (`intel-rapl:0`) is safe to write. Touching sub-zones (`intel-rapl:0:X`) or platform `psys` triggers an EC conflict that hard-throttles the CPU to 400 MHz.
- **RAPL 90 W mode tracking** — The up-to-90 W PL1 ceiling applies while EC profile code 2 is active; the active profile is recorded and shown as `Mode:` in `cctl status`, and `EC default` appears when no profile has been applied yet.
- **OEM PROCHOT Throttle Trip Points & CPU State Telemetry (`cpumon`)** — Hardware thermal throttling is governed by the laptop manufacturer's EC firmware rather than the silicon TjMax (100°C):
  - **EC Code 2 (Performance):** The OEM PROCHOT trip point is **97°C** (97–98°C).
  - **EC Codes 0, 1, 3 (Quiet, Balanced, Standard / EC default):** The OEM PROCHOT trip point is **87°C**. If `/run/cctl/mode` does not exist, the EC defaults to code 3 (87°C PROCHOT threshold).
  - In `cctl cpumon`, the `STATE` readout dynamically tracks this active threshold:
    - `▲ PROCHOT THROTTLE` (Red): CPU temperature $\ge$ active PROCHOT limit (87°C or 97°C) or within 3°C of TjMax.
    - `▲ PL2 LIMIT` (Magenta): Package power $\ge 95\%$ of short-term burst limit (PL2).
    - `▲ BOOST` (Amber): Package power exceeds sustained ceiling ($> 102\%$ of PL1).
    - `■ PL1 LIMIT` (Amber): Sustained power wall ($\ge 90\%$ of PL1 under $> 60\%$ utilization).
    - `■ SOFT THROTTLE` (Amber): Soft frequency throttling when Turbo is disabled (`turbo_on == false`) and CPU utilization is heavy ($\ge 50\%$). The CPU is clamped at base clock and consumes only ~25W (far below 45W PL1) even at 100% load.
    - `● IDLE` (Cyan): Total CPU utilization $< 10\%$.
    - `● NORMAL` (Green): Operating normally between 10% and 50% load without hitting thermal or power limits.
  - **TAU (Time Window):** The TAU value displayed in `cpumon` is **not assumed** — it is live-read directly from `/sys/class/powercap/intel-rapl:0/constraint_0_time_window_us`.

- **CPU EPP vs Governor Dependency** — Under the Linux `intel_pstate` driver, manual EPP (Energy Performance Preference) tuning is only supported when the CPU governor is set to `powersave`. When the governor is set to `performance`, the driver/hardware locks EPP to `performance` and rejects sysfs writes for other values (`-EINVAL`). `cctl epp` checks the active governor and directs the user to switch to `powersave` first (`cctl gov powersave`).
- **Snapshot Restore EPP vs Masked Governor** — During `cctl snapshot restore`, if the snapshot contains an active EPP setting (`power`, `balance_power`, or `balance_performance`) while governor is skipped/masked, and the system's live governor is currently `performance`, `cctl` detects the conflict, alerts the user, and automatically switches the governor to `powersave` so the snapshot's saved EPP can take effect. Users who wish to keep the `performance` governor intact without altering it can mask out EPP (`--mask epp` or `--mask gov,epp`).
- **Snapshot Restore RAPL PL1 vs EC Profile Code 2** — On this chassis, setting RAPL PL1 above 45W (up to 90W) requires EC profile code 2 (`max` mode). If a snapshot contains PL1 > 45W, but EC profile code 2 is masked (`--mask ec_code`) or not applied, `cctl snapshot restore` alerts the user that EC code 2 is required, safely skips PL1, and restores PL2 alone.
- **RAPL PL2 > PL1 Invariant Rule** — Burst power limit (PL2) must always exceed the sustained limit (PL1). This is strictly enforced across `cctl rapl` (interactive help displays `PL2 up to 115W (must be > PL1)`, single/double argument validation rejects PL2 $\le$ PL1), user profiles in `/etc/cctl/profiles.conf`, snapshot restore, and defense-in-depth within `set_rapl_limits()`.
- **Max Performance Mode (`cctl set max`) Characteristics** — Switching to `max` mode outputs an advisory note reminding the user that sustained 60–90W CPU package power will push temperatures toward ~98°C under heavy loads, recommending it for heavy tasks like compilation or demanding games, while suggesting `cpuperf` or `balanced` for day-to-day use.
- **NVIDIA GPU Power Limits in `cctl gpumon`** — On mobile NVIDIA GPUs, `power.limit` often returns `[N/A]`. `cctl gpumon` queries `enforced.power.limit` (the active TDP ceiling, e.g. 70W in Balanced or 80W in Max) and `power.max_limit` (the hardware board ceiling, e.g. 100W). The `PWR IN` box displays `current / limit W` (e.g. `12.5 / 70 W`), and the bar scale normalizes against the active limit so power draw telemetry is visually accurate.

- **Direct EC Port I/O Fan Control & Protocol Quirks (Legacy Method)** — `cctl` uses direct port I/O (`ioperm`, `inb`/`outb` on port `0x66` command and `0x62` data) for individual fan control (`cctl fan cpu <pct>`, `cctl fan gpu <pct>`), Max, and Silent modes. This preserves the ability to adjust a single fan independently without kicking the other fan off its automatic EC thermal curve (which the `tuxedo_io` `W_CL_FANSPEED` ioctl cannot do, as it forces all channels into fixed manual mode).
  - **Hardware Handshake:** All writes require polling Input Buffer Full (IBF, bit 1 of port `0x66`): wait until `((inb(0x66) >> 1) & 1) == 0` before sending each command or data byte.
  - **Individual Fan Duty:**
    - Send command `0x99` to port `0x66`, followed by 2 data bytes to port `0x62`:
      ```
      { fan_idx, raw_duty }
      ```
      where `fan_idx` is `1` (CPU) or `2` (GPU), and `raw_duty = (pct * 255) / 100` (range 25–100%).
    - Directly writes to that fan's hardware PWM channel without altering or locking the other fan.
  - **Max Fan Mode:**
    - Step 1: Send mode command `0x98` to port `0x66` with data byte `0x40` (`FAN_MODE_MAX`) to port `0x62`.
    - Step 2: Send follow-up command `0x99` for each fan: `{ 0x01 (CPU), 0xFF (FAN_DUTY_AUTO) }` and `{ 0x02 (GPU), 0xFF (FAN_DUTY_AUTO) }`.
  - **Silent Fan Mode:**
    - Step 1: Send mode command `0x98` to port `0x66` with data byte `0x20` (`FAN_MODE_SILENT`) to port `0x62`.
    - Step 2: Send follow-up command `0x99` for each fan: `{ 0x01 (CPU), 0x20 (FAN_MODE_SILENT) }` and `{ 0x02 (GPU), 0x20 (FAN_MODE_SILENT) }`.
  - **Standalone Auto Restore Quirk (Reversed Byte Order):**
    - When restoring automatic EC control without a preceding `0x98` mode command, command `0x99` requires **reversed** argument order:
      ```
      { 0xFF (FAN_DUTY_AUTO), fan_idx }
      ```
    - Notice that `{ fan_idx, 0xFF }` only works as a follow-up after a `0x98` mode command. For standalone auto-restore, passing `{ fan_idx, 0xFF }` is ignored by the firmware; `{ 0xFF, fan_idx }` must be used.

- **GPU Fan Duty Register** — ACPI `FANINFO1` byte 2 is stuck at ~15% on this model. `cctl` reads GPU fan duty from `FANINFO2` (`0x64`, byte 0) for accurate readings.

- **Keyboard Backlight Type Override (`force_backlight_type=6`)** — Why the driver installer forces type 6:
  - In `drivers/src/clevo_leds.h:clevo_leds_init()` (path within the mirror repo):
    1. The driver calls `clevo_evaluate_method2(0x0D, 0)` to read the ACPI `_DSM` buffer. `buffer[0x0f]` holds the keyboard type ID. Known IDs include: `0x00` (NONE), `0x01` (FIXED), `0x02` (3ZONE), `0x06` (1ZONE), `0xF3` (PERKEY).
    2. The Colorful Evol P15 EC returns `0x26`. Because this ID is unknown to the driver, probe switches fail to match, registering no `led_classdev` — resulting in no `/sys/class/leds/*::kbd_backlight` and leaving brightness/color ioctls non-functional.
    3. `0x26` (`0b00100110`) vs `0x06` (`0b00000110`): the low nibble is identical, while the high bits likely signify a newer revision or flag. The wire protocol (`0x67` + `0xF4000000 | brightness`, zone color) remains standard single-zone RGB.
    4. Passing module option `force_backlight_type=6` (`tuxedo_keyboard`) bypasses ACPI buffer evaluation directly:
       ```c
       if (force_backlight_type >= 0) type = force; else { /* SPECS retry x3 -> 0x52/0x7A fallback */ }
       ```
    5. Upstream driver fix would be handling `case 0x26: type = 1ZONE;` or matching `(type & 0x0F) == 0x06`. Until upstreamed, forcing type 6 via modprobe is required.
  - **Customizing your keyboard type:**
    If you have a different variant and need to force another keyboard backlight mode, edit `/etc/modprobe.d/tuxedo_keyboard.conf`:
    ```
    options tuxedo_keyboard force_backlight_type=<value>
    ```
    Known values:
    * `1` — FIXED white backlight (`0x01`)
    * `2` — 3-zone RGB (`0x02`)
    * `6` — 1-zone RGB (`0x06`) *(default configured by this project)*
    * `243` — Per-key RGB (`0xF3`)

  - **Sysfs LED Exposure (`/sys/class/leds`):**
    All keyboard backlight controls registered by the driver are exposed under `/sys/class/leds`:
    * For **1-zone RGB (`force_backlight_type=6`)**: A single device entry is created:
      ```
      /sys/class/leds/rgb:kbd_backlight/
      ```
      (Contains controls like `brightness`, `multi_intensity`, `color`, etc.)
    * For **3-zone RGB (`force_backlight_type=2`)**: The kernel driver registers 3 separate LED device entries under `/sys/class/leds` corresponding to each keyboard zone (e.g., `rgb:kbd_backlight`, `rgb:kbd_backlight_1`, `rgb:kbd_backlight_2` or separate zone descriptors), allowing each zone's color and brightness to be tuned individually via sysfs.
    * For **Fixed White (`force_backlight_type=1`)**: A `white:kbd_backlight` directory is exposed for brightness level control.

    > **Note:** `cctl`'s command implementation currently only supports **1-zone RGB** (using type 6). If your hardware uses 3-zone RGB or per-key RGB, you can manage the zones directly through their respective `/sys/class/leds` folders or adapt `cctl`'s source code (`cctl.c`) to control individual zones.

- **GPU Display MUX via UEFI NVRAM (Insyde H2O)** — The laptop features a physical display multiplexer with two BIOS modes:
  - `MSHybrid` (panel driven by Intel iGPU; NVIDIA dGPU provides render offload).
  - `dGPU` (panel wired directly to NVIDIA GeForce RTX card; Intel iGPU is unmapped from PCI display class).
  - The hardware MUX state cannot be flipped on-the-fly inside an active OS session (ACPI `_DSM` methods on this Insyde board hang the display subsystem). Instead, switching is staged via the UEFI NVRAM variable `Setup-a04a27f4-df00-4d42-b552-39511302113d` at file offset 430 (`0x03` = MSHybrid, `0x02` = dGPU). The new mode is latched during POST upon reboot.
  - **How it works:** `cctl mux switch` writes to the UEFI Setup NVRAM variable (`Setup-a04a27f4-df00-4d42-b552-39511302113d` offset 430). The setting is committed to SPI flash and latched by firmware during POST on the next boot. It includes safety guards: blob length verification (1204 B) and unknown value refusal.
- **dGPU mode boots at 40 Hz (X11)** — Happens on every boot when dGPU mode is selected, whether selected in BIOS or with `cctl mux switch`; it is not EC state. The panel's EDID is identical in both MUX modes: the base block's first (preferred) DTD is 2560x1440 @ **40 Hz**, while the 165 Hz timing lives in the DisplayID extension. The nvidia X driver takes the base-block preferred timing as the initial mode (boots 40); modesetting over i915 in MSHybrid prefers the DisplayID timing (boots 165). Not a capability problem — xrandr lists both rates in dGPU mode. Run `cctl rr 165` to restore 165 Hz, or add that command to your desktop environment's autostart/startup applications to apply it automatically.
- **Keyboard effect state (`kbe`)** — Run `cctl kbe` to view the active effect, PID, and preserved base state (it re-elevates automatically — the `/run/cctl_kbe.state` file is root-only; `sudo cat` it directly if you prefer).
- **Release tarball provenance** — The `drivers.tar.gz` attached to releases is fetched straight from the mirror — never rebuilt — and the release workflow refuses to publish if its sha256 drifts from the constant baked into `cctl.c`.
- **drivers-manage source order** — how `cctl drivers-manage` finds `drivers.tar.gz` (first match wins):
  1. persistent cache **`/var/lib/cctl/drivers.tar.gz`** — sha-verified; enables fully offline reinstall; stale/corrupt entries are reported, ignored, and replaced on the next verified acquisition
  2. `drivers.tar.gz` beside the cctl binary — sha-checked on the spot; a stale copy is warned about and skipped
  3. download from the mirror into a private temp dir — on failure it names the folder for a manual copy
  4. full path to a `drivers.tar.gz` you already have — checked in place first; staged only if valid
- **drivers-manage verification** — Every candidate is verified against the sha256 **baked into the cctl binary before extraction** — spoofed or stale files are refused. Whatever passes is also copied to `/var/lib/cctl/` *before* extraction, so the next reinstall or uninstall needs neither internet nor the original file.
