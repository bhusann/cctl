# ── Shared flags ────────────────────────────────────────────────────────────
# cctl routinely runs as root (EC port I/O, NVRAM, sysfs writes), so both
# builds get standard binary hardening: bounds-checked libc calls, stack
# canary, full RELRO + immediate binding, PIE/ASLR, format-string safety.
#
# User-supplied CFLAGS/LDFLAGS are respected (appended) — hardening flags
# are always applied via override so they cannot be accidentally dropped.

CC       ?= gcc
DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1)
DBUS_LIBS   := $(shell pkg-config --libs dbus-1)
LDLIBS   += -lm -pthread $(DBUS_LIBS)

# Default optimisation (overridable: `make CFLAGS="-O2 -g"`)
CFLAGS   ?= -Os

# Hardening — always present regardless of user overrides
override CFLAGS  += -Wall -Wextra -Wshadow \
                    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 \
                    -fstack-protector-strong -fPIE -Wformat-security $(DBUS_CFLAGS)
override LDFLAGS += -pie -Wl,-z,relro -Wl,-z,now

# Release builds strip by default; pass STRIP=0 to keep debug symbols
STRIP ?= 1
ifeq ($(STRIP),1)
override LDFLAGS += -s
endif

SOURCES  = cctl.c src/cctl_gpumon.c src/cctl_cpumon.c
INCLUDES = src/cctl_core.inc src/cctl_controls.inc src/cctl_platform.inc src/cctl_keyboard.inc \
           src/cctl_fancurve.inc \
           src/cctl_telemetry.inc src/cctl_runtime.inc src/cctl_nvidia.inc \
           src/cctl_snapshot.inc src/cctl_commands.inc src/cctl_drivers_cli.inc \
           src/cctl_monitor_common.h src/cctl_shared.h

# ── Default: only cctl binary (no NVIDIA support) ────────────────────────────
cctl: $(SOURCES) $(INCLUDES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

# ── Experimental build with NVIDIA GPU management compiled in ─────────────
# Adds the nvidia module/GPU-toggle commands: on/off/load/unload/loadgame/status.
experimental: $(SOURCES) $(INCLUDES)
	$(CC) $(CFLAGS) -DCCTL_NVIDIA -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

# ── Install ─────────────────────────────────────────────────────────────────
# Build and run cctl's installer, which installs the binary and configures
# the required sudoers rule.
install: cctl
	sudo ./cctl install

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	rm -f cctl experimental

.PHONY: cctl experimental install clean
