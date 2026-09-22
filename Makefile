# ── Shared flags ────────────────────────────────────────────────────────────
# cctl routinely runs as root (EC port I/O, NVRAM, sysfs writes), so both
# builds get standard binary hardening: bounds-checked libc calls, stack
# canary, full RELRO + immediate binding, PIE/ASLR, format-string safety.
CFLAGS  = -Os -Wall -Wextra -Wshadow \
          -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 \
          -fstack-protector-strong -fPIE -Wformat-security
LDFLAGS = -s -pie -Wl,-z,relro -Wl,-z,now

# ── Default: only cctl binary (no NVIDIA support) ────────────────────────────
cctl: cctl.c
	gcc $(CFLAGS) -o $@ $< $(LDFLAGS)

# ── Experimental build with NVIDIA GPU management compiled in ─────────────
# Adds the nvidia module/GPU-toggle commands: on/off/load/unload/loadgame/status/power.
experimental: cctl.c
	gcc $(CFLAGS) -DCCTL_NVIDIA -o $@ $< $(LDFLAGS)

.PHONY: cctl experimental

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	rm -f cctl experimental

.PHONY: clean
