/* ========================================================================
 * DISPLAY SCALING (xrandr --scale-from)
 * ======================================================================== */

/* Implemented with the driver archive helpers in cctl_drivers_cli.inc. */
static int drivers_cache_warm(const char *binary_path, int *stale_cache);

static int scale_set(const char *resolution)
{
    struct display_info info;
    if (query_display_info(&info) < 0)
        return fail("failed to query display info");
    if (!info.internal)
        return fail("the internal laptop panel is not active (scaling is internal-only)");

    char *const args[] = {
        "xrandr", "--output", info.output, "--mode", info.resolution,
        "--rate", info.current_rate, "--scale-from", (char *)resolution, NULL
    };
    if (run(args, RUN_SILENT, NULL, 0) != 0)
        return fail("Failed to set scale to %s", resolution);
    cctl_stream_printf("  Display scale: %s → %s%s\n", resolution, info.resolution, snapshot_applied_tag());
    return 0;
}

static int scale_reset(void)
{
    struct display_info info;
    if (query_display_info(&info) < 0)
        return fail("failed to query display info");
    if (!info.internal)
        return fail("the internal laptop panel is not active (scaling is internal-only)");

    char *const args[] = {
        "xrandr", "--output", info.output, "--mode", info.resolution,
        "--rate", info.current_rate, "--scale", "1x1", NULL
    };
    if (run(args, RUN_SILENT, NULL, 0) != 0)
        return fail("Failed to reset scale");
    cctl_stream_printf("  Scale: native (1x1)%s\n", snapshot_applied_tag());
    return 0;
}

static void scale_show_details(void)
{
    cctl_stream_printf("%sUsage:%s %scctl scale <factor | resolution | off>%s\n\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
    cctl_stream_printf("The internal display does not offer these lower resolutions as native modes.\n"
           "cctl uses X11 display scaling to show the desktop at a requested size\n"
           "while keeping the panel at its native resolution.\n\n");

    struct display_info info;
    if (has_display_support() && query_display_info(&info) == 0 && info.resolution[0]) {
        cctl_stream_printf("%sCurrent Display:%s\n", C_YLW, C_RST);
        cctl_stream_printf("  %sOutput:%s %s  |  %sNative Resolution:%s %s  |  %sCurrent Rate:%s %sHz\n\n",
               C_CYN, C_RST, info.output, C_CYN, C_RST, info.resolution, C_CYN, C_RST, info.current_rate);
        cctl_stream_printf("  %sCurrent Desktop Size:%s %s (from xrandr)\n\n",
               C_CYN, C_RST, info.current_resolution[0] ? info.current_resolution : "N/A");
    }

    cctl_stream_printf("%sOptions:%s\n", C_YLW, C_RST);
    cctl_stream_printf("  %s%-15s%s Fraction between 0.01 and 1.0 (e.g. 0.75 for 75%%, 0.5 for 50%%)\n", C_CYN, "<factor>", C_RST);
    cctl_stream_printf("  %s%-15s%s Explicit WIDTHxHEIGHT (e.g. 1920x1080, 1600x900, 1280x720)\n", C_CYN, "<resolution>", C_RST);
    cctl_stream_printf("  %s%-15s%s Restore native 1:1 display resolution\n\n", C_CYN, "off | reset", C_RST);

    cctl_stream_printf("%sExamples:%s\n", C_YLW, C_RST);
    cctl_stream_printf("  %scctl scale 0.75%s         Set display scale to 75%% (1080p area on a 1440p panel)\n", C_CYN, C_RST);
    cctl_stream_printf("  %scctl scale 1920x1080%s    Set display scale using a 1920x1080 area\n", C_CYN, C_RST);
    cctl_stream_printf("  %scctl scale 0.5%s          Set display scale to 50%%\n", C_CYN, C_RST);
    cctl_stream_printf("  %scctl scale off%s          Reset back to native display resolution\n", C_CYN, C_RST);
}

static int cmd_scale(int argc, char **argv)
{
    if (argc < 3) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        scale_show_details();
        g_stream_mode = prev;
        return 0;
    }

    if (argc > 3)
        return fail("Unexpected extra argument '%s'", argv[3]);

    const char *arg = argv[2];
    if (strcmp(arg, "off") == 0 || strcmp(arg, "reset") == 0)
        return scale_reset();

    /* Check if argument is a number (factor) or a resolution string */
    char *end = NULL;
    errno = 0;
    double factor = strtod(arg, &end);
    if (end != arg && *end == '\0') {
        if (factor < 0.01 || factor > 1.0)
            return fail("Scale factor must be between 0.01 and 1.0");
        struct display_info info;
        if (query_display_info(&info) < 0)
            return fail("failed to query display info");
        /* Parse native resolution */
        int w, h;
        if (sscanf(info.resolution, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
            return fail("cannot parse native resolution '%s'", info.resolution);
        int sw = (int)(w * factor);
        int sh = (int)(h * factor);
        /* Keep even numbers to avoid xrandr issues */
        if (sw % 2) sw--;
        if (sh % 2) sh--;
        if (sw < 640 || sh < 360)
            return fail("Scaled resolution %dx%d is too small (minimum 640x360)", sw, sh);
        char res[32];
        snprintf(res, sizeof(res), "%dx%d", sw, sh);
        cctl_stream_printf("  Factor: %.2f → %s (from %s)\n", factor, res, info.resolution);
        int rc = scale_set(res);
        if (rc == 0) print_done();
        return rc;
    }

    /* Resolution string (contains 'x') */
    int rw = 0, rh = 0;
    char extra = 0;
    if (sscanf(arg, "%dx%d%c", &rw, &rh, &extra) != 2 || rw < 640 || rh < 360)
        return fail("Invalid or too small resolution '%s' (minimum 640x360)", arg);
    int rc = scale_set(arg);
    if (rc == 0) print_done();
    return rc;
}

static int cmd_options(int argc, char **argv)
{
    if (argc == 2) {
        struct cctl_settings settings;
        (void)cctl_settings_read(&settings);
        int prev_mode = g_stream_mode;
        int prev_line = g_stream_line;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        cctl_stream_printf("%sCCTL-specific settings:%s\n", C_YLW, C_RST);
        cctl_stream_printf("  %s%-18s%s %s%-6s%s  Stream output character-by-character or by line\n",
               C_CYN, "streamtext", C_RST,
               settings.streamtext_enabled ? C_GRN : C_DIM,
               settings.streamtext_enabled ? "on" : "off", C_RST);
        cctl_stream_printf("  %s%-18s%s %s%-6s%s  What bare 'cctl' shows: help or status\n",
               C_CYN, "default", C_RST, C_GRN,
               settings.default_is_status ? "status" : "help", C_RST);
        cctl_stream_printf("  %s%-18s%s %s%-6s%s  Enable experimental fan controls and show in help\n",
               C_CYN, "experimental-fan", C_RST,
               settings.experimental_fan_enabled ? C_GRN : C_DIM,
               settings.experimental_fan_enabled ? "on" : "off", C_RST);
        cctl_stream_printf("\n%sUsage:%s %scctl options streamtext <on|off>%s\n",
               C_BLD, C_RST, C_CYN, C_RST);
        cctl_stream_printf("       %scctl options default <help|status>%s\n",
               C_CYN, C_RST);
        cctl_stream_printf("       %scctl options experimental-fan <on|off>%s\n",
               C_CYN, C_RST);
        g_stream_mode = prev_mode;
        g_stream_line = prev_line;
        return 0;
    }

    const char *opt_name = NULL;
    const char *opt_val = NULL;
    if (argc == 4) {
        opt_name = argv[2];
        opt_val = argv[3];
    } else if (argc == 3 && (strcmp(argv[2], "status") == 0 || strcmp(argv[2], "help") == 0)) {
        opt_name = "default";
        opt_val = argv[2];
    } else {
        return fail("Usage: cctl options streamtext <on|off>\n       cctl options default <help|status>\n       cctl options experimental-fan <on|off>");
    }

    require_root(argc, argv);

    struct cctl_settings settings;
    (void)cctl_settings_read(&settings);

    if (strcmp(opt_name, "streamtext") == 0) {
        if (strcmp(opt_val, "on") != 0 && strcmp(opt_val, "off") != 0)
            return fail("Usage: cctl options streamtext <on|off>");
        settings.streamtext_enabled = (strcmp(opt_val, "on") == 0);
        if (cctl_settings_write(&settings) != 0)
            return fail("Could not save settings to %s", CCTL_SETTINGS_FILE);
        g_stream_text_enabled = settings.streamtext_enabled;
        cctl_stream_printf("Text streaming %s.\n", settings.streamtext_enabled ? "enabled" : "disabled");
        return 0;
    }

    if (strcmp(opt_name, "default") == 0 || strcmp(opt_name, "default_action") == 0 ||
        strcmp(opt_name, "default_cmd") == 0 || strcmp(opt_name, "action") == 0) {
        if (strcmp(opt_val, "help") != 0 && strcmp(opt_val, "status") != 0)
            return fail("Usage: cctl options default <help|status>");
        settings.default_is_status = (strcmp(opt_val, "status") == 0);
        if (cctl_settings_write(&settings) != 0)
            return fail("Could not save settings to %s", CCTL_SETTINGS_FILE);
        cctl_stream_printf("Default action on bare 'cctl' set to %s.\n",
               settings.default_is_status ? "status" : "help");
        return 0;
    }

    if (strcmp(opt_name, "experimental-fan") == 0) {
        if (strcmp(opt_val, "on") != 0 && strcmp(opt_val, "off") != 0)
            return fail("Usage: cctl options experimental-fan <on|off>");
        settings.experimental_fan_enabled = (strcmp(opt_val, "on") == 0);
        if (cctl_settings_write(&settings) != 0)
            return fail("Could not save settings to %s", CCTL_SETTINGS_FILE);
        cctl_stream_printf("Experimental fan controls %s and %s in help.\n",
               settings.experimental_fan_enabled ? "enabled" : "disabled",
               settings.experimental_fan_enabled ? "shown" : "hidden");
        return 0;
    }

    return fail("Unknown setting '%s'. Available: streamtext, default, experimental-fan", opt_name);
}

static int cmd_mic(int argc, char **argv)
{
    int rc;
    if (argc >= 3 && strcmp(argv[2], "on") == 0)
        rc = mic_set(1);
    else if (argc >= 3 && strcmp(argv[2], "off") == 0)
        rc = mic_set(0);
    else
        rc = mic_toggle();
    if (rc == 0) print_done();
    return rc;
}

static int cmd_gpumon(int argc, char **argv)
{
    return cctl_gpumon(argc, argv);
}

static int cmd_cpumon(int argc, char **argv)
{
    return cctl_cpumon(argc, argv);
}

static int experimental_fan_controls_enabled(void)
{
    struct cctl_settings settings;
    (void)cctl_settings_read(&settings);
    return settings.experimental_fan_enabled;
}

static int editconf_write_all(int fd, const char *buf, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = write(fd, buf + offset, len - offset);
        if (n > 0) offset += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static int editconf_install_helper(const char *src, uid_t owner)
{
    static const char prefix[] = "/tmp/cctl-profiles-edit-";
    if (strncmp(src, prefix, sizeof(prefix) - 1) != 0 ||
        strchr(src + sizeof(prefix) - 1, '/') != NULL)
        return fail("Invalid temporary custom-profile path");

    char err[256];
    char *text = fc_read_owned_text(src, owner, err, sizeof(err));
    if (!text) return fail("%s", err);

    const char *dir = "/var/lib/cctl";
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        free(text);
        return fail("Could not create %s: %s", dir, strerror(errno));
    }
    struct stat dst;
    if (lstat(dir, &dst) != 0 || !S_ISDIR(dst.st_mode) || S_ISLNK(dst.st_mode) ||
        dst.st_uid != 0 || (dst.st_mode & 0022) != 0) {
        free(text);
        return fail("Refusing to write profiles through an unsafe %s directory", dir);
    }

    char temp[PATH_MAX];
    int n = snprintf(temp, sizeof(temp), "%s/.profiles.conf.XXXXXX", dir);
    if (n < 0 || (size_t)n >= sizeof(temp)) {
        free(text);
        return fail("Custom-profile path is too long");
    }
    int fd = mkstemp(temp);
    if (fd < 0) {
        free(text);
        return fail("Could not create a temporary profile file: %s", strerror(errno));
    }
    int rc = 0;
    if (fchmod(fd, 0644) != 0 ||
        editconf_write_all(fd, text, strlen(text)) != 0 || fsync(fd) != 0)
        rc = -1;
    free(text);
    if (close(fd) != 0) rc = -1;
    if (rc == 0 && rename(temp, USER_PROFILE_FILE) != 0) rc = -1;
    if (rc != 0) {
        int saved_errno = errno;
        unlink(temp);
        return fail("Could not save %s: %s", USER_PROFILE_FILE, strerror(saved_errno));
    }
    return 0;
}

static int editconf_make_temp(char *tmp_path, size_t path_size)
{
    if (path_size < sizeof("/tmp/cctl-profiles-edit-XXXXXX")) return -1;
    snprintf(tmp_path, path_size, "/tmp/cctl-profiles-edit-XXXXXX");
    int out = mkstemp(tmp_path);
    if (out < 0) return -1;
    if (fchmod(out, 0600) != 0) goto fail;

    int in = open(USER_PROFILE_FILE, O_RDONLY | O_NOFOLLOW);
    if (in >= 0) {
        struct stat st;
        if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            st.st_size > FANCURVE_FILE_MAX) {
            close(in);
            goto fail;
        }
        char buf[4096];
        size_t total = 0;
        for (;;) {
            ssize_t n = read(in, buf, sizeof(buf));
            if (n > 0) {
                if ((size_t)n > FANCURVE_FILE_MAX - total) {
                    close(in);
                    errno = EFBIG;
                    goto fail;
                }
                if (editconf_write_all(out, buf, (size_t)n) != 0) {
                    close(in);
                    goto fail;
                }
                total += (size_t)n;
            } else if (n == 0) {
                break;
            } else if (errno != EINTR) {
                close(in);
                goto fail;
            }
        }
        close(in);
    } else if (errno == ENOENT) {
        static const char header[] =
            "# Custom profiles: one profile per line; use 'skip' to leave a setting unchanged.\n"
            "# name ec_profile_code turbo governor epp pl1_watts pl2_watts [refresh_rate]\n"
            "# example 3 1 powersave performance 40 60 144\n";
        if (editconf_write_all(out, header, sizeof(header) - 1) != 0) goto fail;
    } else {
        goto fail;
    }

    if (close(out) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;

fail:
    close(out);
    unlink(tmp_path);
    return -1;
}

static int cmd_editconf(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[2], "__install") == 0) {
        if (geteuid() != 0)
            return fail("Internal profile installer must run as root");
        return editconf_install_helper(argv[3], fc_invoking_uid());
    }
    if (argc > 2)
        return fail("Unexpected extra argument '%s'", argv[2]);

    const char *editor = getenv("SUDO_EDITOR");
    if (!editor || !*editor) editor = getenv("VISUAL");
    if (!editor || !*editor) editor = getenv("EDITOR");
    if (!editor || !*editor) {
        if (command_exists("nano")) editor = "nano";
        else if (command_exists("micro")) editor = "micro";
        else return fail("No editor configured and neither nano nor micro is installed; "
                         "set SUDO_EDITOR, VISUAL, or EDITOR");
    }
    char temp[PATH_MAX];
    if (editconf_make_temp(temp, sizeof(temp)) != 0)
        return fail("Could not prepare a temporary custom-profile file: %s", strerror(errno));
    if (setenv("EDITOR", editor, 1) != 0) {
        unlink(temp);
        return fail("Could not set the configured editor");
    }

    char *const edit_args[] = { "/bin/sh", "-c", "exec $EDITOR \"$1\"",
                                "cctl-editconf", temp, NULL };
    int rc = run(edit_args, RUN_RAW, NULL, 0);
    if (rc != 0) {
        unlink(temp);
        return fail("Editor exited with status %d", rc);
    }

    if (geteuid() == 0) {
        rc = editconf_install_helper(temp, getuid());
        unlink(temp);
        if (rc == 0) cctl_stream_printf("Saved custom profiles to %s\n", USER_PROFILE_FILE);
        return rc;
    }

    char exe[PATH_MAX];
    if (get_self_exe(exe, sizeof(exe)) < 0) {
        unlink(temp);
        return fail("Could not determine the cctl binary path");
    }
    char *const install_args[] = { "sudo", exe, "editconf", "__install", temp, NULL };
    rc = run(install_args, RUN_RAW, NULL, 0);
    unlink(temp);
    if (rc != 0)
        return fail("Could not save custom profiles (installer exited with status %d)", rc);
    cctl_stream_printf("Saved custom profiles to %s\n", USER_PROFILE_FILE);
    return 0;
}

static void print_max_profile_note(void)
{
    cctl_stream_printf("\n%sNote:%s 'max' can sustain 60–90W and reach ~98°C; use for demanding workloads.\n",
           C_YLW, C_RST);
}

static int cmd_set(int argc, char **argv)
{
    int nosafe = 0;
    const char *pos[1] = {NULL};
    int npos = parse_flags(argc, argv, &nosafe, pos, 1);
    if (npos < 0) return 1;

    if (npos == 0) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        cctl_stream_printf("%sUsage:%s %scctl %s <profile> [--nosafe]%s\n\n",
               C_BLD, C_RST, C_CYN_BLD, "set", C_RST);
        cctl_stream_printf("%sValid Profiles:%s\n", C_YLW, C_RST);
        print_profile_table();
        g_stream_mode = prev;
        return 0;
    }

    const char *profile = pos[0];
    const struct profile_def *p = find_profile(profile);
    if (!p) {
        cctl_stream_fprintf(stderr, "Error: Unknown profile '%s'\n", profile);
        cctl_stream_fprintf(stderr, "Valid profiles: ");
        for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
            const char *name = strcmp(PROFILES[i].name, "silent") == 0
                             ? "silent/eco" : PROFILES[i].name;
            cctl_stream_fprintf(stderr, "%s%s", name,
                    i + 1 < sizeof(PROFILES) / sizeof(PROFILES[0]) ? ", " : "\n");
        }
        for (int i = 0; i < user_profile_count; i++)
            cctl_stream_fprintf(stderr, "%s%s", user_profiles[i].name,
                    i + 1 < user_profile_count ? ", " : "\n");
        return 1;
    }

    if (dry_run) {
        stream_printf(STREAM_DELAY_US, "Dry run: profile %s\n",
                      strcmp(p->name, "silent") == 0 ? "silent/eco" : p->name);
        if (p->gpu_profile >= 0)
            stream_printf(STREAM_DELAY_US, "  EC profile code: %s (%d)\n", gpu_profile_name(p->gpu_profile), p->gpu_profile);
        else stream_printf(STREAM_DELAY_US, "  EC profile code: --\n");
        stream_printf(STREAM_DELAY_US, "  Turbo: %s; governor: %s; EPP: %s\n",
               p->turbo < 0 ? "--" : (p->turbo ? "on" : "off"),
               strcmp(p->gov, "skip") == 0 ? "--" : p->gov,
               strcmp(p->epp, "skip") == 0 ? "--" : p->epp);
        if (p->rapl_pl1 > 0 || p->rapl_pl2 > 0) {
            char pl1[16], pl2[16];
            if (p->rapl_pl1 > 0) snprintf(pl1, sizeof(pl1), "%dW", p->rapl_pl1);
            else snprintf(pl1, sizeof(pl1), "unchanged");
            if (p->rapl_pl2 > 0) snprintf(pl2, sizeof(pl2), "%dW", p->rapl_pl2);
            else snprintf(pl2, sizeof(pl2), "unchanged");
            stream_printf(STREAM_DELAY_US, "  RAPL limits: PL1 %s; PL2 %s (preview only)\n", pl1, pl2);
        } else {
            stream_printf(STREAM_DELAY_US, "  RAPL limits: unchanged (use cctl rapl to change them)\n");
        }
        if (p->rr && *p->rr && strcmp(p->rr, "skip") != 0 && has_display_support()) {
            if (strcmp(p->rr, "lowest") == 0) {
                struct display_info dinfo;
                if (is_x11_xrandr_available(&dinfo) && dinfo.rate_count > 0 &&
                    dinfo.internal) {
                    int min_idx = 0;
                    double min_val = strtod(dinfo.available_rates[0], NULL);
                    for (int j = 1; j < dinfo.rate_count; j++) {
                        double v = strtod(dinfo.available_rates[j], NULL);
                        if (v > 0.0 && (min_val <= 0.0 || v < min_val)) { min_val = v; min_idx = j; }
                    }
                    if (strcmp(p->name, "powersave") == 0)
                        stream_printf(STREAM_DELAY_US, "  Refresh rate: %sHz (you can change refresh rate in this mode using cctl rr) (preview only)\n", dinfo.available_rates[min_idx]);
                    else
                        stream_printf(STREAM_DELAY_US, "  Refresh rate: %sHz (preview only)\n", dinfo.available_rates[min_idx]);
                }
            } else {
                stream_printf(STREAM_DELAY_US, "  Refresh rate: %sHz (preview only)\n", p->rr);
            }
        }
        struct fancurve_state curve_st;
        int curve_active = fc_any_active(&curve_st);
        if (profile_requires_fan_auto(p) && !nosafe) {
            if (curve_active)
                stream_printf(STREAM_DELAY_US, "  Fans: left to the active fan curve (auto safety skipped)\n");
            else
                stream_printf(STREAM_DELAY_US, "  Fans: restore automatic control (safety default)\n");
        }
        if (strcmp(p->name, "max") == 0) {
            int prev = g_stream_mode;
            g_stream_mode = STREAM_MODE_LINE;
            g_stream_line = 0;
            print_max_profile_note();
            g_stream_mode = prev;
        }
        return 0;
    }

    require_root(argc, argv);

    struct fancurve_state curve_st;
    int curve_active = fc_is_running(&curve_st);
    int wants_auto = profile_requires_fan_auto(p) && !nosafe;

    if (curve_active && wants_auto)
        fc_warn_active(&curve_st);

    if (wants_auto && !curve_active) {
        cctl_stream_printf("Setting fans to AUTO...\n");
        fan_auto_all();
    }
    int rc = profile_apply(p);

    ec_release_ports();
    if (rc == 0) {
        kbe_profile_pulse(profile);
        if (strcmp(profile, "max") == 0) {
            int prev = g_stream_mode;
            g_stream_mode = STREAM_MODE_LINE;
            g_stream_line = 0;
            print_max_profile_note();
            print_done();
            g_stream_mode = prev;
        } else {
            print_done();
        }
    }
    return rc;
}

static int cmd_fan(int argc, char **argv)
{
    int nosafe = 0;
    int admin_mode = 0;
    const char *pos[2] = {NULL, NULL};
    int npos = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--nosafe") == 0) {
            nosafe = 1;
        } else if (strcmp(argv[i], "--am") == 0) {
            admin_mode = 1;
        } else if (npos < 2) {
            pos[npos++] = argv[i];
        } else {
            return fail("Unexpected extra argument '%s'", argv[i]);
        }
    }

    int experimental_enabled = experimental_fan_controls_enabled();
    if (npos == 0) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        cctl_stream_printf("%sUsage:%s %scctl fan <mode> [pct] [--nosafe]%s\n\n",
               C_BLD, C_RST, C_CYN_BLD, C_RST);
        cctl_stream_printf("%sValid Modes:%s\n", C_YLW, C_RST);
        cctl_stream_printf("  %s%-18s%s Automatic EC fan control for both fans\n", C_CYN, "auto", C_RST);
        cctl_stream_printf("  %s%-18s%s Full 100%% fan speed\n", C_CYN, "max", C_RST);
        if (experimental_enabled) {
            cctl_stream_printf("  %s%-18s%s Quiet mode (forces silent/eco profile; bypass with --nosafe)\n", C_CYN, "silent", C_RST);
            cctl_stream_printf("  %s%-18s%s Set both fans to duty 25-100%% (requires --nosafe)\n", C_CYN, "<pct>", C_RST);
            cctl_stream_printf("  %s%-18s%s Set CPU fan duty (25-100%% --nosafe) or 'auto'\n", C_CYN, "cpu <pct|auto>", C_RST);
            cctl_stream_printf("  %s%-18s%s Set GPU fan duty (25-100%% --nosafe) or 'auto'\n", C_CYN, "gpu <pct|auto>", C_RST);
            cctl_stream_printf("  %sManual fan control is experimental and can ignore temperature safeguards.%s\n",
                   C_YLW, C_RST);
        } else {
            cctl_stream_printf("  %sExperimental fan controls are hidden. Enable with:%s cctl options experimental-fan on\n",
                   C_DIM, C_RST);
        }
        g_stream_mode = prev;
        return 0;
    }

    const char *mode = pos[0];
    const char *val_str = (npos >= 2) ? pos[1] : NULL;

    int experimental_request = strcmp(mode, "silent") == 0;
    if ((strcmp(mode, "cpu") == 0 || strcmp(mode, "gpu") == 0) && val_str) {
        int pct;
        experimental_request = strcmp(val_str, "auto") == 0 ||
                               safe_atoi(val_str, &pct) == 0;
    } else if (strcmp(mode, "auto") == 0 && val_str) {
        experimental_request = strcmp(val_str, "cpu") == 0 || strcmp(val_str, "gpu") == 0;
    } else {
        int pct;
        if (safe_atoi(mode, &pct) == 0)
            experimental_request = 1;
    }
    if (experimental_request && !experimental_enabled)
        return fail("Experimental fan controls are disabled. Enable them with 'cctl options experimental-fan on'.");

    require_root(argc, argv);

    int plain_auto = strcmp(mode, "auto") == 0 && !val_str;
    int changes_fans = 0;
    if (strcmp(mode, "auto") == 0 || strcmp(mode, "max") == 0 ||
        strcmp(mode, "silent") == 0) {
        changes_fans = 1;
    } else if ((strcmp(mode, "cpu") == 0 || strcmp(mode, "gpu") == 0) && val_str) {
        changes_fans = 1;
    } else {
        int pct;
        if (safe_atoi(mode, &pct) == 0 && pct >= 0 && pct <= 100)
            changes_fans = 1;
    }

    struct fancurve_state curve_st;
    if (fc_is_running(&curve_st)) {
        if (plain_auto) {
            stream_printf(STREAM_DELAY_US, "Stopping fan curve '%s' and restoring EC auto...\n",
                          curve_st.name);
            fc_stop_worker(1, 0);
        } else if (changes_fans) {
            fc_warn_fan_change(&curve_st);
            return 1;
        }
    }

    int rc = 0;

    if (strcmp(mode, "auto") == 0) {
        if (val_str && strcmp(val_str, "cpu") == 0) {
            stream_printf(STREAM_DELAY_US, "Setting CPU fan to AUTO...\n");
            rc = legacy_fan_auto(FAN_CPU);
        } else if (val_str && strcmp(val_str, "gpu") == 0) {
            stream_printf(STREAM_DELAY_US, "Setting GPU fan to AUTO...\n");
            rc = legacy_fan_auto(FAN_GPU);
        } else {
            stream_printf(STREAM_DELAY_US, "Setting both fans to AUTO...\n");
            rc = fan_auto_all();
        }
    } else if (strcmp(mode, "max") == 0) {
        stream_printf(STREAM_DELAY_US, "Setting both fans to MAX...\n");
        rc = fan_max_all();
    } else if (strcmp(mode, "silent") == 0) {
        if (!nosafe) {
            stream_printf(STREAM_DELAY_US, "Applying: silent/eco profile (safety default for silent fans; use --nosafe to bypass)...\n");
            profile_silent();
        }
        stream_printf(STREAM_DELAY_US, "Setting both fans to SILENT...\n");
        rc = fan_silent_all();
    } else if (strcmp(mode, "cpu") == 0) {
        if (!val_str)
            return fail("Missing duty percentage or 'auto'");
        if (strcmp(val_str, "auto") == 0) {
            stream_printf(STREAM_DELAY_US, "Setting CPU fan to AUTO...\n");
            rc = legacy_fan_auto(FAN_CPU);
        } else {
            int pct;
            if (safe_atoi(val_str, &pct) < 0)
                return fail("Invalid duty percentage '%s' (use 25-100 or 'auto')", val_str);
            if (pct > 100)
                return fail("Fan duty %d%% is above the maximum allowed 100%%", pct);
            if (pct < 25 && !admin_mode)
                return fail("Fan duty %d%% is below the minimum allowed 25%%", pct);
            if (!nosafe)
                return fail_line("Manual fan duty requires --nosafe (e.g. cctl fan cpu %d --nosafe)\n"
                                 "       Fixed duty ignores temperature and may cause overheating or damage.\n"
                                 "       Use manual fan duty for experimental purposes only.", pct);
            rc = fan_set_duty(FAN_CPU, pct, admin_mode);
        }
    } else if (strcmp(mode, "gpu") == 0) {
        if (!val_str)
            return fail("Missing duty percentage or 'auto'");
        if (strcmp(val_str, "auto") == 0) {
            stream_printf(STREAM_DELAY_US, "Setting GPU fan to AUTO...\n");
            rc = legacy_fan_auto(FAN_GPU);
        } else {
            int pct;
            if (safe_atoi(val_str, &pct) < 0)
                return fail("Invalid duty percentage '%s' (use 25-100 or 'auto')", val_str);
            if (pct > 100)
                return fail("Fan duty %d%% is above the maximum allowed 100%%", pct);
            if (pct < 25 && !admin_mode)
                return fail("Fan duty %d%% is below the minimum allowed 25%%", pct);
            if (!nosafe)
                return fail_line("Manual fan duty requires --nosafe (e.g. cctl fan gpu %d --nosafe)\n"
                                 "       Fixed duty ignores temperature and may cause overheating or damage.\n"
                                 "       Use manual fan duty for experimental purposes only.", pct);
            rc = fan_set_duty(FAN_GPU, pct, admin_mode);
        }
    } else {
        /* Try as a plain number — apply to both fans */
        int pct;
        if (safe_atoi(mode, &pct) >= 0) {
            if (pct > 100)
                return fail("Fan duty %d%% is above the maximum allowed 100%%", pct);
            if (pct < 0)
                return fail("Fan duty %d%% is out of range (0-100)", pct);
            if (pct < 25 && !admin_mode)
                return fail("Fan duty %d%% is below the minimum allowed 25%%", pct);
            if (!nosafe)
                return fail_line("Manual fan duty requires --nosafe (e.g. cctl fan %d --nosafe)\n"
                                 "       Fixed duty ignores temperature and may cause overheating or damage.\n"
                                 "       Use manual fan duty for experimental purposes only.", pct);
            stream_printf(STREAM_DELAY_US, "Setting both fans to %d%%...\n", pct);
            rc = fan_set_duty(0, pct, admin_mode);
        } else {
            fail("Unknown fan mode '%s'", mode);
            cctl_stream_fprintf(stderr, "Valid modes: auto [cpu|gpu], max, silent, cpu <pct|auto>, gpu <pct|auto>, or <pct> --nosafe\n");
            return 1;
        }
    }

    ec_release_ports();
    if (rc == 0) print_done();
    return rc;
}

static int cmd_turbo(int argc, char **argv)
{
    int nosafe = 0;
    const char *pos[1] = {NULL};
    int npos = parse_flags(argc, argv, &nosafe, pos, 1);
    if (npos < 0) return 1;

    if (npos == 0) {
        long no_turbo = read_sysfs_long(TURBO_PATH, -1);
        if (no_turbo >= 0)
            stream_printf(STREAM_DELAY_US, "%sCurrent Turbo:%s %s%s%s\n\n",
                   C_YLW, C_RST,
                   no_turbo == 0 ? C_GRN : C_RED,
                   no_turbo == 0 ? "ON" : "OFF",
                   C_RST);
        cctl_stream_printf("%sUsage:%s %scctl turbo <on|off> [--nosafe]%s\n",
               C_BLD, C_RST, C_CYN_BLD, C_RST);
        return 0;
    }

    const char *action = pos[0];
    int enabled;
    if (strcmp(action, "on") == 0)
        enabled = 1;
    else if (strcmp(action, "off") == 0)
        enabled = 0;
    else
        return fail("Invalid turbo action '%s' (use on or off)", action);

    require_root(argc, argv);

    int live_ec_code = enabled ? cctl_live_ec_state(NULL, NULL) : -1;
    if (enabled && !nosafe && ec_profile_needs_fan_auto(live_ec_code)) {
        struct fancurve_state curve_st;
        if (fc_is_running(&curve_st)) {
            fc_warn_active(&curve_st);
        } else {
            stream_printf(STREAM_DELAY_US, "Setting fans to AUTO...\n");
            fan_auto_all();
        }
    }

    int rc = set_turbo(enabled);
    ec_release_ports();
    if (rc == 0) print_done();
    return rc;
}

static int cmd_fnlock(int argc, char **argv)
{
    int rc;
    if (argc >= 3) {
        if (strcmp(argv[2], "lock") == 0 || strcmp(argv[2], "on") == 0)
            rc = set_fnlock(1);
        else if (strcmp(argv[2], "unlock") == 0 || strcmp(argv[2], "off") == 0)
            rc = set_fnlock(0);
        else if (strcmp(argv[2], "toggle") == 0)
            rc = fnlock_toggle();
        else
            return fail("Invalid fn action '%s' (use lock, unlock, on, off, or omit arg to toggle)", argv[2]);
    } else {
        rc = fnlock_toggle();
    }
    if (rc == 0) print_done();
    return rc;
}

static int cmd_gov(int argc, char **argv)
{
    if (argc < 3) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        struct cpu_profile_state cpu_profile;
        read_cpu_profile(&cpu_profile);
        if (cpu_profile.have_governor)
            cctl_stream_printf("%sCurrent Governor:%s %s%s%s\n\n", C_YLW, C_RST, C_CYN,
                   cpu_profile.governor, C_RST);
        cctl_stream_printf("%sUsage:%s %scctl gov <governor>%s\n\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        print_cpu_policy_values("Available Governors", CPU_AVAILABLE_GOVERNORS_PATH);
        g_stream_mode = prev;
        return 0;
    }
    const char *gov = argv[2];
    int available = cpu_policy_value_available(CPU_AVAILABLE_GOVERNORS_PATH, gov);
    if (available < 0)
        return fail("Cannot read available CPU governors from %s",
                    CPU_AVAILABLE_GOVERNORS_PATH);
    if (!available)
        return fail("Governor '%s' is not available; run 'cctl gov' to see available values", gov);

    require_root(argc, argv);
    int rc = set_governor(gov);
    if (rc == 0) print_done();
    return rc;
}

static int cmd_epp(int argc, char **argv)
{
    if (argc < 3) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        struct cpu_profile_state cpu_profile;
        read_cpu_profile(&cpu_profile);
        if (cpu_profile.have_epp)
            cctl_stream_printf("%sCurrent EPP:%s %s%s%s\n", C_YLW, C_RST, C_CYN,
                   cpu_profile.epp, C_RST);
        if (cpu_profile.have_governor && strcmp(cpu_profile.governor, "performance") == 0)
            cctl_stream_printf("%sNote:%s Governor is %sperformance%s — manual EPP control is locked in this mode.\n"
                   "      Change governor to %spowersave%s ('cctl gov powersave') to configure EPP.\n\n",
                   C_YLW, C_RST, C_CYN, C_RST, C_CYN, C_RST);
        else
            cctl_stream_printf("\n");
        cctl_stream_printf("%sUsage:%s %scctl epp <value>%s\n\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        print_cpu_policy_values("Available EPP Values", CPU_AVAILABLE_EPP_PATH);
        g_stream_mode = prev;
        return 0;
    }
    const char *epp = argv[2];
    int available = cpu_policy_value_available(CPU_AVAILABLE_EPP_PATH, epp);
    if (available < 0)
        return fail("Cannot read available EPP values from %s",
                    CPU_AVAILABLE_EPP_PATH);
    if (!available)
        return fail("EPP value '%s' is not available; run 'cctl epp' to see available values", epp);

    char cur_gov[64] = {0};
    if (read_sysfs_str(CPU_GOVERNOR_PATH, cur_gov, sizeof(cur_gov)) >= 0) {
        if (strcmp(cur_gov, "performance") == 0 && strcmp(epp, "performance") != 0) {
            cctl_stream_fprintf(stderr, "Error: CPU governor is currently 'performance' — manual EPP control is only supported in 'powersave' mode.\n");
            cctl_stream_fprintf(stderr, "       Change the governor first: cctl gov powersave\n");
            return 1;
        }
    }

    require_root(argc, argv);
    int rc = set_epp(epp);
    if (rc == 0) print_done();
    return rc;
}

static int cmd_rapl(int argc, char **argv)
{
    if (argc < 3) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        int cur_pl1 = -1, cur_pl2 = -1;
        if (read_rapl_current(&cur_pl1, &cur_pl2) == 0 && cur_pl1 > 0 && cur_pl2 > 0) {
            cctl_stream_printf("%sCurrent RAPL Limits:%s\n", C_YLW, C_RST);
            cctl_stream_printf("  %sPL1:%s %s%dW%s\n", C_CYN, C_RST, C_CYN, cur_pl1, C_RST);
            cctl_stream_printf("  %sPL2:%s %s%dW%s\n\n", C_CYN, C_RST, C_CYN, cur_pl2, C_RST);
        }
        cctl_stream_printf("%sUsage:%s %scctl rapl <pl1_watts> <pl2_watts>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        cctl_stream_printf("       %scctl rapl skip <pl2>%s      (set PL2 only)\n", C_CYN_BLD, C_RST);
        cctl_stream_printf("       %scctl rapl <pl1> skip%s      (set PL1 only)\n", C_CYN_BLD, C_RST);
        cctl_stream_printf("       %scctl rapl <pl2>%s           (one value = PL2 only)\n\n", C_CYN_BLD, C_RST);
        cctl_stream_printf("  %sLimits:%s PL1 1-%dW (up to %dW with EC profile code 2), PL2 up to %dW (must be > PL1)\n",
                C_DIM, C_RST, RAPL_PL1_MAX_WATTS, RAPL_PL1_MAX_WATTS_MAX, RAPL_PL2_MAX_WATTS);
        g_stream_mode = prev;
        return 0;
    }

    if (argc > 4)
        return fail("Unexpected extra argument '%s'", argv[4]);

    require_root(argc, argv);

    int cur_pl1 = -1, cur_pl2 = -1;
    read_rapl_current(&cur_pl1, &cur_pl2);

    int pl1 = 0, pl2 = 0;
    int skip_pl1 = 0, skip_pl2 = 0;
    int pl1_cap = rapl_pl1_ceiling(); /* 45W; 90W only while EC profile code 2 */

    /* A single argument is PL2-only (`cctl rapl 110`), so the PL1 ceiling
     * must not be applied to it. Two arguments parse as PL1 then PL2;
     * "skip" omits one side. */
    if (argc < 4) {
        if (strcmp(argv[2], "skip") == 0)
            return fail("Nothing to set — give a PL2 wattage");
        if (safe_atoi(argv[2], &pl2) < 0 || pl2 < 1 || pl2 > RAPL_PL2_MAX_WATTS)
            return fail("Invalid PL2 value '%s' (use a wattage up to %d, must be > PL1)", argv[2], RAPL_PL2_MAX_WATTS);
        if (cur_pl1 > 0 && pl2 <= cur_pl1)
            return fail("PL2 (%dW) must be greater than current PL1 (%dW)", pl2, cur_pl1);
        skip_pl1 = 1;
    } else {
        /* Parse PL1 */
        if (strcmp(argv[2], "skip") == 0) {
            skip_pl1 = 1;
        } else {
            if (safe_atoi(argv[2], &pl1) < 0 || pl1 < 1)
                return fail("Invalid PL1 value '%s' (use a wattage 1-%d or 'skip')", argv[2], pl1_cap);
            if (pl1 > pl1_cap) {
                cctl_stream_fprintf(stderr, "Error: PL1 %dW exceeds the %dW limit for the current mode\n",
                        pl1, pl1_cap);
                if (pl1_cap == RAPL_PL1_MAX_WATTS)
                    cctl_stream_fprintf(stderr, "       90W PL1 is supported only in 'max' mode (EC profile code 2) "
                                    "(see Mode in 'cctl status')\n");
                return 1;
            }
        }

        /* Parse PL2 */
        if (strcmp(argv[3], "skip") == 0) {
            skip_pl2 = 1;
        } else {
            if (safe_atoi(argv[3], &pl2) < 0 || pl2 < 1)
                return fail("Invalid PL2 value '%s' (use a wattage up to %d or 'skip')", argv[3], RAPL_PL2_MAX_WATTS);
            if (pl2 > RAPL_PL2_MAX_WATTS)
                return fail("PL2 %dW exceeds the %dW platform limit", pl2, RAPL_PL2_MAX_WATTS);
        }

        /* Enforce PL2 > PL1 rule */
        if (!skip_pl1 && !skip_pl2) {
            if (pl2 <= pl1)
                return fail("PL2 (%dW) must be greater than PL1 (%dW)", pl2, pl1);
        } else if (skip_pl1 && !skip_pl2) {
            if (cur_pl1 > 0 && pl2 <= cur_pl1)
                return fail("PL2 (%dW) must be greater than current PL1 (%dW)", pl2, cur_pl1);
        } else if (!skip_pl1 && skip_pl2) {
            if (cur_pl2 > 0 && pl1 >= cur_pl2)
                return fail("PL1 (%dW) must be less than current PL2 (%dW)", pl1, cur_pl2);
        }
    }

    /* Must set at least one limit */
    if (skip_pl1 && skip_pl2)
        return fail("Nothing to set — both PL1 and PL2 are skipped");

    /* Pass ≤ 0 to skip (set_rapl_limits treats pl1_w ≤ 0 as skip) */
    int rc = set_rapl_limits(skip_pl1 ? -1 : pl1, skip_pl2 ? -1 : pl2,
                             pl1_cap);
    if (rc == 0) print_done();
    return rc;
}

static int cmd_kbc(int argc, char **argv)
{
    if (argc < 3) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        kbd_show_presets();
        g_stream_mode = prev;
        return 0;
    }

    require_root(argc, argv);
    kbe_stop(1);
    /* If 3 numeric args → RGB mode */
    if (argc >= 5) {
        int r, g, b;
        if (safe_atoi(argv[2], &r) == 0 && safe_atoi(argv[3], &g) == 0 && safe_atoi(argv[4], &b) == 0) {
            int rc = kbd_set_color(r, g, b);
            if (rc == 0) print_done();
            return rc;
        }
    }
    /* Single arg → preset */
    int rc = kbd_set_preset(argv[2]);
    if (rc == 0) print_done();
    return rc;
}

static int cmd_kbb(int argc, char **argv)
{
    if (argc < 3) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        int cur_bri = kbd_get_brightness();
        if (cur_bri >= 0)
            cctl_stream_printf("%sCurrent Brightness:%s %s%d%%%s\n\n", C_YLW, C_RST, C_CYN, cur_bri, C_RST);
        cctl_stream_printf("%sUsage:%s %scctl kbb <0-100>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        g_stream_mode = prev;
        return 0;
    }
    int pct;
    if (safe_atoi(argv[2], &pct) < 0 || pct < 0 || pct > 100)
        return fail("Brightness must be 0-100%%");

    require_root(argc, argv);
    kbe_stop(1);
    int rc = kbd_set_brightness(pct);
    if (rc == 0) print_done();
    return rc;
}

static int cmd_kbe(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[2], "status") == 0) {
        /* State file is root-only (0600): if it exists but we can't read
         * it, re-run elevated so `cctl kbe` status stays correct for plain
         * users too (seamless via the NOPASSWD rule when installed).
         * No file at all → plain "none", no elevation. */
        if (geteuid() != 0 && access(KBE_STATE_PATH, F_OK) == 0 &&
            access(KBE_STATE_PATH, R_OK) != 0)
            self_elevate(argc, argv);
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        struct kbe_state ks;
        if (kbe_is_running(&ks)) {
            cctl_stream_printf("%sKeyboard Backlight Effect:%s\n", C_YLW, C_RST);
            cctl_stream_printf("  %sStatus:%s              %s%s%s (active, PID %d)\n", C_CYN, C_RST, C_GRN, ks.effect, C_RST, (int)ks.pid);
            const struct kbe_effect_def *cur = kbe_find_effect(ks.effect);
            if (cur && strcmp(cur->name, "temp") == 0) {
                int t = read_cpu_temp();
                if (t > 0)
                    cctl_stream_printf("  %sLive CPU Temp:%s       %s%d°C%s\n", C_CYN, C_RST, t >= 85 ? C_RED : (t >= 70 ? C_YLW : C_GRN), t, C_RST);
            }
            cctl_stream_printf("  %sOriginal Color:%s      RGB(%d, %d, %d)\n", C_CYN, C_RST, ks.orig_r, ks.orig_g, ks.orig_b);
            int bri_pct = (ks.orig_bri * 100 + 127) / 255;
            cctl_stream_printf("  %sOriginal Brightness:%s %d%% (raw %d)\n\n", C_CYN, C_RST, bri_pct, ks.orig_bri);
        } else {
            cctl_stream_printf("%sKeyboard Backlight Effect:%s\n", C_YLW, C_RST);
            cctl_stream_printf("  %sStatus:%s              %snone%s (stopped)\n\n", C_CYN, C_RST, C_DIM, C_RST);
        }
        cctl_stream_printf("%sAvailable Effects:%s\n", C_YLW, C_RST);
        for (size_t i = 0; i < sizeof(kbe_effects) / sizeof(kbe_effects[0]); i++) {
            cctl_stream_printf("  %s%-16s%s %s\n", C_CYN, kbe_effects[i].name, C_RST, kbe_effects[i].desc);
        }
        cctl_stream_printf("\n%sUsage:%s %scctl kbe <effect>  |  cctl kbe stop%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        g_stream_mode = prev;
        return 0;
    }

    require_root(argc, argv);

    const char *sub = argv[2];
    if (strcmp(sub, "stop") == 0 || strcmp(sub, "off") == 0) {
        return kbe_stop(0);
    }

    const struct kbe_effect_def *fx = kbe_find_effect(sub);
    if (!fx) {
        cctl_stream_fprintf(stderr, "Error: Unknown keyboard effect '%s'\n", sub);
        cctl_stream_fprintf(stderr, "Available effects: ");
        for (size_t i = 0; i < sizeof(kbe_effects) / sizeof(kbe_effects[0]); i++) {
            cctl_stream_fprintf(stderr, "%s%s", kbe_effects[i].name,
                    i + 1 < sizeof(kbe_effects) / sizeof(kbe_effects[0]) ? ", " : "\n");
        }
        return 1;
    }

    return kbe_start(fx->name);
}

static int cmd_kbn(int argc, char **argv)
{
    if (argc < 3)
        return fail("Usage: cctl kbn <flash|pulse|sos> <color> [times]\n       cctl kbn police [<color1> <color2> [times]]");
    const char *effect = argv[2];
    int r = 255, g = 0, b = 0, alt_r = 0, alt_g = 0, alt_b = 255;
    int times = 1;
    int police = strcmp(effect, "police") == 0;

    if (police) {
        if (argc == 3) {
            /* Default police colors: red and blue. */
        } else if (argc == 5 || argc == 6) {
            if (kbn_parse_color(argv[3], &r, &g, &b) < 0)
                return fail("Unknown color '%s'; use a kbc preset or #RRGGBB", argv[3]);
            if (kbn_parse_color(argv[4], &alt_r, &alt_g, &alt_b) < 0)
                return fail("Unknown color '%s'; use a kbc preset or #RRGGBB", argv[4]);
            if (argc == 6 &&
                (safe_atoi(argv[5], &times) < 0 || times < 1 || times > 20))
                return fail("Times must be a whole number from 1 to 20");
        } else {
            return fail("Usage: cctl kbn police [<color1> <color2> [times]]");
        }
    } else {
        if (strcmp(effect, "flash") != 0 && strcmp(effect, "pulse") != 0 &&
            strcmp(effect, "sos") != 0)
            return fail("Unknown notification effect '%s'; use flash, pulse, sos, or police", effect);
        if (argc != 4 && argc != 5)
            return fail("Usage: cctl kbn <flash|pulse|sos> <color> [times]");
        if (kbn_parse_color(argv[3], &r, &g, &b) < 0)
            return fail("Unknown color '%s'; use a kbc preset or #RRGGBB", argv[3]);
        if (argc == 5 &&
            (safe_atoi(argv[4], &times) < 0 || times < 1 || times > 20))
            return fail("Times must be a whole number from 1 to 20");
    }

    if ((r == 0 && g == 0 && b == 0) ||
        (police && alt_r == 0 && alt_g == 0 && alt_b == 0))
        return fail("Notification colors must be visible; 'off' and #000000 cannot be used");

    require_root(argc, argv);
    return kbn_notify(effect, r, g, b, alt_r, alt_g, alt_b, times);
}

static int cmd_webcam(int argc, char **argv)
{
    int rc;
    if (argc >= 3 && strcmp(argv[2], "on") == 0)
        rc = webcam_set(1);
    else if (argc >= 3 && strcmp(argv[2], "off") == 0)
        rc = webcam_set(0);
    else
        rc = webcam_toggle();
    if (rc == 0) print_done();
    return rc;
}

static int cmd_bat(int argc, char **argv)
{
    if (argc < 3) {
        int prev = g_stream_mode;
        g_stream_mode = STREAM_MODE_LINE;
        g_stream_line = 0;
        /* Show current charge mode and thresholds. */
        char charge_type[32] = {0};
        if (bat_read_charge_type(charge_type, sizeof(charge_type)) >= 0)
            cctl_stream_printf("%sCharge mode:%s %s\n", C_CYN, C_RST, charge_type);
        else
            cctl_stream_printf("%sCharge mode:%s unavailable\n", C_CYN, C_RST);
        int start = bat_read_start();
        int end = bat_read_end();
        if (start < 0 || end < 0) {
            cctl_stream_fprintf(stderr, "Error: Cannot read battery thresholds\n");
            g_stream_mode = prev;
            return 1;
        }
        cctl_stream_printf("%sCurrent Thresholds:%s\n", C_YLW, C_RST);
        if (strcasecmp(charge_type, "Standard") == 0) {
            cctl_stream_printf("  %sCharge thresholds:%s      start %d%% → stop %d%% %s(disabled)%s\n",
                   C_CYN, C_RST, start, end, C_RED, C_RST);
        } else {
            cctl_stream_printf("  %sCharge thresholds:%s      start %d%% → stop %d%%\n", C_CYN, C_RST, start, end);
        }

        /* Show available values */
        int avail[16], cnt;
        char ap[256];
        bat_start_avail_path(ap, sizeof(ap));
        cnt = read_avail_thresholds(ap, avail, 16);
        if (cnt > 0) {
            cctl_stream_printf("  %sAvailable start values:%s ", C_CYN, C_RST);
            for (int i = 0; i < cnt; i++) {
                cctl_stream_printf("%s%d%s%s", avail[i] == start ? C_GRN : "", avail[i], C_RST,
                       i < cnt - 1 ? " " : "\n");
            }
        }
        bat_end_avail_path(ap, sizeof(ap));
        cnt = read_avail_thresholds(ap, avail, 16);
        if (cnt > 0) {
            cctl_stream_printf("  %sAvailable stop values:%s  ", C_CYN, C_RST);
            for (int i = 0; i < cnt; i++) {
                cctl_stream_printf("%s%d%s%s", avail[i] == end ? C_GRN : "", avail[i], C_RST,
                       i < cnt - 1 ? " " : "\n");
            }
        }
        cctl_stream_printf("\n%sUsage:%s %scctl bat <start> <stop>  |  cctl bat standard%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        g_stream_mode = prev;
        return 0;
    }

    /* Standard mode ignores custom charge thresholds. Keep "max" as a
     * deprecated alias so existing scripts still work. */
    if ((strcmp(argv[2], "standard") == 0 || strcmp(argv[2], "max") == 0) && argc == 3) {
        require_root(argc, argv);
        if (strcmp(argv[2], "max") == 0)
            cctl_stream_fprintf(stderr, "%sDeprecated:%s use 'cctl bat standard' instead of 'cctl bat max'.\n",
                    C_YLW, C_RST);
        int rc = bat_set_charge_type("Standard");
        if (rc == 0)
            cctl_stream_printf("  %s battery charge mode: Standard\n", dry_run ? "Would set" : "Set");
        if (rc == 0) print_done();
        return rc;
    }

    /* set <start> <stop>; bat_set selects Custom before writing limits. */
    if (argc < 4)
        return fail("Usage: bat <start> <stop> or bat standard");

    require_root(argc, argv);
    int start, end;
    if (safe_atoi(argv[2], &start) < 0 || safe_atoi(argv[3], &end) < 0)
        return fail("Invalid threshold values");
    int rc = bat_set(start, end);
    if (rc == 0) print_done();
    return rc;
}

static int get_installed_microversion(void)
{
    if (access("/usr/local/bin/cctl", X_OK) != 0)
        return -1; /* Not installed */

    char buf[32] = {0};
    char *const args[] = { "/usr/local/bin/cctl", "--microversion", NULL };
    if (run(args, RUN_CAPTURE, buf, sizeof(buf)) != 0)
        return 0;

    int ver = 0;
    safe_atoi(buf, &ver);
    return ver;
}

/* Username safe to embed in a sudoers rule: ^[A-Za-z_][A-Za-z0-9_-]*$.
 * The username is written verbatim into /etc/sudoers.d/cctl — a stray
 * space, newline or metacharacter would produce an unparseable rule and
 * lock sudo out system-wide, so refuse anything outside this set.
 * Uppercase is allowed (legal on Linux); the security property comes from
 * excluding whitespace and every metacharacter, not from case. */
static int sudoers_user_is_safe(const char *s)
{
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (const char *p = s + 1; *p; p++) {
        if (!(isalpha((unsigned char)*p) || isdigit((unsigned char)*p) ||
              *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

static int cctl_uninstall_yes(const char *prompt)
{
    char answer[64];
    cctl_stream_printf("%s", prompt);
    fflush(stdout);
    if (!fgets(answer, sizeof(answer), stdin)) {
        cctl_stream_puts("Cancelled.");
        return 0;
    }
    answer[strcspn(answer, "\r\n")] = '\0';
    if (strcmp(answer, "yes") != 0) {
        cctl_stream_puts("Cancelled; type the full word 'yes' to confirm.");
        return 0;
    }
    return 1;
}

/* Uninstall only files owned by cctl. Refuse symlinks, non-regular files,
 * non-root-owned files, and directories that could redirect cleanup. */
static int cctl_uninstall_check_file(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode) || st.st_uid != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int cctl_uninstall_check_dir(const char *path, struct stat *out)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022) != 0) {
        errno = EPERM;
        return -1;
    }
    if (out) *out = st;
    return 1;
}

static int cctl_uninstall_validate_tree(const char *path, dev_t device)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (st.st_dev != device || st.st_uid != 0) { errno = EPERM; return -1; }
    if (S_ISLNK(st.st_mode) || S_ISREG(st.st_mode)) return 0;
    if (!S_ISDIR(st.st_mode) || (st.st_mode & 0022) != 0) {
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
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child) ||
            cctl_uninstall_validate_tree(child, device) != 0) {
            failed = 1;
            break;
        }
    }
    closedir(dir);
    if (failed && errno == 0) errno = EPERM;
    return failed ? -1 : 0;
}

static int cctl_uninstall_remove_tree(const char *path, dev_t device)
{
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
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) { errno = ENAMETOOLONG; failed = 1; break; }
        struct stat st;
        if (lstat(child, &st) != 0) { failed = 1; break; }
        if (st.st_dev != device) { errno = EPERM; failed = 1; break; }
        if (S_ISDIR(st.st_mode)) {
            if (cctl_uninstall_remove_tree(child, device) != 0) {
                failed = 1;
                break;
            }
        } else if (unlink(child) != 0) {
            failed = 1;
            break;
        }
    }
    closedir(dir);
    if (failed) return -1;
    return rmdir(path);
}

static int cctl_uninstall_remove_file(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode) || st.st_uid != 0) { errno = EPERM; return -1; }
    return unlink(path);
}

static int cmd_uninstall(int argc, char **argv)
{
    (void)argv;
    if (argc != 2)
        return fail("'cctl uninstall' does not accept arguments");
    if (!is_installed_systemwide())
        return fail("'cctl uninstall' is available only from the installed /usr/local/bin/cctl binary");

    int module_stack_loaded = drivers_loaded();

    static const char *const files[] = {
        USER_PROFILE_FILE,
        CCTL_SETTINGS_FILE,
        LEGACY_USER_PROFILE_FILE,
        "/etc/sudoers.d/cctl",
        "/etc/sudoers.d/cctl.bak",
        "/etc/sudoers.d/.cctl.tmp",
        "/usr/local/bin/cctl"
    };
    static const char *const parent_dirs[] = {
        "/etc", "/etc/sudoers.d", "/usr", "/usr/local",
        "/usr/local/bin", "/var", "/var/lib"
    };
    for (size_t i = 0; i < sizeof(parent_dirs) / sizeof(parent_dirs[0]); i++) {
        if (cctl_uninstall_check_dir(parent_dirs[i], NULL) != 1)
            return fail("Refusing uninstall because %s is missing or unsafe", parent_dirs[i]);
    }
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (cctl_uninstall_check_file(files[i]) != 0)
            return fail("Refusing to remove unexpected file at %s", files[i]);
    }

    int have_etc_cctl = cctl_uninstall_check_dir("/etc/cctl", NULL);
    if (have_etc_cctl < 0)
        return fail("Refusing to remove unexpected directory at /etc/cctl");

    struct stat data_dir;
    int have_data_dir = cctl_uninstall_check_dir("/var/lib/cctl", &data_dir);
    if (have_data_dir < 0)
        return fail("Refusing to remove unexpected directory at /var/lib/cctl");
    struct stat varlib_dir;
    if (have_data_dir &&
        (cctl_uninstall_check_dir("/var/lib", &varlib_dir) != 1 ||
         data_dir.st_dev != varlib_dir.st_dev))
        return fail("Refusing to remove /var/lib/cctl because it is on a different filesystem");
    if (have_data_dir && cctl_uninstall_validate_tree("/var/lib/cctl", data_dir.st_dev) != 0)
        return fail("Refusing to remove unsafe contents under /var/lib/cctl");

    if (module_stack_loaded) {
        cctl_stream_fprintf(stderr,
                "TUXEDO/Clevo drivers are loaded. Use 'cctl drivers-manage' to uninstall them first.\n");
        if (!cctl_uninstall_yes("Proceed anyway and uninstall cctl only, leaving the drivers installed? Type 'yes' to continue: "))
            return 0;
    } else if (!cctl_uninstall_yes(
                   "This removes cctl, its profiles, sudo rule, and saved data. Type 'yes' to continue: ")) {
        return 0;
    }

    for (size_t i = 0; i < 3; i++) {
        if (cctl_uninstall_remove_file(files[i]) != 0) {
            cctl_stream_perror("Error removing cctl profiles or settings");
            return 1;
        }
    }
    if (have_etc_cctl) {
        if (rmdir("/etc/cctl") != 0) {
            if (errno == ENOTEMPTY || errno == EEXIST)
                cctl_stream_fprintf(stderr, "Keeping /etc/cctl because it contains other files.\n");
            else {
                cctl_stream_perror("Error removing /etc/cctl");
                return 1;
            }
        }
    }

    for (size_t i = 3; i < 6; i++) {
        if (cctl_uninstall_remove_file(files[i]) != 0) {
            cctl_stream_perror("Error removing cctl sudoers files");
            return 1;
        }
    }
    if (cctl_uninstall_remove_file(files[6]) != 0) {
        cctl_stream_perror("Error removing /usr/local/bin/cctl");
        return 1;
    }
    if (have_data_dir && cctl_uninstall_remove_tree("/var/lib/cctl", data_dir.st_dev) != 0) {
        cctl_stream_perror("Error removing /var/lib/cctl data");
        return 1;
    }

    stream_text("cctl was uninstalled. TUXEDO/Clevo drivers, if installed, were left untouched.", STREAM_DELAY_US);
    return 0;
}

static int cmd_install(int argc, char **argv)
{
    int force = (argc >= 3 && strcmp(argv[2], "--force") == 0);
    if (argc > 3 || (argc == 3 && !force))
        return fail("'cctl install' does not accept arguments");

    /* needs_root=0 in the command table: main() does NOT pre-elevate.
     * The readlink/microversion checks below run unprivileged;
     * require_root() re-execs us as root only after they pass — do not
     * assume EUID==0 at this point. */
    char src[PATH_MAX];
    if (get_self_exe(src, sizeof(src)) < 0) {
        cctl_stream_perror("Error: cannot determine running binary path");
        return 1;
    }

    /* If running from /usr/local/bin/cctl directly, it's already installed */
    if (strcmp(src, "/usr/local/bin/cctl") == 0) {
        stream_printf(STREAM_DELAY_US, "cctl is already installed at /usr/local/bin/cctl\n");
        return 0;
    }

    /* Check microversion of currently installed binary */
    int installed_ver = get_installed_microversion();
    if (!force && installed_ver > CCTL_MICROVERSION) {
        stream_printf(STREAM_DELAY_US, "Installed cctl is newer (build %d > build %d). Use --force to downgrade.\n",
               installed_ver, CCTL_MICROVERSION);
        return 0;
    }

    require_root(argc, argv);

    /* Confirmation prompt: simple enter is not allowed, must type y or n */
    if (installed_ver == CCTL_MICROVERSION)
        stream_printf(STREAM_DELAY_US, "Reinstall cctl at /usr/local/bin/cctl (build %d)? [y/n] ", CCTL_MICROVERSION);
    else if (installed_ver > 0)
        stream_printf(STREAM_DELAY_US, "Upgrade cctl at /usr/local/bin/cctl (build %d → build %d)? [y/n] ",
               installed_ver, CCTL_MICROVERSION);
    else
        stream_printf(STREAM_DELAY_US, "Install cctl to /usr/local/bin/cctl with passwordless sudo? [y/n] ");
    fflush(stdout);

    char ans[32] = {0};
    if (!fgets(ans, sizeof(ans), stdin) ||
        (ans[0] != 'y' && ans[0] != 'Y') ||
        (ans[1] != '\n' && strcasecmp(ans, "yes\n") != 0)) {
        stream_printf(STREAM_DELAY_US, "Aborted.\n");
        return 0;
    }

    /* Keep the interactive confirmation prompt character-streamed, then
     * present install progress and completion one line at a time. */
    g_stream_mode = STREAM_MODE_LINE;
    g_stream_line = 0;

    if (installed_ver > 0)
        stream_printf(STREAM_DELAY_US, "%sUpgrading cctl...%s\n", C_CYN, C_RST);
    else
        stream_printf(STREAM_DELAY_US, "%sInstalling cctl...%s\n", C_CYN, C_RST);

    /* Cache the matching driver archive when possible, but keep cctl
     * installation independent of network availability. */
    int stale_driver_cache = 0;
    int drivers_cached = drivers_cache_warm(src, &stale_driver_cache) == 0;

    /* 1. Copy binary to /usr/local/bin/cctl with mode 0755 */
    char *const args[] = { "install", "-m", "755", src, "/usr/local/bin/cctl", NULL };
    if (run(args, 0, NULL, 0) != 0) {
        cctl_stream_fprintf(stderr, "Error: failed to install binary to /usr/local/bin/cctl\n");
        return 1;
    }
    stream_printf(STREAM_DELAY_US, "Installed: /usr/local/bin/cctl (from %s)\n", src);

    /* 2. Determine the human user (prefer the sudo invoker) */
    const char *user = getenv("SUDO_USER");
    if (!user || !*user) user = getenv("USER");
    if (!user || !*user) {
        struct passwd *pw = getpwuid(getuid());
        user = (pw && pw->pw_name) ? pw->pw_name : "root";
    }

    /* 3. Write sudoers.d/cctl granting passwordless sudo.
     *    Order matters:
     *      a) validate the username charset before it goes near the file,
     *      b) back up the existing rule (cctl.bak — sudo's includedir skips
     *         names containing '.', so the backup can never be auto-loaded),
     *      c) write the new rule to a 0440 temp file,
     *      d) validate it with `visudo -c -f`,
     *      e) rename it into place ONLY if it parses cleanly.
     *    A rejected rule never replaces a working one, and sudo can never
     *    be locked out by a bad install. */
    if (!sudoers_user_is_safe(user)) {
        cctl_stream_fprintf(stderr, "Error: username '%s' is not safe to write into sudoers.\n", user);
        cctl_stream_fprintf(stderr, "       Allowed: first character [A-Za-z_], then [A-Za-z0-9_-].\n");
        cctl_stream_fprintf(stderr, "       Create the rule manually with mode 0440:\n");
        cctl_stream_fprintf(stderr, "         echo '<user> ALL=(ALL) NOPASSWD: /usr/local/bin/cctl' "
                        "| sudo EDITOR=tee visudo -f /etc/sudoers.d/cctl\n");
        return 1;
    }

    const char *sudoers     = "/etc/sudoers.d/cctl";
    const char *sudoers_tmp = "/etc/sudoers.d/.cctl.tmp";
    const char *sudoers_bak = "/etc/sudoers.d/cctl.bak";

    if (access(sudoers, R_OK) == 0) {
        char *const bak_args[] = { "/bin/cp", "-p",
                                   (char *)sudoers, (char *)sudoers_bak, NULL };
        if (run(bak_args, 0, NULL, 0) != 0)
            cctl_stream_fprintf(stderr, "Warning: could not back up existing sudoers rule to %s\n",
                    sudoers_bak);
    }

    unlink(sudoers_tmp); /* stale temp from an earlier failed run */
    int sfd = open(sudoers_tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0440);
    if (sfd < 0) {
        cctl_stream_perror("Error: cannot create sudoers temp file");
        return 1;
    }
    char rule[320];
    int rule_len = snprintf(rule, sizeof(rule),
                            "%s ALL=(ALL) NOPASSWD: /usr/local/bin/cctl\n", user);
    ssize_t w = write(sfd, rule, (size_t)rule_len);
    int close_rc = close(sfd);
    if (w != rule_len || close_rc != 0) {
        cctl_stream_perror("Error: cannot write sudoers temp file");
        unlink(sudoers_tmp);
        return 1;
    }

    const char *visudo_path = NULL;
    if (access("/usr/sbin/visudo", X_OK) == 0)     visudo_path = "/usr/sbin/visudo";
    else if (access("/usr/bin/visudo", X_OK) == 0) visudo_path = "/usr/bin/visudo";
    else if (access("/sbin/visudo", X_OK) == 0)    visudo_path = "/sbin/visudo";

    if (visudo_path) {
        char *const v_args[] = { "visudo", "-c", "-f",
                                 (char *)sudoers_tmp, NULL };
        if (run(v_args, 0, NULL, 0) != 0) {
            cctl_stream_fprintf(stderr,
                    "Error: visudo rejected the generated sudoers rule — "
                    "keeping the existing one.\n");
            unlink(sudoers_tmp);
            return 1;
        }
    } else {
        cctl_stream_fprintf(stderr,
                "Warning: visudo not found — installing rule without syntax validation.\n");
    }

    if (rename(sudoers_tmp, sudoers) != 0) {
        cctl_stream_perror("Error: cannot move sudoers rule into place");
        unlink(sudoers_tmp);
        return 1;
    }
    chmod(sudoers, 0440); /* rename preserves 0440; enforce explicitly anyway */
    stream_printf(STREAM_DELAY_US, "%sSudoers:%s %s (passwordless sudo for %s)\n", C_GRN, C_RST,
           sudoers, user);

    stream_printf(STREAM_DELAY_US, "\n%sDone.%s cctl is installed to /usr/local/bin/cctl and passwordless sudo is configured.\n",
           C_GRN, C_RST);
    stream_printf(STREAM_DELAY_US, "Privileged commands will auto-elevate seamlessly without needing any shell alias.\n");
    if (!drivers_cached && stale_driver_cache)
        stream_printf(STREAM_DELAY_US, "%sNote:%s An older driver package remains in the cache. Run 'cctl drivers-manage' to refresh it; if its checksum does not match, update cctl first.\n",
               C_YLW, C_RST);
    else if (!drivers_cached)
        stream_printf(STREAM_DELAY_US, "%sNote:%s Driver package was not cached. Run 'cctl drivers-manage' when online; if its checksum does not match, update cctl first.\n",
               C_YLW, C_RST);
    return 0;
}
