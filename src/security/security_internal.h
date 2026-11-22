/*
 * XENUS Security Layer - internal helpers shared by the security sources.
 * Not installed; applications only see the public security.h header.
 */
#ifndef XENUS_SECURITY_INTERNAL_H
#define XENUS_SECURITY_INTERNAL_H

#include <xenus/security.h>

/* provided by src/api/event.c (monotonic ms) */
uint32_t xenus_event_now_ms(void);

/* owned by core.c */
xenus_scan_stats_t *xenus__stats(void);
void xenus__ensure(void);                 /* lazy, idempotent init */
const char *xenus__last_exec_subject(void); /* subject of the last exec gate */

/* hooks implemented in scanner.c and driven by core.c */
int xenus__scanner_init(const char *db_dir);
void xenus__scanner_shutdown(void);
int xenus__scanner_reload(void);
int xenus__scanner_set_mode(int mode);
int xenus__scanner_watch_dir(const char *dir);
void xenus__scanner_watch_stop(void);
int xenus__scanner_watch_fd(void);
int xenus__scanner_pump(void);
int xenus__scanner_pending(void);

#endif /* XENUS_SECURITY_INTERNAL_H */
