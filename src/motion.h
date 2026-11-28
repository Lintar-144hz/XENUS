/*
 * XENUS Motion Engine
 *
 * A single, timer-driven animation scheduler for the whole shell.  It
 * shares the existing poll() event loop: while at least one tween is
 * running the loop wakes on the next frame boundary, and when nothing is
 * animating the timeout goes back to -1 so the process sleeps.
 *
 * No threads, no permanent polling, no compositor assumptions.  Every
 * visual effect is produced with plain X11 geometry / colour changes.
 */
#ifndef XENUS_MOTION_H
#define XENUS_MOTION_H

#include <stdint.h>

/* easing curves */
enum {
    MOTION_LINEAR = 0,
    MOTION_EASE_OUT,        /* cubic out                          */
    MOTION_EASE_IN,         /* cubic in                           */
    MOTION_EASE_IN_OUT,     /* cubic in-out                       */
    MOTION_EASE_OUT_QUINT,  /* quintic out, crisp liquid settle   */
    MOTION_EASE_OUT_BACK,   /* overshoot then settle              */
    MOTION_EASE_IN_BACK,    /* wind-up then accelerate away       */
    MOTION_EASE_SMOOTH,     /* smoothstep                         */
    MOTION_EASE_IN_OUT_QUINT /* gentle accelerate, long decelerate */
};

/* animation channels; one tween per (channel, owner) pair */
enum {
    MOTION_CH_GEOMETRY = 1,     /* window frame open / close / minimise */
    MOTION_CH_FOCUS,            /* title bar + border cross-fade */
    MOTION_CH_MENU,             /* launcher / command prompt slide */
    MOTION_CH_DOCK,             /* dock hover growth */
    MOTION_CH_WORKSPACE,        /* workspace slide */
    MOTION_CH_OVERVIEW          /* overview zoom in / out */
};

typedef void (*motion_step_fn)(void *ctx, float t);
typedef void (*motion_done_fn)(void *ctx);

void motion_init(void);
void motion_shutdown(void);

/*
 * Start (or restart) a tween.  t runs from 0.0 to 1.0 over duration_ms.
 * duration_ms <= 0 applies the final frame immediately.  The returned
 * value is 1 when the tween was armed, 0 when the table is full.
 */
int motion_start(int channel, void *owner, int duration_ms, int easing,
                 motion_step_fn step, motion_done_fn done, void *ctx);

/*
 * Cancel the tween owned by (channel, owner).  When finish is non-zero the
 * final frame and the done callback are executed, which lets callers snap
 * a surface back to its resting state before reusing it.
 */
void motion_cancel(int channel, void *owner, int finish);
void motion_cancel_owner(void *owner, int finish);

void motion_tick(void);            /* advance all active tweens */
int motion_timeout_ms(void);       /* ms until the next frame, -1 = idle */
int motion_active(void);

uint64_t motion_now_ms(void);
float motion_ease(int kind, float t);
float motion_ease_out(float t);
float motion_ease_in(float t);
float motion_ease_in_out(float t);
float motion_ease_out_back(float t);
float motion_ease_in_back(float t);
int motion_lerp(int a, int b, float t);
unsigned long motion_mix(unsigned long a, unsigned long b, float t);

#endif /* XENUS_MOTION_H */
