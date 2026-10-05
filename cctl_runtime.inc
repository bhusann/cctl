/* ========================================================================
 * CLI
 * ======================================================================== */

/* Returns 1 if this process is running from /usr/local/bin/cctl (installed),
 * 0 otherwise. Used to decide whether to show the install hint in help. */
static int is_installed_systemwide(void)
{
    char path[PATH_MAX];
    if (get_self_exe(path, sizeof(path)) < 0) return 0;
    return (strcmp(path, "/usr/local/bin/cctl") == 0);
}

/* True when any cctl-supported TUXEDO/Clevo driver module is loaded. */
static int drivers_loaded(void)
{
    static const char *const modules[] = {
        "clevo_acpi", "tuxedo_keyboard", "tuxedo_io"
    };
    FILE *fp = fopen("/proc/modules", "r");
    if (!fp) return access("/dev/tuxedo_io", F_OK) == 0;

    char line[256], name[64];
    int loaded = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%63s", name) != 1) continue;
        for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
            if (strcmp(name, modules[i]) == 0) {
                loaded = 1;
                break;
            }
        }
        if (loaded) break;
    }
    fclose(fp);
    return loaded;
}

static int not_installed_systemwide(void)
{
    return !is_installed_systemwide();
}

static void print_usage(const char *prog);

#ifdef CCTL_NVIDIA
static int nvidia_is_loaded(void)
{
    FILE *fp = fopen("/proc/modules", "r");
    if (!fp) return 0;
    char line[256];
    int loaded = 0;
    while (fgets(line, sizeof(line), fp)) {
        char modname[64];
        if (sscanf(line, "%63s", modname) == 1) {
            if (strcmp(modname, "nvidia") == 0) {
                loaded = 1;
                break;
            }
        }
    }
    fclose(fp);
    return loaded;
}
#endif
