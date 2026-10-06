/* Visual monitor shared by cctl and the standalone prototype. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BAR_WIDTH 36
#define CPU_BAR_WIDTH 36
#define POWER_HEIGHT 8
#define FAN_W 9
#define FAN_H 5
#define FAN_COLUMN 98
#define FAN_HEADING_ROW 4
#define RAM_TOP_ROW 9
#define MAX_CPU_CORES 512
#define PI_D 3.14159265358979323846
#define EC_RAM_PATH "/sys/kernel/debug/ec/ec0/io"
#define EC_FAN_RPM_DIVISOR 2156220
#define R_HWCHECK_CL 0x8008EC05
#define R_CL_FANINFO1 0x8008ED10
#define R_CL_FANINFO2 0x8008ED11

struct cpu_ticks {
    unsigned long long idle;
    unsigned long long total;
};

struct cpu_core {
    int cpu_id;
    int is_e_core;
    double frequency_mhz;
    unsigned int max_frequency_khz;
    unsigned int frequency_cap_khz;
    unsigned int base_frequency_khz;
};

static volatile sig_atomic_t visual_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    visual_running = 0;
}

static int read_cpu_ticks(struct cpu_ticks *ticks)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp)
        return -1;

    char line[256];
    unsigned long long user, nice, system, idle, iowait = 0;
    unsigned long long irq = 0, softirq = 0, steal = 0;
    int fields = 0;
    if (fgets(line, sizeof(line), fp))
        fields = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &user, &nice, &system, &idle, &iowait, &irq,
                        &softirq, &steal);
    fclose(fp);
    if (fields < 4)
        return -1;

    ticks->idle = idle + iowait;
    ticks->total = user + nice + system + idle + iowait + irq + softirq + steal;
    return 0;
}

static int cpu_usage_since(struct cpu_ticks *previous)
{
    struct cpu_ticks current;
    if (read_cpu_ticks(&current) < 0)
        return -1;
    if (previous->total == 0) {
        *previous = current;
        return -1;
    }

    unsigned long long idle_delta = current.idle - previous->idle;
    unsigned long long total_delta = current.total - previous->total;
    *previous = current;
    if (total_delta == 0)
        return 0;
    return (int)(100 * (total_delta - idle_delta) / total_delta);
}

/* Match cctl monitor: CPUs below 85% of the global max frequency are E-cores. */
static unsigned int cpu_max_frequency_khz(int cpu_id)
{
    char path[160];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu_id);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    unsigned int khz = 0;
    if (fscanf(fp, "%u", &khz) != 1)
        khz = 0;
    fclose(fp);
    return khz;
}

static unsigned int cpu_base_frequency_khz(int cpu_id)
{
    char path[160];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/base_frequency", cpu_id);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    unsigned int khz = 0;
    if (fscanf(fp, "%u", &khz) != 1)
        khz = 0;
    fclose(fp);
    return khz;
}

static unsigned int cpu_scaling_max_frequency_khz(int cpu_id)
{
    char path[160];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu_id);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return cpu_max_frequency_khz(cpu_id);
    unsigned int khz = 0;
    if (fscanf(fp, "%u", &khz) != 1)
        khz = 0;
    fclose(fp);
    return khz > 0 ? khz : cpu_max_frequency_khz(cpu_id);
}

static int cpu_turbo_enabled(void)
{
    FILE *fp = fopen("/sys/devices/system/cpu/intel_pstate/no_turbo", "r");
    if (!fp)
        return 0;
    int no_turbo = 1;
    if (fscanf(fp, "%d", &no_turbo) != 1)
        no_turbo = 1;
    fclose(fp);
    return no_turbo == 0;
}

static int read_cpu_core_frequencies(struct cpu_core *cores, int capacity)
{
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp)
        return 0;

    int count = 0;
    char line[256];
    while (count < capacity && fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu MHz", 7) != 0)
            continue;
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        char *end = NULL;
        double mhz = strtod(colon + 1, &end);
        if (end == colon + 1 || mhz < 0.0)
            continue;
        cores[count].cpu_id = count;
        cores[count].frequency_mhz = mhz;
        count++;
    }
    fclose(fp);
    return count;
}

static int discover_cpu_cores(struct cpu_core *cores, int capacity)
{
    int configured = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (configured <= 0)
        configured = 64;
    if (configured > capacity)
        configured = capacity;

    for (int cpu = 0; cpu < configured; cpu++) {
        cores[cpu].max_frequency_khz = cpu_max_frequency_khz(cpu);
        cores[cpu].frequency_cap_khz = cpu_scaling_max_frequency_khz(cpu);
        cores[cpu].base_frequency_khz = cpu_base_frequency_khz(cpu);
    }

    int count = read_cpu_core_frequencies(cores, configured);
    if (count == 0)
        return 0;

    unsigned int global_max_khz = 0;
    for (int cpu = 0; cpu < configured; cpu++) {
        if (cores[cpu].max_frequency_khz > global_max_khz)
            global_max_khz = cores[cpu].max_frequency_khz;
    }

    for (int cpu = 0; cpu < count; cpu++) {
        unsigned int max_khz = cores[cpu].max_frequency_khz;
        cores[cpu].is_e_core = global_max_khz > 0 && max_khz > 0 &&
            (double)max_khz < (double)global_max_khz * 0.85;
    }
    return count;
}

static void refresh_cpu_core_frequencies(struct cpu_core *cores, int count,
                                         int turbo_enabled)
{
    (void)read_cpu_core_frequencies(cores, count);
    for (int cpu = 0; cpu < count; cpu++) {
        unsigned int cap = cpu_scaling_max_frequency_khz(cores[cpu].cpu_id);
        unsigned int base = cores[cpu].base_frequency_khz;
        if (!turbo_enabled && base > 0 && (cap == 0 || cap > base))
            cap = base;
        cores[cpu].frequency_cap_khz = cap;
    }
}

static int read_milli_degrees(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    long milli;
    int ok = fscanf(fp, "%ld", &milli) == 1;
    fclose(fp);
    return ok && milli >= 0 && milli <= 150000 ? (int)(milli / 1000) : -1;
}

static int read_cpu_temperature(void)
{
    static char cached_path[512];
    if (cached_path[0]) {
        int temp = read_milli_degrees(cached_path);
        if (temp >= 0)
            return temp;
        cached_path[0] = '\0';
    }

    DIR *dir = opendir("/sys/class/thermal");
    if (dir) {
        struct dirent *entry;
        char type_path[512], temp_path[512], type[80];
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "thermal_zone", 12) != 0)
                continue;
            snprintf(type_path, sizeof(type_path), "/sys/class/thermal/%s/type",
                     entry->d_name);
            FILE *fp = fopen(type_path, "r");
            if (!fp)
                continue;
            if (!fgets(type, sizeof(type), fp)) {
                fclose(fp);
                continue;
            }
            fclose(fp);
            if (!strstr(type, "x86_pkg") && !strstr(type, "pkg") &&
                !strstr(type, "coretemp"))
                continue;
            snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/%s/temp",
                     entry->d_name);
            int temp = read_milli_degrees(temp_path);
            if (temp >= 0) {
                snprintf(cached_path, sizeof(cached_path), "%s", temp_path);
                closedir(dir);
                return temp;
            }
        }
        closedir(dir);
    }
    return read_milli_degrees("/sys/class/thermal/thermal_zone0/temp");
}

static int read_ram_usage(double *used_gb, double *total_gb)
{
    *used_gb = 0.0;
    *total_gb = 0.0;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp)
        return -1;

    unsigned long long total_kb = 0, available_kb = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %llu kB", &total_kb) == 1)
            continue;
        if (sscanf(line, "MemAvailable: %llu kB", &available_kb) == 1)
            break;
    }
    fclose(fp);
    if (total_kb == 0 || available_kb > total_kb)
        return -1;
    *used_gb = (double)(total_kb - available_kb) / (1024.0 * 1024.0);
    *total_gb = (double)total_kb / (1024.0 * 1024.0);
    return (int)((total_kb - available_kb) * 100 / total_kb);
}

static int tuxedo_open_clevo(void)
{
    if (access("/dev/tuxedo_io", F_OK) != 0)
        return -1;
    int fd = open("/dev/tuxedo_io", O_RDWR);
    if (fd < 0)
        return -1;
    int is_clevo = 0;
    if (ioctl(fd, R_HWCHECK_CL, &is_clevo) < 0 || !is_clevo) {
        close(fd);
        return -1;
    }
    return fd;
}

static int ensure_ec_sys(void)
{
    if (access(EC_RAM_PATH, F_OK) == 0)
        return 0;
    if (geteuid() != 0)
        return -1;

    static int attempted;
    if (attempted)
        return -1;
    attempted = 1;

    pid_t child = fork();
    if (child == 0) {
        execlp("modprobe", "modprobe", "ec_sys", (char *)NULL);
        _exit(127);
    }
    if (child < 0)
        return -1;
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 200000000L };
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
    return access(EC_RAM_PATH, F_OK) == 0 ? 0 : -1;
}

/* Match cctl's telemetry: duty via tuxedo_io and fan RPM via EC RAM. */
static int read_fan_telemetry(int cached_fd, int *cpu_pct, int *gpu_pct,
                              int *cpu_rpm, int *gpu_rpm)
{
    *cpu_pct = *gpu_pct = *cpu_rpm = *gpu_rpm = 0;
    int got_duty = 0;
    int fd = cached_fd >= 0 ? cached_fd : tuxedo_open_clevo();
    if (fd >= 0) {
        int fan1 = 0, fan2 = 0;
        if (ioctl(fd, R_CL_FANINFO1, &fan1) >= 0) {
            *cpu_pct = ((fan1 & 0xFF) * 100) / 255;
            got_duty = 1;
        }
        if (ioctl(fd, R_CL_FANINFO2, &fan2) >= 0) {
            *gpu_pct = ((fan2 & 0xFF) * 100) / 255;
            got_duty = 1;
        }
        if (cached_fd < 0)
            close(fd);
    }

    (void)ensure_ec_sys();
    int ec_fd = open(EC_RAM_PATH, O_RDONLY);
    if (ec_fd < 0)
        return got_duty ? 0 : -1;
    unsigned char ram[256] = {0};
    ssize_t count = read(ec_fd, ram, sizeof(ram));
    close(ec_fd);
    if (count < 0xD4)
        return got_duty ? 0 : -1;

    if (!got_duty) {
        int cpu_raw = count >= 0xF5 ? ram[0xF4] : 0;
        if (cpu_raw == 0)
            cpu_raw = ram[0x89];
        *cpu_pct = (cpu_raw * 100) / 255;
    }
    unsigned int cpu_raw16 = ((unsigned int)ram[0xD0] << 8) | ram[0xD1];
    unsigned int gpu_raw16 = ((unsigned int)ram[0xD2] << 8) | ram[0xD3];
    *cpu_rpm = cpu_raw16 ? (int)(EC_FAN_RPM_DIVISOR / cpu_raw16) : 0;
    *gpu_rpm = gpu_raw16 ? (int)(EC_FAN_RPM_DIVISOR / gpu_raw16) : 0;
    return 0;
}

static int read_package_energy(unsigned long long *energy_uj)
{
    FILE *fp = fopen("/sys/class/powercap/intel-rapl:0/energy_uj", "r");
    if (!fp)
        return -1;
    int ok = fscanf(fp, "%llu", energy_uj) == 1;
    fclose(fp);
    return ok ? 0 : -1;
}

static int read_rapl_limit_watts(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    long long microwatts;
    int ok = fscanf(fp, "%lld", &microwatts) == 1;
    fclose(fp);
    return ok && microwatts >= 0 ? (int)(microwatts / 1000000LL) : -1;
}

static void read_rapl_limits(int *pl1_w, int *pl2_w)
{
    *pl1_w = read_rapl_limit_watts(
        "/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw");
    *pl2_w = read_rapl_limit_watts(
        "/sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw");
}

static double sample_package_power(unsigned long long *previous_energy,
                                   struct timespec *previous_time)
{
    unsigned long long energy;
    struct timespec now;
    if (read_package_energy(&energy) < 0 || clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1.0;

    double elapsed = (double)(now.tv_sec - previous_time->tv_sec) +
                     (double)(now.tv_nsec - previous_time->tv_nsec) / 1e9;
    double watts = (energy >= *previous_energy && elapsed > 0.0) ?
                   (double)(energy - *previous_energy) / elapsed / 1000000.0 : -1.0;
    *previous_energy = energy;
    *previous_time = now;
    return watts;
}

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static void draw_bar(const char *title, int value, int minimum, int maximum,
                     int width, const char *unit, const char *color)
{
    printf("%s%s\033[0m", color, title);
    fputs("\n  [", stdout);

    if (value < 0) {
        for (int i = 0; i < width; i++) putchar('-');
    } else {
        value = clamp(value, minimum, maximum);
        int position = (value - minimum) * (width - 1) /
                       (maximum - minimum);
        fputs(color, stdout);
        for (int i = 0; i < position; i++) putchar('=');
        fputs("\033[1;37m|\033[0m", stdout);
        for (int i = position + 1; i < width; i++) putchar('-');
    }
    putchar(']');
    if (value >= 0)
        printf(" %d%s\n", value, unit);
    else
        fputs(" N/A\n", stdout);
}

static void draw_ram_bar(int usage, double used_gb, double total_gb)
{
    if (usage >= 0)
        printf("\033[1;92mMEMORY (RAM - %.0fGB)\033[0m\n  +", total_gb);
    else
        printf("\033[1;92mMEMORY (RAM): N/A\033[0m\n  +");
    for (int i = 0; i < BAR_WIDTH; i++) putchar('-');
    fputs("+\n", stdout);

    for (int row = 0; row < 3; row++) {
        printf("  |");
        if (usage < 0) {
            for (int i = 0; i < BAR_WIDTH; i++) putchar(' ');
        } else {
            int filled = usage * BAR_WIDTH / 100;
            fputs("\033[1;32m", stdout);
            for (int i = 0; i < filled; i++) putchar('=');
            if (filled < BAR_WIDTH) {
                fputs("\033[1;37m|\033[1;32m", stdout);
                filled++;
            }
            fputs("\033[0m", stdout);
            for (int i = filled; i < BAR_WIDTH; i++) putchar(' ');
        }
        putchar('|');
        if (row == 1) {
            if (usage >= 0)
                printf(" %d%% (%.1f GB)", usage, used_gb);
            else
                fputs(" N/A", stdout);
        }
        putchar('\n');
    }
    fputs("  +", stdout);
    for (int i = 0; i < BAR_WIDTH; i++) putchar('-');
    fputs("+\n   ", stdout);
    for (int i = 0; i < BAR_WIDTH / 2; i++) {
        fputs("\033[1;33m|\033[0m ", stdout);
    }
    putchar('\n');
}

static void print_core_cell(const struct cpu_core *core, int performance,
                            int turbo_enabled, int line, int slot_width)
{
    int barrel_width = performance ? 4 : 3;
    int barrel_height = 3;
    const char *color = performance ? "\033[1;36m" : "\033[1;32m";
    char text[24] = "";
    unsigned char red_fill[sizeof(text)] = {0};

    if (line == 0) {
        if (core->frequency_mhz > 0.0)
            snprintf(text, sizeof(text), "%.1fGHz", core->frequency_mhz / 1000.0);
        else
            snprintf(text, sizeof(text), "N/A");
    } else if (line == 1 || line == 5) {
        int is_top = line == 1;
        text[0] = is_top ? '/' : '\\';
        memset(text + 1, '-', (size_t)(barrel_width - 2));
        text[barrel_width - 1] = is_top ? '\\' : '/';
        text[barrel_width] = '\0';
    } else if (line >= 2 && line <= 4) {
        int inner_width = barrel_width - 2;
        int total_fill_steps = barrel_height * 3;
        int filled_steps = 0;
        double base_steps = 0.0;
        int above_base = turbo_enabled && core->base_frequency_khz > 0 &&
            core->frequency_mhz * 1000.0 > core->base_frequency_khz;
        if (core->frequency_mhz > 0.0 && core->frequency_cap_khz > 0) {
            double fraction = core->frequency_mhz * 1000.0 /
                              core->frequency_cap_khz;
            if (fraction > 1.0)
                fraction = 1.0;
            filled_steps = (int)lround(fraction * total_fill_steps);
            if (filled_steps < 1)
                filled_steps = 1;
            if (filled_steps > total_fill_steps)
                filled_steps = total_fill_steps;
            base_steps = (double)core->base_frequency_khz /
                         core->frequency_cap_khz * total_fill_steps;
        }
        int body_row = line - 2;
        int row_from_bottom = barrel_height - body_row - 1;
        int row_steps = filled_steps - row_from_bottom * 3;
        int occupied_steps = row_steps >= 3 ? 3 :
                             row_steps > 0 ? row_steps : 0;
        char fill_mark = occupied_steps == 3 ? '#' :
                         occupied_steps == 2 ? '=' :
                         occupied_steps == 1 ? '-' : ' ';
        int mark_is_above_base = occupied_steps > 0 && above_base &&
            row_from_bottom * 3 + occupied_steps > base_steps;
        text[0] = '|';
        for (int x = 0; x < inner_width; x++) {
            text[x + 1] = fill_mark;
            red_fill[x + 1] = (unsigned char)mark_is_above_base;
        }
        text[barrel_width - 1] = '|';
        text[barrel_width] = '\0';
    } else if (line == 6) {
        snprintf(text, sizeof(text), "%c%d", performance ? 'P' : 'E',
                 core->cpu_id);
    }

    int text_width = (int)strlen(text);
    int left_pad = (slot_width - text_width) / 2;
    int right_pad = slot_width - text_width - left_pad;
    for (int i = 0; i < left_pad; i++) putchar(' ');
    if (text[0]) {
        fputs(color, stdout);
        for (int i = 0; i < text_width; i++) {
            if (red_fill[i])
                printf("\033[1;31m%c%s", text[i], color);
            else
                putchar(text[i]);
        }
        fputs("\033[0m", stdout);
    }
    for (int i = 0; i < right_pad; i++) putchar(' ');
}

static int terminal_width(void);

static unsigned int core_type_frequency_cap(const struct cpu_core *cores,
                                            int count, int e_core)
{
    unsigned int cap = 0;
    for (int i = 0; i < count; i++) {
        if (!!cores[i].is_e_core == !!e_core &&
            cores[i].frequency_cap_khz > cap)
            cap = cores[i].frequency_cap_khz;
    }
    return cap;
}

static void draw_cpu_cores(const struct cpu_core *cores, int count,
                           int turbo_enabled)
{
    int order[MAX_CPU_CORES];
    int p_count = 0, e_count = 0, used = 0;
    if (count > MAX_CPU_CORES)
        count = MAX_CPU_CORES;
    for (int i = 0; i < count; i++)
        if (!cores[i].is_e_core) {
            order[used++] = i;
            p_count++;
        }
    for (int i = 0; i < count; i++)
        if (cores[i].is_e_core) {
            order[used++] = i;
            e_count++;
        }

    unsigned int p_cap = core_type_frequency_cap(cores, count, 0);
    unsigned int e_cap = core_type_frequency_cap(cores, count, 1);
    printf("\n\033[1;96mP CORES (%d) MAX: ", p_count);
    if (p_cap > 0)
        printf("%.1fGHz", p_cap / 1000000.0);
    else
        fputs("N/A", stdout);
    printf("\033[0m  \033[1;97m|\033[0m  \033[1;92mE CORES (%d) MAX: ", e_count);
    if (e_cap > 0)
        printf("%.1fGHz\033[0m\n", e_cap / 1000000.0);
    else
        fputs("N/A\033[0m\n", stdout);
    if (used == 0) {
        fputs("  CPU core frequencies unavailable\n", stdout);
        return;
    }

    const int slot_width = 7;
    int per_row = terminal_width() / slot_width;
    if (per_row < 1)
        per_row = 1;

    int offset = 0;
    while (offset < used) {
        int row_count = used - offset;
        if (row_count > per_row)
            row_count = per_row;
        for (int line = 0; line < 7; line++) {
            for (int cell = 0; cell < row_count; cell++) {
                const struct cpu_core *core = &cores[order[offset + cell]];
                print_core_cell(core, !core->is_e_core, turbo_enabled,
                                line, slot_width);
            }
            putchar('\n');
        }
        offset += per_row;
    }
}

static int terminal_width(void)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
        return size.ws_col;
    return 120;
}

static void draw_power_gauge(double watts, int pl1_w, int pl2_w,
                             int top_row, int column)
{
    char heading[64];
    if (watts >= 0.0)
        snprintf(heading, sizeof(heading), "CPU PACKAGE POWER: %5.1f W", watts);
    else
        snprintf(heading, sizeof(heading), "CPU PACKAGE POWER: %5s W", "N/A");
    int gauge_column = column + ((int)strlen(heading) - 9) / 2;

    printf("\033[%d;%dH", top_row, column);
    printf("\033[1;95m%s\033[0m", heading);

    printf("\033[%d;%dH", top_row + 1, column);
    if (pl1_w >= 0 && pl2_w >= 0)
        printf("\033[1;93mPL1: %dW  PL2: %dW\033[0m", pl1_w, pl2_w);
    else
        fputs("\033[1;93mPL1: N/A  PL2: N/A\033[0m", stdout);

    printf("\033[%d;%dH+-------+", top_row + 2, gauge_column);
    int filled_steps = watts >= 0.0 ? (int)(watts / 5.0) : 0;
    if (filled_steps > 23) filled_steps = 23;
    for (int row = 0; row < POWER_HEIGHT; row++) {
        printf("\033[%d;%dH", top_row + 3 + row, gauge_column);
        putchar('|');
        int steps_in_row = filled_steps - (POWER_HEIGHT - row - 1) * 3;
        if (steps_in_row < 0) steps_in_row = 0;
        if (steps_in_row > 3) steps_in_row = 3;
        static const char *const marks[] = {
            "       ", "-------", "=======", "#######"
        };
        if (steps_in_row > 0)
            printf("%s%s\033[0m", POWER_HEIGHT - row - 1 < 3 ?
                   "\033[1;34m" : "\033[1;31m", marks[steps_in_row]);
        else
            fputs("       ", stdout);
        putchar('|');
    }
    printf("\033[%d;%dH+-------+", top_row + POWER_HEIGHT + 3, gauge_column);
}

/* Wrap angles into [-pi, pi] for reliable curved-blade rendering. */
static double wrap_angle(double angle)
{
    angle = fmod(angle, 2.0 * PI_D);
    if (angle > PI_D) angle -= 2.0 * PI_D;
    if (angle < -PI_D) angle += 2.0 * PI_D;
    return angle;
}

/* Render a round fan with a ring, three curved blades, and a center hub. */
static void draw_fan(int top_row, int column, double angle, const char *color)
{
    const int cx = FAN_W / 2, cy = FAN_H / 2;
    for (int y = 0; y < FAN_H; y++) {
        char line[FAN_W + 1];
        for (int x = 0; x < FAN_W; x++) {
            double px = x - cx;
            double py = (y - cy) * 2.0;
            double radius = sqrt(px * px + py * py);
            char ch = ' ';

            if (radius < 0.9) {
                ch = '@';
            } else if (radius <= 3.5) {
                double theta = atan2(py, px);
                for (int blade = 0; blade < 3; blade++) {
                    double direction = angle + blade * (2.0 * PI_D / 3.0) +
                                       radius * 0.30;
                    if (fabs(wrap_angle(theta - direction)) < 0.55) {
                        ch = '#';
                        break;
                    }
                }
            } else if (radius >= 3.6 && radius <= 4.6) {
                ch = '.';
            }
            line[x] = ch;
        }
        line[FAN_W] = '\0';
        printf("\033[%d;%dH%s%s\033[0m", top_row + y, column, color, line);
    }
}

static void draw_fans(double cpu_angle, double gpu_angle, int top_row, int column)
{
    printf("\033[%d;%dH\033[1;95mFANS\033[0m", top_row, column + 2);
    draw_fan(top_row + 1, column, cpu_angle, "\033[1;36m");
    draw_fan(top_row + 1 + FAN_H + 1, column, gpu_angle, "\033[1;32m");
}

/* At 100% duty, complete about 1.5 rotations per second. */
static double fan_animation_angle(int duty_pct, int rpm, int available,
                                  double angle, int direction)
{
    if (!available || duty_pct <= 0 || rpm <= 0)
        return 0.0;

    double rotations_per_second = 1.5 * duty_pct / 100.0;
    double step = rotations_per_second * 2.0 * PI_D * 0.25;
    return fmod(angle + direction * step, 2.0 * PI_D);
}

static void draw_fan_readings(int cpu_pct, int gpu_pct, int cpu_rpm,
                              int gpu_rpm, int available)
{
    char cpu_text[40], gpu_text[40];
    if (available) {
        snprintf(cpu_text, sizeof(cpu_text), "CPU %3d%% | %4d RPM", cpu_pct, cpu_rpm);
        snprintf(gpu_text, sizeof(gpu_text), "GPU %3d%% | %4d RPM", gpu_pct, gpu_rpm);
    } else {
        snprintf(cpu_text, sizeof(cpu_text), "CPU fan: N/A");
        snprintf(gpu_text, sizeof(gpu_text), "GPU fan: N/A");
    }

    int label_column = FAN_COLUMN + (FAN_W - (int)strlen(cpu_text)) / 2;
    printf("\033[%d;%dH\033[1;36m%s\033[0m",
           FAN_HEADING_ROW + FAN_H + 1, label_column, cpu_text);
    label_column = FAN_COLUMN + (FAN_W - (int)strlen(gpu_text)) / 2;
    printf("\033[%d;%dH\033[1;32m%s\033[0m",
           FAN_HEADING_ROW + 2 * FAN_H + 2, label_column, gpu_text);
}

static void draw(int temp, int cpu, int ram, double ram_used_gb,
                 double ram_total_gb,
                 double watts, double cpu_fan_angle, double gpu_fan_angle,
                 int cpu_pct, int gpu_pct,
                 int cpu_rpm, int gpu_rpm, int fans_available,
                 const struct cpu_core *cores, int core_count,
                 int turbo_enabled)
{
    fputs("\033[2J\033[H\033[1;97mCPU MONITOR\033[0m\n\n", stdout);
    draw_bar("CPU TEMPERATURE", temp, 30, 100, CPU_BAR_WIDTH,
             "C", "\033[1;33m");
    putchar('\n');
    draw_bar("CPU USAGE", cpu, 0, 100, CPU_BAR_WIDTH,
             "%", "\033[1;36m");
    putchar('\n');
    int pl1_w, pl2_w;
    read_rapl_limits(&pl1_w, &pl2_w);
    draw_power_gauge(watts, pl1_w, pl2_w, 3, 58);
    draw_fans(cpu_fan_angle, gpu_fan_angle, FAN_HEADING_ROW, FAN_COLUMN);
    draw_fan_readings(cpu_pct, gpu_pct, cpu_rpm, gpu_rpm, fans_available);
    printf("\033[%d;1H", RAM_TOP_ROW);
    draw_ram_bar(ram, ram_used_gb, ram_total_gb);
    draw_cpu_cores(cores, core_count, turbo_enabled);
    fflush(stdout);
}

static int cpumonitor_visual(void)
{
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct monitor_input_state input_state;
    monitor_input_start(&input_state);

    struct cpu_ticks previous = {0};
    (void)read_cpu_ticks(&previous);
    int tuxedo_fd = tuxedo_open_clevo();
    struct cpu_core cpu_cores[MAX_CPU_CORES] = {0};
    int cpu_core_count = discover_cpu_cores(cpu_cores, MAX_CPU_CORES);
    int turbo_enabled = cpu_turbo_enabled();
    refresh_cpu_core_frequencies(cpu_cores, cpu_core_count, turbo_enabled);
    unsigned long long previous_energy = 0;
    struct timespec previous_power_time = {0};
    int have_power_baseline = read_package_energy(&previous_energy) == 0 &&
                              clock_gettime(CLOCK_MONOTONIC, &previous_power_time) == 0;
    fputs("\033[?1049h\033[?25l", stdout);
    fflush(stdout);
    double ram_used_gb, ram_total_gb;
    int ram = read_ram_usage(&ram_used_gb, &ram_total_gb);
    int cpu_fan_pct, gpu_fan_pct, cpu_fan_rpm, gpu_fan_rpm;
    int fans_available = read_fan_telemetry(tuxedo_fd, &cpu_fan_pct,
        &gpu_fan_pct, &cpu_fan_rpm, &gpu_fan_rpm) == 0;
    double cpu_fan_angle = 0.0, gpu_fan_angle = 0.0;
    draw(read_cpu_temperature(), -1, ram, ram_used_gb, ram_total_gb, -1.0,
         cpu_fan_angle, gpu_fan_angle,
         cpu_fan_pct, gpu_fan_pct, cpu_fan_rpm, gpu_fan_rpm, fans_available,
         cpu_cores, cpu_core_count, turbo_enabled);

    while (visual_running && !monitor_input_quit_requested(&input_state)) {
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 250000000L };
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR && visual_running) {}
        if (!visual_running)
            break;
        double watts = have_power_baseline ?
                       sample_package_power(&previous_energy, &previous_power_time) : -1.0;
        if (!have_power_baseline &&
            read_package_energy(&previous_energy) == 0 &&
            clock_gettime(CLOCK_MONOTONIC, &previous_power_time) == 0)
            have_power_baseline = 1;
        ram = read_ram_usage(&ram_used_gb, &ram_total_gb);
        int cpu = cpu_usage_since(&previous);
        turbo_enabled = cpu_turbo_enabled();
        refresh_cpu_core_frequencies(cpu_cores, cpu_core_count, turbo_enabled);
        fans_available = read_fan_telemetry(tuxedo_fd, &cpu_fan_pct,
            &gpu_fan_pct, &cpu_fan_rpm, &gpu_fan_rpm) == 0;
        cpu_fan_angle = fan_animation_angle(cpu_fan_pct, cpu_fan_rpm,
            fans_available, cpu_fan_angle, 1);
        gpu_fan_angle = fan_animation_angle(gpu_fan_pct, gpu_fan_rpm,
            fans_available, gpu_fan_angle, -1);
        draw(read_cpu_temperature(), cpu, ram, ram_used_gb, ram_total_gb, watts,
             cpu_fan_angle, gpu_fan_angle,
             cpu_fan_pct, gpu_fan_pct, cpu_fan_rpm, gpu_fan_rpm, fans_available,
             cpu_cores, cpu_core_count, turbo_enabled);
    }

    if (tuxedo_fd >= 0)
        close(tuxedo_fd);
    monitor_input_stop(&input_state);
    fputs("\033[?25h\033[0m\033[?1049l\n", stdout);
    return 0;
}
