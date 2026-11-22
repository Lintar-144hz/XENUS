/*
 * UCore runtime tests - no X server required.
 *
 * These checks are deliberately about invariants the shell relies on:
 * the runtime comes up, procfs/sysfs facts are populated, every snapshot
 * honours its capacity, and unsupported hardware is reported honestly
 * instead of being fabricated.
 */
#include <xenus/ucore.h>

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

static int event_a_calls;
static int event_b_calls;
static int event_c_calls;
static int event_a_mutated;
static int event_unsub_result;
static int event_sub_result;

static void event_callback_b(const xenus_core_event_t *ev, void *user)
{
    (void)ev;
    (void)user;
    event_b_calls++;
}

static void event_callback_c(const xenus_core_event_t *ev, void *user)
{
    (void)ev;
    (void)user;
    event_c_calls++;
}

static void event_callback_a(const xenus_core_event_t *ev, void *user)
{
    (void)ev;
    (void)user;
    event_a_calls++;

    if (!event_a_mutated) {
        event_a_mutated = 1;
        event_unsub_result =
            xenus_core_event_unsubscribe(event_callback_b, NULL);
        event_sub_result =
            xenus_core_event_subscribe(XENUS_CORE_EV_HARDWARE,
                                      event_callback_c, NULL);
    }
}

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));           \
        }                                                                    \
    } while (0)

int main(void)
{
    xenus_core_sysinfo_t info;
    xenus_core_load_t load;
    xenus_core_hardware_t hw;
    xenus_core_proc_t procs[8];
    xenus_core_app_t apps[4];
    xenus_core_service_t svcs[8];
    xenus_core_module_t mods[XENUS_CORE_MAX_MODULES];
    xenus_core_log_entry_t logs[4];
    xenus_core_event_t ev;
    int n;

    CHECK(xenus_core_state() == XENUS_CORE_DOWN, "starts down");
    CHECK(xenus_core_init(NULL) == XENUS_CORE_OK, "init succeeds");
    CHECK(xenus_core_state() == XENUS_CORE_RUNNING, "running after init");
    CHECK(xenus_core_init(NULL) == XENUS_CORE_ERR_STATE,
          "double init rejected");
    CHECK(strcmp(xenus_core_version(), "1.0") == 0, "version exposed");

    info = *xenus_core_sysinfo();
    CHECK(info.os[0] != '\0', "os name populated");
    CHECK(info.kernel[0] != '\0', "kernel populated");
    CHECK(info.arch[0] != '\0', "arch populated");
    CHECK(info.cpus >= 1, "cpu count sane");
    CHECK(info.mem_total_mb > 0, "memory total detected");

    CHECK(xenus_core_load(&load) == XENUS_CORE_OK, "load sampled");
    CHECK(load.mem.total_kb > 0, "meminfo parsed");
    CHECK(load.mem_percent >= 0.0 && load.mem_percent <= 100.0,
          "memory percent bounded");
    CHECK(load.uptime_sec > 0, "uptime sampled");

    n = xenus_core_processes(procs, 8);
    CHECK(n > 0, "process snapshot non-empty");
    CHECK(n <= 8, "process snapshot honours capacity");
    CHECK(procs[0].pid > 0, "pid populated");
    CHECK(procs[0].name[0] != '\0', "process name populated");

    n = xenus_core_apps(apps, 4);
    CHECK(n >= 0 && n <= 4, "application snapshot honours capacity");
    n = xenus_core_services(svcs, 8);
    CHECK(n > 0, "services discovered");
    CHECK(n <= 8, "service snapshot honours capacity");
    n = xenus_core_modules(mods, XENUS_CORE_MAX_MODULES);
    CHECK(n >= 5, "core modules registered");
    CHECK(n <= XENUS_CORE_MAX_MODULES, "module snapshot honours capacity");

    CHECK(xenus_core_hardware(&hw, 1) == XENUS_CORE_OK, "hardware probed");
    CHECK(hw.wifi >= XENUS_HW_UNKNOWN && hw.wifi <= XENUS_HW_DENIED,
          "wifi state is a real enum");
    CHECK(hw.battery >= XENUS_HW_UNKNOWN && hw.battery <= XENUS_HW_DENIED,
          "battery state is a real enum");
    if (hw.battery == XENUS_HW_UNAVAILABLE)
        CHECK(hw.battery_percent == 0, "unavailable battery has no fake level");
    if (hw.volume == XENUS_HW_UNAVAILABLE)
        CHECK(hw.volume_percent == 0, "unavailable volume has no fake level");

    xenus_core_dnd_set(1);
    CHECK(xenus_core_dnd_get() == 1, "dnd can be enabled");
    xenus_core_dnd_set(0);
    CHECK(xenus_core_dnd_get() == 0, "dnd can be disabled");

    CHECK(xenus_core_event_subscribe(XENUS_CORE_EV_HARDWARE,
                                    event_callback_a, NULL) == XENUS_CORE_OK,
          "subscribe callback A");
    CHECK(xenus_core_event_subscribe(XENUS_CORE_EV_HARDWARE,
                                    event_callback_b, NULL) == XENUS_CORE_OK,
          "subscribe callback B");

    CHECK(xenus_core_event_emit(XENUS_CORE_EV_HARDWARE, "test", "snapshot-1")
              == XENUS_CORE_OK,
          "first snapshot event emitted");
    CHECK(event_a_calls == 1 && event_b_calls == 1 && event_c_calls == 0,
          "subscription changes do not alter current dispatch");
    CHECK(event_unsub_result == XENUS_CORE_OK,
          "callback A successfully unsubscribes B");
    CHECK(event_sub_result == XENUS_CORE_OK,
          "callback A successfully subscribes C");

    CHECK(xenus_core_event_emit(XENUS_CORE_EV_HARDWARE, "test", "snapshot-2")
              == XENUS_CORE_OK,
          "second snapshot event emitted");
    CHECK(event_a_calls == 2 && event_b_calls == 1 && event_c_calls == 1,
          "next dispatch uses updated subscriptions");

    xenus_core_event_unsubscribe(event_callback_a, NULL);
    xenus_core_event_unsubscribe(event_callback_c, NULL);

    CHECK(xenus_core_event_emit(XENUS_CORE_EV_HARDWARE, "test", "probe") == 0,
          "event emitted");
    xenus_core_event_pump();
    CHECK(xenus_core_event_poll(&ev) == 0 || ev.type != 0, "event ring usable");

    CHECK(xenus_core_log_entries(logs, 4) >= 0, "log ring readable");
    CHECK(strcmp(xenus_core_status_str(XENUS_CORE_OK), "ok") == 0,
          "status string");

    xenus_core_shutdown();
    CHECK(xenus_core_state() == XENUS_CORE_DOWN, "shutdown returns down");

    printf("ucore_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
