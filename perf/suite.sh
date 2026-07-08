#!/usr/bin/env bash
# Full asteroidz performance suite — run from inside a live asteroidz-perf session.
#   suite.sh            # baseline + synthetic load + Aska
#   suite.sh no-aska    # skip the game
set -u
HZ=$(getconf CLK_TCK)
ASKA_APPID=1898300
DO_ASKA=1; [[ "${1:-}" == "no-aska" ]] && DO_ASKA=0

# --- find the compositor + its Wayland socket (from a session child's env) ---
PID=$(pgrep -x asteroidz | head -1)
[[ -z "$PID" ]] && { echo "asteroidz not running — are you in the session?"; exit 1; }
for kid in $(pgrep -P "$PID") $(pgrep -x dms) $(pgrep -x kitty); do
  w=$(tr '\0' '\n' < /proc/$kid/environ 2>/dev/null | sed -n 's/^WAYLAND_DISPLAY=//p' | head -1)
  [[ -n "$w" ]] && { export WAYLAND_DISPLAY="$w"; break; }
done
: "${WAYLAND_DISPLAY:=wayland-1}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

gfx_ns(){ awk '/drm-client-id:/{c=$2} /drm-engine-gfx/{g[c]=$2} END{s=0;for(k in g)s+=g[k];print s+0}' /proc/$1/fdinfo/* 2>/dev/null; }
cpu_j(){ awk '{print $14+$15}' /proc/$1/stat 2>/dev/null; }
gpu_snap(){ timeout 3 amdgpu_top -J -n 1 2>/dev/null | python3 -c "
import sys,json
raw=sys.stdin.read()
try:d=json.loads(raw)
except Exception:
    import json as j;d=j.JSONDecoder().raw_decode(raw.lstrip())[0]
dev=d.get('devices',[d])[0] if 'devices' in d else d
a=dev.get('gpu_activity',{});v=dev.get('VRAM',{})
g=lambda x:(x.get('value') if isinstance(x,dict) else x)
print(f\"gpu_gfx={g(a.get('GFX',0))}% media={g(a.get('MediaEngine',0))}% vram={g(v.get('Total VRAM Usage',0))}MB\")
" 2>/dev/null; }

sample(){ # $1 label  $2 seconds
  local c0 g0 t0 c1 g1 t1 w
  c0=$(cpu_j $PID); g0=$(gfx_ns $PID); t0=$(date +%s.%N)
  sleep "$2"
  c1=$(cpu_j $PID); g1=$(gfx_ns $PID); t1=$(date +%s.%N)
  w=$(echo "$t1-$t0"|bc)
  printf "  %-16s comp_cpu=%s%%  comp_gfx=%s%%  | %s\n" "$1" \
    "$(echo "scale=1;($c1-$c0)/$HZ/$w*100"|bc)" \
    "$(echo "scale=1;($g1-$g0)/1000000000/$w*100"|bc)" \
    "$(gpu_snap)"
}

echo "=== session ==="
echo "  binary : $(readlink /proc/$PID/exe)"
echo "  flags  : $(grep -m1 -oE '\-O[0-9]|\-flto[^ ]*|\-DNDEBUG' /home/ralf/asteroidz/build/compile_commands.json 2>/dev/null|sort -u|tr '\n' ' ')"
echo "  glthread: $(tr '\0' '\n' </proc/$PID/environ 2>/dev/null|grep MESA_GLTHREAD || echo unset)"
echo "  socket : $WAYLAND_DISPLAY   collector: $(pgrep -f asteroidz-perf-collect >/dev/null && echo running || echo off)"
echo "  monitor: $(amsg 2>/dev/null | head -1)"

echo "=== 1) BASELINE (idle desktop, 10s) ==="
sample idle 10

echo "=== 2) SYNTHETIC LOAD (vkcube + 4 opaque + 2 translucent kitty, 15s) ==="
LOAD=(); vkcube --wsi wayland >/dev/null 2>&1 & LOAD+=($!)
for i in 1 2 3 4; do kitty --config NONE -e sh -c 'while :;do echo $RANDOM;done' >/dev/null 2>&1 & LOAD+=($!); done
for i in 1 2; do kitty --config NONE -o background_opacity=0.55 -e sh -c 'while :;do echo $RANDOM;done' >/dev/null 2>&1 & LOAD+=($!); done
sleep 3; sample "loaded" 15
for p in "${LOAD[@]}"; do kill $p 2>/dev/null; done; sleep 1

if [[ $DO_ASKA == 1 ]]; then
  echo "=== 3) ASKA (real game) ==="
  echo "  launching appid $ASKA_APPID via steam..."
  steam "steam://rungameid/$ASKA_APPID" >/dev/null 2>&1 &
  echo -n "  waiting for aska.exe window "
  for i in $(seq 1 90); do
    amsg 2>/dev/null | grep -qiE 'aska' && { echo " up (${i}s)"; break; }
    echo -n .; sleep 2
  done
  echo "  (sampling 3x15s while in-game — get past the menu for a real load)"
  sample "aska-1" 15; sample "aska-2" 15; sample "aska-3" 15
  echo "  per-process GPU split (compositor vs game):"
  timeout 3 amdgpu_top -J -n1 2>/dev/null | python3 -c "
import sys,json,os
raw=sys.stdin.read()
try:d=json.loads(raw)
except Exception:
    import json as j;d=j.JSONDecoder().raw_decode(raw.lstrip())[0]
dev=d.get('devices',[d])[0] if 'devices' in d else d
fd=dev.get('fdinfo',{})
for name,info in (fd.items() if isinstance(fd,dict) else []):
    print('   ',name,info)
" 2>/dev/null || echo "   (fdinfo unavailable)"
fi
echo "=== done — full time series in ~/asteroidz-perf-*.csv ==="
