# ── Shared flags ────────────────────────────────────────────────────────────
# cctl routinely runs as root (EC port I/O, NVRAM, sysfs writes), so both
# builds get standard binary hardening: bounds-checked libc calls, stack
# canary, full RELRO + immediate binding, PIE/ASLR, format-string safety.
#
# User-supplied CFLAGS/LDFLAGS are respected (appended) — hardening flags
# are always applied via override so they cannot be accidentally dropped.

CC       ?= gcc
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
DESTDIR  ?=

# Default optimisation (overridable: `make CFLAGS="-O2 -g"`)
CFLAGS   ?= -Os

# Hardening — always present regardless of user overrides
override CFLAGS  += -Wall -Wextra -Wshadow \
                    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 \
                    -fstack-protector-strong -fPIE -Wformat-security
override LDFLAGS += -pie -Wl,-z,relro -Wl,-z,now

# Release builds strip by default; pass STRIP=0 to keep debug symbols
STRIP ?= 1
ifeq ($(STRIP),1)
override LDFLAGS += -s
endif

# ── Default: only cctl binary (no NVIDIA support) ────────────────────────────
cctl: cctl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ── Experimental build with NVIDIA GPU management compiled in ─────────────
# Adds the nvidia module/GPU-toggle commands: on/off/load/unload/loadgame/status/power.
experimental: cctl.c
	$(CC) $(CFLAGS) -DCCTL_NVIDIA -o $@ $< $(LDFLAGS)

# ── Install ─────────────────────────────────────────────────────────────────
# Basic binary install; for full setup (sudoers rule, passwordless sudo)
# use `cctl install` after placing the binary.
install: cctl
	install -Dm755 cctl $(DESTDIR)$(BINDIR)/cctl

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	rm -f cctl experimental

.PHONY: cctl experimental install clean
