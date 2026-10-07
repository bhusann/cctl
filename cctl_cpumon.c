/*
 * cctl_cpumon.c - live terminal CPU monitor for `cctl cpumon`.
 *
 * Build:   make
 * Run:     cctl cpumon [--json] [--interval SEC] [--once]
 *          Press q or Ctrl-C to quit the visual monitor.
 *
 * Needs a terminal of at least 118x36 with truecolor + UTF-8.
 *
 * ---------------------------------------------------------------------------
 * LIVE DATA SOURCES (all plain file reads, nothing is spawned)
 *   /proc/stat                                   per-thread + total utilisation
 *   /sys/devices/system/cpu/cpuN/cpufreq/...     per-core frequency and limits
 *   /sys/devices/cpu_core/cpus, cpu_atom/cpus    P-core / E-core split (hybrid Intel)
 *   /sys/devices/system/cpu/cpuN/topology/...    thread -> physical core mapping
 *   /sys/class/hwmon (coretemp / k10temp)        package temperature + Tjmax
 *   /sys/class/powercap/intel-rapl:0             package watts, PL1, PL2, tau
 *   /proc/meminfo                                RAM used / total
 *
 * MAX FREQUENCY TRACKING
 *   Every poll the monitor re-reads each core's cpuinfo_max_freq and
 *   scaling_max_freq (and base_frequency when turbo is OFF), takes the lowest
 *   of them as that core's current ceiling, and uses the highest ceiling in each
 *   group as the bar scale. Flip turbo on/off, or change scaling_max_freq, and
 *   the bars re-scale on their own. A reading above the ceiling widens it, so
 *   a bar can never overflow.
 *
 * FAN
 *   /dev/tuxedo_io ioctl + EC RAM via debugfs   CPU fan duty and RPM
 * ---------------------------------------------------------------------------
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PI 3.14159265358979323846

/* ------------------------------------------------------------------ layout */
#define W 118                     /* canvas size in cells */
#define H 36
#define HIST 33                   /* usage history samples (= chart width) */
#define PHIST 49                  /* power history samples (= power bar width) */
#define MAX_CPU 128
#define FAN_MAX_RPM 5800.0        /* scales fan spin speed */
#define FRAME_SEC 0.05            /* 20 fps */
#define EC_RAM_PATH "/sys/kernel/debug/ec/ec0/io"
#define EC_FAN_RPM_DIVISOR 2156220
#define R_HWCHECK_CL 0x8008EC05
#define R_CL_FANINFO1 0x8008ED10
#define R_CL_FANINFO2 0x8008ED11

#define X_THM   0
#define X_CPU   11
#define W_CPU   39
#define Y_CPU   3
#define H_CPU   19
#define X_RAM   53
#define W_RAM   11
#define H_RAM   18
#define Y_FAN   22
#define W_FAN   64
#define H_FAN   13
#define X_R     65
#define W_R     53
#define Y_PW    19
#define H_PW    16

/* ------------------------------------------------------------------- types */
typedef struct { uint8_t r, g, b; } RGB;
#define RGBc(r, g, b) ((RGB){(r), (g), (b)})

typedef struct {
    bool    valid;
    char    name[96];
    int     ncpu;                          /* logical CPUs */
    int     ncore;                         /* physical cores */
    bool    hybrid;
    uint8_t cpu_group[MAX_CPU];            /* per logical cpu: 0 = P (or all), 1 = E */
    double  cpu_util[MAX_CPU];             /* 0..100 */
    uint8_t core_group[MAX_CPU];           /* per physical core */
    int     cpu_core[MAX_CPU];             /* logical cpu -> physical core */
    double  core_mhz[MAX_CPU];
    double  cpu_mhz[MAX_CPU];              /* per logical cpu frequency */
    int     grp_ncore[2];
    int     grp_ncpu[2];                   /* threads per group */
    double  grp_max_mhz[2];                /* current ceiling per group */
    double  util_total;                    /* 0..100 */
    bool    turbo_known, turbo_on;
    char    governor[24], epp[40];
    int     ec_code;
    double  prochot_c;
    bool    temp_ok;  double temp_c, tjmax_c;
    bool    power_ok; double pkg_w;
    bool    pl_ok;    double pl1_w, pl2_w, tau_s;
    double  ram_used_gb, ram_total_gb;
    bool    fan_ok;   double fan_duty_pct, fan_rpm;
} CpuSnapshot;

typedef struct {
    bool   once, json;
    double interval;
} Config;

static Config g_cfg = { false, false, 0.5 };

/* ----------------------------------------------------------------- globals */
static CpuSnapshot       g_snap;
static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_quit = 0;
static atomic_int        g_show_threads = 1;    /* toggle physical cores vs threads with 't' */
static double            g_t0 = 0;
static float             g_amt = 1.0f;          /* 0 = dull/grey, 1 = full colour */

/* ----------------------------------------------------------------- helpers */
static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static double clamp01(double v) { return clampd(v, 0.0, 1.0); }
static uint8_t u8(double v) { return (uint8_t)(clampd(v, 0, 255) + 0.5); }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void sleep_s(double s)
{
    if (s <= 0) return;
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
}

static void ease(double *cur, double target, double dt, double rate)
{
    *cur += (target - *cur) * (1.0 - exp(-dt * rate));
}

/* ------------------------------------------------------------------ colour */
static const RGB C_DIM   = {88, 96, 112};
static const RGB C_LABEL = {128, 138, 156};
static const RGB C_TEXT  = {226, 232, 240};
static const RGB C_CYAN  = {64, 200, 230};
static const RGB C_MAG   = {236, 92, 200};
static const RGB C_AMBER = {255, 184, 48};
static const RGB C_RED   = {255, 84, 72};
static const RGB C_GREEN = {90, 225, 120};
static const RGB C_GOLD  = {226, 184, 64};
static const RGB C_FRAME = {70, 100, 125};
static const RGB C_WHITE = {255, 255, 255};
static const RGB BG_PANEL = {13, 16, 22};
static const RGB BG_BAR   = {8, 10, 14};
static const RGB BG_CARD  = {17, 21, 30};

static const RGB G_TEMP[]  = {{50,120,255},{0,215,215},{90,225,90},{250,215,50},{255,125,30},{255,55,55}};
static const RGB G_PCORE[] = {{90,70,255},{170,70,255},{255,70,190},{255,170,90},{255,235,170}};
static const RGB G_ECORE[] = {{0,120,255},{0,200,230},{60,230,160},{190,245,100},{255,240,150}};
static const RGB G_LOAD[]  = {{50,220,255},{80,235,120},{250,225,60},{255,130,40},{255,55,90}};
static const RGB G_RAM[]   = {{40,220,255},{70,130,255},{170,90,255},{255,70,190}};
static const RGB G_DUTY[]  = {{40,200,255},{70,130,255},{150,90,255}};
static const RGB G_TITLE[] = {{0,200,255},{150,90,255},{255,70,190}};
static const RGB G_POW[]   = {{60,220,255},{90,230,110},{255,220,60},{255,130,40},{255,60,80}};
#define NSTOP(a) ((int)(sizeof(a) / sizeof((a)[0])))

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

/* Every colour goes through here: g_amt blends "dull grey" -> real colour, which
 * gives the fade-in on start-up. */
static RGB tint(RGB c)
{
    double lum = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    double d = lum * 0.5 + 22.0;
    RGB dull = RGBc(u8(d), u8(d), u8(d * 1.08));
    return mix(dull, c, g_amt);
}

/* ------------------------------------------------------------- framebuffer */
typedef struct { uint32_t cp; RGB fg, bg; uint8_t bold, hasbg; } Cell;
static Cell g_fb[H][W];

static void fb_clear(void)
{
    Cell c = { ' ', {0,0,0}, {0,0,0}, 0, 0 };
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) g_fb[y][x] = c;
}

static void put(int x, int y, uint32_t cp, RGB fg, int bold)
{
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    Cell *c = &g_fb[y][x];
    c->cp = cp; c->fg = tint(fg); c->bold = (uint8_t)bold;
}

static void put_bg(int x, int y, RGB bg)
{
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    g_fb[y][x].bg = tint(bg); g_fb[y][x].hasbg = 1;
}

static void fill_bg(int x, int y, int w, int h, RGB bg)
{
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) put_bg(x + i, y + j, bg);
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

#if defined(__GNUC__)
#define PRINTF_LIKE(a, b) __attribute__((format(printf, a, b)))
#else
#define PRINTF_LIKE(a, b)
#endif

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
    for (int i = 1; i < w - 1; i++) { put(x + i, y, hz, c, 0); put(x + i, y + h - 1, hz, c, 0); }
    for (int j = 1; j < h - 1; j++) { put(x, y + j, vt, c, 0); put(x + w - 1, y + j, vt, c, 0); }
    put(x, y, tl, c, 0); put(x + w - 1, y, tr, c, 0);
    put(x, y + h - 1, bl, c, 0); put(x + w - 1, y + h - 1, br, c, 0);
}

static void module(int x, int y, int w, int h, const char *title, RGB c)
{
    fill_bg(x + 1, y + 1, w - 2, h - 2, BG_PANEL);
    box(x, y, w, h, c, 0);
    text_l(x + 2, y, C_TEXT, 1, " %s ", title);
}

/* label left, value right, inside a row of width w starting at x */
static void kvw(int x, int y, int w, const char *k, RGB kc, const char *v, RGB vc)
{
    text_l(x, y, kc, 0, "%s", k);
    text_r(x + w - 1, y, vc, 1, "%s", v);
}

/* horizontal gauge, 1/8-cell resolution, gradient follows position */
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

/* ------------------------------------------------------------ UI (animated) */
typedef struct {
    double amt;
    double f_util, util_disp, f_temp, temp_disp, f_ram, f_duty, f_rpm, f_pow, pow_disp;
    double f_thr[MAX_CPU];                        /* eased per-thread utilisation */
    double f_core[MAX_CPU];                       /* eased per-core freq / group max */
    double f_cpu[MAX_CPU];                        /* eased per-thread freq / group max */
    double hist[HIST], phist[PHIST], hist_acc;
    double fan_rps, fan_phase, air_phase, wire_phase, pipe_phase, bus_phase;
    double prev_max[2], flash[2];
} Ui;

static void hist_push(double *h, int n, double v)
{
    memmove(h, h + 1, (size_t)(n - 1) * sizeof *h);
    h[n - 1] = v;
}

static double pow_scale(const CpuSnapshot *s)
{
    return (s->pl_ok && s->pl2_w > 0) ? s->pl2_w : 115.0;
}

static void ui_update(Ui *u, const CpuSnapshot *s, double dt)
{
    bool act = s->valid;
    ease(&u->amt, act ? 1.0 : 0.0, dt, 1.8);

    double util = act ? s->util_total : 0;
    ease(&u->f_util, clamp01(util / 100.0), dt, 6);
    ease(&u->util_disp, util, dt, 5);

    double t_temp = (act && s->temp_ok) ? clamp01((s->temp_c - 20.0) / 80.0) : 0;
    ease(&u->f_temp, t_temp, dt, 4);
    ease(&u->temp_disp, (act && s->temp_ok) ? s->temp_c : 0, dt, 5);

    double t_ram = (act && s->ram_total_gb > 0) ? clamp01(s->ram_used_gb / s->ram_total_gb) : 0;
    ease(&u->f_ram, t_ram, dt, 4);

    bool fan = act && s->fan_ok;
    ease(&u->f_duty, fan ? clamp01(s->fan_duty_pct / 100.0) : 0, dt, 5);
    ease(&u->f_rpm, fan ? clamp01(s->fan_rpm / FAN_MAX_RPM) : 0, dt, 5);

    double pw = (act && s->power_ok) ? s->pkg_w : 0;
    ease(&u->f_pow, clamp01(pw / pow_scale(s)), dt, 6);
    ease(&u->pow_disp, pw, dt, 5);

    for (int i = 0; i < s->ncpu; i++)
        ease(&u->f_thr[i], act ? clamp01(s->cpu_util[i] / 100.0) : 0, dt, 8);
    for (int c = 0; c < s->ncore; c++) {
        double mx = s->grp_max_mhz[s->core_group[c]];
        ease(&u->f_core[c], (act && mx > 0) ? clamp01(s->core_mhz[c] / mx) : 0, dt, 7);
    }
    for (int i = 0; i < s->ncpu; i++) {
        double mx = s->grp_max_mhz[s->cpu_group[i]];
        ease(&u->f_cpu[i], (act && mx > 0) ? clamp01(s->cpu_mhz[i] / mx) : 0, dt, 7);
    }

    for (int g = 0; g < 2; g++) {                /* flash the MAX readout when the ceiling moves */
        double mx = s->grp_max_mhz[g];
        if (u->prev_max[g] == 0) u->prev_max[g] = mx;
        if (fabs(mx - u->prev_max[g]) > 1.0) { u->flash[g] = 1.0; u->prev_max[g] = mx; }
        u->flash[g] = fmax(0.0, u->flash[g] - dt / 1.6);
    }

    /* Fan spin: eased so it spins up / winds down. Visual speed is capped well
     * below the aliasing limit (9 blades @ 20 fps) so it never "wagon-wheels". */
    double target = (fan && s->fan_rpm > 1.0) ? 0.10 + 0.85 * clamp01(s->fan_rpm / FAN_MAX_RPM) : 0.0;
    ease(&u->fan_rps, target, dt, target > 0 ? 2.2 : 0.9);
    if (target == 0.0 && u->fan_rps < 0.02) u->fan_rps = 0.0;
    u->fan_phase = fmod(u->fan_phase + u->fan_rps * dt, 1.0);
    u->air_phase = fmod(u->air_phase + (3.0 + 26.0 * u->fan_rps) * dt, 6.0);
    u->wire_phase = fmod(u->wire_phase + (act ? 5.0 + 45.0 * u->f_pow : 0.0) * dt, 8.0);
    u->pipe_phase = fmod(u->pipe_phase + (1.5 + 5.0 * u->f_temp) * dt, 3.0);
    u->bus_phase = fmod(u->bus_phase + (0.6 + 3.5 * u->f_util) * dt, 1.0);

    u->hist_acc += dt;
    while (u->hist_acc >= 0.4) {
        u->hist_acc -= 0.4;
        hist_push(u->hist, HIST, util / 100.0);
        hist_push(u->phist, PHIST, clamp01(pw / pow_scale(s)));
    }
}

/* ------------------------------------------------------------- big digits */
/* 3x5 pixel font, every pixel drawn two cells wide so it looks square */
static const char *k_big[12][5] = {
    {"###","# #","# #","# #","###"}, {" # ","## "," # "," # ","###"},
    {"###","  #","###","#  ","###"},  {"###","  #","###","  #","###"},
    {"# #","# #","###","  #","  #"},  {"###","#  ","###","  #","###"},
    {"###","#  ","###","# #","###"},  {"###","  #","  #","  #","  #"},
    {"###","# #","###","# #","###"},  {"###","# #","###","  #","###"},
    {"# #","  #"," # ","#  ","# #"},  {"   ","   ","###","   ","   "},
};

static int big_width(const char *s) { int n = (int)strlen(s); return n ? n * 7 - 1 : 0; }

static void draw_big(int x, int y, const char *s, const RGB *st, int ns, double tv, double t)
{
    RGB base = grad(st, ns, tv);
    for (int k = 0; s[k]; k++) {
        int gi = (s[k] >= '0' && s[k] <= '9') ? s[k] - '0' : (s[k] == '%' ? 10 : 11);
        for (int r = 0; r < 5; r++) for (int p = 0; p < 3; p++) {
            if (k_big[gi][r][p] != '#') continue;
            double sh = 0.35 * (1.0 - r / 4.0) + 0.06 * sin(t * 3.0 + (k * 3 + p) * 0.7);
            RGB c = mix(mix(base, RGBc(0, 0, 0), 0.18 * r / 4.0), C_WHITE, sh);
            int cx = x + k * 7 + p * 2;
            put(cx, y + r, 0x2588, c, 0); put(cx + 1, y + r, 0x2588, c, 0);
        }
    }
}

/* ------------------------------------------------------------- drawing bits */
static RGB pin_color(double pulse)
{
    return mix(RGBc(150, 112, 34), RGBc(255, 236, 150), clamp01(pulse));
}

static void draw_fan(int cx, int cy, int rx, int ry, double phase, bool stopped, double t, double glow)
{
    const int N = 9;
    double R = rx + 0.6;
    for (int j = -ry; j <= ry; j++) for (int i = -rx; i <= rx; i++) {
        double dy = j * 2.0;                            /* cells are ~2x taller than wide */
        double d = sqrt((double)(i * i) + dy * dy);
        int x = cx + i, y = cy + j;
        if (d > R) continue;
        double a = atan2(dy, (double)i);
        if (d >= R - 1.1) {                             /* RGB ring */
            RGB rc = stopped ? RGBc(62, 68, 82) : hsv(a * 180.0 / PI + t * 90.0, 0.9, 0.35 + 0.65 * glow);
            put(x, y, 0x2588, rc, 0);
            continue;
        }
        if (d <= R * 0.33) {                            /* hub */
            RGB hc = stopped ? RGBc(120, 128, 145) : RGBc(255, 200, 70);
            put(x, y, (i == 0 && j == 0) ? 0x25CF : 0x2588, (i == 0 && j == 0) ? RGBc(40, 30, 10) : hc, 0);
            continue;
        }
        double u = (a / (2 * PI) - phase) * N + d * 0.10;
        double f = u - floor(u);
        if (f >= 0.5) continue;
        double shade = 1.0 - f / 0.5;
        uint32_t g = f < 0.2 ? 0x2588 : (f < 0.38 ? 0x2593 : 0x2592);
        RGB bc = stopped ? RGBc(92, 100, 116) : mix(RGBc(110, 40, 170), RGBc(120, 230, 255), shade);
        put(x, y, g, bc, 0);
    }
    for (int sy = -1; sy <= 1; sy += 2) for (int sx = -1; sx <= 1; sx += 2)   /* corner screws */
        put(cx + sx * rx, cy + sy * ry, 0x25CB, C_DIM, 0);
}

static void draw_title(const CpuSnapshot *s, double t)
{
    static const char *ttl = "CPU//MON";
    put(0, 0, 0x258C, C_CYAN, 1);
    for (int i = 0; ttl[i]; i++) put(2 + i, 0, (unsigned char)ttl[i], grad(G_TITLE, NSTOP(G_TITLE), i / 8.0), 1);
    text_l(12, 0, C_LABEL, 0, "%s", s->name);

    int off = 0;
    if (s->valid) {
        char b[80];
        snprintf(b, sizeof b, "%s \xC2\xB7 %s", s->governor[0] ? s->governor : "?", s->epp[0] ? s->epp : "-");
        off = text_r(W - 1, 0, C_DIM, 0, "%s", b) + 3;
        if (s->turbo_known) {
            double pl = 0.6 + 0.4 * sin(t * 4.0);
            if (s->turbo_on) text_r(W - 1 - off, 0, mix(C_AMBER, C_WHITE, pl * 0.4), 1, "\xE2\x96\xB2 TURBO ON");
            else             text_r(W - 1 - off, 0, C_LABEL, 1, "\xE2\x96\xBC TURBO OFF");
        }
    } else text_r(W - 1, 0, C_AMBER, 1, "\xE2\x97\x90 SYNCING");
    for (int x = 0; x < W; x++) put(x, 1, 0x2500, grad(G_TITLE, NSTOP(G_TITLE), (double)x / W), 0);
}

/* ------------------------------------------------------------- thermometer */
static void draw_thermo(const CpuSnapshot *s, const Ui *u, double t)
{
    const int x0 = X_THM, TOP = 5, BOT = 18, N = BOT - TOP + 1;
    bool ok = s->valid && s->temp_ok;
    RGB base = grad(G_TEMP, NSTOP(G_TEMP), u->f_temp);
    RGB sh[3] = { mix(base, C_WHITE, 0.30), base, mix(base, RGBc(0, 0, 0), 0.30) };
    text_c(x0 + 6, Y_CPU, C_LABEL, 1, "TEMP");

    /* glass */
    fill_bg(x0 + 5, TOP, 3, N, BG_BAR);
    put(x0 + 4, TOP - 1, 0x256D, C_FRAME, 0); put(x0 + 8, TOP - 1, 0x256E, C_FRAME, 0);
    for (int x = 5; x <= 7; x++) put(x0 + x, TOP - 1, 0x2500, C_FRAME, 0);
    for (int y = TOP; y < BOT; y++) { put(x0 + 4, y, 0x2502, C_FRAME, 0); put(x0 + 8, y, 0x2502, C_FRAME, 0); }
    put(x0 + 3, BOT, 0x256D, C_FRAME, 0); put(x0 + 4, BOT, 0x256F, C_FRAME, 0);
    put(x0 + 8, BOT, 0x2570, C_FRAME, 0); put(x0 + 9, BOT, 0x256E, C_FRAME, 0);
    for (int y = BOT + 1; y <= BOT + 2; y++) { put(x0 + 3, y, 0x2502, C_FRAME, 0); put(x0 + 9, y, 0x2502, C_FRAME, 0); }
    put(x0 + 3, BOT + 3, 0x2570, C_FRAME, 0); put(x0 + 9, BOT + 3, 0x256F, C_FRAME, 0);
    for (int x = 4; x <= 8; x++) put(x0 + x, BOT + 3, 0x2500, C_FRAME, 0);

    /* scale: ticks + coloured labels */
    for (int v = 20; v <= 100; v += 20) {
        double f = (v - 20) / 80.0;
        int y = (BOT - 1) - (int)lround(f * (N - 2));
        RGB c = grad(G_TEMP, NSTOP(G_TEMP), f);
        put(x0 + 4, y, 0x2524, c, 0); put(x0 + 3, y, 0x2500, c, 0);
        text_r(x0 + 2, y, c, 0, "%d", v);
    }

    /* mercury */
    double lvl = (1.5 + (N - 2) * clamp01(u->f_temp)) * 8.0;
    for (int k = 0; k < N; k++) {
        int row = BOT - k;
        int e = (int)lround(clampd(lvl - k * 8, 0, 8));
        if (e <= 0) continue;
        bool cap = (lvl - (k + 1) * 8 <= 0);
        for (int i = 0; i < 3; i++) {
            RGB c = cap ? mix(sh[i], C_WHITE, 0.35) : sh[i];
            put(x0 + 5 + i, row, (uint32_t)(0x2580 + e), c, 0);
        }
    }

    /* bulb: bright, pulses when hot */
    double hot = clamp01((u->temp_disp - 80.0) / 15.0);
    double pulse = hot * (0.5 + 0.5 * sin(t * 7.0)) * 0.35;
    for (int y = BOT + 1; y <= BOT + 2; y++) for (int x = 4; x <= 8; x++) {
        double edge = (x == 4) ? 0 : (x == 8 ? 2 : 1);
        put_bg(x0 + x, y, mix(sh[(int)edge], C_WHITE, pulse));
        put(x0 + x, y, ' ', C_WHITE, 0);
    }
    RGB dark = RGBc(28, 14, 34);
    if (ok) {
        text_c(x0 + (4 + 8) / 2, BOT + 2, dark, 1, "%.0f\xC2\xB0""C", u->temp_disp);
    } else text_c(x0 + (4 + 8) / 2, BOT + 2, dark, 1, "--");
    (void)t;
}

/* ---------------------------------------------------------------- CPU chip */
static void thread_order(const CpuSnapshot *s, int *ord, int *n0, int *n1)
{
    int n = 0;
    for (int c = 0; c < s->ncpu; c++) if (s->cpu_group[c] == 0) ord[n++] = c;
    *n0 = n;
    for (int c = 0; c < s->ncpu; c++) if (s->cpu_group[c] == 1) ord[n++] = c;
    *n1 = n - *n0;
}

static void draw_cpu(const CpuSnapshot *s, const Ui *u, double t)
{
    const int x0 = X_CPU, y0 = Y_CPU, w = W_CPU, h = H_CPU;
    const int bx = x0 + 1, by = y0 + 1, bw = w - 2, bh = h - 2;
    const int cx = x0 + w / 2;
    bool act = s->valid;
    double inten = 0.30 + 0.70 * u->f_util;

    fill_bg(bx + 1, by + 1, bw - 2, bh - 2, BG_CARD);

    /* animated rainbow heavy border */
    for (int i = 1; i < bw - 1; i++) {
        put(bx + i, by, 0x2501, hsv(i * 4.0 + t * 70.0, 0.8, inten), 0);
        put(bx + i, by + bh - 1, 0x2501, hsv((bw + bh + (bw - 1 - i)) * 4.0 + t * 70.0, 0.8, inten), 0);
    }
    for (int j = 1; j < bh - 1; j++) {
        put(bx + bw - 1, by + j, 0x2503, hsv((bw + j) * 4.0 + t * 70.0, 0.8, inten), 0);
        put(bx, by + j, 0x2503, hsv((2 * bw + bh + (bh - 1 - j)) * 4.0 + t * 70.0, 0.8, inten), 0);
    }
    put(bx, by, 0x250F, hsv(t * 70.0, 0.8, inten), 0);
    put(bx + bw - 1, by, 0x2513, hsv(bw * 4.0 + t * 70.0, 0.8, inten), 0);
    put(bx + bw - 1, by + bh - 1, 0x251B, hsv((bw + bh) * 4.0 + t * 70.0, 0.8, inten), 0);
    put(bx, by + bh - 1, 0x2517, hsv((2 * bw + bh) * 4.0 + t * 70.0, 0.8, inten), 0);

    /* gold pins on all four sides; a bright wave runs round the package, faster/brighter with load */
    double amp = 0.15 + 0.85 * u->f_util, spd = 4.0 + 6.0 * u->f_util;
    for (int x = x0 + 2; x <= x0 + w - 3; x += 2) {
        double pt = x - x0, pb = w + h + (x0 + w - 1 - x);
        put(x, y0, 0x2503, pin_color(amp * (0.5 + 0.5 * sin(pt * 0.55 - t * spd))), 0);
        put(x, y0 + h - 1, 0x2503, pin_color(amp * (0.5 + 0.5 * sin(pb * 0.55 - t * spd))), 0);
    }
    for (int y = y0 + 2; y <= y0 + h - 3; y++) {
        double pr = w + (y - y0), pl = 2 * w + h + (h - 1 - (y - y0));
        put(x0 + w - 1, y, 0x2501, pin_color(amp * (0.5 + 0.5 * sin(pr * 0.55 - t * spd))), 0);
        put(x0, y, 0x2501, pin_color(amp * (0.5 + 0.5 * sin(pl * 0.55 - t * spd))), 0);
    }
    put(x0, y0, 0x25E4, C_GOLD, 1);                              /* pin-1 marker */

    /* name + rule */
    text_c(cx, by + 1, C_TEXT, 1, "%.33s", s->name);
    for (int x = bx + 2; x < bx + bw - 2; x++) put(x, by + 2, 0x2500, grad(G_TITLE, NSTOP(G_TITLE), (double)(x - bx) / bw), 0);
    text_c(cx, by + 3, C_LABEL, 0, "\xE2\x96\xAA  C P U   U S A G E  \xE2\x96\xAA");

    /* big usage number */
    char b[32];
    if (act) snprintf(b, sizeof b, "%d%%", (int)lround(clampd(u->util_disp, 0, 100)));
    else     snprintf(b, sizeof b, "--");
    draw_big(cx - big_width(b) / 2, by + 4, b, G_LOAD, NSTOP(G_LOAD), u->f_util, t);

    /* info line */
    double avg = 0; int na = 0;
    for (int c = 0; c < s->ncore; c++) if (s->core_mhz[c] > 0) { avg += s->core_mhz[c]; na++; }
    if (act && na) text_c(cx, by + 9, C_LABEL, 0, "%dC / %dT  \xC2\xB7  avg %.2f GHz", s->ncore, s->ncpu, avg / na / 1000.0);
    else           text_c(cx, by + 9, C_DIM, 0, "-- / -- \xC2\xB7 avg --");

    /* usage history, 3 rows tall (24 levels) */
    int hx = cx - HIST / 2;
    for (int i = 0; i < HIST; i++) {
        double v = u->hist[i];
        RGB col = grad(G_LOAD, NSTOP(G_LOAD), v);
        double lvl = v * 24.0;
        for (int k = 0; k < 3; k++) {
            int row = by + 12 - k;
            int e = (int)lround(clampd(lvl - k * 8, 0, 8));
            if (e <= 0) { if (k == 0) put(hx + i, row, 0x2581, C_DIM, 0); continue; }
            put(hx + i, row, (uint32_t)(0x2580 + e), mix(col, RGBc(40, 50, 70), 0.30 * (2 - k) / 2.0), 0);
        }
    }

    /* per-thread equaliser: P threads | E threads, 2 rows tall */
    int ord[MAX_CPU], n0, n1;
    thread_order(s, ord, &n0, &n1);
    bool sep = n0 > 0 && n1 > 0;
    int total = s->ncpu + (sep ? 1 : 0), maxc = bw - 2;
    int cols = total < maxc ? total : maxc;
    int ex = cx - cols / 2, ey = by + 15;
    int p0w = 0, p1w = 0;
    for (int c = 0; c < cols; c++) {
        double v; int g;
        if (sep && total <= maxc) {
            if (c == n0) { put(ex + c, ey, 0x2502, C_FRAME, 0); put(ex + c, ey + 1, 0x2502, C_FRAME, 0); continue; }
            int idx = c < n0 ? c : c - 1;
            v = u->f_thr[ord[idx]]; g = s->cpu_group[ord[idx]];
        } else {                                              /* too many threads: bin them */
            int lo = c * s->ncpu / cols, hi = (c + 1) * s->ncpu / cols;
            if (hi <= lo) hi = lo + 1;
            double acc = 0; for (int k = lo; k < hi; k++) acc += u->f_thr[ord[k]];
            v = acc / (hi - lo); g = s->cpu_group[ord[lo]];
        }
        if (g == 0) p0w++; else p1w++;
        RGB col = grad(g ? G_ECORE : G_PCORE, 5, v);
        double lvl = v * 16.0;
        for (int k = 0; k < 2; k++) {
            int row = ey + 1 - k;
            int e = (int)lround(clampd(lvl - k * 8, 0, 8));
            if (e <= 0) { if (k == 0) put(ex + c, row, 0x2581, C_DIM, 0); continue; }
            put(ex + c, row, (uint32_t)(0x2580 + e), col, 0);
        }
    }
    if (s->hybrid && sep && total <= maxc) {
        int pc = ex + p0w / 2, ecx = ex + p0w + 1 + p1w / 2;
        if (p0w >= 9) text_c(pc, ey + 2, grad(G_PCORE, 5, 0.6), 0, "P threads"); else text_c(pc, ey + 2, grad(G_PCORE, 5, 0.6), 0, "P");
        if (p1w >= 9) text_c(ecx, ey + 2, grad(G_ECORE, 5, 0.6), 0, "E threads"); else text_c(ecx, ey + 2, grad(G_ECORE, 5, 0.6), 0, "E");
    } else text_c(cx, ey + 2, C_DIM, 0, "threads");
}

/* --------------------------------------------------------------------- RAM */
static void draw_ram(const CpuSnapshot *s, const Ui *u, double t)
{
    const int x0 = X_RAM, y0 = Y_CPU, w = W_RAM, h = H_RAM;
    bool act = s->valid && s->ram_total_gb > 0;
    char b[32];
    fill_bg(x0 + 1, y0 + 1, w - 2, h - 2, BG_CARD);
    box(x0, y0, w, h, grad(G_RAM, NSTOP(G_RAM), 0.65), 1);
    text_c(x0 + w / 2, y0, C_TEXT, 1, " RAM ");

    if (act) snprintf(b, sizeof b, "%.1f GB", s->ram_used_gb); else snprintf(b, sizeof b, "--");
    text_c(x0 + w / 2, y0 + 1, C_TEXT, 1, "%s", b);
    if (act) snprintf(b, sizeof b, "(%.1f GB)", s->ram_total_gb); else snprintf(b, sizeof b, "(--)");
    text_c(x0 + w / 2, y0 + 2, C_DIM, 0, "%s", b);

    /* vertical usage bar fills the remaining module area */
    const int bh = 14, ybot = y0 + 3 + bh - 1, fx = x0 + 1;
    double lvl = clamp01(u->f_ram) * bh * 8;
    for (int k = 0; k < bh; k++) {
        int row = ybot - k;
        bool tick = (k % 3) == 0;
        put(fx, row, tick ? 0x251C : 0x2502, C_FRAME, 0);
        put(fx + 8, row, tick ? 0x2524 : 0x2502, C_FRAME, 0);
        for (int i = 1; i <= 7; i++) put_bg(fx + i, row, BG_BAR);
        int e = (int)lround(clampd(lvl - k * 8, 0, 8));
        if (e <= 0) continue;
        RGB col = grad(G_RAM, NSTOP(G_RAM), (k + 0.5) / bh);
        if (lvl - (k + 1) * 8 <= 0) col = mix(col, C_WHITE, 0.35);
        for (int i = 1; i <= 7; i++) put(fx + i, row, (uint32_t)(0x2580 + e), col, 0);
    }
    /* gold horizontal pins on the RAM module's left long side */
    double amp = 0.15 + 0.85 * u->f_ram;
    for (int y = y0 + 2; y < y0 + h - 1; y += 2) {
        double pulse = amp * (0.5 + 0.5 * sin(y * 0.8 - t * 6.0));
        put(x0 - 1, y, 0x2550, pin_color(pulse), 1);
    }
}

/* memory bus between package and RAM */
static void draw_bus(const Ui *u)
{
    for (int i = 0; i < 7; i++) {
        int y = Y_CPU + 3 + i * 2;
        int dir = (i & 1) ? -1 : 1;
        RGB c = (i & 1) ? C_MAG : C_CYAN;
        double q = fmod(u->bus_phase + i * 0.29, 1.0);
        int pos = (int)(q * 3.0); if (dir < 0) pos = 2 - pos;
        for (int k = 0; k < 3; k++) {
            if (k == pos) put(X_CPU + W_CPU + k, y, 0x25CF, mix(c, C_WHITE, 0.6), 1);
            else          put(X_CPU + W_CPU + k, y, 0x2500, mix(C_FRAME, c, 0.35), 0);
        }
    }
}

/* heat pipes from package to fan */
static void draw_pipes(const CpuSnapshot *s, const Ui *u)
{
    static const int px[3] = { X_CPU + 9, X_CPU + 19, X_CPU + 29 };
    RGB base = mix(RGBc(150, 95, 55), grad(G_TEMP, NSTOP(G_TEMP), u->f_temp), 0.55);
    for (int i = 0; i < 3; i++) {
        for (int y = Y_CPU + H_CPU; y < Y_FAN; y++) {
            double q = fmod((y - Y_CPU) - u->pipe_phase + i * 1.1, 3.0); if (q < 0) q += 3.0;
            if (s->valid && q < 1.0) put(px[i], y, 0x25CF, mix(base, C_WHITE, 0.6), 1);
            else                     put(px[i], y, 0x2503, base, 0);
        }
        put(px[i], Y_FAN, 0x2533, base, 0);
    }
}

/* ---------------------------------------------------------------------- FAN */
static void draw_fan_panel(const CpuSnapshot *s, const Ui *u, double t)
{
    bool act = s->valid && s->fan_ok;
    bool stopped = u->fan_rps < 0.03;
    char b[32];
    int x0 = 0, y0 = Y_FAN;
    module(x0, y0, W_FAN, H_FAN, "CPU FAN", C_FRAME);

    draw_fan(x0 + 12, y0 + 6, 10, 5, u->fan_phase, stopped, t, clamp01(u->f_rpm * 1.3));

    int mx = x0 + 34;
    text_c(mx, y0 + 2, C_LABEL, 0, "FAN DUTY");
    if (act) snprintf(b, sizeof b, "%.0f %%", s->fan_duty_pct); else snprintf(b, sizeof b, "-- %%");
    text_c(mx, y0 + 3, C_TEXT, 1, "%s", b);
    hbar(mx - 7, y0 + 4, 14, u->f_duty, G_DUTY, NSTOP(G_DUTY));
    text_c(mx, y0 + 6, C_LABEL, 0, "SPEED");
    if (act) snprintf(b, sizeof b, "%.0f", s->fan_rpm); else snprintf(b, sizeof b, "--");
    text_c(mx, y0 + 7, C_TEXT, 1, "%s RPM", b);
    hbar(mx - 7, y0 + 8, 14, u->f_rpm, G_LOAD, NSTOP(G_LOAD));
    if (!act)         text_c(mx, y0 + 10, C_DIM, 0, "\xE2\x96\xA0 NO DATA");
    else if (stopped) text_c(mx, y0 + 10, C_AMBER, 1, "\xE2\x96\xA0 STOP");
    else              text_c(mx, y0 + 10, C_GREEN, 1, "\xE2\x97\x8F SPIN");

    /* heatsink fins + airflow that warms as it passes through */
    text_l(x0 + 45, y0 + 1, C_DIM, 0, "AIRFLOW \xE2\x96\xB8");
    RGB hot = grad(G_TEMP, NSTOP(G_TEMP), clamp01(u->f_temp * 0.8 + 0.2));
    for (int ri = 0; ri < 5; ri++) {
        int y = y0 + 2 + ri * 2;
        for (int x = x0 + 45; x <= x0 + 61; x++) {
            bool fin = x >= x0 + 51 && x <= x0 + 57 && ((x - x0) % 2 == 1);
            if (fin) continue;
            if (u->fan_rps < 0.03) continue;
            double q = fmod((x - x0) - u->air_phase + ri * 1.7, 6.0); if (q < 0) q += 6.0;
            double heat = clamp01(((x - x0) - 50) / 9.0);
            RGB c = mix(C_CYAN, hot, heat);
            if (q < 1.0)      put(x, y, 0x25B8, mix(c, C_WHITE, 0.4), 1);
            else if (q < 2.0) put(x, y, 0x00B7, c, 0);
        }
    }
    for (int y = y0 + 2; y <= y0 + 10; y++)
        for (int x = x0 + 51; x <= x0 + 57; x += 2) {
            double k = (y - y0 - 2) / 8.0;
            RGB c = mix(mix(RGBc(120, 130, 150), RGBc(210, 215, 225), k), hot, 0.45 * u->f_temp + 0.1);
            put(x, y, 0x2588, c, 0);
        }
}

/* -------------------------------------------------------------- core panels */
typedef struct {
    int cpu;
    int core;
    char lbl[16];
    double mhz;
    double f;
} CoreItem;

static void draw_group(const CpuSnapshot *s, const Ui *u, int y, int rows, int g, bool show_threads, double t)
{
    CoreItem items[MAX_CPU];
    int n = 0;
    const RGB *gr = g ? G_ECORE : G_PCORE;
    const char *title;
    if (s->hybrid) {
        if (show_threads) title = g ? "E-THREADS" : "P-THREADS";
        else              title = g ? "E-CORES" : "P-CORES";
    } else {
        title = show_threads ? "THREADS" : "CORES";
    }
    char lc = s->hybrid ? (g ? 'E' : 'P') : 'C';
    int x = X_R, h = rows + 3;
    bool act = s->valid;

    module(x, y, W_R, h, title, grad(gr, 5, 0.45));

    if (show_threads) {
        int core_idx_in_grp = 0;
        for (int c = 0; c < s->ncore; c++) {
            if (s->core_group[c] != g) continue;
            int thrs[MAX_CPU], nthr = 0;
            for (int cpu = 0; cpu < s->ncpu; cpu++) {
                if (s->cpu_core[cpu] == c) thrs[nthr++] = cpu;
            }
            if (nthr == 0) {
                if (n < MAX_CPU) {
                    items[n].cpu = -1;
                    items[n].core = c;
                    snprintf(items[n].lbl, sizeof items[n].lbl, "%c%d", lc, core_idx_in_grp);
                    items[n].mhz = s->core_mhz[c];
                    items[n].f = u->f_core[c];
                    n++;
                }
            } else {
                for (int ti = 0; ti < nthr && n < MAX_CPU; ti++) {
                    int cpu = thrs[ti];
                    items[n].cpu = cpu;
                    items[n].core = c;
                    if (nthr > 1) {
                        snprintf(items[n].lbl, sizeof items[n].lbl, "%c%d%d", lc, core_idx_in_grp, ti + 1);
                    } else {
                        snprintf(items[n].lbl, sizeof items[n].lbl, "%c%d", lc, core_idx_in_grp);
                    }
                    items[n].mhz = s->cpu_mhz[cpu] > 0 ? s->cpu_mhz[cpu] : s->core_mhz[c];
                    items[n].f = u->f_cpu[cpu];
                    n++;
                }
            }
            core_idx_in_grp++;
        }
    } else {
        int core_idx_in_grp = 0;
        for (int c = 0; c < s->ncore && n < MAX_CPU; c++) {
            if (s->core_group[c] != g) continue;
            items[n].cpu = -1;
            items[n].core = c;
            snprintf(items[n].lbl, sizeof items[n].lbl, "%c%d", lc, core_idx_in_grp);
            items[n].mhz = s->core_mhz[c];
            items[n].f = u->f_core[c];
            n++;
            core_idx_in_grp++;
        }
    }

    double mx = s->grp_max_mhz[g], avg = 0;
    int navg = 0;
    for (int k = 0; k < n; k++) if (items[k].mhz > 0) { avg += items[k].mhz; navg++; }
    if (navg) avg /= navg;

    /* header: current ceiling (flashes when it changes) */
    text_l(x + 2, y + 1, C_LABEL, 0, "MAX");
    if (act && mx > 0) text_l(x + 6, y + 1, mix(C_CYAN, C_WHITE, u->flash[g]), 1, "%.0f MHz", mx);
    else               text_l(x + 6, y + 1, C_DIM, 0, "--");
    text_l(x + 20, y + 1, C_LABEL, 0, "AVG");
    if (act && navg > 0) text_l(x + 24, y + 1, C_TEXT, 1, "%.0f MHz", avg); else text_l(x + 24, y + 1, C_DIM, 0, "--");
    if (act && s->turbo_known) {
        if (s->turbo_on) text_r(x + W_R - 3, y + 1, C_AMBER, 1, "\xE2\x96\xB2 TURBO");
        else             text_r(x + W_R - 3, y + 1, C_LABEL, 0, "\xE2\x96\xBC NO TURBO");
    }

    int shown = rows * 2;
    for (int r = 0; r < rows; r++) for (int c = 0; c < 2; c++) {
        int k = c * rows + r;
        if (k >= n || k >= shown) continue;
        const CoreItem *it = &items[k];
        int cx = x + 2 + c * 26, cy = y + 2 + r;
        double f = it->f;
        text_l(cx, cy, mix(C_DIM, grad(gr, 5, 0.6), clamp01(f * 1.2)), 1, "%-3s", it->lbl);
        hbar(cx + 4, cy, 14, f, gr, 5);
        if (act && it->mhz > 0) text_r(cx + 22, cy, mix(C_LABEL, C_TEXT, f), f > 0.8, "%4.0f", it->mhz);
        else                    text_r(cx + 22, cy, C_DIM, 0, "--");
    }
    if (n > shown) text_r(x + W_R - 3, y + h - 1, C_DIM, 0, " +%d more ", n - shown);
    (void)t;
}

/* ------------------------------------------------------------------- power */
static void draw_power(const CpuSnapshot *s, const Ui *u, double t)
{
    int x = X_R, y = Y_PW;
    bool act = s->valid && s->power_ok;
    char b[48];
    module(x, y, W_R, H_PW, "POWER DELIVERY", C_FRAME);

    text_l(x + 2, y + 1, C_LABEL, 0, "PACKAGE POWER");
    if (act) snprintf(b, sizeof b, "%d", (int)lround(clampd(u->pow_disp, 0, 999))); else snprintf(b, sizeof b, "--");
    draw_big(x + 2, y + 2, b, G_POW, NSTOP(G_POW), clamp01(u->f_pow / 0.87), t);
    text_l(x + 2 + big_width(b) + 1, y + 6, C_TEXT, 1, "W");

    /* PL1 / PL2 readouts */
    int rx = x + 26, rw = 25;
    if (s->pl_ok) {
        snprintf(b, sizeof b, "%.0f W", s->pl2_w); kvw(rx, y + 1, rw, "PL2 (burst)", C_LABEL, b, C_MAG);
        snprintf(b, sizeof b, "%.0f W", s->pl1_w); kvw(rx, y + 2, rw, "PL1 (sustained)", C_LABEL, b, C_CYAN);
        if (s->tau_s > 0) snprintf(b, sizeof b, "%.1f s", s->tau_s); else snprintf(b, sizeof b, "--");
        kvw(rx, y + 3, rw, "TAU", C_LABEL, b, C_TEXT);
    } else {
        kvw(rx, y + 1, rw, "PL2 (burst)", C_LABEL, "--", C_DIM);
        kvw(rx, y + 2, rw, "PL1 (sustained)", C_LABEL, "--", C_DIM);
        kvw(rx, y + 3, rw, "TAU", C_LABEL, "--", C_DIM);
    }
    if (act && s->pl_ok) { snprintf(b, sizeof b, "%+.0f W", s->pl2_w - s->pkg_w); kvw(rx, y + 4, rw, "HEADROOM", C_LABEL, b, C_TEXT); }
    else                   kvw(rx, y + 4, rw, "HEADROOM", C_LABEL, "--", C_DIM);
    const char *st = "--"; RGB sc = C_DIM;
    if (act) {
        bool pw = s->power_ok && s->pl_ok;
        double prochot = s->prochot_c > 0 ? s->prochot_c : (s->ec_code == 2 ? 97.0 : 87.0);
        if (s->temp_ok && (s->temp_c >= prochot || (s->tjmax_c > 0 && s->temp_c >= s->tjmax_c - 3)))
            { st = "\xE2\x96\xB2 PROCHOT THROTTLE"; sc = C_RED; }
        else if (pw && s->pkg_w >= s->pl2_w * 0.95)
            { st = "\xE2\x96\xB2 PL2 LIMIT";        sc = C_MAG; }
        else if (pw && s->pkg_w > s->pl1_w * 1.02)
            { st = "\xE2\x96\xB2 BOOST";            sc = C_AMBER; }
        else if (pw && s->pkg_w >= s->pl1_w * 0.90 && s->util_total > 60)
            { st = "\xE2\x96\xA0 PL1 LIMIT";        sc = C_AMBER; }
        else if (s->turbo_known && !s->turbo_on && s->util_total >= 50 && (!pw || s->pkg_w < s->pl1_w * 0.90))
            { st = "\xE2\x96\xA0 SOFT THROTTLE";    sc = C_AMBER; }
        else if (s->util_total < 10)
            { st = "\xE2\x97\x8F IDLE";             sc = C_CYAN; }
        else
            { st = "\xE2\x97\x8F NORMAL";           sc = C_GREEN; }
    }
    kvw(rx, y + 5, rw, "STATE", C_LABEL, st, sc);

    /* power bar with PL1 / PL2 markers */
    int bx = x + 2, bw = PHIST;
    double sc_w = pow_scale(s);
    if (s->pl_ok) {
        int p1 = (int)lround(clamp01(s->pl1_w / sc_w) * (bw - 1)), p2 = (int)lround(clamp01(s->pl2_w / sc_w) * (bw - 1));
        put(bx + p1, y + 7, 0x25BC, C_CYAN, 1);
        put(bx + p2, y + 7, 0x25BC, C_MAG, 1);
        if (abs(p2 - p1) > 7) text_c(bx + p1, y + 6 + 0, C_CYAN, 0, "PL1");
        text_c(bx + p2 - 1, y + 6 + 0, C_MAG, 0, "PL2");
    }
    hbar(bx, y + 8, bw, u->f_pow, G_POW, NSTOP(G_POW));
    if (s->pl_ok) {
        int p1 = (int)lround(clamp01(s->pl1_w / sc_w) * (bw - 1)), p2 = (int)lround(clamp01(s->pl2_w / sc_w) * (bw - 1));
        put(bx + p1, y + 8, 0x2502, C_CYAN, 1);
        put(bx + p2, y + 8, 0x2502, C_MAG, 1);
    }
    text_l(bx, y + 9, C_DIM, 0, "0 W");
    text_r(bx + bw - 1, y + 9, C_DIM, 0, "%.0f W", sc_w);

    /* VRM -> package wires; pulses flow faster with draw */
    static const RGB wc[3] = {{235,190,40},{120,124,136},{235,190,40}};
    text_l(bx, y + 11, C_AMBER, 1, "VRM");
    for (int r = 0; r < 3; r++) {
        for (int xx = bx + 5; xx <= bx + bw - 7; xx++) {
            if (!act) { put(xx, y + 10 + r, 0x2500, wc[r], 0); continue; }
            double q = fmod(r == 1 ? xx + u->wire_phase : xx - u->wire_phase, 8.0);
            if (q < 0) q += 8.0;
            if (q < 1.0)      put(xx, y + 10 + r, 0x25CF, mix(wc[r], C_WHITE, 0.7), 1);
            else if (q < 2.0) put(xx, y + 10 + r, 0x2501, mix(wc[r], C_WHITE, 0.35), 0);
            else              put(xx, y + 10 + r, 0x2501, wc[r], 0);
        }
    }
    text_r(bx + bw - 1, y + 11, C_CYAN, 1, "\xE2\x96\xB6 CPU");

    /* power history, 2 rows tall */
    for (int i = 0; i < PHIST; i++) {
        double v = u->phist[i];
        RGB col = grad(G_POW, NSTOP(G_POW), v / 0.87);
        double lvl = v * 16.0;
        for (int k = 0; k < 2; k++) {
            int row = y + 14 - k;
            int e = (int)lround(clampd(lvl - k * 8, 0, 8));
            if (e <= 0) { if (k == 0) put(bx + i, row, 0x2581, C_DIM, 0); continue; }
            put(bx + i, row, (uint32_t)(0x2580 + e), col, 0);
        }
    }
}

static void render(const CpuSnapshot *s, const Ui *u, double t)
{
    g_amt = (float)u->amt;
    fb_clear();
    draw_title(s, t);
    draw_thermo(s, u, t);
    draw_bus(u);
    draw_cpu(s, u, t);
    draw_ram(s, u, t);
    draw_pipes(s, u);
    draw_fan_panel(s, u, t);

    /* core panels: P on top, E below; rows are shared out if there are lots of cores */
    bool show_threads = atomic_load(&g_show_threads) != 0;
    int n0 = show_threads ? s->grp_ncpu[0] : s->grp_ncore[0];
    int n1 = show_threads ? s->grp_ncpu[1] : s->grp_ncore[1];
    if (n0 > 0 && n1 > 0) {
        int r0 = (n0 + 1) / 2, r1 = (n1 + 1) / 2, avail = (Y_PW - Y_CPU) - 6;
        if (r0 + r1 > avail) { r0 = (int)fmax(1, avail * r0 / (double)(r0 + r1)); r1 = avail - r0; }
        draw_group(s, u, Y_CPU, r0, 0, show_threads, t);
        draw_group(s, u, Y_CPU + r0 + 3, r1, 1, show_threads, t);
    } else {
        int r = (n0 + n1 + 1) / 2, avail = (Y_PW - Y_CPU) - 3;
        if (r > avail) r = avail;
        if (r < 1) r = 1;
        draw_group(s, u, Y_CPU, r, n0 > 0 ? 0 : 1, show_threads, t);
    }
    draw_power(s, u, t);

    text_l(1, H - 1, C_DIM, 0, "q quit \xC2\xB7 t toggle %s \xC2\xB7 poll %.1fs%s",
           show_threads ? "cores" : "threads",
           g_cfg.interval,
           (s->valid && !s->power_ok) ? " \xC2\xB7 package watts need root (RAPL energy_uj)" : "");
}

/* ----------------------------------------------------------- frame emission */
typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s, size_t n)
{
    if (b->n + n > b->cap) return;
    memcpy(b->p + b->n, s, n); b->n += n;
}

PRINTF_LIKE(2, 3)
static void bprintf(Buf *b, const char *fmt, ...)
{
    char t[96]; va_list ap; va_start(ap, fmt);
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

static bool same(RGB a, RGB b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static void emit_frame(Buf *b, int ox, int oy, bool absolute)
{
    for (int y = 0; y < H; y++) {
        if (absolute) bprintf(b, "\x1b[%d;%dH", oy + y + 1, ox + 1);
        bput(b, "\x1b[0m", 4);
        int bold = 0, hasbg = 0, fgset = 0; RGB fg = {0,0,0}, bg = {0,0,0};
        for (int x = 0; x < W; x++) {
            const Cell *c = &g_fb[y][x];
            if (c->bold != bold) { bput(b, c->bold ? "\x1b[1m" : "\x1b[22m", c->bold ? 4 : 5); bold = c->bold; }
            if (c->hasbg) {
                if (!hasbg || !same(bg, c->bg)) { bprintf(b, "\x1b[48;2;%d;%d;%dm", c->bg.r, c->bg.g, c->bg.b); bg = c->bg; hasbg = 1; }
            } else if (hasbg) { bput(b, "\x1b[49m", 5); hasbg = 0; }
            if (c->cp != ' ' || c->hasbg) {
                if (!fgset || !same(fg, c->fg)) { bprintf(b, "\x1b[38;2;%d;%d;%dm", c->fg.r, c->fg.g, c->fg.b); fg = c->fg; fgset = 1; }
            }
            bcp(b, c->cp);
        }
        bput(b, "\x1b[0m", 4);
        if (!absolute) bput(b, "\n", 1);
    }
}

/* ===================================================================== DATA */

static bool read_first_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) {
        size_t l = strlen(out);
        while (l && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' ')) out[--l] = 0;
    }
    return ok;
}

static bool read_ll(const char *path, long long *v)
{
    char b[64];
    if (!read_first_line(path, b, sizeof b)) return false;
    char *e; long long x = strtoll(b, &e, 10);
    if (e == b) return false;
    *v = x;
    return true;
}

static bool read_ull(const char *path, unsigned long long *v)
{
    char b[64];
    if (!read_first_line(path, b, sizeof b)) return false;
    char *e; unsigned long long x = strtoull(b, &e, 10);
    if (e == b) return false;
    *v = x;
    return true;
}

static int read_ec_profile_code(void)
{
    int fd = open("/run/cctl/mode", O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 3; /* EC default of code 3 if no /run/cctl/mode */
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return 3; }
    char line[128];
    int code = 3;
    /* Line 1: profile, Line 2: method, Line 3: ec_profile_code */
    if (fgets(line, sizeof line, f) &&
        fgets(line, sizeof line, f) &&
        fgets(line, sizeof line, f)) {
        char *e = NULL;
        long x = strtol(line, &e, 10);
        if (e != line && x >= 0 && x <= 3) code = (int)x;
    }
    fclose(f);
    return code;
}

#define SYSCPU "/sys/devices/system/cpu"

/* "0-15,20,22-23" -> mask */
static void parse_cpulist(const char *s, bool *mask)
{
    while (*s) {
        char *e;
        long a = strtol(s, &e, 10);
        if (e == s) break;
        long b = a;
        if (*e == '-') { b = strtol(e + 1, &e, 10); }
        for (long i = a; i <= b && i < MAX_CPU; i++) if (i >= 0) mask[i] = true;
        s = e;
        if (*s == ',') s++;
    }
}

/* ---- static topology, discovered once ---- */
static struct {
    bool inited;
    int  ncpu, ncore;
    bool hybrid;
    uint8_t cpu_group[MAX_CPU];
    int  cpu_core[MAX_CPU];             /* logical -> physical core index */
    uint8_t core_group[MAX_CPU];
    char name[96];
    char temp_path[256], crit_path[256];
    char rapl[256];
} T;

static void clean_cpu_name(char *s)
{
    static const char *junk[] = { "(R)", "(TM)", "(tm)", "(r)", "CPU ", "Processor" };
    for (size_t k = 0; k < sizeof junk / sizeof junk[0]; k++) {
        char *p;
        while ((p = strstr(s, junk[k])) != NULL) memmove(p, p + strlen(junk[k]), strlen(p + strlen(junk[k])) + 1);
    }
    char *at = strstr(s, " @ "); if (at) *at = 0;
    char *d = s, *r = s; bool sp = true;                      /* collapse spaces */
    for (; *r; r++) { if (*r == ' ') { if (sp) continue; sp = true; } else sp = false; *d++ = *r; }
    *d = 0;
    size_t l = strlen(s); while (l && s[l - 1] == ' ') s[--l] = 0;
}

static void find_temp_sensor(void)
{
    for (int i = 0; i < 40 && !T.temp_path[0]; i++) {
        char p[256], nm[64];
        snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/name", i);
        if (!read_first_line(p, nm, sizeof nm)) continue;
        bool intel = !strcmp(nm, "coretemp"), amd = !strcmp(nm, "k10temp") || !strcmp(nm, "zenpower");
        if (!intel && !amd) continue;
        int pick = 0;
        for (int j = 1; j <= 16; j++) {
            char lp[256], lb[64];
            snprintf(lp, sizeof lp, "/sys/class/hwmon/hwmon%d/temp%d_label", i, j);
            if (!read_first_line(lp, lb, sizeof lb)) continue;
            if ((intel && !strcmp(lb, "Package id 0")) || (amd && (!strcmp(lb, "Tctl") || !strcmp(lb, "Tdie")))) { pick = j; if (intel || !strcmp(lb, "Tctl")) break; }
        }
        if (!pick) pick = 1;
        snprintf(T.temp_path, sizeof T.temp_path, "/sys/class/hwmon/hwmon%d/temp%d_input", i, pick);
        snprintf(T.crit_path, sizeof T.crit_path, "/sys/class/hwmon/hwmon%d/temp%d_crit", i, pick);
    }
    for (int i = 0; i < 40 && !T.temp_path[0]; i++) {          /* fallback: thermal zone */
        char p[256], ty[64];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", i);
        if (!read_first_line(p, ty, sizeof ty)) continue;
        if (!strcmp(ty, "x86_pkg_temp")) snprintf(T.temp_path, sizeof T.temp_path, "/sys/class/thermal/thermal_zone%d/temp", i);
    }
}

static void find_rapl(void)
{
    for (int i = 0; i < 4 && !T.rapl[0]; i++) {
        char p[256], nm[64];
        snprintf(p, sizeof p, "/sys/class/powercap/intel-rapl:%d/name", i);
        if (read_first_line(p, nm, sizeof nm) && !strncmp(nm, "package", 7))
            snprintf(T.rapl, sizeof T.rapl, "/sys/class/powercap/intel-rapl:%d", i);
    }
}

static void topology_init(void)
{
    memset(&T, 0, sizeof T);
    T.inited = true;

    snprintf(T.name, sizeof T.name, "CPU");
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char l[256];
        while (fgets(l, sizeof l, f))
            if (!strncmp(l, "model name", 10)) {
                char *c = strchr(l, ':');
                if (c) { snprintf(T.name, sizeof T.name, "%s", c + 2); T.name[strcspn(T.name, "\n")] = 0; }
                break;
            }
        fclose(f);
    }
    clean_cpu_name(T.name);

    long nconf = sysconf(_SC_NPROCESSORS_CONF);
    T.ncpu = (int)(nconf < 1 ? 1 : (nconf > MAX_CPU ? MAX_CPU : nconf));

    bool pm[MAX_CPU] = {0}, em[MAX_CPU] = {0};
    char l[1024];
    bool has_p = read_first_line("/sys/devices/cpu_core/cpus", l, sizeof l);
    if (has_p) parse_cpulist(l, pm);
    bool has_e = read_first_line("/sys/devices/cpu_atom/cpus", l, sizeof l);
    if (has_e) parse_cpulist(l, em);
    if (read_first_line("/sys/devices/cpu_lowpower/cpus", l, sizeof l)) { parse_cpulist(l, em); has_e = true; }
    T.hybrid = has_p && has_e;

    int seen_pkg[MAX_CPU], seen_core[MAX_CPU], nseen = 0;
    for (int c = 0; c < T.ncpu; c++) {
        T.cpu_group[c] = (T.hybrid && em[c]) ? 1 : 0;
        char p[256]; long long pk = 0, ci = c;
        snprintf(p, sizeof p, SYSCPU "/cpu%d/topology/physical_package_id", c); if (!read_ll(p, &pk)) pk = 0;
        snprintf(p, sizeof p, SYSCPU "/cpu%d/topology/core_id", c);             if (!read_ll(p, &ci)) ci = c;
        int found = -1;
        for (int k = 0; k < nseen; k++) if (seen_pkg[k] == pk && seen_core[k] == ci) { found = k; break; }
        if (found < 0) { found = nseen; seen_pkg[nseen] = (int)pk; seen_core[nseen] = (int)ci; T.core_group[nseen] = T.cpu_group[c]; nseen++; }
        T.cpu_core[c] = found;
    }
    T.ncore = nseen;
    find_temp_sensor();
    find_rapl();
}

/* Strip "-1" style garbage like [N/A]; returns kHz */
static double cpufreq_khz(int cpu, const char *file)
{
    char p[256]; long long v;
    snprintf(p, sizeof p, SYSCPU "/cpu%d/cpufreq/%s", cpu, file);
    return read_ll(p, &v) ? (double)v : -1.0;
}

static bool read_proc_stat(CpuSnapshot *s)
{
    static unsigned long long pt[MAX_CPU + 1], pi[MAX_CPU + 1];
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "cpu", 3)) break;
        char *q = line + 3; int id = -1;
        if (isdigit((unsigned char)*q)) id = (int)strtol(q, &q, 10);
        unsigned long long v[8] = {0};
        sscanf(q, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        unsigned long long tot = 0; for (int i = 0; i < 8; i++) tot += v[i];
        unsigned long long idle = v[3] + v[4];
        int slot = id < 0 ? MAX_CPU : id;
        if (slot > MAX_CPU || (id >= 0 && id >= s->ncpu)) continue;
        double dt = (double)(tot - pt[slot]), di = (double)(idle - pi[slot]);
        double ut = dt > 0 ? clampd(100.0 * (dt - di) / dt, 0, 100) : 0;
        if (id < 0) s->util_total = ut; else s->cpu_util[id] = ut;
        pt[slot] = tot; pi[slot] = idle;
    }
    fclose(f);
    return true;
}

static int tuxedo_open_clevo(void)
{
    int fd = open("/dev/tuxedo_io", O_RDWR);
    if (fd < 0) return -1;
    int is_clevo = 0;
    if (ioctl(fd, R_HWCHECK_CL, &is_clevo) < 0 || !is_clevo) {
        close(fd);
        return -1;
    }
    return fd;
}

static int ensure_ec_sys(void)
{
    if (access(EC_RAM_PATH, F_OK) == 0) return 0;
    if (geteuid() != 0) return -1;
    static int attempted;
    if (attempted) return -1;
    attempted = 1;
    pid_t child = fork();
    if (child == 0) {
        execlp("modprobe", "modprobe", "ec_sys", (char *)NULL);
        _exit(127);
    }
    if (child < 0) return -1;
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    usleep(200000);
    return access(EC_RAM_PATH, F_OK) == 0 ? 0 : -1;
}

/* Duty comes from tuxedo_io; RPM comes from EC RAM, matching cctl monitor. */
static bool read_cpu_fan(double *duty_pct, double *rpm)
{
    *duty_pct = *rpm = 0;
    bool got_duty = false;
    int fd = tuxedo_open_clevo();
    if (fd >= 0) {
        int fan1 = 0, fan2 = 0;
        if (ioctl(fd, R_CL_FANINFO1, &fan1) >= 0) {
            *duty_pct = ((fan1 & 0xFF) * 100) / 255.0;
            got_duty = true;
        }
        (void)ioctl(fd, R_CL_FANINFO2, &fan2);
        close(fd);
    }

    (void)ensure_ec_sys();
    int ec_fd = open(EC_RAM_PATH, O_RDONLY);
    if (ec_fd < 0) return got_duty;
    unsigned char ram[256] = {0};
    ssize_t n = read(ec_fd, ram, sizeof ram);
    close(ec_fd);
    if (n < 0xD4) return got_duty;

    if (!got_duty) {
        int cpu_raw = n >= 0xF5 ? ram[0xF4] : 0;
        if (cpu_raw == 0) cpu_raw = ram[0x89];
        *duty_pct = (cpu_raw * 100) / 255.0;
    }
    unsigned int raw = ((unsigned int)ram[0xD0] << 8) | ram[0xD1];
    *rpm = raw ? (double)EC_FAN_RPM_DIVISOR / raw : 0;
    return true;
}

static void live_poll(CpuSnapshot *s)
{
    if (!T.inited) topology_init();
    snprintf(s->name, sizeof s->name, "%s", T.name);
    s->ncpu = T.ncpu; s->ncore = T.ncore; s->hybrid = T.hybrid;
    memcpy(s->cpu_group, T.cpu_group, sizeof s->cpu_group);
    memcpy(s->core_group, T.core_group, sizeof s->core_group);
    memcpy(s->cpu_core, T.cpu_core, sizeof s->cpu_core);
    s->grp_ncore[0] = s->grp_ncore[1] = 0;
    for (int c = 0; c < s->ncore; c++) s->grp_ncore[s->core_group[c]]++;
    s->grp_ncpu[0] = s->grp_ncpu[1] = 0;
    for (int cpu = 0; cpu < s->ncpu; cpu++) s->grp_ncpu[s->cpu_group[cpu]]++;

    read_proc_stat(s);

    /* turbo state */
    long long v;
    s->turbo_known = false;
    if (read_ll(SYSCPU "/intel_pstate/no_turbo", &v))       { s->turbo_known = true; s->turbo_on = (v == 0); }
    else if (read_ll(SYSCPU "/cpufreq/boost", &v))          { s->turbo_known = true; s->turbo_on = (v != 0); }
    read_first_line(SYSCPU "/cpu0/cpufreq/scaling_governor", s->governor, sizeof s->governor);
    read_first_line(SYSCPU "/cpu0/cpufreq/energy_performance_preference", s->epp, sizeof s->epp);

    /* per-core frequency and current ceiling */
    double lim[2] = {0, 0}, peak[2] = {0, 0};
    for (int c = 0; c < s->ncore; c++) s->core_mhz[c] = 0;
    for (int cpu = 0; cpu < s->ncpu; cpu++) s->cpu_mhz[cpu] = 0;
    for (int cpu = 0; cpu < s->ncpu; cpu++) {
        int core = T.cpu_core[cpu];
        double cur = cpufreq_khz(cpu, "scaling_cur_freq");
        if (cur > 0) {
            cur /= 1000.0;
            s->cpu_mhz[cpu] = cur;
            if (cur > s->core_mhz[core]) s->core_mhz[core] = cur;
        }
        double hw = cpufreq_khz(cpu, "cpuinfo_max_freq"), sw = cpufreq_khz(cpu, "scaling_max_freq");
        double c_lim = hw > 0 ? hw : sw;
        if (sw > 0 && c_lim > 0 && sw < c_lim) c_lim = sw;
        if (s->turbo_known && !s->turbo_on) {                 /* turbo off: cap at the base clock when the kernel exposes it */
            double base = cpufreq_khz(cpu, "base_frequency");
            if (base > 0 && (c_lim <= 0 || base < c_lim)) c_lim = base;
        }
        int g = s->cpu_group[cpu];
        if (c_lim / 1000.0 > lim[g]) lim[g] = c_lim / 1000.0;
    }
    for (int c = 0; c < s->ncore; c++) { int g = s->core_group[c]; if (s->core_mhz[c] > peak[g]) peak[g] = s->core_mhz[c]; }
    for (int g = 0; g < 2; g++) s->grp_max_mhz[g] = lim[g] > peak[g] ? lim[g] : peak[g];   /* never let a bar overflow */

    /* EC profile code and PROCHOT limit (default: EC code 3 -> 87°C) */
    s->ec_code = read_ec_profile_code();
    s->prochot_c = (s->ec_code == 2) ? 97.0 : 87.0;

    /* temperature */
    s->temp_ok = false;
    if (T.temp_path[0] && read_ll(T.temp_path, &v)) {
        s->temp_ok = true; s->temp_c = v / 1000.0;
        long long cr; s->tjmax_c = 100;
        if (T.crit_path[0] && read_ll(T.crit_path, &cr) && cr > 60000 && cr < 125000) s->tjmax_c = cr / 1000.0;
    }

    /* RAPL: watts from energy_uj deltas, limits from constraint files */
    static unsigned long long prev_e; static double prev_t; static bool have_prev;
    s->power_ok = false; s->pl_ok = false;
    if (T.rapl[0]) {
        char p[300]; unsigned long long e;
        snprintf(p, sizeof p, "%s/energy_uj", T.rapl);
        if (read_ull(p, &e)) {
            double tn = now_s();
            if (have_prev && tn > prev_t) {
                unsigned long long range = 0, d;
                snprintf(p, sizeof p, "%s/max_energy_range_uj", T.rapl); read_ull(p, &range);
                d = e >= prev_e ? e - prev_e : (range ? range - prev_e + e : 0);
                s->pkg_w = (double)d / 1e6 / (tn - prev_t);
                s->power_ok = true;
            }
            prev_e = e; prev_t = tn; have_prev = true;
        }
        for (int i = 0; i < 3; i++) {
            char nm[32], pp[300]; unsigned long long uw;
            snprintf(pp, sizeof pp, "%s/constraint_%d_name", T.rapl, i);
            if (!read_first_line(pp, nm, sizeof nm)) continue;
            snprintf(pp, sizeof pp, "%s/constraint_%d_power_limit_uw", T.rapl, i);
            if (!read_ull(pp, &uw)) continue;
            if (!strcmp(nm, "long_term")) {
                s->pl1_w = (double)uw / 1e6; s->pl_ok = true;
                snprintf(pp, sizeof pp, "%s/constraint_%d_time_window_us", T.rapl, i);
                unsigned long long tw; if (read_ull(pp, &tw)) s->tau_s = (double)tw / 1e6;
            } else if (!strcmp(nm, "short_term")) s->pl2_w = (double)uw / 1e6;
        }
        if (s->pl_ok && s->pl2_w <= 0) s->pl2_w = s->pl1_w;
    }

    /* RAM */
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char l[160]; double tot = 0, av = 0;
        while (fgets(l, sizeof l, f)) {
            if (!strncmp(l, "MemTotal:", 9)) tot = atof(l + 9);
            else if (!strncmp(l, "MemAvailable:", 13)) av = atof(l + 13);
        }
        fclose(f);
        s->ram_total_gb = tot / 1048576.0; s->ram_used_gb = (tot - av) / 1048576.0;
    }

    s->fan_ok = read_cpu_fan(&s->fan_duty_pct, &s->fan_rpm);
    s->valid = true;
}

static void init_snapshot(CpuSnapshot *s)
{
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "CPU");
    s->ncpu = s->ncore = 1; s->grp_ncore[0] = 1; s->grp_ncpu[0] = 1;
    s->ec_code = 3; s->prochot_c = 87.0;
}

static void *poller(void *arg)
{
    (void)arg;
    CpuSnapshot cur;
    init_snapshot(&cur);
    while (!g_quit) {
        live_poll(&cur);
        pthread_mutex_lock(&g_lock); g_snap = cur; pthread_mutex_unlock(&g_lock);
        for (double w = 0; w < g_cfg.interval && !g_quit; w += 0.05) sleep_s(0.05);
    }
    return NULL;
}

/* ================================================================== TERMINAL */
static struct termios g_old;
static bool g_raw = false;

static void write_all(const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(STDOUT_FILENO, p, n);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

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
    if (g_raw) { tcsetattr(STDIN_FILENO, TCSANOW, &g_old); g_raw = false; }
}

static void term_size(int *cols, int *rows)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { *cols = ws.ws_col; *rows = ws.ws_row; }
    else { *cols = 80; *rows = 24; }
}

static void on_signal(int sig) { (void)sig; g_quit = 1; }

static void handle_keys(void)
{
    if (!g_raw) return;
    char k[16]; ssize_t n = read(STDIN_FILENO, k, sizeof k);
    for (ssize_t i = 0; i < n; i++) {
        if (k[i] == 'q' || k[i] == 'Q' || k[i] == 3) g_quit = 1;
        else if (k[i] == 't' || k[i] == 'T') atomic_store(&g_show_threads, !atomic_load(&g_show_threads));
    }
}

static void usage(void)
{
    printf("Usage: cctl cpumon [--json] [--interval SEC] [--once]\n"
           "  --json          Stream live readings as newline-delimited JSON\n"
           "  --interval SEC  Set live polling interval (default 0.5 seconds)\n"
           "  --once          Show one live frame and exit\n"
           "  -h, --help      Show this help\n"
           "Press q to quit, t to toggle cores/threads.\n");
}

static void json_string(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void json_number(double value, bool valid, int precision)
{
    if (!valid || !isfinite(value)) fputs("null", stdout);
    else printf("%.*f", precision, value);
}

static void print_json_snapshot(const CpuSnapshot *s)
{
    fputs("{\"cpu_model\":", stdout); json_string(s->name);
    fputs(",\"cpu_usage_pct\":", stdout); json_number(s->util_total, s->valid, 1);
    fputs(",\"cpu_temp_c\":", stdout); json_number(s->temp_c, s->temp_ok, 1);
    fputs(",\"prochot_c\":", stdout); json_number(s->prochot_c, s->valid, 0);
    fputs(",\"ec_profile_code\":", stdout); if (s->valid) printf("%d", s->ec_code); else fputs("null", stdout);
    fputs(",\"package_power_w\":", stdout); json_number(s->pkg_w, s->power_ok, 2);
    fputs(",\"pl1_w\":", stdout); json_number(s->pl1_w, s->pl_ok, 1);
    fputs(",\"pl2_w\":", stdout); json_number(s->pl2_w, s->pl_ok, 1);
    fputs(",\"memory_used_gb\":", stdout); json_number(s->ram_used_gb, s->ram_total_gb > 0, 2);
    fputs(",\"memory_total_gb\":", stdout); json_number(s->ram_total_gb, s->ram_total_gb > 0, 2);
    fputs(",\"cpu_fan_duty_pct\":", stdout); json_number(s->fan_duty_pct, s->fan_ok, 1);
    fputs(",\"cpu_fan_rpm\":", stdout); json_number(s->fan_rpm, s->fan_ok, 0);
    fputs(",\"turbo_enabled\":", stdout);
    if (s->turbo_known) fputs(s->turbo_on ? "true" : "false", stdout); else fputs("null", stdout);
    fputs(",\"governor\":", stdout); if (s->governor[0]) json_string(s->governor); else fputs("null", stdout);
    fputs(",\"epp\":", stdout); if (s->epp[0]) json_string(s->epp); else fputs("null", stdout);
    fputs(",\"cores\":[", stdout);
    for (int i = 0; i < s->ncore; i++) {
        if (i) putchar(',');
        printf("{\"index\":%d,\"group\":\"%c\",\"frequency_mhz\":", i,
               s->core_group[i] ? 'E' : 'P');
        json_number(s->core_mhz[i], s->valid && s->core_mhz[i] > 0, 0);
        fputs("}", stdout);
    }
    puts("]}");
}

int cctl_cpumon(int argc, char **argv)
{
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) {
            g_cfg.once = true;
        } else if (!strcmp(argv[i], "--json")) {
            g_cfg.json = true;
        } else if (!strcmp(argv[i], "--interval") && i + 1 < argc) {
            char *end = NULL;
            double interval = strtod(argv[++i], &end);
            if (!end || *end || interval < 0.1 || interval > 60.0) {
                usage();
                return 2;
            }
            g_cfg.interval = interval;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }

    static char out[1 << 19];
    Buf b = { out, 0, sizeof out };
    static Ui ui;
    memset(&ui, 0, sizeof ui);
    g_t0 = now_s();

    if (g_cfg.once) {
        static CpuSnapshot s;
        init_snapshot(&s);
        live_poll(&s);
        sleep_s(0.4);
        live_poll(&s);
        for (int k = 0; k < 40; k++) ui_update(&ui, &s, 0.25);       /* jump straight to steady state */
        if (g_cfg.json) {
            print_json_snapshot(&s);
        } else {
            render(&s, &ui, 0);
            emit_frame(&b, 0, 0, false);
            write_all(b.p, b.n);
        }
        return 0;
    }

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    init_snapshot(&g_snap);
    pthread_t th;
    if (pthread_create(&th, NULL, poller, NULL) != 0) { fprintf(stderr, "thread failed\n"); return 1; }

    if (g_cfg.json) {
        while (!g_quit) {
            sleep_s(g_cfg.interval);
            CpuSnapshot s;
            pthread_mutex_lock(&g_lock); s = g_snap; pthread_mutex_unlock(&g_lock);
            print_json_snapshot(&s);
            fflush(stdout);
        }
        g_quit = 1;
        pthread_join(th, NULL);
        return 0;
    }

    term_enter();
    double last = now_s();
    int pc = 0, pr = 0;
    while (!g_quit) {
        double t = now_s(), dt = clampd(t - last, 0, 0.25);
        last = t;
        handle_keys();

        int cols, rows;
        term_size(&cols, &rows);
        if (cols != pc || rows != pr) { write_str("\x1b[2J"); pc = cols; pr = rows; }
        if (cols < W || rows < H) {
            char m[96];
            int n = snprintf(m, sizeof m, "\x1b[H\x1b[0mTerminal too small: need %dx%d, have %dx%d ", W, H, cols, rows);
            write_all(m, (size_t)n);
            sleep_s(0.2);
            continue;
        }

        static CpuSnapshot s;
        pthread_mutex_lock(&g_lock); s = g_snap; pthread_mutex_unlock(&g_lock);

        ui_update(&ui, &s, dt);
        render(&s, &ui, t - g_t0);
        b.n = 0;
        emit_frame(&b, (cols - W) / 2, (rows - H) / 2, true);
        write_all(b.p, b.n);

        sleep_s(FRAME_SEC - (now_s() - t));
    }

    g_quit = 1;
    pthread_join(th, NULL);
    term_leave();
    return 0;
}
