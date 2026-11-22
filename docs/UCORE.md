# UCore — XENUS's shared runtime substrate

UCore is the dependency-free **hybrid C runtime** that sits between the XENUS
shell, System Center, Control Center and the App API. It is a library linked
into the XENUS process — there is no daemon, no helper thread and no polling
loop of its own. The existing `poll()` event loop calls `xenus_core_tick()`
and every consumer shares the same fixed-size snapshots.

```
src/core/
├── core_internal.h   shared structs and helpers
├── sysinfo.c         uname/procfs/sysfs facts + hardware probes
├── proc.c            process snapshots and TERM/STOP/CONT actions
├── apps.c            freedesktop .desktop registry + launching
└── ucore.c           lifecycle, modules, services, log ring, events, DND
```

Public header: `include/xenus/ucore.h`.

## Design contract

1. **No fabrication.** Hardware that the host does not expose is reported as
   `XENUS_HW_UNAVAILABLE`; an action returns
   `XENUS_CORE_ERR_UNAVAILABLE`. A toggle is never drawn as working when the
   backing sysfs/procfs interface is absent.
2. **Bounded work.** Every snapshot takes an explicit capacity and never
   grows the heap in the event path. The log and event rings are fixed size.
3. **No threads.** Sampling is rate-limited inside `xenus_core_tick()`. The
   shell stays single-threaded.
4. **Real data only.** CPU/memory/load come from `/proc/stat`, `/proc/meminfo`
   and `/proc/loadavg`; processes from `/proc/<pid>/*`; applications from the
   standard `.desktop` search path; services and modules from the registries
   below.
5. **Safe command execution.** The few bridges that need a command (Android
   `cmd`/Termux `termux-*`, `pactl`, `rfkill`) run through `/bin/sh -c` with
   a status marker because XENUS installs `SIGCHLD = SIG_IGN`; `waitpid()`
   is therefore never used.

## Lifecycle

```c
xenus_core_init(cfg.config_path);   /* once, before the first tick */
/* in the event loop: */
xenus_core_tick();                  /* 1 Hz sampler, cheap when idle */
/* on shutdown: */
xenus_core_shutdown();
```

`xenus_core_init()` registers the built-in modules (`sysinfo`, `process`,
`applications`, `hardware`, `notifications`) and emits a lifecycle event.

## Snapshots

| Call | Capacity | Notes |
|------|----------|-------|
| `xenus_core_sysinfo()` | singleton | OS, kernel, arch, hostname, CPU model, CPU count, Android/PRoot/Termux flags, memory, uptime |
| `xenus_core_load(&load)` | singleton | CPU %, load averages, memory detail, process/thread counts |
| `xenus_core_processes(out, max)` | caller | `/proc` scan, RSS, CPU ticks, command line |
| `xenus_core_apps(out, max)` | caller | `.desktop` registry with running PID when detected |
| `xenus_core_services(out, max)` | caller | known services plus UCore-managed modules |
| `xenus_core_modules(out, max)` | caller | registered modules with call counters |
| `xenus_core_log_entries(out, max)` | caller | ring, oldest first |
| `xenus_core_hardware(&hw, refresh)` | singleton | cached for 4 s unless `refresh` is non-zero |

Process actions are `XENUS_PROC_TERM`, `XENUS_PROC_KILL`, `XENUS_PROC_STOP` and
`XENUS_PROC_CONT`. `xenus_core_process_is_critical()` lets a caller refuse to
signal PID 1 and friends.

## Hardware honesty

`xenus_core_hardware_t` carries both a state and a value for every capability:

* `wifi`, `bluetooth`, `airplane` — state plus controllable flag.
* `volume` — state, percentage, mute, controllable flag.
* `brightness` — state, percentage, writable flag (`/sys/class/backlight`).
* `battery` — state, percentage, charging status.

Probes inspect `/sys/class/net`, `/sys/class/rfkill`, `/sys/class/power_supply`,
`/sys/class/backlight`, `/sys/class/leds`, and fall back to the Android
bridge or `pactl` only when those exist. Control Center renders an
unavailable capability as greyed-out *not exposed* and keeps the tile inert.

## Events and notifications

`xenus_core_event_emit(type, source, text)` writes into a 64-entry ring and
forwards to the App API event bus. Subscribers register with
`xenus_core_event_subscribe()`; `xenus_core_event_poll()` drains one event and
`xenus_core_event_pump()` drains the ring. Event types are `LIFECYCLE`,
`RESOURCE`, `PROCESS`, `HARDWARE`, `CONFIG`, `NOTIFY` and `MODULE`.

Do Not Disturb is shell state owned by UCore (`xenus_core_dnd_get/set`) and is
shared by Control Center and Notification Center.

## Consumers

* **System Center** — Overview, Processes, Applications, Services, Device,
  Activity.
* **Control Center** — hardware tiles, sliders, workspace selector.
* **Notification Center** — App API notification ring plus DND state.
* **Tests** — `tests/ucore_test` exercises lifecycle, snapshots, capacity
  bounds, hardware honesty, DND, events and status strings without X.
