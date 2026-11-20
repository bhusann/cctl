# cctl-gui — HTTP endpoint map

`cctl gui [--port PORT]` serves the embedded UI (`GET /`) on
127.0.0.1 only. All state-changing requests are `POST` and must carry an
`X-CCTL` header; bodies are `application/x-www-form-urlencoded`.

## GET (JSON)

| Endpoint | Source |
| --- | --- |
| `/api/status` | child `cctl status --json` |
| `/api/cpu` | child `cctl cpumon --json` (one-shot) |
| `/api/gpu` | child `cctl gpumon --json` (one-shot) |
| `/api/settings` | `/var/lib/cctl/settings` (streamtext, default action, experimental fan) |
| `/api/cpu-options` | sysfs `policy0/scaling_available_governors` + `energy_performance_available_preferences` |
| `/api/profiles` | built-in list + parsed `/var/lib/cctl/profiles.conf` |
| `/api/curves` | active curve state + saved curve names |
| `/api/curve?name=` | saved curve JSON (`#` comment lines stripped) |
| `/api/presets` | keyboard presets, backlight effects, notification effects |
| `/api/snapshots` | child `cctl snapshot` listing (dir is root-only) |
| `/api/version` | fork version |

## POST (JSON envelope `{"rc":N,"stdout":...,"stderr":...}`)

| Endpoint | Command run |
| --- | --- |
| `/api/run` | whitelisted verb + validated args (`cmd`, repeated `args`) |
| `/api/set` | `cctl set <profile>` (+ `--nosafe`) |
| `/api/turbo` | `cctl turbo on|off` (+ `--nosafe`) |
| `/api/fan` | `cctl fan auto|max|silent|cpu|gpu <pct> [--nosafe]` |
| `/api/options` | `cctl options <key> <value>` (experimental-fan, streamtext, default_action) |
| `/api/curve-start` | `cctl fancurve start <name>` |
| `/api/curve-stop` | `cctl fancurve stop` |
| `/api/curve-delete` | `cctl fancurve del <name> --noconfirm` |
| `/api/curve-create` | `cctl fancurve create --am <name> <n> <hyst> <ms> <t1> <f1> ...` (pairs sorted by temp; last point may be `auto`) |
| `/api/profile-create` | `cctl editconf --am --create <name> <ec> <turbo> <gov> <epp> <pl1> <pl2> <refresh>` (`skip` supported) |
| `/api/profile-delete` | `cctl editconf --am --delete <name>` |
| `/api/snapshot` | `cctl snapshot save|restore|view|delete <name>` (`delete` adds `--noconfirm`) |

Editing a curve or profile replaces it: delete, then recreate — the CLI has
no non-interactive in-place edit.

## Privileges

The server binds unprivileged. Children exec `/usr/local/bin/cctl` (or the
server binary itself when it runs as root) and elevate through the installed
passwordless sudoers rule. If no installed cctl exists, children prompt for
a sudo password — run `sudo cctl gui` instead.
