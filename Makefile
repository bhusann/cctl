obj-m += legacymethod/

all: cctl drivers

# ── cctl binary ─────────────────────────────────────────────────────────────
cctl: cctl.c
	gcc -o $@ $< -Os -s

# ── Kernel modules (legacygpu) ──────────────────────────────────────────────
legacygpu:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) CC=clang LD=ld.lld modules

# ── tuxedo-drivers (clevo_acpi, tuxedo_keyboard, tuxedo_io) ─────────────────
drivers:
	$(MAKE) -C drivers

drivers-install:
	sudo ./drivers/driverinstall.sh --install

drivers-uninstall:
	sudo ./drivers/driverinstall.sh --uninstall

drivers-status:
	sudo ./drivers/driverinstall.sh --status

.PHONY: all cctl legacygpu drivers drivers-install drivers-uninstall drivers-status

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean
	$(MAKE) -C drivers clean
	rm -f cctl

.PHONY: clean
