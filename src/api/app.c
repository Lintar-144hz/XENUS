/*
 * XENUS App API - application lifecycle, metadata, services, plugin table.
 */
#include <xenus/app.h>
#include <xenus/config.h>
#include <xenus/event.h>
#include <xenus/notification.h>
#include <xenus/security.h>
#include <xenus/window.h>

#include "internal.h"

#include <stdio.h>
#include <string.h>

#define SVC_MAX 16
#define SVC_NAME_LEN 32

typedef struct {
    int used;
    char name[SVC_NAME_LEN];
    xenus_service_fn fn;
    void *user;
} service_t;

static int app_running;
static xenus_app_info_t app_info;
static char info_name[64];
static char info_version[32];
static char info_desc[128];
static char info_author[64];
static service_t services[SVC_MAX];

/* --------------------------------------------------- built-in services -- */

static int svc_version(const char *args, char *out, size_t out_len,
                       void *user)
{
    (void)args;
    (void)user;
    if (!out || out_len == 0)
        return XENUS_ERR_ARG;
    snprintf(out, out_len, "%s", XENUS_API_VERSION_STRING);
    return XENUS_OK;
}

static int svc_notify(const char *args, char *out, size_t out_len, void *user)
{
    char title[XENUS_NOTIFICATION_TITLE];
    const char *nl;

    (void)out;
    (void)out_len;
    (void)user;
    if (!args || !*args)
        return XENUS_ERR_ARG;
    nl = strchr(args, '\n');
    if (nl) {
        size_t n = (size_t)(nl - args);

        if (n >= sizeof(title))
            n = sizeof(title) - 1;
        memcpy(title, args, n);
        title[n] = '\0';
        return xenus_notify(title, nl + 1);
    }
    return xenus_notify(args, "");
}

static int svc_config_get(const char *args, char *out, size_t out_len,
                          void *user)
{
    const char *v;

    (void)user;
    if (!out || out_len == 0)
        return XENUS_ERR_ARG;
    out[0] = '\0';
    if (!args || !*args)
        return XENUS_ERR_ARG;
    v = xenus_config_get(args, NULL);
    if (!v)
        return XENUS_ERR_NOT_FOUND;
    snprintf(out, out_len, "%s", v);
    return XENUS_OK;
}

static const struct {
    const char *name;
    xenus_service_fn fn;
} builtins[] = {
    { "version", svc_version },
    { "notify", svc_notify },
    { "config.get", svc_config_get }
};

/* ----------------------------------------------------------- lifecycle -- */

static void clear_services(void)
{
    memset(services, 0, sizeof(services));
}

static void register_builtins(void)
{
    size_t i;

    for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        xenus_service_register(builtins[i].name, builtins[i].fn, NULL);
}

int xenus_app_init(const xenus_app_info_t *info)
{
    if (app_running)
        return XENUS_ERR_STATE;
    if (info && (!info->name || !*info->name))
        return XENUS_ERR_ARG;

    info_name[0] = '\0';
    info_version[0] = '\0';
    info_desc[0] = '\0';
    info_author[0] = '\0';
    if (info) {
        snprintf(info_name, sizeof(info_name), "%s", info->name);
        snprintf(info_version, sizeof(info_version), "%s",
                 info->version ? info->version : "");
        snprintf(info_desc, sizeof(info_desc), "%s",
                 info->description ? info->description : "");
        snprintf(info_author, sizeof(info_author), "%s",
                 info->author ? info->author : "");
    }
    app_info.name = info_name;
    app_info.version = info_version;
    app_info.description = info_desc;
    app_info.author = info_author;

    clear_services();
    register_builtins();
    app_running = 1;

    xenus_logf(XENUS_LOG_INFO, "app: %s %s initialized (api %s)",
              info_name[0] ? info_name : "app",
              info_version[0] ? info_version : "",
              XENUS_API_VERSION_STRING);

    {
        xenus_event_t ev;

        memset(&ev, 0, sizeof(ev));
        ev.type = XENUS_EVENT_APP_INIT;
        snprintf(ev.text, sizeof(ev.text), "%s", info_name);
        xenus_event_post(&ev);
    }
    return XENUS_OK;
}

void xenus_app_shutdown(void)
{
    xenus_event_t ev;

    if (!app_running)
        return;

    memset(&ev, 0, sizeof(ev));
    ev.type = XENUS_EVENT_APP_SHUTDOWN;
    snprintf(ev.text, sizeof(ev.text), "%s", info_name);
    xenus_event_post(&ev);

    clear_services();
    app_running = 0;
    xenus_api_close_display();
    xenus_logf(XENUS_LOG_INFO, "app: %s shut down",
              info_name[0] ? info_name : "app");
    info_name[0] = '\0';
    info_version[0] = '\0';
    info_desc[0] = '\0';
    info_author[0] = '\0';
}

int xenus_app_is_running(void)
{
    return app_running;
}

const xenus_app_info_t *xenus_app_info(void)
{
    return app_running ? &app_info : NULL;
}

/* ----------------------------------------------------------- services -- */

int xenus_service_register(const char *name, xenus_service_fn fn, void *user)
{
    int free_slot = -1;
    int i;

    if (!name || !*name || strlen(name) >= SVC_NAME_LEN || !fn)
        return XENUS_ERR_ARG;
    for (i = 0; i < SVC_MAX; i++) {
        if (!services[i].used) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (strcmp(services[i].name, name) == 0)
            return XENUS_ERR_STATE;
    }
    if (free_slot < 0)
        return XENUS_ERR_FULL;
    services[free_slot].used = 1;
    snprintf(services[free_slot].name, sizeof(services[free_slot].name), "%s",
             name);
    services[free_slot].fn = fn;
    services[free_slot].user = user;
    return XENUS_OK;
}

int xenus_service_unregister(const char *name)
{
    int i;

    if (!name)
        return XENUS_ERR_ARG;
    for (i = 0; i < SVC_MAX; i++) {
        if (services[i].used && strcmp(services[i].name, name) == 0) {
            memset(&services[i], 0, sizeof(services[i]));
            return XENUS_OK;
        }
    }
    return XENUS_ERR_NOT_FOUND;
}

int xenus_service_call(const char *name, const char *args, char *out,
                      size_t out_len)
{
    int i;

    if (!name)
        return XENUS_ERR_ARG;
    for (i = 0; i < SVC_MAX; i++) {
        if (services[i].used && strcmp(services[i].name, name) == 0)
            return services[i].fn(args, out, out_len, services[i].user);
    }
    return XENUS_ERR_NOT_FOUND;
}

/*
 * Capability-mediated dispatch.  The trusted core path
 * (xenus_service_call) stays untouched for ABI/behaviour stability;
 * untrusted callers go through here so forbidden capabilities are
 * denied and audited instead of silently allowed.
 */
int xenus_service_call_as(const char *subject, const char *name,
                         const char *args, char *out, size_t out_len)
{
    uint32_t need = 0;

    if (!subject || !*subject || !name || !*name)
        return XENUS_ERR_ARG;
    if (strcmp(name, "version") == 0) {
        need = 0;                   /* public */
    } else if (strcmp(name, "notify") == 0) {
        need = XENUS_CAP_AUDIT;
    } else if (strcmp(name, "config.get") == 0) {
        need = XENUS_CAP_CONFIG;
    } else {
        return XENUS_ERR_NOT_FOUND;
    }
    if (need != 0 && xenus_security_check(subject, need) != XENUS_OK)
        return XENUS_ERR;            /* audited as CAPABILITY_DENIED */
    return xenus_service_call(name, args, out, out_len);
}

int xenus_service_count(void)
{
    int i, n = 0;

    for (i = 0; i < SVC_MAX; i++) {
        if (services[i].used)
            n++;
    }
    return n;
}

/* ------------------------------------------------------- plugin table -- */

static const xenus_api_t api_table = {
    .version_major = XENUS_API_VERSION_MAJOR,
    .version_minor = XENUS_API_VERSION_MINOR,
    .version_check = xenus_api_version_check,
    .log = xenus_log,
    .notify = xenus_notify,
    .config_get = xenus_config_get,
    .config_get_int = xenus_config_get_int,
    .event_subscribe = xenus_event_subscribe,
    .event_post = xenus_event_post,
    .service_call = xenus_service_call
};

const xenus_api_t *xenus_api_table(void)
{
    return &api_table;
}
