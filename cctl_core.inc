/* ========================================================================
 * ANSI COLOR SUPPORT
 * ======================================================================== */
static int use_color;
static int json_output;
static int json_first_field;
static int dry_run;

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

enum run_flags {
    RUN_SILENT  = 1 << 0, /* redirect stdout & stderr to /dev/null */
    RUN_CAPTURE = 1 << 1, /* capture stdout into out buffer */
};

/* Unified process spawner: replaces run_cmd, run_cmd_silent, popen, system, and fork/pipe.
 * Returns process exit code (0 on success, non-zero or -1 on failure). */
static int run(char *const argv[], int flags, char *out, size_t out_sz)
{
    if ((flags & RUN_CAPTURE) && out && out_sz > 0)
        out[0] = '\0';
    int pipefd[2] = { -1, -1 };
    if ((flags & RUN_CAPTURE) && pipe(pipefd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
        return -1;
    }

    if (pid == 0) {
        /* Don't inherit any blocked signal mask from the parent */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);

        if (flags & RUN_CAPTURE) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        } else if (flags & RUN_SILENT) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    if (flags & RUN_CAPTURE) {
        close(pipefd[1]);
        size_t total = 0;
        char discard[1024];
        for (;;) {
            char *dst = discard;
            size_t cap = sizeof(discard);
            if (out && out_sz > 0 && total < out_sz - 1) {
                dst = out + total;
                cap = out_sz - 1 - total;
            }
            ssize_t n = read(pipefd[0], dst, cap);
            if (n > 0) {
                if (dst != discard)
                    total += (size_t)n;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        if (out && out_sz > 0) out[total] = '\0';
        close(pipefd[0]);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void run_quiet(char *const argv[]) { int r = run(argv, RUN_SILENT, NULL, 0); (void)r; }

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return 1;
}

static void print_done(void)
{
    puts(dry_run ? "Dry run complete." : "Done.");
}

#define require_root(argc, argv) do { if (!dry_run) self_elevate(argc, argv); } while (0)

/* Parse --nosafe flag and collect up to max_args positional arguments starting at argv[2].
 * Returns number of positional arguments collected, or -1 on unexpected extra argument. */
static int parse_flags(int argc, char **argv, int *nosafe, const char **args, int max_args)
{
    int count = 0;
    if (nosafe) *nosafe = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--nosafe") == 0) {
            if (nosafe) *nosafe = 1;
        } else {
            if (count < max_args) {
                args[count++] = argv[i];
            } else {
                fail("Unexpected extra argument '%s'", argv[i]);
                return -1;
            }
        }
    }
    return count;
}

/* Resolve the absolute path to this running executable */
static int get_self_exe(char *out, size_t sz)
{
    ssize_t n = readlink("/proc/self/exe", out, sz - 1);
    if (n <= 0) return -1;
    out[n] = '\0';
    return 0;
}

/* Re-execute the current command with sudo if not already running as root. */
static void self_elevate(int argc, char **argv)
{
    if (geteuid() == 0) return;

    char exe_path[PATH_MAX];
    const char *bin = argv[0];
    if (get_self_exe(exe_path, sizeof(exe_path)) == 0) {
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
static int kbd_get_brightness(void);
static int kbd_get_color(int *r, int *g, int *b);
static const char *kbd_find_preset_name(int r, int g, int b);
struct kbe_state {
    pid_t pid;
    long starttime;
    char effect[32];
    int orig_r;
    int orig_g;
    int orig_b;
    int orig_bri;
    char resume_effect[32];
};
static int kbe_is_running(struct kbe_state *st);
static int nvidia_find_pci_address(char *buf, size_t bufsz);
static void json_field(const char *key, const char *value);

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
    if (dry_run) {
        printf("Dry run: EC command 0x%02X with %d data byte(s)\n", cmd, len);
        return 0;
    }
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
    if (dry_run) {
        printf("Dry run: write %s = %s\n", path, value);
        return 0;
    }
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    size_t len = strlen(value);
    ssize_t n;
    do {
        n = write(fd, value, len);
    } while (n < 0 && errno == EINTR);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int write_sysfs_int(const char *path, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return write_sysfs(path, buf);
}

/* Read a sysfs file into buf (stripping trailing newlines). Returns bytes read or -1. */
static int read_sysfs_str(const char *path, char *buf, size_t bufsize)
{
    if (!buf || bufsize < 2) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n;
    do {
        n = read(fd, buf, bufsize - 1);
    } while (n < 0 && errno == EINTR);
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
    char *end;
    errno = 0;
    long value = strtol(buf, &end, 10);
    return (errno == 0 && end != buf && *end == '\0') ? value : fallback;
}

#define CPU_SYSFS_DIR "/sys/devices/system/cpu"

static int cpu_index_from_name(const char *name, int *cpu_num)
{
    if (strncmp(name, "cpu", 3) != 0 || !isdigit((unsigned char)name[3]))
        return -1;
    char *end;
    errno = 0;
    long value = strtol(name + 3, &end, 10);
    if (errno != 0 || *end != '\0' || value < 0 || value > INT_MAX)
        return -1;
    *cpu_num = (int)value;
    return 0;
}

static int cpu_sysfs_path(char *out, size_t out_sz, int cpu_num, const char *suffix)
{
    int n = snprintf(out, out_sz, CPU_SYSFS_DIR "/cpu%d/%s", cpu_num, suffix);
    return (n >= 0 && (size_t)n < out_sz) ? 0 : -1;
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
 * `cctl set` records the active profile (line 1), method (line 2), and EC
 * profile code (line 3).
 * `cctl status` displays it, and the
 * RAPL PL1 ceiling consults it (90W unlocked only while EC profile code 2
 * is active). GPU watts are deliberately
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

static int mode_write(const char *profile, const char *method, int ec_profile_code)
{
    if (mkdir(MODE_DIR, 0700) != 0 && errno != EEXIST)
        return -1;
    unlink(MODE_FILE);
    int fd = open(MODE_FILE, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s\n%s\n%d\n", profile, method,
                       ec_profile_code);
    ssize_t w = write(fd, buf, (size_t)len);
    close(fd);
    if (w != len) { unlink(MODE_FILE); return -1; }
    return 0;
}

/* Returns 0 and fills profile/method when a valid mode file exists,
 * -1 otherwise (no file / empty / corrupt). */
static int mode_read(char *profile, size_t psz, char *method, size_t msz,
                     int *ec_profile_code)
{
    if (psz == 0 || msz == 0) return -1;
    profile[0] = '\0';
    method[0] = '\0';
    if (ec_profile_code) *ec_profile_code = -1;
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
    if (ec_profile_code && fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        int code;
        if (safe_atoi(line, &code) == 0 && code >= 0 && code <= 3)
            *ec_profile_code = code;
    }
    fclose(fp); /* also closes fd */
    return profile[0] ? 0 : -1;
}

/* RAPL PL1 ceiling in watts: 45 normally; 90 only while EC profile code 2
 * is active (the high-power EC profile). */
static int rapl_pl1_ceiling(void)
{
    char profile[32] = {0}, method[16] = {0};
    int ec_profile_code = -1;
    if (mode_read(profile, sizeof(profile), method, sizeof(method),
                  &ec_profile_code) == 0 &&
        (strcmp(profile, "max") == 0 || ec_profile_code == 2))
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

/* Read and validate the UEFI Setup variable, optionally returning its blob.
 * Returns MUX_VAL_DGPU / MUX_VAL_MSHYBRID on success, -1 on error. */
static int mux_read_blob(unsigned char *blob)
{
    int fd = open(MUX_VAR_PATH, O_RDONLY);
    if (fd < 0) return -1;

    unsigned char local[MUX_EXPECTED_LEN];
    unsigned char *data = blob ? blob : local;
    size_t total = 0;
    while (total < MUX_EXPECTED_LEN) {
        ssize_t n = read(fd, data + total, MUX_EXPECTED_LEN - total);
        if (n > 0) total += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    unsigned char extra;
    ssize_t trailing;
    do {
        trailing = read(fd, &extra, 1);
    } while (trailing < 0 && errno == EINTR);
    close(fd);

    if (total != MUX_EXPECTED_LEN || trailing != 0) return -1;
    int val = data[MUX_OFFSET];
    if (val != MUX_VAL_DGPU && val != MUX_VAL_MSHYBRID) return -1;
    return val;
}

static int mux_read(void)
{
    return mux_read_blob(NULL);
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

static int mux_show_internal(const char *prefix)
{
    int nvram = mux_read();
    if (nvram < 0) {
        if (json_output) {
            json_field("GPU MUX", "N/A (NVRAM variable not found or unrecognized)");
            return 1;
        }
        printf("%s%sN/A (NVRAM variable not found or unrecognized)%s\n", prefix, C_DIM, C_RST);
        return 1;
    }
    int running = mux_running_mode();
    if (json_output) {
        char value[128];
        snprintf(value, sizeof(value), "%s%s", mux_mode_str(running),
                 nvram != running ? " (pending reboot)" : "");
        json_field("GPU MUX", value);
        return 0;
    }
    const char *col = (running == MUX_VAL_MSHYBRID) ? C_GRN : C_MAG;
    if (running >= 0 && nvram != running)
        printf("%s%s%s%s  %s← %s pending (reboot to apply)%s\n",
               prefix,
               col, mux_mode_str(running), C_RST,
               C_YLW, mux_mode_str(nvram), C_RST);
    else
        printf("%s%s%s%s\n", prefix, col, mux_mode_str(nvram), C_RST);
    return 0;
}

static void mux_show(void)
{
    mux_show_internal("  GPU MUX:       ");
}

static int power_supply_read(const char *name, const char *field, char *out, size_t out_sz)
{
    char path[512];
    int n = snprintf(path, sizeof(path), "/sys/class/power_supply/%s/%s", name, field);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    return read_sysfs_str(path, out, out_sz);
}

static int power_supply_find_type(const char *wanted_type, char *name, size_t name_sz)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return -1;
    struct dirent *ent;
    char type[32];
    int found = -1;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (power_supply_read(ent->d_name, "type", type, sizeof(type)) < 0) continue;
        if (strcmp(type, wanted_type) == 0) {
            if (name && name_sz > 0) {
                size_t len = strlen(ent->d_name);
                if (len >= name_sz) continue;
                memcpy(name, ent->d_name, len + 1);
            }
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

static long power_supply_read_long(const char *name, const char *field, long fallback)
{
    char value[32], *end;
    if (power_supply_read(name, field, value, sizeof(value)) < 0) return fallback;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    return (errno == 0 && end != value && *end == '\0') ? parsed : fallback;
}

/* AC adapter / battery state for the NVRAM-write power guard.
 * Returns 1 = external power connected, 0 = running on battery,
 * -1 = no mains device found (unknown → guard stays off). */
static int ac_online(void)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return -1;
    struct dirent *ent;
    char type[32];
    int found = -1;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (power_supply_read(ent->d_name, "type", type, sizeof(type)) < 0) continue;
        if (strncmp(type, "Mains", 5) != 0) continue;
        found = (power_supply_read_long(ent->d_name, "online", -1) > 0) ? 1 : 0;
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
    if (power_supply_find_type("Battery", bat_name, sizeof(bat_name)) == 0)
        return bat_name;
    snprintf(bat_name, sizeof(bat_name), "BAT0");
    return bat_name;
}

/* Build a sysfs path for the detected battery device. */
static void bat_sysfs(char *out, size_t sz, const char *suffix)
{
    snprintf(out, sz, "/sys/class/power_supply/%s/%s", bat_detect(), suffix);
}

static long bat_read_long(const char *suffix, long fallback)
{
    char path[256];
    bat_sysfs(path, sizeof(path), suffix);
    return read_sysfs_long(path, fallback);
}

static int bat_read_str(const char *suffix, char *out, size_t sz)
{
    char path[256];
    bat_sysfs(path, sizeof(path), suffix);
    return read_sysfs_str(path, out, sz);
}

/* Battery charge percent, or -1 when no battery is present/readable. */
static int battery_pct(void)
{
    long v = bat_read_long("capacity", -1);
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

    /* Read and validate the full blob once; share the same validation as mux status. */
    unsigned char blob[MUX_EXPECTED_LEN];
    int current = mux_read_blob(blob);
    if (current < 0) {
        fprintf(stderr, "Error: NVRAM variable has an unexpected size or MUX value — refusing.\n");
        return 1;
    }
    int fd;

    int target = (current == MUX_VAL_MSHYBRID) ? MUX_VAL_DGPU : MUX_VAL_MSHYBRID;

    /* Confirmation prompt */
    printf("Switch GPU MUX mode from %s%s%s to %s%s%s? [y/N] ",
           C_DIM, mux_mode_str(current), C_RST,
           C_BLD, mux_mode_str(target), C_RST);
    fflush(stdout);

    char ans[32] = {0};
    if (!fgets(ans, sizeof(ans), stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
        printf("Aborted.\n");
        return 0;
    }

    /* Clear immutability (kernel re-marks every var immutable on each boot) */
    char *const chattr_args[] = { "chattr", "-i", MUX_VAR_PATH, NULL };
    run_quiet(chattr_args); /* best-effort; ignore if chattr missing */

    /* Write the toggled blob */
    blob[MUX_OFFSET] = (unsigned char)target;
    fd = open(MUX_VAR_PATH, O_WRONLY);
    if (fd < 0) {
        perror("Error: cannot open NVRAM variable for writing");
        return 1;
    }
    ssize_t written;
    do {
        written = write(fd, blob, sizeof(blob));
    } while (written < 0 && errno == EINTR);
    close(fd);
    if (written != (ssize_t)sizeof(blob)) {
        fprintf(stderr, "Error: short write (%zd/%zu bytes).\n", written, sizeof(blob));
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
        int cpu_num;
        if (cpu_index_from_name(ent->d_name, &cpu_num) < 0) continue;
        if (cpu_sysfs_path(path, sizeof(path), cpu_num, suffix) < 0) continue;

        if (write_sysfs(path, value) == 0)
            touched++;
    }
    closedir(d);
    return touched > 0 ? 0 : -1;
}
