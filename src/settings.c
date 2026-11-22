/*
 * XENUS Settings - the standalone desktop configuration window.
 *
 * Phase 1 is the foundation only: window lifecycle, ten-category sidebar
 * navigation and consistent placeholder pages.  No configuration, hardware,
 * display, sound or Android control is implemented here yet; those belong to
 * later phases and must only be added once they really work.
 *
 * The window follows the same self-drawn, override-redirect pattern as the
 * System Center, Control Center and Notification Center: lazy creation,
 * reuse, map/unmap, focus tracking and explicit cleanup.
 */
#include "xenus.h"

#include <stdio.h>
#include <string.h>

#define ST_W        820
#define ST_H        600
#define ST_TITLE    36
#define ST_SIDE     210
#define ST_PAD      22
#define ST_ROW_H    34
#define ST_ROW_GAP  4
#define ST_MAX_HITS 16

enum {
    ST_ID_CATEGORY = 100
};

enum {
    ST_PAGE_PERSONALIZATION = 0,
    ST_PAGE_DISPLAY,
    ST_PAGE_DESKTOP,
    ST_PAGE_INPUT,
    ST_PAGE_SOUND,
    ST_PAGE_APPLICATIONS,
    ST_PAGE_SYSTEM,
    ST_PAGE_SECURITY,
    ST_PAGE_ANDROID,
    ST_PAGE_ABOUT,
    ST_PAGE_COUNT
};

typedef struct {
    int x, y, w, h;
    int id;
} StHit;

static Window st_win;
static int st_open;
static int st_x, st_y;
static int st_page;
static int st_hover = -1;
static StHit st_hits[ST_MAX_HITS];
static int st_nhits;

static const char *const st_page_names[ST_PAGE_COUNT] = {
    "Personalization",
    "Display",
    "Desktop",
    "Input",
    "Sound",
    "Applications",
    "System",
    "Security",
    "Android Integration",
    "About"
};

static const char *const st_page_blurbs[ST_PAGE_COUNT] = {
    "Wallpaper, theme and accent controls will live here.",
    "Resolution and scaling controls will live here.",
    "Icon, panel and window behaviour controls will live here.",
    "Keyboard, pointer and shortcut controls will live here.",
    "Volume and audio device controls will live here.",
    "Installed applications and startup entries will live here.",
    "System information and UCore status will live here.",
    "Authentication and security controls will live here.",
    "Android storage and integration controls will live here.",
    "Version and project information will live here."
};

static void st_draw(void);

static unsigned long st_surface(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL, 8);
}

static unsigned long st_surface_hi(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL, 15);
}

static unsigned long st_sidebar_bg(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL, 4);
}

static unsigned long st_line(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL, 20);
}

static unsigned long st_muted(void)
{
    return ui_dim(cfg.col_panel_fg, cfg.col_panel_bg);
}

static void st_text(int x, int y, const char *s, unsigned long c)
{
    draw_text(st_win, x, y, c, s);
}

static void st_small(int x, int y, const char *s, unsigned long c)
{
    draw_text_small(st_win, x, y, c, s);
}

static void st_bold(int x, int y, const char *s, unsigned long c)
{
    draw_text_bold(st_win, x, y, c, s);
}

static void st_clip(int x, int max_x, int y, unsigned long c, const char *s)
{
    draw_text_clip(st_win, x, max_x, y, c, s);
}

static void st_hit(int x, int y, int w, int h, int id)
{
    if (st_nhits >= ST_MAX_HITS)
        return;
    st_hits[st_nhits].x = x;
    st_hits[st_nhits].y = y;
    st_hits[st_nhits].w = w;
    st_hits[st_nhits].h = h;
    st_hits[st_nhits].id = id;
    st_nhits++;
}

static const StHit *st_hit_at(int x, int y)
{
    int i;

    for (i = 0; i < st_nhits; i++)
        if (x >= st_hits[i].x && x < st_hits[i].x + st_hits[i].w &&
            y >= st_hits[i].y && y < st_hits[i].y + st_hits[i].h)
            return &st_hits[i];
    return NULL;
}

static void st_card(int x, int y, int w, int h)
{
    ui_round_fill(st_win, x, y, w, h, 12, st_surface());
    ui_round_outline(st_win, x, y, w, h, 12, st_line());
}

static void st_icon(int page, int cx, int cy, int size, unsigned long fg)
{
    switch (page) {
    case ST_PAGE_PERSONALIZATION:
        ui_draw_sun(st_win, cx, cy, size, fg);
        break;
    case ST_PAGE_DISPLAY:
        ui_draw_chip(st_win, cx, cy, size, fg);
        break;
    case ST_PAGE_DESKTOP:
        ui_draw_house(st_win, cx, cy, size, fg);
        break;
    case ST_PAGE_INPUT:
        ui_draw_sliders(st_win, cx, cy, size, fg);
        break;
    case ST_PAGE_SOUND:
        ui_draw_speaker(st_win, cx, cy, size, fg);
        break;
    case ST_PAGE_APPLICATIONS:
        ui_draw_star(st_win, cx, cy, size, fg);
        break;
    case ST_PAGE_SYSTEM:
        ui_draw_cpu(st_win, cx, cy, size, fg);
        break;
    case ST_PAGE_SECURITY:
        ui_draw_shield(st_win, cx, cy, size, fg);
        break;
    case ST_PAGE_ANDROID:
        ui_draw_chip(st_win, cx, cy, size, fg);
        break;
    default:
        ui_draw_gear(st_win, cx, cy, size, fg);
        break;
    }
}

/* --------------------------------------------------------------- drawing */

static void st_title(void)
{
    int baseline = (ST_TITLE + sc.font_ascent - sc.font_descent) / 2;

    XSetForeground(sc.dpy, sc.gc,
                   ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL :
                          0xffffffUL, 5));
    XFillRectangle(sc.dpy, st_win, sc.gc, 0, 0, ST_W, ST_TITLE);
    ui_draw_traffic(st_win, 12, ST_TITLE / 2, 1, -1, 0, cfg.col_panel_bg);
    st_bold(80, baseline, "XENUS Settings", cfg.col_panel_fg);
    st_small(ST_W - 116, baseline, "Phase 1", st_muted());
}

static void st_sidebar(void)
{
    int x = 12;
    int y = ST_TITLE + 18;
    int w = ST_SIDE - 24;
    int i;
    unsigned long bg = st_sidebar_bg();

    ui_round_fill(st_win, 0, ST_TITLE, ST_SIDE, ST_H - ST_TITLE, 0, bg);
    st_small(x + 4, y + 10, "CATEGORIES", st_muted());
    y += 30;

    for (i = 0; i < ST_PAGE_COUNT; i++) {
        int active = i == st_page;
        int hover = st_hover == ST_ID_CATEGORY + i;
        int baseline = y + (ST_ROW_H + sc.font_ascent - sc.font_descent) / 2;
        unsigned long row_bg = active
            ? ui_mix(ui_accent(), bg, cfg.theme ? 22 : 30)
            : hover ? st_surface_hi() : bg;
        unsigned long fg = active || hover ? cfg.col_panel_fg : st_muted();

        ui_round_fill(st_win, x, y, w, ST_ROW_H, 8, row_bg);
        if (active || hover)
            ui_round_outline(st_win, x, y, w, ST_ROW_H, 8,
                             active ? ui_mix(ui_accent(), row_bg, 45)
                                    : st_line());
        if (active)
            ui_round_fill(st_win, x + 1, y + 9, 3, ST_ROW_H - 18, 2,
                          ui_accent());
        ui_round_fill(st_win, x + 16, y + ST_ROW_H / 2 - 2, 5, 5, 3,
                      active ? ui_accent()
                             : ui_mix(bg, cfg.col_panel_fg, 22));
        st_clip(x + 30, x + w - 8, baseline, fg, st_page_names[i]);
        st_hit(x, y, w, ST_ROW_H, ST_ID_CATEGORY + i);
        y += ST_ROW_H + ST_ROW_GAP;
    }

    st_small(x + 4, ST_H - 46, "Phase 1", st_muted());
    st_small(x + 4, ST_H - 30, "Navigation foundation", st_muted());
}

static void st_foundation_row(int x, int y, int w, const char *label,
                              const char *detail)
{
    ui_round_fill(st_win, x, y - 4, 7, 7, 3, ui_green());
    st_text(x + 16, y + 3, label, cfg.col_panel_fg);
    st_clip(x + 16 + text_width(label) + 14, x + w, y + 3, st_muted(),
            detail);
}

static void st_content(void)
{
    int x = ST_SIDE + ST_PAD;
    int y = ST_TITLE + 22;
    int w = ST_W - x - ST_PAD;
    int card_h = 176;
    char num[40];
    unsigned long acc = ui_accent();

    snprintf(num, sizeof(num), "CATEGORY %02d / %02d", st_page + 1,
             ST_PAGE_COUNT);
    st_small(x, y + 10, num, acc);
    st_bold(x, y + 38, st_page_names[st_page], cfg.col_panel_fg);
    st_small(x, y + 58, "XENUS Settings · Phase 1 foundation", st_muted());
    XSetForeground(sc.dpy, sc.gc, st_line());
    XDrawLine(sc.dpy, st_win, sc.gc, x, y + 74, x + w, y + 74);

    y += 94;
    st_card(x, y, w, card_h);
    {
        int tile = 48;
        const char *badge = "PLACEHOLDER";
        int bw = text_width_small(badge) + 20;
        int bx = x + w - bw - 18;

        ui_round_fill(st_win, x + 20, y + 24, tile, tile, 12,
                      ui_mix(ui_accent(), st_surface(),
                             cfg.theme ? 20 : 28));
        ui_round_outline(st_win, x + 20, y + 24, tile, tile, 12,
                         ui_mix(ui_accent(), st_surface(), 45));
        st_icon(st_page, x + 20 + tile / 2, y + 24 + tile / 2, 22, acc);

        st_bold(x + 86, y + 46, st_page_names[st_page], cfg.col_panel_fg);
        st_clip(x + 86, x + w - 24, y + 70, st_muted(),
                st_page_blurbs[st_page]);

        ui_round_fill(st_win, bx, y + 18, bw, 20, 10,
                      ui_mix(ui_accent(), st_surface(),
                             cfg.theme ? 16 : 22));
        st_small(bx + 10, y + 32, badge, acc);

        st_small(x + 20, y + card_h - 22,
                 "This page is intentionally read-only in Phase 1.",
                 st_muted());
    }

    y += card_h + 16;
    st_card(x, y, w, 150);
    st_small(x + 20, y + 24, "FOUNDATION", st_muted());
    st_foundation_row(x + 20, y + 48, w - 40, "Window lifecycle",
                      "Lazy-created, reused, mapped on demand");
    st_foundation_row(x + 20, y + 78, w - 40, "Navigation",
                      "Ten categories, keyboard and pointer");
    st_foundation_row(x + 20, y + 108, w - 40, "Event routing",
                      "Expose, motion, button, key and cleanup");

    st_small(x, ST_H - 28,
             "Esc closes  |  Up/Down navigates  |  Alt+I toggles Settings",
             st_muted());
}

static void st_draw(void)
{
    if (!st_win)
        return;
    st_nhits = 0;
    XSetForeground(sc.dpy, sc.gc, cfg.col_panel_bg);
    XFillRectangle(sc.dpy, st_win, sc.gc, 0, 0, ST_W, ST_H);
    st_title();
    st_sidebar();
    st_content();
    XFlush(sc.dpy);
}

/* --------------------------------------------------------------- actions */

static void st_select_page(int page)
{
    if (page < 0)
        page = ST_PAGE_COUNT - 1;
    if (page >= ST_PAGE_COUNT)
        page = 0;
    if (page == st_page)
        return;
    st_page = page;
    st_draw();
}

/* ---------------------------------------------------------------- public */

void settings_show(void)
{
    if (!st_win) {
        XSetWindowAttributes wa;

        wa.override_redirect = True;
        wa.background_pixel = cfg.col_panel_bg;
        wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                        ButtonReleaseMask | KeyPressMask;
        st_x = (sc.sw - ST_W) / 2;
        if (st_x < 0)
            st_x = 0;
        st_y = reserve_top() + 24;
        if (st_y + ST_H > sc.sh - reserve_bottom() - 8)
            st_y = sc.sh - reserve_bottom() - ST_H - 8;
        if (st_y < reserve_top())
            st_y = reserve_top();
        st_win = XCreateWindow(sc.dpy, sc.root, st_x, st_y, ST_W, ST_H, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask,
                               &wa);
        XStoreName(sc.dpy, st_win, "XENUS Settings");
        ui_shape_round(st_win, ST_W, ST_H, 14);
    }
    st_open = 1;
    st_hover = -1;
    XMapRaised(sc.dpy, st_win);
    focus_track(st_win);
    taskbar_register(st_win, TASK_APP_SETTINGS, "Settings");
    taskbar_set_hidden(st_win, 0);
    panel_update();
    log_msg("settings show mapped win=0x%lx", (unsigned long)st_win);
    XFlush(sc.dpy);
    st_draw();
}

void settings_hide(void)
{
    if (!st_open)
        return;
    st_open = 0;
    st_hover = -1;
    focus_drop(st_win);
    if (sc.focused && !sc.focused->minimized)
        focus_track(sc.focused->win);
    taskbar_register(st_win, TASK_APP_SETTINGS, "Settings");
    taskbar_set_hidden(st_win, 1);
    panel_update();
    XUnmapWindow(sc.dpy, st_win);
    XFlush(sc.dpy);
    log_msg("settings hide");
}

void settings_close(void)
{
    if (st_win == None)
        return;
    if (st_open) {
        st_open = 0;
        st_hover = -1;
        focus_drop(st_win);
        XUnmapWindow(sc.dpy, st_win);
        XFlush(sc.dpy);
    }
    taskbar_unregister(st_win);
    panel_update();
    log_msg("settings close win=0x%lx", (unsigned long)st_win);
}

void settings_toggle(void)
{
    if (st_open)
        settings_close();
    else
        settings_show();
}

int settings_open(void)
{
    return st_open;
}

Window settings_window(void)
{
    return st_win;
}

void settings_redraw(void)
{
    if (st_open)
        st_draw();
}

void settings_cleanup(void)
{
    if (st_win != None) {
        taskbar_unregister(st_win);
        XDestroyWindow(sc.dpy, st_win);
        st_win = None;
    }
    st_open = 0;
    st_hover = -1;
}

/* --------------------------------------------------------------- events */

int settings_expose(XExposeEvent *ev)
{
    if (!st_win || ev->window != st_win)
        return 0;
    if (ev->count == 0)
        st_draw();
    return 1;
}

int settings_motion(XMotionEvent *ev)
{
    int found = -1;
    const StHit *h;

    if (!st_open || ev->window != st_win)
        return 0;
    h = st_hit_at((int)ev->x, (int)ev->y);
    if (h)
        found = h->id;
    if (found != st_hover) {
        st_hover = found;
        st_draw();
    }
    return 1;
}

int settings_button(XButtonEvent *ev)
{
    const StHit *h;
    int x, y;
    Window child = None;

    if (!st_open)
        return 0;

    if (ev->window == st_win) {
        x = (int)ev->x;
        y = (int)ev->y;
    } else if (ev->window == sc.root) {
        if (!XTranslateCoordinates(sc.dpy, sc.root, st_win,
                                   ev->x_root, ev->y_root,
                                   &x, &y, &child))
            return 0;
    } else {
        return 0;
    }

    if (x < 0 || y < 0 || x >= ST_W || y >= ST_H)
        return 0;

    focus_track(st_win);

    if (ev->button == Button4 || ev->button == Button5) {
        st_select_page(st_page + (ev->button == Button4 ? -1 : 1));
        return 1;
    }
    if (ev->button != Button1)
        return 1;

    if (y < ST_TITLE) {
        int cell = 21;
        int left = 6;

        if (x >= left && x < left + 3 * cell) {
            int light = (x - left) / cell;

            if (light == UI_LIGHT_CLOSE)
                settings_close();
            else if (light == UI_LIGHT_MIN)
                settings_hide();
        } else {
            selfdrag_window(st_win, ev, &st_x, &st_y, ST_W, ST_H);
        }
        return 1;
    }

    h = st_hit_at(x, y);
    if (h && h->id >= ST_ID_CATEGORY &&
        h->id < ST_ID_CATEGORY + ST_PAGE_COUNT) {
        st_page = h->id - ST_ID_CATEGORY;
        st_draw();
    }
    return 1;
}

void settings_key(XKeyEvent *ev)
{
    char tmp[8];
    KeySym sym = NoSymbol;

    if (!st_open)
        return;
    XLookupString(ev, tmp, sizeof(tmp) - 1, &sym, NULL);
    switch (sym) {
    case XK_Escape:
        settings_close();
        break;
    case XK_Up:
    case XK_Left:
        st_select_page(st_page - 1);
        break;
    case XK_Down:
    case XK_Right:
        st_select_page(st_page + 1);
        break;
    case XK_Tab:
        st_select_page(st_page + ((ev->state & ShiftMask) ? -1 : 1));
        break;
    case XK_ISO_Left_Tab:
        st_select_page(st_page - 1);
        break;
    case XK_Home:
        st_select_page(0);
        break;
    case XK_End:
        st_select_page(ST_PAGE_COUNT - 1);
        break;
    default:
        break;
    }
}
