/*
 * UCore - lifecycle manager, module registry, shared logging and event bus.
 */
#include "core_internal.h"

#include <xenus/bios.h>
#include <xenus/event.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    xenus_core_module_t pub;
    int (*init)(void);
    void (*shutdown)(void);
    int used;
} module_slot_t;

typedef struct {
    int used;
    int type;
    xenus_core_event_cb cb;
    void *user;
} event_sub_t;

static struct {
    xenus_core_state_t state;
    uint64_t started_ms;
    uint64_t last_tick_ms;
    char config_path[XENUS_CORE_PATH];
    module_slot_t modules[XENUS_CORE_MAX_MODULES];
    int n_modules;
    xenus_core_log_entry_t logs[XENUS_CORE_LOG_RING];
    int log_head;
    int log_len;
    uint64_t log_seq;
    xenus_core_event_t events[XENUS_CORE_EVENT_RING];
    int ev_head;
    int ev_len;
    uint64_t ev_seq;
    event_sub_t subs[XENUS_CORE_EVENT_SUBS];
    int dnd;
} core;

/* ------------------------------------------------------------- helpers -- */

static uint64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

const char *xenus_core_version(void)
{
    return "1.0";
}

const char *xenus_core_status_str(xenus_core_status_t status)
{
    switch (status) {
    case XENUS_CORE_OK: return "ok";
    case XENUS_CORE_ERR: return "error";
    case XENUS_CORE_ERR_ARG: return "bad argument";
    case XENUS_CORE_ERR_STATE: return "invalid state";
    case XENUS_CORE_ERR_NOMEM: return "out of memory";
    case XENUS_CORE_ERR_IO: return "i/o error";
    case XENUS_CORE_ERR_UNAVAILABLE: return "unavailable";
    case XENUS_CORE_ERR_DENIED: return "permission denied";
    case XENUS_CORE_ERR_FULL: return "table full";
    }
    return "unknown";
}

xenus_core_state_t xenus_core_state(void)
{
    return core.state;
}

uint64_t xenus_core_started_ms(void)
{
    return core.started_ms;
}

const char *xenus_core_config_path(void)
{
    return core.config_path;
}

/* ------------------------------------------------------------- logging -- */

void xenus_core_log(int level, const char *fmt, ...)
{
    xenus_core_log_entry_t *e;
    va_list ap;
    int slot;

    if (level < 0)
        level = 0;
    if (level > 3)
        level = 3;
    if (core.log_len == XENUS_CORE_LOG_RING) {
        slot = core.log_head;
        core.log_head = (core.log_head + 1) % XENUS_CORE_LOG_RING;
    } else {
        slot = (core.log_head + core.log_len) % XENUS_CORE_LOG_RING;
        core.log_len++;
    }
    e = &core.logs[slot];
    memset(e, 0, sizeof(*e));
    e->seq = ++core.log_seq;
    e->time_ms = (uint32_t)now_ms();
    e->level = level;
    snprintf(e->source, sizeof(e->source), "ucore");
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt ? fmt : "", ap);
    va_end(ap);
}

int xenus_core_log_entries(xenus_core_log_entry_t *out, int max)
{
    int i;

    if (!out || max <= 0)
        return 0;
    for (i = 0; i < core.log_len && i < max; i++) {
        int idx = (core.log_head + core.log_len - 1 - i) %
                  XENUS_CORE_LOG_RING;
        out[i] = core.logs[idx];
    }
    return i;
}

void xenus_core_log_clear(void)
{
    memset(core.logs, 0, sizeof(core.logs));
    core.log_head = 0;
    core.log_len = 0;
}

/* --------------------------------------------------------------- events -- */

int xenus_core_event_subscribe(int type, xenus_core_event_cb cb, void *user)
{
    int i;

    if (!cb || type < 0)
        return XENUS_CORE_ERR_ARG;
    for (i = 0; i < XENUS_CORE_EVENT_SUBS; i++) {
        if (!core.subs[i].used) {
            core.subs[i].used = 1;
            core.subs[i].type = type;
            core.subs[i].cb = cb;
            core.subs[i].user = user;
            return XENUS_CORE_OK;
        }
    }
    return XENUS_CORE_ERR_FULL;
}

int xenus_core_event_unsubscribe(xenus_core_event_cb cb, void *user)
{
    int i;

    if (!cb)
        return XENUS_CORE_ERR_ARG;
    for (i = 0; i < XENUS_CORE_EVENT_SUBS; i++) {
        if (core.subs[i].used && core.subs[i].cb == cb &&
            core.subs[i].user == user) {
            memset(&core.subs[i], 0, sizeof(core.subs[i]));
            return XENUS_CORE_OK;
        }
    }
    return XENUS_CORE_ERR_UNAVAILABLE;
}

int xenus_core_event_emit(int type, const char *source, const char *text)
{
    xenus_core_event_t ev;
    xenus_event_t api;
    int slot, i, callback_count = 0;
    xenus_core_event_cb callbacks[XENUS_CORE_EVENT_SUBS];
    void *callback_users[XENUS_CORE_EVENT_SUBS];

    if (type <= 0)
        return XENUS_CORE_ERR_ARG;
    memset(&ev, 0, sizeof(ev));
    ev.seq = ++core.ev_seq;
    ev.time_ms = (uint32_t)now_ms();
    ev.type = type;
    snprintf(ev.source, sizeof(ev.source), "%s",
             source ? source : "ucore");
    snprintf(ev.text, sizeof(ev.text), "%s", text ? text : "");

    if (core.ev_len == XENUS_CORE_EVENT_RING) {
        core.ev_head = (core.ev_head + 1) % XENUS_CORE_EVENT_RING;
        core.ev_len--;
    }
    slot = (core.ev_head + core.ev_len) % XENUS_CORE_EVENT_RING;
    core.events[slot] = ev;
    core.ev_len++;

    /* Snapshot subscribers before dispatch to avoid mutation during iteration. */
    for (i = 0; i < XENUS_CORE_EVENT_SUBS; i++) {
        if (core.subs[i].used &&
            (core.subs[i].type == 0 || core.subs[i].type == type)) {
            callbacks[callback_count] = core.subs[i].cb;
            callback_users[callback_count] = core.subs[i].user;
            callback_count++;
        }
    }

    /* Changes to subscriptions during callbacks apply to the next event. */
    for (i = 0; i < callback_count; i++) {
        if (callbacks[i])
            callbacks[i](&ev, callback_users[i]);
    }

    /* bridge to the public App API event bus (IPC for applications) */
    memset(&api, 0, sizeof(api));
    api.type = XENUS_EVENT_CUSTOM + 100 + type;
    api.time_ms = ev.time_ms;
    api.id = ev.seq;
    {
        size_t cap = sizeof(api.text);
        size_t used = 0;
        size_t n;

        n = strnlen(ev.source, cap - 1);
        memcpy(api.text, ev.source, n);
        used = n;

        if (ev.text[0] && used < cap - 1) {
            api.text[used++] = ':';

            if (used < cap - 1)
                api.text[used++] = ' ';

            n = strnlen(ev.text, cap - used - 1);
            memcpy(api.text + used, ev.text, n);
            used += n;
        }

        api.text[used] = '\0';
    }
    xenus_event_post(&api);
    return XENUS_CORE_OK;
}

int xenus_core_event_poll(xenus_core_event_t *out)
{
    if (!out || core.ev_len == 0)
        return 0;
    *out = core.events[core.ev_head];
    core.ev_head = (core.ev_head + 1) % XENUS_CORE_EVENT_RING;
    core.ev_len--;
    return 1;
}

void xenus_core_event_pump(void)
{
    /* Subscribers are called synchronously in emit(); polling consumers use
     * xenus_core_event_poll().  This keeps the runtime allocation-free. */
}

/* -------------------------------------------------------------- modules -- */


static module_slot_t *module_find(const char *name)
{
    int i;

    if (!name || !*name)
        return NULL;

    for (i = 0; i < XENUS_CORE_MAX_MODULES; i++) {
        if (core.modules[i].used &&
            strcmp(core.modules[i].pub.name, name) == 0)
            return &core.modules[i];
    }
    return NULL;
}

static int module_add(const char *name, const char *version,
                      const char *description, int (*init)(void),
                      void (*shutdown)(void))
{
    module_slot_t *m = NULL;
    int i;
    int rc = 0;

    if (!name || !*name ||
        strlen(name) >= sizeof(m->pub.name))
        return XENUS_CORE_ERR_ARG;

    if (module_find(name))
        return XENUS_CORE_ERR_STATE;

    for (i = 0; i < XENUS_CORE_MAX_MODULES; i++) {
        if (!core.modules[i].used) {
            m = &core.modules[i];
            break;
        }
    }

    if (!m)
        return XENUS_CORE_ERR_FULL;

    memset(m, 0, sizeof(*m));

    m->used = 1;
    m->init = init;
    m->shutdown = shutdown;

    snprintf(m->pub.name, sizeof(m->pub.name), "%s", name);
    snprintf(m->pub.version, sizeof(m->pub.version), "%s",
             version ? version : "1.0");
    snprintf(m->pub.description, sizeof(m->pub.description), "%s",
             description ? description : "");

    core.n_modules++;

    if (init) {
        m->pub.calls++;
        rc = init();

        if (rc != 0) {
            xenus_core_log(3, "module %s init failed (%d)", name, rc);
            return XENUS_CORE_ERR;
        }
    }

    m->pub.active = 1;
    m->pub.started_ms = now_ms();

    return XENUS_CORE_OK;
}

xenus_core_status_t xenus_core_module_register(const char *name,
                                             const char *version,
                                             const char *description,
                                             int (*init)(void),
                                             void (*shutdown)(void))
{
    int rc;

    if (core.state != XENUS_CORE_RUNNING &&
        core.state != XENUS_CORE_STARTING)
        return XENUS_CORE_ERR_STATE;

    if (!name || !*name)
        return XENUS_CORE_ERR_ARG;

    if (module_find(name))
        return XENUS_CORE_ERR_STATE;

    rc = module_add(name, version, description, init, shutdown);

    if (rc == XENUS_CORE_OK) {
        xenus_core_event_emit(XENUS_CORE_EV_MODULE, name,
                             "module registered and active");
    } else if (module_find(name)) {
        xenus_core_event_emit(XENUS_CORE_EV_MODULE, name,
                             "module registered but inactive");
    }

    return (xenus_core_status_t)rc;
}

xenus_core_status_t xenus_core_module_start(const char *name)
{
    module_slot_t *m;
    int rc;

    if (core.state != XENUS_CORE_RUNNING &&
        core.state != XENUS_CORE_STARTING)
        return XENUS_CORE_ERR_STATE;

    if (!name || !*name)
        return XENUS_CORE_ERR_ARG;

    m = module_find(name);

    if (!m)
        return XENUS_CORE_ERR_UNAVAILABLE;

    if (m->pub.active)
        return XENUS_CORE_OK;

    if (m->init) {
        m->pub.calls++;
        rc = m->init();

        if (rc != 0) {
            xenus_core_log(3, "module %s start failed (%d)",
                          name, rc);
            return XENUS_CORE_ERR;
        }
    }

    m->pub.active = 1;
    m->pub.started_ms = now_ms();

    xenus_core_event_emit(XENUS_CORE_EV_MODULE, name, "module started");

    return XENUS_CORE_OK;
}

xenus_core_status_t xenus_core_module_stop(const char *name)
{
    module_slot_t *m;

    if (core.state != XENUS_CORE_RUNNING &&
        core.state != XENUS_CORE_STARTING)
        return XENUS_CORE_ERR_STATE;

    if (!name || !*name)
        return XENUS_CORE_ERR_ARG;

    m = module_find(name);

    if (!m)
        return XENUS_CORE_ERR_UNAVAILABLE;

    if (!m->pub.active)
        return XENUS_CORE_OK;

    /* Mark inactive before invoking the callback to prevent re-entry. */
    m->pub.active = 0;
    m->pub.started_ms = 0;

    if (m->shutdown)
        m->shutdown();

    xenus_core_event_emit(XENUS_CORE_EV_MODULE, name, "module stopped");

    return XENUS_CORE_OK;
}

int xenus_core_modules(xenus_core_module_t *out, int max)
{
    int i, n = 0;

    if (!out || max <= 0)
        return 0;

    for (i = 0; i < XENUS_CORE_MAX_MODULES && n < max; i++) {
        if (core.modules[i].used)
            out[n++] = core.modules[i].pub;
    }

    return n;
}

/* -------------------------------------------------------------- services -- */

static void add_service(xenus_core_service_t *out, int max, int *n,
                        const char *name, const char *status,
                        const char *detail, int pid, int managed)
{
    xenus_core_service_t *s;

    if (*n >= max)
        return;
    s = &out[(*n)++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->status, sizeof(s->status), "%s", status);
    snprintf(s->detail, sizeof(s->detail), "%s", detail ? detail : "");
    s->pid = pid;
    s->managed = managed;
}

int xenus_core_services(xenus_core_service_t *out, int max)
{
    int n = 0;
    int i;

    if (!out || max <= 0)
        return 0;
    add_service(out, max, &n, "ucore", "running",
                "lifecycle + IPC + modules", (int)getpid(), 1);
    for (i = 0; i < XENUS_CORE_MAX_MODULES; i++) {
        module_slot_t *m = &core.modules[i];

        if (!m->used)
            continue;
        add_service(out, max, &n, m->pub.name,
                    m->pub.active ? "running" : "stopped",
                    m->pub.description[0] ? m->pub.description :
                    "UCore module", (int)getpid(), 1);
    }
    add_service(out, max, &n, "x11",
                getenv("DISPLAY") ? "running" : "stopped",
                getenv("DISPLAY") ? getenv("DISPLAY") : "no DISPLAY",
                0, 0);
    add_service(out, max, &n, "systemd",
                core_is_dir("/run/systemd/system") ? "running" :
                "not present",
                "/run/systemd/system", 0, 0);
    add_service(out, max, &n, "dbus",
                getenv("DBUS_SESSION_BUS_ADDRESS") ? "running" :
                "not present", "session bus", 0, 0);
    add_service(out, max, &n, "audio",
                core_cmdline_exists("pactl") ||
                core_cmdline_exists("amixer") ||
                core_cmdline_exists("termux-volume") ? "available" :
                "not present", "volume control backend", 0, 0);
    add_service(out, max, &n, "android bridge",
                core_cmdline_exists("/system/bin/cmd") ? "available" :
                "not present", "/system/bin/cmd", 0, 0);
    add_service(out, max, &n, "termux api",
                core_is_dir("/data/data/com.termux/files/usr/bin") ?
                "available" : "not present",
                "/data/data/com.termux/files/usr/bin", 0, 0);
    return n;
}

/* ------------------------------------------------------- built-in modules -- */

static int mod_info_init(void)
{
    return core_sys_refresh_info() == 0 ? 0 : -1;
}

static int mod_process_init(void)
{
    xenus_core_proc_t tmp[16];

    (void)core_proc_snapshot(tmp, 16);
    return 0;
}

static int mod_apps_init(void)
{
    /* lazy: the app list can be large and is only paid for when viewed */
    return 0;
}

static int mod_hardware_init(void)
{
    return 0;
}

static int mod_notify_init(void)
{
    return 0;
}

/* -------------------------------------------------------------- lifecycle -- */

xenus_core_status_t xenus_core_init(const char *config_path)
{
    if (core.state != XENUS_CORE_DOWN)
        return XENUS_CORE_ERR_STATE;
    memset(&core, 0, sizeof(core));
    core.state = XENUS_CORE_STARTING;
    core.started_ms = now_ms();
    core.last_tick_ms = 0;
    if (config_path)
        snprintf(core.config_path, sizeof(core.config_path), "%s",
                 config_path);

    core_sys_refresh_info();
    core_sys_refresh_load();

    /* BIOS module switches are explicit user choices.  sysinfo is mandatory;
       when no BIOS store exists every helper returns the default (enabled). */
    module_add("sysinfo", "1.0", "procfs/sysfs device facts", mod_info_init,
               NULL);
    if (xenus_bios_runtime_ucore_enabled("process"))
        module_add("process", "1.0", "process viewer + manager",
                   mod_process_init, NULL);
    if (xenus_bios_runtime_ucore_enabled("applications"))
        module_add("applications", "1.0", "freedesktop application registry",
                   mod_apps_init, NULL);
    if (xenus_bios_runtime_ucore_enabled("hardware"))
        module_add("hardware", "1.0", "real hardware bridge",
                   mod_hardware_init, NULL);
    if (xenus_bios_runtime_ucore_enabled("notifications"))
        module_add("notifications", "1.0", "notification and DND state",
                   mod_notify_init, NULL);

    core.state = XENUS_CORE_RUNNING;
    xenus_core_log(1, "UCore %s running on %s/%s", xenus_core_version(),
                  xenus_core_sysinfo()->os, xenus_core_sysinfo()->arch);
    xenus_core_event_emit(XENUS_CORE_EV_LIFECYCLE, "ucore", "runtime started");
    return XENUS_CORE_OK;
}

void xenus_core_shutdown(void)
{
    int i;

    if (core.state == XENUS_CORE_DOWN)
        return;
    core.state = XENUS_CORE_STOPPING;
    xenus_core_log(1, "UCore stopping");
    for (i = XENUS_CORE_MAX_MODULES - 1; i >= 0; i--) {
        module_slot_t *m = &core.modules[i];

        if (m->used && m->shutdown)
            m->shutdown();
    }
    xenus_core_event_emit(XENUS_CORE_EV_LIFECYCLE, "ucore", "runtime stopped");
    memset(&core, 0, sizeof(core));
    core.state = XENUS_CORE_DOWN;
}

int xenus_core_tick(void)
{
    uint64_t now;

    if (core.state != XENUS_CORE_RUNNING)
        return 0;
    now = now_ms();
    if (core.last_tick_ms && now - core.last_tick_ms < 1000)
        return 0;
    core.last_tick_ms = now;
    core_sys_refresh_load();
    return 1;
}

int xenus_core_dnd_get(void)
{
    return core.dnd;
}

void xenus_core_dnd_set(int on)
{
    core.dnd = on ? 1 : 0;
    xenus_core_log(1, "notifications: Do Not Disturb %s", on ? "on" : "off");
    xenus_core_event_emit(XENUS_CORE_EV_NOTIFY, "dnd",
                         on ? "Do Not Disturb on" : "Do Not Disturb off");
}
