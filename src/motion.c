/*
 * XENUS Motion Engine - a small fixed-table tween scheduler.
 *
 * Design notes:
 *   - one shared monotonic clock, read once per tick;
 *   - a bounded table (no allocation) so an animation can never leak;
 *   - 60 Hz frame pacing while active, -1 timeout while idle;
 *   - callbacks are plain C and must not block.
 */
#include "motion.h"

#include <string.h>
#include <time.h>

#define MOTION_MAX 96
#define MOTION_FRAME_MS 16

typedef struct {
    int used;
    int channel;
    void *owner;
    uint64_t start;
    uint64_t last_frame;
    int duration;
    int easing;
    motion_step_fn step;
    motion_done_fn done;
    void *ctx;
} Motion;

static Motion motions[MOTION_MAX];
static int active_count;
static uint64_t fallback_clock;

uint64_t motion_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u +
               (uint64_t)ts.tv_nsec / 1000000u;
    /* extremely unlikely: still keep a monotonic-ish counter */
    fallback_clock += MOTION_FRAME_MS;
    return fallback_clock;
}

static float clamp01(float t)
{
    if (t < 0.0f)
        return 0.0f;
    if (t > 1.0f)
        return 1.0f;
    return t;
}

float motion_ease_out(float t)
{
    float u = 1.0f - clamp01(t);

    return 1.0f - u * u * u;
}

float motion_ease_in(float t)
{
    t = clamp01(t);
    return t * t * t;
}

float motion_ease_out_back(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float u = clamp01(t) - 1.0f;

    return 1.0f + c3 * u * u * u + c1 * u * u;
}

float motion_ease_in_back(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;

    t = clamp01(t);
    return t * t * ((c3 + 1.0f) * t - c3);
}

float motion_ease_in_out(float t)
{
    t = clamp01(t);
    if (t < 0.5f)
        return 4.0f * t * t * t;
    {
        float u = -2.0f * t + 2.0f;

        return 1.0f - u * u * u / 2.0f;
    }
}

float motion_ease(int kind, float t)
{
    t = clamp01(t);
    switch (kind) {
    case MOTION_EASE_OUT:
        return motion_ease_out(t);
    case MOTION_EASE_IN:
        return motion_ease_in(t);
    case MOTION_EASE_IN_OUT:
        return motion_ease_in_out(t);
    case MOTION_EASE_OUT_QUINT: {
        float u = 1.0f - t;

        return 1.0f - u * u * u * u * u;
    }
    case MOTION_EASE_OUT_BACK:
        return motion_ease_out_back(t);
    case MOTION_EASE_IN_BACK:
        return motion_ease_in_back(t);
    case MOTION_EASE_SMOOTH:
        return t * t * (3.0f - 2.0f * t);
    case MOTION_EASE_IN_OUT_QUINT:
        if (t < 0.5f)
            return 16.0f * t * t * t * t * t;
        {
            float u = -2.0f * t + 2.0f;

            return 1.0f - u * u * u * u * u / 2.0f;
        }
    default:
        return t;
    }
}

int motion_lerp(int a, int b, float t)
{
    float v = (float)(b - a) * t;

    /* Round to nearest, including shrinking (negative) deltas.  Adding
       +0.5 on both sides would truncate -33.5 to -33 and leave a 1 px
       overshoot at t == 1. */
    return a + (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

unsigned long motion_mix(unsigned long a, unsigned long b, float t)
{
    unsigned int ar = (unsigned int)((a >> 16) & 0xff);
    unsigned int ag = (unsigned int)((a >> 8) & 0xff);
    unsigned int ab = (unsigned int)(a & 0xff);
    unsigned int br = (unsigned int)((b >> 16) & 0xff);
    unsigned int bg = (unsigned int)((b >> 8) & 0xff);
    unsigned int bb = (unsigned int)(b & 0xff);
    unsigned int r = (unsigned int)((float)ar + ((float)br - (float)ar) * t);
    unsigned int g = (unsigned int)((float)ag + ((float)bg - (float)ag) * t);
    unsigned int bl = (unsigned int)((float)ab + ((float)bb - (float)ab) * t);

    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | bl;
}

void motion_init(void)
{
    memset(motions, 0, sizeof(motions));
    active_count = 0;
    fallback_clock = motion_now_ms();
}

void motion_shutdown(void)
{
    memset(motions, 0, sizeof(motions));
    active_count = 0;
}

static void run_step(Motion *m, float t)
{
    if (m->step)
        m->step(m->ctx, t);
}

static void run_done(Motion *m)
{
    if (m->done)
        m->done(m->ctx);
}

static void remove_at(int i, int finish)
{
    Motion copy;

    if (!motions[i].used)
        return;
    copy = motions[i];
    motions[i].used = 0;
    active_count--;
    if (finish) {
        run_step(&copy, 1.0f);
        run_done(&copy);
    }
}

int motion_start(int channel, void *owner, int duration_ms, int easing,
                 motion_step_fn step, motion_done_fn done, void *ctx)
{
    int i, slot = -1;

    motion_cancel(channel, owner, 0);

    if (duration_ms <= 0 || !step) {
        Motion immediate;

        memset(&immediate, 0, sizeof(immediate));
        immediate.step = step;
        immediate.done = done;
        immediate.ctx = ctx;
        run_step(&immediate, 1.0f);
        run_done(&immediate);
        return 1;
    }

    for (i = 0; i < MOTION_MAX; i++) {
        if (!motions[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return 0;

    motions[slot].used = 1;
    motions[slot].channel = channel;
    motions[slot].owner = owner;
    motions[slot].start = motion_now_ms();
    motions[slot].last_frame = 0;
    motions[slot].duration = duration_ms;
    motions[slot].easing = easing;
    motions[slot].step = step;
    motions[slot].done = done;
    motions[slot].ctx = ctx;
    active_count++;
    return 1;
}

void motion_cancel(int channel, void *owner, int finish)
{
    int i;

    for (i = 0; i < MOTION_MAX; i++) {
        if (motions[i].used && motions[i].channel == channel &&
            motions[i].owner == owner) {
            remove_at(i, finish);
            return;
        }
    }
}

void motion_cancel_owner(void *owner, int finish)
{
    int i;

    for (i = 0; i < MOTION_MAX; i++) {
        if (motions[i].used && motions[i].owner == owner)
            remove_at(i, finish);
    }
}

int motion_active(void)
{
    return active_count > 0;
}

void motion_tick(void)
{
    uint64_t now;
    int i;

    if (active_count <= 0)
        return;

    now = motion_now_ms();
    for (i = 0; i < MOTION_MAX; i++) {
        Motion finish;
        Motion *m = &motions[i];
        uint64_t elapsed;
        float t;

        if (!m->used)
            continue;
        elapsed = now >= m->start ? now - m->start : 0;
        if (elapsed >= (uint64_t)m->duration) {
            /*
             * Retire the slot before invoking user callbacks.  A callback
             * is allowed to start or cancel tweens (including one in this
             * very slot), so only the private copy may be used afterwards.
             */
            finish = *m;
            m->used = 0;
            active_count--;
            run_step(&finish, 1.0f);
            run_done(&finish);
            continue;
        }
        /*
         * Pace every tween to the shared 16 ms frame grid.  A poll wake-up
         * caused by input or by another subsystem must not produce a second
         * visual step inside the same frame; that is what makes a geometry
         * animation look uneven even though the scheduler is correct.
         */
        {
            uint64_t frame = elapsed / MOTION_FRAME_MS;

            if (frame <= m->last_frame)
                continue;
            m->last_frame = frame;
        }
        t = (float)elapsed / (float)m->duration;
        /* A step callback may cancel/replace this tween; do not use m
           after the call. */
        run_step(m, motion_ease(m->easing, t));
    }
}

int motion_timeout_ms(void)
{
    uint64_t now;
    int best = -1;
    int i;

    if (active_count <= 0)
        return -1;

    now = motion_now_ms();
    for (i = 0; i < MOTION_MAX; i++) {
        Motion *m = &motions[i];
        uint64_t elapsed;
        int ms;

        if (!m->used)
            continue;
        elapsed = now >= m->start ? now - m->start : 0;
        if (elapsed >= (uint64_t)m->duration)
            return 0;
        {
            uint64_t left = (uint64_t)m->duration - elapsed;
            uint64_t next = MOTION_FRAME_MS - (elapsed % MOTION_FRAME_MS);

            /* sleep to the next fixed 60 Hz boundary so frame intervals do
               not drift by the event-loop processing time */
            if (next > left)
                next = left;
            ms = (int)next;
        }
        if (ms < 1)
            ms = 1;
        if (best < 0 || ms < best)
            best = ms;
    }
    return best;
}
