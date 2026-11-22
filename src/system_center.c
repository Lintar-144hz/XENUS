/*
 * XENUS System Center - the built-in system management application.
 *
 * UI only: all facts and actions come from UCore (src/core).  The window is
 * override-redirect like the File Manager and Security Center, so it uses
 * the same taskbar registry and self-drag helper.
 */
#include "xenus.h"

#include <xenus/notification.h>
#include <xenus/ucore.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SC_W       780
#define SC_H       580
#define SC_TITLE   32
#define SC_SIDE    158
#define SC_PAD     16
#define SC_ROW     30
#define SC_ROWS    12
#define SC_MAXBTN  32

enum {
    SC_TAB_OVERVIEW = 0,
    SC_TAB_PROCESS,
    SC_TAB_APPS,
    SC_TAB_SERVICES,
    SC_TAB_DEVICE,
    SC_TAB_ACTIVITY,
    SC_TAB_COUNT
};

typedef struct {
    int x, y, w, h;
    int id;
    char label[40];
} ScBtn;

static Window sc_win;
static int sc_open;
static int sc_x, sc_y;
static int sc_tab;
static int sc_hover = -1;
static ScBtn sc_btns[SC_MAXBTN];
static int sc_nbtns;
static char sc_status[200] = "UCore runtime ready";

static xenus_core_proc_t sc_procs[XENUS_CORE_MAX_PROC];
static int sc_nprocs;
static xenus_core_app_t sc_apps[XENUS_CORE_MAX_APPS];
static int sc_napps;
static xenus_core_service_t sc_svcs[XENUS_CORE_MAX_SERVICES];
static int sc_nsvcs;
static xenus_core_module_t sc_mods[XENUS_CORE_MAX_MODULES];
static int sc_nmods;
static xenus_core_log_entry_t sc_logs[XENUS_CORE_LOG_RING];
static int sc_nlogs;
static xenus_core_load_t sc_load;
static xenus_core_hardware_t sc_hw;
static xenus_core_sysinfo_t sc_info;
static int sc_proc_sel = -1;
static int sc_proc_off;
static int sc_app_sel = -1;
static int sc_app_off;
static int sc_svc_off;
static int sc_log_off;
static uint64_t sc_last_ms;
static uint64_t sc_last_apps_ms;

static void sc_draw(void);

static uint64_t sc_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sc_btn(int x, int y, int w, int h, int id, const char *label)
{
    if (sc_nbtns >= SC_MAXBTN)
        return;
    sc_btns[sc_nbtns].x = x;
    sc_btns[sc_nbtns].y = y;
    sc_btns[sc_nbtns].w = w;
    sc_btns[sc_nbtns].h = h;
    sc_btns[sc_nbtns].id = id;
    str_copy(sc_btns[sc_nbtns].label, sizeof(sc_btns[0].label), label);
    sc_nbtns++;
}

static const ScBtn *sc_btn_at(int x, int y)
{
    int i;

    for (i = 0; i < sc_nbtns; i++)
        if (x >= sc_btns[i].x && x < sc_btns[i].x + sc_btns[i].w &&
            y >= sc_btns[i].y && y < sc_btns[i].y + sc_btns[i].h)
            return &sc_btns[i];
    return NULL;
}

static int content_x(void) { return SC_SIDE + SC_PAD; }
static int content_w(void) { return SC_W - content_x() - SC_PAD; }

static unsigned long surface(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL,
                  8);
}

static unsigned long line_color(void)
{
    return ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL : 0xffffffUL,
                  20);
}

static unsigned long muted(void)
{
    return ui_dim(cfg.col_panel_fg, cfg.col_panel_bg);
}

static void sc_card(int x, int y, int w, int h)
{
    ui_round_fill(sc_win, x, y, w, h, 12, surface());
    ui_round_outline(sc_win, x, y, w, h, 12, line_color());
}

static void sc_text(int x, int y, const char *s, unsigned long c)
{
    draw_text(sc_win, x, y, c, s);
}

static void sc_small(int x, int y, const char *s, unsigned long c)
{
    draw_text_small(sc_win, x, y, c, s);
}

static void sc_bold(int x, int y, const char *s, unsigned long c)
{
    draw_text_bold(sc_win, x, y, c, s);
}

static void sc_clip(int x, int max_x, int y, unsigned long c, const char *s)
{
    draw_text_clip(sc_win, x, max_x, y, c, s);
}

static void sc_gauge(int x, int y, int w, double pct, unsigned long color)
{
    int fill;

    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    ui_round_fill(sc_win, x, y, w, 8, 4,
                  ui_mix(cfg.col_panel_bg, cfg.col_panel_fg, 14));
    fill = (int)((double)w * pct / 100.0);
    if (fill > 0)
        ui_round_fill(sc_win, x, y, fill < 8 ? 8 : fill, 8, 4, color);
}

static void sc_status_line(void)
{
    int y = SC_H - 22;

    XSetForeground(sc.dpy, sc.gc, line_color());
    XDrawLine(sc.dpy, sc_win, sc.gc, content_x(), y - 10,
              SC_W - SC_PAD, y - 10);
    sc_small(content_x(), y + 2, sc_status, muted());
}

static void sc_tabs(void)
{
    static const char *names[SC_TAB_COUNT] = {
        "Overview", "Processes", "Applications", "Services", "Device",
        "Activity"
    };
    int i;
    int x = 12;
    int y = SC_TITLE + 14;
    int w = SC_SIDE - 24;
    int bh = 34;

    ui_round_fill(sc_win, 0, SC_TITLE, SC_SIDE, SC_H - SC_TITLE, 0,
                  ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL :
                         0xffffffUL, 4));
    sc_bold(x + 4, SC_TITLE + 28, "System Center", cfg.col_panel_fg);
    sc_small(x + 4, SC_TITLE + 44, "UCore 1.0", muted());
    y = SC_TITLE + 62;

    for (i = 0; i < SC_TAB_COUNT; i++) {
        int active = i == sc_tab;
        unsigned long bg = active ? ui_accent() : surface();

        ui_round_fill(sc_win, x, y, w, bh, 8, bg);
        if (!active)
            ui_round_outline(sc_win, x, y, w, bh, 8, line_color());
        draw_text(sc_win, x + 12, y + (bh + sc.font_ascent -
                                       sc.font_descent) / 2,
                  active ? 0xffffffUL : cfg.col_panel_fg, names[i]);
        y += bh + 4;
    }
    sc_small(x + 4, SC_H - 46, "XENUS · ARM64", muted());
    sc_small(x + 4, SC_H - 30, "C11 + Xlib", muted());
}

/* ------------------------------------------------------------- overview -- */

static void sc_draw_overview(void)
{
    int x = content_x();
    int y = SC_TITLE + 18;
    int w = content_w();
    int half = (w - 12) / 2;
    char buf[160];
    unsigned long acc = ui_accent();

    sc_bold(x, y + sc.font_ascent, "Overview", cfg.col_panel_fg);
    sc_small(x + 120, y + sc.font_ascent,
             "shared UCore snapshot", muted());
    y += 24;

    sc_card(x, y, half, 92);
    sc_small(x + 14, y + 20, "CPU", muted());
    snprintf(buf, sizeof(buf), "%.1f%%", sc_load.cpu_percent);
    draw_text_bold(sc_win, x + 14, y + 48, cfg.col_panel_fg, buf);
    snprintf(buf, sizeof(buf), "%d cores · load %.2f", sc_info.cpus,
             sc_load.load1);
    sc_small(x + 14, y + 68, buf, muted());
    sc_gauge(x + 14, y + 76, half - 28, sc_load.cpu_percent, acc);

    sc_card(x + half + 12, y, half, 92);
    sc_small(x + half + 26, y + 20, "MEMORY", muted());
    snprintf(buf, sizeof(buf), "%.1f%%  %lu / %lu MB",
             sc_load.mem_percent,
             (unsigned long)((sc_load.mem.total_kb -
                              (sc_load.mem.available_kb
                               ? sc_load.mem.available_kb
                               : sc_load.mem.free_kb)) / 1024),
             (unsigned long)(sc_load.mem.total_kb / 1024));
    draw_text_bold(sc_win, x + half + 26, y + 48, cfg.col_panel_fg, buf);
    sc_small(x + half + 26, y + 68, "available + cache reclaimable",
             muted());
    sc_gauge(x + half + 26, y + 76, half - 28, sc_load.mem_percent,
             sc_load.mem_percent > 88 ? ui_red() : acc);
    y += 104;

    {
        int colw = (w - 12) / 2;
        int rows = 5;
        int row;
        struct {
            char k[40];
            char v[96];
        } left[5], right[5];

        snprintf(left[0].k, sizeof(left[0].k), "%s", "Uptime");
        {
            unsigned long up = sc_load.uptime_sec;
            snprintf(left[0].v, sizeof(left[0].v), "%lud %02luh %02lum",
                     up / 86400, (up / 3600) % 24, (up / 60) % 60);
        }
        snprintf(left[1].k, sizeof(left[1].k), "%s", "Visible processes");
        snprintf(left[1].v, sizeof(left[1].v), "%d", sc_nprocs);
        snprintf(left[2].k, sizeof(left[2].k), "%s", "Load average");
        snprintf(left[2].v, sizeof(left[2].v), "%.2f  %.2f  %.2f",
                 sc_load.load1, sc_load.load5, sc_load.load15);
        snprintf(left[3].k, sizeof(left[3].k), "%s", "UCore");
        snprintf(left[3].v, sizeof(left[3].v), "%s · %d modules",
                 xenus_core_version(), sc_nmods);
        snprintf(left[4].k, sizeof(left[4].k), "%s", "Runtime path");
        snprintf(left[4].v, sizeof(left[4].v), "%s",
                 xenus_core_config_path()[0] ? xenus_core_config_path() :
                 "built-in");

        snprintf(right[0].k, sizeof(right[0].k), "%s", "Battery");
        if (sc_hw.battery == XENUS_HW_AVAILABLE)
            snprintf(right[0].v, sizeof(right[0].v), "%d%% %s",
                     sc_hw.battery_percent, sc_hw.battery_status);
        else
            snprintf(right[0].v, sizeof(right[0].v), "not exposed");
        snprintf(right[1].k, sizeof(right[1].k), "%s", "Volume");
        if (sc_hw.volume == XENUS_HW_AVAILABLE)
            snprintf(right[1].v, sizeof(right[1].v), "%d%%%s",
                     sc_hw.volume_percent, sc_hw.muted ? " mute" : "");
        else
            snprintf(right[1].v, sizeof(right[1].v), "not exposed");
        snprintf(right[2].k, sizeof(right[2].k), "%s", "Wi-Fi");
        if (sc_hw.wifi == XENUS_HW_AVAILABLE)
            snprintf(right[2].v, sizeof(right[2].v), "%s %s",
                     sc_hw.wifi_on ? "on" : "off",
                     sc_hw.wifi_iface[0] ? sc_hw.wifi_iface : "");
        else
            snprintf(right[2].v, sizeof(right[2].v), "not exposed");
        snprintf(right[3].k, sizeof(right[3].k), "%s", "Platform");
        snprintf(right[3].v, sizeof(right[3].v), "%s%s%s",
                 sc_info.android ? "Android" : "Linux",
                 sc_info.proot ? " · PRoot" : "",
                 sc_info.termux ? " · Termux" : "");
        snprintf(right[4].k, sizeof(right[4].k), "%s", "Display");
        snprintf(right[4].v, sizeof(right[4].v), "%dx%d · workspace %d",
                 sc.sw, sc.sh, workspace_current() + 1);

        for (row = 0; row < rows && row < 5; row++) {
            int ry = y + 22 + row * 26;

            sc_small(x, ry, left[row].k, muted());
            sc_text(x + colw / 2 - 8 - text_width(left[row].v), ry,
                    left[row].v, cfg.col_panel_fg);
            sc_small(x + colw + 12, ry, right[row].k, muted());
            sc_text(x + colw + 12 + colw - 12 -
                    text_width(right[row].v), ry, right[row].v,
                    cfg.col_panel_fg);
        }
        ui_round_fill(sc_win, x + colw, y, 1, 150, 0, line_color());
        y += 172;
    }

    sc_btn(x, y, 150, 32, 10, "Control Center");
    sc_btn(x + 158, y, 118, 32, 11, "Refresh");
    sc_status_line();
}

/* ------------------------------------------------------------ processes -- */

static void sc_draw_processes(void)
{
    int x = content_x();
    int y = SC_TITLE + 18;
    int w = content_w();
    int row;
    char buf[160];

    sc_bold(x, y + sc.font_ascent, "Processes", cfg.col_panel_fg);
    snprintf(buf, sizeof(buf), "%d visible · sorted by CPU · PID %d",
             sc_nprocs, (int)getpid());
    sc_small(x + 96, y + sc.font_ascent, buf, muted());
    y += 30;

    sc_small(x + 8, y, "PID", muted());
    sc_small(x + 66, y, "NAME", muted());
    sc_small(x + w - 240, y, "CPU", muted());
    sc_small(x + w - 158, y, "MEM", muted());
    sc_small(x + w - 90, y, "STATE", muted());
    y += 8;
    XSetForeground(sc.dpy, sc.gc, line_color());
    XDrawLine(sc.dpy, sc_win, sc.gc, x, y, x + w, y);
    y += 6;

    for (row = 0; row < SC_ROWS; row++) {
        int idx = sc_proc_off + row;
        int ry = y + row * SC_ROW;
        xenus_core_proc_t *p;

        if (idx >= sc_nprocs)
            break;
        p = &sc_procs[idx];
        if (idx == sc_proc_sel) {
            ui_round_fill(sc_win, x, ry, w, SC_ROW - 4, 6,
                          ui_mix(cfg.col_panel_bg, ui_accent(), 26));
        } else if (row & 1) {
            ui_round_fill(sc_win, x, ry, w, SC_ROW - 4, 6, surface());
        }
        snprintf(buf, sizeof(buf), "%d", p->pid);
        sc_text(x + 8, ry + 18, buf, cfg.col_panel_fg);
        sc_clip(x + 66, x + w - 258, ry + 18,
                p->uid == 0 ? ui_yellow() : cfg.col_panel_fg, p->name);
        snprintf(buf, sizeof(buf), "%.1f%%", p->cpu_percent);
        sc_text(x + w - 240, ry + 18, buf,
                p->cpu_percent > 50 ? ui_red() : cfg.col_panel_fg);
        snprintf(buf, sizeof(buf), "%.1f%%", p->mem_percent);
        sc_text(x + w - 158, ry + 18, buf, cfg.col_panel_fg);
        snprintf(buf, sizeof(buf), "%c", p->state);
        sc_text(x + w - 90, ry + 18, buf, muted());
        if (xenus_core_process_is_critical(p->pid))
            sc_small(x + w - 50, ry + 18, "core", muted());
    }

    y += SC_ROWS * SC_ROW + 12;
    sc_btn(x, y, 110, 32, 20, "Refresh");
    sc_btn(x + 118, y, 92, 32, 21, "End");
    sc_btn(x + 218, y, 92, 32, 22, "Suspend");
    sc_btn(x + 318, y, 92, 32, 23, "Resume");
    if (sc_proc_sel >= 0 && sc_proc_sel < sc_nprocs) {
        snprintf(buf, sizeof(buf), "Selected %d · %s", sc_procs[sc_proc_sel].pid,
                 sc_procs[sc_proc_sel].name);
        sc_small(x + 430, y + 20, buf, muted());
    }
    sc_status_line();
}

/* -------------------------------------------------------------- apps ---- */

static void sc_draw_apps(void)
{
    int x = content_x();
    int y = SC_TITLE + 18;
    int w = content_w();
    int row;
    char buf[200];

    sc_bold(x, y + sc.font_ascent, "Applications", cfg.col_panel_fg);
    snprintf(buf, sizeof(buf), "%d installed · .desktop registry", sc_napps);
    sc_small(x + 108, y + sc.font_ascent, buf, muted());
    y += 30;

    for (row = 0; row < SC_ROWS; row++) {
        int idx = sc_app_off + row;
        int ry = y + row * 34;
        xenus_core_app_t *a;

        if (idx >= sc_napps)
            break;
        a = &sc_apps[idx];
        if (idx == sc_app_sel)
            ui_round_fill(sc_win, x, ry, w, 30, 6,
                          ui_mix(cfg.col_panel_bg, ui_accent(), 26));
        else if (row & 1)
            ui_round_fill(sc_win, x, ry, w, 30, 6, surface());
        sc_text(x + 8, ry + 19, a->name, cfg.col_panel_fg);
        sc_clip(x + 250, x + w - 104, ry + 19, muted(), a->exec);
        if (a->running) {
            ui_round_fill(sc_win, x + w - 92, ry + 8, 64, 16, 8,
                          ui_mix(cfg.col_panel_bg, ui_green(), 30));
            sc_small(x + w - 84, ry + 19, "running", ui_green());
        } else {
            sc_small(x + w - 88, ry + 19, "idle", muted());
        }
    }

    y += SC_ROWS * 34 + 12;
    sc_btn(x, y, 110, 32, 30, "Refresh");
    sc_btn(x + 118, y, 110, 32, 31, "Launch");
    if (sc_app_sel >= 0 && sc_app_sel < sc_napps)
        sc_small(x + 240, y + 20, sc_apps[sc_app_sel].exec, muted());
    sc_status_line();
}

/* ----------------------------------------------------------- services --- */

static void sc_draw_services(void)
{
    int x = content_x();
    int y = SC_TITLE + 18;
    int w = content_w();
    int row;

    sc_bold(x, y + sc.font_ascent, "Services & Configuration",
            cfg.col_panel_fg);
    sc_small(x + 180, y + sc.font_ascent,
             "UCore modules + detected host services", muted());
    y += 30;

    for (row = 0; row < 14; row++) {
        int idx = sc_svc_off + row;
        int ry = y + row * 30;
        xenus_core_service_t *s;
        const char *st;

        if (idx >= sc_nsvcs)
            break;
        s = &sc_svcs[idx];
        st = s->status;
        if (row & 1)
            ui_round_fill(sc_win, x, ry, w, 26, 6, surface());
        sc_text(x + 8, ry + 18, s->name, cfg.col_panel_fg);
        {
            unsigned long c = strcmp(st, "running") == 0 ? ui_green()
                              : strcmp(st, "stopped") == 0 ? ui_red()
                              : strcmp(st, "available") == 0 ? ui_yellow()
                                                             : muted();
            sc_small(x + 150, ry + 18, st, c);
        }
        sc_clip(x + 250, x + w - 12, ry + 18, muted(), s->detail);
        if (s->managed)
            sc_small(x + w - 62, ry + 18, "core", muted());
    }

    y += 14 * 30 + 10;
    sc_btn(x, y, 110, 32, 40, "Refresh");
    sc_small(x + 130, y + 20, "Service control is delegated to the host.",
             muted());
    sc_status_line();
}

/* ------------------------------------------------------------- device --- */

static void sc_draw_device(void)
{
    int x = content_x();
    int y = SC_TITLE + 18;
    int w = content_w();
    int i;
    struct {
        char k[40];
        char v[160];
    } rows[14];

    sc_bold(x, y + sc.font_ascent, "Device & System", cfg.col_panel_fg);
    sc_small(x + 150, y + sc.font_ascent,
             "facts from procfs, sysfs and uname", muted());
    y += 34;

#define ROW(n, key, ...) do { \
        snprintf(rows[n].k, sizeof(rows[n].k), "%s", key); \
        snprintf(rows[n].v, sizeof(rows[n].v), __VA_ARGS__); \
    } while (0)
    ROW(0, "Operating system", "%s", sc_info.os);
    ROW(1, "Kernel", "%s", sc_info.kernel);
    ROW(2, "Architecture", "%s", sc_info.arch);
    ROW(3, "Hostname", "%s", sc_info.hostname);
    ROW(4, "CPU", "%s", sc_info.cpu_model);
    ROW(5, "Cores", "%d", sc_info.cpus);
    ROW(6, "Memory", "%lu MB", sc_info.mem_total_mb);
    ROW(7, "Uptime", "%lu s", sc_info.uptime_sec);
    ROW(8, "Platform", "%s%s%s", sc_info.android ? "Android" : "Linux",
        sc_info.proot ? " + PRoot" : "", sc_info.termux ? " + Termux" : "");
    ROW(9, "Android bridge", "%s",
        xenus_core_status_str(XENUS_CORE_OK));
    ROW(10, "Display server", "%s", getenv("DISPLAY") ?
        getenv("DISPLAY") : "unavailable");
    ROW(11, "Display", "%dx%d · %d workspaces", sc.sw, sc.sh,
        workspace_count());
    ROW(12, "UCore", "%s · config %s", xenus_core_version(),
        xenus_core_config_path()[0] ? xenus_core_config_path() : "built-in");
    ROW(13, "Hardware", "wifi %s · bt %s · battery %s",
        sc_hw.wifi == XENUS_HW_AVAILABLE ? "yes" : "no",
        sc_hw.bluetooth == XENUS_HW_AVAILABLE ? "yes" : "no",
        sc_hw.battery == XENUS_HW_AVAILABLE ? "yes" : "no");
#undef ROW

    for (i = 0; i < 14; i++) {
        int ry = y + i * 27;

        if (i & 1)
            ui_round_fill(sc_win, x, ry - 14, w, 24, 5, surface());
        sc_small(x + 10, ry, rows[i].k, muted());
        sc_clip(x + 210, x + w - 10, ry, cfg.col_panel_fg, rows[i].v);
    }
    y += 14 * 27 + 10;
    sc_btn(x, y, 110, 32, 50, "Refresh");
    if (sc_hw.detail[0])
        sc_small(x + 130, y + 20, sc_hw.detail, muted());
    sc_status_line();
}

/* ------------------------------------------------------------ activity --- */

static void sc_draw_activity(void)
{
    int x = content_x();
    int y = SC_TITLE + 18;
    int w = content_w();
    int row;

    sc_bold(x, y + sc.font_ascent, "Activity", cfg.col_panel_fg);
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "%d UCore log entries", sc_nlogs);
        sc_small(x + 84, y + sc.font_ascent, buf, muted());
    }
    y += 30;

    for (row = 0; row < 14; row++) {
        int idx = sc_log_off + row;
        int ry = y + row * 28;
        xenus_core_log_entry_t *l;

        if (idx >= sc_nlogs)
            break;
        l = &sc_logs[idx];
        if (row & 1)
            ui_round_fill(sc_win, x, ry, w, 24, 5, surface());
        sc_small(x + 8, ry + 16,
                 l->level >= 3 ? "ERR" : l->level == 2 ? "WRN" :
                 l->level == 1 ? "INF" : "DBG",
                 l->level >= 3 ? ui_red() : l->level == 2 ? ui_yellow()
                 : ui_accent());
        sc_clip(x + 54, x + w - 10, ry + 16, cfg.col_panel_fg, l->text);
    }
    y += 14 * 28 + 10;
    sc_btn(x, y, 110, 32, 60, "Clear log");
    sc_btn(x + 118, y, 110, 32, 61, "Refresh");
    sc_status_line();
}

/* --------------------------------------------------------------- draw --- */

static void sc_draw(void)
{
    unsigned long bg = cfg.col_panel_bg;

    if (!sc_win)
        return;
    sc_nbtns = 0;
    XSetForeground(sc.dpy, sc.gc, bg);
    XFillRectangle(sc.dpy, sc_win, sc.gc, 0, 0, SC_W, SC_H);

    /* title bar */
    XSetForeground(sc.dpy, sc.gc,
                   ui_mix(cfg.col_panel_bg, cfg.theme ? 0x000000UL :
                          0xffffffUL, 5));
    XFillRectangle(sc.dpy, sc_win, sc.gc, 0, 0, SC_W, SC_TITLE);
    ui_draw_traffic(sc_win, 12, SC_TITLE / 2, 1, -1, 0, bg);
    sc_bold(80, (SC_TITLE + sc.font_ascent - sc.font_descent) / 2,
            "XENUS System Center", cfg.col_panel_fg);
    sc_small(SC_W - 170, (SC_TITLE + sc.font_ascent -
                          sc.font_descent) / 2,
             "UCore · live", muted());

    sc_tabs();
    switch (sc_tab) {
    case SC_TAB_PROCESS: sc_draw_processes(); break;
    case SC_TAB_APPS: sc_draw_apps(); break;
    case SC_TAB_SERVICES: sc_draw_services(); break;
    case SC_TAB_DEVICE: sc_draw_device(); break;
    case SC_TAB_ACTIVITY: sc_draw_activity(); break;
    default: sc_draw_overview(); break;
    }
    XFlush(sc.dpy);
}

/* ------------------------------------------------------------- refresh -- */

static void sc_refresh(int all)
{
    uint64_t now = sc_now();

    xenus_core_load(&sc_load);
    sc_info = *xenus_core_sysinfo();
    sc_nmods = xenus_core_modules(sc_mods, XENUS_CORE_MAX_MODULES);
    sc_nsvcs = xenus_core_services(sc_svcs, XENUS_CORE_MAX_SERVICES);
    sc_nlogs = xenus_core_log_entries(sc_logs, XENUS_CORE_LOG_RING);
    sc_nprocs = xenus_core_processes(sc_procs, XENUS_CORE_MAX_PROC);
    if (all || sc_tab == SC_TAB_APPS ||
        (now - sc_last_apps_ms > 4000)) {
        sc_napps = xenus_core_apps_refresh();
        if (sc_napps > XENUS_CORE_MAX_APPS)
            sc_napps = XENUS_CORE_MAX_APPS;
        xenus_core_apps(sc_apps, XENUS_CORE_MAX_APPS);
        sc_last_apps_ms = now;
    }
    if (sc_proc_sel >= sc_nprocs)
        sc_proc_sel = -1;
    if (sc_app_sel >= sc_napps)
        sc_app_sel = -1;
    sc_last_ms = now;
}

/* ------------------------------------------------------------- actions -- */

static void sc_do_action(int id)
{
    switch (id) {
    case 10:
        control_center_show();
        break;
    case 11:
    case 20:
    case 30:
    case 40:
    case 50:
    case 61:
        sc_refresh(1);
        snprintf(sc_status, sizeof(sc_status), "Refreshed from UCore");
        break;
    case 21:
    case 22:
    case 23:
        if (sc_proc_sel < 0 || sc_proc_sel >= sc_nprocs) {
            snprintf(sc_status, sizeof(sc_status),
                     "Select a process first");
            break;
        } else {
            xenus_core_proc_t *p = &sc_procs[sc_proc_sel];
            xenus_core_proc_action_t act = id == 21 ? XENUS_PROC_TERM
                : id == 22 ? XENUS_PROC_STOP : XENUS_PROC_CONT;
            xenus_core_status_t rc = xenus_core_process_action(p->pid, act);

            if (rc == XENUS_CORE_OK) {
                snprintf(sc_status, sizeof(sc_status),
                         "%s pid %d (%s)", xenus_core_process_action_name(act),
                         p->pid, p->name);
                xenus_notify("Process manager",
                            sc_status);
                sc_refresh(0);
            } else {
                snprintf(sc_status, sizeof(sc_status),
                         "Cannot %s pid %d: %s",
                         xenus_core_process_action_name(act), p->pid,
                         xenus_core_status_str(rc));
            }
        }
        break;
    case 31:
        if (sc_app_sel >= 0 && sc_app_sel < sc_napps) {
            xenus_core_status_t rc = xenus_core_app_launch(sc_app_sel);

            if (rc == XENUS_CORE_OK) {
                snprintf(sc_status, sizeof(sc_status), "Launched %s",
                         sc_apps[sc_app_sel].name);
                xenus_notify("Applications", sc_status);
                sc_napps = xenus_core_apps_refresh();
                xenus_core_apps(sc_apps, XENUS_CORE_MAX_APPS);
            } else {
                snprintf(sc_status, sizeof(sc_status), "Launch failed: %s",
                         xenus_core_status_str(rc));
            }
        } else {
            snprintf(sc_status, sizeof(sc_status), "Select an application");
        }
        break;
    case 60:
        xenus_core_log_clear();
        sc_log_off = 0;
        snprintf(sc_status, sizeof(sc_status), "Activity log cleared");
        break;
    default:
        break;
    }
}

void system_center_show(void)
{
    log_msg("sc show open=%d win=0x%lx", sc_open, (unsigned long)sc_win);
    if (!sc_win) {
        XSetWindowAttributes wa;

        wa.override_redirect = True;
        wa.background_pixel = cfg.col_panel_bg;
        wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                        KeyPressMask;
        sc_x = (sc.sw - SC_W) / 2;
        if (sc_x < 0)
            sc_x = 0;
        sc_y = reserve_top() + 16;
        if (sc_y + SC_H > sc.sh - reserve_bottom())
            sc_y = sc.sh - reserve_bottom() - SC_H - 8;
        if (sc_y < reserve_top())
            sc_y = reserve_top();
        sc_win = XCreateWindow(sc.dpy, sc.root, sc_x, sc_y, SC_W, SC_H, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask,
                               &wa);
        XStoreName(sc.dpy, sc_win, "XENUS System Center");
        ui_shape_round(sc_win, SC_W, SC_H, 14);
    }
    sc_open = 1;
    sc_hover = -1;
    sc_last_ms = sc_now();
    XMapRaised(sc.dpy, sc_win);
    focus_track(sc_win);
    log_msg("sc mapped win=0x%lx", (unsigned long)sc_win);
    taskbar_register(sc_win, TASK_APP_SYSTEM, "System Center");
    taskbar_set_hidden(sc_win, 0);
    panel_update();
    XFlush(sc.dpy);
    sc_draw();
}

void system_center_hide(void)
{
    if (!sc_open)
        return;
    sc_open = 0;
    focus_drop(sc_win);
    if (sc.focused && !sc.focused->minimized)
        focus_track(sc.focused->win);
    taskbar_register(sc_win, TASK_APP_SYSTEM, "System Center");
    taskbar_set_hidden(sc_win, 1);
    panel_update();
    XUnmapWindow(sc.dpy, sc_win);
    XFlush(sc.dpy);
}

void system_center_close(void)
{
    log_msg("sc close open=%d win=0x%lx", sc_open,
            (unsigned long)sc_win);
    if (sc_win == None)
        return;
    if (sc_open) {
        sc_open = 0;
        focus_drop(sc_win);
        XUnmapWindow(sc.dpy, sc_win);
        XFlush(sc.dpy);
    }
    taskbar_unregister(sc_win);
    panel_update();
}

void system_center_toggle(void)
{
    if (sc_open)
        system_center_close();
    else
        system_center_show();
}

int system_center_open(void)
{
    return sc_open;
}

Window system_center_window(void)
{
    return sc_win;
}

void system_center_redraw(void)
{
    if (sc_open)
        sc_draw();
}

void system_center_tick(void)
{
    uint64_t now;

    if (!sc_open)
        return;
    now = sc_now();
    if (sc_last_ms && now - sc_last_ms < 1000)
        return;
    sc_refresh(0);
    sc_draw();
}

void system_center_cleanup(void)
{
    if (sc_win != None) {
        taskbar_unregister(sc_win);
        XDestroyWindow(sc.dpy, sc_win);
        sc_win = None;
    }
    sc_open = 0;
}

/* -------------------------------------------------------------- events -- */

int system_center_expose(XExposeEvent *ev)
{
    if (!sc_win || ev->window != sc_win)
        return 0;
    if (ev->count == 0)
        sc_draw();
    return 1;
}

int system_center_motion(XMotionEvent *ev)
{
    int i;
    int found = -1;

    if (!sc_open || ev->window != sc_win)
        return 0;
    for (i = 0; i < sc_nbtns; i++) {
        if (ev->x >= sc_btns[i].x && ev->x < sc_btns[i].x + sc_btns[i].w &&
            ev->y >= sc_btns[i].y && ev->y < sc_btns[i].y + sc_btns[i].h) {
            found = i;
            break;
        }
    }
    if (found != sc_hover) {
        sc_hover = found;
        sc_draw();
    }
    return 1;
}

int system_center_button(XButtonEvent *ev)
{
    int x, y;
    int wheel = 0;

    if (!sc_open || ev->window != sc_win)
        return 0;
    focus_track(sc_win);

    if (ev->button == Button4 || ev->button == Button5) {
        wheel = ev->button == Button4 ? 1 : -1;
        if (sc_tab == SC_TAB_PROCESS) {
            sc_proc_off -= wheel * 3;
            if (sc_proc_off < 0)
                sc_proc_off = 0;
            if (sc_proc_off > sc_nprocs - SC_ROWS && sc_nprocs > SC_ROWS)
                sc_proc_off = sc_nprocs - SC_ROWS;
        } else if (sc_tab == SC_TAB_APPS) {
            sc_app_off -= wheel * 3;
            if (sc_app_off < 0)
                sc_app_off = 0;
            if (sc_app_off > sc_napps - SC_ROWS && sc_napps > SC_ROWS)
                sc_app_off = sc_napps - SC_ROWS;
        } else if (sc_tab == SC_TAB_SERVICES) {
            sc_svc_off -= wheel;
            if (sc_svc_off < 0)
                sc_svc_off = 0;
            if (sc_svc_off > sc_nsvcs - 14 && sc_nsvcs > 14)
                sc_svc_off = sc_nsvcs - 14;
        } else if (sc_tab == SC_TAB_ACTIVITY) {
            sc_log_off -= wheel;
            if (sc_log_off < 0)
                sc_log_off = 0;
            if (sc_log_off > sc_nlogs - 14 && sc_nlogs > 14)
                sc_log_off = sc_nlogs - 14;
        }
        sc_draw();
        return 1;
    }
    if (ev->button != Button1)
        return 1;

    if (ev->y < SC_TITLE) {
        int cell = 21;
        int left = 6;

        if (ev->x >= left && ev->x < left + 3 * cell) {
            int light = (ev->x - left) / cell;

            if (light == UI_LIGHT_CLOSE)
                system_center_close();
            else if (light == UI_LIGHT_MIN)
                system_center_hide();
        } else {
            selfdrag_window(sc_win, ev, &sc_x, &sc_y, SC_W, SC_H);
        }
        return 1;
    }

    x = (int)ev->x;
    y = (int)ev->y;

    if (x < SC_SIDE) {
        int idx = (y - (SC_TITLE + 62)) / 38;

        if (y >= SC_TITLE + 62 && idx >= 0 && idx < SC_TAB_COUNT) {
            sc_tab = idx;
            sc_status[0] = '\0';
            sc_refresh(0);
            sc_draw();
        }
        return 1;
    }

    {
        const ScBtn *b = sc_btn_at(x, y);

        if (b) {
            sc_do_action(b->id);
            sc_draw();
            return 1;
        }
    }

    if (sc_tab == SC_TAB_PROCESS && y >= SC_TITLE + 62 &&
        y < SC_TITLE + 62 + SC_ROWS * SC_ROW) {
        int idx = sc_proc_off + (y - (SC_TITLE + 62)) / SC_ROW;

        if (idx >= 0 && idx < sc_nprocs) {
            sc_proc_sel = idx;
            sc_draw();
        }
        return 1;
    }
    if (sc_tab == SC_TAB_APPS && y >= SC_TITLE + 62 &&
        y < SC_TITLE + 62 + SC_ROWS * 34) {
        int idx = sc_app_off + (y - (SC_TITLE + 62)) / 34;

        if (idx >= 0 && idx < sc_napps) {
            sc_app_sel = idx;
            sc_draw();
        }
        return 1;
    }
    return 1;
}

void system_center_key(XKeyEvent *ev)
{
    char tmp[8];
    KeySym sym = NoSymbol;

    if (!sc_open)
        return;
    XLookupString(ev, tmp, sizeof(tmp) - 1, &sym, NULL);
    log_msg("sc key sym=0x%lx win=0x%lx", (unsigned long)sym,
            (unsigned long)ev->window);
    if (sym == XK_Escape)
        system_center_close();
    else if (sym == XK_Tab) {
        sc_tab = (sc_tab + 1) % SC_TAB_COUNT;
        sc_refresh(0);
        sc_draw();
    } else if (sym == XK_Up || sym == XK_Down) {
        if (sc_tab == SC_TAB_PROCESS) {
            sc_proc_off += sym == XK_Down ? 1 : -1;
            if (sc_proc_off < 0)
                sc_proc_off = 0;
            if (sc_proc_off > sc_nprocs - SC_ROWS && sc_nprocs > SC_ROWS)
                sc_proc_off = sc_nprocs - SC_ROWS;
            sc_draw();
        }
    }
}
