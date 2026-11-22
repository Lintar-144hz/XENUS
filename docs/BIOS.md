# XENUS BIOS (X-FIRM)

XENUS BIOS is the firmware-style setup environment for XENUS. It is a
standalone `C11 + Xlib` program (`xenus-bios`) opened by the `xfirm` launcher.
All configuration happens in the GUI: there are no BIOS options on a command
line.

```
$ xfirm
```

`xfirm` does exactly two things:

1. Makes sure an X display is reachable. On Android/Termux it starts
   `termux-x11 :0` and brings the Termux:X11 activity forward when needed
   (unless `android.termux_x11` was disabled in a previous BIOS session).
2. Replaces itself with `xenus-bios`.

The setup environment does **not** start, link or require the XENUS desktop or
UCore. It remains usable when both are missing or broken, which is what makes
it a recovery path.

---

## Pages

| Page | What it controls |
|---|---|
| **Overview** | Live environment probe, platform/display/memory/uptime, firmware state and staged-change count |
| **System** | Shell theme (dark/light), accent (blue/silver/graphite), animations, menu-bar clock, focus-follows-mouse |
| **Drivers** | Internal userspace bridges: display, input and storage are required; audio, network and Android bridge are optional |
| **UCore** | Which UCore modules `xenus_core_init()` registers on the next desktop start |
| **Android Integration** | Termux API, Termux:X11, Android command bridge, notifications, clipboard, shared storage |
| **Boot** | Normal/safe startup mode, diagnostic logging, 0–10 second startup delay |
| **Recovery** | Back up, restore the latest backup, verify the store, discard the draft, restore factory defaults |
| **Save & Exit** | Review staged changes, Apply, Save & Exit, Discard & Exit |

Nothing is written until **Apply** or **Save & Exit**. The header always shows
`CLEAN`/`DIRTY` and `VALID`/`INVALID`; the footer explains the current state.

---

## Controls

* Mouse / touchscreen: click a page, row, segmented control, toggle or button.
* `F1`–`F8`: jump directly to a page.
* `Tab` / `PgDn` / `PgUp` / `Shift+Tab`: next or previous page.
* `Up` / `Down`: move the focused row.
* `Left` / `Right`: change the focused value.
* `Enter` / `Space`: activate the focused row.
* `Esc`: exit (asks first when the draft is dirty).
* `Ctrl+S`: Apply.

---

## Configuration store

BIOS keeps its own file, separate from the shell configuration:

```
$XDG_CONFIG_HOME/xenus/bios.conf        # or ~/.config/xenus/bios.conf
$XDG_CONFIG_HOME/xenus/backups/         # timestamped recovery copies
$XDG_CONFIG_HOME/xenus/config           # existing XENUS shell config
```

`bios.conf` is an INI-style text file written atomically (temporary file +
`rename`) and validated before it is saved:

```ini
# XENUS BIOS (X-FIRM) configuration
version=1

[system]
theme=dark            # dark | light
accent=blue           # blue | silver | graphite
animations=1
show_clock=1
focus_follows_mouse=1

[drivers]
display=1             # required
input=1               # required
storage=1             # required
audio=1
network=1
android_bridge=1

[ucore]
sysinfo=1             # required
process=1
applications=1
hardware=1
notifications=1

[android]
termux_api=1
termux_x11=1
android_bridge=1
notifications=1
clipboard=1
storage_access=0

[boot]
mode=normal           # normal | safe
diagnostic=0
delay_seconds=0       # 0..10

[recovery]
backup_before_apply=1
```

Unknown keys are ignored. Missing keys take their defaults. Invalid values
fall back to defaults rather than breaking the setup app, and blocking
validation errors refuse the Apply.

---

## What Apply actually changes

### Shell appearance

`theme`, `accent`, `animations`, `show_clock` and `focus_follows_mouse` are
mirrored into `~/.config/xenus/config`. The sync rewrites only those five keys;
comments, menu entries and every other line are preserved. XENUS reads them on
its next start, and the existing Control Center can refresh them live.

### UCore modules

`xenus_core_init()` consults the BIOS store before registering modules. With no
`bios.conf`, every module is registered exactly as before. Turning a module off
simply means fewer registered services; no data is fabricated. `sysinfo` is
mandatory because every other UCore consumer depends on it.

### Boot behaviour

The `xenus` binary reads the boot section at startup:

* **Safe mode** skips plugin loading and disables shell animation.
* **Diagnostic logging** writes the resolved config path, mode and delay to the
  XENUS log.
* **Boot delay** waits 0–10 seconds after the display is open and before the
  desktop UI settles.

### Drivers and Android

Optional driver flags disable the corresponding userspace bridge for the next
BIOS session. For example, a disabled Android bridge is no longer probed or
shown as available. These are **not** kernel modules and BIOS never calls
`modprobe`, changes Android firmware, or bypasses SELinux/permissions.

### Termux:X11

`android.termux_x11` is read by `xfirm`. When it is off and `$DISPLAY` is not
reachable, `xfirm` explains the problem and exits instead of starting a display
server.

---

## Recovery

Every successful Apply copies the previous store into `backups/` when
`recovery.backup_before_apply` is on. The directory keeps timestamped copies;
collisions within the same second get a numeric suffix. Recovery actions:

* **Back up current configuration** — copy the active store now.
* **Restore latest recovery backup** — validate the newest copy, then replace
  the active store with it.
* **Verify configuration store** — parse and validate the file on disk.
* **Discard staged changes** — drop the in-memory draft.
* **Restore factory defaults** — reset BIOS settings and the managed shell keys.

Restore and factory reset ask for confirmation first. Recovery only manages
XENUS BIOS configuration and the five managed shell keys; it never repartitions
storage or touches user data.

---

## Honest boundaries

XENUS BIOS is a userspace configuration environment, not PC firmware. It can:

* change XENUS shell settings and its own staged configuration;
* choose which UCore modules the desktop registers;
* disable optional Termux/Android bridge probing;
* change XENUS startup mode, logging and delay;
* inspect the platform, X11, memory, uptime, Termux API and Android commands
  that the current user can already reach.

It cannot and does not claim to:

* load/unload kernel drivers or change `/sys`/`/proc` kernel state;
* flash or modify Android firmware, bootloaders, partitions or SELinux;
* grant Android permissions the Termux user does not already have;
* run without an X server — use `xfirm` (Termux:X11 on Android).

Unavailable capabilities stay visibly unavailable. No hardware is simulated.

---

## Build and test

```sh
make bios          # build xenus-bios and xfirm
make test-bios     # headless engine test (staging, validation, persistence)
make test-bios-gui # Xvfb smoke test: window, keyboard staging, Apply, exit
```

`make` builds the BIOS binaries alongside `xenus`; `make install` installs
`xenus-bios` and `xfirm` into `$(PREFIX)/bin`. `make uninstall` removes them.

Settings are deliberately small and local; the binaries require only
`libX11` and libc, matching the rest of XENUS.

---

## Source layout

| File | Purpose |
|---|---|
| `include/xenus/bios.h` | Public configuration model, engine and runtime helpers |
| `src/bios/bios_config.c` | Store paths, parsing, atomic save, staging, validation, diff, backup/restore, shell sync |
| `src/bios/bios_backend.c` | Real platform/X11/Termux/Android/audio/network probing |
| `src/bios/bios_ui.c` | Standalone Xlib setup UI (sidebar, dashboard, pages, input) |
| `src/bios/xfirm.c` | Display/Termux:X11 launcher that execs `xenus-bios` |
| `tests/bios_test.c` | Headless engine test |
| `tests/bios_ui_test.sh` | End-to-end Xvfb GUI smoke test |
