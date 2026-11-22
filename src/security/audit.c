/*
 * XENUS Security Layer - security audit log.
 *
 * Fixed ring of XENUS_AUDIT_RING entries plus all-time counters per event,
 * so important events survive even when FILE_SCANNED keeps cycling through
 * the ring.  Only short metadata is recorded - never file contents.
 */
#define _DEFAULT_SOURCE
#include <xenus/security.h>

#include "security_internal.h"

#include <stdio.h>
#include <string.h>

static xenus_audit_entry_t ring[XENUS_AUDIT_RING];
static size_t ring_len;
static size_t ring_head;
static uint32_t totals[XENUS_AUDIT_EVENT_COUNT];

static int event_ok(int event)
{
    return event >= XENUS_AUDIT_FILE_SCANNED &&
           event < XENUS_AUDIT_EVENT_COUNT;
}

int xenus_audit_log(int event, const char *subject, int risk,
                   const char *detail)
{
    xenus_audit_entry_t *slot;

    if (!event_ok(event))
        return XENUS_ERR_ARG;
    if (risk < 0)
        risk = 0;
    if (risk > 100)
        risk = 100;

    if (ring_len == XENUS_AUDIT_RING) {
        slot = &ring[ring_head];
        ring_head = (ring_head + 1) % XENUS_AUDIT_RING;
    } else {
        slot = &ring[(ring_head + ring_len) % XENUS_AUDIT_RING];
        ring_len++;
    }
    memset(slot, 0, sizeof(*slot));
    slot->time_ms = xenus_event_now_ms();
    slot->event = (int16_t)event;
    slot->risk = (int16_t)risk;
    snprintf(slot->subject, sizeof(slot->subject), "%.*s",
             (int)(sizeof(slot->subject) - 1), subject ? subject : "-");
    snprintf(slot->detail, sizeof(slot->detail), "%.*s",
             (int)(sizeof(slot->detail) - 1), detail ? detail : "");
    totals[event]++;

    /* mirror security-critical events into the session log (metadata only) */
    switch (event) {
    case XENUS_AUDIT_THREAT_DETECTED:
        xenus_logf(XENUS_LOG_WARN, "security: threat risk=%d %s (%s)", risk,
                  slot->subject, slot->detail);
        break;
    case XENUS_AUDIT_THREAT_BLOCKED:
        xenus_logf(XENUS_LOG_WARN, "security: blocked %s (%s)", slot->subject,
                  slot->detail);
        break;
    case XENUS_AUDIT_PLUGIN_REJECTED:
    case XENUS_AUDIT_CAPABILITY_DENIED:
    case XENUS_AUDIT_POLICY_DENIED:
    case XENUS_AUDIT_SANDBOX_FAILED:
    case XENUS_AUDIT_PROCESS_BLOCKED:
    case XENUS_AUDIT_TRUST_REQUIRED:
    case XENUS_AUDIT_SECURITY_FEATURE_UNAVAILABLE:
    case XENUS_AUDIT_BAIT_ACCESSED:
    case XENUS_AUDIT_BAIT_MODIFIED:
    case XENUS_AUDIT_BAIT_DELETED:
    case XENUS_AUDIT_BAIT_EXECUTION_ATTEMPT:
    case XENUS_AUDIT_BAIT_PERMISSION_DENIED:
    case XENUS_AUDIT_MIRAGE_PROBED:
    case XENUS_AUDIT_MIRAGE_ACCESSED:
    case XENUS_AUDIT_MIRAGE_MODIFIED:
    case XENUS_AUDIT_MIRAGE_ESCAPE_ATTEMPT:
        xenus_logf(XENUS_LOG_WARN, "security: %s %s (%s)",
                  xenus_audit_event_name(event), slot->subject, slot->detail);
        break;
    case XENUS_AUDIT_PLUGIN_TRUSTED:
    case XENUS_AUDIT_SCANNER_MODE_CHANGED:
    case XENUS_AUDIT_SANDBOX_CREATED:
    case XENUS_AUDIT_CAPABILITY_GRANTED:
    case XENUS_AUDIT_PROCESS_RESTRICTED:
    case XENUS_AUDIT_POLICY_FALLBACK:
    case XENUS_AUDIT_BAIT_CREATED:
    case XENUS_AUDIT_MIRAGE_CREATED:
        xenus_logf(XENUS_LOG_INFO, "security: %s %s (%s)",
                  xenus_audit_event_name(event), slot->subject, slot->detail);
        break;
    default:
        break;                      /* FILE_SCANNED stays out of the log */
    }
    return XENUS_OK;
}

int xenus_audit_count(void)
{
    return (int)ring_len;
}

int xenus_audit_get(int index, xenus_audit_entry_t *out)
{
    if (!out || index < 0 || (size_t)index >= ring_len)
        return XENUS_ERR_NOT_FOUND;
    *out = ring[(ring_head + (size_t)index) % XENUS_AUDIT_RING];
    return XENUS_OK;
}

uint32_t xenus_audit_total(int event)
{
    if (!event_ok(event))
        return 0;
    return totals[event];
}

int xenus_audit_clear(void)
{
    memset(ring, 0, sizeof(ring));
    ring_len = 0;
    ring_head = 0;
    return XENUS_OK;
}

const char *xenus_audit_event_name(int event)
{
    switch (event) {
    case XENUS_AUDIT_FILE_SCANNED:
        return "FILE_SCANNED";
    case XENUS_AUDIT_THREAT_DETECTED:
        return "THREAT_DETECTED";
    case XENUS_AUDIT_THREAT_BLOCKED:
        return "THREAT_BLOCKED";
    case XENUS_AUDIT_PLUGIN_REJECTED:
        return "PLUGIN_REJECTED";
    case XENUS_AUDIT_PLUGIN_TRUSTED:
        return "PLUGIN_TRUSTED";
    case XENUS_AUDIT_CAPABILITY_DENIED:
        return "CAPABILITY_DENIED";
    case XENUS_AUDIT_POLICY_DENIED:
        return "POLICY_DENIED";
    case XENUS_AUDIT_SCANNER_MODE_CHANGED:
        return "SCANNER_MODE_CHANGED";
    case XENUS_AUDIT_SANDBOX_CREATED:
        return "SANDBOX_CREATED";
    case XENUS_AUDIT_SANDBOX_FAILED:
        return "SANDBOX_FAILED";
    case XENUS_AUDIT_CAPABILITY_GRANTED:
        return "CAPABILITY_GRANTED";
    case XENUS_AUDIT_PROCESS_RESTRICTED:
        return "PROCESS_RESTRICTED";
    case XENUS_AUDIT_PROCESS_BLOCKED:
        return "PROCESS_BLOCKED";
    case XENUS_AUDIT_TRUST_REQUIRED:
        return "TRUST_REQUIRED";
    case XENUS_AUDIT_POLICY_FALLBACK:
        return "POLICY_FALLBACK";
    case XENUS_AUDIT_SECURITY_FEATURE_UNAVAILABLE:
        return "SECURITY_FEATURE_UNAVAILABLE";
    case XENUS_AUDIT_BAIT_CREATED:
        return "BAIT_CREATED";
    case XENUS_AUDIT_BAIT_ACCESSED:
        return "BAIT_ACCESSED";
    case XENUS_AUDIT_BAIT_MODIFIED:
        return "BAIT_MODIFIED";
    case XENUS_AUDIT_BAIT_DELETED:
        return "BAIT_DELETED";
    case XENUS_AUDIT_BAIT_EXECUTION_ATTEMPT:
        return "BAIT_EXECUTION_ATTEMPT";
    case XENUS_AUDIT_BAIT_PERMISSION_DENIED:
        return "BAIT_PERMISSION_DENIED";
    case XENUS_AUDIT_MIRAGE_CREATED:
        return "MIRAGE_CREATED";
    case XENUS_AUDIT_MIRAGE_PROBED:
        return "MIRAGE_PROBED";
    case XENUS_AUDIT_MIRAGE_ACCESSED:
        return "MIRAGE_ACCESSED";
    case XENUS_AUDIT_MIRAGE_MODIFIED:
        return "MIRAGE_MODIFIED";
    case XENUS_AUDIT_MIRAGE_ESCAPE_ATTEMPT:
        return "MIRAGE_ESCAPE_ATTEMPT";
    default:
        return "UNKNOWN";
    }
}
