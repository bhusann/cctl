/* ========================================================================
 * FN LOCK
 * ======================================================================== */

#define FNLOCK_PATH "/sys/devices/platform/tuxedo_keyboard/fn_lock"

static int set_fnlock(int enabled)
{
    if (access(FNLOCK_PATH, F_OK) != 0) {
        cctl_stream_fprintf(stderr, "Error: Fn Lock not available (tuxedo_keyboard not loaded?)\n");
        return -1;
    }
    const char *val = enabled ? "1" : "0";
    if (write_sysfs(FNLOCK_PATH, val) < 0) {
        cctl_stream_fprintf(stderr, "Error: Failed to set Fn Lock to %s\n", enabled ? "ON" : "OFF");
        return -1;
    }
    cctl_stream_printf("  Fn Lock: %s%s\n", enabled ? "ON" : "OFF", snapshot_applied_tag());
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
        cctl_stream_fprintf(stderr, "Error: Fn Lock not available (tuxedo_keyboard not loaded?)\n");
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
        cctl_stream_fprintf(stderr, "Warning: intel_pstate not available, skipping turbo\n");
        return 0; /* non-fatal on AMD/non-pstate */
    }
    /* no_turbo: 0 = turbo ON, 1 = turbo OFF */
    const char *val = enabled ? "0" : "1";
    if (write_sysfs(TURBO_PATH, val) < 0) {
        cctl_stream_fprintf(stderr, "Error: Failed to set turbo to %s\n", enabled ? "ON" : "OFF");
        return -1;
    }
    cctl_stream_printf("  Turbo boost: %s%s\n", enabled ? "ON" : "OFF", snapshot_applied_tag());
    return 0;
}

/* ========================================================================
 * CPU GOVERNOR
 * ======================================================================== */

static int set_governor(const char *gov)
{
    if (cpu_policy_value_available(CPU_AVAILABLE_GOVERNORS_PATH, gov) != 1) {
        cctl_stream_fprintf(stderr,
                            "Error: governor '%s' is not listed by the active CPU driver\n",
                            gov);
        return -1;
    }
    cctl_stream_printf("  Governor: %s%s\n", gov, snapshot_applied_tag());
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
        cctl_stream_fprintf(stderr, "Warning: EPP not available (no intel_pstate), skipping\n");
        return 0;
    }

    if (cpu_policy_value_available(CPU_AVAILABLE_EPP_PATH, val) != 1) {
        cctl_stream_fprintf(stderr,
                            "Error: EPP value '%s' is not listed by the active CPU driver\n",
                            val);
        return -1;
    }

    char cur_gov[64] = {0};
    if (read_sysfs_str(CPU_GOVERNOR_PATH, cur_gov, sizeof(cur_gov)) >= 0) {
        if (strcmp(cur_gov, "performance") == 0 && strcmp(val, "performance") != 0) {
            cctl_stream_fprintf(stderr,
                    "Error: CPU governor is currently 'performance' — manual EPP control is only supported in 'powersave' mode.\n"
                    "       Change governor first: cctl gov powersave\n");
            return -1;
        }
    }

    cctl_stream_printf("  EPP: %s%s\n", val, snapshot_applied_tag());
    int rc = write_to_all_cpus("cpufreq/energy_performance_preference", val);
    if (rc < 0) {
        cctl_stream_fprintf(stderr, "Error: Failed to set EPP to '%s'\n", val);
        return -1;
    }
    return 0;
}

/* ========================================================================
 * RAPL POWER LIMITS
 * ======================================================================== */

/* RAPL ceilings (watts): PL2 hard max 115W (OEM platform limit); PL1 hard
 * max 45W — raised to 90W only while EC profile code 2 is active (see
 * rapl_pl1_ceiling(), based on live TCC detection). */
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

static int rapl_pl1_within_cap(int pl1_w, int pl1_cap)
{
    return pl1_w <= 0 || pl1_w <= pl1_cap;
}

static int rapl_pl2_within_cap(int pl2_w)
{
    return pl2_w <= 0 || pl2_w <= RAPL_PL2_MAX_WATTS;
}

static int rapl_pl2_above_pl1(int pl1_w, int pl2_w)
{
    return pl1_w <= 0 || pl2_w <= 0 || pl2_w > pl1_w;
}

/* Set RAPL power limits. Pass pl1_w/pl2_w <= 0 to skip that limit. */
static int set_rapl_limits(int pl1_w, int pl2_w, int pl1_cap)
{
    /* Defense in depth: enforce the caller's current-mode or profile ceiling. */
    if (!rapl_pl1_within_cap(pl1_w, pl1_cap) || !rapl_pl2_within_cap(pl2_w)) {
        cctl_stream_fprintf(stderr, "Error: RAPL out of range (PL1 max %dW%s, PL2 max %dW)\n",
                pl1_cap, pl1_cap > RAPL_PL1_MAX_WATTS ? " with EC profile code 2" : "",
                RAPL_PL2_MAX_WATTS);
        return -1;
    }

    int old_pl1 = -1, old_pl2 = -1;
    read_rapl_current(&old_pl1, &old_pl2);

    int eff_pl1 = pl1_w > 0 ? pl1_w : old_pl1;
    int eff_pl2 = pl2_w > 0 ? pl2_w : old_pl2;
    if (!rapl_pl2_above_pl1(eff_pl1, eff_pl2)) {
        cctl_stream_fprintf(stderr, "Error: PL2 (%dW) must be greater than PL1 (%dW)\n", eff_pl2, eff_pl1);
        return -1;
    }

    char pl1_str[32], pl2_str[32];
    if (pl1_w > 0) snprintf(pl1_str, sizeof(pl1_str), "%d000000", pl1_w);
    if (pl2_w > 0) snprintf(pl2_str, sizeof(pl2_str), "%d000000", pl2_w);

    DIR *d = opendir("/sys/class/powercap");
    if (!d) {
        cctl_stream_fprintf(stderr, "Warning: RAPL not available (no /sys/class/powercap)\n");
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

    if (dry_run) {
        stream_printf(STREAM_DELAY_US, "Dry run: RAPL limits would be applied\n");
        return 0;
    }

    /* Readback verification: detect locked RAPL limits that silently
     * reject writes.  A mismatch means firmware has the MSR locked. */
    int err = 0;
    int actual_pl1 = -1, actual_pl2 = -1;
    read_rapl_current(&actual_pl1, &actual_pl2);
    if (pl1_w > 0 && actual_pl1 >= 0 && actual_pl1 != pl1_w) {
        cctl_stream_fprintf(stderr, "  Warning: PL1 readback %dW != requested %dW (RAPL may be firmware-locked)\n",
                actual_pl1, pl1_w);
        err = -1;
    }
    if (pl2_w > 0 && actual_pl2 >= 0 && actual_pl2 != pl2_w) {
        cctl_stream_fprintf(stderr, "  Warning: PL2 readback %dW != requested %dW (RAPL may be firmware-locked)\n",
                actual_pl2, pl2_w);
        err = -1;
    }

    if (pl1_w > 0 && pl2_w > 0) {
        if (old_pl1 >= 0 && old_pl2 >= 0)
            cctl_stream_printf("  RAPL: PL1 %dW -> %dW, PL2 %dW -> %dW%s\n", old_pl1, pl1_w, old_pl2, pl2_w, snapshot_applied_tag());
        else
            cctl_stream_printf("  RAPL: PL1=%dW PL2=%dW%s\n", pl1_w, pl2_w, snapshot_applied_tag());
    } else if (pl1_w > 0) {
        if (old_pl1 >= 0)
            cctl_stream_printf("  RAPL: PL1 %dW -> %dW%s\n", old_pl1, pl1_w, snapshot_applied_tag());
        else
            cctl_stream_printf("  RAPL: PL1=%dW%s\n", pl1_w, snapshot_applied_tag());
    } else if (pl2_w > 0) {
        if (old_pl2 >= 0)
            cctl_stream_printf("  RAPL: PL2 %dW -> %dW%s\n", old_pl2, pl2_w, snapshot_applied_tag());
        else
            cctl_stream_printf("  RAPL: PL2=%dW%s\n", pl2_w, snapshot_applied_tag());
    }
    return err;
}

/* ========================================================================
 * GPU PROFILE (via /dev/tuxedo_io IOCTL — best effort)
 * ======================================================================== */

#include <sys/ioctl.h>

#define R_HWCHECK_CL       0x8008EC05
#define R_MOD_VERSION      0x8008EC00
#define W_CL_PERF_PROFILE  0x4008EE15
#define R_CL_PERF_PROFILE  0x8008ED16
#define W_CL_FAN_AUTO_ON_CLOSE 0x0000EE16

#define CCTL_TUXEDO_MIN_VERSION "0.3.10"

#include "cctl_shared.h"

static int tuxedo_io_version_ok(int fd)
{
    char version[64] = {0};
    int major, minor, patch, min_major, min_minor, min_patch;

    if (ioctl(fd, R_MOD_VERSION, version) != 0 ||
        sscanf(version, "%d.%d.%d", &major, &minor, &patch) != 3 ||
        sscanf(CCTL_TUXEDO_MIN_VERSION, "%d.%d.%d",
               &min_major, &min_minor, &min_patch) != 3)
        return 0;
    return major > min_major ||
           (major == min_major && (minor > min_minor ||
            (minor == min_minor && patch >= min_patch)));
}

static const char *gpu_profile_name(int profile)
{
    static const char *names[] = { "silent", "powersave", "high performance", "standard" };
    if (profile >= 0 && profile <= 3) return names[profile];
    return "unknown";
}

/* Open /dev/tuxedo_io, issue an ioctl, and close it. Returns ioctl status or -1. */
static int tuxedo_ioctl(unsigned long req, void *arg)
{
    int fd = tuxedo_open_clevo();
    if (fd < 0) return -1;
    int res = ioctl(fd, req, arg);
    close(fd);
    return res;
}

static int set_gpu_profile_tuxedo(int profile)
{
    if (dry_run) {
        stream_printf(STREAM_DELAY_US, "Dry run: set EC profile code %s (%d)\n", gpu_profile_name(profile), profile);
        return 0;
    }
    return tuxedo_ioctl(W_CL_PERF_PROFILE, &profile);
}

static int set_gpu_profile(int profile)
{
    if (profile < 0 || profile > 3) {
        cctl_stream_fprintf(stderr, "Error: Invalid EC profile code %d (must be 0-3)\n", profile);
        return -1;
    }

    cctl_stream_printf("  EC profile code: %s (%d)%s\n", gpu_profile_name(profile), profile, snapshot_applied_tag());

    if (set_gpu_profile_tuxedo(profile) == 0)
        return 0;

    cctl_stream_fprintf(stderr, "  Warning: tuxedo_io not available, EC profile code not set\n");
    return 0; /* non-fatal: CPU settings still applied */
}

/* ========================================================================
 * FAN CONTROL (Hybrid: tuxedo_io ioctls + direct EC port I/O)
 * ========================================================================
 * - Both fans auto: tuxedo_io ioctl W_CL_FANAUTO (bitmask 0x0F)
 * - Both fans duty: tuxedo_io ioctl W_CL_FANSPEED (packed raw1 | raw2<<8)
 * - Individual fan duty: direct EC port I/O (cmd 0x99) — leaves the other
 *   fan on its automatic thermal curve without locking it
 * - Max mode: tuxedo_io 100% duty, with legacy direct EC mode fallback
 * - Silent mode: legacy direct EC port I/O (cmd 0x98 mode + cmd 0x99)
 * ======================================================================== */

#define FAN_CPU 1
#define FAN_GPU 2

/* Direct EC port commands */
#define EC_CMD_FAN_MODE   0x98
#define EC_CMD_FAN_SPEED  0x99
#define FAN_MODE_MAX      0x40
#define FAN_MODE_SILENT   0x20
#define FAN_DUTY_AUTO     0xFF
/* tuxedo_io ioctl constants */
#define R_CL_FANINFO1   0x8008ED10
#define R_CL_FANINFO2   0x8008ED11
#define R_CL_FANINFO3   0x8008ED12
#define W_CL_FANSPEED   0x4008EE10
#define W_CL_FANAUTO    0x4008EE11
#define FAN_MAX_RAW     255

/* --- tuxedo_io implementations --- */

static int fan_set_speeds_tuxedo(uint8_t raw_fan1, uint8_t raw_fan2)
{
    if (dry_run) {
        stream_printf(STREAM_DELAY_US, "Dry run: set fan duties to %u/%u (raw)\n", raw_fan1, raw_fan2);
        return 0;
    }
    /* Byte order in packed argument: fan1 | (fan2 << 8) */
    int32_t arg = (int32_t)raw_fan1 | ((int32_t)raw_fan2 << 8);
    int res = tuxedo_ioctl(W_CL_FANSPEED, &arg);
    if (res < 0) {
        cctl_stream_perror("Error: tuxedo_io fan duty request failed");
        return -1;
    }
    return 0;
}

static int fan_auto_tuxedo(int32_t mask)
{
    if (dry_run) {
        stream_printf(STREAM_DELAY_US, "Dry run: restore automatic control for fan mask 0x%X\n", mask ? mask : 15);
        return 0;
    }
    /* Note: arg MUST be passed as a pointer (&arg) because tuxedo_io
     * uses copy_from_user. Bitmask 15 (0x0F) releases all fans (1, 2, 3, 4). */
    int32_t arg = mask ? mask : 15;
    int res = tuxedo_ioctl(W_CL_FANAUTO, &arg);
    if (res < 0) {
        cctl_stream_perror("Error: tuxedo_io fan auto request failed");
        return -1;
    }
    return 0;
}

static int fan_auto(int fan_idx)
{
    int mask = (fan_idx == FAN_CPU) ? 1 : ((fan_idx == FAN_GPU) ? 2 : 15);
    return fan_auto_tuxedo(mask);
}

static int legacy_fan_auto(int fan_idx);

static int fan_auto_all(void)
{
    if (fan_auto(0) == 0)
        return 0;

    cctl_stream_fprintf(stderr, "Warning: tuxedo_io fan auto failed; restoring fans through direct EC control\n");
    if (legacy_fan_auto(FAN_CPU) < 0)
        return -1;
    return legacy_fan_auto(FAN_GPU);
}

/* --- Legacy direct EC implementations (fallback Max and Silent) --- */

static int fan_max_all(void)
{
    /* tuxedo_io exposes a both-fan duty ioctl, but no distinct max-mode ioctl.
     * Raw 255 is 100%; retain the EC max-mode sequence as the fallback. */
    if (fan_set_speeds_tuxedo(FAN_MAX_RAW, FAN_MAX_RAW) == 0)
        return 0;

    cctl_stream_fprintf(stderr, "Warning: tuxedo_io max duty failed; trying direct EC max mode\n");
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

/* --- Fan Duty (Individual = Legacy EC, Both = tuxedo_io) --- */

static int fan_set_duty(int fan_idx, int percent, int allow_low)
{
    int minimum = allow_low ? 0 : 25;
    if (percent < minimum || percent > 100) {
        if (!allow_low && percent >= 0 && percent < 25)
            cctl_stream_fprintf(stderr, "Error: Fan duty %d%% is below the minimum allowed 25%%\n", percent);
        else
            cctl_stream_fprintf(stderr, "Error: Fan duty %d%% out of range (%d-100)\n", percent, minimum);
        return -1;
    }
    uint8_t raw = (uint8_t)((percent * 255) / 100);

    if (fan_idx == FAN_CPU || fan_idx == FAN_GPU) {
        /* Individual fan duty: use legacy direct EC port I/O to avoid
         * overriding the other fan's automatic thermal curve */
        uint8_t data[2] = { (uint8_t)fan_idx, raw };
        cctl_stream_printf("  Fan %s duty: %d%% (0x%02X)\n",
               fan_idx == FAN_CPU ? "CPU" : "GPU", percent, raw);
        return send_ec_cmd(EC_CMD_FAN_SPEED, data, 2);
    }

    /* Both fans: use tuxedo_io ioctl */
    cctl_stream_printf("  Fans duty: %d%% (0x%02X)\n", percent, raw);
    return fan_set_speeds_tuxedo(raw, raw);
}

/* Direct EC port individual auto restore */
static int legacy_fan_auto(int fan_idx)
{
    /* EC quirk: standalone 0x99 auto-restore uses { 0xFF, fan_idx } order.
     * Do NOT swap to { fan_idx, 0xFF } — that only works after a mode cmd 0x98. */
    uint8_t data[2] = { FAN_DUTY_AUTO, (uint8_t)fan_idx };
    return send_ec_cmd(EC_CMD_FAN_SPEED, data, 2);
}

/* ========================================================================
 * PROFILES
 * ========================================================================

 * Profiles from the Rust codebase (profiles.rs):
 *
 * max (perf_gpu):
 *   GPU=2(performance), turbo=ON, governor=powersave, EPP=performance,
 *   display=auto; built-in profiles leave RAPL limits unchanged.
 *   Governor is powersave because measured performance depends on EPP alone,
 *   and powersave keeps EPP selectable while idling cooler.
 *
 * cpuperf (perf_cpu):
 *   GPU=3(turbo), turbo=ON, governor=powersave, EPP=performance,
 *   display=auto (no RAPL change)
 *
 * balanced:
 *   GPU=3(turbo), turbo=ON, governor=powersave, EPP=balance_performance,
 *   display=auto
 *
 * powersave:
 *   GPU=1(quiet), turbo=OFF, governor=powersave, EPP=balance_power,
 *   display=auto, OEM defaults (PL1=15W, PL2=30W, GPU 70W)
 *
 * silent (eco alias):
 *   GPU=0(silent), turbo=OFF, governor=powersave, EPP=power,
 *   display=auto
 *
 * Fan behavior note:
 *   Applying EC profile code 2 or 3 switches fans to AUTO. Enabling Turbo
 *   does so only when the active EC code is 2 or 3. --nosafe bypasses this.
 */

enum prof_color {
    PROF_COL_RED,
    PROF_COL_YLW,
    PROF_COL_GRN,
    PROF_COL_CYN,
    PROF_COL_CYN_BLD,
    PROF_COL_MAG,
    PROF_COL_BLU,
    PROF_COL_DIM
};

static const char *prof_color_str(enum prof_color c)
{
    switch (c) {
        case PROF_COL_RED:     return C_RED;
        case PROF_COL_YLW:     return C_YLW;
        case PROF_COL_GRN:     return C_GRN;
        case PROF_COL_CYN:     return C_CYN;
        case PROF_COL_CYN_BLD: return C_CYN_BLD;
        case PROF_COL_MAG:     return C_MAG;
        case PROF_COL_BLU:     return C_BLU;
        case PROF_COL_DIM:     return C_DIM;
    }
    return C_RST;
}

struct profile_def {
    const char *name;
    const char *label;
    enum prof_color color;
    const char *help_desc;
    int gpu_profile;
    int turbo;
    const char *gov;
    const char *epp;
    uint8_t pulse_r, pulse_g, pulse_b;
    const char *ec_tdp;
    int rapl_pl1;
    int rapl_pl2;
    const char *rr;
};

static const struct profile_def PROFILES[] = {
    { "max",       "Performance Max + GPU (80W-100W)", PROF_COL_RED,     "Maximum performance (90/115W + GPU 100W)",              2, 1, "powersave",   "performance",         255,   0,   0, "90/115W + GPU 100W", 0, 0, NULL },
    { "cpuperf",   "Performance CPU Only",             PROF_COL_YLW,     "Performance CPU only (45/115W + GPU 70W)",              3, 1, "powersave",   "performance",         255, 110,   0, "45/115W",            0, 0, NULL },
    { "balanced",  "Balanced",                         PROF_COL_GRN,     "Balanced daily use (45/115W + GPU 70W)",                3, 1, "powersave",   "balance_performance", 200,  50, 255, "45/115W",            0, 0, NULL },
    { "powersave", "Powersave",                        PROF_COL_CYN_BLD, "Power saving, turbo off (15/30W + GPU 70W)",            1, 0, "powersave",   "balance_power",         0, 255,   0, "15/30W",             0, 0, "lowest" },
    { "silent",    "Silent",                           PROF_COL_DIM,     "Ultra power saving (15/30W + GPU 70W)",                 0, 0, "powersave",   "power",                 80, 180, 255, "15/30W",             0, 0, NULL },
};

#define USER_PROFILE_FILE "/var/lib/cctl/profiles.conf"
#define LEGACY_USER_PROFILE_FILE "/etc/cctl/profiles.conf"
#define MAX_USER_PROFILES 32
static struct profile_def user_profiles[MAX_USER_PROFILES];
static char user_profile_names[MAX_USER_PROFILES][32];
static char user_profile_governors[MAX_USER_PROFILES][32];
static char user_profile_epps[MAX_USER_PROFILES][32];
static char user_profile_rrs[MAX_USER_PROFILES][32];
static int user_profile_count;
static int user_profiles_loaded;

static int profile_name_valid(const char *name)
{
    if (!name || !*name || strlen(name) >= sizeof(user_profile_names[0])) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!isalnum(*p) && *p != '_' && *p != '-') return 0;
    return 1;
}

struct user_profile_fields {
    char name[32];
    char governor[32];
    char epp[32];
    char refresh[32];
    int gpu_profile;
    int turbo;
    int pl1;
    int pl2;
    int field_count;
};

static int profile_refresh_valid(const char *value)
{
    if (strcmp(value, "skip") == 0 || strcmp(value, "lowest") == 0 ||
        strcmp(value, "highest") == 0)
        return 1;

    int digits = 0, dots = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (isdigit(*p)) {
            digits++;
        } else if (*p == '.') {
            if (++dots > 1) return 0;
        } else {
            return 0;
        }
    }
    if (!digits) return 0;

    char *end = NULL;
    errno = 0;
    double rate = strtod(value, &end);
    return errno == 0 && end != value && *end == '\0' && rate > 0.0;
}

/* Parse and validate one profile row. Returns 1 for a profile, 0 for a
 * comment/blank row, and -1 with a user-facing reason for invalid input. */
static int parse_user_profile_line(const char *line,
                                   struct user_profile_fields *profile,
                                   char *err, size_t err_sz)
{
    const unsigned char *first = (const unsigned char *)line;
    while (*first && isspace(*first)) first++;
    if (!*first || *first == '#') return 0;

    char gpu_text[16], turbo_text[16], pl1_text[16] = "skip";
    char pl2_text[16] = "skip", extra[2];
    snprintf(profile->refresh, sizeof(profile->refresh), "skip");
    profile->gpu_profile = -1;
    profile->turbo = -1;
    profile->pl1 = -1;
    profile->pl2 = -1;
    int fields = sscanf(line, "%31s %15s %15s %31s %31s %15s %15s %31s %1s",
                        profile->name, gpu_text, turbo_text,
                        profile->governor, profile->epp,
                        pl1_text, pl2_text, profile->refresh, extra);
    if (fields != 5 && fields != 7 && fields != 8) {
        snprintf(err, err_sz, "expected 5, 7, or 8 fields");
        return -1;
    }
    profile->field_count = fields;

    if (!profile_name_valid(profile->name)) {
        snprintf(err, err_sz, "profile name must use only letters, numbers, hyphens, or underscores (max 31 characters)");
        return -1;
    }
    if (strcmp(gpu_text, "skip") != 0 &&
        (safe_atoi(gpu_text, &profile->gpu_profile) < 0 ||
         profile->gpu_profile < 0 || profile->gpu_profile > 3)) {
        snprintf(err, err_sz, "EC profile code must be 0-3 or 'skip'");
        return -1;
    }
    if (strcmp(turbo_text, "skip") != 0 &&
        (safe_atoi(turbo_text, &profile->turbo) < 0 ||
         (profile->turbo != 0 && profile->turbo != 1))) {
        snprintf(err, err_sz, "Turbo must be 0, 1, or 'skip'");
        return -1;
    }
    if (strcmp(profile->governor, "skip") != 0) {
        int available = cpu_policy_value_available(CPU_AVAILABLE_GOVERNORS_PATH,
                                                   profile->governor);
        if (available != 1) {
            if (available < 0)
                snprintf(err, err_sz, "cannot read available CPU governors");
            else
                snprintf(err, err_sz, "governor '%s' is unavailable on this system",
                         profile->governor);
            return -1;
        }
    }
    if (strcmp(profile->epp, "skip") != 0) {
        int available = cpu_policy_value_available(CPU_AVAILABLE_EPP_PATH,
                                                   profile->epp);
        if (available != 1) {
            if (available < 0)
                snprintf(err, err_sz, "cannot read available EPP values");
            else
                snprintf(err, err_sz, "EPP value '%s' is unavailable on this system",
                         profile->epp);
            return -1;
        }
    }

    if (fields >= 7) {
        int pl1_max = profile->gpu_profile == 2 || profile->gpu_profile < 0
                    ? RAPL_PL1_MAX_WATTS_MAX : RAPL_PL1_MAX_WATTS;
        if (strcmp(pl1_text, "skip") != 0 &&
            (safe_atoi(pl1_text, &profile->pl1) < 0 || profile->pl1 < 1 ||
             !rapl_pl1_within_cap(profile->pl1, pl1_max))) {
            snprintf(err, err_sz, "PL1 must be 1-%d W for the selected EC profile, or 'skip'",
                     pl1_max);
            return -1;
        }
        if (strcmp(pl2_text, "skip") != 0 &&
            (safe_atoi(pl2_text, &profile->pl2) < 0 || profile->pl2 < 1 ||
             !rapl_pl2_within_cap(profile->pl2))) {
            snprintf(err, err_sz, "PL2 must be 1-%d W or 'skip'",
                     RAPL_PL2_MAX_WATTS);
            return -1;
        }
        if (!rapl_pl2_above_pl1(profile->pl1, profile->pl2)) {
            snprintf(err, err_sz, "PL2 must be greater than PL1");
            return -1;
        }
    }
    if (fields == 8 && !profile_refresh_valid(profile->refresh)) {
        snprintf(err, err_sz, "refresh rate must be a positive number, 'lowest', 'highest', or 'skip'");
        return -1;
    }
    return 1;
}

static int validate_user_profiles_text(const char *text, char *err,
                                       size_t err_sz)
{
    char names[MAX_USER_PROFILES][32];
    int count = 0;
    unsigned int line_number = 1;
    const char *line_start = text;

    while (*line_start) {
        const char *newline = strchr(line_start, '\n');
        size_t len = newline ? (size_t)(newline - line_start) : strlen(line_start);
        const unsigned char *first = (const unsigned char *)line_start;
        const unsigned char *end = first + len;
        while (first < end && isspace(*first)) first++;
        int parsed = 0;
        struct user_profile_fields profile;
        char reason[256];
        if (first < end && *first != '#') {
            if (len > 255) {
                snprintf(err, err_sz, "line %u: profile row is too long (maximum 255 characters)",
                         line_number);
                return -1;
            }
            char line[256];
            memcpy(line, line_start, len);
            if (len > 0 && line[len - 1] == '\r') len--;
            line[len] = '\0';
            parsed = parse_user_profile_line(line, &profile,
                                             reason, sizeof(reason));
        }
        if (parsed < 0) {
            snprintf(err, err_sz, "line %u: %.220s", line_number, reason);
            return -1;
        }
        if (parsed > 0) {
            int duplicate = strcmp(profile.name, "eco") == 0;
            for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++)
                if (strcmp(PROFILES[i].name, profile.name) == 0)
                    duplicate = 1;
            for (int i = 0; i < count; i++)
                if (strcmp(names[i], profile.name) == 0)
                    duplicate = 1;
            if (duplicate) {
                snprintf(err, err_sz, "line %u: profile name '%s' is already used",
                         line_number, profile.name);
                return -1;
            }
            if (count == MAX_USER_PROFILES) {
                snprintf(err, err_sz, "line %u: maximum of %d custom profiles reached",
                         line_number, MAX_USER_PROFILES);
                return -1;
            }
            snprintf(names[count++], sizeof(names[0]), "%s", profile.name);
        }

        if (!newline) break;
        line_start = newline + 1;
        line_number++;
    }
    return 0;
}

static void load_user_profiles(void)
{
    if (user_profiles_loaded) return;
    user_profiles_loaded = 1;
    FILE *fp = fopen(USER_PROFILE_FILE, "r");
    if (!fp) return;

    char line[256];
    while (user_profile_count < MAX_USER_PROFILES && fgets(line, sizeof(line), fp)) {
        struct user_profile_fields profile;
        char parse_error[256];
        if (parse_user_profile_line(line, &profile,
                                    parse_error, sizeof(parse_error)) != 1)
            continue;
        int duplicate = 0;
        if (strcmp(profile.name, "eco") == 0) duplicate = 1; /* reserved alias for built-in silent */
        for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++)
            if (strcmp(PROFILES[i].name, profile.name) == 0) duplicate = 1;
        for (int i = 0; i < user_profile_count; i++)
            if (strcmp(user_profile_names[i], profile.name) == 0) duplicate = 1;
        if (duplicate) continue;

        int i = user_profile_count++;
        snprintf(user_profile_names[i], sizeof(user_profile_names[i]), "%s", profile.name);
        snprintf(user_profile_governors[i], sizeof(user_profile_governors[i]), "%s",
                 strcmp(profile.governor, "skip") == 0 ? "skip" : profile.governor);
        snprintf(user_profile_epps[i], sizeof(user_profile_epps[i]), "%s",
                 strcmp(profile.epp, "skip") == 0 ? "skip" : profile.epp);
        snprintf(user_profile_rrs[i], sizeof(user_profile_rrs[i]), "%s",
                 (profile.field_count == 8 && strcmp(profile.refresh, "skip") != 0)
                 ? profile.refresh : "skip");
        user_profiles[i] = (struct profile_def) {
            .name = user_profile_names[i], .label = user_profile_names[i],
            .color = PROF_COL_BLU, .help_desc = "User-defined profile",
            .gpu_profile = profile.gpu_profile, .turbo = profile.turbo,
            .gov = user_profile_governors[i], .epp = user_profile_epps[i],
            .pulse_r = 0, .pulse_g = 0, .pulse_b = 0, .ec_tdp = "custom",
            .rapl_pl1 = profile.pl1, .rapl_pl2 = profile.pl2,
            .rr = (strcmp(user_profile_rrs[i], "skip") == 0 ? NULL : user_profile_rrs[i])
        };
    }
    fclose(fp);
}

/* Defined in cctl_platform.inc, which is included after this file. Mirrors the
 * visibility rule used by the DISPLAY section (cctl rr / cctl scale). */
static int has_display_support(void);

/* Refresh cell for the profile table: "lowest"/"highest", "<rate>Hz", or "--" when unset. */
static const char *profile_rr_label(const char *rr, char *buf, size_t len)
{
    if (!rr || !*rr || strcmp(rr, "skip") == 0)
        snprintf(buf, len, "--");
    else if (strcmp(rr, "lowest") == 0 || strcmp(rr, "highest") == 0)
        snprintf(buf, len, "%s", rr);
    else
        snprintf(buf, len, "%sHz", rr);
    return buf;
}

static void profile_table_row(const char *color, int name_width, const char *name,
                              const char *ec, const char *turbo, const char *gov,
                              const char *epp, const char *tdp, const char *rr)
{
    cctl_stream_printf("      %s%-*s%s %-7s %-6s %-12s %-19s", color, name_width, name, C_RST,
           ec, turbo, gov, epp);
    if (rr) cctl_stream_printf(" %-19s %s", tdp, rr);
    else cctl_stream_printf(" %s", tdp);
    cctl_stream_printf("\n");
}

static void print_profile_table(void)
{
    int show_rr = has_display_support();
    size_t name_width = strlen("silent/eco");
    for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
        size_t len = strlen(PROFILES[i].name);
        if (len > name_width) name_width = len;
    }
    load_user_profiles();
    for (int i = 0; i < user_profile_count; i++) {
        size_t len = strlen(user_profiles[i].name);
        if (len > name_width) name_width = len;
    }
    if (show_rr)
        cctl_stream_printf("      %s%-*s %-7s %-6s %-12s %-19s %-19s %s%s\n", C_BLD, (int)name_width,
               "Profile", "EC code", "Turbo", "Governor", "EPP", "EC default TDP",
               "Refresh", C_RST);
    else
        cctl_stream_printf("      %s%-*s %-7s %-6s %-12s %-19s %s%s\n", C_BLD, (int)name_width,
               "Profile", "EC code", "Turbo", "Governor", "EPP", "EC default TDP", C_RST);
    cctl_stream_printf("      %s", C_DIM);
    for (size_t i = 0; i < name_width; i++) cctl_stream_printf("─");
    cctl_stream_printf(" ─────── ────── ──────────── ─────────────────── ───────────────────%s%s\n",
           show_rr ? " ───────" : "", C_RST);
    for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
        const struct profile_def *p = &PROFILES[i];
        const char *display_name = strcmp(p->name, "silent") == 0 ? "silent/eco" : p->name;
        char ec_code[8], rr_buf[16];
        snprintf(ec_code, sizeof(ec_code), "%d", p->gpu_profile);
        profile_table_row(prof_color_str(p->color), (int)name_width, display_name,
                          ec_code,
                          p->turbo ? "ON" : "OFF",
                          p->gov,
                          p->epp,
                          p->ec_tdp,
                          show_rr ? profile_rr_label(p->rr, rr_buf, sizeof(rr_buf)) : NULL);
    }
    for (int i = 0; i < user_profile_count; i++) {
        const struct profile_def *p = &user_profiles[i];
        char tdp[32];
        if (p->rapl_pl1 <= 0 && p->rapl_pl2 <= 0) {
            snprintf(tdp, sizeof(tdp), "--");
        } else {
            char pl1[12], pl2[12];
            if (p->rapl_pl1 > 0) snprintf(pl1, sizeof(pl1), "%d", p->rapl_pl1);
            else snprintf(pl1, sizeof(pl1), "--");
            if (p->rapl_pl2 > 0) snprintf(pl2, sizeof(pl2), "%d", p->rapl_pl2);
            else snprintf(pl2, sizeof(pl2), "--");
            snprintf(tdp, sizeof(tdp), "%s/%sW", pl1, pl2);
        }
        char ec_code[16], rr_buf[16];
        if (p->gpu_profile >= 0) snprintf(ec_code, sizeof(ec_code), "%d", p->gpu_profile);
        else snprintf(ec_code, sizeof(ec_code), "--");
        profile_table_row(prof_color_str(p->color), (int)name_width, p->name,
                          ec_code,
                          p->turbo < 0 ? "--" : (p->turbo ? "ON" : "OFF"),
                          strcmp(p->gov, "skip") == 0 ? "--" : p->gov,
                          strcmp(p->epp, "skip") == 0 ? "--" : p->epp,
                          tdp,
                          show_rr ? profile_rr_label(p->rr, rr_buf, sizeof(rr_buf)) : NULL);
    }
    cctl_stream_printf("\n      %sSetting EC codes 2/3 or enabling Turbo (while in EC code 2/3) will set fans to auto unless a fan curve is active; bypass with --nosafe.%s\n",
           C_DIM, C_RST);
    cctl_stream_printf("\n");
}

static const struct profile_def *find_profile(const char *name)
{
    /* Keep the former built-in name working as an alias. */
    if (strcmp(name, "eco") == 0)
        name = "silent";
    for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
        if (strcmp(PROFILES[i].name, name) == 0)
            return &PROFILES[i];
    }
    load_user_profiles();
    for (int i = 0; i < user_profile_count; i++)
        if (strcmp(user_profiles[i].name, name) == 0)
            return &user_profiles[i];
    return NULL;
}

static int profile_apply_refresh_rate(const struct profile_def *p);

static int profile_apply(const struct profile_def *p)
{
    int err = 0;
    cctl_stream_printf("Applying: %s\n", p->label);
    if (p->gpu_profile >= 0)
        set_gpu_profile(p->gpu_profile); /* non-fatal: CPU settings still apply without tuxedo_io */
    if (p->turbo >= 0 && set_turbo(p->turbo) < 0) err++;
    if (strcmp(p->gov, "skip") != 0 && set_governor(p->gov) < 0) err++;
    if (strcmp(p->epp, "skip") != 0 && set_epp(p->epp) < 0) err++;
    if ((p->rapl_pl1 > 0 || p->rapl_pl2 > 0) &&
        set_rapl_limits(p->rapl_pl1, p->rapl_pl2,
                        p->gpu_profile >= 0
                            ? (p->gpu_profile == 2 ? RAPL_PL1_MAX_WATTS_MAX : RAPL_PL1_MAX_WATTS)
                            : rapl_pl1_ceiling()) < 0)
        err++;
    if (profile_apply_refresh_rate(p) < 0)
        err++;
    return err ? -1 : 0;
}

static int ec_profile_needs_fan_auto(int ec_code)
{
    return ec_code == 2 || ec_code == 3;
}

static int profile_requires_fan_auto(const struct profile_def *p)
{
    if (!p) return 0;
    int ec_code = p->gpu_profile;
    if (ec_code < 0 && p->turbo == 1)
        ec_code = cctl_live_ec_state(NULL, NULL);
    return ec_profile_needs_fan_auto(ec_code);
}

static int profile_silent(void)
{
    const struct profile_def *p = find_profile("silent");
    return p ? profile_apply(p) : -1;
}
