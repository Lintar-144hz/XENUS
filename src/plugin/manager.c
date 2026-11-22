#define _XOPEN_SOURCE 700
/*
 * XENUS plugin manager - discover -> load -> validate -> init -> running ->
 * shutdown -> unload, with failures isolated to the plugin that caused them.
 */
#include <xenus/event.h>
#include <xenus/plugin.h>
#include <xenus/security.h>

#include "loader.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PLUGINS 32
#define PATH_LEN 512

/* subject name used for capability accounting of a loaded plugin */
static void plugin_subject(const char *name, char *out, size_t out_len)
{
    static const char prefix[] = "plugin:";
    const char *use = (name && *name) ? name : "unnamed";
    size_t room;
    size_t n;

    if (out_len == 0)
        return;
    if (out_len <= sizeof(prefix)) {
        out[0] = '\0';
        return;
    }
    memcpy(out, prefix, sizeof(prefix) - 1);
    room = out_len - sizeof(prefix);
    n = strlen(use);
    if (n > room)
        n = room;
    memcpy(out + sizeof(prefix) - 1, use, n);
    out[sizeof(prefix) - 1 + n] = '\0';
}

typedef struct {
    int in_use;
    int state;                        /* xenus_plugin_state_t */
    char path[PATH_LEN];
    char error[192];
    xenus_loaded_plugin_t loaded;
} plugin_entry_t;

static plugin_entry_t table[MAX_PLUGINS];

static void drop_caps(plugin_entry_t *e)
{
    char subject[80];

    if (!e->loaded.name[0])
        return;
    plugin_subject(e->loaded.name, subject, sizeof(subject));
    if (xenus_security_revoke(subject, 0xffffffffu) == XENUS_OK)
        xenus_audit_log(XENUS_AUDIT_CAPABILITY_DENIED, subject, 0,
                       "capabilities revoked on unload");
}
static int mgr_initialized;
static char search_path[768];
static char default_path_buf[768];
/* scratch space for discovery (qsort wants a contiguous array) */
static char found_files[MAX_PLUGINS][PATH_LEN];

static void post_plugin_event(int type, int index, const char *text)
{
    xenus_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.id = (uint64_t)(unsigned)index;
    snprintf(ev.text, sizeof(ev.text), "%.*s", (int)(sizeof(ev.text) - 1),
             text ? text : "");
    xenus_event_post(&ev);
}

/* ------------------------------------------------------------ lifecycle -- */

int xenus_plugins_init(const char *path)
{
    const char *env;

    if (mgr_initialized)
        return XENUS_ERR_STATE;

    search_path[0] = '\0';
    if (path && *path) {
        snprintf(search_path, sizeof(search_path), "%s", path);
    } else {
        env = getenv("XENUS_PLUGIN_PATH");
        if (env && *env) {
            snprintf(search_path, sizeof(search_path), "%s", env);
        } else {
            const char *home = getenv("HOME");

            default_path_buf[0] = '\0';
            if (home && *home)
                snprintf(default_path_buf, sizeof(default_path_buf),
                         "%s/.local/share/xenus/plugins:"
                         "/usr/local/lib/xenus/plugins:"
                         "/usr/lib/xenus/plugins", home);
            else
                snprintf(default_path_buf, sizeof(default_path_buf),
                         "/usr/local/lib/xenus/plugins:"
                         "/usr/lib/xenus/plugins");
            snprintf(search_path, sizeof(search_path), "%s", default_path_buf);
        }
    }

    memset(table, 0, sizeof(table));
    mgr_initialized = 1;
    xenus_logf(XENUS_LOG_DEBUG, "plugins: search path %s", search_path);
    return XENUS_OK;
}

int xenus_plugins_unload_all(void)
{
    int i, n = 0;

    if (!mgr_initialized)
        return XENUS_ERR_STATE;
    for (i = 0; i < MAX_PLUGINS; i++) {
        if (!table[i].in_use)
            continue;
        if (table[i].state == XENUS_PLUGIN_RUNNING ||
            table[i].state == XENUS_PLUGIN_LOADED) {
            drop_caps(&table[i]);
            if (table[i].state == XENUS_PLUGIN_RUNNING)
                xenus_loader_stop(&table[i].loaded);
            else
                xenus_loader_close(&table[i].loaded);
            table[i].state = XENUS_PLUGIN_UNLOADED;
            n++;
        }
    }
    if (n > 0)
        xenus_logf(XENUS_LOG_INFO, "plugins: unloaded %d", n);
    return XENUS_OK;
}

void xenus_plugins_shutdown(void)
{
    if (!mgr_initialized)
        return;
    xenus_plugins_unload_all();
    memset(table, 0, sizeof(table));
    search_path[0] = '\0';
    mgr_initialized = 0;
    xenus_log(XENUS_LOG_DEBUG, "plugins: manager shut down");
}

/* ------------------------------------------------------------ discovery -- */

static int cmp_file(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int path_is_listed(const char *path, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (strcmp(found_files[i], path) == 0)
            return 1;
    }
    return 0;
}

int xenus_plugins_discover(void)
{
    const char *p;
    int n = 0;
    int i;

    if (!mgr_initialized)
        return XENUS_ERR_STATE;

    for (i = 0; i < MAX_PLUGINS; i++) {
        if (table[i].in_use &&
            (table[i].state == XENUS_PLUGIN_LOADED ||
             table[i].state == XENUS_PLUGIN_RUNNING))
            return XENUS_ERR_STATE;         /* never drop a live plugin */
    }

    memset(found_files, 0, sizeof(found_files));
    p = search_path;
    while (*p && n < MAX_PLUGINS) {
        const char *sep = strchr(p, ':');
        size_t dlen = sep ? (size_t)(sep - p) : strlen(p);
        char dir[PATH_LEN];
        DIR *d;
        struct dirent *ent;

        if (dlen > 0 && dlen < sizeof(dir)) {
            memcpy(dir, p, dlen);
            dir[dlen] = '\0';
            d = opendir(dir);
            if (d) {
                while ((ent = readdir(d)) != NULL && n < MAX_PLUGINS) {
                    size_t len = strlen(ent->d_name);
                    char full[PATH_LEN];
                    int w;

                    if (len < 4 || strcmp(ent->d_name + len - 3, ".so") != 0)
                        continue;
                    w = snprintf(full, sizeof(full), "%s/", dir);
                    if (w <= 0 || (size_t)w >= sizeof(full))
                        continue;
                    snprintf(full + w, sizeof(full) - (size_t)w, "%.*s",
                             (int)(sizeof(full) - (size_t)w - 1),
                             ent->d_name);
                    if (path_is_listed(full, n))
                        continue;
                    snprintf(found_files[n], PATH_LEN, "%.*s",
                             PATH_LEN - 1, full);
                    n++;
                }
                closedir(d);
            }
        }
        if (!sep)
            break;
        p = sep + 1;
    }

    if (n > 1)
        qsort(found_files, (size_t)n, PATH_LEN, cmp_file);

    memset(table, 0, sizeof(table));
    for (i = 0; i < n; i++) {
        table[i].in_use = 1;
        table[i].state = XENUS_PLUGIN_DISCOVERED;
        snprintf(table[i].path, PATH_LEN, "%.*s", PATH_LEN - 1,
                 found_files[i]);
        table[i].error[0] = '\0';
    }
    xenus_logf(XENUS_LOG_INFO, "plugins: discovered %d", n);
    return XENUS_OK;
}

/* ---------------------------------------------------------------- load -- */

static int find_path(const char *path, int *free_slot)
{
    int i;

    if (free_slot)
        *free_slot = -1;
    for (i = 0; i < MAX_PLUGINS; i++) {
        if (!table[i].in_use) {
            if (free_slot && *free_slot < 0)
                *free_slot = i;
            continue;
        }
        if (strcmp(table[i].path, path) == 0)
            return i;
    }
    return -1;
}

static void fail_entry(plugin_entry_t *e, int index, int status,
                       const char *err, const char *fallback)
{
    e->state = XENUS_PLUGIN_FAILED;
    snprintf(e->error, sizeof(e->error), "%s",
             (err && *err) ? err : fallback);
    xenus_logf(XENUS_LOG_ERROR, "plugins: %s: %s",
              e->loaded.name[0] ? e->loaded.name : e->path, e->error);
    post_plugin_event(XENUS_EVENT_PLUGIN_FAILED, index,
                      e->loaded.name[0] ? e->loaded.name : e->path);
    (void)status;
}

int xenus_plugins_load(const char *path)
{
    char err[192];
    char subject[80];
    plugin_entry_t *e;
    int idx, slot, st;
    int trust;
    char real[PATH_MAX];
    const char *gpath;

    if (!mgr_initialized)
        return XENUS_ERR_STATE;
    if (!path || !*path)
        return XENUS_ERR_ARG;
    if (access(path, R_OK) != 0)
        return XENUS_ERR_NOT_FOUND;

    gpath = realpath(path, real) != NULL ? real : path;
    idx = find_path(path, &slot);
    if (idx < 0) {
        idx = slot;
        if (idx < 0)
            return XENUS_ERR_FULL;
        table[idx].in_use = 1;
        table[idx].state = XENUS_PLUGIN_DISCOVERED;
        snprintf(table[idx].path, PATH_LEN, "%.*s", PATH_LEN - 1, path);
        table[idx].error[0] = '\0';
    }
    e = &table[idx];
    if (e->state == XENUS_PLUGIN_LOADED || e->state == XENUS_PLUGIN_RUNNING)
        return XENUS_ERR_STATE;

    { /* security gate: path safety, scan, trust, policy */
        xenus_scan_result_t sres;
        char msg[192];

        if (xenus_security_check_plugin(gpath, &sres) != XENUS_OK) {
            snprintf(msg, sizeof(msg), "security: %s",
                     sres.detail[0] ? sres.detail : "plugin rejected");
            fail_entry(e, idx, XENUS_ERR, msg, "rejected by security");
            return XENUS_ERR;
        }
    }

    err[0] = '\0';
    /* open the resolved path the gate actually validated, not the
       (possibly symlinked) caller-supplied path: otherwise the link could
       be swapped between the security check and dlopen */
    st = xenus_loader_open(gpath, &e->loaded, err, sizeof(err));
    if (st != XENUS_OK) {
        fail_entry(e, idx, st, err, "invalid plugin");
        return st;
    }

    e->state = XENUS_PLUGIN_LOADED;
    e->error[0] = '\0';

    /*
     * Capability setup, before init runs: TRUSTED plugins get the declared
     * minimal default set (fs.read + config + audit); everything else -
     * reachable only with the explicit untrusted-override policy - gets
     * NOTHING.  Loading never grants spawn/net/write/device/plugin.load.
     */
    plugin_subject(e->loaded.name, subject, sizeof(subject));
    trust = xenus_trust_get(gpath);
    if (trust == XENUS_TRUST_TRUSTED) {
        if (xenus_security_grant(subject, XENUS_CAP_PLUGIN_DEFAULT) == XENUS_OK)
            xenus_audit_log(XENUS_AUDIT_CAPABILITY_GRANTED, subject, 0,
                           "plugin defaults");
    } else {
        xenus_audit_log(XENUS_AUDIT_CAPABILITY_DENIED, subject, 0,
                       "untrusted plugin: no capabilities");
    }

    /* sandbox/process decision: in-process code cannot be isolated here;
     * the trust gate above already made this TRUSTED-ONLY (or the admin
     * took explicit responsibility via policy) */

    st = xenus_loader_start(&e->loaded, err, sizeof(err));
    if (st != XENUS_OK) {
        /* never call shutdown() on a plugin that failed to start */
        xenus_loader_close(&e->loaded);
        xenus_security_revoke(subject, 0xffffffffu);  /* no caps leak */
        fail_entry(e, idx, st, err, "init failed");
        return st;
    }

    e->state = XENUS_PLUGIN_RUNNING;
    xenus_logf(XENUS_LOG_INFO, "plugins: %s %s running", e->loaded.name,
              e->loaded.version);
    post_plugin_event(XENUS_EVENT_PLUGIN_LOADED, idx, e->loaded.name);
    return XENUS_OK;
}

int xenus_plugins_load_all(void)
{
    int i, ok = 0, bad = 0;

    if (!mgr_initialized)
        return XENUS_ERR_STATE;
    for (i = 0; i < MAX_PLUGINS; i++) {
        if (!table[i].in_use || table[i].state != XENUS_PLUGIN_DISCOVERED)
            continue;
        if (xenus_plugins_load(table[i].path) == XENUS_OK)
            ok++;
        else
            bad++;
    }
    xenus_logf(XENUS_LOG_INFO, "plugins: %d started, %d failed", ok, bad);
    return XENUS_OK;
}

int xenus_plugins_unload(const char *name)
{
    int i;

    if (!mgr_initialized)
        return XENUS_ERR_STATE;
    if (!name || !*name)
        return XENUS_ERR_ARG;
    for (i = 0; i < MAX_PLUGINS; i++) {
        if (!table[i].in_use || !table[i].loaded.name[0])
            continue;
        if (strcmp(table[i].loaded.name, name) != 0)
            continue;
        if (table[i].state == XENUS_PLUGIN_RUNNING ||
            table[i].state == XENUS_PLUGIN_LOADED) {
            drop_caps(&table[i]);
            if (table[i].state == XENUS_PLUGIN_RUNNING) {
                xenus_loader_stop(&table[i].loaded);
                xenus_logf(XENUS_LOG_INFO, "plugins: %s unloaded", name);
            } else {
                xenus_loader_close(&table[i].loaded);
            }
            table[i].state = XENUS_PLUGIN_UNLOADED;
            return XENUS_OK;
        }
        return XENUS_ERR_STATE;
    }
    return XENUS_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------- inspect -- */

int xenus_plugins_count(void)
{
    int i, n = 0;

    for (i = 0; i < MAX_PLUGINS; i++) {
        if (table[i].in_use)
            n++;
    }
    return n;
}

int xenus_plugins_running(void)
{
    int i, n = 0;

    for (i = 0; i < MAX_PLUGINS; i++) {
        if (table[i].in_use && table[i].state == XENUS_PLUGIN_RUNNING)
            n++;
    }
    return n;
}

const xenus_plugin_info_t *xenus_plugins_info(int index)
{
    if (index < 0 || index >= MAX_PLUGINS || !table[index].in_use)
        return NULL;
    if (!table[index].loaded.name[0])
        return NULL;
    return &table[index].loaded.view;
}

int xenus_plugins_state(int index)
{
    if (index < 0 || index >= MAX_PLUGINS || !table[index].in_use)
        return 0;
    return table[index].state;
}

const char *xenus_plugins_error(int index)
{
    if (index < 0 || index >= MAX_PLUGINS || !table[index].in_use)
        return "";
    return table[index].error;
}

const char *xenus_plugins_file(int index)
{
    if (index < 0 || index >= MAX_PLUGINS || !table[index].in_use)
        return "";
    return table[index].path;
}

const char *xenus_plugins_search_path(void)
{
    return search_path;
}

const char *xenus_plugin_state_name(int state)
{
    switch (state) {
    case XENUS_PLUGIN_DISCOVERED:
        return "discovered";
    case XENUS_PLUGIN_LOADED:
        return "loaded";
    case XENUS_PLUGIN_RUNNING:
        return "running";
    case XENUS_PLUGIN_FAILED:
        return "failed";
    case XENUS_PLUGIN_UNLOADED:
        return "unloaded";
    default:
        return "unknown";
    }
}
