/*
 * XENUS App API - event queue and subscriptions.
 *
 * Fixed storage only: a ring of events plus a subscriber table, so posting
 * can never allocate, fail halfway, or leave a dangling pointer behind.
 */
#include <xenus/event.h>

#include <string.h>
#include <time.h>

#define EV_QUEUE_LEN 64
#define EV_MAX_SUBS  32

typedef struct {
    int used;
    int type;                  /* XENUS_EVENT_ANY = every event */
    xenus_event_fn fn;
    void *user;
} sub_t;

static xenus_event_t ev_queue[EV_QUEUE_LEN];
static int ev_head;            /* index of the oldest event */
static int ev_len;
static sub_t ev_subs[EV_MAX_SUBS];

uint32_t xenus_event_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                      (uint64_t)ts.tv_nsec / 1000000u);
}

int xenus_event_subscribe(int type, xenus_event_fn fn, void *user)
{
    int i;

    if (!fn || type < XENUS_EVENT_ANY)
        return XENUS_ERR_ARG;
    for (i = 0; i < EV_MAX_SUBS; i++) {
        if (!ev_subs[i].used) {
            ev_subs[i].used = 1;
            ev_subs[i].type = type;
            ev_subs[i].fn = fn;
            ev_subs[i].user = user;
            return XENUS_OK;
        }
    }
    return XENUS_ERR_FULL;
}

int xenus_event_unsubscribe(xenus_event_fn fn, void *user)
{
    int i;

    if (!fn)
        return XENUS_ERR_ARG;
    for (i = 0; i < EV_MAX_SUBS; i++) {
        if (ev_subs[i].used && ev_subs[i].fn == fn &&
            ev_subs[i].user == user) {
            memset(&ev_subs[i], 0, sizeof(ev_subs[i]));
            return XENUS_OK;
        }
    }
    return XENUS_ERR_NOT_FOUND;
}

int xenus_event_post(const xenus_event_t *event)
{
    int slot;

    if (!event || event->type == XENUS_EVENT_ANY)
        return XENUS_ERR_ARG;

    if (ev_len == EV_QUEUE_LEN) {
        /* full: drop the oldest event and keep going */
        ev_head = (ev_head + 1) % EV_QUEUE_LEN;
        ev_len--;
    }
    slot = (ev_head + ev_len) % EV_QUEUE_LEN;
    ev_queue[slot] = *event;
    if (ev_queue[slot].time_ms == 0)
        ev_queue[slot].time_ms = xenus_event_now_ms();
    ev_len++;
    return XENUS_OK;
}

int xenus_event_pending(void)
{
    return ev_len;
}

void xenus_event_clear(void)
{
    ev_head = 0;
    ev_len = 0;
    memset(ev_queue, 0, sizeof(ev_queue));
}

int xenus_event_dispatch(void)
{
    int delivered = 0;

    while (ev_len > 0) {
        xenus_event_t ev = ev_queue[ev_head];
        int i;

        ev_head = (ev_head + 1) % EV_QUEUE_LEN;
        ev_len--;

        for (i = 0; i < EV_MAX_SUBS; i++) {
            if (!ev_subs[i].used)
                continue;
            if (ev_subs[i].type != XENUS_EVENT_ANY &&
                ev_subs[i].type != ev.type)
                continue;
            ev_subs[i].fn(&ev, ev_subs[i].user);
            delivered++;
        }
    }
    return delivered;
}
