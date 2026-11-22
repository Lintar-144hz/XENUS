/*
 * UCore - platform facts from procfs/sysfs, resource sampling and the real
 * (never simulated) hardware bridge.
 */
#include "core_internal.h"

/* All path buffers are fixed and snprintf-bound; GCC cannot prove that a
 * 255-byte dirent name plus a sysfs prefix fits.  Truncation is harmless
 * here because the name comes from a kernel pseudo-filesystem. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define CORE_CMD_TIMEOUT 450
#define CORE_HW_TTL_MS    4000

static xenus_core_sysinfo_t g_info;
static xenus_core_load_t g_load;
static xenus_core_hardware_t g_hw;
static uint64_t g_hw_at;
static int g_hw_valid;
static unsigned long long g_prev_total;
static unsigned long long g_prev_idle;
static int g_have_prev;
static int g_termux_volume_max = 15;

/* ------------------------------------------------------------- helpers -- */

int core_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}

int core_is_dir(const char *path)
{
    struct stat st;

    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *path_dirs[] = {
    "/data/data/com.termux/files/usr/bin",
    "/system/bin",
    "/usr/local/bin",
    "/usr/bin",
    "/bin",
    NULL
};

static int exec_ok(const char *path)
{
    return path && access(path, X_OK) == 0;
}

int core_cmdline_exists(const char *name)
{
    static char path[XENUS_CORE_PATH];
    const char *env;
    const char *p;
    int i;

    if (!name || !*name)
        return 0;
    if (strchr(name, '/'))
        return exec_ok(name);

    env = getenv("PATH");
    while (env && *env) {
        const char *sep = strchr(env, ':');
        size_t len = sep ? (size_t)(sep - env) : strlen(env);

        if (len > 0 && len < sizeof(path) - strlen(name) - 2) {
            memcpy(path, env, len);
            path[len] = '/';
            snprintf(path + len + 1, sizeof(path) - len - 1, "%s",
                     name);
            if (exec_ok(path))
                return 1;
        }
        if (!sep)
            break;
        env = sep + 1;
    }
    for (i = 0; path_dirs[i]; i++) {
        p = path_dirs[i];
        snprintf(path, sizeof(path), "%s/%s", p, name);
        if (exec_ok(path))
            return 1;
    }
    return 0;
}

static void shell_quote_into(char *dst, size_t size, const char *src)
{
    size_t used = 0;

    if (size == 0)
        return;
    if (used + 1 < size)
        dst[used++] = '\'';
    while (src && *src && used + 5 < size) {
        if (*src == '\'') {
            memcpy(dst + used, "'\\''", 4);
            used += 4;
        } else {
            dst[used++] = *src;
        }
        src++;
    }
    if (used + 1 < size)
        dst[used++] = '\'';
    dst[used] = '\0';
}

/*
 * Run a command through /bin/sh and capture stdout+stderr.  XENUS ignores
 * SIGCHLD, so the child appends its exit code as a marker; we never call
 * waitpid().  A hard timeout kills the whole process group.
 */
int core_run_capture(const char *const argv[], char *out, size_t out_size,
                     int timeout_ms)
{
    char command[1024];
    char quoted[256];
    char *args[5];
    int pipefd[2];
    pid_t pid;
    size_t used = 0;
    uint64_t deadline;
    int status = -1;
    int rc = -3;

    if (!argv || !argv[0] || out_size == 0)
        return -1;
    out[0] = '\0';
    command[0] = '\0';
    {
        size_t pos = 0;
        int i;

        for (i = 0; argv[i] && pos + 2 < sizeof(command); i++) {
            shell_quote_into(quoted, sizeof(quoted), argv[i]);
            pos += (size_t)snprintf(command + pos, sizeof(command) - pos,
                                    "%s%s", i ? " " : "", quoted);
            if (pos >= sizeof(command))
                break;
        }
    }
    {
        size_t pos = strlen(command);
        snprintf(command + pos, sizeof(command) - pos,
                 "\nprintf '\\n__XENUS_RC=%%d\\n' $?");
    }

    if (pipe(pipefd) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        int devnull;

        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        args[0] = "/bin/sh";
        args[1] = "-c";
        args[2] = command;
        args[3] = NULL;
        execve(args[0], args, environ);
        _exit(127);
    }

    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    {
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline = (uint64_t)ts.tv_sec * 1000u +
                   (uint64_t)ts.tv_nsec / 1000000u + (uint64_t)timeout_ms;
    }

    for (;;) {
        struct pollfd pfd;
        struct timespec ts;
        uint64_t now;
        char buf[512];
        ssize_t n;
        int wait_ms;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint64_t)ts.tv_sec * 1000u +
              (uint64_t)ts.tv_nsec / 1000000u;
        if (now >= deadline) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            rc = -2;
            break;
        }
        wait_ms = (int)(deadline - now);
        if (wait_ms > 50)
            wait_ms = 50;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        (void)poll(&pfd, 1, wait_ms);
        n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            size_t copy = (size_t)n;

            if (used + copy >= out_size)
                copy = out_size - used - 1;
            if (copy > 0) {
                memcpy(out + used, buf, copy);
                used += copy;
            }
        } else if (n == 0) {
            rc = 0;
            break;
        } else if (errno != EAGAIN && errno != EINTR) {
            rc = -1;
            break;
        }
    }
    close(pipefd[0]);
    out[used < out_size ? used : out_size - 1] = '\0';

    /* strip the exit-status marker and recover the status */
    {
        char *mark = strstr(out, "__XENUS_RC=");

        if (mark) {
            int code = -1;

            if (sscanf(mark + 10, "%d", &code) == 1)
                status = code;
            while (mark > out && (mark[-1] == '\n' || mark[-1] == '\r'))
                mark--;
            *mark = '\0';
        }
    }
    if (rc == -2)
        return -2;
    return status;
}

int core_run_silent(const char *const argv[], int timeout_ms)
{
    char tmp[256];

    return core_run_capture(argv, tmp, sizeof(tmp), timeout_ms);
}

int core_read_line(const char *path, char *out, size_t size)
{
    FILE *f;
    size_t len;

    if (!path || !out || size == 0)
        return -1;
    out[0] = '\0';
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)size, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    return 0;
}

int core_write_line(const char *path, const char *value)
{
    FILE *f = fopen(path, "w");

    if (!f)
        return -1;
    if (fprintf(f, "%s\n", value) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int read_u64_file(const char *path, unsigned long long *out)
{
    char buf[64];
    char *end;
    unsigned long long v;

    if (core_read_line(path, buf, sizeof(buf)) != 0)
        return -1;
    errno = 0;
    v = strtoull(buf, &end, 10);
    if (errno != 0 || end == buf)
        return -1;
    *out = v;
    return 0;
}

/* ------------------------------------------------------------- sysinfo -- */

static void detect_platform(void)
{
    char release[96] = "";
    struct utsname u;
    const char *env;

    g_info.android = core_exists("/system/build.prop") ||
                     core_exists("/system/bin/app_process64") ||
                     core_exists("/system/bin/app_process32");

    env = getenv("PROOT_TMP_DIR");
    if (uname(&u) == 0)
        snprintf(release, sizeof(release), "%s", u.release);
    g_info.proot = strstr(release, "PRoot") != NULL ||
                   strstr(release, "proot") != NULL ||
                   (env && *env) ||
                   (getenv("PROOT_L2S_DIR") && *getenv("PROOT_L2S_DIR")) ||
                   (getenv("container") &&
                    strstr(getenv("container"), "proot"));

    env = getenv("PREFIX");
    g_info.termux = (env && strstr(env, "com.termux")) ||
                    core_exists("/data/data/com.termux/files/usr/bin") ||
                    (getenv("PATH") &&
                     strstr(getenv("PATH"), "com.termux"));
}

static void read_os_name(void)
{
    FILE *f = fopen("/etc/os-release", "r");
    char line[256];

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
            char *v = line + 12;
            size_t len;

            while (*v == '"' || *v == ' ')
                v++;
            len = strlen(v);
            while (len > 0 &&
                   (v[len - 1] == '\n' || v[len - 1] == '\r' ||
                    v[len - 1] == '"'))
                len--;
            if (len >= sizeof(g_info.os))
                len = sizeof(g_info.os) - 1;
            memcpy(g_info.os, v, len);
            g_info.os[len] = '\0';
            break;
        }
    }
    fclose(f);
}

static void read_cpu_model(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    char fallback[96] = "";

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        char *v;
        size_t len;

        if (!colon)
            continue;
        *colon = '\0';
        v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        len = strlen(v);
        while (len > 0 && (v[len - 1] == '\n' || v[len - 1] == '\r'))
            len--;
        v[len] = '\0';
        if (strncmp(line, "model name", 10) == 0 ||
            strncmp(line, "Hardware", 8) == 0) {
            snprintf(g_info.cpu_model, sizeof(g_info.cpu_model), "%s", v);
            fclose(f);
            return;
        }
        if (strncmp(line, "Processor", 9) == 0 && !fallback[0])
            snprintf(fallback, sizeof(fallback), "%s", v);
    }
    fclose(f);
    if (fallback[0])
        snprintf(g_info.cpu_model, sizeof(g_info.cpu_model), "%s",
                 fallback);
    else
        snprintf(g_info.cpu_model, sizeof(g_info.cpu_model), "%s",
                 g_info.arch);
}

int core_sys_refresh_info(void)
{
    struct utsname u;
    unsigned long long mem_kb = 0;

    memset(&g_info, 0, sizeof(g_info));
    if (uname(&u) == 0) {
        snprintf(g_info.kernel, sizeof(g_info.kernel), "%s", u.release);
        snprintf(g_info.arch, sizeof(g_info.arch), "%s", u.machine);
        snprintf(g_info.hostname, sizeof(g_info.hostname), "%s", u.nodename);
    }
    snprintf(g_info.desktop, sizeof(g_info.desktop), "XENUS");
    read_os_name();
    if (!g_info.os[0])
        snprintf(g_info.os, sizeof(g_info.os), "%s", "Linux");
    g_info.cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_info.cpus < 1)
        g_info.cpus = 1;
    read_cpu_model();
    if (xenus_core_load(&g_load) == 0)
        mem_kb = g_load.mem.total_kb;
    else
        mem_kb = 0;
    if (mem_kb == 0) {
        FILE *f = fopen("/proc/meminfo", "r");
        char line[256];

        if (f) {
            while (fgets(line, sizeof(line), f)) {
                char key[64];
                unsigned long long val;

                if (sscanf(line, "%63s %llu", key, &val) == 2 &&
                    strcmp(key, "MemTotal:") == 0) {
                    mem_kb = val;
                    break;
                }
            }
            fclose(f);
        }
    }
    g_info.mem_total_mb = (unsigned long)(mem_kb / 1024);
    detect_platform();
    g_info.uptime_sec = (unsigned long)xenus_core_uptime();
    return 0;
}

const xenus_core_sysinfo_t *xenus_core_sysinfo(void)
{
    return &g_info;
}

unsigned long long core_cpu_total_ticks(void)
{
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long v[8] = { 0 };
    int i, n;

    if (!f)
        return 0;
    n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4)
        return 0;
    for (i = 1; i < 8; i++)
        v[0] += v[i];
    return v[0];
}

unsigned long long core_cpu_idle_ticks(void)
{
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iow = 0;
    unsigned long long irq = 0, sirq = 0, steal = 0;
    int n;

    if (!f)
        return 0;
    n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &sys, &idle, &iow, &irq, &sirq, &steal);
    fclose(f);
    if (n < 4)
        return 0;
    return idle + iow;
}

static void read_meminfo(xenus_core_meminfo_t *m)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];

    memset(m, 0, sizeof(*m));
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        unsigned long long val;

        if (sscanf(line, "%63s %llu", key, &val) != 2)
            continue;
        if (strcmp(key, "MemTotal:") == 0)
            m->total_kb = val;
        else if (strcmp(key, "MemFree:") == 0)
            m->free_kb = val;
        else if (strcmp(key, "MemAvailable:") == 0)
            m->available_kb = val;
        else if (strcmp(key, "Buffers:") == 0)
            m->buffers_kb = val;
        else if (strcmp(key, "Cached:") == 0)
            m->cached_kb = val;
        else if (strcmp(key, "SwapTotal:") == 0)
            m->swap_total_kb = val;
        else if (strcmp(key, "SwapFree:") == 0)
            m->swap_free_kb = val;
    }
    fclose(f);
}

static unsigned long count_proc_entries(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    unsigned long n = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        const char *p = e->d_name;

        while (*p && *p >= '0' && *p <= '9')
            p++;
        if (*p == '\0' && e->d_name[0])
            n++;
    }
    closedir(d);
    return n;
}

int core_sys_refresh_load(void)
{
    unsigned long long total = core_cpu_total_ticks();
    unsigned long long idle = core_cpu_idle_ticks();
    FILE *f;
    char buf[256];
    double l1 = 0, l5 = 0, l15 = 0;
    unsigned long run = 0, total_proc = 0;

    if (total > g_prev_total && g_have_prev) {
        unsigned long long dt = total - g_prev_total;
        unsigned long long di = idle - g_prev_idle;

        g_load.cpu_percent = dt > 0
            ? (double)(dt - (di <= dt ? di : dt)) * 100.0 / (double)dt
            : 0.0;
    }
    if (total > 0) {
        g_prev_total = total;
        g_prev_idle = idle;
        g_have_prev = 1;
    }

    read_meminfo(&g_load.mem);
    if (g_load.mem.total_kb > 0) {
        unsigned long long avail = g_load.mem.available_kb
            ? g_load.mem.available_kb : g_load.mem.free_kb;
        unsigned long long used = g_load.mem.total_kb > avail
            ? g_load.mem.total_kb - avail : 0;

        g_load.mem_percent = (double)used * 100.0 /
                             (double)g_load.mem.total_kb;
    }

    f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f) &&
            sscanf(buf, "%lf %lf %lf %lu/%lu", &l1, &l5, &l15, &run,
                   &total_proc) >= 3) {
            g_load.load1 = l1;
            g_load.load5 = l5;
            g_load.load15 = l15;
            if (total_proc > 0)
                g_load.processes = count_proc_entries();
            else
                g_load.processes = count_proc_entries();
        }
        fclose(f);
    }
    if (g_load.processes == 0)
        g_load.processes = count_proc_entries();
    f = fopen("/proc/uptime", "r");
    if (f) {
        double up = 0;

        if (fscanf(f, "%lf", &up) == 1 && up >= 0)
            g_load.uptime_sec = (unsigned long)up;
        fclose(f);
    }
    g_info.uptime_sec = g_load.uptime_sec;
    return 0;
}

int xenus_core_load(xenus_core_load_t *out)
{
    if (!out)
        return -1;
    core_sys_refresh_load();
    *out = g_load;
    return 0;
}

int xenus_core_uptime(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0;

    if (!f)
        return 0;
    if (fscanf(f, "%lf", &up) != 1)
        up = 0;
    fclose(f);
    return (int)up;
}

/* ------------------------------------------------------------- hardware -- */

static void probe_wifi(xenus_core_hardware_t *hw)
{
    DIR *d;
    struct dirent *e;
    int found = 0;

    hw->wifi = XENUS_HW_UNAVAILABLE;
    hw->wifi_iface[0] = '\0';
    hw->wifi_ssid[0] = '\0';
    hw->wifi_controllable = 0;

    d = opendir("/sys/class/net");
    if (d) {
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "wlan", 4) == 0 ||
                strncmp(e->d_name, "wlp", 3) == 0 ||
                strncmp(e->d_name, "wifi", 4) == 0) {
                char path[XENUS_CORE_PATH];
                char state[32] = "";

                found = 1;
                snprintf(hw->wifi_iface, sizeof(hw->wifi_iface), "%s",
                         e->d_name);
                snprintf(path, sizeof(path),
                         "/sys/class/net/%s/operstate", e->d_name);
                core_read_line(path, state, sizeof(state));
                hw->wifi_on = strcmp(state, "up") == 0;
                break;
            }
        }
        closedir(d);
    }
    if (found) {
        hw->wifi = XENUS_HW_AVAILABLE;
    } else if (core_cmdline_exists("termux-wifi-connectioninfo")) {
        char out[512];
        const char *argv[] = { "termux-wifi-connectioninfo", NULL };

        if (core_run_capture(argv, out, sizeof(out), CORE_CMD_TIMEOUT) == 0) {
            const char *ssid = strstr(out, "\"ssid\"");

            hw->wifi = XENUS_HW_AVAILABLE;
            hw->wifi_on = strstr(out, "\"supplicant_state\":\"COMPLETED\"") !=
                          NULL || strstr(out, "\"state\":\"CONNECTED\"") !=
                          NULL;
            if (ssid) {
                const char *q = strchr(ssid, ':');
                if (q) {
                    q = strchr(q, '"');
                    if (q) {
                        const char *end = strchr(q + 1, '"');
                        if (end && (size_t)(end - q - 1) <
                            sizeof(hw->wifi_ssid)) {
                            memcpy(hw->wifi_ssid, q + 1,
                                   (size_t)(end - q - 1));
                            hw->wifi_ssid[end - q - 1] = '\0';
                        }
                    }
                }
            }
        }
    }

    hw->wifi_controllable = core_cmdline_exists("termux-wifi-enable") ||
                            core_cmdline_exists("/system/bin/cmd") ||
                            core_cmdline_exists("nmcli") ||
                            core_cmdline_exists("rfkill");
    if (hw->wifi == XENUS_HW_UNAVAILABLE && !hw->wifi_controllable)
        snprintf(hw->detail, sizeof(hw->detail),
                 "Wi-Fi interface not exposed to this container");
}

static void probe_bluetooth(xenus_core_hardware_t *hw)
{
    DIR *d;
    struct dirent *e;
    int found = 0;

    hw->bluetooth = XENUS_HW_UNAVAILABLE;
    hw->bt_name[0] = '\0';
    hw->bluetooth_controllable = 0;

    d = opendir("/sys/class/rfkill");
    if (d) {
        while ((e = readdir(d)) != NULL) {
            char path[XENUS_CORE_PATH];
            char type[32] = "";
            char soft[8] = "", hard[8] = "";

            if (e->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "/sys/class/rfkill/%s/type",
                     e->d_name);
            core_read_line(path, type, sizeof(type));
            if (strcmp(type, "bluetooth") != 0)
                continue;
            found = 1;
            snprintf(path, sizeof(path), "/sys/class/rfkill/%s/soft",
                     e->d_name);
            core_read_line(path, soft, sizeof(soft));
            snprintf(path, sizeof(path), "/sys/class/rfkill/%s/hard",
                     e->d_name);
            core_read_line(path, hard, sizeof(hard));
            hw->bluetooth_on = strcmp(soft, "0") == 0 &&
                               strcmp(hard, "0") == 0;
            break;
        }
        closedir(d);
    }
    if (!found && core_is_dir("/sys/class/bluetooth")) {
        found = 1;
        hw->bluetooth_on = 1;
    }
    if (!found && core_cmdline_exists("/system/bin/cmd")) {
        char out[256];
        const char *argv[] = { "/system/bin/cmd", "bluetooth_manager",
                               "get-state", NULL };

        if (core_run_capture(argv, out, sizeof(out), CORE_CMD_TIMEOUT) == 0) {
            if (strstr(out, "enabled") || strstr(out, "ON"))
                hw->bluetooth_on = 1;
            if (strstr(out, "enabled") || strstr(out, "disabled") ||
                strstr(out, "ON") || strstr(out, "OFF"))
                found = 1;
        }
    }
    if (found)
        hw->bluetooth = XENUS_HW_AVAILABLE;
    if (core_is_dir("/sys/class/bluetooth")) {
        char path[XENUS_CORE_PATH];

        snprintf(path, sizeof(path), "/sys/class/bluetooth/hci0/name");
        core_read_line(path, hw->bt_name, sizeof(hw->bt_name));
    }
    hw->bluetooth_controllable = core_cmdline_exists("rfkill") ||
                                 core_cmdline_exists("/system/bin/cmd") ||
                                 core_cmdline_exists("bluetoothctl");
}

static void probe_airplane(xenus_core_hardware_t *hw)
{
    hw->airplane = XENUS_HW_UNAVAILABLE;
    hw->airplane_on = 0;
    hw->airplane_controllable = 0;

    if (!core_cmdline_exists("/system/bin/cmd"))
        return;
    {
        char out[256];
        const char *argv[] = { "/system/bin/cmd", "connectivity",
                               "airplane-mode", NULL };
        int rc = core_run_capture(argv, out, sizeof(out), CORE_CMD_TIMEOUT);

        if (rc == 0) {
            hw->airplane = XENUS_HW_AVAILABLE;
            hw->airplane_controllable = 1;
            if (strstr(out, "enabled") && !strstr(out, "disabled"))
                hw->airplane_on = 1;
            else if (strstr(out, "on") && !strstr(out, "off"))
                hw->airplane_on = 1;
        }
    }
}

static void probe_volume(xenus_core_hardware_t *hw)
{
    hw->volume = XENUS_HW_UNAVAILABLE;
    hw->volume_controllable = 0;
    hw->volume_percent = 0;
    hw->muted = 0;

    if (core_cmdline_exists("termux-volume")) {
        char out[768];
        const char *argv[] = { "termux-volume", NULL };

        if (core_run_capture(argv, out, sizeof(out), CORE_CMD_TIMEOUT) == 0) {
            const char *p = strstr(out, "\"stream\":\"music\"");

            hw->volume = XENUS_HW_AVAILABLE;
            hw->volume_controllable = 1;
            if (p) {
                const char *v = strstr(p, "\"volume\":");
                const char *m = strstr(p, "\"max_volume\":");

                if (m && sscanf(m + 13, "%d", &g_termux_volume_max) != 1)
                    g_termux_volume_max = 15;
                if (g_termux_volume_max < 1)
                    g_termux_volume_max = 1;
                if (v && sscanf(v + 9, "%d", &hw->volume_percent) == 1)
                    hw->volume_percent = hw->volume_percent * 100 /
                                         g_termux_volume_max;
                hw->muted = hw->volume_percent == 0;
            }
            return;
        }
    }
    if (core_cmdline_exists("pactl")) {
        char out[512];
        const char *getvol[] = { "pactl", "get-sink-volume",
                                 "@DEFAULT_SINK@", NULL };
        const char *getmute[] = { "pactl", "get-sink-mute",
                                  "@DEFAULT_SINK@", NULL };
        int pct = -1;

        if (core_run_capture(getvol, out, sizeof(out), CORE_CMD_TIMEOUT) == 0) {
            const char *p = strstr(out, " / ");
            if (p && sscanf(p + 3, "%d%%", &pct) == 1) {
                hw->volume = XENUS_HW_AVAILABLE;
                hw->volume_controllable = 1;
                hw->volume_percent = pct;
            }
        }
        if (hw->volume == XENUS_HW_AVAILABLE &&
            core_run_capture(getmute, out, sizeof(out),
                             CORE_CMD_TIMEOUT) == 0) {
            hw->muted = strstr(out, "Mute: yes") != NULL;
        }
        if (hw->volume == XENUS_HW_AVAILABLE)
            return;
    }
    if (core_cmdline_exists("amixer")) {
        char out[512];
        const char *argv[] = { "amixer", "get", "Master", NULL };
        int pct = -1;

        if (core_run_capture(argv, out, sizeof(out), CORE_CMD_TIMEOUT) == 0) {
            const char *p = strstr(out, "[");
            while (p) {
                int v;
                if (sscanf(p, "[%d%%]", &v) == 1) {
                    pct = v;
                    break;
                }
                p = strstr(p + 1, "[");
            }
            hw->volume = XENUS_HW_AVAILABLE;
            hw->volume_controllable = 1;
            hw->volume_percent = pct < 0 ? 0 : pct;
            hw->muted = strstr(out, "[off]") != NULL;
        }
    }
}

static void probe_brightness(xenus_core_hardware_t *hw)
{
    DIR *d;
    struct dirent *e;

    hw->brightness = XENUS_HW_UNAVAILABLE;
    hw->brightness_percent = 0;
    hw->brightness_writable = 0;

    d = opendir("/sys/class/backlight");
    if (!d)
        d = opendir("/sys/class/leds");
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char path[XENUS_CORE_PATH];
        unsigned long long cur = 0, max = 0;

        if (e->d_name[0] == '.')
            continue;
        if (strncmp(e->d_name, "lcd-backlight", 13) != 0 &&
            strncmp(e->d_name, "backlight", 9) != 0 &&
            !strstr(e->d_name, "backlight"))
            continue;
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness",
                 e->d_name);
        if (read_u64_file(path, &cur) != 0) {
            snprintf(path, sizeof(path),
                     "/sys/class/leds/%s/brightness", e->d_name);
            if (read_u64_file(path, &cur) != 0)
                continue;
        }
        snprintf(path, sizeof(path),
                 "/sys/class/backlight/%s/max_brightness", e->d_name);
        if (read_u64_file(path, &max) != 0) {
            snprintf(path, sizeof(path),
                     "/sys/class/leds/%s/max_brightness", e->d_name);
            if (read_u64_file(path, &max) != 0 || max == 0)
                max = 255;
        }
        if (max > 0) {
            hw->brightness = XENUS_HW_AVAILABLE;
            hw->brightness_percent = (int)(cur * 100 / max);
            snprintf(path, sizeof(path),
                 "/sys/class/backlight/%s/brightness", e->d_name);
        hw->brightness_writable = access(path, W_OK) == 0;
            break;
        }
    }
    closedir(d);
}

static void probe_battery(xenus_core_hardware_t *hw)
{
    DIR *d = opendir("/sys/class/power_supply");

    hw->battery = XENUS_HW_UNAVAILABLE;
    hw->battery_percent = 0;
    hw->battery_status[0] = '\0';
    if (!d)
        return;
    {
        struct dirent *e;

        while ((e = readdir(d)) != NULL) {
            char path[XENUS_CORE_PATH];
            char type[32] = "";
            char cap[32] = "";
            char status[32] = "";
            int pct = -1;

            if (e->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path),
                     "/sys/class/power_supply/%s/type", e->d_name);
            core_read_line(path, type, sizeof(type));
            if (strcmp(type, "Battery") != 0 &&
                strcmp(type, "battery") != 0)
                continue;
            snprintf(path, sizeof(path),
                     "/sys/class/power_supply/%s/capacity", e->d_name);
            core_read_line(path, cap, sizeof(cap));
            if (sscanf(cap, "%d", &pct) != 1)
                continue;
            snprintf(path, sizeof(path),
                     "/sys/class/power_supply/%s/status", e->d_name);
            core_read_line(path, status, sizeof(status));
            hw->battery = XENUS_HW_AVAILABLE;
            hw->battery_percent = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
            snprintf(hw->battery_status, sizeof(hw->battery_status), "%s",
                     status);
            break;
        }
    }
    closedir(d);
}

xenus_core_status_t xenus_core_hardware(xenus_core_hardware_t *out, int refresh)
{
    struct timespec ts;
    uint64_t now;

    if (!out)
        return XENUS_CORE_ERR_ARG;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;

    if (!g_hw_valid || refresh || now - g_hw_at > CORE_HW_TTL_MS) {
        memset(&g_hw, 0, sizeof(g_hw));
        g_hw.detail[0] = '\0';
        probe_wifi(&g_hw);
        probe_bluetooth(&g_hw);
        probe_airplane(&g_hw);
        probe_volume(&g_hw);
        probe_brightness(&g_hw);
        probe_battery(&g_hw);
        if (g_hw.airplane == XENUS_HW_UNAVAILABLE)
            snprintf(g_hw.detail, sizeof(g_hw.detail),
                     "Android airplane control is not available here");
        g_hw_at = now;
        g_hw_valid = 1;
    }
    *out = g_hw;
    return XENUS_CORE_OK;
}

static void hw_touch(void)
{
    g_hw_at = 0;
    g_hw_valid = 0;
}

xenus_core_status_t xenus_core_wifi_set(int on)
{
    int rc = -1;

    if (core_cmdline_exists("termux-wifi-enable")) {
        const char *argv[] = { "termux-wifi-enable", on ? "true" : "false",
                               NULL };
        rc = core_run_silent(argv, 1200);
    }
    if (rc != 0 && core_cmdline_exists("/system/bin/cmd")) {
        const char *argv[] = { "/system/bin/cmd", "wifi",
                               "set-wifi-enabled",
                               on ? "enabled" : "disabled", NULL };
        rc = core_run_silent(argv, 1200);
    }
    if (rc != 0 && core_cmdline_exists("nmcli")) {
        const char *argv[] = { "nmcli", "radio", "wifi",
                               on ? "on" : "off", NULL };
        rc = core_run_silent(argv, 1200);
    }
    if (rc != 0 && core_cmdline_exists("rfkill")) {
        const char *argv[] = { "rfkill", on ? "unblock" : "block", "wifi",
                               NULL };
        rc = core_run_silent(argv, 1200);
    }
    hw_touch();
    if (rc == 0) {
        xenus_core_event_emit(XENUS_CORE_EV_HARDWARE, "wifi",
                             on ? "Wi-Fi enabled" : "Wi-Fi disabled");
        return XENUS_CORE_OK;
    }
    return rc == -2 ? XENUS_CORE_ERR : XENUS_CORE_ERR_DENIED;
}

xenus_core_status_t xenus_core_bluetooth_set(int on)
{
    int rc = -1;

    if (core_cmdline_exists("/system/bin/cmd")) {
        const char *argv[] = { "/system/bin/cmd", "bluetooth_manager",
                               on ? "enable" : "disable", NULL };
        rc = core_run_silent(argv, 1500);
    }
    if (rc != 0 && core_cmdline_exists("rfkill")) {
        const char *argv[] = { "rfkill", on ? "unblock" : "block",
                               "bluetooth", NULL };
        rc = core_run_silent(argv, 1200);
    }
    if (rc != 0 && core_cmdline_exists("bluetoothctl")) {
        const char *argv[] = { "bluetoothctl", "power", on ? "on" : "off",
                               NULL };
        rc = core_run_silent(argv, 1500);
    }
    hw_touch();
    if (rc == 0) {
        xenus_core_event_emit(XENUS_CORE_EV_HARDWARE, "bluetooth",
                             on ? "Bluetooth enabled" :
                                  "Bluetooth disabled");
        return XENUS_CORE_OK;
    }
    return rc == -2 ? XENUS_CORE_ERR : XENUS_CORE_ERR_DENIED;
}

xenus_core_status_t xenus_core_airplane_set(int on)
{
    const char *argv[] = { "/system/bin/cmd", "connectivity",
                           "airplane-mode", on ? "enable" : "disable", NULL };
    int rc;

    if (!core_cmdline_exists("/system/bin/cmd"))
        return XENUS_CORE_ERR_UNAVAILABLE;
    rc = core_run_silent(argv, 1200);
    hw_touch();
    if (rc == 0) {
        xenus_core_event_emit(XENUS_CORE_EV_HARDWARE, "airplane",
                             on ? "Airplane mode on" :
                                  "Airplane mode off");
        return XENUS_CORE_OK;
    }
    return rc == -2 ? XENUS_CORE_ERR : XENUS_CORE_ERR_DENIED;
}

xenus_core_status_t xenus_core_volume_set(int percent)
{
    char value[16];
    int rc = -1;

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    snprintf(value, sizeof(value), "%d", percent);

    if (core_cmdline_exists("termux-volume")) {
        char lvl[16];
        int level = percent * g_termux_volume_max / 100;

        snprintf(lvl, sizeof(lvl), "%d", level);
        {
            const char *argv[] = { "termux-volume", "music", lvl, NULL };
            rc = core_run_silent(argv, 1000);
        }
    }
    if (rc != 0 && core_cmdline_exists("pactl")) {
        char arg[32];
        const char *argv[5];

        snprintf(arg, sizeof(arg), "%d%%", percent);
        argv[0] = "pactl";
        argv[1] = "set-sink-volume";
        argv[2] = "@DEFAULT_SINK@";
        argv[3] = arg;
        argv[4] = NULL;
        rc = core_run_silent(argv, 1000);
    }
    if (rc != 0 && core_cmdline_exists("amixer")) {
        char arg[16];
        const char *argv[] = { "amixer", "set", "Master", arg, NULL };

        snprintf(arg, sizeof(arg), "%d%%", percent);
        rc = core_run_silent(argv, 1000);
    }
    hw_touch();
    if (rc == 0)
        return XENUS_CORE_OK;
    return rc == -2 ? XENUS_CORE_ERR : XENUS_CORE_ERR_DENIED;
}

xenus_core_status_t xenus_core_volume_mute(int on)
{
    int rc = -1;

    if (core_cmdline_exists("pactl")) {
        const char *argv[] = { "pactl", "set-sink-mute", "@DEFAULT_SINK@",
                               on ? "1" : "0", NULL };
        rc = core_run_silent(argv, 1000);
    }
    if (rc != 0 && core_cmdline_exists("amixer")) {
        const char *argv[] = { "amixer", "set", "Master",
                               on ? "mute" : "unmute", NULL };
        rc = core_run_silent(argv, 1000);
    }
    if (rc != 0 && core_cmdline_exists("termux-volume")) {
        const char *argv[] = { "termux-volume", "music",
                               on ? "0" : "8", NULL };
        rc = core_run_silent(argv, 1000);
    }
    hw_touch();
    if (rc == 0)
        return XENUS_CORE_OK;
    return rc == -2 ? XENUS_CORE_ERR : XENUS_CORE_ERR_DENIED;
}

xenus_core_status_t xenus_core_brightness_set(int percent)
{
    DIR *d;
    struct dirent *e;
    int wrote = 0;

    if (percent < 1)
        percent = 1;
    if (percent > 100)
        percent = 100;
    d = opendir("/sys/class/backlight");
    if (!d)
        return XENUS_CORE_ERR_UNAVAILABLE;
    while ((e = readdir(d)) != NULL) {
        char path[XENUS_CORE_PATH];
        char value[32];
        unsigned long long max = 255;

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness",
                 e->d_name);
        (void)read_u64_file(path, &max);
        if (max == 0)
            max = 255;
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness",
                 e->d_name);
        snprintf(value, sizeof(value), "%llu",
                 (unsigned long long)percent * max / 100);
        if (core_write_line(path, value) != 0)
            continue;
        wrote = 1;
        break;
    }
    closedir(d);
    hw_touch();
    if (wrote)
        return XENUS_CORE_OK;
    return XENUS_CORE_ERR_DENIED;
}
