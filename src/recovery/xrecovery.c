#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
#include <crypt.h>
#include <xenus/ucore.h>

static char auth_file[PATH_MAX];
static char recovery_file[PATH_MAX];
static char audit_file[PATH_MAX];

static int path(char *out, size_t size, const char *home,
                const char *suffix)
{
    int n = snprintf(out, size, "%s/%s", home, suffix);
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

static int setup(void)
{
    const char *home = getenv("HOME");
    char config[PATH_MAX], dir[PATH_MAX];
    struct stat st;

    if (!home || !*home ||
        path(config, sizeof(config), home, ".config") ||
        path(dir, sizeof(dir), home, ".config/xenus") ||
        path(auth_file, sizeof(auth_file), home, ".config/xenus/.auth") ||
        path(recovery_file, sizeof(recovery_file), home, ".recovery") ||
        path(audit_file, sizeof(audit_file), home,
             ".config/xenus/recovery.log"))
        return -1;

    if (mkdir(config, 0700) && errno != EEXIST)
        return -1;
    if (mkdir(dir, 0700) && errno != EEXIST)
        return -1;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode))
        return -1;

    return 0;
}

static int read_hash(const char *file, char *out, size_t size)
{
    FILE *f = fopen(file, "r");
    if (!f)
        return -1;

    int ok = fgets(out, (int)size, f) != NULL;
    fclose(f);
    if (!ok)
        return -1;

    out[strcspn(out, "\r\n")] = 0;
    return out[0] ? 0 : -1;
}

static int write_hash(const char *file, const char *value)
{
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp", file) >=
        (int)sizeof(temp))
        return -1;

    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -1;

    size_t len = strlen(value);
    size_t off = 0;
    int failed = 0;

    while (off < len) {
        ssize_t n = write(fd, value + off, len - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            failed = 1;
            break;
        }
        off += (size_t)n;
    }

    if (!failed && write(fd, "\n", 1) != 1)
        failed = 1;
    if (!failed && fsync(fd))
        failed = 1;
    if (close(fd))
        failed = 1;

    if (!failed && rename(temp, file) == 0)
        return 0;

    unlink(temp);
    return -1;
}

static void audit(const char *event)
{
    int fd = open(audit_file, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) {
        fprintf(stderr, "Warning: audit log unavailable.\n");
        return;
    }

    fchmod(fd, 0600);
    time_t now = time(NULL);
    struct tm tm_now;
    char stamp[32] = "unknown-time";

    if (localtime_r(&now, &tm_now))
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S",
                 &tm_now);

    char line[160];
    int n = snprintf(line, sizeof(line),
                     "%s event=%s uid=%lu\n",
                     stamp, event, (unsigned long)getuid());

    if (n > 0 && (size_t)n < sizeof(line)) {
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(fd, line + off, (size_t)n - off);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                break;
            off += (size_t)w;
        }
        fsync(fd);
    }
    close(fd);
}

static int verify(const char *secret, const char *file)
{
    char stored[512] = {0};
    struct crypt_data data = {0};

    if (read_hash(file, stored, sizeof(stored)))
        return -1;

    char *computed = crypt_r(secret, stored, &data);
    int ok = computed && strcmp(computed, stored) == 0;

    memset(stored, 0, sizeof(stored));
    memset(&data, 0, sizeof(data));
    return ok ? 0 : -1;
}

static int generate_key(char out[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[32];

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;

    size_t off = 0;
    while (off < sizeof(raw)) {
        ssize_t n = read(fd, raw + off, sizeof(raw) - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            memset(raw, 0, sizeof(raw));
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);

    for (size_t i = 0; i < sizeof(raw); i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[64] = 0;
    memset(raw, 0, sizeof(raw));
    return 0;
}

static int enroll(void)
{
    char password[256] = {0};
    char key[65] = {0};
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    struct crypt_data data = {0};

    if (access(auth_file, F_OK) != 0) {
        puts("No active XENUS password found.");
        return 1;
    }
    printf("Current XENUS password: ");
    fflush(stdout);
    if (!fgets(password, sizeof(password), stdin))
        return 1;
    password[strcspn(password, "\r\n")] = 0;

    if (verify(password, auth_file)) {
        memset(password, 0, sizeof(password));
        puts("Password verification failed.");
        return 1;
    }
    memset(password, 0, sizeof(password));

    if (generate_key(key) ||
        !crypt_gensalt_rn("$y$", 0, NULL, 0, salt, sizeof(salt))) {
        puts("Unable to generate recovery credential.");
        return 1;
    }

    char *hash = crypt_r(key, salt, &data);
    if (!hash || strncmp(hash, "$y$", 3) != 0 ||
        write_hash(recovery_file, hash)) {
        memset(key, 0, sizeof(key));
        memset(&data, 0, sizeof(data));
        puts("Unable to save Recovery Key verifier.");
        return 1;
    }

    audit("recovery-enrolled");
    puts("\nSAVE THIS RECOVERY KEY IN A SECURE PLACE:");
    puts(key);
    puts("\nThis key is shown only once. XENUS does not store its plaintext.");

    memset(key, 0, sizeof(key));
    memset(&data, 0, sizeof(data));
    return 0;
}

static int bypass(void)
{
    char key[128] = {0};
    char password[256] = {0};
    char confirm[256] = {0};

    if (xenus_auth_init() != 0) {
        puts("Unable to initialize XENUS authentication.");
        return 1;
    }

    printf("Recovery Key: ");
    fflush(stdout);
    if (!fgets(key, sizeof(key), stdin))
        return 1;
    key[strcspn(key, "\r\n")] = 0;

    if (strlen(key) != 64) {
        memset(key, 0, sizeof(key));
        puts("Invalid Recovery Key.");
        return 1;
    }

    printf("New XENUS password (minimum 8 characters): ");
    fflush(stdout);
    if (!fgets(password, sizeof(password), stdin))
        goto fail;

    password[strcspn(password, "\r\n")] = 0;
    if (strlen(password) < 8) {
        puts("Password must contain at least 8 characters.");
        goto fail;
    }

    printf("Confirm new password: ");
    fflush(stdout);
    if (!fgets(confirm, sizeof(confirm), stdin))
        goto fail;

    confirm[strcspn(confirm, "\r\n")] = 0;
    if (strcmp(password, confirm) != 0) {
        puts("Passwords do not match.");
        goto fail;
    }

    if (xenus_auth_recover(key) != 0) {
        puts("Recovery verification failed. Authentication unchanged.");
        audit("recovery-failed");
        goto fail;
    }

    if (xenus_auth_set_password(password) != 0) {
        puts("CRITICAL: Recovery succeeded but new password could not be saved.");
        puts("Authentication may now be disabled. Preserve your files.");
        audit("recovery-password-save-failed");
        goto fail;
    }

    audit("recovery-completed");
    puts("Recovery successful.");
    puts("New XENUS password has been configured.");
    puts("You can now log in using the new password.");

    memset(key, 0, sizeof(key));
    memset(password, 0, sizeof(password));
    memset(confirm, 0, sizeof(confirm));
    return 0;

fail:
    memset(key, 0, sizeof(key));
    memset(password, 0, sizeof(password));
    memset(confirm, 0, sizeof(confirm));
    return 1;
}

static void status(void)
{
    puts("XENUS Password Recovery");
    printf("Authentication: %s\n",
           access(auth_file, F_OK) == 0 ? "ENABLED" : "DISABLED");
    printf("Recovery verifier: %s\n",
           access(recovery_file, F_OK) == 0 ? "PRESENT" : "NOT PRESENT");
}

static void usage(const char *name)
{
    printf("Usage:\n");
    printf("  %s status\n", name);
    printf("  %s enroll\n", name);
    printf("  %s bypass\n", name);
    printf("  %s help\n", name);
}

int main(int argc, char **argv)
{
    if (setup()) {
        fprintf(stderr, "Unable to initialize recovery paths.\n");
        return 1;
    }

    if (argc != 2 || !strcmp(argv[1], "help")) {
        usage(argv[0]);
        return argc == 2 ? 0 : 1;
    }
    if (!strcmp(argv[1], "status")) {
        status();
        return 0;
    }
    if (!strcmp(argv[1], "enroll"))
        return enroll();
    if (!strcmp(argv[1], "bypass"))
        return bypass();

    usage(argv[0]);
    return 1;
}
