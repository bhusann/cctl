/* ========================================================================
 * DRIVERS MANAGE — source resolution: cache → local file → download → manual path
 * ========================================================================
 * Order (never searches $HOME, never accepts a folder):
 *   1. drivers.tar.gz sitting next to the running cctl binary — sha256
 *      checked on the spot and rejected in place if it does not match,
 *   2. else download it from the mirror repo into a private temp dir
 *      using curl or wget (a failed download is reported explicitly,
 *      with the exact folder a manual copy belongs in),
 *   3. offline (or download failed): accept the full path to a copy —
 *      also checked in place first; staged into /tmp only if valid.
 * Whatever passes, the staged bytes are re-verified against the hash
 * baked into this binary before a single byte is extracted.
 * ======================================================================== */

#define DRIVERS_TARBALL     "drivers.tar.gz"
#define DRIVERS_MIRROR_RAW  "https://raw.githubusercontent.com/bhusann/tuxedo-drivers-cctl-mirror/main/drivers.tar.gz"
#define DRIVERS_MIRROR_REPO "https://github.com/bhusann/tuxedo-drivers-cctl-mirror"
/* SHA256 of drivers.tar.gz from the mirror repo, baked in at build time so
 * cctl only ever installs exactly these driver sources — a re-tarred or
 * swapped file with any other name/content is rejected. When the MIRROR's
 * drivers/ tree changes: re-tar deterministically, update THIS constant,
 * bump CCTL_MICROVERSION, release a new cctl binary. (The driver sources
 * themselves live only in the mirror repo, not in this checkout.) */
#define DRIVERS_SHA256      "3e0c3cf5d4f2f5ddf17f328e2897874ea578476eea5db7356bb44f2bd67a6dbe"

/* Persistent driver cache: survives reboots so reinstall/uninstall works
 * offline with no tarball beside the binary and no typed path. /var/lib
 * (not /var/cache) — FHS marks it as state that must persist, not as
 * re-creatable cache that system cleaners may purge. Root-owned. */
#ifndef DRIVERS_CACHE_DIR
#define DRIVERS_CACHE_DIR   "/var/lib/cctl"
#endif
#ifndef DRIVERS_CACHE_FILE
#define DRIVERS_CACHE_FILE  DRIVERS_CACHE_DIR "/drivers.tar.gz"
#endif

/* Directory containing the running binary. Returns 0 and fills out. */
static int binary_dir(char *out, size_t sz)
{
    char path[PATH_MAX];
    if (get_self_exe(path, sizeof(path)) < 0) return -1;
    char *slash = strrchr(path, '/');
    if (!slash) return -1;
    *slash = '\0';
    if (strlen(path) + 1 > sz) return -1;
    snprintf(out, sz, "%s", path);
    return 0;
}

/* Lowercase hex sha256 of a file, via exec of sha256sum with an argv array
 * (no shell → no quoting problems with odd paths). 0 = success. */
static int file_sha256(const char *path, char out[65])
{
    char *const args[] = { "sha256sum", (char *)path, NULL };
    char buf[256];
    if (run(args, RUN_CAPTURE, buf, sizeof(buf)) != 0 || strlen(buf) < 64)
        return -1;
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)buf[i])) return -1;
        out[i] = (char)tolower((unsigned char)buf[i]);
    }
    out[64] = '\0';
    return 0;
}

/* Download url → dest via curl (preferred) or wget. 0 = success. */
static int download_file(const char *url, const char *dest)
{
    if (command_exists("curl")) {
        char *const args[] = { "curl", "-fsSL", "--connect-timeout", "10",
                               "--max-time", "300", "-o", (char *)dest,
                               (char *)url, NULL };
        if (run(args, 0, NULL, 0) == 0) return 0;
    }
    if (command_exists("wget")) {
        char *const args[] = { "wget", "-q", "--timeout=10", "-O",
                               (char *)dest, (char *)url, NULL };
        if (run(args, 0, NULL, 0) == 0) return 0;
    }
    return -1;
}

/* Best-effort downloads during cctl installation should not dump transport
 * errors over the main install result; the caller prints a concise note. */
static int download_file_quiet(const char *url, const char *dest)
{
    if (command_exists("curl")) {
        char *const args[] = { "curl", "-fsSL", "--connect-timeout", "10",
                               "--max-time", "300", "-o", (char *)dest,
                               (char *)url, NULL };
        if (run(args, RUN_SILENT, NULL, 0) == 0) return 0;
    }
    if (command_exists("wget")) {
        char *const args[] = { "wget", "-q", "--timeout=10", "-O",
                               (char *)dest, (char *)url, NULL };
        if (run(args, RUN_SILENT, NULL, 0) == 0) return 0;
    }
    return -1;
}

/* rm -rf via argv array (paths may contain spaces — never a shell string).
 * Refuses anything outside our own /tmp/cctl-* temp-dir namespace. */
static void remove_tree(const char *path)
{
    if (!path || strncmp(path, "/tmp/cctl-", 10) != 0) return;
    char *const args[] = { "rm", "-rf", (char *)path, NULL };
    int r = run(args, 0, NULL, 0);
    (void)r;
}

/* Verify a drivers.tar.gz against the hash baked into this binary.
 * 0 = match. Prints the reason on failure; prints the short confirmation
 * only when announce_ok (the pre-staging early checks stay quiet so the
 * single "sha256 verified" line refers to the bytes actually extracted). */
static int verify_drivers_sha(const char *path, const char *src_desc, int announce_ok)
{
    char sha[65];
    if (file_sha256(path, sha) != 0) {
        fprintf(stderr, "Error: cannot compute sha256 of %s (is sha256sum installed?)\n",
                path);
        return -1;
    }
    if (strcmp(sha, DRIVERS_SHA256) != 0) {
        fprintf(stderr,
            "%sError:%s %s checksum mismatch — refusing to install.\n"
            "  %ssource:%s   %s%s%s\n"
            "  %sexpected:%s %s%s%s\n"
            "  %sgot:%s      %s%s%s\n"
            "%sThe file may be corrupt, spoofed, or built from different sources.%s\n"
            "%sIf this came from the official cctl mirror, this cctl binary may be\n"
            "outdated. Update cctl and try again; this version only accepts the\n"
            "driver archive whose checksum was built into it.%s\n",
            C_RED, C_RST, src_desc,
            C_CYN, C_RST, C_DIM, path, C_RST,
            C_CYN, C_RST, C_DIM, DRIVERS_SHA256, C_RST,
            C_CYN, C_RST, C_DIM, sha, C_RST,
            C_DIM, C_RST,
            C_YLW, C_RST);
        return -1;
    }
    if (announce_ok)
        printf("  sha256 verified: %.16s...\n", sha);
    return 0;
}

/* Atomically publish verified driver bytes to the persistent cache
 * (copy to ".new", fsync-free rename over the target). Non-fatal by
 * design: prints why and returns -1; callers continue without it. */
static int cache_store(const char *verified_file)
{
    if (mkdir(DRIVERS_CACHE_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Note: cannot create %s: %s\n", DRIVERS_CACHE_DIR, strerror(errno));
        return -1;
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/.new", DRIVERS_CACHE_DIR);
    char *const cp_args[] = { "cp", "--", (char *)verified_file, tmp, NULL };
    if (run(cp_args, 0, NULL, 0) != 0) {
        unlink(tmp);
        fprintf(stderr, "Note: cannot copy %s into %s\n", verified_file, DRIVERS_CACHE_DIR);
        return -1;
    }
    if (chmod(tmp, 0644) != 0 || rename(tmp, DRIVERS_CACHE_FILE) != 0) {
        fprintf(stderr, "Note: cannot publish %s: %s\n", DRIVERS_CACHE_FILE, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Step 0 of drivers-manage: a hash-matching cache entry wins outright.
 * Returns 1 (out filled) on hit; 0 on absent-or-stale — a stale entry is
 * reported once and never used, only replaced after the next verified
 * acquisition. */
static int cache_lookup(char *out, size_t sz, int quiet, int *stale)
{
    char sha[65] = {0};
    if (stale)
        *stale = 0;
    if (access(DRIVERS_CACHE_FILE, R_OK) != 0)
        return 0;
    if (file_sha256(DRIVERS_CACHE_FILE, sha) == 0 && strcmp(sha, DRIVERS_SHA256) == 0) {
        snprintf(out, sz, "%s", DRIVERS_CACHE_FILE);
        return 1;
    }
    if (!quiet)
        printf("Note: %s is outdated or corrupt — fetching a verified replacement.\n",
               DRIVERS_CACHE_FILE);
    if (stale)
        *stale = 1;
    return 0;
}

/* Best-effort archive fetch for cctl install. A missing, stale, or
 * unavailable archive must never prevent the cctl binary/sudoers install. */
static int drivers_cache_warm(const char *binary_path, int *stale_cache)
{
    char cached[PATH_MAX];
    if (stale_cache)
        *stale_cache = 0;
    if (cache_lookup(cached, sizeof(cached), 1, stale_cache)) {
        printf("%sDriver package cache is up to date.%s\n", C_DIM, C_RST);
        return 0;
    }

    /* Prefer a matching archive shipped beside this cctl binary before
     * reaching the network. Never trust it without checking this build's hash. */
    char adjacent[PATH_MAX];
    const char *slash = strrchr(binary_path, '/');
    int adjacent_len = -1;
    if (slash) {
        size_t dir_len = (size_t)(slash - binary_path);
        if (dir_len + sizeof("/" DRIVERS_TARBALL) <= sizeof(adjacent)) {
            memcpy(adjacent, binary_path, dir_len);
            adjacent[dir_len] = '\0';
            adjacent_len = snprintf(adjacent + dir_len,
                                    sizeof(adjacent) - dir_len,
                                    "/%s", DRIVERS_TARBALL);
        }
    }
    if (adjacent_len >= 0 && (size_t)adjacent_len < sizeof(adjacent) &&
        access(adjacent, R_OK) == 0) {
        char sha[65];
        if (file_sha256(adjacent, sha) == 0 && strcmp(sha, DRIVERS_SHA256) == 0) {
            printf("%sFound matching %s beside cctl.%s\n", C_DIM,
                   DRIVERS_TARBALL, C_RST);
            if (cache_store(adjacent) == 0) {
                printf("%sDriver package cached.%s\n", C_DIM, C_RST);
                return 0;
            }
        }
    }

    if (!command_exists("curl") && !command_exists("wget")) {
        return -1;
    }

    char tmp_template[] = "/tmp/cctl-install-drivers.XXXXXX";
    char *tmpdir = mkdtemp(tmp_template);
    if (!tmpdir) {
        return -1;
    }

    char archive[PATH_MAX] = {0};
    int n = snprintf(archive, sizeof(archive), "%s/%s", tmpdir, DRIVERS_TARBALL);
    int cached_ok = 0;
    if (n >= 0 && (size_t)n < sizeof(archive)) {
        if (download_file_quiet(DRIVERS_MIRROR_RAW, archive) == 0) {
            char sha[65];
            if (file_sha256(archive, sha) == 0 &&
                strcmp(sha, DRIVERS_SHA256) == 0 && cache_store(archive) == 0) {
                printf("%sDriver package cache refreshed.%s\n", C_DIM, C_RST);
                cached_ok = 1;
            }
        }
    }

    if (archive[0])
        (void)unlink(archive);
    (void)rmdir(tmpdir);
    return cached_ok ? 0 : -1;
}

static int cmd_drivers_install(int argc, char **argv)
{
    /* No file arguments, ever — cctl locates/downloads the archive itself
     * and verifies its baked-in sha256, so nothing user-named is trusted
     * except via the explicitly-prompted offline path (also verified). */
    if (argc != 2) {
        fprintf(stderr, "Error: 'cctl drivers-manage' does not accept arguments.\n"
                        "       It finds or downloads %s automatically.\n",
                DRIVERS_TARBALL);
        return 1;
    }

    if (geteuid() != 0)
        self_elevate(argc, argv);

    char tmpbase[] = "/tmp/cctl-drivers.XXXXXX";
    char *tmpdir = NULL;
    char tarball[PATH_MAX + 32] = {0}; /* staged copy — always inside tmpdir */
    char src[PATH_MAX + 32] = {0};     /* verified source outside tmp (local/offline) */
    int staged_by_download = 0;
    int src_is_cache = 0;
    int rc = 1;

    /* 0. Persistent cache first: /var/lib/cctl/drivers.tar.gz — survives
     *    reboots, so reinstall/uninstall works fully offline. Only a copy
     *    matching the baked hash is used; a stale one is reported and
     *    replaced after the next verified acquisition. */
    if (cache_lookup(src, sizeof(src), 0, NULL)) {
        src_is_cache = 1;
        printf("Using cached %s\n  from %s\n", DRIVERS_TARBALL, DRIVERS_CACHE_DIR);
    }

    /* 1. Local: drivers.tar.gz beside the cctl binary — sha256-checked
     *    ON THE SPOT before anything is staged. A mismatch does NOT abort:
     *    the outdated/corrupt copy is called out ("do not use it") and the
     *    flow continues with the download, which also reseeds the cache. */
    char bindir[PATH_MAX] = {0};
    if (binary_dir(bindir, sizeof(bindir)) != 0)
        bindir[0] = '\0';
    if (!src[0] && bindir[0]) {
        snprintf(src, sizeof(src), "%s/%s", bindir, DRIVERS_TARBALL);
        if (access(src, R_OK) == 0) {
            char lsha[65] = {0};
            if (file_sha256(src, lsha) != 0 || strcmp(lsha, DRIVERS_SHA256) != 0) {
                fprintf(stderr,
                    "Warning: %s beside the cctl binary is outdated or corrupt.\n"
                    "         Do NOT use that copy: %s\n"
                    "         expected %s\n"
                    "         got      %s\n"
                    "         Fetching a verified copy instead.\n",
                    DRIVERS_TARBALL, src, DRIVERS_SHA256,
                    lsha[0] ? lsha : "unreadable");
                src[0] = '\0';
            }
        } else {
            src[0] = '\0';
        }
    }

    if (!src[0]) {
        /* 2. Not local → fetch the mirror straight into a private temp dir. */
        int can_download = (command_exists("curl") || command_exists("wget"));
        if (can_download) {
            tmpdir = mkdtemp(tmpbase);
            if (!tmpdir)
                perror("Warning: mkdtemp failed");
            else {
                snprintf(tarball, sizeof(tarball), "%s/%s", tmpdir, DRIVERS_TARBALL);
                printf("Fetching %s\n  from %s\n", DRIVERS_TARBALL, DRIVERS_MIRROR_RAW);
                if (download_file(DRIVERS_MIRROR_RAW, tarball) == 0)
                    staged_by_download = 1;
            }
        }

        if (!staged_by_download) {
            /* 3. Download failed (or impossible): say so plainly and offer
             *    the two manual ways to continue. */
            fprintf(stderr,
                "Error: could not provide %s.\n"
                "%s\n"
                "Do ONE of the following:\n"
                "  1. Re-run 'cctl drivers-manage' with an internet connection\n"
                "     (it then downloads automatically).\n"
                "  2. Download %s manually from\n"
                "       %s\n"
                "     and place it in this folder:\n"
                "       %s\n"
                "     then re-run 'cctl drivers-manage'.\n"
                "  3. Or enter the full path to a %s you already have.\n",
                DRIVERS_TARBALL,
                can_download
                    ? "Downloading from the mirror repo failed (no internet?)."
                    : "Neither curl nor wget is available to download it.",
                DRIVERS_TARBALL, DRIVERS_MIRROR_REPO,
                bindir[0] ? bindir : "<directory containing the cctl binary>",
                DRIVERS_TARBALL);
            printf("Full path to %s (Enter to abort): ", DRIVERS_TARBALL);
            char path[PATH_MAX] = {0};
            if (!fgets(path, sizeof(path), stdin) || path[0] == '\0') {
                printf("Aborted.\n");
                rc = 0;
                goto cleanup;
            }
            path[strcspn(path, "\n")] = '\0';

            /* This prompt runs after auto-elevation, so expand ~/ using the
             * invoking user's home rather than root's HOME. */
            if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
                const char *home = NULL;
                const char *sudo_user = getenv("SUDO_USER");
                struct passwd *pw = sudo_user ? getpwnam(sudo_user) : NULL;
                if (pw)
                    home = pw->pw_dir;
                if (!home || !home[0])
                    home = getenv("HOME");
                if (home && home[0]) {
                    char expanded[PATH_MAX];
                    int n = snprintf(expanded, sizeof(expanded), "%s%s",
                                     home, path + 1);
                    if (n < 0 || (size_t)n >= sizeof(expanded)) {
                        fprintf(stderr, "Error: path is too long.\n");
                        goto cleanup;
                    }
                    snprintf(path, sizeof(path), "%s", expanded);
                }
            }

            struct stat pst;
            if (stat(path, &pst) != 0) {
                fprintf(stderr, "Error: no such file: %s\n", path);
                goto cleanup;
            }
            if (S_ISDIR(pst.st_mode)) {
                char archive_path[PATH_MAX];
                int n = snprintf(archive_path, sizeof(archive_path), "%s/%s",
                                 path, DRIVERS_TARBALL);
                if (n < 0 || (size_t)n >= sizeof(archive_path) ||
                    stat(archive_path, &pst) != 0 || !S_ISREG(pst.st_mode)) {
                    fprintf(stderr, "Error: folder does not contain %s: %s\n",
                            DRIVERS_TARBALL, path);
                    goto cleanup;
                }
                snprintf(path, sizeof(path), "%s", archive_path);
            }
            if (access(path, R_OK) != 0) {
                fprintf(stderr, "Error: no such readable file: %s\n", path);
                goto cleanup;
            }
            /* Check the given file IN PLACE first — a mismatch is rejected
             * here, before it is ever copied into our temp dir. */
            if (verify_drivers_sha(path, "given drivers.tar.gz", 0) != 0)
                goto cleanup;
            snprintf(src, sizeof(src), "%s", path);
        }
    }

    /* Stage: a download already landed in tmpdir; a verified local/offline
     * file is copied in now. Re-verify the staged bytes (the ones that will
     * actually be extracted) to close any swap gap after the in-place check. */
    if (staged_by_download) {
        if (verify_drivers_sha(tarball, "downloaded drivers.tar.gz", 1) != 0)
            goto cleanup;
    } else {
        if (!tmpdir) {
            tmpdir = mkdtemp(tmpbase);
            if (!tmpdir) { perror("Error: cannot create temp directory"); goto cleanup; }
        }
        snprintf(tarball, sizeof(tarball), "%s/%s", tmpdir, DRIVERS_TARBALL);
        char *const cp_args[] = { "cp", "--", src, tarball, NULL };
        if (run(cp_args, 0, NULL, 0) != 0) {
            fprintf(stderr, "Error: failed to copy %s into %s\n", src, tmpdir);
            goto cleanup;
        }
        if (verify_drivers_sha(tarball, "staged drivers.tar.gz", 1) != 0)
            goto cleanup;
    }

    /* Publish the verified staged bytes to the persistent cache so a later
     * reinstall/uninstall needs no network. Skipped when the cache itself
     * was the source; failure is non-fatal (cache_store reports why). */
    if (!src_is_cache)
        (void)cache_store(tarball);

    /* Extract into the private temp dir and run the verified installer. */
    {
        char *const tar_args[] = { "tar", "-xzf", tarball, "-C", tmpdir, NULL };
        if (run(tar_args, 0, NULL, 0) != 0) {
            fprintf(stderr, "Error: failed to extract %s\n", tarball);
            goto cleanup;
        }
    }
    char script[PATH_MAX + 64];
    snprintf(script, sizeof(script), "%s/drivers/driverinstall.sh", tmpdir);
    if (access(script, R_OK) != 0) {
        fprintf(stderr, "Error: drivers/driverinstall.sh not found inside %s\n", DRIVERS_TARBALL);
        goto cleanup;
    }

    printf("Launching driver installer (verified sources)...\n");
    {
        char *const sh_args[] = { "bash", script, NULL };
        int installer_status = run(sh_args, 0, NULL, 0);
        if (installer_status != 0) {
            fprintf(stderr, "Driver installer exited with status %d. "
                            "Review the installer output above.\n", installer_status);
            goto cleanup;
        }
    }
    printf("Drivers installed from verified %s.\n", DRIVERS_TARBALL);
    rc = 0;

cleanup:
    if (tmpdir) remove_tree(tmpdir);
    return rc;
}

static int cmd_mux(int argc, char **argv)
{
    if (argc >= 3) {
        if (strcmp(argv[2], "switch") == 0) {
            if (argc > 3)
                return fail("Unexpected extra argument '%s'", argv[3]);
            require_root(argc, argv);
            return mux_switch();
        }
        fprintf(stderr, "Error: Unknown mux subcommand '%s'\n", argv[2]);
        fprintf(stderr, "Usage: cctl mux  |  cctl mux switch\n");
        return 1;
    }

    /* 'cctl mux' with no subcommand — show current mode + pending */
    int rc = mux_show_internal("GPU MUX: ");
    if (mux_running_mode() == MUX_VAL_DGPU && has_display_support()) {
        struct display_info info;
        if (query_display_info(&info) == 0 && strtod(info.current_rate, NULL) == 40.0)
            printf("  Current refresh rate is 40 Hz. To use 165 Hz, run: cctl rr 165\n");
    }
    return rc;
}

enum cmd_section {
    SEC_PROFILES = 1,
    SEC_KEYBOARD,
    SEC_FAN,
    SEC_MUX,
    SEC_PRIVACY,
    SEC_BATTERY,
    SEC_INFO,
    SEC_NVIDIA,
    SEC_DISPLAY,
    SEC_OVERRIDES,
    SEC_SYSTEM
};

struct command {
    const char *name;
    int needs_root;
    int (*handler)(int argc, char **argv);
    enum cmd_section section;
    const char *usage;
    const char *desc;
    int (*visible)(void);
};

static const struct command commands[] = {
    /* Profiles */
    { "set",            0, cmd_set,             SEC_PROFILES,  "<profile> [--nosafe]",            "Apply a power profile", NULL },
    { "editconf",       0, cmd_editconf,        SEC_PROFILES,  "",                                "Edit custom profiles with your configured editor", NULL },

    /* Keyboard */
    { "kbc",            0, cmd_kbc,             SEC_KEYBOARD,  "<R G B | preset>",                "Set keyboard color (no arg: list presets)", NULL },
    { "kbb",            0, cmd_kbb,             SEC_KEYBOARD,  "<pct>",                           "Set brightness (0-100%)", NULL },
    { "kbe",            0, cmd_kbe,             SEC_KEYBOARD,  "[effect|stop]",                   "Keyboard backlight effects (no arg: shows effect preset list & status)", NULL },
    { "fn",             1, cmd_fnlock,          SEC_KEYBOARD,  "[lock|unlock]",                   "Toggle/set Fn Lock (Fn key behavior)", NULL },

    /* Fan */
    { "fan",            0, cmd_fan,             SEC_FAN,       "auto|max",                          "Set both fans (EC-controlled / full)", NULL },
    { "fan",            0, cmd_fan,             SEC_FAN,       "silent [--nosafe]",                 "Quiet mode (forces eco profile first; bypass with --nosafe)", NULL },
    { "fan",            0, cmd_fan,             SEC_FAN,       "<pct> --nosafe",                    "Set both fans to duty (25-100%)", NULL },
    { "fan",            0, cmd_fan,             SEC_FAN,       "cpu|gpu <pct> --nosafe",             "Set individual fan duty (25-100%)", NULL },
    { "fan",            0, cmd_fan,             SEC_FAN,       "cpu|gpu auto",                      "Restore individual fan to auto (independent)", NULL },

    /* GPU MUX */
    { "mux",            0, cmd_mux,             SEC_MUX,       "",                                "Show current MUX mode (MSHybrid / dGPU)", NULL },
    { "mux",            0, cmd_mux,             SEC_MUX,       "switch",                          "Toggle to the other mode (reboot to apply)", NULL },

    /* Privacy */
    { "webcam",         1, cmd_webcam,          SEC_PRIVACY,   "[on|off]",                        "Toggle/set webcam", NULL },
    { "mic",            0, cmd_mic,             SEC_PRIVACY,   "[on|off]",                        "Toggle/set internal microphone (laptop mic only, needs alsa/amixer)", NULL },

    /* Battery */
    { "bat",            0, cmd_bat,             SEC_BATTERY,   "",                                "Show current thresholds", NULL },
    { "bat",            0, cmd_bat,             SEC_BATTERY,   "<start> <stop>",                   "Set charge thresholds (custom)", NULL },
    { "bat",            0, cmd_bat,             SEC_BATTERY,   "max",                              "standard mode (charge to 100%, resume at 95%)", NULL },

    /* Info */
    { "status",         1, cmd_status,          SEC_INFO,      "[--json]",                        "Show current settings (JSON with --json)", NULL },
    { "monitor",        1, cmd_monitor,         SEC_INFO,      "[--text|--json]",                 "Live visual simple monitor (--text/--json available)", NULL },
    { "capabilities",   0, cmd_capabilities,    SEC_INFO,      "[--json]",                        "Show controls available on this system", NULL },

    /* NVIDIA */
#ifdef CCTL_NVIDIA
    { "nvidia",         0, cmd_nvidia,          SEC_NVIDIA,    "<on|off>",                        "Persistent toggle (+initramfs rebuild)", NULL },
    { "nvidia",         0, cmd_nvidia,          SEC_NVIDIA,    "load",                            "Session load (compute modules)", NULL },
    { "nvidia",         0, cmd_nvidia,          SEC_NVIDIA,    "loadgame",                        "Session load (all modules incl. drm)", NULL },
    { "nvidia",         0, cmd_nvidia,          SEC_NVIDIA,    "unload",                          "Session unload + power off", NULL },
    { "nvidia",         0, cmd_nvidia,          SEC_NVIDIA,    "status",                          "Show GPU status & telemetry", NULL },
#endif
    { "nvidia",         0, cmd_nvidia,          SEC_NVIDIA,    "clock    [min] <max>|reset",      "Lock/unlock GPU clocks (no arg: show max clock)", NULL },
    { "nvidia",         0, cmd_nvidia,          SEC_NVIDIA,    "memclock [min] <max>|reset",      "Lock/unlock memory clocks (no arg: show max clock)", NULL },

    /* Display */
    { "rr",             0, cmd_rr,              SEC_DISPLAY,   "[rate]",                          "List/set refresh rate (1=high, 2=low)", has_display_support },
    { "scale",          0, cmd_scale,           SEC_DISPLAY,   "<factor|WxH|off>",                "GPU-side scaling (no arg: explain in detail)", has_display_support },

    /* Overrides */
    { "turbo",          0, cmd_turbo,           SEC_OVERRIDES, "<on|off> [--nosafe]",              "Turbo boost override (on sets fans to auto; bypass with --nosafe)", NULL },
    { "gov",            0, cmd_gov,             SEC_OVERRIDES, "<governor>",                       "CPU governor (powersave, performance)", NULL },
    { "epp",            0, cmd_epp,             SEC_OVERRIDES, "<value>",                          "EPP (performance, balance_performance, balance_power, power)", NULL },
    { "rapl",           0, cmd_rapl,            SEC_OVERRIDES, "<pl1> <pl2>",                      "RAPL power limits (watts, use 'skip' to omit)", NULL },

    /* System */
    { "install",        0, cmd_install,         SEC_SYSTEM,    "[--force]",                       "Install/upgrade system-wide + passwordless sudo", not_installed_systemwide },
    { "uninstall",      1, cmd_uninstall,       SEC_SYSTEM,    "",                                "Remove cctl, its profiles, sudo rule, and saved data", is_installed_systemwide },
    { "drivers-manage", 0, cmd_drivers_install, SEC_SYSTEM,    "",                                "Install, reinstall, or uninstall kernel drivers (auto-fetch or offline; sha256-verified)", NULL },
    { "snapshot",       0, cmd_snapshot,         SEC_SYSTEM,    "<action> [number] [--mask fields]", "Save, restore, view, delete, or list snapshots", NULL },
};

struct section_def {
    enum cmd_section section;
    const char *title;
    enum prof_color header_color;
    const char *subtitle;
};

static const struct section_def sections[] = {
    { SEC_PROFILES,  "PROFILES",                    PROF_COL_YLW,     NULL },
    { SEC_KEYBOARD,  "KEYBOARD",                    PROF_COL_MAG,     NULL },
    { SEC_FAN,       "FAN",                         PROF_COL_YLW,     NULL },
    { SEC_MUX,       "GPU MUX",                     PROF_COL_MAG,     "(UEFI NVRAM, reboot required to apply)" },
    { SEC_PRIVACY,   "PRIVACY",                     PROF_COL_CYN,     NULL },
    { SEC_BATTERY,   "BATTERY",                     PROF_COL_GRN,     NULL },
    { SEC_INFO,      "INFO",                        PROF_COL_CYN_BLD, NULL },
    { SEC_NVIDIA,    "NVIDIA",                      PROF_COL_RED,     NULL },
    { SEC_DISPLAY,   "DISPLAY",                     PROF_COL_BLU,     "(only X11 session is supported, needs xrandr)" },
    { SEC_OVERRIDES, "PROFILE INDIVIDUAL OVERRIDES", PROF_COL_YLW,     NULL },
    { SEC_SYSTEM,    "SYSTEM",                      PROF_COL_CYN_BLD, NULL },
};

static void print_help_desc(const char *desc)
{
    const char *paren = strchr(desc, '(');
    if (!paren) {
        printf("%s\n", desc);
    } else {
        printf("%.*s%s%s%s\n", (int)(paren - desc), desc, C_DIM, paren, C_RST);
    }
}

static void print_help_command(const struct command *cmd)
{
    int target_col = (strcmp(cmd->name, "nvidia") == 0) ? 39 : 29;
    if (strcmp(cmd->name, "set") == 0 || strcmp(cmd->name, "turbo") == 0)
        target_col = 31;
    else if (strcmp(cmd->name, "fan") == 0)
        target_col = 36;

    int col = 4;
    printf("    %s%s%s", C_BLD, cmd->name, C_RST);
    col += (int)strlen(cmd->name);

    if (cmd->usage && cmd->usage[0]) {
        int name_col = 6;
        if (strcmp(cmd->name, "mic") == 0 || strcmp(cmd->name, "gov") == 0 ||
            strcmp(cmd->name, "epp") == 0 || strcmp(cmd->name, "turbo") == 0 ||
            strcmp(cmd->name, "nvidia") == 0 || strcmp(cmd->name, "mux") == 0 ||
            strcmp(cmd->name, "rapl") == 0 ||
            (strcmp(cmd->name, "bat") == 0 && strcmp(cmd->usage, "max") != 0))
            name_col = 7;
        else if (strcmp(cmd->name, "bat") == 0 && strcmp(cmd->usage, "max") == 0)
            name_col = 4;

        int name_len = (int)strlen(cmd->name);
        int gap = name_col > name_len ? name_col - name_len : 1;
        for (int i = 0; i < gap; i++) { putchar(' '); col++; }
        printf("%s", cmd->usage);
        col += (int)strlen(cmd->usage);
    }

    if (col < target_col) {
        for (int i = col; i < target_col; i++) putchar(' ');
    } else {
        putchar(' ');
    }

    print_help_desc(cmd->desc);
}

static void print_usage(const char *prog)
{
    const char *base = strrchr(prog, '/');
    prog = base ? base + 1 : prog;

    /* COLORCONTROL banner supplied by the user. */
    printf(
    "\n"
    "   %s____ ___  _     ___  ____  %s %s____ ___  _   _ _____ ____   ___  _%s\n"
    "  %s/ ___/ _ \\| |   / _ \\|  _ \\ %s%s/ ___/ _ \\| \\ | |_   _|  _ \\ / _ \\| |%s\n"
    " %s| |  | | | | |  | | | | |_) |%s%s |  | | | |  \\| | | | | |_) | | | | |%s\n"
    " %s| |__| |_| | |__| |_| |  _ <|%s%s |__| |_| | |\\  | | | |  _ <| |_| | |___%s\n"
    "  %s\\____\\___/|_____\\___/|_| \\_\\%s%s\\____\\___/|_| \\_| |_| |_| \\_\\\\___/|_____|%s\n"
    "\n",
    C_GRN, C_RST, C_RED, C_RST,
    C_GRN, C_RST, C_RED, C_RST,
    C_GRN, C_RST, C_RED, C_RST,
    C_GRN, C_RST, C_RED, C_RST,
    C_GRN, C_RST, C_RED, C_RST);

    printf("  %sUsage:%s  %s%s%s <command> [options]\n\n", C_BLD, C_RST, C_CYN_BLD, prog, C_RST);
    printf("  %sAppend --dry-run to supported commands; snapshot restore can preview saved settings.%s\n\n", C_DIM, C_RST);

    for (size_t s = 0; s < sizeof(sections) / sizeof(sections[0]); s++) {
        const struct section_def *sec = &sections[s];
        if (sec->section == SEC_DISPLAY && !has_display_support())
            continue;

        if (sec->subtitle)
            printf("  %s%s%s %s%s%s\n", prof_color_str(sec->header_color), sec->title, C_RST, C_DIM, sec->subtitle, C_RST);
        else
            printf("  %s%s%s\n", prof_color_str(sec->header_color), sec->title, C_RST);

        if (sec->section == SEC_PROFILES) {
            for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
                const struct command *cmd = &commands[i];
                if (cmd->section != sec->section) continue;
                print_help_command(cmd);
                printf("\n");
            }
            printf("      %s(max, cpuperf, balanced set fans to auto; bypass with --nosafe)%s\n\n", C_DIM, C_RST);
            print_profile_table();
            continue;
        }

        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
            const struct command *cmd = &commands[i];
            if (cmd->section != sec->section) continue;
            if (cmd->visible && !cmd->visible()) continue;
            print_help_command(cmd);
        }
        printf("\n");
    }

    if (!drivers_loaded()) {
        printf("  %sDRIVERS NOT LOADED%s — some features need them:\n", C_YLW, C_RST);
        printf("    • %skbc/kbb%s   keyboard backlight (%stuxedo_keyboard%s)\n", C_CYN, C_RST, C_DIM, C_RST);
        printf("    • %sset%s GPU performance slots (%stuxedo_io%s)\n", C_CYN, C_RST, C_DIM, C_RST);
        printf("    • %sbat%s      battery charge thresholds (%sclevo_acpi%s)\n", C_CYN, C_RST, C_DIM, C_RST);
        printf("    Fix: run %scctl drivers-manage%s\n\n",
               C_BLD, C_RST);
    }

    if (!is_installed_systemwide()) {
        printf("  %sNOT INSTALLED%s — run %ssudo ./%s install%s to set up:\n",
               C_YLW, C_RST, C_BLD, prog, C_RST);
        printf("    • Adds cctl to your PATH — run %scctl%s from anywhere\n", C_CYN, C_RST);
        printf("    • Passwordless sudo — %ssudo cctl <cmd>%s never prompts for a password\n", C_CYN, C_RST);
        printf("    • Auto-elevation — %scctl%s elevates automatically via passwordless sudo\n\n", C_CYN, C_RST);
    }

    printf("  %sv%s%s\n", C_DIM, CCTL_VERSION, C_RST);
}

int main(int argc, char **argv)
{
    /* Snapshot restore handles --dry-run itself so it can retain the flag
     * across sudo re-execution while reading the root-only snapshot file. */
    int snapshot_command = argc >= 2 && strcasecmp(argv[1], "snapshot") == 0;
    if (!snapshot_command) {
        for (int i = 2; i < argc; ) {
            if (strcmp(argv[i], "--dry-run") == 0) {
                dry_run = 1;
                for (int j = i; j < argc; j++) argv[j] = argv[j + 1];
                argc--;
            } else {
                i++;
            }
        }
    }

    json_output = (argc >= 3 &&
                   ((strcasecmp(argv[1], "status") == 0) ||
                    (strcasecmp(argv[1], "monitor") == 0)) &&
                   strcmp(argv[2], "--json") == 0);
    use_color = isatty(STDOUT_FILENO) && !json_output;
    init_colors();

    /* Root runs get a pinned PATH: helper tools invoked later (amixer, tar,
     * sha256sum, chattr, install, ...) must resolve to the real system
     * binaries, never to whatever a user-writable directory in the
     * inherited PATH might contain. Mirrors sudo's secure_path. */
    if (geteuid() == 0)
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);

    atexit(ec_release_ports);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Normalize the command verb to lowercase. */
    for (char *p = argv[1]; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    if (dry_run && strcmp(argv[1], "set") != 0 && strcmp(argv[1], "fan") != 0 &&
        strcmp(argv[1], "turbo") != 0 &&
        strcmp(argv[1], "gov") != 0 && strcmp(argv[1], "epp") != 0 &&
        strcmp(argv[1], "rapl") != 0 && strcmp(argv[1], "bat") != 0)
        return fail("--dry-run is supported for set, fan, turbo, gov, epp, rapl, and bat");

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("cctl %s\n", CCTL_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "--microversion") == 0) {
        printf("%d\n", CCTL_MICROVERSION);
        return 0;
    }

    /* Private helper used by the unprivileged X11 scale command to update
     * root-owned runtime metadata after xrandr succeeds. */
    if (strcmp(argv[1], "--cctl-record-display-scale") == 0) {
        if (geteuid() != 0 || argc != 3)
            return fail("Invalid internal display scale record request");
        return mode_record_display_scale_root(strcmp(argv[2], "none") == 0 ? NULL : argv[2]);
    }

    /* Hidden diagnostic (like --microversion: no help, no README) —
     * prints the sha256 baked into this binary for driver verification. */
    if (strcmp(argv[1], "--shasecret") == 0) {
        printf("%s\n", DRIVERS_SHA256);
        return 0;
    }

    int cmd_found = 0;
    int rc = 0;
    size_t num_cmds = sizeof(commands) / sizeof(commands[0]);
    for (size_t i = 0; i < num_cmds; i++) {
        if (strcmp(commands[i].name, argv[1]) == 0) {
            cmd_found = 1;
            if (commands[i].needs_root && !dry_run)
                require_root(argc, argv);
            rc = commands[i].handler(argc, argv);
            break;
        }
    }

    if (!cmd_found) {
        fail("Unknown command '%s'", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    return (rc != 0) ? 1 : 0;
}
