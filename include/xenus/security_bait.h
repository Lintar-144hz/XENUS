/*
 * XENUS Bait - filesystem decoy subsystem (detection/telemetry only).
 *
 * Purpose: provide attractive-looking but fully dummy targets for
 * untrusted programs so suspicious activity can be detected and audited.
 * All content is synthetic and clearly marked as decoy.  No real
 * passwords, tokens, cookies or credentials are ever stored here.
 *
 * What Bait IS:
 *   - ADVISORY detection (polling + explicit report hooks) feeding the
 *     audit log (BAIT_* events with pid/uid/process/trust metadata).
 *
 * What Bait IS NOT:
 *   - NOT a security boundary, NOT a sandbox, NOT isolation.
 *   - Detection does NOT imply the accessor is contained.
 *   - Enforcement stays at the process/kernel boundary
 *     (no_new_privs + rlimits + seccomp + mount hiding, see security.h).
 *
 * Design: fixed-size tables only, bounded filesystem footprint under one
 * root directory (mkdtemp-style), non-executable decoys, no symlinks to
 * the real filesystem, fail-closed on errors.
 */
#ifndef XENUS_SECURITY_BAIT_H
#define XENUS_SECURITY_BAIT_H

#include <xenus/common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_BAIT_MAX 32            /* decoy entries per session */
#define XENUS_BAIT_ID_LEN 48
#define XENUS_BAIT_PATH_LEN 512

typedef enum {
    XENUS_BAIT_CONFIG = 0,           /* fake config file */
    XENUS_BAIT_CACHE,                /* fake cache data */
    XENUS_BAIT_DOCUMENT,             /* fake document */
    XENUS_BAIT_EXECUTABLE,           /* fake executable (never harmful) */
    XENUS_BAIT_APPDATA,              /* fake application data */
    XENUS_BAIT_KIND_COUNT
} xenus_bait_kind_t;

const char *xenus_bait_kind_name(int kind);

/*
 * Lifecycle.  init(dir) creates the decoy root (dir == NULL picks
 * $XENUS_BAIT_DIR or a mkdtemp under /tmp).  Decoys are dummy files only.
 * shutdown() removes the decoy tree it created (never user files).
 */
int xenus_bait_init(const char *dir);
void xenus_bait_shutdown(void);
int xenus_bait_active(void);         /* 1 when initialised */
const char *xenus_bait_dir(void);    /* "" when inactive */

int xenus_bait_enable(void);         /* detection ON (default after init) */
int xenus_bait_disable(void);        /* detection OFF (decoys stay inert) */
int xenus_bait_enabled(void);

/* Populate the standard dummy set (idempotent, fail-closed). */
int xenus_bait_create_defaults(void);
/* Create one decoy of a given kind with a safe leaf name. */
int xenus_bait_create(int kind, const char *leaf);

/*
 * Polling check (event-driven friendly: call from the event loop or the
 * Security GUI refresh, never a busy loop).  Re-stats every decoy and
 * emits BAIT_MODIFIED / BAIT_DELETED / BAIT_EXECUTION_ATTEMPT /
 * BAIT_PERMISSION_DENIED as appropriate.  Returns number of new hits.
 */
int xenus_bait_check(void);

/*
 * Explicit telemetry hook: record that a subject touched a decoy.
 * op is a short verb ("read", "write", "exec", "unlink", ...).
 * trust is xenus_trust_t of the accessor (UNKNOWN when not known).
 * Emits BAIT_ACCESSED (or BAIT_EXECUTION_ATTEMPT for exec-like ops)
 * with pid/uid/process-name/trust metadata.  Never executes anything.
 */
int xenus_bait_report(const char *decoy_path, const char *op, int trust);

/* Counters for the Security GUI (hits = accesses + modifications + ...). */
int xenus_bait_count(void);          /* decoys currently tracked */
int xenus_bait_hits(void);           /* total detections since init */
int xenus_bait_path(int index, char *out, size_t out_len);

/*
 * Safety self-check: verifies every decoy still contains only dummy data
 * (decoy marker present, no real credential patterns).  Returns XENUS_OK
 * when clean, XENUS_ERR when anything looks non-dummy (fail-closed: the
 * caller should disable/recreate the set).
 */
int xenus_bait_verify_dummy(void);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_SECURITY_BAIT_H */
