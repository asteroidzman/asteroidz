#!/usr/bin/env bash
# tray-host-test.sh — the tray host must survive a client reading its
# properties.
#
# Pins a crash that took the whole compositor down: the watcher vtable
# declared IsStatusNotifierHostRegistered and ProtocolVersion with a NULL
# getter, which does not mean "no value" to sd-bus -- it means "read it from
# userdata + offset", and the vtable is exported with a NULL userdata.
#
# It hid for a long time because reading those properties is optional.
# quickshell and Steam only call RegisterStatusNotifierItem, so a normal
# session looked fine; every libappindicator client (nm-applet,
# blueman-applet) asks whether a host exists first, and took the session with
# it. A single-property read happens to survive -- it is org.freedesktop.
# DBus.Properties.GetAll, which walks every property, that faults. That is why
# this test calls GetAll and not get-property.
#
# Runs against its OWN dbus-daemon and XDG_RUNTIME_DIR, so it never touches
# the live session's tray. Needs no tray application installed.
#
# Usage: contrib/tray-host-test.sh [path-to-asteroidz]
set -u
BIN="${1:-${ASTEROIDZ:-build/asteroidz}}"
[ -x "$BIN" ] || { echo "tray-host-test: no binary at $BIN" >&2; exit 1; }
command -v dbus-daemon >/dev/null || { echo "needs dbus-daemon" >&2; exit 1; }
command -v busctl >/dev/null || { echo "needs busctl" >&2; exit 1; }

D=$(mktemp -d /tmp/asteroidz-trayhost-XXXXXX)
mkdir -p "$D/xdg"; chmod 700 "$D/xdg"
cleanup() { kill ${COMP:-} ${BUSPID:-} 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT

cat > "$D/c.kdl" <<'KDL'
output HEADLESS-1 { width 1280; height 720; refresh 60 }
bar { enable true; height 40; modules-left "tags"; modules-right "tray" }
KDL

BUSOUT=$(dbus-daemon --session --print-address --print-pid --fork)
BUSADDR=$(echo "$BUSOUT" | sed -n 1p); BUSPID=$(echo "$BUSOUT" | sed -n 2p)

env -i HOME="$HOME" PATH="$PATH" XDG_RUNTIME_DIR="$D/xdg" \
  DBUS_SESSION_BUS_ADDRESS="$BUSADDR" \
  WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=gles2 \
  "$BIN" -c "$D/c.kdl" > "$D/comp.log" 2>&1 &
COMP=$!

for i in $(seq 1 60); do
  sleep 0.5
  SOCK=$(ls "$D/xdg"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1)
  [ -n "${SOCK:-}" ] && break
done
[ -z "${SOCK:-}" ] && { echo "FAIL: compositor never started"; cat "$D/comp.log"; exit 1; }

export DBUS_SESSION_BUS_ADDRESS="$BUSADDR"
OUT=$(busctl --user call org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
        org.freedesktop.DBus.Properties GetAll s org.kde.StatusNotifierWatcher 2>&1)
sleep 1

fail=0
if ! kill -0 $COMP 2>/dev/null; then
  echo "FAIL: compositor died while a client read its watcher properties"
  fail=1
else
  echo "ok - compositor survives GetAll on the watcher interface"
fi
case "$OUT" in
  *IsStatusNotifierHostRegistered*ProtocolVersion*)
    echo "ok - GetAll returns all three properties" ;;
  *)
    echo "FAIL: GetAll did not return the expected properties: $OUT"; fail=1 ;;
esac

# the same over the vendor-neutral name, which shares the vtable
if kill -0 $COMP 2>/dev/null; then
  busctl --user call org.freedesktop.StatusNotifierWatcher /StatusNotifierWatcher \
    org.freedesktop.DBus.Properties GetAll s org.freedesktop.StatusNotifierWatcher \
    >/dev/null 2>&1
  sleep 1
  if kill -0 $COMP 2>/dev/null; then
    echo "ok - and over org.freedesktop.StatusNotifierWatcher"
  else
    echo "FAIL: compositor died on the freedesktop-named watcher"; fail=1
  fi
fi

exit $fail
