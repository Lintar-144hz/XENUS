/*
 * XENUS BIOS engine - persistent store, staged edits, validation, recovery.
 *
 * Plain C11 + POSIX.  No Xlib, no desktop globals, no UCore dependency.
 */
#include <xenus/bios.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Store paths and disk labels are fixed-size by design; truncation at the
 * boundary is harmless and already handled by snprintf. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

/* ------------------------------------------------------------- helpers -- */

static void copy_str(char *dst, size_t size, const char *src)
{
    if (!size)
        return;
    if (!src)
        src = "";
    snprintf(dst, size, "%s", src);
}

static char *trim(char *s)
{
    char *end;

    if (!s)
        return s;
    while (*s && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int parse_bool(const char *v, int fallback)
{
    if (!v)
        return fallback;
    if (strcmp(v, "1") == 0 || strcmp(v, "on") == 0 ||
        strcmp(v, "yes") == 0 || strcmp(v, "true") == 0 ||
        strcmp(v, "enabled") == 0)
        return 1;
    if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0 ||
        strcmp(v, "no") == 0 || strcmp(v, "false") == 0 ||
        strcmp(v, "disabled") == 0)
        return 0;
    return fallback;
}

static int parse_int(const char *v, int lo, int hi, int fallback)
{
    char *end;
    long n;

    if (!v || !*v)
        return fallback;
    errno = 0;
    n = strtol(v, &end, 10);
    if (errno != 0 || end == v || *trim(end) != '\0' || n < lo || n > hi)
        return fallback;
    return (int)n;
}

static int mkdir_p(const char *path)
{
    char buf[XENUS_BIOS_PATH_MAX];
    char *p;
    size_t len;

    if (!path || !*path)
        return -1;
    copy_str(buf, sizeof(buf), path);
    len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = '\0';
    for (p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int parent_dir(const char *path, char *out, size_t size)
{
    const char *slash;

    if (!path || !out || !size)
        return -1;
    slash = strrchr(path, '/');
    if (!slash) {
        copy_str(out, size, ".");
        return 0;
    }
    if (slash == path)
        copy_str(out, size, "/");
    else {
        size_t n = (size_t)(slash - path);

        if (n >= size)
            n = size - 1;
        memcpy(out, path, n);
        out[n] = '\0';
    }
    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in, *out;
    char buf[4096];
    size_t n;

    in = fopen(src, "rb");
    if (!in)
        return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            unlink(dst);
            return -1;
        }
    }
    if (ferror(in)) {
        fclose(in);
        fclose(out);
        unlink(dst);
        return -1;
    }
    fclose(in);
    if (fclose(out) != 0) {
        unlink(dst);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ defaults -- */

void xenus_bios_config_defaults(xenus_bios_config_t *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = XENUS_BIOS_CONFIG_VERSION;

    cfg->system.theme = XENUS_BIOS_THEME_DARK;
    cfg->system.accent = XENUS_BIOS_ACCENT_BLUE;
    cfg->system.animations = 1;
    cfg->system.show_clock = 1;
    cfg->system.focus_follows_mouse = 1;

    cfg->drivers.input = 1;
    cfg->drivers.display = 1;
    cfg->drivers.storage = 1;
    cfg->drivers.audio = 1;
    cfg->drivers.network = 1;
    cfg->drivers.android_bridge = 1;

    cfg->ucore.sysinfo = 1;
    cfg->ucore.process = 1;
    cfg->ucore.applications = 1;
    cfg->ucore.hardware = 1;
    cfg->ucore.notifications = 1;

    cfg->android.termux_api = 1;
    cfg->android.termux_x11 = 1;
    cfg->android.android_bridge = 1;
    cfg->android.notifications = 1;
    cfg->android.clipboard = 1;
    cfg->android.storage_access = 0;

    cfg->boot.mode = XENUS_BIOS_BOOT_NORMAL;
    cfg->boot.diagnostic = 0;
    cfg->boot.delay_seconds = 0;

    cfg->recovery.backup_before_apply = 1;
}

int xenus_bios_config_equal(const xenus_bios_config_t *a,
                            const xenus_bios_config_t *b)
{
    if (!a || !b)
        return 0;
    return a->version == b->version &&
           a->system.theme == b->system.theme &&
           a->system.accent == b->system.accent &&
           a->system.animations == b->system.animations &&
           a->system.show_clock == b->system.show_clock &&
           a->system.focus_follows_mouse == b->system.focus_follows_mouse &&
           a->drivers.input == b->drivers.input &&
           a->drivers.display == b->drivers.display &&
           a->drivers.storage == b->drivers.storage &&
           a->drivers.audio == b->drivers.audio &&
           a->drivers.network == b->drivers.network &&
           a->drivers.android_bridge == b->drivers.android_bridge &&
           a->ucore.sysinfo == b->ucore.sysinfo &&
           a->ucore.process == b->ucore.process &&
           a->ucore.applications == b->ucore.applications &&
           a->ucore.hardware == b->ucore.hardware &&
           a->ucore.notifications == b->ucore.notifications &&
           a->android.termux_api == b->android.termux_api &&
           a->android.termux_x11 == b->android.termux_x11 &&
           a->android.android_bridge == b->android.android_bridge &&
           a->android.notifications == b->android.notifications &&
           a->android.clipboard == b->android.clipboard &&
           a->android.storage_access == b->android.storage_access &&
           a->boot.mode == b->boot.mode &&
           a->boot.diagnostic == b->boot.diagnostic &&
           a->boot.delay_seconds == b->boot.delay_seconds &&
           a->recovery.backup_before_apply ==
               b->recovery.backup_before_apply;
}

/* --------------------------------------------------------------- names -- */

const char *xenus_bios_theme_name(int value)
{
    return value == XENUS_BIOS_THEME_LIGHT ? "Light" : "Dark";
}

const char *xenus_bios_accent_name(int value)
{
    switch (value) {
    case XENUS_BIOS_ACCENT_SILVER: return "Silver";
    case XENUS_BIOS_ACCENT_GRAPHITE: return "Graphite";
    default: return "Harbor Blue";
    }
}

const char *xenus_bios_boot_mode_name(int value)
{
    return value == XENUS_BIOS_BOOT_SAFE ? "Safe" : "Normal";
}

const char *xenus_bios_bool_name(int value)
{
    return value ? "Enabled" : "Disabled";
}

int xenus_bios_theme_parse(const char *text)
{
    if (!text)
        return XENUS_BIOS_THEME_DARK;
    if (strcmp(text, "light") == 0 || strcmp(text, "1") == 0)
        return XENUS_BIOS_THEME_LIGHT;
    return XENUS_BIOS_THEME_DARK;
}

int xenus_bios_accent_parse(const char *text)
{
    if (!text)
        return XENUS_BIOS_ACCENT_BLUE;
    if (strcmp(text, "silver") == 0 || strcmp(text, "1") == 0)
        return XENUS_BIOS_ACCENT_SILVER;
    if (strcmp(text, "graphite") == 0 || strcmp(text, "2") == 0)
        return XENUS_BIOS_ACCENT_GRAPHITE;
    return XENUS_BIOS_ACCENT_BLUE;
}

int xenus_bios_boot_mode_parse(const char *text)
{
    if (!text)
        return XENUS_BIOS_BOOT_NORMAL;
    if (strcmp(text, "safe") == 0 || strcmp(text, "1") == 0)
        return XENUS_BIOS_BOOT_SAFE;
    return XENUS_BIOS_BOOT_NORMAL;
}

/* ----------------------------------------------------------- validation -- */

static void add_issue(xenus_bios_validation_t *out, int error,
                      const char *text)
{
    if (!out || out->count >= XENUS_BIOS_MAX_ISSUES)
        return;
    out->items[out->count].error = error;
    copy_str(out->items[out->count].text,
             sizeof(out->items[out->count].text), text);
    out->count++;
    if (error)
        out->errors++;
}

int xenus_bios_config_validate(const xenus_bios_config_t *cfg,
                               xenus_bios_validation_t *out)
{
    if (!out)
        return XENUS_BIOS_ERR_ARG;
    memset(out, 0, sizeof(*out));
    if (!cfg) {
        add_issue(out, 1, "Configuration is missing.");
        return XENUS_BIOS_ERR_ARG;
    }

    if (cfg->version != XENUS_BIOS_CONFIG_VERSION)
        add_issue(out, 1, "Unsupported BIOS configuration version.");
    if (cfg->system.theme < XENUS_BIOS_THEME_DARK ||
        cfg->system.theme > XENUS_BIOS_THEME_LIGHT)
        add_issue(out, 1, "System theme is out of range.");
    if (cfg->system.accent < XENUS_BIOS_ACCENT_BLUE ||
        cfg->system.accent > XENUS_BIOS_ACCENT_GRAPHITE)
        add_issue(out, 1, "System accent is out of range.");
    if (cfg->system.animations < 0 || cfg->system.animations > 1)
        add_issue(out, 1, "Animation flag must be 0 or 1.");
    if (cfg->system.show_clock < 0 || cfg->system.show_clock > 1)
        add_issue(out, 1, "Menu clock flag must be 0 or 1.");
    if (cfg->system.focus_follows_mouse < 0 ||
        cfg->system.focus_follows_mouse > 1)
        add_issue(out, 1, "Focus mode must be 0 or 1.");

    if (!cfg->drivers.input)
        add_issue(out, 1, "Input bridge is required by BIOS setup.");
    if (!cfg->drivers.display)
        add_issue(out, 1, "Display bridge is required by BIOS setup.");
    if (!cfg->drivers.storage)
        add_issue(out, 1, "Configuration store is required by BIOS setup.");

    if (!cfg->ucore.sysinfo)
        add_issue(out, 1, "UCore sysinfo is mandatory.");
    if (cfg->ucore.process < 0 || cfg->ucore.process > 1 ||
        cfg->ucore.applications < 0 || cfg->ucore.applications > 1 ||
        cfg->ucore.hardware < 0 || cfg->ucore.hardware > 1 ||
        cfg->ucore.notifications < 0 ||
        cfg->ucore.notifications > 1)
        add_issue(out, 1, "A UCore module flag is invalid.");

    if (cfg->android.termux_api < 0 || cfg->android.termux_api > 1 ||
        cfg->android.termux_x11 < 0 || cfg->android.termux_x11 > 1 ||
        cfg->android.android_bridge < 0 ||
        cfg->android.android_bridge > 1 ||
        cfg->android.notifications < 0 ||
        cfg->android.notifications > 1 ||
        cfg->android.clipboard < 0 || cfg->android.clipboard > 1 ||
        cfg->android.storage_access < 0 ||
        cfg->android.storage_access > 1)
        add_issue(out, 1, "An Android capability flag is invalid.");

    if (cfg->boot.mode < XENUS_BIOS_BOOT_NORMAL ||
        cfg->boot.mode > XENUS_BIOS_BOOT_SAFE)
        add_issue(out, 1, "Startup mode is out of range.");
    if (cfg->boot.diagnostic < 0 || cfg->boot.diagnostic > 1)
        add_issue(out, 1, "Diagnostic boot flag must be 0 or 1.");
    if (cfg->boot.delay_seconds < 0 || cfg->boot.delay_seconds > 10)
        add_issue(out, 1, "Boot delay must be between 0 and 10 seconds.");
    if (cfg->recovery.backup_before_apply < 0 ||
        cfg->recovery.backup_before_apply > 1)
        add_issue(out, 1, "Backup policy must be 0 or 1.");

    if (cfg->boot.mode == XENUS_BIOS_BOOT_SAFE)
        add_issue(out, 0,
                  "Safe mode skips plugins and shell animations at the next "
                  "XENUS start.");
    if (cfg->android.termux_x11 && !cfg->android.termux_api)
        add_issue(out, 0,
                  "Termux:X11 is independent of the Termux API; the bridge "
                  "can remain enabled.");
    if (!cfg->android.termux_api &&
        (cfg->android.notifications || cfg->android.clipboard ||
         cfg->android.storage_access))
        add_issue(out, 0,
                  "Notification, clipboard and storage helpers rely on the "
                  "Termux API; they may be unavailable.");
    if (cfg->boot.delay_seconds > 0)
        add_issue(out, 0,
                  "The desktop start will be delayed by the configured "
                  "number of seconds.");

    return out->errors ? XENUS_BIOS_ERR_INVALID : XENUS_BIOS_OK;
}

/* ---------------------------------------------------------------- diff -- */

static void diff_add(xenus_bios_diff_t *out, int max, int *n,
                     const char *field, const char *from, const char *to)
{
    if (!out || !n || *n >= max)
        return;
    copy_str(out[*n].field, sizeof(out[*n].field), field);
    copy_str(out[*n].from, sizeof(out[*n].from), from);
    copy_str(out[*n].to, sizeof(out[*n].to), to);
    (*n)++;
}

#define DIFF_INT(label, av, bv) do { \
        if ((av) != (bv)) { \
            char fa[32], fb[32]; \
            snprintf(fa, sizeof(fa), "%d", (int)(av)); \
            snprintf(fb, sizeof(fb), "%d", (int)(bv)); \
            diff_add(out, max, &n, label, fa, fb); \
        } \
    } while (0)

int xenus_bios_config_diff(const xenus_bios_config_t *from,
                           const xenus_bios_config_t *to,
                           xenus_bios_diff_t *out, int max)
{
    int n = 0;

    if (!from || !to || !out || max <= 0)
        return 0;
    if (from->system.theme != to->system.theme)
        diff_add(out, max, &n, "System > Theme",
                 xenus_bios_theme_name(from->system.theme),
                 xenus_bios_theme_name(to->system.theme));
    if (from->system.accent != to->system.accent)
        diff_add(out, max, &n, "System > Accent",
                 xenus_bios_accent_name(from->system.accent),
                 xenus_bios_accent_name(to->system.accent));
    DIFF_INT("System > Animations", from->system.animations,
             to->system.animations);
    DIFF_INT("System > Menu clock", from->system.show_clock,
             to->system.show_clock);
    DIFF_INT("System > Focus follows mouse",
             from->system.focus_follows_mouse,
             to->system.focus_follows_mouse);

    DIFF_INT("Drivers > Input bridge", from->drivers.input,
             to->drivers.input);
    DIFF_INT("Drivers > Display bridge", from->drivers.display,
             to->drivers.display);
    DIFF_INT("Drivers > Configuration store", from->drivers.storage,
             to->drivers.storage);
    DIFF_INT("Drivers > Audio bridge", from->drivers.audio,
             to->drivers.audio);
    DIFF_INT("Drivers > Network bridge", from->drivers.network,
             to->drivers.network);
    DIFF_INT("Drivers > Android bridge", from->drivers.android_bridge,
             to->drivers.android_bridge);

    DIFF_INT("UCore > sysinfo", from->ucore.sysinfo, to->ucore.sysinfo);
    DIFF_INT("UCore > process", from->ucore.process, to->ucore.process);
    DIFF_INT("UCore > applications", from->ucore.applications,
             to->ucore.applications);
    DIFF_INT("UCore > hardware", from->ucore.hardware, to->ucore.hardware);
    DIFF_INT("UCore > notifications", from->ucore.notifications,
             to->ucore.notifications);

    DIFF_INT("Android > Termux API", from->android.termux_api,
             to->android.termux_api);
    DIFF_INT("Android > Termux:X11", from->android.termux_x11,
             to->android.termux_x11);
    DIFF_INT("Android > Android command bridge",
             from->android.android_bridge, to->android.android_bridge);
    DIFF_INT("Android > Notifications", from->android.notifications,
             to->android.notifications);
    DIFF_INT("Android > Clipboard", from->android.clipboard,
             to->android.clipboard);
    DIFF_INT("Android > Shared storage", from->android.storage_access,
             to->android.storage_access);

    if (from->boot.mode != to->boot.mode)
        diff_add(out, max, &n, "Boot > Startup mode",
                 xenus_bios_boot_mode_name(from->boot.mode),
                 xenus_bios_boot_mode_name(to->boot.mode));
    DIFF_INT("Boot > Diagnostic logging", from->boot.diagnostic,
             to->boot.diagnostic);
    DIFF_INT("Boot > Delay (seconds)", from->boot.delay_seconds,
             to->boot.delay_seconds);
    DIFF_INT("Recovery > Backup before apply",
             from->recovery.backup_before_apply,
             to->recovery.backup_before_apply);

    return n;
}

#undef DIFF_INT

/* ---------------------------------------------------------------- paths -- */

static const char *config_home(char *buf, size_t size)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (!buf || size == 0)
        return NULL;
    if (xdg && xdg[0] == '/') {
        copy_str(buf, size, xdg);
        return buf;
    }
    if (!home || !*home)
        return NULL;
    snprintf(buf, size, "%s/.config", home);
    return buf;
}

const char *xenus_bios_store_path(char *buf, size_t size)
{
    char base[XENUS_BIOS_PATH_MAX];

    if (!config_home(base, sizeof(base)))
        return NULL;
    snprintf(buf, size, "%s/xenus/%s", base, XENUS_BIOS_STORE_NAME);
    return buf;
}

const char *xenus_bios_shell_path(char *buf, size_t size)
{
    char base[XENUS_BIOS_PATH_MAX];

    if (!config_home(base, sizeof(base)))
        return NULL;
    snprintf(buf, size, "%s/xenus/config", base);
    return buf;
}

const char *xenus_bios_backup_dir(char *buf, size_t size)
{
    char base[XENUS_BIOS_PATH_MAX];

    if (!config_home(base, sizeof(base)))
        return NULL;
    snprintf(buf, size, "%s/xenus/backups", base);
    return buf;
}

void xenus_bios_engine_default_paths(xenus_bios_engine_t *e)
{
    char fallback[XENUS_BIOS_PATH_MAX];

    if (!e)
        return;
    if (!xenus_bios_store_path(e->store_path, sizeof(e->store_path))) {
        copy_str(fallback, sizeof(fallback), "./xenus-bios.conf");
        copy_str(e->store_path, sizeof(e->store_path), fallback);
    }
    if (!xenus_bios_shell_path(e->shell_path, sizeof(e->shell_path)))
        copy_str(e->shell_path, sizeof(e->shell_path), "./xenus-config");
    if (!xenus_bios_backup_dir(e->backup_dir, sizeof(e->backup_dir)))
        copy_str(e->backup_dir, sizeof(e->backup_dir), "./xenus-backups");
}

/* --------------------------------------------------------------- text -- */

int xenus_bios_config_text(const xenus_bios_config_t *cfg, char *out,
                           size_t size)
{
    int n;

    if (!cfg || !out || size == 0)
        return XENUS_BIOS_ERR_ARG;
    n = snprintf(out, size,
        "# XENUS BIOS (X-FIRM) configuration\n"
        "# Managed by xenus-bios.  Staged edits are only written here after\n"
        "# validation and Apply / Save & Exit.\n"
        "bios_version=%d\n"
        "\n[system]\n"
        "theme=%s\n"
        "accent=%s\n"
        "animations=%d\n"
        "show_clock=%d\n"
        "focus_follows_mouse=%d\n"
        "\n[drivers]\n"
        "input=%d\n"
        "display=%d\n"
        "storage=%d\n"
        "audio=%d\n"
        "network=%d\n"
        "android_bridge=%d\n"
        "\n[ucore]\n"
        "sysinfo=%d\n"
        "process=%d\n"
        "applications=%d\n"
        "hardware=%d\n"
        "notifications=%d\n"
        "\n[android]\n"
        "termux_api=%d\n"
        "termux_x11=%d\n"
        "android_bridge=%d\n"
        "notifications=%d\n"
        "clipboard=%d\n"
        "storage_access=%d\n"
        "\n[boot]\n"
        "mode=%s\n"
        "diagnostic=%d\n"
        "delay_seconds=%d\n"
        "\n[recovery]\n"
        "backup_before_apply=%d\n",
        cfg->version,
        cfg->system.theme == XENUS_BIOS_THEME_LIGHT ? "light" : "dark",
        cfg->system.accent == XENUS_BIOS_ACCENT_SILVER ? "silver" :
        cfg->system.accent == XENUS_BIOS_ACCENT_GRAPHITE ? "graphite" :
        "blue",
        cfg->system.animations,
        cfg->system.show_clock,
        cfg->system.focus_follows_mouse,
        cfg->drivers.input,
        cfg->drivers.display,
        cfg->drivers.storage,
        cfg->drivers.audio,
        cfg->drivers.network,
        cfg->drivers.android_bridge,
        cfg->ucore.sysinfo,
        cfg->ucore.process,
        cfg->ucore.applications,
        cfg->ucore.hardware,
        cfg->ucore.notifications,
        cfg->android.termux_api,
        cfg->android.termux_x11,
        cfg->android.android_bridge,
        cfg->android.notifications,
        cfg->android.clipboard,
        cfg->android.storage_access,
        cfg->boot.mode == XENUS_BIOS_BOOT_SAFE ? "safe" : "normal",
        cfg->boot.diagnostic,
        cfg->boot.delay_seconds,
        cfg->recovery.backup_before_apply);
    if (n < 0)
        return XENUS_BIOS_ERR;
    return (size_t)n < size ? XENUS_BIOS_OK : XENUS_BIOS_ERR_NOMEM;
}

int xenus_bios_config_load(xenus_bios_config_t *cfg, const char *path)
{
    char defpath[XENUS_BIOS_PATH_MAX];
    char line[512];
    char section[32] = "";
    FILE *f;
    int have_any = 0;

    if (!cfg)
        return XENUS_BIOS_ERR_ARG;
    if (!path) {
        if (!xenus_bios_store_path(defpath, sizeof(defpath)))
            return XENUS_BIOS_ERR_IO;
        path = defpath;
    }
    xenus_bios_config_defaults(cfg);
    f = fopen(path, "r");
    if (!f)
        return errno == ENOENT ? XENUS_BIOS_DEFAULTS : XENUS_BIOS_ERR_IO;

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        char *eq;
        char *key;
        char *val;

        if (!*s || *s == '#')
            continue;
        if (*s == '[') {
            char *end = strchr(s, ']');

            if (!end)
                continue;
            *end = '\0';
            copy_str(section, sizeof(section), trim(s + 1));
            continue;
        }
        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);
        have_any = 1;

        if (strcmp(key, "bios_version") == 0) {
            cfg->version = parse_int(val, 1, 999, cfg->version);
        } else if (strcmp(section, "system") == 0) {
            if (strcmp(key, "theme") == 0)
                cfg->system.theme = xenus_bios_theme_parse(val);
            else if (strcmp(key, "accent") == 0)
                cfg->system.accent = xenus_bios_accent_parse(val);
            else if (strcmp(key, "animations") == 0)
                cfg->system.animations = parse_bool(val, 1);
            else if (strcmp(key, "show_clock") == 0)
                cfg->system.show_clock = parse_bool(val, 1);
            else if (strcmp(key, "focus_follows_mouse") == 0)
                cfg->system.focus_follows_mouse = parse_bool(val, 1);
        } else if (strcmp(section, "drivers") == 0) {
            if (strcmp(key, "input") == 0)
                cfg->drivers.input = parse_bool(val, 1);
            else if (strcmp(key, "display") == 0)
                cfg->drivers.display = parse_bool(val, 1);
            else if (strcmp(key, "storage") == 0)
                cfg->drivers.storage = parse_bool(val, 1);
            else if (strcmp(key, "audio") == 0)
                cfg->drivers.audio = parse_bool(val, 1);
            else if (strcmp(key, "network") == 0)
                cfg->drivers.network = parse_bool(val, 1);
            else if (strcmp(key, "android_bridge") == 0)
                cfg->drivers.android_bridge = parse_bool(val, 1);
        } else if (strcmp(section, "ucore") == 0) {
            if (strcmp(key, "sysinfo") == 0)
                cfg->ucore.sysinfo = parse_bool(val, 1);
            else if (strcmp(key, "process") == 0)
                cfg->ucore.process = parse_bool(val, 1);
            else if (strcmp(key, "applications") == 0)
                cfg->ucore.applications = parse_bool(val, 1);
            else if (strcmp(key, "hardware") == 0)
                cfg->ucore.hardware = parse_bool(val, 1);
            else if (strcmp(key, "notifications") == 0)
                cfg->ucore.notifications = parse_bool(val, 1);
        } else if (strcmp(section, "android") == 0) {
            if (strcmp(key, "termux_api") == 0)
                cfg->android.termux_api = parse_bool(val, 1);
            else if (strcmp(key, "termux_x11") == 0)
                cfg->android.termux_x11 = parse_bool(val, 1);
            else if (strcmp(key, "android_bridge") == 0)
                cfg->android.android_bridge = parse_bool(val, 1);
            else if (strcmp(key, "notifications") == 0)
                cfg->android.notifications = parse_bool(val, 1);
            else if (strcmp(key, "clipboard") == 0)
                cfg->android.clipboard = parse_bool(val, 1);
            else if (strcmp(key, "storage_access") == 0)
                cfg->android.storage_access = parse_bool(val, 0);
        } else if (strcmp(section, "boot") == 0) {
            if (strcmp(key, "mode") == 0)
                cfg->boot.mode = xenus_bios_boot_mode_parse(val);
            else if (strcmp(key, "diagnostic") == 0)
                cfg->boot.diagnostic = parse_bool(val, 0);
            else if (strcmp(key, "delay_seconds") == 0)
                cfg->boot.delay_seconds = parse_int(val, 0, 10, 0);
        } else if (strcmp(section, "recovery") == 0) {
            if (strcmp(key, "backup_before_apply") == 0)
                cfg->recovery.backup_before_apply = parse_bool(val, 1);
        }
    }
    fclose(f);
    if (cfg->version < 1)
        cfg->version = XENUS_BIOS_CONFIG_VERSION;
    return have_any ? XENUS_BIOS_OK : XENUS_BIOS_DEFAULTS;
}

int xenus_bios_config_save(const xenus_bios_config_t *cfg, const char *path)
{
    char defpath[XENUS_BIOS_PATH_MAX];
    char dir[XENUS_BIOS_PATH_MAX];
    char tmp[XENUS_BIOS_PATH_MAX + 32];
    char *text;
    FILE *f;
    size_t len;
    int rc;

    if (!cfg)
        return XENUS_BIOS_ERR_ARG;
    if (!path) {
        if (!xenus_bios_store_path(defpath, sizeof(defpath)))
            return XENUS_BIOS_ERR_IO;
        path = defpath;
    }
    if (parent_dir(path, dir, sizeof(dir)) != 0)
        return XENUS_BIOS_ERR_IO;
    if (mkdir_p(dir) != 0)
        return XENUS_BIOS_ERR_IO;

    text = malloc(4096);
    if (!text)
        return XENUS_BIOS_ERR_NOMEM;
    rc = xenus_bios_config_text(cfg, text, 4096);
    if (rc != XENUS_BIOS_OK) {
        free(text);
        return rc;
    }
    len = strlen(text);
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    f = fopen(tmp, "w");
    if (!f) {
        free(text);
        return XENUS_BIOS_ERR_IO;
    }
    if (fwrite(text, 1, len, f) != len) {
        fclose(f);
        unlink(tmp);
        free(text);
        return XENUS_BIOS_ERR_IO;
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        free(text);
        return XENUS_BIOS_ERR_IO;
    }
    if (fclose(f) != 0) {
        unlink(tmp);
        free(text);
        return XENUS_BIOS_ERR_IO;
    }
    free(text);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return XENUS_BIOS_ERR_IO;
    }
    return XENUS_BIOS_OK;
}

/* --------------------------------------------------------------- engine -- */

int xenus_bios_engine_open(xenus_bios_engine_t *e, const char *path)
{
    char dir[XENUS_BIOS_PATH_MAX];
    int rc;

    if (!e)
        return XENUS_BIOS_ERR_ARG;
    memset(e, 0, sizeof(*e));
    if (path) {
        copy_str(e->store_path, sizeof(e->store_path), path);
        if (parent_dir(path, dir, sizeof(dir)) == 0)
            snprintf(e->shell_path, sizeof(e->shell_path), "%s/config", dir);
        else
            copy_str(e->shell_path, sizeof(e->shell_path), "config");
        snprintf(e->backup_dir, sizeof(e->backup_dir), "%s/backups", dir);
    } else {
        xenus_bios_engine_default_paths(e);
    }

    rc = xenus_bios_config_load(&e->live, e->store_path);
    if (rc < 0)
        return rc;
    e->draft = e->live;
    e->loaded = rc == XENUS_BIOS_OK;
    e->dirty = 0;
    e->last_result = rc;
    copy_str(e->last_message, sizeof(e->last_message),
             rc == XENUS_BIOS_OK ? "Configuration store loaded." :
             "No store yet; defaults staged.");
    return rc;
}

void xenus_bios_engine_touch(xenus_bios_engine_t *e)
{
    if (!e)
        return;
    e->dirty = !xenus_bios_config_equal(&e->live, &e->draft);
}

int xenus_bios_engine_is_dirty(const xenus_bios_engine_t *e)
{
    if (!e)
        return 0;
    return e->dirty || !xenus_bios_config_equal(&e->live, &e->draft);
}

int xenus_bios_engine_discard(xenus_bios_engine_t *e)
{
    if (!e)
        return XENUS_BIOS_ERR_ARG;
    e->draft = e->live;
    e->dirty = 0;
    e->last_result = XENUS_BIOS_OK;
    copy_str(e->last_message, sizeof(e->last_message),
             "Staged changes discarded.");
    return XENUS_BIOS_OK;
}

static int write_previous_backup(xenus_bios_engine_t *e, char *msg,
                                 size_t msg_size)
{
    char latest[XENUS_BIOS_PATH_MAX];
    char stamp[64];
    char name[128];
    time_t now;
    struct tm tmv;
    int rc;

    if (access(e->store_path, R_OK) != 0) {
        if (msg)
            copy_str(msg, msg_size, "No previous store to back up.");
        return XENUS_BIOS_OK;
    }
    if (mkdir_p(e->backup_dir) != 0) {
        if (msg)
            copy_str(msg, msg_size, "Cannot create the backup directory.");
        return XENUS_BIOS_ERR_IO;
    }
    now = time(NULL);
    if (!localtime_r(&now, &tmv)) {
        if (msg)
            copy_str(msg, msg_size, "Cannot read the clock.");
        return XENUS_BIOS_ERR_IO;
    }
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    snprintf(latest, sizeof(latest), "%s/bios-%s.conf", e->backup_dir,
             stamp);
    if (access(latest, F_OK) != 0)
        rc = copy_file(e->store_path, latest);
    else
        rc = -1;
    if (rc != 0) {
        int i;

        for (i = 1; i < 100; i++) {
            snprintf(name, sizeof(name), "bios-%s-%03d.conf", stamp, i);
            snprintf(latest, sizeof(latest), "%s/%s", e->backup_dir, name);
            if (access(latest, F_OK) != 0) {
                rc = copy_file(e->store_path, latest);
                break;
            }
        }
    }
    if (rc != 0) {
        if (msg)
            copy_str(msg, msg_size, "Cannot write the backup copy.");
        return XENUS_BIOS_ERR_IO;
    }
    if (msg)
        copy_str(msg, msg_size, "Previous configuration backed up.");
    return XENUS_BIOS_OK;
}

int xenus_bios_engine_apply(xenus_bios_engine_t *e, int sync_shell,
                            char *msg, size_t msg_size)
{
    xenus_bios_validation_t v;
    char shell_msg[192];
    int got_shell;
    int rc;

    if (!e)
        return XENUS_BIOS_ERR_ARG;
    xenus_bios_engine_touch(e);
    if (!e->dirty) {
        e->last_result = XENUS_BIOS_OK;
        copy_str(e->last_message, sizeof(e->last_message),
                 "No staged changes.");
        if (msg)
            copy_str(msg, msg_size, e->last_message);
        return XENUS_BIOS_OK;
    }
    rc = xenus_bios_config_validate(&e->draft, &v);
    if (rc != XENUS_BIOS_OK) {
        e->last_result = rc;
        snprintf(e->last_message, sizeof(e->last_message),
                 "Apply blocked: %s", v.count ? v.items[0].text :
                 "invalid configuration");
        if (msg)
            copy_str(msg, msg_size, e->last_message);
        return rc;
    }
    if (e->draft.recovery.backup_before_apply)
        (void)write_previous_backup(e, NULL, 0);
    rc = xenus_bios_config_save(&e->draft, e->store_path);
    if (rc != XENUS_BIOS_OK) {
        e->last_result = rc;
        copy_str(e->last_message, sizeof(e->last_message),
                 "Cannot write the configuration store.");
        if (msg)
            copy_str(msg, msg_size, e->last_message);
        return rc;
    }
    got_shell = 0;
    if (sync_shell) {
        int src = xenus_bios_apply_shell_settings(&e->draft, e->shell_path,
                                                  shell_msg,
                                                  sizeof(shell_msg));

        got_shell = src == XENUS_BIOS_OK;
        if (!got_shell) {
            e->last_result = src;
            snprintf(e->last_message, sizeof(e->last_message),
                     "BIOS store saved, but shell sync failed: %s",
                     shell_msg);
            if (msg)
                copy_str(msg, msg_size, e->last_message);
            return src;
        }
    }
    e->live = e->draft;
    e->dirty = 0;
    e->loaded = 1;
    e->apply_count++;
    e->last_result = XENUS_BIOS_OK;
    snprintf(e->last_message, sizeof(e->last_message),
             "Applied. Driver changes take effect at the next BIOS start%s.",
             got_shell ? " and shell appearance is updated" : "");
    if (msg)
        copy_str(msg, msg_size, e->last_message);
    return XENUS_BIOS_OK;
}

/* -------------------------------------------------------------- recovery -- */

static int backup_name_match(const char *name)
{
    size_t len;

    if (!name || strncmp(name, "bios-", 5) != 0)
        return 0;
    len = strlen(name);
    return len > 10 && strcmp(name + len - 5, ".conf") == 0;
}

int xenus_bios_engine_backup_count(const xenus_bios_engine_t *e)
{
    DIR *d;
    struct dirent *ent;
    int n = 0;

    if (!e)
        return 0;
    d = opendir(e->backup_dir);
    if (!d)
        return 0;
    while ((ent = readdir(d)) != NULL)
        if (backup_name_match(ent->d_name))
            n++;
    closedir(d);
    return n;
}

int xenus_bios_engine_latest_backup(const xenus_bios_engine_t *e,
                                    char *name, size_t name_size)
{
    DIR *d;
    struct dirent *ent;
    char best[128] = "";

    if (!e || !name || name_size == 0)
        return 0;
    d = opendir(e->backup_dir);
    if (!d)
        return 0;
    while ((ent = readdir(d)) != NULL) {
        if (!backup_name_match(ent->d_name))
            continue;
        if (strcmp(ent->d_name, best) > 0)
            copy_str(best, sizeof(best), ent->d_name);
    }
    closedir(d);
    if (!best[0])
        return 0;
    copy_str(name, name_size, best);
    return 1;
}

int xenus_bios_engine_backup(xenus_bios_engine_t *e, char *msg,
                             size_t msg_size)
{
    if (!e)
        return XENUS_BIOS_ERR_ARG;
    if (!e->loaded && access(e->store_path, F_OK) != 0) {
        int rc = xenus_bios_config_save(&e->live, e->store_path);

        if (rc != XENUS_BIOS_OK) {
            if (msg)
                copy_str(msg, msg_size, "Cannot create an initial store.");
            return rc;
        }
        e->loaded = 1;
    }
    {
        int rc = write_previous_backup(e, msg, msg_size);

        if (rc == XENUS_BIOS_OK)
            e->last_result = XENUS_BIOS_OK;
        return rc;
    }
}

int xenus_bios_engine_restore_latest(xenus_bios_engine_t *e, char *msg,
                                     size_t msg_size)
{
    char name[128];
    char path[XENUS_BIOS_PATH_MAX];
    xenus_bios_config_t restored;
    xenus_bios_validation_t v;
    char shell_msg[192];
    int rc;

    if (!e)
        return XENUS_BIOS_ERR_ARG;
    if (!xenus_bios_engine_latest_backup(e, name, sizeof(name))) {
        if (msg)
            copy_str(msg, msg_size, "No recovery backup is available.");
        return XENUS_BIOS_ERR_IO;
    }
    snprintf(path, sizeof(path), "%s/%s", e->backup_dir, name);
    rc = xenus_bios_config_load(&restored, path);
    if (rc < 0) {
        if (msg)
            copy_str(msg, msg_size, "The latest backup cannot be read.");
        return rc;
    }
    rc = xenus_bios_config_validate(&restored, &v);
    if (rc != XENUS_BIOS_OK) {
        if (msg)
            snprintf(msg, msg_size, "Backup rejected: %s",
                     v.count ? v.items[0].text : "invalid");
        return rc;
    }
    if (e->live.recovery.backup_before_apply)
        (void)write_previous_backup(e, NULL, 0);
    rc = xenus_bios_config_save(&restored, e->store_path);
    if (rc != XENUS_BIOS_OK) {
        if (msg)
            copy_str(msg, msg_size, "Cannot write the restored store.");
        return rc;
    }
    (void)xenus_bios_apply_shell_settings(&restored, e->shell_path,
                                          shell_msg, sizeof(shell_msg));
    e->live = restored;
    e->draft = restored;
    e->dirty = 0;
    e->loaded = 1;
    e->last_result = XENUS_BIOS_OK;
    snprintf(e->last_message, sizeof(e->last_message),
             "Restored %s.", name);
    if (msg)
        copy_str(msg, msg_size, e->last_message);
    return XENUS_BIOS_OK;
}

int xenus_bios_engine_factory_reset(xenus_bios_engine_t *e, char *msg,
                                    size_t msg_size)
{
    xenus_bios_config_t defaults;
    char shell_msg[192];
    int rc;

    if (!e)
        return XENUS_BIOS_ERR_ARG;
    if (access(e->store_path, R_OK) == 0 &&
        e->live.recovery.backup_before_apply)
        (void)write_previous_backup(e, NULL, 0);
    xenus_bios_config_defaults(&defaults);
    rc = xenus_bios_config_save(&defaults, e->store_path);
    if (rc != XENUS_BIOS_OK) {
        if (msg)
            copy_str(msg, msg_size, "Cannot write the factory defaults.");
        return rc;
    }
    (void)xenus_bios_apply_shell_settings(&defaults, e->shell_path,
                                          shell_msg, sizeof(shell_msg));
    e->live = defaults;
    e->draft = defaults;
    e->dirty = 0;
    e->loaded = 1;
    e->last_result = XENUS_BIOS_OK;
    copy_str(e->last_message, sizeof(e->last_message),
             "Factory defaults restored. Shell appearance was reset.");
    if (msg)
        copy_str(msg, msg_size, e->last_message);
    return XENUS_BIOS_OK;
}

int xenus_bios_engine_verify(xenus_bios_engine_t *e, char *msg,
                             size_t msg_size)
{
    xenus_bios_config_t disk;
    xenus_bios_validation_t v;
    int rc;

    if (!e)
        return XENUS_BIOS_ERR_ARG;
    rc = xenus_bios_config_load(&disk, e->store_path);
    if (rc < 0) {
        if (msg)
            copy_str(msg, msg_size, "Store is unreadable.");
        return rc;
    }
    if (rc == XENUS_BIOS_DEFAULTS) {
        if (msg)
            copy_str(msg, msg_size,
                     "No store on disk; defaults are staged.");
        return XENUS_BIOS_OK;
    }
    rc = xenus_bios_config_validate(&disk, &v);
    if (rc != XENUS_BIOS_OK) {
        if (msg)
            snprintf(msg, msg_size, "Store has %d blocking issue%s.", v.errors,
                     v.errors == 1 ? "" : "s");
        return rc;
    }
    if (!xenus_bios_config_equal(&disk, &e->live)) {
        if (msg)
            copy_str(msg, msg_size,
                     "Store is valid but differs from the loaded state.");
        return XENUS_BIOS_OK;
    }
    if (msg)
        snprintf(msg, msg_size,
                 "Store verified: version %d, %d backup%s available.",
                 disk.version, xenus_bios_engine_backup_count(e),
                 xenus_bios_engine_backup_count(e) == 1 ? "" : "s");
    return XENUS_BIOS_OK;
}

/* ----------------------------------------------------------- shell sync -- */

#define SHELL_KEY_MAX 8

typedef struct {
    const char *key;
    char value[32];
} shell_key_t;

static int rewrite_file(const char *path, shell_key_t *keys, int nkeys,
                        char *msg, size_t msg_size)
{
    char dir[XENUS_BIOS_PATH_MAX];
    char tmp[XENUS_BIOS_PATH_MAX + 32];
    char line[640];
    FILE *in, *out;
    int used[SHELL_KEY_MAX];
    int i;

    if (!path || !keys || nkeys <= 0 || nkeys > SHELL_KEY_MAX)
        return XENUS_BIOS_ERR_ARG;
    memset(used, 0, sizeof(used));
    if (parent_dir(path, dir, sizeof(dir)) != 0 || mkdir_p(dir) != 0) {
        if (msg)
            copy_str(msg, msg_size, "cannot create the XENUS config directory");
        return XENUS_BIOS_ERR_IO;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    in = fopen(path, "r");
    out = fopen(tmp, "w");
    if (!out) {
        if (in)
            fclose(in);
        if (msg)
            copy_str(msg, msg_size, "cannot write the XENUS config file");
        return XENUS_BIOS_ERR_IO;
    }
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            char copy[640];
            char *s;
            char *eq;
            int matched = -1;

            copy_str(copy, sizeof(copy), line);
            s = trim(copy);
            eq = strchr(s, '=');
            if (eq) {
                *eq = '\0';
                for (i = 0; i < nkeys; i++) {
                    if (strcmp(trim(s), keys[i].key) == 0) {
                        matched = i;
                        break;
                    }
                }
            }
            if (matched >= 0) {
                fprintf(out, "%s=%s\n", keys[matched].key,
                        keys[matched].value);
                used[matched] = 1;
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    } else {
        fprintf(out, "# XENUS configuration (created by XENUS BIOS)\n");
    }
    for (i = 0; i < nkeys; i++) {
        if (!used[i])
            fprintf(out, "%s=%s\n", keys[i].key, keys[i].value);
    }
    if (fclose(out) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        if (msg)
            copy_str(msg, msg_size, "cannot replace the XENUS config file");
        return XENUS_BIOS_ERR_IO;
    }
    if (msg)
        copy_str(msg, msg_size, "shell appearance updated");
    return XENUS_BIOS_OK;
}

int xenus_bios_apply_shell_settings(const xenus_bios_config_t *cfg,
                                    const char *shell_path, char *msg,
                                    size_t msg_size)
{
    shell_key_t keys[5];

    if (!cfg || !shell_path)
        return XENUS_BIOS_ERR_ARG;
    memset(keys, 0, sizeof(keys));
    keys[0].key = "theme";
    copy_str(keys[0].value, sizeof(keys[0].value),
             cfg->system.theme == XENUS_BIOS_THEME_LIGHT ? "light" : "dark");
    keys[1].key = "accent";
    copy_str(keys[1].value, sizeof(keys[1].value),
             cfg->system.accent == XENUS_BIOS_ACCENT_SILVER ? "silver" :
             cfg->system.accent == XENUS_BIOS_ACCENT_GRAPHITE ? "graphite" :
             "blue");
    keys[2].key = "animations";
    snprintf(keys[2].value, sizeof(keys[2].value), "%d",
             cfg->system.animations ? 1 : 0);
    keys[3].key = "show_clock";
    snprintf(keys[3].value, sizeof(keys[3].value), "%d",
             cfg->system.show_clock ? 1 : 0);
    keys[4].key = "focus_follows_mouse";
    snprintf(keys[4].value, sizeof(keys[4].value), "%d",
             cfg->system.focus_follows_mouse ? 1 : 0);
    return rewrite_file(shell_path, keys, 5, msg, msg_size);
}

/* ------------------------------------------------------- runtime helpers -- */

static void load_runtime(xenus_bios_config_t *cfg)
{
    if (xenus_bios_config_load(cfg, NULL) < 0)
        xenus_bios_config_defaults(cfg);
}

int xenus_bios_runtime_ucore_enabled(const char *module)
{
    xenus_bios_config_t cfg;

    if (!module || !*module)
        return 1;
    load_runtime(&cfg);
    if (strcmp(module, "sysinfo") == 0)
        return 1;                      /* mandatory regardless of the file */
    if (strcmp(module, "process") == 0)
        return cfg.ucore.process;
    if (strcmp(module, "applications") == 0)
        return cfg.ucore.applications;
    if (strcmp(module, "hardware") == 0)
        return cfg.ucore.hardware;
    if (strcmp(module, "notifications") == 0)
        return cfg.ucore.notifications;
    return 1;
}

int xenus_bios_runtime_boot_mode(void)
{
    xenus_bios_config_t cfg;

    load_runtime(&cfg);
    return cfg.boot.mode;
}

int xenus_bios_runtime_boot_diagnostic(void)
{
    xenus_bios_config_t cfg;

    load_runtime(&cfg);
    return cfg.boot.diagnostic;
}

int xenus_bios_runtime_boot_delay(void)
{
    xenus_bios_config_t cfg;

    load_runtime(&cfg);
    if (cfg.boot.delay_seconds < 0 || cfg.boot.delay_seconds > 10)
        return 0;
    return cfg.boot.delay_seconds;
}

int xenus_bios_runtime_termux_x11(void)
{
    xenus_bios_config_t cfg;

    load_runtime(&cfg);
    return cfg.android.termux_x11;
}
