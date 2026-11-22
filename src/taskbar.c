/*
 * XENUS - taskbar window registry
 *
 * Single source of truth for everything that shows up as a task button:
 * the reparented external clients (wm.c) and XENUS's own override-redirect
 * application windows (file manager, security center).
 *
 * Design notes:
 *  - Managed clients are NOT copied into a second list.  taskbar_sync()
 *    composes the task list from sc.clients plus the small table of internal
 *    windows, so the panel keeps showing exactly the same buttons for the
 *    windows it showed before (same order, same close button, same
 *    minimise/restore behaviour) and additionally lists the internal apps.
 *  - An internal window is identified by its X window id, never by its
 *    title: two windows of the same application stay two separate entries,
 *    while re-registering the same window id never duplicates an entry.
 *  - Entries are a fixed array (no allocation in the event path) and the
 *    registry holds hidden windows too: a minimised/hidden window keeps its
 *    task button and the click handler restores it.
 */
#include "xenus.h"

#include <stdio.h>
#include <string.h>

#define TASKBAR_MAX         (MAX_PANEL_BTNS)
#define TASKBAR_INTERNAL_MAX 32

static TaskInternal internals[TASKBAR_INTERNAL_MAX];
static TaskEntry tasks[TASKBAR_MAX];
static int n_tasks;

/* last published task list, so the root property only changes when the
 * set of taskbar entries actually changes (tests poll it) */
static Window published[TASKBAR_MAX];
static int n_published = -1;

/* ---------------------------------------------------------------- lookup */

static int internal_index(Window w)
{
    int i;

    for (i = 0; i < TASKBAR_INTERNAL_MAX; i++)
        if (internals[i].win == w && internals[i].win != None)
            return i;
    return -1;
}

int taskbar_internal_count(void)
{
    return TASKBAR_INTERNAL_MAX;
}

const TaskInternal *taskbar_internal_at(int i)
{
    if (i < 0 || i >= TASKBAR_INTERNAL_MAX)
        return NULL;
    if (internals[i].win == None)
        return NULL;
    return &internals[i];
}

/* ------------------------------------------------------------- mutation */

void taskbar_register(Window w, int app_type, const char *title)
{
    int i;
    int free_slot = -1;

    if (w == None)
        return;
    i = internal_index(w);
    if (i >= 0) {                        /* already listed: update in place,
                                           never create a second entry */
        if (title)
            str_copy(internals[i].title, sizeof(internals[i].title), title);
        return;
    }
    for (i = 0; i < TASKBAR_INTERNAL_MAX; i++) {
        if (internals[i].win == None) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (internals[i].app_type == app_type) {
            /* the module owns one window at a time: the new id replaces it */
            internals[i].win = None;
            if (free_slot < 0)
                free_slot = i;
        }
    }
    if (free_slot < 0)
        return;                           /* registry full: ignore, never crash */
    internals[free_slot].win = w;
    internals[free_slot].app_type = app_type;
    internals[free_slot].hidden = 0;
    str_copy(internals[free_slot].title, sizeof(internals[free_slot].title),
             title ? title : "Window");
}

void taskbar_unregister(Window w)
{
    int i = internal_index(w);

    if (i >= 0)
        internals[i].win = None;
}

void taskbar_set_title(Window w, const char *title)
{
    int i = internal_index(w);

    if (i >= 0 && title)
        str_copy(internals[i].title, sizeof(internals[i].title), title);
}

void taskbar_set_hidden(Window w, int hidden)
{
    int i = internal_index(w);

    if (i >= 0)
        internals[i].hidden = hidden ? 1 : 0;
}

void taskbar_clear(void)
{
    memset(internals, 0, sizeof(internals));
    n_published = -1;
    n_tasks = 0;
}

/* ----------------------------------------------------------------- sync */

void taskbar_sync(void)
{
    Client *c;
    int i;
    int n = 0;

    for (c = sc.clients; c && n < TASKBAR_MAX; c = c->next) {
        tasks[n].win = c->win;
        tasks[n].client = c;
        tasks[n].app_type = TASK_APP_CLIENT;
        tasks[n].hidden = c->minimized;
        /* exactly the window that owns the X input focus: an internal app
           (File Manager / Security) holding focus must not leave the last
           client marked active as well */
        tasks[n].focused = (sc.focus_win == c->win);
        str_copy(tasks[n].title, sizeof(tasks[n].title), c->name);
        n++;
    }
    for (i = 0; i < TASKBAR_INTERNAL_MAX && n < TASKBAR_MAX; i++) {
        if (internals[i].win == None)
            continue;
        tasks[n].win = internals[i].win;
        tasks[n].client = NULL;
        tasks[n].app_type = internals[i].app_type;
        tasks[n].hidden = internals[i].hidden;
        tasks[n].focused = (sc.focus_win == internals[i].win);
        str_copy(tasks[n].title, sizeof(tasks[n].title), internals[i].title);
        n++;
    }
    n_tasks = n;

    /* publish the entry set on the root window for tests/debugging, only
     * when it actually changed */
    if (sc.dpy && sc.root != None && sc.atom_xenus_task_list != None) {
        int changed = (n != n_published);

        if (!changed) {
            for (i = 0; i < n; i++) {
                if (published[i] != tasks[i].win) {
                    changed = 1;
                    break;
                }
            }
        }
        if (changed) {
            if (n == 0) {
                XDeleteProperty(sc.dpy, sc.root, sc.atom_xenus_task_list);
            } else {
                Window wins[TASKBAR_MAX];

                for (i = 0; i < n; i++)
                    wins[i] = tasks[i].win;
                XChangeProperty(sc.dpy, sc.root, sc.atom_xenus_task_list,
                                XA_WINDOW, 32, PropModeReplace,
                                (unsigned char *)wins, n);
                XFlush(sc.dpy);
            }
            for (i = 0; i < n; i++)
                published[i] = tasks[i].win;
            n_published = n;
        }
    }
}

int taskbar_count(void)
{
    if (n_tasks == 0)
        taskbar_sync();
    return n_tasks;
}

/* title of the focused entry for the menu bar (NULL when none has focus) */
const char *taskbar_active_title(void)
{
    int i;

    taskbar_sync();
    for (i = 0; i < n_tasks; i++)
        if (tasks[i].focused)
            return tasks[i].title;
    return NULL;
}

const TaskEntry *taskbar_at(int i)
{
    if (i < 0 || i >= n_tasks)
        return NULL;
    return &tasks[i];
}

/* ------------------------------------------------------ application key -- */

int taskbar_app_key(int i, char *out, size_t out_size)
{
    const TaskEntry *t;

    if (!out || out_size == 0)
        return -1;
    out[0] = '\0';
    taskbar_sync();
    t = taskbar_at(i);
    if (!t)
        return -1;
    if (t->client) {
        if (t->client->app_class[0])
            snprintf(out, out_size, "app:%s", t->client->app_class);
        else if (t->client->app_instance[0])
            snprintf(out, out_size, "instance:%s", t->client->app_instance);
        else
            snprintf(out, out_size, "window:%s", t->title);
    } else {
        switch (t->app_type) {
        case TASK_APP_FM: snprintf(out, out_size, "xenus:fm"); break;
        case TASK_APP_SECURITY: snprintf(out, out_size, "xenus:security"); break;
        case TASK_APP_SYSTEM: snprintf(out, out_size, "xenus:system"); break;
        case TASK_APP_EDITOR: snprintf(out, out_size, "xenus:editor"); break;
        case TASK_APP_SETTINGS: snprintf(out, out_size, "xenus:settings"); break;
        default: snprintf(out, out_size, "xenus:%d", t->app_type); break;
        }
    }
    return 0;
}

void taskbar_group_state(int i, int *count, int *hidden, int *focused)
{
    char key[96], other[96];
    const TaskEntry *t;
    int n = taskbar_count();
    int c = 0, hid = 1, foc = 0, j;

    if (count) *count = 0;
    if (hidden) *hidden = 0;
    if (focused) *focused = 0;
    if (taskbar_app_key(i, key, sizeof(key)) != 0)
        return;
    for (j = 0; j < n; j++) {
        if (taskbar_app_key(j, other, sizeof(other)) != 0 ||
            strcmp(key, other) != 0)
            continue;
        t = taskbar_at(j);
        if (!t)
            continue;
        c++;
        if (!t->hidden)
            hid = 0;
        if (t->focused)
            foc = 1;
    }
    if (count) *count = c;
    if (hidden) *hidden = hid;
    if (focused) *focused = foc;
}

int taskbar_find_type(int app_type)
{
    int i, n = taskbar_count();

    taskbar_sync();
    for (i = 0; i < n; i++) {
        const TaskEntry *t = taskbar_at(i);

        if (t && !t->client && t->app_type == app_type)
            return i;
    }
    return -1;
}

/* --------------------------------------------------------------- actions */

static const TaskEntry *entry_at(int i)
{
    taskbar_sync();
    return taskbar_at(i);
}

void taskbar_activate(int i)
{
    const TaskEntry *t = entry_at(i);

    if (!t)
        return;
    if (t->client) {
        if (t->client->minimized)
            client_minimize(t->client, 0);
        focus_client(t->client);
        return;
    }
    switch (t->app_type) {
    case TASK_APP_FM:
        fm_show();                        /* maps + raises + focuses */
        break;
    case TASK_APP_SECURITY:
        security_gui_show();
        break;
    case TASK_APP_SYSTEM:
        system_center_show();
        break;
    case TASK_APP_EDITOR:
        text_editor_show();
        break;
    case TASK_APP_SETTINGS:
        settings_show();
        break;
    default:
        break;
    }
}

void taskbar_minimize_entry(int i)
{
    const TaskEntry *t = entry_at(i);

    if (!t)
        return;
    if (t->client) {
        client_minimize(t->client, 1);
        return;
    }
    switch (t->app_type) {
    case TASK_APP_FM: fm_hide(); break;
    case TASK_APP_SECURITY: security_gui_hide(); break;
    case TASK_APP_SYSTEM: system_center_hide(); break;
    case TASK_APP_EDITOR: text_editor_hide(); break;
    case TASK_APP_SETTINGS: settings_hide(); break;
    default: break;
    }
}

void taskbar_maximize_entry(int i)
{
    const TaskEntry *t = entry_at(i);

    if (!t)
        return;
    if (t->client) {
        if (t->client->minimized)
            client_minimize(t->client, 0);
        client_maximize(t->client, !t->client->maximized);
        focus_client(t->client);
    } else {
        /* the built-in panels are fixed-purpose windows */
        taskbar_activate(i);
    }
}

/*
 * Quit asks every window of the application to close through its own
 * WM_DELETE_WINDOW lifecycle.  It never force-kills: a client that does not
 * advertise the protocol simply refuses (see close_client).
 */
void taskbar_quit(int i)
{
    char key[96], other[96];
    Window wins[TASKBAR_MAX];
    int types[TASKBAR_MAX];
    int n = 0, total, j;

    if (taskbar_app_key(i, key, sizeof(key)) != 0)
        return;
    taskbar_sync();
    total = taskbar_count();
    for (j = 0; j < total && n < TASKBAR_MAX; j++) {
        if (taskbar_app_key(j, other, sizeof(other)) != 0 ||
            strcmp(key, other) != 0)
            continue;
        wins[n] = tasks[j].win;
        types[n] = tasks[j].client ? TASK_APP_CLIENT : tasks[j].app_type;
        n++;
    }
    log_msg("quit application '%s' (%d window%s)", key, n, n == 1 ? "" : "s");
    for (j = 0; j < n; j++) {
        if (types[j] == TASK_APP_CLIENT) {
            Client *c = client_find(wins[j]);

            if (c)
                close_client(c);
        } else if (types[j] == TASK_APP_FM) {
            fm_close();
        } else if (types[j] == TASK_APP_SECURITY) {
            security_gui_close();
        } else if (types[j] == TASK_APP_SYSTEM) {
            system_center_close();
        } else if (types[j] == TASK_APP_EDITOR) {
            text_editor_close();
        } else if (types[j] == TASK_APP_SETTINGS) {
            settings_close();
        }
    }
}

void taskbar_close(int i)
{
    const TaskEntry *t = entry_at(i);

    if (!t)
        return;
    if (t->client) {
        close_client(t->client);
        return;
    }
    switch (t->app_type) {
    case TASK_APP_FM:
        fm_close();
        break;
    case TASK_APP_SECURITY:
        security_gui_close();
        break;
    case TASK_APP_SYSTEM:
        system_center_close();
        break;
    case TASK_APP_EDITOR:
        text_editor_close();
        break;
    case TASK_APP_SETTINGS:
        settings_close();
        break;
    default:
        break;
    }
}