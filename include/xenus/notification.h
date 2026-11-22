/*
 * XENUS App API - notifications.
 *
 * Notifications are kept in a small fixed ring, logged, and announced as a
 * XENUS_EVENT_NOTIFICATION event.  A future XENUS release can render them;
 * the API does not depend on that.
 */
#ifndef XENUS_APP_NOTIFICATION_H
#define XENUS_APP_NOTIFICATION_H

#include <xenus/common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_NOTIFICATION_RING 32
#define XENUS_NOTIFICATION_TITLE 64
#define XENUS_NOTIFICATION_BODY 192

typedef struct xenus_notification {
    uint64_t id;                          /* 1-based, never reused */
    uint32_t time_ms;                     /* CLOCK_MONOTONIC milliseconds */
    char title[XENUS_NOTIFICATION_TITLE];
    char body[XENUS_NOTIFICATION_BODY];
} xenus_notification_t;

int xenus_notify(const char *title, const char *body);

/* Most recent notification, XENUS_ERR_NOT_FOUND when the ring is empty. */
int xenus_notify_last(xenus_notification_t *out);

size_t xenus_notify_count(void);           /* stored (ring) count */
uint64_t xenus_notify_total(void);         /* posted since process start */
/* Enumerate the stored ring: index 0 is the oldest entry. */
int xenus_notify_get(size_t index, xenus_notification_t *out);
void xenus_notify_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_APP_NOTIFICATION_H */
