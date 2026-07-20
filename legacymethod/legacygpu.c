// SPDX-License-Identifier: GPL-2.0-only
// One-shot ACPI _DSM GPU profile setter for Clevo P15.
// Usage:
//   make
//   sudo insmod legacygpu.ko profile=2   # 0=quiet 1=standard 2=performance 3=turbo
//   sudo rmmod legacygpu
//
// Based on gpu_perf.rs embedded module.

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/acpi.h>

#define CLEVO_DSM_UUID "93f224e4-fbdc-4bbf-add6-db71bdc0afad"

static int profile = 2;
module_param(profile, int, 0444);
MODULE_PARM_DESC(profile, "Performance profile: 0=quiet, 1=standard, 2=performance, 3=turbo");

static int __init legacygpu_init(void)
{
	acpi_handle handle = NULL;
	acpi_status status;
	guid_t uuid;
	union acpi_object *out_obj;
	union acpi_object arg;
	union acpi_object package;
	u32 cmd_arg;
	const char *paths[] = { "\\_SB.DCHU", "\\_SB.PCI0.LPCB.EC0", NULL };
	int i;

	if (profile < 0 || profile > 3) {
		pr_err("legacygpu: profile %d out of range (0-3)\n", profile);
		return -EINVAL;
	}

	for (i = 0; paths[i]; i++) {
		status = acpi_get_handle(NULL, (acpi_string)paths[i], &handle);
		if (ACPI_SUCCESS(status))
			break;
	}

	if (!handle) {
		pr_err("legacygpu: no valid ACPI handle found (tried \\_SB.DCHU, \\_SB.PCI0.LPCB.EC0)\n");
		return -ENODEV;
	}

	if (guid_parse(CLEVO_DSM_UUID, &uuid) != 0) {
		pr_err("legacygpu: bad UUID\n");
		return -EINVAL;
	}

	// Arg encoding: (sub_cmd << 24) | (data & 0xFFFFFF)
	// sub_cmd 0x19: set performance profile
	cmd_arg = (0x19 << 24) | (profile & 0xFFFFFF);
	arg.type = ACPI_TYPE_INTEGER;
	arg.integer.value = cmd_arg;

	package.type = ACPI_TYPE_PACKAGE;
	package.package.count = 1;
	package.package.elements = &arg;

	out_obj = acpi_evaluate_dsm(handle, &uuid, 0, 0x79, &package);
	if (!out_obj) {
		pr_err("legacygpu: _DSM evaluate failed\n");
		return -EIO;
	}

	if (out_obj->type == ACPI_TYPE_INTEGER)
		pr_info("legacygpu: profile %d set, _DSM returned 0x%llx\n",
			profile, out_obj->integer.value);
	else
		pr_info("legacygpu: profile %d set, _DSM returned non-integer\n",
			profile);

	ACPI_FREE(out_obj);
	return 0; // stay loaded so rmmod works cleanly
}

static void __exit legacygpu_exit(void) {}

module_init(legacygpu_init);
module_exit(legacygpu_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("turbo");
MODULE_DESCRIPTION("Clevo ACPI _DSM GPU performance profile setter");
