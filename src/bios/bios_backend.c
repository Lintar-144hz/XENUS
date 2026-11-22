/*
 * XENUS BIOS backend - real facts from the host, no fabricated state.
 */
#define _POSIX_C_SOURCE 200809L

#include "bios_backend.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

/* Kernel strings are copied into fixed, bounded BIOS fields. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

static void copy_str(char *dst, size_t size, const char *src)
{
    if (!size)
        return;
    if (!src)
        src = "";
    snprintf(dst, size, "%s", src);
}

static int is_dir(const char *path)
{
    struct stat st;

    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int file_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}

static int exec_exists(const char *path)
{
    return path && access(path, X_OK) == 0;
}

int xenus_bios_command_exists(const char *name)
{
    char path[512];
    const char *env;
    const char *sep;
    size_t len;

    if (!name || !*name)
        return 0;
    if (strchr(name, '/'))
        return exec_exists(name);
    env = getenv("PATH");
    while (env && *env) {
        sep = strchr(env, ':');
        len = sep ? (size_t)(sep - env) : strlen(env);
        if (len > 0 && len + strlen(name) + 2 < sizeof(path)) {
            memcpy(path, env, len);
            path[len] = '/';
            snprintf(path + len + 1, sizeof(path) - len - 1, "%s", name);
            if (exec_exists(path))
                return 1;
        }
        if (!sep)
            break;
        env = sep + 1;
    }
    {
        static const char *dirs[] = {
            "/data/data/com.termux/files/usr/bin",
            "/system/bin", "/usr/local/bin", "/usr/bin", "/bin", NULL
        };
        int i;

        for (i = 0; dirs[i]; i++) {
            snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
            if (exec_exists(path))
                return 1;
        }
    }
    return 0;
}

static unsigned long read_ulong(const char *path, unsigned long fallback)
{
    char buf[64];
    char *end;
    unsigned long v;
    FILE *f;

    f = fopen(path, "r");
    if (!f)
        return fallback;
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return fallback;
    }
    fclose(f);
    errno = 0;
    v = strtoul(buf, &end, 10);
    if (errno != 0 || end == buf)
        return fallback;
    return v;
}

static void read_meminfo(xenus_bios_env_t *env)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[160];

    env->mem_total_mb = 0;
    env->mem_available_mb = 0;
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long kb = 0;

        if (sscanf(line, "MemTotal: %lu kB", &kb) == 1)
            env->mem_total_mb = kb / 1024;
        else if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1)
            env->mem_available_mb = kb / 1024;
        else if (sscanf(line, "MemFree: %lu kB", &kb) == 1 &&
                 env->mem_available_mb == 0)
            env->mem_available_mb = kb / 1024;
    }
    fclose(f);
}

static void read_load(xenus_bios_env_t *env)
{
    FILE *f = fopen("/proc/loadavg", "r");

    env->load1 = env->load5 = env->load15 = 0.0;
    if (!f)
        return;
    if (fscanf(f, "%lf %lf %lf", &env->load1, &env->load5,
               &env->load15) != 3)
        env->load1 = env->load5 = env->load15 = 0.0;
    fclose(f);
}

static void read_cpu_model(char *out, size_t size)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];

    copy_str(out, size, "");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *v;

        if (strncmp(line, "model name", 10) != 0 &&
            strncmp(line, "Hardware", 8) != 0 &&
            strncmp(line, "Processor", 9) != 0)
            continue;
        v = strchr(line, ':');
        if (!v)
            continue;
        v++;
        while (*v == ' ' || *v == '\t')
            v++;
        v[strcspn(v, "\r\n")] = '\0';
        if (*v) {
            copy_str(out, size, v);
            break;
        }
    }
    fclose(f);
}

static void read_os_name(char *out, size_t size)
{
    FILE *f = fopen("/etc/os-release", "r");
    char line[256];

    copy_str(out, size, "Linux");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
            char *v = line + 12;
            size_t len;

            while (*v == '"' || *v == ' ' || *v == '\t')
                v++;
            len = strlen(v);
            while (len > 0 && (v[len - 1] == '\n' || v[len - 1] == '\r' ||
                               v[len - 1] == '"'))
                len--;
            if (len) {
                if (len >= size)
                    len = size - 1;
                memcpy(out, v, len);
                out[len] = '\0';
            }
            break;
        }
    }
    fclose(f);
}

static void detect_network(xenus_bios_env_t *env)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *ent;

    env->network_available = 0;
    env->network_iface[0] = '\0';
    if (d) {
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.' ||
                strcmp(ent->d_name, "lo") == 0)
                continue;
            copy_str(env->network_iface, sizeof(env->network_iface),
                     ent->d_name);
            env->network_available = 1;
            break;
        }
        closedir(d);
    }
    if (!env->network_available &&
        xenus_bios_command_exists("termux-wifi-connectioninfo")) {
        env->network_available = 1;
        copy_str(env->network_iface, sizeof(env->network_iface),
                 "termux-wifi");
    }
}

static void detect_audio(xenus_bios_env_t *env)
{
    env->audio_available = 0;
    env->audio_backend[0] = '\0';
    if (xenus_bios_command_exists("pactl")) {
        copy_str(env->audio_backend, sizeof(env->audio_backend), "pactl");
        env->audio_available = 1;
    } else if (xenus_bios_command_exists("amixer")) {
        copy_str(env->audio_backend, sizeof(env->audio_backend), "amixer");
        env->audio_available = 1;
    } else if (xenus_bios_command_exists("termux-volume")) {
        copy_str(env->audio_backend, sizeof(env->audio_backend),
                 "termux-volume");
        env->audio_available = 1;
    }
}

static void detect_termux(xenus_bios_env_t *env)
{
    const char *prefix = getenv("PREFIX");
    const char *termux_env = getenv("TERMUX_VERSION");
    const char *path = getenv("PATH");
    int has_prefix = is_dir("/data/data/com.termux/files/usr/bin");

    env->termux = (termux_env && *termux_env) ||
                  (prefix && strstr(prefix, "com.termux")) ||
                  has_prefix || (path && strstr(path, "com.termux"));
    env->android = file_exists("/system/build.prop") ||
                   file_exists("/system/bin/app_process64") ||
                   file_exists("/system/bin/app_process32");
    if (prefix && strstr(prefix, "com.termux"))
        copy_str(env->termux_prefix, sizeof(env->termux_prefix), prefix);
    else if (has_prefix)
        copy_str(env->termux_prefix, sizeof(env->termux_prefix),
                 "/data/data/com.termux/files/usr");

    env->termux_api_available =
        xenus_bios_command_exists("termux-toast") ||
        xenus_bios_command_exists("termux-notification") ||
        xenus_bios_command_exists("termux-clipboard-set");
    env->notifications_available =
        xenus_bios_command_exists("termux-notification");
    env->clipboard_available =
        xenus_bios_command_exists("termux-clipboard-set") ||
        xenus_bios_command_exists("termux-clipboard-get");
    env->storage_available =
        is_dir("/data/data/com.termux/files/home/storage/shared") ||
        xenus_bios_command_exists("termux-storage-get");
    env->android_cmd_available = exec_exists("/system/bin/cmd");

    if (env->android)
        copy_str(env->android_detail, sizeof(env->android_detail),
                 "Android userspace detected");
    else
        copy_str(env->android_detail, sizeof(env->android_detail),
                 "Android namespace not exposed");
    if (env->termux)
        copy_str(env->termux_detail, sizeof(env->termux_detail),
                 env->termux_api_available ? "Termux API detected" :
                 "Termux detected");
    else
        copy_str(env->termux_detail, sizeof(env->termux_detail),
                 "Termux not detected");
}

static void detect_x11(xenus_bios_env_t *env)
{
    const char *display = getenv("DISPLAY");

    env->x11 = display && *display;
    copy_str(env->display, sizeof(env->display), display ? display : "");
    env->termux_x11_available =
        xenus_bios_command_exists("termux-x11") ||
        (env->x11 && display && strncmp(display, ":", 1) == 0);
}

static void detect_xenus(xenus_bios_env_t *env)
{
    env->xenus_binary_available = xenus_bios_command_exists("xenus") ||
                                  file_exists("./xenus") ||
                                  file_exists("/usr/local/bin/xenus");
}

int xenus_bios_env_probe(xenus_bios_env_t *env)
{
    struct utsname u;
    const char *proot;

    if (!env)
        return -1;
    memset(env, 0, sizeof(*env));
    if (uname(&u) == 0) {
        copy_str(env->kernel, sizeof(env->kernel), u.release);
        copy_str(env->arch, sizeof(env->arch), u.machine);
        copy_str(env->hostname, sizeof(env->hostname), u.nodename);
    } else {
        copy_str(env->kernel, sizeof(env->kernel), "unknown");
        copy_str(env->arch, sizeof(env->arch), "unknown");
        copy_str(env->hostname, sizeof(env->hostname), "localhost");
    }
    read_os_name(env->os, sizeof(env->os));
    read_cpu_model(env->cpu_model, sizeof(env->cpu_model));
    env->cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (env->cpus < 1)
        env->cpus = 1;
    read_meminfo(env);
    read_load(env);
    env->uptime_sec = read_ulong("/proc/uptime", 0);
    proot = getenv("PROOT_TMP_DIR");
    env->proot = (proot && *proot) ||
                 strstr(env->kernel, "PRoot") != NULL ||
                 strstr(env->kernel, "proot") != NULL ||
                 (getenv("PROOT_L2S_DIR") && *getenv("PROOT_L2S_DIR")) ||
                 (getenv("container") &&
                  strstr(getenv("container"), "proot"));
    detect_termux(env);
    detect_x11(env);
    detect_audio(env);
    detect_network(env);
    detect_xenus(env);
    return 0;
}
