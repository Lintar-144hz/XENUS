/*
 * XENUS - window management (reparented frames with a calm Harbor title bar)
 */
#include "xenus.h"
#include "motion.h"

#include <xenus/notification.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------- basics */

Client *client_find(Window w)
{
    Client *c;

    for (c = sc.clients; c; c = c->next)
        if (c->win == w)
            return c;
    return NULL;
}

Client *client_from_frame(Window frame)
{
    Client *c;

    for (c = sc.clients; c; c = c->next)
        if (c->frame == frame)
            return c;
    return NULL;
}

int client_count(void)
{
    Client *c;
    int n = 0;

    for (c = sc.clients; c; c = c->next)
        n++;
    return n;
}

int frame_w(Client *c)
{
    return c->w;
}

int frame_h(Client *c)
{
    return c->title_h + c->h;
}

static void set_wm_state(Window w, long state)
{
    unsigned long data[2];

    data[0] = (unsigned long)state;
    data[1] = 0;
    XChangeProperty(sc.dpy, w, sc.atom_wm_state, sc.atom_wm_state, 32,
                    PropModeReplace, (unsigned char *)data, 2);
}

void frame_sync(Client *c)
{
    if (c->frame == None)
        return;
    if (c->fullscreen) {
        XSetWindowBorderWidth(sc.dpy, c->frame, 0);
        XMoveResizeWindow(sc.dpy, c->frame, 0, 0, sc.sw, sc.sh);
        XMoveResizeWindow(sc.dpy, c->win, 0, 0, sc.sw, sc.sh);
        return;
    }
    XSetWindowBorderWidth(sc.dpy, c->frame, (unsigned)c->bw);
    XMoveResizeWindow(sc.dpy, c->frame, c->x, c->y, (unsigned)c->w,
                      (unsigned)frame_h(c));
    XMoveResizeWindow(sc.dpy, c->win, 0, c->title_h, (unsigned)c->w,
                      (unsigned)c->h);
    ui_shape_round(c->frame, c->w, frame_h(c), 11);
}

/* ---------------------------------------------------------- title bar art */

#define TRAFFIC_D 13
#define TRAFFIC_GAP 8
#define TRAFFIC_LEFT 14

static int traffic_span(void)
{
    return 3 * TRAFFIC_D + 2 * TRAFFIC_GAP;
}

void frame_paint(Client *c)
{
    int W, th, bl;
    unsigned long bg, top, bottom, fg;
    int hover = -1;

    if (c->frame == None || c->fullscreen || c->minimized)
        return;

    W = c->w;
    th = c->title_h;
    {
        float ft = (float)(c->focus_mix < 0 ? 0 :
                           (c->focus_mix > 100 ? 100 : c->focus_mix)) /
                   100.0f;

        bg = motion_mix(cfg.col_title_bg_inactive, cfg.col_title_bg, ft);
        top = ui_mix(bg, 0xffffffUL, c->focus_mix >= 50 ? 8 : 3);
        bottom = ui_mix(bg, 0x000000UL, c->focus_mix >= 50 ? 7 : 5);
        fg = motion_mix(cfg.col_title_fg_inactive, cfg.col_title_fg, ft);
    }

    ui_round_fill_gradient(c->frame, 0, 0, W, th, 0, top, bottom);
    XSetForeground(sc.dpy, sc.gc, ui_mix(top, 0xffffffUL, 8));
    XDrawLine(sc.dpy, c->frame, sc.gc, 0, 0, W, 0);
    XSetForeground(sc.dpy, sc.gc,
                   c->focus_mix >= 50
                       ? ui_mix(ui_mix(bottom, 0x000000UL, 38),
                                cfg.col_title_hover, 22)
                       : ui_mix(bottom, 0x000000UL, 38));
    XDrawLine(sc.dpy, c->frame, sc.gc, 0, th - 1, W, th - 1);

    /*
     * Liquid Glass, honestly: X11 has no alpha or blur, so depth is built
     * from solid tinted layers - an inner rim light, a soft lower shade
     * and a faint accent line.  Nothing here needs a compositor.
     */
    if (W > 12 && th > 8) {
        unsigned long rim = c->focus_mix >= 50
                                ? ui_mix(cfg.col_title_hover,
                                         cfg.col_title_bg, 55)
                                : ui_mix(cfg.col_border_unfocus,
                                         cfg.col_title_bg_inactive, 35);

        XSetForeground(sc.dpy, sc.gc, ui_mix(top, 0xffffffUL, 15));
        XDrawLine(sc.dpy, c->frame, sc.gc, 3, 2, W - 4, 2);
        XSetForeground(sc.dpy, sc.gc, ui_mix(bottom, 0x000000UL, 26));
        XDrawLine(sc.dpy, c->frame, sc.gc, 3, th - 3, W - 4, th - 3);
        ui_round_outline(c->frame, 1, 1, W - 2, th - 2, 10, rim);
    }

    if (c->hover_btn == BTN_CLOSE)
        hover = UI_LIGHT_CLOSE;
    else if (c->hover_btn == BTN_MIN)
        hover = UI_LIGHT_MIN;
    else if (c->hover_btn == BTN_MAX)
        hover = UI_LIGHT_MAX;

    if (W > traffic_span() + 80) {
        ui_draw_traffic(c->frame, TRAFFIC_LEFT, th / 2,
                        c->focus_mix >= 50, hover, c->maximized, bg);
    }

    bl = (th + sc.font_ascent - sc.font_descent) / 2;
    {
        int right = W - 14;
        int left = traffic_span() + 30;
        int tw;
        char shown[NAME_MAX_LEN];

        if (W > traffic_span() + 120) {
            tw = text_width_bold(c->name);
            if (tw <= right - left) {
                draw_text_bold(c->frame, (W - tw) / 2, bl, fg, c->name);
            } else {
                str_copy(shown, sizeof(shown), c->name);
                draw_text_clip_font(c->frame, sc.font_bold, left, right, bl,
                                    fg, shown);
            }
        } else if (right - left > 24) {
            draw_text_clip_font(c->frame, sc.font_bold, left, right, bl, fg,
                                c->name);
        }
    }
}

int frame_button_at(Client *c, int x, int y)
{
    int cell = TRAFFIC_D + TRAFFIC_GAP;
    int left = TRAFFIC_LEFT - 6;

    if (y < 0 || y >= c->title_h)
        return BTN_NONE;
    if (x < left || x >= left + 3 * cell)
        return BTN_NONE;
    switch ((x - left) / cell) {
    case 0:  return BTN_CLOSE;
    case 1:  return BTN_MIN;
    default: return BTN_MAX;
    }
}

/* ----------------------------------------------------------------- focus */

void update_client_list(void)
{
    Window *wins;
    Client *c;
    int n = client_count();
    int i = 0;

    if (n == 0) {
        XDeleteProperty(sc.dpy, sc.root, sc.atom_net_client_list);
        return;
    }
    wins = calloc((size_t)n, sizeof(Window));
    if (!wins)
        return;
    for (c = sc.clients; c; c = c->next)
        wins[i++] = c->win;
    XChangeProperty(sc.dpy, sc.root, sc.atom_net_client_list, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)wins, n);
    free(wins);
}

/*
 * Publish the window that owns the X input focus.  Tracking a single
 * value (sc.focus_win) means the property can never disagree with the
 * focus we actually set, whether it is a managed client or one of our own
 * application windows.  (implementation lives in util.c next to the
 * focus helpers, so tests that link util.c alone stay linkable)
 */

static void focus_step(void *ctx, float t)
{
    Client *c = ctx;
    float ft;

    c->focus_mix = motion_lerp(c->focus_from, c->focus_to, t);
    ft = (float)c->focus_mix / 100.0f;
    if (c->frame != None)
        XSetWindowBorder(sc.dpy, c->frame,
                         motion_mix(cfg.col_border_unfocus,
                                    cfg.col_title_hover, ft));
    if (!c->minimized)
        frame_paint(c);
}

/* cross-fade the title bar and border toward focused / unfocused */
static void focus_to(Client *c, int focused)
{
    int target = focused ? 100 : 0;

    if (!c)
        return;
    if (c->frame == None) {
        c->focus_mix = target;
        return;
    }
    if (!cfg.animations || c->minimized) {
        motion_cancel(MOTION_CH_FOCUS, c, 0);
        c->focus_mix = target;
        XSetWindowBorder(sc.dpy, c->frame,
                         target ? cfg.col_title_hover
                                : cfg.col_border_unfocus);
        if (!c->minimized)
            frame_paint(c);
        return;
    }
    if (c->focus_mix == target) {
        XSetWindowBorder(sc.dpy, c->frame,
                         target ? cfg.col_title_hover
                                : cfg.col_border_unfocus);
        return;
    }
    c->focus_from = c->focus_mix;
    c->focus_to = target;
    if (!motion_start(MOTION_CH_FOCUS, c, 130, MOTION_EASE_OUT, focus_step,
                      NULL, c)) {
        c->focus_mix = target;
        XSetWindowBorder(sc.dpy, c->frame,
                         target ? cfg.col_title_hover
                                : cfg.col_border_unfocus);
        if (!c->minimized)
            frame_paint(c);
    }
}

void unfocus_all(void)
{
    Client *c;

    for (c = sc.clients; c; c = c->next)
        if (c->frame != None)
            focus_to(c, 0);
    sc.focused = NULL;
}

static int ws_switching;

void focus_client(Client *c)
{
    if (!c) {
        unfocus_all();
        update_active_property();
        panel_update();
        return;
    }
    if (!ws_switching && c->workspace != sc.workspace)
        workspace_switch(c->workspace);
    if (c->minimized)
        client_minimize(c, 0);
    if (c->ws_hidden && c->frame != None) {
        c->ws_hidden = 0;
        XMapWindow(sc.dpy, c->frame);
    }

    if (sc.focused == c) {
        if (c->frame != None) {
            XSetWindowBorder(sc.dpy, c->frame, cfg.col_title_hover);
            if (!c->minimized)
                frame_paint(c);
        }
        return;
    }
    if (sc.focused && sc.focused != c)
        focus_to(sc.focused, 0);

    sc.focused = c;
    focus_to(c, 1);
    focus_track(c->win);
    if (c->frame != None) {
        XRaiseWindow(sc.dpy, c->frame);
    }
    panel_raise();
    set_wm_state(c->win, NormalState);
    update_active_property();
    panel_update();
}

/* ------------------------------------------------------ window motion -- */

/*
 * Liquid geometry motion.  A tween carries three keyframes: the start
 * rectangle, an expressive middle pose (overshoot or squash) and the
 * resting rectangle.  The shared engine runs the tween linearly so this
 * code can apply its own easing curve to each segment.
 */
static void geom_key(Client *c, int i, int x, int y, int w, int h)
{
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    c->anim_key[i].x = x;
    c->anim_key[i].y = y;
    c->anim_key[i].w = w;
    c->anim_key[i].h = h;
}

static void geom_lerp(const MotionKey *a, const MotionKey *b, float t,
                      MotionKey *out)
{
    out->x = motion_lerp(a->x, b->x, t);
    out->y = motion_lerp(a->y, b->y, t);
    out->w = motion_lerp(a->w, b->w, t);
    out->h = motion_lerp(a->h, b->h, t);
}

/*
 * Cubic Hermite interpolation for a three-pose motion.
 * The shared tangent at the middle key guarantees C1 continuity:
 * incoming and outgoing velocity match at the keyframe.
 */
static int geom_hermite_value(int p0, int p1, float m0, float m1,
                              float u)
{
    float u2 = u * u;
    float u3 = u2 * u;
    float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    float h10 = u3 - 2.0f * u2 + u;
    float h01 = -2.0f * u3 + 3.0f * u2;
    float h11 = u3 - u2;
    float value = h00 * (float)p0 + h10 * m0
                + h01 * (float)p1 + h11 * m1;

    return (int)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static void client_geom_eval(Client *c, float t, MotionKey *out)
{
    float split = c->anim_split;
    float u;

    if (c->anim_nkeys < 3) {
        float e = motion_ease(c->anim_ease1, t);

        geom_lerp(&c->anim_key[0], &c->anim_key[1], e, out);
        return;
    }

    if (split < 0.05f)
        split = 0.05f;
    if (split > 0.95f)
        split = 0.95f;

    if (t <= split) {
        const MotionKey *a = &c->anim_key[0];
        const MotionKey *b = &c->anim_key[1];
        const MotionKey *z = &c->anim_key[2];

        u = t / split;

        out->x = geom_hermite_value(a->x, b->x, 0.0f,
                                    split * (float)(z->x - a->x), u);
        out->y = geom_hermite_value(a->y, b->y, 0.0f,
                                    split * (float)(z->y - a->y), u);
        out->w = geom_hermite_value(a->w, b->w, 0.0f,
                                    split * (float)(z->w - a->w), u);
        out->h = geom_hermite_value(a->h, b->h, 0.0f,
                                    split * (float)(z->h - a->h), u);
    } else {
        const MotionKey *a = &c->anim_key[0];
        const MotionKey *b = &c->anim_key[1];
        const MotionKey *z = &c->anim_key[2];
        float remaining = 1.0f - split;

        u = (t - split) / remaining;

        out->x = geom_hermite_value(b->x, z->x,
                                    remaining * (float)(z->x - a->x),
                                    0.0f, u);
        out->y = geom_hermite_value(b->y, z->y,
                                    remaining * (float)(z->y - a->y),
                                    0.0f, u);
        out->w = geom_hermite_value(b->w, z->w,
                                    remaining * (float)(z->w - a->w),
                                    0.0f, u);
        out->h = geom_hermite_value(b->h, z->h,
                                    remaining * (float)(z->h - a->h),
                                    0.0f, u);
    }
}

static void client_set_opacity(Client *c, float a)
{
    unsigned long opacity;

    if (!c || c->frame == None || !sc.composited ||
        sc.atom_net_wm_opacity == None)
        return;
    if (a < 0.0f)
        a = 0.0f;
    if (a > 1.0f)
        a = 1.0f;
    opacity = (unsigned long)(a * 4294967295.0f);
    XChangeProperty(sc.dpy, c->frame, sc.atom_net_wm_opacity, XA_CARDINAL,
                    32, PropModeReplace, (unsigned char *)&opacity, 1);
}

static void client_geom_step(void *ctx, float t)
{
    Client *c = ctx;
    MotionKey k;

    if (c->frame == None)
        return;
    client_geom_eval(c, t, &k);
    if (k.w < 1)
        k.w = 1;
    if (k.h < 1)
        k.h = 1;
    XMoveResizeWindow(sc.dpy, c->frame, k.x, k.y, (unsigned)k.w,
                      (unsigned)k.h);
    ui_shape_round(c->frame, k.w, k.h, 11);
    if (c->has_fade) {
        float a = c->fade_from + (c->fade_to - c->fade_from) * t;

        client_set_opacity(c, a);
        if (t >= 1.0f)
            c->has_fade = 0;
    }
    if (t >= 1.0f)
        c->geom_animating = 0;
}

static void client_geom_sync_done(void *ctx)
{
    Client *c = ctx;

    if (c->frame == None)
        return;
    frame_sync(c);
    frame_paint(c);
}

static int client_geom_animate(Client *c, int duration, motion_done_fn done)
{
    if (c->frame == None || !cfg.animations)
        return 0;
    motion_cancel(MOTION_CH_GEOMETRY, c, 0);
    c->geom_animating = 1;
    client_geom_step(c, 0.0f);
    if (!motion_start(MOTION_CH_GEOMETRY, c, duration, MOTION_LINEAR,
                      client_geom_step, done, c)) {
        /* no scheduler slot: never leave the frame at its start pose */
        c->geom_animating = 0;
        frame_sync(c);
        return 0;
    }
    return 1;
}

/* build "grow out of the dock icon" keyframes */
static void geom_open_keys(Client *c)
{
    int w1 = frame_w(c);
    int h1 = frame_h(c);
    int cx = sc.sw / 2, cy = sc.sh - 60, iw = 42, ih = 42;
    int ow, oh;

    dock_client_anchor(c, &cx, &cy, &iw, &ih);
    if (iw < 24)
        iw = 24;
    if (ih < 24)
        ih = 24;

    c->anim_nkeys = MOTION_KEYS;
    geom_key(c, 0, cx - iw / 2, cy - ih / 2, iw, ih);
    ow = w1 + w1 * 4 / 100;
    oh = h1 + h1 * 4 / 100;
    if (ow > sc.sw)
        ow = sc.sw;
    if (oh > sc.sh)
        oh = sc.sh;
    geom_key(c, 1, c->x + (w1 - ow) / 2, c->y + (h1 - oh) / 2, ow, oh);
    geom_key(c, 2, c->x, c->y, w1, h1);
    c->anim_split = 0.64f;
    c->anim_ease0 = MOTION_EASE_IN_OUT;
    c->anim_ease1 = MOTION_EASE_OUT_QUINT;
}

/* build "collapse and fly into the dock icon" keyframes */
static void geom_dock_keys(Client *c)
{
    int w0 = frame_w(c);
    int h0 = frame_h(c);
    int cx = sc.sw / 2, cy = sc.sh - 60, iw = 42, ih = 42;
    int ccx = c->x + w0 / 2, ccy = c->y + h0 / 2;
    int w1 = w0 * 68 / 100;
    int h1 = h0 * 92 / 100;
    int mcx, mcy;

    dock_client_anchor(c, &cx, &cy, &iw, &ih);
    if (iw < 32)
        iw = 32;
    if (ih < 32)
        ih = 32;
    if (w1 < 60)
        w1 = 60;
    if (h1 < 44)
        h1 = 44;
    if (w1 > w0)
        w1 = w0;
    if (h1 > h0)
        h1 = h0;

    /* squash pose: narrower and a touch taller, already drifting to the
       dock so the eye reads one continuous gesture */
    mcx = (ccx * 65 + cx * 35) / 100;
    mcy = (ccy * 65 + cy * 35) / 100;

    c->anim_nkeys = MOTION_KEYS;
    geom_key(c, 0, c->x, c->y, w0, h0);
    geom_key(c, 1, mcx - w1 / 2, mcy - h1 / 2, w1, h1);
    geom_key(c, 2, cx - iw / 2, cy - ih / 2, iw, ih);
    c->anim_split = 0.45f;
    c->anim_ease0 = MOTION_EASE_IN_OUT;
    c->anim_ease1 = MOTION_EASE_IN_OUT_QUINT;
}

static int client_geom_open(Client *c)
{
    if (!cfg.animations || c->frame == None || frame_w(c) < 2 ||
        frame_h(c) < 2) {
        client_set_opacity(c, 1.0f);
        return 0;
    }
    geom_open_keys(c);
    c->fade_from = 0.0f;
    c->fade_to = 1.0f;
    c->has_fade = 1;
    if (!client_geom_animate(c, 260, client_geom_sync_done)) {
        c->has_fade = 0;
        client_set_opacity(c, 1.0f);
        return 0;
    }
    return 1;
}

static int client_geom_to_dock(Client *c, motion_done_fn done, int duration)
{
    if (!cfg.animations || c->frame == None) {
        client_set_opacity(c, 1.0f);
        return 0;
    }
    geom_dock_keys(c);
    c->fade_from = 1.0f;
    c->fade_to = 0.0f;
    c->has_fade = 1;
    if (!client_geom_animate(c, duration, done)) {
        c->has_fade = 0;
        client_set_opacity(c, 1.0f);
        return 0;
    }
    return 1;
}

/* current on-screen frame pose; used to start a transition from where the
   eye already is, even if a previous tween was interrupted mid-flight */
static void client_frame_pose(Client *c, MotionKey *k)
{
    XWindowAttributes wa;

    if (c->frame != None &&
        XGetWindowAttributes(sc.dpy, c->frame, &wa) && wa.width > 0 &&
        wa.height > 0) {
        k->x = wa.x;
        k->y = wa.y;
        k->w = wa.width;
        k->h = wa.height;
        return;
    }
    k->x = c->x;
    k->y = c->y;
    k->w = frame_w(c);
    k->h = frame_h(c);
}

static int client_geom_to_rest(Client *c, const MotionKey *from, int duration,
                               int easing, motion_done_fn done)
{
    if (!cfg.animations || c->frame == None)
        return 0;
    c->anim_nkeys = 2;
    c->anim_key[0] = *from;
    geom_key(c, 1, c->x, c->y, frame_w(c), frame_h(c));
    c->anim_split = 0.5f;
    c->anim_ease0 = easing;
    c->anim_ease1 = easing;
    c->has_fade = 0;
    if (!client_geom_animate(c, duration, done)) {
        frame_sync(c);
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------- geometry ops */

static void clamp_to_desktop(Client *c)
{
    int top = reserve_top();
    int bot = reserve_bottom();
    int fh = frame_h(c);

    if (c->fullscreen)
        return;

    /* keep clear of the top menu bar and the floating dock */
    if (c->y < top)
        c->y = top;
    if (c->y + fh > sc.sh - bot)
        c->y = sc.sh - bot - fh;
    if (c->y < top)
        c->y = top;
    if (c->y > sc.sh - bot - 32)
        c->y = sc.sh - bot - 32;
    if (c->x + c->w < 64)
        c->x = 64 - c->w;
    if (c->x > sc.sw - 64)
        c->x = sc.sw - 64;
}

/* user input wins: cancel any in-flight geometry gesture cleanly */
static void client_interrupt_motion(Client *c)
{
    motion_cancel(MOTION_CH_GEOMETRY, c, 0);
    c->geom_animating = 0;
    c->has_fade = 0;
    client_set_opacity(c, 1.0f);
    if (c->closing)
        c->closing = 0;
}

void client_maximize(Client *c, int on)
{
    int wy, wh;
    MotionKey from;

    if (!c || c->fullscreen)
        return;
    if (on == c->maximized)
        return;
    client_interrupt_motion(c);
    client_frame_pose(c, &from);

    if (on) {
        c->sx = c->x;
        c->sy = c->y;
        c->sw = c->w;
        c->sh = c->h;
        wy = reserve_top();
        wh = sc.sh - reserve_top() - reserve_bottom();
        c->x = 0;
        c->y = wy;
        c->w = sc.sw - 2 * c->bw;
        c->h = wh - c->title_h - 2 * c->bw;
        if (c->h < MIN_CLIENT_SIZE)
            c->h = MIN_CLIENT_SIZE;
        c->maximized = 1;
    } else {
        c->x = c->sx;
        c->y = c->sy;
        c->w = c->sw;
        c->h = c->sh;
        c->maximized = 0;
        clamp_to_desktop(c);
    }
    if (!client_geom_to_rest(c, &from, on ? 260 : 240,
                             on ? MOTION_EASE_OUT_QUINT
                                : MOTION_EASE_IN_OUT_QUINT,
                             client_geom_sync_done)) {
        frame_sync(c);
        frame_paint(c);
    }
    panel_update();
}

static void minimize_finish(Client *c)
{
    Client *o;

    if (!c || c->minimized)
        return;
    c->minimized = 1;
    set_wm_state(c->win, IconicState);
    if (c->frame != None) {
        XUnmapWindow(sc.dpy, c->frame);
        client_set_opacity(c, 1.0f);
    }
    if (sc.focused == c) {
        sc.focused = NULL;
        for (o = sc.clients; o; o = o->next)
            if (o != c && !o->minimized)
                break;
        if (o)
            focus_client(o);
        else {
            /* nothing left to focus: drop the stale owner so the
               active hint and the taskbar dot both go quiet */
            focus_drop(c->win);
            update_active_property();
        }
    }
    log_msg("minimize 0x%lx '%s'", (unsigned long)c->win, c->name);
    panel_update();
}

static void minimize_done(void *ctx)
{
    minimize_finish(ctx);
}

void client_minimize(Client *c, int on)
{
    if (!c || c->minimized == on)
        return;

    if (on) {
        if (c->closing)
            c->closing = 0;
        if (client_geom_to_dock(c, minimize_done, 220))
            return;
        minimize_finish(c);
        return;
    }

    c->minimized = 0;
    set_wm_state(c->win, NormalState);
    if (c->workspace == sc.workspace) {
        c->ws_hidden = 0;
        if (c->frame != None) {
            client_set_opacity(c, cfg.animations ? 0.0f : 1.0f);
            XMapWindow(sc.dpy, c->frame);
            if (!client_geom_open(c))
                frame_sync(c);
        }
    } else {
        client_set_opacity(c, 1.0f);
        c->ws_hidden = 1;
    }
    log_msg("restore 0x%lx '%s'", (unsigned long)c->win, c->name);
    panel_update();
}

void show_desktop_toggle(void)
{
    Client *c;
    int visible = 0;
    /* internal windows hidden by show-desktop, restored on the way back */
    static int hid_fm, hid_sg, hid_sc;

    for (c = sc.clients; c; c = c->next)
        if (!c->minimized && c->workspace == sc.workspace)
            visible++;
    if (control_center_open())
        control_center_hide();
    if (notification_center_open())
        notification_center_hide();
    if (fm_open() || security_gui_open() || system_center_open())
        visible++;

    if (visible) {
        for (c = sc.clients; c; c = c->next)
            if (!c->minimized && c->workspace == sc.workspace)
                client_minimize(c, 1);
        /* internal apps hide to their taskbar entry, like a minimise */
        hid_fm = fm_open();
        if (hid_fm)
            fm_hide();
        hid_sg = security_gui_open();
        if (hid_sg)
            security_gui_hide();
        hid_sc = system_center_open();
        if (hid_sc)
            system_center_hide();
        sc.show_desktop = 1;
    } else {
        for (c = sc.clients; c; c = c->next)
            if (c->minimized && c->workspace == sc.workspace)
                client_minimize(c, 0);
        if (hid_fm) {
            hid_fm = 0;
            fm_show();
        }
        if (hid_sg) {
            hid_sg = 0;
            security_gui_show();
        }
        if (hid_sc) {
            hid_sc = 0;
            system_center_show();
        }
        sc.show_desktop = 0;
        if (sc.clients)
            focus_client(sc.clients);
    }
    panel_update();
}

void snap_client(Client *c, int side)
{
    int wy = reserve_top();
    int wh = sc.sh - reserve_top() - reserve_bottom();

    if (!c || c->fullscreen)
        return;
    if (c->maximized) {
        c->maximized = 0;
    }
    c->x = (side == 0) ? 0 : sc.sw / 2;
    c->y = wy;
    c->w = sc.sw / 2 - 2 * c->bw;
    c->h = wh - c->title_h - 2 * c->bw;
    if (c->h < MIN_CLIENT_SIZE)
        c->h = MIN_CLIENT_SIZE;
    frame_sync(c);
    frame_paint(c);
}

/* -------------------------------------------------------------- commands */

static int client_has_delete(Client *c)
{
    Atom *protocols = NULL;
    int n = 0, has_delete = 0, i;

    if (XGetWMProtocols(sc.dpy, c->win, &protocols, &n) && protocols) {
        for (i = 0; i < n; i++)
            if (protocols[i] == sc.atom_wm_delete)
                has_delete = 1;
        XFree(protocols);
    }
    return has_delete;
}

static void send_close_request(Client *c)
{
    XEvent ev;

    if (client_has_delete(c)) {
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = c->win;
        ev.xclient.message_type = sc.atom_wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)sc.atom_wm_delete;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(sc.dpy, c->win, False, NoEventMask, &ev);
        log_msg("close (WM_DELETE_WINDOW) 0x%lx", (unsigned long)c->win);
    } else {
        /* Graceful close only: never force-kill a client that did not
           advertise WM_DELETE_WINDOW.  The user can still Quit an
           application whose windows do honour the protocol. */
        xenus_notify("Window manager",
                    "The application does not support graceful close");
        log_msg("close refused (no WM_DELETE_WINDOW) 0x%lx '%s'",
                (unsigned long)c->win, c->name);
    }
}

/*
 * The close gesture parks the frame on its dock icon.  Send the request
 * while the frame is still in that final pose: snapping it back to full
 * size first made the desktop flash for one or two frames before the
 * application actually went away.
 *
 * A well-behaved client destroys the window and unmanage() cancels this
 * grace tween.  Only an application that ignores WM_DELETE_WINDOW needs
 * the frame restored, and it then gets a short, quiet pause to answer.
 */
static void close_grace_step(void *ctx, float t)
{
    (void)ctx;
    (void)t;
}

static void close_grace_done(void *ctx)
{
    Client *c = ctx;

    if (!client_find(c->win))
        return;
    if (c->closing) {
        /* the application ignored WM_DELETE_WINDOW: bring the frame back
           with the normal open gesture instead of a one-frame snap */
        c->closing = 0;
        if (!client_geom_open(c)) {
            frame_sync(c);
            frame_paint(c);
        }
        panel_update();
    }
}

static void close_anim_done(void *ctx)
{
    Client *c = ctx;

    if (!client_find(c->win))
        return;
    send_close_request(c);
    if (!motion_start(MOTION_CH_GEOMETRY, c, 1200, MOTION_LINEAR,
                      close_grace_step, close_grace_done, c)) {
        c->closing = 0;
        frame_sync(c);
        frame_paint(c);
    }
}

void close_client(Client *c)
{
    if (!c)
        return;
    if (c->closing)
        return;                         /* gesture already in flight */
    if (!cfg.animations || c->frame == None || c->minimized ||
        !client_has_delete(c)) {
        send_close_request(c);
        return;
    }

    c->closing = 1;
    motion_cancel(MOTION_CH_FOCUS, c, 0);
    if (!client_geom_to_dock(c, close_anim_done, 220)) {
        c->closing = 0;
        send_close_request(c);
    }
}

void cycle_windows(int dir)
{
    Client *c, *prev = NULL, *first = NULL, *last = NULL;
    Client *picked = NULL;

    for (c = sc.clients; c; c = c->next) {
        if (c->workspace != sc.workspace || c->minimized)
            continue;
        if (!first)
            first = c;
        last = c;
    }
    if (!first)
        return;

    if (!sc.focused || sc.focused->workspace != sc.workspace) {
        focus_client(dir >= 0 ? first : last);
        return;
    }

    if (dir >= 0) {
        picked = NULL;
        for (c = sc.focused->next; c; c = c->next)
            if (c->workspace == sc.workspace && !c->minimized) {
                picked = c;
                break;
            }
        if (!picked)
            picked = first;
    } else {
        for (c = first; c && c != sc.focused; c = c->next) {
            if (c->workspace == sc.workspace && !c->minimized)
                prev = c;
        }
        picked = prev ? prev : last;
    }
    focus_client(picked);
}

/*
 * The X server's real input focus - one round trip, only when an action
 * targets "the active window" (Alt+F4, Alt+Down, the menu bar).
 */
static Window current_focus(void)
{
    Window f = None;
    int revert = RevertToNone;

    if (sc.dpy)
        XGetInputFocus(sc.dpy, &f, &revert);
    return f;
}

/* close the focused window, internal applications included */
void active_close(void)
{
    Window fw = current_focus();

    if (fm_open() && (fw == fm_window() || sc.focus_win == fm_window())) {
        fm_close();
        return;
    }
    if (text_editor_open_state() &&
        (fw == text_editor_window() || sc.focus_win == text_editor_window())) {
        text_editor_close();
        return;
    }
    if (security_gui_open() &&
        (fw == security_gui_window() ||
         sc.focus_win == security_gui_window())) {
        security_gui_close();
        return;
    }
    if (system_center_open() &&
        (fw == system_center_window() ||
         sc.focus_win == system_center_window())) {
        system_center_close();
        return;
    }
    if (control_center_open() &&
        (fw == control_center_window() ||
         sc.focus_win == control_center_window())) {
        control_center_hide();
        return;
    }
    if (notification_center_open() &&
        (fw == notification_center_window() ||
         sc.focus_win == notification_center_window())) {
        notification_center_hide();
        return;
    }
    close_client(sc.focused);
}

/* minimise the focused window (or restore it), internal apps included */
void active_minimize(void)
{
    Window fw = current_focus();

    if (fm_open() && (fw == fm_window() || sc.focus_win == fm_window())) {
        fm_hide();
        return;
    }
    if (text_editor_open_state() &&
        (fw == text_editor_window() || sc.focus_win == text_editor_window())) {
        text_editor_hide();
        return;
    }
    if (security_gui_open() &&
        (fw == security_gui_window() ||
         sc.focus_win == security_gui_window())) {
        security_gui_hide();
        return;
    }
    if (system_center_open() &&
        (fw == system_center_window() ||
         sc.focus_win == system_center_window())) {
        system_center_hide();
        return;
    }
    if (control_center_open() &&
        (fw == control_center_window() ||
         sc.focus_win == control_center_window())) {
        control_center_hide();
        return;
    }
    if (notification_center_open() &&
        (fw == notification_center_window() ||
         sc.focus_win == notification_center_window())) {
        notification_center_hide();
        return;
    }
    if (sc.focused)
        client_minimize(sc.focused, sc.focused->minimized ? 0 : 1);
}

/* ----------------------------------------------------------------- manage */

static int window_is_fullscreen(Window w)
{
    Atom actual;
    int fmt;
    unsigned long nitems = 0, bytes = 0;
    unsigned char *data = NULL;
    int found = 0;
    int i;

    if (XGetWindowProperty(sc.dpy, w, sc.atom_net_wm_state, 0, 64, False,
                           sc.atom_net_wm_state, &actual, &fmt, &nitems,
                           &bytes, &data) == Success && data) {
        if (actual == sc.atom_net_wm_state && fmt == 32) {
            Atom *atoms = (Atom *)data;
            for (i = 0; i < (int)nitems; i++)
                if (atoms[i] == sc.atom_net_wm_state_fullscreen)
                    found = 1;
        }
        XFree(data);
    }
    return found;
}

static void fetch_name(Client *c)
{
    Atom actual = None;
    int format = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *data = NULL;

    str_copy(c->name, sizeof(c->name), "untitled");
    if (XGetWindowProperty(sc.dpy, c->win, sc.atom_net_wm_name, 0, 1024,
                           False, sc.atom_utf8_string, &actual, &format,
                           &nitems, &after, &data) == Success &&
        data && actual == sc.atom_utf8_string &&
        format == 8 && nitems > 0) {
        size_t n = nitems < sizeof(c->name) - 1 ? nitems
                                                : sizeof(c->name) - 1;

        memcpy(c->name, data, n);
        c->name[n] = '\0';
        XFree(data);
        return;
    }
    if (data)
        XFree(data);
    {
        char *name = NULL;

        if (XFetchName(sc.dpy, c->win, &name) && name) {
            str_copy(c->name, sizeof(c->name), name);
            XFree(name);
        }
    }
}

static void fetch_app_id(Client *c)
{
    XClassHint hint;

    c->app_class[0] = '\0';
    c->app_instance[0] = '\0';
    memset(&hint, 0, sizeof(hint));
    if (XGetClassHint(sc.dpy, c->win, &hint)) {
        if (hint.res_class)
            str_copy(c->app_class, sizeof(c->app_class), hint.res_class);
        if (hint.res_name)
            str_copy(c->app_instance, sizeof(c->app_instance), hint.res_name);
        if (hint.res_name)
            XFree(hint.res_name);
        if (hint.res_class)
            XFree(hint.res_class);
    }
}

static void create_frame(Client *c, int fx, int fy)
{
    XSetWindowAttributes wa;

    wa.override_redirect = True;
    wa.background_pixel = cfg.col_title_bg_inactive;
    wa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                    EnterWindowMask | ExposureMask;

    c->frame = XCreateWindow(sc.dpy, sc.root, fx, fy, (unsigned)c->w,
                             (unsigned)frame_h(c), (unsigned)c->bw,
                             sc.depth, InputOutput, sc.visual,
                             CWOverrideRedirect | CWBackPixel | CWEventMask,
                             &wa);
    XSetWindowBorder(sc.dpy, c->frame, cfg.col_border_unfocus);
}

void manage(Window w)
{
    XWindowAttributes wa;
    Client *c, *tail;
    int fx, fy;

    if (w == None || w == sc.panel || w == sc.dock || w == sc.menu ||
        w == sc.cmd || w == sc.wm_check)
        return;
    if (overview_is_open())
        overview_close_now();
    if (client_find(w))
        return;
    if (!XGetWindowAttributes(sc.dpy, w, &wa))
        return;
    if (wa.override_redirect)
        return;
    if (wa.width <= 1 && wa.height <= 1) {
        XMapWindow(sc.dpy, w);
        return;
    }

    c = calloc(1, sizeof(Client));
    if (!c)
        return;

    c->win = w;
    c->frame = None;
    c->w = wa.width > 0 ? wa.width : 320;
    c->h = wa.height > 0 ? wa.height : 240;
    c->bw = cfg.border_width;
    c->title_h = cfg.title_height;
    c->workspace = sc.workspace;
    c->fullscreen = window_is_fullscreen(w);
    fetch_name(c);
    fetch_app_id(c);

    fx = wa.x - c->bw;
    fy = wa.y - c->bw - c->title_h;
    if (fy < 0)
        fy = 0;
    c->x = fx;
    c->y = fy;

    XSelectInput(sc.dpy, w, EnterWindowMask | PropertyChangeMask |
                           StructureNotifyMask);
    XSetWindowBorderWidth(sc.dpy, w, 0);
    XAddToSaveSet(sc.dpy, w);
    input_grab_client(w);

    create_frame(c, c->x, c->y);
    if (wa.map_state == IsViewable)
        c->ignore_unmap += 2;           /* reparent: unmap to old parent + self */
    XReparentWindow(sc.dpy, w, c->frame, 0, c->title_h);

    clamp_to_desktop(c);

    if (!sc.clients) {
        sc.clients = c;
    } else {
        for (tail = sc.clients; tail->next; tail = tail->next)
            ;
        tail->next = c;
    }

    set_wm_state(w, NormalState);
    XMapWindow(sc.dpy, w);
    frame_sync(c);
    XMapWindow(sc.dpy, c->frame);
    (void)client_geom_open(c);

    update_client_list();
    focus_client(c);
    log_msg("manage 0x%lx '%s' %dx%d frame+%d+%d", (unsigned long)w, c->name,
            c->w, c->h, c->x, c->y);
}

void unmanage(Client *c, int client_alive)
{
    Client *prev = NULL, *it;

    if (!c)
        return;
    motion_cancel_owner(c, 0);
    overview_forget(c);
    log_msg("unmanage 0x%lx '%s'", (unsigned long)c->win, c->name);

    for (it = sc.clients; it && it != c; it = it->next)
        prev = it;
    if (it == c) {
        if (prev)
            prev->next = c->next;
        else
            sc.clients = c->next;
    }

    if (sc.focused == c)
        sc.focused = NULL;
    focus_drop(c->win);

    if (client_alive) {
        XSelectInput(sc.dpy, c->win, NoEventMask);
        input_ungrab_client(c->win);
        XUnmapWindow(sc.dpy, c->frame);
        XReparentWindow(sc.dpy, c->win, sc.root, c->x + c->bw,
                        c->y + c->bw + c->title_h);
        XRemoveFromSaveSet(sc.dpy, c->win);
        XSetWindowBorderWidth(sc.dpy, c->win, (unsigned)cfg.border_width);
    }
    if (c->frame != None) {
        XDestroyWindow(sc.dpy, c->frame);
        c->frame = None;
    }

    free(c);

    update_client_list();
    /* when nothing holds the focus any more, fall back to a managed client;
       if one of our own application windows still has it, leave it alone */
    if (!sc.focused && sc.clients && sc.focus_win == None) {
        /* skip hidden windows: restoring a minimized one would pop it up */
        for (it = sc.clients; it; it = it->next)
            if (!it->minimized)
                break;
        focus_client(it);
    } else
        update_active_property();
    panel_update();
}

/* ----------------------------------------------------------------- events */

void handle_map_request(XMapRequestEvent *ev)
{
    manage(ev->window);
}

void handle_configure_request(XConfigureRequestEvent *ev)
{
    Client *c = client_find(ev->window);
    XWindowChanges wc;
    unsigned int mask = (unsigned int)ev->value_mask;
    int top = reserve_top();
    int bottom = reserve_bottom();

    if (!c) {
        /* window is not ours (or has not been managed yet) */
        wc.x = ev->x;
        wc.y = ev->y;
        wc.width = ev->width;
        wc.height = ev->height;
        wc.border_width = ev->border_width;
        wc.sibling = ev->above;
        wc.stack_mode = ev->detail;
        XConfigureWindow(sc.dpy, ev->window, mask, &wc);
        return;
    }
    if (c->closing)
        return;                         /* the close gesture owns geometry */

    wc.x = ev->x;
    wc.y = ev->y;
    wc.width = ev->width;
    wc.height = ev->height;
    wc.sibling = ev->above;
    wc.stack_mode = ev->detail;
    mask &= ~(unsigned int)CWBorderWidth;
    if (c->fullscreen) {
        mask &= (unsigned int)(CWSibling | CWStackMode);
        XConfigureWindow(sc.dpy, c->frame, mask, &wc);
        return;
    }

    if (mask & CWWidth)
        c->w = wc.width < MIN_CLIENT_SIZE ? MIN_CLIENT_SIZE : wc.width;
    if (mask & CWHeight)
        c->h = wc.height < MIN_CLIENT_SIZE ? MIN_CLIENT_SIZE : wc.height;
    if (mask & CWX) {
        int nx = wc.x - c->bw;
        if (nx < 0)
            nx = 0;
        if (nx > sc.sw - 32)
            nx = sc.sw - 32;
        c->x = nx;
    }
    if (mask & CWY) {
        int ny = wc.y - c->bw - c->title_h;

        if (ny < top)
            ny = top;
        if (ny > sc.sh - bottom - 32)
            ny = sc.sh - bottom - 32;
        c->y = ny;
    }

    frame_sync(c);
    frame_paint(c);

    if (mask & CWStackMode) {
        wc.sibling = None;
        XConfigureWindow(sc.dpy, c->frame,
                         mask & (unsigned int)(CWSibling | CWStackMode), &wc);
        panel_raise();                  /* keep the panel on top */
    }
    panel_update();
}

void handle_destroy_notify(XDestroyWindowEvent *ev)
{
    Client *c = client_find(ev->window);

    if (c)
        unmanage(c, 0);
}

void handle_unmap_notify(XUnmapEvent *ev)
{
    Client *c = client_find(ev->window);

    if (!c)
        return;
    if (c->ignore_unmap > 0) {
        c->ignore_unmap--;
        return;
    }
    if (c->ws_hidden)
        return;                         /* hidden by a workspace switch */
    if (c->minimized)
        return;                         /* we unmapped the frame ourselves */
    unmanage(c, 1);
}

void handle_enter_notify(XCrossingEvent *ev)
{
    Client *c;

    if (ev->mode != NotifyNormal)
        return;
    if (menu_is_open() || cmd_is_open())
        return;
    c = client_find(ev->window);
    if (!c)
        c = client_from_frame(ev->window);
    if (!c)
        return;
    if (c->hover_btn != BTN_NONE) {     /* stale hover from the last visit */
        c->hover_btn = BTN_NONE;
        frame_paint(c);
    }
    if (cfg.focus_follows_mouse)
        focus_client(c);
}

void handle_property_notify(XPropertyEvent *ev)
{
    Client *c = client_find(ev->window);

    if (!c)
        return;
    if (ev->atom == XA_WM_NAME || ev->atom == sc.atom_net_wm_name) {
        fetch_name(c);
        frame_paint(c);
        panel_update();
    }
}

void handle_client_message(XClientMessageEvent *ev)
{
    Client *c = client_find(ev->window);

    if (!c) {
        /* our own application windows answer _NET_ACTIVE_WINDOW as well,
           so tools (xdotool windowactivate) can raise/focus them */
        if (ev->message_type == sc.atom_net_active) {
            if (ev->window == fm_window() && fm_window() != None) {
                fm_show();
            } else if (ev->window == security_gui_window() &&
                       security_gui_window() != None) {
                security_gui_show();
            } else if (ev->window == system_center_window() &&
                       system_center_window() != None) {
                system_center_show();
            }
        }
        return;
    }
    if (ev->message_type == sc.atom_net_wm_state &&
        (Atom)ev->data.l[1] == sc.atom_net_wm_state_fullscreen) {
        long action = ev->data.l[0];
        int on = c->fullscreen;

        if (action == 0)
            on = 0;
        else if (action == 1)
            on = 1;
        else
            on = !on;
        if (on != c->fullscreen) {
            c->fullscreen = on;
            frame_sync(c);
            frame_paint(c);
            if (on)
                XRaiseWindow(sc.dpy, c->frame);
            panel_raise();
        }
    } else if (ev->message_type == sc.atom_net_active) {
        focus_client(c);
    }
}

void handle_expose(XExposeEvent *ev)
{
    Client *c;

    if (ev->window == sc.root) {         /* dragged windows reveal the */
        desktop_expose(ev);              /* desktop: repaint icons here */
        return;
    }
    if (overview_expose(ev))
        return;
    if (msgbox_expose(ev))
        return;
    if (system_center_expose(ev))
        return;
    if (settings_expose(ev))
        return;
    if (control_center_expose(ev))
        return;
    if (notification_center_expose(ev))
        return;
    if (text_editor_expose(ev))
        return;
    if (security_gui_expose(ev))
        return;
    if (fm_expose(ev))
        return;
    if (ev->count != 0)
        return;
    if ((c = client_from_frame(ev->window)) != NULL) {
        /* never repaint a full-size title bar over a frame that is
           collapsing into the dock: it would flash above the desktop */
        if (!c->closing)
            frame_paint(c);
    }
    else if (ev->window == sc.panel)
        panel_expose(ev);
    else if (ev->window == sc.dock)
        dock_expose(ev);
    else if (ev->window == sc.menu)
        menu_expose(ev);
    else if (ev->window == sc.cmd)
        cmd_expose(ev);
}

void handle_frame_motion(XMotionEvent *ev)
{
    Client *c = client_from_frame(ev->window);
    int b;

    if (!c || c->fullscreen)
        return;
    b = frame_button_at(c, ev->x, ev->y);
    if (b != c->hover_btn) {
        c->hover_btn = b;
        frame_paint(c);
    }
}

static Time last_click_time;
static Window last_click_win;

void handle_frame_button_press(XButtonEvent *ev)
{
    Client *c = client_from_frame(ev->window);
    int dir = 0;
    const int m = 5;

    if (!c)
        return;
    if (overview_is_open()) {
        overview_select(c);
        return;
    }
    focus_client(c);
    if (c->minimized || c->fullscreen)
        return;

    if (strip_mods(ev->state) != 0)
        return;                         /* Alt+click is handled by root grabs */

    if (ev->y < c->title_h) {
        int b = frame_button_at(c, ev->x, ev->y);

        if (b == BTN_CLOSE) {
            close_client(c);
            return;
        }
        if (b == BTN_MAX) {
            client_maximize(c, !c->maximized);
            return;
        }
        if (b == BTN_MIN) {
            client_minimize(c, 1);
            return;
        }
        /* double click on the title bar toggles maximize */
        if (last_click_win == ev->window &&
            ev->time - last_click_time < 400) {
            last_click_win = None;
            client_maximize(c, !c->maximized);
            return;
        }
        last_click_win = ev->window;
        last_click_time = ev->time;
        start_drag(c, ev, DRAG_MOVE, 0);
        return;
    }

    /* edges resize */
    if (ev->x < 0 || (ev->x < m && ev->y >= c->title_h))
        dir |= DIR_LEFT;
    if (ev->x >= c->w || (ev->x >= c->w - m && ev->y >= c->title_h))
        dir |= DIR_RIGHT;
    if (ev->y < 0 || (ev->y >= c->title_h && ev->y < c->title_h + m))
        dir |= DIR_UP;
    if (ev->y >= c->title_h + c->h ||
        (ev->y >= c->title_h && ev->y >= c->title_h + c->h - m))
        dir |= DIR_DOWN;
    if (dir)
        start_drag(c, ev, DRAG_RESIZE, dir);
}

void manage_existing(void)
{
    Window root_ret, parent_ret, *children = NULL;
    unsigned int n = 0, i;

    if (!XQueryTree(sc.dpy, sc.root, &root_ret, &parent_ret, &children, &n))
        return;
    for (i = 0; i < n; i++) {
        XWindowAttributes wa;

        if (!XGetWindowAttributes(sc.dpy, children[i], &wa))
            continue;
        if (!wa.override_redirect) {
            if (wa.map_state != IsUnmapped)
                manage(children[i]);
            continue;
        }
        if (children[i] == sc.panel || children[i] == sc.dock ||
            children[i] == sc.menu || children[i] == sc.cmd ||
            children[i] == sc.wm_check)
            continue;

        /* stale frame left behind by a crashed instance: adopt its clients */
        {
            Window fr = children[i], fr_ret, fr_parent, *grand = NULL;
            unsigned int gn = 0, g;

            if (!XQueryTree(sc.dpy, fr, &fr_ret, &fr_parent, &grand, &gn))
                continue;
            for (g = 0; g < gn; g++) {
                XWindowAttributes gwa;

                if (!XGetWindowAttributes(sc.dpy, grand[g], &gwa))
                    continue;
                XReparentWindow(sc.dpy, grand[g], sc.root,
                                wa.x + wa.border_width,
                                wa.y + wa.border_width +
                                    (wa.height - gwa.height));
                XSetWindowBorderWidth(sc.dpy, grand[g],
                                      (unsigned)cfg.border_width);
                manage(grand[g]);
            }
            if (grand)
                XFree(grand);
            XDestroyWindow(sc.dpy, fr);
        }
    }
    if (children)
        XFree(children);
}


/* ---------------------------------------------------------- workspaces -- */

typedef struct {
    int from;
    int to;
    int dir;
} WsTransition;

static WsTransition ws_trans;
static int ws_animating;

static void ws_step(void *ctx, float t)
{
    Client *c;

    (void)ctx;
    for (c = sc.clients; c; c = c->next) {
        int off;

        if (c->frame == None || c->minimized || c->ws_hidden)
            continue;
        if (c->workspace == ws_trans.to)
            off = (int)((float)ws_trans.dir * (float)sc.sw * (1.0f - t));
        else if (c->workspace == ws_trans.from)
            off = -(int)((float)ws_trans.dir * (float)sc.sw * t);
        else
            continue;
        XMoveWindow(sc.dpy, c->frame, c->x + off, c->y);
    }
}

static void ws_finish(void *ctx)
{
    Client *c, *target = NULL;

    (void)ctx;
    ws_animating = 0;
    for (c = sc.clients; c; c = c->next) {
        if (c->minimized)
            continue;
        if (c->workspace != sc.workspace) {
            if (c->frame != None && !c->ws_hidden) {
                c->ws_hidden = 1;
                XUnmapWindow(sc.dpy, c->frame);
            }
            continue;
        }
        if (c->frame != None) {
            if (c->ws_hidden) {
                c->ws_hidden = 0;
                XMapWindow(sc.dpy, c->frame);
            }
            frame_sync(c);
            frame_paint(c);
        }
        if (!target)
            target = c;
    }

    if (sc.focused && sc.focused->workspace != sc.workspace) {
        focus_to(sc.focused, 0);
        sc.focused = NULL;
        focus_drop(None);
    }
    if (target)
        focus_client(target);
    else {
        sc.focused = NULL;
        update_active_property();
    }
    ws_switching = 0;
    panel_update();
    desktop_damage(0, 0, sc.sw, sc.sh);
    XFlush(sc.dpy);
}

int workspace_count(void)
{
    return XENUS_WORKSPACES;
}

int workspace_current(void)
{
    return sc.workspace;
}

const char *workspace_name(int index)
{
    static const char *names[XENUS_WORKSPACES] = {
        "Desktop 1", "Desktop 2", "Desktop 3", "Desktop 4"
    };

    if (index < 0 || index >= XENUS_WORKSPACES)
        return "Desktop";
    return names[index];
}

void workspace_switch(int index)
{
    Client *c;

    if (index < 0)
        index = XENUS_WORKSPACES - 1;
    if (index >= XENUS_WORKSPACES)
        index = 0;
    if (overview_is_open())
        overview_close_now();
    if (ws_animating)
        motion_cancel(MOTION_CH_WORKSPACE, &ws_trans, 1);
    if (index == sc.workspace && sc.workspace_count == XENUS_WORKSPACES)
        return;

    ws_switching = 1;
    ws_trans.from = sc.workspace;
    ws_trans.to = index;
    ws_trans.dir = index > ws_trans.from ? 1 : -1;
    sc.workspace = index;
    sc.workspace_count = XENUS_WORKSPACES;

    for (c = sc.clients; c; c = c->next) {
        if (c->workspace == index && !c->minimized) {
            if (c->ws_hidden && c->frame != None)
                XMapWindow(sc.dpy, c->frame);
            c->ws_hidden = 0;
        } else if (c->frame != None && !c->ws_hidden &&
                   c->workspace != index && c->workspace != ws_trans.from) {
            c->ws_hidden = 1;
            XUnmapWindow(sc.dpy, c->frame);
        }
    }

    if (cfg.animations && sc.clients) {
        ws_animating = 1;
        ws_step(NULL, 0.0f);
        if (motion_start(MOTION_CH_WORKSPACE, &ws_trans, 230,
                         MOTION_EASE_OUT_QUINT, ws_step, ws_finish, NULL))
            return;
        ws_animating = 0;
    }
    ws_finish(NULL);
}

void workspace_move_active(int index)
{
    Client *c = sc.focused;

    if (!c)
        return;
    if (index < 0)
        index = XENUS_WORKSPACES - 1;
    if (index >= XENUS_WORKSPACES)
        index = 0;
    if (index == c->workspace)
        return;
    c->workspace = index;
    if (c->frame != None) {
        c->ws_hidden = 1;
        XUnmapWindow(sc.dpy, c->frame);
    }
    sc.focused = NULL;
    update_active_property();
    panel_update();
}
