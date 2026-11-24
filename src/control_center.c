/*
 * XENUS Control Center - the Android-inspired quick settings panel.
 *
 * Every tile reflects a real capability reported by UCore.  A capability
 * that the host does not expose is rendered as unavailable and never
 * pretends to toggle (see include/xenus/ucore.h).
 */
#include "xenus.h"
#include "auth_screen.h"
#include "settings_prefs.h"

#include <xenus/notification.h>
#include <xenus/ucore.h>

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CC_W      392
#define CC_H      552
#define CC_TITLE  34
#define CC_PAD    14
#define CC_COLS   2
#define CC_TILE_H 62
#define CC_GAP    10

enum {
    CC_PAGE_QUICK = 0,
    CC_PAGE_SETTINGS
};

enum {
    CC_ID_WIFI = 100,
    CC_ID_BLUETOOTH,
    CC_ID_AIRPLANE,
    CC_ID_DND,
    CC_ID_THEME,
    CC_ID_SYSTEM,
    CC_ID_VOLUME,
    CC_ID_BRIGHTNESS,
    CC_ID_BATTERY,
    CC_ID_SETTINGS,
    CC_ID_BACK,
    CC_ID_ANIM,
    CC_ID_DARK,
    CC_ID_LIGHT,
    CC_ID_LOCK,
    CC_ID_WS_BASE = 140,
    CC_ID_ACCENT_BASE = 150
};

typedef struct {
    int x, y, w, h;
    int id;
} CcHit;

static Window cc_win;
static int cc_open;
static int cc_x, cc_y;
static int cc_page;
static int cc_hover = -1;
static int cc_drag = -1;
static CcHit cc_hits[48];
static int cc_nhits;
static char cc_status[160] = " ";
static xenus_core_hardware_t cc_hw;
static uint64_t cc_last_probe;
static uint64_t cc_last_tick;

static void cc_draw(void);

static uint64_t cc_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static unsigned long cc_surface(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL, 8);
}

static unsigned long cc_surface_hi(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL, 15);
}

static unsigned long cc_line(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL, 22);
}

static unsigned long cc_muted(void)
{
    return ui_dim(cfg.col_panel_fg, cfg.col_panel_bg);
}

static void cc_text(int x, int y, const char *s, unsigned long c)
{
    draw_text(cc_win, x, y, c, s);
}

static void cc_small(int x, int y, const char *s, unsigned long c)
{
    draw_text_small(cc_win, x, y, c, s);
}

static void cc_bold(int x, int y, const char *s, unsigned long c)
{
    draw_text_bold(cc_win, x, y, c, s);
}

static void cc_hit(int x, int y, int w, int h, int id)
{
    if (cc_nhits >= (int)(sizeof(cc_hits) / sizeof(cc_hits[0])))
        return;
    cc_hits[cc_nhits].x = x;
    cc_hits[cc_nhits].y = y;
    cc_hits[cc_nhits].w = w;
    cc_hits[cc_nhits].h = h;
    cc_hits[cc_nhits].id = id;
    cc_nhits++;
}

static const CcHit *cc_hit_at(int x, int y)
{
    int i;

    for (i = 0; i < cc_nhits; i++)
        if (x >= cc_hits[i].x && x < cc_hits[i].x + cc_hits[i].w &&
            y >= cc_hits[i].y && y < cc_hits[i].y + cc_hits[i].h)
            return &cc_hits[i];
    return NULL;
}

static void cc_refresh_hw(int force)
{
    uint64_t now = cc_now();

    if (!force && cc_last_probe && now - cc_last_probe < 4000)
        return;
    xenus_core_hardware(&cc_hw, force);
    cc_last_probe = now;
}

/* --------------------------------------------------------------- drawing */

static void cc_tile(int x, int y, int w, int h, int id, int on, int enabled,
                    const char *label, const char *sub, void (*icon)(Window,
                    int, int, int, unsigned long))
{
    int hover = cc_hover == id;
    unsigned long bg;
    unsigned long fg;

    if (enabled && on)
        bg = ui_mix(ui_accent(), cfg.col_panel_bg, cfg.theme ? 32 : 24);
    else if (hover && enabled)
        bg = cc_surface_hi();
    else
        bg = cc_surface();
    fg = enabled ? (on ? ui_accent_soft() : cfg.col_panel_fg)
                 : ui_dim(cfg.col_panel_fg, cfg.col_panel_bg);
    if (enabled && on && cfg.theme)
        fg = ui_accent();

    ui_round_fill(cc_win, x, y, w, h, 13, bg);
    ui_round_outline(cc_win, x, y, w, h, 13,
                     enabled && on ? ui_mix(ui_accent(), bg, 40) : cc_line());
    if (icon)
        icon(cc_win, x + 15, y + h / 2, 17, fg);
    draw_text(cc_win, x + 42, y + h / 2 - 2, fg, label);
    if (sub && *sub)
        draw_text_small(cc_win, x + 42, y + h / 2 + 15,
                        enabled ? cc_muted() : fg, sub);
    cc_hit(x, y, w, h, id);
}

static void cc_slider(int x, int y, int w, int id, const char *label,
                      const char *value, int percent, int enabled)
{
    unsigned long fg = enabled ? cfg.col_panel_fg
                               : ui_dim(cfg.col_panel_fg, cfg.col_panel_bg);
    unsigned long track = ui_mix(cfg.col_panel_bg, cfg.col_panel_fg, 14);
    int fill;
    int bar_y = y + 25;

    cc_small(x, y + 11, label, cc_muted());
    draw_text(cc_win, x + w - text_width(value), y + 11,
              enabled ? cc_muted() : fg, value);
    ui_round_fill(cc_win, x, bar_y, w, 10, 5, track);
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    fill = w * percent / 100;
    if (fill > 0)
        ui_round_fill(cc_win, x, bar_y, fill < 10 ? 10 : fill, 10, 5,
                      enabled ? ui_accent() : ui_dim(ui_accent(),
                                                     cfg.col_panel_bg));
    if (enabled) {
        int knob = x + fill;

        if (knob > x + w - 12)
            knob = x + w - 12;
        if (knob < x)
            knob = x;
        ui_round_fill(cc_win, knob, bar_y - 3, 12, 16, 6, 0xf2f2f7UL);
        ui_round_outline(cc_win, knob, bar_y - 3, 12, 16, 6,
                         ui_mix(cfg.col_panel_bg, cfg.col_panel_fg, 30));
    }
    cc_hit(x, y - 2, w, 38, id);
}

static void cc_icon_wifi(Window w, int cx, int cy, int size, unsigned long fg)
{
    (void)size;
    ui_draw_wifi(w, cx - 8, cy, fg);
}

static void cc_title(void)
{
    XSetForeground(sc.dpy, sc.gc, cc_surface());
    XFillRectangle(sc.dpy, cc_win, sc.gc, 0, 0, CC_W, CC_TITLE);
    ui_draw_traffic(cc_win, 12, CC_TITLE / 2, 1, -1, 0, cfg.col_panel_bg);
    cc_bold(76, (CC_TITLE + sc.font_ascent - sc.font_descent) / 2,
            cc_page == CC_PAGE_QUICK ? "Control Center" : "Desktop Settings",
            cfg.col_panel_fg);
    cc_small(CC_W - 90, (CC_TITLE + sc.font_ascent - sc.font_descent) / 2,
             cc_page == CC_PAGE_QUICK ? "XENUS" : "quick", cc_muted());
}

static void cc_quick(void)
{
    int x = CC_PAD;
    int y = CC_TITLE + 10;
    int w = (CC_W - 2 * CC_PAD - CC_GAP) / CC_COLS;
    int i;
    char buf[64];
    const char *wifi_state;
    const char *bt_state;
    const char *air_state;
    int wifi_ok = cc_hw.wifi == XENUS_HW_AVAILABLE &&
                  cc_hw.wifi_controllable;
    int bt_ok = cc_hw.bluetooth == XENUS_HW_AVAILABLE &&
                cc_hw.bluetooth_controllable;
    int air_ok = cc_hw.airplane == XENUS_HW_AVAILABLE &&
                 cc_hw.airplane_controllable;
    int vol_ok = cc_hw.volume == XENUS_HW_AVAILABLE &&
                 cc_hw.volume_controllable;
    int bright_ok = cc_hw.brightness == XENUS_HW_AVAILABLE &&
                    cc_hw.brightness_writable;

    wifi_state = cc_hw.wifi != XENUS_HW_AVAILABLE ? "Not exposed"
                 : cc_hw.wifi_on ? (cc_hw.wifi_ssid[0] ? cc_hw.wifi_ssid
                                                       : "On") : "Off";
    bt_state = cc_hw.bluetooth != XENUS_HW_AVAILABLE ? "Not exposed"
               : cc_hw.bluetooth_on ? "On" : "Off";
    air_state = cc_hw.airplane != XENUS_HW_AVAILABLE ? "Not exposed"
                : cc_hw.airplane_on ? "On" : "Off";

    cc_tile(x, y, w, CC_TILE_H, CC_ID_WIFI, cc_hw.wifi_on, wifi_ok,
            "Wi-Fi", wifi_state, cc_icon_wifi);
    cc_tile(x + w + CC_GAP, y, w, CC_TILE_H, CC_ID_BLUETOOTH,
            cc_hw.bluetooth_on, bt_ok, "Bluetooth", bt_state,
            ui_draw_bluetooth);
    y += CC_TILE_H + CC_GAP;
    cc_tile(x, y, w, CC_TILE_H, CC_ID_AIRPLANE, cc_hw.airplane_on, air_ok,
            "Airplane", air_state, ui_draw_airplane);
    cc_tile(x + w + CC_GAP, y, w, CC_TILE_H, CC_ID_DND,
            xenus_core_dnd_get(), 1, "Do Not Disturb",
            xenus_core_dnd_get() ? "Silenced" : "Off", ui_draw_moon);
    y += CC_TILE_H + CC_GAP;
    cc_tile(x, y, w, CC_TILE_H, CC_ID_THEME, cfg.theme, 1, "Appearance",
            cfg.theme ? "Light" : "Dark", cfg.theme ? ui_draw_sun
                                                    : ui_draw_moon);
    cc_tile(x + w + CC_GAP, y, w, CC_TILE_H, CC_ID_SYSTEM, 0, 1,
            "System Center", "Processes & device", ui_draw_chip);
    y += CC_TILE_H + 14;

    /* volume */
    snprintf(buf, sizeof(buf), "%s%d%%", cc_hw.muted ? "muted · " : "",
             cc_hw.volume_percent);
    cc_slider(x, y, CC_W - 2 * CC_PAD, CC_ID_VOLUME, "Volume", buf,
              cc_hw.volume_percent, vol_ok);
    y += 46;
    snprintf(buf, sizeof(buf), "%d%%", cc_hw.brightness_percent);
    cc_slider(x, y, CC_W - 2 * CC_PAD, CC_ID_BRIGHTNESS, "Brightness", buf,
              cc_hw.brightness_percent, bright_ok);
    y += 50;

    /* battery / device card */
    {
        int h = 56;
        char main[64];
        const char *sub;

        if (cc_hw.battery == XENUS_HW_AVAILABLE) {
            snprintf(main, sizeof(main), "%d%%", cc_hw.battery_percent);
            sub = cc_hw.battery_status[0] ? cc_hw.battery_status
                                          : "battery present";
        } else {
            snprintf(main, sizeof(main), "External power");
            sub = "battery not exposed by kernel";
        }
        ui_round_fill(cc_win, x, y, CC_W - 2 * CC_PAD, h, 12, cc_surface());
        ui_round_outline(cc_win, x, y, CC_W - 2 * CC_PAD, h, 12, cc_line());
        ui_draw_battery(cc_win, x + 14, y + h / 2, cfg.col_panel_fg,
                        cc_surface());
        draw_text(cc_win, x + 46, y + 24, cfg.col_panel_fg, main);
        cc_small(x + 46, y + 41, sub, cc_muted());
        cc_hit(x, y, CC_W - 2 * CC_PAD, h, CC_ID_BATTERY);
        y += h + 12;
    }

    /* workspaces */
    cc_small(x, y + 10, "Workspace", cc_muted());
    {
        int bw = (CC_W - 2 * CC_PAD - 3 * 8) / 4;
        int wi;

        for (i = 0; i < workspace_count() && i < 4; i++) {
            int bx = x + i * (bw + 8);
            int active = i == workspace_current();

            ui_round_fill(cc_win, bx, y + 18, bw, 34, 9,
                          active ? ui_mix(ui_accent(), cfg.col_panel_bg, 40)
                                 : cc_surface());
            ui_round_outline(cc_win, bx, y + 18, bw, 34, 9,
                             active ? ui_accent() : cc_line());
            wi = bx + (bw - text_width(workspace_name(i))) / 2;
            cc_text(wi, y + 40, workspace_name(i),
                    active ? (cfg.theme ? ui_accent() : ui_accent_soft())
                           : cfg.col_panel_fg);
            cc_hit(bx, y + 18, bw, 34, CC_ID_WS_BASE + i);
        }
        y += 64;
    }

    cc_hit(x, y, CC_W - 2 * CC_PAD, 30, CC_ID_SETTINGS);
    if (cc_hover == CC_ID_SETTINGS)
        ui_round_fill(cc_win, x, y, CC_W - 2 * CC_PAD, 30, 8, cc_surface_hi());
    cc_text(x + 2, y + 20, "Desktop Settings", cfg.col_panel_fg);
    ui_draw_chevron(cc_win, CC_W - CC_PAD - 8, y + 15, 1, cc_muted(), 10);
    y += 34;

    cc_small(x, y + 12, cc_status, cc_status[0] == ' ' ? cc_muted()
                                                       : ui_yellow());
}

static void cc_settings(void)
{
    int x = CC_PAD;
    int y = CC_TITLE + 16;
    int w = CC_W - 2 * CC_PAD;
    int i;

    cc_small(x, y + 10, "APPEARANCE", cc_muted());
    y += 22;
    ui_round_fill(cc_win, x, y, w, 62, 12, cc_surface());
    ui_round_outline(cc_win, x, y, w, 62, 12, cc_line());
    cc_text(x + 14, y + 24, "Theme", cfg.col_panel_fg);
    cc_small(x + 14, y + 41, cfg.theme ? "Light surfaces" : "Dark charcoal",
             cc_muted());
    {
        int s1 = w - 150;
        int s2 = w - 78;

        ui_round_fill(cc_win, x + s1, y + 17, 64, 28, 8,
                      !cfg.theme ? ui_accent() : cc_surface_hi());
        ui_round_fill(cc_win, x + s2, y + 17, 64, 28, 8,
                      cfg.theme ? ui_accent() : cc_surface_hi());
        cc_text(x + s1 + 14, y + 36, "Dark",
                !cfg.theme ? 0xffffffUL : cfg.col_panel_fg);
        cc_text(x + s2 + 11, y + 36, "Light",
                cfg.theme ? 0xffffffUL : cfg.col_panel_fg);
        cc_hit(x + s1, y + 17, 64, 28, CC_ID_DARK);
        cc_hit(x + s2, y + 17, 64, 28, CC_ID_LIGHT);
    }
    y += 74;

    cc_small(x, y + 10, "ACCENT", cc_muted());
    y += 22;
    ui_round_fill(cc_win, x, y, w, 56, 12, cc_surface());
    ui_round_outline(cc_win, x, y, w, 56, 12, cc_line());
    for (i = 0; i < 3; i++) {
        int sx = x + 18 + i * 52;
        unsigned long swatch = i == 0 ? 0x0a84ffUL
                              : i == 1 ? 0xc7c9d1UL : 0x8e8e93UL;

        ui_round_fill(cc_win, sx, y + 15, 34, 26, 7, swatch);
        if (cfg.accent == i)
            ui_round_outline(cc_win, sx - 3, y + 12, 40, 32, 9,
                             ui_accent_soft());
        cc_hit(sx, y + 15, 34, 26, CC_ID_ACCENT_BASE + i);
    }
    cc_text(x + 200, y + 35,
            cfg.accent == 1 ? "Silver" : cfg.accent == 2 ? "Graphite"
                                                         : "Harbor blue",
            cfg.col_panel_fg);
    y += 68;

    cc_small(x, y + 10, "MOTION", cc_muted());
    y += 22;
    ui_round_fill(cc_win, x, y, w, 56, 12, cc_surface());
    ui_round_outline(cc_win, x, y, w, 56, 12, cc_line());
    cc_text(x + 14, y + 33, "Subtle shell animations", cfg.col_panel_fg);
    ui_draw_toggle(cc_win, x + w - 62, y + 16, 46, 24, cfg.animations,
                   cc_hover == CC_ID_ANIM);
    cc_hit(x + 14, y + 12, w - 28, 34, CC_ID_ANIM);
    y += 68;

    cc_small(x, y + 10, "WORKSPACES", cc_muted());
    y += 22;
    {
        int bw = (w - 3 * 8) / 4;

        for (i = 0; i < workspace_count() && i < 4; i++) {
            int bx = x + i * (bw + 8);
            int active = i == workspace_current();

            ui_round_fill(cc_win, bx, y, bw, 44, 10,
                          active ? ui_mix(ui_accent(), cfg.col_panel_bg, 40)
                                 : cc_surface());
            ui_round_outline(cc_win, bx, y, bw, 44, 10,
                             active ? ui_accent() : cc_line());
            cc_text(bx + (bw - text_width(workspace_name(i))) / 2, y + 27,
                    workspace_name(i),
                    active ? ui_accent_soft() : cfg.col_panel_fg);
            cc_hit(bx, y, bw, 44, CC_ID_WS_BASE + i);
        }
    }
    y += 58;

    /* Session lock */
    ui_round_fill(cc_win, x, y, w, 40, 10, cc_surface());
    ui_round_outline(cc_win, x, y, w, 40, 10, cc_line());
    cc_text(x + 14, y + 25, "Lock Screen", cfg.col_panel_fg);
    cc_small(x + w - 112, y + 25,
             xenus_auth_enabled() ? "Password enabled" : "Set password first",
             cc_muted());
    cc_hit(x, y, w, 40, CC_ID_LOCK);
    y += 50;

    cc_hit(x, y, 130, 32, CC_ID_BACK);
    if (cc_hover == CC_ID_BACK)
        ui_round_fill(cc_win, x, y, 130, 32, 8, cc_surface_hi());
    ui_draw_chevron(cc_win, x + 14, y + 16, 0, cc_muted(), 10);
    cc_text(x + 28, y + 21, "Back to controls", cfg.col_panel_fg);

    cc_small(x, y + 60, "Settings are saved to the XENUS config file.",
             cc_muted());
}

static void cc_draw(void)
{
    uint64_t started = cc_now();
    if (!cc_win)
        return;
    cc_nhits = 0;
    XSetForeground(sc.dpy, sc.gc, cfg.col_panel_bg);
    XFillRectangle(sc.dpy, cc_win, sc.gc, 0, 0, CC_W, CC_H);
    cc_title();
    if (cc_page == CC_PAGE_SETTINGS)
        cc_settings();
    else
        cc_quick();
    XFlush(sc.dpy);
    log_msg("cc profile draw=%llu ms", (unsigned long long)(cc_now() - started));
}

/* --------------------------------------------------------------- actions */

static void cc_save_int(const char *key, int value)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "%d", value);
    config_set_value(key, buf);
}

static void cc_apply_theme(int light)
{
    if (settings_prefs_set_theme(light) != 0) {
        /* Manager unavailable: keep the legacy single-key behaviour so the
           Control Center still works on an unusual config path. */
        config_apply_theme(&cfg, light);
        ui_set_accent(cfg.accent);
        cc_save_int("theme", cfg.theme);
    }
    snprintf(cc_status, sizeof(cc_status), "%s theme applied",
             light ? "Light" : "Dark");
    shell_refresh_theme();
}

static void cc_apply_accent(int accent)
{
    if (settings_prefs_set_accent(accent) != 0) {
        config_apply_accent(&cfg, accent);
        ui_set_accent(cfg.accent);
        cc_save_int("accent", cfg.accent);
    }
    snprintf(cc_status, sizeof(cc_status), "Accent updated");
    shell_refresh_theme();
}

static void cc_action(int id)
{
    xenus_core_status_t rc = XENUS_CORE_ERR_UNAVAILABLE;

    if (id >= CC_ID_WS_BASE && id < CC_ID_WS_BASE + 4) {
        workspace_switch(id - CC_ID_WS_BASE);
        snprintf(cc_status, sizeof(cc_status), "Switched to %s",
                 workspace_name(id - CC_ID_WS_BASE));
        return;
    }
    if (id >= CC_ID_ACCENT_BASE && id < CC_ID_ACCENT_BASE + 3) {
        cc_apply_accent(id - CC_ID_ACCENT_BASE);
        return;
    }

    switch (id) {
    case CC_ID_WIFI:
        if (cc_hw.wifi != XENUS_HW_AVAILABLE) {
            snprintf(cc_status, sizeof(cc_status),
                     "Wi-Fi is not exposed by this host");
            break;
        }
        rc = xenus_core_wifi_set(!cc_hw.wifi_on);
        cc_refresh_hw(1);
        snprintf(cc_status, sizeof(cc_status), "Wi-Fi: %s",
                 rc == XENUS_CORE_OK ? (cc_hw.wifi_on ? "on" : "off")
                                    : xenus_core_status_str(rc));
        break;
    case CC_ID_BLUETOOTH:
        if (cc_hw.bluetooth != XENUS_HW_AVAILABLE) {
            snprintf(cc_status, sizeof(cc_status),
                     "Bluetooth is not exposed by this host");
            break;
        }
        rc = xenus_core_bluetooth_set(!cc_hw.bluetooth_on);
        cc_refresh_hw(1);
        snprintf(cc_status, sizeof(cc_status), "Bluetooth: %s",
                 rc == XENUS_CORE_OK ? (cc_hw.bluetooth_on ? "on" : "off")
                                    : xenus_core_status_str(rc));
        break;
    case CC_ID_AIRPLANE:
        if (cc_hw.airplane != XENUS_HW_AVAILABLE) {
            snprintf(cc_status, sizeof(cc_status),
                     "Airplane mode is not exposed by this host");
            break;
        }
        rc = xenus_core_airplane_set(!cc_hw.airplane_on);
        cc_refresh_hw(1);
        snprintf(cc_status, sizeof(cc_status), "Airplane mode: %s",
                 rc == XENUS_CORE_OK ? (cc_hw.airplane_on ? "on" : "off")
                                    : xenus_core_status_str(rc));
        break;
    case CC_ID_DND:
        xenus_core_dnd_set(!xenus_core_dnd_get());
        snprintf(cc_status, sizeof(cc_status), "Do Not Disturb %s",
                 xenus_core_dnd_get() ? "enabled" : "disabled");
        break;
    case CC_ID_THEME:
        cc_apply_theme(!cfg.theme);
        break;
    case CC_ID_DARK:
        cc_apply_theme(0);
        break;
    case CC_ID_LIGHT:
        cc_apply_theme(1);
        break;
    case CC_ID_SYSTEM:
        control_center_hide();
        system_center_show();
        return;
    case CC_ID_SETTINGS:
        cc_page = CC_PAGE_SETTINGS;
        cc_status[0] = ' ';
        break;
    case CC_ID_BACK:
        cc_page = CC_PAGE_QUICK;
        break;
    case CC_ID_LOCK:
        if (!xenus_auth_enabled()) {
            snprintf(cc_status, sizeof(cc_status),
                     "Set a password in Security settings first");
            break;
        }
        control_center_hide();
        if (auth_screen_open() != 0) {
            snprintf(cc_status, sizeof(cc_status),
                     "Unable to open lock screen");
            control_center_show();
            break;
        }
        if (xenus_auth_lock() != 0) {
            auth_screen_close();
            snprintf(cc_status, sizeof(cc_status),
                     "Unable to lock session");
            control_center_show();
        }
        return;
    case CC_ID_ANIM:
        if (settings_prefs_set_animations(!cfg.animations) != 0) {
            cfg.animations = !cfg.animations;
            cc_save_int("animations", cfg.animations);
        }
        dock_animations_changed();
        snprintf(cc_status, sizeof(cc_status), "Animations %s",
                 cfg.animations ? "on" : "off");
        break;
    case CC_ID_BATTERY:
        if (cc_hw.battery == XENUS_HW_AVAILABLE)
            snprintf(cc_status, sizeof(cc_status), "Battery %d%% · %s",
                     cc_hw.battery_percent, cc_hw.battery_status);
        else
            snprintf(cc_status, sizeof(cc_status),
                     "No battery interface on this host");
        break;
    case CC_ID_VOLUME:
    case CC_ID_BRIGHTNESS:
        break;
    default:
        break;
    }
    cc_draw();
}

static void cc_set_volume_from_x(int x)
{
    int pct;

    if (cc_hw.volume != XENUS_HW_AVAILABLE || !cc_hw.volume_controllable)
        return;
    pct = (x - CC_PAD) * 100 / (CC_W - 2 * CC_PAD);
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    if (xenus_core_volume_set(pct) == XENUS_CORE_OK) {
        cc_hw.volume_percent = pct;
        cc_hw.muted = 0;
        snprintf(cc_status, sizeof(cc_status), "Volume %d%%", pct);
    } else {
        snprintf(cc_status, sizeof(cc_status), "Volume is not controllable");
    }
}

static void cc_set_brightness_from_x(int x)
{
    int pct;

    if (cc_hw.brightness != XENUS_HW_AVAILABLE || !cc_hw.brightness_writable)
        return;
    pct = (x - CC_PAD) * 100 / (CC_W - 2 * CC_PAD);
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    if (xenus_core_brightness_set(pct) == XENUS_CORE_OK) {
        cc_hw.brightness_percent = pct;
        snprintf(cc_status, sizeof(cc_status), "Brightness %d%%", pct);
    } else {
        snprintf(cc_status, sizeof(cc_status),
                 "Brightness is not writable");
    }
}

/* --------------------------------------------------------------- public */

void control_center_show(void)
{
    uint64_t started = cc_now();
    if (!cc_win) {
        XSetWindowAttributes wa;

        wa.override_redirect = True;
        wa.background_pixel = cfg.col_panel_bg;
        wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                        ButtonReleaseMask | KeyPressMask;
        cc_x = sc.sw - CC_W - 14;
        if (cc_x < 8)
            cc_x = 8;
        cc_y = reserve_top() + 6;
        if (cc_y + CC_H > sc.sh - reserve_bottom() - 6)
            cc_y = sc.sh - reserve_bottom() - CC_H - 6;
        if (cc_y < reserve_top())
            cc_y = reserve_top();
        cc_win = XCreateWindow(sc.dpy, sc.root, cc_x, cc_y, CC_W, CC_H, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask,
                               &wa);
        XStoreName(sc.dpy, cc_win, "XENUS Control Center");
        ui_shape_round(cc_win, CC_W, CC_H, 16);
        log_msg("cc profile create=%llu ms", (unsigned long long)(cc_now() - started));
    }
    cc_open = 1;
    cc_page = CC_PAGE_QUICK;
    cc_hover = -1;
    cc_drag = -1;
    cc_status[0] = ' ';
    cc_last_tick = 0;
    XMapRaised(sc.dpy, cc_win);
    focus_track(cc_win);
    log_msg("cc show mapped win=0x%lx", (unsigned long)cc_win);
    XFlush(sc.dpy);
    log_msg("cc profile map=%llu ms", (unsigned long long)(cc_now() - started));
    cc_draw();
    log_msg("cc profile show-total=%llu ms", (unsigned long long)(cc_now() - started));
}

void control_center_show_settings(void)
{
    control_center_show();
    cc_page = CC_PAGE_SETTINGS;
    cc_status[0] = ' ';
    cc_draw();
}

void control_center_hide(void)
{
    if (!cc_open)
        return;
    cc_open = 0;
    cc_drag = -1;
    log_msg("cc hide");
    focus_drop(cc_win);
    if (sc.focused && !sc.focused->minimized)
        focus_track(sc.focused->win);
    XUnmapWindow(sc.dpy, cc_win);
    XFlush(sc.dpy);
}

void control_center_toggle(void)
{
    log_msg("cc toggle open=%d win=0x%lx", cc_open, (unsigned long)cc_win);
    if (cc_open)
        control_center_hide();
    else
        control_center_show();
}

int control_center_open(void)
{
    return cc_open;
}

Window control_center_window(void)
{
    return cc_win;
}

void control_center_tick(void)
{
    uint64_t now;

    if (!cc_open)
        return;
    now = cc_now();
    if (cc_last_tick && now - cc_last_tick < 1000)
        return;
    cc_last_tick = now;
    cc_refresh_hw(0);
    cc_draw();
}

void control_center_cleanup(void)
{
    if (cc_win != None) {
        XDestroyWindow(sc.dpy, cc_win);
        cc_win = None;
    }
    cc_open = 0;
}

int control_center_expose(XExposeEvent *ev)
{
    if (!cc_win || ev->window != cc_win)
        return 0;
    if (ev->count == 0)
        cc_draw();
    return 1;
}

int control_center_motion(XMotionEvent *ev)
{
    int found = -1;
    const CcHit *h;

    if (!cc_open || ev->window != cc_win)
        return 0;

    if (cc_drag == CC_ID_VOLUME && (ev->state & Button1Mask)) {
        cc_set_volume_from_x((int)ev->x);
        cc_draw();
        return 1;
    }
    if (cc_drag == CC_ID_BRIGHTNESS && (ev->state & Button1Mask)) {
        cc_set_brightness_from_x((int)ev->x);
        cc_draw();
        return 1;
    }

    h = cc_hit_at((int)ev->x, (int)ev->y);
    if (h)
        found = h->id;
    if (found != cc_hover) {
        cc_hover = found;
        cc_draw();
    }
    return 1;
}

int control_center_button(XButtonEvent *ev)
{
    const CcHit *h;
    int x, y;

    if (!cc_open || ev->window != cc_win)
        return 0;
    focus_track(cc_win);

    if (ev->button == Button4 || ev->button == Button5) {
        cc_refresh_hw(1);
        cc_draw();
        return 1;
    }
    if (ev->button != Button1)
        return 1;

    x = (int)ev->x;
    y = (int)ev->y;

    if (y < CC_TITLE) {
        int cell = 21;
        int left = 6;

        if (x >= left && x < left + 3 * cell) {
            int light = (x - left) / cell;

            if (light == UI_LIGHT_CLOSE)
                control_center_hide();
            else if (light == UI_LIGHT_MIN)
                control_center_hide();
        } else {
            selfdrag_window(cc_win, ev, &cc_x, &cc_y, CC_W, CC_H);
        }
        return 1;
    }

    h = cc_hit_at(x, y);
    if (!h)
        return 1;
    if (h->id == CC_ID_VOLUME || h->id == CC_ID_BRIGHTNESS) {
        cc_drag = h->id;
        if (h->id == CC_ID_VOLUME)
            cc_set_volume_from_x(x);
        else
            cc_set_brightness_from_x(x);
        cc_draw();
        return 1;
    }
    cc_action(h->id);
    return 1;
}

void control_center_key(XKeyEvent *ev)
{
    char tmp[8];
    KeySym sym = NoSymbol;

    if (!cc_open)
        return;
    XLookupString(ev, tmp, sizeof(tmp) - 1, &sym, NULL);
    if (sym == XK_Escape) {
        if (cc_page == CC_PAGE_SETTINGS)
            cc_page = CC_PAGE_QUICK;
        else
            control_center_hide();
    } else if (sym == XK_Left || sym == XK_Right) {
        int i = workspace_current() + (sym == XK_Right ? 1 : -1);

        workspace_switch(i);
    } else if (sym == XK_Tab) {
        cc_page = cc_page == CC_PAGE_QUICK ? CC_PAGE_SETTINGS
                                           : CC_PAGE_QUICK;
    }
    cc_draw();
}
