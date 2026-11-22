/*
 * XENUS Security Center - the built-in security application window.
 *
 * Override-redirect like fm.c/msgbox.c (self-drawn title bar, no client
 * decoration).  Sections: protection mode + stats, file scanner with
 * bounded targets, permission toggles, scrollable security log.  Opens
 * from the launcher ("Security Center") or the tray shield; never by
 * itself at startup so task button positions stay stable.
 */
#include "xenus.h"
#include <X11/keysym.h>
#include <xenus/ucore.h>

#include <xenus/event.h>
#include <xenus/plugin.h>
#include <xenus/security.h>
#include <xenus/security_bait.h>
#include <xenus/security_mirage.h>
#include <xenus/security_resource.h>

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SG_W 600
#define SG_H 720
#define SG_TITLE 30
#define SG_PAD 10
#define SG_LOG_Y 600
#define SG_LOG_ROW 16
#define SG_LOG_ROWS 6
#define SG_SCAN_CAP 32
#define SG_MAX_BTNS 24

/* muted Harbor status tones: sage / amber / clay, never neon */
#define SG_OK   0x7fb08aUL
#define SG_WARN 0xc9a86aUL
#define SG_BAD  0xc07a7aUL

typedef struct {
    int x, y, w, h;
    int id;
    char label[32];
} SecBtn;

static Window sg_win;
static int sg_open;
static int sg_x, sg_y;                    /* dragged position */
static SecBtn sg_btns[SG_MAX_BTNS];
static int sg_nbtns;
static int sg_hover = -1;
static int sg_hover_light = -1;
static int sg_log_off;
static char sg_scan_status[196] = "choose a target to scan";
static char sg_auth_password[128];
static char sg_auth_confirm[128];
static char sg_auth_status[128] = "Create a password to enable session lock";
static int sg_auth_focus;

/* internal monotonic clock shared with the audit log */
uint32_t xenus_event_now_ms(void);

static const char *pol_names[XENUS_POL_COUNT] = {
    "Allow untrusted plugins",
    "Block known-bad hashes",
    "Deny suspicious files",
    "Require trusted plugins",
    "Audit every file scan"
};

static void sg_draw(void);
static void sg_wipe(char *buf, size_t len);

static void sg_auth_remove(void)
{
    if (!sg_auth_password[0]) {
        snprintf(sg_auth_status, sizeof(sg_auth_status),
                 "Enter current password first");
    } else if (xenus_auth_disable(sg_auth_password) == 0) {
        snprintf(sg_auth_status, sizeof(sg_auth_status),
                 "Password protection disabled");
        sg_wipe(sg_auth_password, sizeof(sg_auth_password));
        sg_wipe(sg_auth_confirm, sizeof(sg_auth_confirm));
        sg_auth_focus = 0;
    } else {
        snprintf(sg_auth_status, sizeof(sg_auth_status),
                 "Incorrect password - not removed");
        sg_wipe(sg_auth_password, sizeof(sg_auth_password));
    }
    sg_draw();
}

static void sg_wipe(char *buf, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--)
        *p++ = 0;
}

static void sg_auth_submit(void)
{
    if (xenus_auth_enabled()) {
        snprintf(sg_auth_status, sizeof(sg_auth_status),
                 "Password is already enabled");
    } else if (!sg_auth_password[0]) {
        snprintf(sg_auth_status, sizeof(sg_auth_status),
                 "Enter a password first");
    } else if (strcmp(sg_auth_password, sg_auth_confirm) != 0) {
        snprintf(sg_auth_status, sizeof(sg_auth_status),
                 "Passwords do not match");
    } else if (xenus_auth_set_password(sg_auth_password) == 0) {
        snprintf(sg_auth_status, sizeof(sg_auth_status),
                 "Password saved - session lock enabled");
        sg_wipe(sg_auth_password, sizeof(sg_auth_password));
        sg_wipe(sg_auth_confirm, sizeof(sg_auth_confirm));
        sg_auth_focus = 0;
    } else {
        snprintf(sg_auth_status, sizeof(sg_auth_status),
                 "Could not save password");
    }
    sg_draw();
}

/* --------------------------------------------------------------- layout -- */

static void sg_btn(int x, int y, int w, int h, int id, const char *label)
{
    if (sg_nbtns >= SG_MAX_BTNS)
        return;
    sg_btns[sg_nbtns].x = x;
    sg_btns[sg_nbtns].y = y;
    sg_btns[sg_nbtns].w = w;
    sg_btns[sg_nbtns].h = h;
    sg_btns[sg_nbtns].id = id;
    str_copy(sg_btns[sg_nbtns].label, sizeof(sg_btns[0].label), label);
    sg_nbtns++;
}

static void sg_paint_btn(const SecBtn *b, int active, int hover)
{
    int cx = b->x + b->w / 2;
    unsigned long bg = active ? ui_accent()
                      : hover  ? ui_mix(cfg.col_panel_bg, 0xffffffUL, 17)
                               : ui_mix(cfg.col_panel_bg, 0xffffffUL, 10);

    ui_round_fill(sg_win, b->x, b->y, b->w, b->h, 7, bg);
    ui_round_outline(sg_win, b->x, b->y, b->w, b->h, 7,
                     active ? ui_mix(ui_accent(), 0xffffffUL, 18)
                            : ui_mix(cfg.col_panel_bg, 0xffffffUL, 17));
    draw_text(sg_win, cx - text_width(b->label) / 2,
              b->y + (b->h + sc.font_ascent - sc.font_descent) / 2,
              active ? 0xffffffUL : cfg.col_menu_fg, b->label);
}

/* Harbor card: menu-tone surface on the panel-tone window, hairline edge */
static void sg_card(int y, int h)
{
    ui_round_fill(sg_win, SG_PAD, y, SG_W - 2 * SG_PAD, h, 12,
                  ui_mix(cfg.col_panel_bg, 0xffffffUL, 7));
    ui_round_outline(sg_win, SG_PAD, y, SG_W - 2 * SG_PAD, h, 12,
                     ui_mix(cfg.col_panel_bg, 0xffffffUL, 14));
    XSetForeground(sc.dpy, sc.gc, ui_mix(cfg.col_panel_bg, 0xffffffUL, 10));
    XDrawLine(sc.dpy, sg_win, sc.gc, SG_PAD + 12, y + 26,
              SG_W - SG_PAD - 12, y + 26);
}

static void sg_section(int top, const char *title)
{
    draw_text_bold(sg_win, SG_PAD + 12, top + sc.font_ascent,
                   cfg.col_panel_fg, title);
}

/* --------------------------------------------------------------- actions -- */

typedef struct {
    int count, threats, worst;
    char worst_name[64];
} ScanAcc;

static ScanAcc sg_acc;

static void sg_scan_dir(const char *dir, const char *suffix, int cap)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL && sg_acc.count < cap) {
        char path[XENUS_PATH_MAX];
        struct stat st;
        xenus_scan_result_t r;
        size_t len;

        if (e->d_name[0] == '.')
            continue;
        if (suffix) {
            len = strlen(e->d_name);
            if (len < strlen(suffix) ||
                strcmp(e->d_name + len - strlen(suffix), suffix) != 0)
                continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (xenus_scan_file(path, &r) != XENUS_OK)
            continue;
        sg_acc.count++;
        if ((int)r.status >= XENUS_THREAT_SUSPICIOUS)
            sg_acc.threats++;
        if (r.risk > sg_acc.worst) {
            sg_acc.worst = r.risk;
            str_copy(sg_acc.worst_name, sizeof(sg_acc.worst_name),
                     e->d_name);
        }
    }
    closedir(d);
}

static void sg_scan_finish(void)
{
    if (sg_acc.count == 0)
        snprintf(sg_scan_status, sizeof(sg_scan_status),
                 "no files found");
    else
        snprintf(sg_scan_status, sizeof(sg_scan_status),
                 "%d files, %d threats, worst risk %d (%s)",
                 sg_acc.count, sg_acc.threats, sg_acc.worst,
                 sg_acc.worst_name[0] ? sg_acc.worst_name : "-");
}

static void sg_run_scan(int id)
{
    const char *home = getenv("HOME");
    char dir[512];

    memset(&sg_acc, 0, sizeof(sg_acc));
    if (xenus_security_get_scan_mode() == XENUS_SCAN_OFF) {
        snprintf(sg_scan_status, sizeof(sg_scan_status),
                 "scanner is off - switch protection on first");
        sg_draw();
        return;
    }
    if (id == 20) {                        /* plugin directories */
        const char *p = xenus_plugins_search_path();

        while (*p && sg_acc.count < SG_SCAN_CAP) {
            const char *sep = strchr(p, ':');
            size_t dlen = sep ? (size_t)(sep - p) : strlen(p);

            if (dlen > 0 && dlen < sizeof(dir)) {
                memcpy(dir, p, dlen);
                dir[dlen] = '\0';
                sg_scan_dir(dir, ".so", SG_SCAN_CAP);
            }
            if (!sep)
                break;
            p = sep + 1;
        }
    } else if (home && *home) {
        if (id == 21)                      /* ~/Desktop */
            snprintf(dir, sizeof(dir), "%s/Desktop", home);
        else                               /* ~/ top-level files */
            snprintf(dir, sizeof(dir), "%s", home);
        sg_scan_dir(dir, NULL, SG_SCAN_CAP);
    }
    sg_scan_finish();
    sg_draw();
}

static void sg_set_mode(int id)
{
    xenus_scan_mode_t m = id == 10 ? XENUS_SCAN_ALWAYS_ON
                       : id == 11 ? XENUS_SCAN_PASSIVE
                                  : XENUS_SCAN_OFF;

    xenus_security_set_scan_mode(m);
    if (m == XENUS_SCAN_ALWAYS_ON)
        xenus_security_watch_defaults();
    panel_update();                        /* tray shield colour */
    sg_draw();
}

/* Detection toggles: user keeps full control.  Enabling lazily creates
 * only tmp-scoped dummy decoys (never user data); disabling leaves the
 * decoys inert but on disk until shutdown. */
static void sg_toggle_bait(void)
{
    if (!xenus_bait_active()) {
        if (xenus_bait_init(NULL) == XENUS_OK)
            xenus_bait_create_defaults();
    } else if (xenus_bait_enabled()) {
        xenus_bait_disable();
    } else {
        xenus_bait_enable();
    }
    sg_draw();
}

static void sg_toggle_mirage(void)
{
    if (!xenus_mirage_active()) {
        if (xenus_mirage_init() == XENUS_OK)
            xenus_mirage_create_defaults();
    } else if (xenus_mirage_enabled()) {
        xenus_mirage_disable();
    } else {
        xenus_mirage_enable();
    }
    sg_draw();
}

/* Short prefix so decoy/sandbox/policy hits read like the spec examples:
 * [BAIT] suspicious process accessed decoy, [MIRAGE] probe, ... */
static const char *sg_prefix(int event)
{
    switch (event) {
    case XENUS_AUDIT_BAIT_CREATED:
    case XENUS_AUDIT_BAIT_ACCESSED:
    case XENUS_AUDIT_BAIT_MODIFIED:
    case XENUS_AUDIT_BAIT_DELETED:
    case XENUS_AUDIT_BAIT_EXECUTION_ATTEMPT:
    case XENUS_AUDIT_BAIT_PERMISSION_DENIED:
        return "[BAIT]";
    case XENUS_AUDIT_MIRAGE_CREATED:
    case XENUS_AUDIT_MIRAGE_PROBED:
    case XENUS_AUDIT_MIRAGE_ACCESSED:
    case XENUS_AUDIT_MIRAGE_MODIFIED:
    case XENUS_AUDIT_MIRAGE_ESCAPE_ATTEMPT:
        return "[MIRAGE]";
    case XENUS_AUDIT_SANDBOX_CREATED:
    case XENUS_AUDIT_SANDBOX_FAILED:
    case XENUS_AUDIT_PROCESS_RESTRICTED:
    case XENUS_AUDIT_PROCESS_BLOCKED:
        return "[SANDBOX]";
    case XENUS_AUDIT_CAPABILITY_GRANTED:
    case XENUS_AUDIT_CAPABILITY_DENIED:
    case XENUS_AUDIT_POLICY_DENIED:
    case XENUS_AUDIT_TRUST_REQUIRED:
    case XENUS_AUDIT_POLICY_FALLBACK:
    case XENUS_AUDIT_SECURITY_FEATURE_UNAVAILABLE:
        return "[POLICY]";
    default:
        return "[SCAN]";
    }
}

/* ---------------------------------------------------------------- paint -- */

static void sg_draw(void)
{
    xenus_scan_stats_t st;
    xenus_scan_mode_t mode;
    int rules = 0, hashes = 0, trusted = 0;
    int watch_on;
    int i;
    char buf[196];
    const char *mode_names[3] = { "always-on", "passive", "off" };
    /* Harbor card stack: quiet two-tone surfaces, generous breathing room */
    const int prot_y = 34, prot_h = 132;
    const int det_y = 174, det_h = 78;
    const int scan_y = 260, scan_h = 80;
    const int perm_y = 348, perm_h = 134;
    const int auth_y = 490, auth_h = 70;
    const int log_y = 568, log_h = 140;
    const int cx0 = SG_PAD + 10;
    const int cx1 = SG_W - SG_PAD - 10;

    if (!sg_win || !sg_open)
        return;
    sg_nbtns = 0;

    ui_round_fill(sg_win, 0, 0, SG_W, SG_H, 14, cfg.col_panel_bg);
    ui_round_fill_gradient(sg_win, 0, 0, SG_W, SG_TITLE, 14,
                           ui_mix(cfg.col_title_bg, 0xffffffUL, 8),
                           ui_mix(cfg.col_title_bg, 0x000000UL, 6));
    XSetForeground(sc.dpy, sc.gc, ui_mix(cfg.col_title_bg, 0xffffffUL, 9));
    XDrawLine(sc.dpy, sg_win, sc.gc, 12, 0, SG_W - 12, 0);
    XSetForeground(sc.dpy, sc.gc, ui_mix(cfg.col_title_bg, 0x000000UL, 34));
    XDrawLine(sc.dpy, sg_win, sc.gc, 0, SG_TITLE - 1, SG_W, SG_TITLE - 1);

    ui_draw_traffic(sg_win, 14, SG_TITLE / 2, 1, sg_hover_light, 0,
                    cfg.col_title_bg);
    {
        int tw = text_width_bold("Security");

        draw_text_bold(sg_win, (SG_W - tw) / 2,
                       (SG_TITLE + sc.font_ascent - sc.font_descent) / 2,
                       cfg.col_title_fg, "Security");
    }

    mode = xenus_security_get_scan_mode();
    watch_on = xenus_security_watch_fd() >= 0;

    /* protection card: segmented mode + live status lines */
    sg_card(prot_y, prot_h);
    sg_section(prot_y + 4, "Protection");
    {
        /* status pill, top right of the card: calm dot + mode + watcher */
        unsigned long mc = mode == XENUS_SCAN_ALWAYS_ON ? SG_OK
                         : mode == XENUS_SCAN_PASSIVE  ? SG_WARN
                                                      : cfg.col_title_fg_inactive;
        char pill[96];
        int pw;

        snprintf(pill, sizeof(pill), "%s  -  watcher %s",
                 mode_names[(int)mode], watch_on ? "active" : "off");
        pw = text_width(pill) + 14;
        XSetForeground(sc.dpy, sc.gc, mc);
        XFillRectangle(sc.dpy, sg_win, sc.gc, cx1 - pw - 10,
                       prot_y + 8, 6, 6);
        draw_text(sg_win, cx1 - pw, prot_y + 8 + sc.font_ascent, mc,
                  pill);
    }
    sg_btn(cx0, prot_y + 28, 110, 22, 10, "Always-on");
    sg_btn(cx0 + 118, prot_y + 28, 110, 22, 11, "Passive");
    sg_btn(cx0 + 236, prot_y + 28, 110, 22, 12, "Off");
    for (i = 0; i < sg_nbtns; i++)
        if (sg_btns[i].id >= 10 && sg_btns[i].id <= 12)
            sg_paint_btn(&sg_btns[i], (sg_btns[i].id - 10) == (int)mode,
                         sg_hover == i);

    xenus_security_stats(&st);
    snprintf(buf, sizeof(buf),
             "scans %u    threats %u    blocked %u    denied %u    "
             "rejected plugins %u",
             (unsigned)st.scans, (unsigned)st.threats,
             (unsigned)st.blocked, (unsigned)st.denied,
             (unsigned)st.rejected_plugins);
    draw_text(sg_win, cx0, prot_y + 66, cfg.col_menu_fg, buf);
    xenus_security_db_info(&rules, &hashes, &trusted);
    snprintf(buf, sizeof(buf), "rules %d    bad hashes %d    trusted %d",
             rules, hashes, trusted);
    draw_text(sg_win, cx0, prot_y + 82, cfg.col_menu_fg, buf);

    /* honest enforcement status: what the kernel/sandbox can really do
     * here (probes ran once at init) - never claim isolation we lack */
    {
        xenus_security_features_t f;

        xenus_security_features(&f);
        snprintf(buf, sizeof(buf),
                 "kernel: %s   sandbox: %s   scanner: %s%s",
                 xenus_feature_state_name(f.kernel_enforcement),
                 xenus_feature_state_name(f.sandbox),
                 mode == XENUS_SCAN_OFF ? "off" : "active",
                 f.proot ? "   (proot: limited)" : "");
        fit_text(buf, sizeof(buf), buf, cx1 - cx0);
        draw_text(sg_win, cx0, prot_y + 98, cfg.col_title_fg_inactive,
                  buf);
    }
    {
        char lim[96];

        xenus_resource_describe(lim, sizeof(lim));
        fit_text(buf, sizeof(buf), lim, cx1 - cx0);
        draw_text(sg_win, cx0, prot_y + 114, cfg.col_title_fg_inactive,
                  buf);
    }

    /* detection card (advisory only: bait/mirage detect, never isolate) */
    sg_card(det_y, det_h);
    sg_section(det_y + 4, "Detection (advisory, not a sandbox)");
    {
        int bait_on = xenus_bait_enabled();
        int mir_on = xenus_mirage_enabled();

        snprintf(buf, sizeof(buf), "Bait: %s  hits: %d",
                 !xenus_bait_active() ? "OFF" : bait_on ? "ON" : "OFF",
                 xenus_bait_hits());
        draw_text(sg_win, cx0, det_y + 44, cfg.col_menu_fg, buf);
        sg_btn(cx1 - 118, det_y + 30, 56, 16, 30,
               bait_on ? "ON" : "OFF");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], bait_on,
                     sg_hover == sg_nbtns - 1);
        sg_btn(cx1 - 56, det_y + 30, 56, 16, 32, "Check");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], 0,
                     sg_hover == sg_nbtns - 1);

        snprintf(buf, sizeof(buf), "Mirage: %s  probes: %d",
                 !xenus_mirage_active() ? "OFF" : mir_on ? "ON" : "OFF",
                 xenus_mirage_probes());
        draw_text(sg_win, cx0, det_y + 66, cfg.col_menu_fg, buf);
        sg_btn(cx1 - 118, det_y + 52, 56, 16, 31,
               mir_on ? "ON" : "OFF");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], mir_on,
                     sg_hover == sg_nbtns - 1);
    }

    /* scanner card */
    sg_card(scan_y, scan_h);
    sg_section(scan_y + 4, "Scanner");
    sg_btn(cx0, scan_y + 28, 118, 22, 20, "Scan plugins");
    sg_btn(cx0 + 126, scan_y + 28, 118, 22, 21, "Scan Desktop");
    sg_btn(cx0 + 252, scan_y + 28, 118, 22, 22, "Scan Home");
    for (i = 0; i < sg_nbtns; i++)
        if (sg_btns[i].id >= 20 && sg_btns[i].id <= 22)
            sg_paint_btn(&sg_btns[i], 0, sg_hover == i);
    fit_text(buf, sizeof(buf), sg_scan_status, cx1 - cx0);
    draw_text(sg_win, cx0, scan_y + 68, cfg.col_menu_fg, buf);

    /* permissions card */
    sg_card(perm_y, perm_h);
    sg_section(perm_y + 4, "Permissions");
    for (i = 0; i < XENUS_POL_COUNT; i++) {
        int y = perm_y + 28 + i * 20;
        int on = xenus_policy_get(i);

        draw_text(sg_win, cx0, y + 14, cfg.col_menu_fg, pol_names[i]);
        sg_btn(cx1 - 56, y + 2, 56, 16, 100 + i,
               on ? "ON" : "OFF");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], on, sg_hover == sg_nbtns - 1);
    }

    /* Account Protection */
    sg_card(auth_y, auth_h);
    sg_section(auth_y + 4, "Account Protection");
    {
        int fy = auth_y + 27;
        int fw = 218;
        int x1 = cx0;
        int x2 = x1 + fw + 12;
        int bx = cx1 - 88;
        char mask1[128], mask2[128];

        memset(mask1, '*', strlen(sg_auth_password));
        mask1[strlen(sg_auth_password)] = '\0';
        memset(mask2, '*', strlen(sg_auth_confirm));
        mask2[strlen(sg_auth_confirm)] = '\0';

        draw_text(sg_win, x1, fy - 3, cfg.col_title_fg_inactive,
                  xenus_auth_enabled() ? "Current password" : "Password");
        draw_text(sg_win, x2, fy - 3, cfg.col_title_fg_inactive,
                  xenus_auth_enabled() ? " " : "Confirm");

        ui_round_fill(sg_win, x1, fy, fw, 24, 6,
                      ui_mix(cfg.col_panel_bg, 0xffffffUL, 10));
        ui_round_fill(sg_win, x2, fy, fw, 24, 6,
                      ui_mix(cfg.col_panel_bg, 0xffffffUL, 10));

        XSetForeground(sc.dpy, sc.gc,
            sg_auth_focus == 1 ? ui_accent() : ui_mix(cfg.col_panel_bg, 0xffffffUL, 18));
        XDrawRectangle(sc.dpy, sg_win, sc.gc, x1, fy, fw, 24);
        XSetForeground(sc.dpy, sc.gc,
            sg_auth_focus == 2 ? ui_accent() : ui_mix(cfg.col_panel_bg, 0xffffffUL, 18));
        XDrawRectangle(sc.dpy, sg_win, sc.gc, x2, fy, fw, 24);

        draw_text(sg_win, x1 + 8, fy + 16, cfg.col_menu_fg,
                  mask1[0] ? mask1 : "Enter password");
        draw_text(sg_win, x2 + 8, fy + 16, cfg.col_menu_fg,
                  mask2[0] ? mask2 : "Repeat password");

        if (!xenus_auth_enabled()) {
            sg_btn(bx, fy, 88, 24, 300, "Save");
            sg_paint_btn(&sg_btns[sg_nbtns - 1], 0,
                         sg_hover == sg_nbtns - 1);
        } else {
            sg_btn(bx, fy, 88, 24, 301, "Remove");
            sg_paint_btn(&sg_btns[sg_nbtns - 1], 0,
                         sg_hover == sg_nbtns - 1);
        }
        fit_text(buf, sizeof(buf), sg_auth_status, cx1 - cx0);
        draw_text(sg_win, cx0, auth_y + 64,
                  cfg.col_title_fg_inactive, buf);
    }

    /* log card */
    sg_card(log_y, log_h);
    sg_section(log_y + 4, "Log");
    sg_btn(cx1 - 60, log_y + 8, 60, 18, 200, "Clear");
    sg_paint_btn(&sg_btns[sg_nbtns - 1], 0, sg_hover == sg_nbtns - 1);
    {
        int n = xenus_audit_count();
        int end = n - sg_log_off;
        int start;
        uint32_t now = xenus_event_now_ms();

        if (end > n)
            end = n;
        if (end < 0)
            end = 0;
        start = end - SG_LOG_ROWS;
        if (start < 0)
            start = 0;
        if (n == 0)
            draw_text(sg_win, cx0, SG_LOG_Y + 12,
                      cfg.col_title_fg_inactive, "audit log is empty");
        for (i = start; i < end; i++) {
            xenus_audit_entry_t e;
            int row = SG_LOG_Y + (i - start) * SG_LOG_ROW;
            unsigned long age;
            unsigned long color;
            char line[196];

            if (xenus_audit_get(i, &e) != XENUS_OK)
                continue;
            age = (unsigned long)((now - e.time_ms) / 1000u);
            color = e.risk >= 50 ? SG_BAD : cfg.col_menu_fg;
            snprintf(line, sizeof(line), "%s %lu s  %s  r%d  %s",
                     sg_prefix(e.event), age,
                     xenus_audit_event_name(e.event), e.risk, e.subject);
            fit_text(buf, sizeof(buf), line, cx1 - cx0);
            draw_text(sg_win, cx0, row + 12, color, buf);
        }
    }

    ui_round_outline(sg_win, 0, 0, SG_W, SG_H, 14,
                     ui_mix(cfg.col_panel_bg, 0xffffffUL, 20));
    XFlush(sc.dpy);
}

/* --------------------------------------------------------------- public -- */

void security_gui_show(void)
{
    if (!sg_win) {
        XSetWindowAttributes wa;

        wa.override_redirect = True;
        wa.background_pixel = cfg.col_panel_bg;
        wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                        KeyPressMask;
        sg_x = (sc.sw - SG_W) / 2;
        if (sg_x < 0)
            sg_x = 0;
        /* keep the title bar reachable: pinned below the menu bar, and
           never pushed above it even when the window is taller than the
           space between the menu bar and the dock (the dock may then
           cover the window's lower part, like a floating panel) */
        sg_y = (sc.sh - SG_H) / 2 - 40;
        if (sg_y < reserve_top())
            sg_y = reserve_top();
        if (sg_y + SG_H > sc.sh - reserve_bottom())
            sg_y = sc.sh - reserve_bottom() - SG_H;
        if (sg_y < reserve_top())
            sg_y = reserve_top();
        sg_win = XCreateWindow(sc.dpy, sc.root, sg_x, sg_y,
                               SG_W, SG_H, 0, CopyFromParent, InputOutput,
                               CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask,
                               &wa);
        XStoreName(sc.dpy, sg_win, "XENUS Security");
        ui_shape_round(sg_win, SG_W, SG_H, 14);
    }
    sg_open = 1;
    sg_hover = -1;
    sg_hover_light = -1;
    XMapRaised(sc.dpy, sg_win);
    focus_track(sg_win);
    taskbar_register(sg_win, TASK_APP_SECURITY, "XENUS Security");
    taskbar_set_hidden(sg_win, 0);
    panel_update();
    XFlush(sc.dpy);
    sg_draw();
}

void security_gui_toggle(void)
{
    if (sg_open)
        security_gui_close();
    else
        security_gui_show();
}

/*
 * Hide the window but keep its taskbar entry (minimise semantics): the
 * entry stays and a click on it brings the window back.
 */
void security_gui_hide(void)
{
    if (!sg_open)
        return;
    sg_open = 0;
    focus_drop(sg_win);
    /* focus falls back to the last focused client window, if any */
    if (sc.focused && !sc.focused->minimized)
        focus_track(sc.focused->win);
    taskbar_register(sg_win, TASK_APP_SECURITY, "XENUS Security");
    taskbar_set_hidden(sg_win, 1);
    panel_update();
    XUnmapWindow(sc.dpy, sg_win);
    XFlush(sc.dpy);
}

/*
 * Close the window and drop its taskbar entry.  The X window object is
 * kept for a fast reopen; reopening registers exactly one fresh entry.
 */
void security_gui_close(void)
{
    if (sg_win == None)
        return;
    if (sg_open) {
        sg_open = 0;
        focus_drop(sg_win);
        XUnmapWindow(sc.dpy, sg_win);
        XFlush(sc.dpy);
    }
    taskbar_unregister(sg_win);
    panel_update();
}

int security_gui_open(void)
{
    return sg_open;
}

Window security_gui_window(void)
{
    return sg_win;
}

void security_gui_redraw(void)
{
    if (sg_open)
        sg_draw();
}

void security_gui_cleanup(void)
{
    if (sg_win != None) {
        taskbar_unregister(sg_win);
        XDestroyWindow(sc.dpy, sg_win);
        sg_win = None;
    }
    sg_open = 0;
}

/* --------------------------------------------------------------- events -- */

int security_gui_expose(XExposeEvent *ev)
{
    if (!sg_win || ev->window != sg_win)
        return 0;
    if (ev->count == 0)
        sg_draw();
    return 1;
}

int security_gui_motion(XMotionEvent *ev)
{
    int i, found = -1;
    int hc = -1;

    if (!sg_open || ev->window != sg_win)
        return 0;
    if (ev->y < SG_TITLE) {
        int cell = 21;
        int left = 8;

        if (ev->x >= left && ev->x < left + 3 * cell)
            hc = (ev->x - left) / cell;
    }
    for (i = 0; i < sg_nbtns; i++) {
        if (ev->x >= sg_btns[i].x && ev->x < sg_btns[i].x + sg_btns[i].w &&
            ev->y >= sg_btns[i].y && ev->y < sg_btns[i].y + sg_btns[i].h) {
            found = i;
            break;
        }
    }
    if (found != sg_hover || hc != sg_hover_light) {
        sg_hover = found;
        sg_hover_light = hc;
        sg_draw();
    }
    return 1;
}

int security_gui_button(XButtonEvent *ev)
{
    int i;

    if (!sg_open || ev->window != sg_win)
        return 0;
    if (ev->button == Button4 || ev->button == Button5) {
        int n = xenus_audit_count();

        if (ev->button == Button4 && sg_log_off + SG_LOG_ROWS < n)
            sg_log_off++;
        else if (ev->button == Button5 && sg_log_off > 0)
            sg_log_off--;
        sg_draw();
        return 1;
    }
    if (ev->button != Button1)
        return 1;
    focus_track(sg_win);
    if (ev->y >= 517 && ev->y < 541) {
        int x1 = SG_PAD + 10;
        int x2 = x1 + 218 + 12;
        if (ev->x >= x1 && ev->x < x1 + 218)
            sg_auth_focus = 1;
        else if (!xenus_auth_enabled() &&
                 ev->x >= x2 && ev->x < x2 + 218)
            sg_auth_focus = 2;
        else
            sg_auth_focus = 0;
        sg_draw();
        if (sg_auth_focus)
            return 1;
    }
    if (ev->y < SG_TITLE) {
        int cell = 21;
        int left = 8;

        if (ev->x >= left && ev->x < left + 3 * cell) {
            int light = (ev->x - left) / cell;

            if (light == UI_LIGHT_CLOSE)
                security_gui_close();
            else if (light == UI_LIGHT_MIN)
                security_gui_hide();
        } else {
            selfdrag_window(sg_win, ev, &sg_x, &sg_y, SG_W, SG_H);
        }
        return 1;
    }
    for (i = 0; i < sg_nbtns; i++) {
        SecBtn *b = &sg_btns[i];

        if (ev->x < b->x || ev->x >= b->x + b->w ||
            ev->y < b->y || ev->y >= b->y + b->h)
            continue;
        if (b->id >= 10 && b->id <= 12)
            sg_set_mode(b->id);
        else if (b->id >= 20 && b->id <= 22)
            sg_run_scan(b->id);
        else if (b->id == 30)
            sg_toggle_bait();
        else if (b->id == 31)
            sg_toggle_mirage();
        else if (b->id == 32) {
            xenus_bait_check();          /* manual detection sweep */
            sg_draw();
        } else if (b->id >= 100 && b->id < 100 + XENUS_POL_COUNT) {
            int key = b->id - 100;

            xenus_policy_set(key, xenus_policy_get(key) ? 0 : 1);
            sg_draw();
        } else if (b->id == 200) {
            xenus_audit_clear();
            sg_log_off = 0;
            sg_draw();
        } else if (b->id == 300) {
            sg_auth_submit();
        } else if (b->id == 301) {
            sg_auth_remove();
        }
        return 1;
    }
    return 1;
}

void security_gui_key(XKeyEvent *ev)
{
    char tmp[8];
    KeySym sym = NoSymbol;
    int n;

    if (!sg_open)
        return;
    n = XLookupString(ev, tmp, sizeof(tmp) - 1, &sym, NULL);

    if (sg_auth_focus) {
        char *target = sg_auth_focus == 1 ? sg_auth_password : sg_auth_confirm;
        size_t len = strlen(target);

        if (sym == XK_Escape) {
            sg_wipe(sg_auth_password, sizeof(sg_auth_password));
            sg_wipe(sg_auth_confirm, sizeof(sg_auth_confirm));
            sg_auth_focus = 0;
            sg_draw();
            return;
        }
        if (sym == XK_Tab) {
            sg_auth_focus = sg_auth_focus == 1 ? 2 : 1;
            sg_draw();
            return;
        }
        if (sym == XK_Return || sym == XK_KP_Enter) {
            if (xenus_auth_enabled()) {
                snprintf(sg_auth_status, sizeof(sg_auth_status),
                         "Press Remove to disable password");
            } else if (sg_auth_focus == 1) {
                sg_auth_focus = 2;
            } else {
                sg_auth_submit();
            }
            sg_draw();
            return;
        }
        if (sym == XK_BackSpace) {
            if (len)
                target[len - 1] = '\0';
            sg_draw();
            return;
        }
        if (n > 0) {
            int i;
            for (i = 0; i < n && len < sizeof(sg_auth_password) - 1; i++) {
                unsigned char c = (unsigned char)tmp[i];
                if (c >= 32 && c <= 126)
                    target[len++] = (char)c;
            }
            target[len] = '\0';
            sg_draw();
        }
        return;
    }

    if (sym == XK_Escape || sym == XK_q)
        security_gui_close();
}
