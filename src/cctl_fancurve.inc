/* ========================================================================
 * FAN CURVES
 * ========================================================================
 * A curve profile is a JSON file under /var/lib/cctl/fancurves.  The
 * `fancurve start` command starts a small detached worker (the same
 * fork/setsid pattern as kbe) that reads both fan-sensor temperatures from
 * tuxedo_io's R_CL_FANINFO1/R_CL_FANINFO2 (third byte), interpolates the
 * configured points, and writes both fans through W_CL_FANSPEED.
 *
 * Safety:
 *   - above 95 C the worker hands both fans back to EC automatic control;
 *   - it reclaims the curve below 90 C;
 *   - stop (alias off) and normal daemon termination restore EC auto.
 * ======================================================================== */

#ifndef FANCURVE_DIR
#define FANCURVE_DIR "/var/lib/cctl/fancurves"
#endif
#ifndef FANCURVE_STATE_PATH
#define FANCURVE_STATE_PATH "/run/cctl_curve.state"
#endif
#ifndef FANCURVE_LOCK_PATH
#define FANCURVE_LOCK_PATH  "/run/cctl_curve.lock"
#endif
#ifndef FANCURVE_MODULE_DIR
#define FANCURVE_MODULE_DIR "/sys/module"
#endif
#ifndef FANCURVE_LOG_FIFO
#define FANCURVE_LOG_FIFO "/run/cctl_curve.fifo"
#endif
#ifndef FANCURVE_MODPROBE
#define FANCURVE_MODPROBE "modprobe"
#endif

#define FANCURVE_IOCTL_RETRIES     3
#define FANCURVE_IOCTL_RETRY_US    100000
#define FANCURVE_START_TIMEOUT_MS  7000
#define FANCURVE_START_POLL_MS     20
#define FANCURVE_SLEEP_CLOCK_THRESHOLD_NS 250000000LL
#define FANCURVE_MIN_POINTS        2
#define FANCURVE_MAX_POINTS        16
#define FANCURVE_MIN_FAN           25
#define FANCURVE_MAX_FAN           100
#define FANCURVE_FAN_AUTO          (-1)
#define FANCURVE_MAX_TEMP          95
#define FANCURVE_HOT_TEMP          95
#define FANCURVE_RESUME_TEMP       90
#define FANCURVE_SLEEP_RESUME_TEMP 85
#define FANCURVE_DEFAULT_HYSTERESIS 3
#define FANCURVE_DEFAULT_INTERVAL  2000
#define FANCURVE_MIN_INTERVAL      250
#define FANCURVE_MAX_INTERVAL      60000
#define FANCURVE_MAX_HYSTERESIS    20
#define FANCURVE_NAME_MAX          120
#define FANCURVE_FILE_MAX          65536

struct fancurve_point {
    int temp;
    int fan;
};

struct fancurve_profile {
    char name[FANCURVE_NAME_MAX + 1];
    int hysteresis;
    int interval_ms;
    int npoints;
    struct fancurve_point points[FANCURVE_MAX_POINTS];
};

struct fancurve_state {
    pid_t pid;
    long starttime;
    time_t epoch;
    int sleep_monitor;
    char name[FANCURVE_NAME_MAX + 1];
    int profile_known;
    struct fancurve_profile profile;
};

/* ---------------------------------------------------------------- names */

static int fancurve_name_valid(const char *name)
{
    size_t len;
    if (!name || !*name) return 0;
    len = strlen(name);
    if (len > FANCURVE_NAME_MAX) return 0;
    if (name[0] == '.') return 0;

    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p == '/' || iscntrl(*p)) return 0;
    }
    return 1;
}

static int fancurve_profile_path(const char *name, char *out, size_t out_sz)
{
    if (!fancurve_name_valid(name)) return -1;
    int n = snprintf(out, out_sz, "%s/%s.json", FANCURVE_DIR, name);
    return (n >= 0 && (size_t)n < out_sz) ? 0 : -1;
}

static int fancurve_profile_exists(const char *name)
{
    char path[PATH_MAX];
    struct stat st;
    if (fancurve_profile_path(name, path, sizeof(path)) < 0) return 0;
    if (lstat(path, &st) == 0) return 1;
    return errno != ENOENT;
}

/* ------------------------------------------------------------ JSON parse */

struct fancurve_json {
    const char *p;
    char err[256];
};

static void fc_json_error(struct fancurve_json *j, const char *fmt, ...)
{
    if (j->err[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->err, sizeof(j->err), fmt, ap);
    va_end(ap);
}

static void fc_json_ws(struct fancurve_json *j)
{
    for (;;) {
        while (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')
            j->p++;
        if (*j->p != '#')
            break;
        while (*j->p && *j->p != '\n')
            j->p++;
    }
}

static int fc_json_eat(struct fancurve_json *j, char c)
{
    fc_json_ws(j);
    if (*j->p != c) {
        fc_json_error(j, "expected '%c'", c);
        return -1;
    }
    j->p++;
    return 0;
}

static int fc_json_string(struct fancurve_json *j, char *out, size_t out_sz)
{
    size_t n = 0;
    if (!out || out_sz == 0) return -1;

    fc_json_ws(j);
    if (*j->p != '"') {
        fc_json_error(j, "expected a string");
        return -1;
    }
    j->p++;

    while (*j->p && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') {
            char esc = *j->p++;
            switch (esc) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u':
                    fc_json_error(j, "\\u escapes are not supported in fan-curve names");
                    return -1;
                default:
                    fc_json_error(j, "invalid escape sequence in string");
                    return -1;
            }
        } else if (c < 0x20) {
            fc_json_error(j, "control character in string");
            return -1;
        }
        if (n + 1 >= out_sz) {
            fc_json_error(j, "string is too long");
            return -1;
        }
        out[n++] = (char)c;
    }

    if (*j->p != '"') {
        fc_json_error(j, "unterminated string");
        return -1;
    }
    j->p++;
    out[n] = '\0';
    return 0;
}

/* Parse an integer or a quoted integer.  Quotes let the generated template
 * use "--" placeholders that users replace in place. */
static int fc_json_integer(struct fancurve_json *j, int *out, int *is_null,
                           const char *field)
{
    int negative = 0;
    long value = 0;

    fc_json_ws(j);
    if (*j->p == '"') {
        char text[64];
        if (fc_json_string(j, text, sizeof(text)) < 0) return -1;
        if (is_null) *is_null = 0;
        if (strcmp(text, "--") == 0 || text[0] == '\0') {
            fc_json_error(j, "%s: replace \"--\" with a number (keep the quotes)", field);
            return -1;
        }
        int parsed;
        if (safe_atoi(text, &parsed) < 0) {
            fc_json_error(j, "%s: expected an integer inside the quotes", field);
            return -1;
        }
        *out = parsed;
        return 0;
    }

    if (strncmp(j->p, "null", 4) == 0 &&
        !isalnum((unsigned char)j->p[4]) && j->p[4] != '_') {
        j->p += 4;
        if (is_null) *is_null = 1;
        return 0;
    }
    if (is_null) *is_null = 0;

    if (j->p[0] == '-' && j->p[1] == '-') {
        fc_json_error(j, "%s: replace \"--\" with a number", field);
        return -1;
    }
    if (*j->p == '-') {
        negative = 1;
        j->p++;
    }
    if (!isdigit((unsigned char)*j->p)) {
        fc_json_error(j, "%s: expected an integer or a quoted number", field);
        return -1;
    }
    if (*j->p == '0' && isdigit((unsigned char)j->p[1])) {
        fc_json_error(j, "leading zero in integer");
        return -1;
    }
    while (isdigit((unsigned char)*j->p)) {
        value = value * 10 + (*j->p - '0');
        if (value > (long)INT_MAX) {
            fc_json_error(j, "integer is too large");
            return -1;
        }
        j->p++;
    }
    if (*j->p == '.' || *j->p == 'e' || *j->p == 'E') {
        fc_json_error(j, "fan-curve values must be integers");
        return -1;
    }
    *out = negative ? (int)-value : (int)value;
    return 0;
}

/* Fan accepts a number, a quoted number, or "a"/"auto" for EC automatic. */
static int fc_json_fan(struct fancurve_json *j, int *out, int *is_null)
{
    fc_json_ws(j);
    if (*j->p == '"') {
        char text[64];
        if (fc_json_string(j, text, sizeof(text)) < 0) return -1;
        if (is_null) *is_null = 0;
        if (strcasecmp(text, "a") == 0 || strcasecmp(text, "auto") == 0) {
            *out = FANCURVE_FAN_AUTO;
            return 0;
        }
        if (strcmp(text, "--") == 0 || text[0] == '\0') {
            fc_json_error(j, "point fan: replace \"--\" with a number or \"a\"");
            return -1;
        }
        int parsed;
        if (safe_atoi(text, &parsed) < 0 || parsed < 0) {
            fc_json_error(j,
                          "point fan: expected a percentage or \"a\" for EC auto");
            return -1;
        }
        *out = parsed;
        return 0;
    }

    if (fc_json_integer(j, out, is_null, "point fan") < 0) return -1;
    if (!*is_null && *out < 0) {
        fc_json_error(j, "point fan: expected a percentage or \"a\" for EC auto");
        return -1;
    }
    return 0;
}

static int fc_json_point(struct fancurve_json *j, struct fancurve_point *pt,
                         int *have_temp, int *have_fan)
{
    int first = 1;
    *have_temp = 0;
    *have_fan = 0;
    pt->temp = 0;
    pt->fan = 0;

    if (fc_json_eat(j, '{') < 0) return -1;

    for (;;) {
        char key[32];
        fc_json_ws(j);
        if (*j->p == '}') {
            j->p++;
            break;
        }
        if (!first && fc_json_eat(j, ',') < 0) return -1;
        first = 0;

        if (fc_json_string(j, key, sizeof(key)) < 0) return -1;
        if (fc_json_eat(j, ':') < 0) return -1;

        if (strcmp(key, "temp") == 0) {
            int is_null = 0;
            if (*have_temp) {
                fc_json_error(j, "duplicate 'temp' in point");
                return -1;
            }
            if (fc_json_integer(j, &pt->temp, &is_null, "point temp") < 0) return -1;
            *have_temp = !is_null;
        } else if (strcmp(key, "fan") == 0) {
            int is_null = 0;
            if (*have_fan) {
                fc_json_error(j, "duplicate 'fan' in point");
                return -1;
            }
            if (fc_json_fan(j, &pt->fan, &is_null) < 0) return -1;
            *have_fan = !is_null;
        } else {
            fc_json_error(j, "unknown point field '%s' (expected 'temp' or 'fan')", key);
            return -1;
        }
    }

    if (!*have_temp || !*have_fan) {
        fc_json_error(j, "every point needs 'temp' and 'fan' (percentage or \"a\")");
        return -1;
    }
    return 0;
}

static int fc_json_profile(const char *text, struct fancurve_profile *prof,
                           char *err, size_t err_sz)
{
    struct fancurve_json j;
    int seen_name = 0, seen_hyst = 0, seen_interval = 0, seen_points = 0;

    memset(prof, 0, sizeof(*prof));
    memset(&j, 0, sizeof(j));
    j.p = text;

    if (fc_json_eat(&j, '{') < 0) goto bad;

    for (;;) {
        char key[32];
        fc_json_ws(&j);
        if (*j.p == '}') {
            j.p++;
            break;
        }
        if ((seen_name || seen_hyst || seen_interval || seen_points) &&
            fc_json_eat(&j, ',') < 0)
            goto bad;

        if (fc_json_string(&j, key, sizeof(key)) < 0) goto bad;
        if (fc_json_eat(&j, ':') < 0) goto bad;

        if (strcmp(key, "name") == 0) {
            if (seen_name++) {
                fc_json_error(&j, "duplicate 'name'");
                goto bad;
            }
            if (fc_json_string(&j, prof->name, sizeof(prof->name)) < 0) goto bad;
        } else if (strcmp(key, "hysteresis") == 0) {
            int is_null = 0;
            if (seen_hyst++) {
                fc_json_error(&j, "duplicate 'hysteresis'");
                goto bad;
            }
            if (fc_json_integer(&j, &prof->hysteresis, &is_null, "hysteresis") < 0) goto bad;
            if (is_null) {
                fc_json_error(&j, "'hysteresis' must be an integer");
                goto bad;
            }
        } else if (strcmp(key, "interval_ms") == 0) {
            int is_null = 0;
            if (seen_interval++) {
                fc_json_error(&j, "duplicate 'interval_ms'");
                goto bad;
            }
            if (fc_json_integer(&j, &prof->interval_ms, &is_null, "interval_ms") < 0) goto bad;
            if (is_null) {
                fc_json_error(&j, "'interval_ms' must be an integer");
                goto bad;
            }
        } else if (strcmp(key, "points") == 0) {
            if (seen_points++) {
                fc_json_error(&j, "duplicate 'points'");
                goto bad;
            }
            if (fc_json_eat(&j, '[') < 0) goto bad;
            for (;;) {
                int have_temp, have_fan;
                fc_json_ws(&j);
                if (*j.p == ']') {
                    j.p++;
                    break;
                }
                if (prof->npoints >= FANCURVE_MAX_POINTS) {
                    fc_json_error(&j, "too many points (maximum %d)", FANCURVE_MAX_POINTS);
                    goto bad;
                }
                if (prof->npoints > 0 && fc_json_eat(&j, ',') < 0) goto bad;
                if (fc_json_point(&j, &prof->points[prof->npoints],
                                  &have_temp, &have_fan) < 0)
                    goto bad;
                prof->npoints++;
            }
        } else {
            fc_json_error(&j, "unknown field '%s'", key);
            goto bad;
        }
    }

    fc_json_ws(&j);
    if (*j.p != '\0') {
        fc_json_error(&j, "trailing data after the JSON object");
        goto bad;
    }

    if (!seen_name)     { fc_json_error(&j, "missing 'name'"); goto bad; }
    if (!seen_hyst)     { fc_json_error(&j, "missing 'hysteresis'"); goto bad; }
    if (!seen_interval) { fc_json_error(&j, "missing 'interval_ms'"); goto bad; }
    if (!seen_points)   { fc_json_error(&j, "missing 'points'"); goto bad; }
    return 0;

bad:
    snprintf(err, err_sz, "%s", j.err[0] ? j.err : "invalid JSON");
    return -1;
}

static int fancurve_validate(const struct fancurve_profile *prof,
                             const char *expected_name,
                             char *err, size_t err_sz)
{
    if (!fancurve_name_valid(prof->name)) {
        snprintf(err, err_sz, "invalid profile name '%s'", prof->name);
        return -1;
    }
    if (expected_name && strcmp(prof->name, expected_name) != 0) {
        snprintf(err, err_sz, "profile name '%s' does not match file name '%s'",
                 prof->name, expected_name);
        return -1;
    }
    if (prof->hysteresis < 0 || prof->hysteresis > FANCURVE_MAX_HYSTERESIS) {
        snprintf(err, err_sz, "hysteresis must be 0-%d C", FANCURVE_MAX_HYSTERESIS);
        return -1;
    }
    if (prof->interval_ms < FANCURVE_MIN_INTERVAL ||
        prof->interval_ms > FANCURVE_MAX_INTERVAL) {
        snprintf(err, err_sz, "interval_ms must be %d-%d",
                 FANCURVE_MIN_INTERVAL, FANCURVE_MAX_INTERVAL);
        return -1;
    }
    if (prof->npoints < FANCURVE_MIN_POINTS ||
        prof->npoints > FANCURVE_MAX_POINTS) {
        snprintf(err, err_sz, "points must contain %d-%d entries",
                 FANCURVE_MIN_POINTS, FANCURVE_MAX_POINTS);
        return -1;
    }

    for (int i = 0; i < prof->npoints; i++) {
        const struct fancurve_point *pt = &prof->points[i];
        if (pt->temp < 0 || pt->temp > FANCURVE_MAX_TEMP) {
            snprintf(err, err_sz, "point %d: temp must be 0-%d C",
                     i + 1, FANCURVE_MAX_TEMP);
            return -1;
        }
        if (pt->fan == FANCURVE_FAN_AUTO) {
            if (i != prof->npoints - 1) {
                snprintf(err, err_sz,
                         "point %d: EC auto (\"a\") is only allowed at the last point",
                         i + 1);
                return -1;
            }
        } else if (pt->fan < FANCURVE_MIN_FAN || pt->fan > FANCURVE_MAX_FAN) {
            snprintf(err, err_sz, "point %d: fan must be %d-%d%% or \"a\"",
                     i + 1, FANCURVE_MIN_FAN, FANCURVE_MAX_FAN);
            return -1;
        }
        if (i > 0 && pt->temp <= prof->points[i - 1].temp) {
            snprintf(err, err_sz,
                     "point %d: temp must be greater than point %d (%d C > %d C)",
                     i + 1, i, pt->temp, prof->points[i - 1].temp);
            return -1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- files */

static char *fc_read_text(const char *path, char *err, size_t err_sz)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        snprintf(err, err_sz, "%s: %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(err, err_sz, "%s is not a regular file", path);
        close(fd);
        return NULL;
    }
    if (st.st_size < 0 || st.st_size > FANCURVE_FILE_MAX) {
        snprintf(err, err_sz, "%s is too large", path);
        close(fd);
        return NULL;
    }

    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        snprintf(err, err_sz, "out of memory");
        close(fd);
        return NULL;
    }

    size_t total = 0;
    while (total < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + total, (size_t)st.st_size - total);
        if (n > 0) {
            total += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(fd);

    if (total != (size_t)st.st_size) {
        snprintf(err, err_sz, "could not read %s", path);
        free(buf);
        return NULL;
    }
    buf[total] = '\0';
    return buf;
}

static int fc_write_temp_text(const char *text, char *out_path, size_t out_sz,
                              char *err, size_t err_sz)
{
    char tmpl[] = "/tmp/cctl-fancurve-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        snprintf(err, err_sz, "could not create a temporary file: %s", strerror(errno));
        return -1;
    }
    if (fchmod(fd, 0600) != 0) {
        snprintf(err, err_sz, "could not secure temporary file: %s", strerror(errno));
        close(fd);
        unlink(tmpl);
        return -1;
    }

    size_t len = strlen(text), off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    if (off != len || close(fd) != 0) {
        snprintf(err, err_sz, "could not write temporary file");
        unlink(tmpl);
        return -1;
    }
    if ((size_t)snprintf(out_path, out_sz, "%s", tmpl) >= out_sz) {
        snprintf(err, err_sz, "temporary path is too long");
        unlink(tmpl);
        return -1;
    }
    return 0;
}

static int fc_ensure_dir(char *err, size_t err_sz)
{
    struct stat st;
    if (lstat(FANCURVE_DIR, &st) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
            snprintf(err, err_sz, "%s is not a directory", FANCURVE_DIR);
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) {
        snprintf(err, err_sz, "cannot inspect %s: %s", FANCURVE_DIR, strerror(errno));
        return -1;
    }

    char *const args[] = { "install", "-d", "-m", "0755", FANCURVE_DIR, NULL };
    if (run(args, RUN_SILENT, NULL, 0) != 0) {
        snprintf(err, err_sz, "could not create %s", FANCURVE_DIR);
        return -1;
    }
    return 0;
}

static int fc_write_profile(const char *name, const char *text, size_t len,
                            char *err, size_t err_sz)
{
    char dest[PATH_MAX];
    if (fancurve_profile_path(name, dest, sizeof(dest)) < 0) {
        snprintf(err, err_sz, "invalid profile name '%s'", name);
        return -1;
    }
    if (fc_ensure_dir(err, err_sz) < 0) return -1;

    struct stat st;
    if (lstat(dest, &st) == 0 && S_ISLNK(st.st_mode)) {
        snprintf(err, err_sz, "refusing to overwrite a profile symlink");
        return -1;
    }

    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s/.%s.tmp.XXXXXX", FANCURVE_DIR, name);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        snprintf(err, err_sz, "profile path is too long");
        return -1;
    }

    int fd = mkstemp(tmp);
    if (fd < 0) {
        snprintf(err, err_sz, "could not create a profile file: %s", strerror(errno));
        return -1;
    }
    if (fchmod(fd, 0644) != 0) {
        snprintf(err, err_sz, "could not set profile permissions: %s", strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w > 0) {
            off += (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    if (off != len || fsync(fd) != 0 || close(fd) != 0) {
        snprintf(err, err_sz, "could not write the fan-curve profile");
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, dest) != 0) {
        snprintf(err, err_sz, "could not install the fan-curve profile: %s",
                 strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Read a regular, non-symlink file whose owner and mode match the caller. */
static char *fc_read_owned_text(const char *path, uid_t uid,
                                char *err, size_t err_sz)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        snprintf(err, err_sz, "%s: %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(err, err_sz, "%s is not a regular file", path);
        close(fd);
        return NULL;
    }
    if (st.st_uid != uid || (st.st_mode & 0077) != 0) {
        snprintf(err, err_sz, "%s has unsafe ownership or permissions", path);
        close(fd);
        return NULL;
    }
    if (st.st_size < 0 || st.st_size > FANCURVE_FILE_MAX) {
        snprintf(err, err_sz, "%s is too large", path);
        close(fd);
        return NULL;
    }

    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        snprintf(err, err_sz, "out of memory");
        close(fd);
        return NULL;
    }

    size_t total = 0;
    while (total < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + total, (size_t)st.st_size - total);
        if (r > 0) {
            total += (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(fd);

    if (total != (size_t)st.st_size) {
        snprintf(err, err_sz, "could not read %s", path);
        free(buf);
        return NULL;
    }
    buf[total] = '\0';
    return buf;
}

static uid_t fc_invoking_uid(void)
{
    if (geteuid() != 0)
        return getuid();

    const char *sudo_uid = getenv("SUDO_UID");
    if (sudo_uid && *sudo_uid) {
        char *end = NULL;
        errno = 0;
        unsigned long v = strtoul(sudo_uid, &end, 10);
        if (errno == 0 && end != sudo_uid && *end == '\0' && v <= UINT_MAX)
            return (uid_t)v;
    }
    return 0;
}

static int fc_install_profile(const char *name, const char *src,
                              char *err, size_t err_sz)
{
    uid_t owner = geteuid();
    if (geteuid() == 0 || access(FANCURVE_DIR, W_OK) == 0) {
        char *text = fc_read_owned_text(src, owner, err, err_sz);
        if (!text) return -1;
        int rc = fc_write_profile(name, text, strlen(text), err, err_sz);
        free(text);
        return rc;
    }

    /* The invoking user has passwordless sudo for cctl, so ask the installed
     * binary's restricted helper to write the profile instead of prompting
     * for a separate `install` password. */
    char exe[PATH_MAX];
    if (get_self_exe(exe, sizeof(exe)) < 0) {
        snprintf(err, err_sz, "could not determine the cctl binary path");
        return -1;
    }

    char *const args[] = { "sudo", exe, "fancurve", "__install",
                           (char *)name, (char *)src, NULL };
    if (run(args, RUN_SILENT, NULL, 0) != 0) {
        snprintf(err, err_sz, "could not save the fan-curve profile");
        return -1;
    }
    return 0;
}

static int fc_cmd_install_helper(int argc, char **argv)
{
    if (argc != 5)
        return fail("Usage: cctl fancurve __install <name> <temp-file>");

    const char *name = argv[3];
    const char *src = argv[4];

    require_root(argc, argv);

    if (!fancurve_name_valid(name))
        return fail("invalid profile name '%s'", name);

    static const char prefix[] = "/tmp/cctl-fancurve-";
    if (strncmp(src, prefix, sizeof(prefix) - 1) != 0 ||
        strchr(src + sizeof(prefix) - 1, '/') != NULL)
        return fail("invalid temporary fan-curve path");

    char err[256];
    char *text = fc_read_owned_text(src, fc_invoking_uid(), err, sizeof(err));
    if (!text)
        return fail("%s", err);

    struct fancurve_profile prof;
    int rc = fc_json_profile(text, &prof, err, sizeof(err));
    if (rc == 0)
        rc = fancurve_validate(&prof, name, err, sizeof(err));
    if (rc < 0) {
        free(text);
        return fail("%s", err);
    }

    rc = fc_write_profile(name, text, strlen(text), err, sizeof(err));
    free(text);
    if (rc < 0)
        return fail("%s", err);
    return 0;
}

static int fc_load_profile(const char *name, struct fancurve_profile *prof,
                           char *err, size_t err_sz)
{
    char path[PATH_MAX];
    if (fancurve_profile_path(name, path, sizeof(path)) < 0) {
        snprintf(err, err_sz, "invalid profile name '%s'", name);
        return -1;
    }

    char *text = fc_read_text(path, err, err_sz);
    if (!text) return -1;

    int rc = fc_json_profile(text, prof, err, err_sz);
    free(text);
    if (rc < 0) return -1;
    return fancurve_validate(prof, name, err, err_sz);
}

/* --------------------------------------------------------------- editor */

static const char *fc_pick_editor(void)
{
    const char *editor = getenv("SUDO_EDITOR");
    if (!editor || !*editor) editor = getenv("VISUAL");
    if (!editor || !*editor) editor = getenv("EDITOR");
    if (!editor || !*editor) {
        if (command_exists("nano")) editor = "nano";
        else if (command_exists("micro")) editor = "micro";
    }
    return editor;
}

static int fc_run_editor(const char *path, char *err, size_t err_sz)
{
    const char *editor = fc_pick_editor();
    if (!editor || !*editor) {
        snprintf(err, err_sz,
                 "no editor configured and neither nano nor micro is installed; "
                 "set SUDO_EDITOR, VISUAL, or EDITOR");
        return -1;
    }

    char cmd[PATH_MAX + 512];
    if (snprintf(cmd, sizeof(cmd), "%s '%s'", editor, path) >= (int)sizeof(cmd)) {
        snprintf(err, err_sz, "editor command is too long");
        return -1;
    }

    char *const args[] = { "sh", "-c", cmd, NULL };
    int rc = run(args, RUN_RAW, NULL, 0);
    if (rc != 0) {
        snprintf(err, err_sz, "editor exited with status %d", rc);
        return -1;
    }
    return 0;
}

/* Validate editor output; on errors offer to reopen the editor. */
static int fc_edit_and_save(const char *initial, const char *name,
                            char *err, size_t err_sz)
{
    char *text = strdup(initial);
    if (!text) {
        snprintf(err, err_sz, "out of memory");
        return -1;
    }

    for (;;) {
        char tmp[PATH_MAX];
        if (fc_write_temp_text(text, tmp, sizeof(tmp), err, err_sz) < 0) {
            free(text);
            return -1;
        }
        if (fc_run_editor(tmp, err, err_sz) < 0) {
            unlink(tmp);
            free(text);
            return -1;
        }

        char read_err[256];
        char *edited = fc_read_text(tmp, read_err, sizeof(read_err));
        if (!edited) {
            unlink(tmp);
            snprintf(err, err_sz, "%s", read_err);
            free(text);
            return -1;
        }

        struct fancurve_profile prof;
        char parse_err[256];
        int rc = fc_json_profile(edited, &prof, parse_err, sizeof(parse_err));
        if (rc == 0)
            rc = fancurve_validate(&prof, name, parse_err, sizeof(parse_err));

        if (rc == 0) {
            rc = fc_install_profile(name, tmp, err, err_sz);
            unlink(tmp);
            if (rc < 0) {
                free(edited);
                free(text);
                return -1;
            }
            free(edited);
            free(text);
            return 0;
        }

        unlink(tmp);
        cctl_stream_fprintf(stderr, "Invalid fan curve: %s\n", parse_err);
        cctl_stream_printf("Reopen the editor? [Y/n] ");
        fflush(stdout);
        char ans[16];
        int got = fgets(ans, sizeof(ans), stdin) != NULL;
        cctl_stream_putchar('\n');
        if (!got || ans[0] == 'n' || ans[0] == 'N') {
            free(edited);
            free(text);
            snprintf(err, err_sz, "no changes saved");
            return -1;
        }
        free(text);
        text = edited;
    }
}

/* ---------------------------------------------------------------- state */

static int fc_read_state(struct fancurve_state *st)
{
    memset(st, 0, sizeof(*st));
    st->starttime = -1;
    st->sleep_monitor = -1;

    int fd = open(FANCURVE_STATE_PATH, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;

    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        close(fd);
        return -1;
    }

    FILE *fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return -1;
    }

    char line[256];
    long pid = -1, start = -1;
    if (!fgets(line, sizeof(line), fp) ||
        sscanf(line, "%ld %ld", &pid, &start) < 1 || pid <= 1) {
        fclose(fp);
        return -1;
    }
    st->pid = (pid_t)pid;
    st->starttime = start;

    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (!fancurve_name_valid(line)) {
        fclose(fp);
        return -1;
    }
    snprintf(st->name, sizeof(st->name), "%.*s", FANCURVE_NAME_MAX, line);

    if (fgets(line, sizeof(line), fp))
        st->epoch = (time_t)strtol(line, NULL, 10);
    if (fgets(line, sizeof(line), fp)) {
        errno = 0;
        char *end = NULL;
        long value = strtol(line, &end, 10);
        while (end && isspace((unsigned char)*end)) end++;
        if (errno || end == line || !end || *end || (value != 0 && value != 1)) {
            fclose(fp);
            return -1;
        }
        st->sleep_monitor = (int)value;
    }

    if (fgets(line, sizeof(line), fp)) {
        int hyst, interval, points;
        char extra;
        line[strcspn(line, "\r\n")] = '\0';
        if (sscanf(line, "config=%d %d %d%c", &hyst, &interval, &points, &extra) == 3 &&
            points >= FANCURVE_MIN_POINTS && points <= FANCURVE_MAX_POINTS) {
            struct fancurve_profile profile = {0};
            snprintf(profile.name, sizeof(profile.name), "%s", st->name);
            profile.hysteresis = hyst;
            profile.interval_ms = interval;
            profile.npoints = points;
            int valid = 1;
            for (int i = 0; i < points; i++) {
                if (!fgets(line, sizeof(line), fp)) { valid = 0; break; }
                line[strcspn(line, "\r\n")] = '\0';
                if (sscanf(line, "point=%d %d%c", &profile.points[i].temp,
                           &profile.points[i].fan, &extra) != 2) {
                    valid = 0;
                    break;
                }
            }
            char err[128];
            if (valid && fancurve_validate(&profile, NULL, err, sizeof(err)) == 0) {
                st->profile = profile;
                st->profile_known = 1;
            }
        }
    }

    fclose(fp);
    return 0;
}

static int fc_worker_process_alive(const struct fancurve_state *st)
{
    if (!st || st->pid <= 1 || st->starttime <= 0 ||
        proc_starttime(st->pid) != st->starttime)
        return 0;
    char path[64], statbuf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)st->pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int alive = 0;
    if (fgets(statbuf, sizeof(statbuf), fp)) {
        char *end = strrchr(statbuf, ')');
        if (end && end[1] == ' ' && end[2] != 'Z') alive = 1;
    }
    fclose(fp);
    return alive;
}

static int fc_signal_worker(const struct fancurve_state *st, int sig)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    int pidfd = (int)syscall(SYS_pidfd_open, st->pid, 0);
    if (pidfd >= 0) {
        if (proc_starttime(st->pid) != st->starttime) {
            close(pidfd);
            errno = ESRCH;
            return -1;
        }
        int rc = (int)syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
        int saved_errno = errno;
        close(pidfd);
        errno = saved_errno;
        return rc;
    }
    if (errno != ENOSYS && errno != EINVAL) return -1;
#endif
    if (!fc_worker_process_alive(st)) {
        errno = ESRCH;
        return -1;
    }
    return kill(st->pid, sig);
}

static int fc_is_running(struct fancurve_state *st)
{
    struct fancurve_state local;
    if (!st) st = &local;
    if (fc_read_state(st) < 0) return 0;

    int alive = fc_worker_process_alive(st);

    if (!alive && geteuid() == 0)
        unlink(FANCURVE_STATE_PATH);
    return alive;
}

/* Non-root callers cannot read /run/cctl_curve.state (0600). Treat an
 * existing state file as active for previews; root commands can verify it. */
static int fc_any_active(struct fancurve_state *st)
{
    if (fc_is_running(st)) return 1;
    if (geteuid() != 0 && access(FANCURVE_STATE_PATH, F_OK) == 0) {
        memset(st, 0, sizeof(*st));
        st->starttime = -1;
        return 1;
    }
    return 0;
}

static void fc_active_label(const struct fancurve_state *st, char *buf, size_t len)
{
    if (st && st->name[0])
        snprintf(buf, len, "fan curve '%s'", st->name);
    else
        snprintf(buf, len, "a fan curve");
}

static void fc_warn_active(const struct fancurve_state *st)
{
    char label[FANCURVE_NAME_MAX + 32];
    fc_active_label(st, label, sizeof(label));
    int prev_mode = g_stream_error_mode;
    int prev_line = g_stream_error_line;
    g_stream_error_mode = STREAM_MODE_LINE;
    g_stream_error_line = 0;
    cctl_stream_fprintf(stderr, "Info: %s is active; keeping curve fan control.\n", label);
    g_stream_error_mode = prev_mode;
    g_stream_error_line = prev_line;
}

static void fc_warn_fan_change(const struct fancurve_state *st)
{
    char label[FANCURVE_NAME_MAX + 32];
    fc_active_label(st, label, sizeof(label));
    cctl_stream_fprintf(stderr, "Warning: %s is running; refusing to change fan control.\n", label);
    cctl_stream_fprintf(stderr, "         Stop it first with 'cctl fancurve stop'.\n");
}

/* ------------------------------------------------------------- daemon */

static volatile sig_atomic_t g_fancurve_running = 1;

static void fc_sig_handler(int sig)
{
    (void)sig;
    g_fancurve_running = 0;
}

static int fc_sleep_ms(int ms)
{
    while (ms > 0 && g_fancurve_running) {
        int chunk = ms > 25 ? 25 : ms;
        usleep((useconds_t)chunk * 1000);
        ms -= chunk;
    }
    return g_fancurve_running ? 0 : -1;
}

static void fc_install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fc_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

static int fc_acquire_lock(void)
{
    fc_install_signal_handlers();

    int fd = open(FANCURVE_LOCK_PATH, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    fchmod(fd, 0600);

    for (int i = 0; i < 75 && g_fancurve_running; i++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            return fd;
        usleep(20000);
    }
    close(fd);
    return -1;
}

static int fc_temp_faninfo(int fd, unsigned long req, int *out)
{
    int32_t raw = 0;
    if (ioctl(fd, req, &raw) < 0) return -1;
    int temp = (raw >> 16) & 0xFF;
    if (temp <= 0 || temp >= 125) return -1;
    *out = temp;
    return 0;
}

/* Fan safety fallback: inspect recognized CPU thermal drivers and use their
 * hottest valid input. This is intentionally more conservative than selecting
 * one package label, and is separate from cpumon's presentation sensor choice. */
static int fc_cpu_temp_hwmon(void)
{
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir) return -1;

    int best = -1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0) continue;

        char path[PATH_MAX], name[64];
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", ent->d_name);
        if (read_sysfs_str(path, name, sizeof(name)) < 0) continue;
        if (strcmp(name, "coretemp") != 0 && strcmp(name, "k10temp") != 0 &&
            strcmp(name, "zenpower") != 0)
            continue;

        char dirpath[PATH_MAX];
        snprintf(dirpath, sizeof(dirpath), "/sys/class/hwmon/%s", ent->d_name);
        DIR *sensors = opendir(dirpath);
        if (!sensors) continue;
        struct dirent *sensor;
        while ((sensor = readdir(sensors)) != NULL) {
            int index = 0;
            char suffix = '\0';
            if (sscanf(sensor->d_name, "temp%d_input%c", &index, &suffix) != 1 || index < 1)
                continue;
            snprintf(path, sizeof(path), "/sys/class/hwmon/%s/%s",
                     ent->d_name, sensor->d_name);
            long milli = read_sysfs_long(path, -1000);
            if (milli < 0) continue;
            int temp = (int)(milli / 1000);
            if (temp > 0 && temp < 125 && temp > best) best = temp;
        }
        closedir(sensors);
    }
    closedir(dir);
    return best;
}

/* nvidia-smi can wake a GPU or keep it out of D3cold. Only query when every
 * NVIDIA display device is already in D0; unknown states are treated as off. */
static int fc_nvidia_gpus_in_d0(void)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    if (!dir) return 0;

    int found = 0, all_in_d0 = 1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[PATH_MAX], value[32];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", ent->d_name);
        if (read_sysfs_str(path, value, sizeof(value)) < 0 || strcmp(value, "0x10de") != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", ent->d_name);
        if (read_sysfs_str(path, value, sizeof(value)) < 0 ||
            (strncmp(value, "0x0300", 6) != 0 && strncmp(value, "0x0302", 6) != 0))
            continue;

        found++;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/power_state", ent->d_name);
        if (read_sysfs_str(path, value, sizeof(value)) < 0 || strcmp(value, "D0") != 0) {
            all_in_d0 = 0;
            break;
        }
    }
    closedir(dir);
    return found > 0 && all_in_d0;
}

static int fc_gpu_temp_nvidia(void)
{
    if (!fc_nvidia_gpus_in_d0()) return -1;

    char out[128];
    char *const args[] = { "nvidia-smi", "--query-gpu=temperature.gpu",
                           "--format=csv,noheader,nounits", NULL };
    if (run(args, RUN_CAPTURE, out, sizeof(out)) != 0) return -1;

    char *p = out;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    char *end = NULL;
    long temp = strtol(p, &end, 10);
    if (end == p || temp <= 0 || temp >= 125) return -1;
    return (int)temp;
}

/* Hottest valid fan-sensor temperature. This EC can report a transient 1 C
 * CPU reading around resume; treat it as invalid and try CPU hwmon instead. */
static int fc_read_temps(int fd, int *cpu, int *gpu)
{
    int c = -1, g = -1;

    if (fc_temp_faninfo(fd, R_CL_FANINFO1, &c) < 0 || c == 1) {
        c = fc_cpu_temp_hwmon();
        if (c < 0) {
            c = read_cpu_temp();
            if (c == 1) c = -1;
        }
    }
    if (fc_temp_faninfo(fd, R_CL_FANINFO2, &g) < 0)
        g = fc_gpu_temp_nvidia();

    *cpu = c;
    *gpu = g;
    return (c > 0 || g > 0) ? 0 : -1;
}

static int fc_auto_point_temp(const struct fancurve_profile *prof)
{
    if (prof->npoints > 0 &&
        prof->points[prof->npoints - 1].fan == FANCURVE_FAN_AUTO)
        return prof->points[prof->npoints - 1].temp;
    return -1;
}

static int fc_interpolate(const struct fancurve_profile *prof, int temp)
{
    int last = prof->npoints - 1;
    if (prof->points[last].fan == FANCURVE_FAN_AUTO) {
        if (temp >= prof->points[last].temp) return FANCURVE_FAN_AUTO;
        last--;
    }

    if (temp <= prof->points[0].temp)
        return prof->points[0].fan;
    if (temp >= prof->points[last].temp)
        return prof->points[last].fan;

    for (int i = 0; i < last; i++) {
        const struct fancurve_point *a = &prof->points[i];
        const struct fancurve_point *b = &prof->points[i + 1];
        if (temp >= a->temp && temp <= b->temp) {
            int span = b->temp - a->temp;
            if (span <= 0) return b->fan;
            return a->fan + ((b->fan - a->fan) * (temp - a->temp)) / span;
        }
    }
    return prof->points[last].fan;
}

static int fc_set_duty(int fd, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint8_t raw = (uint8_t)((pct * 255) / 100);
    int32_t arg = (int32_t)raw | ((int32_t)raw << 8);
    return ioctl(fd, W_CL_FANSPEED, &arg);
}

static int fc_auto(int fd)
{
    int32_t mask = 15;
    return ioctl(fd, W_CL_FANAUTO, &mask);
}

static int fc_arm_auto_on_close(int fd)
{
    return tuxedo_io_version_ok(fd) ? ioctl(fd, W_CL_FAN_AUTO_ON_CLOSE, 0) : -1;
}

static int fc_auto_all_quiet(void)
{
    int32_t mask = 15;
    return tuxedo_ioctl(W_CL_FANAUTO, &mask);
}

struct fancurve_worker_ctx {
    int lock_fd;
    int tuxedo_fd;
    int sleep_inhibit_fd;
    int sleep_resume_pending;
    int sleep_force_apply;
    int sleep_suspended;
    int sleep_monitor_active;
    int sleep_clock_valid;
    int sleep_clock_resume_seen;
    int64_t sleep_clock_offset_ns;
    DBusConnection *sleep_bus;
};

static void fc_log_line(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static int fc_reopen_drivers(struct fancurve_worker_ctx *ctx);

/* CLOCK_BOOTTIME advances during suspend; CLOCK_MONOTONIC does not. Their
 * offset therefore changes only after the machine has actually slept, which
 * lets the temperature fallback distinguish resume from the pre-sleep window. */
static int fc_sleep_clock_offset(int64_t *offset_ns)
{
    struct timespec boot, mono;
    if (clock_gettime(CLOCK_BOOTTIME, &boot) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &mono) != 0)
        return -1;
    int64_t boot_ns = (int64_t)boot.tv_sec * 1000000000LL + boot.tv_nsec;
    int64_t mono_ns = (int64_t)mono.tv_sec * 1000000000LL + mono.tv_nsec;
    *offset_ns = boot_ns - mono_ns;
    return 0;
}

static int fc_sleep_clock_confirms_resume(struct fancurve_worker_ctx *ctx)
{
    int64_t current_offset;
    if (!ctx->sleep_clock_valid ||
        fc_sleep_clock_offset(&current_offset) != 0)
        return 0;
    return current_offset - ctx->sleep_clock_offset_ns >=
           FANCURVE_SLEEP_CLOCK_THRESHOLD_NS;
}

static int fc_sleep_inhibitor_acquire(struct fancurve_worker_ctx *ctx)
{
    DBusMessage *request = dbus_message_new_method_call(
        "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "Inhibit");
    if (!request) return -1;

    const char *what = "sleep";
    const char *who = "cctl fan curve";
    const char *why = "Restore automatic fan control before suspend";
    const char *mode = "delay";
    DBusMessageIter args;
    dbus_message_iter_init_append(request, &args);
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &what) ||
        !dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &who) ||
        !dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &why) ||
        !dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &mode)) {
        dbus_message_unref(request);
        return -1;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        ctx->sleep_bus, request, 5000, &error);
    dbus_message_unref(request);
    if (!reply) {
        if (dbus_error_is_set(&error))
            fc_log_line("sleep inhibitor request failed: %s", error.message);
        dbus_error_free(&error);
        return -1;
    }

    int received_fd = -1;
    int rc = dbus_message_get_args(reply, &error,
                                   DBUS_TYPE_UNIX_FD, &received_fd,
                                   DBUS_TYPE_INVALID);
    int fd = rc ? fcntl(received_fd, F_DUPFD_CLOEXEC, 3) : -1;
    if (fd < 0 && dbus_error_is_set(&error))
        fc_log_line("invalid sleep inhibitor reply: %s", error.message);
    dbus_message_unref(reply);
    dbus_error_free(&error);
    if (fd < 0) return -1;

    if (ctx->sleep_inhibit_fd >= 0)
        close(ctx->sleep_inhibit_fd);
    ctx->sleep_inhibit_fd = fd;
    return 0;
}

static DBusHandlerResult fc_sleep_signal(DBusConnection *connection,
                                          DBusMessage *message, void *userdata)
{
    (void)connection;
    struct fancurve_worker_ctx *ctx = userdata;
    if (!dbus_message_is_signal(message, "org.freedesktop.login1.Manager",
                                "PrepareForSleep"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusError error;
    dbus_error_init(&error);
    dbus_bool_t sleeping = FALSE;
    if (!dbus_message_get_args(message, &error,
                               DBUS_TYPE_BOOLEAN, &sleeping,
                               DBUS_TYPE_INVALID)) {
        dbus_error_free(&error);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    dbus_error_free(&error);

    if (sleeping && !ctx->sleep_suspended) {
        fc_log_line("suspend requested; releasing fans to EC auto");
        ctx->sleep_clock_valid =
            fc_sleep_clock_offset(&ctx->sleep_clock_offset_ns) == 0;
        ctx->sleep_clock_resume_seen = 0;
        if (!ctx->sleep_clock_valid)
            fc_log_line("suspend clock unavailable; temperature fallback waits for login1 resume signal");
        if (ctx->tuxedo_fd >= 0) {
            close(ctx->tuxedo_fd); /* The armed driver close hook restores EC auto. */
            ctx->tuxedo_fd = -1;
        }
        if (ctx->sleep_inhibit_fd >= 0) {
            close(ctx->sleep_inhibit_fd);
            ctx->sleep_inhibit_fd = -1;
        }
        ctx->sleep_suspended = 1;
    } else if (!sleeping && ctx->sleep_suspended) {
        fc_log_line("resume detected; preparing to continue fan curve");
        ctx->sleep_resume_pending = 1;
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

static int fc_sleep_monitor_init(struct fancurve_worker_ctx *ctx)
{
    DBusError error;
    dbus_error_init(&error);
    ctx->sleep_bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    if (!ctx->sleep_bus) {
        if (dbus_error_is_set(&error))
            fc_log_line("sleep notifications unavailable: %s", error.message);
        dbus_error_free(&error);
        return -1;
    }
    dbus_error_free(&error);
    dbus_connection_set_exit_on_disconnect(ctx->sleep_bus, FALSE);

    dbus_bus_add_match(ctx->sleep_bus,
        "type='signal',sender='org.freedesktop.login1',path='/org/freedesktop/login1',"
        "interface='org.freedesktop.login1.Manager',member='PrepareForSleep'", &error);
    if (dbus_error_is_set(&error)) {
        fc_log_line("sleep signal subscription failed: %s", error.message);
        dbus_error_free(&error);
        return -1;
    }
    dbus_error_free(&error);
    if (!dbus_connection_add_filter(ctx->sleep_bus, fc_sleep_signal, ctx, NULL))
        return -1;
    dbus_connection_flush(ctx->sleep_bus);

    if (fc_sleep_inhibitor_acquire(ctx) < 0)
        return -1;
    fc_log_line("suspend safety enabled through login1");
    return 0;
}

static void fc_sleep_monitor_cleanup(struct fancurve_worker_ctx *ctx)
{
    if (ctx->sleep_inhibit_fd >= 0) {
        close(ctx->sleep_inhibit_fd);
        ctx->sleep_inhibit_fd = -1;
    }
    if (ctx->sleep_bus) {
        dbus_connection_remove_filter(ctx->sleep_bus, fc_sleep_signal, ctx);
        dbus_connection_close(ctx->sleep_bus);
        dbus_connection_unref(ctx->sleep_bus);
        ctx->sleep_bus = NULL;
    }
}

static int fc_sleep_monitor_process(struct fancurve_worker_ctx *ctx)
{
    if (ctx->sleep_bus &&
        (!dbus_connection_read_write_dispatch(ctx->sleep_bus, 0) ||
         !dbus_connection_get_is_connected(ctx->sleep_bus))) {
        if (!ctx->sleep_suspended) {
            fc_log_line("login1 connection lost; restoring EC auto and stopping curve");
            g_fancurve_running = 0;
            return -1;
        }
        fc_log_line("login1 connection lost during suspend; monitoring temperature fallback");
        fc_sleep_monitor_cleanup(ctx);
        ctx->sleep_monitor_active = 0;
    }

    if (ctx->sleep_resume_pending) {
        ctx->sleep_resume_pending = 0;
        int inhibitor_acquired = 0;
        if (!ctx->sleep_bus) {
            if (fc_sleep_monitor_init(ctx) == 0) {
                ctx->sleep_monitor_active = 1;
                inhibitor_acquired = 1;
            } else {
                fc_sleep_monitor_cleanup(ctx);
                ctx->sleep_monitor_active = 0;
                fc_log_line("login1 unavailable after wake; resuming with temperature fallback only");
            }
        } else {
            ctx->sleep_monitor_active = 1;
        }
        if ((!inhibitor_acquired && ctx->sleep_bus &&
             fc_sleep_inhibitor_acquire(ctx) < 0) ||
            fc_reopen_drivers(ctx) < 0) {
            fc_log_line("could not resume fan control safely; stopping curve");
            g_fancurve_running = 0;
            return -1;
        }
        ctx->sleep_suspended = 0;
        ctx->sleep_force_apply = 1;
    }
    return 0;
}

static int fc_worker_sleep_ms(int ms, struct fancurve_worker_ctx *ctx)
{
    while (ms > 0 && g_fancurve_running) {
        int was_suspended = ctx->sleep_suspended;
        if (fc_sleep_monitor_process(ctx) < 0) return -1;
        if (ctx->sleep_force_apply || ctx->sleep_suspended != was_suspended)
            return 0;
        int chunk = ms > 250 ? 250 : ms;
        if (ctx->sleep_bus) {
            /* Dispatch immediately when logind sends a sleep/resume signal;
             * otherwise block until this short signal-check interval ends. */
            (void)dbus_connection_read_write_dispatch(ctx->sleep_bus, chunk);
        } else {
            struct timespec delay = {
                .tv_sec = chunk / 1000,
                .tv_nsec = (long)(chunk % 1000) * 1000000L
            };
            while (nanosleep(&delay, &delay) < 0 && errno == EINTR && g_fancurve_running) {}
        }
        if (!g_fancurve_running) return -1;
        ms -= chunk;
        if (fc_sleep_monitor_process(ctx) < 0) return -1;
        if (ctx->sleep_force_apply || ctx->sleep_suspended != was_suspended)
            return 0;
    }
    return g_fancurve_running ? 0 : -1;
}

static int fc_worker_begin(struct fancurve_worker_ctx *ctx,
                           const struct fancurve_profile *prof)
{
    int state_fd = -1;
    FILE *fp = NULL;

    pin_to_e_cores();

    ctx->lock_fd = fc_acquire_lock();
    if (ctx->lock_fd < 0) return -1;

    ctx->tuxedo_fd = tuxedo_open_clevo();
    if (ctx->tuxedo_fd < 0 || fc_arm_auto_on_close(ctx->tuxedo_fd) != 0) {
        cctl_stream_fprintf(stderr, "cctl: driver lacks fan-auto-on-close safety; refusing to start fan curve\n");
        goto fail;
    }

    if (fc_sleep_monitor_init(ctx) < 0) {
        fc_sleep_monitor_cleanup(ctx);
        ctx->sleep_monitor_active = 0;
        fc_log_line("suspend auto-restore unavailable; curve will continue without login1 support");
    } else {
        ctx->sleep_monitor_active = 1;
    }

    state_fd = open(FANCURVE_STATE_PATH,
                    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (state_fd < 0) goto fail;
    fchmod(state_fd, 0600);

    fp = fdopen(state_fd, "w");
    if (!fp) goto fail;
    state_fd = -1; /* owned by fp */

    int failed = cctl_stream_fprintf(fp, "%d %ld\n%s\n%ld\n%d\nconfig=%d %d %d\n",
            (int)getpid(), proc_starttime(getpid()), prof->name, (long)time(NULL),
            ctx->sleep_monitor_active, prof->hysteresis, prof->interval_ms,
            prof->npoints) < 0;
    for (int i = 0; !failed && i < prof->npoints; i++)
        failed = cctl_stream_fprintf(fp, "point=%d %d\n", prof->points[i].temp,
                                     prof->points[i].fan) < 0;
    if (failed || fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        fclose(fp);
        fp = NULL;
        goto fail;
    }
    fclose(fp);
    return 0;

fail:
    if (fp) fclose(fp);
    if (state_fd >= 0) close(state_fd);
    unlink(FANCURVE_STATE_PATH);
    if (ctx->tuxedo_fd >= 0) close(ctx->tuxedo_fd);
    ctx->tuxedo_fd = -1;
    fc_sleep_monitor_cleanup(ctx);
    flock(ctx->lock_fd, LOCK_UN);
    close(ctx->lock_fd);
    ctx->lock_fd = -1;
    return -1;
}

static void fc_log_line(const char *fmt, ...)
{
    char body[448];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char stamp[16] = "";
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm) ||
        strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm) == 0)
        stamp[0] = '\0';

    char line[512];
    snprintf(line, sizeof(line), "[%s] %s\n", stamp, body);

    int fd = open(FANCURVE_LOG_FIFO, O_WRONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return;

    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISFIFO(sb.st_mode)) {
        close(fd);
        return;
    }

    ssize_t ignored = write(fd, line, strlen(line));
    (void)ignored;
    close(fd);
}

static int fc_module_present(const char *name)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), FANCURVE_MODULE_DIR "/%s", name);
    return n > 0 && (size_t)n < sizeof(path) && access(path, F_OK) == 0;
}

static int fc_drivers_ready(void)
{
    return fc_module_present("clevo_acpi") &&
           fc_module_present("tuxedo_io") &&
           access("/dev/tuxedo_io", F_OK) == 0;
}

static int fc_modprobe(const char *module)
{
    char *const args[] = { FANCURVE_MODPROBE, (char *)module, NULL };
    return run(args, RUN_SILENT, NULL, 0);
}

static int fc_auto_direct(void)
{
    for (int attempt = 0; attempt < FANCURVE_IOCTL_RETRIES; attempt++) {
        int cpu = legacy_fan_auto(FAN_CPU);
        int gpu = legacy_fan_auto(FAN_GPU);
        if (cpu == 0 && gpu == 0) {
            ec_release_ports();
            return 0;
        }
        usleep(FANCURVE_IOCTL_RETRY_US);
    }
    ec_release_ports();
    return -1;
}

static int fc_reopen_drivers(struct fancurve_worker_ctx *ctx)
{
    if (ctx->tuxedo_fd >= 0) {
        close(ctx->tuxedo_fd);
        ctx->tuxedo_fd = -1;
    }

    if (!fc_drivers_ready()) {
        fc_log_line("drivers unavailable; running modprobe clevo_acpi");
        fc_modprobe("clevo_acpi");
        fc_log_line("drivers unavailable; running modprobe tuxedo_io");
        fc_modprobe("tuxedo_io");
        for (int i = 0; i < 20 && !fc_drivers_ready(); i++)
            usleep(100000);
    }

    if (!fc_drivers_ready()) {
        fc_log_line("driver reload failed");
        return -1;
    }

    for (int i = 0; i < 20; i++) {
        ctx->tuxedo_fd = tuxedo_open_clevo();
        if (ctx->tuxedo_fd >= 0 && fc_arm_auto_on_close(ctx->tuxedo_fd) == 0) {
            fc_log_line("tuxedo_io reopened");
            return 0;
        }
        if (ctx->tuxedo_fd >= 0) close(ctx->tuxedo_fd);
        ctx->tuxedo_fd = -1;
        usleep(100000);
    }
    fc_log_line("tuxedo_io reopen failed");
    return -1;
}

static int fc_try_auto_tuxedo(struct fancurve_worker_ctx *ctx)
{
    if (ctx->tuxedo_fd < 0) return -1;
    for (int attempt = 0; attempt < FANCURVE_IOCTL_RETRIES; attempt++) {
        if (fc_auto(ctx->tuxedo_fd) == 0) return 0;
        usleep(FANCURVE_IOCTL_RETRY_US);
    }
    return -1;
}

/* 0 = auto through tuxedo_io, 1 = direct EC auto, -1 = no auto possible. */
static int fc_restore_auto(struct fancurve_worker_ctx *ctx, int allow_reload)
{
    if (fc_try_auto_tuxedo(ctx) == 0) {
        fc_log_line("auto restored via W_CL_FANAUTO");
        return 0;
    }
    fc_log_line("W_CL_FANAUTO failed");

    if (allow_reload && fc_reopen_drivers(ctx) == 0 &&
        fc_try_auto_tuxedo(ctx) == 0) {
        fc_log_line("auto restored via W_CL_FANAUTO after driver reload");
        return 0;
    }

    fc_log_line("falling back to direct EC fan auto");
    if (fc_auto_direct() == 0) {
        fc_log_line("auto restored via direct EC fan control");
        return 1;
    }
    fc_log_line("direct EC fan auto failed");
    return -1;
}

static void fc_worker_end(struct fancurve_worker_ctx *ctx)
{
    int auto_ok = 0;
    if (ctx->tuxedo_fd >= 0 && fc_auto(ctx->tuxedo_fd) == 0)
        auto_ok = 1;
    if (!auto_ok) {
        fc_log_line("tuxedo_io auto failed while stopping; trying direct EC auto");
        if (fc_auto_direct() == 0)
            fc_log_line("auto restored via direct EC fan control");
        else
            fc_log_line("failed to restore automatic fan control");
    }
    if (ctx->tuxedo_fd >= 0) {
        close(ctx->tuxedo_fd);
        ctx->tuxedo_fd = -1;
    }
    fc_log_line("worker stopped");
    unlink(FANCURVE_STATE_PATH);
    fc_sleep_monitor_cleanup(ctx);
    if (ctx->lock_fd >= 0) {
        flock(ctx->lock_fd, LOCK_UN);
        close(ctx->lock_fd);
        ctx->lock_fd = -1;
    }
}

static void fc_worker(const struct fancurve_profile *prof)
{
    struct fancurve_worker_ctx ctx = {
        .lock_fd = -1,
        .tuxedo_fd = -1,
        .sleep_inhibit_fd = -1
    };

    /* A disappeared log reader must not kill the worker via SIGPIPE. */
    struct sigaction ignore;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, NULL);

    if (fc_worker_begin(&ctx, prof) < 0)
        exit(1);
    fc_log_line("worker started: curve '%s', interval=%dms, hysteresis=%dC",
                prof->name, prof->interval_ms, prof->hysteresis);

    int last_pct = -1, last_temp = -1;
    int auto_mode = 0, read_failures = 0;
    int auto_resume_temp = FANCURVE_RESUME_TEMP;

    while (g_fancurve_running) {
        if (fc_sleep_monitor_process(&ctx) < 0) break;
        if (ctx.sleep_suspended) {
            /* PrepareForSleep(true) can arrive well before firmware enters
             * sleep. Only enable the temperature fallback once the Linux
             * suspend-aware clock confirms that the machine actually slept. */
            if (!ctx.sleep_clock_resume_seen &&
                fc_sleep_clock_confirms_resume(&ctx)) {
                ctx.sleep_clock_resume_seen = 1;
                fc_log_line("clock confirms system resumed; temperature fallback active");
            }

            /* Keep EC auto while polling for a missed resume notification. */
            int cpu = -1, gpu = -1;
            if (ctx.sleep_clock_resume_seen &&
                fc_read_temps(ctx.tuxedo_fd, &cpu, &gpu) == 0) {
                int temp = cpu > gpu ? cpu : gpu;
                fc_log_line("EC auto monitor CPU=%dC GPU=%dC -> control=%dC",
                            cpu, gpu, temp);
                if (temp > FANCURVE_SLEEP_RESUME_TEMP) {
                    fc_log_line("temperature fallback: %dC > %dC; resuming curve",
                                temp, FANCURVE_SLEEP_RESUME_TEMP);
                    ctx.sleep_resume_pending = 1;
                    if (fc_sleep_monitor_process(&ctx) < 0) break;
                    continue;
                }
            }
            if (fc_worker_sleep_ms(1000, &ctx) < 0) break;
            continue;
        }
        if (ctx.sleep_force_apply) {
            last_pct = -1;
            last_temp = -1;
            ctx.sleep_force_apply = 0;
        }

        int cpu = -1, gpu = -1;
        if (fc_read_temps(ctx.tuxedo_fd, &cpu, &gpu) < 0) {
            read_failures++;
            fc_log_line("read failed (%d/%d)", read_failures,
                        FANCURVE_IOCTL_RETRIES);
            if (read_failures >= FANCURVE_IOCTL_RETRIES && !auto_mode) {
                int restore = fc_restore_auto(&ctx, 1);
                last_pct = -1;
                last_temp = -1;
                if (restore == 0) {
                    auto_mode = 1;
                    auto_resume_temp = FANCURVE_RESUME_TEMP;
                } else {
                    break;
                }
            }
            if (fc_worker_sleep_ms(prof->interval_ms, &ctx) < 0) break;
            continue;
        }
        read_failures = 0;

        int temp = (cpu > gpu) ? cpu : gpu;
        fc_log_line("read CPU=%dC GPU=%dC -> control=%dC", cpu, gpu, temp);

        /* Critical thermal fallback: hand control to the EC. */
        if (temp > FANCURVE_HOT_TEMP) {
            fc_log_line("thermal fallback: %dC > %dC", temp, FANCURVE_HOT_TEMP);
            if (!auto_mode) {
                int restore = fc_restore_auto(&ctx, 1);
                last_pct = -1;
                last_temp = -1;
                if (restore == 0) {
                    auto_mode = 1;
                    auto_resume_temp = FANCURVE_RESUME_TEMP;
                } else {
                    break;
                }
            }
            if (fc_worker_sleep_ms(prof->interval_ms, &ctx) < 0) break;
            continue;
        }

        if (auto_mode) {
            if (temp < auto_resume_temp) {
                fc_log_line("resuming curve: %dC < %dC", temp,
                            auto_resume_temp);
                auto_mode = 0;
                last_pct = -1;
                last_temp = -1;
            } else {
                fc_log_line("auto hold: %dC (resume below %dC)", temp,
                            auto_resume_temp);
                if (fc_worker_sleep_ms(prof->interval_ms, &ctx) < 0) break;
                continue;
            }
        }

        int target = fc_interpolate(prof, temp);
        if (target == FANCURVE_FAN_AUTO) {
            int auto_temp = fc_auto_point_temp(prof);
            fc_log_line("target=EC auto (%dC >= %dC)", temp, auto_temp);
            int restore = fc_restore_auto(&ctx, 1);
            last_pct = -1;
            last_temp = -1;
            if (restore == 0) {
                auto_mode = 1;
                auto_resume_temp = auto_temp - prof->hysteresis;
                if (auto_resume_temp < 0) auto_resume_temp = 0;
            } else {
                break;
            }
            if (fc_worker_sleep_ms(prof->interval_ms, &ctx) < 0) break;
            continue;
        }

        fc_log_line("target=%d%%", target);
        int apply = 0;

        if (last_pct < 0) {
            apply = 1;
        } else if (target != last_pct) {
            int delta = temp - last_temp;
            if (delta < 0) delta = -delta;
            if (prof->hysteresis <= 0 || delta >= prof->hysteresis) {
                apply = 1;
            } else {
                fc_log_line("skip: delta=%dC < hysteresis=%dC (last %d%% at %dC)",
                            delta, prof->hysteresis, last_pct, last_temp);
            }
        } else {
            fc_log_line("skip: target unchanged (%d%%)", last_pct);
        }

        if (apply) {
            int write_ok = 0;
            int raw = (target * 255) / 100;
            for (int attempt = 1; attempt <= FANCURVE_IOCTL_RETRIES; attempt++) {
                if (fc_set_duty(ctx.tuxedo_fd, target) == 0) {
                    write_ok = 1;
                    fc_log_line("W_CL_FANSPEED %d%% (raw=%d) ok", target, raw);
                    break;
                }
                fc_log_line("W_CL_FANSPEED %d%% failed (%d/%d)", target,
                            attempt, FANCURVE_IOCTL_RETRIES);
                usleep(FANCURVE_IOCTL_RETRY_US);
            }

            if (write_ok) {
                last_pct = target;
                last_temp = temp;
            } else {
                fc_log_line("%d fan ioctl failures; restoring auto",
                            FANCURVE_IOCTL_RETRIES);
                int restore = fc_restore_auto(&ctx, 1);
                last_pct = -1;
                last_temp = -1;
                if (restore == 0) {
                    auto_mode = 1;
                    auto_resume_temp = FANCURVE_RESUME_TEMP;
                } else {
                    break;
                }
            }
        }

        if (fc_worker_sleep_ms(prof->interval_ms, &ctx) < 0) break;
    }

    fc_worker_end(&ctx);
}

static int fc_stop_worker(int quiet, int set_auto)
{
    struct fancurve_state st;
    int running = fc_is_running(&st);
    int had_worker = running;

    if (running) {
        if (!fc_worker_process_alive(&st)) running = 0;
        else if (fc_signal_worker(&st, SIGTERM) != 0 && errno != ESRCH)
            return fail("could not stop fan-curve worker PID %d: %s", st.pid, strerror(errno));

        int exited = !running;
        for (int i = 0; running && i < 100; i++) {
            usleep(20000);
            if (!fc_worker_process_alive(&st)) { exited = 1; break; }
        }
        if (!exited) {
            if (fc_worker_process_alive(&st) &&
                fc_signal_worker(&st, SIGKILL) != 0 && errno != ESRCH)
                return fail("could not kill fan-curve worker PID %d: %s", st.pid, strerror(errno));
            for (int i = 0; i < 100; i++) {
                usleep(20000);
                if (!fc_worker_process_alive(&st)) { exited = 1; break; }
            }
        }
        if (!exited)
            return fail("fan-curve worker PID %d did not exit; leaving state intact", st.pid);
    }

    int rc = 0;
    /* An active worker's armed fd has already restored auto before exit;
     * avoid a late duplicate write racing a newly started curve. */
    if (set_auto && !had_worker && fc_auto_all_quiet() != 0 && fc_auto_direct() != 0)
        rc = -1;

    unlink(FANCURVE_STATE_PATH);

    if (!quiet) {
        if (had_worker && rc == 0)
            cctl_stream_printf("Stopped fan curve '%s' [PID %d]; fans set to EC auto.\n",
                   st.name, (int)st.pid);
        else if (had_worker)
            cctl_stream_printf("Stopped fan curve '%s' [PID %d]; failed to set EC auto.\n",
                   st.name, (int)st.pid);
        else if (rc == 0)
            cctl_stream_printf("No fan curve is active. Fans set to EC auto.\n");
        else
            cctl_stream_printf("No fan curve is active; failed to set EC auto.\n");
    }
    return rc;
}

static int fc_start(const struct fancurve_profile *prof)
{
    if (!fc_drivers_ready())
        return fail("fan curves require clevo_acpi and tuxedo_io (run 'cctl drivers-manage')");

    int fd = tuxedo_open_clevo();
    if (fd < 0)
        return fail("fan control requires /dev/tuxedo_io (run 'cctl drivers-manage')");
    if (!tuxedo_io_version_ok(fd)) {
        close(fd);
        return fail("fan curves require tuxedo_io 0.3.10 or newer for crash recovery; update the driver first");
    }
    close(fd);

    pid_t pid = daemonize();
    if (pid < 0)
        return fail("failed to spawn fan-curve daemon");

    if (pid > 0) {
        int ready = 0;
        int sleep_monitor_active = -1;
        /* Worker setup can wait up to 5 seconds for the login1 inhibitor
         * reply; leave margin for startup and state-file publication. */
        for (int waited = 0; waited < FANCURVE_START_TIMEOUT_MS;
             waited += FANCURVE_START_POLL_MS) {
            struct fancurve_state st;
            if (fc_read_state(&st) == 0 && st.pid == pid &&
                strcmp(st.name, prof->name) == 0) {
                ready = 1;
                sleep_monitor_active = st.sleep_monitor;
                break;
            }
            usleep(FANCURVE_START_POLL_MS * 1000);
        }
        if (!ready) {
            kill(pid, SIGTERM);
            return fail("fan-curve daemon failed to start");
        }
        cctl_stream_printf("Started fan curve '%s' [PID %d]\n", prof->name, (int)pid);
        if (sleep_monitor_active == 0)
            cctl_stream_printf("Note: no login1-compatible sleep manager; suspend auto-restore is unavailable.\n");
        return 0;
    }

    fc_worker(prof);
    exit(0);
}

/* Shared validated restart path for the CLI and snapshot restore. */
static int fc_start_profile(const struct fancurve_profile *prof)
{
    char err[256];
    if (fancurve_validate(prof, NULL, err, sizeof(err)) < 0)
        return fail("%s", err);

    struct fancurve_state current;
    if (fc_is_running(&current) && fc_stop_worker(1, 1) != 0)
        return -1;
    return fc_start(prof);
}

/* ----------------------------------------------------------- commands */

#define FC_RULES_TEXT \
    "## Fan curve profile\n" \
    "## Replace each \"--\" with a number, keeping the quotes.\n" \
    "## Example: { \"temp\": \"45\", \"fan\": \"60\" }\n" \
    "## Auto example (final point only): { \"temp\": \"88\", \"fan\": \"a\" }\n" \
    "## The final point starts with fan set to \"a\"; change it if desired.\n" \
    "## Leave \"name\" unchanged; it must match the file name.\n" \
    "## temp = temperature in Celsius (0-95), increasing from top to bottom\n" \
    "## fan = fan speed in percent (25-100), or \"a\" for EC auto on the final point\n" \
    "## At/above the final \"a\" point, both fans go to EC auto; the curve\n" \
    "## resumes below (that temperature - hysteresis).\n" \
    "## hysteresis = temperature deadband in Celsius (0-20)\n" \
    "## interval_ms = polling interval in milliseconds (250-60000)\n" \
    "## Keep 2-16 points; add or remove point lines as needed.\n" \
    "## Agent mode: cctl fancurve create --am <name> <points> <hysteresis> <ms> <temp1> <fan1> ...\n"

static char *fc_make_template(const char *name, int npoints)
{
    char escaped[FANCURVE_NAME_MAX * 2 + 1];
    size_t e = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p == '"' || *p == '\\') {
            escaped[e++] = '\\';
            escaped[e++] = (char)*p;
        } else {
            escaped[e++] = (char)*p;
        }
    }
    escaped[e] = '\0';

    size_t cap = 1536 + (size_t)npoints * 80;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    size_t off = 0;
    int n = snprintf(buf, cap,
                     FC_RULES_TEXT
                     "{\n"
                     "  \"name\": \"%s\",\n"
                     "  \"hysteresis\": %d,\n"
                     "  \"interval_ms\": %d,\n"
                     "  \"points\": [\n",
                     escaped, FANCURVE_DEFAULT_HYSTERESIS, FANCURVE_DEFAULT_INTERVAL);
    if (n < 0 || (size_t)n >= cap) { free(buf); return NULL; }
    off = (size_t)n;

    for (int i = 0; i < npoints; i++) {
        n = snprintf(buf + off, cap - off,
                     "    { \"temp\": \"--\", \"fan\": \"%s\" }%s\n",
                     i + 1 == npoints ? "a" : "--",
                     i + 1 < npoints ? "," : "");
        if (n < 0 || (size_t)n >= cap - off) { free(buf); return NULL; }
        off += (size_t)n;
    }

    n = snprintf(buf + off, cap - off, "  ]\n}\n");
    if (n < 0 || (size_t)n >= cap - off) { free(buf); return NULL; }
    return buf;
}

static int fc_prompt_points(void)
{
    for (;;) {
        cctl_stream_printf("How many points? (%d-%d): ", FANCURVE_MIN_POINTS, FANCURVE_MAX_POINTS);
        fflush(stdout);
        char line[64];
        if (!fgets(line, sizeof(line), stdin))
            return -1;
        int n;
        if (safe_atoi(line, &n) == 0 && n >= FANCURVE_MIN_POINTS &&
            n <= FANCURVE_MAX_POINTS)
            return n;
        cctl_stream_printf("Enter a number between %d and %d.\n",
               FANCURVE_MIN_POINTS, FANCURVE_MAX_POINTS);
    }
}

static void fc_print_status(const char *requested_name)
{
    struct fancurve_state st;

    if (json_output) {
        if (fc_is_running(&st))
            row("Fan Curve:", "%s (active; PID %d)", st.name, (int)st.pid);
        else
            row("Fan Curve:", "none (stopped)");
        return;
    }

    if (requested_name)
        cctl_stream_printf("%sFan Curve:%s %s\n", C_YLW, C_RST, requested_name);
    else
        cctl_stream_printf("%sFan Curve:%s\n", C_YLW, C_RST);

    if (fc_is_running(&st)) {
        if (!requested_name)
            cctl_stream_printf("  %sStatus:%s              %s%s%s (active, PID %d)\n",
                   C_CYN, C_RST, C_GRN, st.name, C_RST, (int)st.pid);
        else if (strcmp(st.name, requested_name) == 0)
            cctl_stream_printf("  %sStatus:%s              %sactive%s [PID %d]\n",
                   C_CYN, C_RST, C_GRN, C_RST, (int)st.pid);
        else
            cctl_stream_printf("  %sStatus:%s              %sinactive%s (active curve: %s%s%s [PID %d])\n",
                   C_CYN, C_RST, C_DIM, C_RST, C_GRN, st.name, C_RST, (int)st.pid);
    } else {
        if (requested_name)
            cctl_stream_printf("  %sStatus:%s              %sinactive%s\n",
                   C_CYN, C_RST, C_DIM, C_RST);
        else
            cctl_stream_printf("  %sStatus:%s              %snone%s (stopped)\n",
                   C_CYN, C_RST, C_DIM, C_RST);
    }
    cctl_stream_printf("\n");
}

static void fc_print_status_row(void)
{
    struct fancurve_state st;
    if (fc_is_running(&st)) {
        if (json_output)
            row("Fan Curve:", "%s (active, PID %d)", st.name, (int)st.pid);
        else
            row("Fan Curve:", "%s%s%s (active, PID %d)",
                C_GRN, st.name, C_RST, (int)st.pid);
    } else if (json_output) {
        row("Fan Curve:", "none (stopped)");
    } else {
        row("Fan Curve:", "%snone (stopped)%s", C_DIM, C_RST);
    }
}

static int fc_list_profiles(void)
{
    int prev = g_stream_mode;
    g_stream_mode = STREAM_MODE_LINE;
    g_stream_line = 0;

    fc_print_status(NULL);

    struct dirent **list = NULL;
    int n = scandir(FANCURVE_DIR, &list, NULL, alphasort);
    int shown = 0;

    cctl_stream_printf("%sSaved Fan Curves:%s\n", C_YLW, C_RST);
    if (n < 0) {
        cctl_stream_printf("No fan-curve profiles yet.\n");
    } else {
        for (int i = 0; i < n; i++) {
            const char *name = list[i]->d_name;
            size_t len = strlen(name);
            if (len <= 5 || strcmp(name + len - 5, ".json") != 0 ||
                name[0] == '.') {
                free(list[i]);
                continue;
            }
            cctl_stream_printf("%s%s%.*s%s", shown ? ", " : "  ", C_CYN,
                   (int)(len - 5), name, C_RST);
            shown++;
            free(list[i]);
        }
        free(list);
    }

    if (n >= 0) {
        if (shown) cctl_stream_putchar('\n');
        else cctl_stream_printf("No fan-curve profiles yet.\n");
    }

    g_stream_mode = prev;
    return 0;
}

static void fc_print_usage(void)
{
    int prev = g_stream_mode;
    g_stream_mode = STREAM_MODE_LINE;
    g_stream_line = 0;

    cctl_stream_printf("%sUsage:%s %scctl fancurve <action> [name]%s\n\n",
           C_BLD, C_RST, C_CYN_BLD, C_RST);
    cctl_stream_printf("%sActions:%s\n", C_YLW, C_RST);
    cctl_stream_printf("  %s%-19s%s Stop the daemon and return both fans to EC automatic control\n",
           C_CYN, "stop|st", C_RST);
    cctl_stream_printf("  %s%-19s%s Start a curve (restarts a running curve)\n",
           C_CYN, "start|s <name>", C_RST);
    cctl_stream_printf("  %s%-19s%s Create a profile: prompt for point count, then open the editor\n",
           C_CYN, "create|c <name>", C_RST);
    cctl_stream_printf("  %s%-19s%s Validate and show one saved profile\n", C_CYN, "view|v <name>", C_RST);
    cctl_stream_printf("  %s%-19s%s Edit a profile in the editor\n", C_CYN, "edit|e <name>", C_RST);
    cctl_stream_printf("  %s%-19s%s Delete an inactive profile (--noconfirm skips the prompt)\n",
           C_CYN, "del <name>", C_RST);
    cctl_stream_printf("  %s%-19s%s Follow worker diagnostics across starts/stops (Ctrl-C to stop)\n",
           C_CYN, "log", C_RST);

    g_stream_mode = prev;
}

static const char *fc_log_color(const char *line)
{
    if (strstr(line, "failed") || strstr(line, "failure")) return C_RED;
    if (strstr(line, "fallback") || strstr(line, "auto hold") ||
        strstr(line, "resuming curve")) return C_YLW;
    if (strstr(line, "worker started")) return C_CYN;
    if (strstr(line, "worker stopped") || strstr(line, "auto restored")) return C_GRN;
    if (strstr(line, "skip:")) return C_DIM;
    if (strstr(line, "target=") || strstr(line, "W_CL_FANSPEED")) return C_MAG;
    return C_WHT;
}

static void fc_print_log_line(const char *line, size_t len)
{
    char text[1024];
    if (len >= sizeof(text)) len = sizeof(text) - 1;
    memcpy(text, line, len);
    text[len] = '\0';
    cctl_stream_printf("%s%.*s%s", fc_log_color(text), (int)len, line, C_RST);
}

static int fc_cmd_start(int argc, char **argv)
{
    if (argc != 4)
        return fail("Usage: cctl fancurve start <name>");

    const char *name = argv[3];
    struct fancurve_profile prof;
    char err[256];
    if (fc_load_profile(name, &prof, err, sizeof(err)) < 0)
        return fail("%s", err);

    require_root(argc, argv);

    return fc_start_profile(&prof);
}

static int fc_cmd_stop(int argc, char **argv)
{
    if (argc != 3)
        return fail("Usage: cctl fancurve stop");
    require_root(argc, argv);
    return fc_stop_worker(0, 1) == 0 ? 0 : 1;
}

static void fc_print_shell_quoted(const char *s)
{
    cctl_stream_putchar('\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'')
            cctl_stream_printf("'\\''");
        else
            cctl_stream_putchar(*p);
    }
    cctl_stream_putchar('\'');
}

static int fc_agent_error(const char *fmt, ...)
{
    va_list ap;
    cctl_stream_fprintf(stderr, "Invalid fan curve: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 1;
}

static int fc_cmd_create_agent(int argc, char **argv)
{
    const char *pos[64];
    int npos = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--am") == 0) continue;
        if (npos >= (int)(sizeof(pos) / sizeof(pos[0])))
            return fc_agent_error("too many arguments");
        pos[npos++] = argv[i];
    }

    if (npos == 0) {
        fputs(FC_RULES_TEXT, stdout);
        fflush(stdout);
        return 0;
    }
    if (npos < 4) {
        cctl_stream_fprintf(stderr, "Usage: cctl fancurve create --am <name> <points> <hysteresis> <ms> <temp1> <fan1> ...\n");
        return 1;
    }

    const char *name = pos[0];
    if (!fancurve_name_valid(name))
        return fc_agent_error("invalid profile name '%.64s'", name);

    int npoints = 0;
    if (safe_atoi(pos[1], &npoints) < 0 ||
        npoints < FANCURVE_MIN_POINTS || npoints > FANCURVE_MAX_POINTS)
        return fc_agent_error("points must contain %d-%d entries",
                              FANCURVE_MIN_POINTS, FANCURVE_MAX_POINTS);

    if (npos != 4 + npoints * 2)
        return fc_agent_error("expected %d temperature/fan pairs after <hysteresis> <ms>",
                              npoints);

    struct fancurve_profile prof;
    memset(&prof, 0, sizeof(prof));
    snprintf(prof.name, sizeof(prof.name), "%s", name);
    prof.npoints = npoints;
    if (safe_atoi(pos[2], &prof.hysteresis) < 0) prof.hysteresis = -1;
    if (safe_atoi(pos[3], &prof.interval_ms) < 0) prof.interval_ms = -1;

    for (int i = 0; i < npoints; i++) {
        int temp = -1;
        int fan = 0;
        if (safe_atoi(pos[4 + i * 2], &temp) < 0) temp = -1;
        const char *fan_text = pos[5 + i * 2];
        if (strcmp(fan_text, "a") == 0 || strcmp(fan_text, "auto") == 0) {
            fan = FANCURVE_FAN_AUTO;
        } else if (safe_atoi(fan_text, &fan) < 0) {
            fan = 0;
        }
        prof.points[i].temp = temp;
        prof.points[i].fan = fan;
    }

    char err[256];
    if (fancurve_validate(&prof, name, err, sizeof(err)) < 0)
        return fc_agent_error("%s", err);

    require_root(argc, argv);
    if (fancurve_profile_exists(name))
        return fail("fan curve '%s' already exists; view and delete it before creating it again", name);

    char escaped[FANCURVE_NAME_MAX * 2 + 1];
    size_t e = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p == '"' || *p == '\\') escaped[e++] = '\\';
        escaped[e++] = (char)*p;
    }
    escaped[e] = '\0';

    size_t cap = 512 + (size_t)npoints * 80;
    char *text = malloc(cap);
    if (!text) return fail("out of memory");
    size_t off = 0;
    int n = snprintf(text, cap,
                     "{\n  \"name\": \"%s\",\n  \"hysteresis\": %d,\n"
                     "  \"interval_ms\": %d,\n  \"points\": [\n",
                     escaped, prof.hysteresis, prof.interval_ms);
    if (n < 0 || (size_t)n >= cap) { free(text); return fail("out of memory"); }
    off = (size_t)n;

    for (int i = 0; i < npoints; i++) {
        if (prof.points[i].fan == FANCURVE_FAN_AUTO) {
            n = snprintf(text + off, cap - off,
                         "    { \"temp\": \"%d\", \"fan\": \"a\" }%s\n",
                         prof.points[i].temp, i + 1 < npoints ? "," : "");
        } else {
            n = snprintf(text + off, cap - off,
                         "    { \"temp\": \"%d\", \"fan\": \"%d\" }%s\n",
                         prof.points[i].temp, prof.points[i].fan,
                         i + 1 < npoints ? "," : "");
        }
        if (n < 0 || (size_t)n >= cap - off) { free(text); return fail("out of memory"); }
        off += (size_t)n;
    }
    n = snprintf(text + off, cap - off, "  ]\n}\n");
    if (n < 0 || (size_t)n >= cap - off) { free(text); return fail("out of memory"); }
    off += (size_t)n;

    if (fc_write_profile(name, text, off, err, sizeof(err)) < 0) {
        free(text);
        return fail("%s", err);
    }
    free(text);
    cctl_stream_printf("Created fan curve '%s'.\n", name);
    cctl_stream_printf("Activate it with: cctl fancurve start ");
    fc_print_shell_quoted(name);
    cctl_stream_putchar('\n');
    return 0;
}

static int fc_cmd_create(int argc, char **argv)
{
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--am") == 0)
            return fc_cmd_create_agent(argc, argv);
    }
    if (argc != 4)
        return fail("Usage: cctl fancurve create <name>");

    const char *name = argv[3];
    if (!fancurve_name_valid(name))
        return fail("invalid profile name '%s'", name);

    if (fancurve_profile_exists(name)) {
        cctl_stream_fprintf(stderr, "Warning: fan curve '%s' already exists. Replace it? [y/N] ", name);
        fflush(stderr);
        if (!isatty(STDIN_FILENO)) {
            cctl_stream_fprintf(stderr, "\nCancelled. Run this command in a terminal to confirm replacement.\n");
            return 1;
        }
        char answer[32];
        if (!fgets(answer, sizeof(answer), stdin)) {
            cctl_stream_puts("Cancelled; the existing fan curve was kept.");
            return 1;
        }
        answer[strcspn(answer, "\r\n")] = '\0';
        if (strcasecmp(answer, "y") != 0 && strcasecmp(answer, "yes") != 0) {
            cctl_stream_puts("Cancelled; the existing fan curve was kept.");
            return 1;
        }
    }

    int npoints = fc_prompt_points();
    if (npoints < 0)
        return fail("no point count supplied");

    char *template = fc_make_template(name, npoints);
    if (!template)
        return fail("out of memory");

    cctl_stream_printf("Opening editor for %s/%s.json ...\n", FANCURVE_DIR, name);
    fflush(stdout);

    char err[256];
    int rc = fc_edit_and_save(template, name, err, sizeof(err));
    free(template);
    if (rc < 0)
        return fail("%s", err);

    cctl_stream_printf("Created fan curve '%s'.\n", name);
    cctl_stream_printf("Activate it with: cctl fancurve start ");
    fc_print_shell_quoted(name);
    cctl_stream_putchar('\n');
    return 0;
}

static int fc_cmd_edit(int argc, char **argv)
{
    if (argc != 4)
        return fail("Usage: cctl fancurve edit <name>");

    const char *name = argv[3];
    char path[PATH_MAX];
    if (fancurve_profile_path(name, path, sizeof(path)) < 0)
        return fail("invalid profile name '%s'", name);

    if (geteuid() != 0 && access(FANCURVE_STATE_PATH, F_OK) == 0)
        require_root(argc, argv);

    struct fancurve_state st;
    if (fc_is_running(&st) && strcmp(st.name, name) == 0)
        return fail("fan curve '%s' is active; run 'cctl fancurve stop' first", name);

    char err[256];
    char *text = fc_read_text(path, err, sizeof(err));
    if (!text)
        return fail("%s", err);

    cctl_stream_printf("Opening editor for %s ...\n", path);
    fflush(stdout);

    int rc = fc_edit_and_save(text, name, err, sizeof(err));
    free(text);
    if (rc < 0)
        return fail("%s", err);

    cctl_stream_printf("Updated fan curve '%s'.\n", name);

    if (fc_is_running(&st) && strcmp(st.name, name) == 0) {
        cctl_stream_printf("This curve is active; run: cctl fancurve start ");
        fc_print_shell_quoted(name);
        cctl_stream_printf(" to apply the new points.\n");
    }
    return 0;
}

static int fc_cmd_view(int argc, char **argv)
{
    if (argc != 4)
        return fail("Usage: cctl fancurve view <name>");

    if (geteuid() != 0 && access(FANCURVE_STATE_PATH, F_OK) == 0)
        require_root(argc, argv);

    const char *name = argv[3];
    struct fancurve_profile prof;
    char err[256];
    if (fc_load_profile(name, &prof, err, sizeof(err)) < 0)
        return fail("%s", err);

    char path[PATH_MAX];
    fancurve_profile_path(name, path, sizeof(path));

    int prev = g_stream_mode;
    g_stream_mode = STREAM_MODE_LINE;
    g_stream_line = 0;

    fc_print_status(prof.name);
    cctl_stream_printf("  Hysteresis: %d C\n", prof.hysteresis);
    cctl_stream_printf("  Interval:   %d ms\n", prof.interval_ms);
    cctl_stream_printf("  Points:     %d\n", prof.npoints);
    cctl_stream_printf("  %-8s %s\n", "Temp", "Fan");
    cctl_stream_printf("  %-8s %s\n", "────", "───");
    for (int i = 0; i < prof.npoints; i++) {
        if (prof.points[i].fan == FANCURVE_FAN_AUTO)
            cctl_stream_printf("  %3d C    auto\n", prof.points[i].temp);
        else
            cctl_stream_printf("  %3d C    %3d%%\n", prof.points[i].temp, prof.points[i].fan);
    }
    cctl_stream_printf("  Source:     %s\n", path);

    g_stream_mode = prev;
    return 0;
}

static int fc_cmd_log(int argc, char **argv)
{
    if (argc != 3)
        return fail("Usage: cctl fancurve log");

    require_root(argc, argv);

    struct fancurve_state watched;
    int attached = fc_is_running(&watched);

    struct stat sb;
    if (lstat(FANCURVE_LOG_FIFO, &sb) == 0) {
        if (!S_ISFIFO(sb.st_mode) || sb.st_uid != 0)
            return fail("%s is not a safe root-owned FIFO", FANCURVE_LOG_FIFO);
        if (unlink(FANCURVE_LOG_FIFO) != 0)
            return fail("cannot remove stale log FIFO: %s", strerror(errno));
    } else if (errno != ENOENT) {
        return fail("cannot inspect %s: %s", FANCURVE_LOG_FIFO, strerror(errno));
    }

    if (mkfifo(FANCURVE_LOG_FIFO, 0600) != 0)
        return fail("cannot create log FIFO: %s", strerror(errno));
    if (chmod(FANCURVE_LOG_FIFO, 0600) != 0) {
        unlink(FANCURVE_LOG_FIFO);
        return fail("cannot secure log FIFO: %s", strerror(errno));
    }

    int fd = open(FANCURVE_LOG_FIFO, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) {
        unlink(FANCURVE_LOG_FIFO);
        return fail("cannot open log FIFO: %s", strerror(errno));
    }

    g_fancurve_running = 1;
    fc_install_signal_handlers();

    int prev_mode = g_stream_mode;
    g_stream_mode = STREAM_MODE_LINE;
    g_stream_line = 0;

    if (attached) {
        cctl_stream_printf("%sFan Curve Log:%s %s%s%s [PID %d]\n",
               C_YLW, C_RST, C_GRN, watched.name, C_RST, (int)watched.pid);
    } else {
        cctl_stream_printf("%sFan Curve Log:%s waiting for a fan curve to start...\n",
               C_YLW, C_RST);
    }
    cctl_stream_printf("%sLive worker activity; earlier messages are not saved. Press Ctrl-C to stop.%s\n\n",
           C_DIM, C_RST);
    fflush(stdout);

    char buf[1024], line[1024];
    size_t line_len = 0;
    int ticks = 0;
    while (g_fancurve_running) {
        if (!attached) {
            struct fancurve_state current;
            if (fc_is_running(&current)) {
                watched = current;
                attached = 1;
                ticks = 0;
                cctl_stream_printf("\n%sFan curve started:%s %s%s%s [PID %d]\n",
                       C_YLW, C_RST, C_GRN, watched.name, C_RST, (int)watched.pid);
                fflush(stdout);
            }
        }

        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (line_len == sizeof(line) - 1) {
                    fc_print_log_line(line, line_len);
                    line_len = 0;
                }
                line[line_len++] = buf[i];
                if (buf[i] == '\n') {
                    fc_print_log_line(line, line_len);
                    line_len = 0;
                }
            }
            fflush(stdout);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;

        if (fc_sleep_ms(100) < 0) break;
        if (attached && ++ticks >= 10) {
            ticks = 0;
            struct fancurve_state current;
            if (!fc_is_running(&current) ||
                current.pid != watched.pid ||
                current.starttime != watched.starttime) {
                if (line_len) {
                    fc_print_log_line(line, line_len);
                    line_len = 0;
                }
                cctl_stream_printf("\n%sFan curve stopped.%s Waiting for the next fan curve to start...\n",
                       C_YLW, C_RST);
                cctl_stream_printf("%sPress Ctrl-C to exit.%s\n", C_DIM, C_RST);
                fflush(stdout);
                attached = 0;
                ticks = 0;
            }
        }
    }

    if (line_len)
        fc_print_log_line(line, line_len);
    g_stream_mode = prev_mode;
    close(fd);
    unlink(FANCURVE_LOG_FIFO);
    return 0;
}

static int fc_cmd_del(int argc, char **argv)
{
    int noconfirm = 0;
    if (argc == 5 && strcmp(argv[4], "--noconfirm") == 0)
        noconfirm = 1;
    else if (argc != 4)
        return fail("Usage: cctl fancurve del <name> [--noconfirm]");

    const char *name = argv[3];
    char path[PATH_MAX];
    if (fancurve_profile_path(name, path, sizeof(path)) < 0)
        return fail("invalid profile name '%s'", name);

    require_root(argc, argv);

    struct fancurve_state st;
    if (fc_is_running(&st) && strcmp(st.name, name) == 0)
        return fail("fan curve '%s' is active; run 'cctl fancurve stop' first", name);

    struct stat sb;
    if (lstat(path, &sb) != 0)
        return fail("fan curve '%s' does not exist", name);

    if (!noconfirm && !cctl_confirm_delete("fan curve", name))
        return 1;

    if (unlink(path) != 0)
        return fail("cannot delete '%s': %s", name, strerror(errno));

    cctl_stream_printf("Deleted fan curve '%s'.\n", name);
    return 0;
}

static int cmd_fancurve(int argc, char **argv)
{
    if (argc < 3) {
        require_root(argc, argv);
        int rc = fc_list_profiles();
        if (rc == 0)
            cctl_stream_printf("\n%sHelp:%s cctl fanc -h\n", C_DIM, C_RST);
        return rc;
    }

    char *action = argv[2];
    for (char *p = action; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    if (strcmp(action, "__install") == 0)
        return fc_cmd_install_helper(argc, argv);
    if (strcmp(action, "log") == 0 || strcmp(action, "--log") == 0)
        return fc_cmd_log(argc, argv);
    if (strcmp(action, "help") == 0 || strcmp(action, "--help") == 0 ||
        strcmp(action, "-h") == 0) {
        fc_print_usage();
        return 0;
    }
    if (strcmp(action, "stop") == 0 || strcmp(action, "st") == 0 ||
        strcmp(action, "off") == 0)
        return fc_cmd_stop(argc, argv);
    if (strcmp(action, "start") == 0 || strcmp(action, "s") == 0 ||
        strcmp(action, "set") == 0)
        return fc_cmd_start(argc, argv);
    if (strcmp(action, "create") == 0 || strcmp(action, "c") == 0)
        return fc_cmd_create(argc, argv);
    if (strcmp(action, "view") == 0 || strcmp(action, "v") == 0)
        return fc_cmd_view(argc, argv);
    if (strcmp(action, "edit") == 0 || strcmp(action, "e") == 0)
        return fc_cmd_edit(argc, argv);
    if (strcmp(action, "del") == 0 || strcmp(action, "delete") == 0)
        return fc_cmd_del(argc, argv);

    fc_print_usage();
    return fail("unknown fan-curve action '%s'", action);
}
