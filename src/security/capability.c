/*
 * XENUS Security Layer - capabilities, permission checks and process
 * restrictions.
 *
 * Capabilities are per-subject bit masks ("plugin:example", "app:foo",
 * ...).  Unknown subjects hold nothing, so every check fails closed and is
 * audited as CAPABILITY_DENIED.
 *
 * xenus_security_restrict_self() applies real kernel knobs (no_new_privs,
 * zero core dumps) to the calling process.  It is the first building block
 * for future plugin subprocess isolation - the session itself is not
 * restricted by default, and no sandbox is claimed before one exists.
 */
#define _DEFAULT_SOURCE
#include <xenus/security.h>

#include "security_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>

#define SUBJ_LEN 40

typedef struct {
    int used;
    char name[SUBJ_LEN];
    uint32_t caps;
} subject_t;

static subject_t subjects[XENUS_CAP_SUBJECTS];

static const struct {
    const char *name;
    uint32_t cap;
} cap_names[] = {
    { "fs.read", XENUS_CAP_FS_READ },
    { "fs.write", XENUS_CAP_FS_WRITE },
    { "fs.exec", XENUS_CAP_FS_EXEC },
    { "fs.watch", XENUS_CAP_FS_WATCH },
    { "net", XENUS_CAP_NET },
    { "device", XENUS_CAP_DEVICE },
    { "spawn", XENUS_CAP_SPAWN },
    { "config", XENUS_CAP_CONFIG },
    { "audit", XENUS_CAP_AUDIT },
    { "plugin.load", XENUS_CAP_PLUGIN_LOAD }
};

static subject_t *find_subject(const char *name)
{
    int i;

    for (i = 0; i < XENUS_CAP_SUBJECTS; i++) {
        if (subjects[i].used && strcmp(subjects[i].name, name) == 0)
            return &subjects[i];
    }
    return NULL;
}

static void deny(const char *subject, uint32_t caps, const char *why)
{
    xenus__stats()->denied++;
    xenus_audit_log(XENUS_AUDIT_CAPABILITY_DENIED, subject, 0, why);
    (void)caps;
}

int xenus_security_grant(const char *subject, uint32_t caps)
{
    int free_slot = -1;
    int i;

    if (!subject || !*subject || strlen(subject) >= SUBJ_LEN || caps == 0)
        return XENUS_ERR_ARG;
    for (i = 0; i < XENUS_CAP_SUBJECTS; i++) {
        if (!subjects[i].used) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (strcmp(subjects[i].name, subject) == 0) {
            subjects[i].caps |= caps;
            return XENUS_OK;
        }
    }
    if (free_slot < 0)
        return XENUS_ERR_FULL;
    subjects[free_slot].used = 1;
    snprintf(subjects[free_slot].name, sizeof(subjects[free_slot].name),
             "%.*s", SUBJ_LEN - 1, subject);
    subjects[free_slot].caps = caps;
    return XENUS_OK;
}

int xenus_security_revoke(const char *subject, uint32_t caps)
{
    subject_t *s;

    if (!subject || !*subject)
        return XENUS_ERR_ARG;
    s = find_subject(subject);
    if (!s)
        return XENUS_ERR_NOT_FOUND;
    s->caps &= ~caps;
    return XENUS_OK;
}

int xenus_security_has(const char *subject, uint32_t caps)
{
    subject_t *s;

    if (!subject || !*subject || caps == 0)
        return 0;
    s = find_subject(subject);
    return s && (s->caps & caps) == caps;
}

int xenus_security_check(const char *subject, uint32_t caps)
{
    if (!subject || !*subject || caps == 0) {
        deny(subject ? subject : "-", caps, "invalid capability request");
        return XENUS_ERR;
    }
    if ((xenus_security_has(subject, caps) & 1) == 0) {
        deny(subject, caps, "capability not granted");
        return XENUS_ERR;
    }
    return XENUS_OK;
}

uint32_t xenus_capability_from_name(const char *name)
{
    size_t i;

    if (!name)
        return XENUS_CAP_NONE;
    for (i = 0; i < sizeof(cap_names) / sizeof(cap_names[0]); i++) {
        if (strcmp(cap_names[i].name, name) == 0)
            return cap_names[i].cap;
    }
    return XENUS_CAP_NONE;
}

const char *xenus_capability_name(uint32_t cap)
{
    size_t i;

    if (cap == XENUS_CAP_NONE)
        return "none";
    for (i = 0; i < sizeof(cap_names) / sizeof(cap_names[0]); i++) {
        if (cap_names[i].cap == cap)
            return cap_names[i].name;
    }
    return "mixed";
}

/*
 * Per-capability enforcement honesty (single bit only).
 * Kernel-enforced = denied by seccomp/nnp/mount/rlimit inside a FULL
 * sandboxed child when the probe says AVAILABLE.  Everything checked
 * only in-process is BYPASSABLE by direct syscall and must be labelled
 * POLICY-LEVEL ONLY.  Unknown/multi-bit inputs are never overstated.
 */
const char *xenus_capability_enforcement(uint32_t cap)
{
    xenus_security_features_t f;
    int bits = 0;
    uint32_t v = cap;

    if (cap == XENUS_CAP_NONE)
        return "none";
    while (v) {
        bits += (int)(v & 1u);
        v >>= 1;
    }
    if (bits != 1)
        return "mixed";
    xenus_security_features(&f);
    switch (cap) {
    case XENUS_CAP_NET:
    case XENUS_CAP_SPAWN:
        /* FULL tier seccomp denies non-unix sockets + clone/fork */
        if (f.seccomp == XENUS_FEATURE_AVAILABLE)
            return "KERNEL-ENFORCED";
        return "POLICY-LEVEL ONLY";
    case XENUS_CAP_FS_READ:
    case XENUS_CAP_FS_WRITE:
    case XENUS_CAP_FS_EXEC:
        /* mount-ns hiding when fs probe passes, else rlimit/seccomp */
        if (f.fs == XENUS_FEATURE_AVAILABLE &&
            f.seccomp == XENUS_FEATURE_AVAILABLE)
            return "KERNEL-ENFORCED";
        if (f.seccomp == XENUS_FEATURE_AVAILABLE ||
            f.no_new_privs == XENUS_FEATURE_AVAILABLE)
            return "LIMITED";
        return "POLICY-LEVEL ONLY";
    case XENUS_CAP_FS_WATCH:
    case XENUS_CAP_CONFIG:
    case XENUS_CAP_AUDIT:
    case XENUS_CAP_DEVICE:
    case XENUS_CAP_PLUGIN_LOAD:
        /* userspace mediation only: bypassable via direct syscall */
        return "BYPASSABLE";
    default:
        return "BYPASSABLE";
    }
}

int xenus_security_restrict_self(void)
{
    struct rlimit rl;
    int ok = 1;

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        ok = 0;

    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    if (setrlimit(RLIMIT_CORE, &rl) != 0)
        ok = 0;

    return ok ? XENUS_OK : XENUS_ERR;
}
