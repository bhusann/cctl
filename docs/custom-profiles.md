# Custom Profiles

Custom profiles let you save combinations of power and display settings under
your own names. Create or edit them with `cctl editconf`, then apply one with
`cctl set <name>`. To check a profile before applying it, use
`cctl set <name> --dry-run`.

## Add a profile

Use one profile per line. Lines beginning with `#` are comments.

```text
# name       EC-code Turbo Governor  EPP                 PL1 PL2 Refresh
quietwork    1       0     powersave balance_power       skip skip 40
highcpu      2       1     powersave performance        90  115  165
```

The first five fields are required. Add both `PL1` and `PL2` to set power
limits, then add the refresh rate if needed. If you want to set a refresh rate
without changing power limits, use `skip skip` for those fields. Use `skip` for
any setting you want to leave unchanged.

Names can use letters, numbers, hyphens, and underscores. Choose a name that
does not match a built-in profile. If a profile does not appear in `cctl set`,
check its spelling, field count, and values.

The profile file is `/var/lib/cctl/profiles.conf`; `cctl editconf` opens it for
you. If you have profiles in the old `/etc/cctl/profiles.conf` file, copy them
to the new file once.

Before saving, cctl checks every profile row. If a row is invalid, it reports
the line and problem, then lets you reopen the editor. The file is saved only
after all rows pass validation.

## Profile fields

| Field | Accepted values | Meaning |
| --- | --- | --- |
| `name` | Letters, numbers, `-`, `_` | Name used with `cctl set`. |
| `EC-code` | `0`–`3` | Laptop power mode: `0` silent, `1` power saving, `2` high performance, `3` standard. |
| `Turbo` | `1`, `0`, `skip` | Enable, disable, or leave Turbo unchanged. |
| `Governor` | Any value shown by `cctl gov`, or `skip` | Choose how the CPU adjusts its speed. |
| `EPP` | Any value shown by `cctl epp`, or `skip` | Set the CPU's performance versus efficiency preference. |
| `PL1` | Watts, or `skip` | Set the sustained CPU power limit. |
| `PL2` | Watts, or `skip` | Set the short-term CPU power limit. It must be higher than PL1 when both are set. |
| `Refresh` | A refresh rate such as `60` or `165`, or `skip` | Set the laptop display refresh rate when available. |

You can omit the optional power-limit fields and refresh rate when they are at
the end of the line. To include a refresh rate without setting power limits,
write `skip skip` before it.

## Power and fan safety

PL2 can be set up to 115 W. PL1 is limited to 45 W in most modes and up to
90 W in high-performance mode (EC code `2`). The laptop may enforce lower
power limits in silent, power-saving, and standard modes, even when a larger
limit is requested.

Applying EC code `2` or `3` normally returns the fans to automatic control for
safety. Add `--nosafe` to `cctl set <name>` to keep the current fan setting.
When a fan curve is running, it keeps control of the fans.

On Intel systems, the `powersave` governor allows the EPP preference to guide
CPU speed. The `performance` governor keeps the CPU at higher speeds and uses
the `performance` EPP setting. The built-in profiles use `powersave` with
different EPP preferences.
