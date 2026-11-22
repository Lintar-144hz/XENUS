# XENUS — Solo Coding Desktop Environment
<div align="center">

<h2>XENUS in Action</h2>

<p>
  A lightweight Linux desktop environment built from the ground up in C.<br>
  <b>XENUS — Minimal Desktop, Maximum Control.</b>
</p>

<br>

<img src="screenshot-desktop.png" alt="XENUS Desktop" width="900">

<br><br>

<table>
  <tr>
    <td align="center">
      <img src="screenshot-menu.png" alt="XENUS Application Menu" width="420">
      <br>
      <sub><b>Application Menu</b></sub>
    </td>
    <td align="center">
      <img src="screenshot-firefox.png" alt="Firefox running on XENUS" width="420">
      <br>
      <sub><b>Applications</b></sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <img src="screenshot.png" alt="XENUS" width="420">
      <br>
      <sub><b>Desktop Environment</b></sub>
    </td>
    <td align="center">
      <img src="screenshot-missing.png" alt="XENUS Fallback State" width="420">
      <br>
      <sub><b>Fallback State</b></sub>
    </td>
  </tr>
</table>

</div>
XENUS (version 0.2) is a lightweight window manager / desktop environment written
from scratch in **C11 + Xlib** and built with a plain **Makefile**. One
event-driven process: no busy polling, no threads, no daemons, no
GTK/Qt/Electron, no compositor.

The goal is a small, readable, self-contained desktop that is pleasant to use
on modest hardware — initially an **ARM64** phone running Ubuntu under PRoot
with **Termux:X11** as the X server, and portable to ordinary Linux desktops.

```
$ ldd xenus
    libX11.so.6 => ...
    libc.so.6 => ...
```

Only `libX11` and libc are required at run time.

---

## Current Status

### Implemented

* **Window management** — reparenting title-bar frames with a macOS-like
  chrome (red/yellow/green dots on the left, glyphs on hover, title beside
  the controls),
  focus-follows-mouse
  (or click-to-focus with `focus_follows_mouse=0`), `Alt`+drag move,
  `Alt`+right-drag / edge resize, double-click title to maximize,
  `Alt+Up`/`Alt+Down`,
  half-screen snap, `Alt+Tab` / `Shift+Alt+Tab`, `Alt+F4`, `Alt+D`
  (show desktop), `_NET_WM_STATE_FULLSCREEN`, `ConfigureRequest` handling.
* **Panel** — Harbor shell: a slim top menu bar (XENUS mark, active
  application with About/Hide/Close, Window menu, Help menu, tray glyphs,
  macOS-style clock) and a centered floating bottom dock (launcher tile,
  one tile per running application with a multi-window count badge and a
  running dot per window, accent dots for the focused one; left click
  focuses/restores, middle click closes, right click opens the app menu,
  and hovering a running tile reveals a close button (×)).
* **Application menu** — pinned entries plus every
  `/usr/share/applications/*.desktop`, compact and searchable (type to
  filter, arrows to move, `Enter` runs, `Esc` closes), `Alt+Space`.
* **Command launcher** — `Alt+F2`, runs a command or opens a URL in the
  detected browser.
 * **Desktop menu** — right click on the wallpaper: terminal, editor, file
   manager, browser, Run…, Applications, New Folder, New File, Refresh,
   Control Center, System Center, Notifications, Security Center, Log out.
* **Desktop shortcuts** — `~/Desktop` is drawn on the wallpaper: click to
  select, double click to open (`*.desktop` runs its `Exec=`), right click
  for Open / Open in File Manager / Properties / Set as wallpaper / Refresh,
  drag to rearrange (positions kept in
  `~/.local/share/xenus/desktop.layout`).
 * **Message dialogs** — including the Plasma-style "program not installed"
   notice instead of failing silently.
 * **Security Center** — built-in security application (no terminal): tray
   shield or the launcher's "Security Center" entry opens a card-style window
   with a live status pill, the protection mode (Always-on / Passive / Off),
   bounded scans (plugins / Desktop / Home), permission toggles and a
   scrollable security log.
* **System Center** — the built-in system application (`Alt+X`): Overview
  (CPU/memory/load/uptime), Processes with real `TERM`/`STOP`/`CONT`
  actions, Applications from the freedesktop registry, Services, Device
  facts from procfs/sysfs/uname, and the UCore activity log. All data comes
  from UCore; nothing is mocked.
* **Control Center** — Android-inspired quick settings (`Alt+C`): Wi-Fi,
  Bluetooth, Airplane mode, Do Not Disturb, light/dark appearance and a
  System Center shortcut, plus volume and brightness sliders, battery /
  external-power state, workspace switcher and a Desktop Settings page
  (theme, accent, animations). Capabilities the host does not expose are
  drawn as *not exposed* and never toggle a dummy.
* **Notification Center** — `Alt+N` or the tray bell: notification history
  from the App API ring, unread badge, Do Not Disturb, clear-all and wheel
  scrolling, with an empty-state card when there is nothing to show.
* **UCore** — the dependency-free hybrid C runtime inside the XENUS process
  (no daemon, no threads): sysinfo/load/procfs process snapshots, the
  freedesktop app registry, service and module registries, a bounded log
  ring, an event bus and an honest hardware probe. The event loop calls
  `xenus_core_tick()`; see `include/xenus/ucore.h` and `docs/UCORE.md`.
* **Workspaces** — four workspaces with `Ctrl+Alt+Left/Right` to switch and
  `Ctrl+Alt+Shift+Left/Right` to move the active window. Existing
  single-desktop behaviour is unchanged until you switch.
* **Sandbox resource limits** — every sandboxed process gets a 3 GiB RAM
  ceiling (3072 MiB, `RLIMIT_DATA` applied in the child before exec, capped
  and never raised; configurable down via `sandbox_memory_mb`), while XENUS
  imposes no CPU quota of its own — the host scheduler decides.
* **Built-in file manager** — `Alt+E`, Finder-inspired: compact toolbar
  with Back / Forward / Up, path field, live search filter (`Ctrl+F`),
  Name/Size/Kind column headers, Favorites + Locations sidebar (Home,
  Desktop, Documents, Downloads, …, Computer), navigate, open, rename
  (`F2`), delete (confirmed), new folder (`Ctrl+N`), hidden files (`Ctrl+H`),
  right-click menu, clear errors for unreadable folders.
* **Media** — files are classified by extension and opened with an installed
  viewer: images → `feh`, video/music → `mpv`; `wallpaper=` in the config
  loads a background image.
* **Configuration** — `~/.config/xenus/config`, plain `key=value`, unknown keys
  reported and ignored, built-in defaults when the file is missing.
* **Lifecycle** — single-instance `fcntl` lock, clean shutdown on
  `SIGINT`/`SIGTERM`/`SIGHUP` with clients reparented back to the root.
* **Test suite** — 146 automated checks on Xvfb plus three small X test
  clients.
* **HAL** — a separate, dependency-free static library (`hal/libhal.a`)
  with GPU / Bluetooth / Audio APIs, Linux and Android backends, and its own
  182-check smoke test.

### In Development

* **HAL ↔ desktop integration.** The library builds, is tested, and detects
  this device's hardware, but `src/` does not link it yet. The panel's status
  glyphs are still drawn from fixed shapes, not from HAL data.

### Planned

Not implemented yet — see [Roadmap](#roadmap).

* Built-in terminal emulator
* Built-in text editor
* Settings UI (configuration is file-only today)
* Notification service
* Clipboard
* Power management
* Real system tray (`_NET_SYSTEM_TRAY` / XEmbed)
* Network and device integration wired to real status data

---

## Features

| Area | Behaviour |
|---|---|
| Look | Harbor theme with a macOS-like window chrome: reparented frame windows, calm charcoal gradient wallpaper with a quiet ellipse motif, dark title bar with red/yellow/green traffic-light dots on the left (glyphs on hover) and the title beside the controls, double-click title = maximize |
| Window management | MapRequest, ConfigureRequest, DestroyNotify, UnmapNotify, focus-follows-mouse, fullscreen via `_NET_WM_STATE_FULLSCREEN` |
| Move | `Alt` + Left-drag, or drag the title bar |
| Resize | `Alt` + Right-drag, or drag a window edge (5 px) |
| Close | `Alt+F4`, the left close dot, the dock hover \(x\), or the dock app menu — always a graceful `WM_DELETE_WINDOW` request; an application that does not advertise the protocol is never force-killed (a notice explains why) |
| Maximize / minimize | `Alt+Up` toggle, `Alt+Down`, the left maximise / minimise dots |
| Snap | `Alt+Left` / `Alt+Right` half screen |
| Show desktop | `Alt+D` or the Window menu's Show Desktop entry |
| Switch windows | `Alt+Tab`, `Shift+Alt+Tab` (wraps) |
| Panel | Harbor shell: a slim top menu bar (XENUS mark, active application with About/Hide/Close, Window menu listing every window + Show Desktop, Help menu, tray glyphs for network / volume / battery / Control Center / Notifications / Security shield, macOS-style clock) and a centered floating bottom dock (launcher tile, one tile per running application with a window-count badge and one dot per window, accent dots + lifted tile for the focused one; left click focuses/restores, middle click closes, right click opens Show / Minimize / Maximize / Close / Quit, hover reveals a close button (×)) |
| App menu | pinned entries + every `/usr/share/applications/*.desktop`, type-to-search with icon tiles, `Alt+Space` or the launcher button |
| Program detection | terminal / editor / browser picked from `PATH` (or your `terminal=`, `browser=` config); built-in menu entries are `@terminal`, `@editor`, `@files`, `@browser`, `@apps`, `@run`, `@desktop`, `@control`, `@control-settings`, `@notifications`, `@system`, `@security`, `@logout` |
| Missing program | a Plasma-style notice dialog tells you what to install instead of failing silently |
| Desktop menu | right click on the wallpaper: terminal, editor, file manager, browser, Run…, Applications, New Folder, New File, Refresh, Control Center, System Center, Notifications, Security Center, Log out |
| Desktop shortcuts | `~/Desktop` on the wallpaper: click selects, double click opens (a `*.desktop` entry runs its `Exec=`), right click = icon menu, drag = rearrange (saved in `~/.local/share/xenus/desktop.layout`); installed applications are shown as icons on the left half (`desktop_apps=1`) |
| Command launcher | `Alt+F2` — runs a command, or opens a URL (`example.com`, `https://…`) in the detected browser |
| Terminal | `Alt+Enter` (configurable) — starts the detected external terminal |
| File manager | `Alt+E` — built in, Finder-inspired: toolbar with Back / Forward / Up / New Folder / Refresh, path field, live search filter, Name/Size/Kind column headers, Favorites + Locations sidebar (Home, Desktop, Documents, …), navigate, rename (F2), delete, new folder, right-click menu, hidden files (Ctrl+H), clear errors for unreadable folders |
| Security Center | tray shield or launcher "Security Center": protection mode (Always-on starts the filesystem watcher), card layout with live status pill, scanner (plugins / Desktop / Home, bounded), permission toggles, scrollable security log with Clear |
| System Center | `Alt+X` — Overview, Processes (real actions), Applications, Services, Device and Activity tabs backed by UCore |
| XENUS BIOS | `xfirm` opens the standalone firmware-style setup UI (X-FIRM): staged System / Drivers / UCore / Android / Boot / Recovery pages, Apply and recovery backups. Runs without the desktop or UCore; see `docs/BIOS.md` |
| Control Center | `Alt+C`, the tray tile or the launcher/desktop "Control Panel" entry — quick settings tiles, volume/brightness sliders, battery, workspaces and Desktop Settings (theme/accent/animations) |
| Notifications | `Alt+N` or the tray bell — history, unread badge, DND, clear all |
| Workspaces | `Ctrl+Alt+Left/Right` switches; `Ctrl+Alt+Shift+Left/Right` moves the active window |
| Media | photos open with feh, video/music with mpv; `wallpaper=` in the config sets the background |
| Config | `~/.config/xenus/config` (key=value) |
| Shutdown | clean `SIGINT`/`SIGTERM`/`SIGHUP` handling, clients reparented back to the root |
| Single instance | `fcntl` lock file in `$XDG_CACHE_HOME/xenus` (auto-released on crash) |

Extra: `_NET_ACTIVE_WINDOW`, `_NET_CLIENT_LIST`, `_NET_SUPPORTED`,
`_NET_SUPPORTING_WM_CHECK`, `WM_STATE`, window title tracking in the menu bar.

---

## Architecture

```
xenus/
├── Makefile                 top-level build (this is what `make` runs)
├── README.md
├── config/
│   └── xenus-config.sample   annotated configuration template
├── docs/
│   ├── XENUS_APP_API.md      App API + plugin ABI reference
│   └── BIOS.md               XENUS BIOS (X-FIRM) setup environment
├── include/xenus/            public App API headers (app, event, plugin, ...)
├── plugins/example/         example plugin (built by `make plugins`)
├── src/                     the desktop itself — one process
│   ├── xenus.h               shared types, config struct, prototypes
│   ├── main.c               X connection, setup, poll() loop, signals, lock
│   ├── wm.c                 client list, frame windows, focus, close, drag
│   ├── input.c              key/button grabs, Alt-drag, desktop clicks
│   ├── panel.c              top menu bar (XENUS/app/Window/Help, tray, clock)
│   ├── dock.c               floating bottom dock (launcher, app tiles, dots)
│   ├── launcher.c           application menu + Alt+F2 prompt
│   ├── desktop.c            desktop shortcuts: icons, menus, moving
│   ├── fm.c                 built-in file manager
│   ├── apps.c               program detection, launch helpers, media
│   ├── msgbox.c             modal message dialogs
│   ├── security_gui.c       built-in Security Center window
│   ├── system_center.c      System Center application (UCore-backed)
│   ├── control_center.c     Android-inspired quick settings panel
│   ├── notification_center.c notification history + unread badge
│   ├── core/                UCore runtime (sysinfo, proc, apps, events)
│   ├── bios/                standalone BIOS engine, Xlib setup UI, xfirm
│   ├── config.c             config parser
│   ├── api/                 App API implementation (libxenusapp.a)
│   ├── plugin/              plugin loader + manager (dlopen)
│   ├── security/            security core: scanner, policy, audit, trust
│   └── util.c               logging, spawn, colours, text helpers
├── hal/                     separate hardware abstraction library
│   ├── Makefile             builds libhal.a + its two tools
│   ├── include/hal/         hal.h, gpu.h, bluetooth.h, audio.h
│   ├── src/                 generic module logic (hal.c, gpu.c, ...)
│   │   ├── backends/linux/    DRM sysfs, rfkill/HCI, ALSA
│   │   └── backends/android/  Android detection + property/sysfs readers
│   └── tests/               hal_smoke.c (182 checks), hal_info.c
├── tests/                   X11 test suite
│   ├── run_tests.sh         146 checks on Xvfb (timeout-guarded by make)
│   ├── api_test.c           App API tests (headless, 300 checks)
│   ├── plugins/             badver/failinit test plugins
│   ├── testclient.c         minimal X client used by the suite
│   ├── rootpixel.c          reads the root window pixel
│   ├── screenshot.c         dumps the root window as PPM
│   └── shoot.sh             one-shot screenshot helper
└── screenshot*.png          captured example sessions
```

Build artifacts (all removed by `make clean`):

| Path | Produced by |
|---|---|
| `build/src/*.o`, `*.d` | `make` (release objects + auto header deps) |
| `build/libxenusapp.a` | `make` (App API + plugin system) |
| `build/plugins/*.so` | `make plugins` (example + test plugins) |
| `xenus` | `make` |
| `tests/testclient`, `tests/rootpixel`, `tests/screenshot`, `tests/api_test` | `make` |
| `build-debug/`, `xenus-debug` | `make debug` |
| `hal/src/**/*.o`, `hal/libhal.a`, `hal/tests/hal_smoke`, `hal/tests/hal_info` | `make hal` / `make test` |

`src/` and `hal/` are **independent**: the desktop does not link the HAL.
There is no `configure` script, no generated sources, and no submodules.

---

## App API and plugins

XENUS exposes a small native C API (`include/xenus/*.h`, implemented in
`src/api/` + `src/plugin/`, linked as `build/libxenusapp.a`): app lifecycle and
services, an event queue, notifications, read-only config access, minimal
top-level windows, and a **plugin system** (one `.so` exporting
`xenus_plugin_info` / `xenus_plugin_init` / `xenus_plugin_shutdown`, validated
against ABI + API version before `init()` runs, communicating only through a
versioned `xenus_api_t` table).  Plugins are discovered from
`~/.local/share/xenus/plugins` (plus `$XENUS_PLUGIN_PATH`); a broken plugin is
logged, marked failed, and never takes the session down.

See **`docs/XENUS_APP_API.md`** for the full reference, and note that
`make test` runs the headless API tests (`tests/api_test`, 300 checks)
between the HAL tests and the Xvfb suite.

---

## HAL

`hal/` is a standalone C11 static library (`libhal.a`) with **no external
dependencies** — only libc and kernel UAPI headers. It is built and tested by
its own Makefile; the top-level `make test` drives it.

### Modules and generic API

| Module | Header | Capabilities |
|---|---|---|
| GPU | `hal/gpu.h` | `HAL_GPU_CAP_RENDER`, `HAL_GPU_CAP_MODESET`, `HAL_GPU_CAP_SOFTWARE` |
| Bluetooth | `hal/bluetooth.h` | `HAL_BT_CAP_ADAPTER`, `HAL_BT_CAP_POWER`, `HAL_BT_CAP_DISCOVERY` |
| Audio | `hal/audio.h` | `HAL_AUDIO_CAP_PLAYBACK`, `HAL_AUDIO_CAP_CAPTURE`, `HAL_AUDIO_CAP_MIXER`, `HAL_AUDIO_CAP_PULSE` |

Generic lifecycle and discovery in `hal/hal.h`:

```c
hal_status_t hal_init(const hal_init_opts_t *opts);
void         hal_shutdown(void);
bool         hal_is_inited(void);
bool         hal_is_android(void);
bool         hal_module_available(hal_module_t module);
uint32_t     hal_available_mask(void);
hal_status_t hal_module_get_info(hal_module_t module, hal_module_info_t *out);
```

plus per-module calls such as `hal_gpu_primary()`,
`hal_bluetooth_adapter_count()`, `hal_audio_device_count()`,
`hal_audio_get_master_volume()`, `hal_bluetooth_set_powered()`.

### Backends and hardware detection

* **Linux backend** — GPU from `/sys/class/drm` (vendor/device/driver/VRAM),
  Bluetooth from `/dev/rfkill` + HCI, audio from `/proc/asound` and `/dev/snd`.
* **Android backend** — selected when `/system/build.prop`,
  `/system/bin/app_process64` (or `app_process32`) exists; reads the same
  sysfs/property interfaces available on Android.
* `hal_init()` picks the backend at run time via `hal_is_android()`.

### Graceful handling of unavailable hardware

A module that cannot be probed reports `available = no`, `caps = 0x0`, and
per-call statuses such as `unsupported` / `unavailable` instead of failing the
process. Example on this ARM64/Android device:

```
$ hal/tests/hal_info
HAL 0.1
available mask: 0x5
gpu:
  backend=android available=yes caps=0x1
  [0] Adreno (kgsl-3d0) vendor=Qualcomm(0x0000) driver=kgsl type=integrated ...
bluetooth:
  backend=android available=no caps=0x0
audio:
  backend=android available=yes caps=0x8
```

That is what the hardware on the development device exposes — it is **not** a
claim of support for any other GPU, adapter, or sound card. Anything the HAL
does not probe stays reported as unavailable.

> The HAL is **not linked into `xenus`** yet (see Current Status).

---

## Requirements

**Build:**

* gcc (override the compiler with `make CC=clang`)
* GNU make
* libX11 headers

```sh
sudo apt update
sudo apt install -y build-essential libx11-dev git
```

**Run:** a running X server and a valid `$DISPLAY` (Termux:X11, Xorg, Xvfb…).
Optional but useful: a web browser (for `@browser` and `Alt+F2` URLs),
`feh` (images), `mpv` (video/audio), and a terminal emulator
(for `Alt+Enter` / `@terminal`). XENUS detects these on `PATH` and shows a
dialog when one is missing.

**Test only:** `xvfb`, `x11-utils` (`xwininfo`, `xprop`, `xdpyinfo`),
`xdotool`, and `timeout` (coreutils):

```sh
sudo apt install -y xvfb x11-utils xdotool
```

This is a C project — dependencies are system packages managed by the
Makefile, not a language-level requirements file.

### Note about Firefox on this PRoot image

Ubuntu 24.04 only ships a snap stub for Firefox, so it was installed manually
on the development device (not a XENUS dependency):

* Firefox ESR 140.17.0 → `/opt/esr/firefox`, launcher `/usr/local/bin/firefox`
* the launcher sets `MOZ_DISABLE_CONTENT_SANDBOX=1`: this device's kernel
  refuses `F_ADD_SEALS` (`EPERM`), which makes Firefox 15x reject its own
  shared memory and crash every content process
* first start takes ~15–40 s (profile creation), later starts are faster

---

## Build

```sh
make
```

Compiles `src/*.c` into `build/src/*.o` (with `-MMD -MP` header tracking) and
links the `xenus` binary plus the X test helpers and the standalone
`xenus-bios` / `xfirm` pair. Only the files whose sources or headers changed
are rebuilt.

Flags actually used:

```
-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2
```

No architecture flags (`-march`, `-mcpu`, …) are used: build natively for your
target. Override optimisation with `make OPT=-O0` or add extras with
`make EXTRA_CFLAGS=…`.

```sh
make hal        # build hal/libhal.a and its tools only
make bios       # build xenus-bios and the xfirm launcher only
```

Binary: ~150 KB (~131 KB stripped), one process, one thread, roughly 1–3 MB
resident memory depending on configuration (the test suite asserts < 40 MB).

---

## Run

```sh
make run                       # ./xenus
make run ARGS='-c my.conf'     # pass arguments through
```

or run the binary directly:

```sh
./xenus                # uses ~/.config/xenus/config
./xenus -c my.conf     # explicit config
./xenus -v             # version
./xenus -h             # help + key bindings
```

**An X server is required**: XENUS calls `XOpenDisplay(NULL)`, so `$DISPLAY`
must be set. Without one it exits with `cannot open display ''`.

### XENUS BIOS

```sh
./xfirm                # open the setup environment
```

`xfirm` starts Termux:X11 when `$DISPLAY` is not reachable (on Android) and
then execs `xenus-bios`. The setup UI is fully standalone: it does not need
`xenus` or UCore and is the recommended recovery path when the desktop will not
start. See `docs/BIOS.md` for every page and the exact boundary of what BIOS
can change.

Stop XENUS with `kill -TERM <pid>` (or Ctrl-C on the terminal that started it).
A second instance on the same account is refused by the single-instance lock.

---

## Debug

```sh
make debug
./xenus-debug
```

Builds `xenus-debug` from `build-debug/` with `-O0 -g`, keeping the release
build in `build/` / `xenus` untouched — you can switch between them without a
`make clean`.

---

## Testing

```sh
make test
```

Runs the following suites and reports each:

1. **HAL** — `hal/tests/hal_smoke` (182 checks) and `hal/tests/hal_info`.
2. **UCore** — `tests/ucore_test` (headless: lifecycle, sysinfo, process/app/
   service/module snapshots, hardware honesty, DND, events, status strings).
3. **BIOS** — `tests/bios_test` (headless: defaults, staged apply, validation,
   diff, atomic store, backups/restore, factory reset, shell sync, runtime
   helpers).
4. **App API** — `tests/api_test` (headless, 300 checks).
5. **Security boundary** — `tests/security_test`.
6. **XENUS on Xvfb** — `tests/run_tests.sh` (146 checks): startup/single
   instance, menu bar + dock geometry, background colour, mapping and borders,
   `_NET_CLIENT_LIST`, focus-follows-mouse, `Alt+Tab` both directions,
   `Alt+drag` move & resize, `ConfigureRequest`, `Alt+F4`, app menu
   open/close/run, `Alt+F2` prompt, help/version, SIGTERM shutdown + restart,
   one process / one thread / RSS / idle CPU / binary size, client
   destruction, maximize/snap/minimize/show-desktop, desktop menu,
   `Alt+F2` URL → browser, the missing-program notice dialog, the built-in
   file manager, the desktop shortcuts (icon painted at its saved
   position, select / deselect by pixel, double click runs `Exec=`, icon
   context menu, New Folder from the desktop menu, drag rewrites the layout
   file), the taskbar registry for File Manager / XENUS Security (open,
   minimise, restore, close, show-desktop), the menu bar launcher menu, the
   dock (growth, left-click restore, right-click app menu, hover close,
   per-application window grouping, graceful Quit, shrink on close),
   the sandbox memory config key, the Harbor redesign (Favorites /
   Locations sidebar, Back / Forward history, live search filter, column
   headers, Security Center cards, gradient wallpaper, dock running dots),
   and the UCore surfaces (Control Center quick settings + persisted theme,
   Notification Center, System Center with its taskbar entry, and workspace
   switching).

The X suite is wrapped in a hard timeout (default **300 s**, override with
`make test TEST_TIMEOUT=600`) so it can never hang the build. It starts its
own Xvfb on display `:97` and a private `$HOME`, traps `INT`/`TERM` to kill
Xvfb, XENUS and test clients and to delete its temp directory — nothing is left
running and no log file grows. It counts only the XENUS instances on its own
display, so a desktop session running elsewhere on the machine does not affect
the result.

Needs `xvfb x11-utils xdotool`. Individual suites:

```sh
bash tests/run_tests.sh   # X suite alone
make -C hal test          # HAL alone
make test-bios            # BIOS engine alone
make test-bios-gui        # BIOS under Xvfb alone
```

---

## Clean

```sh
make clean
```

Removes `build/`, `build-debug/`, `xenus`, `xenus-debug`, the test binaries, the
legacy `src/*.o`, and the HAL artifacts. Sources, `README.md`, the Makefiles,
`config/`, and the screenshots are never touched.

---

## Install / Uninstall

```sh
make install                       # /usr/local/bin/xenus + xenus-bios + xfirm + sample config
make PREFIX=/opt/xenus install      # different prefix
make DESTDIR=/tmp/stage install    # staged install for packaging
make uninstall                     # remove exactly what install added
```

Only the `xenus` binary and `config/xenus-config.sample` are installed. The HAL
is a build-time library only and is not installed. XENUS currently has no
`.desktop` entry, man page, or session file — those are planned, not missing
steps you need to run.

---

## Configuration

`~/.config/xenus/config` — see `config/xenus-config.sample`:

```
panel_height=26           # slim top menu bar (menu bar top, dock bottom fixed)
border_width=3            # frame border (>= 3 = usable resize grip)
title_height=24           # Harbor title bar
desktop_apps=1            # installed apps as desktop icons (left half)
focus_follows_mouse=1
show_clock=1
show_tray=1
sandbox_memory_mb=3072    # sandbox RAM ceiling per process (MiB, 3 GiB)
clock_format=%a %b %d  %H:%M
clock_date_format=%b %d   # second clock line (needs panel_height >= 30)
terminal=xterm            # empty = auto-detect from PATH
browser=                  # empty = auto-detect from PATH
wallpaper=                # image file, loaded with feh --bg-fill
font=-*-fixed-medium-r-*-*-13-*-*-*-*-*-*-*
background=#171b22
wallpaper_top=#262e3b     # vertical gradient wallpaper
wallpaper_bottom=#12151b
border_focused=#7c9cbf
border_unfocused=#2a313d
title_bg=#28303d
title_bg_inactive=#20262f
title_fg=#edf0f5
title_fg_inactive=#98a1b1
title_hover=#313b49
title_close_hover=#b04a4a
panel_bg=#1d232d
panel_fg=#dbe1ea
button_active=#334052
button_inactive=#262c37
button_hover=#2f3a4a
menu_bg=#222936
menu_fg=#dbe1ea
menu_selected=#38506b
menu=Terminal|xterm       # pinned entries, shown above the app list
menu=Editor|${EDITOR:-vi}
menu=Files|@files         # built-in file manager
```

Unknown keys are reported and ignored; missing file = built-in defaults.

---

## Design notes

* Single `poll()` on the X socket with a timeout computed to the next clock
  tick — the only wakeup is the clock, so idle CPU is 0 ticks / 3 s.
* Client windows **are** reparented into a frame window that carries the title
  bar and border; on shutdown they are reparented back to the root.
* Move/resize run inside a short nested loop over `XNextEvent` while a pointer
  grab is active (motion events are compressed); other events are dispatched
  normally during the drag.
* Key/button grabs are re-registered with every NumLock/CapsLock combination.

---

## Platform Support

| Platform | Status |
|---|---|
| **ARM64 (aarch64) Linux** | Initial focus. Developed and tested on Ubuntu 24.04 under PRoot on an Android phone, X server: Termux:X11. |
| **x86_64 Linux** | Expected to build and run — there are no architecture-specific flags and no assembly — but this tree has not been tested on x86_64 hardware. |
| **Android / Termux** | XENUS runs under Termux:X11; the HAL's Android backend is selected automatically when Android system files are present. |
| **Wayland** | Not supported. XENUS is core X11 only. |

**A binary built for one architecture cannot simply be executed on another.**
An ARM64 `xenus` will not run on x86_64 (and vice versa): copy the source
tree to the target machine and run `make` there — there is nothing to
cross-configure, only a native compile.

The HAL's Android backend also depends on Android-specific kernel interfaces
(`/dev/rfkill`, sysfs layouts); on a plain Linux box the Linux backend is used
instead and reports whatever that machine actually exposes.

---

## Roadmap

Already shipped — see [Current Status](#current-status): window management,
panel, application menu, command launcher, file manager, media opening,
dialogs, configuration, tests, and the HAL library.

Planned next, in rough order:

1. **HAL integration** — link `libhal.a` into `xenus` and drive the panel's
   status glyphs from real data (volume, network, adapter state).
2. **System tray** — implement `_NET_SYSTEM_TRAY` / XEmbed so real tray
   applications can dock; replace the decorative glyphs.
3. **Settings UI** — a dialog that reads and writes
   `~/.config/xenus/config`.
4. **Notifications** — a small transient notification window with a queue
   (distinct from the existing modal message dialogs).
5. **Clipboard** — `XFixes` selection ownership and a paste helper.
6. **Terminal and text editor** — optionally built in, while keeping
   external programs detected from `PATH`.
7. **Power and network integration** — battery/AC state and link state;
   this needs new HAL modules, since the HAL currently covers GPU,
   Bluetooth and audio only.

---

## Limitations

* **No compositor** — no transparency, shadows, or vsync; tearing is possible
  on some drivers.
* **No built-in terminal, text editor, or settings window** — XENUS launches
  whatever it finds on `PATH` and edits configuration as a text file.
* **The status glyphs are decorative** — there is no `_NET_SYSTEM_TRAY`
  implementation yet, and they do not reflect real hardware state.
* **No clipboard, notifications, power management, or network configuration.**
* **The HAL is not used by the desktop yet.**
* **One XENUS instance per user account**, enforced by a lock file under
  `$XDG_CACHE_HOME/xenus` (not per display).
* **Core X11 only** — no Wayland support, no accessibility APIs, no
  internationalised input methods beyond what the X server provides.
* **Tested on a single ARM64 device/distribution**; other setups should work
  but are unverified.
* See the Firefox note under [Requirements](#requirements) for a
  device-specific workaround that has nothing to do with XENUS itself.

---

## Future Vision

Long term, XENUS could grow into:

* a more complete desktop environment (session, settings, notifications),
* a Live ISO that boots straight into it,
* a standalone, self-contained Linux-based environment.

These are **future goals, not current capabilities**. Today XENUS is a window
manager/desktop shell plus a set of desktop helpers, built from one Makefile.

---

## Contributing

* Keep it C11 and dependency-light: the desktop links only `libX11` and libc,
  the HAL only libc.
* The build must stay warning-free: `-Wall -Wextra -Wpedantic` with no
  architecture-specific flags.
* New behaviour needs a check in `tests/run_tests.sh` (or `hal/tests`) — run
  `make clean && make && make test` before handing anything over.
* Match the existing style: 4-space indent, no tabs, ~80-column lines,
  `snake_case`, and a short `XENUS - …` file-header comment at the top of each
  source file.
* Do not add a feature to the README that is not in the source, and do not
  remove a check that is currently passing.

## Screenshots

`screenshot.png`, `screenshot-desktop.png`, `screenshot-menu.png`,
`screenshot-missing.png`, `screenshot-firefox.png` in the project root —
captured from a real session with `tests/shoot.sh`.
