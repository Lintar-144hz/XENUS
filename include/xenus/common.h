/*
 * XENUS App API - common definitions: version, status codes, logging.
 *
 * Public header: applications and plugins include <xenus/...> only, never
 * anything under src/.  Single-threaded, same as the rest of XENUS.
 */
#ifndef XENUS_APP_COMMON_H
#define XENUS_APP_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- version -- */

#define XENUS_API_VERSION_MAJOR 1
#define XENUS_API_VERSION_MINOR 0
#define XENUS_API_VERSION_STRING "1.0"

/*
 * Compatibility check: the wanted major must match the host and the wanted
 * minor must not be newer than the host (a newer host keeps old callers
 * working).  Returns XENUS_OK or XENUS_ERR_VERSION.
 */
int xenus_api_version_check(uint32_t want_major, uint32_t want_minor);

/* "1.0" - handy for logging and for the built-in "version" service */
const char *xenus_api_version_string(void);

/* --------------------------------------------------------- status codes -- */

typedef enum {
    XENUS_OK = 0,
    XENUS_ERR = -1,           /* generic failure */
    XENUS_ERR_ARG = -2,       /* bad argument */
    XENUS_ERR_STATE = -3,     /* call not valid in the current state */
    XENUS_ERR_DISPLAY = -4,   /* no X display available */
    XENUS_ERR_NOT_FOUND = -5, /* unknown key / service / file */
    XENUS_ERR_VERSION = -6,   /* API or plugin ABI mismatch */
    XENUS_ERR_IO = -7,        /* read/write or loader failure */
    XENUS_ERR_FULL = -8       /* fixed-size table exhausted */
} xenus_status_t;

const char *xenus_strerror(int status);

/* -------------------------------------------------------------- logging -- */

typedef enum {
    XENUS_LOG_DEBUG = 0,
    XENUS_LOG_INFO = 1,
    XENUS_LOG_WARN = 2,
    XENUS_LOG_ERROR = 3
} xenus_log_level_t;

typedef void (*xenus_log_sink_t)(xenus_log_level_t level, const char *message,
                                void *user);

/* NULL sink restores the default sink (stderr). */
void xenus_log_set_sink(xenus_log_sink_t sink, void *user);
void xenus_log(xenus_log_level_t level, const char *message);
void xenus_logf(xenus_log_level_t level, const char *fmt, ...);
const char *xenus_log_level_name(xenus_log_level_t level);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_APP_COMMON_H */
