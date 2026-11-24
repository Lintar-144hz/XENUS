/*
 * XENUS Config Manager - typed, validated, layered configuration.
 *
 * The desktop shell and the Settings app can use this module for owned
 * settings.  It deliberately does not replace the read-only App API
 * (<xenus/config.h>) or the legacy Config struct: it is a new, central
 * schema-driven store with fixed-size storage and no heap allocation.
 *
 * Layers, lowest to highest precedence:
 *   built-in default -> system file -> user file
 *
 * Secrets must not be stored in ordinary configuration files.  Keys that
 * look like credentials are rejected by xenus_cfg_define(), the loaders and
 * the save path.
 */
#ifndef XENUS_CONFIG_MANAGER_H
#define XENUS_CONFIG_MANAGER_H

#include <xenus/common.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_CFG_MAX_KEYS 128
#define XENUS_CFG_KEY_LEN 64
#define XENUS_CFG_VALUE_LEN 192
#define XENUS_CFG_PATH_LEN 512
#define XENUS_CFG_ERROR_LEN 192

typedef enum {
    XENUS_CFG_STRING = 0,
    XENUS_CFG_INT,
    XENUS_CFG_BOOL
} xenus_cfg_type_t;

typedef enum {
    XENUS_CFG_SOURCE_NONE = -1,
    XENUS_CFG_SOURCE_DEFAULT = 0,
    XENUS_CFG_SOURCE_SYSTEM,
    XENUS_CFG_SOURCE_USER
} xenus_cfg_source_t;

/* A schema entry.  Pointers are borrowed only during define(); the manager
 * copies the key and default, while choices must stay alive (use static
 * string arrays). */
typedef struct {
    const char *key;
    xenus_cfg_type_t type;
    const char *default_value;
    int min_int;
    int max_int;
    size_t max_len;                 /* 0 = XENUS_CFG_VALUE_LEN - 1 */
    const char *const *choices;     /* NULL-terminated, strings only */
} xenus_cfg_spec_t;

typedef struct {
    char key[XENUS_CFG_KEY_LEN];
    xenus_cfg_type_t type;
    char default_value[XENUS_CFG_VALUE_LEN];
    char system_value[XENUS_CFG_VALUE_LEN];
    char user_value[XENUS_CFG_VALUE_LEN];
    int has_system;
    int has_user;
    int min_int;
    int max_int;
    size_t max_len;
    const char *const *choices;
} xenus_cfg_entry_t;

typedef struct {
    xenus_cfg_entry_t entries[XENUS_CFG_MAX_KEYS];
    size_t count;
    char system_path[XENUS_CFG_PATH_LEN];
    char user_path[XENUS_CFG_PATH_LEN];
    char last_error[XENUS_CFG_ERROR_LEN];
    int ignore_unknown;
} xenus_config_manager_t;

/* Reset a manager to an empty schema and no paths. */
void xenus_cfg_init(xenus_config_manager_t *m);

/* Skip unknown keys while loading instead of reporting them as errors.
 * Useful when a schema covers only one subsystem of a shared config file. */
void xenus_cfg_set_ignore_unknown(xenus_config_manager_t *m, int ignore);

/* Register one schema entry or a complete array of entries.  define_all()
 * rolls back entries added by the failed call.  Duplicate keys and
 * secret-like keys are rejected. */
int xenus_cfg_define(xenus_config_manager_t *m, const xenus_cfg_spec_t *spec);
int xenus_cfg_define_all(xenus_config_manager_t *m,
                         const xenus_cfg_spec_t *specs, size_t count);

/* Set both layer paths.  NULL clears a layer. */
int xenus_cfg_set_paths(xenus_config_manager_t *m, const char *system_path,
                        const char *user_path);
/* XENUS_SYSTEM_CONFIG (or /etc/xenus/config) and XENUS_CONFIG
 * (or ~/.config/xenus/config). */
int xenus_cfg_default_paths(xenus_config_manager_t *m);

/* Load system then user.  Missing files are normal (defaults apply).
 * Invalid lines/values are skipped, last_error() describes the first one,
 * and the call returns XENUS_ERR; I/O failures return XENUS_ERR_IO. */
int xenus_cfg_load(xenus_config_manager_t *m);

/* Atomically write only the user-layer overrides (0600, temp file +
 * fsync + rename).  System values and defaults are never copied. */
int xenus_cfg_save_user(xenus_config_manager_t *m);

/* Atomically update only the keys this manager owns, preserving every other
 * line (comments, menu entries, future/unknown keys).  A key present in the
 * file but no longer overridden is removed.  This is the variant to use when
 * the user file is shared with other subsystems. */
int xenus_cfg_save_user_merge(xenus_config_manager_t *m);

const char *xenus_cfg_last_error(const xenus_config_manager_t *m);
const char *xenus_cfg_type_name(xenus_cfg_type_t type);
int xenus_cfg_key_is_secret(const char *key);

/* Effective value / source (user > system > default). */
const char *xenus_cfg_get(const xenus_config_manager_t *m, const char *key);
xenus_cfg_source_t xenus_cfg_source(const xenus_config_manager_t *m,
                                    const char *key);
int xenus_cfg_get_string(const xenus_config_manager_t *m, const char *key,
                         char *out, size_t out_size);
int xenus_cfg_get_int(const xenus_config_manager_t *m, const char *key,
                      int *out);
int xenus_cfg_get_bool(const xenus_config_manager_t *m, const char *key,
                       int *out);

/* Typed user-layer updates.  Invalid values are rejected and leave the
 * previous effective value untouched. */
int xenus_cfg_set_string(xenus_config_manager_t *m, const char *key,
                         const char *value);
int xenus_cfg_set_int(xenus_config_manager_t *m, const char *key, int value);
int xenus_cfg_set_bool(xenus_config_manager_t *m, const char *key, int value);

/* Remove a user override so the system/default layer becomes effective. */
int xenus_cfg_reset_user(xenus_config_manager_t *m, const char *key);

/* Validate every effective value against the schema. */
int xenus_cfg_validate(const xenus_config_manager_t *m);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_CONFIG_MANAGER_H */
