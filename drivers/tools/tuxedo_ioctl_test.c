/*
 * Comprehensive tuxedo_io ioctl tester
 * Exercises every ACPI command exposed via /dev/tuxedo_io
 *
 * Build: gcc -o tuxedo_ioctl_test tuxedo_ioctl_test.c
 * Run:   sudo ./tuxedo_ioctl_test
 *
 * For sysfs-controlled features (backlight, battery, fn lock),
 * see the shell commands printed at the end.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>

/* ---- ioctl definitions (copied from tuxedo_io_ioctl.h) ---- */
#define IOCTL_MAGIC 0xEC

#define MAGIC_READ_CL  IOCTL_MAGIC + 1
#define MAGIC_WRITE_CL IOCTL_MAGIC + 2
#define MAGIC_READ_UW  IOCTL_MAGIC + 3
#define MAGIC_WRITE_UW IOCTL_MAGIC + 4

/* General */
#define R_MOD_VERSION     _IOR(IOCTL_MAGIC, 0x00, char*)
#define R_HWCHECK_CL      _IOR(IOCTL_MAGIC, 0x05, int32_t*)
#define R_HWCHECK_UW      _IOR(IOCTL_MAGIC, 0x06, int32_t*)

/* Clevo reads */
#define R_CL_HW_IF_STR    _IOR(MAGIC_READ_CL, 0x00, char*)
#define R_CL_FANINFO1     _IOR(MAGIC_READ_CL, 0x10, int32_t*)
#define R_CL_FANINFO2     _IOR(MAGIC_READ_CL, 0x11, int32_t*)
#define R_CL_FANINFO3     _IOR(MAGIC_READ_CL, 0x12, int32_t*)
#define R_CL_WEBCAM_SW    _IOR(MAGIC_READ_CL, 0x13, int32_t*)
#define R_CL_FLIGHTMODE_SW _IOR(MAGIC_READ_CL, 0x14, int32_t*)
#define R_CL_TOUCHPAD_SW  _IOR(MAGIC_READ_CL, 0x15, int32_t*)

/* Clevo writes */
#define W_CL_FANSPEED     _IOW(MAGIC_WRITE_CL, 0x10, int32_t*)
#define W_CL_FANAUTO      _IOW(MAGIC_WRITE_CL, 0x11, int32_t*)
#define W_CL_WEBCAM_SW    _IOW(MAGIC_WRITE_CL, 0x12, int32_t*)
#define W_CL_FLIGHTMODE_SW _IOW(MAGIC_WRITE_CL, 0x13, int32_t*)
#define W_CL_TOUCHPAD_SW  _IOW(MAGIC_WRITE_CL, 0x14, int32_t*)
#define W_CL_PERF_PROFILE _IOW(MAGIC_WRITE_CL, 0x15, int32_t*)

/* Uniwill reads */
#define R_UW_HW_IF_STR    _IOR(MAGIC_READ_UW, 0x00, char*)
#define R_UW_MODEL_ID     _IOR(MAGIC_READ_UW, 0x01, int32_t*)
#define R_UW_FANSPEED     _IOR(MAGIC_READ_UW, 0x10, int32_t*)
#define R_UW_FANSPEED2    _IOR(MAGIC_READ_UW, 0x11, int32_t*)
#define R_UW_FAN_TEMP     _IOR(MAGIC_READ_UW, 0x12, int32_t*)
#define R_UW_FAN_TEMP2    _IOR(MAGIC_READ_UW, 0x13, int32_t*)
#define R_UW_MODE         _IOR(MAGIC_READ_UW, 0x14, int32_t*)
#define R_UW_MODE_ENABLE  _IOR(MAGIC_READ_UW, 0x15, int32_t*)
#define R_UW_FANS_OFF_AVAILABLE _IOR(MAGIC_READ_UW, 0x16, int32_t*)
#define R_UW_FANS_MIN_SPEED _IOR(MAGIC_READ_UW, 0x17, int32_t*)
#define R_UW_TDP0         _IOR(MAGIC_READ_UW, 0x18, int32_t*)
#define R_UW_TDP1         _IOR(MAGIC_READ_UW, 0x19, int32_t*)
#define R_UW_TDP2         _IOR(MAGIC_READ_UW, 0x1a, int32_t*)
#define R_UW_TDP0_MIN     _IOR(MAGIC_READ_UW, 0x1b, int32_t*)
#define R_UW_TDP1_MIN     _IOR(MAGIC_READ_UW, 0x1c, int32_t*)
#define R_UW_TDP2_MIN     _IOR(MAGIC_READ_UW, 0x1d, int32_t*)
#define R_UW_TDP0_MAX     _IOR(MAGIC_READ_UW, 0x1e, int32_t*)
#define R_UW_TDP1_MAX     _IOR(MAGIC_READ_UW, 0x1f, int32_t*)
#define R_UW_TDP2_MAX     _IOR(MAGIC_READ_UW, 0x20, int32_t*)
#define R_UW_PROFS_AVAILABLE _IOR(MAGIC_READ_UW, 0x21, int32_t*)

/* Uniwill writes */
#define W_UW_FANSPEED     _IOW(MAGIC_WRITE_UW, 0x10, int32_t*)
#define W_UW_FANSPEED2    _IOW(MAGIC_WRITE_UW, 0x11, int32_t*)
#define W_UW_MODE         _IOW(MAGIC_WRITE_UW, 0x12, int32_t*)
#define W_UW_MODE_ENABLE  _IOW(MAGIC_WRITE_UW, 0x13, int32_t*)
#define W_UW_FANAUTO      _IO(MAGIC_WRITE_UW, 0x14)
#define W_UW_TDP0         _IOW(MAGIC_WRITE_UW, 0x15, int32_t*)
#define W_UW_TDP1         _IOW(MAGIC_WRITE_UW, 0x16, int32_t*)
#define W_UW_TDP2         _IOW(MAGIC_WRITE_UW, 0x17, int32_t*)
#define W_UW_PERF_PROF    _IOW(MAGIC_WRITE_UW, 0x18, int32_t*)

static int fd;
static int is_clevo, is_uniwill;

#define CHECK_OPEN \
    do { if (fd < 0) { fprintf(stderr, "ERROR: /dev/tuxedo_io not found or not accessible\n"); return; } } while(0)

#define IOCTL_READ(cmd, var, label) \
    do { \
        if (ioctl(fd, cmd, &var) == 0) \
            printf("  %-40s = %d (0x%x)\n", label, var, var); \
        else \
            printf("  %-40s = FAILED\n", label); \
    } while(0)

void test_general(void) {
    CHECK_OPEN;
    printf("\n=== GENERAL INFO ===\n");
    char version[64] = {0};
    if (ioctl(fd, R_MOD_VERSION, version) == 0)
        printf("  Driver version = %s\n", version);
    else
        printf("  Driver version = FAILED\n");

    IOCTL_READ(R_HWCHECK_CL, is_clevo,   "Hardware check: Clevo");
    IOCTL_READ(R_HWCHECK_UW, is_uniwill, "Hardware check: Uniwill");

    char if_str[32] = {0};
    if (is_clevo) {
        if (ioctl(fd, R_CL_HW_IF_STR, if_str) == 0)
            printf("  Clevo interface          = %s\n", if_str);
        else
            printf("  Clevo interface          = FAILED\n");
    }
    if (is_uniwill) {
        if (ioctl(fd, R_UW_HW_IF_STR, if_str) == 0)
            printf("  Uniwill interface        = %s\n", if_str);
        else
            printf("  Uniwill interface        = FAILED\n");
        IOCTL_READ(R_UW_MODEL_ID, is_uniwill, "Uniwill model ID");
    }
}

/* ======================== CLEVO ======================== */

void clevo_read_all(void) {
    if (!is_clevo) return;
    CHECK_OPEN;
    printf("\n=== CLEVO: READ SENSORS & SWITCHES ===\n");

    uint32_t fan1=0, fan2=0, fan3=0;
    if (ioctl(fd, R_CL_FANINFO1, &fan1) == 0 &&
        ioctl(fd, R_CL_FANINFO2, &fan2) == 0 &&
        ioctl(fd, R_CL_FANINFO3, &fan3) == 0) {
        printf("  Fan info 1 (ACPI 0x63)   = 0x%08x\n", fan1);
        printf("    Byte0-1 = Fan1 speed/status\n");
        printf("    Byte2-3 = Fan2 speed/status\n");
        printf("  Fan info 2 (ACPI 0x64)   = 0x%08x\n", fan2);
        printf("  Fan info 3 (ACPI 0x6E)   = 0x%08x\n", fan3);
    } else {
        printf("  Fan info                 = FAILED\n");
    }

    IOCTL_READ(R_CL_WEBCAM_SW,    fan1, "Webcam switch    (ACPI 0x06)");
    IOCTL_READ(R_CL_FLIGHTMODE_SW, fan2, "Flight mode sw   (ACPI 0x07)");
    IOCTL_READ(R_CL_TOUCHPAD_SW,  fan3, "Touchpad switch  (ACPI 0x09)");
}

void clevo_write_tests(void) {
    if (!is_clevo) return;
    CHECK_OPEN;
    printf("\n=== CLEVO: WRITE TESTS (READ-ONLY, NOT EXECUTED) ===\n");
    printf("  To set fan speed  (ACPI 0x68):\n");
    printf("    int32_t val = (fan3 << 16) | (fan2 << 8) | fan1;  // 0-255 per fan\n");
    printf("    ioctl(fd, W_CL_FANSPEED, &val);\n\n");
    printf("  To restore fan auto (ACPI 0x69):\n");
    printf("    int32_t mask = 0x0F;  // bitmask: bit0=CPU, bit1=GPU, bit2=Chassis\n");
    printf("    ioctl(fd, W_CL_FANAUTO, &mask);\n\n");
    printf("  To set webcam    (ACPI 0x22):\n");
    printf("    int32_t val = 0;  // off, or 1 = on\n");
    printf("    ioctl(fd, W_CL_WEBCAM_SW, &val);\n\n");
    printf("  To set flight mode (ACPI 0x20):\n");
    printf("    int32_t val = 0;  // off, or 1 = on\n");
    printf("    ioctl(fd, W_CL_FLIGHTMODE_SW, &val);\n\n");
    printf("  To set touchpad  (ACPI 0x2A):\n");
    printf("    int32_t val = 0;  // off, or 1 = on\n");
    printf("    ioctl(fd, W_CL_TOUCHPAD_SW, &val);\n\n");
    printf("  To set performance profile (ACPI 0x79 sub 0x19):\n");
    printf("    int32_t val = 0;  // 0=econo, 1=high, 2=perf, 3=auto\n");
    printf("    ioctl(fd, W_CL_PERF_PROFILE, &val);\n");
}

/* ======================== UNIWILL ======================== */

void uniwill_read_all(void) {
    if (!is_uniwill) return;
    CHECK_OPEN;
    printf("\n=== UNIWILL: READ SENSORS ===\n");

    uint32_t val=0;
    IOCTL_READ(R_UW_FANSPEED,  val, "Fan speed 1  (EC 0x1804)");
    IOCTL_READ(R_UW_FANSPEED2, val, "Fan speed 2  (EC 0x1809)");
    IOCTL_READ(R_UW_FAN_TEMP,  val, "Fan temp 1   (EC 0x043E)");
    IOCTL_READ(R_UW_FAN_TEMP2, val, "Fan temp 2   (EC 0x044F)");
    IOCTL_READ(R_UW_MODE,      val, "Profile mode (EC 0x0751)");
    IOCTL_READ(R_UW_MODE_ENABLE, val, "Mode enable  (EC 0x0741)");
    IOCTL_READ(R_UW_FANS_OFF_AVAILABLE, val, "Fans-off available");
    IOCTL_READ(R_UW_FANS_MIN_SPEED, val, "Min fan speed %");
    IOCTL_READ(R_UW_TDP0,     val, "TDP PL1 current");
    IOCTL_READ(R_UW_TDP1,     val, "TDP PL2 current");
    IOCTL_READ(R_UW_TDP2,     val, "TDP PL4 current");
    IOCTL_READ(R_UW_TDP0_MIN, val, "TDP PL1 min");
    IOCTL_READ(R_UW_TDP1_MIN, val, "TDP PL2 min");
    IOCTL_READ(R_UW_TDP2_MIN, val, "TDP PL4 min");
    IOCTL_READ(R_UW_TDP0_MAX, val, "TDP PL1 max");
    IOCTL_READ(R_UW_TDP1_MAX, val, "TDP PL2 max");
    IOCTL_READ(R_UW_TDP2_MAX, val, "TDP PL4 max");
    IOCTL_READ(R_UW_PROFS_AVAILABLE, val, "Profiles available");
}

void uniwill_write_tests(void) {
    if (!is_uniwill) return;
    CHECK_OPEN;
    printf("\n=== UNIWILL: WRITE TESTS (READ-ONLY, NOT EXECUTED) ===\n");
    printf("  Set fan 1 speed:\n");
    printf("    int32_t val = 128;  // 0-255\n");
    printf("    ioctl(fd, W_UW_FANSPEED, &val);\n\n");
    printf("  Set fan 2 speed:\n");
    printf("    ioctl(fd, W_UW_FANSPEED2, &val);\n\n");
    printf("  Restore fan auto:\n");
    printf("    ioctl(fd, W_UW_FANAUTO);\n\n");
    printf("  Set profile mode (EC 0x0751):\n");
    printf("    int32_t val = 0x10;  // profile value\n");
    printf("    ioctl(fd, W_UW_MODE, &val);\n\n");
    printf("  Set TDP PL1:\n");
    printf("    int32_t val = 35;  // watts\n");
    printf("    ioctl(fd, W_UW_TDP0, &val);\n\n");
    printf("  Set performance profile v1:\n");
    printf("    int32_t val = 1;  // 0=power save, 1=balanced, 2=performance, 3=overboost\n");
    printf("    ioctl(fd, W_UW_PERF_PROF, &val);\n");
}

/* ======================== PRINT SUMMARY ======================== */

void print_sysfs_controls(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║         SYFS CONTROL COMMANDS (run in shell)               ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    printf("── Keyboard Backlight (ACPI 0x67) ──\n");
    printf("  # Set RGB color:\n");
    printf("  echo \"255 0 0\" > /sys/class/leds/rgb:kbd_backlight/multi_intensity\n");
    printf("\n");
    printf("  # Set brightness (all RGB zones):\n");
    printf("  echo 128 > /sys/class/leds/rgb:kbd_backlight/brightness\n");
    printf("\n");
    printf("  # White-only backlight (ACPI 0x27):\n");
    printf("  echo 1 > /sys/class/leds/white:kbd_backlight/brightness\n");
    printf("\n");
    printf("  # Animation mode (3-zone RGB only, ACPI 0x67 mode select):\n");
    printf("  echo 0 > /sys/devices/platform/tuxedo_keyboard/kbd_backlight_mode\n");
    printf("    # 0=CUSTOM  1=BREATHE  2=CYCLE  3=DANCE\n");
    printf("    # 4=FLASH   5=RANDOM   6=TEMPO  7=WAVE\n");
    printf("\n");

    printf("── Fn Lock (ACPI 0x04 / 0x19) ──\n");
    printf("  cat /sys/devices/platform/tuxedo_keyboard/fn_lock\n");
    printf("  echo 1 > /sys/devices/platform/tuxedo_keyboard/fn_lock  # enable\n");
    printf("  echo 0 > /sys/devices/platform/tuxedo_keyboard/fn_lock  # disable\n");
    printf("\n");

    printf("── Battery FlexiCharger (ACPI 0x76/0x77 or 0x04/0x1e) ──\n");
    printf("  # Find your battery:\n");
    printf("  ls /sys/class/power_supply/\n");
    printf("\n");
    printf("  # Enable charge limiting at 60%%-80%%:\n");
    printf("  echo \"Custom\" > /sys/class/power_supply/BAT0/charge_type\n");
    printf("  echo 60 > /sys/class/power_supply/BAT0/charge_control_start_threshold\n");
    printf("  echo 80 > /sys/class/power_supply/BAT0/charge_control_end_threshold\n");
    printf("\n");
    printf("  # Available thresholds:\n");
    printf("  cat /sys/class/power_supply/BAT0/charge_control_start_available_thresholds\n");
    printf("  cat /sys/class/power_supply/BAT0/charge_control_end_available_thresholds\n");
    printf("\n");

    printf("── Module load with force_backlight_type ──\n");
    printf("  sudo rmmod tuxedo_keyboard; sudo insmod src/tuxedo_keyboard.ko force_backlight_type=6\n");
    printf("  # 1=fixed white  2=3-zone RGB  6=1-zone RGB  243=per-key RGB\n");
}

/* ======================== MAIN ======================== */

int main(int argc, char **argv) {
    int do_read = 1, do_write = 0;

    if (argc > 1) {
        if (strcmp(argv[1], "read") == 0)    { do_read = 1; do_write = 0; }
        else if (strcmp(argv[1], "write") == 0) { do_read = 0; do_write = 1; }
        else if (strcmp(argv[1], "all") == 0)   { do_read = 1; do_write = 1; }
        else { fprintf(stderr, "Usage: %s [read|write|all]\n", argv[0]); return 1; }
    }

    fd = open("/dev/tuxedo_io", O_RDWR);
    if (fd < 0)
        perror("Warning: /dev/tuxedo_io");

    test_general();

    if (do_read) {
        clevo_read_all();
        uniwill_read_all();
    }

    if (do_write) {
        clevo_write_tests();
        uniwill_write_tests();
    }

    print_sysfs_controls();

    /* ── ACPI commands NOT exposed via ioctl/sysfs ── */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║   ACPI COMMANDS NOT EXPOSED (kernel-internal only)         ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  These ACPI _DSM commands are called internally by the\n");
    printf("  driver and NOT exposed to userspace. To call them manually,\n");
    printf("  install acpi_call:\n");
    printf("\n");
    printf("    git clone https://github.com/mkottman/acpi_call\n");
    printf("    cd acpi_call && make && sudo insmod acpi_call.ko\n");
    printf("\n");
    printf("  Then call ACPI _DSM with UUID %s\n",
           "93f224e4-fbdc-4bbf-add6-db71bdc0afad");
    printf("  on the CLV0001 device:\n");
    printf("\n");
    printf("  Your CLV0001 ACPI path is: \\_SB_.DCHU\n");
    printf("\n");
    printf("  # Query _DSM supported functions (function index 0):\n");
    printf("    echo \"\\\\_SB_.DCHU._DSM{0,0,0,\\\"93f224e4-fbdc-4bbf-add6-db71bdc0afad\\\"}\" > /proc/acpi/call\n");
    printf("    cat /proc/acpi/call\n");
    printf("\n");
    printf("  # CLEVO_CMD_GET_SPECS (function 0x0D) — detect keyboard type:\n");
    printf("    echo \"\\\\_SB_.DCHU._DSM{0x0D,0,0,{0,0,0,0}}\" > /proc/acpi/call\n");
    printf("    hexdump -C /sys/kernel/debug/acpi/call\n");
    printf("\n");
    printf("  # CLEVO_CMD_GET_BIOS_FEATURES_1 (function 0x52):\n");
    printf("    echo \"\\\\_SB_.DCHU._DSM{0x52,0,0,{0,0,0,0}}\" > /proc/acpi/call\n");
    printf("    cat /proc/acpi/call\n");
    printf("\n");
    printf("  # CLEVO_CMD_GET_BIOS_FEATURES_2 (function 0x7A):\n");
    printf("    echo \"\\\\_SB_.DCHU._DSM{0x7A,0,0,{0,0,0,0}}\" > /proc/acpi/call\n");
    printf("    cat /proc/acpi/call\n");
    printf("\n");
    printf("  # CLEVO_CMD_GET_KB_WHITE_LEDS (function 0x3D):\n");
    printf("    echo \"\\\\_SB_.DCHU._DSM{0x3D,0,0,{0,0,0,0}}\" > /proc/acpi/call\n");
    printf("\n");
    printf("  # CLEVO_CMD_SET_EVENTS_ENABLED (function 0x46, arg=0):\n");
    printf("    echo \"\\\\_SB_.DCHU._DSM{0x46,0,0,{0,0,0,0}}\" > /proc/acpi/call\n");

    if (fd >= 0) close(fd);
    return 0;
}
