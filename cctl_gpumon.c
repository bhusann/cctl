/*
 * cctl_gpumon.c - live terminal NVIDIA monitor, invoked by `cctl gpumon`.
 * The framebuffer, drawing functions, and visual layout are shared with the
 * standalone prototype; hardware data comes from cctl-compatible sources.
 *
 * Needs a terminal of at least 100x26 with truecolor + UTF-8.
 *
 * ---------------------------------------------------------------------------
 * HOW THE D3cold SAFETY WORKS
 *   Every poll starts with read_pm_state(), which only reads sysfs files
 *   (power_state / runtime_status). Those reads are served from kernel state
 *   and do NOT wake the GPU. nvidia-smi is executed ONLY when the state is D0.
 *   Do not add lspci -vv, setpci or config-space reads to the sleeping path:
 *   those can wake the device.
 *
 *   Heads-up: while the GPU is awake, each nvidia-smi call counts as activity
 *   and restarts the runtime-suspend countdown. If you poll faster than the
 *   autosuspend delay, the GPU may never get to sleep while this runs. Raise
 *   --interval if that happens (see power/autosuspend_delay_ms in sysfs).
 *
 * LIVE DATA SOURCES
 *   read_pm_state()    - same PCI power-state sysfs files cctl reads
 *   read_cctl_fans()   - fan duty from tuxedo_io, RPM from EC RAM, like cctl
 *   nvsmi_query()      - nvidia-smi GPU readings, only while PCI state is D0
 *   nvsmi_pcie_rates() - PCIe RX/TX via `nvidia-smi dmon`
 * ---------------------------------------------------------------------------
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

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
#define W 100                     /* canvas size in cells */
#define H 26
#define HIST 19                   /* sparkline samples (= module inner width) */
#define FAN_MAX_RPM 3200.0        /* scales fan spin speed */
#define FRAME_SEC 0.05            /* 20 fps */

/* Match cctl's Clevo fan telemetry sources. */
#define EC_RAM_PATH "/sys/kernel/debug/ec/ec0/io"
#define EC_FAN_RPM_DIVISOR 2156220
#define R_HWCHECK_CL 0x8008EC05
#define R_CL_FANINFO2 0x8008ED11

#define W_MOD   21
#define H_MOD   15
#define Y_MOD   9
#define X_LMOD  0
#define X_TBAR  22
#define X_CARD  28
#define W_CARD  44
#define X_CBAR  73
#define X_RMOD  79
#define Y_CARD  7
#define H_CARD  16
#define Y_BAR   8
#define H_BAR   16

/* ------------------------------------------------------------------- types */
typedef struct { uint8_t r, g, b; } RGB;
#define RGBc(r, g, b) ((RGB){(r), (g), (b)})

typedef enum { PM_UNKNOWN, PM_D0, PM_D1, PM_D2, PM_D3HOT, PM_D3COLD } PciPm;

typedef struct {
    PciPm  pm;
    bool   valid;          /* nvidia-smi data present (only ever true while D0) */
    bool   smi_error;      /* D0 but the query failed */
    bool   telemetry_paused;
    bool   fan_valid;
    char   name[64];
    double temp_c;
    double gpu_clock_mhz, gpu_clock_max_mhz;
    double mem_clock_mhz, mem_clock_max_mhz;
    double vram_used_mib, vram_total_mib;
    double power_w, power_limit_w;
    double fan_duty_pct, fan_rpm;
    double gpu_util_pct;
    int    pstate;                 /* -1 = unknown */
    unsigned long long clk_reasons;
    int    pcie_gen, pcie_gen_max, pcie_width;
    double pcie_rx_mbs, pcie_tx_mbs;
} GpuSnapshot;

typedef struct {
    bool   live, auto_cycle, start_asleep, once;
    char   bdf[32];
    double interval;
    double once_t;
    bool   json;
} Config;

static Config g_cfg = { false, false, false, false, "", 1.0, 6.0, false };

/* ----------------------------------------------------------------- globals */
static GpuSnapshot       g_snap;
static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_quit = 0;
static atomic_int        g_force_sleep = 0;     /* mock only: 'd' key */
static atomic_int        g_wait_d3cold = 0;     /* live: pause NVIDIA queries until D3cold */
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
static const RGB C_NV    = {118, 185, 0};
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
static const RGB G_CLK[]  = {{90,70,255},{170,70,255},{255,70,190},{255,170,90},{255,235,170}};
static const RGB G_MEM[]  = {{40,200,255},{70,130,255},{150,90,255}};
static const RGB G_LOAD[] = {{80,225,110},{230,220,60},{255,130,40},{255,60,60}};
static const RGB G_NVRULE[] = {{118,185,0},{0,200,190},{40,50,70}};
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

/* Every colour goes through here: g_amt blends between "dull grey" (GPU asleep)
 * and the real colour (GPU awake), which gives the fade-in on wake. */
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
    if (s[0] < 0x80)              { cp = s[0];        n = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; n = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; n = 3; }
    else                          { cp = s[0] & 0x07; n = 4; }
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

static void kv(int x, int y, const char *k, RGB kc, const char *v, RGB vc)
{
    text_l(x + 1, y, kc, 0, "%s", k);
    text_r(x + W_MOD - 2, y, vc, 1, "%s", v);
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

/* vertical bar: label row, body, value row, unit row (5 cells wide) */
static void vbar(int x, int y, int h, const char *label, double frac, const RGB *st, int ns,
                 const char *val, const char *unit)
{
    int bh = h - 3, ybot = y + h - 3;
    double lvl = clamp01(frac) * bh * 8;
    text_c(x + 2, y, C_LABEL, 1, "%s", label);
    for (int k = 0; k < bh; k++) {
        int row = ybot - k;
        bool tick = (k % 3) == 0;
        put(x, row, tick ? 0x251C : 0x2502, C_FRAME, 0);
        put(x + 4, row, tick ? 0x2524 : 0x2502, C_FRAME, 0);
        for (int i = 1; i <= 3; i++) put_bg(x + i, row, BG_BAR);
        int e = (int)lround(clampd(lvl - k * 8, 0, 8));
        if (e <= 0) continue;
        RGB col = grad(st, ns, (k + 0.5) / bh);
        if (lvl - (k + 1) * 8 <= 0) col = mix(col, RGBc(255, 255, 255), 0.35);   /* bright cap */
        for (int i = 1; i <= 3; i++) put(x + i, row, (uint32_t)(0x2580 + e), col, 0);
    }
    text_c(x + 2, y + h - 2, C_TEXT, 1, "%s", val);
    text_c(x + 2, y + h - 1, C_DIM, 0, "%s", unit);
}

/* ------------------------------------------------------------ UI (animated) */
typedef struct {
    double amt;                                   /* colour amount (0 dull .. 1 full) */
    double f_temp, f_clk, f_mclk, f_pow, f_duty, f_util, f_vram;
    double fan_rps, fan_phase, wire_phase;
    double rx[HIST], tx[HIST], hist_acc;
} Ui;

static void hist_push(double *h, double v)
{
    memmove(h, h + 1, (HIST - 1) * sizeof *h);
    h[HIST - 1] = v;
}

static bool snap_active(const GpuSnapshot *s) { return s->pm == PM_D0 && s->valid; }

static void ui_update(Ui *u, const GpuSnapshot *s, double dt)
{
    bool act = snap_active(s);
    ease(&u->amt, act ? 1.0 : 0.0, dt, 1.8);

    double clkmax = s->gpu_clock_max_mhz > 0 ? s->gpu_clock_max_mhz : 3000;
    double memmax = s->mem_clock_max_mhz > 0 ? s->mem_clock_max_mhz : 10000;
    double t_temp = act ? clamp01((s->temp_c - 20.0) / 80.0) : 0;
    double t_clk  = act ? clamp01(s->gpu_clock_mhz / clkmax) : 0;
    double t_mclk = act ? clamp01(s->mem_clock_mhz / memmax) : 0;
    double t_pow  = act && s->power_limit_w > 0 ? clamp01(s->power_w / s->power_limit_w) : 0;
    double t_duty = s->fan_valid ? clamp01(s->fan_duty_pct / 100.0) : 0;
    double t_util = act ? clamp01(s->gpu_util_pct / 100.0) : 0;
    double t_vram = act && s->vram_total_mib > 0 ? clamp01(s->vram_used_mib / s->vram_total_mib) : 0;
    ease(&u->f_temp, t_temp, dt, 5); ease(&u->f_clk, t_clk, dt, 6);  ease(&u->f_mclk, t_mclk, dt, 6);
    ease(&u->f_pow, t_pow, dt, 6);   ease(&u->f_duty, t_duty, dt, 5); ease(&u->f_util, t_util, dt, 6);
    ease(&u->f_vram, t_vram, dt, 4);

    /* Fan spin: eased so it spins up / winds down. Visual speed is capped well
     * below the aliasing limit (7 blades @ 20 fps) so it never "wagon-wheels". */
    double target = (s->fan_valid && s->fan_rpm > 1.0) ? 0.10 + 0.85 * clamp01(s->fan_rpm / FAN_MAX_RPM) : 0.0;
    ease(&u->fan_rps, target, dt, target > 0 ? 2.2 : 0.9);
    if (target == 0.0 && u->fan_rps < 0.02) u->fan_rps = 0.0;
    u->fan_phase = fmod(u->fan_phase + u->fan_rps * dt, 1.0);

    u->wire_phase = fmod(u->wire_phase + (act ? 5.0 + 45.0 * u->f_pow : 0.0) * dt, 8.0);

    u->hist_acc += dt;
    while (u->hist_acc >= 0.4) {
        u->hist_acc -= 0.4;
        hist_push(u->rx, act ? s->pcie_rx_mbs : 0);
        hist_push(u->tx, act ? s->pcie_tx_mbs : 0);
    }
}

/* ------------------------------------------------------------- drawing bits */
static void spark(int x, int y, const double *h, RGB c)
{
    double mx = 50.0;                                  /* floor so idle noise stays flat */
    for (int i = 0; i < HIST; i++) if (h[i] > mx) mx = h[i];
    for (int i = 0; i < HIST; i++) {
        double v = h[i] / mx;
        int e = (int)lround(v * 8);
        if (e <= 0) put(x + i, y, 0x2581, C_DIM, 0);
        else        put(x + i, y, (uint32_t)(0x2580 + e), mix(mix(c, C_DIM, 0.5), c, v), 0);
    }
}

static void draw_fan(int cx, int cy, double phase, bool stopped)
{
    const int N = 7;
    for (int j = -3; j <= 3; j++) for (int i = -6; i <= 6; i++) {
        double dy = j * 2.0;                            /* cells are ~2x taller than wide */
        double d = sqrt((double)(i * i) + dy * dy);
        int x = cx + i, y = cy + j;
        if (d > 6.7) continue;
        if (d >= 5.7) {                                 /* outer ring */
            put(x, y, 0x2588, stopped ? RGBc(62, 68, 82) : C_NV, 0);
            continue;
        }
        if (d <= 2.3) {                                 /* hub */
            RGB hc = stopped ? RGBc(120, 128, 145) : RGBc(170, 240, 60);
            put(x, y, (i == 0 && j == 0) ? 0x25CF : 0x2588, hc, 0);
            continue;
        }
        double a = atan2(dy, (double)i);
        double u = (a / (2 * PI) - phase) * N + d * 0.10;
        double f = u - floor(u);
        if (f >= 0.5) continue;
        double shade = 1.0 - f / 0.5;
        uint32_t g = f < 0.2 ? 0x2588 : (f < 0.38 ? 0x2593 : 0x2592);
        RGB bc = stopped ? RGBc(92, 100, 116) : mix(RGBc(35, 95, 140), RGBc(190, 240, 255), shade);
        put(x, y, g, bc, 0);
    }
}

static void draw_title(const GpuSnapshot *s, double t, const char *bdf)
{
    static const char *ttl = "GPU//MON";
    put(0, 0, 0x258C, C_NV, 1);
    for (int i = 0; ttl[i]; i++) put(2 + i, 0, (unsigned char)ttl[i], grad(G_NVRULE, 3, i / 8.0), 1);
    int n = text_l(12, 0, C_LABEL, 0, "%s", s->name);
    if (bdf && *bdf) text_l(12 + n + 2, 0, C_DIM, 0, "%s", bdf);

    static const char *z[] = { "z  ", "zz ", "zzZ" };
    char b[64]; RGB c;
    const char *pmname = "?";
    switch (s->pm) {
    case PM_D0: pmname = "D0"; break;   case PM_D1: pmname = "D1"; break;
    case PM_D2: pmname = "D2"; break;   case PM_D3HOT: pmname = "D3hot"; break;
    case PM_D3COLD: pmname = "D3cold"; break; default: break;
    }
    if (s->pm == PM_D0 && s->telemetry_paused) { snprintf(b, sizeof b, "\xE2\x97\x90 D0 \xC2\xB7 NVIDIA POLLING PAUSED"); c = C_AMBER; }
    else if (s->pm == PM_D0 && s->valid)   { snprintf(b, sizeof b, "\xE2\x97\x8F D0 \xC2\xB7 ACTIVE"); c = C_NV; }
    else if (s->pm == PM_D0 && s->smi_error) { snprintf(b, sizeof b, "\xE2\x96\xB2 D0 \xC2\xB7 NVIDIA-SMI ERROR"); c = C_RED; }
    else if (s->pm == PM_D0)               { snprintf(b, sizeof b, "\xE2\x97\x90 D0 \xC2\xB7 SYNCING"); c = C_AMBER; }
    else if (s->pm == PM_UNKNOWN)          { snprintf(b, sizeof b, "? PCI STATE UNKNOWN \xC2\xB7 NOT QUERYING"); c = C_AMBER; }
    else snprintf(b, sizeof b, "\xE2\x97\x8B %s \xC2\xB7 SUSPENDED  %s", pmname, z[(int)(t * 1.5) % 3]), c = C_LABEL;
    text_r(W - 1, 0, c, 1, "%s", b);

    for (int x = 0; x < W; x++) put(x, 1, 0x2500, grad(G_NVRULE, 3, (double)x / W), 0);
}

static void draw_cable(const GpuSnapshot *s, const Ui *u)
{
    bool act = snap_active(s);

    /* PSU stub */
    box(0, 3, 5, 5, C_FRAME, 0);
    text_c(2, 5, C_AMBER, 1, "\xE2\x9A\xA1");
    put(3, 5, 0, C_AMBER, 0);                                    /* ⚡ is wide (2 cells) */
    for (int r = 0; r < 3; r++) put(4, 4 + r, 0x2502, C_FRAME, 0);

    /* three wires: 12V, return, 12V; pulses flow with power draw */
    static const RGB wc[3] = {{235,190,40},{120,124,136},{235,190,40}};
    for (int r = 0; r < 3; r++) {
        for (int x = 5; x <= 32; x++) {
            if (!act) { put(x, 4 + r, 0x2500, wc[r], 0); continue; }
            double q = fmod(r == 1 ? x + u->wire_phase : x - u->wire_phase, 8.0);
            if (q < 0) q += 8.0;
            if (q < 1.0)      put(x, 4 + r, 0x25CF, mix(wc[r], RGBc(255,255,255), 0.7), 1);
            else if (q < 2.0) put(x, 4 + r, 0x2501, mix(wc[r], RGBc(255,255,255), 0.35), 0);
            else              put(x, 4 + r, 0x2501, wc[r], 0);
        }
    }

    /* connector plugged into the top edge of the card */
    const int cx = 33, cw = 16;
    fill_bg(cx + 1, 4, cw - 2, 3, BG_PANEL);
    box(cx, 3, cw, 5, C_GOLD, 0);
    text_l(cx + 2, 3, C_GOLD, 1, " PWR IN ");
    for (int r = 0; r < 3; r++) put(cx, 4 + r, 0x2502, C_GOLD, 0);             /* wires land here */
    put(cx, 7, 0x2570, C_NV, 1); put(cx + cw - 1, 7, 0x256F, C_NV, 1);
    for (int x = cx + 1; x < cx + cw - 1; x++) put(x, 7, ' ', C_NV, 0);
    if (act) text_c(cx + cw / 2, 4, C_TEXT, 1, "%.1f W", s->power_w);
    else     text_c(cx + cw / 2, 4, C_TEXT, 1, "-- W");
    hbar(cx + 2, 5, cw - 4, u->f_pow, G_LOAD, NSTOP(G_LOAD));
    for (int x = cx + 2; x < cx + cw - 2; x++) put(x, 6, 0x25AA, C_GOLD, 0);    /* pins */
}

static void draw_card(const GpuSnapshot *s, const Ui *u, double t)
{
    bool act = snap_active(s);
    int x0 = X_CARD, y0 = Y_CARD, w = W_CARD;

    fill_bg(x0 + 1, y0 + 1, w - 2, H_CARD - 2, BG_CARD);
    box(x0, y0, w, H_CARD, C_NV, 1);
    for (int x = x0 + 1; x < x0 + w - 1; x++) {
        RGB c = mix(C_NV, RGBc(20, 40, 0), (double)(x - x0) / w);
        put(x, y0 + 1, 0x2580, c, 0);
    }
    text_c(x0 + w / 2, y0 + 2, C_TEXT, 1, "%s", s->name);

    /* fans (the second one counter-rotates, like real triple-axial setups) */
    bool stopped = !s->fan_valid || u->fan_rps < 0.03;
    int cy = y0 + 7;
    draw_fan(x0 + 9, cy, u->fan_phase, stopped);
    draw_fan(x0 + 34, cy, -u->fan_phase, stopped);

    /* readout between the fans */
    int mx = x0 + w / 2;
    char b[32];
    text_c(mx, y0 + 4, C_LABEL, 0, "FAN SPEED");
    if (s->fan_valid) snprintf(b, sizeof b, "%.0f %%", s->fan_duty_pct); else snprintf(b, sizeof b, "-- %%");
    text_c(mx, y0 + 5, C_TEXT, 1, "%s", b);
    hbar(mx - 5, y0 + 6, 10, u->f_duty, G_MEM, NSTOP(G_MEM));
    if (s->fan_valid) snprintf(b, sizeof b, "%.0f", s->fan_rpm); else snprintf(b, sizeof b, "0");
    text_c(mx, y0 + 8, C_TEXT, 1, "%s", b);
    text_c(mx, y0 + 9, C_LABEL, 0, "RPM");
    if (!act)         text_c(mx, y0 + 10, C_DIM, 0, "\xE2\x96\xA0 OFF");
    else if (stopped) text_c(mx, y0 + 10, C_AMBER, 1, "\xE2\x96\xA0 STOP");
    else              text_c(mx, y0 + 10, C_NV, 1, "\xE2\x97\x8F SPIN");

    /* RGB strip (brightness follows load) */
    for (int x = x0 + 2; x < x0 + w - 2; x++)
        put(x, y0 + 12, 0x2501, hsv((x - x0) * 9.0 + t * 70.0, 0.85, 0.30 + 0.70 * u->f_util), 0);

    /* load bar */
    text_l(x0 + 2, y0 + 13, C_LABEL, 0, "LOAD");
    hbar(x0 + 7, y0 + 13, 29, u->f_util, G_LOAD, NSTOP(G_LOAD));
    if (act) snprintf(b, sizeof b, "%.0f%%", s->gpu_util_pct); else snprintf(b, sizeof b, "--");
    text_r(x0 + w - 3, y0 + 13, C_TEXT, 1, "%s", b);

    /* PCIe fingers; they flicker brighter with bus traffic */
    double traffic = act ? clamp01((s->pcie_rx_mbs + s->pcie_tx_mbs) / 3000.0) : 0;
    for (int x = x0 + 5; x < x0 + w - 5; x++) {
        if (x >= x0 + 16 && x < x0 + 22) continue;      /* key notch */
        double pulse = traffic * (0.5 + 0.5 * sin(x * 0.8 - t * 9.0));
        put(x, y0 + H_CARD, (x % 2) ? 0x2590 : 0x258C, mix(RGBc(150, 118, 40), RGBc(255, 235, 150), pulse), 0);
    }
}

static const struct { unsigned long long bit; const char *name; int sev; } k_reasons[] = {
    {0x002, "App Clocks", 0},   {0x004, "SW Power Cap", 1},
    {0x008, "HW Slowdown", 2},  {0x010, "Sync Boost", 0},  {0x020, "SW Thermal", 1},
    {0x040, "HW Thermal", 2},   {0x080, "HW Pwr Brake", 2}, {0x100, "Display Clk", 0},
};

static void draw_left_module(const GpuSnapshot *s, const Ui *u)
{
    bool act = snap_active(s);
    int x = X_LMOD, y = Y_MOD;
    char b[48];
    module(x, y, W_MOD, H_MOD, "STATE", C_FRAME);

    const char *pm = "?"; RGB pc = C_AMBER;
    switch (s->pm) {
    case PM_D0: pm = "D0"; pc = C_NV; break;       case PM_D1: pm = "D1"; break;
    case PM_D2: pm = "D2"; break;                  case PM_D3HOT: pm = "D3hot"; pc = C_LABEL; break;
    case PM_D3COLD: pm = "D3cold"; pc = C_LABEL; break; default: break;
    }
    kv(x, y + 1, "PCI PM", C_LABEL, pm, pc);

    if (act && s->pstate >= 0) snprintf(b, sizeof b, "P%d", s->pstate); else snprintf(b, sizeof b, "--");
    kv(x, y + 2, "P-State", C_LABEL, b, C_CYAN);

    if (act) snprintf(b, sizeof b, "0x%02llX", s->clk_reasons); else snprintf(b, sizeof b, "--");
    kv(x, y + 3, "Clk Reason", C_LABEL, b, C_AMBER);

    int shown = 0, total = 0;
    if (act) {
        for (size_t i = 0; i < sizeof k_reasons / sizeof k_reasons[0]; i++) if (s->clk_reasons & k_reasons[i].bit) total++;
        for (size_t i = 0; i < sizeof k_reasons / sizeof k_reasons[0]; i++) {
            if (!(s->clk_reasons & k_reasons[i].bit)) continue;
            if (shown == 2 && total > 3) { text_l(x + 2, y + 4 + shown, C_DIM, 0, "+%d more", total - 2); shown++; break; }
            if (shown >= 3) break;
            RGB c = k_reasons[i].sev == 2 ? C_RED : (k_reasons[i].sev == 1 ? C_AMBER : RGBc(90, 170, 200));
            text_l(x + 2, y + 4 + shown, c, 0, "\xE2\x96\xB8 %s", k_reasons[i].name);
            shown++;
        }
        if (total == 0) text_l(x + 2, y + 4, C_DIM, 0, "\xE2\x96\xB8 none");
    } else text_l(x + 2, y + 4, C_DIM, 0, "\xE2\x96\xB8 --");

    /* PCIe sub-section */
    put(x, y + 7, 0x251C, C_FRAME, 0); put(x + W_MOD - 1, y + 7, 0x2524, C_FRAME, 0);
    for (int i = 1; i < W_MOD - 1; i++) put(x + i, y + 7, 0x2500, C_FRAME, 0);
    text_l(x + 2, y + 7, C_TEXT, 1, " PCIe ");

    if (act) snprintf(b, sizeof b, "Gen%d x%d", s->pcie_gen, s->pcie_width); else snprintf(b, sizeof b, "--");
    kv(x, y + 8, "Link", C_LABEL, b, C_TEXT);
    if (act) snprintf(b, sizeof b, "Gen%d", s->pcie_gen_max); else snprintf(b, sizeof b, "--");
    kv(x, y + 9, "Max", C_LABEL, b, C_DIM);

    if (act) snprintf(b, sizeof b, "%.0f MB/s", s->pcie_rx_mbs); else snprintf(b, sizeof b, "--");
    text_l(x + 1, y + 10, C_CYAN, 0, "\xE2\x96\xBC RX"); text_r(x + W_MOD - 2, y + 10, C_CYAN, 1, "%s", b);
    spark(x + 1, y + 11, u->rx, C_CYAN);
    if (act) snprintf(b, sizeof b, "%.0f MB/s", s->pcie_tx_mbs); else snprintf(b, sizeof b, "--");
    text_l(x + 1, y + 12, C_MAG, 0, "\xE2\x96\xB2 TX"); text_r(x + W_MOD - 2, y + 12, C_MAG, 1, "%s", b);
    spark(x + 1, y + 13, u->tx, C_MAG);
}

static void draw_right_module(const GpuSnapshot *s, const Ui *u)
{
    bool act = snap_active(s);
    int x = X_RMOD, y = Y_MOD;
    char b[48];
    module(x, y, W_MOD, H_MOD, "MEMORY", C_FRAME);

    if (act) snprintf(b, sizeof b, "%.0f MHz", s->mem_clock_mhz); else snprintf(b, sizeof b, "--");
    kv(x, y + 1, "MEM CLK", C_LABEL, b, C_CYAN);
    hbar(x + 1, y + 2, 19, u->f_mclk, G_MEM, NSTOP(G_MEM));

    if (act) snprintf(b, sizeof b, "%.1f/%.1f GB", s->vram_used_mib / 1024.0, s->vram_total_mib / 1024.0);
    else     snprintf(b, sizeof b, "--");
    kv(x, y + 4, "VRAM", C_LABEL, b, C_TEXT);

    /* VRAM as a 5x5 grid of memory chips that fill from the bottom */
    const int R = 5, C = 5;
    double filled = u->f_vram * R * C;
    for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) {
        int k = (R - 1 - r) * C + c;
        double f = clamp01(filled - k);
        RGB col = f > 0.02 ? grad(G_LOAD, NSTOP(G_LOAD), (k + 0.5) / (R * C)) : RGBc(42, 50, 62);
        uint32_t mid = f >= 0.75 ? 0x2588 : (f >= 0.40 ? 0x2593 : (f > 0.02 ? 0x2592 : 0x2591));
        int px = x + 1 + c * 4, py = y + 5 + r;
        put(px, py, 0x2590, col, 0); put(px + 1, py, mid, col, 0); put(px + 2, py, 0x258C, col, 0);
    }

    double used = act && s->vram_total_mib > 0 ? s->vram_used_mib / s->vram_total_mib * 100.0 : 0;
    if (act) snprintf(b, sizeof b, "%.1f%%", used); else snprintf(b, sizeof b, "--");
    kv(x, y + 10, "USED", C_LABEL, b, C_TEXT);
    if (act) snprintf(b, sizeof b, "%.1f GB", (s->vram_total_mib - s->vram_used_mib) / 1024.0);
    else     snprintf(b, sizeof b, "--");
    kv(x, y + 12, "FREE", C_LABEL, b, C_DIM);
    double free_frac = act && s->vram_total_mib > 0 ? 1.0 - u->f_vram : 0.0;
    hbar(x + 1, y + 13, 19, free_frac, G_LOAD, NSTOP(G_LOAD));
}

static void render(const GpuSnapshot *s, const Ui *u, double t, const char *bdf)
{
    bool act = snap_active(s);
    char b[16];
    g_amt = (float)u->amt;
    fb_clear();
    draw_title(s, t, bdf);
    draw_cable(s, u);
    draw_card(s, u, t);

    if (act) snprintf(b, sizeof b, "%.0f", s->temp_c); else snprintf(b, sizeof b, "--");
    vbar(X_TBAR, Y_BAR, H_BAR, "TEMP", u->f_temp, G_TEMP, NSTOP(G_TEMP), b, "\xC2\xB0""C");
    if (act) snprintf(b, sizeof b, "%.0f", s->gpu_clock_mhz); else snprintf(b, sizeof b, "--");
    vbar(X_CBAR, Y_BAR, H_BAR, "CORE", u->f_clk, G_CLK, NSTOP(G_CLK), b, "MHz");

    draw_left_module(s, u);
    draw_right_module(s, u);

    if (g_cfg.live)
        text_l(1, H - 1, C_DIM, 0, "q quit \xC2\xB7 p stop nvidia-smi polling to allow D3cold \xC2\xB7 LIVE \xC2\xB7 poll %.1fs", g_cfg.interval);
    else
        text_l(1, H - 1, C_DIM, 0, "q quit \xC2\xB7 d toggle D3cold \xC2\xB7 source: MOCK DATA%s", g_cfg.auto_cycle ? " (auto-cycling)" : "");
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
            if (c->cp == 0) continue;           /* wide-char continuation cell */
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

/* ---- PCI power state (sysfs; reading these never wakes the GPU) ---- */
static const char *pci_root(void)
{
    const char *e = getenv("GPUMON_PCI_ROOT");               /* handy for testing */
    return (e && *e) ? e : "/sys/bus/pci/devices";
}

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

static PciPm read_pm_state(const char *bdf)
{
    char path[512], v[32];
    snprintf(path, sizeof path, "%s/%s/power_state", pci_root(), bdf);
    if (!read_first_line(path, v, sizeof v))
        return PM_UNKNOWN;
    if (!strcmp(v, "D0"))     return PM_D0;
    if (!strcmp(v, "D1"))     return PM_D1;
    if (!strcmp(v, "D2"))     return PM_D2;
    if (!strcmp(v, "D3hot"))  return PM_D3HOT;
    if (!strcmp(v, "D3cold")) return PM_D3COLD;
    return PM_UNKNOWN;           /* transitioning/unrecognized: do not query */
}

static bool valid_bdf(const char *s)
{
    if (!*s || strlen(s) > 20) return false;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F') || *s == ':' || *s == '.')) return false;
    return true;
}

static bool autodetect_bdf(char *out, size_t n)
{
    DIR *d = opendir(pci_root());
    if (!d) return false;
    struct dirent *e; bool found = false;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[512], v[32], c[32];
        snprintf(p, sizeof p, "%s/%s/vendor", pci_root(), e->d_name);
        if (!read_first_line(p, v, sizeof v) || strtoul(v, NULL, 16) != 0x10de) continue;
        snprintf(p, sizeof p, "%s/%s/class", pci_root(), e->d_name);
        if (!read_first_line(p, c, sizeof c) || strncmp(c, "0x0300", 6) != 0) continue;
        size_t name_len = strlen(e->d_name);
        if (name_len == 0 || name_len > 20 || name_len >= n) continue;
        memcpy(out, e->d_name, name_len + 1);
        found = true;
    }
    closedir(d);
    return found;
}

/* ---- nvidia-smi (ONLY called when PCI state is D0) ---- */

/* On new drivers this field is also called clocks_event_reasons.active */
#define SMI_FIELDS \
    "power.draw,clocks.gr,clocks.mem,temperature.gpu,memory.used,memory.total," \
    "pstate,clocks_event_reasons.active,pcie.link.gen.current,pcie.link.gen.max," \
    "pcie.link.width.current,power.limit,utilization.gpu,name"
#define SMI_NFIELDS 14

static double num(const char *s, double def)       /* "[N/A]" and friends -> def */
{
    char *e;
    double v = strtod(s, &e);
    return e == s ? def : v;
}

static bool parse_smi_csv(char *line, GpuSnapshot *s)
{
    char *f[SMI_NFIELDS + 4]; int n = 0; char *p = line;
    while (n < SMI_NFIELDS + 3) {
        while (*p == ' ') p++;
        f[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = 0; p = c + 1;
    }
    for (int i = 0; i < n; i++) {
        size_t l = strlen(f[i]);
        while (l && (f[i][l - 1] == '\n' || f[i][l - 1] == '\r' || f[i][l - 1] == ' ')) f[i][--l] = 0;
    }
    if (n < SMI_NFIELDS) return false;

    s->power_w           = num(f[0], 0);
    s->gpu_clock_mhz     = num(f[1], 0);
    s->mem_clock_mhz     = num(f[2], 0);
    s->temp_c            = num(f[3], 0);
    s->vram_used_mib     = num(f[4], 0);
    s->vram_total_mib    = num(f[5], 0);
    s->pstate            = (f[6][0] == 'P') ? atoi(f[6] + 1) : -1;
    /* NVIDIA's legacy GPU Idle flag (bit 0) can remain set during active work. */
    s->clk_reasons       = strtoull(f[7], NULL, 16) & ~0x001ULL;
    s->pcie_gen          = (int)num(f[8], 0);
    s->pcie_gen_max      = (int)num(f[9], 0);
    s->pcie_width        = (int)num(f[10], 0);
    s->power_limit_w     = num(f[11], 0);
    s->gpu_util_pct      = num(f[12], 0);
    snprintf(s->name, sizeof s->name, "%s", f[13]);
    return true;
}

static int tuxedo_open_clevo(void)
{
    int fd = open("/dev/tuxedo_io", O_RDWR);
    if (fd < 0)
        return -1;
    int is_clevo = 0;
    if (ioctl(fd, R_HWCHECK_CL, &is_clevo) < 0 || !is_clevo) {
        close(fd);
        return -1;
    }
    return fd;
}

static void ensure_ec_sys(void)
{
    static bool attempted;
    if (access(EC_RAM_PATH, F_OK) == 0 || geteuid() != 0 || attempted)
        return;
    attempted = true;
    pid_t child = fork();
    if (child == 0) {
        execlp("modprobe", "modprobe", "ec_sys", (char *)NULL);
        _exit(127);
    }
    if (child < 0)
        return;
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 200000000L };
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

/* Match cctl's fan readings: duty from tuxedo_io and RPM from EC RAM.
 * This monitor's single fan readout uses the GPU fan values. */
static bool read_cctl_fans(double *gpu_duty_pct, double *gpu_rpm)
{
    int gpu_duty = 0, got_duty = 0;
    int fd = tuxedo_open_clevo();
    if (fd >= 0) {
        int raw = 0;
        if (ioctl(fd, R_CL_FANINFO2, &raw) >= 0) {
            gpu_duty = ((raw & 0xFF) * 100) / 255;
            got_duty = 1;
        }
        close(fd);
    }

    ensure_ec_sys();
    int ec_fd = open(EC_RAM_PATH, O_RDONLY);
    if (ec_fd < 0)
        return got_duty != 0;
    unsigned char ram[256] = {0};
    ssize_t count = read(ec_fd, ram, sizeof ram);
    close(ec_fd);
    if (count < 0xD4)
        return got_duty != 0;

    unsigned int raw16 = ((unsigned int)ram[0xD2] << 8) | ram[0xD3];
    *gpu_duty_pct = (double)gpu_duty;
    *gpu_rpm = raw16 ? (double)(EC_FAN_RPM_DIVISOR / raw16) : 0.0;
    return true;
}

static void smi_bus_id(const char *bdf, char *out, size_t n)   /* 0000:01:00.0 -> 00000000:01:00.0 */
{
    size_t len = strlen(bdf);
    if (len == 12 && n >= 17) {
        memcpy(out, "0000", 4);
        memcpy(out + 4, bdf, 13);
    } else if (len < n) {
        memcpy(out, bdf, len + 1);
    } else if (n > 0) {
        out[0] = '\0';
    }
}

static bool nvsmi_query(const char *bdf, GpuSnapshot *s)
{
    char id[32], cmd[768], line[1024];
    smi_bus_id(bdf, id, sizeof id);
    snprintf(cmd, sizeof cmd, "nvidia-smi -i %s --query-gpu=" SMI_FIELDS " --format=csv,noheader 2>/dev/null", id);
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    bool got = fgets(line, sizeof line, p) != NULL;
    int rc = pclose(p);
    return got && rc == 0 && parse_smi_csv(line, s);
}

/* PCIe throughput is not a --query-gpu field. `dmon -s t` reports rxpci/txpci in MB/s.
 * NOTE: dmon takes ~1 s per sample, which is fine because polling runs on its own thread.
 * Not verified against every driver's exact column layout - check yours. */
static bool nvsmi_pcie_rates(const char *bdf, double *rx, double *tx)
{
    char id[32], cmd[256], line[256];
    smi_bus_id(bdf, id, sizeof id);
    snprintf(cmd, sizeof cmd, "nvidia-smi dmon -i %s -s t -c 1 2>/dev/null", id);
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    bool ok = false;
    while (fgets(line, sizeof line, p)) {
        double a, b;
        if (line[0] == '#') continue;
        if (sscanf(line, " %*d %lf %lf", &a, &b) == 2) { *rx = a; *tx = b; ok = true; }
    }
    pclose(p);
    return ok;
}

static void live_poll(GpuSnapshot *s)
{
    s->pm = read_pm_state(g_cfg.bdf);
    s->fan_duty_pct = 0;
    s->fan_rpm = 0;
    s->fan_valid = read_cctl_fans(&s->fan_duty_pct, &s->fan_rpm);
    if (s->pm != PM_D0) {                 /* asleep / unknown: no nvidia-smi; keep reading fans */
        s->valid = false; s->smi_error = false;
        s->telemetry_paused = false;
        if (s->pm == PM_D3COLD)
            atomic_store(&g_wait_d3cold, 0);
        return;
    }
    if (atomic_load(&g_wait_d3cold)) {
        s->valid = false;
        s->smi_error = false;
        s->telemetry_paused = true;
        return;
    }

    s->telemetry_paused = false;
    GpuSnapshot t = *s;
    if (nvsmi_query(g_cfg.bdf, &t)) {
        if (atomic_load(&g_wait_d3cold)) {
            t.valid = false;
            t.smi_error = false;
            t.telemetry_paused = true;
            *s = t;
            return;
        }
        PciPm pm_after_query = read_pm_state(g_cfg.bdf);
        if (pm_after_query != PM_D0) {
            t.pm = pm_after_query;
            t.valid = false;
            t.smi_error = false;
            t.fan_duty_pct = 0;
            t.fan_rpm = 0;
            t.fan_valid = read_cctl_fans(&t.fan_duty_pct, &t.fan_rpm);
            t.telemetry_paused = false;
            if (pm_after_query == PM_D3COLD)
                atomic_store(&g_wait_d3cold, 0);
            *s = t;
            return;
        }
        double rx = 0, tx = 0;
        if (!atomic_load(&g_wait_d3cold) && nvsmi_pcie_rates(g_cfg.bdf, &rx, &tx)) { t.pcie_rx_mbs = rx; t.pcie_tx_mbs = tx; }
        if (atomic_load(&g_wait_d3cold)) {
            t.valid = false;
            t.smi_error = false;
            t.telemetry_paused = true;
            *s = t;
            return;
        }
        t.valid = true; t.smi_error = false;
        *s = t;
    } else {
        s->valid = false; s->smi_error = !atomic_load(&g_wait_d3cold);
        s->telemetry_paused = atomic_load(&g_wait_d3cold) != 0;
    }
}

/* ---- mock data ---- */
static void mock_poll(GpuSnapshot *s, double t)
{
    static bool was_asleep = false;
    static double wake_at = -1;
    bool asleep = atomic_load(&g_force_sleep) != 0;
    if (g_cfg.auto_cycle) asleep = fmod(t, 36.0) > 24.0;          /* 24 s awake, 12 s asleep */

    snprintf(s->name, sizeof s->name, "NVIDIA GeForce RTX 4060 Ti");
    s->smi_error = false;
    if (asleep) { s->pm = PM_D3COLD; s->valid = false; was_asleep = true; return; }
    if (was_asleep) { was_asleep = false; wake_at = t + 1.4; }     /* short "syncing" phase after wake */
    s->pm = PM_D0;
    if (t < wake_at) { s->valid = false; return; }
    s->valid = true;

    double load = 0.5 + 0.5 * sin(t * 0.5 - 1.2);
    load = load * load * (3 - 2 * load);
    load = clamp01(load + 0.02 * sin(t * 7.3) + 0.015 * sin(t * 13.1));

    s->gpu_util_pct = load * 100.0;
    s->temp_c = 36.0 + 44.0 * load + 2.0 * sin(t * 1.7);
    s->gpu_clock_max_mhz = 2850; s->gpu_clock_mhz = 210 + (2850 - 210) * load;
    s->mem_clock_max_mhz = 9001; s->mem_clock_mhz = load > 0.12 ? 9001 : (load > 0.05 ? 810 : 405);
    s->power_limit_w = 160;      s->power_w = 12 + 148 * load;
    s->fan_duty_pct = s->temp_c < 52 ? 0 : clampd(28 + (s->temp_c - 52) * 2.6, 28, 100);   /* 0-RPM mode when cool */
    s->fan_rpm = s->fan_duty_pct > 0 ? 600 + s->fan_duty_pct * 26 : 0;
    s->vram_total_mib = 8188;
    s->vram_used_mib = 900 + 5800 * (0.5 + 0.5 * sin(t * 0.21 + 0.7));
    s->pstate = load > 0.85 ? 0 : (load > 0.6 ? 2 : (load > 0.35 ? 3 : (load > 0.12 ? 5 : 8)));
    s->clk_reasons = s->pstate == 8 ? 0 : (s->pstate == 0 ? 0x4 : ((s->pstate == 2 || s->pstate == 3) ? 0x24 : 0));
    s->pcie_gen = load > 0.12 ? 4 : 1; s->pcie_gen_max = 4; s->pcie_width = 8;
    double burst = 0.5 + 0.5 * sin(t * 2.3);
    s->pcie_rx_mbs = load > 0.2 ? load * load * 2400 * burst : 0;
    s->pcie_tx_mbs = load > 0.2 ? load * 700 * (1 - burst * 0.6) : 0;
}

static void source_poll(GpuSnapshot *s, double t)
{
    if (g_cfg.live) live_poll(s); else mock_poll(s, t);
}

static void init_snapshot(GpuSnapshot *s)
{
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "NVIDIA GPU");
    s->pm = PM_UNKNOWN; s->pstate = -1;
}

static void *poller(void *arg)
{
    (void)arg;
    GpuSnapshot cur;
    init_snapshot(&cur);
    while (!g_quit) {
        source_poll(&cur, now_s() - g_t0);
        pthread_mutex_lock(&g_lock); g_snap = cur; pthread_mutex_unlock(&g_lock);
        double iv = g_cfg.live ? g_cfg.interval : 0.25;
        for (double w = 0; w < iv && !g_quit; w += 0.05) sleep_s(0.05);
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

static void term_enter(void)
{
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_old) == 0) {
        struct termios r = g_old;
        r.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        r.c_cc[VMIN] = 0; r.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &r);
        g_raw = true;
    }
    write_all("\x1b[?1049h\x1b[?25l\x1b[2J", 19);
}

static void term_leave(void)
{
    write_all("\x1b[0m\x1b[?25h\x1b[?1049l", 19);
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
        else if ((k[i] == 'd' || k[i] == 'D') && !g_cfg.live) atomic_store(&g_force_sleep, !atomic_load(&g_force_sleep));
        else if ((k[i] == 'p' || k[i] == 'P') && g_cfg.live) atomic_store(&g_wait_d3cold, !atomic_load(&g_wait_d3cold));
    }
}

static const char *pm_name(PciPm pm)
{
    switch (pm) {
    case PM_D0: return "D0";
    case PM_D1: return "D1";
    case PM_D2: return "D2";
    case PM_D3HOT: return "D3hot";
    case PM_D3COLD: return "D3cold";
    default: return "unknown";
    }
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

static void json_num(double v, bool valid, int precision)
{
    if (!valid || !isfinite(v)) fputs("null", stdout);
    else printf("%.*f", precision, v);
}

static void print_json_snapshot(const GpuSnapshot *s)
{
    bool nvsmi = s->pm == PM_D0 && !s->telemetry_paused;
    fputs("{\"power_state\":", stdout); json_string(pm_name(s->pm));
    printf(",\"gpu_name\":");
    if (s->valid && s->name[0]) json_string(s->name); else fputs("null", stdout);
    printf(",\"nvidia_smi_polled\":%s,\"nvidia_smi_error\":%s,\"telemetry_paused\":%s",
           nvsmi ? "true" : "false", s->smi_error ? "true" : "false",
           s->telemetry_paused ? "true" : "false");
    fputs(",\"temperature_c\":", stdout); json_num(s->temp_c, s->valid, 1);
    fputs(",\"gpu_util_pct\":", stdout); json_num(s->gpu_util_pct, s->valid, 1);
    fputs(",\"gpu_clock_mhz\":", stdout); json_num(s->gpu_clock_mhz, s->valid, 0);
    fputs(",\"gpu_clock_max_mhz\":", stdout); json_num(s->gpu_clock_max_mhz, s->valid, 0);
    fputs(",\"memory_clock_mhz\":", stdout); json_num(s->mem_clock_mhz, s->valid, 0);
    fputs(",\"memory_clock_max_mhz\":", stdout); json_num(s->mem_clock_max_mhz, s->valid, 0);
    fputs(",\"vram_used_mib\":", stdout); json_num(s->vram_used_mib, s->valid, 0);
    fputs(",\"vram_total_mib\":", stdout); json_num(s->vram_total_mib, s->valid, 0);
    fputs(",\"power_w\":", stdout); json_num(s->power_w, s->valid, 1);
    fputs(",\"power_limit_w\":", stdout); json_num(s->power_limit_w, s->valid, 1);
    fputs(",\"pstate\":", stdout);
    if (s->valid && s->pstate >= 0) printf("%d", s->pstate); else fputs("null", stdout);
    fputs(",\"clock_reasons_active\":", stdout);
    if (s->valid) printf("%llu", s->clk_reasons); else fputs("null", stdout);
    fputs(",\"pcie_gen\":", stdout); if (s->valid && s->pcie_gen > 0) printf("%d", s->pcie_gen); else fputs("null", stdout);
    fputs(",\"pcie_gen_max\":", stdout); if (s->valid && s->pcie_gen_max > 0) printf("%d", s->pcie_gen_max); else fputs("null", stdout);
    fputs(",\"pcie_width\":", stdout); if (s->valid && s->pcie_width > 0) printf("%d", s->pcie_width); else fputs("null", stdout);
    fputs(",\"pcie_rx_mbs\":", stdout); json_num(s->pcie_rx_mbs, s->valid, 1);
    fputs(",\"pcie_tx_mbs\":", stdout); json_num(s->pcie_tx_mbs, s->valid, 1);
    fputs(",\"gpu_fan_duty_pct\":", stdout); json_num(s->fan_duty_pct, s->fan_valid, 1);
    fputs(",\"gpu_fan_rpm\":", stdout); json_num(s->fan_rpm, s->fan_valid, 0);
    puts("}");
}

int cctl_gpumon(int argc, char **argv)
{
    g_cfg.live = true;
    if (argc > 3 || (argc == 3 && strcmp(argv[2], "--json") != 0)) {
        fprintf(stderr, "Usage: cctl gpumon [--json]\n");
        return 1;
    }
    g_cfg.json = argc == 3;
    if (!g_cfg.bdf[0] && !autodetect_bdf(g_cfg.bdf, sizeof g_cfg.bdf)) {
        fprintf(stderr, "No NVIDIA GPU found in %s.\n", pci_root());
        return 1;
    }
    if (!valid_bdf(g_cfg.bdf)) {
        fprintf(stderr, "Error: invalid NVIDIA PCI device address.\n");
        return 1;
    }

    static char out[1 << 18];
    Buf b = { out, 0, sizeof out };
    Ui ui; memset(&ui, 0, sizeof ui);
    g_t0 = now_s();

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    init_snapshot(&g_snap);
    pthread_t th;
    if (pthread_create(&th, NULL, poller, NULL) != 0) { fprintf(stderr, "thread failed\n"); return 1; }

    if (g_cfg.json) {
        while (!g_quit) {
            sleep_s(g_cfg.interval);
            GpuSnapshot s;
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
        if (cols != pc || rows != pr) { write_all("\x1b[2J", 4); pc = cols; pr = rows; }
        if (cols < W || rows < H) {
            char m[96];
            int n = snprintf(m, sizeof m, "\x1b[H\x1b[0mTerminal too small: need %dx%d, have %dx%d ", W, H, cols, rows);
            write_all(m, (size_t)n);
            sleep_s(0.2);
            continue;
        }

        GpuSnapshot s;
        pthread_mutex_lock(&g_lock); s = g_snap; pthread_mutex_unlock(&g_lock);

        ui_update(&ui, &s, dt);
        render(&s, &ui, t - g_t0, g_cfg.live ? g_cfg.bdf : NULL);
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
