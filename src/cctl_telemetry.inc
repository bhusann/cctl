/* Shared CPU temperature and fan telemetry used by status and GPU monitor. */

#include "cctl_shared.h"

/* Duty comes from tuxedo_io; RPM comes from EC RAM. */
static int read_fan_telemetry_ex(int *cpu_pct, int *gpu_pct, int *cpu_rpm,
                                 int *gpu_rpm, int cached_fd)
{
    struct cctl_fan_telemetry fan;
    int rc = cctl_read_fan_telemetry(&fan, cached_fd);
    *cpu_pct = (int)fan.cpu_duty_pct;
    *gpu_pct = (int)fan.gpu_duty_pct;
    *cpu_rpm = fan.cpu_rpm;
    *gpu_rpm = fan.gpu_rpm;
    return rc;
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
