#ifndef CCTL_SHARED_H
#define CCTL_SHARED_H

#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CCTL_EC_RAM_PATH "/sys/kernel/debug/ec/ec0/io"
#define CCTL_EC_FAN_RPM_DIVISOR 2156220U

#ifndef R_HWCHECK_CL
#define R_HWCHECK_CL 0x8008EC05
#endif

#ifndef R_CL_FANINFO1
#define R_CL_FANINFO1 0x8008ED10
#endif
#ifndef R_CL_FANINFO2
#define R_CL_FANINFO2 0x8008ED11
#endif

/* Clevo fan telemetry: duty from tuxedo_io, RPM from EC RAM. */
struct cctl_fan_telemetry {
    double cpu_duty_pct, gpu_duty_pct;
    int cpu_rpm, gpu_rpm;
    int cpu_temp_c;
    bool cpu_duty_valid, gpu_duty_valid, rpm_valid, cpu_temp_valid;
};

static inline int tuxedo_open_clevo(void)
{
    int fd = open("/dev/tuxedo_io", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int is_clevo = 0;
    if (ioctl(fd, R_HWCHECK_CL, &is_clevo) < 0 || !is_clevo) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Load ec_sys once per process when EC debugfs is missing and cctl is root. */
static inline int cctl_ensure_ec_sys(void)
{
    static int attempted;
    if (access(CCTL_EC_RAM_PATH, F_OK) == 0) return 0;
    if (geteuid() != 0 || attempted) return -1;
    attempted = 1;

    pid_t child = fork();
    if (child == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        execlp("modprobe", "modprobe", "ec_sys", (char *)NULL);
        _exit(127);
    }
    if (child < 0) return -1;

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }

    struct timespec delay = { .tv_sec = 0, .tv_nsec = 200000000L };
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
    return access(CCTL_EC_RAM_PATH, F_OK) == 0 ? 0 : -1;
}

/* Read both fans from the same two sources used everywhere in cctl.
 * Returns 0 when at least one value was read, -1 when neither source worked.
 * Pass a cached tuxedo fd, or -1 to open and close one here. */
static inline int cctl_read_fan_telemetry(struct cctl_fan_telemetry *out,
                                          int cached_fd)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    int got_duty = 0;
    int fd = cached_fd >= 0 ? cached_fd : tuxedo_open_clevo();
    if (fd >= 0) {
        int f1 = 0, f2 = 0;
        if (ioctl(fd, R_CL_FANINFO1, &f1) >= 0) {
            out->cpu_duty_pct = ((f1 & 0xFF) * 100.0) / 255.0;
            out->cpu_duty_valid = true;
            int temp = (f1 >> 16) & 0xFF;
            if (temp > 0 && temp < 125) {
                out->cpu_temp_c = temp;
                out->cpu_temp_valid = true;
            }
            got_duty = 1;
        }
        if (ioctl(fd, R_CL_FANINFO2, &f2) >= 0) {
            out->gpu_duty_pct = ((f2 & 0xFF) * 100.0) / 255.0;
            out->gpu_duty_valid = true;
            got_duty = 1;
        }
        if (cached_fd < 0) close(fd);
    }

    (void)cctl_ensure_ec_sys();
    int ec_fd = open(CCTL_EC_RAM_PATH, O_RDONLY);
    if (ec_fd < 0) return got_duty ? 0 : -1;

    unsigned char ram[256] = {0};
    ssize_t n = read(ec_fd, ram, sizeof ram);
    close(ec_fd);
    if (n < 0xD4) return got_duty ? 0 : -1;

    if (!got_duty) {
        int raw = n >= 0xF5 ? ram[0xF4] : 0;
        if (raw == 0) raw = ram[0x89];
        out->cpu_duty_pct = (raw * 100.0) / 255.0;
        out->cpu_duty_valid = true;
    }
    unsigned int cpu_raw = ((unsigned int)ram[0xD0] << 8) | ram[0xD1];
    unsigned int gpu_raw = ((unsigned int)ram[0xD2] << 8) | ram[0xD3];
    out->cpu_rpm = cpu_raw ? (int)(CCTL_EC_FAN_RPM_DIVISOR / cpu_raw) : 0;
    out->gpu_rpm = gpu_raw ? (int)(CCTL_EC_FAN_RPM_DIVISOR / gpu_raw) : 0;
    out->rpm_valid = true;
    return 0;
}

/* Read one PCI sysfs attribute into a trimmed string. */
static inline int cctl_pci_read_attr(const char *root, const char *dev,
                                     const char *attr, char *out, size_t out_sz)
{
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s/%s", root, dev, attr);
    if (n < 0 || (size_t)n >= sizeof(path) || out_sz == 0) return -1;

    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(out, (int)out_sz, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

/* Find the first NVIDIA display-class PCI device (VGA or 3D controller).
 * Fills name with the sysfs device name and reports whether every NVIDIA
 * display device found is already in D0. Returns 0 when one was found. */
static inline int cctl_find_nvidia_gpu(char *name, size_t namesz,
                                       const char *root, bool *all_d0)
{
    if (all_d0) *all_d0 = false;
    if (!root) root = "/sys/bus/pci/devices";

    DIR *dir = opendir(root);
    if (!dir) return -1;

    bool found = false, every_d0 = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char value[64];
        if (cctl_pci_read_attr(root, ent->d_name, "vendor",
                               value, sizeof(value)) < 0)
            continue;
        if (strtoul(value, NULL, 16) != 0x10de)
            continue;

        if (cctl_pci_read_attr(root, ent->d_name, "class",
                               value, sizeof(value)) < 0)
            continue;
        if (strncmp(value, "0x0300", 6) != 0 &&
            strncmp(value, "0x0302", 6) != 0)
            continue;

        if (!found && name && namesz) {
            size_t len = strlen(ent->d_name);
            if (len >= namesz) {
                closedir(dir);
                return -1;
            }
            memcpy(name, ent->d_name, len + 1);
        }
        found = true;

        if (cctl_pci_read_attr(root, ent->d_name, "power_state",
                               value, sizeof(value)) < 0 ||
            strcmp(value, "D0") != 0)
            every_d0 = false;
    }
    closedir(dir);

    if (!found) return -1;
    if (all_d0) *all_d0 = every_d0;
    return 0;
}

static inline void json_string(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*p < 0x20) printf("\\u%04x", *p);
                else putchar(*p);
        }
    }
    putchar('"');
}

#endif
