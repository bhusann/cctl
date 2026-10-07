/* ========================================================================
 * NUMBERED SNAPSHOTS
 * ======================================================================== */

#define CCTL_SNAPSHOT_DIR "/var/lib/cctl/snapshots"

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
    char display_rate[16];
    int display_scale_known, display_scale_set;
    char display_scale[32];
};

enum snapshot_field {
    SNAPSHOT_VERSION, SNAPSHOT_EC, SNAPSHOT_GPU_CLOCK, SNAPSHOT_MEM_CLOCK, SNAPSHOT_TURBO,
    SNAPSHOT_GOVERNOR, SNAPSHOT_EPP, SNAPSHOT_PL1, SNAPSHOT_PL2, SNAPSHOT_KBD_R, SNAPSHOT_KBD_G,
    SNAPSHOT_KBD_B, SNAPSHOT_KBD_BRIGHTNESS, SNAPSHOT_KBD_EFFECT, SNAPSHOT_FN_LOCK,
    SNAPSHOT_WEBCAM, SNAPSHOT_MICROPHONE, SNAPSHOT_BATTERY_START, SNAPSHOT_BATTERY_STOP,
    SNAPSHOT_DISPLAY_RATE, SNAPSHOT_DISPLAY_SCALE,
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
    MASK_SCALE         = 1ULL << 16
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
    { "bat", MASK_BAT }, { "rr", MASK_RR }, { "scale", MASK_SCALE }
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
    if (mask & MASK_BAT) st->battery_start = st->battery_stop = -1;
    if (mask & MASK_RR) snprintf(st->display_rate, sizeof(st->display_rate), "skip");
    if (mask & MASK_SCALE) {
        st->display_scale_known = -1;
        snprintf(st->display_scale, sizeof(st->display_scale), "skip");
    }
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
    snprintf(st->display_rate, sizeof(st->display_rate), "skip");
    st->display_scale_known = -1;
    snprintf(st->display_scale, sizeof(st->display_scale), "skip");
}

static int cctl_snapshot_number(const char *arg, int *number)
{
    if (!arg || !*arg) return -1;
    for (const char *p = arg; *p; p++)
        if (!isdigit((unsigned char)*p)) return -1;
    errno = 0;
    long value = strtol(arg, NULL, 10);
    if (errno || value < 0 || value > 999999999L) return -1;
    *number = (int)value;
    return 0;
}

static void cctl_snapshot_path(int number, char *path, size_t size)
{
    snprintf(path, size, CCTL_SNAPSHOT_DIR "/%d.conf", number);
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
static int cctl_snapshot_find_existing(int number, char *path, size_t size)
{
    char candidate[128];
    int exists = 0;
    cctl_snapshot_path(number, candidate, sizeof(candidate));
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

static int cctl_snapshot_add_number(int number, int **numbers,
                                   size_t *count, size_t *capacity)
{
    for (size_t i = 0; i < *count; i++)
        if ((*numbers)[i] == number) return 0;
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2 : 16;
        if (next_capacity < *capacity || next_capacity > SIZE_MAX / sizeof(**numbers)) {
            errno = EOVERFLOW;
            return -1;
        }
        int *next = realloc(*numbers, next_capacity * sizeof(**numbers));
        if (!next) return -1;
        *numbers = next;
        *capacity = next_capacity;
    }
    (*numbers)[(*count)++] = number;
    return 0;
}

static int cctl_snapshot_collect_dir(const char *path, int **numbers,
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
        if (len < 6 || len > 14 || strcmp(entry->d_name + len - 5, ".conf") != 0)
            continue;
        size_t digits = len - 5;
        if (digits > 9) continue;
        char number_text[10];
        memcpy(number_text, entry->d_name, digits);
        number_text[digits] = '\0';
        int number;
        if (cctl_snapshot_number(number_text, &number) != 0) continue;

        char file_path[256];
        int written = snprintf(file_path, sizeof(file_path), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(file_path)) continue;
        int exists;
        if (cctl_snapshot_validate_path(file_path, &exists) != 0 || !exists)
            continue;
        if (cctl_snapshot_add_number(number, numbers, count, capacity) != 0) {
            failed = 1;
            break;
        }
    }
    closedir(dir);
    return failed ? -1 : 0;
}

static int cctl_snapshot_compare_numbers(const void *left, const void *right)
{
    int a = *(const int *)left;
    int b = *(const int *)right;
    return (a > b) - (a < b);
}

static int cctl_snapshot_list(void)
{
    int *numbers = NULL;
    size_t count = 0, capacity = 0;
    if (cctl_snapshot_collect_dir(CCTL_SNAPSHOT_DIR, &numbers, &count, &capacity) != 0) {
        free(numbers);
        return fail("Cannot list saved snapshots");
    }
    if (count > 1)
        qsort(numbers, count, sizeof(*numbers), cctl_snapshot_compare_numbers);

    printf("Saved snapshots: ");
    if (count == 0) printf("none");
    for (size_t i = 0; i < count; i++)
        printf("%s%d", i == 0 ? "" : ", ", numbers[i]);
    putchar('\n');
    free(numbers);
    return 0;
}

static void cctl_snapshot_read_current(struct cctl_snapshot_data *st)
{
    cctl_snapshot_init(st);

    /* Runtime metadata is the source for the EC code and cctl-applied GPU
     * clock locks. The profile label itself is intentionally not snapshotted. */
    struct mode_runtime_data mode;
    if (mode_runtime_read(&mode) == 0) {
        st->ec_profile_code = mode.ec_profile_code;
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
        if (mode.display_scale_known) {
            st->display_scale_known = 1;
            st->display_scale_set = mode.display_scale_set;
            snprintf(st->display_scale, sizeof(st->display_scale), "%s", mode.display_scale);
        }
    }

    struct display_info display;
    if (has_display_support() && query_display_info(&display) == 0 && display.current_rate[0])
        snprintf(st->display_rate, sizeof(st->display_rate), "%s", display.current_rate);

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
}

static int cctl_snapshot_write_int(FILE *fp, const char *key, int value)
{
    if (value < 0) return fprintf(fp, "%s=skip\n", key) < 0 ? -1 : 0;
    return fprintf(fp, "%s=%d\n", key, value) < 0 ? -1 : 0;
}

static int cctl_snapshot_write_clock(FILE *fp, const char *key, int known, int is_set,
                                  int min, int max)
{
    if (known < 0) return fprintf(fp, "%s=skip\n", key) < 0 ? -1 : 0;
    if (!is_set) return fprintf(fp, "%s=none\n", key) < 0 ? -1 : 0;
    return fprintf(fp, "%s=%d,%d\n", key, min, max) < 0 ? -1 : 0;
}

static int cctl_snapshot_write_file(int number, const struct cctl_snapshot_data *st)
{
    if (cctl_snapshot_ensure_dirs() != 0) return -1;

    char final_path[128], temp_path[160];
    cctl_snapshot_path(number, final_path, sizeof(final_path));
    snprintf(temp_path, sizeof(temp_path), "%s/.%d.tmp.XXXXXX", CCTL_SNAPSHOT_DIR, number);
    int fd = mkstemp(temp_path);
    if (fd < 0) return -1;
    if (fchmod(fd, 0600) != 0) {
        int e = errno; close(fd); unlink(temp_path); errno = e; return -1;
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        int e = errno; close(fd); unlink(temp_path); errno = e; return -1;
    }

    int failed = fprintf(fp, "# cctl saved snapshot format 2\nversion=2\n") < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "ec_profile_code", st->ec_profile_code) < 0;
    if (!failed) failed = cctl_snapshot_write_clock(fp, "nvidia_clock", st->core_clock_known,
                                                  st->core_clock_set, st->core_clock_min,
                                                  st->core_clock_max) < 0;
    if (!failed) failed = cctl_snapshot_write_clock(fp, "nvidia_memclock", st->mem_clock_known,
                                                  st->mem_clock_set, st->mem_clock_min,
                                                  st->mem_clock_max) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "turbo", st->turbo) < 0;
    if (!failed) failed = fprintf(fp, "governor=%s\nepp=%s\n", st->governor, st->epp) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "pl1_watts", st->pl1) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "pl2_watts", st->pl2) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "keyboard_r", st->kbd_r) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "keyboard_g", st->kbd_g) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "keyboard_b", st->kbd_b) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "keyboard_brightness_raw", st->kbd_brightness) < 0;
    if (!failed) failed = fprintf(fp, "keyboard_effect=%s\n", st->kbd_effect) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "fn_lock", st->fn_lock) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "webcam", st->webcam) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "microphone", st->microphone) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "battery_start", st->battery_start) < 0;
    if (!failed) failed = cctl_snapshot_write_int(fp, "battery_stop", st->battery_stop) < 0;
    if (!failed) failed = fprintf(fp, "display_rate=%s\n", st->display_rate) < 0;
    if (!failed) {
        const char *scale = st->display_scale_known < 0 ? "skip" :
                            st->display_scale_set ? st->display_scale : "none";
        failed = fprintf(fp, "display_scale=%s\n", scale) < 0;
    }

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

static int cctl_snapshot_parse_field(struct cctl_snapshot_data *st, const char *key,
                                  const char *value, unsigned long long *seen)
{
    int field = -1, rc = -1;
#define SNAPSHOT_FIELD(name, index, expression) \
    if (strcmp(key, name) == 0) { field = index; rc = (expression); }
    SNAPSHOT_FIELD("version", SNAPSHOT_VERSION,
                strcmp(value, "2") == 0 ? 0 : -1)
    else SNAPSHOT_FIELD("ec_profile_code", SNAPSHOT_EC, cctl_snapshot_parse_int(value, 0, 3, &st->ec_profile_code))
    else SNAPSHOT_FIELD("nvidia_clock", SNAPSHOT_GPU_CLOCK, cctl_snapshot_parse_clock(value, &st->core_clock_known, &st->core_clock_set, &st->core_clock_min, &st->core_clock_max))
    else SNAPSHOT_FIELD("nvidia_memclock", SNAPSHOT_MEM_CLOCK, cctl_snapshot_parse_clock(value, &st->mem_clock_known, &st->mem_clock_set, &st->mem_clock_min, &st->mem_clock_max))
    else SNAPSHOT_FIELD("turbo", SNAPSHOT_TURBO, cctl_snapshot_parse_int(value, 0, 1, &st->turbo))
    else if (strcmp(key, "governor") == 0) {
        field = SNAPSHOT_GOVERNOR;
        rc = (strcmp(value, "skip") == 0 || strcmp(value, "powersave") == 0 ||
              strcmp(value, "performance") == 0)
             ? cctl_snapshot_copy_token(st->governor, sizeof(st->governor), value) : -1;
    }
    else if (strcmp(key, "epp") == 0) {
        field = SNAPSHOT_EPP;
        rc = (strcmp(value, "skip") == 0 || strcmp(value, "performance") == 0 ||
              strcmp(value, "balance_performance") == 0 || strcmp(value, "balance_power") == 0 ||
              strcmp(value, "power") == 0)
             ? cctl_snapshot_copy_token(st->epp, sizeof(st->epp), value) : -1;
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
#undef SNAPSHOT_FIELD
    if (field < 0 || rc != 0 || (*seen & (1ULL << field))) return -1;
    *seen |= 1ULL << field;
    return 0;
}

static int cctl_snapshot_read_file(int number, struct cctl_snapshot_data *st)
{
    char path[128], line[256];
    cctl_snapshot_path(number, path, sizeof(path));
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
            file_version = strcmp(eq, "2") == 0 ? 2 : 0;
        if (cctl_snapshot_parse_field(st, line, eq, &seen) != 0) { failed = 1; break; }
    }
    unsigned long long required = (1ULL << SNAPSHOT_FIELD_COUNT) - 1;
    if (ferror(fp) || file_version == 0 || seen != required) failed = 1;
    fclose(fp);
    return failed ? -1 : 0;
}

static void cctl_snapshot_preview_restore(int number,
                                          const struct cctl_snapshot_data *st)
{
    char value[96];
    printf("Dry run: snapshot %d would restore these settings; nothing was changed.\n",
           number);
#define SNAPSHOT_PREVIEW(label, expression) do { \
    snprintf(value, sizeof(value), "%s", (expression)); \
    printf("  %-22s %s\n", label, value); \
} while (0)

    if (st->ec_profile_code >= 0) {
        snprintf(value, sizeof(value), "%s (%d)", gpu_profile_name(st->ec_profile_code),
                 st->ec_profile_code);
        printf("  %-22s %s\n", "EC profile code", value);
    } else SNAPSHOT_PREVIEW("EC profile code", "skip (not saved)");
    if (st->turbo >= 0) SNAPSHOT_PREVIEW("Turbo", st->turbo ? "on" : "off");
    else SNAPSHOT_PREVIEW("Turbo", "skip (not saved)");
    SNAPSHOT_PREVIEW("Governor", strcmp(st->governor, "skip") == 0 ? "skip (not saved)" : st->governor);
    SNAPSHOT_PREVIEW("EPP", strcmp(st->epp, "skip") == 0 ? "skip (not saved)" : st->epp);
    if (st->pl1 >= 0) {
        snprintf(value, sizeof(value), "%dW", st->pl1);
        printf("  %-22s %s\n", "PL1", value);
    } else SNAPSHOT_PREVIEW("PL1", "skip (not saved)");
    if (st->pl2 >= 0) {
        snprintf(value, sizeof(value), "%dW", st->pl2);
        printf("  %-22s %s\n", "PL2", value);
    } else SNAPSHOT_PREVIEW("PL2", "skip (not saved)");

    if (st->core_clock_known < 0) SNAPSHOT_PREVIEW("NVIDIA core clock", "skip (not saved)");
    else if (st->core_clock_set) {
        snprintf(value, sizeof(value), "%d-%d MHz", st->core_clock_min, st->core_clock_max);
        printf("  %-22s %s\n", "NVIDIA core clock", value);
    } else SNAPSHOT_PREVIEW("NVIDIA core clock", "reset/unlocked");
    if (st->mem_clock_known < 0) SNAPSHOT_PREVIEW("NVIDIA memory clock", "skip (not saved)");
    else if (st->mem_clock_set) {
        snprintf(value, sizeof(value), "%d-%d MHz", st->mem_clock_min, st->mem_clock_max);
        printf("  %-22s %s\n", "NVIDIA memory clock", value);
    } else SNAPSHOT_PREVIEW("NVIDIA memory clock", "reset/unlocked");

    if (st->kbd_r >= 0 && st->kbd_g >= 0 && st->kbd_b >= 0) {
        snprintf(value, sizeof(value), "RGB(%d, %d, %d)", st->kbd_r, st->kbd_g, st->kbd_b);
        printf("  %-22s %s\n", "Keyboard color", value);
    } else SNAPSHOT_PREVIEW("Keyboard color", "skip (not saved)");
    if (st->kbd_brightness >= 0) {
        snprintf(value, sizeof(value), "%d%%", (st->kbd_brightness * 100 + 127) / 255);
        printf("  %-22s %s\n", "Keyboard brightness", value);
    } else SNAPSHOT_PREVIEW("Keyboard brightness", "skip (not saved)");
    SNAPSHOT_PREVIEW("Keyboard effect", strcmp(st->kbd_effect, "skip") == 0 ? "skip (not saved)" : st->kbd_effect);
    if (st->fn_lock >= 0) SNAPSHOT_PREVIEW("Fn Lock", st->fn_lock ? "on" : "off");
    else SNAPSHOT_PREVIEW("Fn Lock", "skip (not saved)");
    if (st->webcam >= 0) SNAPSHOT_PREVIEW("Webcam", st->webcam ? "on" : "off");
    else SNAPSHOT_PREVIEW("Webcam", "skip (not saved)");
    if (st->microphone >= 0) SNAPSHOT_PREVIEW("Microphone", st->microphone ? "on" : "off");
    else SNAPSHOT_PREVIEW("Microphone", "skip (not saved)");

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
        printf("  %-22s start %s, stop %s\n", "Battery thresholds", start, stop);
    } else SNAPSHOT_PREVIEW("Battery thresholds", "skip (not saved)");
    SNAPSHOT_PREVIEW("Refresh rate", strcmp(st->display_rate, "skip") == 0 ? "skip (not saved)" : st->display_rate);
    if (st->display_scale_known < 0) SNAPSHOT_PREVIEW("Display scale", "skip (not saved)");
    else SNAPSHOT_PREVIEW("Display scale", st->display_scale_set ? st->display_scale : "native (reset)");
#undef SNAPSHOT_PREVIEW
    puts("Fan settings are not part of snapshots.");
}

static const char *cctl_snapshot_value(int value, char *buf, size_t size)
{
    if (value < 0) return "--";
    snprintf(buf, size, "%d", value);
    return buf;
}

static void cctl_snapshot_view(int number, const struct cctl_snapshot_data *st)
{
    int prev = g_stream_mode;
    g_stream_mode = STREAM_MODE_LINE;
    g_stream_line = 0;
    char buf[64];
    printf("Snapshot %d:\n", number);
    if (st->ec_profile_code < 0)
        printf("  EC profile code: --\n");
    else
        printf("  EC profile code: %s (%d)\n", gpu_profile_name(st->ec_profile_code), st->ec_profile_code);
    if (st->core_clock_known < 0) printf("  NVIDIA core clock: --\n");
    else if (!st->core_clock_set) printf("  NVIDIA core clock: reset/unlocked\n");
    else printf("  NVIDIA core clock: %d–%d MHz\n", st->core_clock_min, st->core_clock_max);
    if (st->mem_clock_known < 0) printf("  NVIDIA memory clock: --\n");
    else if (!st->mem_clock_set) printf("  NVIDIA memory clock: reset/unlocked\n");
    else printf("  NVIDIA memory clock: %d–%d MHz\n", st->mem_clock_min, st->mem_clock_max);
    printf("  Turbo: %s\n", st->turbo < 0 ? "--" : (st->turbo ? "ON" : "OFF"));
    printf("  Governor: %s\n", strcmp(st->governor, "skip") == 0 ? "--" : st->governor);
    printf("  EPP: %s\n", strcmp(st->epp, "skip") == 0 ? "--" : st->epp);
    if (st->pl1 < 0) printf("  PL1: --\n");
    else printf("  PL1: %dW\n", st->pl1);
    if (st->pl2 < 0) printf("  PL2: --\n");
    else printf("  PL2: %dW\n", st->pl2);
    if (st->kbd_r < 0) printf("  Keyboard color: --\n");
    else printf("  Keyboard color: RGB(%d, %d, %d)\n", st->kbd_r, st->kbd_g, st->kbd_b);
    if (st->kbd_brightness < 0) printf("  Keyboard brightness: --\n");
    else printf("  Keyboard brightness: %d%%\n", (st->kbd_brightness * 100 + 127) / 255);
    printf("  Keyboard effect: %s\n", strcmp(st->kbd_effect, "skip") == 0 ? "--" : st->kbd_effect);
    printf("  Fn Lock: %s\n", st->fn_lock < 0 ? "--" : (st->fn_lock ? "ON" : "OFF"));
    printf("  Webcam: %s\n", st->webcam < 0 ? "--" : (st->webcam ? "ON" : "OFF"));
    printf("  Microphone: %s\n", st->microphone < 0 ? "--" : (st->microphone ? "ON" : "OFF"));
    printf("  Battery thresholds: %s%% → %s%%\n",
           cctl_snapshot_value(st->battery_start, buf, sizeof(buf)),
           cctl_snapshot_value(st->battery_stop, buf + 32, sizeof(buf) - 32));
    printf("  Refresh rate: %s%s\n", strcmp(st->display_rate, "skip") == 0 ? "--" : st->display_rate,
           strcmp(st->display_rate, "skip") == 0 ? "" : "Hz");
    if (st->display_scale_known < 0) printf("  Display scale: --\n");
    else if (st->display_scale_set) printf("  Display scale: %s\n", st->display_scale);
    else printf("  Display scale: native (1x1)\n");
    g_stream_mode = prev;
}

static int cctl_snapshot_save(int number, unsigned long long mask)
{
    char existing[128];
    int found = cctl_snapshot_find_existing(number, existing, sizeof(existing));
    if (found < 0)
        return fail("Cannot inspect existing snapshot %d", number);
    if (found) {
        fprintf(stderr, "Warning: snapshot %d already exists. Replace it? [y/N] ", number);
        fflush(stderr);
        if (!isatty(STDIN_FILENO)) {
            fprintf(stderr, "\nCancelled. Run this command in a terminal to confirm replacement.\n");
            return 1;
        }
        char answer[32];
        if (!fgets(answer, sizeof(answer), stdin)) {
            puts("Cancelled; the existing snapshot was kept.");
            return 1;
        }
        answer[strcspn(answer, "\r\n")] = '\0';
        if (strcasecmp(answer, "y") != 0 && strcasecmp(answer, "yes") != 0) {
            puts("Cancelled; the existing snapshot was kept.");
            return 1;
        }
    }

    struct cctl_snapshot_data st;
    cctl_snapshot_read_current(&st);
    cctl_snapshot_apply_mask(&st, mask);
    if (cctl_snapshot_write_file(number, &st) != 0) {
        perror("Error: could not save snapshot");
        return 1;
    }
    stream_printf(STREAM_DELAY_US, "Saved snapshot %d to %s/%d.conf\n", number, CCTL_SNAPSHOT_DIR, number);
    return 0;
}

static int cctl_snapshot_delete(int number)
{
    char path[128];
    int exists = 0;
    cctl_snapshot_path(number, path, sizeof(path));
    if (cctl_snapshot_validate_path(path, &exists) != 0)
        return fail("Cannot safely delete snapshot %d", number);
    if (!exists)
        return fail("Snapshot %d does not exist", number);
    if (unlink(path) != 0) {
        perror("Error: could not delete snapshot");
        return 1;
    }
    stream_printf(STREAM_DELAY_US, "Deleted snapshot %d.\n", number);
    return 0;
}

static void cctl_snapshot_divider(void)
{
    printf("%s---\n%s", C_DIM, C_RST);
}

static const char *snapshot_unchanged_tag(void)
{
    return use_color ? "  \033[36m(already set)\033[0m" : "  (already set)";
}

static int cctl_snapshot_apply_step(const char *name, int rc, int *errors)
{
    if (rc != 0) {
        fprintf(stderr, "  %sFailed to restore %s; continuing.%s\n", C_RED, name, C_RST);
        (*errors)++;
        return -1;
    }
    return 0;
}

static int cctl_snapshot_restore(int number, const struct cctl_snapshot_data *st)
{
    int prev = g_stream_mode;
    g_stream_mode = STREAM_MODE_LINE;
    g_stream_line = 0;
    g_snapshot_restoring = 1;
    int errors = 0;
    printf("Restoring snapshot %d one setting at a time...\n", number);

    /* 1. EC profile code: always apply if saved (hardware EC state cannot be queried) */
    cctl_snapshot_divider();
    if (st->ec_profile_code >= 0) {
        if (cctl_snapshot_apply_step("EC profile code", set_gpu_profile(st->ec_profile_code), &errors) == 0) {
            char label[32];
            snprintf(label, sizeof(label), "snapshot-%d", number);
            if (mode_write(label, "restore", st->ec_profile_code) < 0) {
                fprintf(stderr, "  %sWarning: could not record restored EC code in %s%s\n",
                        C_YLW, MODE_FILE, C_RST);
                errors++;
            }
        }
    } else {
        printf("  %sSkipping EC profile code (not saved).%s\n", C_DIM, C_RST);
    }

    /* 2. Turbo: query live state; skip if already matching */
    cctl_snapshot_divider();
    if (st->turbo >= 0) {
        long cur_turbo_raw = read_sysfs_long(TURBO_PATH, -1);
        int cur_turbo = (cur_turbo_raw == 0) ? 1 : ((cur_turbo_raw == 1) ? 0 : -1);
        if (cur_turbo >= 0 && cur_turbo == st->turbo) {
            printf("  Turbo boost: %s%s\n", st->turbo ? "ON" : "OFF", snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("turbo", set_turbo(st->turbo), &errors);
        }
    } else {
        printf("  %sSkipping turbo (not saved).%s\n", C_DIM, C_RST);
    }

    /* 3. Governor & 4. EPP: query live state; skip if already matching */
    struct cpu_profile_state cpu_profile;
    read_cpu_profile(&cpu_profile);

    cctl_snapshot_divider();
    if (strcmp(st->governor, "skip") != 0) {
        if (cpu_profile.have_governor && strcmp(cpu_profile.governor, st->governor) == 0) {
            printf("  Governor: %s%s\n", st->governor, snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("governor", set_governor(st->governor), &errors);
        }
    } else {
        printf("  %sSkipping governor (not saved).%s\n", C_DIM, C_RST);
    }

    cctl_snapshot_divider();
    if (strcmp(st->epp, "skip") != 0) {
        if (cpu_profile.have_epp && strcmp(cpu_profile.epp, st->epp) == 0) {
            printf("  EPP: %s%s\n", st->epp, snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("EPP", set_epp(st->epp), &errors);
        }
    } else {
        printf("  %sSkipping EPP (not saved).%s\n", C_DIM, C_RST);
    }

    /* 5. RAPL: query live state after EC profile set; skip if already matching */
    int pl1_cap = st->ec_profile_code == 2 ? RAPL_PL1_MAX_WATTS_MAX :
                  st->ec_profile_code >= 0 ? RAPL_PL1_MAX_WATTS : rapl_pl1_ceiling();
    int cur_pl1 = -1, cur_pl2 = -1;
    read_rapl_current(&cur_pl1, &cur_pl2);

    int need_pl1 = (st->pl1 >= 0 && st->pl1 != cur_pl1);
    int need_pl2 = (st->pl2 >= 0 && st->pl2 != cur_pl2);

    cctl_snapshot_divider();
    if (st->pl1 < 0 && st->pl2 < 0) {
        printf("  %sSkipping RAPL (not saved).%s\n", C_DIM, C_RST);
    } else if (!need_pl1 && !need_pl2) {
        if (st->pl1 >= 0 && st->pl2 >= 0)
            printf("  RAPL: PL1 %dW, PL2 %dW%s\n", st->pl1, st->pl2, snapshot_unchanged_tag());
        else if (st->pl1 >= 0)
            printf("  RAPL: PL1 %dW%s\n", st->pl1, snapshot_unchanged_tag());
        else
            printf("  RAPL: PL2 %dW%s\n", st->pl2, snapshot_unchanged_tag());
    } else {
        int apply_pl1 = need_pl1 ? st->pl1 : 0;
        int apply_pl2 = need_pl2 ? st->pl2 : 0;
        cctl_snapshot_apply_step("RAPL", set_rapl_limits(apply_pl1, apply_pl2, pl1_cap), &errors);
    }

    /* 6. NVIDIA GPU clocks: always apply if saved */
    cctl_snapshot_divider();
    if (st->core_clock_known < 0) {
        printf("  %sSkipping NVIDIA core clock (not saved).%s\n", C_DIM, C_RST);
    } else if (st->core_clock_set) {
        printf("  NVIDIA core clock: %d–%d MHz%s\n",
               st->core_clock_min, st->core_clock_max, snapshot_applied_tag());
        cctl_snapshot_apply_step("NVIDIA core clock",
            nvidia_clock_apply("-lgc", st->core_clock_min, st->core_clock_max), &errors);
    } else {
        printf("  NVIDIA core clock: reset/unlocked%s\n", snapshot_applied_tag());
        cctl_snapshot_apply_step("NVIDIA core clock reset",
            nvidia_clock_reset_flag("-rgc"), &errors);
    }

    cctl_snapshot_divider();
    if (st->mem_clock_known < 0) {
        printf("  %sSkipping NVIDIA memory clock (not saved).%s\n", C_DIM, C_RST);
    } else if (st->mem_clock_set) {
        printf("  NVIDIA memory clock: %d–%d MHz%s\n",
               st->mem_clock_min, st->mem_clock_max, snapshot_applied_tag());
        cctl_snapshot_apply_step("NVIDIA memory clock",
            nvidia_clock_apply("-lmc", st->mem_clock_min, st->mem_clock_max), &errors);
    } else {
        printf("  NVIDIA memory clock: reset/unlocked%s\n", snapshot_applied_tag());
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
                printf("  Keyboard color: RGB(%d, %d, %d)%s\n",
                       st->kbd_r, st->kbd_g, st->kbd_b, snapshot_unchanged_tag());
            if (st->kbd_brightness >= 0)
                printf("  Keyboard brightness: %d%%%s\n",
                       (st->kbd_brightness * 100 + 127) / 255, snapshot_unchanged_tag());
            if (!preserve_effect)
                printf("  Keyboard effect: %s%s\n", st->kbd_effect, snapshot_unchanged_tag());
            else
                printf("  %sSkipping keyboard effect (not saved).%s\n", C_DIM, C_RST);
        } else {
            if (effect_was_running)
                cctl_snapshot_apply_step("current keyboard effect stop", kbe_stop(1), &errors);

            if (st->kbd_r >= 0 && st->kbd_g >= 0 && st->kbd_b >= 0) {
                if (cur_r == st->kbd_r && cur_g == st->kbd_g && cur_b == st->kbd_b) {
                    printf("  Keyboard color: RGB(%d, %d, %d)%s\n",
                           st->kbd_r, st->kbd_g, st->kbd_b, snapshot_unchanged_tag());
                } else {
                    printf("  Keyboard color: RGB(%d, %d, %d)%s\n",
                           st->kbd_r, st->kbd_g, st->kbd_b, snapshot_applied_tag());
                    cctl_snapshot_apply_step("keyboard color",
                        kbd_write_color(st->kbd_r, st->kbd_g, st->kbd_b), &errors);
                }
            } else {
                printf("  %sSkipping keyboard color (not saved).%s\n", C_DIM, C_RST);
            }

            if (st->kbd_brightness >= 0) {
                if (cur_bri == st->kbd_brightness) {
                    printf("  Keyboard brightness: %d%%%s\n",
                           (st->kbd_brightness * 100 + 127) / 255, snapshot_unchanged_tag());
                } else {
                    printf("  Keyboard brightness: %d%%%s\n",
                           (st->kbd_brightness * 100 + 127) / 255, snapshot_applied_tag());
                    cctl_snapshot_apply_step("keyboard brightness",
                        write_sysfs_int(KBD_PATH "/brightness", st->kbd_brightness), &errors);
                }
            } else {
                printf("  %sSkipping keyboard brightness (not saved).%s\n", C_DIM, C_RST);
            }

            if (strcmp(st->kbd_effect, "skip") != 0 && strcmp(st->kbd_effect, "none") != 0) {
                cctl_snapshot_apply_step("keyboard effect", kbe_start(st->kbd_effect), &errors);
            } else if (preserve_effect && effect_was_running) {
                cctl_snapshot_apply_step("keyboard effect", kbe_start(running_effect), &errors);
            } else if (strcmp(st->kbd_effect, "none") == 0) {
                printf("  Keyboard effect: none%s\n", snapshot_applied_tag());
            } else {
                printf("  %sSkipping keyboard effect (not saved).%s\n", C_DIM, C_RST);
            }
        }
    } else {
        printf("  %sSkipping keyboard settings (not saved).%s\n", C_DIM, C_RST);
    }

    /* 8. Fn Lock: query live state; skip if already matching */
    cctl_snapshot_divider();
    if (st->fn_lock >= 0) {
        int cur_fnlock = get_fnlock();
        if (cur_fnlock >= 0 && cur_fnlock == st->fn_lock) {
            printf("  Fn Lock: %s%s\n", st->fn_lock ? "ON" : "OFF", snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("Fn Lock", set_fnlock(st->fn_lock), &errors);
        }
    } else {
        printf("  %sSkipping Fn Lock (not saved).%s\n", C_DIM, C_RST);
    }

    /* 9. Webcam: query live state; skip if already matching */
    cctl_snapshot_divider();
    if (st->webcam >= 0) {
        int cur_webcam = is_webcam_enabled();
        if (cur_webcam >= 0 && cur_webcam == st->webcam) {
            printf("  Webcam: %s%s\n", st->webcam ? "ON" : "OFF", snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("webcam", webcam_set(st->webcam), &errors);
        }
    } else {
        printf("  %sSkipping webcam (not saved).%s\n", C_DIM, C_RST);
    }

    /* 10. Microphone: query live state; skip if already matching */
    cctl_snapshot_divider();
    if (st->microphone >= 0) {
        int cur_mic = mic_is_enabled();
        if (cur_mic >= 0 && cur_mic == st->microphone) {
            printf("  Laptop internal microphone: %s%s\n", st->microphone ? "ON" : "OFF", snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("microphone", mic_set(st->microphone), &errors);
        }
    } else {
        printf("  %sSkipping microphone (not saved).%s\n", C_DIM, C_RST);
    }

    /* 11. Battery thresholds: query live state; skip if already matching */
    cctl_snapshot_divider();
    if (st->battery_start >= 0 || st->battery_stop >= 0) {
        int cur_start = bat_read_start();
        int cur_stop = bat_read_end();
        int target_start = st->battery_start >= 0 ? st->battery_start : cur_start;
        int target_stop = st->battery_stop >= 0 ? st->battery_stop : cur_stop;
        if (cur_start > 0 && cur_stop > 0 && target_start == cur_start && target_stop == cur_stop) {
            printf("  Set charge thresholds: start %d%% → stop %d%%%s\n",
                   cur_start, cur_stop, snapshot_unchanged_tag());
        } else if (target_start > 0 && target_stop > 0) {
            cctl_snapshot_apply_step("battery thresholds", bat_set(target_start, target_stop), &errors);
        } else {
            fprintf(stderr, "  %sFailed to restore battery thresholds; current values are unavailable.%s\n",
                    C_RED, C_RST);
            errors++;
        }
    } else {
        printf("  %sSkipping battery thresholds (not saved).%s\n", C_DIM, C_RST);
    }

    /* 12. Display refresh rate: query live state; skip if already matching (prevents X11 mode reset / flicker) */
    cctl_snapshot_divider();
    if (strcmp(st->display_rate, "skip") != 0) {
        struct display_info disp;
        int have_disp = (has_display_support() && query_display_info(&disp) == 0 && disp.current_rate[0]);
        if (have_disp && strcmp(disp.current_rate, st->display_rate) == 0) {
            printf("  Refresh rate: %sHz%s\n", st->display_rate, snapshot_unchanged_tag());
        } else {
            cctl_snapshot_apply_step("display refresh rate", rr_set(st->display_rate), &errors);
        }
    } else {
        printf("  %sSkipping display refresh rate (not saved).%s\n", C_DIM, C_RST);
    }

    /* 13. Display scale: query live metadata; skip if already matching */
    cctl_snapshot_divider();
    if (st->display_scale_known < 0) {
        printf("  %sSkipping display scale (not saved).%s\n", C_DIM, C_RST);
    } else {
        struct mode_runtime_data mode;
        int have_mode = (mode_runtime_read(&mode) == 0);
        if (st->display_scale_set) {
            if (have_mode && mode.display_scale_known && mode.display_scale_set &&
                strcmp(mode.display_scale, st->display_scale) == 0) {
                printf("  Display scale: %s%s\n", st->display_scale, snapshot_unchanged_tag());
            } else {
                cctl_snapshot_apply_step("display scale", scale_set(st->display_scale), &errors);
            }
        } else {
            if (have_mode && mode.display_scale_known && !mode.display_scale_set) {
                printf("  Scale: native (1x1)%s\n", snapshot_unchanged_tag());
            } else {
                cctl_snapshot_apply_step("display scale reset", scale_reset(), &errors);
            }
        }
    }

    cctl_snapshot_divider();
    g_snapshot_restoring = 0;

    if (errors) {
        fprintf(stderr, "%sSnapshot %d restored with %d error(s).%s\n",
                C_RED, number, errors, C_RST);
        g_stream_mode = prev;
        return 1;
    }
    printf("%sSnapshot %d restored.%s\n", C_GRN, number, C_RST);
    g_stream_mode = prev;
    return 0;
}

static int cmd_snapshot(int argc, char **argv)
{
    if (argc < 3) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        printf("%sUsage:%s %scctl snapshot <save|restore|view|delete> <number> [--mask fields]%s\n",
               C_BLD, C_RST, C_CYN_BLD, C_RST);
        printf("       %scctl snapshot restore <number> [--mask fields] [--dry-run]%s\n",
               C_CYN_BLD, C_RST);
        printf("       %scctl snapshot list%s\n\n", C_CYN_BLD, C_RST);
        printf("%sActions:%s\n", C_YLW, C_RST);
        printf("  list              Show saved snapshot numbers on one line\n");
        printf("  save <number> [--mask fields]     Save current settings, omitting masked fields\n");
        printf("  restore <number> [--mask fields] [--dry-run]  Apply; preview with --dry-run\n");
        printf("  view <number>     Show a saved snapshot\n");
        printf("  delete <number>   Delete a saved snapshot\n");
        printf("\n%sMask fields:%s\n", C_YLW, C_RST);
        printf("  nvidia_clock,nvidia_memclock,ec_code,turbo,gov,epp,pl1,pl2,\n");
        printf("              webcam,mic,rr,scale,bat,kbe,kbc,kbb,fn_lock\n");
        printf("\n%sNot included:%s fan settings are not saved or restored by snapshots.\n",
               C_YLW, C_RST);
        g_stream_mode = prev;
        return 0;
    }
    if (strcmp(argv[2], "list") == 0) {
        if (argc != 3) return fail("Usage: cctl snapshot list");
        require_root(argc, argv);
        return cctl_snapshot_list();
    }
    if (argc < 4 || argc > 7)
        return fail("Usage: cctl snapshot <save|restore|view|delete> <number> [--mask fields]; restore also accepts --dry-run");
    int number;
    if (cctl_snapshot_number(argv[3], &number) != 0)
        return fail("Snapshot number must be an integer from 0 to 999999999");

    unsigned long long mask = 0;
    int have_mask = 0, snapshot_dry_run = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--mask") == 0) {
            if (have_mask || i + 1 >= argc ||
                cctl_snapshot_parse_mask(argv[++i], &mask) != 0)
                return fail("Invalid mask list (see 'cctl snapshot' for available fields)");
            have_mask = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            if (snapshot_dry_run) return fail("--dry-run was specified more than once");
            snapshot_dry_run = 1;
        } else {
            return fail("Unknown snapshot option '%s'", argv[i]);
        }
    }
    if (mask && strcmp(argv[2], "save") != 0 && strcmp(argv[2], "restore") != 0)
        return fail("--mask is supported only for snapshot save and restore");
    if (snapshot_dry_run && strcmp(argv[2], "restore") != 0)
        return fail("--dry-run is supported only for snapshot restore");

    if (strcmp(argv[2], "save") == 0) {
        require_root(argc, argv);
        return cctl_snapshot_save(number, mask);
    }
    if (strcmp(argv[2], "delete") == 0) {
        require_root(argc, argv);
        return cctl_snapshot_delete(number);
    }
    if (strcmp(argv[2], "view") == 0 || strcmp(argv[2], "restore") == 0) {
        require_root(argc, argv);
        struct cctl_snapshot_data st;
        if (cctl_snapshot_read_file(number, &st) != 0)
            return fail("Cannot read snapshot %d (missing or invalid file)", number);
        if (strcmp(argv[2], "view") == 0) {
            cctl_snapshot_view(number, &st);
            return 0;
        }
        cctl_snapshot_apply_mask(&st, mask);
        if (snapshot_dry_run) {
            cctl_snapshot_preview_restore(number, &st);
            return 0;
        }
        return cctl_snapshot_restore(number, &st);
    }
    return fail("Unknown snapshot action '%s' (use list, save, restore, view, or delete)", argv[2]);
}
