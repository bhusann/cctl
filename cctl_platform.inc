/* ========================================================================
 * TUXEDO IOCTL CONSTANTS
 * ======================================================================== */

#define R_CL_WEBCAM_SW  0x8008ED13
#define W_CL_WEBCAM_SW  0x4008EE12

static int webcam_read_tuxedo(void)
{
    int val = 0;
    return (tuxedo_ioctl(R_CL_WEBCAM_SW, &val) < 0) ? -1 : val;
}

static int webcam_write_tuxedo(int enabled)
{
    int val = enabled ? 1 : 0;
    return (tuxedo_ioctl(W_CL_WEBCAM_SW, &val) < 0) ? -1 : 0;
}

/* Find USB device ID for the webcam (class 0x0E or 0xEF with camera keywords).
 * Returns the sysfs device name (e.g. "1-1.2") or NULL. Caller must free. */
static char *find_webcam_usb_id(void)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) return NULL;

    struct dirent *ent;
    char path[512];
    char *result = NULL;

    while ((ent = readdir(d)) != NULL) {
        /* Skip interfaces (contain ':') */
        if (strchr(ent->d_name, ':'))
            continue;

        /* Check bDeviceClass */
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/bDeviceClass",
                 ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char class_buf[16] = {0};
        ssize_t n = read(fd, class_buf, sizeof(class_buf) - 1);
        close(fd);
        if (n <= 0) continue;
        for (char *p = class_buf; *p; p++) if (*p == '\n' || *p == '\r') *p = '\0';

        int is_video = (strcmp(class_buf, "0e") == 0 || strcmp(class_buf, "14") == 0);
        int is_misc  = (strcmp(class_buf, "ef") == 0 || strcmp(class_buf, "239") == 0);
        if (!is_video && !is_misc) continue;

        /* Check product string for camera keywords */
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/product",
                 ent->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char prod_buf[256] = {0};
        n = read(fd, prod_buf, sizeof(prod_buf) - 1);
        close(fd);
        if (n > 0) {
            for (char *p = prod_buf; *p; p++) *p = (char)tolower((unsigned char)*p);
            if (strstr(prod_buf, "camera") || strstr(prod_buf, "webcam") ||
                strstr(prod_buf, "video") || strstr(prod_buf, "chicony")) {
                free(result);
                result = strdup(ent->d_name);
                break;
            }
        }

        /* If video class but no product match, still accept */
        if (is_video && !result) {
            result = strdup(ent->d_name);
        }
    }
    closedir(d);
    return result;
}

/* Enumerate a USB device's interface names (contain ':'), e.g. "1-8:1.0".
 * Stores up to max entries into ifnames. Returns count, or -1 if the
 * device directory is unreadable. */
static int webcam_list_interfaces(const char *dev_id, char ifnames[][256], int max)
{
    char dirpath[512];
    snprintf(dirpath, sizeof(dirpath), "/sys/bus/usb/devices/%s", dev_id);
    DIR *d = opendir(dirpath);
    if (!d) return -1;

    int cnt = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && cnt < max) {
        if (!strchr(ent->d_name, ':'))
            continue; /* regular attributes, not interfaces */
        snprintf(ifnames[cnt], sizeof(ifnames[cnt]), "%s", ent->d_name);
        cnt++;
    }
    closedir(d);
    return cnt;
}

/* 1 if this interface is currently claimed by driver `drv` (e.g. "uvcvideo"). */
static int iface_driver_is(const char *dev_id, const char *ifname, const char *drv)
{
    char link[540], target[512];
    snprintf(link, sizeof(link), "/sys/bus/usb/devices/%s/%s/driver",
             dev_id, ifname);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) return 0;
    target[n] = '\0';
    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;
    return strcmp(base, drv) == 0;
}

static int is_webcam_enabled(void)
{
    /* Try tuxedo_io first */
    int val = webcam_read_tuxedo();
    if (val >= 0)
        return val;

    /* Fallback: "on" = at least one camera interface claimed by uvcvideo.
     * The device-level driver link exists either way and says nothing about
     * whether the camera is actually usable. */
    char *usb_id = find_webcam_usb_id();
    if (!usb_id) return -1;

    char ifnames[8][256];
    int cnt = webcam_list_interfaces(usb_id, ifnames, 8);
    int enabled = 0;
    for (int i = 0; cnt > 0 && i < cnt; i++) {
        if (iface_driver_is(usb_id, ifnames[i], "uvcvideo")) {
            enabled = 1;
            break;
        }
    }
    free(usb_id);
    return (cnt < 0) ? -1 : enabled;
}

static int webcam_set(int enabled)
{
    /* Try tuxedo_io first */
    if (access("/dev/tuxedo_io", F_OK) == 0) {
        if (webcam_write_tuxedo(enabled) == 0) {
            printf("  Webcam: %s (tuxedo_io)%s\n", enabled ? "ON" : "OFF", snapshot_applied_tag());
            return 0;
        }
    }

    /* Fallback: unbind/bind the camera's INTERFACES on uvcvideo.
     * Writing the device name to drivers/usb/{bind,unbind} was a silent
     * no-op: that is the generic USB *device* driver — the interfaces stay
     * claimed by uvcvideo and the camera keeps streaming either way.
     * The disable switch is unbinding e.g. "1-8:1.0" from uvcvideo. */
    char *usb_id = find_webcam_usb_id();
    if (!usb_id) {
        fprintf(stderr, "Error: No webcam USB device found\n");
        return -1;
    }

    char ifnames[8][256];
    int cnt = webcam_list_interfaces(usb_id, ifnames, 8);
    if (cnt <= 0) {
        fprintf(stderr, "Error: Webcam USB device exposes no interfaces\n");
        free(usb_id);
        return -1;
    }

    int bound[8] = {0};
    int currently_enabled = 0;
    for (int i = 0; i < cnt; i++) {
        bound[i] = iface_driver_is(usb_id, ifnames[i], "uvcvideo");
        if (bound[i]) currently_enabled = 1;
    }
    free(usb_id);

    if (enabled == currently_enabled) {
        printf("  Webcam: already %s\n", enabled ? "ON" : "OFF");
        return 0;
    }

    char op_path[160];
    snprintf(op_path, sizeof(op_path), "/sys/bus/usb/drivers/uvcvideo/%s",
             enabled ? "bind" : "unbind");
    int fd = open(op_path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to open %s: %s\n", op_path, strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr, "       (uvcvideo kernel module not loaded?)\n");
        return -1;
    }

    int attempted = 0, done = 0, last_err = 0;
    for (int i = 0; i < cnt; i++) {
        if (enabled == bound[i]) continue; /* already in the wanted state */
        attempted++;
        if (write(fd, ifnames[i], strlen(ifnames[i])) < 0)
            last_err = errno;
        else
            done++;
    }
    close(fd);

    if (attempted > 0 && done == 0) {
        fprintf(stderr, "Error: Failed to %s webcam: %s\n",
                enabled ? "bind" : "unbind", strerror(last_err));
        return -1;
    }

    printf("  Webcam: %s (USB)%s\n", enabled ? "ON" : "OFF", snapshot_applied_tag());
    return 0;
}

static int webcam_toggle(void)
{
    int current = is_webcam_enabled();
    if (current < 0) {
        fprintf(stderr, "Error: Cannot detect webcam state\n");
        return -1;
    }
    return webcam_set(!current);
}

/* ========================================================================
 * MICROPHONE (amixer Capture switch)
 * ======================================================================== */

/* Find the ALSA HDA Intel PCH card number from /proc/asound/cards.
 * Returns the card number (>= 0) on success, or -1 if not found. */
static int mic_find_card(void)
{
    FILE *fp = fopen("/proc/asound/cards", "r");
    if (!fp) return -1;
    char line[256];
    int card = -1;
    while (fgets(line, sizeof(line), fp)) {
        int num;
        /* Lines look like: " 1 [PCH            ]: HDA-Intel - HDA Intel PCH" */
        if (sscanf(line, " %d", &num) == 1 && strstr(line, "HDA-Intel") && strstr(line, "PCH")) {
            card = num;
            break;
        }
    }
    fclose(fp);
    return card;
}

/* Returns 1 = capture on, 0 = off, -1 = unknown (amixer missing or failed).
 * Never guesses: reporting a disabled/unknown mic as ON in `cctl status`
 * would silently mislead the privacy use-case. */
static int mic_is_enabled(void)
{
    if (!command_exists("amixer")) return -1;

    int card = mic_find_card();
    char card_str[16] = {0};
    char buf[1024] = {0};
    char *const args_card[] = { "amixer", "-c", card_str, "sget", "Capture", NULL };
    char *const args_def[] = { "amixer", "sget", "Capture", NULL };
    if (card >= 0)
        snprintf(card_str, sizeof(card_str), "%d", card);

    if (run(card >= 0 ? args_card : args_def, RUN_CAPTURE, buf, sizeof(buf)) != 0)
        return -1;

    char *p = strstr(buf, "Front Left:");
    if (!p) p = buf;
    if (strstr(p, "[on]")) return 1;
    if (strstr(p, "[off]")) return 0;
    return -1;
}

static int mic_set(int enabled)
{
    if (!command_exists("amixer")) {
        fprintf(stderr, "Error: amixer not found (install alsa-utils)\n");
        return -1;
    }
    const char *verb = enabled ? "cap" : "nocap";
    int card = mic_find_card();
    int rc;

    /* Toggle master capture switch — this gates all mic inputs (internal +
     * headphone) regardless of which source the HDA codec mux has selected. */
    if (card >= 0) {
        char card_str[16];
        snprintf(card_str, sizeof(card_str), "%d", card);
        char *const args[] = { "amixer", "-c", card_str, "sset", "Capture", (char *)verb, NULL };
        rc = run(args, RUN_SILENT, NULL, 0);
    } else {
        char *const args[] = { "amixer", "sset", "Capture", (char *)verb, NULL };
        rc = run(args, RUN_SILENT, NULL, 0);
    }
    if (rc != 0) {
        fprintf(stderr, "Error: amixer failed (is alsa installed?)\n");
        return -1;
    }
    printf("  Laptop internal microphone: %s%s\n", enabled ? "ON" : "OFF", snapshot_applied_tag());
    return 0;
}

static int mic_toggle(void)
{
    int current = mic_is_enabled();
    if (current < 0) {
        fprintf(stderr, "Error: Cannot determine microphone state (amixer missing or failed)\n");
        fprintf(stderr, "Set it explicitly: cctl mic on  |  cctl mic off\n");
        return -1;
    }
    return mic_set(!current);
}

/* ========================================================================
 * REFRESH RATE (xrandr)
 * ======================================================================== */

/* Detect the first connected output name (e.g. "eDP-1"). Returns static buffer. */
struct display_info {
    char output[64];
    char resolution[64];
    char current_rate[16];
    char available_rates[32][16];
    int rate_count;
};

static int query_display_info(struct display_info *info)
{
    /* No fabricated fallbacks: every field must come from a real xrandr
     * parse. The old hardcoded defaults (eDP-1 / 2560x1440 / 60.00) made
     * `status`/`rr` show and act on invented values whenever parsing found
     * nothing — most visibly under XWayland. */
    memset(info, 0, sizeof(*info));

    char output[32768];
    char *const xrandr_args[] = { "xrandr", "--current", NULL };
    if (run(xrandr_args, RUN_CAPTURE, output, sizeof(output)) != 0)
        return -1;
    size_t output_len = strlen(output);
    if (output_len == 0) return -1;
    FILE *fp = fmemopen(output, output_len, "r");
    if (!fp) return -1;

    char line[512];
    int found_output = 0;
    while (fgets(line, sizeof(line), fp)) {
        // Strip trailing newlines
        for (char *p = line; *p; p++) if (*p == '\n' || *p == '\r') *p = '\0';

        // Check for output line: "<name> connected ..."
        if (!found_output && strstr(line, " connected")) {
            char name[64];
            if (sscanf(line, "%63s connected", name) == 1) {
                strcpy(info->output, name);
                found_output = 1;
            }
        }

        // If we found the output, look for resolution lines starting with spaces
        if (found_output && line[0] == ' ' && line[1] == ' ' && line[2] == ' ') {
            char res[64];
            // Read the resolution name
            char *p = line;
            while (*p == ' ') p++;
            if (sscanf(p, "%63s", res) != 1) continue;

            // Parse rates
            p += strlen(res);
            int rate_idx = 0;
            int is_active_res = 0;
            char rates_temp[32][16];

            while (*p && rate_idx < 32) {
                // skip spaces
                while (*p == ' ') p++;
                if (!*p) break;

                // read rate token (till space)
                char rate_tok[32];
                int len = 0;
                while (*p && *p != ' ' && len < 31) {
                    rate_tok[len++] = *p++;
                }
                rate_tok[len] = '\0';

                // Check if this rate is active (contains '*')
                int active = (strchr(rate_tok, '*') != NULL);

                // Clean the rate token (only keep digits and dots)
                char clean_rate[16];
                int c_len = 0;
                for (int i = 0; i < len && c_len < 15; i++) {
                    if (isdigit((unsigned char)rate_tok[i]) || rate_tok[i] == '.') {
                        clean_rate[c_len++] = rate_tok[i];
                    }
                }
                clean_rate[c_len] = '\0';

                if (c_len > 0) {
                    strcpy(rates_temp[rate_idx], clean_rate);
                    if (active) {
                        is_active_res = 1;
                        strcpy(info->current_rate, clean_rate);
                    }
                    rate_idx++;
                }
            }

            // If this is the active resolution, copy everything to display_info
            if (is_active_res) {
                strcpy(info->resolution, res);
                info->rate_count = rate_idx;
                for (int i = 0; i < rate_idx; i++) {
                    strcpy(info->available_rates[i], rates_temp[i]);
                }
                // Once we found the active resolution, we can stop
                break;
            }
        }
    }
    fclose(fp);
    /* Success requires BOTH the connected output AND its active mode —
     * a bare "connected" line with no parsed rate is treated as failure
     * so callers never display a half-invented screen state. */
    return (found_output && info->current_rate[0] != '\0') ? 0 : -1;
}

static int command_exists(const char *cmd)
{
    if (strchr(cmd, '/'))
        return access(cmd, X_OK) == 0;

    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin:/usr/local/bin";

    char pbuf[PATH_MAX];
    strncpy(pbuf, path, sizeof(pbuf) - 1);
    pbuf[sizeof(pbuf) - 1] = '\0';

    char *dir = strtok(pbuf, ":");
    while (dir) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, cmd);
        if (access(full, X_OK) == 0)
            return 1;
        dir = strtok(NULL, ":");
    }
    return 0;
}

/* Fast check: native X11 only — DISPLAY set AND xrandr present AND not a
 * Wayland session (WAYLAND_DISPLAY stays set when Xwayland provides a fake
 * DISPLAY, which is how the README's "X11 only" rule is actually honored). */
static int has_display_support(void)
{
    const char *disp = getenv("DISPLAY");
    if (!disp || !*disp) return 0;
    if (getenv("WAYLAND_DISPLAY")) return 0;
    return command_exists("xrandr");
}

/* Returns 1 if running in an X11 session with xrandr working and an active rate detected. */
static int is_x11_xrandr_available(struct display_info *out_info)
{
    if (!has_display_support()) return 0;

    struct display_info info;
    if (query_display_info(&info) < 0) return 0;
    if (info.current_rate[0] == '\0') return 0;

    if (out_info) *out_info = info;
    return 1;
}

static int rr_set(const char *rate)
{
    /* Validate rate: must be digits and at most one dot */
    for (const char *p = rate; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '.') {
            fprintf(stderr, "Error: Invalid refresh rate '%s'\n", rate);
            return -1;
        }
    }

    struct display_info info;
    if (query_display_info(&info) < 0) {
        fprintf(stderr, "Error: failed to query display info\n");
        return -1;
    }

    char *const args[] = {
        "xrandr", "--output", info.output, "--mode", info.resolution,
        "--rate", (char *)rate, NULL
    };
    if (run(args, RUN_SILENT, NULL, 0) != 0) {
        fprintf(stderr, "Error: Failed to set refresh rate to %sHz\n", rate);
        return -1;
    }
    printf("  Refresh rate: %sHz%s\n", rate, snapshot_applied_tag());
    return 0;
}

static int rr_list(void)
{
    struct display_info info;
    if (query_display_info(&info) < 0) {
        fprintf(stderr, "Error: xrandr not available or failed to parse\n");
        return -1;
    }
    printf("%sCurrent Display:%s\n", C_YLW, C_RST);
    printf("  %sOutput:%s       %s (%s)\n", C_CYN, C_RST, info.output, info.resolution);
    printf("  %sCurrent Rate:%s %sHz\n\n", C_CYN, C_RST, info.current_rate);
    printf("%sAvailable Rates:%s\n", C_YLW, C_RST);
    for (int i = 0; i < info.rate_count; i++) {
        printf("  %s%-8s%s%s\n", C_CYN, info.available_rates[i], C_RST,
               (strcmp(info.available_rates[i], info.current_rate) == 0) ? " (current)" : "");
    }
    printf("\n%sUsage:%s %scctl rr <rate | 1 | 2>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
    return 0;
}

/* Helper to check if a CPU core is an E-core based on its max frequency relative to global max.
 * Caches core classification in a static lookup table on first pass. */
static int is_cpu_e_core(int cpu_num)
{
    static int8_t e_core_cache[256];
    static int cache_initialized = 0;

    if (!cache_initialized) {
        memset(e_core_cache, -1, sizeof(e_core_cache));
        int global_max_khz = 0;
        int core_khz[256];
        memset(core_khz, 0, sizeof(core_khz));

        DIR *d = opendir("/sys/devices/system/cpu");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                int cnum;
                if (cpu_index_from_name(ent->d_name, &cnum) < 0) continue;
                if (cnum >= 256) continue;

                char path[512];
                if (cpu_sysfs_path(path, sizeof(path), cnum,
                                   "cpufreq/cpuinfo_max_freq") < 0) continue;
                long khz = read_sysfs_long(path, -1);
                if (khz > 0) {
                    core_khz[cnum] = (int)khz;
                    if ((int)khz > global_max_khz)
                        global_max_khz = (int)khz;
                }
            }
            closedir(d);
        }
        if (global_max_khz <= 0)
            global_max_khz = 4000000;

        for (int i = 0; i < 256; i++) {
            if (core_khz[i] > 0)
                e_core_cache[i] = (core_khz[i] < (long)(global_max_khz * 0.85)) ? 1 : 0;
            else
                e_core_cache[i] = 0;
        }
        cache_initialized = 1;
    }

    if (cpu_num >= 0 && cpu_num < 256)
        return e_core_cache[cpu_num];
    return 0;
}

/* Pin current process to up to 2 E-cores (keeps off P-cores on hybrid CPUs).
 * Only useful for long-running loops (monitor, kbe daemon); not every launch. */
static void pin_to_e_cores(void)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int pinned = 0;
    int max_cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (max_cpus <= 0) max_cpus = 64;

    for (int i = 0; i < max_cpus && pinned < 2; i++) {
        if (i >= CPU_SETSIZE) continue;
        if (is_cpu_e_core(i)) {
            CPU_SET((unsigned)i, &cpuset);
            pinned++;
        }
    }
    if (pinned > 0)
        sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

/* ========================================================================
 * STATUS
 * ======================================================================== */

static void row(const char *label, const char *fmt, ...)
{
    char val[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(val, sizeof(val), fmt, ap);
    va_end(ap);
    if (json_output) {
        char key[64];
        snprintf(key, sizeof(key), "%s", label);
        size_t len = strlen(key);
        if (len && key[len - 1] == ':') key[len - 1] = '\0';
        json_field(key, val);
        return;
    }
    printf("  %-14s %s\n", label, val);
}

static void json_string(const char *value)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
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

static void json_field(const char *key, const char *value)
{
    if (!json_output) return;
    if (!json_first_field) putchar(',');
    json_first_field = 0;
    json_string(key);
    putchar(':');
    json_string(value);
}

static void format_gpu_fan(char *buf, size_t sz, int pct, int rpm)
{
    if (pct == 0 && rpm == 0) {
        snprintf(buf, sz, "%s%3d%%%s duty, %s%4d RPM%s  %s(GPU in D3cold state)%s",
                 C_DIM, pct, C_RST, C_DIM, rpm, C_RST, C_DIM, C_RST);
    } else {
        snprintf(buf, sz, "%s%3d%%%s duty, %s%4d RPM%s",
                 C_CYN, pct, C_RST, C_CYN, rpm, C_RST);
    }
}

static void query_cpu_max_freqs(int *p_max_mhz, int *e_max_mhz)
{
    int p_max = 0, e_max = 0;
    int max_cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (max_cpus <= 0) max_cpus = 64;
    for (int i = 0; i < max_cpus; i++) {
        char path[128];
        if (cpu_sysfs_path(path, sizeof(path), i, "cpufreq/scaling_max_freq") < 0)
            continue;
        long khz = read_sysfs_long(path, -1);
        if (khz <= 0) {
            if (cpu_sysfs_path(path, sizeof(path), i, "cpufreq/cpuinfo_max_freq") < 0)
                continue;
            khz = read_sysfs_long(path, -1);
        }
        if (khz > 0) {
            int mhz = (int)(khz / 1000);
            if (!is_cpu_e_core(i)) {
                if (mhz > p_max) p_max = mhz;
            } else {
                if (mhz > e_max) e_max = mhz;
            }
        }
    }
    *p_max_mhz = p_max;
    *e_max_mhz = e_max;
}

static void status_system(void)
{
    char buf[128] = {0};

    /* Mode and GPU settings are read together from cctl's runtime record. */
    struct mode_runtime_data mode_state;
    int have_mode_state = mode_runtime_read(&mode_state) == 0;
    if (have_mode_state && mode_state.profile[0])
        row("Mode*:", "%s%s%s %s(%s; self-recorded, not live)%s",
            C_CYN, mode_state.profile, C_RST, C_DIM,
            mode_state.method[0] ? mode_state.method : "method unknown", C_RST);
    else
        row("Mode*:", "%sEC default%s %s(no cctl mode state)%s",
            C_DIM, C_RST, C_DIM, C_RST);

    if (have_mode_state && mode_state.ec_profile_code >= 0)
        row("EC Code*:", "%s%s%s (%d) %s(self-recorded, not live)%s",
            C_CYN, gpu_profile_name(mode_state.ec_profile_code), C_RST,
            mode_state.ec_profile_code, C_DIM, C_RST);
    else
        row("EC Code:", "%sstandard (3; default)%s", C_CYN, C_RST);

    char core_clock[64], mem_clock[64];
    if (have_mode_state && mode_state.core_clock_known) {
        if (mode_state.core_clock_set)
            snprintf(core_clock, sizeof(core_clock), "%d-%d MHz",
                     mode_state.core_clock_min, mode_state.core_clock_max);
        else
            snprintf(core_clock, sizeof(core_clock), "reset/unlocked");
    } else {
        snprintf(core_clock, sizeof(core_clock), "--");
    }
    if (have_mode_state && mode_state.mem_clock_known) {
        if (mode_state.mem_clock_set)
            snprintf(mem_clock, sizeof(mem_clock), "%d-%d MHz",
                     mode_state.mem_clock_min, mode_state.mem_clock_max);
        else
            snprintf(mem_clock, sizeof(mem_clock), "reset/unlocked");
    } else {
        snprintf(mem_clock, sizeof(mem_clock), "--");
    }
    /* 2. Turbo + Governor + EPP */
    int turbo_val = -1;
    if (read_sysfs_str(TURBO_PATH, buf, sizeof(buf)) >= 0)
        turbo_val = atoi(buf);

    struct cpu_profile_state cpu_profile;
    read_cpu_profile(&cpu_profile);

    row("CPU Profile:", "Turbo %s%s%s, gov: %s%s%s, epp: %s%s%s",
        turbo_val == 0 ? C_GRN : (turbo_val == 1 ? C_RED : C_DIM),
        turbo_val == 0 ? "ON" : (turbo_val == 1 ? "OFF" : "N/A"), C_RST,
        cpu_profile.have_governor ? C_CYN : C_DIM,
        cpu_profile.have_governor ? cpu_profile.governor : "N/A", C_RST,
        cpu_profile.have_epp ? C_CYN : C_DIM,
        cpu_profile.have_epp ? cpu_profile.epp : "N/A", C_RST);

    /* 3. RAPL PL1 and PL2 */
    long pl1_uw = read_sysfs_long("/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw", -1);
    long pl2_uw = read_sysfs_long("/sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw", -1);
    if (pl1_uw >= 0 && pl2_uw >= 0)
        row("RAPL Limits:", "PL1 %s%ldW%s, PL2 %s%ldW%s", C_CYN, pl1_uw / 1000000, C_RST, C_CYN, pl2_uw / 1000000, C_RST);
    else if (pl1_uw >= 0)
        row("RAPL Limits:", "PL1 %s%ldW%s, PL2 %sN/A%s", C_CYN, pl1_uw / 1000000, C_RST, C_DIM, C_RST);
    else if (pl2_uw >= 0)
        row("RAPL Limits:", "PL1 %sN/A%s, PL2 %s%ldW%s", C_DIM, C_RST, C_CYN, pl2_uw / 1000000, C_RST);
    else
        row("RAPL Limits:", "%sN/A%s", C_DIM, C_RST);

    /* 4. Current max CPU frequency for P-Core and E-Core */
    int p_max = 0, e_max = 0;
    query_cpu_max_freqs(&p_max, &e_max);
    if (p_max > 0 && e_max > 0)
        row("CPU Max Freq:", "P-Core %s%d MHz%s, E-Core %s%d MHz%s", C_CYN, p_max, C_RST, C_CYN, e_max, C_RST);
    else if (p_max > 0)
        row("CPU Max Freq:", "P-Core %s%d MHz%s", C_CYN, p_max, C_RST);
    else if (e_max > 0)
        row("CPU Max Freq:", "E-Core %s%d MHz%s", C_CYN, e_max, C_RST);
    else
        row("CPU Max Freq:", "%sN/A%s", C_DIM, C_RST);

    row("GPU Clocks*:", "Core %s, memory %s %s(self-recorded, not live)%s",
        core_clock, mem_clock, C_DIM, C_RST);

    /* 5. dGPU Power State */
    char pci_path[512];
    if (nvidia_find_pci_address(pci_path, sizeof(pci_path)) == 0) {
        char state_path[576];
        snprintf(state_path, sizeof(state_path), "%s/power_state", pci_path);
        char state[16] = "unknown";
        read_sysfs_str(state_path, state, sizeof(state));
        row("dGPU Power:", "%s%s%s", (strcmp(state, "D0") == 0) ? C_GRN : C_DIM, state, C_RST);
    } else {
        row("dGPU Power:", "%sN/A (no dGPU found)%s", C_DIM, C_RST);
    }

#ifdef CCTL_NVIDIA
    /* Nvidia GPU */
    int nv_blacklisted = nvidia_is_blacklisted();
    int nv_loaded = nvidia_is_loaded();
    row("Nvidia GPU:", "%s%s%s (modules %s%s%s)",
        nv_blacklisted ? C_RED : C_GRN, nv_blacklisted ? "BLACKLISTED" : "ENABLED", C_RST,
        nv_loaded ? C_GRN : C_DIM, nv_loaded ? "LOADED" : "NOT LOADED", C_RST);
#endif

    /* 6. GPU MUX */
    mux_show();

    /* 7. Refresh Rate (only shown if xrandr is present) */
    struct display_info disp_info;
    if (is_x11_xrandr_available(&disp_info))
        row("Refresh Rate:", "%s%s Hz%s", C_CYN, disp_info.current_rate, C_RST);

    if (have_mode_state && mode_state.display_scale_known) {
        if (mode_state.display_scale_set)
            row("Display Scale*:", "%s%s%s %s(self-recorded, not live)%s",
                C_CYN, mode_state.display_scale, C_RST, C_DIM, C_RST);
        else
            row("Display Scale*:", "%snative (reset)%s %s(self-recorded, not live)%s",
                C_CYN, C_RST, C_DIM, C_RST);
    } else {
        row("Display Scale*:", "%s-- (not recorded by cctl)%s", C_DIM, C_RST);
    }
}

static void status_fans(void)
{
    if (!json_output) printf("\n%s--- Fan Telemetry ---%s\n", C_YLW, C_RST);
    int cpu_pct = 0, gpu_pct = 0, cpu_rpm = 0, gpu_rpm = 0;
    if (read_fan_telemetry(&cpu_pct, &gpu_pct, &cpu_rpm, &gpu_rpm) == 0) {
        row("CPU Fan:", "%s%3d%%%s duty, %s%4d RPM%s", C_CYN, cpu_pct, C_RST, C_CYN, cpu_rpm, C_RST);
        char gfan[128];
        format_gpu_fan(gfan, sizeof(gfan), gpu_pct, gpu_rpm);
        row("GPU Fan:", "%s", gfan);
    } else {
        row("Fans:", "%sN/A (ec_sys or tuxedo_io not available)%s", C_DIM, C_RST);
    }
}

static void status_keyboard_color(const char *label, int r, int g, int b)
{
    char value[128];
    const char *preset = kbd_find_preset_name(r, g, b);
    if (preset)
        snprintf(value, sizeof(value), "%s RGB(%d, %d, %d)", preset, r, g, b);
    else
        snprintf(value, sizeof(value), "custom RGB(%d, %d, %d)", r, g, b);
    row(label, "%s%s%s", preset ? C_CYN : C_YLW, value, C_RST);
}

static void status_keyboard(void)
{
    if (!json_output) printf("\n%s--- Keyboard ---%s\n", C_YLW, C_RST);
    int cur_r = 0, cur_g = 0, cur_b = 0;
    int have_current_color = kbd_get_color(&cur_r, &cur_g, &cur_b) == 0;
    struct kbe_state ks;
    int effect_active = kbe_is_running(&ks);
    if (effect_active) {
        status_keyboard_color("Color (base):", ks.orig_r, ks.orig_g, ks.orig_b);
        if (have_current_color)
            status_keyboard_color("Current Color:", cur_r, cur_g, cur_b);
        else
            row("Current Color:", "%sN/A%s", C_DIM, C_RST);
    } else if (have_current_color) {
        status_keyboard_color("Color:", cur_r, cur_g, cur_b);
    } else {
        row("Color:", "%sN/A (tuxedo_keyboard not loaded)%s", C_DIM, C_RST);
    }

    int bri = kbd_get_brightness();
    if (bri >= 0)
        row("Brightness:", "%s%d%%%s", C_CYN, bri, C_RST);
    else
        row("Brightness:", "%sN/A%s", C_DIM, C_RST);

    if (effect_active)
        row("Effect:", "%s%s%s %s(PID %d)%s", C_CYN, ks.effect, C_RST, C_DIM, (int)ks.pid, C_RST);
    else
        row("Effect:", "%snone%s", C_DIM, C_RST);

    char buf[128];
    if (read_sysfs_str(FNLOCK_PATH, buf, sizeof(buf)) >= 0) {
        int val = atoi(buf);
        row("Fn Lock:", "%s%s%s", val ? C_GRN : C_RED, val ? "ON" : "OFF", C_RST);
    } else {
        row("Fn Lock:", "%sN/A (tuxedo_keyboard not loaded)%s", C_DIM, C_RST);
    }
}

static void status_privacy(void)
{
    if (!json_output) printf("\n%s--- Privacy ---%s\n", C_YLW, C_RST);
    int cam = is_webcam_enabled();
    if (cam < 0)
        row("Webcam:", "%sNot detected%s", C_DIM, C_RST);
    else
        row("Webcam:", "%s%s%s", cam ? C_GRN : C_RED, cam ? "ON" : "OFF", C_RST);

    int mic = mic_is_enabled();
    if (mic < 0)
        row("Microphone:", "%sN/A (amixer not available)%s", C_DIM, C_RST);
    else
        row("Microphone:", "%s%s%s", mic ? C_GRN : C_RED, mic ? "ON" : "OFF", C_RST);
}

static void status_battery(void)
{
    if (!json_output) printf("\n%s--- Battery Info ---%s\n", C_YLW, C_RST);
    char bat_status[32] = {0};
    bat_read_str("status", bat_status, sizeof(bat_status));
    long cap      = bat_read_long("capacity", -1);
    long full     = bat_read_long("charge_full", -1);
    long full_dsn = bat_read_long("charge_full_design", -1);
    long now      = bat_read_long("charge_now", -1);
    long current  = bat_read_long("current_now", LONG_MIN);
    long cycles   = bat_read_long("cycle_count", -1);
    long volt     = bat_read_long("voltage_now", -1);
    int bat_start = bat_read_start();
    int bat_end   = bat_read_end();

    if (cap < 0) {
        row("Battery:", "%sN/A (no battery)%s", C_DIM, C_RST);
    } else {
        if (bat_start > 0 && bat_end > 0)
            row("Battery:", "%ld%% %s  [threshold: %d%%→%d%%]", cap, bat_status, bat_start, bat_end);
        else
            row("Battery:", "%ld%% %s", cap, bat_status);
    }

    if (full > 0 && full_dsn > 0) {
        int health = (int)((full * 100L) / full_dsn);
        const char *hcol = health > 100 ? C_GRN : (health < 80 ? C_RED : C_YLW);
        row("Health:", "%s%d%%%s (%ld / %ld mAh)", hcol, health, C_RST, full / 1000, full_dsn / 1000);
    }
    if (cycles > 0)
        row("Cycles:", "%s%ld%s", C_CYN, cycles, C_RST);
    if (now > 0 && full > 0)
        row("Charge:", "%ld mAh / %ld mAh", now / 1000, full / 1000);
    if (current != LONG_MIN && current != 0) {
        long ma = current / 1000;
        row("Rate:", "%s%+ld mA%s", current > 0 ? C_RED : C_GRN, ma, C_RST);
    }
    if (volt > 0)
        row("Voltage:", "%ld mV", volt / 1000);
}

static void show_status(void)
{
    if (json_output) {
        json_first_field = 1;
        putchar('{');
    } else {
        printf("%s=== System Status ===%s\n\n", C_YLW, C_RST);
    }
    status_system();
    status_fans();
    status_keyboard();
    status_privacy();
    status_battery();
    if (json_output) puts("}");
    else {
        printf("\n%s* cctl records only changes made through cctl; these saved values may not reflect current hardware state.%s\n\n",
               C_DIM, C_RST);
    }
}

/* ========================================================================
 * BATTERY CHARGE THRESHOLDS
 * ======================================================================== */

/* Battery threshold path helpers — use auto-detected battery device. */
static void bat_start_path(char *out, size_t sz) { bat_sysfs(out, sz, "charge_control_start_threshold"); }
static void bat_end_path(char *out, size_t sz)   { bat_sysfs(out, sz, "charge_control_end_threshold"); }
static void bat_start_avail_path(char *out, size_t sz) { bat_sysfs(out, sz, "charge_control_start_available_thresholds"); }
static void bat_end_avail_path(char *out, size_t sz)   { bat_sysfs(out, sz, "charge_control_end_available_thresholds"); }

/* Read available threshold values into an array. Returns count or -1. */
static int read_avail_thresholds(const char *path, int *out, int max)
{
    char buf[128];
    if (read_sysfs_str(path, buf, sizeof(buf)) < 0)
        return -1;
    int count = 0;
    char *p = buf;
    while (*p && count < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        out[count++] = (int)v;
        p = end;
    }
    return count;
}

/* Check if a value is in the available threshold list. */
static int is_valid_threshold(const int *avail, int count, int val)
{
    for (int i = 0; i < count; i++)
        if (avail[i] == val) return 1;
    return 0;
}

/* Print available threshold values to stderr. */
static void print_avail_thresholds(const char *label, const int *avail, int count)
{
    fprintf(stderr, "  %s available: ", label);
    for (int i = 0; i < count; i++)
        fprintf(stderr, "%d%c", avail[i], i < count - 1 ? ' ' : '\n');
}

/* Read current charge control start threshold. Returns value or -1. */
static int bat_read_start(void)
{
    char p[256];
    bat_start_path(p, sizeof(p));
    long v = read_sysfs_long(p, -1);
    return (v >= 0) ? (int)v : -1;
}

/* Read current charge control end threshold. Returns value or -1. */
static int bat_read_end(void)
{
    char p[256];
    bat_end_path(p, sizeof(p));
    long v = read_sysfs_long(p, -1);
    return (v >= 0) ? (int)v : -1;
}

/* Set battery charge thresholds. "off" (the start==end==0 sentinel) selects
 * a near-full top-up range: charge all the way to the max stop threshold,
 * but only resume charging once the battery drops below the highest usable
 * start threshold (e.g. 95/100). Avoids keeping cells at mid-charge without
 * deep-discharge cycling. */
static int bat_set(int start, int end)
{
    char pa[256];
    /* "off" → stop=max, start=highest listed value below stop */
    if (start == 0 && end == 0) {
        int avail[16], cnt;
        bat_end_avail_path(pa, sizeof(pa));
        cnt = read_avail_thresholds(pa, avail, 16);
        end = (cnt > 0) ? avail[cnt - 1] : 100;
        bat_start_avail_path(pa, sizeof(pa));
        cnt = read_avail_thresholds(pa, avail, 16);
        start = 95;
        for (int i = cnt - 1; i >= 0; i--) {
            if (avail[i] < end) { start = avail[i]; break; }
        }
    }

    /* Validate start threshold */
    int start_avail[16], start_count;
    bat_start_avail_path(pa, sizeof(pa));
    start_count = read_avail_thresholds(pa, start_avail, 16);
    if (start_count > 0 && !is_valid_threshold(start_avail, start_count, start)) {
        fprintf(stderr, "Error: Start threshold %d%% is not valid\n", start);
        print_avail_thresholds("Start", start_avail, start_count);
        return -1;
    }

    /* Validate end threshold */
    int end_avail[16], end_count;
    bat_end_avail_path(pa, sizeof(pa));
    end_count = read_avail_thresholds(pa, end_avail, 16);
    if (end_count > 0 && !is_valid_threshold(end_avail, end_count, end)) {
        fprintf(stderr, "Error: End threshold %d%% is not valid\n", end);
        print_avail_thresholds("End", end_avail, end_count);
        return -1;
    }

    if (start >= end) {
        fprintf(stderr, "Error: Start threshold (%d%%) must be less than end threshold (%d%%)\n", start, end);
        return -1;
    }

    bat_start_path(pa, sizeof(pa));
    if (write_sysfs_int(pa, start) < 0) {
        fprintf(stderr, "Error: Failed to set start threshold to %d%%\n", start);
        return -1;
    }

    bat_end_path(pa, sizeof(pa));
    if (write_sysfs_int(pa, end) < 0) {
        fprintf(stderr, "Error: Failed to set end threshold to %d%%\n", end);
        return -1;
    }

    printf("  %s charge thresholds: start %d%% → stop %d%%%s\n",
           dry_run ? "Would set" : "Set", start, end, snapshot_applied_tag());
    return 0;
}
