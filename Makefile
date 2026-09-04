obj-m += legacymethod/

# ── Default: only cctl binary (no NVIDIA support) ────────────────────────
cctl: cctl.c
	gcc -o $@ $< -Os -s -Wall -Wextra -Wshadow

# ── cctl with NVIDIA GPU management compiled in ─────────────────────────
# Adds the nvidia module/GPU-toggle commands: on/off/load/unload/loadgame/status/power.
cctl-nvidia: cctl.c
	gcc -o $@ $< -Os -s -Wall -Wextra -Wshadow -DCCTL_NVIDIA

# ── Everything: cctl + kernel modules ───────────────────────────────────
all: cctl drivers

# ── Kernel modules (legacygpu) ──────────────────────────────────────────────
legacygpu:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) CC=clang LD=ld.lld modules

# ── tuxedo-drivers (clevo_acpi, tuxedo_keyboard, tuxedo_io) ─────────────────
drivers:
	$(MAKE) -C drivers CC=clang LD=ld.lld

drivers-install:
	sudo ./drivers/driverinstall.sh --install

drivers-uninstall:
	sudo ./drivers/driverinstall.sh --uninstall

drivers-status:
	sudo ./drivers/driverinstall.sh --status

.PHONY: all cctl cctl-nvidia legacygpu drivers drivers-install drivers-uninstall drivers-status

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean
	$(MAKE) -C drivers clean
	rm -f cctl cctl-nvidia

.PHONY: clean
