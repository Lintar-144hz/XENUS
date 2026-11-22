/*
 * XENUS plugin ABI and loader interface.
 *
 * A plugin is one shared object that exports three symbols:
 *
 *   const xenus_plugin_info_t *xenus_plugin_info(void);   metadata, no side
 *                                                      effects expected
 *   int  xenus_plugin_init(const xenus_api_t *api);       0 = running
 *   void xenus_plugin_shutdown(void);
 *
 * The host validates the metadata (ABI + required API version) *before*
 * calling init, copies the strings it needs, and only then marks the plugin
 * running.  Plugins talk to XENUS exclusively through xenus_api_t - no XENUS
 * globals, no internal headers - which keeps the door open for per-plugin
 * capability checks later.
 */
#ifndef XENUS_APP_PLUGIN_H
#define XENUS_APP_PLUGIN_H

#include <xenus/app.h>
#include <xenus/common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_PLUGIN_ABI_VERSION 1u

#define XENUS_PLUGIN_ENTRY_INFO "xenus_plugin_info"
#define XENUS_PLUGIN_ENTRY_INIT "xenus_plugin_init"
#define XENUS_PLUGIN_ENTRY_SHUTDOWN "xenus_plugin_shutdown"

/* default export visibility when building with -fvisibility=hidden */
#if defined(__GNUC__)
#define XENUS_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define XENUS_PLUGIN_EXPORT
#endif

typedef struct xenus_plugin_info {
    uint32_t info_size;            /* sizeof(xenus_plugin_info_t) */
    uint32_t abi_version;          /* XENUS_PLUGIN_ABI_VERSION */
    const char *name;              /* unique, short, e.g. "example" */
    const char *version;           /* plugin version, e.g. "1.0.0" */
    const char *description;       /* optional, may be NULL */
    uint32_t api_major;            /* required XENUS_API_VERSION_MAJOR */
    uint32_t api_minor;            /* required XENUS_API_VERSION_MINOR */
} xenus_plugin_info_t;

typedef int (*xenus_plugin_init_fn)(const xenus_api_t *api);
typedef void (*xenus_plugin_shutdown_fn)(void);
typedef const xenus_plugin_info_t *(*xenus_plugin_info_fn)(void);

/* ----------------------------------------------------------- lifecycle -- */

typedef enum {
    XENUS_PLUGIN_DISCOVERED = 1,    /* found on disk, not opened yet */
    XENUS_PLUGIN_LOADED,            /* dlopen + validated, init not run */
    XENUS_PLUGIN_RUNNING,           /* init succeeded */
    XENUS_PLUGIN_FAILED,            /* open/validate/init failed */
    XENUS_PLUGIN_UNLOADED           /* shut down and dlclose'd */
} xenus_plugin_state_t;

/*
 * search_path: ':'-separated directories.  NULL selects $XENUS_PLUGIN_PATH,
 * then the defaults (~/.local/share/xenus/plugins, $PREFIX/lib/xenus/plugins).
 */
int xenus_plugins_init(const char *search_path);
void xenus_plugins_shutdown(void);            /* stop, unload, forget all */

int xenus_plugins_discover(void);             /* scan the search path */
int xenus_plugins_load_all(void);             /* load + validate + init all */
int xenus_plugins_load(const char *path);     /* one file (exists or error) */
int xenus_plugins_unload(const char *name);   /* shutdown + dlclose */
int xenus_plugins_unload_all(void);

int xenus_plugins_count(void);                /* table entries */
int xenus_plugins_running(void);              /* how many are running */
const xenus_plugin_info_t *xenus_plugins_info(int index);  /* NULL if unknown */
int xenus_plugins_state(int index);
const char *xenus_plugins_error(int index);   /* last failure, "" if none */
const char *xenus_plugins_file(int index);    /* discovered file path */
const char *xenus_plugins_search_path(void);
const char *xenus_plugin_state_name(int state);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_APP_PLUGIN_H */
