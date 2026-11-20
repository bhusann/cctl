/*
 * cctl-gui — browser control panel server (fork addition, not upstream cctl).
 *
 * A small HTTP/1.1 server on 127.0.0.1. Every API action maps to a real cctl
 * command executed in a forked child of this same binary (so EC/sysfs/sudoers
 * semantics stay exactly as the CLI enforces them). Read-only endpoints that
 * need no command execution (settings, profiles, curves, presets, snapshots
 * listing, cpufreq option lists) read cctl's own data files in-process.
 *
 * Requires: CCTL_GUI. Must be included after keyboard/fancurve/snapshot incs
 * (uses kbd_presets, kbe_effects, fc_is_running, cctl_settings_read) and
 * before cctl_commands.inc (cmd_gui is referenced by the command table).
 */

#ifdef CCTL_GUI

#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/signalfd.h>

#define GUI_DEFAULT_PORT   8723
#define GUI_BODY_MAX       16384
#define GUI_CHILD_OUT_MAX  65536
#define GUI_CHILD_TIMEOUT  20

/* Binary the API children exec. As root this is this binary; unprivileged
 * servers delegate to the installed cctl, whose sudoers rule lets children
 * elevate passwordlessly (the fork's ./cctl has no rule until installed). */
static char g_gui_child_path[PATH_MAX];

/* ------------------------------------------------------------ string buf */

struct gui_buf {
    char *p;
    size_t len, cap;
};

static void gui_buf_init(struct gui_buf *b)
{
    b->p = NULL; b->len = 0; b->cap = 0;
}

/* Truncating copy without format-truncation churn: pointers into the raw
 * request buffer have unknown length to the compiler. */
static void gui_copy_str(char *dst, size_t dstsz, const char *src)
{
    size_t l = strlen(src);
    if (l > dstsz - 1) l = dstsz - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

static int gui_buf_append(struct gui_buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) return -1;
        b->p = np; b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static int gui_buf_appendf(struct gui_buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(tmp)) return gui_buf_append(b, tmp, (size_t)n);
    char *big = malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int rc = gui_buf_append(b, big, (size_t)n);
    free(big);
    return rc;
}

static int gui_json_string(struct gui_buf *b, const char *s)
{
    gui_buf_append(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc = 0;
        switch (*p) {
            case '"':  esc = '"';  break;
            case '\\': esc = '\\'; break;
            case '\n': esc = 'n';  break;
            case '\r': esc = 'r';  break;
            case '\t': esc = 't';  break;
            default: break;
        }
        if (esc) {
            if (gui_buf_append(b, "\\", 1) < 0 || gui_buf_append(b, &esc, 1) < 0) return -1;
        } else if (*p < 0x20) {
            if (gui_buf_appendf(b, "\\u%04x", *p) < 0) return -1;
        } else {
            if (gui_buf_append(b, (const char *)p, 1) < 0) return -1;
        }
    }
    return gui_buf_append(b, "\"", 1);
}

static int gui_cmd_result(struct gui_buf *b, int rc,
                          const struct gui_buf *out, const struct gui_buf *err)
{
    if (gui_buf_appendf(b, "{\"rc\":%d,\"stdout\":", rc) < 0) return -1;
    if (gui_json_string(b, out->p ? out->p : "") < 0) return -1;
    if (gui_buf_append(b, ",\"stderr\":", strlen(",\"stderr\":")) < 0) return -1;
    if (gui_json_string(b, err->p ? err->p : "") < 0) return -1;
    return gui_buf_append(b, "}", 1);
}

/* Child: pass stdout/stderr back; stdin from /dev/null so interactive
 * helpers never block the server. */
/* Daemon-spawning commands (fancurve start, kbe) exit promptly but may leave
 * grandchildren holding the pipe open, so EOF is not a reliable completion
 * signal: stop once the direct child is gone and the pipes are drained. */
static int gui_child(char **argv, struct gui_buf *out,
                     struct gui_buf *err, int *rc)
{
    int cnt = 0;
    while (argv[cnt]) cnt++;
    char **ea = malloc((size_t)(cnt + 2) * sizeof(char *));
    if (!ea) return -1;
    ea[0] = g_gui_child_path;
    for (int i = 0; i < cnt; i++) ea[i + 1] = argv[i];
    ea[cnt + 1] = NULL;

    int po[2], pe[2];
    if (pipe(po) != 0 || pipe(pe) != 0) { free(ea); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        free(ea);
        close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(po[1], STDOUT_FILENO);
        dup2(pe[1], STDERR_FILENO);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
        execv(g_gui_child_path, ea);
        _exit(127);
    }
    free(ea);

    close(po[1]); close(pe[1]);

    int flags = fcntl(po[0], F_GETFL, 0);
    int flags2 = fcntl(pe[0], F_GETFL, 0);
    if (flags >= 0) fcntl(po[0], F_SETFL, flags | O_NONBLOCK);
    if (flags2 >= 0) fcntl(pe[0], F_SETFL, flags2 | O_NONBLOCK);

    int fds[2] = { po[0], pe[0] };
    struct gui_buf *bufs[2] = { out, err };
    int eof[2] = { 0, 0 };
    int child_done = 0;
    int status = 0;
    int timed_out = 0;
    time_t deadline = time(NULL) + GUI_CHILD_TIMEOUT;

    for (;;) {
        struct pollfd pf[2];
        int nf = 0;
        for (int i = 0; i < 2; i++) {
            if (!eof[i]) { pf[nf].fd = fds[i]; pf[nf].events = POLLIN; nf++; }
        }
        if (nf == 0) break;

        int ms = 200;
        time_t now = time(NULL);
        if (now >= deadline) { timed_out = 1; break; }
        if (child_done && nf > 0) ms = 100; /* drain grace after child exit */

        int pr = poll(pf, nf, ms);
        if (pr < 0 && errno != EINTR) break;
        if (pr == 0 && child_done) break; /* child gone, pipes idle: drained */
        for (int i = 0; pr > 0 && i < nf; i++) {
            if (!(pf[i].revents & (POLLIN | POLLHUP))) continue;
            char chunk[4096];
            ssize_t n = read(pf[i].fd, chunk, sizeof(chunk));
            if (n > 0) {
                struct gui_buf *b = bufs[i];
                size_t room = GUI_CHILD_OUT_MAX - b->len;
                size_t take = (size_t)n < room ? (size_t)n : room;
                if (take > 0) gui_buf_append(b, chunk, take);
            } else if (n == 0) {
                eof[i] = 1;
            } else if (errno == EAGAIN) {
                if (child_done) eof[i] = 1;
            } else if (errno != EINTR) {
                eof[i] = 1;
            }
        }

        if (!child_done) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) child_done = 1;
            else if (w < 0 && errno != EINTR) child_done = 1;
        }

        if (eof[0] && eof[1]) break;
        if (child_done && eof[0] && eof[1]) break;
    }

    close(po[0]); close(pe[0]);

    if (timed_out) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        *rc = -1;
        gui_buf_appendf(err, "cctl command timed out after %d s", GUI_CHILD_TIMEOUT);
    } else if (child_done) {
        *rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    } else {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        *rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return 0;
}

/* ------------------------------------------------------------ validation */

static int gui_name_ok(const char *s, size_t max)
{
    if (!s || !*s || strlen(s) > max) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') &&
            !(*p >= '0' && *p <= '9') && *p != '-' && *p != '_')
            return 0;
    }
    return 1;
}

/* Command arguments: no spaces, no control chars. execv never uses a shell,
 * so this is defense-in-depth against weird payloads. */
static int gui_arg_ok(const char *s)
{
    size_t len = strlen(s);
    if (!len || len > 64) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') &&
            !(*p >= '0' && *p <= '9') && *p != '-' && *p != '_' &&
            *p != '.' && *p != '#')
            return 0;
    }
    return 1;
}

static const char *const gui_run_whitelist[] = {
    "status", "cpumon", "gpumon", "set", "editconf",
    "kbc", "kbb", "kbe", "kbn", "fn",
    "fan", "fancurve", "fanc",
    "mux", "webcam", "mic", "bat",
    "rr", "scale", "turbo", "gov", "epp", "rapl",
    "nvidia", "snapshot", "snap", "options", NULL
};

static int gui_cmd_allowed(const char *cmd)
{
    for (const char *const *w = gui_run_whitelist; *w; w++)
        if (strcmp(cmd, *w) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------ request */

struct gui_req {
    char method[8];
    char path[128];
    char query[256];
    char body[GUI_BODY_MAX];
    size_t body_len;
    char x_cctl[16];
};

static int gui_hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int gui_url_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '%' && r[0] && r[1] && r[2]) {
            int h = gui_hex(r[1]), l = gui_hex(r[2]);
            if (h >= 0 && l >= 0) {
                *w++ = (char)(h * 16 + l);
                r += 2;
                continue;
            }
        }
        if (*r == '+') { *w++ = ' '; continue; }
        *w++ = *r;
    }
    *w = '\0';
    return 0;
}

/* First value of `key` in an urlencoded form body or query string. */
static int gui_form_get(const char *src, const char *key, char *out, size_t out_sz)
{
    size_t klen = strlen(key);
    const char *p = src;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg_len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', seg_len);
        if (eq && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            size_t vlen = seg_len - klen - 1;
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out, eq + 1, vlen);
            out[vlen] = '\0';
            gui_url_decode(out);
            return 0;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return -1;
}

/* All values of `key`, in order, up to n. */
static int gui_form_get_all(const char *src, const char *key,
                            char (*out)[64], int n)
{
    size_t klen = strlen(key);
    int found = 0;
    const char *p = src;
    while (*p && found < n) {
        const char *amp = strchr(p, '&');
        size_t seg_len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', seg_len);
        if (eq && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            size_t vlen = seg_len - klen - 1;
            if (vlen >= 64) vlen = 63;
            memcpy(out[found], eq + 1, vlen);
            out[found][vlen] = '\0';
            gui_url_decode(out[found]);
            found++;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return found;
}

static int gui_read_request(int fd, struct gui_req *req)
{
    char raw[4096];
    size_t raw_len = 0;
    size_t header_end = 0;

    for (;;) {
        ssize_t n = recv(fd, raw + raw_len, sizeof(raw) - raw_len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return n == 0 ? 0 : -1; /* EOF with no full headers: drop */
        }
        raw_len += (size_t)n;
        raw[raw_len < sizeof(raw) ? raw_len : sizeof(raw) - 1] = '\0';

        char *sep = strstr(raw, "\r\n\r\n");
        if (sep) { header_end = (size_t)(sep - raw) + 4; break; }
        if (raw_len >= sizeof(raw)) return -1;
    }

    /* Request line */
    char *nl = strchr(raw, '\r');
    if (!nl) return -1;
    *nl = '\0';
    char *sp1 = strchr(raw, ' ');
    if (!sp1) return -1;
    *sp1 = '\0';
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return -1;
    *sp2 = '\0';

    gui_copy_str(req->method, sizeof(req->method), raw);
    char *q = strchr(sp1 + 1, '?');
    if (q) {
        *q = '\0';
        gui_copy_str(req->query, sizeof(req->query), q + 1);
    } else {
        req->query[0] = '\0';
    }
    gui_copy_str(req->path, sizeof(req->path), sp1 + 1);
    req->x_cctl[0] = '\0';

    long clen = 0;
    char *h = nl + 1;
    while (h < raw + header_end) {
        char *hnl = strstr(h, "\r\n");
        if (!hnl) break;
        *hnl = '\0';
        if (!strncasecmp(h, "content-length:", 15))
            clen = strtol(h + 15, NULL, 10);
        else if (!strncasecmp(h, "x-cctl:", 7))
            gui_copy_str(req->x_cctl, sizeof(req->x_cctl), h + 7);
        h = hnl + 2;
    }
    if (clen < 0 || clen > GUI_BODY_MAX) return -1;

    size_t have = raw_len - header_end;
    req->body_len = have < (size_t)clen ? have : (size_t)clen;
    memcpy(req->body, raw + header_end, req->body_len);
    req->body[req->body_len] = '\0';

    while (req->body_len < (size_t)clen) {
        ssize_t n = recv(fd, req->body + req->body_len,
                         (size_t)clen - req->body_len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return n == 0 ? 0 : -1;
        }
        req->body_len += (size_t)n;
        req->body[req->body_len] = '\0';
    }
    return 1;
}

static void gui_send(int fd, int status, const char *ctype, struct gui_buf *body)
{
    const char *reason = status == 200 ? "OK"
                       : status == 400 ? "Bad Request"
                       : status == 404 ? "Not Found"
                       : status == 405 ? "Method Not Allowed"
                       : status == 408 ? "Request Timeout"
                       : status == 500 ? "Internal Server Error"
                       : "Error";
    struct gui_buf head;
    gui_buf_init(&head);
    gui_buf_appendf(&head,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status, reason, ctype, body->len);

    const char *chunks[2] = { head.p, body->p };
    size_t lens[2] = { head.len, body->len };
    for (int i = 0; i < 2; i++) {
        size_t off = 0;
        while (off < lens[i]) {
            ssize_t n = send(fd, chunks[i] + off, lens[i] - off, MSG_NOSIGNAL);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            off += (size_t)n;
        }
    }
    free(head.p);
}

static void gui_send_json(int fd, int status, struct gui_buf *body)
{
    gui_send(fd, status, "application/json", body);
}

static void gui_send_error(int fd, int status, const char *msg)
{
    struct gui_buf b;
    gui_buf_init(&b);
    gui_buf_appendf(&b, "{\"error\":");
    gui_json_string(&b, msg);
    gui_buf_append(&b, "}", 1);
    gui_send_json(fd, status, &b);
    free(b.p);
}

/* ------------------------------------------------------------ in-process */

static void gui_options_json(struct gui_buf *b)
{
    struct cctl_settings st;
    cctl_settings_read(&st);
    gui_buf_appendf(b, "{\"streamtext\":%d,\"default_is_status\":%d,\"experimental_fan\":%d}",
                    st.streamtext_enabled, st.default_is_status, st.experimental_fan_enabled);
}

static int gui_sysfs_read_list(const char *path, char *out, size_t out_sz)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return -1; }
    size_t n = fread(out, 1, out_sz - 1, fp);
    out[n] = '\0';
    fclose(fp);
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

static void gui_cpu_options_json(struct gui_buf *b)
{
    char gov[256] = "", epp[256] = "";
    gui_sysfs_read_list("/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors",
                        gov, sizeof(gov));
    gui_sysfs_read_list("/sys/devices/system/cpu/cpufreq/policy0/energy_performance_available_preferences",
                        epp, sizeof(epp));
    char gov2[256], epp2[256];
    gui_copy_str(gov2, sizeof(gov2), gov);
    gui_copy_str(epp2, sizeof(epp2), epp);

    gui_buf_append(b, "{\"governors\":[", strlen("{\"governors\":["));
    int first = 1;
    for (char *t = strtok(gov2, " "); t; t = strtok(NULL, " ")) {
        if (!first) gui_buf_append(b, ",", 1);
        gui_json_string(b, t);
        first = 0;
    }
    gui_buf_append(b, "],\"epp\":[", strlen("],\"epp\":["));
    first = 1;
    for (char *t = strtok(epp2, " "); t; t = strtok(NULL, " ")) {
        if (!first) gui_buf_append(b, ",", 1);
        gui_json_string(b, t);
        first = 0;
    }
    gui_buf_append(b, "]}", 2);
}

static void gui_profiles_json(struct gui_buf *b)
{
    gui_buf_append(b, "{\"builtin\":", strlen("{\"builtin\":"));
    gui_buf_append(b, "[\"max\",\"cpuperf\",\"balanced\",\"powersave\",\"silent\",\"eco\"]",
                   strlen("[\"max\",\"cpuperf\",\"balanced\",\"powersave\",\"silent\",\"eco\"]"));
    gui_buf_append(b, ",\"custom\":[", strlen(",\"custom\":["));

    FILE *fp = fopen("/var/lib/cctl/profiles.conf", "r");
    int first = 1;
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '#') continue;
            /* cctl accepts rows of exactly 5, 7, or 8 fields. */
            char tok[16][32];
            int nf = 0;
            char *p = line;
            while (*p && nf < 16) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;
                char *end = p;
                while (*end && *end != ' ' && *end != '\t') end++;
                size_t len = (size_t)(end - p);
                if (len >= sizeof(tok[0])) break;
                memcpy(tok[nf], p, len);
                tok[nf][len] = '\0';
                nf++;
                p = end;
            }
            if (nf != 5 && nf != 7 && nf != 8) continue;
            if (!gui_name_ok(tok[0], 40)) continue;
            const char *pl1 = nf >= 7 ? tok[5] : "skip";
            const char *pl2 = nf >= 7 ? tok[6] : "skip";
            const char *refresh = nf == 8 ? tok[7] : "skip";
            if (!first) gui_buf_append(b, ",", 1);
            gui_buf_appendf(b, "{\"name\":");
            gui_json_string(b, tok[0]);
            gui_buf_appendf(b, ",\"ec\":");
            gui_json_string(b, tok[1]);
            gui_buf_appendf(b, ",\"turbo\":");
            gui_json_string(b, tok[2]);
            gui_buf_appendf(b, ",\"gov\":");
            gui_json_string(b, tok[3]);
            gui_buf_appendf(b, ",\"epp\":");
            gui_json_string(b, tok[4]);
            gui_buf_appendf(b, ",\"pl1\":");
            gui_json_string(b, pl1);
            gui_buf_appendf(b, ",\"pl2\":");
            gui_json_string(b, pl2);
            gui_buf_appendf(b, ",\"refresh\":");
            gui_json_string(b, refresh);
            gui_buf_append(b, "}", 1);
            first = 0;
        }
        fclose(fp);
    }
    gui_buf_append(b, "]}", 2);
}

static int gui_curve_name_ok(const char *name)
{
    if (!gui_name_ok(name, FANCURVE_NAME_MAX)) return 0;
    if (name[0] == '.') return 0;
    return 1;
}

static void gui_curves_json(struct gui_buf *b)
{
    struct fancurve_state st;
    int active = fc_is_running(&st);
    gui_buf_appendf(b, "{\"active\":%d,", active ? 1 : 0);
    if (active) {
        gui_buf_append(b, "\"name\":", strlen("\"name\":"));
        gui_json_string(b, st.name);
        gui_buf_appendf(b, ",\"pid\":%d", (int)st.pid);
    } else {
        gui_buf_append(b, "\"name\":null,\"pid\":null", strlen("\"name\":null,\"pid\":null"));
    }
    gui_buf_append(b, ",\"names\":[", strlen(",\"names\":["));

    struct dirent **list = NULL;
    int n = scandir(FANCURVE_DIR, &list, NULL, alphasort);
    int first = 1;
    for (int i = 0; n > 0 && i < n; i++) {
        const char *nm = list[i]->d_name;
        size_t len = strlen(nm);
        if (len > 5 && strcmp(nm + len - 5, ".json") == 0 && nm[0] != '.') {
            if (!first) gui_buf_append(b, ",", 1);
            char base[64];
            snprintf(base, sizeof(base), "%.*s", (int)(len - 5), nm);
            gui_json_string(b, base);
            first = 0;
        }
        free(list[i]);
    }
    if (list) free(list);
    gui_buf_append(b, "]}", 2);
}

static void gui_curve_read(int fd, const char *name)
{
    struct gui_buf b;
    gui_buf_init(&b);
    if (!gui_curve_name_ok(name)) {
        gui_send_error(fd, 400, "invalid curve name");
        free(b.p);
        return;
    }
    char path[PATH_MAX];
    if (fancurve_profile_path(name, path, sizeof(path)) < 0) {
        gui_send_error(fd, 400, "invalid curve name");
        free(b.p);
        return;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        gui_send_error(fd, 404, "curve not found");
        free(b.p);
        return;
    }
    /* cctl's saved curves carry ## comment lines; browsers need pure JSON. */
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') continue;
        gui_buf_append(&b, line, strlen(line));
    }
    fclose(fp);
    if (b.len == 0) gui_send_error(fd, 404, "curve file empty");
    else gui_send_json(fd, 200, &b);
    free(b.p);
}

static void gui_presets_json(struct gui_buf *b)
{
    gui_buf_append(b, "{\"presets\":[", strlen("{\"presets\":["));
    for (const struct kbd_preset *p = kbd_presets; p->name; p++) {
        if (p != kbd_presets) gui_buf_append(b, ",", 1);
        gui_buf_appendf(b, "{\"name\":");
        gui_json_string(b, p->name);
        gui_buf_appendf(b, ",\"rgb\":[%d,%d,%d]}", p->r, p->g, p->b);
    }
    gui_buf_append(b, "],\"effects\":[", strlen("],\"effects\":["));
    /* kbe_effects has no NULL sentinel; iterate by array size. */
    for (size_t i = 0; i < sizeof(kbe_effects) / sizeof(kbe_effects[0]); i++) {
        if (i > 0) gui_buf_append(b, ",", 1);
        gui_buf_appendf(b, "{\"name\":");
        gui_json_string(b, kbe_effects[i].name);
        gui_buf_appendf(b, ",\"desc\":");
        gui_json_string(b, kbe_effects[i].desc);
        gui_buf_append(b, "}", 1);
    }
    gui_buf_append(b, "],\"notifications\":[\"flash\",\"pulse\",\"sos\",\"police\"]}",
                   strlen("],\"notifications\":[\"flash\",\"pulse\",\"sos\",\"police\"]}"));
}

/* The snapshots dir is root-only; list through a child running
 * `cctl snapshot` (elevates via the installed sudoers rule). */
static void gui_snapshots_list(int fd)
{
    struct gui_buf out, err;
    gui_buf_init(&out); gui_buf_init(&err);
    int rc = 0;
    char *argv[2] = { "snapshot", NULL };
    if (gui_child(argv, &out, &err, &rc) < 0) {
        gui_send_error(fd, 500, "fork failed");
        return;
    }
    if (rc != 0 || !out.p) {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_cmd_result(&b, rc, &out, &err);
        gui_send_json(fd, 502, &b);
        free(b.p);
        free(out.p); free(err.p);
        return;
    }

    struct gui_buf b;
    gui_buf_init(&b);
    gui_buf_append(&b, "{\"names\":[", strlen("{\"names\":["));
    const char *hdr = strstr(out.p, "Saved snapshots:");
    int first = 1;
    if (hdr) {
        char *line = strchr((char *)hdr, '\n');
        while (line && *line) {
            line++;
            if (strncmp(line, "Help", 4) == 0) break;
            char *nl = strchr(line, '\n');
            const char *p = line;
            const char *end = nl ? nl : line + strlen(line);
            while (p < end) {
                while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) p++;
                const char *s = p;
                while (p < end && *p != ' ' && *p != '\t' && *p != ',') p++;
                size_t tl = (size_t)(p - s);
                char tok[72];
                if (tl == 0 || tl >= sizeof(tok)) continue;
                memcpy(tok, s, tl);
                tok[tl] = '\0';
                if (strcmp(tok, "none") == 0 || !gui_name_ok(tok, CCTL_SNAPSHOT_NAME_MAX)) continue;
                if (!first) gui_buf_append(&b, ",", 1);
                gui_json_string(&b, tok);
                first = 0;
            }
            if (!nl) break;
            line = nl;
        }
    }
    gui_buf_append(&b, "]}", 2);
    gui_send_json(fd, 200, &b);
    free(b.p);
    free(out.p); free(err.p);
}

/* ------------------------------------------------------------ endpoints */

static void gui_run_child_json(int fd, char **argv)
{
    struct gui_buf out, err;
    gui_buf_init(&out); gui_buf_init(&err);
    int rc = 0;
    if (gui_child(argv, &out, &err, &rc) < 0) {
        gui_send_error(fd, 500, "fork failed");
    } else {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_cmd_result(&b, rc, &out, &err);
        gui_send_json(fd, 200, &b);
        free(b.p);
    }
    free(out.p); free(err.p);
}

static void gui_passthrough_json(int fd, char **argv)
{
    struct gui_buf out, err;
    gui_buf_init(&out); gui_buf_init(&err);
    int rc = 0;
    if (gui_child(argv, &out, &err, &rc) < 0) {
        gui_send_error(fd, 500, "fork failed");
    } else if (rc == 0 && out.p && out.len > 0) {
        gui_send(fd, 200, "application/json", &out);
    } else {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_cmd_result(&b, rc, &out, &err);
        gui_send_json(fd, rc == 0 ? 502 : 200, &b);
        free(b.p);
    }
    free(out.p); free(err.p);
}

static int gui_handle(int fd, struct gui_req *req)
{
    const char *path = req->path;

    if (strcmp(path, "/") == 0) {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_buf_append(&b, GUI_HTML, strlen(GUI_HTML));
        gui_send(fd, 200, "text/html; charset=utf-8", &b);
        free(b.p);
        return 0;
    }
    if (strcmp(path, "/favicon.ico") == 0 || strcmp(path, "/icon.svg") == 0) {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_buf_append(&b, GUI_ICON, strlen(GUI_ICON));
        gui_send(fd, 200, "image/svg+xml", &b);
        free(b.p);
        return 0;
    }

    if (strcmp(req->method, "GET") != 0) {
        if (strcmp(req->method, "POST") != 0) { gui_send_error(fd, 405, "method not allowed"); return 0; }
        if (!req->x_cctl[0]) { gui_send_error(fd, 403, "missing X-CCTL header"); return 0; }

        /* Generic command runner */
        if (strcmp(path, "/api/run") == 0) {
            char cmd[32];
            if (gui_form_get(req->body, "cmd", cmd, sizeof(cmd)) < 0 || !gui_cmd_allowed(cmd)) {
                gui_send_error(fd, 400, "command not allowed");
                return 0;
            }
            char args[12][64];
            int nargs = gui_form_get_all(req->body, "args", args, 12);
            char *argv[14];
            argv[0] = cmd;
            for (int i = 0; i < nargs; i++) {
                if (!gui_arg_ok(args[i])) { gui_send_error(fd, 400, "invalid argument"); return 0; }
                argv[i + 1] = args[i];
            }
            argv[nargs + 1] = NULL;
            gui_run_child_json(fd, argv);
            return 0;
        }

        /* Structured endpoints: build exact argv from validated fields. */
        char a0[64];

        if (strcmp(path, "/api/set") == 0) {
            if (gui_form_get(req->body, "profile", a0, sizeof(a0)) < 0 || !gui_name_ok(a0, 40)) {
                gui_send_error(fd, 400, "invalid profile name"); return 0;
            }
            char nosafe[8];
            char *argv[4] = { "set", a0, NULL, NULL };
            int n = 2;
            if (gui_form_get(req->body, "nosafe", nosafe, sizeof(nosafe)) == 0 && strcmp(nosafe, "1") == 0)
                argv[n++] = "--nosafe";
            argv[n] = NULL;
            gui_run_child_json(fd, argv);
            return 0;
        }

        if (strcmp(path, "/api/turbo") == 0) {
            if (gui_form_get(req->body, "state", a0, sizeof(a0)) < 0 ||
                (strcmp(a0, "on") != 0 && strcmp(a0, "off") != 0)) {
                gui_send_error(fd, 400, "turbo state must be on or off"); return 0;
            }
            char nosafe[8];
            char *argv[4] = { "turbo", a0, NULL, NULL };
            int n = 2;
            if (gui_form_get(req->body, "nosafe", nosafe, sizeof(nosafe)) == 0 && strcmp(nosafe, "1") == 0)
                argv[n++] = "--nosafe";
            argv[n] = NULL;
            gui_run_child_json(fd, argv);
            return 0;
        }

        if (strcmp(path, "/api/fan") == 0) {
            char args[4][64];
            int nargs = gui_form_get_all(req->body, "args", args, 4);
            if (nargs < 1 || nargs > 4) { gui_send_error(fd, 400, "fan args required"); return 0; }
            for (int i = 0; i < nargs; i++) {
                if (!gui_arg_ok(args[i])) { gui_send_error(fd, 400, "invalid fan argument"); return 0; }
            }
            char *argv[6];
            argv[0] = "fan";
            for (int i = 0; i < nargs; i++) argv[i + 1] = args[i];
            argv[nargs + 1] = NULL;
            gui_run_child_json(fd, argv);
            return 0;
        }

        if (strcmp(path, "/api/options") == 0 || strcmp(path, "/api/options-set") == 0) {
            char key[32], val[16];
            if (gui_form_get(req->body, "key", key, sizeof(key)) < 0 ||
                gui_form_get(req->body, "value", val, sizeof(val)) < 0) {
                gui_send_error(fd, 400, "key and value required"); return 0;
            }
            if (strcmp(key, "experimental-fan") != 0 && strcmp(key, "streamtext") != 0 &&
                strcmp(key, "default_action") != 0) {
                gui_send_error(fd, 400, "unknown option key"); return 0;
            }
            if (strcmp(val, "on") != 0 && strcmp(val, "off") != 0 && strcmp(val, "status") != 0 &&
                strcmp(val, "help") != 0) {
                gui_send_error(fd, 400, "invalid option value"); return 0;
            }
            char *argv[4] = { "options", key, val, NULL };
            gui_run_child_json(fd, argv);
            return 0;
        }

        if (strcmp(path, "/api/curve-start") == 0 || strcmp(path, "/api/curve-stop") == 0 ||
            strcmp(path, "/api/curve-delete") == 0) {
            if (strcmp(path, "/api/curve-start") == 0) {
                if (gui_form_get(req->body, "name", a0, sizeof(a0)) < 0 || !gui_curve_name_ok(a0)) {
                    gui_send_error(fd, 400, "invalid curve name"); return 0;
                }
                char *argv[4] = { "fancurve", "start", a0, NULL };
                gui_run_child_json(fd, argv);
            } else if (strcmp(path, "/api/curve-stop") == 0) {
                char *argv[3] = { "fancurve", "stop", NULL };
                gui_run_child_json(fd, argv);
            } else {
                if (gui_form_get(req->body, "name", a0, sizeof(a0)) < 0 || !gui_curve_name_ok(a0)) {
                    gui_send_error(fd, 400, "invalid curve name"); return 0;
                }
                char *argv[5] = { "fancurve", "del", a0, "--noconfirm", NULL };
                gui_run_child_json(fd, argv);
            }
            return 0;
        }

        if (strcmp(path, "/api/curve-create") == 0) {
            char name[64], hyst[8], ms[10];
            if (gui_form_get(req->body, "name", name, sizeof(name)) < 0 ||
                !gui_curve_name_ok(name)) {
                gui_send_error(fd, 400, "invalid curve name"); return 0;
            }
            if (gui_form_get(req->body, "hysteresis", hyst, sizeof(hyst)) < 0 ||
                gui_form_get(req->body, "interval_ms", ms, sizeof(ms)) < 0) {
                gui_send_error(fd, 400, "hysteresis and interval_ms required"); return 0;
            }
            char pts[FANCURVE_MAX_POINTS * 2][64];
            int np = gui_form_get_all(req->body, "points", pts, FANCURVE_MAX_POINTS * 2);
            if (np < FANCURVE_MIN_POINTS * 2 || np > FANCURVE_MAX_POINTS * 2 || np % 2 != 0) {
                gui_send_error(fd, 400, "points must be temp/fan pairs, 2-16"); return 0;
            }
            int npts = np / 2;
            for (int i = 0; i < np; i++) {
                if (!gui_arg_ok(pts[i])) { gui_send_error(fd, 400, "invalid point value"); return 0; }
            }
            /* cctl requires strictly increasing temps; bubble-sort the pairs
             * so GUI-entered row order always validates. */
            char sorted_t[FANCURVE_MAX_POINTS][64], sorted_f[FANCURVE_MAX_POINTS][64];
            for (int i = 0; i < npts; i++) {
                gui_copy_str(sorted_t[i], sizeof(sorted_t[i]), pts[2 * i]);
                gui_copy_str(sorted_f[i], sizeof(sorted_f[i]), pts[2 * i + 1]);
            }
            for (int a = 0; a < npts - 1; a++) {
                for (int b = 0; b < npts - 1 - a; b++) {
                    if (atoi(sorted_t[b]) <= atoi(sorted_t[b + 1])) continue;
                    char t[64], f[64];
                    gui_copy_str(t, sizeof(t), sorted_t[b]);
                    gui_copy_str(f, sizeof(f), sorted_f[b]);
                    gui_copy_str(sorted_t[b], sizeof(sorted_t[b]), sorted_t[b + 1]);
                    gui_copy_str(sorted_f[b], sizeof(sorted_f[b]), sorted_f[b + 1]);
                    gui_copy_str(sorted_t[b + 1], sizeof(sorted_t[b + 1]), t);
                    gui_copy_str(sorted_f[b + 1], sizeof(sorted_f[b + 1]), f);
                }
            }

            char *argv[3 + 1 + 1 + 1 + FANCURVE_MAX_POINTS * 2 + 1];
            int k = 0;
            argv[k++] = "fancurve"; argv[k++] = "create"; argv[k++] = "--am";
            argv[k++] = name;
            char count[8];
            snprintf(count, sizeof(count), "%d", npts);
            argv[k++] = count;
            argv[k++] = hyst;
            argv[k++] = ms;
            for (int i = 0; i < npts; i++) {
                argv[k++] = sorted_t[i];
                argv[k++] = sorted_f[i];
            }
            argv[k] = NULL;
            gui_run_child_json(fd, argv);
            return 0;
        }

        if (strcmp(path, "/api/profile-create") == 0 || strcmp(path, "/api/profile-delete") == 0) {
            char name[64];
            if (gui_form_get(req->body, "name", name, sizeof(name)) < 0 || !gui_name_ok(name, 40)) {
                gui_send_error(fd, 400, "invalid profile name"); return 0;
            }
            if (strcmp(path, "/api/profile-delete") == 0) {
                char *argv[5] = { "editconf", "--am", "--delete", name, NULL };
                gui_run_child_json(fd, argv);
                return 0;
            }
            char ec[8], turbo[8], gov[32], epp[32], pl1[16], pl2[16], refresh[16];
            if (gui_form_get(req->body, "ec", ec, sizeof(ec)) < 0 ||
                gui_form_get(req->body, "turbo", turbo, sizeof(turbo)) < 0 ||
                gui_form_get(req->body, "gov", gov, sizeof(gov)) < 0 ||
                gui_form_get(req->body, "epp", epp, sizeof(epp)) < 0) {
                gui_send_error(fd, 400, "ec/turbo/gov/epp required"); return 0;
            }
            gui_form_get(req->body, "pl1", pl1, sizeof(pl1));
            if (!pl1[0]) snprintf(pl1, sizeof(pl1), "skip");
            gui_form_get(req->body, "pl2", pl2, sizeof(pl2));
            if (!pl2[0]) snprintf(pl2, sizeof(pl2), "skip");
            gui_form_get(req->body, "refresh", refresh, sizeof(refresh));
            if (!refresh[0]) snprintf(refresh, sizeof(refresh), "skip");
            if (!gui_arg_ok(ec) || !gui_arg_ok(turbo) || !gui_arg_ok(gov) ||
                !gui_arg_ok(epp) || !gui_arg_ok(pl1) || !gui_arg_ok(pl2) ||
                !gui_arg_ok(refresh)) {
                gui_send_error(fd, 400, "invalid profile field value"); return 0;
            }
            char *argv[12] = { "editconf", "--am", "--create", name, ec, turbo, gov, epp,
                               pl1, pl2, refresh, NULL };
            gui_run_child_json(fd, argv);
            return 0;
        }

        if (strcmp(path, "/api/snapshot") == 0) {
            char action[16], name[64];
            if (gui_form_get(req->body, "action", action, sizeof(action)) < 0 ||
                gui_form_get(req->body, "name", name, sizeof(name)) < 0 ||
                !gui_name_ok(name, CCTL_SNAPSHOT_NAME_MAX)) {
                gui_send_error(fd, 400, "action and name required"); return 0;
            }
            if (strcmp(action, "save") != 0 && strcmp(action, "restore") != 0 &&
                strcmp(action, "view") != 0 && strcmp(action, "delete") != 0) {
                gui_send_error(fd, 400, "unknown snapshot action"); return 0;
            }
            int i = 0;
            char *argv[5];
            argv[i++] = "snapshot"; argv[i++] = action; argv[i++] = name;
            if (strcmp(action, "delete") == 0) argv[i++] = "--noconfirm";
            argv[i] = NULL;
            gui_run_child_json(fd, argv);
            return 0;
        }

        gui_send_error(fd, 404, "unknown endpoint");
        return 0;
    }

    /* GET endpoints */
    if (strcmp(path, "/api/version") == 0) {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_buf_appendf(&b, "{\"version\":\"%s\",\"fork\":\"cctl-gui\",\"port\":%d}",
                        CCTL_VERSION, GUI_DEFAULT_PORT);
        gui_send_json(fd, 200, &b);
        free(b.p);
        return 0;
    }
    if (strcmp(path, "/api/status") == 0) {
        char *argv[3] = { "status", "--json", NULL };
        gui_passthrough_json(fd, argv);
        return 0;
    }
    if (strcmp(path, "/api/cpu") == 0) {
        char *argv[3] = { "cpumon", "--json", NULL };
        gui_passthrough_json(fd, argv);
        return 0;
    }
    if (strcmp(path, "/api/gpu") == 0) {
        char *argv[3] = { "gpumon", "--json", NULL };
        gui_passthrough_json(fd, argv);
        return 0;
    }
    if (strcmp(path, "/api/settings") == 0) {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_options_json(&b);
        gui_send_json(fd, 200, &b);
        free(b.p);
        return 0;
    }
    if (strcmp(path, "/api/cpu-options") == 0) {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_cpu_options_json(&b);
        gui_send_json(fd, 200, &b);
        free(b.p);
        return 0;
    }
    if (strcmp(path, "/api/profiles") == 0) {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_profiles_json(&b);
        gui_send_json(fd, 200, &b);
        free(b.p);
        return 0;
    }
    if (strcmp(path, "/api/curves") == 0) {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_curves_json(&b);
        gui_send_json(fd, 200, &b);
        free(b.p);
        return 0;
    }
    if (strcmp(path, "/api/curve") == 0) {
        char name[64] = "";
        gui_form_get(req->query, "name", name, sizeof(name));
        gui_curve_read(fd, name);
        return 0;
    }
    if (strcmp(path, "/api/presets") == 0) {
        struct gui_buf b;
        gui_buf_init(&b);
        gui_presets_json(&b);
        gui_send_json(fd, 200, &b);
        free(b.p);
        return 0;
    }
    if (strcmp(path, "/api/snapshots") == 0) {
        gui_snapshots_list(fd);
        return 0;
    }

    gui_send_error(fd, 404, "unknown endpoint");
    return 0;
}

static void *gui_conn_thread(void *arg)
{
    int fd = *(int *)arg;
    free(arg);
    struct gui_req req;
    int got = gui_read_request(fd, &req);
    if (got == 1) gui_handle(fd, &req);
    else if (got == 0) { /* EOF: nothing to answer */ }
    else gui_send_error(fd, 400, "malformed request");
    close(fd);
    return NULL;
}

static int cmd_gui(int argc, char **argv)
{
    int port = GUI_DEFAULT_PORT;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) return fail("Usage: cctl gui [--port PORT]");
            int v = atoi(argv[++i]);
            if (v < 1 || v > 65535) return fail("Port must be between 1 and 65535");
            port = v;
        } else if (strcmp(argv[i], "--json") == 0) {
            /* accepted no-op: server logs go to stdout regardless */
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            cctl_stream_printf("cctl gui [--port PORT]\n  Browser control panel on http://127.0.0.1:%d\n", port);
            return 0;
        } else {
            return fail("Unknown option '%s'; usage: cctl gui [--port PORT]", argv[i]);
        }
    }

    ssize_t n = readlink("/proc/self/exe", g_gui_child_path, sizeof(g_gui_child_path) - 1);
    if (n <= 0) return fail("Cannot resolve this binary's path");
    g_gui_child_path[n] = '\0';
    if (geteuid() != 0) {
        /* Unprivileged server: children go through the installed cctl so
         * their require_root uses the passwordless sudoers rule. */
        const char *installed = "/usr/local/bin/cctl";
        if (access(installed, X_OK) == 0)
            gui_copy_str(g_gui_child_path, sizeof(g_gui_child_path), installed);
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return fail("socket: %s", strerror(errno));
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int saved = errno;
        close(lfd);
        return saved == EADDRINUSE
            ? fail("Port %d is already in use (is another cctl gui running?)", port)
            : fail("bind: %s", strerror(saved));
    }
    if (listen(lfd, 16) != 0) {
        int saved = errno;
        close(lfd);
        return fail("listen: %s", strerror(saved));
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, 0);
    if (sfd < 0) { close(lfd); return fail("signalfd: %s", strerror(errno)); }

    cctl_stream_printf("cctl-gui %s serving http://127.0.0.1:%d/ (Ctrl-C to stop)\n",
                       CCTL_VERSION, port);
    fflush(stdout);

    for (;;) {
        struct pollfd pf[2] = {
            { .fd = lfd, .events = POLLIN },
            { .fd = sfd, .events = POLLIN },
        };
        int pr = poll(pf, 2, -1);
        if (pr < 0) { if (errno == EINTR) break; break; }
        if (pf[1].revents) {
            struct signalfd_siginfo si;
            if (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) break;
            }
            continue;
        }
        if (!(pf[0].revents & POLLIN)) continue;

        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        int *fdp = malloc(sizeof(int));
        if (!fdp) { close(cfd); continue; }
        *fdp = cfd;
        pthread_t th;
        if (pthread_create(&th, NULL, gui_conn_thread, fdp) != 0) {
            close(cfd);
            free(fdp);
        } else {
            pthread_detach(th);
        }
    }

    close(sfd);
    close(lfd);
    cctl_stream_printf("cctl-gui stopped\n");
    return 0;
}

#endif /* CCTL_GUI */
