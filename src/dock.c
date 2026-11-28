#include "xenus.h"
#include "motion.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define DOCK_MARGIN  8
#define DOCK_PAD     10
#define DOCK_TILE    50
#define DOCK_GAP     8
#define DOCK_H       72
#define DOCK_CUT     18
#define DOCK_DOT_Y   64
#define DOCK_MAX     (MAX_PANEL_BTNS + 8)
#define PIN_COUNT    5

#define PIN_LAUNCHER (-2)
#define PIN_FILES    (-3)
#define PIN_TERM     (-4)
#define PIN_BROWSER  (-5)
#define PIN_SECURITY (-6)

typedef struct {
    int x;
    int task;
    int pin;
    int kind;
    int count;                  /* windows in this application group */
} DockItem;

static DockItem ditems[DOCK_MAX];
static int nditems;
static int dock_x, dock_y, dock_w;
static int hover_dock = -1;
static int hover_anim;
static int hover_from, hover_to;
static unsigned long corner_bg;

static void dock_anim_to(int target);

static void app_initial(const char *title, char *out, size_t out_size)
{
    const char *p;

    if (out_size == 0)
        return;
    out[0] = '\0';
    if (!title)
        return;
    for (p = title; *p; p++) {
        if (isalnum((unsigned char)*p))
            break;
    }
    if (*p == '\0') {
        out[0] = '?';
        out[1] = '\0';
        return;
    }
    out[0] = (char)toupper((unsigned char)*p);
    out[1] = '\0';
}

static void dock_layout(void)
{
    static const int pins[PIN_COUNT] = {
        PIN_LAUNCHER, PIN_FILES, PIN_TERM, PIN_BROWSER, PIN_SECURITY
    };
    int n, shown, max_shown, w, i;

    taskbar_sync();
    n = taskbar_count();
    max_shown = (sc.sw - 120) / (DOCK_TILE + DOCK_GAP);
    if (max_shown < PIN_COUNT + 1)
        max_shown = PIN_COUNT;
    shown = n;
    if (shown > max_shown - PIN_COUNT)
        shown = max_shown - PIN_COUNT;
    if (shown < 0)
        shown = 0;
    if (shown > DOCK_MAX - PIN_COUNT)
        shown = DOCK_MAX - PIN_COUNT;

    nditems = 0;
    for (i = 0; i < PIN_COUNT; i++) {
        ditems[nditems].pin = pins[i];
        ditems[nditems].task = -1;
        ditems[nditems].kind = UI_ICON_GENERIC;
        ditems[nditems].count = 1;
        nditems++;
    }
    for (i = 0; i < shown; i++) {
        char key[96], other[96];
        int j, dup = 0, rep = i, count = 0;

        if (taskbar_app_key(i, key, sizeof(key)) != 0)
            continue;
        for (j = 0; j < i; j++) {
            if (taskbar_app_key(j, other, sizeof(other)) == 0 &&
                strcmp(key, other) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        for (j = 0; j < shown; j++) {
            const TaskEntry *t;

            if (taskbar_app_key(j, other, sizeof(other)) != 0 ||
                strcmp(key, other) != 0)
                continue;
            t = taskbar_at(j);
            if (!t)
                continue;
            count++;
            if (t->focused)
                rep = j;
        }
        if (count == 0)
            count = 1;
        if (nditems >= DOCK_MAX)
            break;
        ditems[nditems].pin = -1;
        ditems[nditems].task = rep;
        ditems[nditems].kind = UI_ICON_GENERIC;
        ditems[nditems].count = count;
        nditems++;
    }

    w = 2 * DOCK_PAD + nditems * DOCK_TILE +
        (nditems > 1 ? (nditems - 1) * DOCK_GAP : 0);
    {
        int tx = DOCK_PAD;

        for (i = 0; i < nditems; i++) {
            ditems[i].x = tx;
            tx += DOCK_TILE + DOCK_GAP;
        }
    }
    dock_w = w;
    dock_x = (sc.sw - w) / 2;
    dock_y = sc.sh - DOCK_MARGIN - DOCK_H;
    if (dock_x < 4)
        dock_x = 4;
    corner_bg = ui_wallpaper_color(dock_y + DOCK_H - 1);
}

int dock_reserved(void)
{
    return DOCK_H + DOCK_MARGIN;
}

static int pin_active(int pin)
{
    int i, n = taskbar_count();

    for (i = 0; i < n; i++) {
        const TaskEntry *t = taskbar_at(i);

        if (!t || !t->focused)
            continue;
        if (pin == PIN_FILES && t->app_type == TASK_APP_FM)
            return 1;
        if (pin == PIN_SECURITY && t->app_type == TASK_APP_SECURITY)
            return 1;
    }
    return 0;
}

static int pin_running(int pin)
{
    int i, n = taskbar_count();

    for (i = 0; i < n; i++) {
        const TaskEntry *t = taskbar_at(i);

        if (!t)
            continue;
        if (pin == PIN_FILES && t->app_type == TASK_APP_FM)
            return 1;
        if (pin == PIN_SECURITY && t->app_type == TASK_APP_SECURITY)
            return 1;
    }
    return 0;
}

static int pin_hidden(int pin)
{
    int i, n = taskbar_count();

    for (i = 0; i < n; i++) {
        const TaskEntry *t = taskbar_at(i);

        if (!t)
            continue;
        if (pin == PIN_FILES && t->app_type == TASK_APP_FM)
            return t->hidden;
        if (pin == PIN_SECURITY && t->app_type == TASK_APP_SECURITY)
            return t->hidden;
    }
    return 0;
}

static int dock_item_closable(int i)
{
    if (i < 0 || i >= nditems)
        return 0;
    if (ditems[i].pin == -1)
        return 1;
    if (ditems[i].pin == PIN_FILES)
        return pin_running(PIN_FILES);
    if (ditems[i].pin == PIN_SECURITY)
        return pin_running(PIN_SECURITY);
    return 0;
}

static void dock_close_pos(int i, int *cx, int *cy)
{
    int size = (i == hover_dock) ? DOCK_TILE + hover_anim : DOCK_TILE;
    int tx = ditems[i].x - (size - DOCK_TILE) / 2;
    int ty = DOCK_H - 8 - size;

    *cx = tx + size - 8;
    *cy = ty + 8;
}

static int dock_close_hit(int i, int x, int y)
{
    int cx, cy, dx, dy;

    dock_close_pos(i, &cx, &cy);
    dx = x - cx;
    dy = y - cy;
    return dx * dx + dy * dy <= 81;
}

static void dock_close_item(int i)
{
    if (!dock_item_closable(i))
        return;
    if (ditems[i].pin == -1)
        taskbar_close(ditems[i].task);
    else if (ditems[i].pin == PIN_FILES)
        fm_close();
    else if (ditems[i].pin == PIN_SECURITY)
        security_gui_close();
}

static void dock_context_menu(int i, int x, int y)
{
    static MenuEntry items[8];
    int n = 0;
    int task = -1;

    if (i < 0 || i >= nditems)
        return;
    if (ditems[i].pin == -1)
        task = ditems[i].task;
    else if (ditems[i].pin == PIN_FILES)
        task = taskbar_find_type(TASK_APP_FM);
    else if (ditems[i].pin == PIN_SECURITY)
        task = taskbar_find_type(TASK_APP_SECURITY);
    if (task < 0)
        return;

#define DOCK_ADD(L, action) do { \
        str_copy(items[n].label, sizeof(items[0].label), (L)); \
        if ((action)[0] == '\0') \
            items[n].cmd[0] = '\0'; \
        else \
            snprintf(items[n].cmd, sizeof(items[0].cmd), \
                     "@wm-task %s %d", (action), task); \
        n++; \
    } while (0)
    DOCK_ADD("Show", "show");
    DOCK_ADD("Minimize", "minimize");
    DOCK_ADD("Maximize", "maximize");
    DOCK_ADD("-", "");
    DOCK_ADD("Close", "close");
    DOCK_ADD("Quit", "quit");
#undef DOCK_ADD
    menu_show_custom(items, n, x, y);
}

static void dock_paint(void)
{
    int i;
    XSetForeground(sc.dpy, sc.gc, corner_bg);
    XFillRectangle(sc.dpy, sc.dock, sc.gc, 0, 0, (unsigned)dock_w,
                   (unsigned)DOCK_H);

    ui_round_fill(sc.dock, 0, 0, dock_w, DOCK_H, DOCK_CUT,
                  ui_mix(cfg.col_panel_bg, 0xffffffUL, 9));
    ui_round_outline(sc.dock, 0, 0, dock_w, DOCK_H, DOCK_CUT,
                     ui_mix(cfg.col_panel_bg, 0xffffffUL, 20));
    XSetForeground(sc.dpy, sc.gc, ui_mix(cfg.col_panel_bg, 0xffffffUL, 26));
    XDrawLine(sc.dpy, sc.dock, sc.gc, DOCK_CUT, 1, dock_w - DOCK_CUT, 1);
    XSetForeground(sc.dpy, sc.gc, ui_mix(cfg.col_panel_bg, 0x000000UL, 48));
    XDrawLine(sc.dpy, sc.dock, sc.gc, DOCK_CUT, DOCK_H - 2,
              dock_w - DOCK_CUT, DOCK_H - 2);

    for (i = 0; i < nditems; i++) {
        int hovered = (i == hover_dock);
        int size = hovered ? DOCK_TILE + hover_anim : DOCK_TILE;
        int tx = ditems[i].x - (size - DOCK_TILE) / 2;
        int ty = DOCK_H - 8 - size;
        int running = 0, hidden = 0, focused = 0;
        const TaskEntry *t = NULL;
        const char *title = NULL;
        char initial[2];

        if (ditems[i].pin != -1) {
            int pin = ditems[i].pin;

            if (pin == PIN_LAUNCHER) {
                ui_draw_app_icon(sc.dock, UI_ICON_LAUNCHER, tx, ty, size, 0,
                                 "S");
                continue;
            }
            if (pin == PIN_FILES) {
                running = pin_running(pin);
                hidden = pin_hidden(pin);
                focused = pin_active(pin);
                ui_draw_app_icon(sc.dock, UI_ICON_FILES, tx, ty, size,
                                 hidden, "F");
            } else if (pin == PIN_TERM) {
                ui_draw_app_icon(sc.dock, UI_ICON_TERMINAL, tx, ty, size, 0,
                                 "T");
            } else if (pin == PIN_BROWSER) {
                ui_draw_app_icon(sc.dock, UI_ICON_BROWSER, tx, ty, size, 0,
                                 "B");
            } else {
                running = pin_running(PIN_SECURITY);
                hidden = pin_hidden(PIN_SECURITY);
                focused = pin_active(PIN_SECURITY);
                ui_draw_app_icon(sc.dock, UI_ICON_SECURITY, tx, ty, size,
                                 hidden, "S");
            }
        } else {
            t = taskbar_at(ditems[i].task);
            if (!t)
                continue;
            running = 1;
            taskbar_group_state(ditems[i].task, NULL, &hidden, &focused);
            title = t->title;
            app_initial(title, initial, sizeof(initial));
            ui_draw_app_icon(sc.dock, UI_ICON_GENERIC, tx, ty, size,
                             hidden, initial[0] ? initial : "?");
            if (ditems[i].count > 1) {
                char cnt[16];
                int bw;

                snprintf(cnt, sizeof(cnt), "%d", ditems[i].count);
                bw = 8 + text_width_small(cnt);
                if (bw < 16)
                    bw = 16;
                ui_round_fill(sc.dock, tx + size - bw, ty + 1, bw, 15, 7,
                              ui_accent());
                draw_text_small(sc.dock, tx + size - bw +
                                (bw - text_width_small(cnt)) / 2,
                                ty + 12, 0xffffffUL, cnt);
            }
        }

        if (running) {
            unsigned long dot = focused ? ui_accent()
                                        : ui_mix(cfg.col_panel_fg,
                                                 cfg.col_panel_bg, 28);
            int dots = ditems[i].count > 0 ? ditems[i].count : 1;
            int ds = focused ? 5 : 4;
            int k, span;

            if (dots > 4)
                dots = 4;
            if (hidden)
                dot = ui_mix(dot, cfg.col_panel_bg, 55);
            span = dots * 6 - 2;
            for (k = 0; k < dots; k++) {
                int dx = ditems[i].x + DOCK_TILE / 2 - span / 2 + k * 6;

                XSetForeground(sc.dpy, sc.gc, dot);
                XFillArc(sc.dpy, sc.dock, sc.gc, dx, DOCK_DOT_Y,
                         (unsigned)ds, (unsigned)ds, 0, 360 * 64);
            }
        }

        if (hovered && dock_item_closable(i)) {
            int ccx, ccy;

            dock_close_pos(i, &ccx, &ccy);
            ui_round_fill(sc.dock, ccx - 8, ccy - 8, 16, 16, 8, 0xe0443eUL);
            XSetForeground(sc.dpy, sc.gc, 0xffffffUL);
            XDrawLine(sc.dpy, sc.dock, sc.gc, ccx - 4, ccy - 4,
                      ccx + 4, ccy + 4);
            XDrawLine(sc.dpy, sc.dock, sc.gc, ccx + 4, ccy - 4,
                      ccx - 4, ccy + 4);
        }
    }
    XFlush(sc.dpy);
}

void dock_create(void)
{
    XSetWindowAttributes wa;

    hover_dock = -1;
    hover_anim = 0;
    hover_from = 0;
    hover_to = 0;
    dock_layout();
    wa.override_redirect = True;
    wa.background_pixel = corner_bg;
    wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                    LeaveWindowMask;

    sc.dock = XCreateWindow(sc.dpy, sc.root, dock_x, dock_y,
                            (unsigned)dock_w, (unsigned)DOCK_H, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel | CWEventMask,
                            &wa);
    XStoreName(sc.dpy, sc.dock, "xenus-dock");
    ui_shape_round(sc.dock, dock_w, DOCK_H, DOCK_CUT);
    XMapRaised(sc.dpy, sc.dock);
    dock_paint();
}

void dock_destroy(void)
{
    if (sc.dock) {
        XDestroyWindow(sc.dpy, sc.dock);
        sc.dock = None;
    }
}

void dock_update(void)
{
    int old_x = dock_x, old_y = dock_y, old_w = dock_w;
    unsigned long old_corner = corner_bg;

    if (!sc.dock)
        return;
    dock_layout();
    if (hover_dock >= nditems)
        hover_dock = -1;
    if (dock_x != old_x || dock_y != old_y || dock_w != old_w) {
        XMoveResizeWindow(sc.dpy, sc.dock, dock_x, dock_y, (unsigned)dock_w,
                          (unsigned)DOCK_H);
        ui_shape_round(sc.dock, dock_w, DOCK_H, DOCK_CUT);
        if (corner_bg != old_corner)
            XSetWindowBackground(sc.dpy, sc.dock, corner_bg);
    }
    dock_paint();
    XFlush(sc.dpy);
}

void dock_raise(void)
{
    if (sc.dock)
        XRaiseWindow(sc.dpy, sc.dock);
}

void dock_resample_bg(void)
{
    dock_update();
}

void dock_expose(XExposeEvent *ev)
{
    if (!sc.dock || ev->window != sc.dock)
        return;
    if (ev->count == 0)
        dock_update();
}

static int dock_hit(int x)
{
    int i;

    for (i = 0; i < nditems; i++) {
        if (x >= ditems[i].x && x < ditems[i].x + DOCK_TILE)
            return i;
    }
    return -1;
}

static void pin_activate(int pin, int anchor_y)
{
    switch (pin) {
    case PIN_LAUNCHER:
        if (menu_is_open())
            menu_hide();
        else
            menu_show_above_dock(anchor_y);
        break;
    case PIN_FILES:
        fm_show();
        break;
    case PIN_TERM:
        apps_run_terminal("terminal");
        break;
    case PIN_BROWSER:
        apps_run("browser", apps_browser());
        break;
    case PIN_SECURITY:
        security_gui_show();
        break;
    default:
        break;
    }
}


int dock_button_press(XButtonEvent *ev)
{
    int i;

    if (!sc.dock || ev->window != sc.dock)
        return 0;
    if (ev->button == Button4 || ev->button == Button5)
        return 1;

    i = dock_hit(ev->x);
    if (i < 0)
        return 1;

    if (ev->button == Button1 && hover_dock == i &&
        dock_close_hit(i, ev->x, ev->y) && dock_item_closable(i)) {
        dock_close_item(i);
        return 1;
    }

    if (ev->button == Button3) {
        dock_context_menu(i, ev->x_root, ev->y_root);
        return 1;
    }

    if (ditems[i].pin != -1) {
        int pin = ditems[i].pin;

        if (ev->button == Button1)
            pin_activate(pin, dock_y);
        else if (ev->button == Button2 &&
                 (pin == PIN_FILES || pin == PIN_SECURITY)) {
            if (pin == PIN_FILES)
                fm_close();
            else
                security_gui_close();
        }
        return 1;
    }

    if (ev->button == Button1)
        taskbar_activate(ditems[i].task);
    else if (ev->button == Button2)
        taskbar_close(ditems[i].task);
    return 1;
}

void dock_motion(XMotionEvent *ev)
{
    int found;

    if (!sc.dock || ev->window != sc.dock)
        return;
    found = dock_hit(ev->x);
    if (found != hover_dock) {
        hover_dock = found;
        dock_anim_to(found >= 0 ? 8 : 0);
        dock_update();
    }
}

void dock_leave(void)
{
    if (hover_dock != -1) {
        hover_dock = -1;
        dock_anim_to(0);
        dock_update();
    }
}

/* --------------------------------------------------------- dock motion -- */

static int dock_anim_target(void)
{
    return hover_dock >= 0 ? 8 : 0;
}

static void dock_anim_step(void *ctx, float t)
{
    (void)ctx;
    hover_anim = motion_lerp(hover_from, hover_to, t);
    dock_update();
}

static void dock_anim_to(int target)
{
    if (!sc.dock)
        return;
    if (!cfg.animations) {
        motion_cancel(MOTION_CH_DOCK, &hover_anim, 0);
        if (hover_anim != target) {
            hover_anim = target;
            dock_update();
        }
        return;
    }
    if (hover_anim == target)
        return;
    hover_from = hover_anim;
    hover_to = target;
    if (!motion_start(MOTION_CH_DOCK, &hover_anim, 140, MOTION_EASE_OUT,
                      dock_anim_step, NULL, NULL)) {
        /* scheduler full: snap instead of leaving the tile mid-grow */
        hover_anim = target;
        dock_update();
    }
}

void dock_tick(void)
{
    /* hover motion is driven by the shared motion engine */
}

int dock_app_anchor(const char *app_key, int *cx, int *cy, int *w, int *h)
{
    char other[160];
    int i, found = 0;

    if (w)
        *w = 42;
    if (h)
        *h = 42;

    taskbar_sync();
    /*
     * Refresh the dock through its public update path: dock_layout() alone
     * would leave dock_w/dock_x advanced while the real X window still has
     * the old geometry, so the next dock_update() would think nothing
     * changed and never resize it.
     */
    dock_update();

    if (app_key && app_key[0]) {
        for (i = 0; i < nditems; i++) {
            if (ditems[i].task < 0)
                continue;
            if (taskbar_app_key(ditems[i].task, other, sizeof(other)) != 0 ||
                strcmp(app_key, other) != 0)
                continue;
            if (cx)
                *cx = dock_x + ditems[i].x + DOCK_TILE / 2;
            if (cy)
                *cy = dock_y + DOCK_H / 2;
            found = 1;
            break;
        }
    }

    if (!found) {
        if (cx)
            *cx = sc.dock ? dock_x + dock_w / 2 : sc.sw / 2;
        if (cy)
            *cy = sc.dock ? dock_y + DOCK_H / 2 : sc.sh - DOCK_H;
    }
    return found;
}

int dock_client_anchor(const Client *c, int *cx, int *cy, int *w, int *h)
{
    char key[160];

    if (c && c->app_class[0])
        snprintf(key, sizeof(key), "app:%s", c->app_class);
    else if (c && c->app_instance[0])
        snprintf(key, sizeof(key), "instance:%s", c->app_instance);
    else if (c)
        snprintf(key, sizeof(key), "window:%s", c->name);
    else
        key[0] = '\0';

    return dock_app_anchor(key, cx, cy, w, h);
}

int dock_anim_timeout_ms(void)
{
    return -1;
}

void dock_animations_changed(void)
{
    if (!sc.dock)
        return;
    if (!cfg.animations) {
        motion_cancel(MOTION_CH_DOCK, &hover_anim, 0);
        hover_anim = dock_anim_target();
        dock_update();
    } else {
        dock_anim_to(dock_anim_target());
    }
}
