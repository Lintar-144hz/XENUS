/*
 * XENUS Settings - the standalone desktop configuration window.
 *
 * Phase 1 supplied the window lifecycle and ten-category navigation.
 * Phase 3 makes Personalization and Desktop real: dark/light themes, local
 * wallpaper selection with a live desktop preview, menu bar/taskbar
 * preferences and dock motion.  Every control changes the running shell and
 * persists through the Phase 2 Config Manager into the shared user config.
 *
 * The window follows the same self-drawn, override-redirect pattern as the
 * System Center, Control Center and Notification Center: lazy creation,
 * reuse, map/unmap, focus tracking and explicit cleanup.
 */
#include "xenus.h"
#include "settings_prefs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ST_W        820
#define ST_H        600
#define ST_TITLE    36
#define ST_SIDE     210
#define ST_PAD      22
#define ST_ROW_H    34
#define ST_ROW_GAP  4
#define ST_MAX_HITS 40
#define ST_WALL_MAX 32
#define ST_WALL_ROWS 4

enum {
    ST_ID_CATEGORY = 100,
    ST_ID_THEME_DARK = 200,
    ST_ID_THEME_LIGHT,
    ST_ID_WALL_PREV,
    ST_ID_WALL_NEXT,
    ST_ID_WALL_ITEM_BASE = 210,
    ST_ID_WALL_PREVIEW = 220,
    ST_ID_WALL_APPLY,
    ST_ID_WALL_CLEAR,
    ST_ID_PANEL_MINUS = 230,
    ST_ID_PANEL_PLUS,
    ST_ID_CLOCK,
    ST_ID_TRAY,
    ST_ID_ANIM,
    ST_ID_DESKTOP_APPS
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
static char st_status[200];

static char st_wall_path[ST_WALL_MAX][512];
static char st_wall_name[ST_WALL_MAX][128];
static int st_wall_count;
static int st_wall_sel;
static int st_wall_page;

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
    "Theme and wallpaper.",
    "Resolution and scaling controls will live here.",
    "Menu bar, taskbar and shell motion.",
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

static void st_small_clip(int x, int max_x, int y, unsigned long c,
                          const char *s)
{
    draw_text_clip_font(st_win, sc.font_small, x, max_x, y, c, s);
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

static void st_set_status(const char *msg)
{
    snprintf(st_status, sizeof(st_status), "%s", msg ? msg : "");
}

static void st_error_status(const char *prefix)
{
    const char *err = settings_prefs_error();

    if (err && *err)
        snprintf(st_status, sizeof(st_status), "%s: %.140s", prefix, err);
    else
        snprintf(st_status, sizeof(st_status), "%s", prefix);
    log_msg("settings: %s", st_status);
}

/* ------------------------------------------------------------ wallpaper -- */

static const char *wall_base(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash && slash[1] ? slash + 1 : path;
}

static int wall_present(const char *path)
{
    int i;

    for (i = 0; i < st_wall_count; i++)
        if (strcmp(st_wall_path[i], path) == 0)
            return i;
    return -1;
}

static void wall_add(const char *path)
{
    struct stat st;

    if (!path || !*path || st_wall_count >= ST_WALL_MAX)
        return;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return;
    if (apps_file_kind(path) != F_KIND_IMAGE)
        return;
    if (wall_present(path) >= 0)
        return;
    snprintf(st_wall_path[st_wall_count], sizeof(st_wall_path[0]), "%s", path);
    snprintf(st_wall_name[st_wall_count], sizeof(st_wall_name[0]), "%s",
             wall_base(path));
    st_wall_count++;
}

static void wall_scan_dir(const char *dir)
{
    DIR *d;
    struct dirent *e;

    if (!dir || !*dir)
        return;
    d = opendir(dir);
    if (!d)
        return;
    while ((e = readdir(d)) != NULL && st_wall_count < ST_WALL_MAX) {
        char path[640];

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        wall_add(path);
    }
    closedir(d);
}

typedef struct {
    char path[512];
    char name[128];
} WallEntry;

static int wall_cmp(const void *a, const void *b)
{
    const WallEntry *ea = a;
    const WallEntry *eb = b;

    return strcmp(ea->name, eb->name);
}

static void wall_sort(void)
{
    WallEntry entries[ST_WALL_MAX];
    int i;

    for (i = 0; i < st_wall_count; i++) {
        memcpy(entries[i].path, st_wall_path[i], sizeof(entries[i].path));
        memcpy(entries[i].name, st_wall_name[i], sizeof(entries[i].name));
    }
    qsort(entries, (size_t)st_wall_count, sizeof(entries[0]), wall_cmp);
    for (i = 0; i < st_wall_count; i++) {
        memcpy(st_wall_path[i], entries[i].path, sizeof(st_wall_path[i]));
        memcpy(st_wall_name[i], entries[i].name, sizeof(st_wall_name[i]));
    }
}

static void wall_scan(void)
{
    char dir[640];
    const char *home = getenv("HOME");
    const char *data = getenv("XDG_DATA_HOME");
    int i;

    st_wall_count = 0;
    st_wall_sel = 0;
    st_wall_page = 0;
    if (!home || !*home)
        home = "";

    snprintf(dir, sizeof(dir), "%s/Pictures", home);
    wall_scan_dir(dir);
    snprintf(dir, sizeof(dir), "%s/Desktop", home);
    wall_scan_dir(dir);
    if (data && *data)
        snprintf(dir, sizeof(dir), "%s/xenus/backgrounds", data);
    else
        snprintf(dir, sizeof(dir), "%s/.local/share/xenus/backgrounds", home);
    wall_scan_dir(dir);
    wall_scan_dir("/usr/share/backgrounds");
    wall_scan_dir("/usr/share/wallpapers");

    wall_sort();

    /* Keep the active image selectable even when it lives elsewhere. */
    if (cfg.wallpaper[0] && wall_present(cfg.wallpaper) < 0)
        wall_add(cfg.wallpaper);

    if (st_wall_count == 0) {
        st_wall_sel = -1;
        return;
    }
    st_wall_sel = 0;
    for (i = 0; i < st_wall_count; i++) {
        if (cfg.wallpaper[0] && strcmp(st_wall_path[i], cfg.wallpaper) == 0) {
            st_wall_sel = i;
            break;
        }
    }
    st_wall_page = st_wall_sel / ST_WALL_ROWS;
}

static const char *wall_selected(void)
{
    if (st_wall_sel < 0 || st_wall_sel >= st_wall_count)
        return NULL;
    return st_wall_path[st_wall_sel];
}

/* --------------------------------------------------------------- drawing -- */

static void st_title(void)
{
    int baseline = (ST_TITLE + sc.font_ascent - sc.font_descent) / 2;

    XSetForeground(sc.dpy, sc.gc,
                   ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL :
                          0xffffffUL, 5));
    XFillRectangle(sc.dpy, st_win, sc.gc, 0, 0, ST_W, ST_TITLE);
    ui_draw_traffic(st_win, 12, ST_TITLE / 2, 1, -1, 0, cfg.col_panel_bg);
    st_bold(80, baseline, "XENUS Settings", cfg.col_panel_fg);
    st_small(ST_W - 116, baseline, "Phase 3", st_muted());
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

    st_small(x + 4, ST_H - 46, "Phase 3", st_muted());
    st_small(x + 4, ST_H - 30, "Desktop preferences", st_muted());
}

static void st_heading(int x, int y, int w)
{
    char num[40];

    snprintf(num, sizeof(num), "CATEGORY %02d / %02d", st_page + 1,
             ST_PAGE_COUNT);
    st_small(x, y + 10, num, ui_accent());
    st_bold(x, y + 38, st_page_names[st_page], cfg.col_panel_fg);
    if (st_page == ST_PAGE_PERSONALIZATION)
        st_small(x, y + 58, "Desktop preferences · live and persistent",
                 st_muted());
    else if (st_page == ST_PAGE_DESKTOP)
        st_small(x, y + 58, "Menu bar, taskbar and shell motion", st_muted());
    else
        st_small(x, y + 58, "XENUS Settings · Phase 3", st_muted());
    XSetForeground(sc.dpy, sc.gc, st_line());
    XDrawLine(sc.dpy, st_win, sc.gc, x, y + 74, x + w, y + 74);
}

static void st_button(int x, int y, int w, int h, const char *label, int id,
                      int selected, int enabled)
{
    unsigned long bg = selected ? ui_accent()
        : ui_mix(st_surface(), cfg.col_panel_fg, cfg.theme ? 10 : 6);
    unsigned long fg = selected ? 0xffffffUL
        : enabled ? cfg.col_panel_fg : st_muted();
    int baseline = y + (h + sc.font_small_ascent - sc.font_small_descent) / 2;
    int tx = x + (w - text_width_small(label)) / 2;

    ui_round_fill(st_win, x, y, w, h, 8, bg);
    ui_round_outline(st_win, x, y, w, h, 8,
                     selected ? ui_mix(ui_accent(), bg, 48) : st_line());
    st_small(tx, baseline, label, fg);
    if (enabled)
        st_hit(x, y, w, h, id);
}

static void st_toggle_row(int x, int y, int w, int id, const char *title,
                          const char *detail, int on)
{
    int hover = st_hover == id;
    int baseline = y + 20;

    if (hover)
        ui_round_fill(st_win, x - 8, y - 2, w + 16, 42, 8, st_surface_hi());
    st_text(x, baseline, title, cfg.col_panel_fg);
    st_small(x, y + 35, detail, st_muted());
    ui_draw_toggle(st_win, x + w - 54, y + 7, 46, 24, on, hover);
    st_hit(x - 8, y - 2, w + 16, 42, id);
}

static void st_theme_card(int x, int y, int w)
{
    int bw = 84;
    int bh = 32;

    st_card(x, y, w, 82);
    st_small(x + 20, y + 22, "THEME", st_muted());
    st_small(x + 20, y + 44, "Dark and light palettes across the shell.",
             st_muted());
    st_button(x + w - 20 - 2 * bw - 8, y + 25, bw, bh, "Dark",
              ST_ID_THEME_DARK, !cfg.theme, 1);
    st_button(x + w - 20 - bw, y + 25, bw, bh, "Light",
              ST_ID_THEME_LIGHT, cfg.theme, 1);
}

static void st_wallpaper_card(int x, int y, int w)
{
    int sel = st_wall_sel;
    int i;
    int page_count;
    int first;
    int rows;
    int by = y + 220;
    int bh = 32;
    const char *current = cfg.wallpaper[0] ? cfg.wallpaper
                                           : "Gradient (no image selected)";
    int feh = apps_have("feh");
    int can_apply = sel >= 0 && sel < st_wall_count;

    st_card(x, y, w, 278);
    st_small(x + 20, y + 22, "WALLPAPER", st_muted());
    st_small_clip(x + 20, x + w - 20, y + 42, st_muted(), current);

    page_count = (st_wall_count + ST_WALL_ROWS - 1) / ST_WALL_ROWS;
    if (st_wall_page >= page_count)
        st_wall_page = page_count > 0 ? page_count - 1 : 0;
    first = st_wall_page * ST_WALL_ROWS;
    rows = st_wall_count - first;
    if (rows > ST_WALL_ROWS)
        rows = ST_WALL_ROWS;

    if (st_wall_count == 0) {
        st_text(x + 24, y + 76, "No local images found.", cfg.col_panel_fg);
        st_small(x + 24, y + 94,
                 "Add images to ~/Pictures or ~/Desktop, then reopen Settings.",
                 st_muted());
    } else {
        for (i = 0; i < rows; i++) {
            int idx = first + i;
            int ry = y + 58 + i * 30;
            int active = idx == st_wall_sel;
            int hover = st_hover == ST_ID_WALL_ITEM_BASE + i;

            ui_round_fill(st_win, x + 18, ry, w - 36, 26, 7,
                          active ? ui_mix(ui_accent(), st_surface(),
                                          cfg.theme ? 30 : 40)
                                 : hover ? st_surface_hi() : st_surface());
            if (active || hover)
                ui_round_outline(st_win, x + 18, ry, w - 36, 26, 7,
                                 active ? ui_accent() : st_line());
            ui_round_fill(st_win, x + 28, ry + 10, 6, 6, 3,
                          active ? ui_accent()
                                 : ui_mix(st_surface(), cfg.col_panel_fg, 26));
            st_clip(x + 42, x + w - 28,
                    ry + (26 + sc.font_ascent - sc.font_descent) / 2,
                    active ? cfg.col_panel_fg : st_muted(),
                    st_wall_name[idx]);
            st_hit(x + 18, ry, w - 36, 26, ST_ID_WALL_ITEM_BASE + i);
        }
    }

    if (page_count > 1) {
        char count[32];
        int pcy = y + 196;

        snprintf(count, sizeof(count), "%d / %d", st_wall_page + 1,
                 page_count);
        st_button(x + 20, pcy - 16, 64, 24, "< Prev", ST_ID_WALL_PREV, 0,
                  st_wall_page > 0);
        st_button(x + 92, pcy - 16, 64, 24, "Next >", ST_ID_WALL_NEXT, 0,
                  st_wall_page + 1 < page_count);
        st_small(x + 172, pcy, count, st_muted());
    }

    if (feh && can_apply)
        st_button(x + 20, by, 84, bh, "Preview", ST_ID_WALL_PREVIEW, 0, 1);
    else
        st_button(x + 20, by, 84, bh, "Preview", ST_ID_WALL_PREVIEW, 0, 0);
    st_button(x + 112, by, 84, bh, "Apply", ST_ID_WALL_APPLY, 0,
              can_apply && feh);
    st_button(x + 204, by, 132, bh, "Clear gradient", ST_ID_WALL_CLEAR, 0,
              cfg.wallpaper[0] != '\0');
    if (!feh)
        st_small_clip(x + 348, x + w - 16, by + 20, ui_yellow(),
                      "Install feh to preview and apply images.");
    else if (!can_apply)
        st_small(x + 348, by + 20, "Select an image first.", st_muted());
}

static void st_personalization(int x, int y, int w)
{
    st_theme_card(x, y, w);
    st_wallpaper_card(x, y + 96, w);
}

static void st_desktop(int x, int y, int w)
{
    int h = menubar_h();
    char value[32];

    st_card(x, y, w, 100);
    st_small(x + 20, y + 22, "PANEL", st_muted());
    st_text(x + 20, y + 54, "Menu bar height", cfg.col_panel_fg);
    snprintf(value, sizeof(value), "%d px", h);
    st_bold(x + w - 226, y + 54, value, ui_accent());
    st_small(x + 20, y + 78, "Between 20 and 40 pixels; applied immediately.",
             st_muted());
    st_button(x + w - 20 - 36 - 8 - 36, y + 34, 36, 32, "-",
              ST_ID_PANEL_MINUS, 0, h > 20);
    st_button(x + w - 20 - 36, y + 34, 36, 32, "+", ST_ID_PANEL_PLUS, 0,
              h < 40);

    y += 114;
    st_card(x, y, w, 216);
    st_small(x + 20, y + 22, "TASKBAR & MOTION", st_muted());
    st_toggle_row(x + 20, y + 34, w - 48, ST_ID_CLOCK, "Clock",
                  "Show the current time in the menu bar.", cfg.show_clock);
    st_toggle_row(x + 20, y + 76, w - 48, ST_ID_TRAY, "System tray",
                  "Show quick settings, notifications and security.",
                  cfg.show_tray);
    st_toggle_row(x + 20, y + 118, w - 48, ST_ID_ANIM, "Desktop animations",
                  "Animate dock hover motion when the shell is idle.",
                  cfg.animations);
    st_toggle_row(x + 20, y + 160, w - 48, ST_ID_DESKTOP_APPS,
                  "Desktop application icons",
                  "Show installed applications as desktop shortcuts.",
                  cfg.desktop_apps);
}

static void st_placeholder(int x, int y, int w)
{
    int card_h = 176;
    int tile = 48;
    const char *badge = "LATER PHASE";
    int bw = text_width_small(badge) + 20;
    int bx = x + w - bw - 18;

    st_card(x, y, w, card_h);
    ui_round_fill(st_win, x + 20, y + 24, tile, tile, 12,
                  ui_mix(ui_accent(), st_surface(),
                         cfg.theme ? 20 : 28));
    ui_round_outline(st_win, x + 20, y + 24, tile, tile, 12,
                     ui_mix(ui_accent(), st_surface(), 45));
    st_icon(st_page, x + 20 + tile / 2, y + 24 + tile / 2, 22, ui_accent());

    st_bold(x + 86, y + 46, st_page_names[st_page], cfg.col_panel_fg);
    st_clip(x + 86, x + w - 24, y + 70, st_muted(),
            st_page_blurbs[st_page]);

    ui_round_fill(st_win, bx, y + 18, bw, 20, 10,
                  ui_mix(ui_accent(), st_surface(), cfg.theme ? 16 : 22));
    st_small(bx + 10, y + 32, badge, ui_accent());

    st_small(x + 20, y + card_h - 22,
             "This page becomes available in a later phase.", st_muted());

    y += card_h + 16;
    st_card(x, y, w, 150);
    st_small(x + 20, y + 24, "ALREADY LIVE", st_muted());
    st_small(x + 20, y + 52, "Personalization: theme and wallpaper",
             cfg.col_panel_fg);
    st_small(x + 20, y + 78, "Desktop: menu bar, taskbar and motion",
             cfg.col_panel_fg);
    st_small(x + 20, y + 104, "Persistence: Phase 2 Config Manager",
             cfg.col_panel_fg);
}

static void st_content(void)
{
    int x = ST_SIDE + ST_PAD;
    int y = ST_TITLE + 22;
    int w = ST_W - x - ST_PAD;

    st_heading(x, y, w);
    y += 94;
    if (st_page == ST_PAGE_PERSONALIZATION)
        st_personalization(x, y, w);
    else if (st_page == ST_PAGE_DESKTOP)
        st_desktop(x, y, w);
    else
        st_placeholder(x, y, w);

    if (st_status[0])
        st_small_clip(x, x + w, ST_H - 58,
                      ui_yellow(), st_status);
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
    st_status[0] = '\0';
    st_draw();
}

static void st_relayout(void)
{
    int top = reserve_top();
    int bottom = sc.sh - reserve_bottom();

    if (!st_win)
        return;
    if (st_y < top)
        st_y = top + 24;
    if (st_y + ST_H > bottom - 8)
        st_y = bottom - ST_H - 8;
    if (st_y < top)
        st_y = top;
    if (st_x + ST_W > sc.sw)
        st_x = sc.sw - ST_W;
    if (st_x < 0)
        st_x = 0;
    XMoveWindow(sc.dpy, st_win, st_x, st_y);
    XRaiseWindow(sc.dpy, st_win);
    focus_track(st_win);
}

static void st_apply_theme(int light)
{
    if (settings_prefs_set_theme(light) != 0) {
        st_error_status("Theme not saved");
        return;
    }
    st_set_status(light ? "Light theme applied and saved."
                        : "Dark theme applied and saved.");
    shell_refresh_theme();
}

static void st_apply_panel_height(int delta)
{
    int target = menubar_h() + delta;

    if (target < 20)
        target = 20;
    if (target > 40)
        target = 40;
    if (target == menubar_h()) {
        st_set_status("Menu bar is already at the limit.");
        st_draw();
        return;
    }
    if (settings_prefs_set_panel_height(target) != 0) {
        st_error_status("Panel preference not saved");
        st_draw();
        return;
    }
    panel_reconfigure();
    desktop_refresh();
    st_relayout();
    snprintf(st_status, sizeof(st_status), "Menu bar height set to %d px.",
             target);
    st_draw();
}

static void st_apply_wallpaper(const char *path)
{
    if (settings_prefs_set_wallpaper(path) != 0) {
        st_error_status("Wallpaper not saved");
        return;
    }
    if (path && *path)
        st_set_status("Wallpaper applied and saved.");
    else
        st_set_status("Gradient wallpaper restored.");
    shell_refresh_theme();
}

static void st_preview_wallpaper(void)
{
    const char *path = wall_selected();

    if (!path)
        return;
    if (!apps_have("feh")) {
        st_set_status("Install feh to preview wallpaper images.");
        st_draw();
        return;
    }
    wallpaper_apply_path(path, 0);
    snprintf(st_status, sizeof(st_status), "Previewing %s (not saved).",
             wall_base(path));
    st_draw();
}

static void st_action(int id)
{
    switch (id) {
    case ST_ID_THEME_DARK:
        st_apply_theme(0);
        break;
    case ST_ID_THEME_LIGHT:
        st_apply_theme(1);
        break;
    case ST_ID_WALL_PREV:
        if (st_wall_page > 0) {
            st_wall_page--;
            st_wall_sel = st_wall_page * ST_WALL_ROWS;
            st_draw();
        }
        break;
    case ST_ID_WALL_NEXT:
        if ((st_wall_page + 1) * ST_WALL_ROWS < st_wall_count) {
            st_wall_page++;
            st_wall_sel = st_wall_page * ST_WALL_ROWS;
            st_draw();
        }
        break;
    case ST_ID_WALL_PREVIEW:
        st_preview_wallpaper();
        break;
    case ST_ID_WALL_APPLY: {
        const char *path = wall_selected();

        if (path)
            st_apply_wallpaper(path);
        break;
    }
    case ST_ID_WALL_CLEAR:
        st_apply_wallpaper("");
        break;
    case ST_ID_PANEL_MINUS:
        st_apply_panel_height(-4);
        break;
    case ST_ID_PANEL_PLUS:
        st_apply_panel_height(4);
        break;
    case ST_ID_CLOCK:
        if (settings_prefs_set_show_clock(!cfg.show_clock) != 0)
            st_error_status("Clock preference not saved");
        else {
            panel_update();
            st_set_status(cfg.show_clock ? "Clock shown." : "Clock hidden.");
            st_draw();
        }
        break;
    case ST_ID_TRAY:
        if (settings_prefs_set_show_tray(!cfg.show_tray) != 0)
            st_error_status("Tray preference not saved");
        else {
            panel_update();
            st_set_status(cfg.show_tray ? "System tray shown."
                                        : "System tray hidden.");
            st_draw();
        }
        break;
    case ST_ID_ANIM:
        if (settings_prefs_set_animations(!cfg.animations) != 0)
            st_error_status("Animation preference not saved");
        else {
            dock_animations_changed();
            st_set_status(cfg.animations
                              ? "Desktop animations enabled."
                              : "Desktop animations disabled.");
            st_draw();
        }
        break;
    case ST_ID_DESKTOP_APPS:
        if (settings_prefs_set_desktop_apps(!cfg.desktop_apps) != 0)
            st_error_status("Desktop icon preference not saved");
        else {
            desktop_refresh();
            st_set_status(cfg.desktop_apps
                              ? "Desktop application icons shown."
                              : "Desktop application icons hidden.");
            st_draw();
        }
        break;
    default:
        if (id >= ST_ID_WALL_ITEM_BASE && id < ST_ID_WALL_ITEM_BASE +
            ST_WALL_ROWS) {
            int idx = st_wall_page * ST_WALL_ROWS +
                      (id - ST_ID_WALL_ITEM_BASE);

            if (idx >= 0 && idx < st_wall_count) {
                st_wall_sel = idx;
                st_draw();
            }
        }
        break;
    }
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
    settings_prefs_reload();
    wall_scan();
    st_status[0] = '\0';
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
    if (!h)
        return 1;
    if (h->id >= ST_ID_CATEGORY &&
        h->id < ST_ID_CATEGORY + ST_PAGE_COUNT) {
        st_select_page(h->id - ST_ID_CATEGORY);
        return 1;
    }
    st_action(h->id);
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
