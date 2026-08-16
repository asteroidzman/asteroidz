#!/usr/bin/env bash
# avk-clip-policy-test.sh — M4B.2: one clip policy, checked in pixels.
#
# WHAT IS BEING PINNED
#
# M4B.1 unified the clip policy for decorations, but resize() keeps a `grabc`
# fast path (client.h) that recomputes decorations immediately AND sets the
# surface clip with client_get_clip() -- the plain window box, with no monitor
# crop. For a FLOATING drag that agrees with the new policy, because a
# move-drag floats the window and floating clients are not cropped either way.
#
# For a SCROLL-TILED client under a RESIZE drag it might not: begin_move_or_
# resize() only floats for CurMove, so a resize-drag leaves the client
# scroll-tiled, and then
#
#     surface      client_get_clip()            -> NOT monitor-cropped
#     decoration   client_clips_to_monitor()    -> cropped (scroll-tiled)
#
# which is the same shape of divergence that produced the cross-output bug.
# This test asks whether it is OBSERVABLE rather than assuming it is: the
# answer decides whether M4B.2 is a fix or a documented no-op.
#
#   CASE=resizegrab   scroll-tiled client, held resize drag, two outputs
#   CASE=overflow     EXTREME scroller overflow -- the reason the crop exists
#
# Both run by default.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-clip-policy"
BREAK="${BREAK:-}"
CASES="${CASES:-overflow resizegrab}"

REPAINT="$(dirname "$0")/wlrepaint/wlrepaint"
[ -x "$REPAINT" ] || { echo "not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

export HL_OUTPUTS=2
W1="${HL_WIDTH:-1920}"
FOCUS=0xc66b25ff

start() {
	OUTDIR="${TMPDIR:-/tmp}/asteroidz-clip-$1-$$"
	HL_OUTDIR="$OUTDIR"
	HL_ENV="ASTEROIDZ_RENDERER=avk"
	[ "$BREAK" = border-owner-monitor-clip ] && \
		HL_ENV="$HL_ENV AZ_BORDER_OWNER_MONITOR_CLIP=1"
	export HL_OUTDIR HL_ENV
	hl_start "border_radius 12
borderpx 6
focuscolor $FOCUS
bordercolor 0x2f6fd0ff
shadows 0
layer_shadows 0
gappih 0
gappiv 0
gappoh 0
gappov 0
animations 0
smartgaps 0
effects { blur { enable 0 } }
layout {
    titlebar { enable 0 }
}"
	sleep 2
}

# Count client fill and border pixels on an output, so "did anything of this
# window reach the neighbour" is one number per material.
probe() { # probe PNG -> "<border> <client>"
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load(); W, H = im.size
B = (0xc6, 0x6b, 0x25); C = (0x20, 0x20, 0x20)
def d(a, b): return sum((a[i]-b[i])**2 for i in range(3)) ** 0.5
b = sum(1 for y in range(0, H, 3) for x in range(0, W, 3) if d(px[x, y], B) < 40)
c = sum(1 for y in range(0, H, 3) for x in range(0, W, 3) if d(px[x, y], C) < 40)
print(b, c)
PY
}

# ── CASE overflow: the reason monitor cropping exists, taken to an extreme ───
case_overflow() {
	echo
	echo "--- EXTREME scroller overflow: nothing may reach the neighbour ---"
	start overflow
	hl_dispatch "view,4" 0.5
	hl_dispatch "set_layout,scroller" 1
	# Many wide columns: the scroller pushes the ones that do not fit far past
	# the monitor edge, which is exactly the state de0b5c5 was written for.
	for i in 1 2 3 4 5 6 7 8; do
		"$REPAINT" --title "wlov$i" --size 1200x800 --solid 202020 --frames 60 \
			--ssd --hold-ms 100 > "$OUTDIR/ov$i.log" 2>&1 &
		HL_SPAWNED_PIDS+=("$!")
		sleep 0.5
	done
	hl_wait_client_count 8 60 >/dev/null 2>&1
	sleep 3
	hl_get "get all-clients" | jq -r '[.clients[]|select(.title|startswith("wlov"))|{t:.title,x:.x,w:.width,m:.monitor}]|.[]|"  \(.t) x=\(.x) w=\(.w) mon=\(.m)"' | head -8
	# How far past its own monitor does the worst column reach?
	OWNER=$(hl_get "get all-clients" | jq -r '.clients[]|select(.title=="wlov1")|.monitor')
	MONX=$(hl_get "get all-monitors" | jq -r --arg m "$OWNER" '.monitors[]|select(.name==$m)|.x')
	MONW=$(hl_get "get all-monitors" | jq -r --arg m "$OWNER" '.monitors[]|select(.name==$m)|.width')
	# The overflow is how far the worst column lies OUTSIDE its owning monitor,
	# in whichever direction the scroller pushed it. Measuring only the
	# rightmost edge missed it entirely: these columns overflow LEFTWARDS.
	OVER=$(hl_get "get all-clients" | jq -r --argjson mx "$MONX" --argjson mw "$MONW" \
		'[.clients[]|select(.title|startswith("wlov"))|[($mx - .x), (.x + .width - ($mx + $mw))]|max]|max')
	echo "  owner: $OWNER spans $MONX..$((MONX + MONW)); worst column lies ${OVER}px outside it"
	grim -o HEADLESS-1 "$OUTDIR/o1.png" 2>/dev/null
	grim -o HEADLESS-2 "$OUTDIR/o2.png" 2>/dev/null
	set -- $(probe "$OUTDIR/o1.png"); B1=$1; C1=$2
	set -- $(probe "$OUTDIR/o2.png"); B2=$1; C2=$2
	echo "  HEADLESS-1: border=$B1 client=$C1"
	echo "  HEADLESS-2: border=$B2 client=$C2"
	if [ "$OWNER" = "HEADLESS-1" ]; then own_b=$B1; own_c=$C1; oth_b=$B2; oth_c=$C2
	else own_b=$B2; own_c=$C2; oth_b=$B1; oth_c=$C1; fi
	# PREMISE: the overflow has to be big enough that cropping is load-bearing.
	hl_assert "the scroller really overflows its output (premise)" \
		"$([ "${OVER:-0}" -gt 400 ] && echo true || echo false)" "true"
	hl_assert "the owning output actually shows the row (premise)" \
		"$([ "${own_c:-0}" -gt 1000 ] && echo true || echo false)" "true"
	hl_assert "no client bleeds onto the neighbouring output" \
		"$([ "${oth_c:-0}" -lt 300 ] && echo true || echo false)" "true"
	hl_assert "no border bleeds onto the neighbouring output" \
		"$([ "${oth_b:-0}" -lt 30 ] && echo true || echo false)" "true"
	hl_stop
}

# ── CASE resizegrab: scroll-tiled + held RESIZE drag ────────────────────────
case_resizegrab() {
	echo
	echo "--- scroll-tiled client under a HELD resize drag ---"
	start resizegrab
	hl_dispatch "view,4" 0.5
	hl_dispatch "set_layout,scroller" 1
	for i in 1 2 3; do
		"$REPAINT" --title "wlrg$i" --size 1100x760 --solid 202020 --frames 400 \
			--ssd --hold-ms 100 > "$OUTDIR/rg$i.log" 2>&1 &
		HL_SPAWNED_PIDS+=("$!")
		sleep 0.6
	done
	hl_wait_client_count 3 60 >/dev/null 2>&1
	sleep 3
	hl_sync_pointer_extent 2>/dev/null || true
	FOC=$(hl_get "get focused-client" | jq -r '"\(.x) \(.y) \(.width) \(.height)"')
	set -- $FOC; FX=$1; FY=$2; FW=$3; FH=$4
	echo "  focused client at $FX,$FY ${FW}x${FH} (scroll-tiled)"
	# Grab near its right edge and drag further right, WITHOUT releasing, so the
	# resize pushes geometry toward/past the monitor edge while grabc is set.
	W_BEFORE=$(hl_get "get focused-client" | jq -r .width)
	X_BEFORE=$(hl_get "get focused-client" | jq -r .x)
	GX=$((FX + FW - 30)); GY=$((FY + FH / 2))
	hl_super_rdrag_hold "$GX" "$GY" "$((GX + 700))" "$GY" 3000 &
	RPID=$!
	sleep 1.8
	hl_get "get all-clients" | jq -r '.clients[]|select(.title|startswith("wlrg"))|"  held: \(.title) x=\(.x) w=\(.width) mon=\(.monitor)"'
	grim -o HEADLESS-1 "$OUTDIR/h1.png" 2>/dev/null
	grim -o HEADLESS-2 "$OUTDIR/h2.png" 2>/dev/null
	wait $RPID 2>/dev/null || true
	sleep 1.5
	# Read the geometry AFTER the release, not during: `get focused-client`
	# returns null while a grab is in progress, so a mid-drag read cannot tell
	# "unchanged" from "unavailable".
	W_AFTER=$(hl_get "get focused-client" | jq -r .width)
	X_AFTER=$(hl_get "get focused-client" | jq -r .x)
	set -- $(probe "$OUTDIR/h1.png"); HB1=$1; HC1=$2
	set -- $(probe "$OUTDIR/h2.png"); HB2=$1; HC2=$2
	echo "  during the held resize -- HEADLESS-1: border=$HB1 client=$HC1"
	echo "  during the held resize -- HEADLESS-2: border=$HB2 client=$HC2"
	OWN=$(hl_get "get all-clients" | jq -r '.clients[]|select(.title=="wlrg1")|.monitor')
	if [ "$OWN" = "HEADLESS-1" ]; then oth_b=$HB2; oth_c=$HC2
	else oth_b=$HB1; oth_c=$HC1; fi
	# PREMISE, and it is the whole test. "A client is visible" is true of a
	# scene where the drag never started, and the first version of this asserted
	# exactly that -- reporting agreement between two policies neither of which
	# had been exercised. The geometry has to have MOVED.
	echo "  geometry: before ${X_BEFORE},w=${W_BEFORE}  after release ${X_AFTER},w=${W_AFTER}"
	if [ "${W_AFTER:-0}" = "${W_BEFORE:-0}" ] && [ "${X_AFTER:-0}" = "${X_BEFORE:-0}" ]; then
		# NOT a failure, and not coverage either. begin_move_or_resize() only
		# prepares a resize corner when the client is FLOATING; for a
		# scroll-tiled one CurResize just sets a cursor. So the grab sets grabc
		# and moves nothing, resize()'s grabc fast path is never driven into an
		# overflowing state, and surface and decoration cannot visibly diverge.
		# Verified by contrast: the same held rdrag resizes a FLOATING window
		# (700x500 -> 950x450), so the harness gesture works.
		hl_skip "resize-grab on a scroll-tiled client changes no geometry by design -- state not reachable, no coverage claimed"
		hl_stop
		return 0
	fi
	# THE QUESTION. If the surface leaks to the neighbour while the border does
	# not, the two policies disagree under a scroll-tiled grab.
	echo "  neighbour during grab: border=$oth_b client=$oth_c"
	hl_assert "surface and decoration agree on the neighbour during a grab" \
		"$([ "${oth_c:-0}" -lt 300 ] && [ "${oth_b:-0}" -lt 30 ] && echo true || echo false)" "true"
	hl_stop
}

for c in $CASES; do
	case "$c" in
		overflow) case_overflow ;;
		resizegrab) case_resizegrab ;;
		*) echo "unknown case $c" >&2; exit 1 ;;
	esac
done
hl_summary
