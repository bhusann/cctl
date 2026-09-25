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

#define CCTL_VERSION      "3.6"
/* NOTE FOR DEVELOPERS / AI AGENTS:
 * Always increment CCTL_MICROVERSION (a 6-digit integer) whenever making code
 * changes and committing. 'cctl install' checks this hidden value to determine
 * if a local binary is newer than /usr/local/bin/cctl. Do NOT document this in
 * README or help menus. */
#ifndef CCTL_MICROVERSION
#define CCTL_MICROVERSION 100024
#endif

/* Preprocessor stringification for embedding integer defines as strings */
#define CCTL_XSTR(x) #x
#define CCTL_STR(x)  CCTL_XSTR(x)

/* Extractable metadata markers — embedded as string literals in the compiled
 * binary so `cctl update` can read version/hash info from a downloaded
 * release WITHOUT executing it.  The @@…=…@@ delimiters are chosen to be
 * vanishingly unlikely in compiled code.
 *
 * Security fix: the prior code ran the downloaded binary as root via
 * popen("downloaded_binary --microversion") to query its version, granting
 * arbitrary code execution to whatever the release asset contained.
 * Marker extraction reads the file as data instead.
 *
 * __attribute__((used)) prevents the compiler from dead-stripping the
 * variables even though no code references them directly; string data
 * lives in .rodata and survives `strip -s`. */
static const char __attribute__((used)) cctl_meta_version[] =
    "@@CCTL_META_VERSION=" CCTL_VERSION "@@";
static const char __attribute__((used)) cctl_meta_microver[] =
    "@@CCTL_META_MICROVER=" CCTL_STR(CCTL_MICROVERSION) "@@";

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

static int read_cpu_temp_ex(int cached_fd);
#define read_cpu_temp() read_cpu_temp_ex(-1)
static int read_fan_telemetry_ex(int *cpu_pct, int *gpu_pct, int *cpu_rpm, int *gpu_rpm, int cached_fd);
#define read_fan_telemetry(c, g, cr, gr) read_fan_telemetry_ex(c, g, cr, gr, -1)
static int is_cpu_e_core(int cpu_num);
#ifdef CCTL_NVIDIA
static int nvidia_is_blacklisted(void);
static int nvidia_is_loaded(void);
#endif
static int bat_read_start(void);
static int bat_read_end(void);
static int command_exists(const char *cmd);

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
    if (errno != 0 || end == str || val < INT_MIN || val > INT_MAX)
        return -1;
    while (*end == '\n' || *end == '\r') end++; /* fgets keeps the newline */
    if (*end != '\0')
        return -1;
    *out = (int)val;
    return 0;
}

/* ========================================================================
 * ACTIVE PROFILE MODE (/run/cctl/mode)
 * ========================================================================
 * `cctl set` / `cctl setR` record the active profile (line 1) and how it
 * was applied (line 2: set | setR). `cctl status` displays it, and the
 * RAPL PL1 ceiling consults it (90W unlocked only while mode == max,
 * matching the EC max profile's power budget). GPU watts are deliberately
 * NEVER read for this — the mode file is the single source of truth.
 * /run is tmpfs, cleared on reboot, so a missing file simply means
 * "EC default" (no profile applied since boot).
 *
 * /run/cctl is root-owned (0700) — unlike /tmp, unprivileged users cannot
 * plant a fake mode file to manipulate the PL1 ceiling.  Both read and
 * write paths use O_NOFOLLOW to refuse symlinks. */
#ifndef MODE_FILE
#define MODE_FILE "/run/cctl/mode"
#endif
#define MODE_DIR  "/run/cctl"

static int mode_write(const char *profile, const char *method)
{
    if (mkdir(MODE_DIR, 0700) != 0 && errno != EEXIST)
        return -1;
    unlink(MODE_FILE);
    int fd = open(MODE_FILE, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s\n%s\n", profile, method);
    ssize_t w = write(fd, buf, (size_t)len);
    close(fd);
    if (w != len) { unlink(MODE_FILE); return -1; }
    return 0;
}

/* Returns 0 and fills profile/method when a valid mode file exists,
 * -1 otherwise (no file / empty / corrupt). */
static int mode_read(char *profile, size_t psz, char *method, size_t msz)
{
    if (psz == 0 || msz == 0) return -1;
    profile[0] = '\0';
    method[0] = '\0';
    int fd = open(MODE_FILE, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return -1; }
    char line[64];
    if (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        snprintf(profile, psz, "%.*s", (int)psz - 1, line);
    }
    if (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        snprintf(method, msz, "%.*s", (int)msz - 1, line);
    }
    fclose(fp); /* also closes fd */
    return profile[0] ? 0 : -1;
}

/* RAPL PL1 ceiling in watts: 45 normally; 90 only while the recorded
 * active mode is "max" (EC max profile = 90/115W CPU + 100W GPU budget). */
static int rapl_pl1_ceiling(void)
{
    char profile[32] = {0}, method[16] = {0};
    if (mode_read(profile, sizeof(profile), method, sizeof(method)) == 0 &&
        strcmp(profile, "max") == 0)
        return 90;
    return 45;
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

/* AC adapter / battery state for the NVRAM-write power guard.
 * Returns 1 = external power connected, 0 = running on battery,
 * -1 = no mains device found (unknown → guard stays off). */
static int ac_online(void)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return -1;
    struct dirent *ent;
    char path[512], type[32];
    int found = -1;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", ent->d_name);
        if (read_sysfs_str(path, type, sizeof(type)) < 0) continue;
        if (strncmp(type, "Mains", 5) != 0) continue;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", ent->d_name);
        found = (read_sysfs_long(path, -1) > 0) ? 1 : 0;
        if (found == 1) break; /* multiple mains devices: any one online wins */
    }
    closedir(d);
    return found;
}

/* Auto-detect battery device name (BAT0, BAT1, etc.) by scanning
 * /sys/class/power_supply/ for the first entry with type=Battery.
 * Result is cached after first call.  Falls back to "BAT0". */
static char bat_name[32] = "";

static const char *bat_detect(void)
{
    if (bat_name[0]) return bat_name;
    DIR *d = opendir("/sys/class/power_supply");
    if (d) {
        struct dirent *ent;
        char path[512], type[32];
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", ent->d_name);
            if (read_sysfs_str(path, type, sizeof(type)) < 0) continue;
            if (strcmp(type, "Battery") == 0) {
                snprintf(bat_name, sizeof(bat_name), "%.31s", ent->d_name);
                closedir(d);
                return bat_name;
            }
        }
        closedir(d);
    }
    snprintf(bat_name, sizeof(bat_name), "BAT0");
    return bat_name;
}

/* Build a sysfs path for the detected battery device. */
static void bat_sysfs(char *out, size_t sz, const char *suffix)
{
    snprintf(out, sz, "/sys/class/power_supply/%s/%s", bat_detect(), suffix);
}

/* Battery charge percent, or -1 when no battery is present/readable. */
static int battery_pct(void)
{
    char path[256];
    bat_sysfs(path, sizeof(path), "capacity");
    long v = read_sysfs_long(path, -1);
    return (v >= 0 && v <= 100) ? (int)v : -1;
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

    /* Power guard: committing a 1204-byte NVRAM variable while the battery
     * is nearly empty with no AC risks losing power mid-write and leaving
     * the Setup variable corrupted. (Deliberately a C-comment-only check —
     * not documented in the README, per project decision.) */
    if (ac_online() == 0) {
        int pct = battery_pct();
        if (pct >= 0 && pct < 10) {
            fprintf(stderr,
                    "Error: battery is less than 10%% and AC is not connected.\n"
                    "       Connect AC to use this.\n");
            return 1;
        }
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

/* RAPL ceilings (watts): PL2 hard max 115W (OEM platform limit); PL1 hard
 * max 45W — raised to 90W ONLY while mode == max (see rapl_pl1_ceiling(),
 * fed by /tmp/cctl.mode; GPU wattage is never read for this). */
#define RAPL_PL1_MAX_WATTS      45
#define RAPL_PL1_MAX_WATTS_MAX  90
#define RAPL_PL2_MAX_WATTS     115

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
    /* Defense in depth: enforce the same ceilings as cmd_rapl even if a
     * caller bypasses it (negative still means "skip"). */
    int pl1_cap = rapl_pl1_ceiling();
    if (pl1_w > pl1_cap || pl2_w > RAPL_PL2_MAX_WATTS) {
        fprintf(stderr, "Error: RAPL out of range (PL1 max %dW%s, PL2 max %dW)\n",
                pl1_cap, pl1_cap > RAPL_PL1_MAX_WATTS ? " in max mode" : "",
                RAPL_PL2_MAX_WATTS);
        return -1;
    }

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

    /* Readback verification: detect locked RAPL limits that silently
     * reject writes.  A mismatch means firmware has the MSR locked. */
    int err = 0;
    int actual_pl1 = -1, actual_pl2 = -1;
    read_rapl_current(&actual_pl1, &actual_pl2);
    if (pl1_w > 0 && actual_pl1 >= 0 && actual_pl1 != pl1_w) {
        fprintf(stderr, "  Warning: PL1 readback %dW != requested %dW (RAPL may be firmware-locked)\n",
                actual_pl1, pl1_w);
        err = -1;
    }
    if (pl2_w > 0 && actual_pl2 >= 0 && actual_pl2 != pl2_w) {
        fprintf(stderr, "  Warning: PL2 readback %dW != requested %dW (RAPL may be firmware-locked)\n",
                actual_pl2, pl2_w);
        err = -1;
    }

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
    return err;
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
    int err = 0;
    printf("Applying: Performance Max + GPU (80W-100W)\n");
    set_gpu_profile(2); /* non-fatal: CPU settings still apply without tuxedo_io */
    if (set_turbo(1) < 0) err++;
    if (set_governor("performance") < 0) err++;
    if (set_epp("performance") < 0) err++;
    if (with_rapl && set_rapl_limits(45, 90) < 0) err++;
    return err ? -1 : 0;
}

static int profile_cpuperf(int with_rapl)
{
    (void)with_rapl;
    int err = 0;
    printf("Applying: Performance CPU Only\n");
    set_gpu_profile(3);
    if (set_turbo(1) < 0) err++;
    if (set_governor("performance") < 0) err++;
    if (set_epp("performance") < 0) err++;
    return err ? -1 : 0;
}

static int profile_balanced(int with_rapl)
{
    int err = 0;
    printf("Applying: Balanced\n");
    set_gpu_profile(3);
    if (set_turbo(1) < 0) err++;
    if (set_governor("powersave") < 0) err++;
    if (set_epp("balance_performance") < 0) err++;
    if (with_rapl && set_rapl_limits(35, 40) < 0) err++;
    return err ? -1 : 0;
}

static int profile_powersave(int with_rapl)
{
    (void)with_rapl;
    int err = 0;
    printf("Applying: Powersave\n");
    set_gpu_profile(1);
    if (set_turbo(0) < 0) err++;
    if (set_governor("powersave") < 0) err++;
    if (set_epp("balance_power") < 0) err++;
    return err ? -1 : 0;
}

static int profile_eco(int with_rapl)
{
    int err = 0;
    printf("Applying: Ultra Powersave\n");
    set_gpu_profile(0);
    if (set_turbo(0) < 0) err++;
    if (set_governor("powersave") < 0) err++;
    if (set_epp("power") < 0) err++;
    if (with_rapl && set_rapl_limits(9, 10) < 0) err++;
    return err ? -1 : 0;
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

/* Enumerate a USB device's interface names (contain ':'), e.g. "1-8:1.0".
 * Stores up to max entries into ifnames. Returns count, or -1 if the
 * device directory is unreadable. */
static int webcam_list_interfaces(const char *dev_id, char ifnames[][256], int max)
{
    char dirpath[512];
    snprintf(dirpath, sizeof(dirpath), "/sys/bus/usb/devices/%s", dev_id);
    DIR *d = opendir(dirpath);
    if (!d) return -1;

    int cnt = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && cnt < max) {
        if (!strchr(ent->d_name, ':'))
            continue; /* regular attributes, not interfaces */
        snprintf(ifnames[cnt], sizeof(ifnames[cnt]), "%s", ent->d_name);
        cnt++;
    }
    closedir(d);
    return cnt;
}

/* 1 if this interface is currently claimed by driver `drv` (e.g. "uvcvideo"). */
static int iface_driver_is(const char *dev_id, const char *ifname, const char *drv)
{
    char link[540], target[512];
    snprintf(link, sizeof(link), "/sys/bus/usb/devices/%s/%s/driver",
             dev_id, ifname);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) return 0;
    target[n] = '\0';
    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;
    return strcmp(base, drv) == 0;
}

static int is_webcam_enabled(void)
{
    /* Try tuxedo_io first */
    int val = webcam_read_tuxedo();
    if (val >= 0)
        return val;

    /* Fallback: "on" = at least one camera interface claimed by uvcvideo.
     * The device-level driver link exists either way and says nothing about
     * whether the camera is actually usable. */
    char *usb_id = find_webcam_usb_id();
    if (!usb_id) return -1;

    char ifnames[8][256];
    int cnt = webcam_list_interfaces(usb_id, ifnames, 8);
    int enabled = 0;
    for (int i = 0; cnt > 0 && i < cnt; i++) {
        if (iface_driver_is(usb_id, ifnames[i], "uvcvideo")) {
            enabled = 1;
            break;
        }
    }
    free(usb_id);
    return (cnt < 0) ? -1 : enabled;
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

    /* Fallback: unbind/bind the camera's INTERFACES on uvcvideo.
     * Writing the device name to drivers/usb/{bind,unbind} was a silent
     * no-op: that is the generic USB *device* driver — the interfaces stay
     * claimed by uvcvideo and the camera keeps streaming either way.
     * The disable switch is unbinding e.g. "1-8:1.0" from uvcvideo. */
    char *usb_id = find_webcam_usb_id();
    if (!usb_id) {
        fprintf(stderr, "Error: No webcam USB device found\n");
        return -1;
    }

    char ifnames[8][256];
    int cnt = webcam_list_interfaces(usb_id, ifnames, 8);
    if (cnt <= 0) {
        fprintf(stderr, "Error: Webcam USB device exposes no interfaces\n");
        free(usb_id);
        return -1;
    }

    int bound[8] = {0};
    int currently_enabled = 0;
    for (int i = 0; i < cnt; i++) {
        bound[i] = iface_driver_is(usb_id, ifnames[i], "uvcvideo");
        if (bound[i]) currently_enabled = 1;
    }
    free(usb_id);

    if (enabled == currently_enabled) {
        printf("  Webcam: already %s\n", enabled ? "ON" : "OFF");
        return 0;
    }

    char op_path[160];
    snprintf(op_path, sizeof(op_path), "/sys/bus/usb/drivers/uvcvideo/%s",
             enabled ? "bind" : "unbind");
    int fd = open(op_path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to open %s: %s\n", op_path, strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr, "       (uvcvideo kernel module not loaded?)\n");
        return -1;
    }

    int attempted = 0, done = 0, last_err = 0;
    for (int i = 0; i < cnt; i++) {
        if (enabled == bound[i]) continue; /* already in the wanted state */
        attempted++;
        if (write(fd, ifnames[i], strlen(ifnames[i])) < 0)
            last_err = errno;
        else
            done++;
    }
    close(fd);

    if (attempted > 0 && done == 0) {
        fprintf(stderr, "Error: Failed to %s webcam: %s\n",
                enabled ? "bind" : "unbind", strerror(last_err));
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

/* Returns 1 = capture on, 0 = off, -1 = unknown (amixer missing or failed).
 * Never guesses: reporting a disabled/unknown mic as ON in `cctl status`
 * would silently mislead the privacy use-case. */
static int mic_is_enabled(void)
{
    if (!command_exists("amixer")) return -1;

    int card = mic_find_card();
    char cmd[128];
    if (card >= 0)
        snprintf(cmd, sizeof(cmd), "amixer -c %d sget Capture 2>/dev/null", card);
    else
        snprintf(cmd, sizeof(cmd), "amixer sget Capture 2>/dev/null");
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    char line[256];
    int enabled = -1;
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
    if (!command_exists("amixer")) {
        fprintf(stderr, "Error: amixer not found (install alsa-utils)\n");
        return -1;
    }
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
    int current = mic_is_enabled();
    if (current < 0) {
        fprintf(stderr, "Error: Cannot determine microphone state (amixer missing or failed)\n");
        fprintf(stderr, "Set it explicitly: cctl mic on  |  cctl mic off\n");
        return -1;
    }
    return mic_set(!current);
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
    /* No fabricated fallbacks: every field must come from a real xrandr
     * parse. The old hardcoded defaults (eDP-1 / 2560x1440 / 60.00) made
     * `status`/`rr` show and act on invented values whenever parsing found
     * nothing — most visibly under XWayland. */
    memset(info, 0, sizeof(*info));

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
    /* Success requires BOTH the connected output AND its active mode —
     * a bare "connected" line with no parsed rate is treated as failure
     * so callers never display a half-invented screen state. */
    return (found_output && info->current_rate[0] != '\0') ? 0 : -1;
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

/* Fast check: native X11 only — DISPLAY set AND xrandr present AND not a
 * Wayland session (WAYLAND_DISPLAY stays set when Xwayland provides a fake
 * DISPLAY, which is how the README's "X11 only" rule is actually honored). */
static int has_display_support(void)
{
    const char *disp = getenv("DISPLAY");
    if (!disp || !*disp) return 0;
    if (getenv("WAYLAND_DISPLAY")) return 0;
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
    printf("%sCurrent Display:%s\n", C_YLW, C_RST);
    printf("  %sOutput:%s       %s (%s)\n", C_CYN, C_RST, info.output, info.resolution);
    printf("  %sCurrent Rate:%s %sHz\n\n", C_CYN, C_RST, info.current_rate);
    printf("%sAvailable Rates:%s\n", C_YLW, C_RST);
    for (int i = 0; i < info.rate_count; i++) {
        printf("  %s%-8s%s%s\n", C_CYN, info.available_rates[i], C_RST,
               (strcmp(info.available_rates[i], info.current_rate) == 0) ? " (current)" : "");
    }
    printf("\n%sUsage:%s %scctl rr <rate | 1 | 2>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
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

    /* Active profile mode — recorded by `cctl set`/`setR` in /tmp/cctl.mode,
     * cleared on reboot (missing file = EC default, nothing applied). */
    {
        char m_prof[32] = {0}, m_how[16] = {0};
        if (mode_read(m_prof, sizeof(m_prof), m_how, sizeof(m_how)) == 0)
            printf("  %-14s %s%s%s %s(%s)%s\n", "Mode:",
                   C_CYN, m_prof, C_RST, C_DIM, m_how, C_RST);
        else
            printf("  %-14s %sEC default%s\n", "Mode:", C_DIM, C_RST);
    }

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
    if (mic < 0)
        printf("  %-14s %sN/A (amixer not available)%s\n", "Microphone:", C_DIM, C_RST);
    else
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
        char bat_status[32] = {0}, bp[256];
        bat_sysfs(bp, sizeof(bp), "status");
        read_sysfs_str(bp, bat_status, sizeof(bat_status));
        bat_sysfs(bp, sizeof(bp), "capacity");
        long cap      = read_sysfs_long(bp, -1);
        bat_sysfs(bp, sizeof(bp), "charge_full");
        long full     = read_sysfs_long(bp, -1);
        bat_sysfs(bp, sizeof(bp), "charge_full_design");
        long full_dsn = read_sysfs_long(bp, -1);
        bat_sysfs(bp, sizeof(bp), "charge_now");
        long now      = read_sysfs_long(bp, -1);
        bat_sysfs(bp, sizeof(bp), "current_now");
        long current  = read_sysfs_long(bp, LONG_MIN);
        bat_sysfs(bp, sizeof(bp), "cycle_count");
        long cycles   = read_sysfs_long(bp, -1);
        bat_sysfs(bp, sizeof(bp), "voltage_now");
        long volt     = read_sysfs_long(bp, -1);
        int bat_start = bat_read_start();
        int bat_end   = bat_read_end();

        if (cap < 0) {
            printf("  %-14s %sN/A (no battery)%s\n", "Battery:", C_DIM, C_RST);
        } else {
            printf("  %-14s %ld%% %s", "Battery:", cap, bat_status);
            if (bat_start > 0 && bat_end > 0)
                printf("  [threshold: %d%%→%d%%]", bat_start, bat_end);
            printf("\n");
        }

        if (full > 0 && full_dsn > 0) {
            int health = (int)((full * 100L) / full_dsn);
            const char *hcol = health > 100 ? C_GRN : (health < 80 ? C_RED : C_YLW);
            printf("  %-14s %s%d%%%s (%ld / %ld mAh)\n", "Health:", hcol, health, C_RST,
                   full / 1000, full_dsn / 1000);
        }
        if (cycles > 0)
            printf("  %-14s %s%ld%s\n", "Cycles:", C_CYN, cycles, C_RST);
        if (now > 0 && full > 0)
            printf("  %-14s %ld mAh / %ld mAh\n", "Charge:", now / 1000, full / 1000);
        if (current != LONG_MIN && current != 0) {
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
                     "/sys/devices/system/cpu/%s/cpufreq/scaling_max_freq", ent->d_name);
            long khz = read_sysfs_long(path, -1);
            if (khz <= 0) {
                snprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/%s/cpufreq/cpuinfo_max_freq", ent->d_name);
                khz = read_sysfs_long(path, -1);
            }
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

/* Battery threshold path helpers — use auto-detected battery device. */
static void bat_start_path(char *out, size_t sz) { bat_sysfs(out, sz, "charge_control_start_threshold"); }
static void bat_end_path(char *out, size_t sz)   { bat_sysfs(out, sz, "charge_control_end_threshold"); }
static void bat_start_avail_path(char *out, size_t sz) { bat_sysfs(out, sz, "charge_control_start_available_thresholds"); }
static void bat_end_avail_path(char *out, size_t sz)   { bat_sysfs(out, sz, "charge_control_end_available_thresholds"); }

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
    char p[256];
    bat_start_path(p, sizeof(p));
    long v = read_sysfs_long(p, -1);
    return (v >= 0) ? (int)v : -1;
}

/* Read current charge control end threshold. Returns value or -1. */
static int bat_read_end(void)
{
    char p[256];
    bat_end_path(p, sizeof(p));
    long v = read_sysfs_long(p, -1);
    return (v >= 0) ? (int)v : -1;
}

/* Set battery charge thresholds. "off" (the start==end==0 sentinel) selects
 * a near-full top-up range: charge all the way to the max stop threshold,
 * but only resume charging once the battery drops below the highest usable
 * start threshold (e.g. 95/100). Avoids keeping cells at mid-charge without
 * deep-discharge cycling. */
static int bat_set(int start, int end)
{
    char pa[256];
    /* "off" → stop=max, start=highest listed value below stop */
    if (start == 0 && end == 0) {
        int avail[16], cnt;
        bat_end_avail_path(pa, sizeof(pa));
        cnt = read_avail_thresholds(pa, avail, 16);
        end = (cnt > 0) ? avail[cnt - 1] : 100;
        bat_start_avail_path(pa, sizeof(pa));
        cnt = read_avail_thresholds(pa, avail, 16);
        start = 95;
        for (int i = cnt - 1; i >= 0; i--) {
            if (avail[i] < end) { start = avail[i]; break; }
        }
    }

    /* Validate start threshold */
    int start_avail[16], start_count;
    bat_start_avail_path(pa, sizeof(pa));
    start_count = read_avail_thresholds(pa, start_avail, 16);
    if (start_count > 0 && !is_valid_threshold(start_avail, start_count, start)) {
        fprintf(stderr, "Error: Start threshold %d%% is not valid\n", start);
        print_avail_thresholds("Start", start_avail, start_count);
        return -1;
    }

    /* Validate end threshold */
    int end_avail[16], end_count;
    bat_end_avail_path(pa, sizeof(pa));
    end_count = read_avail_thresholds(pa, end_avail, 16);
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
    bat_start_path(pa, sizeof(pa));
    snprintf(val, sizeof(val), "%d", start);
    if (write_sysfs(pa, val) < 0) {
        fprintf(stderr, "Error: Failed to set start threshold to %d%%\n", start);
        return -1;
    }

    bat_end_path(pa, sizeof(pa));
    snprintf(val, sizeof(val), "%d", end);
    if (write_sysfs(pa, val) < 0) {
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

static int kbd_get_color(int *r, int *g, int *b)
{
    char buf[64];
    if (read_sysfs_str(KBD_PATH "/multi_intensity", buf, sizeof(buf)) < 0)
        return -1;
    if (sscanf(buf, "%d %d %d", r, g, b) != 3)
        return -1;
    return 0;
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
    uint8_t r, g, b;
};

static const struct kbd_preset kbd_presets[] = {
    { "blue",         0,   0, 255 },
    { "chocolate",  210, 105,  30 },
    { "coral",      255, 127,  80 },
    { "cyan",         0, 255, 255 },
    { "gold",       255, 215,   0 },
    { "gray",       128, 128, 128 },
    { "green",        0, 200,   0 },
    { "indigo",      75,   0, 130 },
    { "lime",         0, 255,   0 },
    { "magenta",    255,   0, 255 },
    { "maroon",     128,   0,   0 },
    { "navy",         0,   0, 128 },
    { "olive",      128, 128,   0 },
    { "orange",     255, 136,   0 },
    { "pink",       255,  20, 147 },
    { "purple",     136,   0, 255 },
    { "red",        255,   0,   0 },
    { "salmon",     250, 128, 114 },
    { "silver",     192, 192, 192 },
    { "teal",         0, 128, 128 },
    { "turquoise",   64, 224, 208 },
    { "violet",     238, 130, 238 },
    { "white",      255, 255, 255 },
    { "yellow",     255, 255,   0 },
    { "off",          0,   0,   0 },
    { NULL,           0,   0,   0 }
};

static int kbd_set_preset(const char *name)
{
    /* Case-insensitive lookup */
    char lower[64];
    size_t i;
    for (i = 0; i < sizeof(lower) - 1 && name[i]; i++)
        lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = '\0';

    for (const struct kbd_preset *p = kbd_presets; p->name; p++) {
        if (strcmp(p->name, lower) == 0) {
            return kbd_set_color(p->r, p->g, p->b);
        }
    }

    fprintf(stderr, "Error: Unknown preset '%s'\n", name);
    fprintf(stderr, "Available presets:\n");
    for (const struct kbd_preset *p = kbd_presets; p->name; p++)
        fprintf(stderr, "  %-11s (%d, %d, %d)\n", p->name, p->r, p->g, p->b);
    return -1;
}

static const char *kbd_find_preset_name(int r, int g, int b)
{
    for (const struct kbd_preset *p = kbd_presets; p->name; p++) {
        if ((int)p->r == r && (int)p->g == g && (int)p->b == b)
            return p->name;
    }
    return NULL;
}

static void kbd_show_presets(void)
{
    printf("%sUsage:%s %scctl kbc <R G B | preset>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
    printf("  %sExamples:%s cctl kbc 255 0 128  |  cctl kbc cyan\n\n", C_DIM, C_RST);

    int cur_r = 0, cur_g = 0, cur_b = 0;
    if (kbd_get_color(&cur_r, &cur_g, &cur_b) == 0) {
        const char *pname = kbd_find_preset_name(cur_r, cur_g, cur_b);
        int bri = kbd_get_brightness();
        printf("%sCurrent Color:%s\n", C_YLW, C_RST);
        if (pname) {
            printf("  %sPreset:%s     %s%s%s\n", C_CYN, C_RST, C_CYN, pname, C_RST);
        } else {
            printf("  %sPreset:%s     %scustom%s\n", C_CYN, C_RST, C_YLW, C_RST);
        }
        if (bri >= 0)
            printf("  %sRGB:%s        %sRGB(%d, %d, %d)%s  %s[brightness: %d%%]%s\n\n",
                   C_CYN, C_RST, C_CYN, cur_r, cur_g, cur_b, C_RST, C_DIM, bri, C_RST);
        else
            printf("  %sRGB:%s        %sRGB(%d, %d, %d)%s\n\n",
                   C_CYN, C_RST, C_CYN, cur_r, cur_g, cur_b, C_RST);
    }

    printf("%sAvailable Presets:%s\n", C_YLW, C_RST);
    int col = 0;
    for (const struct kbd_preset *p = kbd_presets; p->name; p++) {
        printf("  %s%-11s%s (%3d, %3d, %3d)", C_CYN, p->name, C_RST, p->r, p->g, p->b);
        col++;
        if (col % 2 == 0) printf("\n");
    }
    if (col % 2 != 0) printf("\n");
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


static int kbd_get_raw_brightness(int *bri)
{
    long val = read_sysfs_long(KBD_PATH "/brightness", -1);
    if (val < 0) return -1;
    *bri = (int)val;
    return 0;
}

/* Kernel-assigned process start time (field 22 of /proc/<pid>/stat).
 * The (pid, starttime) pair uniquely identifies one process instance for
 * the life of the boot: unlike the PID it can never be recycled, and
 * unlike the process name (comm/argv0) it cannot be chosen or forged by
 * whatever process happens to hold that PID. Returns -1 if unreadable. */
static long proc_starttime(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    /* comm sits inside parentheses and may contain spaces/parens, so parse
     * from the LAST ')' onward. Fields after that: state(3), ppid(4), ...
     * starttime is field 22 → the 19th token after the paren. */
    char *rp = strrchr(buf, ')');
    if (!rp || !rp[1]) return -1;
    char *p = rp + 1;
    int field = 3;
    while (*p == ' ') p++;
    while (field < 22 && *p) {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        field++;
    }
    if (field != 22 || !*p) return -1;
    return strtol(p, NULL, 10);
}

/* State file line 1: "<pid> <starttime>" (starttime optional for
 * compatibility with files from older builds → -1 = unknown/not checked). */
static int kbe_read_state_ex(pid_t *pid, long *starttime, char *effect, size_t effect_sz,
                             int *orig_r, int *orig_g, int *orig_b, int *orig_bri,
                             char *resume_effect, size_t resume_sz)
{
    if (starttime) *starttime = -1;
    FILE *fp = fopen(KBE_STATE_PATH, "r");
    if (!fp) return -1;
    char line[128];
    long p = -1, st = -1;
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return -1; }
    if (sscanf(line, "%ld %ld", &p, &st) < 1 || p <= 1) {
        fclose(fp);
        return -1;
    }
    if (pid) *pid = (pid_t)p;
    if (starttime) *starttime = st;
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
    if (resume_effect && resume_sz > 0) {
        resume_effect[0] = '\0';
        if (fgets(line, sizeof(line), fp)) {
            for (char *c = line; *c; c++) {
                if (*c == '\n' || *c == '\r') *c = '\0';
            }
            if (strcmp(line, "none") != 0) {
                strncpy(resume_effect, line, resume_sz - 1);
                resume_effect[resume_sz - 1] = '\0';
            }
        }
    }
    fclose(fp);
    return 0;
}

static inline int kbe_read_state(pid_t *pid, char *effect, size_t effect_sz,
                                 int *orig_r, int *orig_g, int *orig_b, int *orig_bri)
{
    return kbe_read_state_ex(pid, NULL, effect, effect_sz, orig_r, orig_g, orig_b, orig_bri, NULL, 0);
}

static int kbe_is_running(pid_t *pid, char *effect, size_t effect_sz, int *orig_r, int *orig_g, int *orig_b, int *orig_bri)
{
    pid_t p = 0;
    long st = -1;
    char resume_ef[32] = {0};
    if (kbe_read_state_ex(&p, &st, effect, effect_sz, orig_r, orig_g, orig_b, orig_bri, resume_ef, sizeof(resume_ef)) < 0)
        return 0;

    int alive = (kill(p, 0) == 0 || errno == EPERM);
    /* PID-reuse guard: the recorded start time must still match. If the
     * daemon died hard (SIGKILL/OOM), its state file survives in /run until
     * reboot and the PID may now belong to an unrelated process — the old
     * code would have SIGTERM'd that innocent process as root. */
    if (alive && st > 0 && proc_starttime(p) != st)
        alive = 0;

    if (alive) {
        if (pid) *pid = p;
        if (effect && strcmp(effect, "pulse-profile") == 0) {
            if (resume_ef[0] != '\0') {
                snprintf(effect, effect_sz, "%.20s", resume_ef);
            } else {
                snprintf(effect, effect_sz, "profile pulse");
            }
        }
        return 1;
    }

    /* Stale state file (daemon gone, or PID recycled): clean up when able. */
    if (geteuid() == 0) {
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

    int lock_fd = open(KBE_LOCK_PATH, O_RDWR | O_CREAT, 0600);
    if (lock_fd < 0) exit(1);
    /* Retry instead of give-up: the previous daemon was SIGTERM'd just
     * before this worker was forked (profile pulse) or stopped (kbe start),
     * and under load it may still be finishing its keyboard-restore exit
     * path. The old LOCK_NB-once behavior silently lost the profile-change
     * pulse entirely in that window. ~1.5s max wait. */
    int kbe_locked = 0;
    for (int i = 0; i < 75 && !kbe_locked; i++) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0)
            kbe_locked = 1;
        else
            usleep(20000);
    }
    if (!kbe_locked) {
        close(lock_fd);
        exit(1);
    }

    FILE *fp = fopen(KBE_STATE_PATH, "w");
    if (!fp) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        exit(1);
    }
    chmod(KBE_STATE_PATH, 0600); /* PID + saved state: root-only */
    fprintf(fp, "%d %ld\n%s\n%d %d %d %d\n", (int)getpid(),
            proc_starttime(getpid()), effect, orig_r, orig_g, orig_b, orig_bri);
    fclose(fp);

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
    } else if (strcmp(effect, "pulse-cycle") == 0 || strcmp(effect, "pulse+colorchange") == 0 ||
               strcmp(effect, "pulse_cycle") == 0 || strcmp(effect, "pulsecycle") == 0 ||
               strcmp(effect, "heartbeat-cycle") == 0) {
        mode = 14;
    } else if (strcmp(effect, "police") == 0 || strcmp(effect, "siren") == 0 ||
               strcmp(effect, "cop") == 0 || strcmp(effect, "emergency") == 0) {
        mode = 8;
    } else if (strcmp(effect, "fire") == 0 || strcmp(effect, "flame") == 0 ||
               strcmp(effect, "embers") == 0 || strcmp(effect, "burn") == 0) {
        mode = 9;
    } else if (strcmp(effect, "aurora") == 0 || strcmp(effect, "arora") == 0 ||
               strcmp(effect, "northern-lights") == 0 || strcmp(effect, "borealis") == 0) {
        mode = 10;
    } else if (strcmp(effect, "storm") == 0 || strcmp(effect, "lightning") == 0 ||
               strcmp(effect, "thunder") == 0) {
        mode = 11;
    } else if (strcmp(effect, "starlight") == 0 || strcmp(effect, "stars") == 0 ||
               strcmp(effect, "star") == 0 || strcmp(effect, "twinkle") == 0) {
        mode = 12;
    } else if (strcmp(effect, "temp") == 0 || strcmp(effect, "temperature") == 0 ||
               strcmp(effect, "thermal") == 0 || strcmp(effect, "heatmap") == 0) {
        mode = 13;
    }

    int step = 0;
    int flash_hue = 0;
    int pulse_hue = 0;
    int candle_val = 200;
    int fire_hue = 20, fire_bri = 200;
    int star_twinkle_steps = 0, star_peak_white = 220;
    int cur_temp_val = -1;
    int tgt_r = 0, tgt_g = 220, tgt_b = 255;
    int smooth_r = 0, smooth_g = 220, smooth_b = 255;

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
            case 8: { /* police / siren */
                for (int k = 0; k < 2 && g_kbe_running; k++) {
                    kbe_write_frame(fd_col, 255, 0, 0);
                    kbe_sleep_ms(60);
                    if (!g_kbe_running) break;
                    kbe_write_frame(fd_col, 0, 0, 0);
                    kbe_sleep_ms(50);
                }
                kbe_sleep_ms(70);
                for (int k = 0; k < 2 && g_kbe_running; k++) {
                    kbe_write_frame(fd_col, 0, 0, 255);
                    kbe_sleep_ms(60);
                    if (!g_kbe_running) break;
                    kbe_write_frame(fd_col, 0, 0, 0);
                    kbe_sleep_ms(50);
                }
                kbe_sleep_ms(70);
                break;
            }
            case 9: { /* fire */
                int target_h = 5 + (rand() % 35);
                int target_b = 130 + (rand() % 125);
                if ((rand() % 14) == 0) {
                    target_b = 255;
                    target_h = 36;
                }
                fire_hue = (fire_hue * 6 + target_h * 4) / 10;
                fire_bri = (fire_bri * 6 + target_b * 4) / 10;
                int r, g, b;
                kbe_hue_to_rgb(fire_hue, fire_bri, &r, &g, &b);
                kbe_write_frame(fd_col, r, g, b);
                kbe_sleep_ms(30 + (rand() % 25));
                break;
            }
            case 10: { /* aurora */
                int t = step % 480;
                int cur_hue;
                if (t < 160) {
                    cur_hue = 130 + (t * 50) / 160;
                } else if (t < 320) {
                    cur_hue = 180 + ((t - 160) * 95) / 160;
                } else {
                    cur_hue = (275 + ((t - 320) * 215) / 160) % 360;
                }
                int bri = 130 + ((int)kbe_breathe_lut[step % 128] * 125) / 255;
                int r, g, b;
                kbe_hue_to_rgb(cur_hue, bri, &r, &g, &b);
                kbe_write_frame(fd_col, r, g, b);
                kbe_sleep_ms(25);
                step++;
                break;
            }
            case 11: { /* storm */
                kbe_write_frame(fd_col, 8, 12, 35);
                int wait_ms = 1200 + (rand() % 2800);
                while (wait_ms > 0 && g_kbe_running) {
                    if ((rand() % 10) == 0) {
                        int rumble = 25 + (rand() % 30);
                        kbe_write_frame(fd_col, rumble / 4, rumble / 3, rumble);
                        kbe_sleep_ms(35);
                        kbe_write_frame(fd_col, 8, 12, 35);
                    }
                    int chunk = wait_ms > 80 ? 80 : wait_ms;
                    kbe_sleep_ms(chunk);
                    wait_ms -= chunk;
                }
                if (!g_kbe_running) break;

                kbe_write_frame(fd_col, 255, 255, 255);
                kbe_sleep_ms(45);
                kbe_write_frame(fd_col, 30, 45, 90);
                kbe_sleep_ms(35);
                kbe_write_frame(fd_col, 220, 240, 255);
                kbe_sleep_ms(60);
                if ((rand() % 2) == 0) {
                    kbe_write_frame(fd_col, 20, 30, 60);
                    kbe_sleep_ms(25);
                    kbe_write_frame(fd_col, 180, 210, 255);
                    kbe_sleep_ms(40);
                }
                kbe_write_frame(fd_col, 60, 90, 160);
                kbe_sleep_ms(50);
                kbe_write_frame(fd_col, 20, 30, 70);
                kbe_sleep_ms(60);
                break;
            }
            case 12: { /* starlight */
                if (star_twinkle_steps <= 0) {
                    int sky_bri = 35 + ((int)kbe_breathe_lut[step % 128] * 25) / 255;
                    kbe_write_frame(fd_col, (sky_bri * 12) / 60, (sky_bri * 20) / 60, sky_bri);
                    kbe_sleep_ms(30);
                    step++;
                    if ((rand() % 30) == 0) {
                        star_twinkle_steps = 14;
                        star_peak_white = 180 + (rand() % 75);
                    }
                } else {
                    int progress = 7 - abs(star_twinkle_steps - 7);
                    int factor = (progress * 255) / 7;
                    int tr = 12 + ((star_peak_white - 12) * factor) / 255;
                    int tg = 20 + ((star_peak_white - 20) * factor) / 255;
                    int tb = 55 + ((255 - 55) * factor) / 255;
                    kbe_write_frame(fd_col, tr, tg, tb);
                    kbe_sleep_ms(25);
                    star_twinkle_steps--;
                }
                break;
            }
            case 13: { /* temp */
                if ((step % 20) == 0 || cur_temp_val < 0) {
                    int t = read_cpu_temp();
                    if (t > 0) cur_temp_val = t;
                    else if (cur_temp_val < 0) cur_temp_val = 50;

                    if (cur_temp_val <= 40) {
                        tgt_r = 0; tgt_g = 220; tgt_b = 255;
                    } else if (cur_temp_val <= 60) {
                        int ratio = ((cur_temp_val - 40) * 255) / 20;
                        tgt_r = 0;
                        tgt_g = 220 + ((255 - 220) * ratio) / 255;
                        tgt_b = 255 - ((255 - 40) * ratio) / 255;
                    } else if (cur_temp_val <= 75) {
                        int ratio = ((cur_temp_val - 60) * 255) / 15;
                        tgt_r = (255 * ratio) / 255;
                        tgt_g = 255 - ((255 - 220) * ratio) / 255;
                        tgt_b = 40 - (40 * ratio) / 255;
                    } else if (cur_temp_val <= 85) {
                        int ratio = ((cur_temp_val - 75) * 255) / 10;
                        tgt_r = 255;
                        tgt_g = 220 - ((220 - 40) * ratio) / 255;
                        tgt_b = 0;
                    } else {
                        tgt_r = 255; tgt_g = 0; tgt_b = 0;
                    }
                }

                smooth_r = (smooth_r * 8 + tgt_r * 2) / 10;
                smooth_g = (smooth_g * 8 + tgt_g * 2) / 10;
                smooth_b = (smooth_b * 8 + tgt_b * 2) / 10;

                int out_r = smooth_r;
                int out_g = smooth_g;
                int out_b = smooth_b;

                if (cur_temp_val >= 90) {
                    int pulse_bri = (int)kbe_breathe_lut[step % 128];
                    out_r = (out_r * (128 + pulse_bri / 2)) / 255;
                }

                kbe_write_frame(fd_col, out_r, out_g, out_b);
                kbe_sleep_ms(30);
                step++;
                break;
            }
            case 14: { /* pulse-cycle: same heartbeat wave, hue +55 per beat */
                static const uint8_t pulse_wave[] = {
                    30, 90, 180, 255, 230, 160, 100, 60, 40,
                    90, 170, 230, 190, 130, 80, 40, 20, 10, 0
                };
                int pr, pg, pb;
                kbe_hue_to_rgb(pulse_hue, 255, &pr, &pg, &pb);
                pulse_hue = (pulse_hue + 55) % 360;
                for (size_t i = 0; i < sizeof(pulse_wave) && g_kbe_running; i++) {
                    int val = pulse_wave[i];
                    kbe_write_frame(fd_col, (pr * val) / 255, (pg * val) / 255, (pb * val) / 255);
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

static void kbe_profile_pulse_worker(int pr, int pg, int pb,
                                     const char *resume_effect,
                                     int orig_r, int orig_g, int orig_b, int orig_bri)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = kbe_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int lock_fd = open(KBE_LOCK_PATH, O_RDWR | O_CREAT, 0600);
    if (lock_fd < 0) exit(1);
    /* Retry instead of give-up: the previous daemon was SIGTERM'd just
     * before this worker was forked (profile pulse) or stopped (kbe start),
     * and under load it may still be finishing its keyboard-restore exit
     * path. The old LOCK_NB-once behavior silently lost the profile-change
     * pulse entirely in that window. ~1.5s max wait. */
    int kbe_locked = 0;
    for (int i = 0; i < 75 && !kbe_locked; i++) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0)
            kbe_locked = 1;
        else
            usleep(20000);
    }
    if (!kbe_locked) {
        close(lock_fd);
        exit(1);
    }

    FILE *fp = fopen(KBE_STATE_PATH, "w");
    if (!fp) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        exit(1);
    }
    chmod(KBE_STATE_PATH, 0600); /* PID + saved state: root-only */
    fprintf(fp, "%d %ld\npulse-profile\n%d %d %d %d\n%s\n",
            (int)getpid(), proc_starttime(getpid()),
            orig_r, orig_g, orig_b, orig_bri,
            (resume_effect && *resume_effect) ? resume_effect : "none");
    fclose(fp);

    int fd_col = open(KBD_PATH "/multi_intensity", O_WRONLY);
    int fd_bri = open(KBD_PATH "/brightness", O_WRONLY);

    kbe_write_bri(fd_bri, 255);

    static const uint8_t oneshot_lut[] = {
        20, 60, 120, 180, 235, 255, 255, 230, 190, 140, 90, 50, 25, 10, 0
    };

    for (size_t i = 0; i < sizeof(oneshot_lut) && g_kbe_running; i++) {
        int val = oneshot_lut[i];
        int r = (pr * val) / 255;
        int g = (pg * val) / 255;
        int b = (pb * val) / 255;
        kbe_write_frame(fd_col, r, g, b);
        kbe_sleep_ms(25);
    }

    if (fd_col >= 0) close(fd_col);
    if (fd_bri >= 0) close(fd_bri);

    if (!g_kbe_running) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d %d %d", orig_r, orig_g, orig_b);
        write_sysfs(KBD_PATH "/multi_intensity", buf);
        snprintf(buf, sizeof(buf), "%d", orig_bri);
        write_sysfs(KBD_PATH "/brightness", buf);

        unlink(KBE_STATE_PATH);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        unlink(KBE_LOCK_PATH);
        exit(0);
    }

    flock(lock_fd, LOCK_UN);
    close(lock_fd);

    if (resume_effect && *resume_effect && strcmp(resume_effect, "none") != 0) {
        kbe_daemon_worker(resume_effect, orig_r, orig_g, orig_b, orig_bri);
        exit(0);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%d %d %d", orig_r, orig_g, orig_b);
    write_sysfs(KBD_PATH "/multi_intensity", buf);
    snprintf(buf, sizeof(buf), "%d", orig_bri);
    write_sysfs(KBD_PATH "/brightness", buf);

    unlink(KBE_STATE_PATH);
    unlink(KBE_LOCK_PATH);
    exit(0);
}

static void kbe_profile_pulse(const char *profile)
{
    if (access(KBD_PATH "/multi_intensity", F_OK) != 0)
        return;

    int pr = 0, pg = 0, pb = 0;
    if (strcmp(profile, "max") == 0) {
        pr = 255; pg = 0; pb = 0;        /* Red */
    } else if (strcmp(profile, "cpuperf") == 0) {
        pr = 255; pg = 110; pb = 0;      /* Orange */
    } else if (strcmp(profile, "balanced") == 0) {
        pr = 200; pg = 50; pb = 255;      /* Violet */
    } else if (strcmp(profile, "powersave") == 0) {
        pr = 0; pg = 255; pb = 0;        /* Green */
    } else if (strcmp(profile, "eco") == 0) {
        pr = 80; pg = 180; pb = 255;     /* Light Blue */
    } else {
        return;
    }

    pid_t old_pid = 0;
    char running_effect[32] = {0};
    char resume_effect[32] = {0};
    int orig_r = 255, orig_g = 255, orig_b = 255, orig_bri = 255;

    long old_st = -1;
    int had_running = kbe_read_state_ex(&old_pid, &old_st, running_effect, sizeof(running_effect),
                                        &orig_r, &orig_g, &orig_b, &orig_bri,
                                        resume_effect, sizeof(resume_effect));

    /* Same PID-reuse guard as kbe_is_running: only signal the previous
     * daemon if its recorded start time still matches this PID. */
    if (had_running == 0 && (kill(old_pid, 0) == 0 || errno == EPERM) &&
        (old_st < 0 || proc_starttime(old_pid) == old_st)) {
        if (strcmp(running_effect, "pulse-profile") != 0) {
            strncpy(resume_effect, running_effect, sizeof(resume_effect) - 1);
            resume_effect[sizeof(resume_effect) - 1] = '\0';
        }
        kill(old_pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            usleep(10000);
            if (kill(old_pid, 0) != 0 && errno == ESRCH) break;
        }
    } else {
        resume_effect[0] = '\0';
        if (kbd_get_color(&orig_r, &orig_g, &orig_b) < 0) {
            orig_r = 255; orig_g = 255; orig_b = 255;
        }
        if (kbd_get_raw_brightness(&orig_bri) < 0) {
            orig_bri = 255;
        }
    }

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid > 0) return;

    setsid();
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > 2) close(null_fd);
    }

    kbe_profile_pulse_worker(pr, pg, pb, resume_effect, orig_r, orig_g, orig_b, orig_bri);
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

/* Read CPU package temperature in degrees C.
 * First tries tuxedo_io FANINFO1 byte 2 (EC reading).
 * If unavailable or invalid, falls back to sysfs thermal_zone. */
static int read_cpu_temp_ex(int cached_fd)
{
    /* Try tuxedo_io FANINFO1 byte 2 first */
    int fd = (cached_fd >= 0) ? cached_fd : tuxedo_open_clevo();
    if (fd >= 0) {
        int f1 = 0;
        if (ioctl(fd, R_CL_FANINFO1, &f1) >= 0) {
            int t = (f1 >> 16) & 0xFF;
            if (cached_fd < 0) close(fd);
            if (t > 0 && t < 125) return t;
        } else if (cached_fd < 0) {
            close(fd);
        }
    }

    /* Fallback: thermal_zone via sysfs */
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

    /* Pre-computed core classification, sized to the actual CPU count */
    int *is_e_core = calloc((size_t)max_cpus, sizeof(int)); /* 1 = E-core, 0 = P-core */
    float *freqs = malloc((size_t)max_cpus * sizeof(float));
    if (!is_e_core || !freqs) {
        free(is_e_core);
        free(freqs);
        fprintf(stderr, "Error: out of memory allocating CPU arrays\n");
        return -1;
    }
    for (int i = 0; i < max_cpus; i++) {
        is_e_core[i] = is_cpu_e_core(i);
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

        /* Query dynamic max frequencies for P-cores and E-cores */
        int p_max_mhz = 0, e_max_mhz = 0;
        int check_cpus = (cpu_count > 0) ? cpu_count : max_cpus;
        for (int i = 0; i < check_cpus; i++) {
            char path[128];
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", i);
            long khz = read_sysfs_long(path, -1);
            if (khz <= 0) {
                snprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
                khz = read_sysfs_long(path, -1);
            }
            if (khz > 0) {
                int mhz = (int)(khz / 1000);
                if (!is_e_core[i]) {
                    if (mhz > p_max_mhz) p_max_mhz = mhz;
                } else {
                    if (mhz > e_max_mhz) e_max_mhz = mhz;
                }
            }
        }

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
        int temp = read_cpu_temp_ex(tuxedo_fd);
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
    printf("      %sProfile     Turbo  Governor     EPP                EC default CPU & GPU TDP (set)  RAPL CPU TDP override (setR only)%s\n", C_BLD, C_RST);
    printf("      %s─────────── ────── ──────────── ────────────────── ────────────────────────────── ────────────────────────────────%s\n", C_DIM, C_RST);
    printf("      %smax%s         ON     performance  performance        90/115W + GPU 100W              PL1 45 / PL2 90W\n", C_RED, C_RST);
    printf("      %scpuperf%s     ON     performance  performance        45/115W + GPU 70W               %s(no RAPL change)%s\n", C_YLW, C_RST, C_DIM, C_RST);
    printf("      %sbalanced%s    ON     powersave    balance_performance 45/115W + GPU 70W              PL1 35 / PL2 40W\n", C_GRN, C_RST);
    printf("      %spowersave%s   OFF    powersave    balance_power      15/30W  + GPU 70W               %s(no RAPL change)%s\n", C_CYN_BLD, C_RST, C_DIM, C_RST);
    printf("      %seco%s         OFF    powersave    power              15/30W  + GPU 70W               PL1 9 / PL2 10W\n\n", C_DIM, C_RST);

    /* ── Keyboard ───────────────────────────────────────────────────────── */
    printf("  %sKEYBOARD%s\n", C_MAG, C_RST);
    printf("    %skbc%s   <R G B | preset>   Set keyboard color %s(no arg: list presets)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %skbb%s   <pct>              Set brightness %s(0-100%%)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %skbe%s   [effect|stop]      Keyboard backlight effects %s(no arg: shows effect preset list & status)%s\n", C_BLD, C_RST, C_DIM, C_RST);
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
    printf("    %sbat max%s                  standard mode %s(charge to 100%%, resume at 95%%)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);

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
    printf("    %snvidia%s clock    [min] <max>|reset  Lock/unlock GPU clocks %s(no arg: show max clock)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("    %snvidia%s memclock [min] <max>|reset  Lock/unlock memory clocks %s(no arg: show max clock)%s\n\n", C_BLD, C_RST, C_DIM, C_RST);

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

    /* ── System ─────────────────────────────────────────────────────────── */
    printf("  %sSYSTEM%s\n", C_CYN_BLD, C_RST);
    /* State-dependent advertising: an uninstalled copy tells you how to
     * install; the installed binary tells you how to update. The hidden
     * one still works if invoked — this is help visibility only. */
    if (!is_installed_systemwide())
        printf("    %sinstall%s [--force]        Install/upgrade system-wide + passwordless sudo\n", C_BLD, C_RST);
    printf("    %sdrivers-manage%s          Install, reinstall, or uninstall kernel drivers %s(auto-fetch or offline; sha256-verified)%s\n", C_BLD, C_RST, C_DIM, C_RST);
    if (is_installed_systemwide())
        printf("    %supdate%s                  Update cctl from GitHub releases\n\n", C_BLD, C_RST);
    else
        printf("\n"); /* keep the section's blank line without the update entry */

    /* Driver hint — only shown when the TUXEDO/Clevo stack is not loaded */
    if (!drivers_loaded()) {
        printf("  %sDRIVERS NOT LOADED%s — some features need them:\n", C_YLW, C_RST);
        printf("    • %skbc/kbb%s   keyboard backlight (%stuxedo_keyboard%s)\n", C_CYN, C_RST, C_DIM, C_RST);
        printf("    • %sset/setR%s GPU performance slots (%stuxedo_io%s)\n", C_CYN, C_RST, C_DIM, C_RST);
        printf("    • %sbat%s      battery charge thresholds (%sclevo_acpi%s)\n", C_CYN, C_RST, C_DIM, C_RST);
        printf("    Fix: run %scctl drivers-manage%s\n\n",
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

struct nvidia_clock_info {
    int cur_graphics;
    int cur_memory;
    int cur_sm;
    int cur_video;
    int max_graphics;
    int max_memory;
    int max_sm;
    int max_video;
};

/* Parse nvidia-smi -q -d CLOCK output to get max supported clocks */
static int nvidia_query_clocks(struct nvidia_clock_info *ci)
{
    ci->cur_graphics = -1;
    ci->cur_memory = -1;
    ci->cur_sm = -1;
    ci->cur_video = -1;
    ci->max_graphics = -1;
    ci->max_memory = -1;
    ci->max_sm = -1;
    ci->max_video = -1;

    FILE *fp = popen("nvidia-smi -q -d CLOCK 2>/dev/null", "r");
    if (!fp) return -1;

    char line[256];
    int section = 0; /* 0: other, 1: Clocks, 2: Max Clocks */

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "    ", 4) == 0 && line[4] != ' ' && line[4] != '\t') {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "Clocks", 6) == 0 && (p[6] == '\n' || p[6] == '\r' || p[6] == ' ' || p[6] == '\0'))
                section = 1;
            else if (strncmp(p, "Max Clocks", 10) == 0 && (p[10] == '\n' || p[10] == '\r' || p[10] == ' ' || p[10] == '\0'))
                section = 2;
            else
                section = 0;
            continue;
        }

        if (section == 1 || section == 2) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            int val = 0;
            if (sscanf(colon + 1, " %d MHz", &val) != 1)
                continue;

            if (strstr(line, "Graphics")) {
                if (section == 1) ci->cur_graphics = val;
                else ci->max_graphics = val;
            } else if (strstr(line, "Memory")) {
                if (section == 1) ci->cur_memory = val;
                else ci->max_memory = val;
            } else if (strstr(line, "SM")) {
                if (section == 1) ci->cur_sm = val;
                else ci->max_sm = val;
            } else if (strstr(line, "Video")) {
                if (section == 1) ci->cur_video = val;
                else ci->max_video = val;
            }
        }
    }
    pclose(fp);

    if (ci->max_graphics > 0 || ci->max_memory > 0)
        return 0;
    return -1;
}

/* Parse clock arguments:
 *   cctl nvidia clock <max>            -> min=0, max=<max>
 *   cctl nvidia clock <min> <max>      -> min=<min>, max=<max>
 *   cctl nvidia clock <min>,<max>      -> min=<min>, max=<max>
 * Also validates: min >= 0, max > 0, min <= max, and max <= max_supported.
 * Returns 0 on success, -1 on error. */
static int nvidia_parse_clock_inputs(int argc, char **argv, int max_supported,
                                     const char *cmd_name, int *out_min, int *out_max)
{
    if (argc < 4) {
        return -1;
    }
    if (argc > 5) {
        fprintf(stderr, "Error: Unexpected extra argument '%s'\n", argv[5]);
        fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                cmd_name, cmd_name);
        return -1;
    }

    int min = 0, max = 0;

    if (argc == 4) {
        /* Single argument: either "<max>" or "<min>,<max>" */
        const char *arg = argv[3];
        if (strchr(arg, ',')) {
            char extra = 0;
            if (sscanf(arg, "%d,%d%c", &min, &max, &extra) != 2) {
                fprintf(stderr, "Error: Invalid clock range '%s' (expected <min>,<max>)\n", arg);
                fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                        cmd_name, cmd_name);
                return -1;
            }
        } else {
            char extra = 0;
            if (sscanf(arg, "%d%c", &max, &extra) != 1) {
                fprintf(stderr, "Error: Invalid clock value '%s'\n", arg);
                fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                        cmd_name, cmd_name);
                return -1;
            }
            min = 0;
        }
    } else {
        /* Two arguments: "<min> <max>" or "<min>, <max>" */
        char s_min[32];
        strncpy(s_min, argv[3], sizeof(s_min) - 1);
        s_min[sizeof(s_min) - 1] = '\0';
        size_t slen = strlen(s_min);
        if (slen > 0 && s_min[slen - 1] == ',')
            s_min[slen - 1] = '\0';

        char extra1 = 0, extra2 = 0;
        if (sscanf(s_min, "%d%c", &min, &extra1) != 1) {
            fprintf(stderr, "Error: Invalid minimum clock '%s'\n", argv[3]);
            fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                    cmd_name, cmd_name);
            return -1;
        }
        if (sscanf(argv[4], "%d%c", &max, &extra2) != 1) {
            fprintf(stderr, "Error: Invalid maximum clock '%s'\n", argv[4]);
            fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                    cmd_name, cmd_name);
            return -1;
        }
    }

    if (min < 0) {
        fprintf(stderr, "Error: Minimum clock cannot be negative (%d)\n", min);
        return -1;
    }
    if (max <= 0) {
        fprintf(stderr, "Error: Maximum clock must be greater than 0 (%d)\n", max);
        return -1;
    }
    if (min > max) {
        fprintf(stderr, "Error: Minimum clock (%d MHz) cannot exceed maximum clock (%d MHz)\n",
                min, max);
        return -1;
    }
    if (max_supported > 0 && max > max_supported) {
        fprintf(stderr, "Error: Clock %d MHz exceeds maximum supported %d MHz\n",
                max, max_supported);
        return -1;
    }
    if (max_supported > 0 && min > max_supported) {
        fprintf(stderr, "Error: Minimum clock %d MHz exceeds maximum supported %d MHz\n",
                min, max_supported);
        return -1;
    }

    *out_min = min;
    *out_max = max;
    return 0;
}

static int nvidia_clock_show(void)
{
    struct nvidia_clock_info ci;
    if (nvidia_query_clocks(&ci) != 0 || ci.max_graphics <= 0) {
        fprintf(stderr, "Error: Unable to query GPU clocks via nvidia-smi\n");
        return 1;
    }
    printf("%sGPU Graphics Clock:%s\n", C_YLW, C_RST);
    printf("  %sMax Supported:%s %s%d MHz%s\n\n", C_CYN, C_RST, C_CYN, ci.max_graphics, C_RST);
    printf("%sUsage:%s %scctl nvidia clock [min] <max>  |  cctl nvidia clock reset%s\n",
           C_BLD, C_RST, C_CYN_BLD, C_RST);
    printf("  %sExamples:%s cctl nvidia clock 1500        %s(auto minimum 0 MHz)%s\n",
           C_YLW, C_RST, C_DIM, C_RST);
    printf("            cctl nvidia clock 210 1500\n");
    printf("            cctl nvidia clock 210,1500\n");
    printf("            cctl nvidia clock reset\n");
    return 0;
}

static int nvidia_memclock_show(void)
{
    struct nvidia_clock_info ci;
    if (nvidia_query_clocks(&ci) != 0 || ci.max_memory <= 0) {
        fprintf(stderr, "Error: Unable to query GPU memory clocks via nvidia-smi\n");
        return 1;
    }
    printf("%sGPU Memory Clock:%s\n", C_YLW, C_RST);
    printf("  %sMax Supported:%s %s%d MHz%s\n\n", C_CYN, C_RST, C_CYN, ci.max_memory, C_RST);
    printf("%sUsage:%s %scctl nvidia memclock [min] <max>  |  cctl nvidia memclock reset%s\n",
           C_BLD, C_RST, C_CYN_BLD, C_RST);
    printf("  %sExamples:%s cctl nvidia memclock 5000     %s(auto minimum 0 MHz)%s\n",
           C_YLW, C_RST, C_DIM, C_RST);
    printf("            cctl nvidia memclock 405 5000\n");
    printf("            cctl nvidia memclock 405,5000\n");
    printf("            cctl nvidia memclock reset\n");
    return 0;
}

#ifdef CCTL_NVIDIA
#define NVIDIA_USAGE_STR "nvidia {on|off|load|loadgame|unload|status|power|clock|memclock}"
#else
#define NVIDIA_USAGE_STR "nvidia {power|clock|memclock}"
#endif

static int cmd_nvidia(int argc, char **argv)
{
    if (argc < 3) {
        printf("%sUsage:%s %scctl %s%s\n\n", C_BLD, C_RST, C_CYN_BLD, NVIDIA_USAGE_STR, C_RST);
        printf("%sCommands:%s\n", C_YLW, C_RST);
        printf("  %s%-10s%s Hardware D0/D3cold power control (no arg: show state)\n", C_CYN, "power", C_RST);
        printf("  %s%-10s%s Lock/unlock GPU core clocks (no arg: show max clock)\n", C_CYN, "clock", C_RST);
        printf("  %s%-10s%s Lock/unlock GPU memory clocks (no arg: show max clock)\n", C_CYN, "memclock", C_RST);
        return 0;
    }
    const char *action = argv[2];

#ifndef CCTL_NVIDIA
    /* Module/GPU-toggle commands exist only in the private build. In this
     * build they are treated as ordinary unknown actions — the private
     * build's name must never appear in public output. */
    if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0 ||
        strcmp(action, "load") == 0 || strcmp(action, "loadgame") == 0 ||
        strcmp(action, "unload") == 0 || strcmp(action, "status") == 0) {
        fprintf(stderr, "Error: Unknown nvidia action '%s'\n", action);
        fprintf(stderr, "Usage: %s\n", NVIDIA_USAGE_STR);
        return 1;
    }
#endif

#ifdef CCTL_NVIDIA
    if (strcmp(action, "status") == 0) {
        nvidia_show_status();
        return 0;
    }
#endif

    /* Queries without arguments run unprivileged (no root needed, all builds) */
    if (strcmp(action, "power") == 0 && argc < 4) {
        return nvidia_power_show();
    }
    if (strcmp(action, "clock") == 0 && argc < 4) {
        return nvidia_clock_show();
    }
    if (strcmp(action, "memclock") == 0 && argc < 4) {
        return nvidia_memclock_show();
    }

    int clk_min = 0, clk_max = 0;
    int is_reset = 0;

    /* Validate arguments before requesting root elevation */
    if (strcmp(action, "power") == 0) {
        if (strcmp(argv[3], "on") != 0 && strcmp(argv[3], "off") != 0) {
            fprintf(stderr, "Error: Unknown nvidia power argument '%s'\n", argv[3]);
            fprintf(stderr, "Usage: cctl nvidia power [on|off]\n");
            return 1;
        }
    } else if (strcmp(action, "clock") == 0) {
        if (strcmp(argv[3], "reset") == 0) {
            if (argc > 4) {
                fprintf(stderr, "Error: Unexpected extra argument '%s'\n", argv[4]);
                fprintf(stderr, "Usage: cctl nvidia clock [min] <max>  |  cctl nvidia clock reset\n");
                return 1;
            }
            is_reset = 1;
        } else {
            struct nvidia_clock_info ci;
            nvidia_query_clocks(&ci);
            if (nvidia_parse_clock_inputs(argc, argv, ci.max_graphics, "clock", &clk_min, &clk_max) != 0)
                return 1;
        }
    } else if (strcmp(action, "memclock") == 0) {
        if (strcmp(argv[3], "reset") == 0) {
            if (argc > 4) {
                fprintf(stderr, "Error: Unexpected extra argument '%s'\n", argv[4]);
                fprintf(stderr, "Usage: cctl nvidia memclock [min] <max>  |  cctl nvidia memclock reset\n");
                return 1;
            }
            is_reset = 1;
        } else {
            struct nvidia_clock_info ci;
            nvidia_query_clocks(&ci);
            if (nvidia_parse_clock_inputs(argc, argv, ci.max_memory, "memclock", &clk_min, &clk_max) != 0)
                return 1;
        }
#ifndef CCTL_NVIDIA
    } else {
        fprintf(stderr, "Error: Unknown nvidia action '%s'\n", action);
        fprintf(stderr, "Usage: %s\n", NVIDIA_USAGE_STR);
        return 1;
#endif
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
        if (strcmp(argv[3], "on") == 0)
            return nvidia_power_set(1);
        if (strcmp(argv[3], "off") == 0)
            return nvidia_power_set(0);
    } else if (strcmp(action, "clock") == 0) {
        if (is_reset)
            return nvidia_clock_reset();
        return nvidia_clock_set(clk_min, clk_max);
    } else if (strcmp(action, "memclock") == 0) {
        if (is_reset)
            return nvidia_memclock_reset();
        return nvidia_memclock_set(clk_min, clk_max);
    } else {
        fprintf(stderr, "Error: Unknown nvidia action '%s'\n", action);
        fprintf(stderr, "Usage: %s\n", NVIDIA_USAGE_STR);
        return 1;
    }
    return 0;
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
    printf("%sUsage:%s %scctl scale <factor | resolution | off>%s\n\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
    printf("GPU-side scaling renders the desktop/games at a lower internal resolution\n"
           "and stretches it up to native panel resolution using hardware GPU scaler,\n"
           "boosting performance or enlarging UI with zero CPU overhead.\n\n");

    struct display_info info;
    if (has_display_support() && query_display_info(&info) == 0 && info.resolution[0]) {
        printf("%sCurrent Display:%s\n", C_YLW, C_RST);
        printf("  %sOutput:%s %s  |  %sNative Resolution:%s %s  |  %sCurrent Rate:%s %sHz\n\n",
               C_CYN, C_RST, info.output, C_CYN, C_RST, info.resolution, C_CYN, C_RST, info.current_rate);
    }

    printf("%sOptions:%s\n", C_YLW, C_RST);
    printf("  %s%-15s%s Fraction between 0.01 and 1.0 (e.g. 0.75 for 75%%, 0.5 for 50%%)\n", C_CYN, "<factor>", C_RST);
    printf("  %s%-15s%s Explicit WIDTHxHEIGHT (e.g. 1920x1080, 1600x900, 1280x720)\n", C_CYN, "<resolution>", C_RST);
    printf("  %s%-15s%s Restore native 1:1 display resolution\n\n", C_CYN, "off | reset", C_RST);

    printf("%sExamples:%s\n", C_YLW, C_RST);
    printf("  %scctl scale 0.75%s         Render at 75%% resolution (1080p equivalent on 1440p panel)\n", C_CYN, C_RST);
    printf("  %scctl scale 1920x1080%s    Render at explicit 1920x1080 resolution\n", C_CYN, C_RST);
    printf("  %scctl scale 0.5%s          Render at 50%% resolution (large UI / maximum fps)\n", C_CYN, C_RST);
    printf("  %scctl scale off%s          Reset back to native display resolution\n", C_CYN, C_RST);
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
        else {
            fprintf(stderr, "Error: Unexpected extra argument '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!profile) {
        printf("%sUsage:%s %scctl %s <profile> [--nosafe]%s\n\n",
               C_BLD, C_RST, C_CYN_BLD, with_rapl ? "setr" : "set", C_RST);
        printf("%sValid Profiles:%s\n", C_YLW, C_RST);
        printf("  %s%-12s%s %s\n", C_RED, "max", C_RST, "Maximum performance (90/115W + GPU 100W)");
        printf("  %s%-12s%s %s\n", C_YLW, "cpuperf", C_RST, "Performance CPU only (45/115W + GPU 70W)");
        printf("  %s%-12s%s %s\n", C_GRN, "balanced", C_RST, "Balanced daily use (45/115W + GPU 70W)");
        printf("  %s%-12s%s %s\n", C_CYN_BLD, "powersave", C_RST, "Power saving, turbo off (15/30W + GPU 70W)");
        printf("  %s%-12s%s %s\n", C_DIM, "eco", C_RST, "Ultra power saving (15/30W + GPU 70W)");
        return 0;
    }

    /* Validate before recording: only the five known profiles ever reach
     * /tmp/cctl.mode (cctl status and the RAPL PL1 ceiling read it). */
    if (strcmp(profile, "max") != 0 && strcmp(profile, "cpuperf") != 0 &&
        strcmp(profile, "balanced") != 0 && strcmp(profile, "powersave") != 0 &&
        strcmp(profile, "eco") != 0) {
        fprintf(stderr, "Error: Unknown profile '%s'\n", profile);
        fprintf(stderr, "Valid profiles: max, cpuperf, balanced, powersave, eco\n");
        return 1;
    }

    if (geteuid() != 0)
        self_elevate(argc, argv);

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
    if (rc == 0) {
        /* Record active mode + how it was applied (set vs setR) — only once
         * the profile actually landed, so `status` and the RAPL ceiling
         * never report a mode that failed to apply. */
        if (mode_write(profile, with_rapl ? "setR" : "set") < 0)
            fprintf(stderr, "Warning: could not record active mode in %s\n", MODE_FILE);
        kbe_profile_pulse(profile);
        printf("Done.\n");
    }
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
        printf("%sUsage:%s %scctl fan <mode> [pct] [--nosafe]%s\n\n",
               C_BLD, C_RST, C_CYN_BLD, C_RST);
        printf("%sValid Modes:%s\n", C_YLW, C_RST);
        printf("  %s%-12s%s Automatic EC fan control\n", C_CYN, "auto", C_RST);
        printf("  %s%-12s%s Full 100%% fan speed\n", C_CYN, "max", C_RST);
        printf("  %s%-12s%s Quiet mode (forces eco profile; bypass with --nosafe)\n", C_CYN, "silent", C_RST);
        printf("  %s%-12s%s Set both fans to duty 21-100%% (requires --nosafe)\n", C_CYN, "<pct>", C_RST);
        printf("  %s%-12s%s Set CPU fan duty 21-100%% (requires --nosafe)\n", C_CYN, "cpu <pct>", C_RST);
        printf("  %s%-12s%s Set GPU fan duty 21-100%% (requires --nosafe)\n", C_CYN, "gpu <pct>", C_RST);
        return 0;
    }

    if (geteuid() != 0)
        self_elevate(argc, argv);

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
        long no_turbo = read_sysfs_long(TURBO_PATH, -1);
        if (no_turbo >= 0)
            printf("%sCurrent Turbo:%s %s%s%s\n\n",
                   C_YLW, C_RST,
                   no_turbo == 0 ? C_GRN : C_RED,
                   no_turbo == 0 ? "ON" : "OFF",
                   C_RST);
        printf("%sUsage:%s %scctl turbo <on|off> [--nosafe]%s\n",
               C_BLD, C_RST, C_CYN_BLD, C_RST);
        return 0;
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

    if (geteuid() != 0)
        self_elevate(argc, argv);

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
        char cur_gov[64] = {0};
        if (read_sysfs_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", cur_gov, sizeof(cur_gov)) >= 0)
            printf("%sCurrent Governor:%s %s%s%s\n\n", C_YLW, C_RST, C_CYN, cur_gov, C_RST);
        printf("%sUsage:%s %scctl gov <powersave | performance>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        return 0;
    }
    const char *gov = argv[2];
    if (strcmp(gov, "powersave") != 0 && strcmp(gov, "performance") != 0) {
        fprintf(stderr, "Error: Invalid governor '%s' (use powersave or performance)\n", gov);
        return 1;
    }
    if (geteuid() != 0)
        self_elevate(argc, argv);
    int rc = set_governor(gov);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_epp(int argc, char **argv)
{
    if (argc < 3) {
        char cur_epp[64] = {0};
        if (read_sysfs_str("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference", cur_epp, sizeof(cur_epp)) >= 0)
            printf("%sCurrent EPP:%s %s%s%s\n\n", C_YLW, C_RST, C_CYN, cur_epp, C_RST);
        printf("%sUsage:%s %scctl epp <value>%s\n\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        printf("%sValid Values:%s\n", C_YLW, C_RST);
        printf("  %sperformance%s\n", C_CYN, C_RST);
        printf("  %sbalance_performance%s\n", C_CYN, C_RST);
        printf("  %sbalance_power%s\n", C_CYN, C_RST);
        printf("  %spower%s\n", C_CYN, C_RST);
        return 0;
    }
    const char *epp = argv[2];
    if (strcmp(epp, "performance") != 0 && strcmp(epp, "balance_performance") != 0 &&
        strcmp(epp, "balance_power") != 0 && strcmp(epp, "power") != 0) {
        fprintf(stderr, "Error: Invalid EPP '%s'\n", epp);
        fprintf(stderr, "Valid values: performance, balance_performance, balance_power, power\n");
        return 1;
    }
    if (geteuid() != 0)
        self_elevate(argc, argv);
    int rc = set_epp(epp);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_rapl(int argc, char **argv)
{
    if (argc < 3) {
        int cur_pl1 = -1, cur_pl2 = -1;
        if (read_rapl_current(&cur_pl1, &cur_pl2) == 0 && cur_pl1 > 0 && cur_pl2 > 0) {
            printf("%sCurrent RAPL Limits:%s\n", C_YLW, C_RST);
            printf("  %sPL1:%s %s%dW%s\n", C_CYN, C_RST, C_CYN, cur_pl1, C_RST);
            printf("  %sPL2:%s %s%dW%s\n\n", C_CYN, C_RST, C_CYN, cur_pl2, C_RST);
        }
        printf("%sUsage:%s %scctl rapl <pl1_watts> <pl2_watts>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        printf("       %scctl rapl skip <pl2>%s      (set PL2 only)\n", C_CYN_BLD, C_RST);
        printf("       %scctl rapl <pl1> skip%s      (set PL1 only)\n", C_CYN_BLD, C_RST);
        printf("       %scctl rapl <pl2>%s           (one value = PL2 only)\n\n", C_CYN_BLD, C_RST);
        printf("  %sLimits:%s PL1 1-%dW (up to %dW while Mode is max), PL2 1-%dW\n",
                C_DIM, C_RST, RAPL_PL1_MAX_WATTS, RAPL_PL1_MAX_WATTS_MAX, RAPL_PL2_MAX_WATTS);
        return 0;
    }

    int pl1 = 0, pl2 = 0;
    int skip_pl1 = 0, skip_pl2 = 0;
    int pl1_cap = rapl_pl1_ceiling(); /* 45W; 90W only while mode == max */

    /* A single argument is PL2-only (`cctl rapl 110`), so the PL1 ceiling
     * must not be applied to it. Two arguments parse as PL1 then PL2;
     * "skip" omits one side. */
    if (argc < 4) {
        if (strcmp(argv[2], "skip") == 0) {
            fprintf(stderr, "Error: Nothing to set — give a PL2 wattage\n");
            return 1;
        }
        if (safe_atoi(argv[2], &pl2) < 0 || pl2 < 1 || pl2 > RAPL_PL2_MAX_WATTS) {
            fprintf(stderr, "Error: Invalid PL2 value '%s' (use a wattage 1-%d)\n",
                    argv[2], RAPL_PL2_MAX_WATTS);
            return 1;
        }
        skip_pl1 = 1;
    } else {
        /* Parse PL1 */
        if (strcmp(argv[2], "skip") == 0) {
            skip_pl1 = 1;
        } else {
            if (safe_atoi(argv[2], &pl1) < 0 || pl1 < 1) {
                fprintf(stderr, "Error: Invalid PL1 value '%s' (use a wattage 1-%d or 'skip')\n",
                        argv[2], pl1_cap);
                return 1;
            }
            if (pl1 > pl1_cap) {
                fprintf(stderr, "Error: PL1 %dW exceeds the %dW limit for the current mode\n",
                        pl1, pl1_cap);
                if (pl1_cap == RAPL_PL1_MAX_WATTS)
                    fprintf(stderr, "       90W PL1 unlocks only while 'cctl set max' is active "
                                    "(see Mode in 'cctl status')\n");
                return 1;
            }
        }

        /* Parse PL2 */
        if (strcmp(argv[3], "skip") == 0) {
            skip_pl2 = 1;
        } else {
            if (safe_atoi(argv[3], &pl2) < 0 || pl2 < 1) {
                fprintf(stderr, "Error: Invalid PL2 value '%s' (use a wattage 1-%d or 'skip')\n",
                        argv[3], RAPL_PL2_MAX_WATTS);
                return 1;
            }
            if (pl2 > RAPL_PL2_MAX_WATTS) {
                fprintf(stderr, "Error: PL2 %dW exceeds the %dW platform limit\n",
                        pl2, RAPL_PL2_MAX_WATTS);
                return 1;
            }
        }
    }

    /* Must set at least one limit */
    if (skip_pl1 && skip_pl2) {
        fprintf(stderr, "Error: Nothing to set — both PL1 and PL2 are skipped\n");
        return 1;
    }

    if (geteuid() != 0)
        self_elevate(argc, argv);

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
    /* Single arg → preset */
    int rc = kbd_set_preset(argv[2]);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_kbb(int argc, char **argv)
{
    if (argc < 3) {
        int cur_bri = kbd_get_brightness();
        if (cur_bri >= 0)
            printf("%sCurrent Brightness:%s %s%d%%%s\n\n", C_YLW, C_RST, C_CYN, cur_bri, C_RST);
        printf("%sUsage:%s %scctl kbb <0-100>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        return 0;
    }
    int pct;
    if (safe_atoi(argv[2], &pct) < 0 || pct < 0 || pct > 100) {
        fprintf(stderr, "Error: Brightness must be 0-100%%\n");
        return 1;
    }
    if (geteuid() != 0)
        self_elevate(argc, argv);
    kbe_stop(1);
    int rc = kbd_set_brightness(pct);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_kbe(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[2], "status") == 0) {
        /* State file is root-only (0600): if it exists but we can't read
         * it, re-run elevated so `cctl kbe` status stays correct for plain
         * users too (seamless via the NOPASSWD rule when installed).
         * No file at all → plain "none", no elevation. */
        if (geteuid() != 0 && access(KBE_STATE_PATH, F_OK) == 0 &&
            access(KBE_STATE_PATH, R_OK) != 0)
            self_elevate(argc, argv);
        pid_t pid = 0;
        char effect[32] = {0};
        int orig_r = 255, orig_g = 255, orig_b = 255, orig_bri = 255;
        if (kbe_is_running(&pid, effect, sizeof(effect), &orig_r, &orig_g, &orig_b, &orig_bri)) {
            printf("%sKeyboard Backlight Effect:%s\n", C_YLW, C_RST);
            printf("  %sStatus:%s              %s%s%s (active, PID %d)\n", C_CYN, C_RST, C_GRN, effect, C_RST, (int)pid);
            if (strcmp(effect, "temp") == 0 || strcmp(effect, "temperature") == 0) {
                int t = read_cpu_temp();
                if (t > 0)
                    printf("  %sLive CPU Temp:%s       %s%d°C%s\n", C_CYN, C_RST, t >= 85 ? C_RED : (t >= 70 ? C_YLW : C_GRN), t, C_RST);
            }
            printf("  %sOriginal Color:%s      RGB(%d, %d, %d)\n", C_CYN, C_RST, orig_r, orig_g, orig_b);
            int bri_pct = (orig_bri * 100 + 127) / 255;
            printf("  %sOriginal Brightness:%s %d%% (raw %d)\n\n", C_CYN, C_RST, bri_pct, orig_bri);
        } else {
            printf("%sKeyboard Backlight Effect:%s\n", C_YLW, C_RST);
            printf("  %sStatus:%s              %snone%s (stopped)\n\n", C_CYN, C_RST, C_DIM, C_RST);
        }
        printf("%sAvailable Effects:%s\n", C_YLW, C_RST);
        printf("  %s%-16s%s %s\n", C_CYN, "breathe", C_RST, "Smooth fade in/out (uses current color)");
        printf("  %s%-16s%s %s\n", C_CYN, "breathe-cycle", C_RST, "Smooth breathe shifting through colors");
        printf("  %s%-16s%s %s\n", C_CYN, "cycle", C_RST, "Smooth continuous rainbow cycle");
        printf("  %s%-16s%s %s\n", C_CYN, "flash", C_RST, "Strobe flash bursts (uses current color)");
        printf("  %s%-16s%s %s\n", C_CYN, "flash-cycle", C_RST, "Strobe flash bursts cycling colors");
        printf("  %s%-16s%s %s\n", C_CYN, "candle", C_RST, "Realistic flickering candle flame");
        printf("  %s%-16s%s %s\n", C_CYN, "pulse", C_RST, "Heartbeat double-pulse (uses current color)");
        printf("  %s%-16s%s %s\n", C_CYN, "pulse-cycle", C_RST, "Heartbeat double-pulse cycling colors");
        printf("  %s%-16s%s %s\n", C_CYN, "police", C_RST, "Emergency red and blue alternating strobe");
        printf("  %s%-16s%s %s\n", C_CYN, "fire", C_RST, "Dynamic warm flickering campfire flames");
        printf("  %s%-16s%s %s\n", C_CYN, "aurora", C_RST, "Northern Lights emerald, cyan, and violet drift");
        printf("  %s%-16s%s %s\n", C_CYN, "storm", C_RST, "Dark moody sky with electric lightning strikes");
        printf("  %s%-16s%s %s\n", C_CYN, "starlight", C_RST, "Midnight sky with twinkling star shimmers");
        printf("  %s%-16s%s %s\n\n", C_CYN, "temp", C_RST, "Live CPU thermal heatmap (cyan→green→yellow→red)");
        printf("%sUsage:%s %scctl kbe <effect>  |  cctl kbe stop%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
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
        strcmp(sub, "pulse") != 0 && strcmp(sub, "heartbeat") != 0 &&
        strcmp(sub, "pulse-cycle") != 0 && strcmp(sub, "pulse+colorchange") != 0 &&
        strcmp(sub, "pulse_cycle") != 0 && strcmp(sub, "pulsecycle") != 0 &&
        strcmp(sub, "heartbeat-cycle") != 0 &&
        strcmp(sub, "police") != 0 && strcmp(sub, "siren") != 0 &&
        strcmp(sub, "cop") != 0 && strcmp(sub, "emergency") != 0 &&
        strcmp(sub, "fire") != 0 && strcmp(sub, "flame") != 0 &&
        strcmp(sub, "embers") != 0 && strcmp(sub, "burn") != 0 &&
        strcmp(sub, "aurora") != 0 && strcmp(sub, "arora") != 0 &&
        strcmp(sub, "northern-lights") != 0 && strcmp(sub, "borealis") != 0 &&
        strcmp(sub, "storm") != 0 && strcmp(sub, "lightning") != 0 &&
        strcmp(sub, "thunder") != 0 &&
        strcmp(sub, "starlight") != 0 && strcmp(sub, "stars") != 0 &&
        strcmp(sub, "star") != 0 && strcmp(sub, "twinkle") != 0 &&
        strcmp(sub, "temp") != 0 && strcmp(sub, "temperature") != 0 &&
        strcmp(sub, "thermal") != 0 && strcmp(sub, "heatmap") != 0) {
        fprintf(stderr, "Error: Unknown keyboard effect '%s'\n", sub);
        fprintf(stderr, "Available effects: breathe, breathe-cycle, cycle, flash, flash-cycle, candle, pulse, pulse-cycle, police, fire, aurora, storm, starlight, temp\n");
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
        printf("%sCurrent Thresholds:%s\n", C_YLW, C_RST);
        printf("  %sCharge thresholds:%s      start %d%% → stop %d%%\n", C_CYN, C_RST, start, end);

        /* Show available values */
        int avail[16], cnt;
        char ap[256];
        bat_start_avail_path(ap, sizeof(ap));
        cnt = read_avail_thresholds(ap, avail, 16);
        if (cnt > 0) {
            printf("  %sAvailable start values:%s ", C_CYN, C_RST);
            for (int i = 0; i < cnt; i++) {
                printf("%s%d%s%s", avail[i] == start ? C_GRN : "", avail[i], C_RST,
                       i < cnt - 1 ? " " : "\n");
            }
        }
        bat_end_avail_path(ap, sizeof(ap));
        cnt = read_avail_thresholds(ap, avail, 16);
        if (cnt > 0) {
            printf("  %sAvailable stop values:%s  ", C_CYN, C_RST);
            for (int i = 0; i < cnt; i++) {
                printf("%s%d%s%s", avail[i] == end ? C_GRN : "", avail[i], C_RST,
                       i < cnt - 1 ? " " : "\n");
            }
        }
        printf("\n%sUsage:%s %scctl bat <start> <end>  |  cctl bat max%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        return 0;
    }

    /* "max" → widest usable range (95%→100% top-up; the driver rejects 0,
     * so thresholds cannot actually be turned off — the old off/default
     * aliases implied an impossible "off" and were removed). */
    if (strcmp(argv[2], "max") == 0) {
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

/* Username safe to embed in a sudoers rule: ^[A-Za-z_][A-Za-z0-9_-]*$.
 * The username is written verbatim into /etc/sudoers.d/cctl — a stray
 * space, newline or metacharacter would produce an unparseable rule and
 * lock sudo out system-wide, so refuse anything outside this set.
 * Uppercase is allowed (legal on Linux); the security property comes from
 * excluding whitespace and every metacharacter, not from case. */
static int sudoers_user_is_safe(const char *s)
{
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (const char *p = s + 1; *p; p++) {
        if (!(isalpha((unsigned char)*p) || isdigit((unsigned char)*p) ||
              *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

static int cmd_install(int argc, char **argv)
{
    int force = (argc >= 3 && strcmp(argv[2], "--force") == 0);
    if (argc > 3 || (argc == 3 && !force)) {
        fprintf(stderr, "Error: 'cctl install' does not accept arguments.\n");
        return 1;
    }

    /* needs_root=0 in the command table: main() does NOT pre-elevate.
     * The readlink/microversion checks below run unprivileged;
     * self_elevate() re-execs us as root only after they pass — do not
     * assume EUID==0 at this point. */
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
    if (!force && installed_ver > CCTL_MICROVERSION) {
        printf("Installed cctl is newer (build %d > build %d). Use --force to downgrade.\n",
               installed_ver, CCTL_MICROVERSION);
        return 0;
    }

    if (geteuid() != 0)
        self_elevate(argc, argv);

    /* Confirmation prompt: simple enter is not allowed, must type y or n */
    if (installed_ver == CCTL_MICROVERSION)
        printf("Reinstall cctl at /usr/local/bin/cctl (build %d)? [y/n] ", CCTL_MICROVERSION);
    else if (installed_ver > 0)
        printf("Upgrade cctl at /usr/local/bin/cctl (build %d → build %d)? [y/n] ",
               installed_ver, CCTL_MICROVERSION);
    else
        printf("Install cctl to /usr/local/bin/cctl with passwordless sudo? [y/n] ");
    fflush(stdout);

    char ans[32] = {0};
    if (!fgets(ans, sizeof(ans), stdin) ||
        (ans[0] != 'y' && ans[0] != 'Y') ||
        (ans[1] != '\n' && strcasecmp(ans, "yes\n") != 0)) {
        printf("Aborted.\n");
        return 0;
    }

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

    /* 3. Write sudoers.d/cctl granting passwordless sudo.
     *    Order matters:
     *      a) validate the username charset before it goes near the file,
     *      b) back up the existing rule (cctl.bak — sudo's includedir skips
     *         names containing '.', so the backup can never be auto-loaded),
     *      c) write the new rule to a 0440 temp file,
     *      d) validate it with `visudo -c -f`,
     *      e) rename it into place ONLY if it parses cleanly.
     *    A rejected rule never replaces a working one, and sudo can never
     *    be locked out by a bad install. */
    if (!sudoers_user_is_safe(user)) {
        fprintf(stderr, "Error: username '%s' is not safe to write into sudoers.\n", user);
        fprintf(stderr, "       Allowed: first character [A-Za-z_], then [A-Za-z0-9_-].\n");
        fprintf(stderr, "       Create the rule manually with mode 0440:\n");
        fprintf(stderr, "         echo '<user> ALL=(ALL) NOPASSWD: /usr/local/bin/cctl' "
                        "| sudo EDITOR=tee visudo -f /etc/sudoers.d/cctl\n");
        return 1;
    }

    const char *sudoers     = "/etc/sudoers.d/cctl";
    const char *sudoers_tmp = "/etc/sudoers.d/.cctl.tmp";
    const char *sudoers_bak = "/etc/sudoers.d/cctl.bak";

    if (access(sudoers, R_OK) == 0) {
        char *const bak_args[] = { "/bin/cp", "-p",
                                   (char *)sudoers, (char *)sudoers_bak, NULL };
        if (run_cmd("/bin/cp", bak_args) != 0)
            fprintf(stderr, "Warning: could not back up existing sudoers rule to %s\n",
                    sudoers_bak);
    }

    unlink(sudoers_tmp); /* stale temp from an earlier failed run */
    int sfd = open(sudoers_tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0440);
    if (sfd < 0) {
        perror("Error: cannot create sudoers temp file");
        return 1;
    }
    char rule[320];
    int rule_len = snprintf(rule, sizeof(rule),
                            "%s ALL=(ALL) NOPASSWD: /usr/local/bin/cctl\n", user);
    ssize_t w = write(sfd, rule, (size_t)rule_len);
    int close_rc = close(sfd);
    if (w != rule_len || close_rc != 0) {
        perror("Error: cannot write sudoers temp file");
        unlink(sudoers_tmp);
        return 1;
    }

    const char *visudo_path = NULL;
    if (access("/usr/sbin/visudo", X_OK) == 0)     visudo_path = "/usr/sbin/visudo";
    else if (access("/usr/bin/visudo", X_OK) == 0) visudo_path = "/usr/bin/visudo";
    else if (access("/sbin/visudo", X_OK) == 0)    visudo_path = "/sbin/visudo";

    if (visudo_path) {
        char *const v_args[] = { "visudo", "-c", "-f",
                                 (char *)sudoers_tmp, NULL };
        if (run_cmd(visudo_path, v_args) != 0) {
            fprintf(stderr,
                    "Error: visudo rejected the generated sudoers rule — "
                    "keeping the existing one.\n");
            unlink(sudoers_tmp);
            return 1;
        }
    } else {
        fprintf(stderr,
                "Warning: visudo not found — installing rule without syntax validation.\n");
    }

    if (rename(sudoers_tmp, sudoers) != 0) {
        perror("Error: cannot move sudoers rule into place");
        unlink(sudoers_tmp);
        return 1;
    }
    chmod(sudoers, 0440); /* rename preserves 0440; enforce explicitly anyway */
    printf("Sudoers:  %s (passwordless sudo for %s)\n", sudoers, user);

    printf("\nDone. cctl is installed to /usr/local/bin/cctl and configured with passwordless sudo.\n");
    printf("Privileged commands will auto-elevate seamlessly without needing any shell alias.\n");
    return 0;
}

/* ========================================================================
 * DRIVERS MANAGE — source resolution: cache → local file → download → manual path
 * ========================================================================
 * Order (never searches $HOME, never accepts a folder):
 *   1. drivers.tar.gz sitting next to the running cctl binary — sha256
 *      checked on the spot and rejected in place if it does not match,
 *   2. else download it from the mirror repo into a private temp dir
 *      using curl or wget (a failed download is reported explicitly,
 *      with the exact folder a manual copy belongs in),
 *   3. offline (or download failed): accept the full path to a copy —
 *      also checked in place first; staged into /tmp only if valid.
 * Whatever passes, the staged bytes are re-verified against the hash
 * baked into this binary before a single byte is extracted.
 * ======================================================================== */

#define DRIVERS_TARBALL     "drivers.tar.gz"
#define DRIVERS_MIRROR_RAW  "https://raw.githubusercontent.com/bhusann/tuxedo-drivers-cctl-mirror/main/drivers.tar.gz"
#define DRIVERS_MIRROR_REPO "https://github.com/bhusann/tuxedo-drivers-cctl-mirror"
/* SHA256 of drivers.tar.gz from the mirror repo, baked in at build time so
 * cctl only ever installs exactly these driver sources — a re-tarred or
 * swapped file with any other name/content is rejected. When the MIRROR's
 * drivers/ tree changes: re-tar deterministically, update THIS constant,
 * bump CCTL_MICROVERSION, release a new cctl binary. (The driver sources
 * themselves live only in the mirror repo, not in this checkout.) */
#define DRIVERS_SHA256      "ca6cb6d2bcc7abb8168e16c76ca42220bc957e611f718ee678c6be4d593dd4c2"

/* Embedded marker for extract_binary_marker() — lets `cctl update` read
 * this binary's expected driver hash without executing it. */
static const char __attribute__((used)) cctl_meta_sha256[] =
    "@@CCTL_META_SHA256=" DRIVERS_SHA256 "@@";

/* Persistent driver cache: survives reboots so reinstall/uninstall works
 * offline with no tarball beside the binary and no typed path. /var/lib
 * (not /var/cache) — FHS marks it as state that must persist, not as
 * re-creatable cache that system cleaners may purge. Root-owned. */
#ifndef DRIVERS_CACHE_DIR
#define DRIVERS_CACHE_DIR   "/var/lib/cctl"
#endif
#ifndef DRIVERS_CACHE_FILE
#define DRIVERS_CACHE_FILE  DRIVERS_CACHE_DIR "/drivers.tar.gz"
#endif

/* Directory containing the running binary. Returns 0 and fills out. */
static int binary_dir(char *out, size_t sz)
{
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return -1;
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) return -1;
    *slash = '\0';
    if (strlen(path) + 1 > sz) return -1;
    snprintf(out, sz, "%s", path);
    return 0;
}

/* Lowercase hex sha256 of a file, via exec of sha256sum with an argv array
 * (no shell → no quoting problems with odd paths). 0 = success. */
static int file_sha256(const char *path, char out[65])
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        char *const args[] = { "sha256sum", (char *)path, NULL };
        execvp("sha256sum", args);
        _exit(127);
    }
    close(pipefd[1]);
    char buf[256] = {0};
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (n < 64 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)buf[i])) return -1;
        out[i] = (char)tolower((unsigned char)buf[i]);
    }
    out[64] = '\0';
    return 0;
}

/* Scan a binary file for an embedded "@@PREFIX=value@@" marker and extract
 * "value" into out.  DOES NOT execute the file — reads it as raw data.
 * Returns 0 on success, -1 if the marker is not found or unreadable.
 *
 * Used by `cctl update` to read version/hash metadata from a downloaded
 * release binary without running it — closing the prior arbitrary-code-
 * execution-as-root hole where popen() ran the unverified download. */
static int extract_binary_marker(const char *filepath, const char *prefix,
                                 char *out, size_t outsz)
{
    if (!out || outsz == 0) return -1;
    out[0] = '\0';

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 50 * 1024 * 1024) {
        close(fd);
        return -1;
    }

    size_t filesz = (size_t)st.st_size;
    unsigned char *buf = malloc(filesz);
    if (!buf) { close(fd); return -1; }

    size_t total = 0;
    while (total < filesz) {
        ssize_t n = read(fd, buf + total, filesz - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
    if (total != filesz) { free(buf); return -1; }

    size_t pfxlen = strlen(prefix);
    int found = 0;
    for (size_t i = 0; i + pfxlen + 2 <= filesz; i++) {
        if (memcmp(buf + i, prefix, pfxlen) != 0)
            continue;
        /* Found prefix — extract value up to the closing "@@" */
        size_t start = i + pfxlen;
        for (size_t j = start; j + 1 < filesz; j++) {
            if (buf[j] == '@' && buf[j + 1] == '@') {
                size_t vlen = j - start;
                if (vlen >= outsz) vlen = outsz - 1;
                memcpy(out, buf + start, vlen);
                out[vlen] = '\0';
                found = 1;
                break;
            }
        }
        break; /* only check the first occurrence */
    }

    free(buf);
    return found ? 0 : -1;
}

/* Download url → dest via curl (preferred) or wget. 0 = success. */
static int download_file(const char *url, const char *dest)
{
    if (command_exists("curl")) {
        char *const args[] = { "curl", "-fsSL", "--connect-timeout", "10",
                               "--max-time", "300", "-o", (char *)dest,
                               (char *)url, NULL };
        if (run_cmd("curl", args) == 0) return 0;
    }
    if (command_exists("wget")) {
        char *const args[] = { "wget", "-q", "--timeout=10", "-O",
                               (char *)dest, (char *)url, NULL };
        if (run_cmd("wget", args) == 0) return 0;
    }
    return -1;
}

/* rm -rf via argv array (paths may contain spaces — never a shell string).
 * Refuses anything outside our own /tmp/cctl-* temp-dir namespace. */
static void remove_tree(const char *path)
{
    if (!path || strncmp(path, "/tmp/cctl-", 10) != 0) return;
    char *const args[] = { "rm", "-rf", (char *)path, NULL };
    int r = run_cmd("rm", args);
    (void)r;
}

/* Verify a drivers.tar.gz against the hash baked into this binary.
 * 0 = match. Prints the reason on failure; prints the short confirmation
 * only when announce_ok (the pre-staging early checks stay quiet so the
 * single "sha256 verified" line refers to the bytes actually extracted). */
static int verify_drivers_sha(const char *path, const char *src_desc, int announce_ok)
{
    char sha[65];
    if (file_sha256(path, sha) != 0) {
        fprintf(stderr, "Error: cannot compute sha256 of %s (is sha256sum installed?)\n",
                path);
        return -1;
    }
    if (strcmp(sha, DRIVERS_SHA256) != 0) {
        fprintf(stderr,
            "Error: %s checksum mismatch — refusing to install.\n"
            "  source:   %s\n"
            "  expected: %s\n"
            "  got:      %s\n"
            "The file is corrupt, spoofed, or built from different sources.\n",
            src_desc, path, DRIVERS_SHA256, sha);
        return -1;
    }
    if (announce_ok)
        printf("  sha256 verified: %.16s...\n", sha);
    return 0;
}

/* Atomically publish verified driver bytes to the persistent cache
 * (copy to ".new", fsync-free rename over the target). Non-fatal by
 * design: prints why and returns -1; callers continue without it. */
static int cache_store(const char *verified_file)
{
    if (mkdir(DRIVERS_CACHE_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Note: cannot create %s: %s\n", DRIVERS_CACHE_DIR, strerror(errno));
        return -1;
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/.new", DRIVERS_CACHE_DIR);
    char *const cp_args[] = { "cp", "--", (char *)verified_file, tmp, NULL };
    if (run_cmd("cp", cp_args) != 0) {
        unlink(tmp);
        fprintf(stderr, "Note: cannot copy %s into %s\n", verified_file, DRIVERS_CACHE_DIR);
        return -1;
    }
    if (chmod(tmp, 0644) != 0 || rename(tmp, DRIVERS_CACHE_FILE) != 0) {
        fprintf(stderr, "Note: cannot publish %s: %s\n", DRIVERS_CACHE_FILE, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Step 0 of drivers-manage: a hash-matching cache entry wins outright.
 * Returns 1 (out filled) on hit; 0 on absent-or-stale — a stale entry is
 * reported once and never used, only replaced after the next verified
 * acquisition. */
static int cache_lookup(char *out, size_t sz)
{
    char sha[65] = {0};
    if (access(DRIVERS_CACHE_FILE, R_OK) != 0)
        return 0;
    if (file_sha256(DRIVERS_CACHE_FILE, sha) == 0 && strcmp(sha, DRIVERS_SHA256) == 0) {
        snprintf(out, sz, "%s", DRIVERS_CACHE_FILE);
        return 1;
    }
    printf("Note: %s is outdated or corrupt — fetching a verified replacement.\n",
           DRIVERS_CACHE_FILE);
    return 0;
}

static int cmd_drivers_install(int argc, char **argv)
{
    /* No file arguments, ever — cctl locates/downloads the archive itself
     * and verifies its baked-in sha256, so nothing user-named is trusted
     * except via the explicitly-prompted offline path (also verified). */
    if (argc != 2) {
        fprintf(stderr, "Error: 'cctl drivers-manage' does not accept arguments.\n"
                        "       It finds or downloads %s automatically.\n",
                DRIVERS_TARBALL);
        return 1;
    }

    if (geteuid() != 0)
        self_elevate(argc, argv);

    char tmpbase[] = "/tmp/cctl-drivers.XXXXXX";
    char *tmpdir = NULL;
    char tarball[PATH_MAX + 32] = {0}; /* staged copy — always inside tmpdir */
    char src[PATH_MAX + 32] = {0};     /* verified source outside tmp (local/offline) */
    int staged_by_download = 0;
    int src_is_cache = 0;
    int rc = 1;

    /* 0. Persistent cache first: /var/lib/cctl/drivers.tar.gz — survives
     *    reboots, so reinstall/uninstall works fully offline. Only a copy
     *    matching the baked hash is used; a stale one is reported and
     *    replaced after the next verified acquisition. */
    if (cache_lookup(src, sizeof(src))) {
        src_is_cache = 1;
        printf("Using cached %s\n  from %s\n", DRIVERS_TARBALL, DRIVERS_CACHE_DIR);
    }

    /* 1. Local: drivers.tar.gz beside the cctl binary — sha256-checked
     *    ON THE SPOT before anything is staged. A mismatch does NOT abort:
     *    the outdated/corrupt copy is called out ("do not use it") and the
     *    flow continues with the download, which also reseeds the cache. */
    char bindir[PATH_MAX] = {0};
    if (binary_dir(bindir, sizeof(bindir)) != 0)
        bindir[0] = '\0';
    if (!src[0] && bindir[0]) {
        snprintf(src, sizeof(src), "%s/%s", bindir, DRIVERS_TARBALL);
        if (access(src, R_OK) == 0) {
            char lsha[65] = {0};
            if (file_sha256(src, lsha) != 0 || strcmp(lsha, DRIVERS_SHA256) != 0) {
                fprintf(stderr,
                    "Warning: %s beside the cctl binary is outdated or corrupt.\n"
                    "         Do NOT use that copy: %s\n"
                    "         expected %s\n"
                    "         got      %s\n"
                    "         Fetching a verified copy instead.\n",
                    DRIVERS_TARBALL, src, DRIVERS_SHA256,
                    lsha[0] ? lsha : "unreadable");
                src[0] = '\0';
            }
        } else {
            src[0] = '\0';
        }
    }

    if (!src[0]) {
        /* 2. Not local → fetch the mirror straight into a private temp dir. */
        int can_download = (command_exists("curl") || command_exists("wget"));
        if (can_download) {
            tmpdir = mkdtemp(tmpbase);
            if (!tmpdir)
                perror("Warning: mkdtemp failed");
            else {
                snprintf(tarball, sizeof(tarball), "%s/%s", tmpdir, DRIVERS_TARBALL);
                printf("Fetching %s\n  from %s\n", DRIVERS_TARBALL, DRIVERS_MIRROR_RAW);
                if (download_file(DRIVERS_MIRROR_RAW, tarball) == 0)
                    staged_by_download = 1;
            }
        }

        if (!staged_by_download) {
            /* 3. Download failed (or impossible): say so plainly and offer
             *    the two manual ways to continue. */
            fprintf(stderr,
                "Error: could not provide %s.\n"
                "%s\n"
                "Do ONE of the following:\n"
                "  1. Re-run 'cctl drivers-manage' with an internet connection\n"
                "     (it then downloads automatically).\n"
                "  2. Download %s manually from\n"
                "       %s\n"
                "     and place it in this folder:\n"
                "       %s\n"
                "     then re-run 'cctl drivers-manage'.\n"
                "  3. Or enter the full path to a %s you already have.\n",
                DRIVERS_TARBALL,
                can_download
                    ? "Downloading from the mirror repo failed (no internet?)."
                    : "Neither curl nor wget is available to download it.",
                DRIVERS_TARBALL, DRIVERS_MIRROR_REPO,
                bindir[0] ? bindir : "<directory containing the cctl binary>",
                DRIVERS_TARBALL);
            printf("Full path to %s (Enter to abort): ", DRIVERS_TARBALL);
            char path[PATH_MAX] = {0};
            if (!fgets(path, sizeof(path), stdin) || path[0] == '\0') {
                printf("Aborted.\n");
                rc = 0;
                goto cleanup;
            }
            path[strcspn(path, "\n")] = '\0';
            struct stat pst;
            if (stat(path, &pst) != 0) {
                fprintf(stderr, "Error: no such file: %s\n", path);
                goto cleanup;
            }
            if (S_ISDIR(pst.st_mode)) {
                fprintf(stderr, "Error: only the %s file itself is accepted, not a folder.\n"
                                "       Point to the tar.gz file.\n", DRIVERS_TARBALL);
                goto cleanup;
            }
            if (access(path, R_OK) != 0) {
                fprintf(stderr, "Error: no such readable file: %s\n", path);
                goto cleanup;
            }
            /* Check the given file IN PLACE first — a mismatch is rejected
             * here, before it is ever copied into our temp dir. */
            if (verify_drivers_sha(path, "given drivers.tar.gz", 0) != 0)
                goto cleanup;
            snprintf(src, sizeof(src), "%s", path);
        }
    }

    /* Stage: a download already landed in tmpdir; a verified local/offline
     * file is copied in now. Re-verify the staged bytes (the ones that will
     * actually be extracted) to close any swap gap after the in-place check. */
    if (staged_by_download) {
        if (verify_drivers_sha(tarball, "downloaded drivers.tar.gz", 1) != 0)
            goto cleanup;
    } else {
        if (!tmpdir) {
            tmpdir = mkdtemp(tmpbase);
            if (!tmpdir) { perror("Error: cannot create temp directory"); goto cleanup; }
        }
        snprintf(tarball, sizeof(tarball), "%s/%s", tmpdir, DRIVERS_TARBALL);
        char *const cp_args[] = { "cp", "--", src, tarball, NULL };
        if (run_cmd("cp", cp_args) != 0) {
            fprintf(stderr, "Error: failed to copy %s into %s\n", src, tmpdir);
            goto cleanup;
        }
        if (verify_drivers_sha(tarball, "staged drivers.tar.gz", 1) != 0)
            goto cleanup;
    }

    /* Publish the verified staged bytes to the persistent cache so a later
     * reinstall/uninstall needs no network. Skipped when the cache itself
     * was the source; failure is non-fatal (cache_store reports why). */
    if (!src_is_cache)
        (void)cache_store(tarball);

    /* Extract into the private temp dir and run the verified installer. */
    {
        char *const tar_args[] = { "tar", "-xzf", tarball, "-C", tmpdir, NULL };
        if (run_cmd("tar", tar_args) != 0) {
            fprintf(stderr, "Error: failed to extract %s\n", tarball);
            goto cleanup;
        }
    }
    char script[PATH_MAX + 64];
    snprintf(script, sizeof(script), "%s/drivers/driverinstall.sh", tmpdir);
    if (access(script, R_OK) != 0) {
        fprintf(stderr, "Error: drivers/driverinstall.sh not found inside %s\n", DRIVERS_TARBALL);
        goto cleanup;
    }

    printf("Launching driver installer (verified sources)...\n");
    {
        char *const sh_args[] = { "bash", script, NULL };
        if (run_cmd("bash", sh_args) != 0) {
            fprintf(stderr, "Driver installer exited with an error.\n");
            goto cleanup;
        }
    }
    printf("Drivers installed from verified %s.\n", DRIVERS_TARBALL);
    rc = 0;

cleanup:
    if (tmpdir) remove_tree(tmpdir);
    return rc;
}

/* ========================================================================
 * UPDATE — fetch the latest release binary from GitHub
 * ========================================================================
 * The release asset itself reports its (secret, strictly-increasing)
 * microversion; if it is newer than ours, install it over
 * /usr/local/bin/cctl. Everything runs as root in ONE pass: staging the
 * file across the sudo boundary is deliberately avoided because sudo
 * strips our environment and a user-writable staging path would allow the
 * binary to be swapped between verification and install.
 * ======================================================================== */

#ifndef UPDATE_ASSET_URL
#define UPDATE_ASSET_URL   "https://github.com/bhusann/cctl/releases/latest/download/cctl"
#endif
#ifndef UPDATE_DRIVERS_URL
#define UPDATE_DRIVERS_URL "https://github.com/bhusann/cctl/releases/latest/download/drivers.tar.gz"
#endif
#define UPDATE_RELEASES    "https://github.com/bhusann/cctl/releases"

static int cmd_update(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Error: 'cctl update' does not accept arguments.\n");
        return 1;
    }

    if (geteuid() != 0)
        self_elevate(argc, argv);

    char tmpbase[] = "/tmp/cctl-update.XXXXXX";
    char *tmpdir = mkdtemp(tmpbase);
    if (!tmpdir) {
        perror("Error: cannot create temp directory");
        return 1;
    }

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/cctl", tmpdir);

    printf("Checking for the latest release at\n  %s\n", UPDATE_RELEASES);
    if (download_file(UPDATE_ASSET_URL, dest) != 0) {
        fprintf(stderr,
            "Error: cannot download the latest cctl (no internet, no curl/wget,\n"
            "       or the release asset is missing).\n"
            "       Check manually: %s\n", UPDATE_RELEASES);
        remove_tree(tmpdir);
        return 1;
    }
    chmod(dest, 0755);

    /* Extract version info from the downloaded binary WITHOUT executing it.
     * Each cctl build embeds @@CCTL_META_*=value@@ markers in its .rodata
     * section.  Scanning for these is safe — the file is read as data,
     * never run.
     *
     * Security fix: the prior approach ran popen("downloaded_binary
     * --microversion") as root, granting arbitrary code execution to
     * whatever the release asset contained — the only trust was TLS to
     * github.com.  A compromised release or supply-chain attack on CI
     * would own the machine.
     *
     * TODO: add release signing (minisign or GPG) for defense-in-depth
     * beyond TLS + GitHub account trust. */
    int remote_ver = 0;
    char rel[64] = {0};
    {
        char buf[64];
        if (extract_binary_marker(dest, "@@CCTL_META_MICROVER=", buf, sizeof(buf)) == 0)
            safe_atoi(buf, &remote_ver);
        extract_binary_marker(dest, "@@CCTL_META_VERSION=", rel, sizeof(rel));
    }

    if (remote_ver <= 0) {
        fprintf(stderr,
            "Error: cannot read version markers from downloaded file — aborting\n"
            "       (corrupt download, not a cctl binary, or pre-marker release).\n");
        remove_tree(tmpdir);
        return 1;
    }

    if (remote_ver <= CCTL_MICROVERSION) {
        printf("cctl is already up to date: v%s (build %d).\n",
               CCTL_VERSION, CCTL_MICROVERSION);
        remove_tree(tmpdir);
        return 0;
    }

    printf("Found update: %s (build %d) — currently running: v%s (build %d)\n",
           rel[0] ? rel : "new release", remote_ver, CCTL_VERSION, CCTL_MICROVERSION);
    printf("Proceed with update? [y/n] ");
    fflush(stdout);

    char ans[32] = {0};
    if (!fgets(ans, sizeof(ans), stdin) ||
        (ans[0] != 'y' && ans[0] != 'Y') ||
        (ans[1] != '\n' && strcasecmp(ans, "yes\n") != 0)) {
        printf("Update aborted.\n");
        remove_tree(tmpdir);
        return 0;
    }

    /* ── All-or-nothing staging ─────────────────────────────────────────
     * Both assets must be downloaded AND verified into tmpdir before a
     * single byte is placed anywhere: a failed tarball fetch or hash gate
     * aborts the whole update (binary stays, cache stays).
     *
     * The expected drivers hash is extracted from the NEW binary's baked
     * @@CCTL_META_SHA256@@ marker — NOT the currently running binary's
     * DRIVERS_SHA256 constant.  Each release pairs a binary with a
     * specific drivers.tar.gz; the old binary's hash would reject any
     * update that ships new driver sources. */
    char expected_drv_sha[65] = {0};
    if (extract_binary_marker(dest, "@@CCTL_META_SHA256=", expected_drv_sha,
                              sizeof(expected_drv_sha)) != 0 ||
        strlen(expected_drv_sha) != 64) {
        fprintf(stderr,
            "Error: update aborted — cannot extract driver hash from new binary.\n"
            "       The downloaded file may be corrupt or a pre-marker release.\n");
        remove_tree(tmpdir);
        return 1;
    }

    printf("Fetching %s for the offline cache...\n", DRIVERS_TARBALL);
    char dtar[PATH_MAX];
    char dsha[65] = {0};
    snprintf(dtar, sizeof(dtar), "%s/%s", tmpdir, DRIVERS_TARBALL);
    if (download_file(UPDATE_DRIVERS_URL, dtar) != 0) {
        fprintf(stderr,
            "Error: update aborted — could not download %s (offline?).\n"
            "       Nothing was installed: cctl stays at v%s (build %d) and\n"
            "       %s was left unchanged. Both assets must be staged and\n"
            "       verified before either is placed — retry 'cctl update'.\n",
            DRIVERS_TARBALL, CCTL_VERSION, CCTL_MICROVERSION, DRIVERS_CACHE_FILE);
        remove_tree(tmpdir);
        return 1;
    }
    if (file_sha256(dtar, dsha) != 0 || strcmp(dsha, expected_drv_sha) != 0) {
        fprintf(stderr,
            "Error: update aborted — downloaded %s failed its sha256 check.\n"
            "       expected %s\n"
            "       got      %s\n"
            "       Nothing was installed: cctl stays at v%s (build %d) and\n"
            "       %s was left unchanged.\n",
            DRIVERS_TARBALL, expected_drv_sha,
            dsha[0] ? dsha : "unreadable",
            CCTL_VERSION, CCTL_MICROVERSION, DRIVERS_CACHE_FILE);
        remove_tree(tmpdir);
        return 1;
    }

    if (rel[0])
        printf("Update available: %s (build %d) — installed: v%s (build %d)\n",
               rel, remote_ver, CCTL_VERSION, CCTL_MICROVERSION);
    else
        printf("Update available: build %d — installed: build %d\n",
               remote_ver, CCTL_MICROVERSION);

    /* ── Commit phase ── both assets staged + verified above. Cache goes
     * first: if its placement fails, nothing else has moved and the whole
     * update reruns cleanly next time; the binary install goes last. */
    if (cache_store(dtar) != 0) {
        fprintf(stderr,
            "Error: update aborted — could not refresh %s.\n"
            "       The new binary was NOT installed (all-or-nothing):\n"
            "       cctl stays at v%s (build %d). Retry 'cctl update'.\n",
            DRIVERS_CACHE_FILE, CCTL_VERSION, CCTL_MICROVERSION);
        remove_tree(tmpdir);
        return 1;
    }
    char *const args[] = { "install", "-m", "755", dest, "/usr/local/bin/cctl", NULL };
    if (run_cmd("install", args) != 0) {
        fprintf(stderr,
            "Error: failed to install the updated binary.\n"
            "       (The driver cache was already refreshed — harmless: a\n"
            "       verified cache with the old binary works, and rerunning\n"
            "       'cctl update' converges.)\n");
        remove_tree(tmpdir);
        return 1;
    }
    printf("Updated /usr/local/bin/cctl → %s (build %d).\n",
           rel[0] ? rel : "new release", remote_ver);
    printf("Offline cache refreshed: %s\n", DRIVERS_CACHE_FILE);

    if (access("/etc/sudoers.d/cctl", R_OK) != 0)
        printf("Note: no sudoers rule found — run 'cctl install' to set up passwordless sudo.\n");

    remove_tree(tmpdir);
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
    { "set",     0, cmd_set },     /* root required for apply, checked in handler */
    { "setr",    0, cmd_set },     /* root required for apply, checked in handler */
    { "fan",     0, cmd_fan },     /* root required for apply, checked in handler */
    { "turbo",   0, cmd_turbo },   /* root required for apply, checked in handler */
    { "fn",      1, cmd_fnlock },
    { "gov",     0, cmd_gov },     /* root required for set, checked in handler */
    { "epp",     0, cmd_epp },     /* root required for set, checked in handler */
    { "rapl",    0, cmd_rapl },    /* root required for set, checked in handler */
    { "kbc",     0, cmd_kbc },
    { "kbb",     0, cmd_kbb },     /* root required for set, checked in handler */
    { "kbe",     0, cmd_kbe },
    { "webcam",  1, cmd_webcam },
    { "bat",     0, cmd_bat },     /* root required for set, checked in handler */
    { "nvidia",  0, cmd_nvidia },
    { "install", 0, cmd_install },
    { "update",  0, cmd_update },
    { "mux",     0, cmd_mux },      /* root required for switch, checked in handler */
    { "drivers-manage", 0, cmd_drivers_install },

};

int main(int argc, char **argv)
{
    use_color = isatty(STDOUT_FILENO);
    init_colors();

    /* Root runs get a pinned PATH: helper tools invoked later (amixer, tar,
     * sha256sum, chattr, install, ...) must resolve to the real system
     * binaries, never to whatever a user-writable directory in the
     * inherited PATH might contain. Mirrors sudo's secure_path. */
    if (geteuid() == 0)
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);

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

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("cctl %s\n", CCTL_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "--microversion") == 0) {
        printf("%d\n", CCTL_MICROVERSION);
        return 0;
    }

    /* Hidden diagnostic (like --microversion: no help, no README) —
     * prints the sha256 baked into this binary for driver verification. */
    if (strcmp(argv[1], "--shasecret") == 0) {
        printf("%s\n", DRIVERS_SHA256);
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
