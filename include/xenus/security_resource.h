/*
 * XENUS resource protection - hard limits for untrusted child processes.
 *
 * Enforcement point is the kernel (rlimits applied in the child before
 * exec, plus supervisor SIGTERM -> grace -> SIGKILL that only ever
 * targets the supervised pid).  XENUS itself is never limited by these
 * calls; only the child that explicitly applies them.
 *
 * Classification: KERNEL-ENFORCED where the rlimit probe passes,
 * LIMITED/UNAVAILABLE otherwise (callers must fail closed).
 */
#ifndef XENUS_SECURITY_RESOURCE_H
#define XENUS_SECURITY_RESOURCE_H

#include <xenus/common.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xenus_resource_limits {
    unsigned long ram_bytes;        /* RLIMIT_DATA ceiling, 0 = leave alone.
                                       Default 3072 MiB (3 GiB) per
                                       sandboxed process; the ceiling is
                                       capped, never raised. */
    unsigned int cpu_seconds;       /* RLIMIT_CPU quota, 0 = no XENUS quota
                                       imposed (host scheduler decides).
                                       Default 0. */
    unsigned long nproc;            /* RLIMIT_NPROC, 0 = leave alone */
    unsigned long nofile;           /* RLIMIT_NOFILE, 0 = leave alone */
    unsigned long fsize_bytes;      /* RLIMIT_FSIZE, 0 = leave alone */
    unsigned int wall_seconds;      /* supervisor wall-clock cap, 0 = none */
} xenus_resource_limits_t;

void xenus_resource_defaults(xenus_resource_limits_t *out);
int xenus_resource_set(const xenus_resource_limits_t *lim); /* NULL = reset */
int xenus_resource_get(xenus_resource_limits_t *out);

/*
 * Apply the configured limits to the CALLING process (child side, before
 * exec).  Always sets RLIMIT_CORE to 0.  Returns XENUS_OK when the baseline
 * (core + requested rlimits that the kernel accepted) is in place,
 * XENUS_ERR otherwise (caller must fail closed and never exec loose).
 */
int xenus_resource_apply(void);

/*
 * Supervisor: wait up to grace_ms for pid, then SIGTERM, wait again,
 * then SIGKILL.  Only ever signals pid (never process groups, never
 * XENUS itself).  Returns 1 when the target exited, 0 on error.
 * status_out (optional) receives the waitpid status when reaped here.
 */
int xenus_resource_constrain(pid_t pid, unsigned int grace_ms,
                             int *status_out);

/* Human-readable one-liner for the Security GUI (bounded). */
int xenus_resource_describe(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* XENUS_SECURITY_RESOURCE_H */
