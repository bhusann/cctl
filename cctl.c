/*
 * cctl - Lightweight Clevo P15 performance profile & fan controller
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

static int read_cpu_temp(void);
static int read_fan_telemetry_ex(int *cpu_pct, int *gpu_pct, int *cpu_rpm, int *gpu_rpm, int cached_fd);
#define read_fan_telemetry(c, g, cr, gr) read_fan_telemetry_ex(c, g, cr, gr, -1)
static int is_cpu_e_core(int cpu_num);
static int nvidia_is_blacklisted(void);
static int nvidia_is_loaded(void);
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

        if (access(path, F_OK) == 0 && write_sysfs(path, value) == 0)
            touched++;
    }
    closedir(d);
    return touched > 0 ? 0 : -1;
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
    snprintf(pl2_str, sizeof(pl2_str), "%d000000", pl2_w);

    DIR *d = opendir("/sys/class/powercap");
    if (!d) {
        fprintf(stderr, "Warning: RAPL not available (no /sys/class/powercap)\n");
        return 0;
    }

    struct dirent *ent;
    char path[512];

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

        /* Write PL2 */
        snprintf(path, sizeof(path), "/sys/class/powercap/%s/constraint_1_power_limit_uw",
                 ent->d_name);
        if (access(path, W_OK) == 0)
            write_sysfs(path, pl2_str);
    }
    closedir(d);

    if (pl1_w > 0) {
        if (old_pl1 >= 0 && old_pl2 >= 0)
            printf("  RAPL: PL1 %dW -> %dW, PL2 %dW -> %dW\n", old_pl1, pl1_w, old_pl2, pl2_w);
        else
            printf("  RAPL: PL1=%dW PL2=%dW\n", pl1_w, pl2_w);
    } else {
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

/* File used to persist the last user-set GPU duty across cctl invocations.
 * ACPI FANINFO1 doesn't expose the GPU duty register on this Clevo model,
 * and direct EC IO-port writes don't update any readable register for GPU,
 * so we save our set value here instead. */
#define GPU_DUTY_FILE "/tmp/.cctl-gpu-duty"

static int load_gpu_duty(void)
{
    FILE *f = fopen(GPU_DUTY_FILE, "r");
    if (!f) return -1;
    int val;
    if (fscanf(f, "%d", &val) != 1) val = -1;
    fclose(f);
    return val;
}

static void save_gpu_duty(int pct)
{
    FILE *f = fopen(GPU_DUTY_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", pct);
        fclose(f);
    }
}

static void clear_gpu_duty(void)
{
    unlink(GPU_DUTY_FILE);
}

static int fan_auto(int fan_idx)
{
    /* EC quirk: standalone 0x99 auto-restore uses { 0xFF, fan_idx } order.
     * Do NOT swap to { fan_idx, 0xFF } — that only works after a mode cmd 0x98. */
    uint8_t data[2] = { FAN_DUTY_AUTO, (uint8_t)fan_idx };
    if (fan_idx == FAN_GPU) clear_gpu_duty();
    return send_ec_cmd(EC_CMD_FAN_SPEED, data, 2);
}

static int fan_auto_all(void)
{
    clear_gpu_duty();
    if (fan_auto(FAN_CPU) < 0) return -1;
    return fan_auto(FAN_GPU);
}

static int fan_max_all(void)
{
    clear_gpu_duty();
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
    clear_gpu_duty();
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
    int rc = send_ec_cmd(EC_CMD_FAN_SPEED, data, 2);
    if (rc == 0 && fan_idx == FAN_GPU)
        save_gpu_duty(percent);
    return rc;
}

/* ========================================================================
 * PROFILES
 * ========================================================================

 * Profiles from the Rust codebase (profiles.rs):
 *
 * max (perf_gpu):
 *   GPU=2(performance), turbo=ON, governor=performance, EPP=performance,
 *   display=auto, RAPL PL1=45W PL2=90W
 *
 * cpuperf (perf_cpu):
 *   GPU=3(turbo), turbo=ON, governor=performance, EPP=performance,
 *   display=auto, RAPL PL2=70W
 *
 * balanced:
 *   GPU=3(turbo), turbo=ON, governor=powersave, EPP=balance_performance,
 *   display=auto, RAPL PL1=35W PL2=40W
 *
 * powersave:
 *   GPU=1(quiet), turbo=OFF, governor=powersave, EPP=balance_power,
 *   display=40Hz
 *
 * eco (powersave_ultra):
 *   GPU=0(quiet), turbo=OFF, governor=powersave, EPP=power,
 *   display=40Hz, RAPL PL1=9W PL2=10W
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
    printf("Applying: Performance CPU Only\n");
    set_gpu_profile(3);
    set_turbo(1);
    set_governor("performance");
    set_epp("performance");
    if (with_rapl) set_rapl_limits(-1, 70);
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
            for (char *p = prod_buf; *p; p++) *p = tolower((unsigned char)*p);
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

static int mic_is_enabled(void)
{
    FILE *fp = popen("amixer -c 0 sget Capture 2>/dev/null", "r");
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
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "amixer -c 0 sset Capture %s >/dev/null 2>&1", verb);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "Error: amixer failed (is alsa installed?)\n");
        return -1;
    }
    printf("  Microphone: %s\n", enabled ? "ON" : "OFF");
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

    FILE *fp = popen("xrandr --query 2>/dev/null", "r");
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
                    global_max_khz = khz;
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
        printf("  Turbo:     %s%s%s\n", val == 0 ? C_GRN : C_RED, val == 0 ? "ON" : "OFF", C_RST);
    } else {
        printf("  Turbo:     %sN/A (intel_pstate not loaded)%s\n", C_DIM, C_RST);
    }

    /* Governor (read cpu0) */
    if (read_sysfs_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", buf, sizeof(buf)) >= 0) {
        printf("  Governor:  %s%s%s\n", C_CYN, buf, C_RST);
    } else {
        printf("  Governor:  %sN/A%s\n", C_DIM, C_RST);
    }

    /* EPP (read cpu0) */
    if (read_sysfs_str("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference", buf, sizeof(buf)) >= 0) {
        printf("  EPP:       %s%s%s\n", C_CYN, buf, C_RST);
    } else {
        printf("  EPP:       %sN/A%s\n", C_DIM, C_RST);
    }

    /* RAPL PL1 */
    long pl1_uw = read_sysfs_long("/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw", -1);
    if (pl1_uw >= 0) {
        printf("  RAPL PL1:  %s%ldW%s\n", C_CYN, pl1_uw / 1000000, C_RST);
    } else {
        printf("  RAPL PL1:  %sN/A%s\n", C_DIM, C_RST);
    }

    /* RAPL PL2 */
    long pl2_uw = read_sysfs_long("/sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw", -1);
    if (pl2_uw >= 0) {
        printf("  RAPL PL2:  %s%ldW%s\n", C_CYN, pl2_uw / 1000000, C_RST);
    } else {
        printf("  RAPL PL2:  %sN/A%s\n", C_DIM, C_RST);
    }

    /* CPU Temp */
    int temp = read_cpu_temp();
    if (temp >= 0) {
        const char *temp_color = C_GRN;
        if (temp > 80) temp_color = C_RED;
        else if (temp > 65) temp_color = C_YLW;
        printf("  CPU Temp:  %s%d°C%s\n", temp_color, temp, C_RST);
    } else {
        printf("  CPU Temp:  %sN/A%s\n", C_DIM, C_RST);
    }

    /* Webcam */
    int cam = is_webcam_enabled();
    if (cam < 0) {
        printf("  Webcam:    %sNot detected%s\n", C_DIM, C_RST);
    } else {
        printf("  Webcam:    %s%s%s\n", cam ? C_GRN : C_RED, cam ? "ON" : "OFF", C_RST);
    }

    /* Microphone */
    int mic = mic_is_enabled();
    printf("  Microphone: %s%s%s\n", mic ? C_GRN : C_RED, mic ? "ON" : "OFF", C_RST);

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

        printf("  Battery:     %ld%% %s", cap, bat_status);
        if (bat_start > 0 && bat_end > 0)
            printf("  [threshold: %d%%→%d%%]", bat_start, bat_end);
        printf("\n");

        if (full > 0 && full_dsn > 0) {
            int health = (int)((full * 100L) / full_dsn);
            const char *hcol = health > 100 ? C_GRN : (health < 80 ? C_RED : C_YLW);
            printf("  Health:      %s%d%%%s (%ld / %ld mAh)\n", hcol, health, C_RST,
                   full / 1000, full_dsn / 1000);
        }
        if (cycles > 0)
            printf("  Cycles:     %s%ld%s\n", C_CYN, cycles, C_RST);
        if (now > 0)
            printf("  Charge:     %ld mAh / %ld mAh\n", now / 1000, full / 1000);
        if (current != 0) {
            long ma = current / 1000; /* microamps → milliamps */
            printf("  Rate:       %s%+ld mA%s\n", current > 0 ? C_RED : C_GRN, ma, C_RST);
        }
        if (volt > 0)
            printf("  Voltage:    %ld mV\n", volt / 1000);
    }

    /* Nvidia GPU */
    int nv_blacklisted = nvidia_is_blacklisted();
    int nv_loaded = nvidia_is_loaded();
    printf("  Nvidia GPU: %s%s%s (modules %s%s%s)\n",
           nv_blacklisted ? C_RED : C_GRN, nv_blacklisted ? "BLACKLISTED" : "ENABLED", C_RST,
           nv_loaded ? C_GRN : C_DIM, nv_loaded ? "LOADED" : "NOT LOADED", C_RST);

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
        printf("  P-Core:    %s%d MHz%s\n", C_CYN, p_max, C_RST);
    else
        printf("  P-Core:    %sN/A%s\n", C_DIM, C_RST);
    if (e_max > 0)
        printf("  E-Core:    %s%d MHz%s\n", C_CYN, e_max, C_RST);
    else
        printf("  E-Core:    %sN/A%s\n", C_DIM, C_RST);

    /* Fan Telemetry */
    int cpu_pct = 0, gpu_pct = 0, cpu_rpm = 0, gpu_rpm = 0;
    if (read_fan_telemetry(&cpu_pct, &gpu_pct, &cpu_rpm, &gpu_rpm) == 0) {
        printf("\n%s--- Fan Telemetry ---%s\n", C_YLW, C_RST);
        printf("  CPU Fan:   %s%3d%%%s duty, %s%4d RPM%s\n", C_CYN, cpu_pct, C_RST, C_CYN, cpu_rpm, C_RST);
        printf("  GPU Fan:   %s%3d%%%s duty, %s%4d RPM%s\n", C_CYN, gpu_pct, C_RST, C_CYN, gpu_rpm, C_RST);
    } else {
        printf("\n%s--- Fan Telemetry ---%s\n", C_YLW, C_RST);
        printf("  Fans:      %sN/A (ec_sys or tuxedo_io not available)%s\n", C_DIM, C_RST);
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

/* Set battery charge thresholds. Use widest range (40, 100) to effectively disable. */
static int bat_set(int start, int end)
{
    /* "off" → widest available range: start=min, end=max */
    if (start == 0 && end == 0) {
        int avail[16], cnt;
        cnt = read_avail_thresholds(BAT_START_AVAIL_PATH, avail, 16);
        start = (cnt > 0) ? avail[0] : 40;
        cnt = read_avail_thresholds(BAT_END_AVAIL_PATH, avail, 16);
        end = (cnt > 0) ? avail[cnt - 1] : 100;
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

    char val[8];
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
    { "off",        "000000" },
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
        lower[i] = tolower((unsigned char)name[i]);
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
 * GPU duty resolution order:
 *   1. Saved file  — from cctl's own fan_set_duty() (cross-invocation)
 *   2. EC RAM[0xCF] — written by ACPI method or EC internal (Fn+1 etc.)
 *   3. FANINFO1 byte 2  — coarse approximation, only used when RPM>0
 *   4. 0 — fan appears stopped (no RPM, no readable duty register)
 * RPM comes from EC RAM 0xD0-0xD3 via debugfs (auto-loads ec_sys). */
static int read_fan_telemetry_ex(int *cpu_pct, int *gpu_pct, int *cpu_rpm, int *gpu_rpm, int cached_fd)
{
    *cpu_pct = *gpu_pct = *cpu_rpm = *gpu_rpm = 0;
    int got_duty = 0;

    /* Try tuxedo IOCTL for CPU duty (R_CL_FANINFO1 byte 0 is reliable). */
    int fd = (cached_fd >= 0) ? cached_fd : tuxedo_open_clevo();
    if (fd >= 0) {
        int f1 = 0;
        if (ioctl(fd, R_CL_FANINFO1, &f1) >= 0) {
            *cpu_pct = ((f1 & 0xFF) * 100) / 255;
            got_duty = 1;
            /* Tentative GPU duty from FANINFO1 byte 2 — will be overridden
             * below by EC RAM or saved file if better data is available. */
            *gpu_pct = (((f1 >> 16) & 0xFF) * 100) / 255;
        }
        if (cached_fd < 0) close(fd);
    }

    /* EC RAM via debugfs. Read once and use for everything below. */
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
        int cpu_raw = ram[0xF4];
        if (cpu_raw == 0) cpu_raw = ram[0x89];
        *cpu_pct = (cpu_raw * 100) / 255;
    }

    /* GPU duty — authoritative source chain */
    int saved = load_gpu_duty();
    if (saved >= 0) {
        *gpu_pct = saved;                           /* (1) cctl set it */
    } else if (ram[0xCF] > 0) {
        *gpu_pct = (ram[0xCF] * 100) / 255;         /* (2) EC/ACPI set it */
    } else {
        /* (3) keep FANINFO1 byte 2 approximation if fan is spinning,
         *     otherwise the fan is stopped → 0 */
        unsigned int grpm = ((unsigned int)ram[0xD2] << 8) | ram[0xD3];
        if (grpm == 0)
            *gpu_pct = 0;
    }

    /* RPM from EC RAM: 0xD0:D1 (CPU), 0xD2:D3 (GPU), formula: EC_FAN_RPM_DIVISOR / raw16 */
    unsigned int cpu_raw16 = ((unsigned int)ram[0xD0] << 8) | ram[0xD1];
    *cpu_rpm = cpu_raw16 > 0 ? EC_FAN_RPM_DIVISOR / cpu_raw16 : 0;

    unsigned int gpu_raw16 = ((unsigned int)ram[0xD2] << 8) | ram[0xD3];
    *gpu_rpm = gpu_raw16 > 0 ? EC_FAN_RPM_DIVISOR / gpu_raw16 : 0;

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
    return (int)((100.0 * (d_total - d_idle)) / d_total);
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

#include <signal.h>

static volatile int cpumonitor_running = 1;
static void cpumonitor_sigint(int sig) { (void)sig; cpumonitor_running = 0; }

static int cpumonitor(void)
{
    /* Count CPUs */
    int max_cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (max_cpus <= 0) max_cpus = 64;

    struct sigaction sa = { .sa_handler = cpumonitor_sigint };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    printf("Monitor — Press Ctrl+C to stop.\n\n");

    /* Read max frequencies once (don't change during monitor) */
    int p_max_mhz = 0, e_max_mhz = 0;
    {
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
                int ffd = open(path, O_RDONLY);
                if (ffd < 0) continue;
                char fbuf[32] = {0};
                ssize_t n = read(ffd, fbuf, sizeof(fbuf) - 1);
                close(ffd);
                if (n <= 0) continue;
                int mhz = atoi(fbuf) / 1000;
                 if (!is_cpu_e_core(cpu_num)) {
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
        float freqs[64];
        int cpu_count = 0;
        FILE *fp = fopen("/proc/cpuinfo", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp) && cpu_count < max_cpus) {
                if (strncmp(line, "cpu MHz", 7) == 0) {
                    char *p = strchr(line, ':');
                    if (p) {
                        freqs[cpu_count++] = atof(p + 1);
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
        printf("--- [ P-CORES ] (Performance) Max: %d MHz ---\n", p_max_mhz);
        for (int i = 0; i < cpu_count; i++) {
            if (!is_cpu_e_core(i)) {
                printf("Thread %2d: %7.2f MHz%s", i, freqs[i],
                       (p_threads_printed % 2 == 1 || i == cpu_count - 1) ? "\n" : "  |  ");
                p_threads_printed++;
            }
        }
        if (p_threads_printed % 2 != 0) printf("\n");

        int e_cores_printed = 0;
        for (int i = 0; i < cpu_count; i++) {
            if (is_cpu_e_core(i)) {
                if (e_cores_printed == 0) {
                    printf("\n--- [ E-CORES ] (Efficiency) Max: %d MHz ---\n", e_max_mhz);
                }
                printf("Core %2d:   %7.2f MHz\n", i, freqs[i]);
                e_cores_printed++;
            }
        }

        printf("\n--- [ POWER & TEMP ] ---\n");
        int temp = read_cpu_temp();
        if (temp >= 0) {
            printf("CPU Temp:      %d°C\n", temp);
        } else {
            printf("CPU Temp:      N/A\n");
        }
        if (e1 >= 0 && e2 >= 0) {
            double dt = (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec) / 1e9;
            if (dt > 0) {
                long duj = e2 - e1;
                if (duj < 0) duj += 1000000000000LL; /* energy counter wraparound */
                printf("Package Power: %.2f Watts\n", (duj / dt) / 1000000.0);
            }
        } else {
            printf("Package Power: N/A\n");
        }

        /* Read current PL1/PL2 using pread from cached descriptors */
        {
            long pl1 = -1, pl2 = -1;
            char b[32];
            ssize_t nn;
            if (pl1_fd >= 0) {
                memset(b, 0, sizeof(b));
                nn = pread(pl1_fd, b, sizeof(b) - 1, 0);
                if (nn > 0) pl1 = atol(b) / 1000000;
            }
            if (pl2_fd >= 0) {
                memset(b, 0, sizeof(b));
                nn = pread(pl2_fd, b, sizeof(b) - 1, 0);
                if (nn > 0) pl2 = atol(b) / 1000000;
            }
            printf("PL1:          %ldW\n", pl1);
            printf("PL2:          %ldW\n", pl2);
        }

        /* CPU Usage */
        int cpu_usage = get_cpu_usage_pct();
        if (cpu_usage >= 0)
            printf("CPU Usage:    %d%%\n", cpu_usage);
        else
            printf("CPU Usage:    --\n");

        /* Memory Usage */
        long mem_used, mem_total;
        get_mem_usage(&mem_used, &mem_total);
        if (mem_total > 0)
            printf("Memory:       %ld MB / %ld MB (%ld%%)\n",
                   mem_used, mem_total, (mem_used * 100) / mem_total);
        else
            printf("Memory:       N/A\n");

        printf("\n--- [ FANS ] ---\n");
        if (has_fans) {
            printf("CPU Fan: %3d%% duty  %4d RPM\n", cpu_pct, cpu_rpm);
            printf("GPU Fan: %3d%% duty  %4d RPM\n", gpu_pct, gpu_rpm);
        } else {
            printf("Fan telemetry: N/A (ec_sys not loaded)\n");
        }

        printf("\nPress [Ctrl+C] to stop.\n");
        fflush(stdout);
    }

    if (energy_fd >= 0) close(energy_fd);
    if (pl1_fd >= 0) close(pl1_fd);
    if (pl2_fd >= 0) close(pl2_fd);
    if (tuxedo_fd >= 0) close(tuxedo_fd);

    printf("\n");
    return 0;
}

/* ========================================================================
 * CLI
 * ======================================================================== */

static void print_usage(const char *prog)
{
    const char *base = strrchr(prog, '/');
    prog = base ? base + 1 : prog;
    /* Section headers: bold yellow
     * Command names: bold white
     * Arguments/placeholders: dim
     * Profile names: color-coded by intensity
     * Fan modes: color-coded
     * Notes: dim */
    printf(
    "\n"
    "            %s _                           _             _ %s\n"
    "   %s___ ___ %s | | ___  _ __ ___ ___  _ __ | |_ _ __ ___ | |\n"
    "  %s/ __/ _ \\%s| |/ _ \\| '__/ __/ _ \\| '_ \\| __| '__/ _ \\| |\n"
    " %s| (_| (_) %s| | (_) | | | (_| (_) | | | | |_| | | (_) | |\n"
    "  %s\\___\\___/%s|_|\\___/|_|  \\___\\___/|_| |_|\\__|_|  \\___/|_|\n"
    "\n",
    C_CYN_BLD, C_RST, C_CYN_BLD, C_RST, C_CYN_BLD, C_RST,
    C_CYN_BLD, C_RST, C_CYN_BLD, C_RST);
    printf("%sUsage:%s\n", C_YLW, C_RST);
    printf("  %s%s set%s <%sprofile%s>       Apply a performance profile %s(no RAPL)%s\n", C_BLD, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s setR%s <%sprofile%s>      Apply a performance profile %s(with RAPL limits)%s\n", C_BLD, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s fan%s <%smode%s> [%svalue%s]  Control fans\n", C_BLD, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s turbo%s <%son|off%s>      Set turbo boost %s(standalone override)%s\n", C_BLD, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s gov%s <%sgovernor%s>      Set CPU governor %s(standalone override)%s\n", C_BLD, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s epp%s <%svalue%s>         Set EPP %s(standalone override)%s\n", C_BLD, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s rapl%s <%spl1%s> <%spl2%s>    Set RAPL power limits in watts %s(standalone)%s\n", C_BLD, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s kbc%s %sR G B%s           Set keyboard color %s(0-255 per channel)%s\n", C_MAG, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s kbcp%s <%sname%s>         Set keyboard color from preset\n", C_MAG, prog, C_RST, C_DIM, C_RST);
    printf("  %s%s kbb%s <%spct%s>           Set keyboard brightness %s(0-100%%)%s\n", C_MAG, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s mic%s [%son|off%s]        Toggle microphone %s(or set on/off)%s\n", C_RED, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s rr%s [%srate%s]           Set/list refresh rates\n", C_BLU, prog, C_RST, C_DIM, C_RST);
    printf("  %s%s webcam%s [%son|off%s]     Toggle webcam %s(or set on/off)%s\n", C_RED, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s bat%s [%sstart%s] [%sstop%s]   Show/set battery charge thresholds %s(sudo for set)%s\n", C_GRN, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s nvidia%s <%son|off|status%s> Nvidia GPU toggle and status %s(needs root)%s\n", C_BLD, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s nvidia%s <%sload|unload%s>   Session-only GPU load/unload %s(blacklist mode, needs root)%s\n", C_BLD, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s nvidia-power%s [%son|off%s] GPU hardware power %s(D0/D3cold, needs root)%s\n", C_BLD, prog, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %s%s status%s              Show current settings\n", C_BLD, prog, C_RST);
    printf("  %s%s monitor%s             Live CPU freq + power + fan monitor\n", C_BLD, prog, C_RST);

    printf("\n%sProfiles:%s\n", C_YLW, C_RST);
    printf("  %smax%s        Performance Max + GPU %s(80W-100W)%s\n", C_RED, C_RST, C_DIM, C_RST);
    printf("  %scpuperf%s    Performance CPU Only\n", C_YLW, C_RST);
    printf("  %sbalanced%s   Balanced\n", C_GRN, C_RST);
    printf("  %spowersave%s  Powersave\n", C_CYN_BLD, C_RST);
    printf("  %seco%s        Ultra Powersave\n", C_DIM, C_RST);
    printf("\n  %ssetR applies RAPL limits: max(45/90W) cpuperf(70W) balanced(35/40W) eco(9/10W)%s\n", C_DIM, C_RST);

    printf("\n%sFan modes:%s\n", C_YLW, C_RST);
    printf("  %sauto%s       Both fans to auto %s(EC-controlled)%s\n", C_CYN_BLD, C_RST, C_DIM, C_RST);
    printf("  %smax%s        Both fans to maximum speed\n", C_RED, C_RST);
    printf("  %ssilent%s     Both fans to silent mode\n", C_DIM, C_RST);
    printf("  %scpu%s <%spct%s>  CPU fan to %s21-100%%%s\n", C_YLW, C_RST, C_DIM, C_RST, C_DIM, C_RST);
    printf("  %sgpu%s <%spct%s>  GPU fan to %s21-100%%%s\n", C_YLW, C_RST, C_DIM, C_RST, C_DIM, C_RST);

    printf("\n%sBattery thresholds:%s\n", C_GRN, C_RST);
    printf("  %scctl bat%s           Show current thresholds with available values\n", C_BLD, C_RST);
    printf("  %scctl bat%s %s<start> <stop>%s  Set start and stop charge %%\n", C_BLD, C_RST, C_DIM, C_RST);
    printf("  %scctl bat off%s       Widest charge range %s(start=min, stop=max)%s\n", C_BLD, C_RST, C_DIM, C_RST);

    printf("\n%sKeyboard color presets:%s\n", C_MAG, C_RST);
    printf("  blue, chocolate, coral, cyan, gold, gray, green, indigo, lime,\n");
    printf("  magenta, maroon, navy, off, olive, orange, pink, purple, red,\n");
    printf("  salmon, silver, teal, turquoise, violet, white, yellow\n");
}

/* ========================================================================
 * NVIDIA GPU
 * ========================================================================
 * Commands:
 *   nvidia on|off [--force]  — Persistent toggle: blacklist/unblacklist +
 *                               initramfs rebuild + modprobe/rmmod.
 *   nvidia load              — Session-only: wake GPU (D3cold→D0), temp-remove
 *                               blacklist, modprobe nvidia + nvidia_uvm, restore
 *                               blacklist. Requires blacklist mode.
 *   nvidia unload            — Session-only: rmmod all nvidia modules, power off
 *                               GPU (D0→D3cold). Requires blacklist mode.
 *   nvidia status            — Show boot config, module state, GPU telemetry.
 *   nvidia-power [on|off]    — Direct PCI runtime PM control (D0/D3cold).
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

static int rebuild_initramfs(void)
{
    if (access("/usr/bin/mkinitcpio", X_OK) == 0) {
        printf("  Rebuilding initramfs with mkinitcpio (this may take a minute)...\n");
        char *const args[] = { "mkinitcpio", "-P", NULL };
        return run_cmd("mkinitcpio", args);
    }
    if (access("/usr/bin/dracut", X_OK) == 0) {
        printf("  Rebuilding initramfs with dracut (this may take a minute)...\n");
        char *const args[] = { "dracut", "--force", NULL };
        return run_cmd("dracut", args);
    }
    if (access("/usr/sbin/update-initramfs", X_OK) == 0) {
        printf("  Rebuilding initramfs with update-initramfs (this may take a minute)...\n");
        char *const args[] = { "update-initramfs", "-u", NULL };
        return run_cmd("update-initramfs", args);
    }
    fprintf(stderr, "Warning: No initramfs tool found (mkinitcpio/dracut/update-initramfs).\n");
    fprintf(stderr, "You may need to rebuild initramfs manually before rebooting.\n");
    return 0;
}

static int try_unload_nvidia(void)
{
    printf("  Attempting to unload NVIDIA modules...\n");
    if (nvidia_gpu_in_use()) {
        fprintf(stderr, "Error: GPU is actively in use by running processes:\n");
        system("nvidia-smi --query-compute-apps=pid,name --format=csv,noheader 2>/dev/null | awk -F', ' '{print \"    PID \" $1 \": \" $2}'");
        fprintf(stderr, "Cannot unload modules while GPU is in use.\n");
        return -1;
    }

    const char *modules[] = { "nvidia_drm", "nvidia_modeset", "nvidia_uvm", "nvidia" };
    int failed = 0;
    for (size_t i = 0; i < 4; i++) {
        FILE *check = fopen("/proc/modules", "r");
        if (!check) continue;
        char line[256];
        int loaded = 0;
        while (fgets(line, sizeof(line), check)) {
            char name[64];
            if (sscanf(line, "%63s", name) == 1 && strcmp(name, modules[i]) == 0) {
                loaded = 1;
                break;
            }
        }
        fclose(check);

        if (loaded) {
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

static int nvidia_set_off(int force)
{
    if (nvidia_is_blacklisted()) {
        if (nvidia_is_loaded()) {
            printf("  NVIDIA is blacklisted but modules are still loaded (reboot pending).\n");
            int confirm = force;
            if (!confirm) {
                printf("  Attempt to unload modules now? [y/N] ");
                char reply[16];
                if (fgets(reply, sizeof(reply), stdin) && (reply[0] == 'y' || reply[0] == 'Y')) {
                    confirm = 1;
                }
            }
            if (confirm) try_unload_nvidia();
        } else {
            printf("  NVIDIA is already blacklisted and modules are unloaded.\n");
        }
        return 0;
    }

    int confirm = force;
    if (!confirm) {
        printf("  Blacklist NVIDIA and rebuild initramfs? [y/N] ");
        char reply[16];
        if (!fgets(reply, sizeof(reply), stdin) || (reply[0] != 'y' && reply[0] != 'Y')) {
            printf("  Aborted.\n");
            return 0;
        }
    }

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

    if (rebuild_initramfs() < 0) {
        fprintf(stderr, "Error: Initramfs rebuild failed. Removing blacklist config.\n");
        unlink("/etc/modprobe.d/blacklist-nvidia.conf");
        return -1;
    }

    if (nvidia_is_loaded()) {
        try_unload_nvidia();
    }

    printf("\n  NVIDIA is %sblacklisted%s. Reboot to complete. After reboot, only iGPU will be active.\n", C_RED, C_RST);
    return 0;
}

static int nvidia_set_on(int force)
{
    if (!nvidia_is_blacklisted()) {
        if (nvidia_is_loaded()) {
            printf("  NVIDIA is already enabled and modules are loaded.\n");
        } else {
            printf("  NVIDIA is enabled but modules aren't loaded.\n");
            int confirm = force;
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

    int confirm = force;
    if (!confirm) {
        printf("  Unblacklist NVIDIA and rebuild initramfs? [y/N] ");
        char reply[16];
        if (!fgets(reply, sizeof(reply), stdin) || (reply[0] != 'y' && reply[0] != 'Y')) {
            printf("  Aborted.\n");
            return 0;
        }
    }

    printf("  Removing /etc/modprobe.d/blacklist-nvidia.conf...\n");
    unlink("/etc/modprobe.d/blacklist-nvidia.conf");

    if (rebuild_initramfs() < 0) {
        fprintf(stderr, "Error: Initramfs rebuild failed.\n");
        return -1;
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
        printf("  Pending:       %sReboot needed to unload%s\n", C_YLW, C_RST);
    } else if (!blacklisted && !loaded) {
        printf("  Pending:       %sReboot needed to load%s\n", C_YLW, C_RST);
    }

    if (loaded) {
        if (access("/usr/bin/nvidia-smi", X_OK) == 0) {
            printf("\n%s--- NVIDIA GPU Telemetry ---%s\n", C_YLW, C_RST);
            system("nvidia-smi --query-gpu=name,driver_version,memory.used,memory.total,power.draw,temperature.gpu --format=csv,noheader,nounits 2>/dev/null | "
                   "awk -F', ' '{print \"  GPU:           \" $1 \"\\n  Driver:        \" $2 \"\\n  VRAM:          \" $3 \" / \" $4 \" MiB\\n  Power draw:    \" $5 \" W\\n  Temperature:   \" $6 \"°C\"}'");
            
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

static int nvidia_load(void)
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
    }

    /* Restore blacklist immediately */
    rename("/etc/modprobe.d/blacklist-nvidia.conf.bak",
           "/etc/modprobe.d/blacklist-nvidia.conf");

    if (ret == 0) {
        printf("  NVIDIA modules loaded (nvidia + nvidia_uvm). GPU available for this session.\n");
        printf("  %sNote:%s For display/gaming, also run: sudo modprobe nvidia_drm\n", C_YLW, C_RST);
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

    printf("  Unloading NVIDIA modules...\n");
    const char *modules[] = { "nvidia_drm", "nvidia_modeset", "nvidia_uvm", "nvidia" };
    int failed = 0;
    for (size_t i = 0; i < 4; i++) {
        /* Check if this module is currently loaded */
        FILE *check = fopen("/proc/modules", "r");
        if (!check) continue;
        char line[256];
        int loaded = 0;
        while (fgets(line, sizeof(line), check)) {
            char name[64];
            if (sscanf(line, "%63s", name) == 1 && strcmp(name, modules[i]) == 0) {
                loaded = 1;
                break;
            }
        }
        fclose(check);
        if (!loaded) continue;

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

static int nvidia_power_set(int on)
{
    char pci_path[512];
    if (nvidia_find_pci_address(pci_path, sizeof(pci_path)) != 0) {
        fprintf(stderr, "  Error: No NVIDIA GPU found on PCI bus.\n");
        return 1;
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

static int cmd_nvidia_power(int argc, char **argv)
{
    if (argc < 3) {
        /* Show current state */
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

    const char *action = argv[2];
    if (geteuid() != 0) {
        fprintf(stderr, "Error: Must run as root (sudo %s nvidia-power %s)\n", argv[0], action);
        return 1;
    }

    if (strcmp(action, "on") == 0) {
        return nvidia_power_set(1);
    } else if (strcmp(action, "off") == 0) {
        return nvidia_power_set(0);
    } else {
        fprintf(stderr, "Error: Unknown action '%s'\n", action);
        fprintf(stderr, "Usage: nvidia-power [on|off]\n");
        return 1;
    }
}

static int cmd_nvidia(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Error: Missing action for nvidia command\n");
        fprintf(stderr, "Usage: nvidia {on|off|load|unload|status} [--force]\n");
        return 1;
    }
    const char *action = argv[2];
    int force = (argc >= 4 && (strcmp(argv[3], "--force") == 0 || strcmp(argv[3], "-f") == 0));

    if (strcmp(action, "status") == 0) {
        nvidia_show_status();
        return 0;
    }

    /* All other actions need root */
    if (geteuid() != 0) {
        fprintf(stderr, "Error: Must run as root (sudo %s nvidia %s)\n", argv[0], action);
        return 1;
    }

    if (strcmp(action, "off") == 0) {
        return nvidia_set_off(force);
    } else if (strcmp(action, "on") == 0) {
        return nvidia_set_on(force);
    } else if (strcmp(action, "load") == 0) {
        return nvidia_load();
    } else if (strcmp(action, "unload") == 0) {
        return nvidia_unload();
    } else {
        fprintf(stderr, "Error: Unknown nvidia action '%s'\n", action);
        fprintf(stderr, "Usage: nvidia {on|off|load|unload|status} [--force]\n");
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
    int rc = rr_set(argv[2]);
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
    int with_rapl = (strcmp(argv[1], "setR") == 0);
    if (argc < 3) {
        fprintf(stderr, "Error: Missing profile name\n");
        fprintf(stderr, "Valid profiles: max, cpuperf, balanced, powersave, eco\n");
        return 1;
    }
    const char *profile = argv[2];
    int rc = 0;

    if (strcmp(profile, "max") == 0)
        rc = profile_max(with_rapl);
    else if (strcmp(profile, "cpuperf") == 0)
        rc = profile_cpuperf(with_rapl);
    else if (strcmp(profile, "balanced") == 0)
        rc = profile_balanced(with_rapl);
    else if (strcmp(profile, "powersave") == 0)
        rc = profile_powersave(with_rapl);
    else if (strcmp(profile, "eco") == 0)
        rc = profile_eco(with_rapl);
    else {
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
    if (argc < 3) {
        fprintf(stderr, "Error: Missing fan mode\n");
        fprintf(stderr, "Valid modes: auto, max, silent, cpu <pct>, gpu <pct>\n");
        return 1;
    }
    const char *mode = argv[2];
    int rc = 0;

    if (strcmp(mode, "auto") == 0) {
        printf("Setting both fans to AUTO...\n");
        rc = fan_auto_all();
    } else if (strcmp(mode, "max") == 0) {
        printf("Setting both fans to MAX...\n");
        rc = fan_max_all();
    } else if (strcmp(mode, "silent") == 0) {
        printf("Setting both fans to SILENT...\n");
        rc = fan_silent_all();
    } else if (strcmp(mode, "cpu") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: Missing duty percentage\n");
            return 1;
        }
        int pct;
        if (safe_atoi(argv[3], &pct) < 0) {
            fprintf(stderr, "Error: Invalid duty percentage '%s'\n", argv[3]);
            return 1;
        }
        rc = fan_set_duty(FAN_CPU, pct);
    } else if (strcmp(mode, "gpu") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: Missing duty percentage\n");
            return 1;
        }
        int pct;
        if (safe_atoi(argv[3], &pct) < 0) {
            fprintf(stderr, "Error: Invalid duty percentage '%s'\n", argv[3]);
            return 1;
        }
        rc = fan_set_duty(FAN_GPU, pct);
    } else {
        fprintf(stderr, "Error: Unknown fan mode '%s'\n", mode);
        fprintf(stderr, "Valid modes: auto, max, silent, cpu <pct>, gpu <pct>\n");
        return 1;
    }

    ec_release_ports();
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_turbo(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Error: Missing turbo action (on/off)\n");
        return 1;
    }
    int enabled;
    if (strcmp(argv[2], "on") == 0)
        enabled = 1;
    else if (strcmp(argv[2], "off") == 0)
        enabled = 0;
    else {
        fprintf(stderr, "Error: Invalid turbo action '%s' (use on or off)\n", argv[2]);
        return 1;
    }
    int rc = set_turbo(enabled);
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
    if (argc < 4) {
        fprintf(stderr, "Error: Usage: rapl <pl1_watts> <pl2_watts>\n");
        return 1;
    }
    int pl1, pl2;
    if (safe_atoi(argv[2], &pl1) < 0 || safe_atoi(argv[3], &pl2) < 0) {
        fprintf(stderr, "Error: Invalid PL1 or PL2 limit value\n");
        return 1;
    }
    if (pl1 < 1 || pl2 < 1) {
        fprintf(stderr, "Error: Power limits must be >= 1 watt\n");
        return 1;
    }
    int rc = set_rapl_limits(pl1, pl2);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_kbc(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "Error: Usage: kbc <R> <G> <B> (0-255 each)\n");
        return 1;
    }
    int r, g, b;
    if (safe_atoi(argv[2], &r) < 0 || safe_atoi(argv[3], &g) < 0 || safe_atoi(argv[4], &b) < 0) {
        fprintf(stderr, "Error: Invalid RGB color values\n");
        return 1;
    }
    int rc = kbd_set_color(r, g, b);
    if (rc == 0) printf("Done.\n");
    return rc;
}

static int cmd_kbcp(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Error: Usage: kbcp <preset name>\n");
        fprintf(stderr, "Available presets: blue, chocolate, coral, cyan, gold, gray,\n");
        fprintf(stderr, "  green, indigo, lime, magenta, maroon, navy, off, olive,\n");
        fprintf(stderr, "  orange, pink, purple, red, salmon, silver, teal,\n");
        fprintf(stderr, "  turquoise, violet, white, yellow\n");
        return 1;
    }
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
    int rc = kbd_set_brightness(pct);
    if (rc == 0) printf("Done.\n");
    return rc;
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
        if (start == 0 && end == 0)
            printf("disabled (full charge range)\n");
        else
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

    /* "off" or "default" → widest range */
    if (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "default") == 0) {
        if (geteuid() != 0) {
            fprintf(stderr, "Error: Must run as root (sudo %s bat off)\n", argv[0]);
            return 1;
        }
        int rc = bat_set(0, 0);
        if (rc == 0) printf("Done.\n");
        return rc;
    }

    /* set <start> <end> */
    if (argc < 4) {
        fprintf(stderr, "Error: Usage: bat <start> <end> or bat off\n");
        return 1;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "Error: Must run as root (sudo %s bat %s %s)\n", argv[0], argv[2], argv[3]);
        return 1;
    }
    int start, end;
    if (safe_atoi(argv[2], &start) < 0 || safe_atoi(argv[3], &end) < 0) {
        fprintf(stderr, "Error: Invalid threshold values\n");
        return 1;
    }
    int rc = bat_set(start, end);
    if (rc == 0) printf("Done.\n");
    return rc;
}

struct command {
    const char *name;
    int needs_root;
    int (*handler)(int argc, char **argv);
};

static const struct command commands[] = {
    { "status",  0, cmd_status },
    { "rr",      0, cmd_rr },
    { "mic",     0, cmd_mic },
    { "monitor", 1, cmd_monitor },
    { "set",     1, cmd_set },
    { "setR",    1, cmd_set },
    { "fan",     1, cmd_fan },
    { "turbo",   1, cmd_turbo },
    { "gov",     1, cmd_gov },
    { "epp",     1, cmd_epp },
    { "rapl",    1, cmd_rapl },
    { "kbc",     1, cmd_kbc },
    { "kbcp",    1, cmd_kbcp },
    { "kbb",     1, cmd_kbb },
    { "webcam",  1, cmd_webcam },
    { "bat",     0, cmd_bat },     /* root required for set, checked in handler */
    { "nvidia",  0, cmd_nvidia },
    { "nvidia-power", 0, cmd_nvidia_power },
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
        int max_cpus = sysconf(_SC_NPROCESSORS_CONF);
        if (max_cpus <= 0) max_cpus = 64;

        for (int i = 0; i < max_cpus; i++) {
            if (is_cpu_e_core(i)) {
                CPU_SET(i, &cpuset);
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

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    int cmd_found = 0;
    int rc = 0;
    size_t num_cmds = sizeof(commands) / sizeof(commands[0]);
    for (size_t i = 0; i < num_cmds; i++) {
        if (strcmp(commands[i].name, argv[1]) == 0) {
            cmd_found = 1;
            if (commands[i].needs_root && geteuid() != 0) {
                fprintf(stderr, "Error: Must run as root (sudo %s ...)\n", argv[0]);
                return 1;
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
