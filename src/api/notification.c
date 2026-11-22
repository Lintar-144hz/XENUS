/*
 * XENUS App API - notifications (fixed ring + event announcement).
 */
#include <xenus/event.h>
#include <xenus/notification.h>

#include "internal.h"

#include <stdio.h>
#include <string.h>

static xenus_notification_t ring[XENUS_NOTIFICATION_RING];
static size_t ring_len;                 /* stored entries */
static size_t ring_head;                /* oldest entry */
static uint64_t notify_total;
static uint64_t notify_next_id = 1;

int xenus_notify(const char *title, const char *body)
{
    xenus_notification_t n;
    xenus_event_t ev;
    size_t slot;
    int written;

    if (!title || !*title)
        return XENUS_ERR_ARG;

    memset(&n, 0, sizeof(n));
    n.id = notify_next_id++;
    n.time_ms = xenus_event_now_ms();
    snprintf(n.title, sizeof(n.title), "%s", title);
    snprintf(n.body, sizeof(n.body), "%s", body ? body : "");

    if (ring_len == XENUS_NOTIFICATION_RING) {
        slot = ring_head;
        ring_head = (ring_head + 1) % XENUS_NOTIFICATION_RING;
    } else {
        slot = (ring_head + ring_len) % XENUS_NOTIFICATION_RING;
        ring_len++;
    }
    ring[slot] = n;
    notify_total++;

    xenus_logf(XENUS_LOG_INFO, "notification: %s%s%s", n.title,
              n.body[0] ? " - " : "", n.body);

    memset(&ev, 0, sizeof(ev));
    ev.type = XENUS_EVENT_NOTIFICATION;
    ev.id = n.id;
    ev.time_ms = n.time_ms;
    written = snprintf(ev.text, sizeof(ev.text), "%s%s%s", n.title,
                       n.body[0] ? ": " : "", n.body);
    if (written < 0)
        ev.text[0] = '\0';
    xenus_event_post(&ev);
    return XENUS_OK;
}

int xenus_notify_last(xenus_notification_t *out)
{
    if (!out)
        return XENUS_ERR_ARG;
    if (ring_len == 0)
        return XENUS_ERR_NOT_FOUND;
    *out = ring[(ring_head + ring_len - 1) % XENUS_NOTIFICATION_RING];
    return XENUS_OK;
}

size_t xenus_notify_count(void)
{
    return ring_len;
}

uint64_t xenus_notify_total(void)
{
    return notify_total;
}

int xenus_notify_get(size_t index, xenus_notification_t *out)
{
    if (!out)
        return XENUS_ERR_ARG;
    if (index >= ring_len)
        return XENUS_ERR_NOT_FOUND;
    *out = ring[(ring_head + index) % XENUS_NOTIFICATION_RING];
    return XENUS_OK;
}

void xenus_notify_clear(void)
{
    memset(ring, 0, sizeof(ring));
    ring_len = 0;
    ring_head = 0;
}
