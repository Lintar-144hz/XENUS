/*
 * XENUS Settings preferences - the bridge between the Settings UI, the
 * Phase 2 Config Manager and the live shell Config.
 *
 * The schema covers only the keys the Settings app really controls.  It
 * shares the active user config file with the rest of the shell and writes
 * through the merge saver, so menus, comments and future keys survive.
 */
#ifndef XENUS_SETTINGS_PREFS_H
#define XENUS_SETTINGS_PREFS_H

#include "xenus.h"

#include <xenus/config_manager.h>

int settings_prefs_init(void);
void settings_prefs_shutdown(void);
int settings_prefs_reload(void);
const char *settings_prefs_error(void);

/* Apply and persist a theme, including the matching palette. */
int settings_prefs_set_theme(int light);
int settings_prefs_set_accent(int accent);
/* Empty path clears the image and restores the gradient wallpaper. */
int settings_prefs_set_wallpaper(const char *path);
int settings_prefs_set_panel_height(int px);
int settings_prefs_set_show_clock(int on);
int settings_prefs_set_show_tray(int on);
int settings_prefs_set_animations(int on);
int settings_prefs_set_desktop_apps(int on);
int settings_prefs_set_focus_follows_mouse(int on);

#endif /* XENUS_SETTINGS_PREFS_H */
