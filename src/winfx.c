/*
 * XENUS shell-window motion.
 *
 * File Manager, Security Center and other shell-owned surfaces are
 * override-redirect windows: they never pass through manage(), so the
 * Client frame tween cannot drive them.  This module gives them the same
 * three-keyframe gesture (dock/launcher origin -> overshoot -> rest) on
 * the shared motion thread, without a second scheduler or a second easing
 * implementation.
 */
#include <stdio.h>
#include <string.h>

#include "xenus.h"
#include "motion.h"

#define WINFX_OPEN_MS   260
#define WINFX_CLOSE_MS  220

static int origin_valid;
static int origin_cx, origin_cy, origin_w, origin_h;

void winfx_origin_set(int cx, int cy, int w, int h)
{
    origin_cx = cx;
    origin_cy = cy;
    origin_w = w > 0 ? w : 42;
    origin_h = h > 0 ? h : 42;
    origin_valid = 1;
}

static int origin_take(int *cx, int *cy, int *w, int *h)
{
    if (!origin_valid)
        return 0;
    origin_valid = 0;
    if (cx)
        *cx = origin_cx;
    if (cy)
        *cy = origin_cy;
    if (w)
        *w = origin_w;
    if (h)
        *h = origin_h;
    return 1;
}

static void winfx_key(WinFx *fx, int i, int x, int y, int w, int h)
{
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    fx->key[i].x = x;
    fx->key[i].y = y;
    fx->key[i].w = w;
    fx->key[i].h = h;
}

static void winfx_lerp(const MotionKey *a, const MotionKey *b, float t,
                       MotionKey *out)
{
    out->x = motion_lerp(a->x, b->x, t);
    out->y = motion_lerp(a->y, b->y, t);
    out->w = motion_lerp(a->w, b->w, t);
    out->h = motion_lerp(a->h, b->h, t);
}

static void winfx_eval(const WinFx *fx, float t, MotionKey *out)
{
    float split = fx->split;
    float u, e;

    if (split < 0.05f)
        split = 0.05f;
    if (split > 0.95f)
        split = 0.95f;
    if (t <= split) {
        u = t / split;
        e = motion_ease(fx->ease0, u);
        winfx_lerp(&fx->key[0], &fx->key[1], e, out);
    } else {
        u = (t - split) / (1.0f - split);
        e = motion_ease(fx->ease1, u);
        winfx_lerp(&fx->key[1], &fx->key[2], e, out);
    }
}

static void winfx_apply(WinFx *fx, const MotionKey *k)
{
    if (fx->win == None)
        return;
    XMoveResizeWindow(sc.dpy, fx->win, k->x, k->y,
                      (unsigned)k->w, (unsigned)k->h);
    if (fx->shaped)
        ui_shape_round(fx->win, k->w, k->h, fx->radius);
}

static void winfx_step(void *ctx, float t)
{
    WinFx *fx = ctx;
    MotionKey k;

    if (fx->win == None)
        return;
    winfx_eval(fx, t, &k);
    winfx_apply(fx, &k);
    fx->has_pose = 1;
}

static void winfx_done_cb(void *ctx)
{
    WinFx *fx = ctx;
    void (*done)(void *) = fx->done;
    void *done_ctx = fx->done_ctx;

    fx->animating = 0;
    fx->closing = 0;
    fx->done = NULL;
    fx->done_ctx = NULL;
    if (done)
        done(done_ctx);
}

/* Query the window's current resting/partial pose.  Works while unmapped. */
static int winfx_current(const WinFx *fx, MotionKey *out)
{
    XWindowAttributes wa;

    if (fx->win == None)
        return 0;
    if (!XGetWindowAttributes(sc.dpy, fx->win, &wa))
        return 0;
    if (wa.width < 1 || wa.height < 1)
        return 0;
    out->x = wa.x;
    out->y = wa.y;
    out->w = wa.width;
    out->h = wa.height;
    return 1;
}

int winfx_open(WinFx *fx, Window win, const char *app_key,
               int x, int y, int w, int h, int shaped, int radius,
               void (*done)(void *ctx), void *done_ctx)
{
    MotionKey start;
    int ax = sc.sw / 2, ay = sc.sh - 60, aw = 42, ah = 42;
    int have_start = 0;
    int ow, oh;

    if (!fx || win == None)
        return 0;

    motion_cancel(MOTION_CH_GEOMETRY, fx, 0);
    fx->win = win;
    fx->x = x;
    fx->y = y;
    fx->w = w;
    fx->h = h;
    fx->shaped = shaped;
    fx->radius = radius;
    fx->closing = 0;
    fx->done = done;
    fx->done_ctx = done_ctx;
    snprintf(fx->app_key, sizeof(fx->app_key), "%s",
             app_key && app_key[0] ? app_key : "");

    if (!cfg.animations || w < 2 || h < 2) {
        MotionKey rest = { x, y, w, h };

        origin_valid = 0;
        winfx_apply(fx, &rest);
        fx->animating = 0;
        fx->has_pose = 1;
        if (done)
            done(done_ctx);
        return 0;
    }

    /*
     * Launcher/Dock supply an explicit origin.  Otherwise resume from the
     * window's current pose (re-show, interrupted close) or fall back to
     * the dock tile that represents this app.
     */
    if (origin_take(&ax, &ay, &aw, &ah)) {
        start.x = ax - aw / 2;
        start.y = ay - ah / 2;
        start.w = aw;
        start.h = ah;
        have_start = 1;
    } else if (fx->has_pose && winfx_current(fx, &start)) {
        have_start = 1;
    }
    if (!have_start) {
        dock_app_anchor(fx->app_key, &ax, &ay, &aw, &ah);
        if (aw < 24)
            aw = 24;
        if (ah < 24)
            ah = 24;
        start.x = ax - aw / 2;
        start.y = ay - ah / 2;
        start.w = aw;
        start.h = ah;
    }

    winfx_key(fx, 0, start.x, start.y, start.w, start.h);
    ow = w + w * 4 / 100;
    oh = h + h * 4 / 100;
    if (ow > sc.sw)
        ow = sc.sw;
    if (oh > sc.sh)
        oh = sc.sh;
    winfx_key(fx, 1, x + (w - ow) / 2, y + (h - oh) / 2, ow, oh);
    winfx_key(fx, 2, x, y, w, h);
    fx->split = 0.64f;
    fx->ease0 = MOTION_EASE_IN_OUT;
    fx->ease1 = MOTION_EASE_OUT;

    /* Park at the origin before the caller maps the window. */
    winfx_apply(fx, &fx->key[0]);
    fx->animating = 1;
    if (!motion_start(MOTION_CH_GEOMETRY, fx, WINFX_OPEN_MS, MOTION_LINEAR,
                      winfx_step, winfx_done_cb, fx)) {
        MotionKey rest = { x, y, w, h };

        winfx_apply(fx, &rest);
        fx->animating = 0;
        fx->has_pose = 1;
        if (done)
            done(done_ctx);
        return 0;
    }
    return 1;
}

int winfx_close(WinFx *fx, Window win,
                void (*done)(void *ctx), void *done_ctx)
{
    MotionKey start;
    int cx, cy, iw = 42, ih = 42;
    int w1, h1, mcx, mcy;

    if (!fx || win == None) {
        if (done)
            done(done_ctx);
        return 0;
    }

    motion_cancel(MOTION_CH_GEOMETRY, fx, 0);
    origin_valid = 0;               /* a close never starts from a menu */
    fx->win = win;
    fx->closing = 1;
    fx->done = done;
    fx->done_ctx = done_ctx;

    if (!cfg.animations || !winfx_current(fx, &start)) {
        fx->animating = 0;
        fx->closing = 0;
        if (done)
            done(done_ctx);
        return 0;
    }

    dock_app_anchor(fx->app_key, &cx, &cy, &iw, &ih);
    if (iw < 32)
        iw = 32;
    if (ih < 32)
        ih = 32;
    w1 = start.w * 68 / 100;
    h1 = start.h * 92 / 100;
    if (w1 < 60)
        w1 = 60;
    if (h1 < 44)
        h1 = 44;
    if (w1 > start.w)
        w1 = start.w;
    if (h1 > start.h)
        h1 = start.h;

    mcx = ((start.x + start.w / 2) * 65 + cx * 35) / 100;
    mcy = ((start.y + start.h / 2) * 65 + cy * 35) / 100;

    winfx_key(fx, 0, start.x, start.y, start.w, start.h);
    winfx_key(fx, 1, mcx - w1 / 2, mcy - h1 / 2, w1, h1);
    winfx_key(fx, 2, cx - iw / 2, cy - ih / 2, iw, ih);
    fx->split = 0.45f;
    fx->ease0 = MOTION_EASE_IN_OUT;
    fx->ease1 = MOTION_EASE_IN_OUT;

    fx->animating = 1;
    if (!motion_start(MOTION_CH_GEOMETRY, fx, WINFX_CLOSE_MS, MOTION_LINEAR,
                      winfx_step, winfx_done_cb, fx)) {
        fx->animating = 0;
        fx->closing = 0;
        fx->has_pose = 1;
        if (done)
            done(done_ctx);
        return 0;
    }
    return 1;
}

void winfx_cancel(WinFx *fx, int finish)
{
    if (!fx)
        return;
    motion_cancel(MOTION_CH_GEOMETRY, fx, finish);
    if (!finish) {
        fx->animating = 0;
        fx->closing = 0;
        fx->done = NULL;
        fx->done_ctx = NULL;
    }
}
