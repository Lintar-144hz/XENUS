/*
 * XENUS App API - minimal top-level window creation and interaction.
 *
 * The API owns its own X connection (independent of the window manager's)
 * and a small fixed pool of windows, so nothing is allocated on the heap.
 * Without a DISPLAY every call fails with XENUS_ERR_DISPLAY - no crash.
 */
#ifndef XENUS_APP_WINDOW_H
#define XENUS_APP_WINDOW_H

#include <xenus/common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_WINDOW_AUTO (-1)      /* x/y: let the window manager place it */

typedef struct xenus_window xenus_window_t;   /* opaque */

typedef struct xenus_window_attrs {
    int x, y;                      /* XENUS_WINDOW_AUTO or pixels */
    unsigned int width, height;
    const char *title;
    int override_redirect;         /* 0 = normal managed window */
    unsigned long background;      /* 0xRRGGBB, 0 = default */
} xenus_window_attrs_t;

int xenus_window_create(const xenus_window_attrs_t *attrs, xenus_window_t **out);
void xenus_window_destroy(xenus_window_t *w);

int xenus_window_show(xenus_window_t *w);
int xenus_window_hide(xenus_window_t *w);
int xenus_window_set_title(xenus_window_t *w, const char *title);
int xenus_window_set_size(xenus_window_t *w, unsigned int width,
                         unsigned int height);
int xenus_window_set_background(xenus_window_t *w, unsigned long color);
unsigned long xenus_window_id(const xenus_window_t *w);   /* XID, 0 if none */

/*
 * Polls pending X events, translates the ones belonging to API windows into
 * xenus_event_t (queued for xenus_event_dispatch) and returns how many were
 * queued.  max_events <= 0 means "use the built-in limit".
 */
int xenus_window_process_events(xenus_window_t *w, int max_events);

/* Explicit display handling; xenus_window_create opens it lazily. */
int xenus_api_open_display(void);
void xenus_api_close_display(void);
int xenus_api_has_display(void);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_APP_WINDOW_H */
