/*
 * UCore - application manager.
 *
 * Scans the freedesktop .desktop files already present in the container and
 * correlates them with the live process table.  No package manager and no
 * external metadata service are involved.
 */
#include "core_internal.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define APP_DIRS 12

static xenus_core_app_t g_apps[XENUS_CORE_MAX_APPS];
static int g_app_count;
static int g_apps_loaded;

static void trim(char *s)
{
    size_t len;
    char *start = s;

    while (*start == ' ' || *start == '\t')
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = '\0';
}

static void strip_exec_codes(char *s)
{
    char *r = s;
    char *w = s;

    while (*r) {
        if (*r == '%' && r[1]) {
            r += 2;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
    trim(s);
}

static int app_exists(const char *exec)
{
    int i;

    for (i = 0; i < g_app_count; i++)
        if (strcmp(g_apps[i].exec, exec) == 0)
            return 1;
    return 0;
}

static void parse_desktop(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    char name[XENUS_CORE_NAME] = "";
    char generic[XENUS_CORE_NAME] = "";
    char exec[192] = "";
    char icon[XENUS_CORE_NAME] = "";
    char categories[96] = "";
    int terminal = 0;
    int no_display = 0;
    int hidden = 0;
    int in_entry = 0;
    int type_ok = 0;
    xenus_core_app_t *a;

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *eq;

        trim(line);
        if (line[0] == '[') {
            in_entry = strcmp(line, "[Desktop Entry]") == 0;
            continue;
        }
        if (!in_entry || line[0] == '#' || line[0] == '\0')
            continue;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        {
            char *key = line;
            char *val = eq + 1;

            trim(key);
            trim(val);
            if (strcmp(key, "Name") == 0)
                snprintf(name, sizeof(name), "%s", val);
            else if (strcmp(key, "GenericName") == 0)
                snprintf(generic, sizeof(generic), "%s", val);
            else if (strcmp(key, "Exec") == 0)
                snprintf(exec, sizeof(exec), "%s", val);
            else if (strcmp(key, "Icon") == 0)
                snprintf(icon, sizeof(icon), "%s", val);
            else if (strcmp(key, "Categories") == 0)
                snprintf(categories, sizeof(categories), "%s", val);
            else if (strcmp(key, "Terminal") == 0)
                terminal = strcmp(val, "true") == 0;
            else if (strcmp(key, "NoDisplay") == 0)
                no_display = strcmp(val, "true") == 0;
            else if (strcmp(key, "Hidden") == 0)
                hidden = strcmp(val, "true") == 0;
            else if (strcmp(key, "Type") == 0)
                type_ok = strcmp(val, "Application") == 0;
        }
    }
    fclose(f);

    if (no_display || hidden || !name[0] || !exec[0])
        return;
    if (!type_ok)
        return;
    strip_exec_codes(exec);
    if (!exec[0] || g_app_count >= XENUS_CORE_MAX_APPS)
        return;
    if (app_exists(exec))
        return;

    a = &g_apps[g_app_count];
    memset(a, 0, sizeof(*a));
    snprintf(a->name, sizeof(a->name), "%s", name);
    snprintf(a->generic_name, sizeof(a->generic_name), "%s", generic);
    snprintf(a->exec, sizeof(a->exec), "%s", exec);
    snprintf(a->icon, sizeof(a->icon), "%s", icon);
    snprintf(a->categories, sizeof(a->categories), "%s", categories);
    a->terminal = terminal;
    a->pid = -1;
    g_app_count++;
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char path[1024];
        size_t len = strlen(e->d_name);

        if (e->d_name[0] == '.' || len < 8 ||
            strcmp(e->d_name + len - 8, ".desktop") != 0)
            continue;
        if (g_app_count >= XENUS_CORE_MAX_APPS)
            break;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        parse_desktop(path);
    }
    closedir(d);
}

static int app_cmp(const void *a, const void *b)
{
    const xenus_core_app_t *x = a;
    const xenus_core_app_t *y = b;

    return strcmp(x->name, y->name);
}

static const char *base_name(const char *path, char *out, size_t size)
{
    const char *p = strrchr(path, '/');
    size_t len;

    p = p ? p + 1 : path;
    len = strlen(p);
    if (len >= size)
        len = size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

int core_apps_load(void)
{
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_DATA_DIRS");
    const char *prefix = getenv("PREFIX");
    char dirs[APP_DIRS][XENUS_CORE_PATH];
    int nd = 0;
    int i;

    g_app_count = 0;
    memset(g_apps, 0, sizeof(g_apps));

    if (home && *home) {
        snprintf(dirs[nd], sizeof(dirs[nd]),
                 "%s/.local/share/applications", home);
        nd++;
    }
    dirs[nd][0] = '\0';
    snprintf(dirs[nd], sizeof(dirs[nd]), "/usr/local/share/applications");
    nd++;
    if (prefix && *prefix && nd < APP_DIRS) {
        snprintf(dirs[nd], sizeof(dirs[nd]), "%s/share/applications",
                 prefix);
        nd++;
    }
    if (xdg && *xdg) {
        char *copy = strdup(xdg);
        char *save = NULL;
        char *tok;

        if (copy) {
            tok = strtok_r(copy, ":", &save);
            while (tok && nd < APP_DIRS) {
                snprintf(dirs[nd], sizeof(dirs[nd]), "%s/applications",
                         tok);
                nd++;
                tok = strtok_r(NULL, ":", &save);
            }
            free(copy);
        }
    }
    if (nd < APP_DIRS) {
        snprintf(dirs[nd], sizeof(dirs[nd]), "/usr/share/applications");
        nd++;
    }

    for (i = 0; i < nd; i++) {
        if (dirs[i][0])
            scan_dir(dirs[i]);
    }

    qsort(g_apps, (size_t)g_app_count, sizeof(g_apps[0]), app_cmp);

    /* correlate with the live process table */
    {
        xenus_core_proc_t procs[XENUS_CORE_MAX_PROC];
        int n = core_proc_snapshot(procs, XENUS_CORE_MAX_PROC);
        int j;

        for (i = 0; i < g_app_count; i++) {
            char want[XENUS_CORE_NAME];
            char first[192];
            char *sp;

            snprintf(first, sizeof(first), "%s", g_apps[i].exec);
            sp = strchr(first, ' ');
            if (sp)
                *sp = '\0';
            base_name(first, want, sizeof(want));
            for (j = 0; j < n; j++) {
                if (strcmp(procs[j].name, want) == 0 ||
                    (procs[j].cmdline[0] &&
                     strstr(procs[j].cmdline, want))) {
                    g_apps[i].running = 1;
                    g_apps[i].pid = procs[j].pid;
                    break;
                }
            }
        }
    }
    g_apps_loaded = 1;
    return g_app_count;
}

int xenus_core_apps_refresh(void)
{
    return core_apps_load();
}

int xenus_core_apps(xenus_core_app_t *out, int max)
{
    int i;

    if (!out || max <= 0)
        return 0;
    if (!g_apps_loaded)
        core_apps_load();
    for (i = 0; i < g_app_count && i < max; i++)
        out[i] = g_apps[i];
    return i;
}

static int spawn_detached(const char *cmd)
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);

        setsid();
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    return 0;
}

xenus_core_status_t xenus_core_app_launch(int index)
{
    char text[220];

    if (index < 0 || index >= g_app_count)
        return XENUS_CORE_ERR_ARG;
    if (spawn_detached(g_apps[index].exec) != 0)
        return XENUS_CORE_ERR;
    snprintf(text, sizeof(text), "launched %s", g_apps[index].name);
    xenus_core_log(1, "app: %s", text);
    xenus_core_event_emit(XENUS_CORE_EV_PROCESS, "applications", text);
    return XENUS_CORE_OK;
}
