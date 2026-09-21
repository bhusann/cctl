obj-m += legacymethod/

# ── Default: only cctl binary (no NVIDIA support) ────────────────────────
cctl: cctl.c
	gcc -o $@ $< -Os -s -Wall -Wextra -Wshadow

# ── Experimental build with NVIDIA GPU management compiled in ─────────────
# Adds the nvidia module/GPU-toggle commands: on/off/load/unload/loadgame/status/power.
experimental: cctl.c
	gcc -o $@ $< -Os -s -Wall -Wextra -Wshadow -DCCTL_NVIDIA

# ── tuxedo-drivers (clevo_acpi, tuxedo_keyboard, tuxedo_io) ─────────────────
# Compile kernel drivers locally in-tree for testing (no installation)
test-drivers:
	$(MAKE) -C drivers CC=clang LD=ld.lld

# ── Kernel modules (legacygpu) ──────────────────────────────────────────────
legacygpu:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) CC=clang LD=ld.lld modules

.PHONY: cctl experimental legacygpu test-drivers

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean
	$(MAKE) -C drivers clean
	rm -f cctl experimental cctl-nvidia

.PHONY: clean
