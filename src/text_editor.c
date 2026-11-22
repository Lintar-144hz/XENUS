/*
 * XENUS Text Editor v0.1
 * Native Xlib text editor. UTF-8 byte storage, 4 MiB limit.
 */
#include "xenus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TE_MAX_SIZE (4u * 1024u * 1024u)
#define TE_W 760
#define TE_H 540
#define TE_TITLE 34
#define TE_STATUS 27
#define TE_PAD 16

static Window te_win;
static int te_x, te_y;
static int te_w = TE_W, te_h = TE_H;
static int te_restore_x, te_restore_y;
static int te_restore_w = TE_W, te_restore_h = TE_H;
static int te_maximized;
static int te_close_armed;
static int te_hover = -1;
static int te_visible;
static int te_dirty;
static char te_path[1024];
static char te_status[160];
static char te_data[TE_MAX_SIZE + 1];
static size_t te_len;
static size_t te_cursor;
static int te_first_line;

static int te_char_w(void)
{
    int w = text_width_mono("M");
    return w > 0 ? w : 8;
}

static int te_line_h(void)
{
    int h = sc.font_mono_ascent + sc.font_mono_descent + 5;
    return h > 0 ? h : 17;
}

static void te_message(const char *s)
{
    snprintf(te_status, sizeof(te_status), "%s", s ? s : "");
}

static int te_line_count(void)
{
    int n = 1;
    size_t i;

    for (i = 0; i < te_len; i++)
        if (te_data[i] == '\n')
            n++;
    return n;
}

static void te_cursor_position(int *line, int *col)
{
    size_t i;
    int l = 0, c = 0;

    for (i = 0; i < te_cursor && i < te_len; i++) {
        if (te_data[i] == '\n') {
            l++;
            c = 0;
        } else {
            c++;
        }
    }
    if (line) *line = l;
    if (col) *col = c;
}

static size_t te_line_start(int wanted)
{
    size_t i;
    int line = 0;

    for (i = 0; i < te_len; i++) {
        if (line == wanted)
            return i;
        if (te_data[i] == '\n')
            line++;
    }
    return te_len;
}

static size_t te_line_end(size_t start)
{
    while (start < te_len && te_data[start] != '\n')
        start++;
    return start;
}

static void te_keep_cursor_visible(void)
{
    int line;
    int visible;
    te_cursor_position(&line, NULL);

    visible = (te_h - TE_TITLE - TE_STATUS - 18) / te_line_h();
    if (visible < 1) visible = 1;

    if (line < te_first_line)
        te_first_line = line;
    else if (line >= te_first_line + visible)
        te_first_line = line - visible + 1;
    if (te_first_line < 0)
        te_first_line = 0;
}

static void te_draw(void)
{
    int y, line, col, visible, current_line;
    size_t pos;
    char title[120], info[160], row[1024];
    unsigned long bg = cfg.col_background;
    unsigned long fg = cfg.col_title_fg;
    unsigned long muted = cfg.col_title_fg_inactive;
    unsigned long accent = 0x30d158UL;

    if (!te_win || !sc.dpy)
        return;

    XSetForeground(sc.dpy, sc.gc, bg);
    XFillRectangle(sc.dpy, te_win, sc.gc, 0, 0, te_w, te_h);

    XSetForeground(sc.dpy, sc.gc, cfg.col_title_bg);
    XFillRectangle(sc.dpy, te_win, sc.gc, 0, 0, te_w, TE_TITLE);

    snprintf(title, sizeof(title), "%.110s%s",
             te_path[0] ? strrchr(te_path, '/') ?
             strrchr(te_path, '/') + 1 : te_path : "Untitled", te_dirty ? " *" : "");
    ui_draw_traffic(te_win, 14, TE_TITLE / 2, 1, te_hover,
                    te_maximized, cfg.col_title_bg);

    {
        int tw = text_width_bold(title);
        draw_text_bold(te_win, (te_w - tw) / 2, 23, fg, title);
    }

    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawLine(sc.dpy, te_win, sc.gc, 0, TE_TITLE,
              te_w, TE_TITLE);

    te_cursor_position(&current_line, &col);
    visible = (te_h - TE_TITLE - TE_STATUS - 18) / te_line_h();
    if (visible < 1) visible = 1;

    pos = te_line_start(te_first_line);
    y = TE_TITLE + 17 + sc.font_mono_ascent;

    for (line = 0; line < visible && pos <= te_len; line++) {
        size_t end = te_line_end(pos);
        size_t n = end - pos;
        int row_no = te_first_line + line;

        if (n >= sizeof(row))
            n = sizeof(row) - 1;
        memcpy(row, te_data + pos, n);
        row[n] = '\0';

        snprintf(info, sizeof(info), "%4d", row_no + 1);
        draw_text_small(te_win, 8, y, muted, info);
        draw_text_mono(te_win, 52, y, fg, row);

        if (row_no == current_line) {
            int cx = 52 + col * te_char_w();
            XSetForeground(sc.dpy, sc.gc, accent);
            XFillRectangle(sc.dpy, te_win, sc.gc,
                           cx, y - sc.font_mono_ascent,
                           2, te_line_h());
        }

        if (end >= te_len)
            break;
        pos = end + 1;
        y += te_line_h();
    }

    XSetForeground(sc.dpy, sc.gc, cfg.col_panel_bg);
    XFillRectangle(sc.dpy, te_win, sc.gc, 0, te_h - TE_STATUS,
                   te_w, TE_STATUS);
    snprintf(info, sizeof(info), "Ln %d, Col %d   |   %lu bytes",
             current_line + 1, col + 1, (unsigned long)te_len);
    draw_text_small(te_win, 12, te_h - 9, fg, info);
    draw_text_small(te_win, 300, te_h - 9, muted,
                    te_status[0] ? te_status : "Ctrl+S Save   Ctrl+Q Close");

    XSetForeground(sc.dpy, sc.gc, accent);
    XDrawRectangle(sc.dpy, te_win, sc.gc, 0, 0, te_w - 1, te_h - 1);
}

static int te_save(void)
{
    FILE *f;

    if (!te_path[0]) {
        te_message("No file path");
        te_draw();
        return 0;
    }

    f = fopen(te_path, "wb");
    if (!f) {
        te_message("Cannot write this file");
        te_draw();
        return 0;
    }
    {
        size_t written = fwrite(te_data, 1, te_len, f);
        int close_error = fclose(f);
        if (written != te_len || close_error != 0) {
            te_message("Save failed");
            te_draw();
            return 0;
        }
    }

    te_dirty = 0;
    te_close_armed = 0;
    te_message("Saved");
    taskbar_set_title(te_win, strrchr(te_path, '/') ?
                      strrchr(te_path, '/') + 1 : te_path);
    te_draw();
    return 1;
}

static void te_destroy(void)
{
    Window old = te_win;

    if (!old)
        return;
    te_visible = 0;
    te_win = None;
    focus_drop(old);
    taskbar_unregister(old);
    XDestroyWindow(sc.dpy, old);
    panel_update();
}

void text_editor_close(void)
{
    if (te_dirty && !te_close_armed) {
        te_close_armed = 1;
        te_message("Unsaved changes - close again to discard, Ctrl+S to save");
        te_draw();
        return;
    }
    te_destroy();
}

void text_editor_hide(void)
{
    if (!te_win || !te_visible)
        return;
    te_visible = 0;
    XUnmapWindow(sc.dpy, te_win);
    taskbar_set_hidden(te_win, 1);
    focus_drop(te_win);
    panel_update();
}

void text_editor_show(void)
{
    if (!te_win)
        return;
    te_visible = 1;
    XMapRaised(sc.dpy, te_win);
    XSetInputFocus(sc.dpy, te_win, RevertToPointerRoot, CurrentTime);
    focus_track(te_win);
    taskbar_set_hidden(te_win, 0);
    panel_update();
    te_draw();
}

int text_editor_open(const char *path)
{
    FILE *f;
    struct stat st;
    size_t n, i;
    XSetWindowAttributes attr;

    if (!path || !*path || !sc.dpy)
        return 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;

    if (fstat(fileno(f), &st) == 0 && st.st_size > (off_t)TE_MAX_SIZE) {
        fclose(f);
        return 0;
    }

    n = fread(te_data, 1, TE_MAX_SIZE + 1, f);
    fclose(f);
    if (n > TE_MAX_SIZE)
        return 0;

    /* Reject binary/control-heavy data; keep ordinary text and UTF-8 bytes. */
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)te_data[i];
        if (c == 0 || (c < 32 && c != '\n' && c != '\r' &&
                       c != '\t' && c != '\f'))
            return 0;
    }

    te_data[n] = '\0';
    te_len = n;
    te_cursor = 0;
    te_first_line = 0;
    te_dirty = 0;
    snprintf(te_path, sizeof(te_path), "%s", path);
    te_message("Opened");

    if (!te_win) {
        memset(&attr, 0, sizeof(attr));
        attr.override_redirect = True;
        attr.background_pixel = cfg.col_background;
        attr.event_mask = ExposureMask | KeyPressMask |
                          ButtonPressMask | PointerMotionMask |
                          StructureNotifyMask;

        te_w = TE_W;
        te_h = TE_H;
        te_maximized = 0;
        te_close_armed = 0;
        te_x = (DisplayWidth(sc.dpy, DefaultScreen(sc.dpy)) - te_w) / 2;
        te_y = (DisplayHeight(sc.dpy, DefaultScreen(sc.dpy)) - te_h) / 2;
        te_restore_x = te_x;
        te_restore_y = te_y;
        te_restore_w = te_w;
        te_restore_h = te_h;
        if (te_x < 0) te_x = 0;
        if (te_y < 0) te_y = 0;
        te_win = XCreateWindow(sc.dpy, sc.root,
            te_x, te_y,
            te_w, te_h, 1, CopyFromParent, InputOutput, CopyFromParent,
            CWOverrideRedirect | CWBackPixel | CWEventMask, &attr);
        if (!te_win)
            return 0;
    }

    taskbar_register(te_win, TASK_APP_EDITOR, "Text Editor");
    text_editor_show();
    return 1;
}

int text_editor_open_state(void) { return te_win != None; }
Window text_editor_window(void) { return te_win; }

int text_editor_expose(XExposeEvent *ev)
{
    if (te_win && ev->window == te_win && ev->count == 0) {
        te_draw();
        return 1;
    }
    return 0;
}

static void te_toggle_maximize(void)
{
    int sw = DisplayWidth(sc.dpy, DefaultScreen(sc.dpy));
    int sh = DisplayHeight(sc.dpy, DefaultScreen(sc.dpy));

    if (!te_maximized) {
        te_restore_x = te_x;
        te_restore_y = te_y;
        te_restore_w = te_w;
        te_restore_h = te_h;

        te_x = 0;
        te_y = 0;
        te_w = sw;
        te_h = sh;
        te_maximized = 1;
    } else {
        te_x = te_restore_x;
        te_y = te_restore_y;
        te_w = te_restore_w;
        te_h = te_restore_h;
        te_maximized = 0;
    }

    XMoveResizeWindow(sc.dpy, te_win, te_x, te_y,
                      (unsigned int)te_w, (unsigned int)te_h);
    te_keep_cursor_visible();
    te_draw();
}

int text_editor_button(XButtonEvent *ev)
{
    int row, col, total, cw;
    size_t start, end;

    if (!te_win || ev->window != te_win)
        return 0;
    if (ev->button != Button1)
        return 1;

    focus_track(te_win);

    if (ev->y < TE_TITLE) {
        if (ev->x >= 8 && ev->x < 8 + 3 * 21) {
            int light = (ev->x - 8) / 21;

            if (light == UI_LIGHT_CLOSE)
                text_editor_close();
            else if (light == UI_LIGHT_MIN)
                text_editor_hide();
            else if (light == UI_LIGHT_MAX)
                te_toggle_maximize();
        } else {
            selfdrag_window(te_win, ev, &te_x, &te_y, te_w, te_h);
        }
        return 1;
    }

    /* Convert the clicked screen position into a text cursor position. */
    row = (ev->y - (TE_TITLE + 17)) / te_line_h();
    if (row < 0)
        row = 0;

    row += te_first_line;
    total = te_line_count();
    if (row >= total)
        row = total - 1;

    start = te_line_start(row);
    end = te_line_end(start);
    cw = te_char_w();

    col = ev->x <= 52 ? 0 : (ev->x - 52 + cw / 2) / cw;
    if (start + (size_t)col > end)
        te_cursor = end;
    else
        te_cursor = start + (size_t)col;

    te_close_armed = 0;
    te_keep_cursor_visible();
    te_draw();
    return 1;
}

int text_editor_motion(XMotionEvent *ev)
{
    int hover = -1;

    if (!te_win || ev->window != te_win)
        return 0;

    if (ev->y < TE_TITLE &&
        ev->x >= 8 && ev->x < 8 + 3 * 21)
        hover = (ev->x - 8) / 21;

    if (hover != te_hover) {
        te_hover = hover;
        te_draw();
    }

    return 1;
}

void text_editor_key(XKeyEvent *ev)
{
    KeySym sym;
    char ch[16];
    int len, line, col;
    size_t start, end;

    if (!te_win || ev->window != te_win)
        return;

    len = XLookupString(ev, ch, sizeof(ch), &sym, NULL);
    te_message("");

    if ((ev->state & ControlMask) && (sym == XK_s || sym == XK_S)) {
        te_save();
        return;
    }
    if ((ev->state & ControlMask) && (sym == XK_q || sym == XK_Q)) {
        text_editor_close();
        return;
    }

    te_cursor_position(&line, &col);
    start = te_line_start(line);
    end = te_line_end(start);

    switch (sym) {
    case XK_Left:
        if (te_cursor) te_cursor--;
        break;
    case XK_Right:
        if (te_cursor < te_len) te_cursor++;
        break;
    case XK_Up:
        if (line > 0) {
            size_t prev = te_line_start(line - 1);
            size_t prev_end = te_line_end(prev);
            te_cursor = prev + (size_t)col;
            if (te_cursor > prev_end) te_cursor = prev_end;
        }
        break;
    case XK_Down:
        if (end < te_len) {
            size_t next = end + 1;
            size_t next_end = te_line_end(next);
            te_cursor = next + (size_t)col;
            if (te_cursor > next_end) te_cursor = next_end;
        }
        break;
    case XK_Home:
        te_cursor = start;
        break;
    case XK_End:
        te_cursor = end;
        break;
    case XK_BackSpace:
        if (te_cursor) {
            memmove(te_data + te_cursor - 1, te_data + te_cursor,
                    te_len - te_cursor);
            te_cursor--;
            te_len--;
            te_dirty = 1;
        }
        break;
    case XK_Delete:
        if (te_cursor < te_len) {
            memmove(te_data + te_cursor, te_data + te_cursor + 1,
                    te_len - te_cursor - 1);
            te_len--;
            te_dirty = 1;
        }
        break;
    case XK_Return:
    case XK_KP_Enter:
        if (te_len < TE_MAX_SIZE) {
            memmove(te_data + te_cursor + 1, te_data + te_cursor,
                    te_len - te_cursor);
            te_data[te_cursor++] = '\n';
            te_len++;
            te_dirty = 1;
        }
        break;
    case XK_Page_Up:
        te_cursor = te_line_start(line > 20 ? line - 20 : 0);
        break;
    case XK_Page_Down: {
        int target = line + 20;
        int total = te_line_count();
        if (target >= total) target = total - 1;
        te_cursor = te_line_start(target);
        break;
    }
    default:
        if (!(ev->state & ControlMask) && len > 0 &&
            te_len + (size_t)len <= TE_MAX_SIZE) {
            int i;
            for (i = 0; i < len; i++) {
                unsigned char c = (unsigned char)ch[i];
                if (c < 32 || c == 127)
                    continue;
                memmove(te_data + te_cursor + 1, te_data + te_cursor,
                        te_len - te_cursor);
                te_data[te_cursor++] = c;
                te_len++;
                te_dirty = 1;
            }
        }
        break;
    }

    te_data[te_len] = '\0';
    if (te_dirty)
        te_close_armed = 0;
    te_keep_cursor_visible();
    te_draw();
}

void text_editor_cleanup(void)
{
    if (te_win) {
        taskbar_unregister(te_win);
        XDestroyWindow(sc.dpy, te_win);
    }
    te_win = None;
    te_visible = 0;
}
