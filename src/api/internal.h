/*
 * XENUS App API - internal helpers shared by the api translation units.
 * Not installed; applications only see the public headers in include/xenus.
 */
#ifndef XENUS_API_INTERNAL_H
#define XENUS_API_INTERNAL_H

#include <xenus/common.h>

/* CLOCK_MONOTONIC milliseconds, wraps after ~49 days (fine for deltas). */
uint32_t xenus_event_now_ms(void);

#endif /* XENUS_API_INTERNAL_H */
