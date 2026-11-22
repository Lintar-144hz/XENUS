/*
 * XENUS App API - version, status codes, logging.
 */
#include <xenus/common.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ version -- */

int xenus_api_version_check(uint32_t want_major, uint32_t want_minor)
{
    if (want_major != XENUS_API_VERSION_MAJOR)
        return XENUS_ERR_VERSION;
    if (want_minor > XENUS_API_VERSION_MINOR)
        return XENUS_ERR_VERSION;
    return XENUS_OK;
}

const char *xenus_api_version_string(void)
{
    return XENUS_API_VERSION_STRING;
}

/* -------------------------------------------------------- status codes -- */

const char *xenus_strerror(int status)
{
    switch (status) {
    case XENUS_OK:
        return "success";
    case XENUS_ERR:
        return "generic failure";
    case XENUS_ERR_ARG:
        return "invalid argument";
    case XENUS_ERR_STATE:
        return "invalid state";
    case XENUS_ERR_DISPLAY:
        return "no display available";
    case XENUS_ERR_NOT_FOUND:
        return "not found";
    case XENUS_ERR_VERSION:
        return "version mismatch";
    case XENUS_ERR_IO:
        return "input/output error";
    case XENUS_ERR_FULL:
        return "table full";
    default:
        return "unknown status";
    }
}

/* ------------------------------------------------------------- logging -- */

static xenus_log_sink_t log_sink;
static void *log_sink_user;

void xenus_log_set_sink(xenus_log_sink_t sink, void *user)
{
    log_sink = sink;
    log_sink_user = user;
}

const char *xenus_log_level_name(xenus_log_level_t level)
{
    switch (level) {
    case XENUS_LOG_DEBUG:
        return "debug";
    case XENUS_LOG_INFO:
        return "info";
    case XENUS_LOG_WARN:
        return "warn";
    case XENUS_LOG_ERROR:
        return "error";
    default:
        return "log";
    }
}

void xenus_log(xenus_log_level_t level, const char *message)
{
    if (log_sink) {
        log_sink(level, message ? message : "", log_sink_user);
        return;
    }
    fprintf(stderr, "[xenus-api] %s: %s\n", xenus_log_level_name(level),
            message ? message : "");
    fflush(stderr);
}

void xenus_logf(xenus_log_level_t level, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    if (!fmt) {
        xenus_log(level, "");
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    xenus_log(level, buf);
}
