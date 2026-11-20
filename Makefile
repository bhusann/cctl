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

.DEFAULT_GOAL := cctl

# Default optimisation (overridable: `make CFLAGS="-O2 -g"`)
CFLAGS   ?= -Os

# ── Fork flags ──────────────────────────────────────────────────────────────
# This fork embeds the browser control panel (`cctl gui`) and NVIDIA GPU
# management in the default build; upstream keeps NVIDIA support optional.
override CFLAGS  += -DCCTL_GUI -DCCTL_NVIDIA

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
           src/cctl_skill.inc SKILL.md \
           src/cctl_gui_server.inc src/cctl_gui_html.inc gui/web/index.html \
           src/cctl_monitor_common.h src/cctl_shared.h

# `cctl --am` prints this skill. Keep the embedded copy in sync with SKILL.md.
src/cctl_skill.inc: SKILL.md tools/gen_skill_inc.py
	python3 tools/gen_skill_inc.py SKILL.md $@


# The GUI page + icon are embedded the same way (tools/gen_gui_html.py).
src/cctl_gui_html.inc: gui/web/index.html docs/cctl-icon.svg tools/gen_gui_html.py
	python3 tools/gen_gui_html.py gui/web/index.html docs/cctl-icon.svg $@


# ── Default: cctl binary with browser GUI and NVIDIA GPU management ──────────
cctl: $(SOURCES) $(INCLUDES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

# ── Install ─────────────────────────────────────────────────────────────────
# Build and run cctl's installer, which installs the binary and configures
# the required sudoers rule.
install: cctl
	sudo ./cctl install

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	rm -f cctl

.PHONY: cctl install clean
