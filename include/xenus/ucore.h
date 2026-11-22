/*
 * XENUS UCore - the shared runtime substrate for the desktop shell and the
 * System Center.
 *
 * UCore is deliberately small and dependency-free (libc + Linux procfs /
 * sysfs).  It is a *library* that runs inside the XENUS process: no daemon,
 * no threads, no polling.  The event loop calls xenus_core_tick() and every
 * consumer shares the same fixed-size snapshots and event bus.
 *
 * Nothing here fabricates hardware state.  A capability that the host does
 * not expose is reported as XENUS_HW_UNAVAILABLE / XENUS_CORE_ERR_UNAVAILABLE
 * and callers must render it as unavailable instead of toggling a dummy.
 */
#ifndef XENUS_UCORE_H
#define XENUS_UCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_CORE_VERSION_MAJOR 1
#define XENUS_CORE_VERSION_MINOR 0

#define XENUS_CORE_MAX_PROC     256
#define XENUS_CORE_MAX_APPS     128
#define XENUS_CORE_MAX_SERVICES 48
#define XENUS_CORE_MAX_MODULES  16
#define XENUS_CORE_LOG_RING     128
#define XENUS_CORE_EVENT_RING   64
#define XENUS_CORE_EVENT_SUBS   16
#define XENUS_CORE_NAME         64
#define XENUS_CORE_PATH         256

typedef enum {
    XENUS_CORE_OK = 0,
    XENUS_CORE_ERR = 1,
    XENUS_CORE_ERR_ARG = 2,
    XENUS_CORE_ERR_STATE = 3,
    XENUS_CORE_ERR_NOMEM = 4,
    XENUS_CORE_ERR_IO = 5,
    XENUS_CORE_ERR_UNAVAILABLE = 6,
    XENUS_CORE_ERR_DENIED = 7,
    XENUS_CORE_ERR_FULL = 8
} xenus_core_status_t;

typedef enum {
    XENUS_CORE_DOWN = 0,
    XENUS_CORE_STARTING,
    XENUS_CORE_RUNNING,
    XENUS_CORE_STOPPING
} xenus_core_state_t;

typedef enum {
    XENUS_HW_UNKNOWN = 0,
    XENUS_HW_AVAILABLE,
    XENUS_HW_UNAVAILABLE,
    XENUS_HW_DENIED
} xenus_hw_state_t;

typedef enum {
    XENUS_PROC_TERM = 0,
    XENUS_PROC_KILL,
    XENUS_PROC_STOP,
    XENUS_PROC_CONT
} xenus_core_proc_action_t;

/* UCore event types (also emitted on the App API bus as CUSTOM+type). */
enum {
    XENUS_CORE_EV_LIFECYCLE = 1,
    XENUS_CORE_EV_RESOURCE,
    XENUS_CORE_EV_PROCESS,
    XENUS_CORE_EV_HARDWARE,
    XENUS_CORE_EV_CONFIG,
    XENUS_CORE_EV_NOTIFY,
    XENUS_CORE_EV_MODULE
};

typedef struct {
    uint64_t total_kb;
    uint64_t free_kb;
    uint64_t available_kb;
    uint64_t buffers_kb;
    uint64_t cached_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
} xenus_core_meminfo_t;

typedef struct {
    double cpu_percent;
    double load1, load5, load15;
    unsigned long uptime_sec;
    unsigned long processes;
    unsigned long threads;
    xenus_core_meminfo_t mem;
    double mem_percent;
} xenus_core_load_t;

typedef struct {
    int pid;
    int ppid;
    int threads;
    int kernel_thread;
    char name[XENUS_CORE_NAME];
    char state;
    char cmdline[160];
    unsigned long rss_kb;
    unsigned long long cpu_ticks;
    double cpu_percent;
    double mem_percent;
    unsigned int uid;
} xenus_core_proc_t;

typedef struct {
    char name[XENUS_CORE_NAME];
    char generic_name[XENUS_CORE_NAME];
    char exec[192];
    char icon[XENUS_CORE_NAME];
    char categories[96];
    int terminal;
    int running;
    int pid;
} xenus_core_app_t;

typedef struct {
    char name[XENUS_CORE_NAME];
    char status[24];
    char detail[112];
    int pid;
    int managed;                  /* registered UCore module */
} xenus_core_service_t;

typedef struct {
    char name[48];
    char version[16];
    char description[96];
    int active;
    unsigned int calls;
    uint64_t started_ms;
} xenus_core_module_t;

typedef struct {
    uint64_t seq;
    uint32_t time_ms;
    int level;
    char source[32];
    char text[192];
} xenus_core_log_entry_t;

typedef struct {
    uint64_t seq;
    uint32_t time_ms;
    int type;
    char source[32];
    char text[192];
} xenus_core_event_t;

typedef struct {
    char os[XENUS_CORE_NAME];
    char kernel[64];
    char arch[32];
    char hostname[XENUS_CORE_NAME];
    char cpu_model[96];
    char desktop[32];
    int cpus;
    int android;
    int proot;
    int termux;
    int display_w;
    int display_h;
    unsigned long mem_total_mb;
    unsigned long uptime_sec;
} xenus_core_sysinfo_t;

typedef struct {
    xenus_hw_state_t wifi;
    xenus_hw_state_t bluetooth;
    xenus_hw_state_t airplane;
    xenus_hw_state_t volume;
    xenus_hw_state_t brightness;
    xenus_hw_state_t battery;
    int wifi_on;
    int bluetooth_on;
    int airplane_on;
    int muted;
    int volume_percent;
    int brightness_percent;
    int battery_percent;
    int wifi_controllable;
    int bluetooth_controllable;
    int airplane_controllable;
    int volume_controllable;
    int brightness_writable;
    char battery_status[24];
    char wifi_iface[32];
    char wifi_ssid[64];
    char bt_name[64];
    char detail[160];
} xenus_core_hardware_t;

/* ---------------------------------------------------------- authentication */

int xenus_auth_init(void);
int xenus_auth_enabled(void);
int xenus_auth_locked(void);
int xenus_auth_set_password(const char *password);
int xenus_auth_set_password_recoverable(
    const char *password, char *recovery_key, size_t key_size);
int xenus_auth_recover(const char *recovery_key);
int xenus_auth_change_password(const char *old_password, const char *new_password);
int xenus_auth_verify(const char *password);
int xenus_auth_lock(void);
int xenus_auth_disable(const char *password);

/* ------------------------------------------------------------- lifecycle */

xenus_core_status_t xenus_core_init(const char *config_path);
void xenus_core_shutdown(void);
xenus_core_state_t xenus_core_state(void);
const char *xenus_core_version(void);
const char *xenus_core_status_str(xenus_core_status_t status);
uint64_t xenus_core_started_ms(void);

/* Rate-limited sampler (1 Hz).  Safe to call from the event loop. */
int xenus_core_tick(void);

/* ---------------------------------------------------------------- system */

const xenus_core_sysinfo_t *xenus_core_sysinfo(void);
int xenus_core_load(xenus_core_load_t *out);
int xenus_core_uptime(void);
const char *xenus_core_config_path(void);

/* --------------------------------------------------------------- process */

int xenus_core_processes(xenus_core_proc_t *out, int max);
int xenus_core_process_get(int pid, xenus_core_proc_t *out);
xenus_core_status_t xenus_core_process_action(int pid,
                                            xenus_core_proc_action_t action);
const char *xenus_core_process_action_name(xenus_core_proc_action_t action);
int xenus_core_process_is_critical(int pid);

/* ---------------------------------------------------------- applications */

int xenus_core_apps(xenus_core_app_t *out, int max);
int xenus_core_apps_refresh(void);
xenus_core_status_t xenus_core_app_launch(int index);

/* -------------------------------------------------------------- services */

int xenus_core_services(xenus_core_service_t *out, int max);

/* --------------------------------------------------------------- modules */

int xenus_core_modules(xenus_core_module_t *out, int max);
xenus_core_status_t xenus_core_module_register(const char *name,
                                             const char *version,
                                             const char *description,
                                             int (*init)(void),
                                             void (*shutdown)(void));
xenus_core_status_t xenus_core_module_start(const char *name);
xenus_core_status_t xenus_core_module_stop(const char *name);

/* --------------------------------------------------------------- logging */

void xenus_core_log(int level, const char *fmt, ...);
int xenus_core_log_entries(xenus_core_log_entry_t *out, int max);
void xenus_core_log_clear(void);

/* --------------------------------------------------------- IPC / events */

typedef void (*xenus_core_event_cb)(const xenus_core_event_t *ev, void *user);

int xenus_core_event_subscribe(int type, xenus_core_event_cb cb, void *user);
int xenus_core_event_unsubscribe(xenus_core_event_cb cb, void *user);
int xenus_core_event_emit(int type, const char *source, const char *text);
int xenus_core_event_poll(xenus_core_event_t *out);
void xenus_core_event_pump(void);

/* -------------------------------------------------------------- hardware */

xenus_core_status_t xenus_core_hardware(xenus_core_hardware_t *out, int refresh);
xenus_core_status_t xenus_core_wifi_set(int on);
xenus_core_status_t xenus_core_bluetooth_set(int on);
xenus_core_status_t xenus_core_airplane_set(int on);
xenus_core_status_t xenus_core_volume_set(int percent);
xenus_core_status_t xenus_core_volume_mute(int on);
xenus_core_status_t xenus_core_brightness_set(int percent);

/* --------------------------------------------------- notifications / DND */

int xenus_core_dnd_get(void);
void xenus_core_dnd_set(int on);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_UCORE_H */
