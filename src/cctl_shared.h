#ifndef CCTL_SHARED_H
#define CCTL_SHARED_H

#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
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
