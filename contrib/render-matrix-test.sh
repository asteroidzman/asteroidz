#!/usr/bin/env bash
# render-matrix-test.sh — headless verification across the render matrix:
# {vulkan, gles2} x {effects off, effects on} x {SDR single-pass, fake-HDR
# two-pass}. Effects OFF is the default (it exercises the no-blur/no-shadow/
# square-corner guards); EFFECTS=1 enables blur+shadows+rounding.
#
# Exercises: monocle titlebar strip, tile layout, overview, jump mode, and
# the screenshot UI, then checks the compositor log for validation errors.
#
# Usage: contrib/render-matrix-test.sh [BINARY]
#   BINARY  compositor to test (default: /usr/bin/asteroidz)
# Env: OUTDIR (default /tmp/asteroidz-noeffects), VALIDATE=0 to skip the
#      Vulkan validation layers, RENDERER=gles2|vulkan (default vulkan),
#      EFFECTS=1 to run with blur/shadows/rounding ENABLED instead.
set -u
BIN="${1:-/usr/bin/asteroidz}"
OUTDIR="${OUTDIR:-/tmp/asteroidz-render-matrix}"
VALIDATE="${VALIDATE:-1}"
RENDERER="${RENDERER:-vulkan}"
EFFECTS="${EFFECTS:-0}"
mkdir -p "$OUTDIR"
FAIL=0

run_case() { # $1=case name, $2=extra output KDL
	local tag="$1" out_kdl="$2"
	local xdg="$OUTDIR/xdg-$tag"
	rm -rf "$xdg"; mkdir -p "$xdg"; chmod 700 "$xdg"
	local fx_kdl="border_radius 0
shadows 0"
	if [ "$EFFECTS" = "1" ]; then
		fx_kdl="border_radius 9
shadows 1
shadows_size 20
shadows_blur 10
effects { blur { enable 1; optimized 1; passes 2; radius 6;
    params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2; } } }"
	fi
	cat > "$OUTDIR/$tag.kdl" <<CFG
$fx_kdl
borderpx 2
theme { bg-color 0xa6c8ffff; fg-color 0x00315fff; focus-bg-color 0xa6c8ffff; focus-fg-color 0x00315fff }
output HEADLESS-1 { width 1920; height 1080; refresh 60; $out_kdl }
layout { titlebar { enable 1; height 32 } }
tag 1 { layout monocle; name t1 }
CFG
	local envv=(env -i HOME="$HOME" PATH="$PATH" XDG_RUNTIME_DIR="$xdg"
		WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1
		WLR_RENDERER="$RENDERER")
	[ "$VALIDATE" = "1" ] && [ "$RENDERER" = "vulkan" ] &&
		envv+=(VK_LOADER_LAYERS_ENABLE='*validation*')
	"${envv[@]}" "$BIN" -d -c "$OUTDIR/$tag.kdl" >"$OUTDIR/$tag.log" 2>&1 &
	local comp=$!
	local sock=""
	for _ in $(seq 1 40); do sleep 0.2
		sock=$(ls "$xdg"/wayland-* 2>/dev/null | grep -v lock | head -1 | xargs -r basename)
		[ -n "$sock" ] && break
	done
	if [ -z "$sock" ]; then echo "[$tag] FAIL: no socket"; FAIL=1; return; fi
	export XDG_RUNTIME_DIR="$xdg" WAYLAND_DISPLAY="$sock"
	local sig; sig=$(ls "$xdg"/asteroidz-*.sock 2>/dev/null | head -1)
	D() { ASTEROIDZ_INSTANCE_SIGNATURE="$sig" amsg dispatch "$1" >/dev/null 2>&1; sleep 0.4; }

	local kpids=()
	for t in W1 W2 W3; do
		kitty --title "$t" sh -c "echo $t; sleep 900" >/dev/null 2>&1 &
		kpids+=($!); sleep 0.9
	done
	sleep 1
	grim "$OUTDIR/${tag}_monocle.png" 2>/dev/null
	# jump FIRST, from a settled desktop: dispatching toggle_overview,jump
	# while a previous overview is still animating closed lands in the
	# "already open" branch and opens a plain overview instead
	D "toggle_overview,jump"; sleep 1.5
	grim "$OUTDIR/${tag}_jump.png" 2>/dev/null
	D "toggle_overview"; sleep 1.5
	D "set_layout,tile"; sleep 1
	grim "$OUTDIR/${tag}_tile.png" 2>/dev/null
	D "toggle_overview"; sleep 1.5
	grim "$OUTDIR/${tag}_overview.png" 2>/dev/null
	D "toggle_overview"; sleep 1.5
	D "screenshot_ui,screen"; sleep 1.5

	kill "${kpids[@]}" 2>/dev/null; sleep 0.3
	kill "$comp" 2>/dev/null
	wait "$comp" 2>/dev/null
	local rc=$?

	# the compositor redirects stderr to its persistent log; scan both
	local statelog="$HOME/.local/state/asteroidz/asteroidz.log"
	local vuids errors
	vuids=$(grep -c "VUID" "$OUTDIR/$tag.log" "$statelog" 2>/dev/null | awk -F: '{s+=$2} END{print s+0}')
	# "cannot apply output color transforms" is GLES's expected HDR refusal
	errors=$(grep -E "\[ERROR\]" "$OUTDIR/$tag.log" 2>/dev/null | grep -cvE "experimental|global shortcuts|does not support HDR|cannot apply output color transforms")
	local shots=0
	for s in monocle tile overview jump; do
		[ -s "$OUTDIR/${tag}_${s}.png" ] && shots=$((shots+1))
	done
	echo "[$tag] exit=$rc shots=$shots/4 vuids=$vuids errors=$errors"
	{ [ "$rc" -ne 0 ] && [ "$rc" -ne 143 ]; } || [ "$shots" -ne 4 ] || [ "$vuids" != "0" ] && FAIL=1
}

# truncate the shared persistent log so VUID counts are this run's only
: > "$HOME/.local/state/asteroidz/asteroidz.log" 2>/dev/null || true

SUF=""; [ "$EFFECTS" = "1" ] && SUF="-fx"
run_case "${RENDERER}${SUF}-sdr" ""       # single-pass path (no blend buffer)
run_case "${RENDERER}${SUF}-hdr" "hdr 1"  # two-pass on vulkan; refusal on gles

if [ "$FAIL" = "0" ]; then echo "PASS: render-matrix harness clean"; else echo "FAIL"; fi
exit "$FAIL"
