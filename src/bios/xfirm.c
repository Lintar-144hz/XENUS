/*
 * xfirm - the only command-line entry point for XENUS BIOS.
 *
 * It does not configure anything itself.  Its entire job is to make sure an X
 * display exists (Termux:X11 when running on Android/Termux) and then replace
 * itself with xenus-bios.  All configuration happens in the GUI.
 */
#define _POSIX_C_SOURCE 200809L

#include <xenus/bios.h>

#include <X11/Xlib.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* A binary path built from /proc/self/exe plus a suffix is bounded. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

static int executable(const char *path)
{
    return path && access(path, X_OK) == 0;
}

static int display_ready(const char *display)
{
    Display *dpy;

    if (!display || !*display)
        return 0;
    dpy = XOpenDisplay(display);
    if (!dpy)
        return 0;
    XCloseDisplay(dpy);
    return 1;
}

static void sleep_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static int command_exists(const char *name)
{
    char path[512];
    const char *env;
    const char *sep;
    size_t len;

    if (!name || !*name)
        return 0;
    if (strchr(name, '/'))
        return executable(name);
    env = getenv("PATH");
    while (env && *env) {
        sep = strchr(env, ':');
        len = sep ? (size_t)(sep - env) : strlen(env);
        if (len > 0 && len + strlen(name) + 2 < sizeof(path)) {
            memcpy(path, env, len);
            path[len] = '/';
            snprintf(path + len + 1, sizeof(path) - len - 1, "%s", name);
            if (executable(path))
                return 1;
        }
        if (!sep)
            break;
        env = sep + 1;
    }
    return 0;
}

static int start_termux_x11(const char *display)
{
    pid_t pid;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull;

        setsid();
        devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execlp("termux-x11", "termux-x11", display, (char *)NULL);
        _exit(127);
    }
    /* The app is safe to launch when present; if it is already open Android
       simply brings the existing activity forward. */
    if (command_exists("am")) {
        pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", O_RDWR);

            setsid();
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO)
                    close(devnull);
            }
            execlp("am", "am", "start", "--user", "0", "-n",
                   "com.termux.x11/.MainActivity", (char *)NULL);
            _exit(127);
        }
    }
    return 0;
}

static int find_bios(char *out, size_t size)
{
    const char *env = getenv("XENUS_BIOS_BIN");
    char self[PATH_MAX];
    ssize_t n;
    char *slash;

    if (env && *env && executable(env)) {
        snprintf(out, size, "%s", env);
        return 1;
    }
    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        slash = strrchr(self, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, size, "%s/xenus-bios", self);
            if (executable(out))
                return 1;
        }
    }
    if (command_exists("xenus-bios")) {
        snprintf(out, size, "xenus-bios");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char bios[PATH_MAX];
    const char *display;
    int auto_x11;
    int i;

    (void)argc;
    (void)argv;

    setenv("XENUS_BIOS_SESSION", "1", 0);
    display = getenv("DISPLAY");
    if (!find_bios(bios, sizeof(bios))) {
        fprintf(stderr,
                "xfirm: xenus-bios was not found next to xfirm or in PATH.\n");
        return 127;
    }

    if (display_ready(display))
        goto launch;

    auto_x11 = xenus_bios_runtime_termux_x11();
    if (!display || !*display)
        display = ":0";
    setenv("DISPLAY", display, 1);

    if (!auto_x11) {
        fprintf(stderr,
                "xfirm: no X display and Termux:X11 auto-start is disabled "
                "in XENUS BIOS.\n");
        return 1;
    }
    if (!command_exists("termux-x11")) {
        fprintf(stderr,
                "xfirm: DISPLAY=%s is not reachable and termux-x11 is not "
                "installed.\n", display);
        return 1;
    }

    if (start_termux_x11(display) != 0) {
        fprintf(stderr, "xfirm: cannot start termux-x11: %s\n",
                strerror(errno));
        return 1;
    }
    for (i = 0; i < 40; i++) {
        if (display_ready(display))
            break;
        sleep_ms(100);
    }
    if (!display_ready(display)) {
        fprintf(stderr,
                "xfirm: Termux:X11 did not become ready on %s.\n", display);
        return 1;
    }

launch:
    setenv("DISPLAY", display ? display : "", 1);
    execlp(bios, bios, (char *)NULL);
    fprintf(stderr, "xfirm: cannot launch %s: %s\n", bios, strerror(errno));
    return 127;
}
