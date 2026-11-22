/*
 * XENUS App API - event subscription and dispatch.
 *
 * Events are plain value objects in a fixed-size queue; strings live inside
 * the event so a queued event never points at caller memory.
 */
#ifndef XENUS_APP_EVENT_H
#define XENUS_APP_EVENT_H

#include <xenus/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XENUS_EVENT_ANY = 0,            /* subscription wildcard (never posted) */
    XENUS_EVENT_APP_INIT = 1,
    XENUS_EVENT_APP_SHUTDOWN,
    XENUS_EVENT_NOTIFICATION,
    XENUS_EVENT_CONFIG_CHANGED,
    XENUS_EVENT_PLUGIN_LOADED,
    XENUS_EVENT_PLUGIN_FAILED,
    XENUS_EVENT_WINDOW_EXPOSE,
    XENUS_EVENT_WINDOW_CLOSE,
    XENUS_EVENT_WINDOW_BUTTON,
    XENUS_EVENT_WINDOW_KEY,
    XENUS_EVENT_WINDOW_CONFIGURE,
    XENUS_EVENT_CUSTOM = 1000       /* applications may post from here up */
} xenus_event_type_t;

typedef struct xenus_event {
    int type;                      /* xenus_event_type_t */
    uint32_t time_ms;              /* CLOCK_MONOTONIC milliseconds */
    uint64_t id;                   /* notification id / plugin index / 0 */
    int32_t x, y;
    int32_t width, height;
    uint32_t state;                /* modifier / button mask */
    uint32_t code;                 /* keysym, button number, ... */
    char text[128];                /* NUL-terminated payload */
    void *window;                  /* xenus_window_t * when window-related */
} xenus_event_t;

typedef void (*xenus_event_fn)(const xenus_event_t *event, void *user);

/* type = XENUS_EVENT_ANY subscribes to every event. */
int xenus_event_subscribe(int type, xenus_event_fn fn, void *user);
int xenus_event_unsubscribe(xenus_event_fn fn, void *user);

/* Queues one event; the oldest event is dropped when the queue is full. */
int xenus_event_post(const xenus_event_t *event);

/* Delivers every queued event to matching subscribers, returns the count. */
int xenus_event_dispatch(void);
int xenus_event_pending(void);
void xenus_event_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_APP_EVENT_H */
