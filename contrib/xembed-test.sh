#!/usr/bin/env bash
# Isolated nested XEmbed test, without dbus-run-session's subshell gymnastics:
# start our own bus, then launch everything against it in one shell.
set -u
REPO=/home/ralf/asteroidz
OUT=/tmp/claude-1000/-home-ralf-asteroidz/ff7146da-a00e-4563-baaf-bd3ccc6e9e3b/scratchpad/xe3
rm -rf "$OUT"; mkdir -p "$OUT/xdg"; chmod 700 "$OUT/xdg"

cat > "$OUT/config.kdl" <<'KDL'
output HEADLESS-1 { width 1280; height 720; refresh 60 }
theme { bg-color 0x2a6fd6ff; fg-color 0xffffffff }
bar { enable true; height 40; modules-left "tags"; modules-center "clock"; modules-right "tray" }
KDL

# our own session bus, so nothing here can reach the real tray
BUSOUT=$(dbus-daemon --session --print-address --print-pid --fork 2>/dev/null)
BUSADDR=$(echo "$BUSOUT" | sed -n 1p)
BUSPID=$(echo "$BUSOUT" | sed -n 2p)
echo "isolated bus: ${BUSADDR%%,*}  (pid $BUSPID)"

XBEFORE=$(ls /tmp/.X11-unix 2>/dev/null | tr '\n' ' ')

env -i HOME="$HOME" PATH="$PATH" XDG_RUNTIME_DIR="$OUT/xdg" \
  DBUS_SESSION_BUS_ADDRESS="$BUSADDR" \
  WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=gles2 \
  "$REPO/build/asteroidz" -c "$OUT/config.kdl" > "$OUT/comp.log" 2>&1 &
COMP=$!

SOCK=""
for i in $(seq 1 40); do
  sleep 0.5
  SOCK=$(ls "$OUT/xdg"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1)
  [ -n "$SOCK" ] && break
done
if [ -z "$SOCK" ]; then
  echo "FAIL: no wayland socket (compositor alive? $(kill -0 $COMP 2>/dev/null && echo yes || echo no))"
  echo "--- comp.log:"; cat "$OUT/comp.log"
  kill $COMP $BUSPID 2>/dev/null; exit 1
fi
export XDG_RUNTIME_DIR="$OUT/xdg" WAYLAND_DISPLAY=$(basename "$SOCK")
export DBUS_SESSION_BUS_ADDRESS="$BUSADDR"
echo "wayland: $WAYLAND_DISPLAY"

# XWayland's socket appears a few seconds after the wayland one, and
# /proc/PID/environ is NOT the place to look: it shows the environment at exec,
# so the compositor's own setenv("DISPLAY") never lands there. Poll for a new
# socket in /tmp/.X11-unix instead, and take the one that was not there before.
NEWX=""
for i in $(seq 1 24); do
  sleep 0.5
  XAFTER=$(ls /tmp/.X11-unix 2>/dev/null | tr '\n' ' ')
  for x in $XAFTER; do case " $XBEFORE " in *" $x "*) ;; *) NEWX="$x";; esac; done
  [ -n "$NEWX" ] && break
done
if [ -z "$NEWX" ]; then
  echo "FAIL: nested XWayland never created a socket"
  kill $COMP $BUSPID 2>/dev/null; exit 1
fi
export DISPLAY=":${NEWX#X}"
echo "xwayland DISPLAY=$DISPLAY"

"$REPO/build/asteroidz-xembed" > "$OUT/xembed.log" 2>&1 &
BRIDGE=$!
sleep 2
echo "--- bridge:"; cat "$OUT/xembed.log"

"$REPO/contrib/xtrayicon/xtrayicon" 00cc44 25 > "$OUT/icon.log" 2>&1 &
ICON=$!
sleep 4
echo "--- xtrayicon:"; cat "$OUT/icon.log"
echo "--- bridge after dock:"; cat "$OUT/xembed.log"
echo "--- items on the isolated bus:"
busctl --user get-property org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
  org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems 2>&1

grim -o HEADLESS-1 "$OUT/bar.png" 2>/dev/null && echo "shot: $OUT/bar.png"
kill $ICON $BRIDGE $COMP $BUSPID 2>/dev/null
