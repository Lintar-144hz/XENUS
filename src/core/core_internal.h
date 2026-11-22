/* UCore internal interfaces shared by the core translation units. */
#ifndef XENUS_CORE_INTERNAL_H
#define XENUS_CORE_INTERNAL_H

#include <xenus/ucore.h>

/* core/system sampling used by process and app managers */
int core_proc_snapshot(xenus_core_proc_t *out, int max);
int core_sys_refresh_load(void);
int core_sys_refresh_info(void);
int core_apps_load(void);

/* boot-time CPU ticks from /proc/stat (all CPUs, sum of fields) */
unsigned long long core_cpu_total_ticks(void);
unsigned long long core_cpu_idle_ticks(void);

/* generic helpers */
int core_cmdline_exists(const char *name);
int core_run_capture(const char *const argv[], char *out, size_t out_size,
                     int timeout_ms);
int core_run_silent(const char *const argv[], int timeout_ms);
int core_read_line(const char *path, char *out, size_t size);
int core_write_line(const char *path, const char *value);
int core_exists(const char *path);
int core_is_dir(const char *path);

#endif /* XENUS_CORE_INTERNAL_H */
