#ifndef CCTL_MONITOR_COMMON_H
#define CCTL_MONITOR_COMMON_H

/*
 * Shared terminal UI primitives for cpumon and gpumon.
 *
 * CCTL_MON_W and CCTL_MON_H must be defined before including this header.
 * The canvas size differs between the two monitors, so the framebuffer is
 * instantiated from those per-translation-unit dimensions.
 */

#ifndef CCTL_MON_W
#error "Define CCTL_MON_W before including cctl_monitor_common.h"
#endif
#ifndef CCTL_MON_H
#error "Define CCTL_MON_H before including cctl_monitor_common.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "cctl_shared.h"

#define PI 3.14159265358979323846
#define NSTOP(a) ((int)(sizeof(a) / sizeof((a)[0])))

#if defined(__GNUC__)
#define PRINTF_LIKE(a, b) __attribute__((format(printf, a, b)))
#else
#define PRINTF_LIKE(a, b)
#endif

/* ------------------------------------------------------------------ math */

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
        if (written < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (written == 0) return;
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

/* ---------------------------------------------------------------- colour */

typedef struct { uint8_t r, g, b; } RGB;
#define RGBc(r, g, b) ((RGB){(r), (g), (b)})

static const RGB C_DIM   = {88, 96, 112};
static const RGB C_LABEL = {128, 138, 156};
static const RGB C_TEXT  = {226, 232, 240};
static const RGB C_CYAN  = {64, 200, 230};
static const RGB C_MAG   = {236, 92, 200};
static const RGB C_AMBER = {255, 184, 48};
static const RGB C_RED   = {255, 84, 72};
static const RGB C_GOLD  = {226, 184, 64};
static const RGB C_FRAME = {70, 100, 125};
static const RGB BG_PANEL = {13, 16, 22};
static const RGB BG_BAR   = {8, 10, 14};
static const RGB BG_CARD  = {17, 21, 28};

static const RGB G_TEMP[] = {{50,120,255},{0,215,215},{90,225,90},{250,215,50},{255,125,30},{255,55,55}};

static RGB mix(RGB a, RGB b, double t)
{
    t = clamp01(t);
    return RGBc(u8(a.r + (b.r - a.r) * t), u8(a.g + (b.g - a.g) * t), u8(a.b + (b.b - a.b) * t));
}

static RGB grad(const RGB *st, int n, double t)
{
    t = clamp01(t);
    if (n == 1) return st[0];
    double p = t * (n - 1);
    int i = (int)p;
    if (i >= n - 1) return st[n - 1];
    return mix(st[i], st[i + 1], p - i);
}

static RGB hsv(double h, double s, double v)
{
    h = fmod(h, 360.0); if (h < 0) h += 360.0;
    double c = v * s, x = c * (1 - fabs(fmod(h / 60.0, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
    if      (h < 60)  { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    return RGBc(u8((r + m) * 255), u8((g + m) * 255), u8((b + m) * 255));
}

static float g_amt = 1.0f;          /* 0 = dull/grey, 1 = full colour */

/* Every colour goes through here: g_amt blends between "dull grey" and the
 * real colour, which gives the monitor's fade-in. */
static RGB tint(RGB c)
{
    double lum = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    double d = lum * 0.5 + 22.0;
    RGB dull = RGBc(u8(d), u8(d), u8(d * 1.08));
    return mix(dull, c, g_amt);
}

/* ------------------------------------------------------------ framebuffer */

typedef struct { uint32_t cp; RGB fg, bg; uint8_t bold, hasbg; } Cell;
static Cell g_fb[CCTL_MON_H][CCTL_MON_W];

static void fb_clear(void)
{
    Cell c = { ' ', {0,0,0}, {0,0,0}, 0, 0 };
    for (int y = 0; y < CCTL_MON_H; y++)
        for (int x = 0; x < CCTL_MON_W; x++)
            g_fb[y][x] = c;
}

static void put(int x, int y, uint32_t cp, RGB fg, int bold)
{
    if (x < 0 || x >= CCTL_MON_W || y < 0 || y >= CCTL_MON_H) return;
    Cell *c = &g_fb[y][x];
    c->cp = cp; c->fg = tint(fg); c->bold = (uint8_t)bold;
}

static void put_bg(int x, int y, RGB bg)
{
    if (x < 0 || x >= CCTL_MON_W || y < 0 || y >= CCTL_MON_H) return;
    g_fb[y][x].bg = tint(bg); g_fb[y][x].hasbg = 1;
}

static void fill_bg(int x, int y, int w, int h, RGB bg)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put_bg(x + i, y + j, bg);
}

static uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t cp; int n;
    if (s[0] < 0x80)                { cp = s[0];        n = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; n = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; n = 3; }
    else                            { cp = s[0] & 0x07; n = 4; }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { n = i; break; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *ps += n;
    return cp;
}

static int utf8_len(const char *s)
{
    int n = 0;
    while (*s) { utf8_next(&s); n++; }
    return n;
}

static int textv(int x, int y, int align, RGB c, int bold, const char *fmt, va_list ap)
{
    char b[256];
    vsnprintf(b, sizeof b, fmt, ap);
    int len = utf8_len(b);
    int sx = align == 1 ? x - len / 2 : (align == 2 ? x - len + 1 : x);
    const char *p = b;
    for (int i = 0; i < len; i++) put(sx + i, y, utf8_next(&p), c, bold);
    return len;
}

#define DEFINE_TEXT(NAME, ALIGN)                                                        \
    static int NAME(int x, int y, RGB c, int bold, const char *fmt, ...) PRINTF_LIKE(5, 6); \
    static int NAME(int x, int y, RGB c, int bold, const char *fmt, ...)                \
    { va_list ap; va_start(ap, fmt); int n = textv(x, y, ALIGN, c, bold, fmt, ap); va_end(ap); return n; }
DEFINE_TEXT(text_l, 0)   /* x = first column   */
DEFINE_TEXT(text_c, 1)   /* x = centre column  */
DEFINE_TEXT(text_r, 2)   /* x = last column    */

static void box(int x, int y, int w, int h, RGB c, int heavy)
{
    uint32_t tl = heavy ? 0x250F : 0x256D, tr = heavy ? 0x2513 : 0x256E;
    uint32_t bl = heavy ? 0x2517 : 0x2570, br = heavy ? 0x251B : 0x256F;
    uint32_t hz = heavy ? 0x2501 : 0x2500, vt = heavy ? 0x2503 : 0x2502;
    for (int i = 1; i < w - 1; i++) {
        put(x + i, y, hz, c, 0);
        put(x + i, y + h - 1, hz, c, 0);
    }
    for (int j = 1; j < h - 1; j++) {
        put(x, y + j, vt, c, 0);
        put(x + w - 1, y + j, vt, c, 0);
    }
    put(x, y, tl, c, 0); put(x + w - 1, y, tr, c, 0);
    put(x, y + h - 1, bl, c, 0); put(x + w - 1, y + h - 1, br, c, 0);
}

static void module(int x, int y, int w, int h, const char *title, RGB c)
{
    fill_bg(x + 1, y + 1, w - 2, h - 2, BG_PANEL);
    box(x, y, w, h, c, 0);
    text_l(x + 2, y, C_TEXT, 1, " %s ", title);
}

/* Horizontal gauge, 1/8-cell resolution, gradient follows position. */
static void hbar(int x, int y, int w, double frac, const RGB *st, int ns)
{
    double lvl = clamp01(frac) * w;
    for (int i = 0; i < w; i++) {
        put_bg(x + i, y, BG_BAR);
        int e = (int)lround(clampd(lvl - i, 0, 1) * 8);
        if (e <= 0) continue;
        put(x + i, y, (uint32_t)(0x2590 - e), grad(st, ns, (i + 0.5) / w), 0);
    }
}

static void hist_push(double *h, int n, double v)
{
    memmove(h, h + 1, (size_t)(n - 1) * sizeof *h);
    h[n - 1] = v;
}

/* ----------------------------------------------------------- frame emitter */

typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s, size_t n)
{
    if (b->n + n > b->cap) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

PRINTF_LIKE(2, 3)
static void bprintf(Buf *b, const char *fmt, ...)
{
    char t[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n > 0) bput(b, t, (size_t)(n < (int)sizeof t ? n : (int)sizeof t - 1));
}

static void bcp(Buf *b, uint32_t cp)
{
    char t[4]; int n;
    if (cp < 0x80)         { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800)   { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    bput(b, t, (size_t)n);
}

static bool same(RGB a, RGB b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static void emit_frame(Buf *b, int ox, int oy, bool absolute)
{
    for (int y = 0; y < CCTL_MON_H; y++) {
        if (absolute) bprintf(b, "\x1b[%d;%dH", oy + y + 1, ox + 1);
        bput(b, "\x1b[0m", 4);
        int bold = 0, hasbg = 0, fgset = 0;
        RGB fg = {0,0,0}, bg = {0,0,0};
        for (int x = 0; x < CCTL_MON_W; x++) {
            const Cell *c = &g_fb[y][x];
            if (c->cp == 0) continue;           /* wide-char continuation cell */
            if (c->bold != bold) {
                bput(b, c->bold ? "\x1b[1m" : "\x1b[22m", c->bold ? 4 : 5);
                bold = c->bold;
            }
            if (c->hasbg) {
                if (!hasbg || !same(bg, c->bg)) {
                    bprintf(b, "\x1b[48;2;%d;%d;%dm", c->bg.r, c->bg.g, c->bg.b);
                    bg = c->bg; hasbg = 1;
                }
            } else if (hasbg) {
                bput(b, "\x1b[49m", 5); hasbg = 0;
            }
            if (c->cp != ' ' || c->hasbg) {
                if (!fgset || !same(fg, c->fg)) {
                    bprintf(b, "\x1b[38;2;%d;%d;%dm", c->fg.r, c->fg.g, c->fg.b);
                    fg = c->fg; fgset = 1;
                }
            }
            bcp(b, c->cp);
        }
        bput(b, "\x1b[0m", 4);
        if (!absolute) bput(b, "\n", 1);
    }
}

/* ---------------------------------------------------------------- terminal */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_quit = 0;
static double g_t0 = 0;
static struct termios g_old;
static bool g_raw = false;

static void write_str(const char *s) { write_all(s, strlen(s)); }

static void term_enter(void)
{
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_old) == 0) {
        struct termios r = g_old;
        r.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        r.c_cc[VMIN] = 0; r.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &r);
        g_raw = true;
    }
    write_str("\x1b[?1049h\x1b[?25l\x1b[2J");
}

static void term_leave(void)
{
    write_str("\x1b[0m\x1b[?25h\x1b[?1049l");
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_old);
        g_raw = false;
    }
}

static void on_signal(int sig) { (void)sig; g_quit = 1; }

#endif
