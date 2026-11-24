#include <xenus/ucore.h>
#include "xenus.h"
#include "auth_screen.h"

#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <string.h>
#include <stdio.h>

#define AUTH_INPUT_MAX 256

static Window auth_window;
static GC auth_gc;
static int active;
static int keyboard_grabbed;
static int pointer_grabbed;

static char password[AUTH_INPUT_MAX];
static size_t password_len;
static char message[128] = "Enter your password to continue";

static void wipe_input(void)
{
    volatile char *p = password;

    for (size_t i = 0; i < sizeof(password); i++)
        p[i] = 0;

    password_len = 0;
}

static void rounded_rect(int x, int y, int w, int h, int r,
                         unsigned long color, int filled)
{
    if (w <= 0 || h <= 0)
        return;

    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    XSetForeground(sc.dpy, auth_gc, color);

    if (filled) {
        XFillRectangle(sc.dpy, auth_window, auth_gc,
                       x + r, y, w - 2 * r, h);
        XFillRectangle(sc.dpy, auth_window, auth_gc,
                       x, y + r, w, h - 2 * r);

        XFillArc(sc.dpy, auth_window, auth_gc,
                 x, y, 2 * r, 2 * r, 0, 360 * 64);
        XFillArc(sc.dpy, auth_window, auth_gc,
                 x + w - 2 * r, y, 2 * r, 2 * r, 0, 360 * 64);
        XFillArc(sc.dpy, auth_window, auth_gc,
                 x, y + h - 2 * r, 2 * r, 2 * r, 0, 360 * 64);
        XFillArc(sc.dpy, auth_window, auth_gc,
                 x + w - 2 * r, y + h - 2 * r,
                 2 * r, 2 * r, 0, 360 * 64);
    } else {
        XDrawLine(sc.dpy, auth_window, auth_gc,
                  x + r, y, x + w - r, y);
        XDrawLine(sc.dpy, auth_window, auth_gc,
                  x + r, y + h, x + w - r, y + h);
        XDrawLine(sc.dpy, auth_window, auth_gc,
                  x, y + r, x, y + h - r);
        XDrawLine(sc.dpy, auth_window, auth_gc,
                  x + w, y + r, x + w, y + h - r);

        XDrawArc(sc.dpy, auth_window, auth_gc,
                 x, y, 2 * r, 2 * r, 90 * 64, 90 * 64);
        XDrawArc(sc.dpy, auth_window, auth_gc,
                 x + w - 2 * r, y, 2 * r, 2 * r,
                 0, 90 * 64);
        XDrawArc(sc.dpy, auth_window, auth_gc,
                 x, y + h - 2 * r, 2 * r, 2 * r,
                 180 * 64, 90 * 64);
        XDrawArc(sc.dpy, auth_window, auth_gc,
                 x + w - 2 * r, y + h - 2 * r,
                 2 * r, 2 * r, 270 * 64, 90 * 64);
    }
}

static void draw_centered(const char *text, int y,
                          XFontStruct *font, unsigned long color)
{
    if (!text || !font)
        return;

    int len = (int)strlen(text);
    int width = XTextWidth(font, text, len);

    XSetForeground(sc.dpy, auth_gc, color);
    XSetFont(sc.dpy, auth_gc, font->fid);

    XDrawString(sc.dpy, auth_window, auth_gc,
                (sc.sw - width) / 2, y, text, len);
}

/* Shared layout: drawing and mouse handling use identical coordinates. */
static void get_layout(int *field_x, int *field_y,
                       int *field_w, int *button_x, int *button_y,
                       int *button_w, int *button_h)
{
    int cx = sc.sw / 2;
    int width = sc.sw - 40;

    if (width > 340)
        width = 340;
    if (width < 180)
        width = sc.sw - 24;

    *field_w = width;
    *field_x = cx - width / 2;
    *field_y = sc.sh / 2 + 8;

    *button_w = width;
    *button_h = 44;
    *button_x = cx - width / 2;
    *button_y = *field_y + 62;
}

static void auth_submit(void)
{
    password[password_len] = '\0';

    if (xenus_auth_verify(password) == 0) {
        auth_screen_close();
        return;
    }

    snprintf(message, sizeof(message),
             "Incorrect password. Please try again.");

    wipe_input();
    auth_screen_draw();
}

void auth_screen_draw(void)
{
    if (!active || !auth_window)
        return;

    Display *dpy = sc.dpy;

    unsigned long bg = cfg.col_background;
    unsigned long fg = cfg.col_menu_fg;
    unsigned long accent = cfg.col_menu_sel;
    unsigned long muted = cfg.col_border_unfocus;
    unsigned long panel = cfg.col_menu_bg;

    int cx = sc.sw / 2;
    int cy = sc.sh / 2;

    int field_x, field_y, field_w;
    int button_x, button_y, button_w, button_h;

    get_layout(&field_x, &field_y, &field_w,
               &button_x, &button_y, &button_w, &button_h);

    /* Background */
    XSetForeground(dpy, auth_gc, bg);
    XFillRectangle(dpy, auth_window, auth_gc,
                   0, 0, (unsigned int)sc.sw,
                   (unsigned int)sc.sh);

    /* Subtle avatar circle */
    XSetForeground(dpy, auth_gc, panel);
    XFillArc(dpy, auth_window, auth_gc,
             cx - 42, cy - 172, 84, 84, 0, 360 * 64);

    XSetForeground(dpy, auth_gc, accent);
    XDrawArc(dpy, auth_window, auth_gc,
             cx - 42, cy - 172, 84, 84, 0, 360 * 64);

    draw_centered("X", cy - 118, sc.font_bold, fg);

    /* Welcome */
    draw_centered("Welcome to XENUS",
                  cy - 62, sc.font_bold, fg);

    draw_centered("Your desktop. Your space.",
                  cy - 38, sc.font, muted);

    /* Password field */
    rounded_rect(field_x, field_y, field_w, 46,
                 12, panel, 1);

    rounded_rect(field_x, field_y, field_w, 46,
                 12, muted, 0);

    char masked[AUTH_INPUT_MAX];
    size_t n = password_len;

    if (n >= sizeof(masked))
        n = sizeof(masked) - 1;

    memset(masked, '*', n);
    masked[n] = '\0';

    const char *display_text = n ? masked : "Password";
    unsigned long text_color = n ? fg : muted;

    XSetForeground(dpy, auth_gc, text_color);
    XSetFont(dpy, auth_gc, sc.font->fid);

    XDrawString(dpy, auth_window, auth_gc,
                field_x + 16, field_y + 29,
                display_text, (int)strlen(display_text));

    /* Unlock button */
    rounded_rect(button_x, button_y, button_w, button_h,
                 12, accent, 1);

    draw_centered("Continue  >",
                  button_y + 28, sc.font_bold, bg);

    /* Status and footer */
    draw_centered(message, button_y + 75,
                  sc.font, fg);

    draw_centered("Press ENTER to continue",
                  button_y + 112, sc.font, muted);

    XFlush(dpy);
}

int auth_screen_open(void)
{
    if (active)
        return 0;

    XSetWindowAttributes attr;
    memset(&attr, 0, sizeof(attr));

    attr.override_redirect = True;
    attr.background_pixel = cfg.col_background;
    attr.event_mask = ExposureMask | KeyPressMask |
                      ButtonPressMask | StructureNotifyMask;

    auth_window = XCreateWindow(
        sc.dpy, sc.root,
        0, 0, (unsigned int)sc.sw, (unsigned int)sc.sh,
        0, CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel | CWEventMask,
        &attr
    );

    if (!auth_window)
        return -1;

    auth_gc = XCreateGC(sc.dpy, auth_window, 0, NULL);

    if (!auth_gc) {
        XDestroyWindow(sc.dpy, auth_window);
        auth_window = None;
        return -1;
    }

    wipe_input();

    snprintf(message, sizeof(message),
             "Enter your password to continue");

    active = 1;

    XMapRaised(sc.dpy, auth_window);
    XSetInputFocus(sc.dpy, auth_window, RevertToParent, CurrentTime);
    XSync(sc.dpy, False);

    int grab = XGrabKeyboard(
        sc.dpy, auth_window, True,
        GrabModeAsync, GrabModeAsync, CurrentTime
    );

    if (grab != GrabSuccess) {
        auth_screen_close();
        return -1;
    }

    keyboard_grabbed = 1;

    grab = XGrabPointer(
        sc.dpy, auth_window, True,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync,
        auth_window, None, CurrentTime
    );

    if (grab != GrabSuccess) {
        auth_screen_close();
        return -1;
    }

    pointer_grabbed = 1;

    auth_screen_draw();
    XFlush(sc.dpy);

    return 0;
}

void auth_screen_close(void)
{
    if (keyboard_grabbed)
        XUngrabKeyboard(sc.dpy, CurrentTime);

    if (pointer_grabbed)
        XUngrabPointer(sc.dpy, CurrentTime);

    keyboard_grabbed = 0;
    pointer_grabbed = 0;

    wipe_input();
    active = 0;

    if (auth_gc)
        XFreeGC(sc.dpy, auth_gc);

    if (auth_window)
        XDestroyWindow(sc.dpy, auth_window);

    auth_gc = NULL;
    auth_window = None;

    XFlush(sc.dpy);
}

int auth_screen_active(void)
{
    return active;
}

int auth_screen_handle_event(XEvent *ev)
{
    if (!active || !ev)
        return 0;

    if (ev->type == Expose) {
        if (ev->xexpose.window == auth_window)
            auth_screen_draw();

        return 1;
    }

    if (ev->type == KeyPress) {
        KeySym key;
        char input[32];

        int len = XLookupString(
            &ev->xkey, input, sizeof(input), &key, NULL
        );

        if (key == XK_BackSpace) {
            if (password_len > 0)
                password[--password_len] = '\0';
        } else if (key == XK_Return || key == XK_KP_Enter) {
            auth_submit();
            return 1;
        } else if (key == XK_Escape) {
            /* Login tidak dapat dibatalkan dengan Escape. */
        } else if (len > 0 &&
                   password_len + (size_t)len < sizeof(password) - 1) {
            for (int i = 0; i < len; i++) {
                unsigned char c = (unsigned char)input[i];

                if (c >= 32 && c <= 126)
                    password[password_len++] = (char)c;
            }

            password[password_len] = '\0';
        }

        auth_screen_draw();
        return 1;
    }

    if (ev->type == ButtonPress) {
        int field_x, field_y, field_w;
        int button_x, button_y, button_w, button_h;

        get_layout(&field_x, &field_y, &field_w,
                   &button_x, &button_y, &button_w, &button_h);

        int x = ev->xbutton.x;
        int y = ev->xbutton.y;

        if (x >= button_x && x < button_x + button_w &&
            y >= button_y && y < button_y + button_h) {
            auth_submit();
        }

        return 1;
    }

    return 1;
}
