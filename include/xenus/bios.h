/*
 * XENUS BIOS (X-FIRM) - public configuration engine.
 *
 * This header is deliberately free of Xlib and of the desktop runtime.  The
 * BIOS setup application, the xfirm launcher and existing XENUS components can
 * all use the same staged configuration store without linking the GUI.
 *
 * The store is separate from the shell configuration:
 *
 *   ~/.config/xenus/bios.conf      persistent BIOS settings
 *   ~/.config/xenus/backups/       timestamped recovery copies
 *   ~/.config/xenus/config         existing XENUS shell configuration
 *
 * BIOS only changes what it can reach safely from Termux/Linux userspace.
 * It never claims to load kernel modules, alter Android firmware or bypass
 * Android permissions.
 */
#ifndef XENUS_BIOS_H
#define XENUS_BIOS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_BIOS_VERSION         "1.0"
#define XENUS_BIOS_CONFIG_VERSION  1
#define XENUS_BIOS_PATH_MAX        512
#define XENUS_BIOS_MAX_ISSUES      16
#define XENUS_BIOS_MAX_DIFF        32
#define XENUS_BIOS_STORE_NAME      "bios.conf"

/* engine / config results */
enum {
    XENUS_BIOS_OK = 0,
    XENUS_BIOS_DEFAULTS = 1,          /* load: no file yet, defaults used */
    XENUS_BIOS_ERR = -1,
    XENUS_BIOS_ERR_ARG = -2,
    XENUS_BIOS_ERR_IO = -3,
    XENUS_BIOS_ERR_INVALID = -4,
    XENUS_BIOS_ERR_NOMEM = -5
};

/* shell appearance (mirrors the existing XENUS config keys) */
enum {
    XENUS_BIOS_THEME_DARK = 0,
    XENUS_BIOS_THEME_LIGHT = 1
};

enum {
    XENUS_BIOS_ACCENT_BLUE = 0,
    XENUS_BIOS_ACCENT_SILVER = 1,
    XENUS_BIOS_ACCENT_GRAPHITE = 2
};

enum {
    XENUS_BIOS_BOOT_NORMAL = 0,
    XENUS_BIOS_BOOT_SAFE = 1
};

typedef struct {
    int theme;                 /* XENUS_BIOS_THEME_* */
    int accent;                /* XENUS_BIOS_ACCENT_* */
    int animations;
    int show_clock;
    int focus_follows_mouse;
} xenus_bios_system_t;

typedef struct {
    /* display, input and storage are required by the setup environment. */
    int input;
    int display;
    int storage;
    int audio;
    int network;
    int android_bridge;
} xenus_bios_drivers_t;

typedef struct {
    int sysinfo;               /* mandatory */
    int process;
    int applications;
    int hardware;
    int notifications;
} xenus_bios_ucore_t;

typedef struct {
    int termux_api;
    int termux_x11;
    int android_bridge;
    int notifications;
    int clipboard;
    int storage_access;
} xenus_bios_android_t;

typedef struct {
    int mode;                  /* XENUS_BIOS_BOOT_* */
    int diagnostic;
    int delay_seconds;         /* 0..10, before the desktop UI appears */
} xenus_bios_boot_t;

typedef struct {
    int backup_before_apply;
} xenus_bios_recovery_t;

typedef struct {
    int version;
    xenus_bios_system_t system;
    xenus_bios_drivers_t drivers;
    xenus_bios_ucore_t ucore;
    xenus_bios_android_t android;
    xenus_bios_boot_t boot;
    xenus_bios_recovery_t recovery;
} xenus_bios_config_t;

typedef struct {
    int error;                 /* 1 = blocking error, 0 = warning */
    char text[160];
} xenus_bios_issue_t;

typedef struct {
    xenus_bios_issue_t items[XENUS_BIOS_MAX_ISSUES];
    int count;
    int errors;
} xenus_bios_validation_t;

typedef struct {
    char field[48];
    char from[64];
    char to[64];
} xenus_bios_diff_t;

typedef struct {
    char store_path[XENUS_BIOS_PATH_MAX];
    char shell_path[XENUS_BIOS_PATH_MAX];
    char backup_dir[XENUS_BIOS_PATH_MAX];
    xenus_bios_config_t live;  /* last committed configuration */
    xenus_bios_config_t draft; /* staged edits */
    int loaded;
    int dirty;
    int apply_count;
    int last_result;           /* XENUS_BIOS_OK ... */
    char last_message[192];
} xenus_bios_engine_t;

/* ------------------------------------------------------------- defaults */
void xenus_bios_config_defaults(xenus_bios_config_t *cfg);
int xenus_bios_config_equal(const xenus_bios_config_t *a,
                            const xenus_bios_config_t *b);
int xenus_bios_config_diff(const xenus_bios_config_t *from,
                           const xenus_bios_config_t *to,
                           xenus_bios_diff_t *out, int max);
int xenus_bios_config_validate(const xenus_bios_config_t *cfg,
                               xenus_bios_validation_t *out);

/* ---------------------------------------------------------------- names */
const char *xenus_bios_theme_name(int value);
const char *xenus_bios_accent_name(int value);
const char *xenus_bios_boot_mode_name(int value);
const char *xenus_bios_bool_name(int value);
int xenus_bios_theme_parse(const char *text);
int xenus_bios_accent_parse(const char *text);
int xenus_bios_boot_mode_parse(const char *text);

/* ------------------------------------------------------- configuration */
/*
 * Resolve the default store path.  Returns buf, or NULL when there is no
 * usable HOME/XDG_CONFIG_HOME.
 */
const char *xenus_bios_store_path(char *buf, size_t size);
const char *xenus_bios_shell_path(char *buf, size_t size);
const char *xenus_bios_backup_dir(char *buf, size_t size);
void xenus_bios_engine_default_paths(xenus_bios_engine_t *e);

/*
 * load: 0 = loaded, XENUS_BIOS_DEFAULTS = no file (cfg is defaults),
 * XENUS_BIOS_ERR_IO = unreadable file.
 */
int xenus_bios_config_load(xenus_bios_config_t *cfg, const char *path);
int xenus_bios_config_save(const xenus_bios_config_t *cfg, const char *path);
int xenus_bios_config_text(const xenus_bios_config_t *cfg, char *out,
                           size_t size);

/* --------------------------------------------------------------- engine */
int xenus_bios_engine_open(xenus_bios_engine_t *e, const char *path);
void xenus_bios_engine_touch(xenus_bios_engine_t *e);
int xenus_bios_engine_is_dirty(const xenus_bios_engine_t *e);
int xenus_bios_engine_apply(xenus_bios_engine_t *e, int sync_shell,
                            char *msg, size_t msg_size);
int xenus_bios_engine_discard(xenus_bios_engine_t *e);
int xenus_bios_engine_backup(xenus_bios_engine_t *e, char *msg,
                             size_t msg_size);
int xenus_bios_engine_restore_latest(xenus_bios_engine_t *e, char *msg,
                                     size_t msg_size);
int xenus_bios_engine_factory_reset(xenus_bios_engine_t *e, char *msg,
                                    size_t msg_size);
int xenus_bios_engine_verify(xenus_bios_engine_t *e, char *msg,
                             size_t msg_size);
int xenus_bios_engine_backup_count(const xenus_bios_engine_t *e);
int xenus_bios_engine_latest_backup(const xenus_bios_engine_t *e,
                                    char *name, size_t name_size);

/*
 * Safe shell-config sync.  Only the keys the BIOS manages are rewritten;
 * every other line, comment and menu entry is preserved.  Existing XENUS
 * reads these keys at its next start (and Control Center can refresh them).
 */
int xenus_bios_apply_shell_settings(const xenus_bios_config_t *cfg,
                                    const char *shell_path, char *msg,
                                    size_t msg_size);

/* ------------------------------------------------- runtime integration */
/*
 * These helpers read the default store on demand and are safe when the file
 * is absent (all defaults are returned).  They are used by the desktop's
 * startup path and by xfirm, never by the BIOS GUI itself.
 */
int xenus_bios_runtime_ucore_enabled(const char *module);
int xenus_bios_runtime_boot_mode(void);
int xenus_bios_runtime_boot_diagnostic(void);
int xenus_bios_runtime_boot_delay(void);
int xenus_bios_runtime_termux_x11(void);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_BIOS_H */
