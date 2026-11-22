/*
 * XENUS BIOS (X-FIRM) - firmware setup UI.
 *
 * Self-contained Xlib application: no XENUS desktop symbols, no UCore link,
 * no window manager required.  It draws into a back buffer and copies the
 * finished frame to the window, and it uses only core X fonts.
 */
#define _POSIX_C_SOURCE 200809L

#include <xenus/bios.h>

#include "bios_backend.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HEADER_H      56
#define FOOTER_H      58
#define SIDEBAR_W     220
#define CONTENT_PAD   28
#define MAX_HITS      160
#define MAX_KEY_ROWS  16
#define MAX_DIFFS     XENUS_BIOS_MAX_DIFF

enum {
    PAGE_OVERVIEW = 0,
    PAGE_SYSTEM,
    PAGE_DRIVERS,
    PAGE_UCORE,
    PAGE_ANDROID,
    PAGE_BOOT,
    PAGE_RECOVERY,
    PAGE_SAVE,
    PAGE_COUNT
};

enum {
    ACT_NONE = 0,
    ACT_NAV = 100,               /* + page */
    ACT_CLOSE = 120,
    ACT_FOOTER_APPLY = 200,
    ACT_FOOTER_DISCARD,
    ACT_FOOTER_SAVE_EXIT,
    ACT_SYSTEM_OPT = 300,        /* + field*8 + option */
    ACT_SYSTEM_TOGGLE = 340,     /* + field */
    ACT_DRIVER_ROW = 400,        /* + index */
    ACT_UCORE_ROW = 450,         /* + index */
    ACT_ANDROID_ROW = 500,       /* + index */
    ACT_BOOT_MODE = 550,         /* + mode */
    ACT_BOOT_DIAG = 560,
    ACT_BOOT_DELAY_MINUS = 561,
    ACT_BOOT_DELAY_PLUS = 562,
    ACT_RECOVERY_ROW = 600,      /* + action */
    ACT_SAVE_ROW = 650,          /* + action */
    ACT_CONFIRM_YES = 700,
    ACT_CONFIRM_NO = 701,
    ACT_QUICK = 750               /* + action */
};

enum {
    CONFIRM_NONE = 0,
    CONFIRM_EXIT,
    CONFIRM_DISCARD,
    CONFIRM_RESTORE,
    CONFIRM_FACTORY
};

typedef struct {
    int x, y, w, h, id, row;
} Hit;

typedef struct {
    Display *dpy;
    int screen;
    int W, H;
    int quit;
    Window win;
    Pixmap buffer;
    GC gc;
    Colormap cmap;
    XFontStruct *font_title;
    XFontStruct *font_bold;
    XFontStruct *font_body;
    XFontStruct *font_small;
    XFontStruct *font_mono;

    unsigned long c_bg;
    unsigned long c_sidebar;
    unsigned long c_card;
    unsigned long c_card2;
    unsigned long c_line;
    unsigned long c_fg;
    unsigned long c_muted;
    unsigned long c_accent;
    unsigned long c_accent_soft;
    unsigned long c_green;
    unsigned long c_yellow;
    unsigned long c_red;
    unsigned long c_white;

    int page;
    int focus_row;
    int hover_id;
    int confirm;
    char status[256];
    int status_error;

    xenus_bios_engine_t eng;
    xenus_bios_env_t env;
    Hit hits[MAX_HITS];
    int nhits;
    int diff_off;
    uint64_t last_probe_ms;
    uint64_t last_draw_ms;
} BiosUi;

static BiosUi ui;
static Drawable canvas;

/* ------------------------------------------------------------- helpers -- */

static uint64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void copy_str(char *dst, size_t size, const char *src)
{
    if (!size)
        return;
    if (!src)
        src = "";
    snprintf(dst, size, "%s", src);
}

static unsigned long rgb(unsigned long hex)
{
    return hex;
}

static unsigned long mix(unsigned long a, unsigned long b, int pct)
{
    unsigned long ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    unsigned long br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    unsigned long r = (ar * (100 - pct) + br * pct) / 100;
    unsigned long g = (ag * (100 - pct) + bg * pct) / 100;
    unsigned long bl = (ab * (100 - pct) + bb * pct) / 100;

    return (r << 16) | (g << 8) | bl;
}

static void set_color(unsigned long c)
{
    XSetForeground(ui.dpy, ui.gc, c);
}

static void fill_rect(int x, int y, int w, int h, unsigned long c)
{
    if (w <= 0 || h <= 0)
        return;
    set_color(c);
    XFillRectangle(ui.dpy, canvas, ui.gc, x, y, (unsigned)w, (unsigned)h);
}

static void round_fill(int x, int y, int w, int h, int r, unsigned long c)
{
    if (w <= 0 || h <= 0)
        return;
    if (r <= 1) {
        fill_rect(x, y, w, h, c);
        return;
    }
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    set_color(c);
    XFillRectangle(ui.dpy, canvas, ui.gc, x + r, y, (unsigned)(w - 2 * r),
                   (unsigned)h);
    XFillRectangle(ui.dpy, canvas, ui.gc, x, y + r, (unsigned)w,
                   (unsigned)(h - 2 * r));
    XFillArc(ui.dpy, canvas, ui.gc, x, y, (unsigned)(2 * r),
             (unsigned)(2 * r), 90 * 64, 90 * 64);
    XFillArc(ui.dpy, canvas, ui.gc, x + w - 2 * r, y, (unsigned)(2 * r),
             (unsigned)(2 * r), 0, 90 * 64);
    XFillArc(ui.dpy, canvas, ui.gc, x, y + h - 2 * r, (unsigned)(2 * r),
             (unsigned)(2 * r), 180 * 64, 90 * 64);
    XFillArc(ui.dpy, canvas, ui.gc, x + w - 2 * r, y + h - 2 * r,
             (unsigned)(2 * r), (unsigned)(2 * r), 270 * 64, 90 * 64);
}

static void round_outline(int x, int y, int w, int h, int r, unsigned long c)
{
    if (w <= 0 || h <= 0)
        return;
    if (r <= 1) {
        set_color(c);
        XDrawRectangle(ui.dpy, canvas, ui.gc, x, y, (unsigned)w,
                       (unsigned)h);
        return;
    }
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    set_color(c);
    XDrawLine(ui.dpy, canvas, ui.gc, x + r, y, x + w - r - 1, y);
    XDrawLine(ui.dpy, canvas, ui.gc, x + r, y + h - 1, x + w - r - 1,
              y + h - 1);
    XDrawLine(ui.dpy, canvas, ui.gc, x, y + r, x, y + h - r - 1);
    XDrawLine(ui.dpy, canvas, ui.gc, x + w - 1, y + r, x + w - 1,
              y + h - r - 1);
    XDrawArc(ui.dpy, canvas, ui.gc, x, y, (unsigned)(2 * r),
             (unsigned)(2 * r), 90 * 64, 90 * 64);
    XDrawArc(ui.dpy, canvas, ui.gc, x + w - 2 * r, y, (unsigned)(2 * r),
             (unsigned)(2 * r), 0, 90 * 64);
    XDrawArc(ui.dpy, canvas, ui.gc, x, y + h - 2 * r, (unsigned)(2 * r),
             (unsigned)(2 * r), 180 * 64, 90 * 64);
    XDrawArc(ui.dpy, canvas, ui.gc, x + w - 2 * r, y + h - 2 * r,
             (unsigned)(2 * r), (unsigned)(2 * r), 270 * 64, 90 * 64);
}

static int text_w(XFontStruct *f, const char *s)
{
    if (!f || !s)
        return 0;
    return XTextWidth(f, s, (int)strlen(s));
}

static void text_at(XFontStruct *f, int x, int baseline, unsigned long c,
                    const char *s)
{
    if (!f || !s)
        return;
    set_color(c);
    XSetFont(ui.dpy, ui.gc, f->fid);
    XDrawString(ui.dpy, canvas, ui.gc, x, baseline, s, (int)strlen(s));
}

static void text_right(XFontStruct *f, int right, int baseline,
                       unsigned long c, const char *s)
{
    text_at(f, right - text_w(f, s), baseline, c, s);
}

static void text_clip(XFontStruct *f, int x, int max_x, int baseline,
                      unsigned long c, const char *s)
{
    char buf[512];
    size_t len;
    size_t i;
    int w;

    if (!f || !s || x >= max_x)
        return;
    w = text_w(f, s);
    if (x + w <= max_x) {
        text_at(f, x, baseline, c, s);
        return;
    }
    copy_str(buf, sizeof(buf), s);
    len = strlen(buf);
    for (i = len; i > 0; i--) {
        buf[i] = '\0';
        if (x + text_w(f, buf) + text_w(f, "...") <= max_x) {
            strncat(buf, "...", sizeof(buf) - strlen(buf) - 1);
            text_at(f, x, baseline, c, buf);
            return;
        }
    }
}

static int baseline_in(int y, int h, XFontStruct *f)
{
    if (!f)
        return y + h / 2;
    return y + (h + f->ascent - f->descent) / 2;
}

static void hit_add(int x, int y, int w, int h, int id, int row)
{
    if (ui.nhits >= MAX_HITS || w <= 0 || h <= 0)
        return;
    ui.hits[ui.nhits].x = x;
    ui.hits[ui.nhits].y = y;
    ui.hits[ui.nhits].w = w;
    ui.hits[ui.nhits].h = h;
    ui.hits[ui.nhits].id = id;
    ui.hits[ui.nhits].row = row;
    ui.nhits++;
}

static const Hit *hit_at(int x, int y)
{
    int i;

    for (i = ui.nhits - 1; i >= 0; i--) {
        const Hit *h = &ui.hits[i];

        if (x >= h->x && x < h->x + h->w && y >= h->y &&
            y < h->y + h->h)
            return h;
    }
    return NULL;
}

static int content_x(void)
{
    return SIDEBAR_W + CONTENT_PAD;
}

static int content_w(void)
{
    return ui.W - content_x() - CONTENT_PAD;
}

static int content_top(void)
{
    return HEADER_H + 26 + 70;
}

static void set_status(const char *text, int error)
{
    copy_str(ui.status, sizeof(ui.status), text);
    ui.status_error = error ? 1 : 0;
}

static int diff_count(xenus_bios_diff_t *out, int max)
{
    return xenus_bios_config_diff(&ui.eng.live, &ui.eng.draft, out, max);
}

static int collect_validation(xenus_bios_validation_t *v)
{
    xenus_bios_config_validate(&ui.eng.draft, v);
    return v->errors;
}

static int page_rows(int page)
{
    static const int rows[PAGE_COUNT] = { 3, 5, 6, 5, 6, 3, 5, 4 };

    if (page < 0 || page >= PAGE_COUNT)
        return 0;
    return rows[page];
}

/* --------------------------------------------------------------- fonts -- */

static XFontStruct *load_one(const char *name)
{
    XFontStruct *f = XLoadQueryFont(ui.dpy, name);

    return f;
}

static XFontStruct *load_first(const char *const *names)
{
    int i;

    for (i = 0; names[i]; i++) {
        XFontStruct *f = load_one(names[i]);

        if (f)
            return f;
    }
    return NULL;
}

static void load_fonts(void)
{
    static const char *const title_fonts[] = {
        "10x20", "9x15bold", "9x15", "fixed", NULL
    };
    static const char *const bold_fonts[] = {
        "9x15bold", "10x20", "fixed", NULL
    };
    static const char *const body_fonts[] = {
        "9x15", "10x20", "fixed", NULL
    };
    static const char *const small_fonts[] = {
        "6x13", "7x13", "9x15", "fixed", NULL
    };
    static const char *const mono_fonts[] = {
        "6x13", "fixed", NULL
    };

    ui.font_title = load_first(title_fonts);
    ui.font_bold = load_first(bold_fonts);
    ui.font_body = load_first(body_fonts);
    ui.font_small = load_first(small_fonts);
    ui.font_mono = load_first(mono_fonts);
    if (!ui.font_title)
        ui.font_title = ui.font_body;
    if (!ui.font_bold)
        ui.font_bold = ui.font_body;
    if (!ui.font_small)
        ui.font_small = ui.font_body;
    if (!ui.font_mono)
        ui.font_mono = ui.font_body;
}

/* -------------------------------------------------------------- colors -- */

static unsigned long alloc_color(const char *spec, unsigned long fallback)
{
    XColor c;
    XColor exact;

    if (!spec || !XParseColor(ui.dpy, ui.cmap, spec, &c))
        return fallback;
    if (!XAllocColor(ui.dpy, ui.cmap, &c)) {
        (void)exact;
        return fallback;
    }
    return c.pixel;
}

static void load_colors(void)
{
    ui.c_bg = alloc_color("#0b0d12", rgb(0x0b0d12));
    ui.c_sidebar = alloc_color("#11141c", rgb(0x11141c));
    ui.c_card = alloc_color("#171b25", rgb(0x171b25));
    ui.c_card2 = alloc_color("#1d2230", rgb(0x1d2230));
    ui.c_line = alloc_color("#2a3040", rgb(0x2a3040));
    ui.c_fg = alloc_color("#f2f5fa", rgb(0xf2f5fa));
    ui.c_muted = alloc_color("#8b95a7", rgb(0x8b95a7));
    ui.c_accent = alloc_color("#2f81f7", rgb(0x2f81f7));
    ui.c_green = alloc_color("#3fb950", rgb(0x3fb950));
    ui.c_yellow = alloc_color("#d29922", rgb(0xd29922));
    ui.c_red = alloc_color("#f85149", rgb(0xf85149));
    ui.c_white = alloc_color("#ffffff", rgb(0xffffff));
    ui.c_accent_soft = mix(ui.c_accent, ui.c_bg, 22);
}

/* -------------------------------------------------------------- chrome -- */

static void draw_logo(int x, int y, int size)
{
    int pad = size / 4;

    round_fill(x, y, size, size, size / 4, ui.c_accent);
    set_color(ui.c_white);
    XSetLineAttributes(ui.dpy, ui.gc, 2, LineSolid, CapRound, JoinRound);
    XDrawLine(ui.dpy, canvas, ui.gc, x + pad, y + pad,
              x + size - pad - 1, y + size - pad - 1);
    XDrawLine(ui.dpy, canvas, ui.gc, x + size - pad - 1, y + pad,
              x + pad, y + size - pad - 1);
    XSetLineAttributes(ui.dpy, ui.gc, 0, LineSolid, CapButt, JoinMiter);
}

static void draw_chip(int x, int y, int w, int h, const char *text,
                      unsigned long color, unsigned long bg)
{
    round_fill(x, y, w, h, h / 2, bg);
    text_at(ui.font_small, x + (w - text_w(ui.font_small, text)) / 2,
            baseline_in(y, h, ui.font_small), color, text);
}

static void draw_toggle(int x, int y, int w, int h, int on, int enabled)
{
    unsigned long bg = on ? ui.c_accent : mix(ui.c_card2, ui.c_line, 55);
    unsigned long knob = enabled ? ui.c_white : ui.c_muted;
    int ks = h - 6;

    if (!enabled)
        bg = mix(ui.c_card2, ui.c_line, 25);
    round_fill(x, y, w, h, h / 2, bg);
    round_fill(on ? x + w - ks - 3 : x + 3, y + 3, ks, ks, ks / 2, knob);
}

static void draw_segment(int x, int y, int w, int h, const char *label,
                         int active, int enabled)
{
    unsigned long bg = active ? ui.c_accent : ui.c_card2;
    unsigned long fg = active ? ui.c_white : ui.c_fg;

    if (!enabled) {
        bg = mix(ui.c_card2, ui.c_bg, 25);
        fg = ui.c_muted;
    }
    round_fill(x, y, w, h, 7, bg);
    if (!active)
        round_outline(x, y, w, h, 7, ui.c_line);
    text_at(ui.font_small, x + (w - text_w(ui.font_small, label)) / 2,
            baseline_in(y, h, ui.font_small), fg, label);
}

static void draw_card(int x, int y, int w, int h, const char *title)
{
    round_fill(x, y, w, h, 12, ui.c_card);
    round_outline(x, y, w, h, 12, ui.c_line);
    if (title && *title)
        text_at(ui.font_bold, x + 18, y + 25, ui.c_fg, title);
}

static void focus_outline(int x, int y, int w, int h, int row)
{
    if (row < 0 || row != ui.focus_row)
        return;
    round_outline(x, y, w, h, 8, ui.c_accent);
    round_outline(x + 1, y + 1, w - 2, h - 2, 7,
                  mix(ui.c_accent, ui.c_card, 45));
}

static void page_header(const char *title, const char *subtitle)
{
    int x = content_x();
    int y = HEADER_H + 26;

    text_at(ui.font_title, x, y + ui.font_title->ascent, ui.c_fg, title);
    text_at(ui.font_small, x, y + 50, ui.c_muted, subtitle);
    set_color(ui.c_line);
    XDrawLine(ui.dpy, canvas, ui.gc, x, y + 60, x + content_w(), y + 60);
}

static void draw_header(void)
{
    char buf[128];
    time_t now = time(NULL);
    struct tm tmv;
    struct tm *tmp = localtime(&now);
    xenus_bios_diff_t diffs[MAX_DIFFS];
    int n = diff_count(diffs, MAX_DIFFS);
    int vx = ui.W - 18;

    fill_rect(0, 0, ui.W, HEADER_H, ui.c_bg);
    set_color(ui.c_line);
    XDrawLine(ui.dpy, canvas, ui.gc, 0, HEADER_H - 1, ui.W, HEADER_H - 1);
    draw_logo(18, (HEADER_H - 34) / 2, 34);
    text_at(ui.font_title, 64, 26, ui.c_fg, "XENUS BIOS");
    text_at(ui.font_small, 65, 43, ui.c_muted,
            "X-FIRM firmware setup  -  standalone configuration");

    /* close control (touch-friendly) */
    round_fill(vx - 18, (HEADER_H - 22) / 2, 22, 22, 6,
               ui.hover_id == ACT_CLOSE ? ui.c_card2 : ui.c_bg);
    set_color(ui.c_muted);
    XDrawLine(ui.dpy, canvas, ui.gc, vx - 12, (HEADER_H - 6) / 2,
              vx, (HEADER_H + 6) / 2);
    XDrawLine(ui.dpy, canvas, ui.gc, vx, (HEADER_H - 6) / 2,
              vx - 12, (HEADER_H + 6) / 2);
    hit_add(vx - 22, 8, 30, HEADER_H - 16, ACT_CLOSE, -1);

    /* right-side status chips */
    if (tmp && localtime_r(&now, &tmv))
        strftime(buf, sizeof(buf), "%a %d %b  %H:%M", &tmv);
    else
        copy_str(buf, sizeof(buf), "--:--");
    text_right(ui.font_small, vx - 32, 34, ui.c_muted, buf);
    vx -= 32 + text_w(ui.font_small, buf) + 14;

    {
        char status[64];
        int errors = 0;
        xenus_bios_validation_t v;

        errors = collect_validation(&v);
        if (errors) {
            snprintf(status, sizeof(status), "ISSUES %d", errors);
            draw_chip(vx - 78, 18, 78, 20, status, ui.c_red,
                      mix(ui.c_red, ui.c_bg, 78));
        } else {
            draw_chip(vx - 58, 18, 58, 20, "VALID", ui.c_green,
                      mix(ui.c_green, ui.c_bg, 80));
        }
        vx -= 90;
    }
    if (n > 0) {
        snprintf(buf, sizeof(buf), "STAGED %d", n);
        draw_chip(vx - 92, 18, 92, 20, buf, ui.c_yellow,
                  mix(ui.c_yellow, ui.c_bg, 82));
    } else {
        draw_chip(vx - 64, 18, 64, 20, "CLEAN", ui.c_muted,
                  mix(ui.c_muted, ui.c_bg, 82));
    }
}

static void draw_sidebar(void)
{
    static const char *const names[PAGE_COUNT] = {
        "Overview", "System", "Drivers", "UCore",
        "Android Integration", "Boot", "Recovery", "Save & Exit"
    };
    static const char *const keys[PAGE_COUNT] = {
        "OV", "SY", "DR", "UC", "AN", "BT", "RC", "SE"
    };
    int x = 0;
    int y = HEADER_H;
    int i;
    int nav_y;

    fill_rect(x, y, SIDEBAR_W, ui.H - HEADER_H - FOOTER_H, ui.c_sidebar);
    set_color(ui.c_line);
    XDrawLine(ui.dpy, canvas, ui.gc, SIDEBAR_W - 1, y, SIDEBAR_W - 1,
              ui.H - FOOTER_H);

    text_at(ui.font_small, 22, HEADER_H + 24, ui.c_muted, "SETUP PAGES");
    nav_y = HEADER_H + 38;
    for (i = 0; i < PAGE_COUNT; i++) {
        int active = ui.page == i;
        int hover = ui.hover_id == ACT_NAV + i;
        int item_h = 43;
        int bx = 14;

        if (active)
            round_fill(bx, nav_y, SIDEBAR_W - 28, item_h, 9,
                       ui.c_accent_soft);
        else if (hover)
            round_fill(bx, nav_y, SIDEBAR_W - 28, item_h, 9, ui.c_card);
        if (active)
            round_fill(bx, nav_y + 9, 3, item_h - 18, 1, ui.c_accent);
        round_fill(bx + 14, nav_y + 11, 22, 22, 6,
                   active ? ui.c_accent : ui.c_card2);
        text_at(ui.font_small, bx + 19,
                nav_y + 11 + (22 + ui.font_small->ascent -
                              ui.font_small->descent) / 2,
                active ? ui.c_white : ui.c_muted, keys[i]);
        text_at(ui.font_body, bx + 46,
                nav_y + (item_h + ui.font_body->ascent -
                         ui.font_body->descent) / 2,
                active ? ui.c_fg : mix(ui.c_fg, ui.c_sidebar, 35), names[i]);
        hit_add(bx, nav_y, SIDEBAR_W - 28, item_h, ACT_NAV + i, -1);
        nav_y += item_h + 5;
    }

    {
        int by = ui.H - FOOTER_H - 82;

        set_color(ui.c_line);
        XDrawLine(ui.dpy, canvas, ui.gc, 20, by, SIDEBAR_W - 20, by);
        text_at(ui.font_bold, 20, by + 24, ui.c_fg, "X-FIRM 1.0");
        text_at(ui.font_small, 20, by + 42, ui.c_muted,
                "Standalone setup environment");
        text_at(ui.font_small, 20, by + 58, ui.c_muted,
                "C11 + Xlib  -  UCore optional");
    }
}

static void draw_footer(void)
{
    int y = ui.H - FOOTER_H;
    int x = content_x();
    int bx;
    int errors = 0;
    xenus_bios_validation_t v;
    unsigned long status_color;

    fill_rect(0, y, ui.W, FOOTER_H, ui.c_bg);
    set_color(ui.c_line);
    XDrawLine(ui.dpy, canvas, ui.gc, 0, y, ui.W, y);

    errors = collect_validation(&v);
    status_color = ui.status_error ? ui.c_red :
                   ui.eng.dirty ? ui.c_yellow :
                   errors ? ui.c_red : ui.c_green;
    fill_rect(x, y + 22, 7, 7, status_color);
    text_clip(ui.font_small, x + 15, ui.W - 390, y + 28,
              ui.status_error ? ui.c_fg : ui.c_muted, ui.status);
    text_at(ui.font_small, x, y + 47, mix(ui.c_muted, ui.c_bg, 25),
            "PgUp/PgDn or Tab pages  -  arrows adjust  -  Enter activate  -  "
            "Esc exits");

    bx = ui.W - 18;
    bx -= 104;
    draw_segment(bx, y + 14, 104, 30, "Save & Exit",
                 ui.page == PAGE_SAVE, 1);
    hit_add(bx, y + 14, 104, 30, ACT_FOOTER_SAVE_EXIT, -1);
    bx -= 84;
    draw_segment(bx, y + 14, 76, 30, "Apply", ui.eng.dirty, 1);
    hit_add(bx, y + 14, 76, 30, ACT_FOOTER_APPLY, -1);
    bx -= 92;
    draw_segment(bx, y + 14, 84, 30, "Discard",
                 ui.hover_id == ACT_FOOTER_DISCARD, ui.eng.dirty);
    hit_add(bx, y + 14, 84, 30, ACT_FOOTER_DISCARD, -1);
}

/* --------------------------------------------------------- generic rows -- */

typedef struct {
    const char *label;
    const char *desc;
    const char *value;
    int option_count;
    const char *const *options;
    int selected;
    int toggle;               /* -1 none, else 0/1 */
    int enabled;
    int required;
} RowView;

static void draw_setting_row(int x, int y, int w, int h, int row,
                             const RowView *rv, const unsigned long *option_ids)
{
    int base = y + 19;
    int desc_base = y + 36;
    int rx = x + w - 18;

    if (row == ui.focus_row)
        round_fill(x + 8, y + 2, w - 16, h - 4, 8,
                   mix(ui.c_card2, ui.c_accent, 12));
    text_clip(ui.font_body, x + 18, x + w - 300, base, ui.c_fg, rv->label);
    text_clip(ui.font_small, x + 18, x + w - 300, desc_base, ui.c_muted,
              rv->desc);

    if (rv->option_count > 0) {
        int ow = rv->option_count == 2 ? 74 : 92;
        int gap = 6;
        int total = rv->option_count * ow +
                    (rv->option_count - 1) * gap;
        int ox = rx - total;
        int i;

        for (i = 0; i < rv->option_count; i++) {
            draw_segment(ox, y + 13, ow, 28, rv->options[i],
                         rv->selected == i, rv->enabled);
            if (option_ids)
                hit_add(ox, y + 13, ow, 28, (int)option_ids[i], row);
            ox += ow + gap;
        }
    } else if (rv->toggle >= 0) {
        draw_toggle(rx - 46, y + (h - 24) / 2, 46, 24, rv->toggle,
                    rv->enabled);
        hit_add(x + 8, y + 2, w - 16, h - 4,
                ACT_SYSTEM_TOGGLE + row, row);
    } else if (rv->required) {
        draw_chip(rx - 76, y + (h - 20) / 2, 76, 20, "REQUIRED",
                  ui.c_muted, ui.c_card2);
    } else if (rv->value) {
        text_right(ui.font_body, rx, base, ui.c_fg, rv->value);
    }
    focus_outline(x + 8, y + 2, w - 16, h - 4, row);
}

/* ------------------------------------------------------------- overview -- */

static int enabled_driver_count(void)
{
    const xenus_bios_drivers_t *d = &ui.eng.draft.drivers;

    return d->input + d->display + d->storage + d->audio + d->network +
           d->android_bridge;
}

static int enabled_ucore_count(void)
{
    const xenus_bios_ucore_t *u = &ui.eng.draft.ucore;

    return u->sysinfo + u->process + u->applications + u->hardware +
           u->notifications;
}

static int enabled_android_count(void)
{
    const xenus_bios_android_t *a = &ui.eng.draft.android;

    return a->termux_api + a->termux_x11 + a->android_bridge +
           a->notifications + a->clipboard + a->storage_access;
}

static void stat_card(int x, int y, int w, int h, const char *label,
                      const char *value, const char *detail,
                      unsigned long accent)
{
    round_fill(x, y, w, h, 10, ui.c_card);
    round_outline(x, y, w, h, 10, ui.c_line);
    fill_rect(x + 1, y + 12, 3, h - 24, accent);
    text_at(ui.font_small, x + 16, y + 20, ui.c_muted, label);
    text_clip(ui.font_bold, x + 16, x + w - 12, y + 44, ui.c_fg, value);
    text_clip(ui.font_small, x + 16, x + w - 12, y + 64, ui.c_muted,
              detail);
}

static void draw_overview(void)
{
    int x = content_x();
    int y = content_top();
    int w = content_w();
    int gap = 10;
    int statw = (w - gap * 3) / 4;
    int leftw = (w - 12) * 56 / 100;
    int rightw = w - 12 - leftw;
    int row;
    char buf[160];
    xenus_bios_diff_t diffs[MAX_DIFFS];
    int ndiff = diff_count(diffs, MAX_DIFFS);
    int errors;
    xenus_bios_validation_t v;

    page_header("Overview",
                "Firmware environment, configuration store and staged state.");

    round_fill(x, y, w, 86, 12, ui.c_card);
    round_outline(x, y, w, 86, 12, ui.c_line);
    draw_logo(x + 18, y + 22, 42);
    text_at(ui.font_title, x + 76, y + 38, ui.c_fg, "XENUS BIOS");
    text_at(ui.font_small, x + 77, y + 57, ui.c_muted,
            "X-FIRM is a standalone setup environment. The desktop and UCore "
            "may both be unavailable.");
    errors = collect_validation(&v);
    if (ndiff > 0)
        draw_chip(x + w - 132, y + 22, 114, 22, "STAGED CHANGES",
                  ui.c_yellow, mix(ui.c_yellow, ui.c_bg, 82));
    else
        draw_chip(x + w - 132, y + 22, 114, 22, "CONFIGURATION CLEAN",
                  ui.c_green, mix(ui.c_green, ui.c_bg, 82));
    if (errors)
        snprintf(buf, sizeof(buf), "%d validation issue%s", errors,
                 errors == 1 ? "" : "s");
    else
        copy_str(buf, sizeof(buf), "Draft validates without blocking errors");
    text_at(ui.font_small, x + w - 18 - text_w(ui.font_small, buf), y + 62,
            errors ? ui.c_red : ui.c_muted, buf);
    y += 98;

    snprintf(buf, sizeof(buf), "%s%s%s",
             ui.env.android ? "Android" : "Linux",
             ui.env.termux ? " - Termux" : "",
             ui.env.proot ? " - PRoot" : "");
    stat_card(x, y, statw, 84, "PLATFORM", buf,
              ui.env.arch[0] ? ui.env.arch : "unknown", ui.c_accent);
    stat_card(x + statw + gap, y, statw, 84, "DISPLAY",
              ui.env.x11 ? "X11 online" : "X11 unavailable",
              ui.env.display[0] ? ui.env.display : "no DISPLAY",
              ui.env.x11 ? ui.c_green : ui.c_red);
    snprintf(buf, sizeof(buf), "%lu MB total", ui.env.mem_total_mb);
    stat_card(x + (statw + gap) * 2, y, statw, 84, "MEMORY", buf,
              ui.env.mem_available_mb ? "available snapshot" : "procfs",
              ui.c_accent);
    {
        unsigned long up = ui.env.uptime_sec;
        snprintf(buf, sizeof(buf), "%lud %02luh %02lum", up / 86400,
                 (up / 3600) % 24, (up / 60) % 60);
    }
    stat_card(x + (statw + gap) * 3, y, statw, 84, "UPTIME", buf,
              ui.env.hostname, ui.c_accent);
    y += 96;

    round_fill(x, y, leftw, 218, 12, ui.c_card);
    round_outline(x, y, leftw, 218, 12, ui.c_line);
    text_at(ui.font_bold, x + 18, y + 25, ui.c_fg, "Environment");
    {
        struct {
            char k[40];
            char v[120];
        } rows[6];

        copy_str(rows[0].k, sizeof(rows[0].k), "Operating system");
        copy_str(rows[0].v, sizeof(rows[0].v), ui.env.os);
        copy_str(rows[1].k, sizeof(rows[1].k), "Kernel");
        copy_str(rows[1].v, sizeof(rows[1].v), ui.env.kernel);
        copy_str(rows[2].k, sizeof(rows[2].k), "Architecture");
        copy_str(rows[2].v, sizeof(rows[2].v), ui.env.arch);
        copy_str(rows[3].k, sizeof(rows[3].k), "Processor");
        copy_str(rows[3].v, sizeof(rows[3].v), ui.env.cpu_model[0] ?
                 ui.env.cpu_model : "not reported");
        copy_str(rows[4].k, sizeof(rows[4].k), "CPU cores");
        snprintf(rows[4].v, sizeof(rows[4].v), "%d", ui.env.cpus);
        copy_str(rows[5].k, sizeof(rows[5].k), "Hostname");
        copy_str(rows[5].v, sizeof(rows[5].v), ui.env.hostname);
        for (row = 0; row < 6; row++) {
            int ry = y + 52 + row * 27;

            text_at(ui.font_small, x + 18, ry, ui.c_muted, rows[row].k);
            text_clip(ui.font_small, x + 158, x + leftw - 16, ry,
                      ui.c_fg, rows[row].v);
        }
    }

    round_fill(x + leftw + 12, y, rightw, 218, 12, ui.c_card);
    round_outline(x + leftw + 12, y, rightw, 218, 12, ui.c_line);
    text_at(ui.font_bold, x + leftw + 30, y + 25, ui.c_fg, "Firmware state");
    {
        struct {
            char k[48];
            char v[128];
        } rows[6];

        copy_str(rows[0].k, sizeof(rows[0].k), "Store");
        copy_str(rows[0].v, sizeof(rows[0].v),
                 ui.eng.loaded ? "persistent file loaded" :
                 "defaults staged");
        copy_str(rows[1].k, sizeof(rows[1].k), "Staged changes");
        snprintf(rows[1].v, sizeof(rows[1].v), "%d", ndiff);
        copy_str(rows[2].k, sizeof(rows[2].k), "Internal drivers");
        snprintf(rows[2].v, sizeof(rows[2].v), "%d / 6 enabled",
                 enabled_driver_count());
        copy_str(rows[3].k, sizeof(rows[3].k), "UCore modules");
        snprintf(rows[3].v, sizeof(rows[3].v), "%d / 5 enabled",
                 enabled_ucore_count());
        copy_str(rows[4].k, sizeof(rows[4].k), "Android capabilities");
        snprintf(rows[4].v, sizeof(rows[4].v), "%d / 6 requested",
                 enabled_android_count());
        copy_str(rows[5].k, sizeof(rows[5].k), "Recovery backups");
        snprintf(rows[5].v, sizeof(rows[5].v), "%d",
                 xenus_bios_engine_backup_count(&ui.eng));
        for (row = 0; row < 6; row++) {
            int ry = y + 52 + row * 27;

            text_at(ui.font_small, x + leftw + 30, ry, ui.c_muted,
                    rows[row].k);
            text_right(ui.font_small, x + w - 18, ry, ui.c_fg, rows[row].v);
        }
    }
    y += 230;

    {
        const char *labels[3] = { "Open System", "Drivers", "Save & Exit" };
        int bw = (w - 20) / 3;
        int i;

        for (i = 0; i < 3; i++) {
            int bx = x + i * (bw + 10);

            draw_segment(bx, y, bw, 34, labels[i], i == 0, 1);
            hit_add(bx, y, bw, 34, ACT_QUICK + i, i);
        }
    }
}

/* --------------------------------------------------------------- system -- */

static void draw_system(void)
{
    static const char *const theme_opts[2] = { "Dark", "Light" };
    static const char *const accent_opts[3] = {
        "Harbor Blue", "Silver", "Graphite"
    };
    static const char *const labels[5] = {
        "Theme", "Accent", "Animations", "Menu bar clock",
        "Focus follows mouse"
    };
    static const char *const descs[5] = {
        "Dark graphite surfaces or light paper surfaces",
        "The single XENUS accent used across the shell",
        "Subtle shell motion; safe mode disables it",
        "Date and time at the right end of the menu bar",
        "Pointer focus follows the mouse across windows"
    };
    int x = content_x();
    int y = content_top();
    int w = content_w();
    int i;
    int cy;

    page_header("System",
                "XENUS shell appearance and desktop behavior. Staged first, "
                "then applied to the live configuration file.");
    draw_card(x, y, w, 306, "Appearance & behavior");
    cy = y + 38;
    for (i = 0; i < 5; i++) {
        RowView rv;
        unsigned long ids[3] = { 0, 0, 0 };

        memset(&rv, 0, sizeof(rv));
        rv.label = labels[i];
        rv.desc = descs[i];
        rv.enabled = 1;
        rv.toggle = -1;
        if (i == 0 || i == 1) {
            rv.option_count = i == 0 ? 2 : 3;
            rv.options = i == 0 ? theme_opts : accent_opts;
            rv.selected = i == 0 ? ui.eng.draft.system.theme :
                          ui.eng.draft.system.accent;
            ids[0] = ACT_SYSTEM_OPT + i * 8 + 0;
            ids[1] = ACT_SYSTEM_OPT + i * 8 + 1;
            ids[2] = ACT_SYSTEM_OPT + i * 8 + 2;
        } else {
            rv.toggle = i == 2 ? ui.eng.draft.system.animations :
                        i == 3 ? ui.eng.draft.system.show_clock :
                        ui.eng.draft.system.focus_follows_mouse;
        }
        draw_setting_row(x + 8, cy, w - 16, 52, i, &rv, ids);
        cy += 52;
    }
    y += 318;

    draw_card(x, y, w, 106, "Applied files");
    text_at(ui.font_small, x + 18, y + 50, ui.c_muted, "BIOS store");
    text_clip(ui.font_mono, x + 128, x + w - 18, y + 50, ui.c_fg,
              ui.eng.store_path);
    text_at(ui.font_small, x + 18, y + 71, ui.c_muted, "Shell config");
    text_clip(ui.font_mono, x + 128, x + w - 18, y + 71, ui.c_fg,
              ui.eng.shell_path);
    text_at(ui.font_small, x + 18, y + 92, ui.c_muted,
            "Apply rewrites only theme, accent, animations, show_clock and "
            "focus_follows_mouse; all other lines are preserved.");
}

/* -------------------------------------------------------------- drivers -- */

static const char *driver_label(int i)
{
    static const char *const labels[6] = {
        "Display bridge", "Input bridge", "Configuration store",
        "Audio bridge", "Network bridge", "Android bridge"
    };

    return i >= 0 && i < 6 ? labels[i] : "";
}

static const char *driver_desc(int i)
{
    static const char *const desc[6] = {
        "X11 window, font and framebuffer access required by setup",
        "Pointer, keyboard and touchscreen events required by setup",
        "Persistent configuration and recovery files required by setup",
        "Volume backend: pactl, amixer or termux-volume",
        "Network interface facts from sysfs or the Termux API",
        "Android command bridge at /system/bin/cmd when exposed"
    };

    return i >= 0 && i < 6 ? desc[i] : "";
}

static int *driver_flag(int i)
{
    xenus_bios_drivers_t *d = &ui.eng.draft.drivers;

    switch (i) {
    case 0: return &d->display;
    case 1: return &d->input;
    case 2: return &d->storage;
    case 3: return &d->audio;
    case 4: return &d->network;
    case 5: return &d->android_bridge;
    default: return NULL;
    }
}

static int driver_available(int i)
{
    switch (i) {
    case 0: return ui.env.x11;
    case 1: return 1;
    case 2: return 1;
    case 3: return ui.env.audio_available;
    case 4: return ui.env.network_available;
    case 5: return ui.env.android_cmd_available;
    default: return 0;
    }
}

static const char *driver_detail(int i)
{
    switch (i) {
    case 0: return ui.env.display[0] ? ui.env.display : "no DISPLAY";
    case 1: return "X11 input events";
    case 2: return ui.eng.store_path;
    case 3: return ui.env.audio_backend[0] ? ui.env.audio_backend :
                   "not exposed";
    case 4: return ui.env.network_iface[0] ? ui.env.network_iface :
                   "not exposed";
    case 5: return ui.env.android_cmd_available ? "/system/bin/cmd" :
                   "not exposed";
    default: return "";
    }
}

static void draw_drivers(void)
{
    int x = content_x();
    int y = content_top();
    int w = content_w();
    int i;
    int cy;

    page_header("Drivers",
                "Internal XENUS BIOS bridges. These are userspace interfaces, "
                "not kernel drivers or Android firmware.");
    draw_card(x, y, w, 346, "Firmware interfaces");
    cy = y + 38;
    for (i = 0; i < 6; i++) {
        int required = i < 3;
        int *flag = driver_flag(i);
        int available = driver_available(i);
        int rx = x + w - 18;

        if (i == ui.focus_row)
            round_fill(x + 8, cy + 2, w - 16, 46, 8,
                       mix(ui.c_card2, ui.c_accent, 12));
        text_at(ui.font_body, x + 18, cy + 20, ui.c_fg, driver_label(i));
        text_clip(ui.font_small, x + 18, x + w - 320, cy + 38, ui.c_muted,
                  driver_desc(i));
        if (required) {
            draw_chip(rx - 78, cy + 13, 78, 20, "REQUIRED", ui.c_muted,
                      ui.c_card2);
            hit_add(x + 8, cy + 2, w - 16, 46, ACT_DRIVER_ROW + i, i);
        } else {
            if (available)
                draw_chip(rx - 166, cy + 13, 76, 20, "AVAILABLE",
                          ui.c_green, mix(ui.c_green, ui.c_bg, 82));
            else
                draw_chip(rx - 166, cy + 13, 76, 20, "NOT EXPOSED",
                          ui.c_red, mix(ui.c_red, ui.c_bg, 82));
            draw_toggle(rx - 46, cy + 11, 46, 24, flag ? *flag : 0, 1);
            hit_add(x + 8, cy + 2, w - 16, 46, ACT_DRIVER_ROW + i, i);
        }
        focus_outline(x + 8, cy + 2, w - 16, 46, i);
        cy += 50;
    }
    y += 358;

    draw_card(x, y, w, 90, "Startup semantics");
    text_at(ui.font_small, x + 18, y + 48, ui.c_muted,
            "Optional driver flags are persisted now and evaluated at the "
            "next XENUS BIOS start. BIOS never loads or unloads kernel "
            "modules.");
    text_at(ui.font_small, x + 18, y + 68, ui.c_muted,
            "Required bridges keep the setup environment itself operable. "
            "Unavailable capabilities remain inert.");
    text_clip(ui.font_small, x + 18, x + w - 18, y + 84,
              mix(ui.c_muted, ui.c_bg, 20), driver_detail(
              ui.focus_row >= 0 && ui.focus_row < 6 ? ui.focus_row : 0));
}

/* ---------------------------------------------------------------- ucore -- */

static const char *ucore_label(int i)
{
    static const char *const labels[5] = {
        "sysinfo", "process", "applications", "hardware", "notifications"
    };

    return i >= 0 && i < 5 ? labels[i] : "";
}

static const char *ucore_desc(int i)
{
    static const char *const desc[5] = {
        "Mandatory procfs/sysfs facts used by every UCore consumer",
        "Process snapshots and TERM / STOP / CONT actions",
        "freedesktop .desktop registry and application launching",
        "Real hardware bridge; unavailable devices stay unavailable",
        "Notification ring and Do Not Disturb state"
    };

    return i >= 0 && i < 5 ? desc[i] : "";
}

static int *ucore_flag(int i)
{
    xenus_bios_ucore_t *u = &ui.eng.draft.ucore;

    switch (i) {
    case 0: return &u->sysinfo;
    case 1: return &u->process;
    case 2: return &u->applications;
    case 3: return &u->hardware;
    case 4: return &u->notifications;
    default: return NULL;
    }
}

static void draw_ucore(void)
{
    int x = content_x();
    int y = content_top();
    int w = content_w();
    int i;
    int cy;

    page_header("UCore",
                "Core runtime modules for the XENUS desktop. BIOS itself does "
                "not link or require UCore.");
    draw_card(x, y, w, 84, "Runtime relationship");
    text_at(ui.font_small, x + 18, y + 50, ui.c_muted,
            "BIOS runs standalone. xenus_core_init() reads these flags on the "
            "next desktop start and registers only the enabled modules.");
    draw_chip(x + w - 112, y + 18, 94, 22, "OPTIONAL", ui.c_muted,
              ui.c_card2);
    text_at(ui.font_small, x + w - 112, y + 56, ui.c_muted,
            ui.env.xenus_binary_available ? "xenus binary found" :
            "xenus binary not found");
    y += 96;

    draw_card(x, y, w, 296, "Modules");
    cy = y + 38;
    for (i = 0; i < 5; i++) {
        int *flag = ucore_flag(i);
        int rx = x + w - 18;

        if (i == ui.focus_row)
            round_fill(x + 8, cy + 2, w - 16, 46, 8,
                       mix(ui.c_card2, ui.c_accent, 12));
        text_at(ui.font_body, x + 18, cy + 20, ui.c_fg, ucore_label(i));
        text_clip(ui.font_small, x + 18, x + w - 320, cy + 38, ui.c_muted,
                  ucore_desc(i));
        if (i == 0) {
            draw_chip(rx - 148, cy + 13, 70, 20, "REQUIRED", ui.c_muted,
                      ui.c_card2);
            draw_toggle(rx - 46, cy + 11, 46, 24, 1, 0);
            hit_add(x + 8, cy + 2, w - 16, 46, ACT_UCORE_ROW + i, i);
        } else {
            draw_chip(rx - 148, cy + 13, 76, 20, "NEXT START",
                      ui.c_yellow, mix(ui.c_yellow, ui.c_bg, 82));
            draw_toggle(rx - 46, cy + 11, 46, 24, flag ? *flag : 0, 1);
            hit_add(x + 8, cy + 2, w - 16, 46, ACT_UCORE_ROW + i, i);
        }
        focus_outline(x + 8, cy + 2, w - 16, 46, i);
        cy += 50;
    }
    y += 308;

    draw_card(x, y, w, 68, "Safety");
    text_at(ui.font_small, x + 18, y + 48, ui.c_muted,
            "Disabling a module never fabricates data: consumers simply see "
            "fewer registered services. sysinfo remains mandatory.");
}

/* -------------------------------------------------------------- android -- */

static const char *android_label(int i)
{
    static const char *const labels[6] = {
        "Termux API", "Termux:X11", "Android command bridge",
        "Android notifications", "Clipboard bridge", "Shared storage"
    };

    return i >= 0 && i < 6 ? labels[i] : "";
}

static const char *android_desc(int i)
{
    static const char *const desc[6] = {
        "termux-* helpers already installed and exposed by Termux",
        "X11 display session; xfirm can auto-start termux-x11",
        "/system/bin/cmd when the Android namespace is reachable",
        "termux-notification; never bypasses Android permissions",
        "termux-clipboard-set / get when installed",
        "Termux shared storage (~/storage) when granted by the user"
    };

    return i >= 0 && i < 6 ? desc[i] : "";
}

static int *android_flag(int i)
{
    xenus_bios_android_t *a = &ui.eng.draft.android;

    switch (i) {
    case 0: return &a->termux_api;
    case 1: return &a->termux_x11;
    case 2: return &a->android_bridge;
    case 3: return &a->notifications;
    case 4: return &a->clipboard;
    case 5: return &a->storage_access;
    default: return NULL;
    }
}

static int android_available(int i)
{
    switch (i) {
    case 0: return ui.env.termux_api_available;
    case 1: return ui.env.termux_x11_available || ui.env.x11;
    case 2: return ui.env.android_cmd_available;
    case 3: return ui.env.notifications_available;
    case 4: return ui.env.clipboard_available;
    case 5: return ui.env.storage_available;
    default: return 0;
    }
}

static void draw_android(void)
{
    int x = content_x();
    int y = content_top();
    int w = content_w();
    int i;
    int cy;
    char buf[200];

    page_header("Android Integration",
                "Capabilities that Termux actually exposes. No firmware, "
                "kernel or permission bypass is attempted.");
    draw_card(x, y, w, 90, "Environment");
    snprintf(buf, sizeof(buf), "%s%s%s",
             ui.env.android ? "Android" : "Linux",
             ui.env.termux ? " - Termux" : "",
             ui.env.proot ? " - PRoot" : "");
    text_at(ui.font_small, x + 18, y + 48, ui.c_muted, "Platform");
    text_at(ui.font_mono, x + 120, y + 48, ui.c_fg, buf);
    text_at(ui.font_small, x + 18, y + 68, ui.c_muted, "Display");
    text_clip(ui.font_mono, x + 120, x + w - 18, y + 68, ui.c_fg,
              ui.env.display[0] ? ui.env.display : "unavailable");
    text_at(ui.font_small, x + w / 2, y + 48, ui.c_muted, "Termux");
    text_clip(ui.font_small, x + w / 2 + 60, x + w - 18, y + 48,
              ui.c_fg, ui.env.termux_detail);
    text_at(ui.font_small, x + w / 2, y + 68, ui.c_muted, "Prefix");
    text_clip(ui.font_mono, x + w / 2 + 60, x + w - 18, y + 68,
              ui.c_fg, ui.env.termux_prefix[0] ? ui.env.termux_prefix :
              "not detected");
    y += 102;

    draw_card(x, y, w, 332, "Available Android capabilities");
    cy = y + 38;
    for (i = 0; i < 6; i++) {
        int *flag = android_flag(i);
        int available = android_available(i);
        int rx = x + w - 18;

        if (i == ui.focus_row)
            round_fill(x + 8, cy + 2, w - 16, 44, 8,
                       mix(ui.c_card2, ui.c_accent, 12));
        text_at(ui.font_body, x + 18, cy + 19, ui.c_fg, android_label(i));
        text_clip(ui.font_small, x + 18, x + w - 320, cy + 36, ui.c_muted,
                  android_desc(i));
        if (available)
            draw_chip(rx - 166, cy + 11, 76, 20, "AVAILABLE", ui.c_green,
                      mix(ui.c_green, ui.c_bg, 82));
        else
            draw_chip(rx - 166, cy + 11, 76, 20, "NOT EXPOSED", ui.c_red,
                      mix(ui.c_red, ui.c_bg, 82));
        draw_toggle(rx - 46, cy + 9, 46, 24, flag ? *flag : 0, 1);
        hit_add(x + 8, cy + 2, w - 16, 44, ACT_ANDROID_ROW + i, i);
        focus_outline(x + 8, cy + 2, w - 16, 44, i);
        cy += 48;
    }
    y += 344;

    draw_card(x, y, w, 70, "Boundary");
    text_at(ui.font_small, x + 18, y + 48, ui.c_muted,
            "XENUS BIOS only reads interfaces the Termux user can already "
            "reach. It cannot change Android firmware, kernel drivers, SELinux "
            "or app permissions.");
}

/* ----------------------------------------------------------------- boot -- */

static void draw_boot(void)
{
    static const char *const modes[2] = { "Normal", "Safe" };
    int x = content_x();
    int y = content_top();
    int w = content_w();
    int rx = x + w - 18;
    char buf[160];

    page_header("Boot",
                "Startup behavior consumed by XENUS at the next desktop "
                "start. BIOS remains a separate setup environment.");
    draw_card(x, y, w, 206, "Startup configuration");
    {
        int cy = y + 38;
        int row;

        for (row = 0; row < 3; row++) {
            int *flag = NULL;

            if (row == ui.focus_row)
                round_fill(x + 8, cy + 2, w - 16, 48, 8,
                           mix(ui.c_card2, ui.c_accent, 12));
            if (row == 0) {
                text_at(ui.font_body, x + 18, cy + 21, ui.c_fg,
                        "Startup mode");
                text_at(ui.font_small, x + 18, cy + 39, ui.c_muted,
                        "Safe mode skips plugins and shell animations");
                draw_segment(rx - 148, cy + 12, 70, 28, modes[0],
                             ui.eng.draft.boot.mode == 0, 1);
                draw_segment(rx - 72, cy + 12, 72, 28, modes[1],
                             ui.eng.draft.boot.mode == 1, 1);
                hit_add(rx - 148, cy + 12, 70, 28, ACT_BOOT_MODE + 0, row);
                hit_add(rx - 72, cy + 12, 72, 28, ACT_BOOT_MODE + 1, row);
            } else if (row == 1) {
                text_at(ui.font_body, x + 18, cy + 21, ui.c_fg,
                        "Diagnostic logging");
                text_at(ui.font_small, x + 18, cy + 39, ui.c_muted,
                        "Log BIOS-derived startup facts to the XENUS log");
                flag = &ui.eng.draft.boot.diagnostic;
                draw_toggle(rx - 46, cy + 12, 46, 24, *flag, 1);
                hit_add(x + 8, cy + 2, w - 16, 48, ACT_BOOT_DIAG, row);
            } else {
                char val[32];

                text_at(ui.font_body, x + 18, cy + 21, ui.c_fg,
                        "Boot delay");
                text_at(ui.font_small, x + 18, cy + 39, ui.c_muted,
                        "Seconds to wait before the desktop UI appears (0-10)");
                snprintf(val, sizeof(val), "%d s",
                         ui.eng.draft.boot.delay_seconds);
                draw_segment(rx - 148, cy + 12, 30, 28, "-", 0, 1);
                draw_segment(rx - 112, cy + 12, 54, 28, val, 0, 1);
                draw_segment(rx - 52, cy + 12, 30, 28, "+", 0, 1);
                hit_add(rx - 148, cy + 12, 30, 28, ACT_BOOT_DELAY_MINUS,
                        row);
                hit_add(rx - 52, cy + 12, 30, 28, ACT_BOOT_DELAY_PLUS, row);
            }
            focus_outline(x + 8, cy + 2, w - 16, 48, row);
            cy += 52;
        }
    }
    y += 218;

    draw_card(x, y, w, 112, "Next start summary");
    snprintf(buf, sizeof(buf), "Mode: %s   -   Delay: %d s   -   "
             "Diagnostics: %s",
             xenus_bios_boot_mode_name(ui.eng.draft.boot.mode),
             ui.eng.draft.boot.delay_seconds,
             ui.eng.draft.boot.diagnostic ? "on" : "off");
    text_at(ui.font_body, x + 18, y + 52, ui.c_fg, buf);
    text_at(ui.font_small, x + 18, y + 74, ui.c_muted,
            "Normal mode starts the configured desktop. Safe mode skips "
            "plugins and shell animations.");
    text_at(ui.font_small, x + 18, y + 92, ui.c_muted,
            "BIOS is always reachable with xfirm, even when the desktop fails.");
}

/* ------------------------------------------------------------- recovery -- */

static void draw_recovery(void)
{
    static const char *const labels[5] = {
        "Back up current configuration",
        "Restore latest recovery backup",
        "Verify configuration store",
        "Discard staged changes",
        "Restore factory defaults"
    };
    static const char *const desc[5] = {
        "Create a timestamped copy in the recovery directory",
        "Validate a previous copy, then replace the active store",
        "Parse and validate the file on disk",
        "Drop the in-memory draft without writing anything",
        "Reset BIOS settings and shell appearance to defaults"
    };
    int x = content_x();
    int y = content_top();
    int w = content_w();
    char latest[128];
    char buf[220];
    int backups;
    int i;
    int cy;

    page_header("Recovery",
                "Configuration integrity, timestamped copies and safe "
                "restoration. No partition or firmware operations.");
    backups = xenus_bios_engine_backup_count(&ui.eng);
    if (!xenus_bios_engine_latest_backup(&ui.eng, latest, sizeof(latest)))
        copy_str(latest, sizeof(latest), "none");
    draw_card(x, y, w, 132, "Configuration store");
    text_at(ui.font_small, x + 18, y + 50, ui.c_muted, "Store");
    text_clip(ui.font_mono, x + 108, x + w - 18, y + 50, ui.c_fg,
              ui.eng.store_path);
    text_at(ui.font_small, x + 18, y + 70, ui.c_muted, "State");
    snprintf(buf, sizeof(buf), "%s - %d backup%s",
             ui.eng.loaded ? "loaded and writable" : "defaults staged",
             backups, backups == 1 ? "" : "s");
    text_at(ui.font_small, x + 108, y + 70, ui.c_fg, buf);
    text_at(ui.font_small, x + 18, y + 90, ui.c_muted, "Latest");
    text_clip(ui.font_mono, x + 108, x + w - 18, y + 90, ui.c_fg, latest);
    text_at(ui.font_small, x + 18, y + 112, ui.c_muted,
            ui.eng.last_message[0] ? ui.eng.last_message :
            "No recovery operation has run in this session.");
    y += 144;

    draw_card(x, y, w, 264, "Recovery actions");
    cy = y + 38;
    for (i = 0; i < 5; i++) {
        int destructive = i == 4;
        int rx = x + w - 18;

        if (i == ui.focus_row)
            round_fill(x + 8, cy + 2, w - 16, 40, 8,
                       mix(ui.c_card2, ui.c_accent, 12));
        text_at(ui.font_body, x + 18, cy + 19, ui.c_fg, labels[i]);
        text_clip(ui.font_small, x + 18, x + w - 180, cy + 35, ui.c_muted,
                  desc[i]);
        draw_segment(rx - 74, cy + 7, 74, 26, "RUN",
                     i == ui.focus_row,
                     i != 1 || backups > 0);
        hit_add(x + 8, cy + 2, w - 16, 40, ACT_RECOVERY_ROW + i, i);
        focus_outline(x + 8, cy + 2, w - 16, 40, i);
        if (destructive) {
            set_color(ui.c_red);
            XDrawLine(ui.dpy, canvas, ui.gc, x + 8, cy + 41, x + w - 8,
                      cy + 41);
        }
        cy += 44;
    }
    y += 276;

    draw_card(x, y, w, 72, "Safety");
    text_at(ui.font_small, x + 18, y + 48, ui.c_muted,
            "Restore and factory reset ask for confirmation. Before any "
            "overwrite, the current store is copied to the backup directory.");
    text_at(ui.font_small, x + 18, y + 66, ui.c_muted,
            "Recovery only manages XENUS BIOS configuration and the managed "
            "shell keys.");
}

/* ------------------------------------------------------------ save/exit -- */

static void draw_save(void)
{
    xenus_bios_diff_t diffs[MAX_DIFFS];
    int ndiff = diff_count(diffs, MAX_DIFFS);
    xenus_bios_validation_t v;
    int errors;
    int x = content_x();
    int y = content_top();
    int w = content_w();
    int shown;
    int i;
    int cy;
    char buf[256];

    page_header("Save & Exit",
                "Review the staged configuration. Nothing is written until "
                "Apply or Save & Exit succeeds.");
    errors = collect_validation(&v);
    draw_card(x, y, w, 96, "Review");
    if (ndiff)
        snprintf(buf, sizeof(buf), "%d staged change%s ready for apply",
                 ndiff, ndiff == 1 ? "" : "s");
    else
        copy_str(buf, sizeof(buf), "No staged changes; configuration is clean");
    text_at(ui.font_body, x + 18, y + 50, errors ? ui.c_yellow : ui.c_fg, buf);
    if (errors)
        snprintf(buf, sizeof(buf), "%d blocking validation issue%s: %s",
                 errors, errors == 1 ? "" : "s",
                 v.count ? v.items[0].text : "invalid configuration");
    else
        copy_str(buf, sizeof(buf), "Draft passes validation. Optional driver "
                 "changes become active at the next BIOS start.");
    text_clip(ui.font_small, x + 18, x + w - 18, y + 70,
              errors ? ui.c_red : ui.c_muted, buf);
    text_clip(ui.font_mono, x + 18, x + w - 18, y + 88, ui.c_muted,
              ui.eng.store_path);
    y += 108;

    draw_card(x, y, w, 286, "Pending changes");
    shown = 9;
    if (ndiff == 0) {
        text_at(ui.font_body, x + 18, y + 58, ui.c_muted,
                "The live configuration and the staged draft are identical.");
    } else {
        int max_off = ndiff > shown ? ndiff - shown : 0;

        if (ui.diff_off > max_off)
            ui.diff_off = max_off;
        if (ui.diff_off < 0)
            ui.diff_off = 0;
        cy = y + 40;
        for (i = 0; i < shown; i++) {
            int idx = ui.diff_off + i;

            if (idx >= ndiff)
                break;
            if (i & 1)
                round_fill(x + 8, cy - 15, w - 16, 24, 5, ui.c_card2);
            text_clip(ui.font_small, x + 18, x + w - 330, cy, ui.c_fg,
                      diffs[idx].field);
            text_at(ui.font_mono, x + w - 310, cy, ui.c_muted,
                    diffs[idx].from);
            text_at(ui.font_small, x + w - 196, cy, ui.c_accent, "->");
            text_clip(ui.font_mono, x + w - 172, x + w - 18, cy,
                      ui.c_fg, diffs[idx].to);
            cy += 26;
        }
        if (ndiff > shown)
            text_at(ui.font_small, x + 18, y + 272, mix(ui.c_muted,
                    ui.c_bg, 20), "Mouse wheel scrolls the change list");
    }
    y += 298;

    {
        static const char *const actions[3] = {
            "Apply", "Save & Exit", "Discard & Exit"
        };
        int bw = (w - 20) / 3;
        int i;

        for (i = 0; i < 3; i++) {
            int bx = x + i * (bw + 10);

            draw_segment(bx, y, bw, 38, actions[i], i == 1, 1);
            hit_add(bx, y, bw, 38, ACT_SAVE_ROW + i, i);
        }
    }
}

/* --------------------------------------------------------------- confirm -- */

static void draw_confirm(void)
{
    const char *title = "Confirm";
    const char *body = "";
    int w = 540;
    int h = 218;
    int x = (ui.W - w) / 2;
    int y = (ui.H - h) / 2;
    int line_y;

    switch (ui.confirm) {
    case CONFIRM_EXIT:
        title = "Exit without applying?";
        body = "Staged changes will be lost.\n"
               "The active BIOS configuration is unchanged.";
        break;
    case CONFIRM_DISCARD:
        title = "Discard staged changes?";
        body = "The draft will be replaced by the last applied\n"
               "configuration. Nothing is written to disk.";
        break;
    case CONFIRM_RESTORE:
        title = "Restore latest backup?";
        body = "The active BIOS store and managed shell keys will\n"
               "be replaced. The current store is backed up first.";
        break;
    case CONFIRM_FACTORY:
        title = "Restore factory defaults?";
        body = "All BIOS settings and managed shell keys will be\n"
               "reset. The current store is backed up first.";
        break;
    default:
        break;
    }

    fill_rect(0, 0, ui.W, ui.H, mix(ui.c_bg, rgb(0), 42));
    round_fill(x, y, w, h, 14, ui.c_card);
    round_outline(x, y, w, h, 14, ui.c_line);
    fill_rect(x + 20, y + 20, 4, 32, ui.confirm == CONFIRM_EXIT ||
              ui.confirm == CONFIRM_DISCARD ? ui.c_yellow : ui.c_red);
    text_at(ui.font_bold, x + 38, y + 40, ui.c_fg, title);
    for (line_y = 0; line_y < 2; line_y++) {
        char line[160];
        const char *p = body;
        int n = 0;

        while (*p && n < line_y) {
            if (*p == '\n')
                n++;
            p++;
        }
        if (line_y == 0 || n == line_y) {
            size_t len = 0;

            while (p[len] && p[len] != '\n' && len < sizeof(line) - 1) {
                line[len] = p[len];
                len++;
            }
            line[len] = '\0';
            if (len)
                text_at(ui.font_small, x + 38, y + 78 + line_y * 22,
                        ui.c_muted, line);
        }
    }
    draw_segment(x + w - 212, y + h - 58, 84, 32, "Cancel", 0, 1);
    draw_segment(x + w - 118, y + h - 58, 98, 32, "Confirm", 1, 1);
    hit_add(x + w - 212, y + h - 58, 84, 32, ACT_CONFIRM_NO, -1);
    hit_add(x + w - 118, y + h - 58, 98, 32, ACT_CONFIRM_YES, -1);
}

/* ---------------------------------------------------------------- draw -- */

static void draw_all(void)
{
    if (!ui.dpy || !ui.win || !ui.buffer)
        return;
    ui.nhits = 0;
    fill_rect(0, 0, ui.W, ui.H, ui.c_bg);
    draw_header();
    draw_sidebar();
    switch (ui.page) {
    case PAGE_SYSTEM: draw_system(); break;
    case PAGE_DRIVERS: draw_drivers(); break;
    case PAGE_UCORE: draw_ucore(); break;
    case PAGE_ANDROID: draw_android(); break;
    case PAGE_BOOT: draw_boot(); break;
    case PAGE_RECOVERY: draw_recovery(); break;
    case PAGE_SAVE: draw_save(); break;
    default: draw_overview(); break;
    }
    draw_footer();
    if (ui.confirm) {
        ui.nhits = 0;
        draw_confirm();
    }
    XCopyArea(ui.dpy, ui.buffer, ui.win, ui.gc, 0, 0,
              (unsigned)ui.W, (unsigned)ui.H, 0, 0);
    XFlush(ui.dpy);
    ui.last_draw_ms = now_ms();
}

/* -------------------------------------------------------------- actions -- */

static void go_page(int page)
{
    if (page < 0 || page >= PAGE_COUNT)
        return;
    ui.page = page;
    ui.focus_row = 0;
    ui.hover_id = 0;
    ui.diff_off = 0;
    set_status("Page changed. Edits remain staged until Apply.",
               0);
}

static void probe_env(void)
{
    xenus_bios_env_probe(&ui.env);
    /* Optional bridge switches were written for the next BIOS start.  A
       disabled bridge is not probed or advertised for use, which makes the
       setting real without touching kernel modules. */
    if (!ui.eng.live.drivers.audio) {
        ui.env.audio_available = 0;
        copy_str(ui.env.audio_backend, sizeof(ui.env.audio_backend),
                 "disabled by BIOS");
    }
    if (!ui.eng.live.drivers.network) {
        ui.env.network_available = 0;
        copy_str(ui.env.network_iface, sizeof(ui.env.network_iface),
                 "disabled by BIOS");
    }
    if (!ui.eng.live.drivers.android_bridge) {
        ui.env.android_cmd_available = 0;
        copy_str(ui.env.android_detail, sizeof(ui.env.android_detail),
                 "Android bridge disabled by BIOS");
    }
    ui.last_probe_ms = now_ms();
}

static int apply_changes(int exit_after)
{
    char msg[256];
    int rc = xenus_bios_engine_apply(&ui.eng, 1, msg, sizeof(msg));

    set_status(msg, rc != XENUS_BIOS_OK);
    if (rc == XENUS_BIOS_OK) {
        probe_env();
        if (exit_after)
            ui.quit = 1;
    }
    return rc;
}

static void request_exit(void)
{
    if (xenus_bios_engine_is_dirty(&ui.eng)) {
        ui.confirm = CONFIRM_EXIT;
        set_status("Exit requested with staged changes; confirmation needed.",
                   0);
    } else {
        ui.quit = 1;
    }
}

static void discard_staged(void)
{
    char msg[128];
    int rc = xenus_bios_engine_discard(&ui.eng);

    snprintf(msg, sizeof(msg), "Staged changes discarded.");
    set_status(msg, rc != XENUS_BIOS_OK);
    probe_env();
}

static void run_backup(void)
{
    char msg[220];
    int rc = xenus_bios_engine_backup(&ui.eng, msg, sizeof(msg));

    set_status(msg, rc != XENUS_BIOS_OK);
    probe_env();
}

static void run_verify(void)
{
    char msg[220];
    int rc = xenus_bios_engine_verify(&ui.eng, msg, sizeof(msg));

    set_status(msg, rc != XENUS_BIOS_OK);
}

static void run_restore(void)
{
    char msg[220];
    int rc = xenus_bios_engine_restore_latest(&ui.eng, msg, sizeof(msg));

    set_status(msg, rc != XENUS_BIOS_OK);
    probe_env();
}

static void run_factory(void)
{
    char msg[220];
    int rc = xenus_bios_engine_factory_reset(&ui.eng, msg, sizeof(msg));

    set_status(msg, rc != XENUS_BIOS_OK);
    probe_env();
}

static void confirm_yes(void)
{
    int kind = ui.confirm;

    ui.confirm = CONFIRM_NONE;
    switch (kind) {
    case CONFIRM_EXIT:
        ui.quit = 1;
        break;
    case CONFIRM_DISCARD:
        discard_staged();
        break;
    case CONFIRM_RESTORE:
        run_restore();
        break;
    case CONFIRM_FACTORY:
        run_factory();
        break;
    default:
        break;
    }
}

static void toggle_system(int field)
{
    int *v = NULL;

    switch (field) {
    case 2: v = &ui.eng.draft.system.animations; break;
    case 3: v = &ui.eng.draft.system.show_clock; break;
    case 4: v = &ui.eng.draft.system.focus_follows_mouse; break;
    default: break;
    }
    if (!v)
        return;
    *v = !*v;
    xenus_bios_engine_touch(&ui.eng);
    set_status("System change staged. Apply to write it.", 0);
}

static void cycle_system_enum(int field, int dir)
{
    if (field == 0) {
        int n = 2;
        int v = ui.eng.draft.system.theme + dir;

        if (v < 0)
            v = n - 1;
        if (v >= n)
            v = 0;
        ui.eng.draft.system.theme = v;
    } else if (field == 1) {
        int n = 3;
        int v = ui.eng.draft.system.accent + dir;

        if (v < 0)
            v = n - 1;
        if (v >= n)
            v = 0;
        ui.eng.draft.system.accent = v;
    } else {
        toggle_system(field);
        return;
    }
    xenus_bios_engine_touch(&ui.eng);
    set_status("System change staged. Apply to write it.", 0);
}

static void toggle_driver(int i)
{
    int *flag = driver_flag(i);

    if (i < 3) {
        set_status("This bridge is required by the setup environment.", 1);
        return;
    }
    if (!flag)
        return;
    *flag = !*flag;
    xenus_bios_engine_touch(&ui.eng);
    set_status("Driver change staged; it takes effect at the next BIOS "
               "start.", 0);
}

static void toggle_ucore(int i)
{
    int *flag = ucore_flag(i);

    if (i == 0) {
        set_status("UCore sysinfo is mandatory and cannot be disabled.", 1);
        return;
    }
    if (!flag)
        return;
    *flag = !*flag;
    xenus_bios_engine_touch(&ui.eng);
    set_status("UCore change staged for the next XENUS desktop start.", 0);
}

static void toggle_android(int i)
{
    int *flag = android_flag(i);
    int available = android_available(i);

    if (!flag)
        return;
    if (!available && !*flag) {
        set_status("That capability is not exposed by this Termux/Android "
                   "environment.", 1);
        return;
    }
    *flag = !*flag;
    xenus_bios_engine_touch(&ui.eng);
    if (!available)
        set_status("Capability disabled. It is not exposed here, so it "
                   "cannot be re-enabled until it is available.", 0);
    else
        set_status("Android capability preference staged. No privileged "
                   "operation is performed by BIOS.", 0);
}

static void adjust_boot(int row, int dir)
{
    if (row == 0) {
        int v = ui.eng.draft.boot.mode + dir;

        if (v < 0)
            v = 1;
        if (v > 1)
            v = 0;
        ui.eng.draft.boot.mode = v;
    } else if (row == 1) {
        ui.eng.draft.boot.diagnostic = !ui.eng.draft.boot.diagnostic;
    } else {
        int v = ui.eng.draft.boot.delay_seconds + dir;

        if (v < 0)
            v = 0;
        if (v > 10)
            v = 10;
        ui.eng.draft.boot.delay_seconds = v;
    }
    xenus_bios_engine_touch(&ui.eng);
    set_status("Boot change staged for the next XENUS start.", 0);
}

static void activate_row(int page, int row)
{
    switch (page) {
    case PAGE_OVERVIEW:
        if (row == 0) go_page(PAGE_SYSTEM);
        else if (row == 1) go_page(PAGE_DRIVERS);
        else go_page(PAGE_SAVE);
        break;
    case PAGE_SYSTEM:
        if (row < 2) cycle_system_enum(row, 1);
        else toggle_system(row);
        break;
    case PAGE_DRIVERS:
        toggle_driver(row);
        break;
    case PAGE_UCORE:
        toggle_ucore(row);
        break;
    case PAGE_ANDROID:
        toggle_android(row);
        break;
    case PAGE_BOOT:
        if (row == 0 || row == 1)
            adjust_boot(row, 1);
        else
            adjust_boot(row, 1);
        break;
    case PAGE_RECOVERY:
        if (row == 0) run_backup();
        else if (row == 1) ui.confirm = CONFIRM_RESTORE;
        else if (row == 2) run_verify();
        else if (row == 3) {
            if (xenus_bios_engine_is_dirty(&ui.eng))
                ui.confirm = CONFIRM_DISCARD;
            else
                set_status("There are no staged changes to discard.", 0);
        } else ui.confirm = CONFIRM_FACTORY;
        break;
    case PAGE_SAVE:
        if (row == 0) apply_changes(0);
        else if (row == 1) apply_changes(1);
        else {
            if (xenus_bios_engine_is_dirty(&ui.eng)) {
                ui.confirm = CONFIRM_DISCARD;
                set_status("Discard and exit requested; confirmation needed.",
                           0);
            } else ui.quit = 1;
        }
        break;
    default:
        break;
    }
}

static void handle_action(int id)
{
    if (id >= ACT_NAV && id < ACT_NAV + PAGE_COUNT) {
        go_page(id - ACT_NAV);
        return;
    }
    if (id >= ACT_SYSTEM_OPT && id < ACT_SYSTEM_OPT + 5 * 8) {
        int off = id - ACT_SYSTEM_OPT;
        int field = off / 8;
        int opt = off % 8;

        if (field == 0 && opt < 2) {
            ui.eng.draft.system.theme = opt;
            ui.focus_row = 0;
        } else if (field == 1 && opt < 3) {
            ui.eng.draft.system.accent = opt;
            ui.focus_row = 1;
        } else {
            return;
        }
        xenus_bios_engine_touch(&ui.eng);
        set_status("System change staged. Apply to write it.", 0);
        return;
    }
    if (id >= ACT_SYSTEM_TOGGLE && id < ACT_SYSTEM_TOGGLE + 5) {
        int field = id - ACT_SYSTEM_TOGGLE;

        toggle_system(field);
        ui.focus_row = field;
        return;
    }
    if (id >= ACT_DRIVER_ROW && id < ACT_DRIVER_ROW + 6) {
        ui.focus_row = id - ACT_DRIVER_ROW;
        toggle_driver(ui.focus_row);
        return;
    }
    if (id >= ACT_UCORE_ROW && id < ACT_UCORE_ROW + 5) {
        ui.focus_row = id - ACT_UCORE_ROW;
        toggle_ucore(ui.focus_row);
        return;
    }
    if (id >= ACT_ANDROID_ROW && id < ACT_ANDROID_ROW + 6) {
        ui.focus_row = id - ACT_ANDROID_ROW;
        toggle_android(ui.focus_row);
        return;
    }
    if (id >= ACT_BOOT_MODE && id < ACT_BOOT_MODE + 2) {
        ui.focus_row = 0;
        ui.eng.draft.boot.mode = id - ACT_BOOT_MODE;
        xenus_bios_engine_touch(&ui.eng);
        set_status("Boot mode staged for the next XENUS start.", 0);
        return;
    }
    if (id == ACT_BOOT_DIAG) {
        ui.focus_row = 1;
        ui.eng.draft.boot.diagnostic = !ui.eng.draft.boot.diagnostic;
        xenus_bios_engine_touch(&ui.eng);
        set_status("Diagnostic boot setting staged.", 0);
        return;
    }
    if (id == ACT_BOOT_DELAY_MINUS || id == ACT_BOOT_DELAY_PLUS) {
        ui.focus_row = 2;
        adjust_boot(2, id == ACT_BOOT_DELAY_PLUS ? 1 : -1);
        return;
    }
    if (id >= ACT_RECOVERY_ROW && id < ACT_RECOVERY_ROW + 5) {
        ui.focus_row = id - ACT_RECOVERY_ROW;
        activate_row(PAGE_RECOVERY, ui.focus_row);
        return;
    }
    if (id >= ACT_SAVE_ROW && id < ACT_SAVE_ROW + 3) {
        ui.focus_row = id - ACT_SAVE_ROW;
        activate_row(PAGE_SAVE, ui.focus_row);
        return;
    }
    if (id >= ACT_QUICK && id < ACT_QUICK + 3) {
        ui.focus_row = id - ACT_QUICK;
        activate_row(PAGE_OVERVIEW, ui.focus_row);
        return;
    }
    if (id == ACT_CONFIRM_YES) {
        confirm_yes();
        return;
    }
    if (id == ACT_CONFIRM_NO) {
        ui.confirm = CONFIRM_NONE;
        set_status("Action cancelled.", 0);
        return;
    }
    if (id == ACT_CLOSE) {
        request_exit();
        return;
    }
    if (id == ACT_FOOTER_APPLY) {
        apply_changes(0);
        return;
    }
    if (id == ACT_FOOTER_DISCARD) {
        if (xenus_bios_engine_is_dirty(&ui.eng))
            ui.confirm = CONFIRM_DISCARD;
        else
            set_status("There are no staged changes to discard.", 0);
        return;
    }
    if (id == ACT_FOOTER_SAVE_EXIT) {
        apply_changes(1);
        return;
    }
}

/* --------------------------------------------------------------- events -- */

static void on_button(XButtonEvent *ev)
{
    const Hit *h;

    if (ev->button == Button4 || ev->button == Button5) {
        if (ui.page == PAGE_SAVE && ui.confirm == CONFIRM_NONE) {
            xenus_bios_diff_t diffs[MAX_DIFFS];
            int n = diff_count(diffs, MAX_DIFFS);

            if (ev->button == Button5)
                ui.diff_off += 1;
            else
                ui.diff_off -= 1;
            if (ui.diff_off > n - 9)
                ui.diff_off = n - 9;
            if (ui.diff_off < 0)
                ui.diff_off = 0;
            draw_all();
        }
        return;
    }
    if (ev->button != Button1)
        return;
    h = hit_at((int)ev->x, (int)ev->y);
    if (!h)
        return;
    if (h->row >= 0)
        ui.focus_row = h->row;
    handle_action(h->id);
    draw_all();
}

static void on_motion(XMotionEvent *ev)
{
    const Hit *h = hit_at((int)ev->x, (int)ev->y);
    int id = h ? h->id : 0;

    if (id != ui.hover_id) {
        ui.hover_id = id;
        draw_all();
    }
}

static void on_key(XKeyEvent *ev)
{
    KeySym sym = XLookupKeysym(ev, 0);
    int rows = page_rows(ui.page);

    if (ui.confirm != CONFIRM_NONE) {
        if (sym == XK_Escape || sym == XK_n || sym == XK_N) {
            ui.confirm = CONFIRM_NONE;
            set_status("Action cancelled.", 0);
        } else if (sym == XK_Return || sym == XK_KP_Enter ||
                   sym == XK_y || sym == XK_Y) {
            confirm_yes();
        }
        draw_all();
        return;
    }

    if (sym >= XK_F1 && sym <= XK_F8) {
        go_page((int)(sym - XK_F1));
    } else if (sym == XK_Tab && (ev->state & ShiftMask)) {
        go_page((ui.page + PAGE_COUNT - 1) % PAGE_COUNT);
    } else if (sym == XK_ISO_Left_Tab) {
        go_page((ui.page + PAGE_COUNT - 1) % PAGE_COUNT);
    } else if (sym == XK_Page_Down || sym == XK_Tab) {
        go_page((ui.page + 1) % PAGE_COUNT);
    } else if (sym == XK_Page_Up) {
        go_page((ui.page + PAGE_COUNT - 1) % PAGE_COUNT);
    } else if (sym == XK_Up) {
        ui.focus_row = (ui.focus_row + rows - 1) % rows;
    } else if (sym == XK_Down) {
        ui.focus_row = (ui.focus_row + 1) % rows;
    } else if (sym == XK_Left || sym == XK_Right) {
        int dir = sym == XK_Right ? 1 : -1;

        switch (ui.page) {
        case PAGE_SYSTEM:
            if (ui.focus_row < 2)
                cycle_system_enum(ui.focus_row, dir);
            else
                toggle_system(ui.focus_row);
            break;
        case PAGE_DRIVERS:
            toggle_driver(ui.focus_row);
            break;
        case PAGE_UCORE:
            toggle_ucore(ui.focus_row);
            break;
        case PAGE_ANDROID:
            toggle_android(ui.focus_row);
            break;
        case PAGE_BOOT:
            adjust_boot(ui.focus_row, dir);
            break;
        case PAGE_SAVE:
            ui.diff_off += dir * 3;
            if (ui.diff_off < 0)
                ui.diff_off = 0;
            break;
        default:
            break;
        }
    } else if (sym == XK_Return || sym == XK_KP_Enter ||
               sym == XK_space) {
        activate_row(ui.page, ui.focus_row);
    } else if (sym == XK_Escape) {
        request_exit();
    }
    draw_all();
}

static int handle_event(XEvent *ev)
{
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.window == ui.win && ev->xexpose.count == 0)
            draw_all();
        return 1;
    case ButtonPress:
        if (ev->xbutton.window == ui.win) {
            XSetInputFocus(ui.dpy, ui.win, RevertToParent, CurrentTime);
            on_button(&ev->xbutton);
        }
        return 1;
    case MotionNotify:
        if (ev->xmotion.window == ui.win)
            on_motion(&ev->xmotion);
        return 1;
    case KeyPress:
        if (ev->xkey.window == ui.win)
            on_key(&ev->xkey);
        return 1;
    case ClientMessage:
        if (ev->xclient.window == ui.win &&
            ev->xclient.message_type ==
                XInternAtom(ui.dpy, "WM_PROTOCOLS", False))
            request_exit();
        return 1;
    case FocusIn:
        return 1;
    default:
        return 0;
    }
}

/* --------------------------------------------------------------- setup -- */

static int init_window(void)
{
    Atom wm_delete;
    XClassHint class_hint;
    XSizeHints hints;
    XSetWindowAttributes wa;
    int screen_w;
    int screen_h;
    int x, y;

    ui.dpy = XOpenDisplay(NULL);
    if (!ui.dpy)
        return -1;
    ui.screen = DefaultScreen(ui.dpy);
    screen_w = DisplayWidth(ui.dpy, ui.screen);
    screen_h = DisplayHeight(ui.dpy, ui.screen);
    ui.W = screen_w - 32;
    ui.H = screen_h - 32;
    if (ui.W > 1280)
        ui.W = 1280;
    if (ui.H > 820)
        ui.H = 820;
    if (ui.W < 760)
        ui.W = screen_w - 8;
    if (ui.H < 560)
        ui.H = screen_h - 8;
    x = (screen_w - ui.W) / 2;
    y = (screen_h - ui.H) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;

    ui.cmap = DefaultColormap(ui.dpy, ui.screen);
    load_colors();
    load_fonts();
    if (!ui.font_body) {
        fprintf(stderr, "xenus-bios: no usable X core font found.\n");
        return -1;
    }
    ui.gc = XCreateGC(ui.dpy, DefaultRootWindow(ui.dpy), 0, NULL);
    if (!ui.gc)
        return -1;

    memset(&wa, 0, sizeof(wa));
    wa.override_redirect = True;
    wa.background_pixel = ui.c_bg;
    wa.border_pixel = 0;
    wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                    KeyPressMask | FocusChangeMask;
    ui.win = XCreateWindow(ui.dpy, DefaultRootWindow(ui.dpy), x, y,
                           (unsigned)ui.W, (unsigned)ui.H, 0,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel | CWBorderPixel |
                           CWEventMask, &wa);
    if (!ui.win)
        return -1;
    ui.buffer = XCreatePixmap(ui.dpy, ui.win, (unsigned)ui.W,
                              (unsigned)ui.H,
                              (unsigned)DefaultDepth(ui.dpy, ui.screen));
    if (!ui.buffer)
        return -1;

    XStoreName(ui.dpy, ui.win, "XENUS BIOS (X-FIRM)");
    class_hint.res_name = (char *)"xenus-bios";
    class_hint.res_class = (char *)"XenusBios";
    XSetClassHint(ui.dpy, ui.win, &class_hint);
    memset(&hints, 0, sizeof(hints));
    hints.flags = USPosition | USSize | PMinSize | PMaxSize;
    hints.x = x;
    hints.y = y;
    hints.width = ui.W;
    hints.height = ui.H;
    hints.min_width = hints.max_width = ui.W;
    hints.min_height = hints.max_height = ui.H;
    XSetWMNormalHints(ui.dpy, ui.win, &hints);
    wm_delete = XInternAtom(ui.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(ui.dpy, ui.win, &wm_delete, 1);
    {
        Atom utf8 = XInternAtom(ui.dpy, "_NET_WM_NAME", False);
        XChangeProperty(ui.dpy, ui.win, utf8,
                        XInternAtom(ui.dpy, "UTF8_STRING", False), 8,
                        PropModeReplace,
                        (const unsigned char *)"XENUS BIOS (X-FIRM)", 19);
    }
    XMapRaised(ui.dpy, ui.win);
    XSetInputFocus(ui.dpy, ui.win, RevertToParent, CurrentTime);
    return 0;
}

static void cleanup(void)
{
    if (ui.dpy) {
        if (ui.buffer)
            XFreePixmap(ui.dpy, ui.buffer);
        if (ui.win)
            XDestroyWindow(ui.dpy, ui.win);
        if (ui.gc)
            XFreeGC(ui.dpy, ui.gc);
        if (ui.font_title)
            XFreeFont(ui.dpy, ui.font_title);
        if (ui.font_bold && ui.font_bold != ui.font_title)
            XFreeFont(ui.dpy, ui.font_bold);
        if (ui.font_body && ui.font_body != ui.font_title &&
            ui.font_body != ui.font_bold)
            XFreeFont(ui.dpy, ui.font_body);
        if (ui.font_small && ui.font_small != ui.font_title &&
            ui.font_small != ui.font_bold && ui.font_small != ui.font_body)
            XFreeFont(ui.dpy, ui.font_small);
        if (ui.font_mono && ui.font_mono != ui.font_title &&
            ui.font_mono != ui.font_bold && ui.font_mono != ui.font_body &&
            ui.font_mono != ui.font_small)
            XFreeFont(ui.dpy, ui.font_mono);
        XCloseDisplay(ui.dpy);
    }
    memset(&ui, 0, sizeof(ui));
}

int main(int argc, char **argv)
{
    int rc;
    int fd;

    (void)argc;
    (void)argv;

    memset(&ui, 0, sizeof(ui));
    ui.status_error = 0;
    copy_str(ui.status, sizeof(ui.status),
             "Ready. Staged changes are not written until Apply or Save & Exit.");
    ui.page = PAGE_OVERVIEW;
    ui.focus_row = 0;
    ui.diff_off = 0;

    rc = xenus_bios_engine_open(&ui.eng, NULL);
    if (rc < 0) {
        fprintf(stderr, "xenus-bios: cannot open configuration store: %s\n",
                ui.eng.store_path);
        return 1;
    }
    probe_env();
    if (init_window() != 0) {
        fprintf(stderr,
                "xenus-bios: cannot open the X display. Start Termux:X11 and "
                "run xfirm.\n");
        cleanup();
        return 1;
    }
    canvas = ui.buffer;
    draw_all();
    fd = ConnectionNumber(ui.dpy);

    while (!ui.quit) {
        while (XPending(ui.dpy)) {
            XEvent ev;

            XNextEvent(ui.dpy, &ev);
            handle_event(&ev);
        }
        {
            uint64_t now = now_ms();

            if (now - ui.last_probe_ms > 2500) {
                probe_env();
                draw_all();
            } else if (now - ui.last_draw_ms > 1000) {
                draw_all();
            }
        }
        {
            struct pollfd pfd;

            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, 400) < 0 && errno != EINTR)
                break;
        }
    }
    cleanup();
    return 0;
}
