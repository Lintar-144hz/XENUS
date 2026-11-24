/*
 * Unit tests for the XENUS Config Manager (Phase 2).
 *
 * Build: make tests/config_manager_test
 * Run:   ./tests/config_manager_test
 */
#include <xenus/config_manager.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int passed;
static int failed;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            passed++;                                                          \
        } else {                                                               \
            failed++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

#define CHECK_INT(actual, expected)                                            \
    do {                                                                       \
        long a_ = (long)(actual), e_ = (long)(expected);                       \
        if (a_ == e_) {                                                        \
            passed++;                                                          \
        } else {                                                               \
            failed++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s == %ld, want %ld\n", __FILE__,     \
                    __LINE__, #actual, a_, e_);                                \
        }                                                                      \
    } while (0)

#define CHECK_STR(actual, expected)                                            \
    do {                                                                       \
        const char *a_ = (actual), *e_ = (expected);                           \
        if ((!a_ && !e_) || (a_ && e_ && strcmp(a_, e_) == 0)) {               \
            passed++;                                                          \
        } else {                                                               \
            failed++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s == '%s', want '%s'\n", __FILE__,   \
                    __LINE__, #actual, a_ ? a_ : "(null)",                     \
                    e_ ? e_ : "(null)");                                       \
        }                                                                      \
    } while (0)

static const char *const theme_choices[] = { "dark", "light", NULL };

static const xenus_cfg_spec_t base_specs[] = {
    { "ui.theme", XENUS_CFG_STRING, "dark", 0, 0, 0, theme_choices },
    { "ui.width", XENUS_CFG_INT, "820", 640, 3840, 0, NULL },
    { "ui.animations", XENUS_CFG_BOOL, "1", 0, 0, 0, NULL },
    { "ui.name", XENUS_CFG_STRING, "xenus", 0, 0, 32, NULL },
};

static void setup(xenus_config_manager_t *m)
{
    xenus_cfg_init(m);
    CHECK_INT(xenus_cfg_define_all(m, base_specs,
                                   sizeof(base_specs) / sizeof(base_specs[0])),
              XENUS_OK);
    CHECK_INT(m->count, 4);
}

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (!f)
        return -1;
    fputs(text, f);
    return fclose(f);
}

static char *slurp(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f)
        return NULL;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static int count_temp_files(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    int n = 0;

    if (!d)
        return -1;
    while ((ent = readdir(d)) != NULL) {
        if (strstr(ent->d_name, ".tmp."))
            n++;
    }
    closedir(d);
    return n;
}

/* ------------------------------------------------------------ defaults -- */

static void test_defaults_and_setters(void)
{
    xenus_config_manager_t m;
    char buf[64];
    int n;

    setup(&m);
    CHECK_STR(xenus_cfg_get(&m, "ui.theme"), "dark");
    CHECK_INT(xenus_cfg_source(&m, "ui.theme"), XENUS_CFG_SOURCE_DEFAULT);
    CHECK_INT(xenus_cfg_get_int(&m, "ui.width", &n), XENUS_OK);
    CHECK_INT(n, 820);
    CHECK_INT(xenus_cfg_get_bool(&m, "ui.animations", &n), XENUS_OK);
    CHECK_INT(n, 1);
    CHECK_INT(xenus_cfg_get_string(&m, "ui.name", buf, sizeof(buf)), XENUS_OK);
    CHECK_STR(buf, "xenus");
    CHECK_STR(xenus_cfg_get(&m, "missing.key"), NULL);
    CHECK_INT(xenus_cfg_source(&m, "missing.key"), XENUS_CFG_SOURCE_NONE);
    CHECK_INT(xenus_cfg_get_string(&m, "ui.theme", buf, 3), XENUS_ERR_ARG);
    CHECK_STR(xenus_cfg_type_name(XENUS_CFG_STRING), "string");
    CHECK_STR(xenus_cfg_type_name(XENUS_CFG_INT), "int");
    CHECK_STR(xenus_cfg_type_name(XENUS_CFG_BOOL), "bool");

    CHECK_INT(xenus_cfg_set_string(&m, "ui.theme", "light"), XENUS_OK);
    CHECK_STR(xenus_cfg_get(&m, "ui.theme"), "light");
    CHECK_INT(xenus_cfg_source(&m, "ui.theme"), XENUS_CFG_SOURCE_USER);

    CHECK_INT(xenus_cfg_set_string(&m, "ui.theme", "neon"), XENUS_ERR_ARG);
    CHECK_STR(xenus_cfg_get(&m, "ui.theme"), "light");
    CHECK(strstr(xenus_cfg_last_error(&m), "ui.theme") != NULL);

    CHECK_INT(xenus_cfg_set_int(&m, "ui.width", 1000), XENUS_OK);
    CHECK_INT(xenus_cfg_get_int(&m, "ui.width", &n), XENUS_OK);
    CHECK_INT(n, 1000);
    CHECK_INT(xenus_cfg_set_int(&m, "ui.width", 5000), XENUS_ERR_ARG);
    CHECK_INT(xenus_cfg_set_int(&m, "ui.width", 639), XENUS_ERR_ARG);
    CHECK_INT(xenus_cfg_get_int(&m, "ui.width", &n), XENUS_OK);
    CHECK_INT(n, 1000);

    CHECK_INT(xenus_cfg_set_bool(&m, "ui.animations", 0), XENUS_OK);
    CHECK_INT(xenus_cfg_get_bool(&m, "ui.animations", &n), XENUS_OK);
    CHECK_INT(n, 0);

    CHECK_INT(xenus_cfg_set_string(&m, "ui.name", "a-very-long-name-that-is-not-allowed"),
              XENUS_ERR_ARG);
    CHECK_INT(xenus_cfg_set_int(&m, "ui.theme", 1), XENUS_ERR_ARG);
    CHECK_INT(xenus_cfg_set_bool(&m, "ui.missing", 1), XENUS_ERR_NOT_FOUND);
    CHECK_INT(xenus_cfg_reset_user(&m, "ui.theme"), XENUS_OK);
    CHECK_INT(xenus_cfg_source(&m, "ui.theme"), XENUS_CFG_SOURCE_DEFAULT);
    CHECK_INT(xenus_cfg_reset_user(&m, "ui.missing"), XENUS_ERR_NOT_FOUND);
    CHECK_INT(xenus_cfg_validate(&m), XENUS_OK);
}

static void test_define_errors(void)
{
    xenus_config_manager_t m;
    xenus_cfg_spec_t bad;
    xenus_cfg_spec_t pair[2];

    setup(&m);
    CHECK_INT(xenus_cfg_define(&m, &base_specs[0]), XENUS_ERR_ARG);
    CHECK_INT(m.count, 4);

    bad = (xenus_cfg_spec_t){ "api_key", XENUS_CFG_STRING, "x", 0, 0, 0, NULL };
    CHECK_INT(xenus_cfg_define(&m, &bad), XENUS_ERR_ARG);
    CHECK(xenus_cfg_key_is_secret("server.password"));
    CHECK(xenus_cfg_key_is_secret("AUTH_TOKEN"));
    CHECK(!xenus_cfg_key_is_secret("ui.theme"));

    bad = (xenus_cfg_spec_t){ "bad key", XENUS_CFG_STRING, "x", 0, 0, 0, NULL };
    CHECK_INT(xenus_cfg_define(&m, &bad), XENUS_ERR_ARG);
    bad = (xenus_cfg_spec_t){ "bad.key", XENUS_CFG_INT, "nope", 0, 10, 0, NULL };
    CHECK_INT(xenus_cfg_define(&m, &bad), XENUS_ERR_ARG);
    bad = (xenus_cfg_spec_t){ "bad.key", XENUS_CFG_INT, "5", 10, 0, 0, NULL };
    CHECK_INT(xenus_cfg_define(&m, &bad), XENUS_ERR_ARG);
    bad = (xenus_cfg_spec_t){ "bad.key", XENUS_CFG_INT, "5", 0, 10, 0,
                              theme_choices };
    CHECK_INT(xenus_cfg_define(&m, &bad), XENUS_ERR_ARG);

    /* define_all rolls back the entries added by the failed call. */
    pair[0] = (xenus_cfg_spec_t){ "extra.key", XENUS_CFG_STRING, "v", 0, 0, 0,
                                  NULL };
    pair[1] = (xenus_cfg_spec_t){ "client_secret", XENUS_CFG_STRING, "v", 0, 0,
                                  0, NULL };
    CHECK_INT(xenus_cfg_define_all(&m, pair, 2), XENUS_ERR_ARG);
    CHECK_INT(m.count, 4);
    CHECK_STR(xenus_cfg_get(&m, "extra.key"), NULL);

    /* A full schema reports XENUS_ERR_FULL. */
    {
        xenus_cfg_spec_t spec;
        int i;

        for (i = 0; i < XENUS_CFG_MAX_KEYS - 4; i++) {
            char key[XENUS_CFG_KEY_LEN];

            snprintf(key, sizeof(key), "fill.%d", i);
            spec = (xenus_cfg_spec_t){ key, XENUS_CFG_INT, "0", 0, 100, 0,
                                       NULL };
            CHECK_INT(xenus_cfg_define(&m, &spec), XENUS_OK);
        }
        spec = (xenus_cfg_spec_t){ "one.too.many", XENUS_CFG_INT, "0", 0, 100,
                                   0, NULL };
        CHECK_INT(xenus_cfg_define(&m, &spec), XENUS_ERR_FULL);
    }
}

/* ---------------------------------------------------------- load layers -- */

static void test_load_layers(const char *dir)
{
    xenus_config_manager_t m;
    char sys[512], user[512];
    int n;

    snprintf(sys, sizeof(sys), "%s/system.conf", dir);
    snprintf(user, sizeof(user), "%s/user.conf", dir);

    CHECK_INT(write_file(sys,
        "# system\n"
        "ui.theme=light\n"
        "ui.width=900\n"
        "ui.animations=yes\n"
        "unknown.key=1\n"
        "not-a-kv-line\n"), 0);
    CHECK_INT(write_file(user,
        "ui.width=1000\n"
        "ui.name=alice\n"
        "ui.theme=neon\n"), 0);

    setup(&m);
    CHECK_INT(xenus_cfg_set_paths(&m, sys, user), XENUS_OK);
    CHECK_INT(xenus_cfg_load(&m), XENUS_ERR);
    CHECK(strstr(xenus_cfg_last_error(&m), "system.conf") != NULL);

    CHECK_STR(xenus_cfg_get(&m, "ui.theme"), "light");
    CHECK_INT(xenus_cfg_source(&m, "ui.theme"), XENUS_CFG_SOURCE_SYSTEM);
    CHECK_INT(xenus_cfg_get_int(&m, "ui.width", &n), XENUS_OK);
    CHECK_INT(n, 1000);
    CHECK_INT(xenus_cfg_source(&m, "ui.width"), XENUS_CFG_SOURCE_USER);
    CHECK_STR(xenus_cfg_get(&m, "ui.name"), "alice");
    CHECK_INT(xenus_cfg_get_bool(&m, "ui.animations", &n), XENUS_OK);
    CHECK_INT(n, 1);
    CHECK_INT(xenus_cfg_validate(&m), XENUS_OK);

    /* A secret line and an invalid value are skipped, not stored. */
    CHECK_INT(write_file(user,
        "ui.width=99999\n"
        "server.password=hunter2\n"
        "ui.name=bob\n"), 0);
    CHECK_INT(xenus_cfg_load(&m), XENUS_ERR);
    CHECK_INT(xenus_cfg_get_int(&m, "ui.width", &n), XENUS_OK);
    CHECK_INT(n, 900);
    CHECK_INT(xenus_cfg_source(&m, "ui.width"), XENUS_CFG_SOURCE_SYSTEM);
    CHECK_STR(xenus_cfg_get(&m, "ui.name"), "bob");
    CHECK(xenus_cfg_last_error(&m)[0] != '\0');

    /* Reload clears stale layers and accepts clean files. */
    CHECK_INT(write_file(sys, "ui.width=700\n"), 0);
    CHECK_INT(write_file(user, "ui.name=carol\n"), 0);
    CHECK_INT(xenus_cfg_load(&m), XENUS_OK);
    CHECK_STR(xenus_cfg_last_error(&m), "");
    CHECK_INT(xenus_cfg_get_int(&m, "ui.width", &n), XENUS_OK);
    CHECK_INT(n, 700);
    CHECK_INT(xenus_cfg_source(&m, "ui.width"), XENUS_CFG_SOURCE_SYSTEM);
    CHECK_INT(xenus_cfg_source(&m, "ui.theme"), XENUS_CFG_SOURCE_DEFAULT);

    /* Missing files are normal and leave defaults in place. */
    snprintf(sys, sizeof(sys), "%s/does-not-exist", dir);
    snprintf(user, sizeof(user), "%s/also-missing", dir);
    CHECK_INT(xenus_cfg_set_paths(&m, sys, user), XENUS_OK);
    CHECK_INT(xenus_cfg_load(&m), XENUS_OK);
    CHECK_STR(xenus_cfg_get(&m, "ui.theme"), "dark");
    CHECK_INT(xenus_cfg_get_int(&m, "ui.width", &n), XENUS_OK);
    CHECK_INT(n, 820);
}

/* -------------------------------------------------------------- saving -- */

static void test_save(const char *dir)
{
    xenus_config_manager_t m, reloaded;
    char sys[512], user[512], nested[512], *body;
    struct stat st;
    int n;

    snprintf(sys, sizeof(sys), "%s/save-system.conf", dir);
    snprintf(user, sizeof(user), "%s/nested/dir/save-user.conf", dir);
    CHECK_INT(write_file(sys, "ui.width=700\n"), 0);

    setup(&m);
    CHECK_INT(xenus_cfg_set_paths(&m, sys, user), XENUS_OK);
    CHECK_INT(xenus_cfg_load(&m), XENUS_OK);
    CHECK_INT(xenus_cfg_set_string(&m, "ui.theme", "light"), XENUS_OK);
    CHECK_INT(xenus_cfg_set_int(&m, "ui.width", 1100), XENUS_OK);
    CHECK_INT(xenus_cfg_set_bool(&m, "ui.animations", 0), XENUS_OK);
    /* ui.name stays a default and must not be written. */
    CHECK_INT(xenus_cfg_save_user(&m), XENUS_OK);
    CHECK_INT(stat(user, &st), 0);
    CHECK_INT(st.st_mode & 0777, 0600);

    body = slurp(user);
    CHECK(body != NULL);
    if (body) {
        CHECK(strstr(body, "ui.theme=light") != NULL);
        CHECK(strstr(body, "ui.width=1100") != NULL);
        CHECK(strstr(body, "ui.animations=0") != NULL);
        CHECK(strstr(body, "ui.name=") == NULL);
        CHECK(strstr(body, "ui.width=700") == NULL);
    }

    /* No temporary files remain behind. */
    snprintf(nested, sizeof(nested), "%s/nested/dir", dir);
    CHECK_INT(count_temp_files(nested), 0);

    xenus_cfg_init(&reloaded);
    CHECK_INT(xenus_cfg_define_all(&reloaded, base_specs,
                                   sizeof(base_specs) / sizeof(base_specs[0])),
              XENUS_OK);
    CHECK_INT(xenus_cfg_set_paths(&reloaded, sys, user), XENUS_OK);
    CHECK_INT(xenus_cfg_load(&reloaded), XENUS_OK);
    CHECK_STR(xenus_cfg_get(&reloaded, "ui.theme"), "light");
    CHECK_INT(xenus_cfg_source(&reloaded, "ui.theme"), XENUS_CFG_SOURCE_USER);
    CHECK_INT(xenus_cfg_get_int(&reloaded, "ui.width", &n), XENUS_OK);
    CHECK_INT(n, 1100);
    CHECK_INT(xenus_cfg_get_bool(&reloaded, "ui.animations", &n), XENUS_OK);
    CHECK_INT(n, 0);
    CHECK_STR(xenus_cfg_get(&reloaded, "ui.name"), "xenus");
    CHECK_INT(xenus_cfg_source(&reloaded, "ui.name"), XENUS_CFG_SOURCE_DEFAULT);

    /* Resetting a user key then saving removes the line. */
    CHECK_INT(xenus_cfg_reset_user(&m, "ui.width"), XENUS_OK);
    CHECK_INT(xenus_cfg_save_user(&m), XENUS_OK);
    body = slurp(user);
    CHECK(body != NULL);
    if (body) {
        CHECK(strstr(body, "ui.width=") == NULL);
        CHECK(strstr(body, "ui.theme=light") != NULL);
    }
    CHECK_INT(xenus_cfg_set_paths(&m, NULL, NULL), XENUS_OK);
    CHECK_INT(xenus_cfg_save_user(&m), XENUS_ERR_ARG);
}

static void test_corrupt_save(const char *dir)
{
    xenus_config_manager_t m;
    char user[512];

    snprintf(user, sizeof(user), "%s/corrupt-user.conf", dir);
    setup(&m);
    CHECK_INT(xenus_cfg_set_paths(&m, NULL, user), XENUS_OK);
    m.entries[1].has_user = 1;
    snprintf(m.entries[1].user_value, sizeof(m.entries[1].user_value), "nope");
    CHECK_INT(xenus_cfg_validate(&m), XENUS_ERR);
    CHECK_INT(xenus_cfg_save_user(&m), XENUS_ERR);
    CHECK_INT(access(user, F_OK), -1);

    setup(&m);
    CHECK_INT(xenus_cfg_set_paths(&m, NULL, user), XENUS_OK);
    snprintf(m.entries[0].key, sizeof(m.entries[0].key), "account.password");
    CHECK_INT(xenus_cfg_validate(&m), XENUS_ERR);
    CHECK_INT(xenus_cfg_save_user(&m), XENUS_ERR);
    CHECK_INT(access(user, F_OK), -1);
}

/* ------------------------------------------------------- shared config -- */

static void test_ignore_unknown(void)
{
    xenus_config_manager_t m;
    char user[512];

    snprintf(user, sizeof(user), "/tmp/xenus-cfg-shared-%ld.conf",
             (long)getpid());
    CHECK_INT(write_file(user,
        "# shared shell config\n"
        "menu1=Terminal|@terminal\n"
        "menu2=Files|@files\n"
        "ui.theme=light\n"
        "server.password=hunter2\n"), 0);

    setup(&m);
    CHECK_INT(xenus_cfg_set_paths(&m, NULL, user), XENUS_OK);
    CHECK_INT(xenus_cfg_load(&m), XENUS_ERR);
    CHECK(xenus_cfg_last_error(&m)[0] != '\0');

    xenus_cfg_set_ignore_unknown(&m, 1);
    CHECK_INT(xenus_cfg_load(&m), XENUS_OK);
    CHECK_STR(xenus_cfg_get(&m, "ui.theme"), "light");
    CHECK_INT(xenus_cfg_source(&m, "ui.theme"), XENUS_CFG_SOURCE_USER);

    unlink(user);
}

static void test_bool_words(void)
{
    xenus_config_manager_t m;
    char user[512];
    const char *values[] = { "light", "dark", "on", "off", "yes", "no" };
    const int expected[] = { 1, 0, 1, 0, 1, 0 };
    size_t i;
    int n;

    snprintf(user, sizeof(user), "/tmp/xenus-cfg-bool-%ld.conf",
             (long)getpid());
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char text[128];

        snprintf(text, sizeof(text), "ui.animations=%s\n", values[i]);
        CHECK_INT(write_file(user, text), 0);
        setup(&m);
        CHECK_INT(xenus_cfg_set_paths(&m, NULL, user), XENUS_OK);
        CHECK_INT(xenus_cfg_load(&m), XENUS_OK);
        CHECK_INT(xenus_cfg_get_bool(&m, "ui.animations", &n), XENUS_OK);
        CHECK_INT(n, expected[i]);
    }
    unlink(user);
}

static void test_shared_merge(const char *dir)
{
    xenus_config_manager_t m, reloaded;
    char user[512], *body;
    struct stat st;
    int n;

    snprintf(user, sizeof(user), "%s/shared-user.conf", dir);
    CHECK_INT(write_file(user,
        "# XENUS user config\n"
        "menu1=Terminal|@terminal\n"
        "menu2=Files|@files\n"
        "ui.theme=dark\n"
        "ui.width=900\n"
        "ui.width=999\n"
        "future.key=keep-me\n"
        "# trailing comment\n"), 0);

    setup(&m);
    CHECK_INT(xenus_cfg_set_paths(&m, NULL, user), XENUS_OK);
    xenus_cfg_set_ignore_unknown(&m, 1);
    CHECK_INT(xenus_cfg_load(&m), XENUS_OK);
    CHECK_INT(xenus_cfg_get_int(&m, "ui.width", &n), XENUS_OK);
    CHECK_INT(n, 999);              /* last duplicate wins on load */

    CHECK_INT(xenus_cfg_set_int(&m, "ui.width", 1200), XENUS_OK);
    CHECK_INT(xenus_cfg_set_string(&m, "ui.theme", "light"), XENUS_OK);
    CHECK_INT(xenus_cfg_save_user_merge(&m), XENUS_OK);
    CHECK_INT(stat(user, &st), 0);
    CHECK_INT(st.st_mode & 0777, 0600);

    body = slurp(user);
    CHECK(body != NULL);
    if (body) {
        CHECK(strstr(body, "# XENUS user config") != NULL);
        CHECK(strstr(body, "menu1=Terminal|@terminal") != NULL);
        CHECK(strstr(body, "menu2=Files|@files") != NULL);
        CHECK(strstr(body, "future.key=keep-me") != NULL);
        CHECK(strstr(body, "# trailing comment") != NULL);
        CHECK(strstr(body, "ui.theme=light") != NULL);
        CHECK(strstr(body, "ui.width=1200") != NULL);
        CHECK(strstr(body, "ui.theme=dark") == NULL);
        CHECK(strstr(body, "ui.width=900") == NULL);
        CHECK(strstr(body, "ui.width=999") == NULL);
    }

    xenus_cfg_init(&reloaded);
    CHECK_INT(xenus_cfg_define_all(&reloaded, base_specs,
                                   sizeof(base_specs) / sizeof(base_specs[0])),
              XENUS_OK);
    xenus_cfg_set_ignore_unknown(&reloaded, 1);
    CHECK_INT(xenus_cfg_set_paths(&reloaded, NULL, user), XENUS_OK);
    CHECK_INT(xenus_cfg_load(&reloaded), XENUS_OK);
    CHECK_STR(xenus_cfg_get(&reloaded, "ui.theme"), "light");
    CHECK_INT(xenus_cfg_source(&reloaded, "ui.theme"), XENUS_CFG_SOURCE_USER);
    CHECK_INT(xenus_cfg_get_int(&reloaded, "ui.width", &n), XENUS_OK);
    CHECK_INT(n, 1200);

    /* A managed key with no user override is removed by a merge save. */
    CHECK_INT(xenus_cfg_reset_user(&m, "ui.width"), XENUS_OK);
    CHECK_INT(xenus_cfg_save_user_merge(&m), XENUS_OK);
    body = slurp(user);
    CHECK(body != NULL);
    if (body) {
        CHECK(strstr(body, "ui.width=") == NULL);
        CHECK(strstr(body, "menu1=Terminal|@terminal") != NULL);
        CHECK(strstr(body, "ui.theme=light") != NULL);
    }
}

/* ------------------------------------------------------------- env paths -- */

static void test_default_paths(void)
{
    xenus_config_manager_t m;
    char saved_sys[512], saved_user[512], saved_xdg[512], saved_home[512];
    const char *v;
    int had_sys, had_user, had_xdg, had_home;

    v = getenv("XENUS_SYSTEM_CONFIG");
    had_sys = v != NULL;
    snprintf(saved_sys, sizeof(saved_sys), "%s", v ? v : "");
    v = getenv("XENUS_CONFIG");
    had_user = v != NULL;
    snprintf(saved_user, sizeof(saved_user), "%s", v ? v : "");
    v = getenv("XDG_CONFIG_HOME");
    had_xdg = v != NULL;
    snprintf(saved_xdg, sizeof(saved_xdg), "%s", v ? v : "");
    v = getenv("HOME");
    had_home = v != NULL;
    snprintf(saved_home, sizeof(saved_home), "%s", v ? v : "");

    setenv("XENUS_SYSTEM_CONFIG", "/tmp/xenus-sys-test", 1);
    setenv("XENUS_CONFIG", "/tmp/xenus-user-test", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/xdg-test", 1);
    setenv("HOME", "/tmp/home-test", 1);

    xenus_cfg_init(&m);
    CHECK_INT(xenus_cfg_default_paths(&m), XENUS_OK);
    CHECK_STR(m.system_path, "/tmp/xenus-sys-test");
    CHECK_STR(m.user_path, "/tmp/xenus-user-test");

    unsetenv("XENUS_CONFIG");
    CHECK_INT(xenus_cfg_default_paths(&m), XENUS_OK);
    CHECK_STR(m.user_path, "/tmp/xdg-test/xenus/config");

    unsetenv("XDG_CONFIG_HOME");
    CHECK_INT(xenus_cfg_default_paths(&m), XENUS_OK);
    CHECK_STR(m.user_path, "/tmp/home-test/.config/xenus/config");

    unsetenv("XENUS_SYSTEM_CONFIG");
    CHECK_INT(xenus_cfg_default_paths(&m), XENUS_OK);
    CHECK_STR(m.system_path, "/etc/xenus/config");

    if (had_sys) setenv("XENUS_SYSTEM_CONFIG", saved_sys, 1);
    else unsetenv("XENUS_SYSTEM_CONFIG");
    if (had_user) setenv("XENUS_CONFIG", saved_user, 1);
    else unsetenv("XENUS_CONFIG");
    if (had_xdg) setenv("XDG_CONFIG_HOME", saved_xdg, 1);
    else unsetenv("XDG_CONFIG_HOME");
    if (had_home) setenv("HOME", saved_home, 1);
    else unsetenv("HOME");
}

static void test_path_limits(void)
{
    xenus_config_manager_t m;
    char long_path[700];

    setup(&m);
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    CHECK_INT(xenus_cfg_set_paths(&m, long_path, NULL), XENUS_ERR_ARG);
    CHECK_INT(xenus_cfg_set_paths(&m, "/tmp/a", long_path), XENUS_ERR_ARG);
    CHECK_INT(xenus_cfg_set_paths(&m, "/tmp/a", "/tmp/b"), XENUS_OK);
    CHECK_STR(m.system_path, "/tmp/a");
    CHECK_STR(m.user_path, "/tmp/b");
}

int main(void)
{
    char dir[] = "/tmp/xenus-cfg-test.XXXXXX";

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }

    test_defaults_and_setters();
    test_define_errors();
    test_load_layers(dir);
    test_path_limits();
    test_default_paths();
    test_save(dir);
    test_ignore_unknown();
    test_bool_words();
    test_shared_merge(dir);
    test_corrupt_save(dir);

    /* best-effort cleanup */
    {
        char cmd[400];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0)
            fprintf(stderr, "warning: could not remove %s\n", dir);
    }

    printf("config_manager_test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
