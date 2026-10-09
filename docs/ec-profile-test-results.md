# EC Profiles and Governor/EPP Load Test Results

These full-load measurements on the author's 10-core, 16-thread CPU compare
the performance, power, and temperature tradeoffs. Results vary with cooling
and workload.

The `performance` EPP results were the same with either governor, so they are
combined below.

For the highest measured performance, code `2` with Turbo on and EPP
`performance` reached 85–87W, then sustained 75–76W near 98°C. Code `2` with
`powersave` / `balance_performance` sustained 63–64W around 91°C. Code `3`
offers a lower-power standard mode; codes `0` and `1` have 15W PL1 defaults.

#### EC code 3 — Standard

Defaults during this test: TAU 28 seconds, PL1 45W, PL2 115W, TCC offset 13°C
(87°C PROCHOT).

| Governor / EPP | Turbo | Peak and sustained frequency and power |
|---|---:|---|
| performance or powersave / performance | On | Peak: P 4100 ± 200 MHz; E 3100–3285 MHz; 67–70W; reaches 87°C PROCHOT.<br>────────────<br>Sustained: P 3300 MHz; E 2600 MHz; 45W after the 28-second TAU. |
| performance or powersave / performance | Off | Peak & sustained: P 2400 MHz; E 1800 MHz; about 24W (frequency-limited). |
| powersave / balance_performance | On | Peak: P 3800 MHz; E 2800 MHz; 63W; reaches 87°C PROCHOT.<br>────────────<br>Sustained: P 3300 MHz; E 2600 MHz; 45W after TAU. |
| powersave / balance_performance | Off | Peak & sustained: P 2400 MHz; E 1800 MHz; about 24W (frequency-limited). |

#### EC code 2 — High performance

Defaults during this test: TAU 80 seconds, PL1 90W, PL2 115W, TCC offset 2°C
(98°C PROCHOT).

| Governor / EPP | Turbo | Peak and sustained frequency and power |
|---|---:|---|
| performance or powersave / performance | On | Peak: P 4100–4300 MHz; E 3200–3300 MHz; 85–87W at 98–99°C PROCHOT.<br>────────────<br>Sustained: P 3800–3900 MHz; E 3000–3100 MHz; 75–76W at 98°C. |
| performance or powersave / performance | Off | Peak & sustained: P 2400 MHz; E 1800 MHz; about 25W. |
| powersave / balance_performance | On | Peak & sustained: P 3800 MHz; E 2800 MHz; 63–64W at about 91°C. |
| powersave / balance_performance | Off | Peak & sustained: P 2400 MHz; E 1800 MHz; about 25W. |

#### EC code 1 — Powersave, and EC code 0 — Silent

These two codes behaved the same in the test. Defaults: TAU 8 seconds, PL1
15W, PL2 30W. TCC offset is 15°C for code 1 (85°C PROCHOT) and 10°C for code
0 (90°C PROCHOT). No PROCHOT event was observed in these test cases.

| Governor / EPP | Turbo | Peak and sustained frequency and power |
|---|---:|---|
| performance or powersave / performance | On | Peak: P 2700 MHz; E 2100 MHz; 30W.<br>────────────<br>Sustained: P 1500–1600 MHz; E 1200 MHz; 15W after the 8-second TAU. |
| performance or powersave / performance | Off | Peak: P 2400 MHz; E 1800 MHz; 25W.<br>────────────<br>Sustained: P 1500–1600 MHz; E 1200 MHz; 15W after TAU. |
| powersave / balance_performance | On | Peak: P 2600–2700 MHz; E 2100 MHz; 30W.<br>────────────<br>Sustained: P 1500–1600 MHz; E 1200 MHz; 15W after TAU. |
| powersave / balance_performance | Off | Peak: P 2400 MHz; E 1800 MHz; 25W.<br>────────────<br>Sustained: P 1500–1600 MHz; E 1200 MHz; 15W after TAU. |

`balance_power` and `power` behaved similarly across EC codes and are listed
once here. Codes `0` and `1` sustain lower clocks on `balance_power` after
reaching their 15W PL1 limit.

| Governor / EPP | Turbo | Peak and sustained frequency and power across EC codes |
|---|---:|---|
| powersave / balance_power | On or off | Peak: P 2200 MHz; E 1600 MHz; about 22W.<br>────────────<br>Sustained: Codes `2` and `3` remain around P 2200 MHz, E 1600 MHz, 22W; codes `0` and `1` reach 15W after TAU and settle around P 1500–1600 MHz, E 1200 MHz. |
| powersave / power | On or off | Peak & sustained: P 1100 MHz; E 1100 MHz; about 12W across all codes. |


