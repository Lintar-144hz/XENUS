/*
 * UCore - process viewer / manager.
 *
 * Everything is read from /proc; no ps or top subprocess.  CPU percentages
 * are deltas between two snapshots, so a caller that lists processes once
 * gets a useful value on the next refresh.
 */
#include "core_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROC_HISTORY XENUS_CORE_MAX_PROC

typedef struct {
    int pid;
    unsigned long long ticks;
} proc_hist_t;

static proc_hist_t hist[PROC_HISTORY];
static int hist_n;
static unsigned long long last_total;

static int numeric_name(const char *s)
{
    if (!s || !*s)
        return 0;
    while (*s) {
        if (!isdigit((unsigned char)*s))
            return 0;
        s++;
    }
    return 1;
}

static int read_cmdline(int pid, char *out, size_t size)
{
    char path[64];
    FILE *f;
    size_t n = 0;

    out[0] = '\0';
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = fread(out, 1, size - 1, f);
    fclose(f);
    if (n == 0)
        return 0;
    out[n] = '\0';
    while (n > 0) {
        n--;
        if (out[n] == '\0')
            out[n] = ' ';
    }
    return 0;
}

static unsigned int read_uid(int pid)
{
    char path[64];
    FILE *f;
    char line[128];
    unsigned int uid = 0;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            if (sscanf(line + 4, "%u", &uid) != 1)
                uid = 0;
            break;
        }
    }
    fclose(f);
    return uid;
}

static unsigned long long hist_lookup(int pid)
{
    int i;

    for (i = 0; i < hist_n; i++)
        if (hist[i].pid == pid)
            return hist[i].ticks;
    return 0;
}

static void hist_store(int pid, unsigned long long ticks)
{
    int i;

    for (i = 0; i < hist_n; i++) {
        if (hist[i].pid == pid) {
            hist[i].ticks = ticks;
            return;
        }
    }
    if (hist_n < PROC_HISTORY) {
        hist[hist_n].pid = pid;
        hist[hist_n].ticks = ticks;
        hist_n++;
    }
}

int core_proc_snapshot(xenus_core_proc_t *out, int max)
{
    DIR *d;
    struct dirent *e;
    int n = 0;
    unsigned long long total = core_cpu_total_ticks();
    unsigned long long dtotal = (last_total && total > last_total)
                                ? total - last_total : 0;
    int cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    unsigned long long mem_total = 0;
    xenus_core_load_t load;

    if (!out || max <= 0)
        return 0;
    if (cpus < 1)
        cpus = 1;
    if (xenus_core_load(&load) == 0)
        mem_total = load.mem.total_kb;

    d = opendir("/proc");
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL && n < max) {
        char path[64];
        FILE *f;
        char stat[512];
        char *open_paren;
        char *close_paren;
        char *tail;
        char *save = NULL;
        char *tok;
        int field;
        int pid;
        unsigned long long utime = 0, stime = 0, ticks;
        long rss_pages = 0;
        int threads = 0;
        char state = '?';
        int ppid = 0;
        xenus_core_proc_t *p;

        if (!numeric_name(e->d_name))
            continue;
        pid = atoi(e->d_name);
        if (pid <= 0)
            continue;
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (!fgets(stat, sizeof(stat), f)) {
            fclose(f);
            continue;
        }
        fclose(f);
        open_paren = strchr(stat, '(');
        close_paren = strrchr(stat, ')');
        if (!open_paren || !close_paren || close_paren < open_paren)
            continue;

        p = &out[n];
        memset(p, 0, sizeof(*p));
        p->pid = pid;
        {
            size_t len = (size_t)(close_paren - open_paren - 1);
            if (len >= sizeof(p->name))
                len = sizeof(p->name) - 1;
            memcpy(p->name, open_paren + 1, len);
            p->name[len] = '\0';
        }

        tail = close_paren + 1;
        field = 2;                        /* comm consumed; next is state */
        tok = strtok_r(tail, " ", &save);
        while (tok) {
            field++;
            if (field == 3)
                state = tok[0];
            else if (field == 4)
                ppid = atoi(tok);
            else if (field == 14)
                utime = strtoull(tok, NULL, 10);
            else if (field == 15)
                stime = strtoull(tok, NULL, 10);
            else if (field == 20)
                threads = atoi(tok);
            else if (field == 24)
                rss_pages = atol(tok);
            tok = strtok_r(NULL, " ", &save);
        }
        p->state = state;
        p->ppid = ppid;
        p->threads = threads > 0 ? threads : 1;
        p->kernel_thread = (pid == 2 || ppid == 2);
        p->uid = read_uid(pid);
        ticks = utime + stime;
        p->cpu_ticks = ticks;
        if (dtotal > 0) {
            unsigned long long old_ticks = hist_lookup(pid);
            unsigned long long dt = ticks > old_ticks
                                    ? ticks - old_ticks : 0;
            p->cpu_percent = (double)dt * 100.0 *
                             (double)cpus / (double)dtotal;
        }
        hist_store(pid, ticks);
        if (rss_pages > 0) {
            long page_kb = sysconf(_SC_PAGESIZE) / 1024;
            p->rss_kb = (unsigned long)rss_pages *
                        (unsigned long)(page_kb > 0 ? page_kb : 4);
        }
        if (mem_total > 0)
            p->mem_percent = (double)p->rss_kb * 100.0 /
                             (double)mem_total;
        read_cmdline(pid, p->cmdline, sizeof(p->cmdline));
        n++;
    }
    closedir(d);
    last_total = total;

    /* top-like ordering: busiest first, then memory */
    {
        int i, j;

        for (i = 1; i < n; i++) {
            xenus_core_proc_t key = out[i];

            j = i - 1;
            while (j >= 0 &&
                   (out[j].cpu_percent < key.cpu_percent ||
                    (out[j].cpu_percent == key.cpu_percent &&
                     out[j].mem_percent < key.mem_percent))) {
                out[j + 1] = out[j];
                j--;
            }
            out[j + 1] = key;
        }
    }
    return n;
}

int xenus_core_processes(xenus_core_proc_t *out, int max)
{
    return core_proc_snapshot(out, max);
}

int xenus_core_process_get(int pid, xenus_core_proc_t *out)
{
    xenus_core_proc_t list[XENUS_CORE_MAX_PROC];
    int n, i;

    if (!out || pid <= 0)
        return XENUS_CORE_ERR_ARG;
    n = core_proc_snapshot(list, XENUS_CORE_MAX_PROC);
    for (i = 0; i < n; i++) {
        if (list[i].pid == pid) {
            *out = list[i];
            return XENUS_CORE_OK;
        }
    }
    return XENUS_CORE_ERR_UNAVAILABLE;
}

const char *xenus_core_process_action_name(xenus_core_proc_action_t action)
{
    switch (action) {
    case XENUS_PROC_TERM: return "end";
    case XENUS_PROC_KILL: return "kill";
    case XENUS_PROC_STOP: return "suspend";
    case XENUS_PROC_CONT: return "resume";
    }
    return "?";
}

int xenus_core_process_is_critical(int pid)
{
    return pid <= 1 || pid == (int)getpid() ||
           pid == (int)getppid();
}

xenus_core_status_t xenus_core_process_action(int pid,
                                            xenus_core_proc_action_t action)
{
    int sig;
    char text[160];

    if (pid <= 0)
        return XENUS_CORE_ERR_ARG;
    if (xenus_core_process_is_critical(pid))
        return XENUS_CORE_ERR_DENIED;
    switch (action) {
    case XENUS_PROC_TERM: sig = SIGTERM; break;
    case XENUS_PROC_KILL: sig = SIGKILL; break;
    case XENUS_PROC_STOP: sig = SIGSTOP; break;
    case XENUS_PROC_CONT: sig = SIGCONT; break;
    default: return XENUS_CORE_ERR_ARG;
    }
    if (kill(pid, sig) != 0) {
        if (errno == ESRCH)
            return XENUS_CORE_ERR_UNAVAILABLE;
        if (errno == EPERM)
            return XENUS_CORE_ERR_DENIED;
        return XENUS_CORE_ERR;
    }
    snprintf(text, sizeof(text), "%s pid %d",
             xenus_core_process_action_name(action), pid);
    xenus_core_log(2, "process: %s", text);
    xenus_core_event_emit(XENUS_CORE_EV_PROCESS, "process", text);
    return XENUS_CORE_OK;
}
