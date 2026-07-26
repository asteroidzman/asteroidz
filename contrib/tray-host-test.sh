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
# Also pins ADOPTION. A compositor restart re-execs, and every fd is CLOEXEC,
# so the D-Bus connection goes with it: the watcher name is released and
# reclaimed. Whether an application notices and re-registers is entirely up to
# that application -- Qt and libappindicator watch the name and come back,
# plenty register exactly once at startup and never again. Those were simply
# gone until they were restarted, which is what "the tray keeps losing clients"
# was. So an item already on the bus when the compositor starts must be picked
# up without the application doing anything.
#
# Runs against its OWN dbus-daemon and XDG_RUNTIME_DIR, so it never touches
# the live session's tray. Needs no tray application installed --
# contrib/snitem stands in for one.
#
# Usage: contrib/tray-host-test.sh [path-to-asteroidz]
set -u
BIN="${1:-${ASTEROIDZ:-build/asteroidz}}"
[ -x "$BIN" ] || { echo "tray-host-test: no binary at $BIN" >&2; exit 1; }
command -v dbus-daemon >/dev/null || { echo "needs dbus-daemon" >&2; exit 1; }
command -v busctl >/dev/null || { echo "needs busctl" >&2; exit 1; }

D=$(mktemp -d /tmp/asteroidz-trayhost-XXXXXX)
mkdir -p "$D/xdg"; chmod 700 "$D/xdg"
cleanup() { kill ${COMP:-} ${ITEMPID:-} ${BUSPID:-} 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT

cat > "$D/c.kdl" <<'KDL'
output HEADLESS-1 { width 1280; height 720; refresh 60 }
bar { enable true; height 40; modules-left "tags"; modules-right "tray" }
KDL

BUSOUT=$(dbus-daemon --session --print-address --print-pid --fork)
BUSADDR=$(echo "$BUSOUT" | sed -n 1p); BUSPID=$(echo "$BUSOUT" | sed -n 2p)

# An "application" already showing a tray icon, BEFORE the compositor exists.
SNITEM="$(dirname "$0")/snitem/snitem"
ITEMNAME=""
if [ -x "$SNITEM" ]; then
  DBUS_SESSION_BUS_ADDRESS="$BUSADDR" "$SNITEM" > "$D/item.name" 2>/dev/null &
  ITEMPID=$!
  for i in $(seq 1 20); do sleep 0.2; ITEMNAME=$(cat "$D/item.name" 2>/dev/null); [ -n "$ITEMNAME" ] && break; done
fi

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

# Adoption: the item that was already on the bus must be in the tray, without
# it ever having called RegisterStatusNotifierItem.
if [ -z "${ITEMNAME:-}" ]; then
  echo "skip - contrib/snitem not built (cd contrib/snitem && make)"
elif kill -0 $COMP 2>/dev/null; then
  ITEMS=$(busctl --user get-property org.kde.StatusNotifierWatcher \
            /StatusNotifierWatcher org.kde.StatusNotifierWatcher \
            RegisteredStatusNotifierItems 2>&1)
  case "$ITEMS" in
    *"$ITEMNAME"*)
      echo "ok - an item already on the bus is adopted without re-registering" ;;
    *)
      echo "FAIL: $ITEMNAME was not adopted; watcher reports: $ITEMS"; fail=1 ;;
  esac
fi

exit $fail
