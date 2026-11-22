/*
 * XENUS App API - read-only configuration access.
 *
 * Reads the same key=value file the XENUS session uses
 * ($XENUS_CONFIG or ~/.config/xenus/config) without depending on the
 * window manager's Config struct.  Fixed-size storage: no allocation.
 */
#ifndef XENUS_APP_CONFIG_H
#define XENUS_APP_CONFIG_H

#include <xenus/common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_CONFIG_MAX_KEYS 128
#define XENUS_CONFIG_KEY_LEN 64
#define XENUS_CONFIG_VALUE_LEN 192

/* path = NULL selects $XENUS_CONFIG, else ~/.config/xenus/config. */
int xenus_config_load(const char *path);
int xenus_config_reload(void);
const char *xenus_config_path(void);
void xenus_config_clear(void);

const char *xenus_config_get(const char *key, const char *default_value);
int xenus_config_get_int(const char *key, int default_value);
int xenus_config_get_bool(const char *key, int default_value); /* 1/0 */
int xenus_config_has(const char *key);

size_t xenus_config_count(void);
const char *xenus_config_key(size_t index);
const char *xenus_config_value(size_t index);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_APP_CONFIG_H */
