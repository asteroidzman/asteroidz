#!/usr/bin/env bash
# Isolated headless benchmark for asteroidz.
# Usage: bench.sh <asteroidz-binary> <config> <label> [duration_s]
# Launches a headless compositor (isolated HOME, noop seat), spawns synthetic
# load, and samples the compositor's own CPU% and GPU-busy% (DRM fdinfo).
set -u
BIN="$1"; CONF="$2"; LABEL="$3"; DUR="${4:-10}"
RT="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
BHOME="/tmp/benchhome-$LABEL"; mkdir -p "$BHOME"
HZ=$(getconf CLK_TCK)
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 LIBSEAT_BACKEND=noop \
       WLR_RENDERER=gles2 XDG_RUNTIME_DIR="$RT"
unset WAYLAND_DISPLAY

DISP="/tmp/bench-$LABEL.disp"; rm -f "$DISP"
HOME="$BHOME" "$BIN" -c "$CONF" -s "echo \$WAYLAND_DISPLAY > $DISP" -d >/dev/null 2>&1 &
COMP=$!
SOCK=""
for _ in $(seq 1 60); do
  sleep 0.1
  kill -0 "$COMP" 2>/dev/null || break
  [[ -s "$DISP" ]] && { SOCK="$(cat "$DISP")"; break; }
done
if [[ -z "$SOCK" ]] || ! kill -0 "$COMP" 2>/dev/null; then
  echo "[$LABEL] FAILED to start (see $BHOME/.local/state/asteroidz/*.log)"
  tail -15 "$BHOME"/.local/state/asteroidz/*.log 2>/dev/null; kill "$COMP" 2>/dev/null; exit 1
fi
export WAYLAND_DISPLAY="$SOCK"
RES=$(grep -oiE '[0-9]{3,}x[0-9]{3,}' "$BHOME"/.local/state/asteroidz/*.log 2>/dev/null | head -1)

# fds of the same DRM context report the SAME cumulative gfx ns, so take the
# max (not the sum) to avoid multiple-counting the one gfx engine
gpu_ns() { awk '/drm-engine-gfx/{if($2>m)m=$2} END{print m+0}' /proc/$COMP/fdinfo/* 2>/dev/null; }
cpu_j()  { awk '{print $14+$15}' /proc/$COMP/stat 2>/dev/null; }

# --- synthetic load into the headless compositor ---
LOAD=()
vkcube --wsi wayland >/dev/null 2>&1 & LOAD+=($!)   # continuous animation = steady damage
for i in 1 2 3; do
  HOME="$BHOME" kitty --config NONE -o font_size=14 \
    -e sh -c 'while :; do printf "%s %s %s\n" $RANDOM $RANDOM $RANDOM; done' >/dev/null 2>&1 &
  LOAD+=($!)
done
sleep 2   # let clients map
clients=$(amsg 2>/dev/null | grep -ciE 'title|appid|client' || echo '?')

# nudge to trigger animations/blur recompute during the sample
( for _ in $(seq 1 "$DUR"); do
    amsg dispatch focusstack 1 >/dev/null 2>&1; sleep 1
  done ) & NUDGE=$!

# --- sample ---
c0=$(cpu_j); g0=$(gpu_ns); t0=$(date +%s.%N)
sleep "$DUR"
c1=$(cpu_j); g1=$(gpu_ns); t1=$(date +%s.%N)
wall=$(echo "$t1 - $t0" | bc)
cpu=$(echo "scale=2; ($c1-$c0)/$HZ/$wall*100" | bc)
gpu=$(echo "scale=1; ($g1-$g0)/1000000000/$wall*100" | bc)
rss=$(awk '/VmRSS/{printf "%.0f",$2/1024}' /proc/$COMP/status 2>/dev/null)

printf "[%-12s] cpu=%5s%%  gpu=%5s%%  rss=%sMB  res=%s  load=%s clients (%.1fs)\n" \
  "$LABEL" "$cpu" "$gpu" "$rss" "${RES:-?}" "${#LOAD[@]}" "$wall"

kill "$NUDGE" 2>/dev/null
for p in "${LOAD[@]}"; do kill "$p" 2>/dev/null; done
kill "$COMP" 2>/dev/null; wait "$COMP" 2>/dev/null
rm -rf "$BHOME"
