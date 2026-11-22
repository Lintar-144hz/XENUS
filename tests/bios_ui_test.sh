#!/usr/bin/env bash
# XENUS BIOS GUI smoke test: standalone Xvfb session, keyboard staging and
# Apply persistence.  No XENUS desktop or UCore runtime is started.
set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN="$ROOT/xenus-bios"
DPY=":98"
WORK=$(mktemp -d /tmp/xenus-bios-ui.XXXXXX)
LOG="$WORK/bios.log"
XVFB_PID=""
BIOS_PID=""
PASS=0
FAIL=0

log() { printf '%s\n' "$*"; }
ok()  { PASS=$((PASS + 1)); log "  PASS: $*"; }
bad() { FAIL=$((FAIL + 1)); log "  FAIL: $*"; }

cleanup() {
    [ -n "$BIOS_PID" ] && kill "$BIOS_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

for tool in Xvfb xdotool xwininfo xdpyinfo timeout; do
    command -v "$tool" >/dev/null 2>&1 || {
        log "missing test dependency: $tool"; exit 2; }
done
[ -x "$BIN" ] || { log "build xenus-bios first (make bios)"; exit 2; }

export HOME="$WORK/home"
mkdir -p "$HOME"
STOREDIR="$HOME/.config/xenus"
STORE="$STOREDIR/bios.conf"
SHELLCFG="$STOREDIR/config"

Xvfb "$DPY" -screen 0 1024x768x24 >"$WORK/xvfb.log" 2>&1 &
XVFB_PID=$!
for _ in $(seq 1 40); do
    xdpyinfo -display "$DPY" >/dev/null 2>&1 && break
    sleep 0.1
done
if ! xdpyinfo -display "$DPY" >/dev/null 2>&1; then
    log "Xvfb failed to start"; exit 2
fi

DISPLAY="$DPY" "$BIN" >"$LOG" 2>&1 &
BIOS_PID=$!

WIN=""
for _ in $(seq 1 50); do
    WIN=$(DISPLAY="$DPY" xdotool search --name "XENUS BIOS" 2>/dev/null | head -1)
    [ -n "$WIN" ] && break
    sleep 0.1
done
if [ -z "$WIN" ]; then
    bad "BIOS window appears"
    log "--- log ---"; cat "$LOG"
    exit 1
fi
ok "BIOS window appears without a desktop or window manager"

GEOM=$(DISPLAY="$DPY" xwininfo -id "$WIN" 2>/dev/null)
W=$(printf '%s\n' "$GEOM" | sed -n 's/^ *Width:  *\([0-9]*\)/\1/p')
H=$(printf '%s\n' "$GEOM" | sed -n 's/^ *Height:  *\([0-9]*\)/\1/p')
if [ -n "$W" ] && [ -n "$H" ] && [ "$W" -gt 600 ] && [ "$H" -gt 400 ]; then
    ok "setup window has a usable geometry (${W}x${H})"
else
    bad "setup window geometry (${W}x${H})"
fi

DISPLAY="$DPY" xdotool windowfocus "$WIN" 2>/dev/null || true
DISPLAY="$DPY" xdotool key F2
sleep 0.2
DISPLAY="$DPY" xdotool key Right
sleep 0.2
if [ -e "$STORE" ]; then
    bad "staged theme is not written before Apply"
else
    ok "staged theme is not written before Apply"
fi

DISPLAY="$DPY" xdotool key F8
sleep 0.2
DISPLAY="$DPY" xdotool key Return
for _ in $(seq 1 30); do
    [ -e "$STORE" ] && break
    sleep 0.1
done
if [ -e "$STORE" ] && grep -q '^theme=light$' "$STORE"; then
    ok "Apply writes the staged theme to the BIOS store"
else
    bad "Apply writes the staged theme to the BIOS store"
fi
if [ -e "$SHELLCFG" ] && grep -q '^theme=light$' "$SHELLCFG"; then
    ok "Apply syncs the managed XENUS shell key"
else
    bad "Apply syncs the managed XENUS shell key"
fi

DISPLAY="$DPY" xdotool key Escape
sleep 0.3
if kill -0 "$BIOS_PID" 2>/dev/null; then
    bad "Escape exits after a clean Apply"
else
    ok "Escape exits after a clean Apply"
fi

log ""
log "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
