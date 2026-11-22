/*
 * XENUS Security Layer - security policy and the policy gate.
 *
 * Policies are simple 0/1 switches with safe defaults; everything that is
 * not explicitly allowed is refused (fail-closed) at the gate.
 */
#include <xenus/security.h>

#include "security_internal.h"

static const int defaults[XENUS_POL_COUNT] = {
    0,     /* ALLOW_UNTRUSTED_PLUGINS: in-process native code requires
              TRUSTED by default; enabling this is an explicit unsafe
              override (documented in docs/SECURITY.md) */
    1,     /* BLOCK_KNOWN_BAD: known-bad hash => BLOCKED */
    0,     /* DENY_SUSPICIOUS: suspicious runs in the sandbox instead */
    0,     /* REQUIRE_TRUSTED_PLUGINS (legacy; subsumed by the default
              above, kept for compatibility) */
    1      /* AUDIT_FILE_SCANS */
};

static int values[XENUS_POL_COUNT];
static int values_ready;

static void ensure_defaults(void)
{
    int i;

    if (values_ready)
        return;
    for (i = 0; i < XENUS_POL_COUNT; i++)
        values[i] = defaults[i];
    values_ready = 1;
}

int xenus_policy_set(int key, int value)
{
    ensure_defaults();
    if (key < 0 || key >= XENUS_POL_COUNT)
        return XENUS_ERR_ARG;
    if (value != 0 && value != 1)
        return XENUS_ERR_ARG;
    values[key] = value;
    return XENUS_OK;
}

int xenus_policy_get(int key)
{
    ensure_defaults();
    if (key < 0 || key >= XENUS_POL_COUNT)
        return -1;
    return values[key];
}

int xenus_policy_default(int key)
{
    if (key < 0 || key >= XENUS_POL_COUNT)
        return -1;
    return defaults[key];
}

int xenus_policy_reset(void)
{
    int i;

    for (i = 0; i < XENUS_POL_COUNT; i++)
        values[i] = defaults[i];
    values_ready = 1;
    return XENUS_OK;
}

int xenus_security_allow(const xenus_scan_result_t *result)
{
    if (!result)
        return XENUS_ERR_ARG;
    ensure_defaults();

    switch (result->status) {
    case XENUS_THREAT_SAFE:
    case XENUS_THREAT_UNKNOWN:
        return XENUS_OK;
    case XENUS_THREAT_SUSPICIOUS:
        if (values[XENUS_POL_DENY_SUSPICIOUS]) {
            xenus__stats()->denied++;
            xenus_audit_log(XENUS_AUDIT_POLICY_DENIED, result->detail,
                           result->risk, "suspicious refused by policy");
            return XENUS_ERR;
        }
        return XENUS_OK;
    case XENUS_THREAT_MALICIOUS:
    case XENUS_THREAT_BLOCKED:
        xenus__stats()->denied++;
        xenus_audit_log(XENUS_AUDIT_POLICY_DENIED, result->detail, result->risk,
                       "malicious refused by policy");
        return XENUS_ERR;
    default:
        return XENUS_ERR;           /* unknown status: fail closed */
    }
}

/*
 * Scanner + trust + policy -> one enforcement decision (fail-closed).
 * Pure decision: no side effects, no auditing (callers audit outcomes).
 * See docs/SECURITY.md for the full mapping table.
 */
int xenus_security_decide(const xenus_scan_result_t *result,
                         xenus_decision_t *out)
{
    xenus_security_features_t f;

    if (!out)
        return XENUS_ERR_ARG;
    *out = XENUS_DECIDE_DENY;            /* fail-closed default */
    if (!result)
        return XENUS_ERR_ARG;
    ensure_defaults();

    switch (result->status) {
    case XENUS_THREAT_BLOCKED:
    case XENUS_THREAT_MALICIOUS:
        return XENUS_OK;                 /* known-bad / malicious: DENY */
    case XENUS_THREAT_SUSPICIOUS:
        if (values[XENUS_POL_DENY_SUSPICIOUS])
            return XENUS_OK;             /* DENY */
        if (result->trust == XENUS_TRUST_TRUSTED) {
            *out = XENUS_DECIDE_ALLOW;   /* trusted freedom */
            return XENUS_OK;
        }
        xenus_security_features(&f);
        if (f.sandbox == XENUS_FEATURE_UNAVAILABLE)
            return XENUS_OK;             /* cannot isolate: DENY */
        *out = XENUS_DECIDE_SANDBOX;
        return XENUS_OK;
    case XENUS_THREAT_SAFE:
    case XENUS_THREAT_UNKNOWN:
        break;
    default:
        return XENUS_OK;                 /* unknown status: DENY */
    }

    switch (result->trust) {
    case XENUS_TRUST_BLOCKED:
        return XENUS_OK;                 /* DENY */
    case XENUS_TRUST_TRUSTED:
        *out = XENUS_DECIDE_ALLOW;
        return XENUS_OK;
    case XENUS_TRUST_UNTRUSTED:
        xenus_security_features(&f);
        if (f.sandbox == XENUS_FEATURE_UNAVAILABLE)
            return XENUS_OK;             /* fail-closed: no sandbox, no run */
        *out = XENUS_DECIDE_SANDBOX;
        return XENUS_OK;
    default:
        /* UNKNOWN trust: restricted execution is the policy default -
         * not "safe", not unrestricted */
        *out = XENUS_DECIDE_RESTRICT;
        return XENUS_OK;
    }
}
