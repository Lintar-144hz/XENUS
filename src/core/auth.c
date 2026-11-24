#define _GNU_SOURCE
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xenus/ucore.h>

#define AUTH_FILE ".auth"
#define AUTH_HASH_SIZE 256
#define AUTH_MAX_FAILURES 5

static struct {
    int initialized;
    int enabled;
    int locked;
    int failures;
    char path[512];
} auth;

static int auth_path(char *out, size_t size)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return -1;

    int n = snprintf(out, size, "%s/.config/xenus/%s", home, AUTH_FILE);
    return n > 0 && (size_t)n < size ? 0 : -1;
}

static int auth_read(char *hash, size_t size)
{
    FILE *f = fopen(auth.path, "r");
    if (!f)
        return errno == ENOENT ? 0 : -1;

    int ok = fgets(hash, (int)size, f) != NULL;
    fclose(f);
    if (!ok)
        return -1;

    hash[strcspn(hash, "\r\n")] = '\0';
    return hash[0] ? 1 : -1;
}

static int auth_write_all(int fd, const char *data, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        ssize_t written = write(fd, data + offset, len - offset);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (written == 0)
            return -1;

        offset += (size_t)written;
    }

    return 0;
}

static int auth_write_file(const char *path, const char *value)
{
    char tmp[600];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    if (n < 0 || (size_t)n >= sizeof(tmp))
        return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;

    size_t len = strlen(value);
    int ok = auth_write_all(fd, value, len) == 0 &&
             fchmod(fd, 0600) == 0 &&
             fsync(fd) == 0;

    if (close(fd) != 0)
        ok = 0;

    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }

    return 0;
}

static int auth_write(const char *hash)
{
    return auth_write_file(auth.path, hash);
}


int xenus_auth_init(void)
{
    char hash[AUTH_HASH_SIZE];
    struct stat st;

    memset(&auth, 0, sizeof(auth));
    if (auth_path(auth.path, sizeof(auth.path)) != 0)
        return -1;

    char dir[sizeof(auth.path)];
    snprintf(dir, sizeof(dir), "%s", auth.path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return -1;
    *slash = '\0';

    const char *home = getenv("HOME");
    char config_dir[sizeof(auth.path)];

    int n = snprintf(config_dir, sizeof(config_dir),
                     "%s/.config", home);
    if (n < 0 || (size_t)n >= sizeof(config_dir))
        return -1;

    if (mkdir(config_dir, 0700) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return -1;

    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;

    int result = auth_read(hash, sizeof(hash));
    if (result < 0)
        return -1;

    auth.enabled = result == 1;
    auth.locked = auth.enabled;
    auth.initialized = 1;
    return 0;
}

int xenus_auth_enabled(void)
{
    return auth.initialized && auth.enabled;
}

int xenus_auth_locked(void)
{
    return auth.initialized && auth.locked;
}

int xenus_auth_set_password(const char *password)
{
    if (!auth.initialized || !password || strlen(password) < 8)
        return -1;

    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    struct crypt_data data = {0};

    if (!crypt_gensalt_rn("$y$", 0, NULL, 0, salt, sizeof(salt)))
        return -1;

    char *hash = crypt_r(password, salt, &data);
    if (!hash || strncmp(hash, "$y$", 3) != 0)
        return -1;

    if (auth_write(hash) != 0)
        return -1;

    auth.enabled = 1;
    auth.locked = 0;
    auth.failures = 0;
    return 0;
}

int xenus_auth_verify(const char *password)
{
    if (!auth.initialized || !auth.enabled || !password)
        return -1;

    if (auth.failures >= AUTH_MAX_FAILURES)
        return -1;

    char stored[AUTH_HASH_SIZE];
    int result = auth_read(stored, sizeof(stored));
    if (result != 1)
        return -1;

    struct crypt_data data = {0};
    char *computed = crypt_r(password, stored, &data);
    if (computed && strcmp(computed, stored) == 0) {
        auth.failures = 0;
        auth.locked = 0;
        return 0;
    }

    auth.failures++;
    return -1;
}

int xenus_auth_lock(void)
{
    if (!auth.initialized || !auth.enabled)
        return -1;
    auth.locked = 1;
    return 0;
}

int xenus_auth_disable(const char *password)
{
    if (!auth.initialized || !auth.enabled ||
        xenus_auth_verify(password) != 0)
        return -1;

    if (unlink(auth.path) != 0)
        return -1;

    auth.enabled = 0;
    auth.locked = 0;
    auth.failures = 0;
    return 0;
}


int xenus_auth_change_password(const char *old_password,
                               const char *new_password)
{
    if (!auth.initialized || !auth.enabled ||
        !old_password || !new_password ||
        strlen(new_password) < 8)
        return -1;

    if (xenus_auth_verify(old_password) != 0)
        return -1;

    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    struct crypt_data data = {0};

    if (!crypt_gensalt_rn("$y$", 0, NULL, 0, salt, sizeof(salt)))
        return -1;

    char *hash = crypt_r(new_password, salt, &data);
    if (!hash || strncmp(hash, "$y$", 3) != 0)
        return -1;

    if (auth_write(hash) != 0)
        return -1;

    auth.failures = 0;
    return 0;
}

/* ======================= XENUS RECOVERY ======================= */

static int auth_recovery_path(char *out, size_t size)
{
    int n = snprintf(out, size, "%s/.recovery",
                     getenv("HOME") ? getenv("HOME") : "");
    return n > 0 && (size_t)n < size ? 0 : -1;
}

static int auth_read_path(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    int ok = fgets(out, (int)size, f) != NULL;
    fclose(f);

    if (!ok)
        return -1;

    out[strcspn(out, "\r\n")] = '\0';
    return out[0] ? 1 : -1;
}

static int auth_write_path(const char *path, const char *value)
{
    return auth_write_file(path, value);
}

static int auth_generate_recovery_key(char *out, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[32];

    if (!out || size < 65)
        return -1;

    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;

    size_t got = fread(raw, 1, sizeof(raw), f);
    fclose(f);

    if (got != sizeof(raw)) {
        memset(raw, 0, sizeof(raw));
        return -1;
    }

    for (size_t i = 0; i < sizeof(raw); i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }

    out[64] = '\0';
    memset(raw, 0, sizeof(raw));
    return 0;
}

int xenus_auth_set_password_recoverable(
    const char *password, char *recovery_key, size_t key_size)
{
    char key[65] = {0};
    char recovery_path[600];
    char password_hash[AUTH_HASH_SIZE] = {0};
    char recovery_hash[AUTH_HASH_SIZE] = {0};
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    struct crypt_data data = {0};

    if (!auth.initialized || auth.enabled || !password ||
        strlen(password) < 8 || !recovery_key || key_size < 65)
        return -1;

    if (auth_recovery_path(recovery_path, sizeof(recovery_path)) != 0)
        return -1;

    if (auth_generate_recovery_key(key, sizeof(key)) != 0)
        return -1;

    if (!crypt_gensalt_rn("$y$", 0, NULL, 0, salt, sizeof(salt)))
        goto fail;

    char *hash = crypt_r(password, salt, &data);
    if (!hash || strncmp(hash, "$y$", 3) != 0)
        goto fail;

    snprintf(password_hash, sizeof(password_hash), "%s", hash);

    memset(&data, 0, sizeof(data));
    if (!crypt_gensalt_rn("$y$", 0, NULL, 0, salt, sizeof(salt)))
        goto fail;

    hash = crypt_r(key, salt, &data);
    if (!hash || strncmp(hash, "$y$", 3) != 0)
        goto fail;

    snprintf(recovery_hash, sizeof(recovery_hash), "%s", hash);

    /* Recovery verifier first; password verifier last. */
    if (auth_write_path(recovery_path, recovery_hash) != 0)
        goto fail;

    if (auth_write(password_hash) != 0) {
        unlink(recovery_path);
        goto fail;
    }

    memcpy(recovery_key, key, sizeof(key));
    auth.enabled = 1;
    auth.locked = 0;
    auth.failures = 0;

    memset(key, 0, sizeof(key));
    memset(password_hash, 0, sizeof(password_hash));
    memset(recovery_hash, 0, sizeof(recovery_hash));
    memset(&data, 0, sizeof(data));
    return 0;

fail:
    memset(key, 0, sizeof(key));
    memset(password_hash, 0, sizeof(password_hash));
    memset(recovery_hash, 0, sizeof(recovery_hash));
    memset(&data, 0, sizeof(data));
    return -1;
}

int xenus_auth_recover(const char *recovery_key)
{
    char recovery_path[600];
    char auth_backup[620];
    char recovery_backup[620];
    char stored[AUTH_HASH_SIZE] = {0};
    struct crypt_data data = {0};
    struct stat st;

    if (!auth.initialized || !auth.enabled ||
        !recovery_key || strlen(recovery_key) != 64)
        return -1;

    if (auth_recovery_path(recovery_path, sizeof(recovery_path)) != 0)
        return -1;

    if (auth_read_path(recovery_path, stored, sizeof(stored)) != 1)
        return -1;

    char *computed = crypt_r(recovery_key, stored, &data);
    int valid = computed && strcmp(computed, stored) == 0;

    memset(&data, 0, sizeof(data));
    memset(stored, 0, sizeof(stored));

    if (!valid)
        return -1;

    if (snprintf(auth_backup, sizeof(auth_backup), "%s.recovered",
                 auth.path) >= (int)sizeof(auth_backup) ||
        snprintf(recovery_backup, sizeof(recovery_backup), "%s.recovered",
                 recovery_path) >= (int)sizeof(recovery_backup))
        return -1;

    /* Jangan menimpa backup recovery dari proses sebelumnya. */
    if (lstat(auth_backup, &st) == 0 || errno != ENOENT)
        return -1;

    if (lstat(recovery_backup, &st) == 0 || errno != ENOENT)
        return -1;

    /* Pindahkan kedua verifier sebelum mengubah status autentikasi. */
    if (rename(auth.path, auth_backup) != 0)
        return -1;

    if (rename(recovery_path, recovery_backup) != 0) {
        rename(auth_backup, auth.path);
        return -1;
    }

    auth.enabled = 0;
    auth.locked = 0;
    auth.failures = 0;

    /* File backup bukan lagi verifier aktif; bersihkan jika memungkinkan. */
    unlink(auth_backup);
    unlink(recovery_backup);

    return 0;
}
