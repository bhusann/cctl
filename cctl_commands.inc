/* ========================================================================
 * DISPLAY SCALING (xrandr --scale-from)
 * ======================================================================== */

static int scale_set(const char *resolution)
{
    struct display_info info;
    if (query_display_info(&info) < 0)
        return fail("failed to query display info");

    char *const args[] = {
        "xrandr", "--output", info.output, "--mode", info.resolution,
        "--scale-from", (char *)resolution, NULL
    };
    if (run(args, RUN_SILENT, NULL, 0) != 0)
        return fail("Failed to set scale to %s", resolution);
    if (mode_record_display_scale(resolution) < 0)
        fprintf(stderr, "Warning: scale was applied but could not be recorded in %s\n", MODE_FILE);
    printf("  Display scale: %s → %s\n", resolution, info.resolution);
    return 0;
}

static int scale_reset(void)
{
    struct display_info info;
    if (query_display_info(&info) < 0)
        return fail("failed to query display info");

    char *const args[] = {
        "xrandr", "--output", info.output, "--mode", info.resolution,
        "--scale", "1x1", NULL
    };
    if (run(args, RUN_SILENT, NULL, 0) != 0)
        return fail("Failed to reset scale");
    if (mode_record_display_scale(NULL) < 0)
        fprintf(stderr, "Warning: scale was reset but could not be recorded in %s\n", MODE_FILE);
    printf("  Scale: native (1x1)\n");
    return 0;
}

static void scale_show_details(void)
{
    printf("%sUsage:%s %scctl scale <factor | resolution | off>%s\n\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
    printf("The internal display does not offer these lower resolutions as native modes.\n"
           "cctl uses X11 display scaling to show the desktop at a requested size\n"
           "while keeping the panel at its native resolution.\n\n");

    struct display_info info;
    if (has_display_support() && query_display_info(&info) == 0 && info.resolution[0]) {
        printf("%sCurrent Display:%s\n", C_YLW, C_RST);
        printf("  %sOutput:%s %s  |  %sNative Resolution:%s %s  |  %sCurrent Rate:%s %sHz\n\n",
               C_CYN, C_RST, info.output, C_CYN, C_RST, info.resolution, C_CYN, C_RST, info.current_rate);
    }

    printf("%sOptions:%s\n", C_YLW, C_RST);
    printf("  %s%-15s%s Fraction between 0.01 and 1.0 (e.g. 0.75 for 75%%, 0.5 for 50%%)\n", C_CYN, "<factor>", C_RST);
    printf("  %s%-15s%s Explicit WIDTHxHEIGHT (e.g. 1920x1080, 1600x900, 1280x720)\n", C_CYN, "<resolution>", C_RST);
    printf("  %s%-15s%s Restore native 1:1 display resolution\n\n", C_CYN, "off | reset", C_RST);

    printf("%sExamples:%s\n", C_YLW, C_RST);
    printf("  %scctl scale 0.75%s         Set display scale to 75%% (1080p area on a 1440p panel)\n", C_CYN, C_RST);
    printf("  %scctl scale 1920x1080%s    Set display scale using a 1920x1080 area\n", C_CYN, C_RST);
    printf("  %scctl scale 0.5%s          Set display scale to 50%%\n", C_CYN, C_RST);
    printf("  %scctl scale off%s          Reset back to native display resolution\n", C_CYN, C_RST);
}

static int cmd_scale(int argc, char **argv)
{
    if (argc < 3) {
        scale_show_details();
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
        printf("  Factor: %.2f → %s (from %s)\n", factor, res, info.resolution);
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

static int cmd_monitor(int argc, char **argv)
{
    if (argc > 3 || (argc == 3 && strcmp(argv[2], "--json") != 0))
        return fail("Unexpected extra argument '%s'", argv[2]);
    if (argc == 3) return cpumonitor_json();
    return cpumonitor();
}

static int cmd_capabilities(int argc, char **argv)
{
    int as_json = argc == 3 && strcmp(argv[2], "--json") == 0;
    if (argc > 3 || (argc == 3 && !as_json))
        return fail("Usage: cctl capabilities [--json]");

    char battery[32] = "";
    int have_battery = power_supply_find_type("Battery", battery, sizeof(battery)) == 0;
    /* Use an explicit path buffer to keep the capability check easy to audit. */
    char battery_path[512];
    snprintf(battery_path, sizeof(battery_path),
             "/sys/class/power_supply/%s/charge_control_end_threshold", battery);
    int battery_control = have_battery && access(battery_path, F_OK) == 0;

    struct capability { const char *name; int available; } caps[] = {
        { "fan_control", access("/dev/tuxedo_io", F_OK) == 0 },
        { "keyboard_backlight", access(KBD_PATH "/brightness", F_OK) == 0 },
        { "battery_thresholds", battery_control },
        { "gpu_mux", access(MUX_VAR_PATH, F_OK) == 0 },
        { "x11_display", has_display_support() },
        { "nvidia_gpu", nvidia_find_pci_address(battery_path, sizeof(battery_path)) == 0 },
    };
    if (as_json) {
        int old_json = json_output;
        json_output = 1;
        json_first_field = 1;
        putchar('{');
        for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
            json_field(caps[i].name, caps[i].available ? "available" : "unavailable");
        puts("}");
        json_output = old_json;
    } else {
        for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
            printf("%-22s %s\n", caps[i].name, caps[i].available ? "available" : "unavailable");
    }
    return 0;
}

static int create_profile_conf(void)
{
    char tmp_path[] = "/tmp/cctl-profiles.XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;

    static const char header[] =
        "# Custom profiles: one profile per line; use 'skip' to leave a setting unchanged.\n"
        "# name ec_profile_code turbo governor epp pl1_watts pl2_watts\n";
    size_t offset = 0;
    while (offset < sizeof(header) - 1) {
        ssize_t n = write(fd, header + offset, sizeof(header) - 1 - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            unlink(tmp_path);
            return -1;
        }
        offset += (size_t)n;
    }
    int chmod_rc = fchmod(fd, 0600);
    int close_rc = close(fd);
    if (chmod_rc < 0 || close_rc < 0) {
        unlink(tmp_path);
        return -1;
    }

    char *const install_root[] = {
        "install", "-D", "-m", "0644", tmp_path, USER_PROFILE_FILE, NULL
    };
    char *const install_user[] = {
        "sudo", "install", "-D", "-m", "0644", tmp_path, USER_PROFILE_FILE, NULL
    };
    int rc = run(geteuid() == 0 ? install_root : install_user, RUN_SILENT, NULL, 0);
    unlink(tmp_path);
    return rc;
}

static int cmd_editconf(int argc, char **argv)
{
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
    if (setenv("SUDO_EDITOR", editor, 1) != 0)
        return fail("Could not set the configured editor for sudoedit");

    if (access(USER_PROFILE_FILE, F_OK) != 0 && errno == ENOENT) {
        if (create_profile_conf() != 0)
            return fail("Could not create %s", USER_PROFILE_FILE);
    }

    char *const edit_args[] = { "sudoedit", USER_PROFILE_FILE, NULL };
    int rc = run(edit_args, 0, NULL, 0);
    if (rc != 0)
        return fail("Editor exited with status %d", rc);
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    int nosafe = 0;
    const char *pos[1] = {NULL};
    int npos = parse_flags(argc, argv, &nosafe, pos, 1);
    if (npos < 0) return 1;

    if (npos == 0) {
        printf("%sUsage:%s %scctl %s <profile> [--nosafe]%s\n\n",
               C_BLD, C_RST, C_CYN_BLD, "set", C_RST);
        printf("%sValid Profiles:%s\n", C_YLW, C_RST);
        load_user_profiles();
        for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
            printf("  %s%-12s%s %s\n",
                   prof_color_str(PROFILES[i].color),
                   PROFILES[i].name,
                   C_RST,
                   PROFILES[i].help_desc);
        }
        for (int i = 0; i < user_profile_count; i++)
            printf("  %s%-12s%s %s\n", C_BLU, user_profiles[i].name, C_RST,
                   user_profiles[i].help_desc);
        return 0;
    }

    const char *profile = pos[0];
    const struct profile_def *p = find_profile(profile);
    if (!p) {
        fprintf(stderr, "Error: Unknown profile '%s'\n", profile);
        fprintf(stderr, "Valid profiles: ");
        for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
            fprintf(stderr, "%s%s", PROFILES[i].name,
                    i + 1 < sizeof(PROFILES) / sizeof(PROFILES[0]) ? ", " : "\n");
        }
        for (int i = 0; i < user_profile_count; i++)
            fprintf(stderr, "%s%s", user_profiles[i].name,
                    i + 1 < user_profile_count ? ", " : "\n");
        return 1;
    }

    if (dry_run) {
        printf("Dry run: profile %s\n", p->name);
        if (p->gpu_profile >= 0)
            printf("  EC profile code: %s (%d)\n", gpu_profile_name(p->gpu_profile), p->gpu_profile);
        else printf("  EC profile code: --\n");
        printf("  Turbo: %s; governor: %s; EPP: %s\n",
               p->turbo < 0 ? "--" : (p->turbo ? "on" : "off"),
               strcmp(p->gov, "skip") == 0 ? "--" : p->gov,
               strcmp(p->epp, "skip") == 0 ? "--" : p->epp);
        if (p->rapl_pl1 > 0 || p->rapl_pl2 > 0) {
            char pl1[16], pl2[16];
            if (p->rapl_pl1 > 0) snprintf(pl1, sizeof(pl1), "%dW", p->rapl_pl1);
            else snprintf(pl1, sizeof(pl1), "unchanged");
            if (p->rapl_pl2 > 0) snprintf(pl2, sizeof(pl2), "%dW", p->rapl_pl2);
            else snprintf(pl2, sizeof(pl2), "unchanged");
            printf("  RAPL limits: PL1 %s; PL2 %s (preview only)\n", pl1, pl2);
        } else {
            printf("  RAPL limits: unchanged (use cctl rapl to change them)\n");
        }
        if (p->fan_safety && !nosafe)
            printf("  Fans: restore automatic control (safety default)\n");
        return 0;
    }

    require_root(argc, argv);

    if (p->fan_safety && !nosafe) {
        printf("Setting both fans to AUTO (safety default; use --nosafe to bypass)...\n");
        fan_auto_all();
    }
    int rc = profile_apply(p);

    ec_release_ports();
    if (rc == 0) {
        /* Record the active profile only once
         * the profile actually landed, so `status` and the RAPL ceiling
         * never report a mode that failed to apply. */
        int active_ec_code = p->gpu_profile;
        if (active_ec_code < 0) {
            char old_profile[64], old_method[32];
            int old_ec_code = -1;
            if (mode_read(old_profile, sizeof(old_profile), old_method, sizeof(old_method),
                          &old_ec_code) == 0)
                active_ec_code = old_ec_code;
        }
        if (mode_write(profile, "set", active_ec_code) < 0)
            fprintf(stderr, "Warning: could not record active mode in %s\n", MODE_FILE);
        kbe_profile_pulse(profile);
        print_done();
    }
    return rc;
}

static int cmd_fan(int argc, char **argv)
{
    int nosafe = 0;
    const char *pos[2] = {NULL, NULL};
    int npos = parse_flags(argc, argv, &nosafe, pos, 2);
    if (npos < 0) return 1;

    if (npos == 0) {
        printf("%sUsage:%s %scctl fan <mode> [pct] [--nosafe]%s\n\n",
               C_BLD, C_RST, C_CYN_BLD, C_RST);
        printf("%sValid Modes:%s\n", C_YLW, C_RST);
        printf("  %s%-18s%s Automatic EC fan control (both fans, or cpu/gpu)\n", C_CYN, "auto [cpu|gpu]", C_RST);
        printf("  %s%-18s%s Full 100%% fan speed\n", C_CYN, "max", C_RST);
        printf("  %s%-18s%s Quiet mode (forces eco profile; bypass with --nosafe)\n", C_CYN, "silent", C_RST);
        printf("  %s%-18s%s Set both fans to duty 25-100%% (requires --nosafe)\n", C_CYN, "<pct>", C_RST);
        printf("  %s%-18s%s Set CPU fan duty (25-100%% --nosafe) or 'auto'\n", C_CYN, "cpu <pct|auto>", C_RST);
        printf("  %s%-18s%s Set GPU fan duty (25-100%% --nosafe) or 'auto'\n", C_CYN, "gpu <pct|auto>", C_RST);
        return 0;
    }

    const char *mode = pos[0];
    const char *val_str = (npos >= 2) ? pos[1] : NULL;

    require_root(argc, argv);

    int rc = 0;

    if (strcmp(mode, "auto") == 0) {
        if (val_str && strcmp(val_str, "cpu") == 0) {
            printf("Setting CPU fan to AUTO...\n");
            rc = legacy_fan_auto(FAN_CPU);
        } else if (val_str && strcmp(val_str, "gpu") == 0) {
            printf("Setting GPU fan to AUTO...\n");
            rc = legacy_fan_auto(FAN_GPU);
        } else {
            printf("Setting both fans to AUTO...\n");
            rc = fan_auto_all();
        }
    } else if (strcmp(mode, "max") == 0) {
        printf("Setting both fans to MAX...\n");
        rc = fan_max_all();
    } else if (strcmp(mode, "silent") == 0) {
        if (!nosafe) {
            printf("Applying: eco profile (safety default for silent fans; use --nosafe to bypass)...\n");
            profile_eco();
        }
        printf("Setting both fans to SILENT...\n");
        rc = fan_silent_all();
    } else if (strcmp(mode, "cpu") == 0) {
        if (!val_str)
            return fail("Missing duty percentage or 'auto'");
        if (strcmp(val_str, "auto") == 0) {
            printf("Setting CPU fan to AUTO...\n");
            rc = legacy_fan_auto(FAN_CPU);
        } else {
            int pct;
            if (safe_atoi(val_str, &pct) < 0)
                return fail("Invalid duty percentage '%s' (use 25-100 or 'auto')", val_str);
            if (!nosafe)
                return fail("Manual fan duty requires --nosafe (e.g. cctl fan cpu %d --nosafe)\n"
                            "       Fixed duty ignores temperature and may cause overheating or damage.\n"
                            "       Use manual fan duty for experimental purposes only.", pct);
            rc = fan_set_duty(FAN_CPU, pct);
        }
    } else if (strcmp(mode, "gpu") == 0) {
        if (!val_str)
            return fail("Missing duty percentage or 'auto'");
        if (strcmp(val_str, "auto") == 0) {
            printf("Setting GPU fan to AUTO...\n");
            rc = legacy_fan_auto(FAN_GPU);
        } else {
            int pct;
            if (safe_atoi(val_str, &pct) < 0)
                return fail("Invalid duty percentage '%s' (use 25-100 or 'auto')", val_str);
            if (!nosafe)
                return fail("Manual fan duty requires --nosafe (e.g. cctl fan gpu %d --nosafe)\n"
                            "       Fixed duty ignores temperature and may cause overheating or damage.\n"
                            "       Use manual fan duty for experimental purposes only.", pct);
            rc = fan_set_duty(FAN_GPU, pct);
        }
    } else {
        /* Try as a plain number — apply to both fans */
        int pct;
        if (safe_atoi(mode, &pct) >= 0 && pct >= 25 && pct <= 100) {
            if (!nosafe)
                return fail("Manual fan duty requires --nosafe (e.g. cctl fan %d --nosafe)\n"
                            "       Fixed duty ignores temperature and may cause overheating or damage.\n"
                            "       Use manual fan duty for experimental purposes only.", pct);
            printf("Setting both fans to %d%%...\n", pct);
            rc = fan_set_duty(0, pct);
        } else {
            fail("Unknown fan mode '%s'", mode);
            fprintf(stderr, "Valid modes: auto [cpu|gpu], max, silent, cpu <pct|auto>, gpu <pct|auto>, or <pct> --nosafe\n");
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
            printf("%sCurrent Turbo:%s %s%s%s\n\n",
                   C_YLW, C_RST,
                   no_turbo == 0 ? C_GRN : C_RED,
                   no_turbo == 0 ? "ON" : "OFF",
                   C_RST);
        printf("%sUsage:%s %scctl turbo <on|off> [--nosafe]%s\n",
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

    if (enabled && !nosafe) {
        printf("Setting both fans to AUTO (safety default for turbo; use --nosafe to bypass)...\n");
        fan_auto_all();
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
        struct cpu_profile_state cpu_profile;
        read_cpu_profile(&cpu_profile);
        if (cpu_profile.have_governor)
            printf("%sCurrent Governor:%s %s%s%s\n\n", C_YLW, C_RST, C_CYN,
                   cpu_profile.governor, C_RST);
        printf("%sUsage:%s %scctl gov <powersave | performance>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        return 0;
    }
    const char *gov = argv[2];
    if (strcmp(gov, "powersave") != 0 && strcmp(gov, "performance") != 0)
        return fail("Invalid governor '%s' (use powersave or performance)", gov);

    require_root(argc, argv);
    int rc = set_governor(gov);
    if (rc == 0) print_done();
    return rc;
}

static int cmd_epp(int argc, char **argv)
{
    if (argc < 3) {
        struct cpu_profile_state cpu_profile;
        read_cpu_profile(&cpu_profile);
        if (cpu_profile.have_epp)
            printf("%sCurrent EPP:%s %s%s%s\n\n", C_YLW, C_RST, C_CYN,
                   cpu_profile.epp, C_RST);
        printf("%sUsage:%s %scctl epp <value>%s\n\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        printf("%sValid Values:%s\n", C_YLW, C_RST);
        printf("  %sperformance%s\n", C_CYN, C_RST);
        printf("  %sbalance_performance%s\n", C_CYN, C_RST);
        printf("  %sbalance_power%s\n", C_CYN, C_RST);
        printf("  %spower%s\n", C_CYN, C_RST);
        return 0;
    }
    const char *epp = argv[2];
    if (strcmp(epp, "performance") != 0 && strcmp(epp, "balance_performance") != 0 &&
        strcmp(epp, "balance_power") != 0 && strcmp(epp, "power") != 0) {
        fprintf(stderr, "Error: Invalid EPP '%s'\n", epp);
        fprintf(stderr, "Valid values: performance, balance_performance, balance_power, power\n");
        return 1;
    }
    require_root(argc, argv);
    int rc = set_epp(epp);
    if (rc == 0) print_done();
    return rc;
}

static int cmd_rapl(int argc, char **argv)
{
    if (argc < 3) {
        int cur_pl1 = -1, cur_pl2 = -1;
        if (read_rapl_current(&cur_pl1, &cur_pl2) == 0 && cur_pl1 > 0 && cur_pl2 > 0) {
            printf("%sCurrent RAPL Limits:%s\n", C_YLW, C_RST);
            printf("  %sPL1:%s %s%dW%s\n", C_CYN, C_RST, C_CYN, cur_pl1, C_RST);
            printf("  %sPL2:%s %s%dW%s\n\n", C_CYN, C_RST, C_CYN, cur_pl2, C_RST);
        }
        printf("%sUsage:%s %scctl rapl <pl1_watts> <pl2_watts>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        printf("       %scctl rapl skip <pl2>%s      (set PL2 only)\n", C_CYN_BLD, C_RST);
        printf("       %scctl rapl <pl1> skip%s      (set PL1 only)\n", C_CYN_BLD, C_RST);
        printf("       %scctl rapl <pl2>%s           (one value = PL2 only)\n\n", C_CYN_BLD, C_RST);
        printf("  %sLimits:%s PL1 1-%dW (up to %dW with EC profile code 2), PL2 1-%dW\n",
                C_DIM, C_RST, RAPL_PL1_MAX_WATTS, RAPL_PL1_MAX_WATTS_MAX, RAPL_PL2_MAX_WATTS);
        return 0;
    }

    if (argc > 4)
        return fail("Unexpected extra argument '%s'", argv[4]);

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
            return fail("Invalid PL2 value '%s' (use a wattage 1-%d)", argv[2], RAPL_PL2_MAX_WATTS);
        skip_pl1 = 1;
    } else {
        /* Parse PL1 */
        if (strcmp(argv[2], "skip") == 0) {
            skip_pl1 = 1;
        } else {
            if (safe_atoi(argv[2], &pl1) < 0 || pl1 < 1)
                return fail("Invalid PL1 value '%s' (use a wattage 1-%d or 'skip')", argv[2], pl1_cap);
            if (pl1 > pl1_cap) {
                fprintf(stderr, "Error: PL1 %dW exceeds the %dW limit for the current mode\n",
                        pl1, pl1_cap);
                if (pl1_cap == RAPL_PL1_MAX_WATTS)
                    fprintf(stderr, "       90W PL1 is supported only in 'max' mode (EC profile code 2) "
                                    "(see Mode in 'cctl status')\n");
                return 1;
            }
        }

        /* Parse PL2 */
        if (strcmp(argv[3], "skip") == 0) {
            skip_pl2 = 1;
        } else {
            if (safe_atoi(argv[3], &pl2) < 0 || pl2 < 1)
                return fail("Invalid PL2 value '%s' (use a wattage 1-%d or 'skip')", argv[3], RAPL_PL2_MAX_WATTS);
            if (pl2 > RAPL_PL2_MAX_WATTS)
                return fail("PL2 %dW exceeds the %dW platform limit", pl2, RAPL_PL2_MAX_WATTS);
        }
    }

    /* Must set at least one limit */
    if (skip_pl1 && skip_pl2)
        return fail("Nothing to set — both PL1 and PL2 are skipped");

    require_root(argc, argv);

    /* Pass ≤ 0 to skip (set_rapl_limits treats pl1_w ≤ 0 as skip) */
    int rc = set_rapl_limits(skip_pl1 ? -1 : pl1, skip_pl2 ? -1 : pl2,
                             rapl_pl1_ceiling());
    if (rc == 0) print_done();
    return rc;
}

static int cmd_kbc(int argc, char **argv)
{
    if (argc < 3) {
        kbd_show_presets();
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
        int cur_bri = kbd_get_brightness();
        if (cur_bri >= 0)
            printf("%sCurrent Brightness:%s %s%d%%%s\n\n", C_YLW, C_RST, C_CYN, cur_bri, C_RST);
        printf("%sUsage:%s %scctl kbb <0-100>%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
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
        struct kbe_state ks;
        if (kbe_is_running(&ks)) {
            printf("%sKeyboard Backlight Effect:%s\n", C_YLW, C_RST);
            printf("  %sStatus:%s              %s%s%s (active, PID %d)\n", C_CYN, C_RST, C_GRN, ks.effect, C_RST, (int)ks.pid);
            const struct kbe_effect_def *cur = kbe_find_effect(ks.effect);
            if (cur && strcmp(cur->name, "temp") == 0) {
                int t = read_cpu_temp();
                if (t > 0)
                    printf("  %sLive CPU Temp:%s       %s%d°C%s\n", C_CYN, C_RST, t >= 85 ? C_RED : (t >= 70 ? C_YLW : C_GRN), t, C_RST);
            }
            printf("  %sOriginal Color:%s      RGB(%d, %d, %d)\n", C_CYN, C_RST, ks.orig_r, ks.orig_g, ks.orig_b);
            int bri_pct = (ks.orig_bri * 100 + 127) / 255;
            printf("  %sOriginal Brightness:%s %d%% (raw %d)\n\n", C_CYN, C_RST, bri_pct, ks.orig_bri);
        } else {
            printf("%sKeyboard Backlight Effect:%s\n", C_YLW, C_RST);
            printf("  %sStatus:%s              %snone%s (stopped)\n\n", C_CYN, C_RST, C_DIM, C_RST);
        }
        printf("%sAvailable Effects:%s\n", C_YLW, C_RST);
        for (size_t i = 0; i < sizeof(kbe_effects) / sizeof(kbe_effects[0]); i++) {
            printf("  %s%-16s%s %s\n", C_CYN, kbe_effects[i].name, C_RST, kbe_effects[i].desc);
        }
        printf("\n%sUsage:%s %scctl kbe <effect>  |  cctl kbe stop%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        return 0;
    }

    require_root(argc, argv);

    const char *sub = argv[2];
    if (strcmp(sub, "stop") == 0 || strcmp(sub, "off") == 0) {
        return kbe_stop(0);
    }

    const struct kbe_effect_def *fx = kbe_find_effect(sub);
    if (!fx) {
        fprintf(stderr, "Error: Unknown keyboard effect '%s'\n", sub);
        fprintf(stderr, "Available effects: ");
        for (size_t i = 0; i < sizeof(kbe_effects) / sizeof(kbe_effects[0]); i++) {
            fprintf(stderr, "%s%s", kbe_effects[i].name,
                    i + 1 < sizeof(kbe_effects) / sizeof(kbe_effects[0]) ? ", " : "\n");
        }
        return 1;
    }

    return kbe_start(fx->name);
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
        /* Show current thresholds */
        int start = bat_read_start();
        int end = bat_read_end();
        if (start < 0 || end < 0) {
            fprintf(stderr, "Error: Cannot read battery thresholds\n");
            return 1;
        }
        printf("%sCurrent Thresholds:%s\n", C_YLW, C_RST);
        printf("  %sCharge thresholds:%s      start %d%% → stop %d%%\n", C_CYN, C_RST, start, end);

        /* Show available values */
        int avail[16], cnt;
        char ap[256];
        bat_start_avail_path(ap, sizeof(ap));
        cnt = read_avail_thresholds(ap, avail, 16);
        if (cnt > 0) {
            printf("  %sAvailable start values:%s ", C_CYN, C_RST);
            for (int i = 0; i < cnt; i++) {
                printf("%s%d%s%s", avail[i] == start ? C_GRN : "", avail[i], C_RST,
                       i < cnt - 1 ? " " : "\n");
            }
        }
        bat_end_avail_path(ap, sizeof(ap));
        cnt = read_avail_thresholds(ap, avail, 16);
        if (cnt > 0) {
            printf("  %sAvailable stop values:%s  ", C_CYN, C_RST);
            for (int i = 0; i < cnt; i++) {
                printf("%s%d%s%s", avail[i] == end ? C_GRN : "", avail[i], C_RST,
                       i < cnt - 1 ? " " : "\n");
            }
        }
        printf("\n%sUsage:%s %scctl bat <start> <end>  |  cctl bat max%s\n", C_BLD, C_RST, C_CYN_BLD, C_RST);
        return 0;
    }

    /* "max" → widest usable range (95%→100% top-up; the driver rejects 0,
     * so thresholds cannot actually be turned off — the old off/default
     * aliases implied an impossible "off" and were removed). */
    if (strcmp(argv[2], "max") == 0) {
        require_root(argc, argv);
        int rc = bat_set(0, 0);
        if (rc == 0) print_done();
        return rc;
    }

    /* set <start> <end> */
    if (argc < 4)
        return fail("Usage: bat <start> <end> or bat max");

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
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(answer, sizeof(answer), stdin)) {
        puts("Cancelled.");
        return 0;
    }
    answer[strcspn(answer, "\r\n")] = '\0';
    if (strcmp(answer, "yes") != 0) {
        puts("Cancelled; type the full word 'yes' to confirm.");
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
        fprintf(stderr,
                "TUXEDO/Clevo drivers are loaded. Use 'cctl drivers-manage' to uninstall them first.\n");
        if (!cctl_uninstall_yes("Proceed anyway and uninstall cctl only, leaving the drivers installed? Type 'yes' to continue: "))
            return 0;
    } else if (!cctl_uninstall_yes(
                   "This removes cctl, its profiles, sudo rule, and saved data. Type 'yes' to continue: ")) {
        return 0;
    }

    if (cctl_uninstall_remove_file(USER_PROFILE_FILE) != 0) {
        perror("Error removing custom profiles");
        return 1;
    }
    if (have_etc_cctl) {
        if (rmdir("/etc/cctl") != 0) {
            if (errno == ENOTEMPTY || errno == EEXIST)
                fprintf(stderr, "Keeping /etc/cctl because it contains other files.\n");
            else {
                perror("Error removing /etc/cctl");
                return 1;
            }
        }
    }

    for (size_t i = 1; i < 4; i++) {
        if (cctl_uninstall_remove_file(files[i]) != 0) {
            perror("Error removing cctl sudoers files");
            return 1;
        }
    }
    if (cctl_uninstall_remove_file("/usr/local/bin/cctl") != 0) {
        perror("Error removing /usr/local/bin/cctl");
        return 1;
    }
    if (have_data_dir && cctl_uninstall_remove_tree("/var/lib/cctl", data_dir.st_dev) != 0) {
        perror("Error removing /var/lib/cctl data");
        return 1;
    }

    puts("cctl was uninstalled. TUXEDO/Clevo drivers, if installed, were left untouched.");
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
        perror("Error: cannot determine running binary path");
        return 1;
    }

    /* If running from /usr/local/bin/cctl directly, it's already installed */
    if (strcmp(src, "/usr/local/bin/cctl") == 0) {
        printf("cctl is already installed at /usr/local/bin/cctl\n");
        return 0;
    }

    /* Check microversion of currently installed binary */
    int installed_ver = get_installed_microversion();
    if (!force && installed_ver > CCTL_MICROVERSION) {
        printf("Installed cctl is newer (build %d > build %d). Use --force to downgrade.\n",
               installed_ver, CCTL_MICROVERSION);
        return 0;
    }

    require_root(argc, argv);

    /* Confirmation prompt: simple enter is not allowed, must type y or n */
    if (installed_ver == CCTL_MICROVERSION)
        printf("Reinstall cctl at /usr/local/bin/cctl (build %d)? [y/n] ", CCTL_MICROVERSION);
    else if (installed_ver > 0)
        printf("Upgrade cctl at /usr/local/bin/cctl (build %d → build %d)? [y/n] ",
               installed_ver, CCTL_MICROVERSION);
    else
        printf("Install cctl to /usr/local/bin/cctl with passwordless sudo? [y/n] ");
    fflush(stdout);

    char ans[32] = {0};
    if (!fgets(ans, sizeof(ans), stdin) ||
        (ans[0] != 'y' && ans[0] != 'Y') ||
        (ans[1] != '\n' && strcasecmp(ans, "yes\n") != 0)) {
        printf("Aborted.\n");
        return 0;
    }

    if (installed_ver > 0)
        printf("Upgrading cctl...\n");
    else
        printf("Installing cctl...\n");

    /* 1. Copy binary to /usr/local/bin/cctl with mode 0755 */
    char *const args[] = { "install", "-m", "755", src, "/usr/local/bin/cctl", NULL };
    if (run(args, 0, NULL, 0) != 0) {
        fprintf(stderr, "Error: failed to install binary to /usr/local/bin/cctl\n");
        return 1;
    }
    printf("Installed: /usr/local/bin/cctl (from %s)\n", src);

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
        fprintf(stderr, "Error: username '%s' is not safe to write into sudoers.\n", user);
        fprintf(stderr, "       Allowed: first character [A-Za-z_], then [A-Za-z0-9_-].\n");
        fprintf(stderr, "       Create the rule manually with mode 0440:\n");
        fprintf(stderr, "         echo '<user> ALL=(ALL) NOPASSWD: /usr/local/bin/cctl' "
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
            fprintf(stderr, "Warning: could not back up existing sudoers rule to %s\n",
                    sudoers_bak);
    }

    unlink(sudoers_tmp); /* stale temp from an earlier failed run */
    int sfd = open(sudoers_tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0440);
    if (sfd < 0) {
        perror("Error: cannot create sudoers temp file");
        return 1;
    }
    char rule[320];
    int rule_len = snprintf(rule, sizeof(rule),
                            "%s ALL=(ALL) NOPASSWD: /usr/local/bin/cctl\n", user);
    ssize_t w = write(sfd, rule, (size_t)rule_len);
    int close_rc = close(sfd);
    if (w != rule_len || close_rc != 0) {
        perror("Error: cannot write sudoers temp file");
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
            fprintf(stderr,
                    "Error: visudo rejected the generated sudoers rule — "
                    "keeping the existing one.\n");
            unlink(sudoers_tmp);
            return 1;
        }
    } else {
        fprintf(stderr,
                "Warning: visudo not found — installing rule without syntax validation.\n");
    }

    if (rename(sudoers_tmp, sudoers) != 0) {
        perror("Error: cannot move sudoers rule into place");
        unlink(sudoers_tmp);
        return 1;
    }
    chmod(sudoers, 0440); /* rename preserves 0440; enforce explicitly anyway */
    printf("Sudoers:  %s (passwordless sudo for %s)\n", sudoers, user);

    printf("\nDone. cctl is installed to /usr/local/bin/cctl and configured with passwordless sudo.\n");
    printf("Privileged commands will auto-elevate seamlessly without needing any shell alias.\n");
    return 0;
}
