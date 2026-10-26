#ifndef CCTL_SHARED_H
#define CCTL_SHARED_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

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
