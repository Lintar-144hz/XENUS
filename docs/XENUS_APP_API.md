# XENUS App API and plugin system

XENUS exposes a small native C API so applications and plugins can talk to the
desktop without linking against XENUS internals.  Everything lives in
`include/xenus/*.h`; nothing under `src/` is public.

```
include/xenus/common.h         version, status codes, logging
include/xenus/event.h          event queue, subscriptions, dispatch
include/xenus/notification.h   fixed notification ring
include/xenus/config.h         read-only key=value config access
include/xenus/net.h            DNS + TCP/UDP transport for apps
include/xenus/window.h         minimal top-level window creation
include/xenus/app.h            app lifecycle, services, xenus_api_t table
include/xenus/plugin.h         plugin ABI + plugin manager interface
```

The implementation is `src/api/*.c` + `src/plugin/*.c`, compiled into
`build/libxenusapp.a` (linked into `xenus`, `tests/api_test`, and available to
third-party programs after `make install`).

Design rules:

* **No allocation.** Every table is fixed size and static (config 128 keys,
  events 64/64, notifications 32, services 16, windows 8, plugins 32).
  Exhaustion returns `XENUS_ERR_FULL`, never a crash.
* **Headless-safe.** Only the window API touches X, through its own
  `Display *`; without `$DISPLAY` every call fails with `XENUS_ERR_DISPLAY`.
* **Single-threaded**, like the rest of XENUS.
* **Versioned.** `XENUS_API_VERSION_MAJOR` / `MINOR` (`1.0`), checked with
  `xenus_api_version_check()`.

## Version and status codes

```c
int rc = xenus_api_version_check(XENUS_API_VERSION_MAJOR,
                                XENUS_API_VERSION_MINOR);   /* XENUS_OK */
if (rc != XENUS_OK)
        fprintf(stderr, "%s\n", xenus_strerror(rc));
```

| code | meaning |
|------|---------|
| `XENUS_OK` | success |
| `XENUS_ERR` | generic failure |
| `XENUS_ERR_ARG` | bad argument |
| `XENUS_ERR_STATE` | not valid in the current state |
| `XENUS_ERR_DISPLAY` | no X display available |
| `XENUS_ERR_NOT_FOUND` | unknown key / service / file |
| `XENUS_ERR_VERSION` | API or plugin ABI mismatch |
| `XENUS_ERR_IO` | read/write or loader failure |
| `XENUS_ERR_FULL` | fixed-size table exhausted |

Logging: `xenus_log(level, msg)` / `xenus_logf(level, fmt, ...)`, sink
replaceable with `xenus_log_set_sink()` (NULL restores stderr).

## Applications

```c
#include <xenus/app.h>

xenus_app_info_t info = { "myapp", "0.1", "what it does", "me" };

xenus_app_init(&info);                 /* strings are copied */
/* ... */
xenus_app_shutdown();                  /* idempotent */
```

`xenus_app_init()` also registers the built-in services:

| service | args | out |
|---------|------|-----|
| `version` | – | API version string |
| `notify` | `"title\nbody"` (or just `title`) | – |
| `config.get` | config key | value (`XENUS_ERR_NOT_FOUND` if absent) |

Own services: `xenus_service_register(name, fn, user)`,
`xenus_service_unregister(name)`, `xenus_service_call(name, args, out, len)`.

## Events

```c
static void on_event(const xenus_event_t *ev, void *user) { ... }

xenus_event_subscribe(XENUS_EVENT_ANY, on_event, NULL);   /* or a single type */
xenus_event_post(&ev);                 /* queue, oldest dropped when full */
xenus_event_dispatch();                /* delivers queued events, returns # */
```

Types: `XENUS_EVENT_APP_INIT/SHUTDOWN`, `NOTIFICATION`, `CONFIG_CHANGED`,
`PLUGIN_LOADED`, `PLUGIN_FAILED`, `WINDOW_EXPOSE/CLOSE/BUTTON/KEY/CONFIGURE`,
`XENUS_EVENT_CUSTOM` (1000+, for applications).
`XENUS_EVENT_ANY` is subscription-only and can never be posted.

## Notifications

```c
xenus_notify("Title", "body");          /* ring (32) + log + event */
xenus_notify_last(&n);                  /* newest, XENUS_ERR_NOT_FOUND if empty */
```

The ring keeps the newest 32 entries; `xenus_notify_total()` counts all since
process start.

## Configuration

Same `key = value` file the session uses (`$XENUS_CONFIG` or
`~/.config/xenus/config`), read independently of the window manager:

```c
xenus_config_load(NULL);                /* or an explicit path */
const char *v = xenus_config_get("name", "default");
int n = xenus_config_get_int("gap", 4);
int on = xenus_config_get_bool("focus_follows_mouse", 1);
```

`xenus_config_load()` posts `XENUS_EVENT_CONFIG_CHANGED`.

## Network transport

`<xenus/net.h>` is an app-facing transport layer: it discovers a resolver,
resolves IPv4/IPv6 names itself (UDP with TCP fallback), and opens connected
TCP or UDP sockets.  It does not use `getaddrinfo()`, so it works in a PRoot
container without a system resolver.  It is linked into `libxenusapp.a` and
installed with the other public headers.

```c
xenus_net_init(NULL);                  /* or explicit timeout/servers */
xenus_net_addr_t addrs[XENUS_NET_ADDR_MAX];
int n = xenus_net_resolve("example.com", addrs, XENUS_NET_ADDR_MAX, 5000);
int fd = xenus_net_tcp_connect("example.com", 80, 5000);
xenus_net_send_all(fd, request, request_len, 5000);
int got = xenus_net_recv(fd, buf, sizeof(buf), 5000);
xenus_net_close(fd);
xenus_net_shutdown();
```

`xenus_net_config_default()` reads `$XENUS_NET_DNS` (comma/space separated),
then `/etc/resolv.conf`, the systemd stub resolver, and the Termux
`resolv.conf`.  All calls return `XENUS_NET_OK` or one of the
`XENUS_NET_ERR_*` statuses; `xenus_net_strerror()` describes them.
`xenus_net_get_info()` reports socket availability and the servers in use.

This is transport only: HTTP is plain TCP, and HTTPS means TCP port 443.
TLS is left to the application.  The driver is not part of `xenus_api_t`, so
the plugin ABI is unchanged; plugins that need networking can link
`libxenusapp.a` directly.

## Windows

```c
xenus_window_attrs_t a = {
    .x = XENUS_WINDOW_AUTO, .y = XENUS_WINDOW_AUTO,
    .width = 640, .height = 400, .title = "hello",
    .background = 0x1c1f26
};
xenus_window_t *w = NULL;
if (xenus_window_create(&a, &w) == XENUS_OK) {
        xenus_window_show(w);
        while (running) {
                xenus_window_process_events(w, 0);   /* -> xenus_event_t */
                xenus_event_dispatch();
        }
        xenus_window_destroy(w);
}
```

XENUS's own event loop dispatches App API events every iteration, but it does
**not** pump API windows created by plugins (see Limitations).

## Plugins

A plugin is one shared object exporting exactly three symbols:

```c
XENUS_PLUGIN_EXPORT const xenus_plugin_info_t *xenus_plugin_info(void);
XENUS_PLUGIN_EXPORT int  xenus_plugin_init(const xenus_api_t *api);
XENUS_PLUGIN_EXPORT void xenus_plugin_shutdown(void);
```

`xenus_plugin_info()` returns static metadata:

```c
static const xenus_plugin_info_t info = {
        .info_size   = sizeof(xenus_plugin_info_t),
        .abi_version = XENUS_PLUGIN_ABI_VERSION,     /* 1 */
        .name        = "example",
        .version     = "1.0.0",
        .description = "Minimal XENUS example plugin",
        .api_major   = XENUS_API_VERSION_MAJOR,
        .api_minor   = XENUS_API_VERSION_MINOR
};
```

The host loads the file with `dlopen(RTLD_NOW|RTLD_LOCAL)` and validates, in
order, before `init()` is ever called:

1. the three entry points exist,
2. `info_size` covers at least the v1 layout,
3. `abi_version == XENUS_PLUGIN_ABI_VERSION`,
4. name and version are non-empty,
5. required API version passes `xenus_api_version_check()`.

Then `xenus_plugin_init(api)` runs: `0` marks the plugin **running**, any
other value marks it **failed** (and `shutdown()` is *not* called).
Failures are isolated - the offending plugin is logged and reported through
`XENUS_EVENT_PLUGIN_FAILED`, XENUS keeps running.

Plugins talk to XENUS **only** through the `xenus_api_t` table passed to
`init()` (`version_check`, `log`, `notify`, `config_get`, `config_get_int`,
`event_subscribe`, `event_post`, `service_call`).  They never link XENUS
symbols, so a plugin build needs nothing but libc and the public headers:

```sh
gcc -Iinclude -fPIC -shared -o myplugin.so myplugin.c
```

### Manager

```c
xenus_plugins_init(NULL);      /* path list, or $XENUS_PLUGIN_PATH, or defaults */
xenus_plugins_discover();      /* scan for *.so, sorted by name */
xenus_plugins_load_all();      /* never fails because a plugin fails */
xenus_plugins_unload("example");
xenus_plugins_shutdown();
```

Default search path: `~/.local/share/xenus/plugins:/usr/local/lib/xenus/plugins:/usr/lib/xenus/plugins`.

States (`xenus_plugin_state_name()` gives the string):

```
DISCOVERED -> LOADED -> RUNNING      (init returned 0)
     \           \-> FAILED          (open/validate/init failed)
RUNNING -> UNLOADED                  (shutdown + dlclose)
```

Inspection: `xenus_plugins_count()`, `xenus_plugins_running()`,
`xenus_plugins_state(i)`, `xenus_plugins_info(i)`, `xenus_plugins_error(i)`,
`xenus_plugins_file(i)`.

`xenus` itself initializes the manager at startup
(`src/main.c`): `xenus_app_init()`, then discover + load before the event
loop, and shut everything down on exit.

## Building and testing

```sh
make            # xenus, test helpers, build/libxenusapp.a, plugins
make plugins    # only build/plugins/{libxenus-example,test-badver,test-failinit}.so
make test       # HAL tests, App API tests, then the Xvfb suite
make test-net   # live DNS + HTTP + TCP/443 transport checks
```

`tests/api_test <example.so> <badver.so> <failinit.so>` runs headless
(no X server needed) and covers version/status helpers, app lifecycle,
config, events, notifications, services, window calls without a display,
plugin discovery of junk files, and the real plugins:

* `plugins/example/example.c` - loads, logs, notifies, shuts down,
* `tests/plugins/badver.c` - requires API 99.0, rejected before `init()`,
* `tests/plugins/failinit.c` - `init()` returns non-zero, never running.

## Limitations

* The window API is minimal (create/show/hide/title/size/background and
  event translation); there are no widgets, no ICCCM/EWMH beyond
  `WM_DELETE_WINDOW`, and plugin windows are not pumped by XENUS's loop.
* Configuration access is read-only; changes are made by editing the file
  (the reload posts `CONFIG_CHANGED`).
* The plugin ABI is version 1 with a fixed 32-entry table; there is no
  sandboxing, per-plugin capabilities, or dependency resolution yet.
* Everything is single-threaded and fixed-size; long input is truncated by
  the static buffers rather than rejected.
