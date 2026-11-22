/*
 * XENUS App API - application lifecycle, metadata and services.
 *
 * Applications link against this layer instead of XENUS internals: they only
 * ever include the public xenus headers.  Plugins receive the same
 * functionality through the xenus_api_t table handed to xenus_plugin_init().
 */
#ifndef XENUS_APP_APP_H
#define XENUS_APP_APP_H

#include <xenus/common.h>
#include <xenus/event.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------- lifecycle/metadata -- */

typedef struct xenus_app_info {
    const char *name;             /* required, short identifier */
    const char *version;          /* required, free-form */
    const char *description;      /* optional */
    const char *author;           /* optional */
} xenus_app_info_t;

/* Strings are copied; the caller's buffers may go away immediately. */
int xenus_app_init(const xenus_app_info_t *info);   /* NULL = anonymous app */
void xenus_app_shutdown(void);                     /* idempotent */
int xenus_app_is_running(void);
const xenus_app_info_t *xenus_app_info(void);       /* NULL when not running */

/* ----------------------------------------------------------- services -- */

typedef int (*xenus_service_fn)(const char *args, char *out, size_t out_len,
                               void *user);

int xenus_service_register(const char *name, xenus_service_fn fn, void *user);
int xenus_service_unregister(const char *name);
/*
 * Calls a registered service.  Built-ins registered by xenus_app_init():
 *   "version"    -> out = API version string
 *   "notify"     -> args = "title\nbody"
 *   "config.get" -> args = config key, out = value
 */
int xenus_service_call(const char *name, const char *args, char *out,
                      size_t out_len);
int xenus_service_count(void);
/*
 * Mediated call: enforces the caller's capabilities before dispatch
 * (fail-closed, audited as CAPABILITY_DENIED/GRAANTED metadata).
 * Mapping: "version" -> public, "notify" -> AUDIT, "config.get" ->
 * CONFIG, unknown services -> NOT_FOUND (never granted implicitly).
 * In-process mediation is POLICY-LEVEL ONLY (bypassable via direct
 * libc/syscall); kernel enforcement applies inside sandboxed children.
 */
int xenus_service_call_as(const char *subject, const char *name,
                         const char *args, char *out, size_t out_len);

/* ------------------------------------------------------- plugin table -- */

/*
 * Stable function table handed to plugins.  New fields may only be appended
 * in a minor release; plugins must check compatibility through
 * version_check() and only touch fields implied by their required minor.
 */
typedef struct xenus_api {
    uint32_t version_major;
    uint32_t version_minor;
    int (*version_check)(uint32_t want_major, uint32_t want_minor);
    void (*log)(xenus_log_level_t level, const char *message);
    int (*notify)(const char *title, const char *body);
    const char *(*config_get)(const char *key, const char *default_value);
    int (*config_get_int)(const char *key, int default_value);
    int (*event_subscribe)(int type, xenus_event_fn fn, void *user);
    int (*event_post)(const xenus_event_t *event);
    int (*service_call)(const char *name, const char *args, char *out,
                        size_t out_len);
} xenus_api_t;

/* Always valid (static storage). */
const xenus_api_t *xenus_api_table(void);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_APP_APP_H */
