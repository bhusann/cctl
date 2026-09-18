obj-m += legacymethod/

# ── Default: only cctl binary (no NVIDIA support) ────────────────────────
cctl: cctl.c
	gcc -o $@ $< -Os -s -Wall -Wextra -Wshadow

# ── cctl with NVIDIA GPU management compiled in ─────────────────────────
# Adds the nvidia module/GPU-toggle commands: on/off/load/unload/loadgame/status/power.
cctl-nvidia: cctl.c
	gcc -o $@ $< -Os -s -Wall -Wextra -Wshadow -DCCTL_NVIDIA

# ── tuxedo-drivers (clevo_acpi, tuxedo_keyboard, tuxedo_io) ─────────────────
# Install drivers via DKMS using installer script
drivers:
	sudo ./drivers/driverinstall.sh --install

# Compile kernel drivers locally in-tree for testing (no installation)
test-drivers:
	$(MAKE) -C drivers CC=clang LD=ld.lld

# ── Kernel modules (legacygpu) ──────────────────────────────────────────────
legacygpu:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) CC=clang LD=ld.lld modules

.PHONY: cctl cctl-nvidia legacygpu drivers test-drivers

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean
	$(MAKE) -C drivers clean
	rm -f cctl cctl-nvidia

.PHONY: clean
