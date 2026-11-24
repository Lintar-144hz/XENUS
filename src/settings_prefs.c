#include "settings_prefs.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PALETTE_COUNT 19

typedef struct {
    const char *key;
    size_t offset;
} PaletteKey;

static const PaletteKey palette[PALETTE_COUNT] = {
    { "background", offsetof(Config, s_background) },
    { "wallpaper_top", offsetof(Config, s_wallpaper_top) },
    { "wallpaper_bottom", offsetof(Config, s_wallpaper_bottom) },
    { "border_focused", offsetof(Config, s_border_focus) },
    { "border_unfocused", offsetof(Config, s_border_unfocus) },
    { "title_bg", offsetof(Config, s_title_bg) },
    { "title_bg_inactive", offsetof(Config, s_title_bg_inactive) },
    { "title_fg", offsetof(Config, s_title_fg) },
    { "title_fg_inactive", offsetof(Config, s_title_fg_inactive) },
    { "title_hover", offsetof(Config, s_title_hover) },
    { "title_close_hover", offsetof(Config, s_close_hover) },
    { "panel_bg", offsetof(Config, s_panel_bg) },
    { "panel_fg", offsetof(Config, s_panel_fg) },
    { "button_active", offsetof(Config, s_btn_active) },
    { "button_inactive", offsetof(Config, s_btn_inactive) },
    { "button_hover", offsetof(Config, s_btn_hover) },
    { "menu_bg", offsetof(Config, s_menu_bg) },
    { "menu_fg", offsetof(Config, s_menu_fg) },
    { "menu_selected", offsetof(Config, s_menu_sel) },
};

static xenus_config_manager_t prefs;
static int ready;

static char *palette_field(size_t index)
{
    return (char *)&cfg + palette[index].offset;
}

static int define_schema(void)
{
    Config def;
    xenus_cfg_spec_t specs[9 + PALETTE_COUNT];
    size_t n = 0;
    size_t i;
    int rc;

    config_defaults(&def);

    specs[n++] = (xenus_cfg_spec_t){ "theme", XENUS_CFG_BOOL, "0",
                                     0, 0, 0, NULL };
    specs[n++] = (xenus_cfg_spec_t){ "accent", XENUS_CFG_INT, "0",
                                     0, 2, 0, NULL };
    specs[n++] = (xenus_cfg_spec_t){ "wallpaper", XENUS_CFG_STRING, "",
                                     0, 0, XENUS_CFG_VALUE_LEN - 1, NULL };
    specs[n++] = (xenus_cfg_spec_t){ "panel_height", XENUS_CFG_INT, "28",
                                     16, 64, 0, NULL };
    specs[n++] = (xenus_cfg_spec_t){ "show_clock", XENUS_CFG_BOOL, "1",
                                     0, 0, 0, NULL };
    specs[n++] = (xenus_cfg_spec_t){ "show_tray", XENUS_CFG_BOOL, "1",
                                     0, 0, 0, NULL };
    specs[n++] = (xenus_cfg_spec_t){ "animations", XENUS_CFG_BOOL, "1",
                                     0, 0, 0, NULL };
    specs[n++] = (xenus_cfg_spec_t){ "focus_follows_mouse", XENUS_CFG_BOOL,
                                     "1", 0, 0, 0, NULL };
    specs[n++] = (xenus_cfg_spec_t){ "desktop_apps", XENUS_CFG_BOOL, "0",
                                     0, 0, 0, NULL };

    for (i = 0; i < PALETTE_COUNT; i++) {
        specs[n++] = (xenus_cfg_spec_t){
            palette[i].key, XENUS_CFG_STRING,
            (const char *)&def + palette[i].offset,
            0, 0, 31, NULL
        };
    }

    rc = xenus_cfg_define_all(&prefs, specs, n);
    if (rc != XENUS_OK)
        return rc;
    xenus_cfg_set_ignore_unknown(&prefs, 1);
    return XENUS_OK;
}

static int ensure_ready(void)
{
    const char *system_path;
    char sys[512];

    if (ready)
        return 0;

    xenus_cfg_init(&prefs);
    if (define_schema() != XENUS_OK)
        return -1;

    system_path = getenv("XENUS_SYSTEM_CONFIG");
    if (system_path && *system_path)
        snprintf(sys, sizeof(sys), "%s", system_path);
    else
        snprintf(sys, sizeof(sys), "/etc/xenus/config");
    if (xenus_cfg_set_paths(&prefs, sys, cfg.config_path) != XENUS_OK)
        return -1;

    ready = 1;
    return 0;
}

static void apply_cfg(void)
{
    int v;
    int custom_palette = 0;
    int theme_set;
    size_t i;

    if (xenus_cfg_source(&prefs, "theme") != XENUS_CFG_SOURCE_DEFAULT &&
        xenus_cfg_get_bool(&prefs, "theme", &v) == XENUS_OK)
        cfg.theme = v;
    if (xenus_cfg_source(&prefs, "accent") != XENUS_CFG_SOURCE_DEFAULT &&
        xenus_cfg_get_int(&prefs, "accent", &v) == XENUS_OK)
        cfg.accent = v;
    if (xenus_cfg_source(&prefs, "panel_height") != XENUS_CFG_SOURCE_DEFAULT &&
        xenus_cfg_get_int(&prefs, "panel_height", &v) == XENUS_OK)
        cfg.panel_height = v;
    if (xenus_cfg_source(&prefs, "show_clock") != XENUS_CFG_SOURCE_DEFAULT &&
        xenus_cfg_get_bool(&prefs, "show_clock", &v) == XENUS_OK)
        cfg.show_clock = v;
    if (xenus_cfg_source(&prefs, "show_tray") != XENUS_CFG_SOURCE_DEFAULT &&
        xenus_cfg_get_bool(&prefs, "show_tray", &v) == XENUS_OK)
        cfg.show_tray = v;
    if (xenus_cfg_source(&prefs, "animations") != XENUS_CFG_SOURCE_DEFAULT &&
        xenus_cfg_get_bool(&prefs, "animations", &v) == XENUS_OK)
        cfg.animations = v;
    if (xenus_cfg_source(&prefs, "focus_follows_mouse") !=
            XENUS_CFG_SOURCE_DEFAULT &&
        xenus_cfg_get_bool(&prefs, "focus_follows_mouse", &v) == XENUS_OK)
        cfg.focus_follows_mouse = v;
    if (xenus_cfg_source(&prefs, "desktop_apps") != XENUS_CFG_SOURCE_DEFAULT &&
        xenus_cfg_get_bool(&prefs, "desktop_apps", &v) == XENUS_OK)
        cfg.desktop_apps = v;
    if (xenus_cfg_source(&prefs, "wallpaper") != XENUS_CFG_SOURCE_DEFAULT) {
        const char *wp = xenus_cfg_get(&prefs, "wallpaper");

        if (wp)
            str_copy(cfg.wallpaper, sizeof(cfg.wallpaper), wp);
    }

    theme_set = xenus_cfg_source(&prefs, "theme") != XENUS_CFG_SOURCE_DEFAULT;
    for (i = 0; i < PALETTE_COUNT; i++) {
        if (xenus_cfg_source(&prefs, palette[i].key) ==
            XENUS_CFG_SOURCE_DEFAULT)
            continue;
        custom_palette = 1;
        xenus_cfg_get_string(&prefs, palette[i].key,
                             palette_field(i), 32);
    }
    if (theme_set && !custom_palette)
        config_apply_theme(&cfg, cfg.theme);
    else
        config_resolve_colors(&cfg);
}

int settings_prefs_init(void)
{
    if (ensure_ready() != 0)
        return -1;
    if (xenus_cfg_load(&prefs) == XENUS_ERR_IO)
        return -1;
    apply_cfg();
    return 0;
}

void settings_prefs_shutdown(void)
{
    xenus_cfg_init(&prefs);
    ready = 0;
}

int settings_prefs_reload(void)
{
    int rc;

    if (ensure_ready() != 0)
        return -1;
    rc = xenus_cfg_load(&prefs);
    apply_cfg();
    return rc == XENUS_ERR_IO ? -1 : 0;
}

const char *settings_prefs_error(void)
{
    if (prefs.last_error[0])
        return prefs.last_error;
    if (!ready)
        return "settings preferences are not initialised";
    return xenus_cfg_last_error(&prefs);
}

static int reload_for_write(void)
{
    if (ensure_ready() != 0)
        return -1;
    if (xenus_cfg_load(&prefs) == XENUS_ERR_IO)
        return -1;
    return 0;
}

static int save_and_apply(void)
{
    if (xenus_cfg_save_user_merge(&prefs) != XENUS_OK)
        return -1;
    apply_cfg();
    return 0;
}

int settings_prefs_set_theme(int light)
{
    Config next;
    size_t i;

    if (reload_for_write() != 0)
        return -1;
    if (xenus_cfg_set_bool(&prefs, "theme", light ? 1 : 0) != XENUS_OK)
        return -1;

    next = cfg;
    config_apply_theme(&next, light ? 1 : 0);
    for (i = 0; i < PALETTE_COUNT; i++) {
        const char *value = (const char *)&next + palette[i].offset;

        if (xenus_cfg_set_string(&prefs, palette[i].key, value) != XENUS_OK)
            return -1;
    }
    if (xenus_cfg_save_user_merge(&prefs) != XENUS_OK)
        return -1;
    cfg = next;
    return 0;
}

int settings_prefs_set_accent(int accent)
{
    Config next;

    if (reload_for_write() != 0)
        return -1;
    if (xenus_cfg_set_int(&prefs, "accent", accent) != XENUS_OK)
        return -1;

    next = cfg;
    config_apply_accent(&next, accent);
    if (xenus_cfg_set_string(&prefs, "menu_selected", next.s_menu_sel) !=
        XENUS_OK)
        return -1;
    if (xenus_cfg_save_user_merge(&prefs) != XENUS_OK)
        return -1;
    cfg = next;
    return 0;
}

int settings_prefs_set_wallpaper(const char *path)
{
    if (!path)
        path = "";
    if (reload_for_write() != 0)
        return -1;
    if (xenus_cfg_set_string(&prefs, "wallpaper", path) != XENUS_OK)
        return -1;
    return save_and_apply();
}

int settings_prefs_set_panel_height(int px)
{
    if (reload_for_write() != 0)
        return -1;
    if (xenus_cfg_set_int(&prefs, "panel_height", px) != XENUS_OK)
        return -1;
    return save_and_apply();
}

int settings_prefs_set_show_clock(int on)
{
    if (reload_for_write() != 0)
        return -1;
    if (xenus_cfg_set_bool(&prefs, "show_clock", on ? 1 : 0) != XENUS_OK)
        return -1;
    return save_and_apply();
}

int settings_prefs_set_show_tray(int on)
{
    if (reload_for_write() != 0)
        return -1;
    if (xenus_cfg_set_bool(&prefs, "show_tray", on ? 1 : 0) != XENUS_OK)
        return -1;
    return save_and_apply();
}

int settings_prefs_set_animations(int on)
{
    if (reload_for_write() != 0)
        return -1;
    if (xenus_cfg_set_bool(&prefs, "animations", on ? 1 : 0) != XENUS_OK)
        return -1;
    return save_and_apply();
}

int settings_prefs_set_desktop_apps(int on)
{
    if (reload_for_write() != 0)
        return -1;
    if (xenus_cfg_set_bool(&prefs, "desktop_apps", on ? 1 : 0) != XENUS_OK)
        return -1;
    return save_and_apply();
}

int settings_prefs_set_focus_follows_mouse(int on)
{
    if (reload_for_write() != 0)
        return -1;
    if (xenus_cfg_set_bool(&prefs, "focus_follows_mouse", on ? 1 : 0) !=
        XENUS_OK)
        return -1;
    return save_and_apply();
}
