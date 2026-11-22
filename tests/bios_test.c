/*
 * XENUS BIOS engine tests - staged configuration, validation, persistence,
 * backup/restore, factory reset and runtime integration helpers.
 *
 * No X server is required.
 */
#define _POSIX_C_SOURCE 200809L

#include <xenus/bios.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static int checks;

#define CHECK(cond, text) do { \
        checks++; \
        if (cond) { \
            printf("  PASS: %s\n", text); \
        } else { \
            failures++; \
            printf("  FAIL: %s\n", text); \
        } \
    } while (0)

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int found = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int main(void)
{
    char tmpl[] = "/tmp/xenus-bios-test.XXXXXX";
    char store[512];
    char shell[512];
    char msg[256];
    char text[4096];
    char *dir;
    xenus_bios_engine_t e;
    xenus_bios_config_t defaults;
    xenus_bios_config_t bad;
    xenus_bios_validation_t v;
    xenus_bios_diff_t diffs[XENUS_BIOS_MAX_DIFF];
    int rc;
    int backups;

    dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "cannot create temporary directory\n");
        return 1;
    }
    if (setenv("XDG_CONFIG_HOME", dir, 1) != 0) {
        fprintf(stderr, "cannot set XDG_CONFIG_HOME\n");
        return 1;
    }

    printf("== XENUS BIOS engine ==\n");
    CHECK(xenus_bios_store_path(store, sizeof(store)) != NULL,
          "store path resolves");
    CHECK(xenus_bios_shell_path(shell, sizeof(shell)) != NULL,
          "shell config path resolves");
    CHECK(strstr(store, "xenus/bios.conf") != NULL,
          "store uses the XENUS config directory");
    CHECK(strstr(shell, "xenus/config") != NULL,
          "shell path stays compatible with XENUS");

    xenus_bios_config_defaults(&defaults);
    CHECK(defaults.system.theme == XENUS_BIOS_THEME_DARK,
          "default theme is dark");
    CHECK(defaults.ucore.sysinfo == 1, "sysinfo is enabled by default");
    CHECK(defaults.boot.mode == XENUS_BIOS_BOOT_NORMAL,
          "default boot mode is normal");

    rc = xenus_bios_engine_open(&e, NULL);
    CHECK(rc == XENUS_BIOS_DEFAULTS, "first open stages defaults");
    CHECK(!xenus_bios_engine_is_dirty(&e), "fresh engine is clean");
    CHECK(xenus_bios_config_equal(&e.live, &e.draft),
          "draft starts as a copy of live");

    e.draft.system.theme = XENUS_BIOS_THEME_LIGHT;
    e.draft.system.animations = 0;
    xenus_bios_engine_touch(&e);
    CHECK(xenus_bios_engine_is_dirty(&e), "change marks the draft dirty");
    CHECK(access(store, F_OK) != 0, "staged change is not persisted");
    CHECK(xenus_bios_config_diff(&e.live, &e.draft, diffs,
                                XENUS_BIOS_MAX_DIFF) == 2,
          "diff reports the two staged fields");
    CHECK(xenus_bios_config_validate(&e.draft, &v) == XENUS_BIOS_OK,
          "default draft validates");
    CHECK(v.errors == 0, "validation has no blocking errors");

    rc = xenus_bios_engine_apply(&e, 1, msg, sizeof(msg));
    CHECK(rc == XENUS_BIOS_OK, "apply writes a valid draft");
    CHECK(access(store, F_OK) == 0, "store exists after apply");
    CHECK(access(shell, F_OK) == 0, "shell config exists after sync");
    CHECK(file_contains(store, "theme=light"), "store contains new theme");
    CHECK(file_contains(store, "animations=0"),
          "store contains new animation flag");
    CHECK(file_contains(shell, "theme=light"),
          "shell config contains the managed theme key");
    CHECK(file_contains(shell, "animations=0"),
          "shell config contains the managed animation key");
    CHECK(!xenus_bios_engine_is_dirty(&e), "apply clears the dirty state");
    CHECK(xenus_bios_config_equal(&e.live, &e.draft),
          "apply promotes the draft to live");

    e.draft.system.accent = XENUS_BIOS_ACCENT_GRAPHITE;
    xenus_bios_engine_touch(&e);
    rc = xenus_bios_engine_apply(&e, 1, msg, sizeof(msg));
    CHECK(rc == XENUS_BIOS_OK, "second apply succeeds");
    backups = xenus_bios_engine_backup_count(&e);
    CHECK(backups >= 1, "apply keeps a recovery backup of the previous store");

    rc = xenus_bios_engine_restore_latest(&e, msg, sizeof(msg));
    CHECK(rc == XENUS_BIOS_OK, "latest backup restores");
    CHECK(e.live.system.accent == XENUS_BIOS_ACCENT_BLUE,
          "restore brought back the previous accent");
    CHECK(file_contains(store, "accent=blue"),
          "restored store was written to disk");

    backups = xenus_bios_engine_backup_count(&e);
    rc = xenus_bios_engine_backup(&e, msg, sizeof(msg));
    CHECK(rc == XENUS_BIOS_OK, "manual backup succeeds");
    CHECK(xenus_bios_engine_backup_count(&e) == backups + 1,
          "manual backup increments the recovery count");

    rc = xenus_bios_engine_verify(&e, msg, sizeof(msg));
    CHECK(rc == XENUS_BIOS_OK, "store verification succeeds");

    rc = xenus_bios_engine_factory_reset(&e, msg, sizeof(msg));
    CHECK(rc == XENUS_BIOS_OK, "factory reset succeeds");
    CHECK(e.live.system.theme == XENUS_BIOS_THEME_DARK,
          "factory reset restores defaults");
    CHECK(file_contains(shell, "theme=dark"),
          "factory reset syncs the managed shell keys");
    CHECK(!xenus_bios_engine_is_dirty(&e), "factory reset leaves a clean engine");

    e.draft.boot.mode = XENUS_BIOS_BOOT_SAFE;
    e.draft.ucore.process = 0;
    e.draft.android.termux_x11 = 0;
    xenus_bios_engine_touch(&e);
    rc = xenus_bios_engine_apply(&e, 1, msg, sizeof(msg));
    CHECK(rc == XENUS_BIOS_OK, "safe-mode draft applies");
    CHECK(xenus_bios_runtime_boot_mode() == XENUS_BIOS_BOOT_SAFE,
          "runtime boot mode reads the applied value");
    CHECK(xenus_bios_runtime_ucore_enabled("process") == 0,
          "runtime UCore switch reads the applied value");
    CHECK(xenus_bios_runtime_ucore_enabled("sysinfo") == 1,
          "mandatory sysinfo stays enabled");
    CHECK(xenus_bios_runtime_termux_x11() == 0,
          "runtime Termux:X11 preference is applied");

    bad = e.live;
    bad.ucore.sysinfo = 0;
    CHECK(xenus_bios_config_validate(&bad, &v) == XENUS_BIOS_ERR_INVALID,
          "disabling sysinfo is rejected");
    CHECK(v.errors >= 1, "sysinfo validation reports an error");

    bad = e.live;
    bad.drivers.display = 0;
    CHECK(xenus_bios_config_validate(&bad, &v) == XENUS_BIOS_ERR_INVALID,
          "disabling the required display bridge is rejected");

    bad = e.live;
    bad.boot.delay_seconds = 11;
    CHECK(xenus_bios_config_validate(&bad, &v) == XENUS_BIOS_ERR_INVALID,
          "out-of-range boot delay is rejected");

    bad = e.live;
    rc = xenus_bios_config_text(&e.live, text, sizeof(text));
    CHECK(rc == XENUS_BIOS_OK, "configuration text serialises");
    CHECK(strstr(text, "[system]") != NULL &&
          strstr(text, "[drivers]") != NULL &&
          strstr(text, "[ucore]") != NULL &&
          strstr(text, "[android]") != NULL &&
          strstr(text, "[boot]") != NULL &&
          strstr(text, "[recovery]") != NULL,
          "serialised store contains every section");

    xenus_bios_engine_discard(&e);
    CHECK(!xenus_bios_engine_is_dirty(&e), "discard leaves a clean engine");

    {
        char cmd[600];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        {
            int cleanup_rc = system(cmd);

            (void)cleanup_rc;   /* best-effort cleanup */
        }
    }

    printf("\n%d checks, %d failure%s\n", checks, failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
