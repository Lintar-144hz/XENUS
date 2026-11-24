/*
 * XENUS - Solo Coding Desktop Environment
 * X11 connection, startup/shutdown and the single event loop.
 */
#include "xenus.h"
#include "auth_screen.h"
#include "cache.h"
#include "settings_prefs.h"

#include <xenus/app.h>
#include <xenus/bios.h>
#include <xenus/net.h>
#include <xenus/event.h>
#include <xenus/plugin.h>
#include <xenus/security.h>
#include <xenus/security_bait.h>
#include <xenus/security_mirage.h>
#include <xenus/security_resource.h>
#include <xenus/ucore.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

Xenus sc;
Config cfg;

static int x_error_code;
static int startup_phase;
static int another_wm;

/* --------------------------------------------------------------- errors -- */

static int on_xerror(Display *dpy, XErrorEvent *ev)
{
    char msg[256];

    (void)dpy;
    x_error_code = ev->error_code;

    if (startup_phase && ev->error_code == BadAccess)
        another_wm = 1;

    XGetErrorText(sc.dpy, ev->error_code, msg, sizeof(msg));
    /* a client that vanished between two events is completely normal */
    if (ev->error_code != BadWindow && ev->error_code != BadDrawable)
        log_msg("X error: %s (request %d.%d, resource 0x%lx)", msg,
                ev->request_code, ev->minor_code, (unsigned long)ev->resourceid);
    return 0;
}

static int trap_xerrors(void)
{
    x_error_code = 0;
    XSync(sc.dpy, False);
    return x_error_code;
}

/* -------------------------------------------------------------- signals -- */

static void on_signal(int sig)
{
    (void)sig;
    sc.running = 0;
}

static void setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                        /* no SA_RESTART: poll() wakes up */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);               /* no zombies from spawned apps */
}

/* ----------------------------------------------------------------- lock -- */

static int acquire_lock(void)
{
    const char *home = getenv("HOME");
    const char *cache;
    char base[400];
    struct flock fl;
    int fd;

    if (!home || !*home)
        home = "/tmp";

    cache = getenv("XDG_CACHE_HOME");
    if (cache && *cache) {
        snprintf(base, sizeof(base), "%s", cache);
    } else {
        snprintf(base, sizeof(base), "%s/.cache", home);
    }
    if (mkdir(base, 0700) != 0 && errno != EEXIST) {
        log_msg("cannot create cache dir %s: %s", base, strerror(errno));
        return -2;
    }
    snprintf(sc.lock_path, sizeof(sc.lock_path), "%s/xenus", base);
    if (mkdir(sc.lock_path, 0700) != 0 && errno != EEXIST) {
        log_msg("cannot create dir %s: %s", sc.lock_path, strerror(errno));
        return -2;
    }
    snprintf(sc.lock_path, sizeof(sc.lock_path), "%s/xenus/xenus.lock", base);

    fd = open(sc.lock_path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        log_msg("cannot open lock file %s: %s", sc.lock_path, strerror(errno));
        return -2;
    }

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        close(fd);
        return -1;
    }

    if (ftruncate(fd, 0) == 0) {
        char pid[32];
        int n = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
        ssize_t wr = write(fd, pid, (size_t)n);
        (void)wr;                           /* informational only */
    }
    sc.lock_fd = fd;
    return 0;
}

/* ------------------------------------------------------------ font/atoms -- */

static void setup_atoms(void)
{
    sc.atom_wm_protocols = XInternAtom(sc.dpy, "WM_PROTOCOLS", False);
    sc.atom_wm_delete = XInternAtom(sc.dpy, "WM_DELETE_WINDOW", False);
    sc.atom_wm_state = XInternAtom(sc.dpy, "WM_STATE", False);
    sc.atom_net_active = XInternAtom(sc.dpy, "_NET_ACTIVE_WINDOW", False);
    sc.atom_net_client_list =
        XInternAtom(sc.dpy, "_NET_CLIENT_LIST", False);
    sc.atom_net_wm_name = XInternAtom(sc.dpy, "_NET_WM_NAME", False);
    sc.atom_utf8_string = XInternAtom(sc.dpy, "UTF8_STRING", False);
    sc.atom_net_wm_state = XInternAtom(sc.dpy, "_NET_WM_STATE", False);
    sc.atom_net_wm_state_fullscreen =
        XInternAtom(sc.dpy, "_NET_WM_STATE_FULLSCREEN", False);
    sc.atom_net_supported = XInternAtom(sc.dpy, "_NET_SUPPORTED", False);
    sc.atom_net_supporting_wm_check =
        XInternAtom(sc.dpy, "_NET_SUPPORTING_WM_CHECK", False);
    sc.atom_xenus_task_list = XInternAtom(sc.dpy, "_XENUS_TASK_LIST", False);
}

/* announce the EWMH hints we implement (lets tools such as xdotool see us) */
static void setup_ewmh(void)
{
    Atom supported[8];
    int n = 0;
    const char *name = "xenus";

    sc.wm_check = XCreateSimpleWindow(sc.dpy, sc.root, -10, -10, 1, 1, 0, 0, 0);
    XChangeProperty(sc.dpy, sc.wm_check, sc.atom_net_wm_name,
                    sc.atom_utf8_string, 8, PropModeReplace,
                    (const unsigned char *)name, (int)strlen(name));
    XChangeProperty(sc.dpy, sc.wm_check, sc.atom_net_supporting_wm_check,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&sc.wm_check, 1);

    supported[n++] = sc.atom_net_active;
    supported[n++] = sc.atom_net_client_list;
    supported[n++] = sc.atom_net_wm_name;
    supported[n++] = sc.atom_net_wm_state;
    supported[n++] = sc.atom_net_wm_state_fullscreen;
    supported[n++] = sc.atom_net_supported;
    supported[n++] = sc.atom_net_supporting_wm_check;

    XChangeProperty(sc.dpy, sc.root, sc.atom_net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)supported, n);
    XChangeProperty(sc.dpy, sc.root, sc.atom_net_supporting_wm_check,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&sc.wm_check, 1);
}

static void setup_cursors(void)
{
    trap_xerrors();
    sc.cur_normal = XCreateFontCursor(sc.dpy, XC_left_ptr);
    sc.cur_move = XCreateFontCursor(sc.dpy, XC_fleur);
    sc.cur_resize = XCreateFontCursor(sc.dpy, XC_bottom_right_corner);
    if (trap_xerrors()) {
        sc.cur_normal = None;
        sc.cur_move = None;
        sc.cur_resize = None;
        log_msg("cursor font unavailable, using server default");
    }
    XDefineCursor(sc.dpy, sc.root, sc.cur_normal);
}

/* ------------------------------------------------------------- wallpaper -- */

static Pixmap wallpaper_pm;

static int wp_isqrt(unsigned long v)
{
    unsigned long r = 0, b = 1UL << 30;

    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return (int)r;
}

static void setup_wallpaper(void)
{
    int r1, g1, b1, r2, g2, b2, y;
    GC wgc;

    if (wallpaper_pm != None) {
        XSetWindowBackground(sc.dpy, sc.root, cfg.col_background);
        XFreePixmap(sc.dpy, wallpaper_pm);
        wallpaper_pm = None;
    }
    if (!color_rgb(cfg.s_wallpaper_top, &r1, &g1, &b1) ||
        !color_rgb(cfg.s_wallpaper_bottom, &r2, &g2, &b2)) {
        XSetWindowBackground(sc.dpy, sc.root, cfg.col_background);
        XClearWindow(sc.dpy, sc.root);
        return;
    }

    wallpaper_pm = XCreatePixmap(sc.dpy, sc.root, (unsigned)sc.sw,
                                 (unsigned)sc.sh, (unsigned)sc.depth);
    if (wallpaper_pm == None)
        return;
    wgc = XCreateGC(sc.dpy, wallpaper_pm, 0, NULL);

    for (y = 0; y < sc.sh; y++) {
        int r = r1 + ((r2 - r1) * y) / (sc.sh > 1 ? sc.sh - 1 : 1);
        int g = g1 + ((g2 - g1) * y) / (sc.sh > 1 ? sc.sh - 1 : 1);
        int b = b1 + ((b2 - b1) * y) / (sc.sh > 1 ? sc.sh - 1 : 1);
        unsigned long px;

        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        px = (unsigned long)r << 16 | (unsigned long)g << 8 | (unsigned long)b;
        XSetForeground(sc.dpy, wgc, px);
        XFillRectangle(sc.dpy, wallpaper_pm, wgc, 0, y, (unsigned)sc.sw, 1);
    }
    if (cfg.col_wallpaper_top != cfg.col_wallpaper_bottom) {
        static const struct {
            unsigned long color;
            int cx, cy, rw, rh, strength;
        } glows[] = {
            { 0x3d6f9eUL, 82, 18, 64, 58, 18 },
            { 0x6b4f9aUL, 24, 82, 58, 52, 14 }
        };
        unsigned int gi;

        for (gi = 0; gi < sizeof(glows) / sizeof(glows[0]); gi++) {
            int cx = sc.sw * glows[gi].cx / 100;
            int cy = sc.sh * glows[gi].cy / 100;
            int rw = sc.sw * glows[gi].rw / 100;
            int rh = sc.sh * glows[gi].rh / 100;
            int sy;

            for (sy = cy - rh / 2; sy <= cy + rh / 2; sy++) {
                long dy = 2L * (sy - cy);
                long rem = (long)rh * rh - dy * dy;
                int halfw, seg;
                int br, bg, bb;
                unsigned long base;

                if (rem <= 0 || sy < 0 || sy >= sc.sh)
                    continue;
                halfw = (int)((long)rw * wp_isqrt((unsigned long)rem) /
                              (2L * rh));
                if (halfw < 1)
                    continue;
                br = r1 + ((r2 - r1) * sy) / (sc.sh > 1 ? sc.sh - 1 : 1);
                bg = g1 + ((g2 - g1) * sy) / (sc.sh > 1 ? sc.sh - 1 : 1);
                bb = b1 + ((b2 - b1) * sy) / (sc.sh > 1 ? sc.sh - 1 : 1);
                base = (unsigned long)br << 16 | (unsigned long)bg << 8 |
                       (unsigned long)bb;
                for (seg = 0; seg < 32; seg++) {
                    int x0 = cx - halfw + 2 * halfw * seg / 32;
                    int x1 = cx - halfw + 2 * halfw * (seg + 1) / 32;
                    double tx = -1.0 + (2.0 * seg + 1.0) / 32.0;
                    double fade = 1.0 - (double)(dy * dy) /
                                  ((double)rh * rh) - tx * tx;
                    int pct;

                    if (fade <= 0.0 || x1 <= x0)
                        continue;
                    pct = (int)(glows[gi].strength * fade + 0.5);
                    if (pct <= 0)
                        continue;
                    XSetForeground(sc.dpy, wgc,
                                   ui_mix(base, glows[gi].color, pct));
                    XFillRectangle(sc.dpy, wallpaper_pm, wgc, x0, sy,
                                   (unsigned)(x1 - x0), 1);
                }
            }
        }
    }
    XFreeGC(sc.dpy, wgc);
    XSetWindowBackgroundPixmap(sc.dpy, sc.root, wallpaper_pm);
    XClearWindow(sc.dpy, sc.root);
    XFlush(sc.dpy);
    wallpaper_apply();                  /* image wallpaper overrides gradient */
}

/* Rebuild every shell surface after a live theme / accent change. */
void shell_refresh_theme(void)
{
    Client *c;

    ui_set_accent(cfg.accent);
    setup_wallpaper();
    desktop_refresh();
    for (c = sc.clients; c; c = c->next)
        frame_paint(c);
    panel_update();
    system_center_redraw();
    settings_redraw();
    control_center_tick();
    notification_center_tick();
    XFlush(sc.dpy);
}

/* --------------------------------------------------------------- startup -- */

static void setup_root(void)
{
    XSetWindowAttributes attrs;

    XSetErrorHandler(on_xerror);

    attrs.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                       ButtonPressMask | ButtonReleaseMask | KeyPressMask |
                       PropertyChangeMask | StructureNotifyMask |
                       ExposureMask | PointerMotionMask;
    XChangeWindowAttributes(sc.dpy, sc.root, CWEventMask, &attrs);

    startup_phase = 1;
    XSync(sc.dpy, False);
    startup_phase = 0;

    if (another_wm) {
        char dpy_name[256];
        str_copy(dpy_name, sizeof(dpy_name), DisplayString(sc.dpy));
        close(sc.lock_fd);
        XCloseDisplay(sc.dpy);
        sc.dpy = NULL;
        die("another window manager is already running on display %s",
            dpy_name);
    }

    XSetWindowBackground(sc.dpy, sc.root, cfg.col_background);
    XClearWindow(sc.dpy, sc.root);

    sc.gc = XCreateGC(sc.dpy, sc.root, 0, NULL);
    input_grab_keys();
    input_grab_buttons();
}

/* ----------------------------------------------------------------- loop -- */

static void dispatch(XEvent *ev)
{
    if (auth_screen_active()) {
        auth_screen_handle_event(ev);
        return;
    }

    switch (ev->type) {
    case MapRequest:
        handle_map_request(&ev->xmaprequest);
        break;
    case ConfigureRequest:
        handle_configure_request(&ev->xconfigurerequest);
        break;
    case DestroyNotify:
        handle_destroy_notify(&ev->xdestroywindow);
        break;
    case UnmapNotify:
        handle_unmap_notify(&ev->xunmap);
        break;
    case EnterNotify:
        handle_enter_notify(&ev->xcrossing);
        break;
    case LeaveNotify:
        if (ev->xcrossing.window == sc.dock)
            dock_leave();
        break;
    case KeyPress:
        handle_key_press(&ev->xkey);
        break;
    case ButtonPress:
        handle_button_press(&ev->xbutton);
        break;
    case ButtonRelease:
        break;
    case MotionNotify:
        if (msgbox_motion(&ev->xmotion))
            break;
        if (system_center_motion(&ev->xmotion))
            break;
        if (settings_motion(&ev->xmotion))
            break;
        if (control_center_motion(&ev->xmotion))
            break;
        if (notification_center_motion(&ev->xmotion))
            break;
        if (security_gui_motion(&ev->xmotion))
            break;
        if (fm_motion(&ev->xmotion))
            break; else if (ev->xmotion.window == sc.root)
            desktop_motion(&ev->xmotion);
        else if (ev->xmotion.window == sc.panel)
            panel_motion(&ev->xmotion);
        else if (ev->xmotion.window == sc.dock)
            dock_motion(&ev->xmotion);
        else if (ev->xmotion.window == sc.menu)
            menu_motion(&ev->xmotion);
        else
            handle_frame_motion(&ev->xmotion);
        break;
    case Expose:
        if (ev->xexpose.window == sc.root)
            desktop_expose(&ev->xexpose);
        else
            handle_expose(&ev->xexpose);
        break;
    case PropertyNotify:
        handle_property_notify(&ev->xproperty);
        if (ev->xproperty.window == sc.root)
            desktop_root_property(&ev->xproperty);
        break;
    case ClientMessage:
        handle_client_message(&ev->xclient);
        break;
    case MappingNotify:
        handle_mapping_notify(&ev->xmapping);
        break;
    case FocusIn:
        /* some clients try to steal focus - keep ours if the pointer is
           sitting in one of our managed windows */
        break;
    default:
        break;
    }
}

static void event_loop(void)
{
    enum { X_EVENT_BATCH = 32 };

    while (sc.running) {
        xenus_cache_tick();
        xenus_core_tick();
        system_center_tick();
        control_center_tick();
        notification_center_tick();

        if (xenus_security_pending()) {
            xenus_security_pump();

            if (xenus_bait_enabled())
                xenus_bait_check();

            security_gui_redraw();
        }

        struct pollfd pfds[2];
        int np = 1;
        int wfd;
        int timeout;
        int rc;
        int processed = 0;

        xenus_event_dispatch();

        /* Process a bounded batch so other services cannot starve. */
        int auth_batch = auth_screen_active();

        while (processed < X_EVENT_BATCH && XPending(sc.dpy) > 0) {
            XEvent ev;
            event_next_coalesced(&ev);

            if (auth_batch) {
                if (auth_screen_active())
                    auth_screen_handle_event(&ev);
            } else {
                dispatch(&ev);

                if (auth_screen_active())
                    auth_batch = 1;
            }

            processed++;
        }

        /* Discard stale desktop events queued before authentication ended. */
        if (auth_batch && !auth_screen_active()) {
            XEvent stale;
            while (XPending(sc.dpy) > 0)
                XNextEvent(sc.dpy, &stale);
        }

        XFlush(sc.dpy);

        pfds[0].fd = ConnectionNumber(sc.dpy);
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;

        wfd = xenus_security_watch_fd();
        if (wfd >= 0) {
            pfds[1].fd = wfd;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
            np = 2;
        }

        /* If the X queue is still busy, poll without blocking. */
        timeout = (processed == X_EVENT_BATCH || xenus_security_pending()) ? 0 : panel_timeout_ms();
        {
            int dms = dock_anim_timeout_ms();

            if (dms >= 0 && (timeout < 0 || dms < timeout))
                timeout = dms;
        }
        rc = poll(pfds, (nfds_t)np, timeout);

        if (rc < 0) {
            if (errno == EINTR)
                continue;
            log_msg("poll failed: %s", strerror(errno));
            break;
        }

        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            log_msg("X11 connection poll error: 0x%x",
                    (unsigned int)pfds[0].revents);
            break;
        }

        if (np == 2) {
            short errors = POLLERR | POLLHUP | POLLNVAL;

            if (pfds[1].revents & errors) {
                log_msg("security watcher poll error: 0x%x",
                        (unsigned int)pfds[1].revents);
                break;
            }

            if (pfds[1].revents & POLLIN) {
                xenus_security_pump();

                if (xenus_bait_enabled())
                    xenus_bait_check();

                security_gui_redraw();
            }
        }

        panel_tick();
        dock_tick();
    }
}

/* -------------------------------------------------------------- shutdown -- */

static void cleanup(void)
{
    system_center_cleanup();
    settings_cleanup();
    settings_prefs_shutdown();
    control_center_cleanup();
    notification_center_cleanup();
    security_gui_cleanup();
    text_editor_cleanup();
    xenus_core_shutdown();
    xenus_bait_shutdown();
    xenus_mirage_shutdown();
    xenus_security_shutdown();
    xenus_plugins_shutdown();
    xenus_net_shutdown();
    xenus_app_shutdown();

    menu_hide();
    cmd_hide();
    fm_cleanup();
    desktop_cleanup();

    while (sc.clients) {
        Client *c = sc.clients;

        sc.clients = c->next;
        if (c->frame != None) {
            XSelectInput(sc.dpy, c->win, NoEventMask);
            XReparentWindow(sc.dpy, c->win, sc.root, c->x + c->bw,
                            c->y + c->bw + c->title_h);
            XRemoveFromSaveSet(sc.dpy, c->win);
            XDestroyWindow(sc.dpy, c->frame);
        }
        free(c);
    }
    sc.focused = NULL;
    apps_free();

    XUngrabKey(sc.dpy, AnyKey, AnyModifier, sc.root);
    XUngrabButton(sc.dpy, AnyButton, AnyModifier, sc.root);
    XUngrabKeyboard(sc.dpy, CurrentTime);
    XUngrabPointer(sc.dpy, CurrentTime);

    panel_destroy();
    if (sc.menu != None) {
        XDestroyWindow(sc.dpy, sc.menu);
        sc.menu = None;
    }
    if (sc.cmd != None) {
        XDestroyWindow(sc.dpy, sc.cmd);
        sc.cmd = None;
    }
    if (sc.wm_check != None) {
        XDestroyWindow(sc.dpy, sc.wm_check);
        sc.wm_check = None;
    }

    XDeleteProperty(sc.dpy, sc.root, sc.atom_net_client_list);
    XDeleteProperty(sc.dpy, sc.root, sc.atom_xenus_task_list);
    XDeleteProperty(sc.dpy, sc.root, sc.atom_net_active);
    XDeleteProperty(sc.dpy, sc.root, sc.atom_net_supported);
    XDeleteProperty(sc.dpy, sc.root, sc.atom_net_supporting_wm_check);

    if (wallpaper_pm != None) {
        XFreePixmap(sc.dpy, wallpaper_pm);
        wallpaper_pm = None;
    }
    if (sc.gc)
        XFreeGC(sc.dpy, sc.gc);
    ui_cleanup_fonts();
    if (sc.cur_normal)
        XFreeCursor(sc.dpy, sc.cur_normal);
    if (sc.cur_move)
        XFreeCursor(sc.dpy, sc.cur_move);
    if (sc.cur_resize)
        XFreeCursor(sc.dpy, sc.cur_resize);

    XSync(sc.dpy, False);
    XCloseDisplay(sc.dpy);
    sc.dpy = NULL;

    if (sc.lock_fd >= 0) {
        close(sc.lock_fd);
        sc.lock_fd = -1;
    }
    log_msg("clean shutdown");
}

/* ------------------------------------------------------------------ main -- */

static void usage(int code)
{
    printf("XENUS %s - Solo Coding Desktop Environment\n", XENUS_VERSION);
    printf("usage: xenus [-c config] [-v] [-h]\n\n");
    printf("  -c FILE   read configuration from FILE\n"
           "            (default: ~/.config/xenus/config)\n"
           "  -v        print version and exit\n"
           "  -h        show this help\n\n");
    printf("bindings:\n"
           "  Alt+F4        close window          Alt+Tab      switch windows\n"
           "  Alt+F2        run a command         Alt+Space    application menu\n"
           "  Alt+L-drag    move window           Alt+R-drag   resize window\n"
           "  Alt+Enter     start terminal        Alt+E        file manager\n"
           "  Alt+S         security center       Alt+Up       maximize\n"
           "  Alt+I         settings              Alt+C        control center\n"
           "  Alt+Down      minimize              Alt+Left/Right snap halves\n"
           "  Alt+D         show desktop\n"
           "  right click desktop   desktop menu\n"
           "\n"
           "desktop shortcuts:\n"
           "  click  select        double click  open\n"
           "  right click icon     icon menu     drag  move\n"
           "  shortcuts are files in ~/Desktop (*.desktop runs Exec=)\n");
    exit(code);
}

int main(int argc, char **argv)
{
    char default_path[512];
    const char *home;
    const char *conf = NULL;
    int opt;
    int bios_safe = 0;
    int bios_diagnostic = 0;
    int bios_delay = 0;

    memset(&sc, 0, sizeof(sc));
    sc.lock_fd = -1;
    sc.running = 1;

    while ((opt = getopt(argc, argv, "c:vh")) != -1) {
        switch (opt) {
        case 'c':
            conf = optarg;
            break;
        case 'v':
            printf("xenus %s\n", XENUS_VERSION);
            return 0;
        case 'h':
            usage(0);
            return 0;
        default:
            usage(1);
            return 1;
        }
    }

    setup_signals();
    config_defaults(&cfg);

    home = getenv("HOME");
    if (!home)
        home = "";
    snprintf(default_path, sizeof(default_path), "%s/.config/xenus/config", home);
    str_copy(cfg.config_path, sizeof(cfg.config_path),
             conf ? conf : default_path);

    if (config_load(&cfg, cfg.config_path) != 0) {
        if (conf)
            die("config file not found: %s", cfg.config_path);
        log_msg("no config at %s, using defaults", cfg.config_path);
    } else {
        log_msg("loaded config %s", cfg.config_path);
    }
    config_add_defaults_menu(&cfg);

    /* Settings-owned keys are validated through the Phase 2 Config Manager.
       The manager shares this same user file and only touches its own keys. */
    if (settings_prefs_init() != 0)
        log_msg("Settings preferences unavailable: %s",
                settings_prefs_error());

    /* XENUS BIOS startup flags are optional: no store means normal boot with
       no delay.  Safe mode is consumed here so the setup app can recover a
       desktop that would otherwise start plugins or motion. */
    bios_safe = xenus_bios_runtime_boot_mode() == XENUS_BIOS_BOOT_SAFE;
    bios_diagnostic = xenus_bios_runtime_boot_diagnostic();
    bios_delay = xenus_bios_runtime_boot_delay();
    if (bios_safe) {
        cfg.animations = 0;
        log_msg("BIOS: safe startup mode; plugins and shell motion disabled");
    }
    if (bios_diagnostic)
        log_msg("BIOS: diagnostic boot; config=%s safe=%d delay=%d",
                cfg.config_path, bios_safe, bios_delay);

    /* sandbox resource limits: one ceiling per sandboxed process.
       RAM comes from sandbox_memory_mb (default 3072 MiB = 3 GiB); XENUS
       imposes no CPU quota of its own - the host scheduler decides. */
    {
        xenus_resource_limits_t rl;

        xenus_resource_defaults(&rl);
        rl.ram_bytes = (unsigned long)cfg.sandbox_memory_mb * 1024ul *
                       1024ul;
        rl.cpu_seconds = 0;
        xenus_resource_set(&rl);
    }

    {
        int lock_rc = acquire_lock();
        if (lock_rc == -1)
            die("another XENUS instance is already running (lock %s)",
                sc.lock_path);
        if (lock_rc != 0)
            die("cannot create lock file %s", sc.lock_path);
    }

    sc.dpy = XOpenDisplay(NULL);
    if (!sc.dpy) {
        if (sc.lock_fd >= 0) {
            close(sc.lock_fd);
        }
        die("cannot open display '%s'",
            getenv("DISPLAY") ? getenv("DISPLAY") : "");
    }

    if (bios_delay > 0) {
        log_msg("BIOS: startup delay %d second(s)", bios_delay);
        sleep((unsigned)bios_delay);
    }

    sc.screen = DefaultScreen(sc.dpy);
    sc.root = RootWindow(sc.dpy, sc.screen);
    sc.sw = DisplayWidth(sc.dpy, sc.screen);
    sc.sh = DisplayHeight(sc.dpy, sc.screen);
    sc.depth = DefaultDepth(sc.dpy, sc.screen);
    sc.cmap = DefaultColormap(sc.dpy, sc.screen);

    XSetErrorHandler(on_xerror);

    config_resolve_colors(&cfg);
    ui_load_fonts();
    setup_atoms();
    setup_cursors();
    setup_root();
    setup_ewmh();
    setup_wallpaper();

    log_msg("XENUS %s started on %dx%d (display %s)", XENUS_VERSION, sc.sw,
            sc.sh, DisplayString(sc.dpy));

    {
        xenus_app_info_t app = { XENUS_NAME, XENUS_VERSION,
                                "Solo Coding Desktop Environment", NULL };

        xenus_app_init(&app);
        xenus_net_init(NULL);
    }

    xenus_core_init(cfg.config_path);

    /* Startup authentication: desktop belum dibuat sebelum login. */
    if (xenus_auth_init() != 0) {
        log_msg("XENUS: authentication initialization failed");
        return 1;
    }

    if (xenus_auth_enabled()) {
        if (auth_screen_open() != 0) {
            log_msg("XENUS: unable to open authentication screen");
            return 1;
        }

        while (auth_screen_active()) {
            XEvent auth_event;
            XNextEvent(sc.dpy, &auth_event);
            auth_screen_handle_event(&auth_event);
        }
    }

    apps_scan();
    panel_create();
    manage_existing();
    desktop_init();
    panel_update();


    xenus_security_init(NULL);        /* scanner + policy + audit + gate */

    if (!bios_safe && xenus_plugins_init(NULL) == XENUS_OK) {
        xenus_plugins_discover();
        xenus_plugins_load_all();
    } else if (bios_safe) {
        log_msg("BIOS: plugin loading skipped by safe startup mode");
    }

    event_loop();

    cleanup();
    return 0;
}
