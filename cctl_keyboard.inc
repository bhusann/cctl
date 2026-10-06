/* ========================================================================
 * KEYBOARD BACKLIGHT
 * ======================================================================== */

#define KBD_PATH "/sys/class/leds/rgb:kbd_backlight"

static int kbd_get_brightness(void)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/brightness", KBD_PATH);
    long raw = read_sysfs_long(path, -1);
    if (raw < 0) return -1;
    return (int)((raw * 100 + 127) / 255);
}

static int kbd_get_color(int *r, int *g, int *b)
{
    char buf[64];
    if (read_sysfs_str(KBD_PATH "/multi_intensity", buf, sizeof(buf)) < 0)
        return -1;
    if (sscanf(buf, "%d %d %d", r, g, b) != 3)
        return -1;
    return 0;
}

static int kbd_write_color(int r, int g, int b)
{
    char val[64];
    snprintf(val, sizeof(val), "%d %d %d", r, g, b);
    return write_sysfs(KBD_PATH "/multi_intensity", val);
}

static void kbd_restore_state(int r, int g, int b, int bri)
{
    kbd_write_color(r, g, b);
    write_sysfs_int(KBD_PATH "/brightness", bri);
}

static int kbd_set_color(int r, int g, int b)
{
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        fprintf(stderr, "Error: RGB values must be 0-255\n");
        return -1;
    }
    if (kbd_write_color(r, g, b) < 0) {
        fprintf(stderr, "Error: Failed to set keyboard color (is tuxedo_keyboard loaded?)\n");
        return -1;
    }
    int bri = kbd_get_brightness();
    if (bri >= 0)
        printf("  Keyboard color: RGB(%d, %d, %d)  [brightness: %d%%]\n", r, g, b, bri);
    else
        printf("  Keyboard color: RGB(%d, %d, %d)\n", r, g, b);
    return 0;
}

static int kbd_set_brightness(int percent)
{
    if (percent < 0 || percent > 100) {
        fprintf(stderr, "Error: Brightness must be 0-100%%\n");
        return -1;
    }
    int raw = (percent * 255) / 100;
    char path[128];
    snprintf(path, sizeof(path), "%s/brightness", KBD_PATH);

    if (write_sysfs_int(path, raw) < 0) {
        fprintf(stderr, "Error: Failed to set keyboard brightness (is tuxedo_keyboard loaded?)\n");
        return -1;
    }
    printf("  Keyboard brightness: %d%%\n", percent);
    return 0;
}

/* ========================================================================
 * KEYBOARD COLOR PRESETS
 * ======================================================================== */

struct kbd_preset {
    const char *name;
    uint8_t r, g, b;
};

static const struct kbd_preset kbd_presets[] = {
    { "blue",         0,   0, 255 },
    { "chocolate",  210, 105,  30 },
    { "coral",      255, 127,  80 },
    { "cyan",         0, 255, 255 },
    { "gold",       255, 215,   0 },
    { "gray",       128, 128, 128 },
    { "green",        0, 200,   0 },
    { "indigo",      75,   0, 130 },
    { "lime",       128, 255,   0 },
    { "magenta",    255,   0, 255 },
    { "maroon",     128,   0,   0 },
    { "navy",         0,   0, 128 },
    { "olive",      128, 128,   0 },
    { "orange",     255, 136,   0 },
    { "pink",       255,  20, 147 },
    { "purple",     136,   0, 255 },
    { "red",        255,   0,   0 },
    { "salmon",     250, 128, 114 },
    { "silver",     192, 192, 192 },
    { "teal",         0, 128, 128 },
    { "turquoise",   64, 224, 208 },
    { "violet",     238, 130, 238 },
    { "white",      255, 255, 255 },
    { "yellow",     255, 255,   0 },
    { "off",          0,   0,   0 },
    { NULL,           0,   0,   0 }
};

static int kbd_set_preset(const char *name)
{
    /* Case-insensitive lookup */
    char lower[64];
    size_t i;
    for (i = 0; i < sizeof(lower) - 1 && name[i]; i++)
        lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = '\0';

    for (const struct kbd_preset *p = kbd_presets; p->name; p++) {
        if (strcmp(p->name, lower) == 0) {
            return kbd_set_color(p->r, p->g, p->b);
        }
    }

    fprintf(stderr, "Error: Unknown preset '%s'\n", name);
    fprintf(stderr, "Available presets:\n");
    for (const struct kbd_preset *p = kbd_presets; p->name; p++)
        fprintf(stderr, "  %-11s (%d, %d, %d)\n", p->name, p->r, p->g, p->b);
    return -1;
}

static const char *kbd_find_preset_name(int r, int g, int b)
{
    for (const struct kbd_preset *p = kbd_presets; p->name; p++) {
        if ((int)p->r == r && (int)p->g == g && (int)p->b == b)
            return p->name;
    }
    return NULL;
}

static void kbd_show_presets(void)
{
    printf("%sUsage:%s %scctl kbc <R G B | preset>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
    printf("  %sExamples:%s cctl kbc 255 0 128  |  cctl kbc cyan\n\n", C_DIM, C_RST);

    int cur_r = 0, cur_g = 0, cur_b = 0;
    if (kbd_get_color(&cur_r, &cur_g, &cur_b) == 0) {
        const char *pname = kbd_find_preset_name(cur_r, cur_g, cur_b);
        int bri = kbd_get_brightness();
        printf("%sCurrent Color:%s\n", C_YLW, C_RST);
        if (pname) {
            printf("  %sPreset:%s     %s%s%s\n", C_CYN, C_RST, C_CYN, pname, C_RST);
        } else {
            printf("  %sPreset:%s     %scustom%s\n", C_CYN, C_RST, C_YLW, C_RST);
        }
        if (bri >= 0)
            printf("  %sRGB:%s        %sRGB(%d, %d, %d)%s  %s[brightness: %d%%]%s\n\n",
                   C_CYN, C_RST, C_CYN, cur_r, cur_g, cur_b, C_RST, C_DIM, bri, C_RST);
        else
            printf("  %sRGB:%s        %sRGB(%d, %d, %d)%s\n\n",
                   C_CYN, C_RST, C_CYN, cur_r, cur_g, cur_b, C_RST);
    }

    printf("%sAvailable Presets:%s\n", C_YLW, C_RST);
    int col = 0;
    for (const struct kbd_preset *p = kbd_presets; p->name; p++) {
        printf("  %s%-11s%s (%3d, %3d, %3d)", C_CYN, p->name, C_RST, p->r, p->g, p->b);
        col++;
        if (col % 2 == 0) printf("\n");
    }
    if (col % 2 != 0) printf("\n");
}

/* ========================================================================
 * KEYBOARD EFFECTS (kbe)
 * ======================================================================== */

static void get_mem_usage(long *used_mb, long *total_mb);

#define KBE_LOCK_PATH   "/run/cctl_kbe.lock"
#define KBE_STATE_PATH  "/run/cctl_kbe.state"

static volatile sig_atomic_t g_kbe_running = 1;

static void kbe_sig_handler(int sig)
{
    (void)sig;
    g_kbe_running = 0;
}

static const uint8_t kbe_breathe_lut[128] = {
      8,   8,   8,   8,   8,   8,   8,   8,   8,   9,   9,   9,  10,  10,  11,  12,
     13,  15,  16,  18,  20,  23,  25,  28,  32,  35,  39,  43,  48,  53,  58,  64,
     70,  76,  82,  89,  96, 103, 111, 118, 126, 134, 142, 150, 157, 165, 173, 181,
    188, 195, 202, 209, 215, 221, 227, 232, 237, 241, 244, 248, 250, 252, 254, 255,
    255, 255, 254, 252, 250, 248, 244, 241, 237, 232, 227, 221, 215, 209, 202, 195,
    188, 181, 173, 165, 157, 150, 142, 134, 126, 118, 111, 103,  96,  89,  82,  76,
     70,  64,  58,  53,  48,  43,  39,  35,  32,  28,  25,  23,  20,  18,  16,  15,
     13,  12,  11,  10,  10,   9,   9,   9,   8,   8,   8,   8,   8,   8,   8,   8
};

static void kbe_sleep_ms(int ms)
{
    while (ms > 0 && g_kbe_running) {
        int chunk = ms > 25 ? 25 : ms;
        usleep((useconds_t)chunk * 1000);
        ms -= chunk;
    }
}

static void kbe_hue_to_rgb(int hue, int brightness, int *r, int *g, int *b)
{
    hue = ((hue % 360) + 360) % 360;
    int sector = hue / 60;
    int rem = hue % 60;
    int inc = (rem * 255) / 60;
    int dec = 255 - inc;
    int r1 = 0, g1 = 0, b1 = 0;

    switch (sector) {
        case 0: r1 = 255; g1 = inc; b1 = 0;   break;
        case 1: r1 = dec; g1 = 255; b1 = 0;   break;
        case 2: r1 = 0;   g1 = 255; b1 = inc; break;
        case 3: r1 = 0;   g1 = dec; b1 = 255; break;
        case 4: r1 = inc; g1 = 0;   b1 = 255; break;
        default: r1 = 255; g1 = 0;  b1 = dec; break;
    }

    if (brightness < 255) {
        if (brightness < 0) brightness = 0;
        r1 = (r1 * brightness) / 255;
        g1 = (g1 * brightness) / 255;
        b1 = (b1 * brightness) / 255;
    }

    *r = r1;
    *g = g1;
    *b = b1;
}

static inline void kbe_write_frame(int fd_col, int r, int g, int b)
{
    if (fd_col < 0) return;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d %d %d", r, g, b);
    if (len > 0) {
        lseek(fd_col, 0, SEEK_SET);
        ssize_t w = write(fd_col, buf, (size_t)len);
        (void)w;
    }
}

static inline void kbe_write_bri(int fd_bri, int bri)
{
    if (fd_bri < 0) return;
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", bri);
    if (len > 0) {
        lseek(fd_bri, 0, SEEK_SET);
        ssize_t w = write(fd_bri, buf, (size_t)len);
        (void)w;
    }
}


static int kbd_get_raw_brightness(int *bri)
{
    long val = read_sysfs_long(KBD_PATH "/brightness", -1);
    if (val < 0) return -1;
    *bri = (int)val;
    return 0;
}

/* Kernel-assigned process start time (field 22 of /proc/<pid>/stat).
 * The (pid, starttime) pair uniquely identifies one process instance for
 * the life of the boot: unlike the PID it can never be recycled, and
 * unlike the process name (comm/argv0) it cannot be chosen or forged by
 * whatever process happens to hold that PID. Returns -1 if unreadable. */
static long proc_starttime(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    /* comm sits inside parentheses and may contain spaces/parens, so parse
     * from the LAST ')' onward. Fields after that: state(3), ppid(4), ...
     * starttime is field 22 → the 19th token after the paren. */
    char *rp = strrchr(buf, ')');
    if (!rp || !rp[1]) return -1;
    char *p = rp + 1;
    int field = 3;
    while (*p == ' ') p++;
    while (field < 22 && *p) {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        field++;
    }
    if (field != 22 || !*p) return -1;
    return strtol(p, NULL, 10);
}

static int kbe_read_state(struct kbe_state *st)
{
    memset(st, 0, sizeof(*st));
    st->starttime = -1;
    st->orig_r = 255;
    st->orig_g = 255;
    st->orig_b = 255;
    st->orig_bri = 255;

    FILE *fp = fopen(KBE_STATE_PATH, "r");
    if (!fp) return -1;
    char line[128];
    long p = -1, start = -1;
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return -1; }
    if (sscanf(line, "%ld %ld", &p, &start) < 1 || p <= 1) {
        fclose(fp);
        return -1;
    }
    st->pid = (pid_t)p;
    st->starttime = start;
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    for (char *c = line; *c; c++) {
        if (*c == '\n' || *c == '\r') *c = '\0';
    }
    strncpy(st->effect, line, sizeof(st->effect) - 1);

    if (fgets(line, sizeof(line), fp)) {
        int r = 255, g = 255, b = 255, bri = 255;
        if (sscanf(line, "%d %d %d %d", &r, &g, &b, &bri) >= 3) {
            st->orig_r = r;
            st->orig_g = g;
            st->orig_b = b;
            st->orig_bri = bri;
        }
    }
    if (fgets(line, sizeof(line), fp)) {
        for (char *c = line; *c; c++) {
            if (*c == '\n' || *c == '\r') *c = '\0';
        }
        if (strcmp(line, "none") != 0) {
            strncpy(st->resume_effect, line, sizeof(st->resume_effect) - 1);
        }
    }
    fclose(fp);
    return 0;
}

static int kbe_is_running(struct kbe_state *st)
{
    struct kbe_state local_st;
    if (!st) st = &local_st;

    if (kbe_read_state(st) < 0)
        return 0;

    int alive = (kill(st->pid, 0) == 0 || errno == EPERM);
    if (alive && st->starttime > 0 && proc_starttime(st->pid) != st->starttime)
        alive = 0;

    if (alive) {
        if (strcmp(st->effect, "pulse-profile") == 0) {
            if (st->resume_effect[0] != '\0') {
                snprintf(st->effect, sizeof(st->effect), "%.20s", st->resume_effect);
            } else {
                snprintf(st->effect, sizeof(st->effect), "profile pulse");
            }
        }
        return 1;
    }

    if (geteuid() == 0) {
        unlink(KBE_STATE_PATH);
    }
    return 0;
}

static int kbe_stop(int quiet)
{
    struct kbe_state ks;
    if (!kbe_is_running(&ks)) {
        if (!quiet)
            printf("No active keyboard effect is currently running.\n");
        return 0;
    }

    kill(ks.pid, SIGTERM);

    int exited = 0;
    for (int i = 0; i < 30; i++) {
        usleep(20000);
        if (kill(ks.pid, 0) != 0 && errno == ESRCH) {
            exited = 1;
            break;
        }
    }

    if (!exited) {
        kill(ks.pid, SIGKILL);
        usleep(20000);
    }

    kbd_restore_state(ks.orig_r, ks.orig_g, ks.orig_b, ks.orig_bri);
    unlink(KBE_STATE_PATH);
    unlink(KBE_LOCK_PATH);

    if (!quiet)
        printf("Stopped keyboard effect '%s' [PID %d] and restored original state.\n", ks.effect, (int)ks.pid);

    return 0;
}

static int kbe_candle_step(int *flicker_val)
{
    int target = 160 + (rand() % 95);
    if ((rand() % 15) == 0)
        target = 90;
    *flicker_val = (*flicker_val * 6 + target * 4) / 10;
    return *flicker_val;
}

static pid_t daemonize(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > 2) close(null_fd);
        }
    }
    return pid;
}

static int kbe_acquire_worker_lock(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = kbe_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int lock_fd = open(KBE_LOCK_PATH, O_RDWR | O_CREAT, 0600);
    if (lock_fd < 0) return -1;

    for (int i = 0; i < 75; i++) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0)
            return lock_fd;
        usleep(20000);
    }
    close(lock_fd);
    return -1;
}

struct kbe_worker_ctx {
    int lock_fd;
    int fd_col;
    int fd_bri;
    int orig_r, orig_g, orig_b, orig_bri;
};

static int kbe_worker_begin(struct kbe_worker_ctx *ctx, const char *effect,
                            int orig_r, int orig_g, int orig_b, int orig_bri,
                            const char *resume_effect)
{
    pin_to_e_cores();

    ctx->lock_fd = kbe_acquire_worker_lock();
    if (ctx->lock_fd < 0) return -1;

    FILE *fp = fopen(KBE_STATE_PATH, "w");
    if (!fp) {
        flock(ctx->lock_fd, LOCK_UN);
        close(ctx->lock_fd);
        ctx->lock_fd = -1;
        return -1;
    }
    chmod(KBE_STATE_PATH, 0600);
    fprintf(fp, "%d %ld\n%s\n%d %d %d %d\n%s\n",
            (int)getpid(), proc_starttime(getpid()),
            effect, orig_r, orig_g, orig_b, orig_bri,
            (resume_effect && *resume_effect) ? resume_effect : "none");
    fclose(fp);

    ctx->orig_r = orig_r;
    ctx->orig_g = orig_g;
    ctx->orig_b = orig_b;
    ctx->orig_bri = orig_bri;
    ctx->fd_col = open(KBD_PATH "/multi_intensity", O_WRONLY);
    ctx->fd_bri = open(KBD_PATH "/brightness", O_WRONLY);
    return 0;
}

static void kbe_worker_end(struct kbe_worker_ctx *ctx, int restore)
{
    if (restore) {
        if (ctx->fd_bri >= 0 && ctx->orig_bri >= 0)
            kbe_write_bri(ctx->fd_bri, ctx->orig_bri);
        if (ctx->fd_col >= 0)
            kbe_write_frame(ctx->fd_col, ctx->orig_r, ctx->orig_g, ctx->orig_b);
    }
    if (ctx->fd_col >= 0) { close(ctx->fd_col); ctx->fd_col = -1; }
    if (ctx->fd_bri >= 0) { close(ctx->fd_bri); ctx->fd_bri = -1; }
    unlink(KBE_STATE_PATH);
    if (ctx->lock_fd >= 0) {
        flock(ctx->lock_fd, LOCK_UN);
        close(ctx->lock_fd);
        ctx->lock_fd = -1;
    }
    unlink(KBE_LOCK_PATH);
}

static const uint8_t kbe_pulse_wave[] = {
    30, 90, 180, 255, 230, 160, 100, 60, 40,
    90, 170, 230, 190, 130, 80, 40, 20, 10, 0
};

static void fx_breathe(int fd, int r, int g, int b, int *step)
{
    int bri = (int)kbe_breathe_lut[*step % 128];
    kbe_write_frame(fd, (r * bri) / 255, (g * bri) / 255, (b * bri) / 255);
    kbe_sleep_ms(25);
    (*step)++;
}

static void fx_breathe_cycle(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b;
    int bri = (int)kbe_breathe_lut[*step % 128];
    int hue = (*step / 2) % 360;
    int cr, cg, cb;
    kbe_hue_to_rgb(hue, bri, &cr, &cg, &cb);
    kbe_write_frame(fd, cr, cg, cb);
    kbe_sleep_ms(25);
    (*step)++;
}

static void fx_cycle(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b;
    int cr, cg, cb;
    kbe_hue_to_rgb(*step % 360, 255, &cr, &cg, &cb);
    kbe_write_frame(fd, cr, cg, cb);
    kbe_sleep_ms(50); /* 20 Hz: battery friendly */
    (*step)++;
}

static void fx_flash(int fd, int r, int g, int b, int *step)
{
    (void)step;
    for (int i = 0; i < 3 && g_kbe_running; i++) {
        kbe_write_frame(fd, r, g, b);
        kbe_sleep_ms(70);
        if (!g_kbe_running) return;
        kbe_write_frame(fd, 0, 0, 0);
        kbe_sleep_ms(i == 2 ? 750 : 70);
    }
}

static void fx_flash_cycle(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b;
    int fr, fg, fb;
    kbe_hue_to_rgb(*step, 255, &fr, &fg, &fb);
    *step = (*step + 55) % 360;
    for (int i = 0; i < 3 && g_kbe_running; i++) {
        kbe_write_frame(fd, fr, fg, fb);
        kbe_sleep_ms(70);
        if (!g_kbe_running) return;
        kbe_write_frame(fd, 0, 0, 0);
        kbe_sleep_ms(i == 2 ? 750 : 70);
    }
}

static void fx_candle(int fd, int r, int g, int b, int *step)
{
    (void)step;
    static int candle_val = 200;
    int val = kbe_candle_step(&candle_val);
    kbe_write_frame(fd, (r * val) / 255, (g * val) / 255, (b * val) / 255);
    kbe_sleep_ms(35 + (rand() % 35));
}

static void fx_pulse(int fd, int r, int g, int b, int *step)
{
    (void)step;
    for (size_t i = 0; i < sizeof(kbe_pulse_wave) && g_kbe_running; i++) {
        int val = kbe_pulse_wave[i];
        kbe_write_frame(fd, (r * val) / 255, (g * val) / 255, (b * val) / 255);
        kbe_sleep_ms(22);
    }
    kbe_sleep_ms(700);
}

static void fx_pulse_cycle(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b;
    int pr, pg, pb;
    kbe_hue_to_rgb(*step, 255, &pr, &pg, &pb);
    *step = (*step + 55) % 360;
    for (size_t i = 0; i < sizeof(kbe_pulse_wave) && g_kbe_running; i++) {
        int val = kbe_pulse_wave[i];
        kbe_write_frame(fd, (pr * val) / 255, (pg * val) / 255, (pb * val) / 255);
        kbe_sleep_ms(22);
    }
    kbe_sleep_ms(700);
}

static void fx_police(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b; (void)step;
    const uint8_t colors[2][3] = { {255, 0, 0}, {0, 0, 255} };
    for (int c = 0; c < 2 && g_kbe_running; c++) {
        for (int k = 0; k < 2 && g_kbe_running; k++) {
            kbe_write_frame(fd, colors[c][0], colors[c][1], colors[c][2]);
            kbe_sleep_ms(60);
            if (!g_kbe_running) return;
            kbe_write_frame(fd, 0, 0, 0);
            kbe_sleep_ms(50);
        }
        kbe_sleep_ms(70);
    }
}

static void fx_gunfire(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b; (void)step;
    enum { GUNFIRE_FLASH, GUNFIRE_AFTERGLOW, GUNFIRE_GAP, GUNFIRE_PAUSE };
    static int phase = GUNFIRE_FLASH;
    static int shots_left = 0;

    if (phase == GUNFIRE_PAUSE) {
        kbe_sleep_ms(350 + rand() % 501);
        shots_left = 4 + rand() % 5; /* short automatic-fire burst */
        phase = GUNFIRE_FLASH;
        return;
    }
    if (shots_left == 0) {
        shots_left = 4 + rand() % 5;
    }

    switch (phase) {
    case GUNFIRE_FLASH:
        kbe_write_frame(fd, 255, 238 + rand() % 18, 190 + rand() % 66);
        kbe_sleep_ms(25 + rand() % 16);
        phase = GUNFIRE_AFTERGLOW;
        break;
    case GUNFIRE_AFTERGLOW:
        kbe_write_frame(fd, 255, 90 + rand() % 81, 8 + rand() % 25);
        kbe_sleep_ms(20 + rand() % 21);
        phase = GUNFIRE_GAP;
        break;
    case GUNFIRE_GAP:
        kbe_write_frame(fd, 0, 0, 0);
        if (--shots_left > 0) {
            kbe_sleep_ms(35 + rand() % 46); /* irregular rapid shot spacing */
            phase = GUNFIRE_FLASH;
        } else {
            phase = GUNFIRE_PAUSE;
        }
        break;
    case GUNFIRE_PAUSE:
        break; /* handled above */
    }
}

static void fx_fire(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b; (void)step;
    static int fire_hue = 20, fire_bri = 200;
    int target_h = 5 + (rand() % 35);
    int target_b = 130 + (rand() % 125);
    if ((rand() % 14) == 0) {
        target_b = 255;
        target_h = 36;
    }
    fire_hue = (fire_hue * 6 + target_h * 4) / 10;
    fire_bri = (fire_bri * 6 + target_b * 4) / 10;
    int fr, fg, fb;
    kbe_hue_to_rgb(fire_hue, fire_bri, &fr, &fg, &fb);
    kbe_write_frame(fd, fr, fg, fb);
    kbe_sleep_ms(30 + (rand() % 25));
}

static void fx_aurora(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b;
    int t = *step % 480;
    int cur_hue;
    if (t < 160) {
        cur_hue = 130 + (t * 50) / 160;
    } else if (t < 320) {
        cur_hue = 180 + ((t - 160) * 95) / 160;
    } else {
        cur_hue = (275 + ((t - 320) * 215) / 160) % 360;
    }
    int bri = 130 + ((int)kbe_breathe_lut[*step % 128] * 125) / 255;
    int ar, ag, ab;
    kbe_hue_to_rgb(cur_hue, bri, &ar, &ag, &ab);
    kbe_write_frame(fd, ar, ag, ab);
    kbe_sleep_ms(50); /* 20 Hz: battery friendly */
    (*step)++;
}

static void fx_storm(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b; (void)step;
    kbe_write_frame(fd, 8, 12, 35);
    int wait_ms = 1200 + (rand() % 2800);
    while (wait_ms > 0 && g_kbe_running) {
        if ((rand() % 10) == 0) {
            int rumble = 25 + (rand() % 30);
            kbe_write_frame(fd, rumble / 4, rumble / 3, rumble);
            kbe_sleep_ms(35);
            kbe_write_frame(fd, 8, 12, 35);
        }
        int chunk = wait_ms > 80 ? 80 : wait_ms;
        kbe_sleep_ms(chunk);
        wait_ms -= chunk;
    }
    if (!g_kbe_running) return;

    kbe_write_frame(fd, 255, 255, 255);
    kbe_sleep_ms(45);
    kbe_write_frame(fd, 30, 45, 90);
    kbe_sleep_ms(35);
    kbe_write_frame(fd, 220, 240, 255);
    kbe_sleep_ms(60);
    if ((rand() % 2) == 0) {
        kbe_write_frame(fd, 20, 30, 60);
        kbe_sleep_ms(25);
        kbe_write_frame(fd, 180, 210, 255);
        kbe_sleep_ms(40);
    }
    kbe_write_frame(fd, 60, 90, 160);
    kbe_sleep_ms(50);
    kbe_write_frame(fd, 20, 30, 70);
    kbe_sleep_ms(60);
}

static void kbe_sos_step(int fd, int r, int g, int b, int *step)
{
    /* Morse SOS: ... --- ...; one unit is five 30 ms frames. */
    static const uint8_t sos_units[] = {
        1, 1, 1, 1, 1, 3, 3, 1, 3, 1, 3, 3, 1, 1, 1, 1, 1, 7
    };
    /* 34 Morse units at five frames per unit make a 170-frame cycle. */
    int frame = *step % 170;
    *step = (frame + 1) % 170;
    int segment = 0;
    for (size_t i = 0; i < sizeof(sos_units); i++) {
        int duration = sos_units[i] * 5;
        if (frame < duration) {
            segment = (int)i;
            break;
        }
        frame -= duration;
    }
    if ((segment % 2) == 0)
        kbe_write_frame(fd, r, g, b);
    else
        kbe_write_frame(fd, 0, 0, 0);
    kbe_sleep_ms(30);
}

static void fx_sos(int fd, int r, int g, int b, int *step)
{
    kbe_sos_step(fd, r, g, b, step);
}

static void fx_firecrackers(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b; (void)step;
    enum { FIRECRACKERS_NIGHT, FIRECRACKERS_LAUNCH, FIRECRACKERS_APEX,
           FIRECRACKERS_DETONATE, FIRECRACKERS_DECAY };
    static int phase = FIRECRACKERS_NIGHT;
    static int frame = 0, night_frames = -1, salvo_remaining = 0;
    static int hue = 0, decay_brightness = 255, decay_frames = 10;

    if (phase == FIRECRACKERS_NIGHT) {
        if (night_frames < 0) {
            int wait_ms = 800 + rand() % 1400;
            night_frames = (wait_ms + 29) / 30;
            int roll = rand() % 100;
            salvo_remaining = roll < 70 ? 1 : 2 + rand() % 2;
        }
        kbe_write_frame(fd, 2, 4, 12);
        kbe_sleep_ms(30);
        if (--night_frames <= 0) {
            phase = FIRECRACKERS_LAUNCH;
            frame = 0;
        }
        return;
    }

    if (phase == FIRECRACKERS_LAUNCH) {
        int factor = (frame * 150) / 4; /* 0% to about 60% in five frames */
        int cr, cg, cb;
        kbe_hue_to_rgb(30, factor, &cr, &cg, &cb);
        kbe_write_frame(fd, cr, cg, cb);
        kbe_sleep_ms(30);
        if (++frame >= 5) {
            phase = FIRECRACKERS_APEX;
            frame = 0;
        }
        return;
    }

    if (phase == FIRECRACKERS_APEX) {
        kbe_write_frame(fd, 0, 0, 0);
        kbe_sleep_ms(30 + rand() % 21);
        hue = rand() % 360; /* new spark color for every cracker */
        phase = FIRECRACKERS_DETONATE;
        return;
    }

    if (phase == FIRECRACKERS_DETONATE) {
        kbe_write_frame(fd, 255, 255, 255);
        kbe_sleep_ms(40);
        decay_brightness = 255;
        decay_frames = 10 + rand() % 3;
        frame = 0;
        phase = FIRECRACKERS_DECAY;
        return;
    }

    if (phase == FIRECRACKERS_DECAY) {
        if (frame > 0)
            decay_brightness = (decay_brightness * 7) / 10;
        decay_brightness += (rand() % 77) - 38; /* roughly ±15% flicker */
        if (decay_brightness < 0) decay_brightness = 0;
        if (decay_brightness > 255) decay_brightness = 255;
        if (frame + 1 >= decay_frames && decay_brightness > 15)
            decay_brightness = 15;
        int cr, cg, cb;
        kbe_hue_to_rgb(hue, decay_brightness, &cr, &cg, &cb);
        kbe_write_frame(fd, cr, cg, cb);
        kbe_sleep_ms(30);
        if (++frame >= decay_frames) {
            frame = 0;
            if (--salvo_remaining > 0) {
                /* Start the next rocket immediately after this ember decay. */
                phase = FIRECRACKERS_LAUNCH;
            } else {
                night_frames = -1;
                phase = FIRECRACKERS_NIGHT;
            }
        }
        return;
    }

}

static int kbe_color_lerp(int start, int end, int ratio)
{
    return start + ((end - start) * ratio) / 255;
}

static void fx_temp(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b;
    static int cur_temp_val = -1;
    static int tgt_r = 0, tgt_g = 220, tgt_b = 255;
    static int smooth_r = 0, smooth_g = 220, smooth_b = 255;
    static int sos_step = 0;

    if ((*step % 20) == 0 || cur_temp_val < 0) {
        int t = read_cpu_temp();
        if (t > 0) cur_temp_val = t;
        else if (cur_temp_val < 0) cur_temp_val = 50;

        if (cur_temp_val < 40) {
            tgt_r = 0; tgt_g = 220; tgt_b = 255;
        } else if (cur_temp_val <= 60) {
            int ratio = ((cur_temp_val - 40) * 255) / 20;
            tgt_r = kbe_color_lerp(128, 0, ratio);
            tgt_g = kbe_color_lerp(255, 128, ratio);
            tgt_b = 0;
        } else if (cur_temp_val <= 75) {
            int ratio = ((cur_temp_val - 60) * 255) / 15;
            tgt_r = kbe_color_lerp(0, 255, ratio);
            tgt_g = kbe_color_lerp(128, 255, ratio);
            tgt_b = 0;
        } else if (cur_temp_val <= 85) {
            int ratio = ((cur_temp_val - 75) * 255) / 10;
            tgt_r = 255;
            tgt_g = kbe_color_lerp(255, 128, ratio);
            tgt_b = 0;
        } else if (cur_temp_val < 90) {
            int ratio = ((cur_temp_val - 85) * 255) / 5;
            tgt_r = 255;
            tgt_g = kbe_color_lerp(128, 0, ratio);
            tgt_b = 0;
        } else {
            tgt_r = 255; tgt_g = 0; tgt_b = 0;
        }
    }

    smooth_r = (smooth_r * 8 + tgt_r * 2) / 10;
    smooth_g = (smooth_g * 8 + tgt_g * 2) / 10;
    smooth_b = (smooth_b * 8 + tgt_b * 2) / 10;

    if (cur_temp_val >= 90) {
        kbe_sos_step(fd, 255, 0, 0, &sos_step);
        (*step)++;
        return;
    }
    sos_step = 0;

    kbe_write_frame(fd, smooth_r, smooth_g, smooth_b);
    kbe_sleep_ms(30);
    (*step)++;
}

static void fx_ram(int fd, int r, int g, int b, int *step)
{
    (void)r; (void)g; (void)b;
    static int used_percent = -1;
    static int tgt_r = 0, tgt_g = 220, tgt_b = 255;
    static int smooth_r = 0, smooth_g = 220, smooth_b = 255;

    if ((*step % 20) == 0 || used_percent < 0) {
        long used_mb = 0, total_mb = 0;
        get_mem_usage(&used_mb, &total_mb);
        if (total_mb > 0) {
            used_percent = (int)((used_mb * 100) / total_mb);
            if (used_percent < 0) used_percent = 0;
            if (used_percent > 100) used_percent = 100;
        } else if (used_percent < 0) {
            used_percent = 50;
        }

        if (used_percent <= 25) {
            tgt_r = 0; tgt_g = 220; tgt_b = 255;
        } else if (used_percent <= 50) {
            int ratio = ((used_percent - 25) * 255) / 25;
            tgt_r = 0;
            tgt_g = 220 + (35 * ratio) / 255;
            tgt_b = 255 - (195 * ratio) / 255;
        } else if (used_percent <= 70) {
            int ratio = ((used_percent - 50) * 255) / 20;
            tgt_r = (255 * ratio) / 255;
            tgt_g = 255 - (45 * ratio) / 255;
            tgt_b = 60 - (60 * ratio) / 255;
        } else if (used_percent <= 85) {
            int ratio = ((used_percent - 70) * 255) / 15;
            tgt_r = 255;
            tgt_g = 210 - (210 * ratio) / 255;
            tgt_b = 0;
        } else {
            tgt_r = 255; tgt_g = 0; tgt_b = 0;
        }
    }

    smooth_r = (smooth_r * 8 + tgt_r * 2) / 10;
    smooth_g = (smooth_g * 8 + tgt_g * 2) / 10;
    smooth_b = (smooth_b * 8 + tgt_b * 2) / 10;
    int out_r = smooth_r, out_g = smooth_g, out_b = smooth_b;
    if (used_percent >= 95) {
        int pulse_bri = (int)kbe_breathe_lut[*step % 128];
        out_r = (out_r * (128 + pulse_bri / 2)) / 255;
    }

    kbe_write_frame(fd, out_r, out_g, out_b);
    kbe_sleep_ms(30);
    (*step)++;
}

struct kbe_effect_def {
    const char *name;
    const char *const aliases[5];
    void (*step_fn)(int fd_col, int r, int g, int b, int *step);
    const char *desc;
};

static const struct kbe_effect_def kbe_effects[] = {
    { "breathe",       { "breath", NULL },                                                             fx_breathe,       "Smooth fade in/out (uses current color)" },
    { "breathe-cycle", { "breathe+colorchange", "breathe_cycle", "breathecycle", NULL },               fx_breathe_cycle, "Smooth breathe shifting through colors" },
    { "cycle",         { "rainbow", "spectrum", "slow-cycle", "slow_colorchanging", NULL },            fx_cycle,         "Smooth continuous rainbow cycle" },
    { "flash",         { "strobe", NULL },                                                             fx_flash,         "Strobe flash bursts (uses current color)" },
    { "flash-cycle",   { "flash+colorchange", "flash_cycle", "flashcycle", NULL },                     fx_flash_cycle,   "Strobe flash bursts cycling colors" },
    { "candle",        { "flicker", NULL },                                                            fx_candle,        "Realistic flickering candle flame" },
    { "pulse",         { "heartbeat", NULL },                                                          fx_pulse,         "Heartbeat double-pulse (uses current color)" },
    { "pulse-cycle",   { "pulse+colorchange", "pulse_cycle", "pulsecycle", "heartbeat-cycle", NULL }, fx_pulse_cycle,   "Heartbeat double-pulse cycling colors" },
    { "police",        { "siren", "cop", "emergency", NULL },                                          fx_police,        "Emergency red and blue alternating strobe" },
    { "gunfire",       { "gun-fire", "machinegun", NULL },                                              fx_gunfire,       "Rapid irregular warm muzzle flashes in short bursts" },
    { "fire",          { "flame", "embers", "burn", NULL },                                            fx_fire,          "Dynamic warm flickering campfire flames" },
    { "aurora",        { "arora", "northern-lights", "borealis", NULL },                               fx_aurora,        "Northern Lights emerald, cyan, and violet drift" },
    { "storm",         { "lightning", "thunder", NULL },                                               fx_storm,         "Dark moody sky with electric lightning strikes" },
    { "temp",          { "temperature", "thermal", "heatmap", NULL },                                  fx_temp,          "CPU: cyan <40°C, green→yellow→orange→red as it warms; red SOS at 90°C+" },
    { "ram",           { "memory", "mem", NULL },                                                       fx_ram,           "RAM: cyan ≤25%, gradual green→yellow→orange→red; pulses at 95%+" },
    { "sos",           { NULL },                                                                         fx_sos,           "Morse SOS signal in the current color" },
    { "firecrackers",  { "fireworks", NULL },                                                           fx_firecrackers, "Orange launch trails, white flashes, and colored ember salvos" },
};

static const struct kbe_effect_def *kbe_find_effect(const char *name)
{
    for (size_t i = 0; i < sizeof(kbe_effects) / sizeof(kbe_effects[0]); i++) {
        if (strcmp(kbe_effects[i].name, name) == 0)
            return &kbe_effects[i];
        for (int a = 0; kbe_effects[i].aliases[a]; a++) {
            if (strcmp(kbe_effects[i].aliases[a], name) == 0)
                return &kbe_effects[i];
        }
    }
    return NULL;
}

static void kbe_daemon_worker(const char *effect, int orig_r, int orig_g, int orig_b, int orig_bri)
{
    const struct kbe_effect_def *fx = kbe_find_effect(effect);
    if (!fx) exit(1);

    struct kbe_worker_ctx ctx;
    if (kbe_worker_begin(&ctx, fx->name, orig_r, orig_g, orig_b, orig_bri, NULL) < 0)
        exit(1);

    if (orig_bri <= 0) {
        kbe_write_bri(ctx.fd_bri, 255);
    }

    int base_r = orig_r;
    int base_g = orig_g;
    int base_b = orig_b;
    if (base_r == 0 && base_g == 0 && base_b == 0) {
        base_r = 0;
        base_g = 255;
        base_b = 255;
    }
    if (strcmp(fx->name, "candle") == 0 && orig_r == 255 && orig_g == 255 && orig_b == 255) {
        base_r = 255;
        base_g = 140;
        base_b = 20;
    }

    int step = 0;
    srand((unsigned int)(time(NULL) ^ getpid()));

    while (g_kbe_running) {
        fx->step_fn(ctx.fd_col, base_r, base_g, base_b, &step);
    }

    kbe_worker_end(&ctx, 1);
    exit(0);
}

static int kbe_start(const char *effect)
{
    if (access(KBD_PATH "/multi_intensity", F_OK) != 0) {
        fprintf(stderr, "Error: Keyboard backlight interface not found (is tuxedo_keyboard loaded?)\n");
        return 1;
    }

    struct kbe_state ks;
    int had_running = kbe_is_running(&ks);
    int orig_r = ks.orig_r, orig_g = ks.orig_g, orig_b = ks.orig_b, orig_bri = ks.orig_bri;

    if (had_running) {
        kbe_stop(1);
    } else {
        if (kbd_get_color(&orig_r, &orig_g, &orig_b) < 0) {
            orig_r = 255; orig_g = 255; orig_b = 255;
        }
        if (kbd_get_raw_brightness(&orig_bri) < 0) {
            orig_bri = 255;
        }
    }

    pid_t pid = daemonize();
    if (pid < 0) {
        fprintf(stderr, "Error: Failed to spawn background effect daemon\n");
        return 1;
    }

    if (pid > 0) {
        printf("Started keyboard effect '%s' [PID %d]\n", effect, (int)pid);
        return 0;
    }

    kbe_daemon_worker(effect, orig_r, orig_g, orig_b, orig_bri);
    exit(0);
}

static void kbe_profile_pulse_worker(int pr, int pg, int pb,
                                     const char *resume_effect,
                                     int orig_r, int orig_g, int orig_b, int orig_bri)
{
    struct kbe_worker_ctx ctx;
    if (kbe_worker_begin(&ctx, "pulse-profile", orig_r, orig_g, orig_b, orig_bri, resume_effect) < 0)
        exit(1);

    kbe_write_bri(ctx.fd_bri, 255);

    static const uint8_t oneshot_lut[] = {
        20, 60, 120, 180, 235, 255, 255, 230, 190, 140, 90, 50, 25, 10, 0
    };

    for (size_t i = 0; i < sizeof(oneshot_lut) && g_kbe_running; i++) {
        int val = oneshot_lut[i];
        kbe_write_frame(ctx.fd_col, (pr * val) / 255, (pg * val) / 255, (pb * val) / 255);
        kbe_sleep_ms(25);
    }

    if (!g_kbe_running || !resume_effect || !*resume_effect || strcmp(resume_effect, "none") == 0) {
        kbe_worker_end(&ctx, 1);
        exit(0);
    }

    kbe_worker_end(&ctx, 0);
    kbe_daemon_worker(resume_effect, orig_r, orig_g, orig_b, orig_bri);
    exit(0);
}

static void kbe_profile_pulse(const char *profile)
{
    if (access(KBD_PATH "/multi_intensity", F_OK) != 0)
        return;

    const struct profile_def *p = find_profile(profile);
    if (!p)
        return;

    int pr = p->pulse_r, pg = p->pulse_g, pb = p->pulse_b;
    if (pr == 0 && pg == 0 && pb == 0)
        return;

    struct kbe_state ks;
    int had_running = kbe_is_running(&ks);
    int orig_r = ks.orig_r, orig_g = ks.orig_g, orig_b = ks.orig_b, orig_bri = ks.orig_bri;
    char resume_effect[32] = {0};

    if (had_running) {
        if (strcmp(ks.effect, "pulse-profile") != 0) {
            strncpy(resume_effect, ks.effect, sizeof(resume_effect) - 1);
        } else if (ks.resume_effect[0]) {
            strncpy(resume_effect, ks.resume_effect, sizeof(resume_effect) - 1);
        }
        kill(ks.pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            usleep(10000);
            if (kill(ks.pid, 0) != 0 && errno == ESRCH) break;
        }
    } else {
        if (kbd_get_color(&orig_r, &orig_g, &orig_b) < 0) {
            orig_r = 255; orig_g = 255; orig_b = 255;
        }
        if (kbd_get_raw_brightness(&orig_bri) < 0) {
            orig_bri = 255;
        }
    }

    pid_t pid = daemonize();
    if (pid != 0) return;

    kbe_profile_pulse_worker(pr, pg, pb, resume_effect, orig_r, orig_g, orig_b, orig_bri);
    exit(0);
}
