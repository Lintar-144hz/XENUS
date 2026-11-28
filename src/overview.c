/*
 * XENUS Overview - live window zoom-out / zoom-in.
 *
 * The shell has no compositor, so this is deliberately honest: client
 * windows are mapped, moved and resized on screen; there is no texture
 * scaling or alpha blending.  The result is a Mission-Control style grid
 * built from the real windows.  Only managed clients are included.
 */
#include "xenus.h"
#include "motion.h"

#include <stdio.h>
#include <string.h>

#define OV_MAX 64
#define OV_GAP 18
#define OV_LABEL_H 20

typedef struct {
    Client *c;
    int sx, sy, sw, sh;         /* animation start */
    int dx, dy, dw, dh;         /* animation target */
    int cx, cy, cw, ch;         /* current, updated by the step */
    int child_w, child_h;       /* last child size we pushed */
    int closing;                /* close tween in flight */
} OvItem;

static Window ov_bg;
static int ov_open;
static OvItem ov_items[OV_MAX];
static int ov_n;
static int ov_pending;

/* Tween callbacks carry the Client pointer, never an OvItem pointer: the
   item array is compacted when a window disappears, so indices are not
   stable for the lifetime of an animation. */
static OvItem *ov_item_for(Client *c)
{
    int i;

    for (i = 0; i < ov_n; i++)
        if (ov_items[i].c == c)
            return &ov_items[i];
    return NULL;
}

static void ov_paint(void)
{
    int i;

    if (ov_bg == None || !ov_open)
        return;
    XSetForeground(sc.dpy, sc.gc,
                   ui_mix(cfg.col_background, 0x000000UL, 28));
    XFillRectangle(sc.dpy, ov_bg, sc.gc, 0, 0, (unsigned)sc.sw,
                   (unsigned)sc.sh);

    draw_text_bold(ov_bg, 40, reserve_top() + 34, cfg.col_panel_fg,
                   "Overview");
    draw_text_small(ov_bg, 40 + text_width_bold("Overview") + 14,
                    reserve_top() + 34,
                    ui_dim(cfg.col_panel_fg, cfg.col_background),
                    "click a window to focus · Esc to close");

    for (i = 0; i < ov_n; i++) {
        OvItem *it = &ov_items[i];
        char label[NAME_MAX_LEN + 32];
        int lw, lx;
        int label_y = it->cy + it->ch + 4;

        if (it->c->workspace != sc.workspace)
            snprintf(label, sizeof(label), "%s · %s", it->c->name,
                     workspace_name(it->c->workspace));
        else
            snprintf(label, sizeof(label), "%s", it->c->name);
        lw = text_width_small(label);
        lx = it->cx + (it->cw - lw) / 2;
        if (lx < it->cx)
            lx = it->cx;
        draw_text_clip_font(ov_bg, sc.font_small, lx,
                            it->cx + it->cw, label_y,
                            it->c->workspace == sc.workspace
                                ? cfg.col_panel_fg
                                : ui_dim(cfg.col_panel_fg,
                                         cfg.col_background),
                            label);
    }
    XFlush(sc.dpy);
}

static void ov_create_background(void)
{
    XSetWindowAttributes wa;

    if (ov_bg != None)
        return;
    memset(&wa, 0, sizeof(wa));
    wa.override_redirect = True;
    wa.background_pixel = ui_mix(cfg.col_background, 0x000000UL, 28);
    wa.event_mask = ExposureMask | ButtonPressMask;
    ov_bg = XCreateWindow(sc.dpy, sc.root, 0, 0, (unsigned)sc.sw,
                          (unsigned)sc.sh, 0, CopyFromParent, InputOutput,
                          CopyFromParent,
                          CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
    XStoreName(sc.dpy, ov_bg, "xenus-overview");
}

static void ov_push_child(OvItem *it)
{
    Client *c = it->c;
    int cw = it->cw;
    int ch = it->ch - c->title_h;

    if (cw < 8)
        cw = 8;
    if (ch < 8)
        ch = 8;
    if (cw == it->child_w && ch == it->child_h)
        return;
    it->child_w = cw;
    it->child_h = ch;
    XMoveResizeWindow(sc.dpy, c->win, 0, c->title_h, (unsigned)cw,
                      (unsigned)ch);
}

static void ov_step(void *ctx, float t)
{
    Client *c = ctx;
    OvItem *it = ov_item_for(c);

    if (!it || c->frame == None)
        return;
    it->cx = motion_lerp(it->sx, it->dx, t);
    it->cy = motion_lerp(it->sy, it->dy, t);
    it->cw = motion_lerp(it->sw, it->dw, t);
    it->ch = motion_lerp(it->sh, it->dh, t);
    if (it->cw < 28)
        it->cw = 28;
    if (it->ch < 24)
        it->ch = 24;
    XMoveResizeWindow(sc.dpy, c->frame, it->cx, it->cy,
                      (unsigned)it->cw, (unsigned)it->ch);
    ui_shape_round(c->frame, it->cw, it->ch, 9);
    ov_push_child(it);
}

static void ov_open_done(void *ctx)
{
    Client *c = ctx;
    OvItem *it = ov_item_for(c);

    if (!it)
        return;
    it->cx = it->dx;
    it->cy = it->dy;
    it->cw = it->dw;
    it->ch = it->dh;
    ov_push_child(it);
    if (c->frame != None)
        frame_paint(c);
}

static void ov_hide_background(void)
{
    if (ov_bg != None)
        XUnmapWindow(sc.dpy, ov_bg);
    ov_n = 0;
    ov_pending = 0;
    XFlush(sc.dpy);
}

/* restore one window to its resting workspace geometry */
static void ov_settle_item(OvItem *it)
{
    Client *c = it->c;

    it->closing = 0;
    if (!client_find(c->win))
        return;
    frame_sync(c);
    frame_paint(c);
    if (c->workspace != sc.workspace) {
        c->ws_hidden = 1;
        XUnmapWindow(sc.dpy, c->frame);
    } else {
        c->ws_hidden = 0;
        XMapWindow(sc.dpy, c->frame);
    }
}

static void ov_close_done(void *ctx)
{
    Client *c = ctx;
    OvItem *it = ov_item_for(c);

    if (it)
        ov_settle_item(it);
    if (ov_pending > 0)
        ov_pending--;
    if (ov_pending == 0)
        ov_hide_background();
}

int overview_is_open(void)
{
    return ov_open;
}

void overview_init(void)
{
    ov_bg = None;
    ov_open = 0;
    ov_n = 0;
    ov_pending = 0;
}

static int ov_cols(int n)
{
    int c = 1;

    while (c * c < n)
        c++;
    return c;
}

void overview_toggle(void)
{
    int i, n = 0, cols, rows, gap = OV_GAP, label_h = OV_LABEL_H;
    int area_x = 40, area_y, area_w, area_h;
    int cell_w, cell_h, thumb_h;
    Client *c;

    if (ov_open) {
        overview_close();
        return;
    }
    if (menu_is_open())
        menu_hide();
    if (cmd_is_open())
        cmd_hide();

    for (c = sc.clients; c; c = c->next) {
        if (c->minimized || c->frame == None)
            continue;
        if (n >= OV_MAX)
            break;
        ov_items[n].c = c;
        n++;
    }
    if (n == 0)
        return;

    ov_n = n;
    ov_open = 1;
    ov_pending = 0;
    ov_create_background();

    area_y = reserve_top() + 52;
    area_w = sc.sw - 2 * area_x;
    area_h = sc.sh - area_y - reserve_bottom() - 52;
    if (area_w < 200)
        area_w = 200;
    if (area_h < 160)
        area_h = 160;
    cols = ov_cols(n);
    rows = (n + cols - 1) / cols;
    cell_w = (area_w - gap * (cols - 1)) / cols;
    cell_h = (area_h - gap * (rows - 1)) / rows;
    if (cell_w < 80)
        cell_w = 80;
    if (cell_h < 70)
        cell_h = 70;
    thumb_h = cell_h - label_h;
    if (thumb_h < 50)
        thumb_h = 50;

    XMoveResizeWindow(sc.dpy, ov_bg, 0, 0, (unsigned)sc.sw,
                      (unsigned)sc.sh);
    XMapRaised(sc.dpy, ov_bg);
    XGrabKeyboard(sc.dpy, ov_bg, True, GrabModeAsync, GrabModeAsync,
                  CurrentTime);

    for (i = 0; i < n; i++) {
        OvItem *it = &ov_items[i];
        int col = i % cols;
        int row = i / cols;
        int cell_x = area_x + col * (cell_w + gap);
        int cell_y = area_y + row * (cell_h + gap);
        float scale = 1.0f;
        float sx, sy;
        int fw, fh;

        c = it->c;
        fw = frame_w(c);
        fh = frame_h(c);
        if (fw < 2)
            fw = 2;
        if (fh < 2)
            fh = 2;
        sx = (float)(cell_w - 12) / (float)fw;
        sy = (float)(thumb_h - 8) / (float)fh;
        scale = sx < sy ? sx : sy;
        if (scale > 1.0f)
            scale = 1.0f;
        it->dw = (int)((float)fw * scale);
        it->dh = (int)((float)fh * scale);
        if (it->dw < 48)
            it->dw = 48;
        if (it->dh < 40)
            it->dh = 40;
        it->dx = cell_x + (cell_w - it->dw) / 2;
        it->dy = cell_y + (thumb_h - it->dh) / 2;
        it->sx = it->cx = c->x;
        it->sy = it->cy = c->y;
        it->sw = it->cw = fw;
        it->sh = it->ch = fh;
        it->child_w = c->w;
        it->child_h = c->h;
        it->closing = 0;

        if (it->sx != it->dx || it->sy != it->dy ||
            it->sw != it->dw || it->sh != it->dh) {
            XMapWindow(sc.dpy, c->frame);
        }
        XRaiseWindow(sc.dpy, c->frame);
        if (!cfg.animations ||
            !motion_start(MOTION_CH_OVERVIEW, c, 230, MOTION_EASE_OUT,
                          ov_step, ov_open_done, c)) {
            it->cx = it->dx;
            it->cy = it->dy;
            it->cw = it->dw;
            it->ch = it->dh;
            XMoveResizeWindow(sc.dpy, c->frame, it->cx, it->cy,
                              (unsigned)it->cw, (unsigned)it->ch);
            ui_shape_round(c->frame, it->cw, it->ch, 9);
            ov_push_child(it);
            frame_paint(c);
        }
    }

    ov_paint();
    panel_raise();
    dock_raise();
    XFlush(sc.dpy);
}

void overview_close(void)
{
    int i;

    if (!ov_open)
        return;
    ov_open = 0;
    XUngrabKeyboard(sc.dpy, CurrentTime);
    ov_pending = 0;
    for (i = 0; i < ov_n; i++) {
        OvItem *it = &ov_items[i];
        Client *c = it->c;

        if (!client_find(c->win))
            continue;
        it->sx = it->cx;
        it->sy = it->cy;
        it->sw = it->cw;
        it->sh = it->ch;
        it->dx = c->x;
        it->dy = c->y;
        it->dw = frame_w(c);
        it->dh = frame_h(c);
        if (!cfg.animations ||
            !motion_start(MOTION_CH_OVERVIEW, c, 200, MOTION_EASE_OUT,
                          ov_step, ov_close_done, c)) {
            ov_settle_item(it);
            continue;
        }
        it->closing = 1;
        ov_pending++;
    }
    if (ov_pending == 0)
        ov_hide_background();
}

void overview_close_now(void)
{
    int i;

    if (!ov_open && ov_pending == 0 && ov_n == 0)
        return;
    ov_open = 0;
    XUngrabKeyboard(sc.dpy, CurrentTime);
    for (i = 0; i < ov_n; i++) {
        OvItem *it = &ov_items[i];
        Client *c = it->c;

        motion_cancel(MOTION_CH_OVERVIEW, c, 0);
        it->closing = 0;
        if (!client_find(c->win))
            continue;
        frame_sync(c);
        frame_paint(c);
        if (c->workspace != sc.workspace) {
            c->ws_hidden = 1;
            XUnmapWindow(sc.dpy, c->frame);
        } else {
            c->ws_hidden = 0;
            XMapWindow(sc.dpy, c->frame);
        }
    }
    ov_pending = 0;
    ov_hide_background();
}

void overview_select(Client *c)
{
    if (!c)
        return;
    if (!ov_open) {
        focus_client(c);
        return;
    }
    overview_close_now();
    if (c->workspace != sc.workspace)
        workspace_switch(c->workspace);
    if (c->minimized)
        client_minimize(c, 0);
    focus_client(c);
}

void overview_forget(Client *c)
{
    int i, j;

    if (!c)
        return;
    for (i = 0; i < ov_n; i++) {
        if (ov_items[i].c != c)
            continue;
        /* unmanage already cancelled the close tween, so its done
           callback will never run: retire its pending slot here */
        if (ov_items[i].closing && ov_pending > 0)
            ov_pending--;
        for (j = i; j + 1 < ov_n; j++)
            ov_items[j] = ov_items[j + 1];
        ov_n--;
        if (ov_open) {
            if (ov_n == 0) {
                ov_open = 0;
                XUngrabKeyboard(sc.dpy, CurrentTime);
                ov_hide_background();
            } else {
                ov_paint();
            }
        } else if (ov_pending == 0) {
            ov_hide_background();
        }
        return;
    }
}

int overview_expose(XExposeEvent *ev)
{
    if (ov_bg == None || ev->window != ov_bg)
        return 0;
    if (ev->count == 0)
        ov_paint();
    return 1;
}

int overview_button(XButtonEvent *ev)
{
    if (ov_bg == None || ev->window != ov_bg)
        return 0;
    if (ev->button == Button1 || ev->button == Button3) {
        if (ov_open)
            overview_close();
        return 1;
    }
    return 1;
}

void overview_cleanup(void)
{
    int i;

    XUngrabKeyboard(sc.dpy, CurrentTime);
    for (i = 0; i < ov_n; i++)
        motion_cancel(MOTION_CH_OVERVIEW, ov_items[i].c, 0);
    if (ov_bg != None) {
        XDestroyWindow(sc.dpy, ov_bg);
        ov_bg = None;
    }
    ov_open = 0;
    ov_n = 0;
    ov_pending = 0;
}
