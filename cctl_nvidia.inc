#ifdef CCTL_NVIDIA
/* ========================================================================
 * NVIDIA GPU
 * ========================================================================
 * Commands:
 *   nvidia on|off  — Persistent toggle: blacklist/unblacklist +
 *                               initramfs rebuild + modprobe/rmmod.
 *   nvidia load              — Session-only: wake GPU (D3cold→D0), temp-remove
 *                               blacklist, modprobe nvidia + nvidia_uvm, restore
 *                               blacklist. Requires blacklist mode.
 *   nvidia loadgame          — Session-only: wake GPU (D3cold→D0), temp-remove
 *                               blacklist, modprobe all 4 nvidia modules, restore
 *                               blacklist. Requires blacklist mode.
 *   nvidia unload            — Session-only: rmmod all nvidia modules, power off
 *                               GPU (D0→D3cold). Requires blacklist mode.
 *   nvidia status            — Show boot config, module state, GPU telemetry.
 *   nvidia clock <min,max>   — Auto-enable persistence + lock GPU clocks (-lgc).
 *   nvidia clock reset       — Unlock GPU clocks (-rgc); disable persistence
 *                               when no graphics or memory clock lock remains.
 *   nvidia memclock <min,max>— Auto-enable persistence + lock memory clocks (-lmc).
 *   nvidia memclock reset    — Unlock memory clocks (-rmc); disable persistence
 *                               when no graphics or memory clock lock remains.
 * ======================================================================== */

static int nvidia_is_blacklisted(void)
{
    /* If a previous run was interrupted during nvidia_load, repair the blacklist */
    if (access("/etc/modprobe.d/blacklist-nvidia.conf", F_OK) != 0 &&
        access("/etc/modprobe.d/blacklist-nvidia.conf.bak", F_OK) == 0) {
        rename("/etc/modprobe.d/blacklist-nvidia.conf.bak",
               "/etc/modprobe.d/blacklist-nvidia.conf");
    }

    if (access("/etc/modprobe.d/blacklist-nvidia.conf", F_OK) != 0)
        return 0;
    FILE *fp = fopen("/etc/modprobe.d/blacklist-nvidia.conf", "r");
    if (!fp) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "blacklist nvidia", 16) == 0) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

static int nvidia_gpu_in_use(void)
{
    if (!nvidia_is_loaded()) return 0;
    char line[128] = {0};
    char *const args[] = { "nvidia-smi", "--query-compute-apps=pid",
                           "--format=csv,noheader", NULL };
    if (run(args, RUN_CAPTURE, line, sizeof(line)) != 0)
        return 0;
    return (line[0] != '\0');
}

/* Detect whether the NVIDIA GPU is driving a connected display (DRM active).
 * Returns 1 if an nvidia/nvidia_drm-driven, connected connector is found. */
static int nvidia_display_in_use(void)
{
    DIR *d = opendir("/sys/class/drm");
    if (!d) return 0;
    struct dirent *ent;
    char path[512];
    int in_use = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "card", 4) != 0) continue;
        if (ent->d_name[4] < '0' || ent->d_name[4] > '9') continue;

        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/driver", ent->d_name);
        char driver[64] = {0};
        ssize_t l = readlink(path, driver, sizeof(driver) - 1);
        if (l <= 0) continue;
        driver[l] = '\0';
        char *base = strrchr(driver, '/');
        if (!base) continue;
        if (strcmp(base + 1, "nvidia") != 0 && strcmp(base + 1, "nvidia_drm") != 0)
            continue;

        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", ent->d_name);
        FILE *fp = fopen(path, "r");
        if (fp) {
            char st[16] = {0};
            if (fgets(st, sizeof(st), fp) && strncmp(st, "connected", 9) == 0)
                in_use = 1;
            fclose(fp);
        }
        if (in_use) break;
    }
    closedir(d);
    return in_use;
}

#endif /* CCTL_NVIDIA */

/* Forward declaration for the private load/unload PCI power helper. */
static int nvidia_find_pci_address(char *buf, size_t bufsz);

#ifdef CCTL_NVIDIA

/* Unload order = reverse of load; blacklist order = same as unload */
static const char *const nvidia_modules[] = {
    "nvidia_drm", "nvidia_modeset", "nvidia_uvm", "nvidia"
};
#define NV_MODULE_COUNT (sizeof(nvidia_modules) / sizeof(nvidia_modules[0]))

/* Check whether an initramfs image exists in /boot.
 * Returns 1 if found, 0 otherwise. */
static int initramfs_present(void)
{
    DIR *d = opendir("/boot");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strstr(e->d_name, "initramfs") || strstr(e->d_name, "initrd")) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* Print a distro-appropriate install command for the initramfs rebuild tool. */
static void print_initramfs_install_hint(void)
{
    char id[64] = {0};
    FILE *fp = fopen("/etc/os-release", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "ID=", 3) == 0) {
                char *val = line + 3;
                while (*val == '"' || *val == '\'') val++;
                size_t len = strlen(val);
                while (len > 0 && (val[len - 1] == '"' || val[len - 1] == '\'' ||
                                   val[len - 1] == '\n' || val[len - 1] == '\r'))
                    val[--len] = '\0';
                strncpy(id, val, sizeof(id) - 1);
                break;
            }
        }
        fclose(fp);
    }

    if (strstr(id, "arch") || strstr(id, "manjaro") || strstr(id, "endeavouros"))
        fprintf(stderr, "  Install: sudo pacman -S mkinitcpio\n");
    else if (strstr(id, "fedora"))
        fprintf(stderr, "  Install: sudo dnf install dracut\n");
    else if (strstr(id, "rhel") || strstr(id, "centos") || strstr(id, "rocky") || strstr(id, "alma"))
        fprintf(stderr, "  Install: sudo dnf install dracut\n");
    else if (strstr(id, "suse"))
        fprintf(stderr, "  Install: sudo zypper install dracut\n");
    else if (strstr(id, "debian") || strstr(id, "ubuntu") || strstr(id, "mint") ||
             strstr(id, "pop") || strstr(id, "elementary") || strstr(id, "zorin"))
        fprintf(stderr, "  Install: sudo apt install initramfs-tools\n");
    else
        fprintf(stderr, "  Install one of:\n"
                "    sudo pacman -S mkinitcpio          (Arch/Manjaro)\n"
                "    sudo dnf install dracut             (Fedora/RHEL)\n"
                "    sudo zypper install dracut           (openSUSE)\n"
                "    sudo apt install initramfs-tools     (Debian/Ubuntu)\n");
}

/* Pre-check whether an initramfs rebuild can be performed.
 * Must be called BEFORE writing any config files so that on failure
 * nothing is left in a half-applied state.
 *
 * Returns:
 *   1  -> initramfs tool present; caller may write config then rebuild
 *   0  -> no initramfs on this system; no rebuild needed (config-only)
 *  -1  -> initramfs present but no rebuild tool installed; caller MUST abort
 */
static int initramfs_rebuild_ready(void)
{
    int has_initramfs = initramfs_present();
    if (!has_initramfs) {
        printf("  No initramfs detected in /boot. The modprobe blacklist\n");
        printf("  takes effect directly on next boot without a rebuild.\n");
        return 0;
    }

    if (access("/usr/bin/mkinitcpio", X_OK) == 0 ||
        access("/usr/bin/dracut", X_OK) == 0 ||
        access("/usr/sbin/update-initramfs", X_OK) == 0 ||
        access("/sbin/update-initramfs", X_OK) == 0)
        return 1;

    /* initramfs present but no tool to rebuild it */
    fprintf(stderr, "\n  %sError: This system uses an initramfs but no rebuild tool is installed.%s\n\n", C_RED, C_RST);
    print_initramfs_install_hint();
    fprintf(stderr, "  Install the tool first, then re-run this command.\n\n");
    return -1;
}

/* Rebuild the initramfs using whichever tool is available.
 * Caller must have already confirmed initramfs_rebuild_ready() == 1.
 * Returns 0 on success, -1 on failure. */
static int rebuild_initramfs(void)
{
    if (access("/usr/bin/mkinitcpio", X_OK) == 0) {
        printf("  Rebuilding initramfs with mkinitcpio (this may take a minute)...\n");
        char *const args[] = { "mkinitcpio", "-P", NULL };
        return run(args, 0, NULL, 0);
    }
    if (access("/usr/bin/dracut", X_OK) == 0) {
        printf("  Rebuilding initramfs with dracut (this may take a minute)...\n");
        char *const args[] = { "dracut", "--force", NULL };
        return run(args, 0, NULL, 0);
    }
    if (access("/usr/sbin/update-initramfs", X_OK) == 0) {
        printf("  Rebuilding initramfs with update-initramfs (this may take a minute)...\n");
        char *const args[] = { "update-initramfs", "-u", NULL };
        return run(args, 0, NULL, 0);
    }
    if (access("/sbin/update-initramfs", X_OK) == 0) {
        printf("  Rebuilding initramfs with update-initramfs (this may take a minute)...\n");
        char *const args[] = { "update-initramfs", "-u", NULL };
        return run(args, 0, NULL, 0);
    }
    /* Should never reach here if initramfs_rebuild_ready() was called first */
    fprintf(stderr, "Error: No initramfs tool found (mkinitcpio/dracut/update-initramfs).\n");
    return -1;
}

/* Scan /proc/modules and populate loaded[NV_MODULE_COUNT] with 1 for each
 * nvidia module that is currently loaded. */
static void nvidia_scan_loaded(int loaded[])
{
    memset(loaded, 0, NV_MODULE_COUNT * sizeof(int));
    FILE *fp = fopen("/proc/modules", "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char name[64];
        if (sscanf(line, "%63s", name) == 1) {
            for (size_t i = 0; i < NV_MODULE_COUNT; i++) {
                if (strcmp(name, nvidia_modules[i]) == 0) {
                    loaded[i] = 1;
                    break;
                }
            }
        }
    }
    fclose(fp);
}

/* Write blacklist configuration file for all NVIDIA modules */
static int nvidia_write_blacklist(void)
{
    FILE *fp = fopen("/etc/modprobe.d/blacklist-nvidia.conf", "w");
    if (!fp) {
        perror("fopen blacklist-nvidia.conf");
        return -1;
    }
    fprintf(fp, "# Disabled by cctl nvidia off\n");
    for (size_t i = 0; i < NV_MODULE_COUNT; i++) {
        fprintf(fp, "blacklist %s\n", nvidia_modules[i]);
        fprintf(fp, "alias %s off\n", nvidia_modules[i]);
    }
    fclose(fp);
    return 0;
}

static int try_unload_nvidia(void)
{
    printf("  Attempting to unload NVIDIA modules...\n");
    if (nvidia_gpu_in_use()) {
        fprintf(stderr, "Error: GPU is actively in use by running processes:\n");
        char *const query_args[] = {
            "sh", "-c",
            "nvidia-smi --query-compute-apps=pid,name --format=csv,noheader 2>/dev/null | awk -F', ' '{print \"    PID \" $1 \": \" $2}'",
            NULL
        };
        run_quiet(query_args);
        fprintf(stderr, "Cannot unload modules while GPU is in use.\n");
        return -1;
    }

    int loaded[NV_MODULE_COUNT];
    nvidia_scan_loaded(loaded);

    int failed = 0;
    for (size_t i = 0; i < NV_MODULE_COUNT; i++) {
        if (loaded[i]) {
            char *const args[] = { "modprobe", "-r", (char *)nvidia_modules[i], NULL };
            if (run(args, RUN_SILENT, NULL, 0) == 0) {
                printf("  Unloaded %s\n", nvidia_modules[i]);
            } else {
                fprintf(stderr, "  Failed to unload %s\n", nvidia_modules[i]);
                failed = 1;
            }
        }
    }
    return failed ? -1 : 0;
}

static int try_load_nvidia(void)
{
    printf("  Attempting to load NVIDIA modules...\n");
    char *const args[] = { "modprobe", "nvidia", NULL };
    if (run(args, RUN_SILENT, NULL, 0) == 0) {
        printf("  NVIDIA modules loaded successfully.\n");
        return 0;
    }
    fprintf(stderr, "  Could not load NVIDIA modules. Reboot may be required.\n");
    return -1;
}

static int reply_is_yes(const char *s)
{
    char buf[64];
    size_t n = 0;
    while (*s && n < sizeof(buf) - 1) {
        buf[n++] = (char)tolower((unsigned char)*s);
        s++;
    }
    buf[n] = '\0';
    size_t a = 0;
    while (a < n && (buf[a] == ' ' || buf[a] == '\t' || buf[a] == '\n' || buf[a] == '\r')) a++;
    size_t b = n;
    while (b > a && (buf[b-1] == ' ' || buf[b-1] == '\t' || buf[b-1] == '\n' || buf[b-1] == '\r')) b--;
    buf[b] = '\0';
    return strcmp(buf + a, "yes") == 0;
}

static int nvidia_set_off(void)
{
    if (nvidia_is_blacklisted() && !nvidia_is_loaded()) {
        printf("  NVIDIA is already blacklisted and modules are unloaded.\n");
        return 0;
    }

    /* Experimental / permanent-disable warning + double confirmation */
    printf("\n");
    printf("  %sWARNING: 'nvidia off' PERMANENTLY disables the discrete GPU at boot.%s\n", C_YLW, C_RST);
    printf("  This is experimental and can break games and other dGPU-accelerated\n");
    printf("  software. The GPU stays OFF after reboot until you run 'nvidia on'.\n");
    printf("  This option is intended for developers, hardware tinkerers, and users\n");
    printf("  who know exactly what they are doing.\n");
    printf("  %sTry 'nvidia unload' first%s to switch the GPU off for this session\n", C_CYN, C_RST);
    printf("  only (it reverts after reboot). Use 'nvidia off' as a LAST RESORT.\n");

    char reply[64];
    printf("  Type 'yes' to continue, or anything else to abort: ");
    if (!fgets(reply, sizeof(reply), stdin) || !reply_is_yes(reply)) {
        printf("  Aborted.\n");
        return 0;
    }
    printf("  Are you sure? Type 'yes' again to proceed: ");
    if (!fgets(reply, sizeof(reply), stdin) || !reply_is_yes(reply)) {
        printf("  Aborted.\n");
        return 0;
    }

    if (nvidia_is_blacklisted()) {
        /* blacklisted but modules still loaded -> just unload now */
        printf("  NVIDIA is blacklisted but modules are still loaded (reboot pending).\n");
        try_unload_nvidia();
        return 0;
    }

    /* Pre-check: can initramfs be rebuilt if needed? Must be called
     * BEFORE writing the blacklist so nothing is left half-applied. */
    int initrd_ready = initramfs_rebuild_ready();
    if (initrd_ready < 0)
        return -1;

    printf("  Writing blacklist to /etc/modprobe.d/blacklist-nvidia.conf...\n");
    if (nvidia_write_blacklist() != 0)
        return -1;

    if (initrd_ready == 1) {
        if (rebuild_initramfs() < 0) {
            fprintf(stderr, "Error: Initramfs rebuild failed. Removing blacklist config.\n");
            unlink("/etc/modprobe.d/blacklist-nvidia.conf");
            return -1;
        }
    }

    if (nvidia_is_loaded()) {
        try_unload_nvidia();
    }

    printf("\n  NVIDIA is %sblacklisted%s. Reboot to complete. After reboot, only iGPU will be active.\n", C_RED, C_RST);
    return 0;
}

static int nvidia_set_on(void)
{
    if (!nvidia_is_blacklisted()) {
        if (nvidia_is_loaded()) {
            printf("  NVIDIA is already enabled and modules are loaded.\n");
        } else {
            printf("  NVIDIA is enabled but modules aren't loaded.\n");
            printf("  Attempt to load modules now? [y/N] ");
            char reply[16];
            if (fgets(reply, sizeof(reply), stdin) && (reply[0] == 'y' || reply[0] == 'Y'))
                try_load_nvidia();
        }
        return 0;
    }

    printf("  Unblacklist NVIDIA and rebuild initramfs? [y/N] ");
    char reply[16];
    if (!fgets(reply, sizeof(reply), stdin) || (reply[0] != 'y' && reply[0] != 'Y')) {
        printf("  Aborted.\n");
        return 0;
    }

    /* Pre-check: can initramfs be rebuilt if needed? */
    int initrd_ready = initramfs_rebuild_ready();
    if (initrd_ready < 0)
        return -1;

    printf("  Removing /etc/modprobe.d/blacklist-nvidia.conf...\n");
    unlink("/etc/modprobe.d/blacklist-nvidia.conf");

    if (initrd_ready == 1) {
        if (rebuild_initramfs() < 0) {
            fprintf(stderr, "Error: Initramfs rebuild failed. Restoring blacklist config.\n");
            nvidia_write_blacklist();
            return -1;
        }
    }

    if (!nvidia_is_loaded()) {
        try_load_nvidia();
    }

    printf("\n  NVIDIA is %senabled%s. Reboot to complete. After reboot, NVIDIA will be available.\n", C_GRN, C_RST);
    return 0;
}

static void nvidia_show_status(void)
{
    int blacklisted = nvidia_is_blacklisted();
    int loaded = nvidia_is_loaded();

    printf("  Boot config:   %s%s%s\n", blacklisted ? C_RED : C_GRN,
           blacklisted ? "BLACKLISTED" : "ENABLED", C_RST);
    printf("  Module state:  %s%s%s\n", loaded ? C_GRN : C_DIM,
           loaded ? "LOADED" : "NOT LOADED", C_RST);

    if (blacklisted && loaded) {
        printf("  GPU state:     %sON%s (session-only) — stays OFF after reboot\n", C_GRN, C_RST);
    } else if (blacklisted && !loaded) {
        printf("  GPU state:     %sOFF%s (persistent)\n", C_DIM, C_RST);
    } else if (!blacklisted && loaded) {
        printf("  GPU state:     %sON%s (persistent)\n", C_GRN, C_RST);
    } else {
        printf("  GPU state:     %sOFF%s\n", C_DIM, C_RST);
        printf("  Pending:       %sReboot needed to load%s\n", C_YLW, C_RST);
    }

    if (loaded) {
        if (access("/usr/bin/nvidia-smi", X_OK) == 0) {
            printf("\n%s--- NVIDIA GPU Telemetry ---%s\n", C_YLW, C_RST);
            char *const telemetry_args[] = {
                "sh", "-c",
                "nvidia-smi --query-gpu=name,driver_version,memory.used,memory.total,power.draw,temperature.gpu,persistence_mode --format=csv,noheader,nounits 2>/dev/null | "
                "awk -F', ' '{print \"  GPU:           \" $1 \"\\n  Driver:        \" $2 \"\\n  VRAM:          \" $3 \" / \" $4 \" MiB\\n  Power draw:    \" $5 \" W\\n  Temperature:   \" $6 \"°C\\n  Persistence:   \" $7}'",
                NULL
            };
            run_quiet(telemetry_args);

            char procs_buf[1024] = {0};
            char *const p_args[] = { "nvidia-smi", "--query-compute-apps=pid,name,used_memory",
                                     "--format=csv,noheader", NULL };
            int has_procs = 0;
            if (run(p_args, RUN_CAPTURE, procs_buf, sizeof(procs_buf)) == 0 && procs_buf[0]) {
                char *line = procs_buf;
                while (*line) {
                    char *next = strchr(line, '\n');
                    if (next) *next = '\0';
                    char pid[32] = {0}, name[128] = {0}, mem[64] = {0};
                    if (sscanf(line, "%31[^,], %127[^,], %63[^\n]", pid, name, mem) >= 2) {
                        if (!has_procs) {
                            printf("\n  GPU Processes:\n");
                            has_procs = 1;
                        }
                        printf("    PID %-8s %-30s %s\n", pid, name, mem);
                    }
                    if (!next) break;
                    line = next + 1;
                }
            }
            if (!has_procs) {
                printf("  Processes:     none\n");
            }
        }
    }
}

static void nvidia_pci_set_control(const char *val, const char *msg)
{
    char pci_path[512];
    if (nvidia_find_pci_address(pci_path, sizeof(pci_path)) == 0) {
        char ctrl_path[576];
        snprintf(ctrl_path, sizeof(ctrl_path), "%s/power/control", pci_path);
        FILE *fp = fopen(ctrl_path, "w");
        if (fp) {
            fprintf(fp, "%s", val);
            fclose(fp);
            printf("  GPU %s.\n", msg);
        }
    }
}

static int nvidia_load(int load_game)
{
    if (!nvidia_is_blacklisted()) {
        fprintf(stderr, "  Error: NVIDIA is not blacklisted. Use 'nvidia on' instead.\n");
        return 1;
    }
    if (nvidia_is_loaded()) {
        printf("  NVIDIA module is already loaded.\n");
        return 0;
    }

    /* Wake GPU from D3cold if needed */
    nvidia_pci_set_control("on", "powered on (D0)");

    /* Temporarily move blacklist aside so modprobe works.
     * Block termination signals while the file is swapped so an interrupt doesn't
     * leave the blacklist missing. */
    printf("  Loading NVIDIA modules (session-only)...\n");
    sigset_t block_mask, orig_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGHUP);
    sigaddset(&block_mask, SIGINT);
    sigaddset(&block_mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &block_mask, &orig_mask);

    rename("/etc/modprobe.d/blacklist-nvidia.conf",
           "/etc/modprobe.d/blacklist-nvidia.conf.bak");

    char *const args_nv[] = { "modprobe", "nvidia", NULL };
    int ret = run(args_nv, 0, NULL, 0);
    if (ret == 0) {
        char *const args_uvm[] = { "modprobe", "nvidia_uvm", NULL };
        run(args_uvm, 0, NULL, 0);
        if (load_game) {
            char *const args_modeset[] = { "modprobe", "nvidia_modeset", NULL };
            run(args_modeset, 0, NULL, 0);
            char *const args_drm[] = { "modprobe", "nvidia_drm", NULL };
            run(args_drm, 0, NULL, 0);
        }
    }

    /* Restore blacklist immediately */
    rename("/etc/modprobe.d/blacklist-nvidia.conf.bak",
           "/etc/modprobe.d/blacklist-nvidia.conf");

    sigprocmask(SIG_SETMASK, &orig_mask, NULL);

    if (ret == 0) {
        if (load_game) {
            printf("  NVIDIA modules loaded (nvidia + nvidia_uvm + nvidia_modeset + nvidia_drm). GPU available for this session.\n");
        } else {
            printf("  NVIDIA modules loaded (nvidia + nvidia_uvm). GPU available for this session.\n");
            printf("  %sNote:%s For display/gaming, also run: sudo modprobe nvidia_drm (or use loadgame)\n", C_YLW, C_RST);
        }
        printf("  On next reboot, NVIDIA will remain off (blacklist intact).\n");
    } else {
        fprintf(stderr, "  Failed to load NVIDIA modules.\n");
    }
    return ret;
}

static int nvidia_unload(void)
{
    if (!nvidia_is_blacklisted()) {
        fprintf(stderr, "  Error: NVIDIA is not blacklisted. Use 'nvidia off' instead.\n");
        return 1;
    }
    if (!nvidia_is_loaded()) {
        printf("  NVIDIA modules are not loaded.\n");
        return 0;
    }

    if (nvidia_gpu_in_use()) {
        fprintf(stderr, "  Error: GPU is actively in use by running processes.\n");
        return 1;
    }

    if (nvidia_display_in_use()) {
        fprintf(stderr, "  Warning: GPU appears to be driving a display (nvidia_drm active).\n");
        fprintf(stderr, "  Unload may fail unless the session has released it (see AutoAddGPU note in README).\n");
    }

    printf("  Unloading NVIDIA modules...\n");
    int loaded[NV_MODULE_COUNT];
    nvidia_scan_loaded(loaded);

    int failed = 0;
    for (size_t i = 0; i < NV_MODULE_COUNT; i++) {
        if (!loaded[i]) continue;

        /* Use rmmod directly instead of modprobe -r (avoids blacklist alias interference) */
        char *const args[] = { "rmmod", (char *)nvidia_modules[i], NULL };
        if (run(args, 0, NULL, 0) == 0) {
            printf("  Unloaded %s\n", nvidia_modules[i]);
        } else {
            fprintf(stderr, "  Failed to unload %s\n", nvidia_modules[i]);
            failed = 1;
        }
    }

    /* Verify nvidia is actually gone */
    if (nvidia_is_loaded()) {
        fprintf(stderr, "  Warning: NVIDIA module is still loaded.\n");
        return -1;
    }

    /* Power off the GPU via PCI runtime PM */
    nvidia_pci_set_control("auto", "powered off (D3cold)");
    return failed ? -1 : 0;
}
#endif /* CCTL_NVIDIA */

/* Find the NVIDIA GPU PCI sysfs path for the private load/unload commands. */
static int nvidia_find_pci_address(char *buf, size_t bufsz)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", ent->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        char vendor[16] = {0};
        if (fgets(vendor, sizeof(vendor), fp)) {
            vendor[strcspn(vendor, "\n")] = 0;
        }
        fclose(fp);
        if (strcmp(vendor, "0x10de") != 0) continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", ent->d_name);
        fp = fopen(path, "r");
        if (!fp) continue;
        char class[16] = {0};
        if (fgets(class, sizeof(class), fp)) {
            class[strcspn(class, "\n")] = 0;
        }
        fclose(fp);
        /* VGA compatible controller: class 0x030000 or 0x0300xx */
        if (strncmp(class, "0x0300", 6) == 0) {
            snprintf(buf, bufsz, "/sys/bus/pci/devices/%s", ent->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

/* Run nvidia-smi -pm <1|0> (toggle persistence mode) */
static int nvidia_pm_set(int on)
{
    char *const args[] = { "nvidia-smi", "-pm", on ? "1" : "0", NULL };
    return run(args, 0, NULL, 0);
}

/* Keep persistence enabled while either cctl-managed clock lock remains. */
static int nvidia_pm_disable_if_unlocked(void)
{
    struct mode_runtime_data data;
    if (mode_runtime_read(&data) != 0)
        return 0; /* Without reliable metadata, leave persistence enabled. */

    int graphics_locked = data.core_clock_known && data.core_clock_set;
    int memory_locked = data.mem_clock_known && data.mem_clock_set;
    if (graphics_locked || memory_locked)
        return 0;
    return nvidia_pm_set(0);
}

static int nvidia_clock_apply(const char *flag, int min, int max)
{
    if (nvidia_pm_set(1) != 0)
        fprintf(stderr, "  Warning: failed to enable persistence mode\n");
    char range[32];
    snprintf(range, sizeof(range), "%d,%d", min, max);
    char *const args[] = { "nvidia-smi", (char *)flag, range, NULL };
    int rc = run(args, 0, NULL, 0);
    if (rc == 0 && mode_record_nvidia_clock(strcmp(flag, "-lmc") == 0, 1, min, max) < 0)
        fprintf(stderr, "Warning: could not record NVIDIA clock settings in %s\n", MODE_FILE);
    return rc;
}

static int nvidia_clock_reset_flag(const char *flag)
{
    char *const args[] = { "nvidia-smi", (char *)flag, NULL };
    int rc = run(args, 0, NULL, 0);
    int memory_clock = strcmp(flag, "-rmc") == 0;
    if (rc == 0) {
        if (mode_record_nvidia_clock(memory_clock, 0, 0, 0) < 0) {
            fprintf(stderr, "Warning: could not record NVIDIA clock reset in %s; leaving persistence mode enabled\n", MODE_FILE);
        } else if (nvidia_pm_disable_if_unlocked() != 0) {
            fprintf(stderr, "Warning: NVIDIA clocks were reset, but persistence mode could not be disabled\n");
        }
    }
    return rc;
}

struct nvidia_clock_info {
    int cur_graphics;
    int cur_memory;
    int cur_sm;
    int cur_video;
    int max_graphics;
    int max_memory;
    int max_sm;
    int max_video;
};

/* Parse nvidia-smi -q -d CLOCK output to get max supported clocks */
static int nvidia_query_clocks(struct nvidia_clock_info *ci)
{
    ci->cur_graphics = -1;
    ci->cur_memory = -1;
    ci->cur_sm = -1;
    ci->cur_video = -1;
    ci->max_graphics = -1;
    ci->max_memory = -1;
    ci->max_sm = -1;
    ci->max_video = -1;

    char buf[4096] = {0};
    char *const args[] = { "nvidia-smi", "-q", "-d", "CLOCK", NULL };
    if (run(args, RUN_CAPTURE, buf, sizeof(buf)) != 0)
        return -1;

    char *line = buf;
    int section = 0; /* 0: other, 1: Clocks, 2: Max Clocks */

    while (*line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';

        if (strncmp(line, "    ", 4) == 0 && line[4] != ' ' && line[4] != '\t') {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "Clocks", 6) == 0 && (p[6] == '\0' || p[6] == ' ' || p[6] == '\r'))
                section = 1;
            else if (strncmp(p, "Max Clocks", 10) == 0 && (p[10] == '\0' || p[10] == ' ' || p[10] == '\r'))
                section = 2;
            else
                section = 0;
            if (!next) break;
            line = next + 1;
            continue;
        }

        if (section == 1 || section == 2) {
            char *colon = strchr(line, ':');
            if (colon) {
                int val = 0;
                if (sscanf(colon + 1, " %d MHz", &val) == 1) {
                    if (strstr(line, "Graphics")) {
                        if (section == 1) ci->cur_graphics = val;
                        else ci->max_graphics = val;
                    } else if (strstr(line, "Memory")) {
                        if (section == 1) ci->cur_memory = val;
                        else ci->max_memory = val;
                    } else if (strstr(line, "SM")) {
                        if (section == 1) ci->cur_sm = val;
                        else ci->max_sm = val;
                    } else if (strstr(line, "Video")) {
                        if (section == 1) ci->cur_video = val;
                        else ci->max_video = val;
                    }
                }
            }
        }
        if (!next) break;
        line = next + 1;
    }

    if (ci->max_graphics > 0 || ci->max_memory > 0)
        return 0;
    return -1;
}

/* Parse clock arguments:
 *   cctl nvidia clock <max>            -> min=0, max=<max>
 *   cctl nvidia clock <min> <max>      -> min=<min>, max=<max>
 *   cctl nvidia clock <min>,<max>      -> min=<min>, max=<max>
 * Also validates: min >= 0, max > 0, min <= max, and max <= max_supported.
 * Returns 0 on success, -1 on error. */
static int nvidia_parse_clock_inputs(int argc, char **argv, int max_supported,
                                     const char *cmd_name, int *out_min, int *out_max)
{
    if (argc < 4) {
        return -1;
    }
    if (argc > 5) {
        fprintf(stderr, "Error: Unexpected extra argument '%s'\n", argv[5]);
        fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                cmd_name, cmd_name);
        return -1;
    }

    int min = 0, max = 0;

    if (argc == 4) {
        /* Single argument: either "<max>" or "<min>,<max>" */
        const char *arg = argv[3];
        const char *comma = strchr(arg, ',');
        if (comma) {
            char s1[32], s2[32];
            size_t len1 = (size_t)(comma - arg);
            if (len1 >= sizeof(s1) || strlen(comma + 1) >= sizeof(s2)) {
                fprintf(stderr, "Error: Invalid clock range '%s' (expected <min>,<max>)\n", arg);
                return -1;
            }
            strncpy(s1, arg, len1);
            s1[len1] = '\0';
            snprintf(s2, sizeof(s2), "%s", comma + 1);
            if (safe_atoi(s1, &min) < 0 || safe_atoi(s2, &max) < 0) {
                fprintf(stderr, "Error: Invalid clock range '%s' (expected <min>,<max>)\n", arg);
                fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                        cmd_name, cmd_name);
                return -1;
            }
        } else {
            if (safe_atoi(arg, &max) < 0) {
                fprintf(stderr, "Error: Invalid clock value '%s'\n", arg);
                fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                        cmd_name, cmd_name);
                return -1;
            }
            min = 0;
        }
    } else {
        /* Two arguments: "<min> <max>" or "<min>, <max>" */
        char s_min[32];
        strncpy(s_min, argv[3], sizeof(s_min) - 1);
        s_min[sizeof(s_min) - 1] = '\0';
        size_t slen = strlen(s_min);
        if (slen > 0 && s_min[slen - 1] == ',')
            s_min[slen - 1] = '\0';

        if (safe_atoi(s_min, &min) < 0) {
            fprintf(stderr, "Error: Invalid minimum clock '%s'\n", argv[3]);
            fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                    cmd_name, cmd_name);
            return -1;
        }
        if (safe_atoi(argv[4], &max) < 0) {
            fprintf(stderr, "Error: Invalid maximum clock '%s'\n", argv[4]);
            fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                    cmd_name, cmd_name);
            return -1;
        }
    }

    if (min < 0) {
        fprintf(stderr, "Error: Minimum clock cannot be negative (%d)\n", min);
        return -1;
    }
    if (max <= 0) {
        fprintf(stderr, "Error: Maximum clock must be greater than 0 (%d)\n", max);
        return -1;
    }
    if (min > max) {
        fprintf(stderr, "Error: Minimum clock (%d MHz) cannot exceed maximum clock (%d MHz)\n",
                min, max);
        return -1;
    }
    if (max_supported > 0 && max > max_supported) {
        fprintf(stderr, "Error: Clock %d MHz exceeds maximum supported %d MHz\n",
                max, max_supported);
        return -1;
    }
    if (max_supported > 0 && min > max_supported) {
        fprintf(stderr, "Error: Minimum clock %d MHz exceeds maximum supported %d MHz\n",
                min, max_supported);
        return -1;
    }

    *out_min = min;
    *out_max = max;
    return 0;
}

static int nvidia_clock_display(const char *label, const char *subcmd, int max_val,
                                int ex_single, int ex_min, int ex_max)
{
    printf("%s%s:%s\n", C_YLW, label, C_RST);
    printf("  %sMax Supported:%s %s%d MHz%s\n\n", C_CYN, C_RST, C_CYN, max_val, C_RST);
    printf("%sUsage:%s %scctl nvidia %s [min] <max>  |  cctl nvidia %s reset%s\n",
           C_BLD, C_RST, C_CYN_BLD, subcmd, subcmd, C_RST);
    printf("  %sExamples:%s cctl nvidia %s %d        %s(auto minimum 0 MHz)%s\n",
           C_YLW, C_RST, subcmd, ex_single, C_DIM, C_RST);
    printf("            cctl nvidia %s %d %d\n", subcmd, ex_min, ex_max);
    printf("            cctl nvidia %s %d,%d\n", subcmd, ex_min, ex_max);
    printf("            cctl nvidia %s reset\n", subcmd);
    return 0;
}

struct nvidia_clk_target {
    const char *name;      /* "clock" or "memclock" */
    const char *desc;      /* "GPU Graphics Clock" or "GPU Memory Clock" */
    const char *set_flag;  /* "-lgc" or "-lmc" */
    const char *rst_flag;  /* "-rgc" or "-rmc" */
    int def_val, ex_min, ex_max;
};

static const struct nvidia_clk_target nvidia_clk_targets[] = {
    { "clock",    "GPU Graphics Clock", "-lgc", "-rgc", 1500, 210, 1500 },
    { "memclock", "GPU Memory Clock",   "-lmc", "-rmc", 5000, 405, 5000 },
};

static const struct nvidia_clk_target *nvidia_find_clk_target(const char *name)
{
    for (size_t i = 0; i < sizeof(nvidia_clk_targets) / sizeof(nvidia_clk_targets[0]); i++) {
        if (strcmp(name, nvidia_clk_targets[i].name) == 0)
            return &nvidia_clk_targets[i];
    }
    return NULL;
}

static int nvidia_clock_show_target(const struct nvidia_clk_target *tgt)
{
    struct nvidia_clock_info ci;
    if (nvidia_query_clocks(&ci) != 0) {
        fprintf(stderr, "Error: Unable to query %s via nvidia-smi\n", tgt->desc);
        return 1;
    }
    int max_clk = (strcmp(tgt->name, "clock") == 0) ? ci.max_graphics : ci.max_memory;
    if (max_clk <= 0) {
        fprintf(stderr, "Error: Unable to query %s via nvidia-smi\n", tgt->desc);
        return 1;
    }
    return nvidia_clock_display(tgt->desc, tgt->name, max_clk, tgt->def_val, tgt->ex_min, tgt->ex_max);
}

#ifdef CCTL_NVIDIA
#define NVIDIA_USAGE_STR "nvidia {on|off|load|loadgame|unload|status|clock|memclock}"
#else
#define NVIDIA_USAGE_STR "nvidia {clock|memclock}"
#endif

static int cmd_nvidia(int argc, char **argv)
{
    if (argc < 3) {
        printf("%sUsage:%s %scctl %s%s\n\n", C_BLD, C_RST, C_CYN_BLD, NVIDIA_USAGE_STR, C_RST);
        printf("%sCommands:%s\n", C_YLW, C_RST);
#ifdef CCTL_NVIDIA
        printf("  %s%-10s%s Show GPU status & telemetry\n", C_CYN, "status", C_RST);
#endif
        printf("  %s%-10s%s Lock/unlock GPU core clocks (no arg: show max clock)\n", C_CYN, "clock", C_RST);
        printf("  %s%-10s%s Lock/unlock GPU memory clocks (no arg: show max clock)\n", C_CYN, "memclock", C_RST);
        return 0;
    }
    const char *action = argv[2];
    const struct nvidia_clk_target *clk_tgt = nvidia_find_clk_target(action);

#ifndef CCTL_NVIDIA
    /* Module/GPU-toggle commands exist only in the private build. In this
     * build they are treated as ordinary unknown actions — the private
     * build's name must never appear in public output. */
    if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0 ||
        strcmp(action, "load") == 0 || strcmp(action, "loadgame") == 0 ||
        strcmp(action, "unload") == 0 || strcmp(action, "status") == 0) {
        fprintf(stderr, "Error: Unknown nvidia action '%s'\n", action);
        fprintf(stderr, "Usage: %s\n", NVIDIA_USAGE_STR);
        return 1;
    }
#endif

#ifdef CCTL_NVIDIA
    if (strcmp(action, "status") == 0) {
        nvidia_show_status();
        return 0;
    }
#endif

    /* Queries without arguments run unprivileged (no root needed, all builds) */
    if (clk_tgt && argc < 4) {
        return nvidia_clock_show_target(clk_tgt);
    }

    int clk_min = 0, clk_max = 0;
    int is_reset = 0;

    /* Validate arguments before requesting root elevation */
    if (clk_tgt) {
        if (strcmp(argv[3], "reset") == 0) {
            if (argc > 4) {
                fprintf(stderr, "Error: Unexpected extra argument '%s'\n", argv[4]);
                fprintf(stderr, "Usage: cctl nvidia %s [min] <max>  |  cctl nvidia %s reset\n",
                        clk_tgt->name, clk_tgt->name);
                return 1;
            }
            is_reset = 1;
        } else {
            struct nvidia_clock_info ci;
            nvidia_query_clocks(&ci);
            int max_supported = (strcmp(clk_tgt->name, "clock") == 0) ? ci.max_graphics : ci.max_memory;
            if (nvidia_parse_clock_inputs(argc, argv, max_supported, clk_tgt->name, &clk_min, &clk_max) != 0)
                return 1;
        }
#ifndef CCTL_NVIDIA
    } else {
        fprintf(stderr, "Error: Unknown nvidia action '%s'\n", action);
        fprintf(stderr, "Usage: %s\n", NVIDIA_USAGE_STR);
        return 1;
#endif
    }

    /* All other actions need root */
    require_root(argc, argv);

#ifdef CCTL_NVIDIA
    if (strcmp(action, "off") == 0) {
        return nvidia_set_off();
    } else if (strcmp(action, "on") == 0) {
        return nvidia_set_on();
    } else if (strcmp(action, "load") == 0) {
        return nvidia_load(0);
    } else if (strcmp(action, "loadgame") == 0) {
        return nvidia_load(1);
    } else if (strcmp(action, "unload") == 0) {
        return nvidia_unload();
    } else
#endif
    if (clk_tgt) {
        if (is_reset)
            return nvidia_clock_reset_flag(clk_tgt->rst_flag);
        return nvidia_clock_apply(clk_tgt->set_flag, clk_min, clk_max);
    } else {
        fprintf(stderr, "Error: Unknown nvidia action '%s'\n", action);
        fprintf(stderr, "Usage: %s\n", NVIDIA_USAGE_STR);
        return 1;
    }
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    if (argc > 3 || (argc == 3 && strcmp(argv[2], "--json") != 0))
        return fail("Unexpected extra argument '%s'", argv[2]);
    json_output = (argc == 3);
    show_status();
    json_output = 0;
    return 0;
}

static int cmd_rr(int argc, char **argv)
{
    if (argc < 3)
        return rr_list();

    /* Shortcuts: 1 → highest rate, 2 → lowest rate (e.g. 165Hz / 40Hz) */
    const char *rate = argv[2];
    if (strcmp(rate, "1") == 0 || strcmp(rate, "2") == 0) {
        struct display_info info;
        if (query_display_info(&info) < 0 || info.rate_count < 2) {
            fprintf(stderr, "Error: cannot detect available rates\n");
            return -1;
        }
        int idx = (rate[0] == '1') ? 0 : (info.rate_count - 1);
        rate = info.available_rates[idx];
    }

    int rc = rr_set(rate);
    if (rc == 0) print_done();
    return rc;
}
