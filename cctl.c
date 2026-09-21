/*
 * cctl - Lightweight Clevo P15 performance profile & fan controller
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 bhusann
 *
 * Pure C, no dependencies beyond libc. Direct EC port I/O for fan control,
 * sysfs writes for CPU power management.
 *
 * Build:  gcc -o cctl cctl.c -Os -s
 * Usage:  sudo ./cctl set <profile>
 *         sudo ./cctl fan <mode> [value]
 *         sudo ./cctl status
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/io.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sched.h>
#include <sys/wait.h>
#include <limits.h>
#include <pwd.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/file.h>

#define CCTL_VERSION      "3.0"
/* NOTE FOR DEVELOPERS / AI AGENTS:
 * Always increment CCTL_MICROVERSION (a 6-digit integer) whenever making code
 * changes and committing. 'cctl install' checks this hidden value to determine
 * if a local binary is newer than /usr/local/bin/cctl. Do NOT document this in
 * README or help menus. */
#define CCTL_MICROVERSION 100004

/* ========================================================================
 * ANSI COLOR SUPPORT
 * ======================================================================== */
static int use_color;

static const char *C_RST, *C_BLD, *C_DIM, *C_CYN, *C_CYN_BLD, *C_YLW, *C_WHT, *C_RED, *C_GRN, *C_MAG, *C_BLU;

static void init_colors(void)
{
    if (use_color) {
        C_RST = "\033[0m";   C_BLD = "\033[1m";    C_DIM = "\033[2m";
        C_CYN = "\033[36m";  C_CYN_BLD = "\033[1;36m";
        C_YLW = "\033[1;33m"; C_WHT = "\033[1;37m";
        C_RED = "\033[1;31m"; C_GRN = "\033[1;32m"; C_MAG = "\033[1;35m";
        C_BLU = "\033[1;34m";
    } else {
        C_RST = C_BLD = C_DIM = C_CYN = C_CYN_BLD = C_YLW = C_WHT = "";
        C_RED = C_GRN = C_MAG = C_BLU = "";
    }
}

/* Best-effort command/chown wrappers: intentionally discard the return
 * value in a way that satisfies -Wunused-result on every gcc (assigning
 * the result to a variable counts as "used"; the (void) silences the
 * unused-variable warning). */
static void run_quiet(const char *cmd) { int r = system(cmd); (void)r; }

/* Re-execute the current command with sudo if not already running as root. */
static void self_elevate(int argc, char **argv)
{
    if (geteuid() == 0) return;

    char exe_path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    const char *bin = argv[0];
    if (n > 0) {
        exe_path[n] = '\0';
        bin = exe_path;
    }

    char **args = malloc((size_t)(argc + 2) * sizeof(char *));
    if (!args) {
        fprintf(stderr, "Error: memory allocation failed during self-elevation\n");
        exit(1);
    }
    args[0] = "sudo";
    args[1] = (char *)bin;
    for (int i = 1; i < argc; i++) {
        args[i + 1] = argv[i];
    }
    args[argc + 1] = NULL;

    execvp("sudo", args);
    perror("Error: failed to re-run with sudo");
    free(args);
    exit(1);
}

static int read_cpu_temp(void);
static int read_fan_telemetry_ex(int *cpu_pct, int *gpu_pct, int *cpu_rpm, int *gpu_rpm, int cached_fd);
#define read_fan_telemetry(c, g, cr, gr) read_fan_telemetry_ex(c, g, cr, gr, -1)
static int is_cpu_e_core(int cpu_num);
#ifdef CCTL_NVIDIA
static int nvidia_is_blacklisted(void);
static int nvidia_is_loaded(void);
#endif
static int bat_read_start(void);
static int bat_read_end(void);

/* ========================================================================
 * EC PORT I/O
 * ======================================================================== */

#define EC_CMD_PORT  0x66
#define EC_DATA_PORT 0x62

static int ec_ports_acquired = 0;

static int ec_acquire_ports(void)
{
    if (ec_ports_acquired)
        return 0;
    if (ioperm(EC_DATA_PORT, 1, 1) != 0 || ioperm(EC_CMD_PORT, 1, 1) != 0) {
        fprintf(stderr, "Error: Failed to get EC port permissions (run as root)\n");
        return -1;
    }
    ec_ports_acquired = 1;
    return 0;
}

static void ec_release_ports(void)
{
    if (ec_ports_acquired) {
        ioperm(EC_DATA_PORT, 1, 0);
        ioperm(EC_CMD_PORT, 1, 0);
        ec_ports_acquired = 0;
    }
}

static int wait_ibf(void)
{
    int i = 0;
    while (((inb(EC_CMD_PORT) >> 1) & 0x1) != 0 && i < 10000) {
        usleep(10);
        i++;
    }
    if (i >= 10000) {
        fprintf(stderr, "Error: EC timeout (IBF)\n");
        return -1;
    }
    return 0;
}

static int send_ec_cmd(uint8_t cmd, const uint8_t *data, int len)
{
    if (ec_acquire_ports() < 0)
        return -1;

    if (wait_ibf() < 0) return -1;
    outb(cmd, EC_CMD_PORT);

    for (int i = 0; i < len; i++) {
        if (wait_ibf() < 0) return -1;
        outb(data[i], EC_DATA_PORT);
    }
    return 0;
}

/* ========================================================================
 * SYSFS HELPERS
 * ======================================================================== */

static int write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

/* Read a sysfs file into buf (stripping trailing newlines). Returns bytes read or -1. */
static int read_sysfs_str(const char *path, char *buf, size_t bufsize)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufsize - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    for (char *p = buf; *p; p++)
        if (*p == '\n' || *p == '\r') *p = '\0';
    return (int)n;
}

/* Read a sysfs file as a long integer. Returns the value or fallback on failure. */
static long read_sysfs_long(const char *path, long fallback)
{
    char buf[32];
    if (read_sysfs_str(path, buf, sizeof(buf)) < 0) return fallback;
    return atol(buf);
}

/* Parse an integer with validation. Returns 0 on success, -1 on failure. */
static int safe_atoi(const char *str, int *out)
{
    if (!str || !*str) return -1;
    char *end;
    errno = 0;
    long val = strtol(str, &end, 10);
    if (errno != 0 || *end != '\0' || val < INT_MIN || val > INT_MAX)
        return -1;
    *out = (int)val;
    return 0;
}

/* ========================================================================
 * GPU MUX SWITCHING (UEFI NVRAM)
 * ========================================================================
 * Board: COLORFUL P15 23 (Insyde H2O BIOS).
 * Setup-a04a27f4-df00-4d42-b552-39511302113d, file offset 430:
 *   0x03 = MSHybrid (iGPU + dGPU), 0x02 = dGPU only.
 * MUX is latched at POST — write stages the request, reboot applies it.
 * SaSetup side-effect bytes are firmware-synced, never touched here.
 * ======================================================================== */

#define MUX_VAR_PATH "/sys/firmware/efi/efivars/Setup-a04a27f4-df00-4d42-b552-39511302113d"
#define MUX_OFFSET      430
#define MUX_EXPECTED_LEN 1204
#define MUX_VAL_DGPU     0x02
#define MUX_VAL_MSHYBRID 0x03

/* Read the MUX byte from the UEFI Setup variable.
 * Returns MUX_VAL_DGPU / MUX_VAL_MSHYBRID on success, -1 on error. */
static int mux_read(void)
{
    int fd = open(MUX_VAR_PATH, O_RDONLY);
    if (fd < 0) return -1;

    unsigned char blob[MUX_EXPECTED_LEN + 16];
    ssize_t n = read(fd, blob, sizeof(blob));
    close(fd);

    if (n != MUX_EXPECTED_LEN) return -1;
    int val = blob[MUX_OFFSET];
    if (val != MUX_VAL_DGPU && val != MUX_VAL_MSHYBRID) return -1;
    return val;
}

static const char *mux_mode_str(int val)
{
    if (val == MUX_VAL_MSHYBRID) return "MSHybrid";
    if (val == MUX_VAL_DGPU)     return "dGPU";
    return "Unknown";
}

/* Detect the actual running MUX mode from PCI topology.
 * Intel iGPU at 00:02.0 with display class 0x0300xx → MSHybrid;
 * absent or non-display → dGPU.  Returns MUX_VAL_*, or -1 on error. */
static int mux_running_mode(void)
{
    char cls[16] = {0};
    if (read_sysfs_str("/sys/bus/pci/devices/0000:00:02.0/class", cls, sizeof(cls)) < 0)
        return MUX_VAL_DGPU; /* device absent → dGPU */
    /* class is e.g. "0x030000" for VGA-compatible controller */
    unsigned long val = strtoul(cls, NULL, 16);
    return ((val >> 8) == 0x0300) ? MUX_VAL_MSHYBRID : MUX_VAL_DGPU;
}

static void mux_show(void)
{
    int nvram = mux_read();
    if (nvram < 0) {
        printf("  %-14s %sN/A (NVRAM variable not found or unrecognized)%s\n", "GPU MUX:", C_DIM, C_RST);
        return;
    }
    int running = mux_running_mode();
    const char *col = (running == MUX_VAL_MSHYBRID) ? C_GRN : C_MAG;
    if (running >= 0 && nvram != running)
        printf("  %-14s %s%s%s  %s← %s pending (reboot to apply)%s\n",
               "GPU MUX:",
               col, mux_mode_str(running), C_RST,
               C_YLW, mux_mode_str(nvram), C_RST);
    else
        printf("  %-14s %s%s%s\n", "GPU MUX:", col, mux_mode_str(nvram), C_RST);
}

/* Toggle the MUX to the opposite mode.  Returns 0 on success, 1 on error. */
static int mux_switch(void)
{
    /* Preflight: NVRAM var must exist */
    if (access(MUX_VAR_PATH, F_OK) != 0) {
        fprintf(stderr, "Error: NVRAM variable not found: %s\n", MUX_VAR_PATH);
        return 1;
    }

    /* Preflight: must be root */
    if (geteuid() != 0) {
        fprintf(stderr, "Error: MUX switch requires root (sudo cctl mux switch)\n");
        return 1;
    }

    /* Read the full blob */
    int fd = open(MUX_VAR_PATH, O_RDONLY);
    if (fd < 0) {
        perror("Error: cannot open NVRAM variable for reading");
        return 1;
    }
    unsigned char blob[MUX_EXPECTED_LEN + 16];
    ssize_t n = read(fd, blob, sizeof(blob));
    close(fd);

    if (n != MUX_EXPECTED_LEN) {
        fprintf(stderr, "Error: NVRAM variable is %zd bytes, expected %d — refusing.\n",
                n, MUX_EXPECTED_LEN);
        return 1;
    }

    int current = blob[MUX_OFFSET];
    if (current != MUX_VAL_DGPU && current != MUX_VAL_MSHYBRID) {
        fprintf(stderr, "Error: byte @%d is 0x%02x, not 0x%02x/0x%02x — refusing "
                "(BIOS update may have relocated the option).\n",
                MUX_OFFSET, current, MUX_VAL_DGPU, MUX_VAL_MSHYBRID);
        return 1;
    }

    int target = (current == MUX_VAL_MSHYBRID) ? MUX_VAL_DGPU : MUX_VAL_MSHYBRID;

    /* Confirmation prompt */
    printf("Switch GPU MUX mode from %s%s%s to %s%s%s?\n",
           C_DIM, mux_mode_str(current), C_RST,
           C_BLD, mux_mode_str(target), C_RST);
    printf("Are you sure you want to switch to %s? [y/N] ", mux_mode_str(target));
    fflush(stdout);

    char ans[32] = {0};
    if (!fgets(ans, sizeof(ans), stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
        printf("Aborted.\n");
        return 0;
    }

    /* Clear immutability (kernel re-marks every var immutable on each boot) */
    run_quiet("chattr -i " MUX_VAR_PATH);  /* best-effort; ignore if chattr missing */

    /* Write the toggled blob */
    blob[MUX_OFFSET] = (unsigned char)target;
    fd = open(MUX_VAR_PATH, O_WRONLY);
    if (fd < 0) {
        perror("Error: cannot open NVRAM variable for writing");
        return 1;
    }
    ssize_t written = write(fd, blob, (size_t)n);
    close(fd);
    if (written != n) {
        fprintf(stderr, "Error: short write (%zd/%zd bytes).\n", written, n);
        return 1;
    }

    /* Readback verify */
    int readback = mux_read();
    if (readback != target) {
        fprintf(stderr, "Error: readback 0x%02x != target 0x%02x — write may have failed!\n",
                readback, target);
        return 1;
    }

    printf("GPU MUX switched: %s%s%s → %s%s%s\n",
           C_DIM, mux_mode_str(current), C_RST,
           C_BLD, mux_mode_str(target), C_RST);
    printf("%sReboot to apply.%s\n", C_YLW, C_RST);
    return 0;
}

/* Iterate /sys/devices/system/cpu/cpuN/<file> for all online CPUs */
static int write_to_all_cpus(const char *suffix, const char *value)
{
    DIR *d = opendir("/sys/devices/system/cpu");
    if (!d) return -1;

    struct dirent *ent;
    int touched = 0;
    char path[512];

    while ((ent = readdir(d)) != NULL) {
        /* Match "cpu0", "cpu1", etc. — skip "cpufreq", "cpuidle" */
        if (strncmp(ent->d_name, "cpu", 3) != 0)
            continue;
        if (ent->d_name[3] < '0' || ent->d_name[3] > '9')
            continue;

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/%s/%s", ent->d_name, suffix);

        if (write_sysfs(path, value) == 0)
            touched++;
    }
    closedir(d);
    return touched > 0 ? 0 : -1;
}

/* ========================================================================
 * FN LOCK
 * ======================================================================== */

#define FNLOCK_PATH "/sys/devices/platform/tuxedo_keyboard/fn_lock"

static int set_fnlock(int enabled)
{
    if (access(FNLOCK_PATH, F_OK) != 0) {
        fprintf(stderr, "Error: Fn Lock not available (tuxedo_keyboard not loaded?)\n");
        return -1;
    }
    const char *val = enabled ? "1" : "0";
    if (write_sysfs(FNLOCK_PATH, val) < 0) {
        fprintf(stderr, "Error: Failed to set Fn Lock to %s\n", enabled ? "ON" : "OFF");
        return -1;
    }
    printf("  Fn Lock: %s\n", enabled ? "ON" : "OFF");
    return 0;
}

static int get_fnlock(void)
{
    char buf[16];
    if (read_sysfs_str(FNLOCK_PATH, buf, sizeof(buf)) < 0)
        return -1;
    return atoi(buf) ? 1 : 0;
}

static int fnlock_toggle(void)
{
    int cur = get_fnlock();
    if (cur < 0) {
        fprintf(stderr, "Error: Fn Lock not available (tuxedo_keyboard not loaded?)\n");
        return -1;
    }
    return set_fnlock(!cur);
}

/* ========================================================================
 * CPU TURBO
 * ======================================================================== */

#define TURBO_PATH "/sys/devices/system/cpu/intel_pstate/no_turbo"

static int set_turbo(int enabled)
{
    if (access(TURBO_PATH, F_OK) != 0) {
        fprintf(stderr, "Warning: intel_pstate not available, skipping turbo\n");
        return 0; /* non-fatal on AMD/non-pstate */
    }
    /* no_turbo: 0 = turbo ON, 1 = turbo OFF */
    const char *val = enabled ? "0" : "1";
    if (write_sysfs(TURBO_PATH, val) < 0) {
        fprintf(stderr, "Error: Failed to set turbo to %s\n", enabled ? "ON" : "OFF");
        return -1;
    }
    printf("  Turbo boost: %s\n", enabled ? "ON" : "OFF");
    return 0;
}

/* ========================================================================
 * CPU GOVERNOR
 * ======================================================================== */

static int set_governor(const char *gov)
{
    printf("  Governor: %s\n", gov);
    return write_to_all_cpus("cpufreq/scaling_governor", gov);
}

/* ========================================================================
 * CPU EPP (Energy Performance Preference)
 * ======================================================================== */

static int set_epp(const char *val)
{
    /* Check that intel_pstate EPP path exists at all */
    const char *probe = "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference";
    if (access(probe, F_OK) != 0) {
        fprintf(stderr, "Warning: EPP not available (no intel_pstate), skipping\n");
        return 0;
    }
    printf("  EPP: %s\n", val);
    return write_to_all_cpus("cpufreq/energy_performance_preference", val);
}

/* ========================================================================
 * RAPL POWER LIMITS
 * ======================================================================== */

/* Read a RAPL sysfs file and return value in watts, or -1 on failure */
static long read_rapl_watts(const char *path)
{
    long val = read_sysfs_long(path, -1000000);
    return (val <= -1000000) ? -1 : val / 1000000;
}

/* Read current RAPL PL1/PL2 from package-0. Returns 0 on success. */
static int read_rapl_current(int *pl1, int *pl2)
{
    *pl1 = -1;
    *pl2 = -1;
    char path[512];
    snprintf(path, sizeof(path),
             "/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw");
    *pl1 = (int)read_rapl_watts(path);
    snprintf(path, sizeof(path),
             "/sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw");
    *pl2 = (int)read_rapl_watts(path);
    return (*pl1 >= 0 || *pl2 >= 0) ? 0 : -1;
}

/* Set RAPL power limits. Pass pl1_w <= 0 to skip PL1. */
static int set_rapl_limits(int pl1_w, int pl2_w)
{
    int old_pl1 = -1, old_pl2 = -1;
    read_rapl_current(&old_pl1, &old_pl2);

    char pl1_str[32], pl2_str[32];
    if (pl1_w > 0) snprintf(pl1_str, sizeof(pl1_str), "%d000000", pl1_w);
    if (pl2_w > 0) snprintf(pl2_str, sizeof(pl2_str), "%d000000", pl2_w);

    DIR *d = opendir("/sys/class/powercap");
    if (!d) {
        fprintf(stderr, "Warning: RAPL not available (no /sys/class/powercap)\n");
        return 0;
    }

    struct dirent *ent;
    char path[576];

    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "intel-rapl:", 11) != 0) continue;

        /* Skip sub-zones (intel-rapl:0:0, intel-rapl:0:1, etc.)
           and psys (intel-rapl:1). Only write to package-0.
           Writing to psys or sub-zones causes EC conflict → 0.4GHz throttle. */
        if (strcmp(ent->d_name, "intel-rapl:0") != 0) continue;

        /* Only adjust zones whose constraint_0 is "long_term" */
        char buf[64] = {0};
        snprintf(path, sizeof(path), "/sys/class/powercap/%s/constraint_0_name", ent->d_name);
        if (read_sysfs_str(path, buf, sizeof(buf)) < 0) continue;
        if (strcmp(buf, "long_term") != 0) continue;

        /* Write PL1 if requested */
        if (pl1_w > 0) {
            snprintf(path, sizeof(path), "/sys/class/powercap/%s/constraint_0_power_limit_uw",
                     ent->d_name);
            write_sysfs(path, pl1_str);
        }

        /* Write PL2 (only when requested — never write a skipped/negative limit) */
        snprintf(path, sizeof(path), "/sys/class/powercap/%s/constraint_1_power_limit_uw",
                 ent->d_name);
        if (pl2_w > 0 && access(path, W_OK) == 0)
            write_sysfs(path, pl2_str);
    }
    closedir(d);

    if (pl1_w > 0 && pl2_w > 0) {
        if (old_pl1 >= 0 && old_pl2 >= 0)
            printf("  RAPL: PL1 %dW -> %dW, PL2 %dW -> %dW\n", old_pl1, pl1_w, old_pl2, pl2_w);
        else
            printf("  RAPL: PL1=%dW PL2=%dW\n", pl1_w, pl2_w);
    } else if (pl1_w > 0) {
        if (old_pl1 >= 0)
            printf("  RAPL: PL1 %dW -> %dW\n", old_pl1, pl1_w);
        else
            printf("  RAPL: PL1=%dW\n", pl1_w);
    } else if (pl2_w > 0) {
        if (old_pl2 >= 0)
            printf("  RAPL: PL2 %dW -> %dW\n", old_pl2, pl2_w);
        else
            printf("  RAPL: PL2=%dW\n", pl2_w);
    }
    return 0;
}

/* ========================================================================
 * GPU PROFILE (via /dev/tuxedo_io IOCTL — best effort)
 * ======================================================================== */

#include <sys/ioctl.h>

#define R_HWCHECK_CL       0x8008EC05
#define W_CL_PERF_PROFILE  0x4008EE15

static const char *gpu_profile_name(int profile)
{
    static const char *names[] = { "quiet", "standard", "performance", "turbo" };
    if (profile >= 0 && profile <= 3) return names[profile];
    return "unknown";
}

/* Open /dev/tuxedo_io and verify Clevo hardware. Returns fd or -1. */
static int tuxedo_open_clevo(void)
{
    if (access("/dev/tuxedo_io", F_OK) != 0)
        return -1;
    int fd = open("/dev/tuxedo_io", O_RDWR);
    if (fd < 0) return -1;
    int is_cl = 0;
    if (ioctl(fd, R_HWCHECK_CL, &is_cl) < 0 || !is_cl) {
        close(fd);
        return -1;
    }
    return fd;
}

static int set_gpu_profile_tuxedo(int profile)
{
    int fd = tuxedo_open_clevo();
    if (fd < 0) return -1;

    int arg = profile;
    if (ioctl(fd, W_CL_PERF_PROFILE, &arg) < 0) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int set_gpu_profile(int profile)
{
    if (profile < 0 || profile > 3) {
        fprintf(stderr, "Error: Invalid GPU profile %d (must be 0-3)\n", profile);
        return -1;
    }

    printf("  GPU profile: %s (%d)\n", gpu_profile_name(profile), profile);

    if (set_gpu_profile_tuxedo(profile) == 0)
        return 0;

    fprintf(stderr, "  Warning: tuxedo_io not available, GPU profile not set\n");
    return 0; /* non-fatal: CPU settings still applied */
}

/* ========================================================================
 * FAN CONTROL
 * ========================================================================
 *
 * EC byte-order note:
 * This Clevo EC uses DIFFERENT argument orders for cmd 0x99 depending on
 * context.  After a mode command (0x98), the follow-up 0x99 takes
 * { fan_idx, value }.  For standalone auto-restore, 0x99 takes
 * { 0xFF, fan_idx }.  fan_set_duty() also uses { fan_idx, raw_duty }.
 * This is quirky but confirmed working via live testing (max → 6700 RPM,
 * silent → 950 RPM, cpu 50 → 3789 RPM).  Do not "fix" the byte order.
 * ======================================================================== */

#define FAN_CPU 1
#define FAN_GPU 2

#define EC_CMD_FAN_MODE   0x98
#define EC_CMD_FAN_SPEED  0x99
#define FAN_MODE_MAX      0x40
#define FAN_MODE_SILENT   0x20
#define FAN_DUTY_AUTO     0xFF
#define EC_FAN_RPM_DIVISOR 2156220

static int fan_auto(int fan_idx)
{
    /* EC quirk: standalone 0x99 auto-restore uses { 0xFF, fan_idx } order.
     * Do NOT swap to { fan_idx, 0xFF } — that only works after a mode cmd 0x98. */
    uint8_t data[2] = { FAN_DUTY_AUTO, (uint8_t)fan_idx };
    return send_ec_cmd(EC_CMD_FAN_SPEED, data, 2);
}

static int fan_auto_all(void)
{
    if (fan_auto(FAN_CPU) < 0) return -1;
    return fan_auto(FAN_GPU);
}

static int fan_max_all(void)
{
    uint8_t mode = FAN_MODE_MAX;
    if (send_ec_cmd(EC_CMD_FAN_MODE, &mode, 1) < 0) return -1;

    /* EC quirk: after mode cmd 0x98, follow-up 0x99 uses { fan_idx, value } order */
    uint8_t cpu[2] = { FAN_CPU, FAN_DUTY_AUTO };
    if (send_ec_cmd(EC_CMD_FAN_SPEED, cpu, 2) < 0) return -1;

    uint8_t gpu[2] = { FAN_GPU, FAN_DUTY_AUTO };
    return send_ec_cmd(EC_CMD_FAN_SPEED, gpu, 2);
}

static int fan_silent_all(void)
{
    uint8_t mode = FAN_MODE_SILENT;
    if (send_ec_cmd(EC_CMD_FAN_MODE, &mode, 1) < 0) return -1;

    /* EC quirk: after mode cmd 0x98, follow-up 0x99 uses { fan_idx, value } order */
    uint8_t cpu[2] = { FAN_CPU, FAN_MODE_SILENT };
    if (send_ec_cmd(EC_CMD_FAN_SPEED, cpu, 2) < 0) return -1;

    uint8_t gpu[2] = { FAN_GPU, FAN_MODE_SILENT };
    return send_ec_cmd(EC_CMD_FAN_SPEED, gpu, 2);
}

static int fan_set_duty(int fan_idx, int percent)
{
    if (percent < 21 || percent > 100) {
        fprintf(stderr, "Error: Fan duty %d%% out of range (21-100)\n", percent);
        return -1;
    }
    uint8_t raw = (uint8_t)((percent * 255) / 100);
    /* EC quirk: standalone 0x99 duty uses { fan_idx, raw } — same order as after a mode cmd */
    uint8_t data[2] = { (uint8_t)fan_idx, raw };
    printf("  Fan %s duty: %d%% (0x%02X)\n",
           fan_idx == FAN_CPU ? "CPU" : "GPU", percent, raw);
    return send_ec_cmd(EC_CMD_FAN_SPEED, data, 2);
}

/* ========================================================================
 * PROFILES
 * ========================================================================

 * Profiles from the Rust codebase (profiles.rs):
 *
 * max (perf_gpu):
 *   GPU=2(performance), turbo=ON, governor=performance, EPP=performance,
 *   display=auto, RAPL PL1=45W PL2=90W
 *   (RAPL values are setR-only; with plain 'set' the platform defaults run:
 *    PL1=90W, PL2=115W, GPU free to use its full 100W)
 *
 * cpuperf (perf_cpu):
 *   GPU=3(turbo), turbo=ON, governor=performance, EPP=performance,
 *   display=auto (no RAPL change)
 *
 * balanced:
 *   GPU=3(turbo), turbo=ON, governor=powersave, EPP=balance_performance,
 *   display=auto, RAPL PL1=35W PL2=40W
 *
 * powersave:
 *   GPU=1(quiet), turbo=OFF, governor=powersave, EPP=balance_power,
 *   display=auto, OEM defaults (PL1=15W, PL2=30W, GPU 70W)
 *
 * eco:
 *   GPU=0(silent), turbo=OFF, governor=powersave, EPP=power,
 *   display=auto, RAPL PL1=9W PL2=10W
 *
 * Fan behavior note:
 *   max, cpuperf, and balanced automatically switch fans to AUTO unless
 *   --nosafe is passed, preventing silent fan lock from causing thermal throttling.
 */

static int profile_max(int with_rapl)
{
    printf("Applying: Performance Max + GPU (80W-100W)\n");
    set_gpu_profile(2);
    set_turbo(1);
    set_governor("performance");
    set_epp("performance");
    if (with_rapl) set_rapl_limits(45, 90);
    return 0;
}

static int profile_cpuperf(int with_rapl)
{
    (void)with_rapl;
    printf("Applying: Performance CPU Only\n");
    set_gpu_profile(3);
    set_turbo(1);
    set_governor("performance");
    set_epp("performance");
    return 0;
}

static int profile_balanced(int with_rapl)
{
    printf("Applying: Balanced\n");
    set_gpu_profile(3);
    set_turbo(1);
    set_governor("powersave");
    set_epp("balance_performance");
    if (with_rapl) set_rapl_limits(35, 40);
    return 0;
}

static int profile_powersave(int with_rapl)
{
    (void)with_rapl;
    printf("Applying: Powersave\n");
    set_gpu_profile(1);
    set_turbo(0);
    set_governor("powersave");
    set_epp("balance_power");
    return 0;
}

static int profile_eco(int with_rapl)
{
    printf("Applying: Ultra Powersave\n");
    set_gpu_profile(0);
    set_turbo(0);
    set_governor("powersave");
    set_epp("power");
    if (with_rapl) set_rapl_limits(9, 10);
    return 0;
}

/* ========================================================================
 * TUXEDO IOCTL CONSTANTS
 * ======================================================================== */

#define R_CL_FANINFO1   0x8008ED10
#define R_CL_FANINFO2   0x8008ED11
#define W_CL_FANSPEED   0x4008EE10
#define W_CL_FANAUTO    0x4008EE11
#define R_CL_WEBCAM_SW  0x8008ED13
#define W_CL_WEBCAM_SW  0x4008EE12

static int webcam_read_tuxedo(void)
{
    int fd = tuxedo_open_clevo();
    if (fd < 0) return -1;

    int val = 0;
    int res = ioctl(fd, R_CL_WEBCAM_SW, &val);
    close(fd);
    if (res < 0) return -1;
    return val;
}

static int webcam_write_tuxedo(int enabled)
{
    int fd = tuxedo_open_clevo();
    if (fd < 0) return -1;

    int val = enabled ? 1 : 0;
    int res = ioctl(fd, W_CL_WEBCAM_SW, &val);
    close(fd);
    return (res < 0) ? -1 : 0;
}

/* Find USB device ID for the webcam (class 0x0E or 0xEF with camera keywords).
 * Returns the sysfs device name (e.g. "1-1.2") or NULL. Caller must free. */
static char *find_webcam_usb_id(void)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) return NULL;

    struct dirent *ent;
    char path[512];
    char *result = NULL;

    while ((ent = readdir(d)) != NULL) {
        /* Skip interfaces (contain ':') */
        if (strchr(ent->d_name, ':'))
            continue;

        /* Check bDeviceClass */
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/bDeviceClass",
                 ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char class_buf[16] = {0};
        ssize_t n = read(fd, class_buf, sizeof(class_buf) - 1);
        close(fd);
        if (n <= 0) continue;
        for (char *p = class_buf; *p; p++) if (*p == '\n' || *p == '\r') *p = '\0';

        int is_video = (strcmp(class_buf, "0e") == 0 || strcmp(class_buf, "14") == 0);
        int is_misc  = (strcmp(class_buf, "ef") == 0 || strcmp(class_buf, "239") == 0);
        if (!is_video && !is_misc) continue;

        /* Check product string for camera keywords */
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/product",
                 ent->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char prod_buf[256] = {0};
        n = read(fd, prod_buf, sizeof(prod_buf) - 1);
        close(fd);
        if (n > 0) {
            for (char *p = prod_buf; *p; p++) *p = (char)tolower((unsigned char)*p);
            if (strstr(prod_buf, "camera") || strstr(prod_buf, "webcam") ||
                strstr(prod_buf, "video") || strstr(prod_buf, "chicony")) {
                free(result);
                result = strdup(ent->d_name);
                break;
            }
        }

        /* If video class but no product match, still accept */
        if (is_video && !result) {
            result = strdup(ent->d_name);
        }
    }
    closedir(d);
    return result;
}

static int is_webcam_enabled(void)
{
    /* Try tuxedo_io first */
    int val = webcam_read_tuxedo();
    if (val >= 0)
        return val;

    /* Fallback: check USB driver binding */
    char *usb_id = find_webcam_usb_id();
    if (!usb_id) return -1;

    char path[256];
    snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/driver", usb_id);
    int enabled = (access(path, F_OK) == 0);
    free(usb_id);
    return enabled;
}

static int webcam_set(int enabled)
{
    /* Try tuxedo_io first */
    if (access("/dev/tuxedo_io", F_OK) == 0) {
        if (webcam_write_tuxedo(enabled) == 0) {
            printf("  Webcam: %s (tuxedo_io)\n", enabled ? "ON" : "OFF");
            return 0;
        }
    }

    /* Fallback: USB bind/unbind */
    char *usb_id = find_webcam_usb_id();
    if (!usb_id) {
        fprintf(stderr, "Error: No webcam USB device found\n");
        return -1;
    }

    char path[256];
    snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/driver", usb_id);
    int currently_enabled = (access(path, F_OK) == 0);

    if (enabled == currently_enabled) {
        printf("  Webcam: already %s\n", enabled ? "ON" : "OFF");
        free(usb_id);
        return 0;
    }

    if (enabled)
        snprintf(path, sizeof(path), "/sys/bus/usb/drivers/usb/bind");
    else
        snprintf(path, sizeof(path), "/sys/bus/usb/drivers/usb/unbind");

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to open %s: %s\n", path, strerror(errno));
        free(usb_id);
        return -1;
    }
    ssize_t n = write(fd, usb_id, strlen(usb_id));
    close(fd);
    free(usb_id);

    /* Unbind may return short write because the USB device vanishes mid-write.
       If the fd was opened successfully, assume the operation succeeded. */
    if (n < 0) {
        fprintf(stderr, "Error: Failed to %s webcam: %s\n",
                enabled ? "bind" : "unbind", strerror(errno));
        return -1;
    }

    printf("  Webcam: %s (USB)\n", enabled ? "ON" : "OFF");
    return 0;
}

static int webcam_toggle(void)
{
    int current = is_webcam_enabled();
    if (current < 0) {
        fprintf(stderr, "Error: Cannot detect webcam state\n");
        return -1;
    }
    return webcam_set(!current);
}

/* ========================================================================
 * MICROPHONE (amixer Capture switch)
 * ======================================================================== */

/* Find the ALSA HDA Intel PCH card number from /proc/asound/cards.
 * Returns the card number (>= 0) on success, or -1 if not found. */
static int mic_find_card(void)
{
    FILE *fp = fopen("/proc/asound/cards", "r");
    if (!fp) return -1;
    char line[256];
    int card = -1;
    while (fgets(line, sizeof(line), fp)) {
        int num;
        /* Lines look like: " 1 [PCH            ]: HDA-Intel - HDA Intel PCH" */
        if (sscanf(line, " %d", &num) == 1 && strstr(line, "HDA-Intel") && strstr(line, "PCH")) {
            card = num;
            break;
        }
    }
    fclose(fp);
    return card;
}

static int mic_is_enabled(void)
{
    int card = mic_find_card();
    char cmd[128];
    if (card >= 0)
        snprintf(cmd, sizeof(cmd), "amixer -c %d sget Capture 2>/dev/null", card);
    else
        snprintf(cmd, sizeof(cmd), "amixer sget Capture 2>/dev/null");
    FILE *fp = popen(cmd, "r");
    if (!fp) return 1; /* assume enabled if amixer fails */

    char line[256];
    int enabled = 1;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Front Left:")) {
            if (strstr(line, "[on]")) enabled = 1;
            else if (strstr(line, "[off]")) enabled = 0;
            break;
        }
    }
    pclose(fp);
    return enabled;
}

static int mic_set(int enabled)
{
    const char *verb = enabled ? "cap" : "nocap";
    int card = mic_find_card();
    char cmd[256];

    /* Toggle master capture switch — this gates all mic inputs (internal +
     * headphone) regardless of which source the HDA codec mux has selected. */
    if (card >= 0)
        snprintf(cmd, sizeof(cmd), "amixer -c %d sset Capture %s >/dev/null 2>&1", card, verb);
    else
        snprintf(cmd, sizeof(cmd), "amixer sset Capture %s >/dev/null 2>&1", verb);
    if (system(cmd) != 0) {
        fprintf(stderr, "Error: amixer failed (is alsa installed?)\n");
        return -1;
    }
    printf("  Laptop internal microphone: %s\n", enabled ? "ON" : "OFF");
    return 0;
}

static int mic_toggle(void)
{
    return mic_set(!mic_is_enabled());
}

/* ========================================================================
 * REFRESH RATE (xrandr)
 * ======================================================================== */

/* Detect the first connected output name (e.g. "eDP-1"). Returns static buffer. */
struct display_info {
    char output[64];
    char resolution[64];
    char current_rate[16];
    char available_rates[32][16];
    int rate_count;
};

static int query_display_info(struct display_info *info)
{
    memset(info, 0, sizeof(*info));
    strcpy(info->output, "eDP-1");
    strcpy(info->resolution, "2560x1440");
    strcpy(info->current_rate, "60.00");

    FILE *fp = popen("xrandr --current 2>/dev/null", "r");
    if (!fp) return -1;

    char line[512];
    int found_output = 0;
    while (fgets(line, sizeof(line), fp)) {
        // Strip trailing newlines
        for (char *p = line; *p; p++) if (*p == '\n' || *p == '\r') *p = '\0';

        // Check for output line: "<name> connected ..."
        if (!found_output && strstr(line, " connected")) {
            char name[64];
            if (sscanf(line, "%63s connected", name) == 1) {
                strcpy(info->output, name);
                found_output = 1;
            }
        }

        // If we found the output, look for resolution lines starting with spaces
        if (found_output && line[0] == ' ' && line[1] == ' ' && line[2] == ' ') {
            char res[64];
            // Read the resolution name
            char *p = line;
            while (*p == ' ') p++;
            if (sscanf(p, "%63s", res) != 1) continue;

            // Parse rates
            p += strlen(res);
            int rate_idx = 0;
            int is_active_res = 0;
            char rates_temp[32][16];

            while (*p && rate_idx < 32) {
                // skip spaces
                while (*p == ' ') p++;
                if (!*p) break;

                // read rate token (till space)
                char rate_tok[32];
                int len = 0;
                while (*p && *p != ' ' && len < 31) {
                    rate_tok[len++] = *p++;
                }
                rate_tok[len] = '\0';

                // Check if this rate is active (contains '*')
                int active = (strchr(rate_tok, '*') != NULL);

                // Clean the rate token (only keep digits and dots)
                char clean_rate[16];
                int c_len = 0;
                for (int i = 0; i < len && c_len < 15; i++) {
                    if (isdigit((unsigned char)rate_tok[i]) || rate_tok[i] == '.') {
                        clean_rate[c_len++] = rate_tok[i];
                    }
                }
                clean_rate[c_len] = '\0';

                if (c_len > 0) {
                    strcpy(rates_temp[rate_idx], clean_rate);
                    if (active) {
                        is_active_res = 1;
                        strcpy(info->current_rate, clean_rate);
                    }
                    rate_idx++;
                }
            }

            // If this is the active resolution, copy everything to display_info
            if (is_active_res) {
                strcpy(info->resolution, res);
                info->rate_count = rate_idx;
                for (int i = 0; i < rate_idx; i++) {
                    strcpy(info->available_rates[i], rates_temp[i]);
                }
                // Once we found the active resolution, we can stop
                break;
            }
        }
    }
    pclose(fp);
    return found_output ? 0 : -1;
}

static int command_exists(const char *cmd)
{
    if (strchr(cmd, '/'))
        return access(cmd, X_OK) == 0;

    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin:/usr/local/bin";

    char pbuf[PATH_MAX];
    strncpy(pbuf, path, sizeof(pbuf) - 1);
    pbuf[sizeof(pbuf) - 1] = '\0';

    char *dir = strtok(pbuf, ":");
    while (dir) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, cmd);
        if (access(full, X_OK) == 0)
            return 1;
        dir = strtok(NULL, ":");
    }
    return 0;
}

/* Fast check: DISPLAY is set and xrandr binary exists in PATH */
static int has_display_support(void)
{
    const char *disp = getenv("DISPLAY");
    if (!disp || !*disp) return 0;
    return command_exists("xrandr");
}

/* Returns 1 if running in an X11 session with xrandr working and an active rate detected. */
static int is_x11_xrandr_available(struct display_info *out_info)
{
    if (!has_display_support()) return 0;

    struct display_info info;
    if (query_display_info(&info) < 0) return 0;
    if (info.current_rate[0] == '\0') return 0;

    if (out_info) *out_info = info;
    return 1;
}

static int rr_set(const char *rate)
{
    /* Validate rate: must be digits and at most one dot */
    for (const char *p = rate; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '.') {
            fprintf(stderr, "Error: Invalid refresh rate '%s'\n", rate);
            return -1;
        }
    }

    struct display_info info;
    if (query_display_info(&info) < 0) {
        fprintf(stderr, "Error: failed to query display info\n");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* child */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp("xrandr", "xrandr", "--output", info.output, "--mode", info.resolution,
               "--rate", rate, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Error: Failed to set refresh rate to %sHz\n", rate);
        return -1;
    }
    printf("  Refresh rate: %sHz\n", rate);
    return 0;
}

static int rr_list(void)
{
    struct display_info info;
    if (query_display_info(&info) < 0) {
        fprintf(stderr, "Error: xrandr not available or failed to parse\n");
        return -1;
    }
    printf("  Display: %s (%s)\n", info.output, info.resolution);
    printf("  Current: %sHz\n", info.current_rate);
    printf("  Available:\n");
    for (int i = 0; i < info.rate_count; i++) {
        printf("    %sHz%s\n", info.available_rates[i],
               (strcmp(info.available_rates[i], info.current_rate) == 0) ? "  (current)" : "");
    }
    return 0;
}

/* Helper to check if a CPU core is an E-core based on its max frequency relative to global max.
 * Cache the global max frequency on first call. */
static int is_cpu_e_core(int cpu_num)
{
    static int global_max_khz = -1;
    if (global_max_khz < 0) {
        global_max_khz = 0;
        DIR *d = opendir("/sys/devices/system/cpu");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strncmp(ent->d_name, "cpu", 3) != 0) continue;
                if (ent->d_name[3] < '0' || ent->d_name[3] > '9') continue;
                char path[512];
                snprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/%s/cpufreq/cpuinfo_max_freq", ent->d_name);
                long khz = read_sysfs_long(path, -1);
                if (khz > global_max_khz) {
                    global_max_khz = (int)khz;
                }
            }
            closedir(d);
        }
        if (global_max_khz <= 0) {
            global_max_khz = 4000000; // fallback default
        }
    }

    char path[256];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu_num);
    long khz = read_sysfs_long(path, -1);
    if (khz <= 0) return 0; // default to P-core (0) if can't read

    /* If max freq is less than 85% of global max, classify as E-core */
    return (khz < (long)(global_max_khz * 0.85)) ? 1 : 0;
}

/* ========================================================================
 * STATUS
 * ======================================================================== */

static void show_status(void)
{
    char buf[128] = {0};

    printf("%s=== System Status ===%s\n\n", C_YLW, C_RST);

    /* Turbo */
    if (read_sysfs_str(TURBO_PATH, buf, sizeof(buf)) >= 0) {
        int val = atoi(buf);
        printf("  %-14s %s%s%s\n", "Turbo:", val == 0 ? C_GRN : C_RED, val == 0 ? "ON" : "OFF", C_RST);
    } else {
        printf("  %-14s %sN/A (intel_pstate not loaded)%s\n", "Turbo:", C_DIM, C_RST);
    }

    /* Governor (read cpu0) */
    if (read_sysfs_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", buf, sizeof(buf)) >= 0) {
        printf("  %-14s %s%s%s\n", "Governor:", C_CYN, buf, C_RST);
    } else {
        printf("  %-14s %sN/A%s\n", "Governor:", C_DIM, C_RST);
    }

    /* EPP (read cpu0) */
    if (read_sysfs_str("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference", buf, sizeof(buf)) >= 0) {
        printf("  %-14s %s%s%s\n", "EPP:", C_CYN, buf, C_RST);
    } else {
        printf("  %-14s %sN/A%s\n", "EPP:", C_DIM, C_RST);
    }

    /* RAPL PL1 */
    long pl1_uw = read_sysfs_long("/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw", -1);
    if (pl1_uw >= 0) {
        printf("  %-14s %s%ldW%s\n", "RAPL PL1:", C_CYN, pl1_uw / 1000000, C_RST);
    } else {
        printf("  %-14s %sN/A%s\n", "RAPL PL1:", C_DIM, C_RST);
    }

    /* RAPL PL2 */
    long pl2_uw = read_sysfs_long("/sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw", -1);
    if (pl2_uw >= 0) {
        printf("  %-14s %s%ldW%s\n", "RAPL PL2:", C_CYN, pl2_uw / 1000000, C_RST);
    } else {
        printf("  %-14s %sN/A%s\n", "RAPL PL2:", C_DIM, C_RST);
    }

    /* Webcam */
    int cam = is_webcam_enabled();
    if (cam < 0) {
        printf("  %-14s %sNot detected%s\n", "Webcam:", C_DIM, C_RST);
    } else {
        printf("  %-14s %s%s%s\n", "Webcam:", cam ? C_GRN : C_RED, cam ? "ON" : "OFF", C_RST);
    }

    /* Microphone */
    int mic = mic_is_enabled();
    printf("  %-14s %s%s%s\n", "Microphone:", mic ? C_GRN : C_RED, mic ? "ON" : "OFF", C_RST);

    /* Fn Lock */
    if (read_sysfs_str(FNLOCK_PATH, buf, sizeof(buf)) >= 0) {
        int val = atoi(buf);
        printf("  %-14s %s%s%s\n", "Fn Lock:", val ? C_GRN : C_RED, val ? "ON" : "OFF", C_RST);
    } else {
        printf("  %-14s %sN/A (tuxedo_keyboard not loaded)%s\n", "Fn Lock:", C_DIM, C_RST);
    }

    /* Battery */
    {
        char bat_status[32] = {0};
        read_sysfs_str("/sys/class/power_supply/BAT0/status", bat_status, sizeof(bat_status));
        long cap      = read_sysfs_long("/sys/class/power_supply/BAT0/capacity", -1);
        long full     = read_sysfs_long("/sys/class/power_supply/BAT0/charge_full", -1);
        long full_dsn = read_sysfs_long("/sys/class/power_supply/BAT0/charge_full_design", -1);
        long now      = read_sysfs_long("/sys/class/power_supply/BAT0/charge_now", -1);
        long current  = read_sysfs_long("/sys/class/power_supply/BAT0/current_now", -1);
        long cycles   = read_sysfs_long("/sys/class/power_supply/BAT0/cycle_count", -1);
        long volt     = read_sysfs_long("/sys/class/power_supply/BAT0/voltage_now", -1);
        int bat_start = bat_read_start();
        int bat_end   = bat_read_end();

        printf("  %-14s %ld%% %s", "Battery:", cap, bat_status);
        if (bat_start > 0 && bat_end > 0)
            printf("  [threshold: %d%%→%d%%]", bat_start, bat_end);
        printf("\n");

        if (full > 0 && full_dsn > 0) {
            int health = (int)((full * 100L) / full_dsn);
            const char *hcol = health > 100 ? C_GRN : (health < 80 ? C_RED : C_YLW);
            printf("  %-14s %s%d%%%s (%ld / %ld mAh)\n", "Health:", hcol, health, C_RST,
                   full / 1000, full_dsn / 1000);
        }
        if (cycles > 0)
            printf("  %-14s %s%ld%s\n", "Cycles:", C_CYN, cycles, C_RST);
        if (now > 0)
            printf("  %-14s %ld mAh / %ld mAh\n", "Charge:", now / 1000, full / 1000);
        if (current != 0) {
            long ma = current / 1000; /* microamps → milliamps */
            printf("  %-14s %s%+ld mA%s\n", "Rate:", current > 0 ? C_RED : C_GRN, ma, C_RST);
        }
        if (volt > 0)
            printf("  %-14s %ld mV\n", "Voltage:", volt / 1000);
    }

    /* Nvidia GPU */
#ifdef CCTL_NVIDIA
    int nv_blacklisted = nvidia_is_blacklisted();
    int nv_loaded = nvidia_is_loaded();
    printf("  %-14s %s%s%s (modules %s%s%s)\n",
           "Nvidia GPU:",
           nv_blacklisted ? C_RED : C_GRN, nv_blacklisted ? "BLACKLISTED" : "ENABLED", C_RST,
           nv_loaded ? C_GRN : C_DIM, nv_loaded ? "LOADED" : "NOT LOADED", C_RST);
#endif

    /* GPU MUX */
    mux_show();

    /* Refresh Rate (only shown if xrandr is present) */
    struct display_info disp_info;
    if (is_x11_xrandr_available(&disp_info)) {
        printf("  %-14s %s%s Hz%s\n", "Refresh Rate:", C_CYN, disp_info.current_rate, C_RST);
    }

    /* CPU Max Frequency (P-core vs E-core) */
    printf("\n%s--- CPU Max Frequency ---%s\n", C_YLW, C_RST);
    int p_max = 0, e_max = 0;
    DIR *d = opendir("/sys/devices/system/cpu");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strncmp(ent->d_name, "cpu", 3) != 0) continue;
            if (ent->d_name[3] < '0' || ent->d_name[3] > '9') continue;
            int cpu_num = atoi(ent->d_name + 3);
            char path[512];
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/%s/cpufreq/cpuinfo_max_freq", ent->d_name);
            long khz = read_sysfs_long(path, -1);
            if (khz <= 0) continue;
            int mhz = (int)(khz / 1000);
            if (!is_cpu_e_core(cpu_num)) {
                if (mhz > p_max) p_max = mhz;
            } else {
                if (mhz > e_max) e_max = mhz;
            }
        }
        closedir(d);
    }
    if (p_max > 0)
        printf("  %-14s %s%d MHz%s\n", "P-Core:", C_CYN, p_max, C_RST);
    else
        printf("  %-14s %sN/A%s\n", "P-Core:", C_DIM, C_RST);
    if (e_max > 0)
        printf("  %-14s %s%d MHz%s\n", "E-Core:", C_CYN, e_max, C_RST);
    else
        printf("  %-14s %sN/A%s\n", "E-Core:", C_DIM, C_RST);

    /* Fan Telemetry */
    int cpu_pct = 0, gpu_pct = 0, cpu_rpm = 0, gpu_rpm = 0;
    if (read_fan_telemetry(&cpu_pct, &gpu_pct, &cpu_rpm, &gpu_rpm) == 0) {
        printf("\n%s--- Fan Telemetry ---%s\n", C_YLW, C_RST);
        printf("  %-14s %s%3d%%%s duty, %s%4d RPM%s\n", "CPU Fan:", C_CYN, cpu_pct, C_RST, C_CYN, cpu_rpm, C_RST);
        if (gpu_pct == 0 && gpu_rpm == 0) {
            printf("  %-14s %s%3d%%%s duty, %s%4d RPM%s  %s(GPU in D3cold state)%s\n",
                   "GPU Fan:", C_DIM, gpu_pct, C_RST, C_DIM, gpu_rpm, C_RST, C_DIM, C_RST);
        } else {
            printf("  %-14s %s%3d%%%s duty, %s%4d RPM%s\n",
                   "GPU Fan:", C_CYN, gpu_pct, C_RST, C_CYN, gpu_rpm, C_RST);
        }
    } else {
        printf("\n%s--- Fan Telemetry ---%s\n", C_YLW, C_RST);
        printf("  %-14s %sN/A (ec_sys or tuxedo_io not available)%s\n", "Fans:", C_DIM, C_RST);
    }

    printf("\n");
}

/* ========================================================================
 * BATTERY CHARGE THRESHOLDS
 * ======================================================================== */

#define BAT_START_PATH "/sys/class/power_supply/BAT0/charge_control_start_threshold"
#define BAT_END_PATH   "/sys/class/power_supply/BAT0/charge_control_end_threshold"
#define BAT_START_AVAIL_PATH "/sys/class/power_supply/BAT0/charge_control_start_available_thresholds"
#define BAT_END_AVAIL_PATH   "/sys/class/power_supply/BAT0/charge_control_end_available_thresholds"

/* Read available threshold values into an array. Returns count or -1. */
static int read_avail_thresholds(const char *path, int *out, int max)
{
    char buf[128];
    if (read_sysfs_str(path, buf, sizeof(buf)) < 0)
        return -1;
    int count = 0;
    char *p = buf;
    while (*p && count < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        out[count++] = (int)v;
        p = end;
    }
    return count;
}

/* Check if a value is in the available threshold list. */
static int is_valid_threshold(const int *avail, int count, int val)
{
    for (int i = 0; i < count; i++)
        if (avail[i] == val) return 1;
    return 0;
}

/* Print available threshold values to stderr. */
static void print_avail_thresholds(const char *label, const int *avail, int count)
{
    fprintf(stderr, "  %s available: ", label);
    for (int i = 0; i < count; i++)
        fprintf(stderr, "%d%c", avail[i], i < count - 1 ? ' ' : '\n');
}

/* Read current charge control start threshold. Returns value or -1. */
static int bat_read_start(void)
{
    long v = read_sysfs_long(BAT_START_PATH, -1);
    return (v >= 0) ? (int)v : -1;
}

/* Read current charge control end threshold. Returns value or -1. */
static int bat_read_end(void)
{
    long v = read_sysfs_long(BAT_END_PATH, -1);
    return (v >= 0) ? (int)v : -1;
}

/* Set battery charge thresholds. "off" (the start==end==0 sentinel) selects
 * a near-full top-up range: charge all the way to the max stop threshold,
 * but only resume charging once the battery drops below the highest usable
 * start threshold (e.g. 95/100). Avoids keeping cells at mid-charge without
 * deep-discharge cycling. */
static int bat_set(int start, int end)
{
    /* "off" → stop=max, start=highest listed value below stop */
    if (start == 0 && end == 0) {
        int avail[16], cnt;
        cnt = read_avail_thresholds(BAT_END_AVAIL_PATH, avail, 16);
        end = (cnt > 0) ? avail[cnt - 1] : 100;
        cnt = read_avail_thresholds(BAT_START_AVAIL_PATH, avail, 16);
        start = 95;
        for (int i = cnt - 1; i >= 0; i--) {
            if (avail[i] < end) { start = avail[i]; break; }
        }
    }

    /* Validate start threshold */
    int start_avail[16], start_count;
    start_count = read_avail_thresholds(BAT_START_AVAIL_PATH, start_avail, 16);
    if (start_count > 0 && !is_valid_threshold(start_avail, start_count, start)) {
        fprintf(stderr, "Error: Start threshold %d%% is not valid\n", start);
        print_avail_thresholds("Start", start_avail, start_count);
        return -1;
    }

    /* Validate end threshold */
    int end_avail[16], end_count;
    end_count = read_avail_thresholds(BAT_END_AVAIL_PATH, end_avail, 16);
    if (end_count > 0 && !is_valid_threshold(end_avail, end_count, end)) {
        fprintf(stderr, "Error: End threshold %d%% is not valid\n", end);
        print_avail_thresholds("End", end_avail, end_count);
        return -1;
    }

    if (start >= end) {
        fprintf(stderr, "Error: Start threshold (%d%%) must be less than end threshold (%d%%)\n", start, end);
        return -1;
    }

    char val[12];
    snprintf(val, sizeof(val), "%d", start);
    if (write_sysfs(BAT_START_PATH, val) < 0) {
        fprintf(stderr, "Error: Failed to set start threshold to %d%%\n", start);
        return -1;
    }

    snprintf(val, sizeof(val), "%d", end);
    if (write_sysfs(BAT_END_PATH, val) < 0) {
        fprintf(stderr, "Error: Failed to set end threshold to %d%%\n", end);
        return -1;
    }

    printf("  Charge thresholds: start %d%% → stop %d%%\n", start, end);
    return 0;
}

/* ========================================================================
 * KEYBOARD BACKLIGHT
 * ======================================================================== */

#define KBD_PATH "/sys/class/leds/rgb:kbd_backlight"

static int kbd_get_brightness(void)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/brightness", KBD_PATH);
    long raw = read_sysfs_long(path, -1);
    if (raw < 0) return -1;
    return (int)((raw * 100 + 127) / 255);
}

static int kbd_set_color(int r, int g, int b)
{
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        fprintf(stderr, "Error: RGB values must be 0-255\n");
        return -1;
    }
    char val[64];
    snprintf(val, sizeof(val), "%d %d %d", r, g, b);

    char path[128];
    snprintf(path, sizeof(path), "%s/multi_intensity", KBD_PATH);

    if (write_sysfs(path, val) < 0) {
        fprintf(stderr, "Error: Failed to set keyboard color (is tuxedo_keyboard loaded?)\n");
        return -1;
    }
    int bri = kbd_get_brightness();
    if (bri >= 0)
        printf("  Keyboard color: RGB(%d, %d, %d)  [brightness: %d%%]\n", r, g, b, bri);
    else
        printf("  Keyboard color: RGB(%d, %d, %d)\n", r, g, b);
    return 0;
}

static int kbd_set_brightness(int percent)
{
    if (percent < 0 || percent > 100) {
        fprintf(stderr, "Error: Brightness must be 0-100%%\n");
        return -1;
    }
    int raw = (percent * 255) / 100;
    char val[8];
    snprintf(val, sizeof(val), "%d", raw);

    char path[128];
    snprintf(path, sizeof(path), "%s/brightness", KBD_PATH);

    if (write_sysfs(path, val) < 0) {
        fprintf(stderr, "Error: Failed to set keyboard brightness (is tuxedo_keyboard loaded?)\n");
        return -1;
    }
    printf("  Keyboard brightness: %d%%\n", percent);
    return 0;
}

/* ========================================================================
 * KEYBOARD COLOR PRESETS
 * ======================================================================== */

struct kbd_preset {
    const char *name;
    const char *hex;
};

static const struct kbd_preset kbd_presets[] = {
    { "blue",       "0000ff" },
    { "chocolate",  "d2691e" },
    { "coral",      "ff7f50" },
    { "cyan",       "00ffff" },
    { "gold",       "ffd700" },
    { "gray",       "808080" },
    { "green",      "00c800" },
    { "indigo",     "4b0082" },
    { "lime",       "00ff00" },
    { "magenta",    "ff00ff" },
    { "maroon",     "800000" },
    { "navy",       "000080" },
    { "olive",      "808000" },
    { "orange",     "ff8800" },
    { "pink",       "ff1493" },
    { "purple",     "8800ff" },
    { "red",        "ff0000" },
    { "salmon",     "fa8072" },
    { "silver",     "c0c0c0" },
    { "teal",       "008080" },
    { "turquoise",  "40e0d0" },
    { "violet",     "ee82ee" },
    { "white",      "ffffff" },
    { "yellow",     "ffff00" },
    { "off",        "000000" },
    { NULL, NULL }
};

static int hex_to_rgb(const char *hex, int *r, int *g, int *b)
{
    if (strlen(hex) != 6) return -1;
    char buf[3] = {0};
    buf[0] = hex[0]; buf[1] = hex[1];
    *r = (int)strtol(buf, NULL, 16);
    buf[0] = hex[2]; buf[1] = hex[3];
    *g = (int)strtol(buf, NULL, 16);
    buf[0] = hex[4]; buf[1] = hex[5];
    *b = (int)strtol(buf, NULL, 16);
    return 0;
}

static int kbd_set_preset(const char *name)
{
    // Check if it is direct hex format: "#RRGGBB" or "RRGGBB"
    const char *hex_ptr = NULL;
    if (name[0] == '#' && strlen(name) == 7) {
        hex_ptr = name + 1;
    } else if (strlen(name) == 6) {
        int is_hex = 1;
        for (int j = 0; j < 6; j++) {
            if (!isxdigit((unsigned char)name[j])) {
                is_hex = 0;
                break;
            }
        }
        if (is_hex) hex_ptr = name;
    }

    if (hex_ptr) {
        int r, g, b;
        if (hex_to_rgb(hex_ptr, &r, &g, &b) == 0) {
            return kbd_set_color(r, g, b);
        }
    }

    /* Case-insensitive lookup */
    char lower[64];
    size_t i;
    for (i = 0; i < sizeof(lower) - 1 && name[i]; i++)
        lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = '\0';

    for (const struct kbd_preset *p = kbd_presets; p->name; p++) {
        if (strcmp(p->name, lower) == 0) {
            int r, g, b;
            if (hex_to_rgb(p->hex, &r, &g, &b) < 0) return -1;
            return kbd_set_color(r, g, b);
        }
    }

    fprintf(stderr, "Error: Unknown preset '%s'\n", name);
    fprintf(stderr, "Available presets:\n");
    for (const struct kbd_preset *p = kbd_presets; p->name; p++)
        fprintf(stderr, "  %s (#%s)\n", p->name, p->hex);
    return -1;
}

static void kbd_show_presets(void)
{
    printf("Usage: cctl kbc <R G B | #hex | preset>\n");
    printf("  Examples: cctl kbc 255 0 128  |  cctl kbc #ff0080  |  cctl kbc cyan\n\n");
    printf("Available Presets:\n");
    int col = 0;
    for (const struct kbd_preset *p = kbd_presets; p->name; p++) {
        printf("  %-11s (#%s)", p->name, p->hex);
        col++;
        if (col % 3 == 0) printf("\n");
    }
    if (col % 3 != 0) printf("\n");
}

/* ========================================================================
 * KEYBOARD EFFECTS (kbe)
 * ======================================================================== */

#define KBE_LOCK_PATH   "/run/cctl_kbe.lock"
#define KBE_STATE_PATH  "/run/cctl_kbe.state"

static volatile sig_atomic_t g_kbe_running = 1;

static void kbe_sig_handler(int sig)
{
    (void)sig;
    g_kbe_running = 0;
}

static const uint8_t kbe_breathe_lut[128] = {
      8,   8,   8,   8,   8,   8,   8,   8,   8,   9,   9,   9,  10,  10,  11,  12,
     13,  15,  16,  18,  20,  23,  25,  28,  32,  35,  39,  43,  48,  53,  58,  64,
     70,  76,  82,  89,  96, 103, 111, 118, 126, 134, 142, 150, 157, 165, 173, 181,
    188, 195, 202, 209, 215, 221, 227, 232, 237, 241, 244, 248, 250, 252, 254, 255,
    255, 255, 254, 252, 250, 248, 244, 241, 237, 232, 227, 221, 215, 209, 202, 195,
    188, 181, 173, 165, 157, 150, 142, 134, 126, 118, 111, 103,  96,  89,  82,  76,
     70,  64,  58,  53,  48,  43,  39,  35,  32,  28,  25,  23,  20,  18,  16,  15,
     13,  12,  11,  10,  10,   9,   9,   9,   8,   8,   8,   8,   8,   8,   8,   8
};

static void kbe_sleep_ms(int ms)
{
    while (ms > 0 && g_kbe_running) {
        int chunk = ms > 25 ? 25 : ms;
        usleep((useconds_t)chunk * 1000);
        ms -= chunk;
    }
}

static void kbe_hue_to_rgb(int hue, int brightness, int *r, int *g, int *b)
{
    hue = ((hue % 360) + 360) % 360;
    int sector = hue / 60;
    int rem = hue % 60;
    int inc = (rem * 255) / 60;
    int dec = 255 - inc;
    int r1 = 0, g1 = 0, b1 = 0;

    switch (sector) {
        case 0: r1 = 255; g1 = inc; b1 = 0;   break;
        case 1: r1 = dec; g1 = 255; b1 = 0;   break;
        case 2: r1 = 0;   g1 = 255; b1 = inc; break;
        case 3: r1 = 0;   g1 = dec; b1 = 255; break;
        case 4: r1 = inc; g1 = 0;   b1 = 255; break;
        default: r1 = 255; g1 = 0;  b1 = dec; break;
    }

    if (brightness < 255) {
        if (brightness < 0) brightness = 0;
        r1 = (r1 * brightness) / 255;
        g1 = (g1 * brightness) / 255;
        b1 = (b1 * brightness) / 255;
    }

    *r = r1;
    *g = g1;
    *b = b1;
}

static inline void kbe_write_frame(int fd_col, int r, int g, int b)
{
    if (fd_col < 0) return;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d %d %d", r, g, b);
    if (len > 0) {
        lseek(fd_col, 0, SEEK_SET);
        ssize_t w = write(fd_col, buf, (size_t)len);
        (void)w;
    }
}

static inline void kbe_write_bri(int fd_bri, int bri)
{
    if (fd_bri < 0) return;
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", bri);
    if (len > 0) {
        lseek(fd_bri, 0, SEEK_SET);
        ssize_t w = write(fd_bri, buf, (size_t)len);
        (void)w;
    }
}

static int kbd_get_color(int *r, int *g, int *b)
{
    char buf[64];
    if (read_sysfs_str(KBD_PATH "/multi_intensity", buf, sizeof(buf)) < 0)
        return -1;
    if (sscanf(buf, "%d %d %d", r, g, b) != 3)
        return -1;
    return 0;
}

static int kbd_get_raw_brightness(int *bri)
{
    long val = read_sysfs_long(KBD_PATH "/brightness", -1);
    if (val < 0) return -1;
    *bri = (int)val;
    return 0;
}

static int kbe_read_state(pid_t *pid, char *effect, size_t effect_sz, int *orig_r, int *orig_g, int *orig_b, int *orig_bri)
{
    FILE *fp = fopen(KBE_STATE_PATH, "r");
    if (!fp) return -1;
    char line[128];
    long p = -1;
    if (!fgets(line, sizeof(line), fp) || sscanf(line, "%ld", &p) != 1 || p <= 1) {
        fclose(fp);
        return -1;
    }
    if (pid) *pid = (pid_t)p;
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    for (char *c = line; *c; c++) {
        if (*c == '\n' || *c == '\r') *c = '\0';
    }
    if (effect && effect_sz > 0) {
        strncpy(effect, line, effect_sz - 1);
        effect[effect_sz - 1] = '\0';
    }
    if (fgets(line, sizeof(line), fp)) {
        int r = 255, g = 255, b = 255, bri = 255;
        if (sscanf(line, "%d %d %d %d", &r, &g, &b, &bri) >= 3) {
            if (orig_r) *orig_r = r;
            if (orig_g) *orig_g = g;
            if (orig_b) *orig_b = b;
            if (orig_bri) *orig_bri = bri;
        }
    }
    fclose(fp);
    return 0;
}

static int kbe_is_running(pid_t *pid, char *effect, size_t effect_sz, int *orig_r, int *orig_g, int *orig_b, int *orig_bri)
{
    pid_t p = 0;
    if (kbe_read_state(&p, effect, effect_sz, orig_r, orig_g, orig_b, orig_bri) < 0)
        return 0;
    if (kill(p, 0) == 0 || errno == EPERM) {
        if (pid) *pid = p;
        return 1;
    }
    if (errno == ESRCH && geteuid() == 0) {
        unlink(KBE_STATE_PATH);
    }
    return 0;
}

static int kbe_stop(int quiet)
{
    pid_t pid = 0;
    char effect[32] = {0};
    int orig_r = 255, orig_g = 255, orig_b = 255, orig_bri = 255;

    if (!kbe_is_running(&pid, effect, sizeof(effect), &orig_r, &orig_g, &orig_b, &orig_bri)) {
        if (!quiet)
            printf("No active keyboard effect is currently running.\n");
        return 0;
    }

    kill(pid, SIGTERM);

    int exited = 0;
    for (int i = 0; i < 30; i++) {
        usleep(20000);
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            exited = 1;
            break;
        }
    }

    if (!exited) {
        kill(pid, SIGKILL);
        usleep(20000);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%d %d %d", orig_r, orig_g, orig_b);
    write_sysfs(KBD_PATH "/multi_intensity", buf);
    snprintf(buf, sizeof(buf), "%d", orig_bri);
    write_sysfs(KBD_PATH "/brightness", buf);

    unlink(KBE_STATE_PATH);
    unlink(KBE_LOCK_PATH);

    if (!quiet)
        printf("Stopped keyboard effect '%s' [PID %d] and restored original state.\n", effect, (int)pid);

    return 0;
}

static int kbe_candle_step(int *flicker_val)
{
    int target = 160 + (rand() % 95);
    if ((rand() % 15) == 0)
        target = 90;
    *flicker_val = (*flicker_val * 6 + target * 4) / 10;
    return *flicker_val;
}

static void kbe_daemon_worker(const char *effect, int orig_r, int orig_g, int orig_b, int orig_bri)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = kbe_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int lock_fd = open(KBE_LOCK_PATH, O_RDWR | O_CREAT, 0644);
    if (lock_fd < 0) exit(1);
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        close(lock_fd);
        exit(1);
    }

    FILE *fp = fopen(KBE_STATE_PATH, "w");
    if (!fp) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        exit(1);
    }
    fprintf(fp, "%d\n%s\n%d %d %d %d\n", (int)getpid(), effect, orig_r, orig_g, orig_b, orig_bri);
    fclose(fp);
    chmod(KBE_STATE_PATH, 0644);

    int fd_col = open(KBD_PATH "/multi_intensity", O_WRONLY);
    int fd_bri = open(KBD_PATH "/brightness", O_WRONLY);

    if (orig_bri <= 0) {
        kbe_write_bri(fd_bri, 255);
    }

    int base_r = orig_r;
    int base_g = orig_g;
    int base_b = orig_b;
    if (base_r == 0 && base_g == 0 && base_b == 0) {
        base_r = 0;
        base_g = 255;
        base_b = 255;
    }

    int mode = 0;
    if (strcmp(effect, "breathe") == 0 || strcmp(effect, "breath") == 0) {
        mode = 1;
    } else if (strcmp(effect, "breathe-cycle") == 0 || strcmp(effect, "breathe+colorchange") == 0 ||
               strcmp(effect, "breathe_cycle") == 0 || strcmp(effect, "breathecycle") == 0) {
        mode = 2;
    } else if (strcmp(effect, "cycle") == 0 || strcmp(effect, "rainbow") == 0 ||
               strcmp(effect, "spectrum") == 0 || strcmp(effect, "slow-cycle") == 0 ||
               strcmp(effect, "slow_colorchanging") == 0) {
        mode = 3;
    } else if (strcmp(effect, "flash") == 0 || strcmp(effect, "strobe") == 0) {
        mode = 4;
    } else if (strcmp(effect, "flash-cycle") == 0 || strcmp(effect, "flash+colorchange") == 0 ||
               strcmp(effect, "flash_cycle") == 0 || strcmp(effect, "flashcycle") == 0) {
        mode = 5;
    } else if (strcmp(effect, "candle") == 0 || strcmp(effect, "flicker") == 0) {
        mode = 6;
        if (orig_r == 255 && orig_g == 255 && orig_b == 255) {
            base_r = 255;
            base_g = 140;
            base_b = 20;
        }
    } else if (strcmp(effect, "pulse") == 0 || strcmp(effect, "heartbeat") == 0) {
        mode = 7;
    }

    int step = 0;
    int flash_hue = 0;
    int candle_val = 200;

    srand((unsigned int)(time(NULL) ^ getpid()));

    while (g_kbe_running) {
        switch (mode) {
            case 1: {
                int bri = (int)kbe_breathe_lut[step % 128];
                int r = (base_r * bri) / 255;
                int g = (base_g * bri) / 255;
                int b = (base_b * bri) / 255;
                kbe_write_frame(fd_col, r, g, b);
                kbe_sleep_ms(25);
                step++;
                break;
            }
            case 2: {
                int bri = (int)kbe_breathe_lut[step % 128];
                int hue = (step / 2) % 360;
                int r, g, b;
                kbe_hue_to_rgb(hue, bri, &r, &g, &b);
                kbe_write_frame(fd_col, r, g, b);
                kbe_sleep_ms(25);
                step++;
                break;
            }
            case 3: {
                int hue = step % 360;
                int r, g, b;
                kbe_hue_to_rgb(hue, 255, &r, &g, &b);
                kbe_write_frame(fd_col, r, g, b);
                kbe_sleep_ms(25);
                step++;
                break;
            }
            case 4: {
                for (int i = 0; i < 3 && g_kbe_running; i++) {
                    kbe_write_frame(fd_col, base_r, base_g, base_b);
                    kbe_sleep_ms(70);
                    if (!g_kbe_running) break;
                    kbe_write_frame(fd_col, 0, 0, 0);
                    kbe_sleep_ms(i == 2 ? 750 : 70);
                }
                break;
            }
            case 5: {
                int fr, fg, fb;
                kbe_hue_to_rgb(flash_hue, 255, &fr, &fg, &fb);
                flash_hue = (flash_hue + 55) % 360;
                for (int i = 0; i < 3 && g_kbe_running; i++) {
                    kbe_write_frame(fd_col, fr, fg, fb);
                    kbe_sleep_ms(70);
                    if (!g_kbe_running) break;
                    kbe_write_frame(fd_col, 0, 0, 0);
                    kbe_sleep_ms(i == 2 ? 750 : 70);
                }
                break;
            }
            case 6: {
                int val = kbe_candle_step(&candle_val);
                int r = (base_r * val) / 255;
                int g = (base_g * val) / 255;
                int b = (base_b * val) / 255;
                kbe_write_frame(fd_col, r, g, b);
                kbe_sleep_ms(35 + (rand() % 35));
                break;
            }
            case 7: {
                static const uint8_t pulse_wave[] = {
                    30, 90, 180, 255, 230, 160, 100, 60, 40,
                    90, 170, 230, 190, 130, 80, 40, 20, 10, 0
                };
                for (size_t i = 0; i < sizeof(pulse_wave) && g_kbe_running; i++) {
                    int val = pulse_wave[i];
                    int r = (base_r * val) / 255;
                    int g = (base_g * val) / 255;
                    int b = (base_b * val) / 255;
                    kbe_write_frame(fd_col, r, g, b);
                    kbe_sleep_ms(22);
                }
                kbe_sleep_ms(700);
                break;
            }
            default:
                g_kbe_running = 0;
                break;
        }
    }

    kbe_write_bri(fd_bri, orig_bri);
    kbe_write_frame(fd_col, orig_r, orig_g, orig_b);

    if (fd_col >= 0) close(fd_col);
    if (fd_bri >= 0) close(fd_bri);

    unlink(KBE_STATE_PATH);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    unlink(KBE_LOCK_PATH);
}

static int kbe_start(const char *effect)
{
    if (access(KBD_PATH "/multi_intensity", F_OK) != 0) {
        fprintf(stderr, "Error: Keyboard backlight interface not found (is tuxedo_keyboard loaded?)\n");
        return 1;
    }

    pid_t old_pid = 0;
    char old_effect[32] = {0};
    int orig_r = 255, orig_g = 255, orig_b = 255, orig_bri = 255;
    int had_running = kbe_is_running(&old_pid, old_effect, sizeof(old_effect), &orig_r, &orig_g, &orig_b, &orig_bri);

    if (had_running) {
        kbe_stop(1);
    } else {
        if (kbd_get_color(&orig_r, &orig_g, &orig_b) < 0) {
            orig_r = 255; orig_g = 255; orig_b = 255;
        }
        if (kbd_get_raw_brightness(&orig_bri) < 0) {
            orig_bri = 255;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: Failed to spawn background effect daemon\n");
        return 1;
    }

    if (pid > 0) {
        printf("Started keyboard effect '%s' [PID %d]\n", effect, (int)pid);
        return 0;
    }

    setsid();
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > 2) close(null_fd);
    }

    kbe_daemon_worker(effect, orig_r, orig_g, orig_b, orig_bri);
    exit(0);
}

/* ========================================================================
 * EC RAM (for fan telemetry)
 * ======================================================================== */

#define EC_RAM_PATH "/sys/kernel/debug/ec/ec0/io"

/* Try to load ec_sys so we can read EC RAM for fan RPM.
 * Returns 0 on success, -1 if still unavailable. */
static int ensure_ec_sys(void)
{
    if (access(EC_RAM_PATH, F_OK) == 0)
        return 0;
    /* modprobe needs root; if we're not root, just return failure */
    if (geteuid() != 0)
        return -1;
    int rc = system("modprobe ec_sys 2>/dev/null");
    if (rc != 0)
        return -1;
    /* Give udev a moment to create the file */
    usleep(200000);
    return access(EC_RAM_PATH, F_OK) == 0 ? 0 : -1;
}

/* Read fan duty/RPM. Returns 0 on success.
 * cpu_pct/gpu_pct: duty 0-100, cpu_rpm/gpu_rpm: RPM or 0 if unavailable.
 *
 * CPU duty comes from tuxedo_io ioctl (R_CL_FANINFO1 byte 0, always correct).
 * GPU duty comes from tuxedo_io ioctl (R_CL_FANINFO2 byte 0, confirmed accurate).
 * RPM comes from EC RAM 0xD0-0xD3 via debugfs (auto-loads ec_sys). */
static int read_fan_telemetry_ex(int *cpu_pct, int *gpu_pct, int *cpu_rpm, int *gpu_rpm, int cached_fd)
{
    *cpu_pct = *gpu_pct = *cpu_rpm = *gpu_rpm = 0;
    int got_duty = 0;

    /* Try tuxedo IOCTL for CPU and GPU duty. */
    int fd = (cached_fd >= 0) ? cached_fd : tuxedo_open_clevo();
    if (fd >= 0) {
        int f1 = 0, f2 = 0;
        if (ioctl(fd, R_CL_FANINFO1, &f1) >= 0) {
            *cpu_pct = ((f1 & 0xFF) * 100) / 255;
            got_duty = 1;
        }
        if (ioctl(fd, R_CL_FANINFO2, &f2) >= 0) {
            *gpu_pct = ((f2 & 0xFF) * 100) / 255;
            got_duty = 1;
        }
        if (cached_fd < 0) close(fd);
    }

    /* EC RAM via debugfs for RPM only. */
    ensure_ec_sys();

    int ec_fd = open(EC_RAM_PATH, O_RDONLY);
    if (ec_fd < 0)
        return got_duty ? 0 : -1;

    unsigned char ram[256];
    memset(ram, 0, sizeof(ram));
    ssize_t n = read(ec_fd, ram, sizeof(ram));
    close(ec_fd);
    if (n < 0xD4) return got_duty ? 0 : -1;

    /* CPU duty from EC RAM (only if ioctl didn't provide it) */
    if (!got_duty) {
        /* ram[0xF4] sits past the 0xD4 RPM area — only use it if actually read */
        int cpu_raw = (n >= 0xF5) ? ram[0xF4] : 0;
        if (cpu_raw == 0) cpu_raw = ram[0x89];
        *cpu_pct = (cpu_raw * 100) / 255;
    }

    /* RPM from EC RAM: 0xD0:D1 (CPU), 0xD2:D3 (GPU), formula: EC_FAN_RPM_DIVISOR / raw16 */
    unsigned int cpu_raw16 = ((unsigned int)ram[0xD0] << 8) | ram[0xD1];
    *cpu_rpm = (int)(cpu_raw16 > 0 ? EC_FAN_RPM_DIVISOR / cpu_raw16 : 0);

    unsigned int gpu_raw16 = ((unsigned int)ram[0xD2] << 8) | ram[0xD3];
    *gpu_rpm = (int)(gpu_raw16 > 0 ? EC_FAN_RPM_DIVISOR / gpu_raw16 : 0);

    return 0;
}

/* ========================================================================
 * CPU USAGE (from /proc/stat)
 * ======================================================================== */

static unsigned long long prev_idle = 0, prev_total = 0;

/* Read cumulative CPU jiffies from /proc/stat line 1 ("cpu ...").
 * Returns 0 on success, fills idle and total. */
static int read_cpu_jiffies(unsigned long long *idle, unsigned long long *total)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;
    char line[512];
    if (fgets(line, sizeof(line), fp)) {
        unsigned long long user, nice, system, idle_j, iowait, irq, softirq, steal;
        if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &system, &idle_j, &iowait, &irq, &softirq, &steal) >= 4) {
            *idle = idle_j + iowait;
            *total = user + nice + system + idle_j + iowait + irq + softirq + steal;
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

/* Returns CPU usage as a percentage (0-100). First call returns -1 (no baseline). */
static int get_cpu_usage_pct(void)
{
    unsigned long long idle, total;
    if (read_cpu_jiffies(&idle, &total) < 0) return -1;

    if (prev_total == 0) {
        /* First call — just store baseline */
        prev_idle = idle;
        prev_total = total;
        return -1;
    }

    unsigned long long d_idle = idle - prev_idle;
    unsigned long long d_total = total - prev_total;
    prev_idle = idle;
    prev_total = total;

    if (d_total == 0) return 0;
    return (int)((100.0 * (double)(d_total - d_idle)) / (double)d_total);
}

/* ========================================================================
 * MEMORY USAGE (from /proc/meminfo)
 * ======================================================================== */

static void get_mem_usage(long *used_mb, long *total_mb)
{
    *used_mb = *total_mb = 0;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;

    long mem_total = 0, mem_avail = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %ld kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemAvailable: %ld kB", &mem_avail) == 1) break;
    }
    fclose(fp);

    *total_mb = mem_total / 1024;
    *used_mb = (mem_total - mem_avail) / 1024;
}

/* Read CPU package temperature in millidegrees from thermal zone or hwmon.
 * Returns temperature in degrees C, or -1 on failure. */
static int read_cpu_temp(void)
{
    /* Try thermal_zone first */
    DIR *d = opendir("/sys/class/thermal");
    if (d) {
        struct dirent *ent;
        char path[512], type_buf[64];
        while ((ent = readdir(d)) != NULL) {
            if (strncmp(ent->d_name, "thermal_zone", 12) != 0) continue;
            snprintf(path, sizeof(path), "/sys/class/thermal/%s/type", ent->d_name);
            if (read_sysfs_str(path, type_buf, sizeof(type_buf)) < 0) continue;
            /* Look for x86_pkg_temp or coretemp or generic pkg temp */
            if (strstr(type_buf, "x86_pkg") || strstr(type_buf, "pkg") || strstr(type_buf, "coretemp")) {
                snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", ent->d_name);
                long milli = read_sysfs_long(path, -1000);
                if (milli >= -50000 && milli <= 150000) {
                    closedir(d);
                    return (int)(milli / 1000);
                }
            }
        }
        closedir(d);
    }
    /* Fallback: first thermal zone */
    long milli = read_sysfs_long("/sys/class/thermal/thermal_zone0/temp", -1000);
    return (milli >= 0) ? (int)(milli / 1000) : -1;
}

/* ========================================================================
 * CPU MONITOR
 * ======================================================================== */

static volatile int cpumonitor_running = 1;
static void cpumonitor_sigint(int sig) { (void)sig; cpumonitor_running = 0; }

static int cpumonitor(void)
{
    /* Count CPUs */
    int max_cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (max_cpus <= 0) max_cpus = 64;

    struct sigaction sa = { .sa_handler = cpumonitor_sigint };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    printf("Monitor — Press Ctrl+C to stop.\n\n");

    /* Read max frequencies once (don't change during monitor) */
    int p_max_mhz = 0, e_max_mhz = 0;
    /* Pre-computed core classification, sized to the actual CPU count */
    int *is_e_core = calloc((size_t)max_cpus, sizeof(int)); /* 1 = E-core, 0 = P-core */
    float *freqs = malloc((size_t)max_cpus * sizeof(float));
    if (!is_e_core || !freqs) {
        free(is_e_core);
        free(freqs);
        fprintf(stderr, "Error: out of memory allocating CPU arrays\n");
        return -1;
    }
    {
        DIR *d = opendir("/sys/devices/system/cpu");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strncmp(ent->d_name, "cpu", 3) != 0) continue;
                if (ent->d_name[3] < '0' || ent->d_name[3] > '9') continue;
                int cpu_num = atoi(ent->d_name + 3);
                if (cpu_num >= max_cpus) continue;
                char path[512];
                snprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/%s/cpufreq/cpuinfo_max_freq", ent->d_name);
                int ffd = open(path, O_RDONLY);
                if (ffd < 0) continue;
                char fbuf[32] = {0};
                ssize_t n = read(ffd, fbuf, sizeof(fbuf) - 1);
                close(ffd);
                if (n <= 0) continue;
                int mhz = atoi(fbuf) / 1000;
                is_e_core[cpu_num] = is_cpu_e_core(cpu_num);
                if (!is_e_core[cpu_num]) {
                    if (mhz > p_max_mhz) p_max_mhz = mhz;
                } else {
                    if (mhz > e_max_mhz) e_max_mhz = mhz;
                }
            }
            closedir(d);
        }
    }

    /* Open file descriptors to cache across loops */
    int energy_fd = open("/sys/class/powercap/intel-rapl:0/energy_uj", O_RDONLY);
    int pl1_fd = open("/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw", O_RDONLY);
    int pl2_fd = open("/sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw", O_RDONLY);
    int tuxedo_fd = tuxedo_open_clevo();

    while (cpumonitor_running) {
        /* Read RAPL energy */
        long e1 = -1;
        if (energy_fd >= 0) {
            char buf[32] = {0};
            ssize_t n = pread(energy_fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) e1 = atol(buf);
        }

        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        /* Read CPU frequencies from /proc/cpuinfo */
        int cpu_count = 0;
        FILE *fp = fopen("/proc/cpuinfo", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp) && cpu_count < max_cpus) {
                if (strncmp(line, "cpu MHz", 7) == 0) {
                    char *p = strchr(line, ':');
                    if (p) {
                        freqs[cpu_count++] = (float)atof(p + 1);
                    }
                }
            }
            fclose(fp);
        }

        /* Read fan telemetry using cached tuxedo_fd */
        int cpu_pct, gpu_pct, cpu_rpm, gpu_rpm;
        int has_fans = (read_fan_telemetry_ex(&cpu_pct, &gpu_pct, &cpu_rpm, &gpu_rpm, tuxedo_fd) == 0);

        usleep(500000); /* 0.5s */

        /* Read RAPL energy again */
        long e2 = -1;
        if (energy_fd >= 0) {
            char buf[32] = {0};
            ssize_t n = pread(energy_fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) e2 = atol(buf);
        }

        struct timespec t2;
        clock_gettime(CLOCK_MONOTONIC, &t2);

        /* Clear screen and print */
        printf("\033[H\033[J");
        int p_threads_printed = 0;
        printf("%s--- [ P-CORES ] (Performance) Max: %d MHz ---%s\n", C_YLW, p_max_mhz, C_RST);
        for (int i = 0; i < cpu_count; i++) {
            if (!is_e_core[i]) {
                printf("Thread %2d: %s%7.2f MHz%s%s", i, C_CYN, freqs[i], C_RST,
                       (p_threads_printed % 2 == 1 || i == cpu_count - 1) ? "\n" : "  |  ");
                p_threads_printed++;
            }
        }
        if (p_threads_printed % 2 != 0) printf("\n");

        int e_cores_printed = 0;
        for (int i = 0; i < cpu_count; i++) {
            if (is_e_core[i]) {
                if (e_cores_printed == 0) {
                    printf("\n%s--- [ E-CORES ] (Efficiency) Max: %d MHz ---%s\n", C_YLW, e_max_mhz, C_RST);
                }
                printf("Core %2d:   %s%7.2f MHz%s\n", i, C_CYN, freqs[i], C_RST);
                e_cores_printed++;
            }
        }

        printf("\n%s--- [ POWER & TEMP ] ---%s\n", C_YLW, C_RST);
        int temp = read_cpu_temp();
        if (temp >= 0) {
            const char *tc = temp >= 85 ? C_RED : temp >= 70 ? C_YLW : C_GRN;
            printf("CPU Temp:      %s%d°C%s\n", tc, temp, C_RST);
        } else {
            printf("CPU Temp:      %sN/A%s\n", C_DIM, C_RST);
        }
        if (e1 >= 0 && e2 >= 0) {
            double dt = (double)(t2.tv_sec - t1.tv_sec) + (double)(t2.tv_nsec - t1.tv_nsec) / 1e9;
            if (dt > 0) {
                long duj = e2 - e1;
                printf("Package Power: %s%.2f Watts%s\n", C_CYN, ((double)duj / dt) / 1000000.0, C_RST);
            }
        } else {
            printf("Package Power: %sN/A%s\n", C_DIM, C_RST);
        }

        /* Read current PL1/PL2 using pread from cached descriptors */
        {
            long pl1 = -1, pl2 = -1;
            char b[32];
            ssize_t nn;
            if (pl1_fd >= 0) {
                nn = pread(pl1_fd, b, sizeof(b) - 1, 0);
                if (nn > 0) { b[nn] = '\0'; pl1 = atol(b) / 1000000; }
            }
            if (pl2_fd >= 0) {
                nn = pread(pl2_fd, b, sizeof(b) - 1, 0);
                if (nn > 0) { b[nn] = '\0'; pl2 = atol(b) / 1000000; }
            }
            printf("PL1:          %s%ldW%s\n", C_CYN, pl1, C_RST);
            printf("PL2:          %s%ldW%s\n", C_CYN, pl2, C_RST);
        }

        /* CPU Usage */
        int cpu_usage = get_cpu_usage_pct();
        if (cpu_usage >= 0) {
            const char *uc = cpu_usage >= 90 ? C_RED : cpu_usage >= 60 ? C_YLW : C_GRN;
            printf("CPU Usage:    %s%d%%%s\n", uc, cpu_usage, C_RST);
        } else
            printf("CPU Usage:    %s--%s\n", C_DIM, C_RST);

        /* Memory Usage */
        long mem_used, mem_total;
        get_mem_usage(&mem_used, &mem_total);
        if (mem_total > 0) {
            long mem_pct = (mem_used * 100) / mem_total;
            const char *mc = mem_pct >= 90 ? C_RED : mem_pct >= 70 ? C_YLW : C_CYN;
            printf("Memory:       %s%ld MB / %ld MB (%ld%%)%s\n",
                   mc, mem_used, mem_total, mem_pct, C_RST);
        } else
            printf("Memory:       %sN/A%s\n", C_DIM, C_RST);

        printf("\n%s--- [ FANS ] ---%s\n", C_YLW, C_RST);
        if (has_fans) {
            printf("CPU Fan: %s%3d%% duty  %4d RPM%s\n", C_CYN, cpu_pct, cpu_rpm, C_RST);
            if (gpu_pct == 0 && gpu_rpm == 0) {
                printf("GPU Fan: %s%3d%% duty  %4d RPM%s  %s(GPU in D3cold state)%s\n",
                       C_DIM, gpu_pct, gpu_rpm, C_RST, C_DIM, C_RST);
            } else {
                printf("GPU Fan: %s%3d%% duty  %4d RPM%s\n", C_CYN, gpu_pct, gpu_rpm, C_RST);
            }
        } else {
            printf("Fan telemetry: %sN/A (ec_sys not loaded)%s\n", C_DIM, C_RST);
        }

        printf("\n%sPress [Ctrl+C] to stop.%s\n", C_DIM, C_RST);
        fflush(stdout);
    }

    if (energy_fd >= 0) close(energy_fd);
    if (pl1_fd >= 0) close(pl1_fd);
    if (pl2_fd >= 0) close(pl2_fd);
    if (tuxedo_fd >= 0) close(tuxedo_fd);

    free(is_e_core);
    free(freqs);

    printf("\n");
    return 0;
}

/* ========================================================================
 * CLI
 * ======================================================================== */

/* Returns 1 if this process is running from /usr/local/bin/cctl (installed),
 * 0 otherwise. Used to decide whether to show the install hint in help. */
static int is_installed_systemwide(void)
{
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return 0;
    path[n] = '\0';
    return (strcmp(path, "/usr/local/bin/cctl") == 0);
}

/* True when the TUXEDO/Clevo driver stack is loaded. /dev/tuxedo_io only
 * appears once tuxedo_io is up, and clevo_acpi + tuxedo_keyboard come with
 * it as hard modprobe dependencies — so one stat covers the whole stack. */
static int drivers_loaded(void)
{
    return access("/dev/tuxedo_io", F_OK) == 0;
}

static void print_usage(const char *prog)
{
    const char *base = strrchr(prog, '/');
    prog = base ? base + 1 : prog;

    /* ASCII art generated via:
     * curl "https://asciified.thelicato.io/api/v2/ascii?text=COLORCONTROL&font=slant" */
    printf(
    "\n"
    "   %s____ ___  _     ___  ____  %s %s____ ___  _   _ _____ ____   ___  _     %s\n"
    "  %s/ ___/ _ \\| |   / _ \\|  _ \\ %s%s/ ___/ _ \\| \\ | |_   _|  _ \\ / _ \\| |    %s\n"
    " %s| |  | | | | |  | | | | |_) |%s%s |  | | | |  \\| | | | | |_) | | | | |    %s\n"
    " %s| |__| |_| | |__| |_| |  _ <%s%s| |__| |_| | |\\  | | | |  _ <| |_| | |___ %s\n"
    "  %s\\____\\___/|_____\\___/|_| \\_\\%s%s\\____\\___/|_| \\_| |_| |_| \\_\\___/|_____|%s\n"
    "\n",
    C_GRN, C_RST, C_RED, C_RST,
    C_GRN, C_RST, C_RED, C_RST,
    C_GRN, C_RST, C_RED, C_RST,
    C_GRN, C_RST, C_RED, C_RST,
    C_GRN, C_RST, C_RED, C_RST);

    printf("  %sUsage:%s  %s%s%s <command> [options]\n\n", C_BLD, C_RST, C_CYN_BLD, prog, C_RST);

    /* ── Profiles ──────────────────────────────────────────────────────── */
    printf("  %sPROFILES%s\n", C_YLW, C_RST);
    printf("    %sset%s   <profile> [--nosafe] Apply preset %s(EC defaults + table values below)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %ssetR%s  <profile> [--nosafe] Apply preset %s(EC defaults + preconfigured CPU TDP override)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("      %s(max, cpuperf, balanced set fans to auto; bypass with --nosafe)%s\n\n", C_DIM, C_RST);
    printf("      %sProfile     Turbo  Governor     EPP                EC default CPU & GPU TDP (set)  RAPL CPU TDP overide (setR only)%s\n", C_BLD, C_RST);
    printf("      %s─────────── ────── ──────────── ────────────────── ────────────────────────────── ────────────────────────────────%s\n", C_DIM, C_RST);
    printf("      %smax%s         ON     performance  performance        90/115W + GPU 100W              PL1 45 / PL2 90W\n", C_RED, C_RST);
    printf("      %scpuperf%s     ON     performance  performance        45/115W + GPU 70W               %s(no RAPL change)%s\n", C_YLW, C_RST, C_DIM, C_RST);
    printf("      %sbalanced%s    ON     powersave    balance_performance 45/115W + GPU 70W              PL1 35 / PL2 40W\n", C_GRN, C_RST);
    printf("      %spowersave%s   OFF    powersave    balance_power      15/30W  + GPU 70W               %s(no RAPL change)%s\n", C_CYN_BLD, C_RST, C_DIM, C_RST);
    printf("      %seco%s         OFF    powersave    power              15/30W  + GPU 70W               PL1 9 / PL2 10W\n\n", C_DIM, C_RST);

    /* ── Keyboard ───────────────────────────────────────────────────────── */
    printf("  %sKEYBOARD%s\n", C_MAG, C_RST);
    printf("    %skbc%s   <R G B | #hex | preset> Set keyboard color %s(no arg: list presets)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %skbb%s   <pct>              Set brightness %s(0-100%%)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %skbe%s   [effect|stop]      Keyboard backlight effects %s(no arg: show status)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %sfn%s    [lock|unlock]      Toggle/set Fn Lock %s(Fn key behavior)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);

    /* ── Fan ────────────────────────────────────────────────────────────── */
    printf("  %sFAN%s\n", C_YLW, C_RST);
    printf("    %sfan%s   auto|max           Set both fans %s(EC-controlled / full)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %sfan%s   silent [--nosafe]  Quiet mode %s(forces eco profile first; bypass with --nosafe)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %sfan%s   <pct> --nosafe     Set both fans to duty %s(21-100%%)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %sfan%s   cpu|gpu <pct> --nosafe Set individual fan duty %s(21-100%%)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);

    /* ── GPU MUX ───────────────────────────────────────────────────────── */
    printf("  %sGPU MUX%s %s(UEFI NVRAM, reboot required to apply)%s\n", C_MAG, C_RST, C_DIM, C_RST);
    printf("    %smux%s                      Show current MUX mode %s(MSHybrid / dGPU)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %smux%s    switch            Toggle to the other mode %s(reboot to apply)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);

    /* ── Privacy ────────────────────────────────────────────────────────── */
    printf("  %sPRIVACY%s\n", C_CYN, C_RST);
    printf("    %swebcam%s [on|off]          Toggle/set webcam\n",      C_BLD, C_RST);
    printf("    %smic%s    [on|off]          Toggle/set internal microphone %s(laptop mic only, needs alsa/amixer)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);

    /* ── Battery ────────────────────────────────────────────────────────── */
    printf("  %sBATTERY%s\n", C_GRN, C_RST);
    printf("    %sbat%s                      Show current thresholds\n",       C_BLD, C_RST);
    printf("    %sbat%s    <start> <stop>    Set charge thresholds %s(custom)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %sbat max%s                  standard mode %s(change to max - 100%%)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);

    /* ── Info ───────────────────────────────────────────────────────────── */
    printf("  %sINFO%s\n", C_CYN_BLD, C_RST);
    printf("    %sstatus%s                   Show all current settings\n",  C_BLD, C_RST);
    printf("    %smonitor%s                  Live CPU/power/fan monitor\n\n", C_BLD, C_RST);

    /* ── NVIDIA ─────────────────────────────────────────────────────────── */
    printf("  %sNVIDIA%s\n", C_RED, C_RST);
#ifdef CCTL_NVIDIA
    printf("    %snvidia%s power    [on|off]           Hardware D0/D3cold control\n", C_BLD, C_RST);
    printf("    %snvidia%s <on|off>                    Persistent toggle %s(+initramfs rebuild)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %snvidia%s load                        Session load %s(compute modules)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %snvidia%s loadgame                    Session load %s(all modules incl. drm)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %snvidia%s unload                      Session unload + power off\n", C_BLD, C_RST);
    printf("    %snvidia%s status                      Show GPU status & telemetry\n", C_BLD, C_RST);
#else
    printf("    %snvidia%s power    [on|off]           Hardware D0/D3cold control %s(no arg: show state)%s\n", C_BLD, C_RST, C_DIM, C_RST);
#endif
    printf("    %snvidia%s clock    <min,max> | reset  Lock/unlock GPU clocks %s(auto persistence)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %snvidia%s memclock <min,max> | reset  Lock/unlock memory clocks %s(auto persistence)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);

    /* ── Display ────────────────────────────────────────────────────────── */
    if (has_display_support()) {
        printf("  %sDISPLAY%s %s(only X11 session is supported, needs xrandr)%s\n", C_BLU, C_RST, C_DIM, C_RST);
        printf("    %srr%s    [rate]             List/set refresh rate %s(1=high, 2=low)%s\n", C_BLD, C_RST, C_DIM, C_RST);
        printf("    %sscale%s <factor|WxH|off>   GPU-side scaling %s(no arg: explain in detail)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);
    }

    /* ── Profile Individual Overrides ───────────────────────────────────── */
    printf("  %sPROFILE INDIVIDUAL OVERRIDES%s\n", C_YLW, C_RST);
    printf("    %sturbo%s  <on|off> [--nosafe] Turbo boost override %s(on sets fans to auto; bypass with --nosafe)%s\n",  C_BLD, C_RST, C_DIM, C_RST);
    printf("    %sgov%s    <governor>        CPU governor %s(powersave, performance)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %sepp%s    <value>           EPP %s(performance, balance_performance, balance_power, power)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %srapl%s   <pl1> <pl2>       RAPL power limits %s(watts, use 'skip' to omit)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);

    /* Driver hint — only shown when the TUXEDO/Clevo stack is not loaded */
    if (!drivers_loaded()) {
        printf("  %sDRIVERS NOT LOADED%s — some features need them:\n", C_YLW, C_RST);
        printf("    • %skbc/kbb%s   keyboard backlight (%stuxedo_keyboard%s)\n", C_CYN, C_RST, C_DIM, C_RST);
        printf("    • %sset/setR%s GPU performance slots (%stuxedo_io%s)\n", C_CYN, C_RST, C_DIM, C_RST);
        printf("    • %sbat%s      battery charge thresholds (%sclevo_acpi%s)\n", C_CYN, C_RST, C_DIM, C_RST);
        printf("    Fix: run %scctl drivers-install%s\n\n",
               C_BLD, C_RST);
    }

    /* Install hint — only shown when not installed system-wide */
    if (!is_installed_systemwide()) {
        printf("  %sNOT INSTALLED%s — run %ssudo ./%s install%s to set up:\n",
               C_YLW, C_RST, C_BLD, prog, C_RST);
        printf("    • Adds cctl to your PATH — run %scctl%s from anywhere\n", C_CYN, C_RST);
        printf("    • Passwordless sudo — %ssudo cctl <cmd>%s never prompts for a password\n", C_CYN, C_RST);
        printf("    • Auto-elevation — %scctl%s elevates automatically via passwordless sudo\n\n", C_CYN, C_RST);
    }

    printf("  %sv%s%s\n", C_DIM, CCTL_VERSION, C_RST);
}

static int nvidia_is_loaded(void)
{
    FILE *fp = fopen("/proc/modules", "r");
    if (!fp) return 0;
    char line[256];
    int loaded = 0;
    while (fgets(line, sizeof(line), fp)) {
        char modname[64];
        if (sscanf(line, "%63s", modname) == 1) {
            if (strcmp(modname, "nvidia") == 0) {
                loaded = 1;
                break;
            }
        }
    }
    fclose(fp);
    return loaded;
}

#ifdef CCTL_NVIDIA
/* ========================================================================
 * NVIDIA GPU
 * ========================================================================
 * Commands:
 *   nvidia on|off  — Persistent toggle: blacklist/unblacklist +
 *                               initramfs rebuild + modprobe/rmmod.
 *   nvidia load              — Session-only: wake GPU (D3cold→D0), temp-remove
 *                               blacklist, modprobe nvidia + nvidia_uvm, restore
 *                               blacklist. Requires blacklist mode.
 *   nvidia loadgame          — Session-only: wake GPU (D3cold→D0), temp-remove
 *                               blacklist, modprobe all 4 nvidia modules, restore
 *                               blacklist. Requires blacklist mode.
 *   nvidia unload            — Session-only: rmmod all nvidia modules, power off
 *                               GPU (D0→D3cold). Requires blacklist mode.
 *   nvidia status            — Show boot config, module state, GPU telemetry.
 *   nvidia clock <min,max>   — Auto-enable persistence + lock GPU clocks (-lgc).
 *   nvidia clock reset       — Unlock GPU clocks (-rgc), persistence untouched.
 *   nvidia memclock <min,max>— Auto-enable persistence + lock memory clocks (-lmc).
 *   nvidia memclock reset    — Unlock memory clocks (-rmc), persistence untouched.
 *   nvidia power [on|off]    — Direct PCI runtime PM control (D0/D3cold).
 *                               Useful when no nvidia driver is loaded.
 * ======================================================================== */

static int nvidia_is_blacklisted(void)
{
    if (access("/etc/modprobe.d/blacklist-nvidia.conf", F_OK) != 0)
        return 0;
    FILE *fp = fopen("/etc/modprobe.d/blacklist-nvidia.conf", "r");
    if (!fp) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "blacklist nvidia", 16) == 0) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

static int nvidia_gpu_in_use(void)
{
    if (!nvidia_is_loaded()) return 0;
    FILE *fp = popen("nvidia-smi --query-compute-apps=pid --format=csv,noheader 2>/dev/null", "r");
    if (!fp) return 0;
    char line[128];
    int in_use = 0;
    if (fgets(line, sizeof(line), fp)) {
        in_use = 1;
    }
    pclose(fp);
    return in_use;
}

/* Detect whether the NVIDIA GPU is driving a connected display (DRM active).
 * Returns 1 if an nvidia/nvidia_drm-driven, connected connector is found. */
static int nvidia_display_in_use(void)
{
    DIR *d = opendir("/sys/class/drm");
    if (!d) return 0;
    struct dirent *ent;
    char path[512];
    int in_use = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "card", 4) != 0) continue;
        if (ent->d_name[4] < '0' || ent->d_name[4] > '9') continue;

        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/driver", ent->d_name);
        char driver[64] = {0};
        ssize_t l = readlink(path, driver, sizeof(driver) - 1);
        if (l <= 0) continue;
        driver[l] = '\0';
        char *base = strrchr(driver, '/');
        if (!base) continue;
        if (strcmp(base + 1, "nvidia") != 0 && strcmp(base + 1, "nvidia_drm") != 0)
            continue;

        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", ent->d_name);
        FILE *fp = fopen(path, "r");
        if (fp) {
            char st[16] = {0};
            if (fgets(st, sizeof(st), fp) && strncmp(st, "connected", 9) == 0)
                in_use = 1;
            fclose(fp);
        }
        if (in_use) break;
    }
    closedir(d);
    return in_use;
}

static int run_cmd_silent(const char *cmd, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(cmd, argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}
#endif /* CCTL_NVIDIA */

static int run_cmd(const char *cmd, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(cmd, argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Forward declarations: common PCI power helpers (defined after the
 * CCTL_NVIDIA block so both builds share them). */
static int nvidia_find_pci_address(char *buf, size_t bufsz);
static int nvidia_power_show(void);
static int nvidia_power_set(int on);

#ifdef CCTL_NVIDIA
/* Check whether an initramfs image exists in /boot.
 * Returns 1 if found, 0 otherwise. */
static int initramfs_present(void)
{
    DIR *d = opendir("/boot");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strstr(e->d_name, "initramfs") || strstr(e->d_name, "initrd")) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* Print a distro-appropriate install command for the initramfs rebuild tool. */
static void print_initramfs_install_hint(void)
{
    char id[64] = {0};
    FILE *fp = fopen("/etc/os-release", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "ID=", 3) == 0) {
                char *val = line + 3;
                while (*val == '"' || *val == '\'') val++;
                size_t len = strlen(val);
                while (len > 0 && (val[len - 1] == '"' || val[len - 1] == '\'' ||
                                   val[len - 1] == '\n' || val[len - 1] == '\r'))
                    val[--len] = '\0';
                strncpy(id, val, sizeof(id) - 1);
                break;
            }
        }
        fclose(fp);
    }

    if (strstr(id, "arch") || strstr(id, "manjaro") || strstr(id, "endeavouros"))
        fprintf(stderr, "  Install: sudo pacman -S mkinitcpio\n");
    else if (strstr(id, "fedora"))
        fprintf(stderr, "  Install: sudo dnf install dracut\n");
    else if (strstr(id, "rhel") || strstr(id, "centos") || strstr(id, "rocky") || strstr(id, "alma"))
        fprintf(stderr, "  Install: sudo dnf install dracut\n");
    else if (strstr(id, "suse"))
        fprintf(stderr, "  Install: sudo zypper install dracut\n");
    else if (strstr(id, "debian") || strstr(id, "ubuntu") || strstr(id, "mint") ||
             strstr(id, "pop") || strstr(id, "elementary") || strstr(id, "zorin"))
        fprintf(stderr, "  Install: sudo apt install initramfs-tools\n");
    else
        fprintf(stderr, "  Install one of:\n"
                "    sudo pacman -S mkinitcpio          (Arch/Manjaro)\n"
                "    sudo dnf install dracut             (Fedora/RHEL)\n"
                "    sudo zypper install dracut           (openSUSE)\n"
                "    sudo apt install initramfs-tools     (Debian/Ubuntu)\n");
}

/* Pre-check whether an initramfs rebuild can be performed.
 * Must be called BEFORE writing any config files so that on failure
 * nothing is left in a half-applied state.
 *
 * Returns:
 *   1  -> initramfs tool present; caller may write config then rebuild
 *   0  -> no initramfs on this system; no rebuild needed (config-only)
 *  -1  -> initramfs present but no rebuild tool installed; caller MUST abort
 */
static int initramfs_rebuild_ready(void)
{
    int has_initramfs = initramfs_present();
    if (!has_initramfs) {
        printf("  No initramfs detected in /boot. The modprobe blacklist\n");
        printf("  takes effect directly on next boot without a rebuild.\n");
        return 0;
    }

    if (access("/usr/bin/mkinitcpio", X_OK) == 0 ||
        access("/usr/bin/dracut", X_OK) == 0 ||
        access("/usr/sbin/update-initramfs", X_OK) == 0 ||
        access("/sbin/update-initramfs", X_OK) == 0)
        return 1;

    /* initramfs present but no tool to rebuild it */
    fprintf(stderr, "\n  %sError: This system uses an initramfs but no rebuild tool is installed.%s\n\n", C_RED, C_RST);
    print_initramfs_install_hint();
    fprintf(stderr, "  Install the tool first, then re-run this command.\n\n");
    return -1;
}

/* Rebuild the initramfs using whichever tool is available.
 * Caller must have already confirmed initramfs_rebuild_ready() == 1.
 * Returns 0 on success, -1 on failure. */
static int rebuild_initramfs(void)
{
    if (access("/usr/bin/mkinitcpio", X_OK) == 0) {
        printf("  Rebuilding initramfs with mkinitcpio (this may take a minute)...\n");
        char *const args[] = { "mkinitcpio", "-P", NULL };
        return run_cmd("/usr/bin/mkinitcpio", args);
    }
    if (access("/usr/bin/dracut", X_OK) == 0) {
        printf("  Rebuilding initramfs with dracut (this may take a minute)...\n");
        char *const args[] = { "dracut", "--force", NULL };
        return run_cmd("/usr/bin/dracut", args);
    }
    if (access("/usr/sbin/update-initramfs", X_OK) == 0) {
        printf("  Rebuilding initramfs with update-initramfs (this may take a minute)...\n");
        char *const args[] = { "update-initramfs", "-u", NULL };
        return run_cmd("/usr/sbin/update-initramfs", args);
    }
    if (access("/sbin/update-initramfs", X_OK) == 0) {
        printf("  Rebuilding initramfs with update-initramfs (this may take a minute)...\n");
        char *const args[] = { "update-initramfs", "-u", NULL };
        return run_cmd("/sbin/update-initramfs", args);
    }
    /* Should never reach here if initramfs_rebuild_ready() was called first */
    fprintf(stderr, "Error: No initramfs tool found (mkinitcpio/dracut/update-initramfs).\n");
    return -1;
}

static int try_unload_nvidia(void)
{
    printf("  Attempting to unload NVIDIA modules...\n");
    if (nvidia_gpu_in_use()) {
        fprintf(stderr, "Error: GPU is actively in use by running processes:\n");
        run_quiet("nvidia-smi --query-compute-apps=pid,name --format=csv,noheader 2>/dev/null | awk -F', ' '{print \"    PID \" $1 \": \" $2}'");
        fprintf(stderr, "Cannot unload modules while GPU is in use.\n");
        return -1;
    }

    const char *modules[] = { "nvidia_drm", "nvidia_modeset", "nvidia_uvm", "nvidia" };
    int loaded[4] = {0};

    /* Single scan of /proc/modules for all nvidia modules */
    FILE *check = fopen("/proc/modules", "r");
    if (check) {
        char line[256];
        while (fgets(line, sizeof(line), check)) {
            char name[64];
            if (sscanf(line, "%63s", name) == 1) {
                for (size_t i = 0; i < 4; i++) {
                    if (strcmp(name, modules[i]) == 0) {
                        loaded[i] = 1;
                        break;
                    }
                }
            }
        }
        fclose(check);
    }

    int failed = 0;
    for (size_t i = 0; i < 4; i++) {
        if (loaded[i]) {
            char *const args[] = { "modprobe", "-r", (char *)modules[i], NULL };
            if (run_cmd_silent("modprobe", args) == 0) {
                printf("  Unloaded %s\n", modules[i]);
            } else {
                fprintf(stderr, "  Failed to unload %s\n", modules[i]);
                failed = 1;
            }
        }
    }
    return failed ? -1 : 0;
}

static int try_load_nvidia(void)
{
    printf("  Attempting to load NVIDIA modules...\n");
    char *const args[] = { "modprobe", "nvidia", NULL };
    if (run_cmd_silent("modprobe", args) == 0) {
        printf("  NVIDIA modules loaded successfully.\n");
        return 0;
    }
    fprintf(stderr, "  Could not load NVIDIA modules. Reboot may be required.\n");
    return -1;
}

static int reply_is_yes(const char *s)
{
    char buf[64];
    size_t n = 0;
    while (*s && n < sizeof(buf) - 1) {
        buf[n++] = (char)tolower((unsigned char)*s);
        s++;
    }
    buf[n] = '\0';
    size_t a = 0;
    while (a < n && (buf[a] == ' ' || buf[a] == '\t' || buf[a] == '\n' || buf[a] == '\r')) a++;
    size_t b = n;
    while (b > a && (buf[b-1] == ' ' || buf[b-1] == '\t' || buf[b-1] == '\n' || buf[b-1] == '\r')) b--;
    buf[b] = '\0';
    return strcmp(buf + a, "yes") == 0;
}

static int nvidia_set_off(void)
{
    if (nvidia_is_blacklisted() && !nvidia_is_loaded()) {
        printf("  NVIDIA is already blacklisted and modules are unloaded.\n");
        return 0;
    }

    /* Experimental / permanent-disable warning + double confirmation */
    printf("\n");
    printf("  %sWARNING: 'nvidia off' PERMANENTLY disables the discrete GPU at boot.%s\n", C_YLW, C_RST);
    printf("  This is experimental and can break games and other dGPU-accelerated\n");
    printf("  software. The GPU stays OFF after reboot until you run 'nvidia on'.\n");
    printf("  This option is intended for developers, hardware tinkerers, and users\n");
    printf("  who know exactly what they are doing.\n");
    printf("  %sTry 'nvidia unload' first%s to switch the GPU off for this session\n", C_CYN, C_RST);
    printf("  only (it reverts after reboot). Use 'nvidia off' as a LAST RESORT.\n");

    char reply[64];
    printf("  Type 'yes' to continue, or anything else to abort: ");
    if (!fgets(reply, sizeof(reply), stdin) || !reply_is_yes(reply)) {
        printf("  Aborted.\n");
        return 0;
    }
    printf("  Are you sure? Type 'yes' again to proceed: ");
    if (!fgets(reply, sizeof(reply), stdin) || !reply_is_yes(reply)) {
        printf("  Aborted.\n");
        return 0;
    }

    if (nvidia_is_blacklisted()) {
        /* blacklisted but modules still loaded -> just unload now */
        printf("  NVIDIA is blacklisted but modules are still loaded (reboot pending).\n");
        try_unload_nvidia();
        return 0;
    }

    /* Pre-check: can initramfs be rebuilt if needed? Must be called
     * BEFORE writing the blacklist so nothing is left half-applied. */
    int initrd_ready = initramfs_rebuild_ready();
    if (initrd_ready < 0)
        return -1;

    printf("  Writing blacklist to /etc/modprobe.d/blacklist-nvidia.conf...\n");
    FILE *fp = fopen("/etc/modprobe.d/blacklist-nvidia.conf", "w");
    if (!fp) {
        perror("fopen blacklist-nvidia.conf");
        return -1;
    }
    fprintf(fp, "# Disabled by cctl nvidia off\n");
    const char *modules[] = { "nvidia_drm", "nvidia_modeset", "nvidia_uvm", "nvidia" };
    for (size_t i = 0; i < 4; i++) {
        fprintf(fp, "blacklist %s\n", modules[i]);
        fprintf(fp, "alias %s off\n", modules[i]);
    }
    fclose(fp);

    if (initrd_ready == 1) {
        if (rebuild_initramfs() < 0) {
            fprintf(stderr, "Error: Initramfs rebuild failed. Removing blacklist config.\n");
            unlink("/etc/modprobe.d/blacklist-nvidia.conf");
            return -1;
        }
    }

    if (nvidia_is_loaded()) {
        try_unload_nvidia();
    }

    printf("\n  NVIDIA is %sblacklisted%s. Reboot to complete. After reboot, only iGPU will be active.\n", C_RED, C_RST);
    return 0;
}

static int nvidia_set_on(void)
{
    if (!nvidia_is_blacklisted()) {
        if (nvidia_is_loaded()) {
            printf("  NVIDIA is already enabled and modules are loaded.\n");
        } else {
            printf("  NVIDIA is enabled but modules aren't loaded.\n");
            int confirm = 0;
            if (!confirm) {
                printf("  Attempt to load modules now? [y/N] ");
                char reply[16];
                if (fgets(reply, sizeof(reply), stdin) && (reply[0] == 'y' || reply[0] == 'Y')) {
                    confirm = 1;
                }
            }
            if (confirm) try_load_nvidia();
        }
        return 0;
    }

    int confirm = 0;
    if (!confirm) {
        printf("  Unblacklist NVIDIA and rebuild initramfs? [y/N] ");
        char reply[16];
        if (!fgets(reply, sizeof(reply), stdin) || (reply[0] != 'y' && reply[0] != 'Y')) {
            printf("  Aborted.\n");
            return 0;
        }
    }

    /* Pre-check: can initramfs be rebuilt if needed? */
    int initrd_ready = initramfs_rebuild_ready();
    if (initrd_ready < 0)
        return -1;

    printf("  Removing /etc/modprobe.d/blacklist-nvidia.conf...\n");
    unlink("/etc/modprobe.d/blacklist-nvidia.conf");

    if (initrd_ready == 1) {
        if (rebuild_initramfs() < 0) {
            fprintf(stderr, "Error: Initramfs rebuild failed. Restoring blacklist config.\n");
            FILE *fp = fopen("/etc/modprobe.d/blacklist-nvidia.conf", "w");
            if (fp) {
                fprintf(fp, "# Disabled by cctl nvidia off\n");
                const char *modules[] = { "nvidia_drm", "nvidia_modeset", "nvidia_uvm", "nvidia" };
                for (size_t i = 0; i < 4; i++) {
                    fprintf(fp, "blacklist %s\n", modules[i]);
                    fprintf(fp, "alias %s off\n", modules[i]);
                }
                fclose(fp);
            }
            return -1;
        }
    }

    if (!nvidia_is_loaded()) {
        try_load_nvidia();
    }

    printf("\n  NVIDIA is %senabled%s. Reboot to complete. After reboot, NVIDIA will be available.\n", C_GRN, C_RST);
    return 0;
}

static void nvidia_show_status(void)
{
    int blacklisted = nvidia_is_blacklisted();
    int loaded = nvidia_is_loaded();

    printf("  Boot config:   %s%s%s\n", blacklisted ? C_RED : C_GRN,
           blacklisted ? "BLACKLISTED" : "ENABLED", C_RST);
    printf("  Module state:  %s%s%s\n", loaded ? C_GRN : C_DIM,
           loaded ? "LOADED" : "NOT LOADED", C_RST);

    if (blacklisted && loaded) {
        printf("  GPU state:     %sON%s (session-only) — stays OFF after reboot\n", C_GRN, C_RST);
    } else if (blacklisted && !loaded) {
        printf("  GPU state:     %sOFF%s (persistent)\n", C_DIM, C_RST);
    } else if (!blacklisted && loaded) {
        printf("  GPU state:     %sON%s (persistent)\n", C_GRN, C_RST);
    } else {
        printf("  GPU state:     %sOFF%s\n", C_DIM, C_RST);
        printf("  Pending:       %sReboot needed to load%s\n", C_YLW, C_RST);
    }

    if (loaded) {
        if (access("/usr/bin/nvidia-smi", X_OK) == 0) {
            printf("\n%s--- NVIDIA GPU Telemetry ---%s\n", C_YLW, C_RST);
            run_quiet("nvidia-smi --query-gpu=name,driver_version,memory.used,memory.total,power.draw,temperature.gpu,persistence_mode --format=csv,noheader,nounits 2>/dev/null | "
                   "awk -F', ' '{print \"  GPU:           \" $1 \"\\n  Driver:        \" $2 \"\\n  VRAM:          \" $3 \" / \" $4 \" MiB\\n  Power draw:    \" $5 \" W\\n  Temperature:   \" $6 \"°C\\n  Persistence:   \" $7}'");
            
            FILE *p_fp = popen("nvidia-smi --query-compute-apps=pid,name,used_memory --format=csv,noheader 2>/dev/null", "r");
            if (p_fp) {
                char line[256];
                int has_procs = 0;
                while (fgets(line, sizeof(line), p_fp)) {
                    if (!has_procs) {
                        printf("\n  GPU Processes:\n");
                        has_procs = 1;
                    }
                    char pid[32] = {0}, name[128] = {0}, mem[64] = {0};
                    if (sscanf(line, "%31[^,], %127[^,], %63[^\n]", pid, name, mem) >= 2) {
                        printf("    PID %-8s %-30s %s\n", pid, name, mem);
                    }
                }
                pclose(p_fp);
                if (!has_procs) {
                    printf("  Processes:     none\n");
                }
            }
        }
    }
}

static int nvidia_load(int load_game)
{
    if (!nvidia_is_blacklisted()) {
        fprintf(stderr, "  Error: NVIDIA is not blacklisted. Use 'nvidia on' instead.\n");
        return 1;
    }
    if (nvidia_is_loaded()) {
        printf("  NVIDIA module is already loaded.\n");
        return 0;
    }

    /* Wake GPU from D3cold if needed */
    char pci_path[512];
    if (nvidia_find_pci_address(pci_path, sizeof(pci_path)) == 0) {
        char ctrl_path[576];
        snprintf(ctrl_path, sizeof(ctrl_path), "%s/power/control", pci_path);
        FILE *fp = fopen(ctrl_path, "w");
        if (fp) {
            fprintf(fp, "on");
            fclose(fp);
            printf("  GPU powered on (D0).\n");
        }
    }

    /* Temporarily move blacklist aside so modprobe works */
    printf("  Loading NVIDIA modules (session-only)...\n");
    rename("/etc/modprobe.d/blacklist-nvidia.conf",
           "/etc/modprobe.d/blacklist-nvidia.conf.bak");

    char *const args_nv[] = { "modprobe", "nvidia", NULL };
    int ret = run_cmd("modprobe", args_nv);
    if (ret == 0) {
        char *const args_uvm[] = { "modprobe", "nvidia_uvm", NULL };
        run_cmd("modprobe", args_uvm);
        if (load_game) {
            char *const args_modeset[] = { "modprobe", "nvidia_modeset", NULL };
            run_cmd("modprobe", args_modeset);
            char *const args_drm[] = { "modprobe", "nvidia_drm", NULL };
            run_cmd("modprobe", args_drm);
        }
    }

    /* Restore blacklist immediately */
    rename("/etc/modprobe.d/blacklist-nvidia.conf.bak",
           "/etc/modprobe.d/blacklist-nvidia.conf");

    if (ret == 0) {
        if (load_game) {
            printf("  NVIDIA modules loaded (nvidia + nvidia_uvm + nvidia_modeset + nvidia_drm). GPU available for this session.\n");
        } else {
            printf("  NVIDIA modules loaded (nvidia + nvidia_uvm). GPU available for this session.\n");
            printf("  %sNote:%s For display/gaming, also run: sudo modprobe nvidia_drm (or use loadgame)\n", C_YLW, C_RST);
        }
        printf("  On next reboot, NVIDIA will remain off (blacklist intact).\n");
    } else {
        fprintf(stderr, "  Failed to load NVIDIA modules.\n");
    }
    return ret;
}

static int nvidia_unload(void)
{
    if (!nvidia_is_blacklisted()) {
        fprintf(stderr, "  Error: NVIDIA is not blacklisted. Use 'nvidia off' instead.\n");
        return 1;
    }
    if (!nvidia_is_loaded()) {
        printf("  NVIDIA modules are not loaded.\n");
        return 0;
    }

    if (nvidia_gpu_in_use()) {
        fprintf(stderr, "  Error: GPU is actively in use by running processes.\n");
        return 1;
    }

    if (nvidia_display_in_use()) {
        fprintf(stderr, "  Warning: GPU appears to be driving a display (nvidia_drm active).\n");
        fprintf(stderr, "  Unload may fail unless the session has released it (see AutoAddGPU note in README).\n");
    }

    printf("  Unloading NVIDIA modules...\n");
    const char *modules[] = { "nvidia_drm", "nvidia_modeset", "nvidia_uvm", "nvidia" };
    int loaded[4] = {0};

    /* Single scan of /proc/modules for all nvidia modules */
    FILE *check = fopen("/proc/modules", "r");
    if (check) {
        char line[256];
        while (fgets(line, sizeof(line), check)) {
            char name[64];
            if (sscanf(line, "%63s", name) == 1) {
                for (size_t i = 0; i < 4; i++) {
                    if (strcmp(name, modules[i]) == 0) {
                        loaded[i] = 1;
                        break;
                    }
                }
            }
        }
        fclose(check);
    }

    int failed = 0;
    for (size_t i = 0; i < 4; i++) {
        if (!loaded[i]) continue;

        /* Use rmmod directly instead of modprobe -r (avoids blacklist alias interference) */
        char *const args[] = { "rmmod", (char *)modules[i], NULL };
        if (run_cmd("rmmod", args) == 0) {
            printf("  Unloaded %s\n", modules[i]);
        } else {
            fprintf(stderr, "  Failed to unload %s\n", modules[i]);
            failed = 1;
        }
    }

    /* Verify nvidia is actually gone */
    if (nvidia_is_loaded()) {
        fprintf(stderr, "  Warning: NVIDIA module is still loaded.\n");
        return -1;
    }

    /* Power off the GPU via PCI runtime PM */
    char pci_path[512];
    if (nvidia_find_pci_address(pci_path, sizeof(pci_path)) == 0) {
        char ctrl_path[576];
        snprintf(ctrl_path, sizeof(ctrl_path), "%s/power/control", pci_path);
        FILE *fp = fopen(ctrl_path, "w");
        if (fp) {
            fprintf(fp, "auto");
            fclose(fp);
            printf("  GPU powered off (D3cold).\n");
        }
    }
    return failed ? -1 : 0;
}
#endif /* CCTL_NVIDIA */

/* ========================================================================
 * NVIDIA PCI POWER (available in all builds)
 * Direct PCI runtime PM control (D0/D3cold). No driver modules required.
 * ======================================================================== */

/* Find the nvidia GPU PCI sysfs path (vendor 0x10de, VGA class 0x0300xx) */
static int nvidia_find_pci_address(char *buf, size_t bufsz)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", ent->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        char vendor[16] = {0};
        if (fgets(vendor, sizeof(vendor), fp)) {
            vendor[strcspn(vendor, "\n")] = 0;
        }
        fclose(fp);
        if (strcmp(vendor, "0x10de") != 0) continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", ent->d_name);
        fp = fopen(path, "r");
        if (!fp) continue;
        char class[16] = {0};
        if (fgets(class, sizeof(class), fp)) {
            class[strcspn(class, "\n")] = 0;
        }
        fclose(fp);
        /* VGA compatible controller: class 0x030000 or 0x0300xx */
        if (strncmp(class, "0x0300", 6) == 0) {
            snprintf(buf, bufsz, "/sys/bus/pci/devices/%s", ent->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

/* Show current GPU PCI power state (no root needed). Returns 0 on success. */
static int nvidia_power_show(void)
{
    char pci_path[512];
    if (nvidia_find_pci_address(pci_path, sizeof(pci_path)) != 0) {
        fprintf(stderr, "  Error: No NVIDIA GPU found on PCI bus.\n");
        return 1;
    }
    char state_path[576];
    snprintf(state_path, sizeof(state_path), "%s/power_state", pci_path);
    char state[16] = "unknown";
    FILE *fp = fopen(state_path, "r");
    if (fp) {
        if (fgets(state, sizeof(state), fp))
            state[strcspn(state, "\n")] = 0;
        fclose(fp);
    }
    printf("  GPU power: %s%s%s\n",
           strcmp(state, "D3cold") == 0 ? C_DIM : C_GRN, state, C_RST);
    return 0;
}

/* Run nvidia-smi -pm <1|0> (toggle persistence mode) */
static int nvidia_pm_set(int on)
{
    char *const args[] = { "nvidia-smi", "-pm", on ? "1" : "0", NULL };
    return run_cmd("nvidia-smi", args);
}

/* Check if NVIDIA persistence mode is currently enabled */
static int nvidia_pm_is_enabled(void)
{
    if (!nvidia_is_loaded()) return 0;
    FILE *fp = popen("nvidia-smi --query-gpu=persistence_mode --format=csv,noheader 2>/dev/null", "r");
    if (!fp) return 0;
    char line[64];
    int enabled = 0;
    if (fgets(line, sizeof(line), fp)) {
        if (strncasecmp(line, "Enabled", 7) == 0)
            enabled = 1;
    }
    pclose(fp);
    return enabled;
}

static int nvidia_power_set(int on)
{
    char pci_path[512];
    if (nvidia_find_pci_address(pci_path, sizeof(pci_path)) != 0) {
        fprintf(stderr, "  Error: No NVIDIA GPU found on PCI bus.\n");
        return 1;
    }

    /* If turning off and persistence mode is enabled, disable it first so the
     * driver releases the GPU and allows PCI runtime PM to enter D3cold. */
    if (!on && nvidia_pm_is_enabled()) {
        nvidia_pm_set(0);
    }

    char ctrl_path[576], state_path[576];
    snprintf(ctrl_path, sizeof(ctrl_path), "%s/power/control", pci_path);
    snprintf(state_path, sizeof(state_path), "%s/power_state", pci_path);

    FILE *fp = fopen(ctrl_path, "w");
    if (!fp) {
        perror("  Failed to write PCI power control");
        return 1;
    }
    fprintf(fp, "%s", on ? "on" : "auto");
    fclose(fp);

    /* Wait for PCI runtime PM to transition power state */
    usleep(200000); /* 200ms */

    /* Read back power state */
    char state[16] = "unknown";
    fp = fopen(state_path, "r");
    if (fp) {
        if (fgets(state, sizeof(state), fp))
            state[strcspn(state, "\n")] = 0;
        fclose(fp);
    }

    printf("  GPU power: %s%s%s\n",
           strcmp(state, "D3cold") == 0 ? C_DIM : C_GRN, state, C_RST);
    return 0;
}

/* Auto-enable persistence + lock GPU clocks to [min,max] (nvidia-smi -lgc) */
static int nvidia_clock_set(int min, int max)
{
    if (nvidia_pm_set(1) != 0)
        fprintf(stderr, "  Warning: failed to enable persistence mode\n");
    char range[32];
    snprintf(range, sizeof(range), "%d,%d", min, max);
    char *const args[] = { "nvidia-smi", "-lgc", range, NULL };
    return run_cmd("nvidia-smi", args);
}

/* Run nvidia-smi -rgc (reset/unlock GPU clocks) */
static int nvidia_clock_reset(void)
{
    char *const args[] = { "nvidia-smi", "-rgc", NULL };
    return run_cmd("nvidia-smi", args);
}

/* Auto-enable persistence + lock GPU memory clocks to [min,max] (nvidia-smi -lmc) */
static int nvidia_memclock_set(int min, int max)
{
    if (nvidia_pm_set(1) != 0)
        fprintf(stderr, "  Warning: failed to enable persistence mode\n");
    char range[32];
    snprintf(range, sizeof(range), "%d,%d", min, max);
    char *const args[] = { "nvidia-smi", "-lmc", range, NULL };
    return run_cmd("nvidia-smi", args);
}

/* Run nvidia-smi -rmc (reset/unlock GPU memory clocks) */
static int nvidia_memclock_reset(void)
{
    char *const args[] = { "nvidia-smi", "-rmc", NULL };
    return run_cmd("nvidia-smi", args);
}

/* Parse "<min>,<max>" into two valid clock values. Returns 0 on success. */
static int nvidia_parse_clock_range(const char *str, int *min, int *max)
{
    int a = 0, b = 0;
    if (sscanf(str, "%d,%d", &a, &b) != 2) return -1;
    if (a <= 0 || b <= 0 || a > b) return -1;
    *min = a;
    *max = b;
    return 0;
}

#ifdef CCTL_NVIDIA
#define NVIDIA_USAGE_STR "nvidia {on|off|load|loadgame|unload|status|power|clock|memclock}"
#else
#define NVIDIA_USAGE_STR "nvidia {power|clock|memclock} (module commands require make experimental)"
#endif

static int cmd_nvidia(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Error: Missing action for nvidia command\n");
        fprintf(stderr, "Usage: %s\n", NVIDIA_USAGE_STR);
        return 1;
    }
    const char *action = argv[2];

#ifndef CCTL_NVIDIA
    /* Module/GPU-toggle commands are only compiled into the experimental build. */
    if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0 ||
        strcmp(action, "load") == 0 || strcmp(action, "loadgame") == 0 ||
        strcmp(action, "unload") == 0 || strcmp(action, "status") == 0) {
        fprintf(stderr, "Error: 'nvidia %s' requires experimental build (make experimental).\n", action);
        return 1;
    }
#endif

#ifdef CCTL_NVIDIA
    if (strcmp(action, "status") == 0) {
        nvidia_show_status();
        return 0;
    }
#endif

    /* nvidia power with no argument shows state (no root needed, all builds) */
    if (strcmp(action, "power") == 0 && argc < 4) {
        return nvidia_power_show();
    }

    /* All other actions need root */
    if (geteuid() != 0) {
        self_elevate(argc, argv);
    }

#ifdef CCTL_NVIDIA
    if (strcmp(action, "off") == 0) {
        return nvidia_set_off();
    } else if (strcmp(action, "on") == 0) {
        return nvidia_set_on();
    } else if (strcmp(action, "load") == 0) {
        return nvidia_load(0);
    } else if (strcmp(action, "loadgame") == 0) {
        return nvidia_load(1);
    } else if (strcmp(action, "unload") == 0) {
        return nvidia_unload();
    } else
#endif
    if (strcmp(action, "power") == 0) {
        if (argc < 4)
            return nvidia_power_show();
        if (strcmp(argv[3], "on") == 0)
            return nvidia_power_set(1);
        if (strcmp(argv[3], "off") == 0)
            return nvidia_power_set(0);
        fprintf(stderr, "Error: Unknown nvidia power argument '%s'\n", argv[3]);
        fprintf(stderr, "Usage: nvidia power [on|off]\n");
        return 1;
    } else if (strcmp(action, "clock") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: Missing argument for nvidia clock\n");
            fprintf(stderr, "Usage: nvidia clock <min>,<max> | reset\n");
            return 1;
        }
        if (strcmp(argv[3], "reset") == 0)
            return nvidia_clock_reset();
        int min, max;
        if (nvidia_parse_clock_range(argv[3], &min, &max) != 0) {
            fprintf(stderr, "Error: Invalid clock range '%s' (expected <min>,<max> with min <= max)\n", argv[3]);
            fprintf(stderr, "Usage: nvidia clock <min>,<max> | reset\n");
            return 1;
        }
        return nvidia_clock_set(min, max);
    } else if (strcmp(action, "memclock") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: Missing argument for nvidia memclock\n");
            fprintf(stderr, "Usage: nvidia memclock <min>,<max> | reset\n");
            return 1;
        }
        if (strcmp(argv[3], "reset") == 0)
            return nvidia_memclock_reset();
        int min, max;
        if (nvidia_parse_clock_range(argv[3], &min, &max) != 0) {
            fprintf(stderr, "Error: Invalid clock range '%s' (expected <min>,<max> with min <= max)\n", argv[3]);
            fprintf(stderr, "Usage: nvidia memclock <min>,<max> | reset\n");
            return 1;
        }
        return nvidia_memclock_set(min, max);
    } else {
        fprintf(stderr, "Error: Unknown nvidia action '%s'\n", action);
        fprintf(stderr, "Usage: %s\n", NVIDIA_USAGE_STR);
        return 1;
    }
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    show_status();
    return 0;
}

static int cmd_rr(int argc, char **argv)
{
    if (argc < 3)
        return rr_list();

    /* Shortcuts: 1 → highest rate, 2 → lowest rate (e.g. 165Hz / 40Hz) */
    const char *rate = argv[2];
    if (strcmp(rate, "1") == 0 || strcmp(rate, "2") == 0) {
        struct display_info info;
        if (query_display_info(&info) < 0 || info.rate_count < 2) {
            fprintf(stderr, "Error: cannot detect available rates\n");
            return -1;
        }
        int idx = (rate[0] == '1') ? 0 : (info.rate_count - 1);
        rate = info.available_rates[idx];
    }

    int rc = rr_set(rate);
    if (rc == 0) printf("Done.\n");
    return rc;
}

/* ========================================================================
 * DISPLAY SCALING (xrandr --scale-from)
 * ======================================================================== */

static int scale_set(const char *resolution)
{
    struct display_info info;
    if (query_display_info(&info) < 0) {
        fprintf(stderr, "Error: failed to query display info\n");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp("xrandr", "xrandr", "--output", info.output, "--mode", info.resolution,
               "--scale-from", resolution, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Error: Failed to set scale to %s\n", resolution);
        return -1;
    }
    printf("  Scale: %s → %s (GPU upscaled)\n", resolution, info.resolution);
    return 0;
}

static int scale_reset(void)
{
    struct display_info info;
    if (query_display_info(&info) < 0) {
        fprintf(stderr, "Error: failed to query display info\n");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp("xrandr", "xrandr", "--output", info.output, "--mode", info.resolution,
               "--scale", "1x1", (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Error: Failed to reset scale\n");
        return -1;
    }
    printf("  Scale: native (1x1)\n");
    return 0;
}

static void scale_show_details(void)
{
    printf("Usage: cctl scale <factor | resolution | off>\n\n");
    printf("GPU-side scaling renders the desktop/games at a lower internal resolution\n"
           "and stretches it up to native panel resolution using hardware GPU scaler,\n"
           "boosting performance or enlarging UI with zero CPU overhead.\n\n");

    struct display_info info;
    if (has_display_support() && query_display_info(&info) == 0 && info.resolution[0]) {
        printf("Current Display:\n");
        printf("  Output: %s  |  Native Resolution: %s  |  Current Rate: %sHz\n\n",
               info.output, info.resolution, info.current_rate);
    }

    printf("Options:\n");
    printf("  <factor>        Fraction between 0.01 and 1.0 (e.g. 0.75 for 75%%, 0.5 for 50%%)\n");
    printf("  <resolution>    Explicit WIDTHxHEIGHT (e.g. 1920x1080, 1600x900, 1280x720)\n");
    printf("  off | reset     Restore native 1:1 display resolution\n\n");

    printf("Examples:\n");
    printf("  cctl scale 0.75         Render at 75%% resolution (1080p equivalent on 1440p panel)\n");
    printf("  cctl scale 1920x1080    Render at explicit 1920x1080 resolution\n");
    printf("  cctl scale 0.5          Render at 50%% resolution (large UI / maximum fps)\n");
    printf("  cctl scale off          Reset back to native display resolution\n");
}

static int cmd_scale(int argc, char **argv)
{
    if (argc < 3) {
        scale_show_details();
        return 0;
    }

    const char *arg = argv[2];
    if (strcmp(arg, "off") == 0 || strcmp(arg, "reset") == 0)
        return scale_reset();

    /* Check if argument is a number (factor) or a resolution string */
    int is_factor = 1;
    for (const char *p = arg; *p; p++) {
        if (*p == 'x') { is_factor = 0; break; }
        if (!isdigit((unsigned char)*p) && *p != '.' && *p != '-') { is_factor = 0; break; }
    }
    /* If purely numeric (possibly with one dot), treat as factor */
    if (is_factor && *arg) {
        double factor = atof(arg);
        if (factor <= 0.0 || factor > 1.0) {
            fprintf(stderr, "Error: Scale factor must be between 0.01 and 1.0\n");
            return 1;
        }
        struct display_info info;
        if (query_display_info(&info) < 0) {
            fprintf(stderr, "Error: failed to query display info\n");
            return -1;
        }
        /* Parse native resolution */
        int w, h;
        if (sscanf(info.resolution, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
            fprintf(stderr, "Error: cannot parse native resolution '%s'\n", info.resolution);
            return -1;
        }
        int sw = (int)(w * factor);
        int sh = (int)(h * factor);
        /* Keep even numbers to avoid xrandr issues */
        if (sw % 2) sw--;
        if (sh % 2) sh--;
        char res[32];
        snprintf(res, sizeof(res), "%dx%d", sw, sh);
        printf("  Factor: %.2f → %s (from %s)\n", factor, res, info.resolution);
        int rc = scale_set(res);
        if (rc == 0) printf("Done.\n");
        return rc;
    }

    /* Resolution string (contains 'x') */
    int rc = scale_set(arg);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_mic(int argc, char **argv)
{
    int rc;
    if (argc >= 3 && strcmp(argv[2], "on") == 0)
        rc = mic_set(1);
    else if (argc >= 3 && strcmp(argv[2], "off") == 0)
        rc = mic_set(0);
    else
        rc = mic_toggle();
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_monitor(int argc, char **argv)
{
    (void)argc; (void)argv;
    return cpumonitor();
}

static int cmd_set(int argc, char **argv)
{
    int with_rapl = (strcmp(argv[1], "setr") == 0);
    int nosafe = 0;
    const char *profile = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--nosafe") == 0)
            nosafe = 1;
        else if (!profile)
            profile = argv[i];
    }

    if (!profile) {
        fprintf(stderr, "Error: Missing profile name\n");
        fprintf(stderr, "Valid profiles: max, cpuperf, balanced, powersave, eco\n");
        return 1;
    }
    int rc = 0;

    if (strcmp(profile, "max") == 0) {
        if (!nosafe) {
            printf("Setting both fans to AUTO (safety default; use --nosafe to bypass)...\n");
            fan_auto_all();
        }
        rc = profile_max(with_rapl);
    } else if (strcmp(profile, "cpuperf") == 0) {
        if (!nosafe) {
            printf("Setting both fans to AUTO (safety default; use --nosafe to bypass)...\n");
            fan_auto_all();
        }
        rc = profile_cpuperf(with_rapl);
    } else if (strcmp(profile, "balanced") == 0) {
        if (!nosafe) {
            printf("Setting both fans to AUTO (safety default; use --nosafe to bypass)...\n");
            fan_auto_all();
        }
        rc = profile_balanced(with_rapl);
    } else if (strcmp(profile, "powersave") == 0) {
        rc = profile_powersave(with_rapl);
    } else if (strcmp(profile, "eco") == 0) {
        rc = profile_eco(with_rapl);
    } else {
        fprintf(stderr, "Error: Unknown profile '%s'\n", profile);
        fprintf(stderr, "Valid profiles: max, cpuperf, balanced, powersave, eco\n");
        return 1;
    }

    ec_release_ports();
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_fan(int argc, char **argv)
{
    int nosafe = 0;
    const char *mode = NULL;
    const char *val_str = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--nosafe") == 0)
            nosafe = 1;
        else if (!mode)
            mode = argv[i];
        else if (!val_str)
            val_str = argv[i];
    }

    if (!mode) {
        fprintf(stderr, "Error: Missing fan mode\n");
        fprintf(stderr, "Valid modes: auto, max, silent, cpu <pct>, gpu <pct>\n");
        return 1;
    }
    int rc = 0;

    if (strcmp(mode, "auto") == 0) {
        printf("Setting both fans to AUTO...\n");
        rc = fan_auto_all();
    } else if (strcmp(mode, "max") == 0) {
        printf("Setting both fans to MAX...\n");
        rc = fan_max_all();
    } else if (strcmp(mode, "silent") == 0) {
        if (!nosafe) {
            printf("Applying: eco profile (safety default for silent fans; use --nosafe to bypass)...\n");
            profile_eco(0);
        }
        printf("Setting both fans to SILENT...\n");
        rc = fan_silent_all();
    } else if (strcmp(mode, "cpu") == 0) {
        if (!val_str) {
            fprintf(stderr, "Error: Missing duty percentage\n");
            return 1;
        }
        int pct;
        if (safe_atoi(val_str, &pct) < 0) {
            fprintf(stderr, "Error: Invalid duty percentage '%s'\n", val_str);
            return 1;
        }
        if (!nosafe) {
            fprintf(stderr, "Error: Manual fan duty requires --nosafe (e.g. cctl fan cpu %d --nosafe)\n", pct);
            return 1;
        }
        rc = fan_set_duty(FAN_CPU, pct);
    } else if (strcmp(mode, "gpu") == 0) {
        if (!val_str) {
            fprintf(stderr, "Error: Missing duty percentage\n");
            return 1;
        }
        int pct;
        if (safe_atoi(val_str, &pct) < 0) {
            fprintf(stderr, "Error: Invalid duty percentage '%s'\n", val_str);
            return 1;
        }
        if (!nosafe) {
            fprintf(stderr, "Error: Manual fan duty requires --nosafe (e.g. cctl fan gpu %d --nosafe)\n", pct);
            return 1;
        }
        rc = fan_set_duty(FAN_GPU, pct);
    } else {
        /* Try as a plain number — apply to both fans */
        int pct;
        if (safe_atoi(mode, &pct) >= 0 && pct >= 21 && pct <= 100) {
            if (!nosafe) {
                fprintf(stderr, "Error: Manual fan duty requires --nosafe (e.g. cctl fan %d --nosafe)\n", pct);
                return 1;
            }
            printf("Setting both fans to %d%%...\n", pct);
            if (fan_set_duty(FAN_CPU, pct) < 0) rc = -1;
            if (fan_set_duty(FAN_GPU, pct) < 0) rc = -1;
        } else {
            fprintf(stderr, "Error: Unknown fan mode '%s'\n", mode);
            fprintf(stderr, "Valid modes: auto, max, silent, cpu <pct> --nosafe, gpu <pct> --nosafe, or <pct> --nosafe\n");
            return 1;
        }
    }

    ec_release_ports();
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_turbo(int argc, char **argv)
{
    int nosafe = 0;
    const char *action = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--nosafe") == 0)
            nosafe = 1;
        else if (!action)
            action = argv[i];
    }

    if (!action) {
        fprintf(stderr, "Error: Missing turbo action (on/off)\n");
        return 1;
    }
    int enabled;
    if (strcmp(action, "on") == 0)
        enabled = 1;
    else if (strcmp(action, "off") == 0)
        enabled = 0;
    else {
        fprintf(stderr, "Error: Invalid turbo action '%s' (use on or off)\n", action);
        return 1;
    }

    if (enabled && !nosafe) {
        printf("Setting both fans to AUTO (safety default for turbo; use --nosafe to bypass)...\n");
        fan_auto_all();
    }

    int rc = set_turbo(enabled);
    ec_release_ports();
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_fnlock(int argc, char **argv)
{
    int rc;
    if (argc >= 3) {
        if (strcmp(argv[2], "lock") == 0 || strcmp(argv[2], "on") == 0)
            rc = set_fnlock(1);
        else if (strcmp(argv[2], "unlock") == 0 || strcmp(argv[2], "off") == 0)
            rc = set_fnlock(0);
        else if (strcmp(argv[2], "toggle") == 0)
            rc = fnlock_toggle();
        else {
            fprintf(stderr, "Error: Invalid fn action '%s' (use lock, unlock, on, off, or omit arg to toggle)\n", argv[2]);
            return 1;
        }
    } else {
        rc = fnlock_toggle();
    }
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_gov(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Error: Missing governor (powersave/performance)\n");
        return 1;
    }
    const char *gov = argv[2];
    if (strcmp(gov, "powersave") != 0 && strcmp(gov, "performance") != 0) {
        fprintf(stderr, "Error: Invalid governor '%s' (use powersave or performance)\n", gov);
        return 1;
    }
    int rc = set_governor(gov);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_epp(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Error: Missing EPP value\n");
        fprintf(stderr, "Valid values: performance, balance_performance, balance_power, power\n");
        return 1;
    }
    const char *epp = argv[2];
    if (strcmp(epp, "performance") != 0 && strcmp(epp, "balance_performance") != 0 &&
        strcmp(epp, "balance_power") != 0 && strcmp(epp, "power") != 0) {
        fprintf(stderr, "Error: Invalid EPP '%s'\n", epp);
        fprintf(stderr, "Valid values: performance, balance_performance, balance_power, power\n");
        return 1;
    }
    int rc = set_epp(epp);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_rapl(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Error: Usage: rapl <pl1_watts> <pl2_watts>\n");
        fprintf(stderr, "       rapl skip <pl2>      (set PL2 only)\n");
        fprintf(stderr, "       rapl <pl1> skip      (set PL1 only)\n");
        return 1;
    }

    int pl1 = 0, pl2 = 0;
    int skip_pl1 = 0, skip_pl2 = 0;

    /* Parse PL1 */
    if (strcmp(argv[2], "skip") == 0) {
        skip_pl1 = 1;
    } else {
        if (safe_atoi(argv[2], &pl1) < 0 || pl1 < 1) {
            fprintf(stderr, "Error: Invalid PL1 value '%s' (use a wattage ≥ 1 or 'skip')\n", argv[2]);
            return 1;
        }
    }

    /* Parse PL2 */
    if (argc >= 4) {
        if (strcmp(argv[3], "skip") == 0) {
            skip_pl2 = 1;
        } else {
            if (safe_atoi(argv[3], &pl2) < 0 || pl2 < 1) {
                fprintf(stderr, "Error: Invalid PL2 value '%s' (use a wattage ≥ 1 or 'skip')\n", argv[3]);
                return 1;
            }
        }
    } else {
        /* Only one arg given: interpret as PL2, skip PL1 */
        skip_pl1 = 1;
        pl2 = pl1;
        pl1 = 0;
        if (pl2 < 1) {
            fprintf(stderr, "Error: Power limit must be >= 1 watt\n");
            return 1;
        }
    }

    /* Must set at least one limit */
    if (skip_pl1 && skip_pl2) {
        fprintf(stderr, "Error: Nothing to set — both PL1 and PL2 are skipped\n");
        return 1;
    }

    /* Pass ≤ 0 to skip (set_rapl_limits treats pl1_w ≤ 0 as skip) */
    int rc = set_rapl_limits(skip_pl1 ? -1 : pl1, skip_pl2 ? -1 : pl2);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_kbc(int argc, char **argv)
{
    if (argc < 3) {
        kbd_show_presets();
        return 0;
    }

    if (geteuid() != 0)
        self_elevate(argc, argv);
    kbe_stop(1);
    /* If 3 numeric args → RGB mode */
    if (argc >= 5) {
        int r, g, b;
        if (safe_atoi(argv[2], &r) == 0 && safe_atoi(argv[3], &g) == 0 && safe_atoi(argv[4], &b) == 0) {
            int rc = kbd_set_color(r, g, b);
            if (rc == 0) printf("Done.\n");
            return rc;
        }
    }
    /* Single arg → hex or preset */
    int rc = kbd_set_preset(argv[2]);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_kbb(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Error: Usage: kbb <0-100>\n");
        return 1;
    }
    int pct;
    if (safe_atoi(argv[2], &pct) < 0) {
        fprintf(stderr, "Error: Invalid brightness value '%s'\n", argv[2]);
        return 1;
    }
    kbe_stop(1);
    int rc = kbd_set_brightness(pct);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_kbe(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[2], "status") == 0) {
        pid_t pid = 0;
        char effect[32] = {0};
        int orig_r = 255, orig_g = 255, orig_b = 255, orig_bri = 255;
        if (kbe_is_running(&pid, effect, sizeof(effect), &orig_r, &orig_g, &orig_b, &orig_bri)) {
            printf("Keyboard Backlight Effect:\n");
            printf("  Status:              %s%s%s (active, PID %d)\n", C_GRN, effect, C_RST, (int)pid);
            printf("  Original Color:      RGB(%d, %d, %d)\n", orig_r, orig_g, orig_b);
            int bri_pct = (orig_bri * 100 + 127) / 255;
            printf("  Original Brightness: %d%% (raw %d)\n\n", bri_pct, orig_bri);
            printf("To stop effect:        cctl kbe stop\n");
        } else {
            printf("Keyboard Backlight Effect:\n");
            printf("  Status:              %snone%s (stopped)\n\n", C_DIM, C_RST);
            printf("Available Effects:\n");
            printf("  %-16s %s\n", "breathe", "Smooth fade in/out (uses current color)");
            printf("  %-16s %s\n", "breathe-cycle", "Smooth breathe shifting through colors");
            printf("  %-16s %s\n", "cycle", "Smooth continuous rainbow cycle");
            printf("  %-16s %s\n", "flash", "Strobe flash bursts (uses current color)");
            printf("  %-16s %s\n", "flash-cycle", "Strobe flash bursts cycling colors");
            printf("  %-16s %s\n", "candle", "Realistic flickering candle flame");
            printf("  %-16s %s\n\n", "pulse", "Heartbeat double-pulse (uses current color)");
            printf("Usage: cctl kbe <effect>  |  cctl kbe stop\n");
        }
        return 0;
    }

    if (geteuid() != 0) {
        self_elevate(argc, argv);
    }

    const char *sub = argv[2];
    if (strcmp(sub, "stop") == 0 || strcmp(sub, "off") == 0) {
        return kbe_stop(0);
    }

    if (strcmp(sub, "breathe") != 0 && strcmp(sub, "breath") != 0 &&
        strcmp(sub, "breathe-cycle") != 0 && strcmp(sub, "breathe+colorchange") != 0 &&
        strcmp(sub, "breathe_cycle") != 0 && strcmp(sub, "breathecycle") != 0 &&
        strcmp(sub, "cycle") != 0 && strcmp(sub, "rainbow") != 0 &&
        strcmp(sub, "spectrum") != 0 && strcmp(sub, "slow-cycle") != 0 &&
        strcmp(sub, "slow_colorchanging") != 0 &&
        strcmp(sub, "flash") != 0 && strcmp(sub, "strobe") != 0 &&
        strcmp(sub, "flash-cycle") != 0 && strcmp(sub, "flash+colorchange") != 0 &&
        strcmp(sub, "flash_cycle") != 0 && strcmp(sub, "flashcycle") != 0 &&
        strcmp(sub, "candle") != 0 && strcmp(sub, "flicker") != 0 &&
        strcmp(sub, "pulse") != 0 && strcmp(sub, "heartbeat") != 0) {
        fprintf(stderr, "Error: Unknown keyboard effect '%s'\n", sub);
        fprintf(stderr, "Available effects: breathe, breathe-cycle, cycle, flash, flash-cycle, candle, pulse\n");
        return 1;
    }

    return kbe_start(sub);
}

static int cmd_webcam(int argc, char **argv)
{
    int rc;
    if (argc >= 3 && strcmp(argv[2], "on") == 0)
        rc = webcam_set(1);
    else if (argc >= 3 && strcmp(argv[2], "off") == 0)
        rc = webcam_set(0);
    else
        rc = webcam_toggle();
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_bat(int argc, char **argv)
{
    if (argc < 3) {
        /* Show current thresholds */
        int start = bat_read_start();
        int end = bat_read_end();
        if (start < 0 || end < 0) {
            fprintf(stderr, "Error: Cannot read battery thresholds\n");
            return 1;
        }
        printf("  Charge thresholds: ");
        printf("start %d%% → stop %d%%\n", start, end);

        /* Show available values */
        int avail[16], cnt;
        cnt = read_avail_thresholds(BAT_START_AVAIL_PATH, avail, 16);
        if (cnt > 0) {
            printf("  Available start values: ");
            for (int i = 0; i < cnt; i++) {
                printf("%s%d%s%s", avail[i] == start ? C_GRN : "", avail[i], C_RST,
                       i < cnt - 1 ? " " : "\n");
            }
        }
        cnt = read_avail_thresholds(BAT_END_AVAIL_PATH, avail, 16);
        if (cnt > 0) {
            printf("  Available stop values:  ");
            for (int i = 0; i < cnt; i++) {
                printf("%s%d%s%s", avail[i] == end ? C_GRN : "", avail[i], C_RST,
                       i < cnt - 1 ? " " : "\n");
            }
        }
        return 0;
    }

    /* "max", "off", or "default" → widest range (charge to max) */
    if (strcmp(argv[2], "max") == 0 || strcmp(argv[2], "off") == 0 || strcmp(argv[2], "default") == 0) {
        if (geteuid() != 0)
            self_elevate(argc, argv);
        int rc = bat_set(0, 0);
        if (rc == 0) printf("Done.\n");
        return rc;
    }

    /* set <start> <end> */
    if (argc < 4) {
        fprintf(stderr, "Error: Usage: bat <start> <end> or bat max\n");
        return 1;
    }
    if (geteuid() != 0)
        self_elevate(argc, argv);
    int start, end;
    if (safe_atoi(argv[2], &start) < 0 || safe_atoi(argv[3], &end) < 0) {
        fprintf(stderr, "Error: Invalid threshold values\n");
        return 1;
    }
    int rc = bat_set(start, end);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int get_installed_microversion(void)
{
    if (access("/usr/local/bin/cctl", X_OK) != 0)
        return -1; /* Not installed */

    FILE *fp = popen("/usr/local/bin/cctl --microversion 2>/dev/null", "r");
    if (!fp) return 0;

    char buf[32];
    int ver = 0;
    if (fgets(buf, sizeof(buf), fp)) {
        safe_atoi(buf, &ver);
    }
    pclose(fp);
    return ver;
}

static int cmd_install(int argc, char **argv)
{
    int force = (argc >= 3 && strcmp(argv[2], "--force") == 0);
    if (argc > 3 || (argc == 3 && !force)) {
        fprintf(stderr, "Error: 'cctl install' does not accept file arguments.\n");
        return 1;
    }

    /* main() already verified EUID==0 (needs_root). */
    char src[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", src, sizeof(src) - 1);
    if (n <= 0) {
        perror("Error: cannot determine running binary path");
        return 1;
    }
    src[n] = '\0';

    /* If running from /usr/local/bin/cctl directly, it's already installed */
    if (strcmp(src, "/usr/local/bin/cctl") == 0) {
        printf("cctl is already installed at /usr/local/bin/cctl\n");
        return 0;
    }

    /* Check microversion of currently installed binary */
    int installed_ver = get_installed_microversion();
    if (!force && installed_ver >= CCTL_MICROVERSION) {
        printf("cctl is already up to date at /usr/local/bin/cctl\n");
        return 0;
    }

    if (geteuid() != 0)
        self_elevate(argc, argv);

    if (installed_ver > 0)
        printf("Upgrading cctl...\n");
    else
        printf("Installing cctl...\n");

    /* 1. Copy binary to /usr/local/bin/cctl with mode 0755 */
    char *const args[] = { "install", "-m", "755", src, "/usr/local/bin/cctl", NULL };
    if (run_cmd("install", args) != 0) {
        fprintf(stderr, "Error: failed to install binary to /usr/local/bin/cctl\n");
        return 1;
    }
    printf("Installed: /usr/local/bin/cctl (from %s)\n", src);

    /* 2. Determine the human user (prefer the sudo invoker) */
    const char *user = getenv("SUDO_USER");
    if (!user || !*user) user = getenv("USER");
    if (!user || !*user) {
        struct passwd *pw = getpwuid(getuid());
        user = (pw && pw->pw_name) ? pw->pw_name : "root";
    }

    /* 3. Write sudoers.d/cctl granting passwordless sudo */
    const char *sudoers = "/etc/sudoers.d/cctl";
    FILE *fp = fopen(sudoers, "w");
    if (!fp) {
        perror("Error: cannot open sudoers file for writing");
        return 1;
    }
    fprintf(fp, "%s ALL=(ALL) NOPASSWD: /usr/local/bin/cctl\n", user);
    fclose(fp);
    chmod(sudoers, 0440);
    printf("Sudoers:  %s (passwordless sudo for %s)\n", sudoers, user);

    printf("\nDone. cctl is installed to /usr/local/bin/cctl and configured with passwordless sudo.\n");
    printf("Privileged commands will auto-elevate seamlessly without needing any shell alias.\n");
    return 0;
}

static int cmd_drivers_install(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Resolve the user's home (honor SUDO_USER when run via sudo) */
    const char *home = getenv("HOME");
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user && *sudo_user) {
        struct passwd *pw = getpwnam(sudo_user);
        if (pw && pw->pw_dir) home = pw->pw_dir;
    }
    if (!home || !*home) home = ".";

    /* Find cctl-drivers.tar.gz somewhere under the user's home */
    char find_cmd[PATH_MAX + 64];
    snprintf(find_cmd, sizeof(find_cmd),
             "find '%s' -name cctl-drivers.tar.gz 2>/dev/null", home);

    FILE *fp = popen(find_cmd, "r");
    if (!fp) {
        fprintf(stderr, "Error: failed to search for drivers archive\n");
        return 1;
    }
    char archive[PATH_MAX] = {0};
    if (fgets(archive, sizeof(archive), fp))
        archive[strcspn(archive, "\n")] = '\0';
    pclose(fp);

    if (archive[0] == '\0') {
        fprintf(stderr, "cctl-drivers.tar.gz not found under %s.\n", home);
        for (;;) {
            char path[PATH_MAX];
            printf("Enter the full path to cctl-drivers.tar.gz (or press Enter to abort): ");
            if (!fgets(path, sizeof(path), stdin) || path[0] == '\0') {
                archive[0] = '\0';
                break;
            }
            path[strcspn(path, "\n")] = '\0';
            if (access(path, F_OK) == 0) {
                snprintf(archive, sizeof(archive), "%s", path);
                break;
            }
            fprintf(stderr, "  No such file: %s\n", path);
        }
        if (archive[0] == '\0') {
            printf("Aborted.\n");
            return 0;
        }
    }

    printf("Drivers archive found: %s\n", archive);

    char ans[16];
    printf("Shall I install it? [y/N] ");
    if (!fgets(ans, sizeof(ans), stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
        printf("Aborted.\n");
        return 0;
    }

    /* Extract to a temp dir */
    char tmpl[] = "/tmp/cctl-drivers.XXXXXX";
    char *tmpdir = mkdtemp(tmpl);
    if (!tmpdir) {
        perror("Error: cannot create temp directory");
        return 1;
    }

    char tar_cmd[PATH_MAX + 64];
    snprintf(tar_cmd, sizeof(tar_cmd), "tar -xzf '%s' -C '%s'", archive, tmpdir);
    if (system(tar_cmd) != 0) {
        fprintf(stderr, "Error: failed to extract archive\n");
        char rm_cmd[PATH_MAX + 64];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmpdir);
        run_quiet(rm_cmd);
        return 1;
    }

    /* Locate the extracted installer script */
    char script[PATH_MAX];
    snprintf(script, sizeof(script), "%s/drivers/driverinstall.sh", tmpdir);
    if (access(script, F_OK) != 0) {
        fprintf(stderr, "Error: driverinstall.sh not found in extracted archive\n");
        char rm_cmd[PATH_MAX + 64];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmpdir);
        run_quiet(rm_cmd);
        return 1;
    }

    printf("Launching interactive driver installer...\n");
    char launch[PATH_MAX + 64];
    if (geteuid() == 0)
        snprintf(launch, sizeof(launch), "bash '%s'", script);
    else
        snprintf(launch, sizeof(launch), "sudo bash '%s'", script);

    int rc = system(launch);

    /* Clean up the temp dir */
    char rm_cmd[PATH_MAX + 64];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmpdir);
    run_quiet(rm_cmd);

    if (rc != 0) {
        fprintf(stderr, "Driver installer exited with an error.\n");
        return 1;
    }
    return 0;
}

static int cmd_mux(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[2], "switch") == 0) {
        if (geteuid() != 0)
            self_elevate(argc, argv);
        return mux_switch();
    }

    /* 'cctl mux' with no subcommand — show current mode + pending */
    int nvram = mux_read();
    if (nvram < 0) {
        fprintf(stderr, "GPU MUX: N/A (NVRAM variable not found or unrecognized)\n");
        return 1;
    }
    int running = mux_running_mode();
    const char *col = (running == MUX_VAL_MSHYBRID) ? C_GRN : C_MAG;
    if (running >= 0 && nvram != running)
        printf("GPU MUX: %s%s%s  %s← %s pending (reboot to apply)%s\n",
               col, mux_mode_str(running), C_RST,
               C_YLW, mux_mode_str(nvram), C_RST);
    else
        printf("GPU MUX: %s%s%s\n", col, mux_mode_str(nvram), C_RST);
    return 0;
}

struct command {
    const char *name;
    int needs_root;
    int (*handler)(int argc, char **argv);
};

static const struct command commands[] = {
    { "status",  1, cmd_status },
    { "rr",      0, cmd_rr },
    { "scale",   0, cmd_scale },
    { "mic",     0, cmd_mic },
    { "monitor", 1, cmd_monitor },
    { "set",     1, cmd_set },
    { "setr",    1, cmd_set },
    { "fan",     1, cmd_fan },
    { "turbo",   1, cmd_turbo },
    { "fn",      1, cmd_fnlock },
    { "gov",     1, cmd_gov },
    { "epp",     1, cmd_epp },
    { "rapl",    1, cmd_rapl },
    { "kbc",     0, cmd_kbc },
    { "kbb",     1, cmd_kbb },
    { "kbe",     0, cmd_kbe },
    { "webcam",  1, cmd_webcam },
    { "bat",     0, cmd_bat },     /* root required for set, checked in handler */
    { "nvidia",  0, cmd_nvidia },
    { "install", 0, cmd_install },
    { "mux",     0, cmd_mux },      /* root required for switch, checked in handler */
    { "drivers-install", 0, cmd_drivers_install },

};

int main(int argc, char **argv)
{
    use_color = isatty(STDOUT_FILENO);
    init_colors();

    /* Pin to E-cores to keep off P-cores if hybrid architecture is detected */
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        int pinned_count = 0;
        int max_cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
        if (max_cpus <= 0) max_cpus = 64;

        for (int i = 0; i < max_cpus; i++) {
            if (i >= CPU_SETSIZE) continue; /* cannot represent in cpu_set_t */
            if (is_cpu_e_core(i)) {
                CPU_SET((unsigned)i, &cpuset);
                pinned_count++;
                if (pinned_count >= 2) break; // pin to up to 2 E-cores
            }
        }
        if (pinned_count > 0) {
            sched_setaffinity(0, sizeof(cpuset), &cpuset);
        }
    }
    atexit(ec_release_ports);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Normalize the command verb to lowercase so any case works
     * (e.g. setR, SetR and setr are all treated as the RAPL variant). */
    for (char *p = argv[1]; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--microversion") == 0) {
        printf("%d\n", CCTL_MICROVERSION);
        return 0;
    }

    int cmd_found = 0;
    int rc = 0;
    size_t num_cmds = sizeof(commands) / sizeof(commands[0]);
    for (size_t i = 0; i < num_cmds; i++) {
        if (strcmp(commands[i].name, argv[1]) == 0) {
            cmd_found = 1;
            if (commands[i].needs_root && geteuid() != 0) {
                self_elevate(argc, argv);
            }
            rc = commands[i].handler(argc, argv);
            break;
        }
    }

    if (!cmd_found) {
        fprintf(stderr, "Error: Unknown command '%s'\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    return rc;
}
