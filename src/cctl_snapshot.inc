/* ========================================================================
 * NAMED SNAPSHOTS
 * ======================================================================== */

#ifndef CCTL_SNAPSHOT_DIR
#define CCTL_SNAPSHOT_DIR "/var/lib/cctl/snapshots"
#endif
#define CCTL_SNAPSHOT_NAME_MAX 64

struct cctl_snapshot_data {
    int ec_profile_code;
    int core_clock_known, core_clock_set, core_clock_min, core_clock_max;
    int mem_clock_known, mem_clock_set, mem_clock_min, mem_clock_max;
    int turbo;
    char governor[64];
    char epp[64];
    int pl1, pl2;
    int kbd_r, kbd_g, kbd_b, kbd_brightness;
    char kbd_effect[32];
    int fn_lock, webcam, microphone;
    int battery_start, battery_stop;
    char battery_mode[16];
    char display_rate[16];
    int display_scale_known, display_scale_set;
    char display_scale[32];
    int fancurve_state;
    struct fancurve_profile fancurve_profile;
    char fancurve_legacy_name[FANCURVE_NAME_MAX + 1];
};

enum snapshot_fancurve_state {
    SNAPSHOT_FANCURVE_SKIP = -1,
    SNAPSHOT_FANCURVE_NONE = 0,
    SNAPSHOT_FANCURVE_EMBEDDED = 1,
    SNAPSHOT_FANCURVE_LEGACY = 2
};

enum snapshot_field {
    SNAPSHOT_VERSION, SNAPSHOT_EC, SNAPSHOT_GPU_CLOCK, SNAPSHOT_MEM_CLOCK, SNAPSHOT_TURBO,
    SNAPSHOT_GOVERNOR, SNAPSHOT_EPP, SNAPSHOT_PL1, SNAPSHOT_PL2, SNAPSHOT_KBD_R, SNAPSHOT_KBD_G,
    SNAPSHOT_KBD_B, SNAPSHOT_KBD_BRIGHTNESS, SNAPSHOT_KBD_EFFECT, SNAPSHOT_FN_LOCK,
    SNAPSHOT_WEBCAM, SNAPSHOT_MICROPHONE, SNAPSHOT_BATTERY_START, SNAPSHOT_BATTERY_STOP,
    SNAPSHOT_DISPLAY_RATE, SNAPSHOT_DISPLAY_SCALE, SNAPSHOT_FANCURVE,
    SNAPSHOT_BATTERY_MODE,
    SNAPSHOT_FIELD_COUNT
};

enum snapshot_mask {
    MASK_EC_CODE       = 1ULL << 0,
    MASK_TURBO         = 1ULL << 1,
    MASK_GOV           = 1ULL << 2,
    MASK_EPP           = 1ULL << 3,
    MASK_PL1           = 1ULL << 4,
    MASK_PL2           = 1ULL << 5,
    MASK_NVIDIA_CLOCK  = 1ULL << 6,
    MASK_NVIDIA_MEMCLK = 1ULL << 7,
    MASK_KBE           = 1ULL << 8,
    MASK_KBC           = 1ULL << 9,
    MASK_KBB           = 1ULL << 10,
    MASK_FN_LOCK       = 1ULL << 11,
    MASK_WEBCAM        = 1ULL << 12,
    MASK_MIC           = 1ULL << 13,
    MASK_BAT           = 1ULL << 14,
    MASK_RR            = 1ULL << 15,
    MASK_SCALE         = 1ULL << 16,
    MASK_NOSAFE        = 1ULL << 17,
    MASK_FANCURVE      = 1ULL << 18
};

struct snapshot_mask_option {
    const char *name;
    unsigned long long bit;
};

static const struct snapshot_mask_option snapshot_mask_options[] = {
    { "nvidia_clock", MASK_NVIDIA_CLOCK }, { "nvidia_memclock", MASK_NVIDIA_MEMCLK },
    { "ec_code", MASK_EC_CODE }, { "turbo", MASK_TURBO }, { "gov", MASK_GOV },
    { "epp", MASK_EPP }, { "pl1", MASK_PL1 }, { "pl2", MASK_PL2 },
    { "kbe", MASK_KBE }, { "kbc", MASK_KBC }, { "kbb", MASK_KBB },
    { "fn_lock", MASK_FN_LOCK }, { "webcam", MASK_WEBCAM }, { "mic", MASK_MIC },
    { "bat", MASK_BAT }, { "rr", MASK_RR }, { "scale", MASK_SCALE },
    { "fancurve", MASK_FANCURVE },
    { "nosafe", MASK_NOSAFE }
};

static int cctl_snapshot_parse_mask(const char *arg, unsigned long long *mask)
{
    if (!arg || !*arg || strlen(arg) >= 256) return -1;
    char fields[256];
    snprintf(fields, sizeof(fields), "%s", arg);
    unsigned long long parsed = 0;
    char *cursor = fields;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ',');
        if (next) *next++ = '\0';
        while (isspace((unsigned char)*cursor)) cursor++;
        char *end = cursor + strlen(cursor);
        while (end > cursor && isspace((unsigned char)end[-1])) *--end = '\0';
        if (!*cursor) return -1;

        int found = 0;
        for (size_t i = 0; i < sizeof(snapshot_mask_options) / sizeof(snapshot_mask_options[0]); i++) {
            if (strcmp(cursor, snapshot_mask_options[i].name) == 0) {
                parsed |= snapshot_mask_options[i].bit;
                found = 1;
                break;
            }
        }
        if (!found || (next && !*next)) return -1;
        cursor = next;
    }
    *mask = parsed;
    return parsed ? 0 : -1;
}

static void cctl_snapshot_apply_mask(struct cctl_snapshot_data *st,
                                     unsigned long long mask)
{
    if (mask & MASK_EC_CODE) st->ec_profile_code = -1;
    if (mask & MASK_TURBO) st->turbo = -1;
    if (mask & MASK_GOV) snprintf(st->governor, sizeof(st->governor), "skip");
    if (mask & MASK_EPP) snprintf(st->epp, sizeof(st->epp), "skip");
    if (mask & MASK_PL1) st->pl1 = -1;
    if (mask & MASK_PL2) st->pl2 = -1;
    if (mask & MASK_NVIDIA_CLOCK) st->core_clock_known = -1;
    if (mask & MASK_NVIDIA_MEMCLK) st->mem_clock_known = -1;
    if (mask & MASK_KBE) snprintf(st->kbd_effect, sizeof(st->kbd_effect), "skip");
    if (mask & MASK_KBC) st->kbd_r = st->kbd_g = st->kbd_b = -1;
    if (mask & MASK_KBB) st->kbd_brightness = -1;
    if (mask & MASK_FN_LOCK) st->fn_lock = -1;
    if (mask & MASK_WEBCAM) st->webcam = -1;
    if (mask & MASK_MIC) st->microphone = -1;
    if (mask & MASK_BAT) {
        st->battery_start = st->battery_stop = -1;
        snprintf(st->battery_mode, sizeof(st->battery_mode), "skip");
    }
    if (mask & MASK_RR) snprintf(st->display_rate, sizeof(st->display_rate), "skip");
    if (mask & MASK_SCALE) {
        st->display_scale_known = -1;
        snprintf(st->display_scale, sizeof(st->display_scale), "skip");
    }
    if (mask & MASK_FANCURVE)
        st->fancurve_state = SNAPSHOT_FANCURVE_SKIP;
}

static void cctl_snapshot_init(struct cctl_snapshot_data *st)
{
    memset(st, 0, sizeof(*st));
    st->ec_profile_code = -1;
    st->core_clock_known = -1;
    st->mem_clock_known = -1;
    st->turbo = -1;
    snprintf(st->governor, sizeof(st->governor), "skip");
    snprintf(st->epp, sizeof(st->epp), "skip");
    st->pl1 = st->pl2 = -1;
    st->kbd_r = st->kbd_g = st->kbd_b = st->kbd_brightness = -1;
    snprintf(st->kbd_effect, sizeof(st->kbd_effect), "skip");
    st->fn_lock = st->webcam = st->microphone = -1;
    st->battery_start = st->battery_stop = -1;
    snprintf(st->battery_mode, sizeof(st->battery_mode), "skip");
    snprintf(st->display_rate, sizeof(st->display_rate), "skip");
    st->display_scale_known = -1;
    snprintf(st->display_scale, sizeof(st->display_scale), "skip");
    st->fancurve_state = SNAPSHOT_FANCURVE_SKIP;
}

static int cctl_snapshot_name_valid(const char *name)
{
    if (!name || !*name || strlen(name) > CCTL_SNAPSHOT_NAME_MAX) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return 0;
    }
    return 1;
}

static void cctl_snapshot_path(const char *name, char *path, size_t size)
{
    snprintf(path, size, CCTL_SNAPSHOT_DIR "/%s.conf", name);
}

static int cctl_snapshot_validate_path(const char *path, int *exists)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) { *exists = 0; return 0; }
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077) != 0) {
        errno = EPERM;
        return -1;
    }
    *exists = 1;
    return 0;
}

/* Returns 1 if found, 0 if absent, and -1 for an unsafe or inaccessible file. */
static int cctl_snapshot_find_existing(const char *name, char *path, size_t size)
{
    char candidate[128];
    int exists = 0;
    cctl_snapshot_path(name, candidate, sizeof(candidate));
    if (strlen(candidate) >= size) { errno = ENAMETOOLONG; return -1; }
    if (cctl_snapshot_validate_path(candidate, &exists) != 0) return -1;
    if (!exists) return 0;
    memcpy(path, candidate, strlen(candidate) + 1);
    return 1;
}

static int cctl_snapshot_ensure_dirs(void)
{
    const char *paths[] = { "/var/lib/cctl", CCTL_SNAPSHOT_DIR };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (mkdir(paths[i], i == 0 ? 0755 : 0700) != 0 && errno != EEXIST)
            return -1;
        struct stat st;
        if (lstat(paths[i], &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
            (i == 0 && (st.st_mode & 0022) != 0)) {
            errno = EPERM;
            return -1;
        }
        if (i == 1 && chmod(paths[i], 0700) != 0)
            return -1;
    }
    return 0;
}

static int cctl_snapshot_add_name(const char *name, char ***names,
                                 size_t *count, size_t *capacity)
{
    for (size_t i = 0; i < *count; i++)
        if (strcmp((*names)[i], name) == 0) return 0;
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2 : 16;
        if (next_capacity < *capacity || next_capacity > SIZE_MAX / sizeof(**names)) {
            errno = EOVERFLOW;
            return -1;
        }
        char **next = realloc(*names, next_capacity * sizeof(**names));
        if (!next) return -1;
        *names = next;
        *capacity = next_capacity;
    }
    size_t len = strlen(name);
    (*names)[*count] = malloc(len + 1);
    if (!(*names)[*count]) return -1;
    memcpy((*names)[*count], name, len + 1);
    (*count)++;
    return 0;
}

static int cctl_snapshot_collect_dir(const char *path, char ***names,
                                     size_t *count, size_t *capacity)
{
    struct stat dir_stat;
    if (lstat(path, &dir_stat) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(dir_stat.st_mode) || dir_stat.st_uid != 0 ||
        (dir_stat.st_mode & 0022) != 0) {
        errno = EPERM;
        return -1;
    }

    DIR *dir = opendir(path);
    if (!dir) return -1;
    int failed = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) failed = 1;
            break;
        }
        size_t len = strlen(entry->d_name);
        if (len < 6 || len > CCTL_SNAPSHOT_NAME_MAX + 5 ||
            strcmp(entry->d_name + len - 5, ".conf") != 0)
            continue;
        size_t name_len = len - 5;
        char name[CCTL_SNAPSHOT_NAME_MAX + 1];
        memcpy(name, entry->d_name, name_len);
        name[name_len] = '\0';
        if (!cctl_snapshot_name_valid(name)) continue;

        char file_path[256];
        int written = snprintf(file_path, sizeof(file_path), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(file_path)) continue;
        int exists;
        if (cctl_snapshot_validate_path(file_path, &exists) != 0 || !exists)
            continue;
        if (cctl_snapshot_add_name(name, names, count, capacity) != 0) {
            failed = 1;
            break;
        }
    }
    closedir(dir);
    return failed ? -1 : 0;
}

static int cctl_snapshot_compare_names(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static int cctl_snapshot_list(void)
{
    char **names = NULL;
    size_t count = 0, capacity = 0;
    if (cctl_snapshot_collect_dir(CCTL_SNAPSHOT_DIR, &names, &count, &capacity) != 0) {
        for (size_t i = 0; i < count; i++) free(names[i]);
        free(names);
        return fail("Cannot list saved snapshots");
    }
    if (count > 1)
        qsort(names, count, sizeof(*names), cctl_snapshot_compare_names);

    cctl_stream_printf("Saved snapshots: ");
    if (count == 0) cctl_stream_printf("none");
    for (size_t i = 0; i < count; i++)
        cctl_stream_printf("%s%s", i == 0 ? "" : ", ", names[i]);
    cctl_stream_putchar('\n');
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    return 0;
}

static void cctl_snapshot_curve_summary(const struct fancurve_profile *profile,
                                        char *out, size_t out_size)
{
    size_t used = 0;
    if (!out || out_size == 0) return;
    out[0] = '\0';
    for (int i = 0; profile && i < profile->npoints; i++) {
        const struct fancurve_point *point = &profile->points[i];
        char fan[16];
        if (point->fan == FANCURVE_FAN_AUTO)
            snprintf(fan, sizeof(fan), "auto");
        else
            snprintf(fan, sizeof(fan), "%d%%", point->fan);
        int wrote = snprintf(out + used, out_size - used, "%s%dC=%s",
                             i ? ", " : "", point->temp, fan);
        if (wrote < 0 || (size_t)wrote >= out_size - used) break;
        used += (size_t)wrote;
    }
}

static const char *cctl_snapshot_battery_mode_label(const char *mode)
{
    if (strcmp(mode, "standard") == 0) return "Standard";
    if (strcmp(mode, "custom") == 0) return "Custom";
    return "--";
}

static int cctl_snapshot_read_current(struct cctl_snapshot_data *st)
{
    cctl_snapshot_init(st);

    /* Read EC code live from the TCC offset; GPU locks use cctl runtime data. */
    st->ec_profile_code = cctl_live_ec_state(NULL, NULL);
    struct fancurve_state curve;
    if (fc_is_running(&curve)) {
        if (curve.profile_known) {
            st->fancurve_profile = curve.profile;
        } else {
            char err[256];
            if (fc_load_profile(curve.name, &st->fancurve_profile,
                                err, sizeof(err)) < 0) {
                cctl_stream_fprintf(stderr,
                    "Error: cannot include the active fan curve in the snapshot: %s\n", err);
                return -1;
            }
        }
        st->fancurve_state = SNAPSHOT_FANCURVE_EMBEDDED;
    } else {
        st->fancurve_state = SNAPSHOT_FANCURVE_NONE;
    }

    struct mode_runtime_data mode;
    if (mode_runtime_read(&mode) == 0) {
        if (mode.core_clock_known) {
            st->core_clock_known = 1;
            st->core_clock_set = mode.core_clock_set;
            st->core_clock_min = mode.core_clock_min;
            st->core_clock_max = mode.core_clock_max;
        }
        if (mode.mem_clock_known) {
            st->mem_clock_known = 1;
            st->mem_clock_set = mode.mem_clock_set;
            st->mem_clock_min = mode.mem_clock_min;
            st->mem_clock_max = mode.mem_clock_max;
        }
    }

    struct display_info display;
    if (has_display_support() && query_display_info(&display) == 0 && display.current_rate[0]) {
        snprintf(st->display_rate, sizeof(st->display_rate), "%s", display.current_rate);
        if (display.current_resolution[0] && display.resolution[0]) {
            st->display_scale_known = 1;
            st->display_scale_set = strcmp(display.current_resolution, display.resolution) != 0;
            snprintf(st->display_scale, sizeof(st->display_scale), "%.*s",
                     (int)sizeof(st->display_scale) - 1,
                     st->display_scale_set ? display.current_resolution : "none");
        }
    }

    long turbo = read_sysfs_long(TURBO_PATH, -1);
    if (turbo == 0 || turbo == 1) st->turbo = turbo == 0 ? 1 : 0;

    struct cpu_profile_state cpu_profile;
    read_cpu_profile(&cpu_profile);
    if (cpu_profile.have_governor &&
        strspn(cpu_profile.governor, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(cpu_profile.governor))
        snprintf(st->governor, sizeof(st->governor), "%.*s",
                 (int)sizeof(st->governor) - 1, cpu_profile.governor);
    if (cpu_profile.have_epp &&
        strspn(cpu_profile.epp, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(cpu_profile.epp))
        snprintf(st->epp, sizeof(st->epp), "%.*s",
                 (int)sizeof(st->epp) - 1, cpu_profile.epp);

    read_rapl_current(&st->pl1, &st->pl2);

    struct kbe_state ks;
    if (kbe_is_running(&ks)) {
        /* The live LED color is an animation frame; save the effect's base. */
        st->kbd_r = ks.orig_r;
        st->kbd_g = ks.orig_g;
        st->kbd_b = ks.orig_b;
        st->kbd_brightness = ks.orig_bri;
        if (kbe_find_effect(ks.effect))
            snprintf(st->kbd_effect, sizeof(st->kbd_effect), "%s", ks.effect);
        else
            snprintf(st->kbd_effect, sizeof(st->kbd_effect), "none");
    } else {
        if (kbd_get_color(&st->kbd_r, &st->kbd_g, &st->kbd_b) == 0)
            kbd_get_raw_brightness(&st->kbd_brightness);
        snprintf(st->kbd_effect, sizeof(st->kbd_effect), "none");
    }

    st->fn_lock = get_fnlock();
    st->webcam = is_webcam_enabled();
    st->microphone = mic_is_enabled();
    st->battery_start = bat_read_start();
    st->battery_stop = bat_read_end();
    char charge_mode[32] = {0};
    if (bat_read_charge_type(charge_mode, sizeof(charge_mode)) >= 0) {
        if (strcasecmp(charge_mode, "Standard") == 0)
            snprintf(st->battery_mode, sizeof(st->battery_mode), "standard");
        else if (strcasecmp(charge_mode, "Custom") == 0)
            snprintf(st->battery_mode, sizeof(st->battery_mode), "custom");
    }
    return 0;
}

static int cctl_snapshot_write_int(FILE *fp, const char *key, int value)
{
    if (value < 0) return cctl_stream_fprintf(fp, "%s=skip\n", key) < 0 ? -1 : 0;
    return cctl_stream_fprintf(fp, "%s=%d\n", key, value) < 0 ? -1 : 0;
}

static int cctl_snapshot_write_clock(FILE *fp, const char *key, int known, int is_set,
                                  int min, int max)
{
    if (known < 0) return cctl_stream_fprintf(fp, "%s=skip\n", key) < 0 ? -1 : 0;
    if (!is_set) return cctl_stream_fprintf(fp, "%s=none\n", key) < 0 ? -1 : 0;
    return cctl_stream_fprintf(fp, "%s=%d,%d\n", key, min, max) < 0 ? -1 : 0;
}

static int cctl_snapshot_write_fancurve(FILE *fp,
                                        const struct cctl_snapshot_data *st)
{
    if (st->fancurve_state == SNAPSHOT_FANCURVE_SKIP)
        return cctl_stream_fprintf(fp, "fancurve=skip\n") < 0 ? -1 : 0;
    if (st->fancurve_state == SNAPSHOT_FANCURVE_NONE)
        return cctl_stream_fprintf(fp, "fancurve=none\n") < 0 ? -1 : 0;
    if (st->fancurve_state != SNAPSHOT_FANCURVE_EMBEDDED)
        return -1;

    const struct fancurve_profile *profile = &st->fancurve_profile;
    if (cctl_stream_fprintf(fp, "fancurve=curve:%d:%d:",
                            profile->hysteresis, profile->interval_ms) < 0)
        return -1;
    for (int i = 0; i < profile->npoints; i++) {
        const struct fancurve_point *point = &profile->points[i];
        if (cctl_stream_fprintf(fp, "%s%d,%s", i ? ";" : "", point->temp,
                point->fan == FANCURVE_FAN_AUTO ? "a" : "") < 0)
            return -1;
        if (point->fan != FANCURVE_FAN_AUTO &&
            cctl_stream_fprintf(fp, "%d", point->fan) < 0)
            return -1;
    }
    return cctl_stream_fprintf(fp, "\n") < 0 ? -1 : 0;
}

static int cctl_snapshot_write_file(const char *name, const struct cctl_snapshot_data *st)
{
    if (cctl_snapshot_ensure_dirs() != 0) return -1;

    char final_path[160], temp_path[192];
    cctl_snapshot_path(name, final_path, sizeof(final_path));
    snprintf(temp_path, sizeof(temp_path), "%s/.%s.tmp.XXXXXX", CCTL_SNAPSHOT_DIR, name);
    int fd = mkstemp(temp_path);
    if (fd < 0) return -1;
    if (fchmod(fd, 0600) != 0) {
        int e = errno; close(fd); unlink(temp_path); errno = e; return -1;
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        int e = errno; close(fd); unlink(temp_path); errno = e; return -1;
    }

    int failed = cctl_stream_fprintf(fp, "# cctl saved snapshot format 5\nversion=5\n") < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "ec_profile_code", st->ec_profile_code) < 0;
    if (!failed) failed = cctl_snapshot_write_clock(fp, "nvidia_clock", st->core_clock_known,
                                                  st->core_clock_set, st->core_clock_min,
                                                  st->core_clock_max) < 0;
    if (!failed) failed = cctl_snapshot_write_clock(fp, "nvidia_memclock", st->mem_clock_known,
                                                  st->mem_clock_set, st->mem_clock_min,
                                                  st->mem_clock_max) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "turbo", st->turbo) < 0;
    if (!failed) failed = cctl_stream_fprintf(fp, "governor=%s\nepp=%s\n", st->governor, st->epp) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "pl1_watts", st->pl1) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "pl2_watts", st->pl2) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "keyboard_r", st->kbd_r) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "keyboard_g", st->kbd_g) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "keyboard_b", st->kbd_b) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "keyboard_brightness_raw", st->kbd_brightness) < 0;
    if (!failed) failed = cctl_stream_fprintf(fp, "keyboard_effect=%s\n", st->kbd_effect) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "fn_lock", st->fn_lock) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "webcam", st->webcam) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "microphone", st->microphone) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "battery_start", st->battery_start) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "battery_stop", st->battery_stop) < 0;
    if (!failed) failed = cctl_stream_fprintf(fp, "battery_mode=%s\n", st->battery_mode) < 0;
    if (!failed) failed = cctl_stream_fprintf(fp, "display_rate=%s\n", st->display_rate) < 0;
    if (!failed) {
        const char *scale = st->display_scale_known < 0 ? "skip" :
                            st->display_scale_set ? st->display_scale : "none";
        failed = cctl_stream_fprintf(fp, "display_scale=%s\n", scale) < 0;
    }
    if (!failed) failed = cctl_snapshot_write_fancurve(fp, st) < 0;

    if (!failed && fflush(fp) != 0) failed = 1;
    if (!failed && fsync(fd) != 0) failed = 1;
    int saved_errno = errno;
    if (fclose(fp) != 0) { failed = 1; saved_errno = errno; }
    if (failed) {
        unlink(temp_path);
        errno = saved_errno ? saved_errno : EIO;
        return -1;
    }
    if (rename(temp_path, final_path) != 0) {
        int e = errno; unlink(temp_path); errno = e; return -1;
    }
    return 0;
}

static int cctl_snapshot_parse_int(const char *value, int min, int max, int *out)
{
    if (strcmp(value, "skip") == 0) { *out = -1; return 0; }
    int n;
    if (safe_atoi(value, &n) != 0 || n < min || n > max) return -1;
    *out = n;
    return 0;
}

static int cctl_snapshot_parse_clock(const char *value, int *known, int *is_set,
                                  int *min, int *max)
{
    if (strcmp(value, "skip") == 0) { *known = -1; *is_set = 0; return 0; }
    if (strcmp(value, "none") == 0) { *known = 1; *is_set = 0; return 0; }
    int lo, hi;
    if (sscanf(value, "%d,%d", &lo, &hi) != 2 || lo < 0 || hi <= 0 || lo > hi)
        return -1;
    *known = *is_set = 1;
    *min = lo;
    *max = hi;
    return 0;
}

static int cctl_snapshot_copy_token(char *dst, size_t size, const char *value)
{
    size_t len = strlen(value);
    if (len == 0 || len >= size ||
        strspn(value, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != len)
        return -1;
    snprintf(dst, size, "%s", value);
    return 0;
}

static int cctl_snapshot_parse_curve_int(char **cursor, int *value, char delimiter)
{
    errno = 0;
    char *end = NULL;
    long parsed = strtol(*cursor, &end, 10);
    if (errno || end == *cursor || parsed < INT_MIN || parsed > INT_MAX ||
        !end || *end != delimiter)
        return -1;
    *value = (int)parsed;
    *cursor = end + 1;
    return 0;
}

static int cctl_snapshot_parse_fancurve_config(const char *value,
                                                struct fancurve_profile *profile)
{
    if (strncmp(value, "curve:", 6) != 0) return -1;
    memset(profile, 0, sizeof(*profile));
    snprintf(profile->name, sizeof(profile->name), "snapshot");
    char *cursor = (char *)value + 6;
    if (cctl_snapshot_parse_curve_int(&cursor, &profile->hysteresis, ':') < 0 ||
        cctl_snapshot_parse_curve_int(&cursor, &profile->interval_ms, ':') < 0)
        return -1;

    while (*cursor) {
        if (profile->npoints >= FANCURVE_MAX_POINTS) return -1;
        struct fancurve_point *point = &profile->points[profile->npoints];
        if (cctl_snapshot_parse_curve_int(&cursor, &point->temp, ',') < 0)
            return -1;
        if (*cursor == 'a') {
            point->fan = FANCURVE_FAN_AUTO;
            cursor++;
        } else {
            errno = 0;
            char *end = NULL;
            long fan = strtol(cursor, &end, 10);
            if (errno || end == cursor || fan < INT_MIN || fan > INT_MAX)
                return -1;
            point->fan = (int)fan;
            cursor = end;
        }
        profile->npoints++;
        if (*cursor == '\0') break;
        if (*cursor++ != ';' || *cursor == '\0') return -1;
    }

    char err[128];
    return fancurve_validate(profile, NULL, err, sizeof(err));
}

static int cctl_snapshot_parse_field(struct cctl_snapshot_data *st, const char *key,
                                  const char *value, unsigned long long *seen)
{
    int field = -1, rc = -1;
#define SNAPSHOT_FIELD(name, index, expression) \
    if (strcmp(key, name) == 0) { field = index; rc = (expression); }
    SNAPSHOT_FIELD("version", SNAPSHOT_VERSION,
                (strcmp(value, "2") == 0 || strcmp(value, "3") == 0 ||
                 strcmp(value, "4") == 0 || strcmp(value, "5") == 0) ? 0 : -1)
    else SNAPSHOT_FIELD("ec_profile_code", SNAPSHOT_EC, cctl_snapshot_parse_int(value, 0, 3, &st->ec_profile_code))
    else SNAPSHOT_FIELD("nvidia_clock", SNAPSHOT_GPU_CLOCK, cctl_snapshot_parse_clock(value, &st->core_clock_known, &st->core_clock_set, &st->core_clock_min, &st->core_clock_max))
    else SNAPSHOT_FIELD("nvidia_memclock", SNAPSHOT_MEM_CLOCK, cctl_snapshot_parse_clock(value, &st->mem_clock_known, &st->mem_clock_set, &st->mem_clock_min, &st->mem_clock_max))
    else SNAPSHOT_FIELD("turbo", SNAPSHOT_TURBO, cctl_snapshot_parse_int(value, 0, 1, &st->turbo))
    else if (strcmp(key, "governor") == 0) {
        field = SNAPSHOT_GOVERNOR;
        rc = cctl_snapshot_copy_token(st->governor, sizeof(st->governor), value);
    }
    else if (strcmp(key, "epp") == 0) {
        field = SNAPSHOT_EPP;
        rc = cctl_snapshot_copy_token(st->epp, sizeof(st->epp), value);
    }
    else SNAPSHOT_FIELD("pl1_watts", SNAPSHOT_PL1, cctl_snapshot_parse_int(value, 1, 90, &st->pl1))
    else SNAPSHOT_FIELD("pl2_watts", SNAPSHOT_PL2, cctl_snapshot_parse_int(value, 1, 115, &st->pl2))
    else SNAPSHOT_FIELD("keyboard_r", SNAPSHOT_KBD_R, cctl_snapshot_parse_int(value, 0, 255, &st->kbd_r))
    else SNAPSHOT_FIELD("keyboard_g", SNAPSHOT_KBD_G, cctl_snapshot_parse_int(value, 0, 255, &st->kbd_g))
    else SNAPSHOT_FIELD("keyboard_b", SNAPSHOT_KBD_B, cctl_snapshot_parse_int(value, 0, 255, &st->kbd_b))
    else SNAPSHOT_FIELD("keyboard_brightness_raw", SNAPSHOT_KBD_BRIGHTNESS, cctl_snapshot_parse_int(value, 0, 255, &st->kbd_brightness))
    else if (strcmp(key, "keyboard_effect") == 0) {
        field = SNAPSHOT_KBD_EFFECT;
        if (strcmp(value, "skip") == 0 || strcmp(value, "none") == 0) {
            snprintf(st->kbd_effect, sizeof(st->kbd_effect), "%s", value);
            rc = 0;
        } else {
            rc = cctl_snapshot_copy_token(st->kbd_effect, sizeof(st->kbd_effect), value);
            if (rc == 0 && !kbe_find_effect(st->kbd_effect)) rc = -1;
        }
    }
    else SNAPSHOT_FIELD("fn_lock", SNAPSHOT_FN_LOCK, cctl_snapshot_parse_int(value, 0, 1, &st->fn_lock))
    else SNAPSHOT_FIELD("webcam", SNAPSHOT_WEBCAM, cctl_snapshot_parse_int(value, 0, 1, &st->webcam))
    else SNAPSHOT_FIELD("microphone", SNAPSHOT_MICROPHONE, cctl_snapshot_parse_int(value, 0, 1, &st->microphone))
    else SNAPSHOT_FIELD("battery_start", SNAPSHOT_BATTERY_START, cctl_snapshot_parse_int(value, 1, 100, &st->battery_start))
    else SNAPSHOT_FIELD("battery_stop", SNAPSHOT_BATTERY_STOP, cctl_snapshot_parse_int(value, 1, 100, &st->battery_stop))
    else if (strcmp(key, "battery_mode") == 0) {
        field = SNAPSHOT_BATTERY_MODE;
        if (strcmp(value, "skip") == 0 || strcmp(value, "standard") == 0 ||
            strcmp(value, "custom") == 0) {
            snprintf(st->battery_mode, sizeof(st->battery_mode), "%s", value);
            rc = 0;
        }
    }
    else if (strcmp(key, "display_rate") == 0) {
        field = SNAPSHOT_DISPLAY_RATE;
        size_t len = strlen(value);
        rc = (strcmp(value, "skip") == 0 ||
              (len > 0 && len < sizeof(st->display_rate) &&
               strspn(value, "0123456789.") == len)) ? 0 : -1;
        if (rc == 0)
            snprintf(st->display_rate, sizeof(st->display_rate), "%s", value);
    }
    else if (strcmp(key, "display_scale") == 0) {
        field = SNAPSHOT_DISPLAY_SCALE;
        if (strcmp(value, "skip") == 0) {
            st->display_scale_known = -1;
            rc = 0;
        } else if (strcmp(value, "none") == 0) {
            st->display_scale_known = 1;
            st->display_scale_set = 0;
            snprintf(st->display_scale, sizeof(st->display_scale), "none");
            rc = 0;
        } else {
            int width, height;
            char extra;
            rc = sscanf(value, "%dx%d%c", &width, &height, &extra) == 2 &&
                 width >= 640 && height >= 360 &&
                 strlen(value) < sizeof(st->display_scale) ? 0 : -1;
            if (rc == 0) {
                st->display_scale_known = st->display_scale_set = 1;
                snprintf(st->display_scale, sizeof(st->display_scale), "%s", value);
            }
        }
    }
    else if (strcmp(key, "fancurve") == 0) {
        field = SNAPSHOT_FANCURVE;
        if (strcmp(value, "skip") == 0) {
            st->fancurve_state = SNAPSHOT_FANCURVE_SKIP;
            rc = 0;
        } else if (strcmp(value, "none") == 0) {
            st->fancurve_state = SNAPSHOT_FANCURVE_NONE;
            rc = 0;
        } else if (strncmp(value, "profile:", 8) == 0 &&
                   fancurve_name_valid(value + 8)) {
            st->fancurve_state = SNAPSHOT_FANCURVE_LEGACY;
            snprintf(st->fancurve_legacy_name, sizeof(st->fancurve_legacy_name),
                     "%s", value + 8);
            rc = 0;
        } else if (cctl_snapshot_parse_fancurve_config(
                       value, &st->fancurve_profile) == 0) {
            st->fancurve_state = SNAPSHOT_FANCURVE_EMBEDDED;
            rc = 0;
        }
    }
#undef SNAPSHOT_FIELD
    if (field < 0 || rc != 0 || (*seen & (1ULL << field))) return -1;
    *seen |= 1ULL << field;
    return 0;
}

static int cctl_snapshot_read_file(const char *name, struct cctl_snapshot_data *st)
{
    char path[160], line[256];
    cctl_snapshot_path(name, path, sizeof(path));
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat stbuf;
    if (fstat(fd, &stbuf) != 0 || !S_ISREG(stbuf.st_mode) || stbuf.st_uid != 0 ||
        (stbuf.st_mode & 0077) != 0) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return -1; }

    cctl_snapshot_init(st);
    unsigned long long seen = 0;
    int failed = 0, file_version = 0;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) { failed = 1; break; }
        *eq++ = '\0';
        if (strcmp(line, "version") == 0)
            file_version = (strcmp(eq, "2") == 0 || strcmp(eq, "3") == 0 ||
                            strcmp(eq, "4") == 0 || strcmp(eq, "5") == 0)
                         ? atoi(eq) : 0;
        if (cctl_snapshot_parse_field(st, line, eq, &seen) != 0) { failed = 1; break; }
    }
    unsigned long long required = (1ULL << SNAPSHOT_FIELD_COUNT) - 1;
    if (file_version < 3)
        required &= ~(1ULL << SNAPSHOT_FANCURVE);
    if (file_version < 5)
        required &= ~(1ULL << SNAPSHOT_BATTERY_MODE);
    if (ferror(fp) || file_version == 0 || seen != required) failed = 1;
    fclose(fp);
    if (!failed && st->fancurve_state == SNAPSHOT_FANCURVE_LEGACY) {
        char err[256];
        if (fc_load_profile(st->fancurve_legacy_name, &st->fancurve_profile,
                            err, sizeof(err)) == 0)
            st->fancurve_state = SNAPSHOT_FANCURVE_EMBEDDED;
    }
    return failed ? -1 : 0;
}

static void cctl_snapshot_preview_restore(const char *name,
                                          const struct cctl_snapshot_data *st,
                                          int nosafe)
{
    char value[512];
    cctl_stream_printf("Dry run: snapshot '%s' would restore these settings; nothing was changed.\n",
           name);
#define SNAPSHOT_PREVIEW(label, expression) do { \
    snprintf(value, sizeof(value), "%s", (expression)); \
    cctl_stream_printf("  %-22s %s\n", label, value); \
} while (0)

    if (st->ec_profile_code >= 0) {
        snprintf(value, sizeof(value), "%s (%d)", gpu_profile_name(st->ec_profile_code),
                 st->ec_profile_code);
        cctl_stream_printf("  %-22s %s\n", "EC profile code", value);
    } else SNAPSHOT_PREVIEW("EC profile code", "skip (not saved)");
    if (st->turbo >= 0) SNAPSHOT_PREVIEW("Turbo", st->turbo ? "on" : "off");
    else SNAPSHOT_PREVIEW("Turbo", "skip (not saved)");
    SNAPSHOT_PREVIEW("Governor", strcmp(st->governor, "skip") == 0 ? "skip (not saved)" : st->governor);
    SNAPSHOT_PREVIEW("EPP", strcmp(st->epp, "skip") == 0 ? "skip (not saved)" : st->epp);
    int preview_cur_pl1 = -1, preview_cur_pl2 = -1;
    read_rapl_current(&preview_cur_pl1, &preview_cur_pl2);
    int preview_pl1_cap = st->ec_profile_code == 2
                        ? RAPL_PL1_MAX_WATTS_MAX : RAPL_PL1_MAX_WATTS;

    if (st->pl1 >= 0) {
        if (!rapl_pl1_within_cap(st->pl1, preview_pl1_cap))
            snprintf(value, sizeof(value), "%dW (skipped: requires EC profile code 2)", st->pl1);
        else
            snprintf(value, sizeof(value), "%dW", st->pl1);
        cctl_stream_printf("  %-22s %s\n", "PL1", value);
    } else SNAPSHOT_PREVIEW("PL1", "skip (not saved)");
    if (st->pl2 >= 0) {
        int eff_p1 = (st->pl1 >= 0 && !(st->pl1 > RAPL_PL1_MAX_WATTS && st->ec_profile_code != 2))
                     ? st->pl1 : preview_cur_pl1;
        if (!rapl_pl2_within_cap(st->pl2))
            snprintf(value, sizeof(value), "%dW (skipped: maximum is %dW)",
                     st->pl2, RAPL_PL2_MAX_WATTS);
        else if (!rapl_pl2_above_pl1(eff_p1, st->pl2))
            snprintf(value, sizeof(value), "%dW (skipped: not > %s PL1 %dW)", st->pl2,
                     st->pl1 < 0 ? "current" : "effective", eff_p1);
        else
            snprintf(value, sizeof(value), "%dW", st->pl2);
        cctl_stream_printf("  %-22s %s\n", "PL2", value);
    } else SNAPSHOT_PREVIEW("PL2", "skip (not saved)");

    if (st->core_clock_known < 0) SNAPSHOT_PREVIEW("NVIDIA core clock", "skip (not saved)");
    else if (st->core_clock_set) {
        snprintf(value, sizeof(value), "%d-%d MHz", st->core_clock_min, st->core_clock_max);
        cctl_stream_printf("  %-22s %s\n", "NVIDIA core clock", value);
    } else SNAPSHOT_PREVIEW("NVIDIA core clock", "reset/unlocked");
    if (st->mem_clock_known < 0) SNAPSHOT_PREVIEW("NVIDIA memory clock", "skip (not saved)");
    else if (st->mem_clock_set) {
        snprintf(value, sizeof(value), "%d-%d MHz", st->mem_clock_min, st->mem_clock_max);
        cctl_stream_printf("  %-22s %s\n", "NVIDIA memory clock", value);
    } else SNAPSHOT_PREVIEW("NVIDIA memory clock", "reset/unlocked");

    if (st->kbd_r >= 0 && st->kbd_g >= 0 && st->kbd_b >= 0) {
        snprintf(value, sizeof(value), "RGB(%d, %d, %d)", st->kbd_r, st->kbd_g, st->kbd_b);
        cctl_stream_printf("  %-22s %s\n", "Keyboard color", value);
    } else SNAPSHOT_PREVIEW("Keyboard color", "skip (not saved)");
    if (st->kbd_brightness >= 0) {
        snprintf(value, sizeof(value), "%d%%", (st->kbd_brightness * 100 + 127) / 255);
        cctl_stream_printf("  %-22s %s\n", "Keyboard brightness", value);
    } else SNAPSHOT_PREVIEW("Keyboard brightness", "skip (not saved)");
    SNAPSHOT_PREVIEW("Keyboard effect", strcmp(st->kbd_effect, "skip") == 0 ? "skip (not saved)" : st->kbd_effect);
    if (st->fn_lock >= 0) SNAPSHOT_PREVIEW("Fn Lock", st->fn_lock ? "on" : "off");
    else SNAPSHOT_PREVIEW("Fn Lock", "skip (not saved)");
    if (st->webcam >= 0) SNAPSHOT_PREVIEW("Webcam", st->webcam ? "on" : "off");
    else SNAPSHOT_PREVIEW("Webcam", "skip (not saved)");
    if (st->microphone >= 0) SNAPSHOT_PREVIEW("Microphone", st->microphone ? "on" : "off");
    else SNAPSHOT_PREVIEW("Microphone", "skip (not saved)");

    SNAPSHOT_PREVIEW("Battery charge mode",
        strcmp(st->battery_mode, "skip") == 0 ? "skip (not saved)" :
        cctl_snapshot_battery_mode_label(st->battery_mode));
    if (st->battery_start >= 0 || st->battery_stop >= 0) {
        char start[24], stop[24];
        int current_start = st->battery_start >= 0 ? st->battery_start : bat_read_start();
        int current_stop = st->battery_stop >= 0 ? st->battery_stop : bat_read_end();
        if (current_start > 0)
            snprintf(start, sizeof(start), "%d%%", current_start);
        else
            snprintf(start, sizeof(start), "unavailable");
        if (current_stop > 0)
            snprintf(stop, sizeof(stop), "%d%%", current_stop);
        else
            snprintf(stop, sizeof(stop), "unavailable");
        cctl_stream_printf("  %-22s start %s, stop %s\n", "Battery thresholds", start, stop);
    } else SNAPSHOT_PREVIEW("Battery thresholds", "skip (not saved)");
    SNAPSHOT_PREVIEW("Refresh rate", strcmp(st->display_rate, "skip") == 0 ? "skip (not saved)" : st->display_rate);
    if (st->display_scale_known < 0) SNAPSHOT_PREVIEW("Display scale", "skip (not saved)");
    else SNAPSHOT_PREVIEW("Display scale", st->display_scale_set ? st->display_scale : "native (reset)");
    if (st->fancurve_state == SNAPSHOT_FANCURVE_SKIP)
        SNAPSHOT_PREVIEW("Fan curve", "skip (not saved)");
    else if (st->fancurve_state == SNAPSHOT_FANCURVE_NONE)
        SNAPSHOT_PREVIEW("Fan curve", "none (stop an active curve)");
    else if (st->fancurve_state == SNAPSHOT_FANCURVE_EMBEDDED) {
        cctl_snapshot_curve_summary(&st->fancurve_profile, value, sizeof(value));
        cctl_stream_printf("  %-22s %s\n", "Fan curve", value);
    } else SNAPSHOT_PREVIEW("Fan curve", "curve config unavailable in older snapshot");
#undef SNAPSHOT_PREVIEW
    struct fancurve_state current_curve;
    if (fc_any_active(&current_curve))
        cctl_stream_puts("Fan safety: active curve keeps control during restore; initial AUTO is skipped.");
    else if (nosafe)
        cctl_stream_puts("Fan safety: initial AUTO skipped (--mask nosafe).");
    else
        cctl_stream_puts("Fan safety: fans set to AUTO before restore; saved curve state is applied last.");
    cctl_stream_puts("Manual fan speeds are not saved; the saved fan curve configuration is restored last.");
}

static const char *cctl_snapshot_value(int value, char *buf, size_t size)
{
    if (value < 0) return "--";
    snprintf(buf, size, "%d", value);
    return buf;
}

static void cctl_snapshot_view(const char *name, const struct cctl_snapshot_data *st)
{
    int prev = g_stream_mode;
    g_stream_mode = STREAM_MODE_LINE;
    g_stream_line = 0;
    char buf[64];
    cctl_stream_printf("Snapshot '%s':\n", name);
    if (st->ec_profile_code < 0)
        cctl_stream_printf("  EC profile code: --\n");
    else
        cctl_stream_printf("  EC profile code: %s (%d)\n", gpu_profile_name(st->ec_profile_code), st->ec_profile_code);
    if (st->core_clock_known < 0) cctl_stream_printf("  NVIDIA core clock: --\n");
    else if (!st->core_clock_set) cctl_stream_printf("  NVIDIA core clock: reset/unlocked\n");
    else cctl_stream_printf("  NVIDIA core clock: %d–%d MHz\n", st->core_clock_min, st->core_clock_max);
    if (st->mem_clock_known < 0) cctl_stream_printf("  NVIDIA memory clock: --\n");
    else if (!st->mem_clock_set) cctl_stream_printf("  NVIDIA memory clock: reset/unlocked\n");
    else cctl_stream_printf("  NVIDIA memory clock: %d–%d MHz\n", st->mem_clock_min, st->mem_clock_max);
    cctl_stream_printf("  Turbo: %s\n", st->turbo < 0 ? "--" : (st->turbo ? "ON" : "OFF"));
    cctl_stream_printf("  Governor: %s\n", strcmp(st->governor, "skip") == 0 ? "--" : st->governor);
    cctl_stream_printf("  EPP: %s\n", strcmp(st->epp, "skip") == 0 ? "--" : st->epp);
    if (st->pl1 < 0) cctl_stream_printf("  PL1: --\n");
    else cctl_stream_printf("  PL1: %dW\n", st->pl1);
    if (st->pl2 < 0) cctl_stream_printf("  PL2: --\n");
    else cctl_stream_printf("  PL2: %dW\n", st->pl2);
    if (st->kbd_r < 0) cctl_stream_printf("  Keyboard color: --\n");
    else cctl_stream_printf("  Keyboard color: RGB(%d, %d, %d)\n", st->kbd_r, st->kbd_g, st->kbd_b);
    if (st->kbd_brightness < 0) cctl_stream_printf("  Keyboard brightness: --\n");
    else cctl_stream_printf("  Keyboard brightness: %d%%\n", (st->kbd_brightness * 100 + 127) / 255);
    cctl_stream_printf("  Keyboard effect: %s\n", strcmp(st->kbd_effect, "skip") == 0 ? "--" : st->kbd_effect);
    cctl_stream_printf("  Fn Lock: %s\n", st->fn_lock < 0 ? "--" : (st->fn_lock ? "ON" : "OFF"));
    cctl_stream_printf("  Webcam: %s\n", st->webcam < 0 ? "--" : (st->webcam ? "ON" : "OFF"));
    cctl_stream_printf("  Microphone: %s\n", st->microphone < 0 ? "--" : (st->microphone ? "ON" : "OFF"));
    cctl_stream_printf("  Battery thresholds: %s%% → %s%%\n",
           cctl_snapshot_value(st->battery_start, buf, sizeof(buf)),
           cctl_snapshot_value(st->battery_stop, buf + 32, sizeof(buf) - 32));
    cctl_stream_printf("  Battery charge mode: %s\n",
           cctl_snapshot_battery_mode_label(st->battery_mode));
    cctl_stream_printf("  Refresh rate: %s%s\n", strcmp(st->display_rate, "skip") == 0 ? "--" : st->display_rate,
           strcmp(st->display_rate, "skip") == 0 ? "" : "Hz");
    if (st->display_scale_known < 0) cctl_stream_printf("  Display scale: --\n");
    else if (st->display_scale_set) cctl_stream_printf("  Display scale: %s\n", st->display_scale);
    else cctl_stream_printf("  Display scale: native (1x1)\n");
    if (st->fancurve_state == SNAPSHOT_FANCURVE_SKIP)
        cctl_stream_printf("  Fan curve: --\n");
    else if (st->fancurve_state == SNAPSHOT_FANCURVE_NONE)
        cctl_stream_printf("  Fan curve: none (stopped)\n");
    else if (st->fancurve_state == SNAPSHOT_FANCURVE_EMBEDDED) {
        char curve_summary[512];
        cctl_snapshot_curve_summary(&st->fancurve_profile,
                                    curve_summary, sizeof(curve_summary));
        cctl_stream_printf("  Fan curve points: %s\n", curve_summary);
    } else cctl_stream_printf("  Fan curve: unavailable (older snapshot)\n");
    g_stream_mode = prev;
}

static int cctl_snapshot_save(const char *name, unsigned long long mask)
{
    char existing[128];
    int found = cctl_snapshot_find_existing(name, existing, sizeof(existing));
    if (found < 0)
        return fail("Cannot inspect existing snapshot '%s'", name);
    if (found) {
        cctl_stream_fprintf(stderr, "Warning: snapshot '%s' already exists. Replace it? [y/N] ", name);
        fflush(stderr);
        if (!isatty(STDIN_FILENO)) {
            cctl_stream_fprintf(stderr, "\nCancelled. Run this command in a terminal to confirm replacement.\n");
            return 1;
        }
        char answer[32];
        if (!fgets(answer, sizeof(answer), stdin)) {
            cctl_stream_puts("Cancelled; the existing snapshot was kept.");
            return 1;
        }
        answer[strcspn(answer, "\r\n")] = '\0';
        if (strcasecmp(answer, "y") != 0 && strcasecmp(answer, "yes") != 0) {
            cctl_stream_puts("Cancelled; the existing snapshot was kept.");
            return 1;
        }
    }

    struct cctl_snapshot_data st;
    if (cctl_snapshot_read_current(&st) != 0)
        return 1;
    cctl_snapshot_apply_mask(&st, mask);
    if (cctl_snapshot_write_file(name, &st) != 0) {
        cctl_stream_perror("Error: could not save snapshot");
        return 1;
    }
    stream_printf(STREAM_DELAY_US, "Saved snapshot '%s' to %s/%s.conf\n", name,
                  CCTL_SNAPSHOT_DIR, name);
    return 0;
}

static int cctl_snapshot_delete(const char *name, int noconfirm)
{
    char path[160];
    int exists = 0;
    cctl_snapshot_path(name, path, sizeof(path));
    if (cctl_snapshot_validate_path(path, &exists) != 0)
        return fail("Cannot safely delete snapshot '%s'", name);
    if (!exists)
        return fail("Snapshot '%s' does not exist", name);
    if (!noconfirm && !cctl_confirm_delete("snapshot", name))
        return 1;
    if (unlink(path) != 0) {
        cctl_stream_perror("Error: could not delete snapshot");
        return 1;
    }
    stream_printf(STREAM_DELAY_US, "Deleted snapshot '%s'.\n", name);
    return 0;
}

static void cctl_snapshot_divider(void)
{
    cctl_stream_printf("%s---\n%s", C_DIM, C_RST);
}

static const char *snapshot_unchanged_tag(void)
{
    return use_color ? "  \033[36m(already set)\033[0m" : "  (already set)";
}

static int cctl_snapshot_apply_step(const char *name, int rc, int *errors)
{
    if (rc != 0) {
        cctl_stream_fprintf(stderr, "  %sFailed to restore %s; continuing.%s\n", C_RED, name, C_RST);
        (*errors)++;
        return -1;
    }
    return 0;
}

static void cctl_snapshot_restore_battery(const struct cctl_snapshot_data *st,
                                          int *errors)
{
    int have_targets = st->battery_start >= 0 || st->battery_stop >= 0;
    int current_start = bat_read_start();
    int current_stop = bat_read_end();
    int target_start = st->battery_start >= 0 ? st->battery_start : current_start;
    int target_stop = st->battery_stop >= 0 ? st->battery_stop : current_stop;
    int thresholds_available = target_start > 0 && target_stop > 0;
    int thresholds_match = thresholds_available &&
                           current_start == target_start && current_stop == target_stop;
    char current_mode[32] = {0};
    int have_mode = bat_read_charge_type(current_mode, sizeof(current_mode)) >= 0;

    if (strcmp(st->battery_mode, "standard") == 0) {
        int thresholds_reapplied = have_targets && thresholds_available && !thresholds_match;
        if (thresholds_reapplied)
            cctl_snapshot_apply_step("battery thresholds",
                bat_set(target_start, target_stop), errors);
        else if (have_targets && !thresholds_available) {
            cctl_stream_fprintf(stderr,
                "  %sCould not restore saved battery thresholds; current values are unavailable.%s\n",
                C_RED, C_RST);
            (*errors)++;
        }
        if (have_targets && thresholds_available && !thresholds_reapplied)
            cctl_stream_printf("  Charge thresholds: start %d%% → stop %d%%%s\n",
                               target_start, target_stop, snapshot_unchanged_tag());

        if (!thresholds_reapplied && have_mode &&
            strcasecmp(current_mode, "Standard") == 0) {
            cctl_stream_printf("  Battery charge mode: Standard%s\n",
                               snapshot_unchanged_tag());
        } else if (cctl_snapshot_apply_step("battery charge mode",
                    bat_set_charge_type("Standard"), errors) == 0) {
            cctl_stream_printf("  Battery charge mode: Standard%s\n",
                               snapshot_applied_tag());
        }
        return;
    }

    if (strcmp(st->battery_mode, "custom") == 0) {
        if (have_targets && thresholds_available) {
            if (have_mode && strcasecmp(current_mode, "Custom") == 0 && thresholds_match) {
                cctl_stream_printf("  Battery charge mode: Custom%s\n",
                                   snapshot_unchanged_tag());
                cctl_stream_printf("  Charge thresholds: start %d%% → stop %d%%%s\n",
                                   target_start, target_stop, snapshot_unchanged_tag());
            } else {
                cctl_snapshot_apply_step("battery thresholds",
                    bat_set(target_start, target_stop), errors);
            }
        } else {
            if (have_targets) {
                cctl_stream_fprintf(stderr,
                    "  %sCould not restore saved battery thresholds; current values are unavailable.%s\n",
                    C_RED, C_RST);
                (*errors)++;
            }
            if (have_mode && strcasecmp(current_mode, "Custom") == 0) {
                cctl_stream_printf("  Battery charge mode: Custom%s\n",
                                   snapshot_unchanged_tag());
            } else if (cctl_snapshot_apply_step("battery charge mode",
                        bat_set_charge_type("Custom"), errors) == 0) {
                cctl_stream_printf("  Battery charge mode: Custom%s\n",
                                   snapshot_applied_tag());
            }
        }
        return;
    }

    /* Older snapshots and masked fields have no saved mode. Preserve the
     * original behavior: restoring thresholds selects Custom mode. */
    if (!have_targets) {
        cctl_stream_printf("  %sSkipping battery settings (not saved).%s\n", C_DIM, C_RST);
    } else if (thresholds_available && have_mode &&
               strcasecmp(current_mode, "Custom") == 0 && thresholds_match) {
        cctl_stream_printf("  Battery charge mode: Custom%s\n",
                           snapshot_unchanged_tag());
        cctl_stream_printf("  Charge thresholds: start %d%% → stop %d%%%s\n",
                           target_start, target_stop, snapshot_unchanged_tag());
    } else if (thresholds_available) {
        cctl_snapshot_apply_step("battery thresholds",
            bat_set(target_start, target_stop), errors);
    } else {
        cctl_stream_fprintf(stderr,
            "  %sCould not restore saved battery thresholds; current values are unavailable.%s\n",
            C_RED, C_RST);
        (*errors)++;
    }
}

static void cctl_snapshot_apply_fan_safety(const char *target, int nosafe,
                                           int *fans_auto_applied,
                                           int *fan_safety_bypassed,
                                           int *errors)
{
    if (*fans_auto_applied || *fan_safety_bypassed)
        return;
    if (nosafe) {
        cctl_stream_printf("  %sSkipping automatic fan control (--mask nosafe).%s\n", C_DIM, C_RST);
        *fan_safety_bypassed = 1;
        return;
    }

    struct fancurve_state curve_st;
    if (fc_is_running(&curve_st)) {
        cctl_stream_printf("  Fan curve '%s' active; keeping curve control.\n",
               curve_st.name);
        *fan_safety_bypassed = 1;
        return;
    }

    cctl_stream_printf("  Setting fans to AUTO for %s...\n", target);
    if (cctl_snapshot_apply_step("automatic fan control", fan_auto_all(), errors) == 0)
        *fans_auto_applied = 1;
}

static void cctl_snapshot_restore_fancurve(const char *snapshot_name,
                                           const struct cctl_snapshot_data *st,
                                           int *errors)
{
    if (st->fancurve_state == SNAPSHOT_FANCURVE_SKIP) {
        cctl_stream_printf("  %sSkipping fan curve (not saved).%s\n", C_DIM, C_RST);
        return;
    }

    struct fancurve_state current;
    int active = fc_is_running(&current);
    if (st->fancurve_state == SNAPSHOT_FANCURVE_NONE) {
        if (!active) {
            cctl_stream_printf("  Fan curve: none (stopped)%s\n", snapshot_unchanged_tag());
            return;
        }
        cctl_stream_printf("  Fan curve: stopping active curve (snapshot has none)\n");
        cctl_snapshot_apply_step("fan curve stop", fc_stop_worker(1, 1), errors);
        return;
    }

    struct fancurve_profile profile = st->fancurve_profile;
    if (st->fancurve_state == SNAPSHOT_FANCURVE_LEGACY) {
        char err[256];
        if (fc_load_profile(st->fancurve_legacy_name, &profile,
                            err, sizeof(err)) < 0) {
            cctl_stream_fprintf(stderr,
                "  %sCould not restore the fan curve from this older snapshot: %s.%s\n",
                C_RED, err, C_RST);
            (*errors)++;
            return;
        }
    }
    snprintf(profile.name, sizeof(profile.name), "snapshot-%s", snapshot_name);
    char curve_summary[512];
    cctl_snapshot_curve_summary(&profile, curve_summary, sizeof(curve_summary));

    if (active) {
        cctl_stream_printf("  Fan curve: replacing active curve with points %s\n",
                           curve_summary);
    } else {
        cctl_stream_printf("  Fan curve: starting points %s\n", curve_summary);
    }
    cctl_snapshot_apply_step("fan curve restore", fc_start_profile(&profile), errors);
}

static int cctl_snapshot_restore(const char *name, const struct cctl_snapshot_data *st, int nosafe)
{
    int prev = g_stream_mode;
    g_stream_mode = STREAM_MODE_LINE;
    g_stream_line = 0;
    g_snapshot_restoring = 1;
    int errors = 0;
    int fans_auto_applied = 0;
    int fan_safety_bypassed = 0;
    cctl_stream_printf("Restoring snapshot '%s' one setting at a time...\n", name);

    struct fancurve_state current_curve;
    int curve_active_at_start = fc_is_running(&current_curve);
    if (curve_active_at_start) {
        cctl_stream_printf("Active fan curve detected; it keeps control during restore.\n");
        fan_safety_bypassed = 1;
    } else if (!nosafe) {
        cctl_stream_printf("Setting both fans to AUTO before snapshot restore...\n");
        if (fan_auto_all() != 0) {
            cctl_stream_fprintf(stderr,
                "Error: could not restore EC automatic fan control; snapshot restore was stopped.\n");
            g_snapshot_restoring = 0;
            g_stream_mode = prev;
            return 1;
        }
        fans_auto_applied = 1;
    } else {
        cctl_stream_printf("Skipping initial fan AUTO (--mask nosafe).\n");
    }

    /* 1. EC profile code: apply the saved code when present. */
    cctl_snapshot_divider();
    if (st->ec_profile_code >= 0) {
        cctl_snapshot_apply_step("EC profile code", set_gpu_profile(st->ec_profile_code), &errors);
        if (ec_profile_needs_fan_auto(st->ec_profile_code)) {
            char target[64];
            snprintf(target, sizeof(target), "EC profile code %d", st->ec_profile_code);
            cctl_snapshot_apply_fan_safety(target, nosafe, &fans_auto_applied,
                                           &fan_safety_bypassed, &errors);
        }
    } else {
        cctl_stream_printf("  %sSkipping EC profile code (not saved).%s\n", C_DIM, C_RST);
    }

    /* 2. Turbo: query live state; skip if already matching */
    cctl_snapshot_divider();
    if (st->turbo >= 0) {
        if (st->turbo == 1) {
            int live_ec_code = cctl_live_ec_state(NULL, NULL);
            if (ec_profile_needs_fan_auto(live_ec_code) && !fans_auto_applied) {
                char target[80];
                snprintf(target, sizeof(target), "Turbo ON in EC profile code %d", live_ec_code);
                cctl_snapshot_apply_fan_safety(target, nosafe, &fans_auto_applied,
                                               &fan_safety_bypassed, &errors);
            }
        }
        long cur_turbo_raw = read_sysfs_long(TURBO_PATH, -1);
        int cur_turbo = (cur_turbo_raw == 0) ? 1 : ((cur_turbo_raw == 1) ? 0 : -1);
        if (cur_turbo >= 0 && cur_turbo == st->turbo) {
            cctl_stream_printf("  Turbo boost: %s%s\n", st->turbo ? "ON" : "OFF", snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("turbo", set_turbo(st->turbo), &errors);
        }
    } else {
        cctl_stream_printf("  %sSkipping turbo (not saved).%s\n", C_DIM, C_RST);
    }

    /* 3. Governor & 4. EPP: query live state; skip if already matching */
    struct cpu_profile_state cpu_profile;
    read_cpu_profile(&cpu_profile);

    cctl_snapshot_divider();
    if (strcmp(st->governor, "skip") != 0) {
        if (cpu_profile.have_governor && strcmp(cpu_profile.governor, st->governor) == 0) {
            cctl_stream_printf("  Governor: %s%s\n", st->governor, snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("governor", set_governor(st->governor), &errors);
        }
    } else {
        cctl_stream_printf("  %sSkipping governor (not saved).%s\n", C_DIM, C_RST);
    }

    cctl_snapshot_divider();
    if (strcmp(st->epp, "skip") != 0) {
        if (strcmp(st->governor, "skip") == 0) {
            char cur_gov[64] = {0};
            if (read_sysfs_str(CPU_GOVERNOR_PATH, cur_gov, sizeof(cur_gov)) >= 0 &&
                strcmp(cur_gov, "performance") == 0 &&
                strcmp(st->epp, "performance") != 0) {
                cctl_stream_printf("  %sNote:%s Governor is in %sperformance%s mode (EPP is locked).\n"
                       "        Changing governor to %spowersave%s to apply saved EPP '%s'.\n"
                       "        (Use '--mask epp' if you do not want governor to change).\n",
                       C_YLW, C_RST, C_CYN, C_RST, C_CYN, C_RST, st->epp);
                cctl_snapshot_apply_step("governor", set_governor("powersave"), &errors);
                read_cpu_profile(&cpu_profile);
            }
        }
        if (cpu_profile.have_epp && strcmp(cpu_profile.epp, st->epp) == 0) {
            cctl_stream_printf("  EPP: %s%s\n", st->epp, snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("EPP", set_epp(st->epp), &errors);
        }
    } else {
        cctl_stream_printf("  %sSkipping EPP (not saved).%s\n", C_DIM, C_RST);
    }

    /* 5. RAPL: query live state after EC profile set; skip if already matching */
    int pl1_cap = st->ec_profile_code == 2 ? RAPL_PL1_MAX_WATTS_MAX :
                  st->ec_profile_code >= 0 ? RAPL_PL1_MAX_WATTS : rapl_pl1_ceiling();
    int cur_pl1 = -1, cur_pl2 = -1;
    read_rapl_current(&cur_pl1, &cur_pl2);

    int need_pl1 = (st->pl1 >= 0 && st->pl1 != cur_pl1);
    int need_pl2 = (st->pl2 >= 0 && st->pl2 != cur_pl2);

    cctl_snapshot_divider();
    int snapshot_pl1_cap = st->ec_profile_code == 2
                         ? RAPL_PL1_MAX_WATTS_MAX : RAPL_PL1_MAX_WATTS;
    int pl1_rejected = 0;
    if (st->pl1 > 0 && !rapl_pl1_within_cap(st->pl1, snapshot_pl1_cap)) {
        if (st->pl2 >= 0) {
            cctl_stream_printf("  %sNote:%s EC profile code 2 is needed to set RAPL PL1 above 45W (saved: %dW).\n"
                   "        Skipping PL1 and restoring PL2 (%dW) alone.\n",
                   C_YLW, C_RST, st->pl1, st->pl2);
        } else {
            cctl_stream_printf("  %sNote:%s EC profile code 2 is needed to set RAPL PL1 above 45W (saved: %dW).\n"
                   "        Skipping PL1.\n",
                   C_YLW, C_RST, st->pl1);
        }
        pl1_rejected = 1;
        need_pl1 = 0;
    }

    int pl2_rejected = 0;
    int eff_pl1 = (st->pl1 >= 0 && !pl1_rejected) ? st->pl1 : cur_pl1;
    if (st->pl2 > 0 && (!rapl_pl2_within_cap(st->pl2) ||
                        !rapl_pl2_above_pl1(eff_pl1, st->pl2))) {
        cctl_stream_printf("  %sNote:%s Saved PL2 (%dW) must be greater than %s PL1 (%dW). Skipping PL2.\n",
               C_YLW, C_RST, st->pl2,
               (st->pl1 < 0) ? "current system" : (pl1_rejected ? "live" : "restored"),
               eff_pl1);
        pl2_rejected = 1;
        need_pl2 = 0;
    }

    if (st->pl1 < 0 && st->pl2 < 0) {
        cctl_stream_printf("  %sSkipping RAPL (not saved).%s\n", C_DIM, C_RST);
    } else if (st->pl1 < 0) {
        cctl_stream_printf("  %sSkipping PL1 (not saved).%s\n", C_DIM, C_RST);
        if (pl2_rejected) {
            /* PL2 rejected with note above */
        } else if (!need_pl2) {
            cctl_stream_printf("  RAPL: PL2 %dW%s\n", st->pl2, snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("RAPL PL2", set_rapl_limits(0, st->pl2, pl1_cap), &errors);
        }
    } else if (pl1_rejected) {
        if (st->pl2 < 0) {
            /* PL1 rejected and PL2 not saved */
        } else if (pl2_rejected) {
            /* Both rejected */
        } else if (!need_pl2) {
            cctl_stream_printf("  RAPL: PL2 %dW%s\n", st->pl2, snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("RAPL PL2", set_rapl_limits(0, st->pl2, pl1_cap), &errors);
        }
    } else if (!need_pl1 && !need_pl2) {
        if (!pl1_rejected && !pl2_rejected && st->pl1 >= 0 && st->pl2 >= 0)
            cctl_stream_printf("  RAPL: PL1 %dW, PL2 %dW%s\n", st->pl1, st->pl2, snapshot_unchanged_tag());
        else if (!pl1_rejected && st->pl1 >= 0)
            cctl_stream_printf("  RAPL: PL1 %dW%s\n", st->pl1, snapshot_unchanged_tag());
        else if (!pl2_rejected && st->pl2 >= 0)
            cctl_stream_printf("  RAPL: PL2 %dW%s\n", st->pl2, snapshot_unchanged_tag());
    } else {
        int apply_pl1 = need_pl1 ? st->pl1 : 0;
        int apply_pl2 = need_pl2 ? st->pl2 : 0;
        cctl_snapshot_apply_step("RAPL", set_rapl_limits(apply_pl1, apply_pl2, pl1_cap), &errors);
    }

    /* 6. NVIDIA GPU clocks: always apply if saved */
    cctl_snapshot_divider();
    if (st->core_clock_known < 0) {
        cctl_stream_printf("  %sSkipping NVIDIA core clock (not saved).%s\n", C_DIM, C_RST);
    } else if (st->core_clock_set) {
        cctl_stream_printf("  NVIDIA core clock: %d–%d MHz%s\n",
               st->core_clock_min, st->core_clock_max, snapshot_applied_tag());
        cctl_snapshot_apply_step("NVIDIA core clock",
            nvidia_clock_apply("-lgc", st->core_clock_min, st->core_clock_max), &errors);
    } else {
        cctl_stream_printf("  NVIDIA core clock: reset/unlocked%s\n", snapshot_applied_tag());
        cctl_snapshot_apply_step("NVIDIA core clock reset",
            nvidia_clock_reset_flag("-rgc"), &errors);
    }

    cctl_snapshot_divider();
    if (st->mem_clock_known < 0) {
        cctl_stream_printf("  %sSkipping NVIDIA memory clock (not saved).%s\n", C_DIM, C_RST);
    } else if (st->mem_clock_set) {
        cctl_stream_printf("  NVIDIA memory clock: %d–%d MHz%s\n",
               st->mem_clock_min, st->mem_clock_max, snapshot_applied_tag());
        cctl_snapshot_apply_step("NVIDIA memory clock",
            nvidia_clock_apply("-lmc", st->mem_clock_min, st->mem_clock_max), &errors);
    } else {
        cctl_stream_printf("  NVIDIA memory clock: reset/unlocked%s\n", snapshot_applied_tag());
        cctl_snapshot_apply_step("NVIDIA memory clock reset",
            nvidia_clock_reset_flag("-rmc"), &errors);
    }

    /* 7. Keyboard: check live color, brightness, and effect */
    cctl_snapshot_divider();
    int have_kbd_update = st->kbd_r >= 0 || st->kbd_g >= 0 || st->kbd_b >= 0 ||
                          st->kbd_brightness >= 0 || strcmp(st->kbd_effect, "skip") != 0;
    if (have_kbd_update) {
        struct kbe_state ks;
        int preserve_effect = strcmp(st->kbd_effect, "skip") == 0;
        int effect_was_running = kbe_is_running(&ks);
        char running_effect[sizeof(ks.effect)] = {0};
        if (effect_was_running)
            snprintf(running_effect, sizeof(running_effect), "%s", ks.effect);

        int cur_r = -1, cur_g = -1, cur_b = -1, cur_bri = -1;
        if (effect_was_running) {
            cur_r = ks.orig_r; cur_g = ks.orig_g; cur_b = ks.orig_b; cur_bri = ks.orig_bri;
        } else {
            kbd_get_color(&cur_r, &cur_g, &cur_b);
            kbd_get_raw_brightness(&cur_bri);
        }

        int target_effect_matches = 0;
        if (preserve_effect) {
            target_effect_matches = 1;
        } else if (strcmp(st->kbd_effect, "none") == 0) {
            target_effect_matches = !effect_was_running;
        } else {
            target_effect_matches = (effect_was_running && strcmp(running_effect, st->kbd_effect) == 0);
        }

        int target_col_matches = (st->kbd_r < 0) || (cur_r == st->kbd_r && cur_g == st->kbd_g && cur_b == st->kbd_b);
        int target_bri_matches = (st->kbd_brightness < 0) || (cur_bri == st->kbd_brightness);

        if (target_effect_matches && target_col_matches && target_bri_matches) {
            if (st->kbd_r >= 0)
                cctl_stream_printf("  Keyboard color: RGB(%d, %d, %d)%s\n",
                       st->kbd_r, st->kbd_g, st->kbd_b, snapshot_unchanged_tag());
            if (st->kbd_brightness >= 0)
                cctl_stream_printf("  Keyboard brightness: %d%%%s\n",
                       (st->kbd_brightness * 100 + 127) / 255, snapshot_unchanged_tag());
            if (!preserve_effect)
                cctl_stream_printf("  Keyboard effect: %s%s\n", st->kbd_effect, snapshot_unchanged_tag());
            else if (effect_was_running)
                cctl_stream_printf("  Keyboard effect: preserving active effect '%s'%s\n", running_effect, snapshot_unchanged_tag());
            else
                cctl_stream_printf("  %sSkipping keyboard effect (not saved).%s\n", C_DIM, C_RST);
        } else {
            if (effect_was_running)
                cctl_snapshot_apply_step("current keyboard effect stop", kbe_stop(1), &errors);

            if (st->kbd_r >= 0 && st->kbd_g >= 0 && st->kbd_b >= 0) {
                if (cur_r == st->kbd_r && cur_g == st->kbd_g && cur_b == st->kbd_b) {
                    cctl_stream_printf("  Keyboard color: RGB(%d, %d, %d)%s\n",
                           st->kbd_r, st->kbd_g, st->kbd_b, snapshot_unchanged_tag());
                } else {
                    cctl_stream_printf("  Keyboard color: RGB(%d, %d, %d)%s\n",
                           st->kbd_r, st->kbd_g, st->kbd_b, snapshot_applied_tag());
                    cctl_snapshot_apply_step("keyboard color",
                        kbd_write_color(st->kbd_r, st->kbd_g, st->kbd_b), &errors);
                }
            } else {
                cctl_stream_printf("  %sSkipping keyboard color (not saved).%s\n", C_DIM, C_RST);
            }

            if (st->kbd_brightness >= 0) {
                if (cur_bri == st->kbd_brightness) {
                    cctl_stream_printf("  Keyboard brightness: %d%%%s\n",
                           (st->kbd_brightness * 100 + 127) / 255, snapshot_unchanged_tag());
                } else {
                    cctl_stream_printf("  Keyboard brightness: %d%%%s\n",
                           (st->kbd_brightness * 100 + 127) / 255, snapshot_applied_tag());
                    cctl_snapshot_apply_step("keyboard brightness",
                        write_sysfs_int(KBD_PATH "/brightness", st->kbd_brightness), &errors);
                }
            } else {
                cctl_stream_printf("  %sSkipping keyboard brightness (not saved).%s\n", C_DIM, C_RST);
            }

            if (strcmp(st->kbd_effect, "skip") != 0 && strcmp(st->kbd_effect, "none") != 0) {
                pid_t pid = 0;
                if (kbe_start_ext(st->kbd_effect, &pid, 1) == 0) {
                    cctl_stream_printf("  Keyboard effect: %s (PID %d)%s\n", st->kbd_effect, (int)pid, snapshot_applied_tag());
                } else {
                    cctl_stream_fprintf(stderr, "  %sFailed to restore keyboard effect '%s'; continuing.%s\n", C_RED, st->kbd_effect, C_RST);
                    errors++;
                }
            } else if (preserve_effect && effect_was_running) {
                pid_t pid = 0;
                if (kbe_start_ext(running_effect, &pid, 1) == 0) {
                    cctl_stream_printf("  Keyboard effect: preserving active effect '%s' (restarted with restored color, PID %d)\n",
                           running_effect, (int)pid);
                } else {
                    cctl_stream_fprintf(stderr, "  %sFailed to restart preserved keyboard effect '%s'; continuing.%s\n", C_RED, running_effect, C_RST);
                    errors++;
                }
            } else if (strcmp(st->kbd_effect, "none") == 0) {
                cctl_stream_printf("  Keyboard effect: none%s\n", snapshot_applied_tag());
            } else {
                cctl_stream_printf("  %sSkipping keyboard effect (not saved).%s\n", C_DIM, C_RST);
            }
        }
    } else {
        cctl_stream_printf("  %sSkipping keyboard settings (not saved).%s\n", C_DIM, C_RST);
    }

    /* 8. Fn Lock: query live state; skip if already matching */
    cctl_snapshot_divider();
    if (st->fn_lock >= 0) {
        int cur_fnlock = get_fnlock();
        if (cur_fnlock >= 0 && cur_fnlock == st->fn_lock) {
            cctl_stream_printf("  Fn Lock: %s%s\n", st->fn_lock ? "ON" : "OFF", snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("Fn Lock", set_fnlock(st->fn_lock), &errors);
        }
    } else {
        cctl_stream_printf("  %sSkipping Fn Lock (not saved).%s\n", C_DIM, C_RST);
    }

    /* 9. Webcam: query live state; skip if already matching */
    cctl_snapshot_divider();
    if (st->webcam >= 0) {
        int cur_webcam = is_webcam_enabled();
        if (cur_webcam >= 0 && cur_webcam == st->webcam) {
            cctl_stream_printf("  Webcam: %s%s\n", st->webcam ? "ON" : "OFF", snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("webcam", webcam_set(st->webcam), &errors);
        }
    } else {
        cctl_stream_printf("  %sSkipping webcam (not saved).%s\n", C_DIM, C_RST);
    }

    /* 10. Microphone: query live state; skip if already matching */
    cctl_snapshot_divider();
    if (st->microphone >= 0) {
        int cur_mic = mic_is_enabled();
        if (cur_mic >= 0 && cur_mic == st->microphone) {
            cctl_stream_printf("  Laptop internal microphone: %s%s\n", st->microphone ? "ON" : "OFF", snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("microphone", mic_set(st->microphone), &errors);
        }
    } else {
        cctl_stream_printf("  %sSkipping microphone (not saved).%s\n", C_DIM, C_RST);
    }

    /* 11. Battery charge mode and thresholds. */
    cctl_snapshot_divider();
    cctl_snapshot_restore_battery(st, &errors);

    /* 12. Display refresh rate: query live state; skip if already matching (prevents X11 mode reset / flicker) */
    cctl_snapshot_divider();
    if (strcmp(st->display_rate, "skip") != 0) {
        struct display_info disp;
        int have_disp = (has_display_support() && query_display_info(&disp) == 0 && disp.current_rate[0]);
        if (have_disp && strcmp(disp.current_rate, st->display_rate) == 0) {
            cctl_stream_printf("  Refresh rate: %sHz%s\n", st->display_rate, snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("display refresh rate", rr_set(st->display_rate), &errors);
        }
    } else {
        cctl_stream_printf("  %sSkipping display refresh rate (not saved).%s\n", C_DIM, C_RST);
    }

    /* 13. Display scale: compare the live X11 desktop dimensions. */
    cctl_snapshot_divider();
    if (st->display_scale_known < 0) {
        cctl_stream_printf("  %sSkipping display scale (not saved).%s\n", C_DIM, C_RST);
    } else {
        struct display_info disp;
        int have_disp = has_display_support() && query_display_info(&disp) == 0 &&
                        disp.current_resolution[0] && disp.resolution[0];
        if (st->display_scale_set) {
            if (have_disp && strcmp(disp.current_resolution, st->display_scale) == 0) {
                cctl_stream_printf("  Display scale: %s%s\n", st->display_scale, snapshot_unchanged_tag());
            } else {
                cctl_snapshot_apply_step("display scale", scale_set(st->display_scale), &errors);
            }
        } else {
            if (have_disp && strcmp(disp.current_resolution, disp.resolution) == 0) {
                cctl_stream_printf("  Scale: native (1x1)%s\n", snapshot_unchanged_tag());
            } else {
                cctl_snapshot_apply_step("display scale reset", scale_reset(), &errors);
            }
        }
    }

    /* 14. Restore the saved fan curve last, after all other settings. */
    cctl_snapshot_divider();
    cctl_snapshot_restore_fancurve(name, st, &errors);

    cctl_snapshot_divider();
    g_snapshot_restoring = 0;

    if (errors) {
        cctl_stream_fprintf(stderr, "%sSnapshot '%s' restored with %d error(s).%s\n",
                C_RED, name, errors, C_RST);
        g_stream_mode = prev;
        return 1;
    }
    cctl_stream_printf("%sSnapshot '%s' restored.%s\n", C_GRN, name, C_RST);
    g_stream_mode = prev;
    return 0;
}

static int cmd_snapshot(int argc, char **argv)
{
    if (argc < 3) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        cctl_stream_printf("%sUsage:%s %scctl snap <save|s|restore|r|view|v|delete|del> [name] [options]%s\n",
               C_BLD, C_RST, C_CYN_BLD, C_RST);
        cctl_stream_printf("       %scctl snapshot <action> [name] [options]%s\n\n", C_CYN_BLD, C_RST);
        cctl_stream_printf("%sActions:%s\n", C_YLW, C_RST);
        cctl_stream_printf("  save/s <name> [--mask fields]       Save current settings\n");
        cctl_stream_printf("  restore/r <name> [--mask fields] [--dry-run]  Apply or preview settings\n");
        cctl_stream_printf("  view/v [name]                       List snapshots or show one snapshot\n");
        cctl_stream_printf("  delete/del <name> [--noconfirm]      Delete a snapshot (prompts by default)\n");
        cctl_stream_printf("  Names: letters, numbers, '-' and '_' only; no spaces (max %d characters).\n",
               CCTL_SNAPSHOT_NAME_MAX);
        cctl_stream_printf("\n%sMask fields:%s\n", C_YLW, C_RST);
        cctl_stream_printf("  nvidia_clock,nvidia_memclock,ec_code,turbo,gov,epp,pl1,pl2,\n");
        cctl_stream_printf("              webcam,mic,rr,scale,bat,kbe,kbc,kbb,fn_lock,fancurve,nosafe\n");
        cctl_stream_printf("  Use 'nosafe' only with restore to bypass automatic fan safety.\n");
        cctl_stream_printf("  'bat' masks battery mode and thresholds; 'fancurve' preserves curve config.\n");
        cctl_stream_printf("\n%sFan settings:%s manual fan speeds are not saved. The active fan curve\n"
               "  configuration is saved and restored last; --mask fancurve preserves it.\n"
        "  Restore sets fans to AUTO first if no curve is active; nosafe skips it.\n",
               C_YLW, C_RST);
        g_stream_mode = prev;
        return 0;
    }

    const char *action = argv[2];
    if (strcmp(action, "s") == 0) action = "save";
    else if (strcmp(action, "r") == 0) action = "restore";
    else if (strcmp(action, "v") == 0) action = "view";
    else if (strcmp(action, "del") == 0) action = "delete";

    if (strcmp(action, "view") == 0 && argc == 3) {
        require_root(argc, argv);
        return cctl_snapshot_list();
    }
    if (argc < 4 || argc > 7)
        return fail("Usage: cctl snap <save|s|restore|r|view|v|delete|del> [name] [--mask fields] [--dry-run] [--noconfirm]");
    const char *name = argv[3];
    if (!cctl_snapshot_name_valid(name))
        return fail("Snapshot names must be 1-%d characters using only letters, numbers, '-' or '_'",
                    CCTL_SNAPSHOT_NAME_MAX);

    unsigned long long mask = 0;
    int have_mask = 0, snapshot_dry_run = 0, snapshot_noconfirm = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--mask") == 0) {
            if (have_mask || i + 1 >= argc ||
                cctl_snapshot_parse_mask(argv[++i], &mask) != 0)
                return fail("Invalid mask list (see 'cctl snap' for available fields)");
            have_mask = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            if (snapshot_dry_run) return fail("--dry-run was specified more than once");
            snapshot_dry_run = 1;
        } else if (strcmp(argv[i], "--noconfirm") == 0) {
            if (snapshot_noconfirm) return fail("--noconfirm was specified more than once");
            snapshot_noconfirm = 1;
        } else {
            return fail("Unknown snapshot option '%s'", argv[i]);
        }
    }
    if (mask && strcmp(action, "save") != 0 && strcmp(action, "restore") != 0)
        return fail("--mask is supported only for snapshot save and restore");
    if ((mask & MASK_NOSAFE) && strcmp(action, "restore") != 0)
        return fail("--mask nosafe is supported only for snapshot restore");
    if (snapshot_dry_run && strcmp(action, "restore") != 0)
        return fail("--dry-run is supported only for snapshot restore");
    if (snapshot_noconfirm && strcmp(action, "delete") != 0 && strcmp(action, "del") != 0)
        return fail("--noconfirm is supported only for snapshot delete");

    if (strcmp(action, "save") == 0) {
        require_root(argc, argv);
        return cctl_snapshot_save(name, mask);
    }
    if (strcmp(action, "delete") == 0) {
        require_root(argc, argv);
        return cctl_snapshot_delete(name, snapshot_noconfirm);
    }
    if (strcmp(action, "view") == 0 || strcmp(action, "restore") == 0) {
        require_root(argc, argv);
        struct cctl_snapshot_data st;
        if (cctl_snapshot_read_file(name, &st) != 0)
            return fail("Cannot read snapshot '%s' (missing or invalid file)", name);
        if (strcmp(action, "view") == 0) {
            cctl_snapshot_view(name, &st);
            return 0;
        }
        cctl_snapshot_apply_mask(&st, mask);
        if (snapshot_dry_run) {
            cctl_snapshot_preview_restore(name, &st, (mask & MASK_NOSAFE) != 0);
            return 0;
        }
        return cctl_snapshot_restore(name, &st, (mask & MASK_NOSAFE) != 0);
    }
    return fail("Unknown snapshot action '%s' (use save, restore, view, or delete)", argv[2]);
}
