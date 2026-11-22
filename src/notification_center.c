/*
 * XENUS Notification Center - the notification history surface.
 *
 * The data lives in the App API notification ring; this file only renders
 * it and exposes unread state for the panel tray badge.
 */
#include "xenus.h"

#include <xenus/notification.h>
#include <xenus/ucore.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NC_W       372
#define NC_H       450
#define NC_TITLE   34
#define NC_PAD     14
#define NC_CARD_H  68
#define NC_CARD_GAP 8

enum {
    NC_ID_DND = 200,
    NC_ID_CLEAR,
    NC_ID_CLOSE
};

typedef struct {
    int x, y, w, h;
    int id;
} NcHit;

static Window nc_win;
static int nc_open;
static int nc_x, nc_y;
static int nc_hover = -1;
static int nc_off;
static NcHit nc_hits[16];
static int nc_nhits;
static uint64_t nc_last_read;
static char nc_status[120] = " ";
static xenus_notification_t nc_items[XENUS_NOTIFICATION_RING];
static int nc_count;

static void nc_draw(void);

static int nc_visible_rows(void)
{
    int y = NC_TITLE + 10 + 54;
    int rows = (NC_H - y - 54) / (NC_CARD_H + NC_CARD_GAP);

    return rows < 1 ? 1 : rows;
}

static unsigned long nc_surface(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL, 8);
}

static unsigned long nc_surface_hi(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL, 15);
}

static unsigned long nc_line(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL, 22);
}

static unsigned long nc_muted(void)
{
    return ui_dim(cfg.col_panel_fg, cfg.col_panel_bg);
}

static void nc_hit(int x, int y, int w, int h, int id)
{
    if (nc_nhits >= (int)(sizeof(nc_hits) / sizeof(nc_hits[0])))
        return;
    nc_hits[nc_nhits].x = x;
    nc_hits[nc_nhits].y = y;
    nc_hits[nc_nhits].w = w;
    nc_hits[nc_nhits].h = h;
    nc_hits[nc_nhits].id = id;
    nc_nhits++;
}

static const NcHit *nc_hit_at(int x, int y)
{
    int i;

    for (i = 0; i < nc_nhits; i++)
        if (x >= nc_hits[i].x && x < nc_hits[i].x + nc_hits[i].w &&
            y >= nc_hits[i].y && y < nc_hits[i].y + nc_hits[i].h)
            return &nc_hits[i];
    return NULL;
}

static void nc_load(void)
{
    size_t total = xenus_notify_count();
    size_t i;

    nc_count = 0;
    for (i = 0; i < total && nc_count < XENUS_NOTIFICATION_RING; i++) {
        xenus_notification_t n;

        if (xenus_notify_get(i, &n) == XENUS_OK)
            nc_items[nc_count++] = n;
    }
    {
        int max_off = nc_count - nc_visible_rows();

        if (max_off < 0)
            max_off = 0;
        if (nc_off > max_off)
            nc_off = max_off;
        if (nc_off < 0)
            nc_off = 0;
    }
}

static void nc_age(uint32_t then_ms, char *buf, size_t size)
{
    struct timespec ts;
    uint64_t now;
    unsigned long age;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        snprintf(buf, size, "now");
        return;
    }
    now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    age = now > then_ms ? (unsigned long)((now - then_ms) / 1000u) : 0;
    if (age < 60)
        snprintf(buf, size, "%lus ago", age);
    else if (age < 3600)
        snprintf(buf, size, "%lum ago", age / 60);
    else if (age < 86400)
        snprintf(buf, size, "%luh ago", age / 3600);
    else
        snprintf(buf, size, "%lud ago", age / 86400);
}

static void nc_draw(void)
{
    int x = NC_PAD;
    int y = NC_TITLE + 10;
    int w = NC_W - 2 * NC_PAD;
    int rows;
    int i;

    if (!nc_win)
        return;
    nc_nhits = 0;
    nc_load();
    XSetForeground(sc.dpy, sc.gc, cfg.col_panel_bg);
    XFillRectangle(sc.dpy, nc_win, sc.gc, 0, 0, NC_W, NC_H);

    XSetForeground(sc.dpy, sc.gc, nc_surface());
    XFillRectangle(sc.dpy, nc_win, sc.gc, 0, 0, NC_W, NC_TITLE);
    ui_draw_traffic(nc_win, 12, NC_TITLE / 2, 1, -1, 0, cfg.col_panel_bg);
    draw_text_bold(nc_win, 76, (NC_TITLE + sc.font_ascent -
                                sc.font_descent) / 2,
                   cfg.col_panel_fg, "Notifications");
    /* DND strip */
    ui_round_fill(nc_win, x, y, w, 44, 11, nc_surface());
    ui_round_outline(nc_win, x, y, w, 44, 11, nc_line());
    ui_draw_moon(nc_win, x + 16, y + 22, 16,
                 xenus_core_dnd_get() ? ui_accent_soft() : nc_muted());
    draw_text(nc_win, x + 42, y + 27, cfg.col_panel_fg, "Do Not Disturb");
    draw_text_small(nc_win, x + 148, y + 27, nc_muted(),
                    xenus_core_dnd_get() ? "Silenced" : "Off");
    ui_draw_toggle(nc_win, x + w - 58, y + 10, 44, 24,
                   xenus_core_dnd_get(), nc_hover == NC_ID_DND);
    nc_hit(x, y, w, 44, NC_ID_DND);
    y += 54;

    rows = nc_visible_rows();

    if (nc_count == 0) {
        ui_round_fill(nc_win, x, y, w, 120, 13, nc_surface());
        ui_round_outline(nc_win, x, y, w, 120, 13, nc_line());
        ui_draw_bell(nc_win, NC_W / 2, y + 42, 26, nc_muted());
        {
            const char *s = "No notifications";
            int tw = text_width(s);

            draw_text(nc_win, (NC_W - tw) / 2, y + 82, cfg.col_panel_fg, s);
            s = "Application and system events appear here.";
            tw = text_width_small(s);
            draw_text_small(nc_win, (NC_W - tw) / 2, y + 101,
                            nc_muted(), s);
        }
        y += 132;
    } else {
        for (i = 0; i < rows && nc_off + i < nc_count; i++) {
            int idx = nc_count - 1 - (nc_off + i);
            xenus_notification_t *n = &nc_items[idx];
            int cy = y + i * (NC_CARD_H + NC_CARD_GAP);
            int fresh = n->id > nc_last_read;
            char age[32];

            ui_round_fill(nc_win, x, cy, w, NC_CARD_H, 11,
                          fresh ? nc_surface_hi() : nc_surface());
            ui_round_outline(nc_win, x, cy, w, NC_CARD_H, 11, nc_line());
            if (fresh)
                ui_round_fill(nc_win, x + 1, cy + 14, 3, NC_CARD_H - 28, 2,
                              ui_accent());
            nc_age(n->time_ms, age, sizeof(age));
            draw_text_clip(nc_win, x + 14, x + w - 76, cy + 23,
                           cfg.col_panel_fg, n->title);
            draw_text_small(nc_win, x + w - 74, cy + 23, nc_muted(), age);
            draw_text_clip(nc_win, x + 14, x + w - 14, cy + 43,
                           nc_muted(), n->body);
            draw_text_small(nc_win, x + 14, cy + 58, nc_muted(),
                            fresh ? "new" : "seen");
        }
    }

    /* footer */
    if (nc_hover == NC_ID_CLEAR)
        ui_round_fill(nc_win, x, NC_H - 42, 110, 30, 8, nc_surface_hi());
    draw_text(nc_win, x + 2, NC_H - 22, cfg.col_panel_fg, "Clear all");
    nc_hit(x, NC_H - 42, 110, 30, NC_ID_CLEAR);
    if (nc_count > rows)
        draw_text_small(nc_win, x + 130, NC_H - 22, nc_muted(),
                        "scroll for older");
    if (nc_status[0] != ' ')
        draw_text_small(nc_win, x + 240, NC_H - 22, ui_yellow(), nc_status);
    XFlush(sc.dpy);
}

static void nc_action(int id)
{
    if (id == NC_ID_DND) {
        xenus_core_dnd_set(!xenus_core_dnd_get());
        snprintf(nc_status, sizeof(nc_status), "%s",
                 xenus_core_dnd_get() ? "DND on" : "DND off");
    } else if (id == NC_ID_CLEAR) {
        xenus_notify_clear();
        nc_last_read = xenus_notify_total();
        nc_off = 0;
        nc_count = 0;
        snprintf(nc_status, sizeof(nc_status), "Cleared");
    } else if (id == NC_ID_CLOSE) {
        notification_center_hide();
        return;
    }
    nc_draw();
}

void notification_center_show(void)
{
    if (!nc_win) {
        XSetWindowAttributes wa;

        wa.override_redirect = True;
        wa.background_pixel = cfg.col_panel_bg;
        wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                        ButtonReleaseMask | KeyPressMask;
        nc_x = sc.sw - NC_W - 14;
        if (nc_x < 8)
            nc_x = 8;
        nc_y = reserve_top() + 6;
        if (nc_y + NC_H > sc.sh - reserve_bottom() - 6)
            nc_y = sc.sh - reserve_bottom() - NC_H - 6;
        if (nc_y < reserve_top())
            nc_y = reserve_top();
        nc_win = XCreateWindow(sc.dpy, sc.root, nc_x, nc_y, NC_W, NC_H, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask,
                               &wa);
        XStoreName(sc.dpy, nc_win, "XENUS Notifications");
        ui_shape_round(nc_win, NC_W, NC_H, 16);
    }
    nc_open = 1;
    nc_hover = -1;
    nc_off = 0;
    nc_status[0] = ' ';
    nc_last_read = xenus_notify_total();
    XMapRaised(sc.dpy, nc_win);
    focus_track(nc_win);
    panel_update();
    XFlush(sc.dpy);
    nc_draw();
}

void notification_center_hide(void)
{
    if (!nc_open)
        return;
    nc_open = 0;
    focus_drop(nc_win);
    if (sc.focused && !sc.focused->minimized)
        focus_track(sc.focused->win);
    XUnmapWindow(sc.dpy, nc_win);
    XFlush(sc.dpy);
    panel_update();
}

void notification_center_toggle(void)
{
    if (nc_open)
        notification_center_hide();
    else
        notification_center_show();
}

int notification_center_open(void)
{
    return nc_open;
}

Window notification_center_window(void)
{
    return nc_win;
}

int notification_center_unread(void)
{
    uint64_t total = xenus_notify_total();

    if (nc_open)
        return 0;
    if (total <= nc_last_read)
        return 0;
    {
        uint64_t unread = total - nc_last_read;
        return unread > (uint64_t)INT_MAX ? INT_MAX : (int)unread;
    }
}

void notification_center_tick(void)
{
    static int last_unread = -1;
    int unread = notification_center_unread();

    if (unread != last_unread) {
        last_unread = unread;
        panel_update();
    }
    if (nc_open)
        nc_draw();
}

void notification_center_cleanup(void)
{
    if (nc_win != None) {
        XDestroyWindow(sc.dpy, nc_win);
        nc_win = None;
    }
    nc_open = 0;
}

int notification_center_expose(XExposeEvent *ev)
{
    if (!nc_win || ev->window != nc_win)
        return 0;
    if (ev->count == 0)
        nc_draw();
    return 1;
}

int notification_center_motion(XMotionEvent *ev)
{
    int found = -1;
    const NcHit *h;

    if (!nc_open || ev->window != nc_win)
        return 0;
    h = nc_hit_at((int)ev->x, (int)ev->y);
    if (h)
        found = h->id;
    if (found != nc_hover) {
        nc_hover = found;
        nc_draw();
    }
    return 1;
}

int notification_center_button(XButtonEvent *ev)
{
    const NcHit *h;
    int x, y;

    if (!nc_open || ev->window != nc_win)
        return 0;
    focus_track(nc_win);

    if (ev->button == Button4 || ev->button == Button5) {
        if (ev->button == Button4)
            nc_off--;
        else
            nc_off++;
        if (nc_off < 0)
            nc_off = 0;
        nc_draw();
        return 1;
    }
    if (ev->button != Button1)
        return 1;

    x = (int)ev->x;
    y = (int)ev->y;

    if (y < NC_TITLE) {
        int cell = 21;
        int left = 6;

        if (x >= left && x < left + 3 * cell) {
            int light = (x - left) / cell;

            if (light == UI_LIGHT_CLOSE || light == UI_LIGHT_MIN)
                notification_center_hide();
        } else {
            selfdrag_window(nc_win, ev, &nc_x, &nc_y, NC_W, NC_H);
        }
        return 1;
    }

    h = nc_hit_at(x, y);
    if (h)
        nc_action(h->id);
    return 1;
}

void notification_center_key(XKeyEvent *ev)
{
    char tmp[8];
    KeySym sym = NoSymbol;

    if (!nc_open)
        return;
    XLookupString(ev, tmp, sizeof(tmp) - 1, &sym, NULL);
    if (sym == XK_Escape)
        notification_center_hide();
    else if (sym == XK_Delete) {
        nc_action(NC_ID_CLEAR);
        return;
    } else if (sym == XK_Up || sym == XK_Down) {
        nc_off += sym == XK_Down ? 1 : -1;
        if (nc_off < 0)
            nc_off = 0;
        nc_draw();
    }
}
