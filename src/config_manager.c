/*
 * XENUS Config Manager - typed, validated, layered configuration.
 *
 * Fixed-size storage, no heap allocation, single-threaded like the rest of
 * XENUS.  The manager owns a schema and three layers per key:
 *
 *   built-in default  <  system file  <  user file
 *
 * Only user overrides are ever written back, and writes use a 0600 temp
 * file, fsync and rename so a crash cannot leave a truncated config behind.
 */
#include <xenus/config_manager.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------- helpers -- */

static void set_error(xenus_config_manager_t *m, const char *fmt, ...)
{
    va_list ap;

    if (!m)
        return;
    va_start(ap, fmt);
    vsnprintf(m->last_error, sizeof(m->last_error), fmt, ap);
    va_end(ap);
}

static unsigned char lower_ch(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c - 'A' + 'a');
    return c;
}

static int ascii_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower_ch((unsigned char)*a) != lower_ch((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int ascii_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int ascii_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static int valid_key(const char *key)
{
    size_t i, len;

    if (!key || !*key)
        return 0;
    len = strlen(key);
    if (len >= XENUS_CFG_KEY_LEN)
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)key[i];

        if (!(ascii_alpha(c) || ascii_digit(c) || c == '_' || c == '-' ||
              c == '.'))
            return 0;
    }
    return 1;
}

static int key_like(const char *key, const char *needle)
{
    size_t i, nlen = strlen(needle);

    if (nlen == 0 || strlen(key) < nlen)
        return 0;
    for (i = 0; key[i]; i++) {
        size_t j;

        for (j = 0; j < nlen; j++) {
            if (lower_ch((unsigned char)key[i + j]) !=
                lower_ch((unsigned char)needle[j]))
                break;
        }
        if (j == nlen)
            return 1;
        if (!key[i + j])
            break;
    }
    return 0;
}

int xenus_cfg_key_is_secret(const char *key)
{
    static const char *const words[] = {
        "password", "passwd", "passphrase", "secret", "token",
        "api_key", "apikey", "access_key", "private_key", "credential",
        "credentials", "auth_key", "session_key", "cookie", NULL
    };
    size_t i;

    if (!key || !*key)
        return 0;
    for (i = 0; words[i]; i++) {
        if (key_like(key, words[i]))
            return 1;
    }
    return 0;
}

static int parse_int(const char *s, int *out)
{
    char *end;
    long n;

    if (!s || !*s)
        return 0;
    errno = 0;
    n = strtol(s, &end, 10);
    if (errno != 0 || end == s)
        return 0;
    while (*end && isspace((unsigned char)*end))
        end++;
    if (*end)
        return 0;
    if (n < INT_MIN || n > INT_MAX)
        return 0;
    *out = (int)n;
    return 1;
}

static int parse_bool(const char *s, int *out)
{
    if (!s)
        return 0;
    if (strcmp(s, "1") == 0 || ascii_ieq(s, "true") ||
        ascii_ieq(s, "yes") || ascii_ieq(s, "on") ||
        ascii_ieq(s, "light")) {
        *out = 1;
        return 1;
    }
    if (strcmp(s, "0") == 0 || ascii_ieq(s, "false") ||
        ascii_ieq(s, "no") || ascii_ieq(s, "off") ||
        ascii_ieq(s, "dark")) {
        *out = 0;
        return 1;
    }
    return 0;
}

/*
 * Validate and canonicalise a value.  On success `out` holds the normalised
 * form (decimals for ints, 0/1 for bools) and is always NUL terminated.
 */
static int normalise(const xenus_cfg_entry_t *e, const char *value, char *out,
                     size_t out_size)
{
    size_t len;

    if (!value)
        return 0;
    len = strlen(value);

    switch (e->type) {
    case XENUS_CFG_STRING: {
        size_t max = e->max_len ? e->max_len : XENUS_CFG_VALUE_LEN - 1;
        size_t i;

        if (len > max || len >= out_size)
            return 0;
        for (i = 0; i < len; i++) {
            unsigned char c = (unsigned char)value[i];

            /* keep the file format line-oriented and printable */
            if (c == '\n' || c == '\r' || c == '\0')
                return 0;
        }
        if (e->choices) {
            size_t c;

            for (c = 0; e->choices[c]; c++) {
                if (strcmp(e->choices[c], value) == 0)
                    break;
            }
            if (!e->choices[c])
                return 0;
        }
        memcpy(out, value, len + 1);
        return 1;
    }
    case XENUS_CFG_INT: {
        int n;

        if (!parse_int(value, &n) || n < e->min_int || n > e->max_int)
            return 0;
        snprintf(out, out_size, "%d", n);
        return 1;
    }
    case XENUS_CFG_BOOL: {
        int b;

        if (!parse_bool(value, &b))
            return 0;
        snprintf(out, out_size, "%d", b ? 1 : 0);
        return 1;
    }
    default:
        return 0;
    }
}

static xenus_cfg_entry_t *find_entry(const xenus_config_manager_t *m,
                                     const char *key)
{
    size_t i;

    if (!m || !key)
        return NULL;
    for (i = 0; i < m->count; i++) {
        if (strcmp(m->entries[i].key, key) == 0)
            return (xenus_cfg_entry_t *)&m->entries[i];
    }
    return NULL;
}

static const char *effective(const xenus_cfg_entry_t *e)
{
    if (e->has_user)
        return e->user_value;
    if (e->has_system)
        return e->system_value;
    return e->default_value;
}

static char *trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

/* ----------------------------------------------------------------- API -- */

void xenus_cfg_init(xenus_config_manager_t *m)
{
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
}

void xenus_cfg_set_ignore_unknown(xenus_config_manager_t *m, int ignore)
{
    if (m)
        m->ignore_unknown = ignore ? 1 : 0;
}

const char *xenus_cfg_type_name(xenus_cfg_type_t type)
{
    switch (type) {
    case XENUS_CFG_STRING: return "string";
    case XENUS_CFG_INT:    return "int";
    case XENUS_CFG_BOOL:   return "bool";
    default:               return "unknown";
    }
}

int xenus_cfg_define(xenus_config_manager_t *m, const xenus_cfg_spec_t *spec)
{
    xenus_cfg_entry_t *e;
    char normalised[XENUS_CFG_VALUE_LEN];

    if (!m || !spec) {
        set_error(m, "define: missing manager or spec");
        return XENUS_ERR_ARG;
    }
    if (!valid_key(spec->key)) {
        set_error(m, "define: invalid key '%s'",
                  spec->key ? spec->key : "(null)");
        return XENUS_ERR_ARG;
    }
    if (xenus_cfg_key_is_secret(spec->key)) {
        set_error(m, "define: refusing secret-like key '%s'", spec->key);
        return XENUS_ERR_ARG;
    }
    if (find_entry(m, spec->key)) {
        set_error(m, "define: duplicate key '%s'", spec->key);
        return XENUS_ERR_ARG;
    }
    if (m->count >= XENUS_CFG_MAX_KEYS) {
        set_error(m, "define: schema full (%d keys)", XENUS_CFG_MAX_KEYS);
        return XENUS_ERR_FULL;
    }
    if (spec->type != XENUS_CFG_STRING && spec->type != XENUS_CFG_INT &&
        spec->type != XENUS_CFG_BOOL) {
        set_error(m, "define: bad type for '%s'", spec->key);
        return XENUS_ERR_ARG;
    }
    if (spec->type == XENUS_CFG_INT && spec->min_int > spec->max_int) {
        set_error(m, "define: inverted range for '%s'", spec->key);
        return XENUS_ERR_ARG;
    }
    if (spec->type != XENUS_CFG_STRING && spec->choices) {
        set_error(m, "define: choices need a string key ('%s')", spec->key);
        return XENUS_ERR_ARG;
    }
    if (spec->max_len >= XENUS_CFG_VALUE_LEN) {
        set_error(m, "define: max_len too large for '%s'", spec->key);
        return XENUS_ERR_ARG;
    }

    e = &m->entries[m->count];
    memset(e, 0, sizeof(*e));
    snprintf(e->key, sizeof(e->key), "%s", spec->key);
    e->type = spec->type;
    e->min_int = spec->min_int;
    e->max_int = spec->max_int;
    e->max_len = spec->max_len;
    e->choices = spec->choices;

    if (!normalise(e, spec->default_value ? spec->default_value : "",
                   normalised, sizeof(normalised))) {
        set_error(m, "define: invalid default for '%s'", spec->key);
        return XENUS_ERR_ARG;
    }
    memcpy(e->default_value, normalised, sizeof(e->default_value));
    m->count++;
    return XENUS_OK;
}

int xenus_cfg_define_all(xenus_config_manager_t *m,
                         const xenus_cfg_spec_t *specs, size_t count)
{
    size_t i, saved;

    if (!m || (!specs && count)) {
        set_error(m, "define_all: missing manager or specs");
        return XENUS_ERR_ARG;
    }
    saved = m->count;
    for (i = 0; i < count; i++) {
        int rc = xenus_cfg_define(m, &specs[i]);

        if (rc != XENUS_OK) {
            memset(&m->entries[saved], 0,
                   sizeof(m->entries[0]) * (m->count - saved));
            m->count = saved;
            return rc;
        }
    }
    return XENUS_OK;
}

int xenus_cfg_set_paths(xenus_config_manager_t *m, const char *system_path,
                        const char *user_path)
{
    if (!m) {
        set_error(m, "set_paths: missing manager");
        return XENUS_ERR_ARG;
    }
    if ((system_path && strlen(system_path) >= sizeof(m->system_path)) ||
        (user_path && strlen(user_path) >= sizeof(m->user_path))) {
        set_error(m, "set_paths: path too long");
        return XENUS_ERR_ARG;
    }
    if (system_path)
        snprintf(m->system_path, sizeof(m->system_path), "%s", system_path);
    else
        m->system_path[0] = '\0';
    if (user_path)
        snprintf(m->user_path, sizeof(m->user_path), "%s", user_path);
    else
        m->user_path[0] = '\0';
    return XENUS_OK;
}

int xenus_cfg_default_paths(xenus_config_manager_t *m)
{
    const char *system_path;
    char user_path[XENUS_CFG_PATH_LEN];

    if (!m) {
        set_error(m, "default_paths: missing manager");
        return XENUS_ERR_ARG;
    }
    system_path = getenv("XENUS_SYSTEM_CONFIG");
    if (!system_path || !*system_path)
        system_path = "/etc/xenus/config";

    user_path[0] = '\0';
    {
        const char *env = getenv("XENUS_CONFIG");
        const char *xdg;
        const char *home;

        if (env && *env) {
            snprintf(user_path, sizeof(user_path), "%s", env);
        } else if ((xdg = getenv("XDG_CONFIG_HOME")) && xdg[0] == '/') {
            snprintf(user_path, sizeof(user_path), "%s/xenus/config", xdg);
        } else if ((home = getenv("HOME")) && *home) {
            snprintf(user_path, sizeof(user_path), "%s/.config/xenus/config",
                     home);
        }
    }
    return xenus_cfg_set_paths(m, system_path, user_path[0] ? user_path : NULL);
}

/* ---------------------------------------------------------------- load -- */

static int read_layer(xenus_config_manager_t *m, const char *path, int user,
                      int *saw_error)
{
    FILE *f;
    char line[1024];
    int lineno = 0;

    if (!path || !*path)
        return XENUS_OK;
    f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT)
            return XENUS_OK;      /* missing layer is normal */
        set_error(m, "%s: %s", path, strerror(errno));
        return XENUS_ERR_IO;
    }

    while (fgets(line, sizeof(line), f)) {
        char *s, *eq, *key, *val;
        xenus_cfg_entry_t *e;
        char normalised[XENUS_CFG_VALUE_LEN];

        lineno++;
        s = trim(line);
        if (*s == '\0' || *s == '#')
            continue;
        eq = strchr(s, '=');
        if (!eq) {
            if (!*saw_error)
                set_error(m, "%s:%d: missing '='", path, lineno);
            *saw_error = 1;
            continue;
        }
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);

        if (!valid_key(key)) {
            if (m->ignore_unknown)
                continue;
            if (!*saw_error)
                set_error(m, "%s:%d: invalid key", path, lineno);
            *saw_error = 1;
            continue;
        }
        if (xenus_cfg_key_is_secret(key)) {
            if (m->ignore_unknown)
                continue;
            if (!*saw_error)
                set_error(m, "%s:%d: secret-like keys are not allowed",
                          path, lineno);
            *saw_error = 1;
            continue;
        }
        e = find_entry(m, key);
        if (!e) {
            if (m->ignore_unknown)
                continue;
            if (!*saw_error)
                set_error(m, "%s:%d: unknown key '%s'", path, lineno, key);
            *saw_error = 1;
            continue;
        }
        if (!normalise(e, val, normalised, sizeof(normalised))) {
            if (!*saw_error)
                set_error(m, "%s:%d: invalid value for '%s'", path, lineno,
                          key);
            *saw_error = 1;
            continue;
        }
        if (user) {
            memcpy(e->user_value, normalised, sizeof(e->user_value));
            e->has_user = 1;
        } else {
            memcpy(e->system_value, normalised, sizeof(e->system_value));
            e->has_system = 1;
        }
    }
    if (ferror(f)) {
        set_error(m, "%s: read error", path);
        fclose(f);
        return XENUS_ERR_IO;
    }
    fclose(f);
    return XENUS_OK;
}

int xenus_cfg_load(xenus_config_manager_t *m)
{
    size_t i;
    int saw_error = 0;
    int rc;

    if (!m) {
        set_error(m, "load: missing manager");
        return XENUS_ERR_ARG;
    }
    /* Start from defaults every time so reloads do not keep stale layers. */
    for (i = 0; i < m->count; i++) {
        m->entries[i].has_system = 0;
        m->entries[i].has_user = 0;
        m->entries[i].system_value[0] = '\0';
        m->entries[i].user_value[0] = '\0';
    }
    m->last_error[0] = '\0';

    rc = read_layer(m, m->system_path, 0, &saw_error);
    if (rc != XENUS_OK)
        return rc;
    rc = read_layer(m, m->user_path, 1, &saw_error);
    if (rc != XENUS_OK)
        return rc;
    return saw_error ? XENUS_ERR : XENUS_OK;
}

/* ---------------------------------------------------------- accessors -- */

const char *xenus_cfg_last_error(const xenus_config_manager_t *m)
{
    if (!m)
        return "";
    return m->last_error;
}

const char *xenus_cfg_get(const xenus_config_manager_t *m, const char *key)
{
    xenus_cfg_entry_t *e = find_entry(m, key);

    return e ? effective(e) : NULL;
}

xenus_cfg_source_t xenus_cfg_source(const xenus_config_manager_t *m,
                                    const char *key)
{
    xenus_cfg_entry_t *e = find_entry(m, key);

    if (!e)
        return XENUS_CFG_SOURCE_NONE;
    if (e->has_user)
        return XENUS_CFG_SOURCE_USER;
    if (e->has_system)
        return XENUS_CFG_SOURCE_SYSTEM;
    return XENUS_CFG_SOURCE_DEFAULT;
}

int xenus_cfg_get_string(const xenus_config_manager_t *m, const char *key,
                         char *out, size_t out_size)
{
    const char *v = xenus_cfg_get(m, key);
    size_t len;

    if (!v || !out || out_size == 0)
        return XENUS_ERR_ARG;
    len = strlen(v);
    if (len >= out_size)
        return XENUS_ERR_ARG;
    memcpy(out, v, len + 1);
    return XENUS_OK;
}

int xenus_cfg_get_int(const xenus_config_manager_t *m, const char *key,
                      int *out)
{
    const char *v = xenus_cfg_get(m, key);

    if (!v || !out || !parse_int(v, out))
        return XENUS_ERR_ARG;
    return XENUS_OK;
}

int xenus_cfg_get_bool(const xenus_config_manager_t *m, const char *key,
                       int *out)
{
    const char *v = xenus_cfg_get(m, key);

    if (!v || !out || !parse_bool(v, out))
        return XENUS_ERR_ARG;
    return XENUS_OK;
}

/* ------------------------------------------------------------ setters -- */

static int store_user(xenus_config_manager_t *m, xenus_cfg_entry_t *e,
                      const char *value)
{
    char normalised[XENUS_CFG_VALUE_LEN];

    if (!normalise(e, value, normalised, sizeof(normalised))) {
        set_error(m, "set: invalid %s value for '%s'",
                  xenus_cfg_type_name(e->type), e->key);
        return XENUS_ERR_ARG;
    }
    memcpy(e->user_value, normalised, sizeof(e->user_value));
    e->has_user = 1;
    return XENUS_OK;
}

int xenus_cfg_set_string(xenus_config_manager_t *m, const char *key,
                         const char *value)
{
    xenus_cfg_entry_t *e = find_entry(m, key);

    if (!e) {
        set_error(m, "set: unknown key '%s'", key ? key : "(null)");
        return XENUS_ERR_NOT_FOUND;
    }
    if (e->type != XENUS_CFG_STRING) {
        set_error(m, "set: '%s' is not a string", key);
        return XENUS_ERR_ARG;
    }
    return store_user(m, e, value);
}

int xenus_cfg_set_int(xenus_config_manager_t *m, const char *key, int value)
{
    xenus_cfg_entry_t *e = find_entry(m, key);
    char buf[32];

    if (!e) {
        set_error(m, "set: unknown key '%s'", key ? key : "(null)");
        return XENUS_ERR_NOT_FOUND;
    }
    if (e->type != XENUS_CFG_INT) {
        set_error(m, "set: '%s' is not an int", key);
        return XENUS_ERR_ARG;
    }
    snprintf(buf, sizeof(buf), "%d", value);
    return store_user(m, e, buf);
}

int xenus_cfg_set_bool(xenus_config_manager_t *m, const char *key, int value)
{
    xenus_cfg_entry_t *e = find_entry(m, key);

    if (!e) {
        set_error(m, "set: unknown key '%s'", key ? key : "(null)");
        return XENUS_ERR_NOT_FOUND;
    }
    if (e->type != XENUS_CFG_BOOL) {
        set_error(m, "set: '%s' is not a bool", key);
        return XENUS_ERR_ARG;
    }
    return store_user(m, e, value ? "1" : "0");
}

int xenus_cfg_reset_user(xenus_config_manager_t *m, const char *key)
{
    xenus_cfg_entry_t *e = find_entry(m, key);

    if (!e) {
        set_error(m, "reset: unknown key '%s'", key ? key : "(null)");
        return XENUS_ERR_NOT_FOUND;
    }
    memset(e->user_value, 0, sizeof(e->user_value));
    e->has_user = 0;
    return XENUS_OK;
}

int xenus_cfg_validate(const xenus_config_manager_t *m)
{
    size_t i;
    char scratch[XENUS_CFG_VALUE_LEN];

    if (!m) {
        set_error((xenus_config_manager_t *)m, "validate: missing manager");
        return XENUS_ERR_ARG;
    }
    for (i = 0; i < m->count; i++) {
        const xenus_cfg_entry_t *e = &m->entries[i];

        if (!valid_key(e->key) || xenus_cfg_key_is_secret(e->key)) {
            set_error((xenus_config_manager_t *)m,
                      "validate: invalid or secret-like key");
            return XENUS_ERR;
        }
        if (e->type != XENUS_CFG_STRING && e->type != XENUS_CFG_INT &&
            e->type != XENUS_CFG_BOOL) {
            set_error((xenus_config_manager_t *)m,
                      "validate: bad type for '%s'", e->key);
            return XENUS_ERR;
        }
        if (!normalise(e, e->default_value, scratch, sizeof(scratch))) {
            set_error((xenus_config_manager_t *)m,
                      "validate: invalid default for '%s'", e->key);
            return XENUS_ERR;
        }
        if (e->has_system &&
            !normalise(e, e->system_value, scratch, sizeof(scratch))) {
            set_error((xenus_config_manager_t *)m,
                      "validate: invalid system value for '%s'", e->key);
            return XENUS_ERR;
        }
        if (e->has_user &&
            !normalise(e, e->user_value, scratch, sizeof(scratch))) {
            set_error((xenus_config_manager_t *)m,
                      "validate: invalid user value for '%s'", e->key);
            return XENUS_ERR;
        }
    }
    return XENUS_OK;
}

/* ---------------------------------------------------------------- save -- */

/* Create every missing parent directory with 0700 permissions. */
static int ensure_parent(const char *path)
{
    char dir[XENUS_CFG_PATH_LEN];
    char *p;

    if (!path || strlen(path) >= sizeof(dir))
        return -1;
    snprintf(dir, sizeof(dir), "%s", path);
    p = strrchr(dir, '/');
    if (!p)
        return 0;
    if (p == dir)
        return 0;                 /* write to /name */
    *p = '\0';
    for (p = dir + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(dir, 0700) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int xenus_cfg_save_user(xenus_config_manager_t *m)
{
    char tmp[XENUS_CFG_PATH_LEN + 16];
    size_t i;
    int fd = -1;
    FILE *f = NULL;
    int ok = 0;

    if (!m) {
        set_error(m, "save: missing manager");
        return XENUS_ERR_ARG;
    }
    if (!m->user_path[0]) {
        set_error(m, "save: no user config path");
        return XENUS_ERR_ARG;
    }
    if (xenus_cfg_validate(m) != XENUS_OK) {
        char err[XENUS_CFG_ERROR_LEN];

        snprintf(err, sizeof(err), "%s", m->last_error);
        set_error(m, "save: refusing invalid configuration: %s", err);
        return XENUS_ERR;
    }
    if (ensure_parent(m->user_path) != 0) {
        set_error(m, "save: cannot create directory for '%s': %s",
                  m->user_path, strerror(errno));
        return XENUS_ERR_IO;
    }
    if (strlen(m->user_path) + sizeof(".tmp.XXXXXX") >= sizeof(tmp)) {
        set_error(m, "save: user path too long");
        return XENUS_ERR_ARG;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", m->user_path);
    fd = mkstemp(tmp);
    if (fd < 0) {
        set_error(m, "save: mkstemp '%s': %s", tmp, strerror(errno));
        return XENUS_ERR_IO;
    }
    if (fchmod(fd, 0600) != 0) {
        set_error(m, "save: chmod '%s': %s", tmp, strerror(errno));
        goto out;
    }
    f = fdopen(fd, "w");
    if (!f) {
        set_error(m, "save: fdopen '%s': %s", tmp, strerror(errno));
        goto out;
    }
    fd = -1;                       /* owned by f now */

    fputs("# XENUS user configuration - generated by the Config Manager.\n"
          "# Only user overrides are stored here.\n", f);
    for (i = 0; i < m->count; i++) {
        const xenus_cfg_entry_t *e = &m->entries[i];

        if (!e->has_user)
            continue;
        if (fprintf(f, "%s=%s\n", e->key, e->user_value) < 0) {
            set_error(m, "save: write error for '%s'", e->key);
            goto out;
        }
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        set_error(m, "save: flush '%s': %s", tmp, strerror(errno));
        goto out;
    }
    if (fclose(f) != 0) {
        f = NULL;
        set_error(m, "save: close '%s': %s", tmp, strerror(errno));
        goto out_cleanup;
    }
    f = NULL;
    if (rename(tmp, m->user_path) != 0) {
        set_error(m, "save: rename '%s': %s", m->user_path, strerror(errno));
        goto out_cleanup;
    }
    ok = 1;

out:
    if (f && fclose(f) != 0 && ok)
        ok = 0;
    if (fd >= 0) {
        if (close(fd) != 0 && ok)
            ok = 0;
        fd = -1;
    }
out_cleanup:
    if (!ok) {
        if (fd >= 0)
            close(fd);
        unlink(tmp);
        return XENUS_ERR_IO;
    }
    return XENUS_OK;
}

/* --------------------------------------------------------- merge save -- */

/* Extract the key of a key=value line without modifying the original text. */
static int line_key(const char *line, char *key, size_t key_size)
{
    const char *p = line;
    const char *eq;
    size_t n;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#')
        return 0;
    eq = strchr(p, '=');
    if (!eq)
        return 0;
    while (eq > p && (eq[-1] == ' ' || eq[-1] == '\t'))
        eq--;
    n = (size_t)(eq - p);
    if (n == 0 || n >= key_size)
        return 0;
    memcpy(key, p, n);
    key[n] = '\0';
    return 1;
}

static int copy_line(FILE *out, const char *line)
{
    size_t n = strlen(line);

    if (fputs(line, out) < 0)
        return -1;
    if (n > 0 && line[n - 1] != '\n' && fputc('\n', out) == EOF)
        return -1;
    return 0;
}

int xenus_cfg_save_user_merge(xenus_config_manager_t *m)
{
    char tmp[XENUS_CFG_PATH_LEN + 16];
    char line[1024];
    FILE *in = NULL;
    FILE *f = NULL;
    size_t i;
    int fd = -1;
    int ok = 0;

    if (!m) {
        set_error(m, "save: missing manager");
        return XENUS_ERR_ARG;
    }
    if (!m->user_path[0]) {
        set_error(m, "save: no user config path");
        return XENUS_ERR_ARG;
    }
    if (xenus_cfg_validate(m) != XENUS_OK) {
        char err[XENUS_CFG_ERROR_LEN];

        snprintf(err, sizeof(err), "%s", m->last_error);
        set_error(m, "save: refusing invalid configuration: %s", err);
        return XENUS_ERR;
    }
    if (ensure_parent(m->user_path) != 0) {
        set_error(m, "save: cannot create directory for '%s': %s",
                  m->user_path, strerror(errno));
        return XENUS_ERR_IO;
    }
    if (strlen(m->user_path) + sizeof(".tmp.XXXXXX") >= sizeof(tmp)) {
        set_error(m, "save: user path too long");
        return XENUS_ERR_ARG;
    }

    in = fopen(m->user_path, "r");
    if (!in && errno != ENOENT) {
        set_error(m, "save: cannot read '%s': %s", m->user_path,
                  strerror(errno));
        return XENUS_ERR_IO;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", m->user_path);
    fd = mkstemp(tmp);
    if (fd < 0) {
        set_error(m, "save: mkstemp '%s': %s", tmp, strerror(errno));
        if (in)
            fclose(in);
        return XENUS_ERR_IO;
    }
    if (fchmod(fd, 0600) != 0) {
        set_error(m, "save: chmod '%s': %s", tmp, strerror(errno));
        goto out;
    }
    f = fdopen(fd, "w");
    if (!f) {
        set_error(m, "save: fdopen '%s': %s", tmp, strerror(errno));
        goto out;
    }
    fd = -1;                       /* owned by f now */

    if (!in && fputs("# XENUS user configuration - generated by the "
                     "Config Manager.\n", f) < 0)
        goto write_error;

    while (in && fgets(line, sizeof(line), in)) {
        char key[XENUS_CFG_KEY_LEN];

        /* Managed keys are dropped here and re-emitted once at the end so a
           duplicate legacy line can never shadow the value just saved. */
        if (line_key(line, key, sizeof(key)) && find_entry(m, key))
            continue;
        if (copy_line(f, line) != 0)
            goto write_error;
    }
    if (in && ferror(in)) {
        set_error(m, "save: read error on '%s'", m->user_path);
        goto out;
    }
    if (in) {
        fclose(in);
        in = NULL;
    }

    for (i = 0; i < m->count; i++) {
        const xenus_cfg_entry_t *e = &m->entries[i];

        if (!e->has_user)
            continue;
        if (fprintf(f, "%s=%s\n", e->key, e->user_value) < 0)
            goto write_error;
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        set_error(m, "save: flush '%s': %s", tmp, strerror(errno));
        goto out;
    }
    if (fclose(f) != 0) {
        f = NULL;
        set_error(m, "save: close '%s': %s", tmp, strerror(errno));
        goto out_cleanup;
    }
    f = NULL;
    if (rename(tmp, m->user_path) != 0) {
        set_error(m, "save: rename '%s': %s", m->user_path, strerror(errno));
        goto out_cleanup;
    }
    ok = 1;
    goto out;

write_error:
    set_error(m, "save: write error on '%s'", tmp);
out:
    if (in) {
        fclose(in);
        in = NULL;
    }
    if (f && fclose(f) != 0 && ok)
        ok = 0;
    if (fd >= 0) {
        if (close(fd) != 0 && ok)
            ok = 0;
        fd = -1;
    }
out_cleanup:
    if (!ok) {
        if (fd >= 0)
            close(fd);
        unlink(tmp);
        return XENUS_ERR_IO;
    }
    return XENUS_OK;
}
