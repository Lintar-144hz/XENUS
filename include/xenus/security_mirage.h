/*
 * XENUS Mirage - environment/process decoy subsystem (detection only).
 *
 * Purpose: offer fake environment indicators, fake resources, fake
 * capability targets and fake process/resource metadata so probing by
 * ordinary applications (or suspicious ones) can be detected and audited
 * without ever granting access to the real resource.
 *
 * What Mirage IS:
 *   - ADVISORY detection: every lookup/probe is denied and audited
 *     (MIRAGE_PROBED / ACCESSED / MODIFIED / ESCAPE_ATTEMPT).
 *
 * What Mirage IS NOT:
 *   - NOT a security boundary, NOT isolation, NOT a capability grant.
 *   - A probe hit does NOT mean the prober is contained.
 *
 * Design: fixed-size in-memory registry only, dummy values only, no real
 * handles, no real environment mutation, fail-closed (unknown => deny).
 */
#ifndef XENUS_SECURITY_MIRAGE_H
#define XENUS_SECURITY_MIRAGE_H

#include <xenus/common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_MIRAGE_MAX 32
#define XENUS_MIRAGE_NAME_LEN 64

typedef enum {
    XENUS_MIRAGE_ENV = 0,            /* decoy environment indicator */
    XENUS_MIRAGE_RESOURCE,           /* decoy resource handle */
    XENUS_MIRAGE_CAPABILITY,         /* decoy capability target */
    XENUS_MIRAGE_PROCESS,            /* decoy process/resource metadata */
    XENUS_MIRAGE_KIND_COUNT
} xenus_mirage_kind_t;

const char *xenus_mirage_kind_name(int kind);

int xenus_mirage_init(void);
void xenus_mirage_shutdown(void);
int xenus_mirage_active(void);

int xenus_mirage_enable(void);
int xenus_mirage_disable(void);
int xenus_mirage_enabled(void);

/* Register the standard dummy set (idempotent). */
int xenus_mirage_create_defaults(void);
/* Register one decoy by kind + safe name. */
int xenus_mirage_create(int kind, const char *name);

/*
 * Probe a decoy: ALWAYS denied (XENUS_ERR), always audited.
 * op is a short verb ("get", "open", "read", "write", "escape", ...).
 * trust is xenus_trust_t of the caller (UNKNOWN when not known).
 * - normal ops    -> MIRAGE_PROBED / MIRAGE_ACCESSED / MIRAGE_MODIFIED
 * - escape-like   -> MIRAGE_ESCAPE_ATTEMPT (higher risk)
 * out_value (optional) receives the dummy value, never real data.
 */
int xenus_mirage_probe(const char *name, const char *op, int trust);
int xenus_mirage_lookup(const char *name, const char *op, int trust,
                        char *out_value, size_t out_len);

/* Counters for the Security GUI. */
int xenus_mirage_count(void);
int xenus_mirage_probes(void);       /* total probe events since init */
int xenus_mirage_name(int index, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_SECURITY_MIRAGE_H */
