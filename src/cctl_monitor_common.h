#ifndef CCTL_MONITOR_COMMON_H
#define CCTL_MONITOR_COMMON_H

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "cctl_shared.h"

static inline double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline double clamp01(double v)
{
    return clampd(v, 0.0, 1.0);
}

static inline uint8_t u8(double v)
{
    return (uint8_t)(clampd(v, 0, 255) + 0.5);
}

static inline double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline void sleep_s(double s)
{
    if (s <= 0) return;
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
}

static inline void ease(double *cur, double target, double dt, double rate)
{
    *cur += (target - *cur) * (1.0 - exp(-dt * rate));
}

static inline bool read_first_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) {
        size_t len = strlen(out);
        while (len && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' '))
            out[--len] = '\0';
    }
    return ok;
}

static inline void write_all(const char *p, size_t n)
{
    while (n) {
        ssize_t written = write(STDOUT_FILENO, p, n);
        if (written <= 0) return;
        p += written;
        n -= (size_t)written;
    }
}

static inline void term_size(int *cols, int *rows)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

#endif
