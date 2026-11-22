/*
 * XENUS Security Layer - capability permissions, trust, policy, audit log,
 * path validation and the built-in malware scanner.
 *
 * Layering (see docs/SECURITY.md):
 *
 *   App / Plugin
 *        |
 *   XENUS Security Core  (trust, permissions, capabilities, policy,
 *        |               audit, malware scanner)          <- this header
 *   Linux security      (no_new_privs, seccomp, capabilities,
 *        |               namespaces, rlimits)
 *   Linux kernel
 *
 * XENUS userspace is NOT a hardware security boundary: it detects and
 * refuses risky operations, it does not contain native code that already
 * runs inside the same process.  Honest limits are documented in
 * docs/SECURITY.md - no fake sandbox claims.
 *
 * Design: fixed-size/static storage only (no allocation), fail-closed for
 * unclear operations, bounded caches/queues, never execute analysed files.
 */
#ifndef XENUS_APP_SECURITY_H
#define XENUS_APP_SECURITY_H

#include <xenus/common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENUS_PATH_MAX 1024          /* accepted scan/audit path length */
#define XENUS_SCAN_RING 64           /* scan result cache entries */
#define XENUS_AUDIT_RING 64          /* stored audit entries */
#define XENUS_TRUST_MAX 16           /* remembered trust decisions */
#define XENUS_POLICY_MAX 8
#define XENUS_CAP_SUBJECTS 16
#define XENUS_WATCH_MAX 8            /* inotify watch directories */

/* ------------------------------------------------------------ scanner -- */

typedef enum {
    XENUS_SCAN_ALWAYS_ON = 0,        /* event-driven filesystem monitoring */
    XENUS_SCAN_PASSIVE = 1,          /* checks only on sensitive actions */
    XENUS_SCAN_OFF = 2               /* no automatic scanning at all */
} xenus_scan_mode_t;

typedef enum {
    XENUS_THREAT_SAFE = 0,           /* 0-19  */
    XENUS_THREAT_UNKNOWN = 1,        /* 20-49 */
    XENUS_THREAT_SUSPICIOUS = 2,     /* 50-74 */
    XENUS_THREAT_MALICIOUS = 3,      /* 75-100 */
    XENUS_THREAT_BLOCKED = 4         /* policy refused it (score 100) */
} xenus_threat_status_t;

typedef enum {
    XENUS_HASH_NOT_COMPUTED = 0,
    XENUS_HASH_COMPUTED,
    XENUS_HASH_KNOWN_BAD,
    XENUS_HASH_KNOWN_TRUSTED,
    XENUS_HASH_SKIPPED               /* too large / mode off */
} xenus_hash_status_t;

typedef enum {
    XENUS_TRUST_UNKNOWN = 0,
    XENUS_TRUST_TRUSTED = 1,
    XENUS_TRUST_UNTRUSTED = 2,
    XENUS_TRUST_BLOCKED = 3
} xenus_trust_t;

typedef enum {
    XENUS_REASON_NONE = 0,
    XENUS_REASON_PATH,               /* suspicious/traversing location */
    XENUS_REASON_NAME,               /* double extension, odd name */
    XENUS_REASON_TYPE,               /* content does not match the type */
    XENUS_REASON_PERM,               /* setuid/setgid/world-writable */
    XENUS_REASON_ELF,                /* ELF header/arch/interp anomalies */
    XENUS_REASON_SCRIPT_DESTRUCTIVE,
    XENUS_REASON_SCRIPT_PRIV,
    XENUS_REASON_SCRIPT_CRED,
    XENUS_REASON_SCRIPT_PERSIST,
    XENUS_REASON_SCRIPT_REMOTE,
    XENUS_REASON_SCRIPT_OBFUSC,
    XENUS_REASON_SYMLINK,
    XENUS_REASON_HASH,               /* known-bad / known-good hash */
    XENUS_REASON_SIGNATURE,          /* rules.db signature hit */
    XENUS_REASON_TRUST,
    XENUS_REASON_PLUGIN,
    XENUS_REASON_POLICY,
    XENUS_REASON_SIZE,
    XENUS_REASON_IO
} xenus_reason_t;

#define XENUS_SCANFLAG_CACHED   (1u << 0)
#define XENUS_SCANFLAG_DEEP     (1u << 1)   /* content inspected */
#define XENUS_SCANFLAG_SYMLINK  (1u << 2)
#define XENUS_SCANFLAG_EXEC     (1u << 3)
#define XENUS_SCANFLAG_SCRIPT   (1u << 4)
#define XENUS_SCANFLAG_ELF      (1u << 5)
#define XENUS_SCANFLAG_SKIPPED  (1u << 6)   /* not analysed (off/too large) */

typedef struct xenus_scan_result {
    int16_t status;                 /* xenus_threat_status_t */
    int16_t risk;                   /* 0..100 */
    int16_t reason;                 /* xenus_reason_t, highest weight */
    int16_t hash_status;            /* xenus_hash_status_t */
    int16_t trust;                  /* xenus_trust_t */
    uint32_t flags;                 /* XENUS_SCANFLAG_* */
    char detail[96];                /* short, never file content */
} xenus_scan_result_t;

typedef struct xenus_scan_stats {
    uint32_t scans;                 /* real scans (cache misses) */
    uint32_t cache_hits;
    uint32_t threats;               /* SUSPICIOUS or worse */
    uint32_t blocked;
    uint32_t rejected_plugins;
    uint32_t watch_events;          /* inotify events processed */
    uint32_t denied;                /* capability + policy denials */
} xenus_scan_stats_t;

/* ---------------------------------------------------------- lifecycle -- */

/*
 * Loads the database from db_dir (NULL = $XENUS_SECURITY_DIR, else
 * ~/.config/xenus/security if it exists, else /etc/xenus/security).
 * Missing directories are fine: built-in rules stay active.  Idempotent;
 * every later call reloads the databases.  $XENUS_SCAN_MODE
 * (always-on|passive|off) is honoured on init.
 */
int xenus_security_init(const char *db_dir);
void xenus_security_shutdown(void);
const char *xenus_security_db_dir(void);
int xenus_security_reload_db(void);

int xenus_security_set_scan_mode(xenus_scan_mode_t mode);
xenus_scan_mode_t xenus_security_get_scan_mode(void);
const char *xenus_scan_mode_name(xenus_scan_mode_t mode);

/* rule/hash/trust entries loaded from the databases (built-ins excluded) */
int xenus_security_db_info(int *rules, int *hashes, int *trusted);

void xenus_security_stats(xenus_scan_stats_t *out);

/* ----------------------------------------------------------- scanning -- */

/*
 * Scans one file.  Never executes it.  Returns XENUS_OK with a filled
 * result, XENUS_ERR_ARG (bad arguments), XENUS_ERR_NOT_FOUND (no such file),
 * XENUS_ERR_STATE (scanner mode is OFF).  Result statuses:
 * SAFE/UNKNOWN/SUSPICIOUS/MALICIOUS/BLOCKED (risk thresholds 0/20/50/75).
 */
int xenus_scan_file(const char *path, xenus_scan_result_t *result);

/* SHA-256 of a file as 64 lowercase hex chars (files up to 4 MiB). */
int xenus_hash_file(const char *path, char out_hex[65]);

/* Policy gate: XENUS_OK = may proceed, XENUS_ERR = refuse (fail-closed). */
int xenus_security_allow(const xenus_scan_result_t *result);

/* Plugin gate: path validation -> scan -> trust -> policy.
 * Rejects before dlopen() (ELF constructors would already have run). */
int xenus_security_check_plugin(const char *path, xenus_scan_result_t *result);

/* ------------------------------------------------------------ trust --- */

int xenus_trust_get(const char *path);          /* xenus_trust_t, UNKNOWN */
int xenus_trust_set(const char *path, int trust);
int xenus_trust_forget(const char *path);

/* ------------------------------------------------------ capabilities --- */

typedef enum {
    XENUS_CAP_NONE      = 0,
    XENUS_CAP_FS_READ   = 1u << 0,
    XENUS_CAP_FS_WRITE  = 1u << 1,
    XENUS_CAP_FS_EXEC   = 1u << 2,
    XENUS_CAP_FS_WATCH  = 1u << 3,
    XENUS_CAP_NET       = 1u << 4,
    XENUS_CAP_DEVICE    = 1u << 5,
    XENUS_CAP_SPAWN     = 1u << 6,
    XENUS_CAP_CONFIG    = 1u << 7,
    XENUS_CAP_AUDIT     = 1u << 8,
    XENUS_CAP_PLUGIN_LOAD = 1u << 9
} xenus_capability_t;

/* what a validated plugin gets by default (declarative until isolation) */
#define XENUS_CAP_PLUGIN_DEFAULT \
    (XENUS_CAP_FS_READ | XENUS_CAP_CONFIG | XENUS_CAP_AUDIT)

int xenus_security_grant(const char *subject, uint32_t caps);
int xenus_security_revoke(const char *subject, uint32_t caps);
int xenus_security_has(const char *subject, uint32_t caps);   /* 1 / 0 */
/* XENUS_OK when every cap is granted, XENUS_ERR otherwise (fail-closed) */
int xenus_security_check(const char *subject, uint32_t caps);
uint32_t xenus_capability_from_name(const char *name);
const char *xenus_capability_name(uint32_t cap);
/*
 * Honest enforcement label for one capability bit (single-bit caps only;
 * multi-bit returns "mixed").  In-process userspace checks are
 * POLICY-LEVEL ONLY / BYPASSABLE via direct libc/syscall; real
 * containment lives in the sandboxed child (seccomp/nnp/mount/rlimit).
 * Returns one of: "KERNEL-ENFORCED", "LIMITED", "POLICY-LEVEL ONLY",
 * "BYPASSABLE", "mixed", "none".
 */
const char *xenus_capability_enforcement(uint32_t cap);

/*
 * Applies no_new_privs + zero core dumps to the calling process.
 * Best effort: XENUS_OK when no_new_privs is set.  Reserved for future
 * plugin subprocess isolation; the session itself is not restricted by
 * default.
 */
int xenus_security_restrict_self(void);

/* ------------------------------------------------------------- policy -- */

typedef enum {
    XENUS_POL_ALLOW_UNTRUSTED_PLUGINS = 0, /* 1 = load untrusted plugins */
    XENUS_POL_BLOCK_KNOWN_BAD,             /* 1 = BLOCKED on bad hash */
    XENUS_POL_DENY_SUSPICIOUS,             /* 1 = policy refuses suspicious */
    XENUS_POL_REQUIRE_TRUSTED_PLUGINS,     /* 1 = plugin must be TRUSTED */
    XENUS_POL_AUDIT_FILE_SCANS,            /* 1 = audit every scan */
    XENUS_POL_COUNT                        /* number of keys */
} xenus_policy_key_t;

int xenus_policy_set(int key, int value);      /* value must be 0 or 1 */
int xenus_policy_get(int key);                 /* current value */
int xenus_policy_default(int key);             /* built-in default */
int xenus_policy_reset(void);

/* --------------------------------------------------------------- path -- */

/* XENUS_OK = absolute, no "..", no control chars, within XENUS_PATH_MAX. */
int xenus_path_is_safe(const char *path);

/* -------------------------------------------------------------- audit -- */

typedef enum {
    XENUS_AUDIT_FILE_SCANNED = 1,
    XENUS_AUDIT_THREAT_DETECTED,
    XENUS_AUDIT_THREAT_BLOCKED,
    XENUS_AUDIT_PLUGIN_REJECTED,
    XENUS_AUDIT_PLUGIN_TRUSTED,
    XENUS_AUDIT_CAPABILITY_DENIED,
    XENUS_AUDIT_POLICY_DENIED,
    XENUS_AUDIT_SCANNER_MODE_CHANGED,
    /* enforcement (appended - values above stay stable) */
    XENUS_AUDIT_SANDBOX_CREATED,
    XENUS_AUDIT_SANDBOX_FAILED,
    XENUS_AUDIT_CAPABILITY_GRANTED,
    XENUS_AUDIT_PROCESS_RESTRICTED,
    XENUS_AUDIT_PROCESS_BLOCKED,
    XENUS_AUDIT_TRUST_REQUIRED,
    XENUS_AUDIT_POLICY_FALLBACK,
    XENUS_AUDIT_SECURITY_FEATURE_UNAVAILABLE,
    /* decoy detection (appended - values above stay stable).
     * Bait/Mirage are ADVISORY detection only, never a sandbox claim. */
    XENUS_AUDIT_BAIT_CREATED,
    XENUS_AUDIT_BAIT_ACCESSED,
    XENUS_AUDIT_BAIT_MODIFIED,
    XENUS_AUDIT_BAIT_DELETED,
    XENUS_AUDIT_BAIT_EXECUTION_ATTEMPT,
    XENUS_AUDIT_BAIT_PERMISSION_DENIED,
    XENUS_AUDIT_MIRAGE_CREATED,
    XENUS_AUDIT_MIRAGE_PROBED,
    XENUS_AUDIT_MIRAGE_ACCESSED,
    XENUS_AUDIT_MIRAGE_MODIFIED,
    XENUS_AUDIT_MIRAGE_ESCAPE_ATTEMPT,
    XENUS_AUDIT_EVENT_COUNT              /* number of defined events */
} xenus_audit_event_t;

typedef struct xenus_audit_entry {
    uint32_t time_ms;
    int16_t event;                  /* xenus_audit_event_t */
    int16_t risk;
    char subject[64];
    char detail[64];
} xenus_audit_entry_t;

/* detail is short metadata only - never file contents or credentials. */
int xenus_audit_log(int event, const char *subject, int risk,
                   const char *detail);
int xenus_audit_count(void);                     /* stored entries */
int xenus_audit_get(int index, xenus_audit_entry_t *out); /* 0 = oldest */
uint32_t xenus_audit_total(int event);           /* all-time per event */
int xenus_audit_clear(void);
const char *xenus_audit_event_name(int event);

/* ------------------------------------------------------ fs monitoring -- */

/* ONLY honoured in ALWAYS_ON mode (XENUS_ERR_STATE otherwise). */
int xenus_security_watch_dir(const char *dir);
int xenus_security_watch_defaults(void);         /* db + plugin directories */
void xenus_security_watch_stop(void);
int xenus_security_watch_fd(void);
int xenus_security_pending(void);               /* -1 = not watching */
/* Bounded: processes at most 16 queued events, never scans the world. */
int xenus_security_pump(void);

/* --------------------------------------------- enforcement / sandbox --- */

/*
 * Runtime feature detection.  Probed once (fork-based verification, cached);
 * used for honest status reporting and fail-closed decisions.  A feature is
 * AVAILABLE only when the probe *verified* real kernel behaviour - never
 * because a header exists.  Under PRoot/Termux the same probes decide what
 * actually works instead of assuming systemd/root/namespaces.
 */
typedef enum {
    XENUS_FEATURE_UNAVAILABLE = 0,     /* probe failed: no enforcement */
    XENUS_FEATURE_LIMITED = 1,         /* partial / degraded enforcement */
    XENUS_FEATURE_AVAILABLE = 2        /* probe verified real behaviour */
} xenus_feature_state_t;

typedef struct xenus_security_features {
    xenus_feature_state_t kernel_enforcement; /* nnp + rlimit + seccomp */
    xenus_feature_state_t no_new_privs;
    xenus_feature_state_t rlimit;
    xenus_feature_state_t seccomp;            /* verified install + SIGSYS */
    xenus_feature_state_t fs;                 /* mount-ns path blocking */
    xenus_feature_state_t sandbox;            /* best reachable tier:
                                                AVAILABLE = seccomp+fs,
                                                LIMITED = seccomp only,
                                                UNAVAILABLE = no seccomp */
    int scanner_active;                      /* != XENUS_SCAN_OFF */
    int policy_active;                       /* policy gate present */
    int proot;                               /* PRoot-like environment */
} xenus_security_features_t;

const char *xenus_feature_state_name(xenus_feature_state_t state);
int xenus_security_features(xenus_security_features_t *out);

/*
 * Single policy decision for an operation on a scanned target.
 * fail-closed: NULL/error results, KNOWN_BAD, blocked or malicious targets
 * and untrusted targets whose required sandbox is unavailable -> DENY.
 * Mapping (see docs/SECURITY.md):
 *   KNOWN_BAD / BLOCKED / MALICIOUS / trust BLOCKED -> DENY
 *   SUSPICIOUS  -> DENY when policy says so, else SANDBOX
 *   trust UNTRUSTED -> SANDBOX, or DENY when the sandbox is unavailable
 *   trust TRUSTED + SAFE -> ALLOW
 *   otherwise -> RESTRICT (policy default for UNKNOWN, not "safe")
 */
typedef enum {
    XENUS_DECIDE_ALLOW = 0,           /* trusted + clean: run normally */
    XENUS_DECIDE_RESTRICT,            /* unknown: run with restrictions */
    XENUS_DECIDE_SANDBOX,             /* untrusted/suspicious: isolation */
    XENUS_DECIDE_DENY                 /* blocked: do not run */
} xenus_decision_t;

int xenus_security_decide(const xenus_scan_result_t *result,
                         xenus_decision_t *out);

/*
 * Applies kernel restrictions to the *calling* process (intended for a
 * freshly forked child, before exec).  Requires the matching probe to be
 * AVAILABLE; any failure returns XENUS_ERR and the caller must fail closed
 * (do not exec).  Tiers:
 *   NONE     - no restrictions
 *   RESTRICT - no_new_privs + rlimits + defensive seccomp (net/spawn ok)
 *   FULL     - RESTRICT + seccomp denies network/spawn + mount-ns hiding
 *              of sensitive paths when the fs probe is available
 */
typedef enum {
    XENUS_SANDBOX_NONE = 0,
    XENUS_SANDBOX_RESTRICT = 1,
    XENUS_SANDBOX_FULL = 2
} xenus_sandbox_tier_t;

int xenus_sandbox_apply(xenus_sandbox_tier_t tier);
const char *xenus_sandbox_tier_name(xenus_sandbox_tier_t tier);

/*
 * Gate for spawning a command through XENUS: resolves the target, scans it,
 * applies trust + policy via xenus_security_decide().  Fills *out with the
 * decision (DENY on any error - fail-closed) and audits the outcome
 * (PROCESS_BLOCKED for DENY, PROCESS_RESTRICTED / POLICY_FALLBACK for
 * RESTRICT).  Subject metadata only: never logs the full command line.
 */
int xenus_security_gate_exec(const char *cmd, xenus_decision_t *out);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_APP_SECURITY_H */
