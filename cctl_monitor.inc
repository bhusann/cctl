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

    static int attempted = 0;
    if (attempted)
        return -1;
    attempted = 1;

    char *const args[] = { "modprobe", "ec_sys", NULL };
    if (run(args, RUN_SILENT, NULL, 0) != 0)
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
    static char cached_zone_path[512] = {0};
    if (cached_zone_path[0] != '\0') {
        long milli = read_sysfs_long(cached_zone_path, -1000);
        if (milli >= -50000 && milli <= 150000)
            return (int)(milli / 1000);
        cached_zone_path[0] = '\0';
    }

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
                snprintf(cached_zone_path, sizeof(cached_zone_path), "/sys/class/thermal/%s/temp", ent->d_name);
                long milli = read_sysfs_long(cached_zone_path, -1000);
                if (milli >= -50000 && milli <= 150000) {
                    closedir(d);
                    return (int)(milli / 1000);
                }
                cached_zone_path[0] = '\0';
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
static long read_fd_long(int fd, long fallback);

static int cpumonitor_json(void)
{
    struct sigaction sa = { .sa_handler = cpumonitor_sigint };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    cpumonitor_running = 1;
    int tuxedo_fd = tuxedo_open_clevo();
    int energy_fd = open("/sys/class/powercap/intel-rapl:0/energy_uj", O_RDONLY);
    int pl1_fd = open("/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw", O_RDONLY);
    int pl2_fd = open("/sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw", O_RDONLY);
    while (cpumonitor_running) {
        long energy1 = energy_fd >= 0 ? read_fd_long(energy_fd, -1) : -1;
        struct timespec t1, t2;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int cpu_usage = get_cpu_usage_pct();
        int temp = read_cpu_temp_ex(tuxedo_fd);
        long used_mb, total_mb;
        get_mem_usage(&used_mb, &total_mb);
        int cpu_pct = 0, gpu_pct = 0, cpu_rpm = 0, gpu_rpm = 0;
        int have_fans = read_fan_telemetry_ex(&cpu_pct, &gpu_pct, &cpu_rpm, &gpu_rpm,
                                               tuxedo_fd) == 0;

        int p_max_mhz = 0, e_max_mhz = 0;
        query_cpu_max_freqs(&p_max_mhz, &e_max_mhz);

        struct timespec delay = { .tv_sec = 1, .tv_nsec = 0 };
        while (cpumonitor_running && nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
        long energy2 = energy_fd >= 0 ? read_fd_long(energy_fd, -1) : -1;
        clock_gettime(CLOCK_MONOTONIC, &t2);
        long pl1 = pl1_fd >= 0 ? read_fd_long(pl1_fd, -1) : -1;
        long pl2 = pl2_fd >= 0 ? read_fd_long(pl2_fd, -1) : -1;
        double elapsed = (double)(t2.tv_sec - t1.tv_sec) +
                         (double)(t2.tv_nsec - t1.tv_nsec) / 1e9;
        double watts = (energy1 >= 0 && energy2 >= energy1 && elapsed > 0) ?
                       (double)(energy2 - energy1) / elapsed / 1000000.0 : -1.0;

        printf("{\"cpu_usage_pct\":");
        if (cpu_usage >= 0) printf("%d", cpu_usage); else fputs("null", stdout);
        printf(",\"cpu_temp_c\":");
        if (temp >= 0) printf("%d", temp); else fputs("null", stdout);
        printf(",\"memory_used_mb\":");
        if (total_mb > 0) printf("%ld", used_mb); else fputs("null", stdout);
        printf(",\"memory_total_mb\":");
        if (total_mb > 0) printf("%ld", total_mb); else fputs("null", stdout);
        printf(",\"package_power_w\":");
        if (watts >= 0.0) printf("%.2f", watts); else fputs("null", stdout);
        printf(",\"pl1_w\":");
        if (pl1 >= 0) printf("%ld", pl1 / 1000000); else fputs("null", stdout);
        printf(",\"pl2_w\":");
        if (pl2 >= 0) printf("%ld", pl2 / 1000000); else fputs("null", stdout);
        printf(",\"p_core_max_mhz\":");
        if (p_max_mhz > 0) printf("%d", p_max_mhz); else fputs("null", stdout);
        printf(",\"e_core_max_mhz\":");
        if (e_max_mhz > 0) printf("%d", e_max_mhz); else fputs("null", stdout);
        printf(",\"cpu_fan_pct\":");
        if (have_fans) printf("%d", cpu_pct); else fputs("null", stdout);
        printf(",\"gpu_fan_pct\":");
        if (have_fans) printf("%d", gpu_pct); else fputs("null", stdout);
        printf(",\"cpu_fan_rpm\":");
        if (have_fans) printf("%d", cpu_rpm); else fputs("null", stdout);
        printf(",\"gpu_fan_rpm\":");
        if (have_fans) printf("%d", gpu_rpm); else fputs("null", stdout);
        puts("}");
        fflush(stdout);

    }
    if (tuxedo_fd >= 0) close(tuxedo_fd);
    if (energy_fd >= 0) close(energy_fd);
    if (pl1_fd >= 0) close(pl1_fd);
    if (pl2_fd >= 0) close(pl2_fd);
    return 0;
}

struct cpu_freq_reader {
    int scaling_fd;
    int cpuinfo_fd;
};

static long read_fd_long(int fd, long fallback)
{
    char buf[32];
    ssize_t n;
    do {
        n = pread(fd, buf, sizeof(buf) - 1, 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return fallback;
    buf[n] = '\0';
    for (char *p = buf; *p; p++)
        if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
    char *end;
    errno = 0;
    long value = strtol(buf, &end, 10);
    return (errno == 0 && end != buf && *end == '\0') ? value : fallback;
}

static void query_cpu_max_freqs_cached(const struct cpu_freq_reader *readers,
                                       int cpu_count, int *p_max_mhz, int *e_max_mhz)
{
    int p_max = 0, e_max = 0;
    for (int i = 0; i < cpu_count; i++) {
        long khz = readers[i].scaling_fd >= 0 ?
                   read_fd_long(readers[i].scaling_fd, -1) : -1;
        if (khz <= 0 && readers[i].cpuinfo_fd >= 0)
            khz = read_fd_long(readers[i].cpuinfo_fd, -1);
        if (khz <= 0) continue;
        int mhz = (int)(khz / 1000);
        if (is_cpu_e_core(i)) {
            if (mhz > e_max) e_max = mhz;
        } else if (mhz > p_max) {
            p_max = mhz;
        }
    }
    *p_max_mhz = p_max;
    *e_max_mhz = e_max;
}

static int cpumonitor(void)
{
    pin_to_e_cores();

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
    struct cpu_freq_reader *freq_readers = calloc((size_t)max_cpus, sizeof(*freq_readers));
    if (!is_e_core || !freqs || !freq_readers) {
        free(is_e_core);
        free(freqs);
        free(freq_readers);
        fprintf(stderr, "Error: out of memory allocating CPU arrays\n");
        return -1;
    }
    for (int i = 0; i < max_cpus; i++) {
        is_e_core[i] = is_cpu_e_core(i);
        freq_readers[i].scaling_fd = -1;
        freq_readers[i].cpuinfo_fd = -1;
        char path[128];
        if (cpu_sysfs_path(path, sizeof(path), i, "cpufreq/scaling_max_freq") == 0)
            freq_readers[i].scaling_fd = open(path, O_RDONLY);
        if (cpu_sysfs_path(path, sizeof(path), i, "cpufreq/cpuinfo_max_freq") == 0)
            freq_readers[i].cpuinfo_fd = open(path, O_RDONLY);
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
        query_cpu_max_freqs_cached(freq_readers, max_cpus, &p_max_mhz, &e_max_mhz);

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
            char gfan[128];
            format_gpu_fan(gfan, sizeof(gfan), gpu_pct, gpu_rpm);
            printf("GPU Fan: %s\n", gfan);
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
    for (int i = 0; i < max_cpus; i++) {
        if (freq_readers[i].scaling_fd >= 0) close(freq_readers[i].scaling_fd);
        if (freq_readers[i].cpuinfo_fd >= 0) close(freq_readers[i].cpuinfo_fd);
    }

    free(is_e_core);
    free(freqs);
    free(freq_readers);

    printf("\n");
    return 0;
}
