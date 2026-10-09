/* Shared CPU temperature and fan telemetry used by status and GPU monitor. */

#define EC_RAM_PATH "/sys/kernel/debug/ec/ec0/io"

/* Load ec_sys only when root and the EC debugfs endpoint is not available. */
static int ensure_ec_sys(void)
{
    if (access(EC_RAM_PATH, F_OK) == 0) return 0;
    if (geteuid() != 0) return -1;
    static int attempted;
    if (attempted) return -1;
    attempted = 1;
    char *const args[] = { "modprobe", "ec_sys", NULL };
    if (run(args, RUN_SILENT, NULL, 0) != 0) return -1;
    usleep(200000);
    return access(EC_RAM_PATH, F_OK) == 0 ? 0 : -1;
}

/* Duty comes from tuxedo_io; RPM comes from EC RAM. */
static int read_fan_telemetry_ex(int *cpu_pct, int *gpu_pct, int *cpu_rpm,
                                 int *gpu_rpm, int cached_fd)
{
    *cpu_pct = *gpu_pct = *cpu_rpm = *gpu_rpm = 0;
    int got_duty = 0;
    int fd = cached_fd >= 0 ? cached_fd : tuxedo_open_clevo();
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

    (void)ensure_ec_sys();
    int ec_fd = open(EC_RAM_PATH, O_RDONLY);
    if (ec_fd < 0) return got_duty ? 0 : -1;
    unsigned char ram[256] = {0};
    ssize_t n = read(ec_fd, ram, sizeof ram);
    close(ec_fd);
    if (n < 0xD4) return got_duty ? 0 : -1;

    if (!got_duty) {
        int raw = n >= 0xF5 ? ram[0xF4] : 0;
        if (raw == 0) raw = ram[0x89];
        *cpu_pct = (raw * 100) / 255;
    }
    unsigned int cpu_raw = ((unsigned int)ram[0xD0] << 8) | ram[0xD1];
    unsigned int gpu_raw = ((unsigned int)ram[0xD2] << 8) | ram[0xD3];
    *cpu_rpm = cpu_raw ? (int)(EC_FAN_RPM_DIVISOR / cpu_raw) : 0;
    *gpu_rpm = gpu_raw ? (int)(EC_FAN_RPM_DIVISOR / gpu_raw) : 0;
    return 0;
}

static void get_mem_usage(long *used_mb, long *total_mb)
{
    *used_mb = *total_mb = 0;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;
    long total_kb = 0, available_kb = 0;
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        if (sscanf(line, "MemTotal: %ld kB", &total_kb) == 1) continue;
        if (sscanf(line, "MemAvailable: %ld kB", &available_kb) == 1) break;
    }
    fclose(fp);
    if (total_kb > 0 && available_kb <= total_kb) {
        *total_mb = total_kb / 1024;
        *used_mb = (total_kb - available_kb) / 1024;
    }
}

/* Read CPU temperature from tuxedo_io, then fall back to thermal_zone sysfs. */
static int read_cpu_temp_ex(int cached_fd)
{
    int fd = cached_fd >= 0 ? cached_fd : tuxedo_open_clevo();
    if (fd >= 0) {
        int f1 = 0;
        if (ioctl(fd, R_CL_FANINFO1, &f1) >= 0) {
            int temp = (f1 >> 16) & 0xFF;
            if (cached_fd < 0) close(fd);
            if (temp > 0 && temp < 125) return temp;
        } else if (cached_fd < 0) {
            close(fd);
        }
    }

    static char zone_temp_path[512];
    if (zone_temp_path[0]) {
        long milli = read_sysfs_long(zone_temp_path, -1000);
        if (milli >= -50000 && milli <= 150000) return (int)(milli / 1000);
        zone_temp_path[0] = '\0';
    }

    DIR *dir = opendir("/sys/class/thermal");
    if (dir) {
        struct dirent *ent;
        char path[512], type[64];
        while ((ent = readdir(dir))) {
            if (strncmp(ent->d_name, "thermal_zone", 12) != 0) continue;
            snprintf(path, sizeof path, "/sys/class/thermal/%s/type", ent->d_name);
            if (read_sysfs_str(path, type, sizeof type) < 0) continue;
            if (!strstr(type, "x86_pkg") && !strstr(type, "pkg") &&
                !strstr(type, "coretemp")) continue;
            snprintf(zone_temp_path, sizeof zone_temp_path,
                     "/sys/class/thermal/%s/temp", ent->d_name);
            long milli = read_sysfs_long(zone_temp_path, -1000);
            if (milli >= -50000 && milli <= 150000) {
                closedir(dir);
                return (int)(milli / 1000);
            }
            zone_temp_path[0] = '\0';
        }
        closedir(dir);
    }
    long milli = read_sysfs_long("/sys/class/thermal/thermal_zone0/temp", -1000);
    return milli >= 0 ? (int)(milli / 1000) : -1;
}
