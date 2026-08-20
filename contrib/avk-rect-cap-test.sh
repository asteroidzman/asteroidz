#!/usr/bin/env bash
# avk-rect-cap-test.sh -- the rectangle cap is a cost knob, not a quality one.
#
# A blur's rebuild region collapses to its bounding box past
# AZ_BLUR_DAMAGE_MAX_RECTS rectangles. Live, on a tag transition, that fires on
# 16% of blur chains and inflates their area 1.74x, because the bar declares a
# blur region per module and the region reaches 39 rectangles. It never fired on
# any headless fixture, which is how it went unnoticed through M4F entirely.
#
# ── THE CLAIM THIS TEST EXISTS TO CHECK ──────────────────────────────────
#
# Raising the cap CANNOT change a pixel. The collapse is conservative in one
# direction only: the bounding box always CONTAINS the region, so a collapsed
# chain rebuilds a superset of what it needed and produces the same result more
# expensively. If that is right, every cap setting renders identical output and
# the only difference is cost.
#
# If it is WRONG -- if the region were ever used as something other than "which
# pixels to rebuild" -- then the settings would differ and this test fails. That
# is the whole point of asserting equality rather than measuring speed: a
# speed-only test would happily report a win from a build that had started
# rendering the wrong thing.
#
# ── THE PREMISE ──────────────────────────────────────────────────────────
#
# A fixture that never trips the cap renders identically at every setting for a
# reason that has nothing to do with the argument above, and would pass. So the
# first assertion is that the default cap FALLS BACK here, and the second is
# that a raised cap stops it falling back. Without both, the equality result is
# about nothing.
#
# ── WHAT ACTUALLY FRAGMENTS THE REGION, MEASURED THE HARD WAY ────────────
#
# The first version of this fixture spawned twelve windows carrying a hundred
# and twenty CLIPPED multi-rect blur nodes, drove damage_all, and recorded
# rects_max = 1. Per-node clipping does not fragment the rebuild region at all.
#
# The chain is
#
#     rebuild <- result_region <- output_damage <- frame_damage INTERSECT dependency
#
# so the rectangle count is inherited from the FRAME DAMAGE. damage_all is one
# rectangle, therefore the intersection is one rectangle, therefore the cap is
# structurally unreachable -- which is also why the collapse never appeared in
# any earlier headless profiling, since all of it used damage_all.
#
# A tag transition damages old-union-new for every moving window, every frame,
# so the frame damage arrives in many scattered clusters. But that alone still
# gave rects_max = 1 with twelve animating windows: a SMALL blur node's
# dependency box only reaches its own cluster, so its intersection is one
# rectangle however fragmented the screen is.
#
# THE NODE HAS TO BE WIDE. Live, the fragmented node is the BAR --- it spans the
# whole display, so its dependency crosses every damage cluster at once and the
# intersection inherits all of them. That is where 39 rectangles comes from, and
# no headless fixture has ever had a screen-spanning blur node, which is the
# real reason this has never reproduced off the live machine.
#
# So the fixture adds one: a full-width, short window pinned at the top. The
# geometry is what matters, not the shell protocol.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-rect-cap"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-rectcap-$$"
mkdir -p "$OUTDIR"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

W="${W:-1920}"
H="${H:-1080}"
SCALE="${SCALE:-1.5}"
WINDOWS="${WINDOWS:-12}"
CYCLES="${CYCLES:-10}"

CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 16; blur_sigma 10; position_y 4 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.0; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

export WLBGEFFECT_SSD=1
STATS=""

capture() { # capture NAME [CAP]
	local name="$1" cap="${2:-}"
	local dir="$OUTDIR/$name" cdir="$OUTDIR/$name-cap"
	mkdir -p "$dir" "$cdir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	HL_ENV="AZ_SHADOW_DITHER_AMP=0 AZ_AVK_CAPTURE_DIR=$cdir"
	[ -n "$cap" ] && HL_ENV="$HL_ENV AZ_BLUR_DAMAGE_MAX_RECTS=$cap"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1
	# The bar stand-in, FIRST so it sits behind everything and stays on both
	# tags' screens: full width, short, at the top.
	hl_spawn_wlbgeffect "rcbar" 300 "rcbar" >/dev/null
	hl_wait_client_count 1 200
	hl_dispatch toggle_floating 0.2
	hl_dispatch "move_window,0,0" 0.2
	hl_dispatch "resize_window,$(( W * 2 / 3 )),90" 0.3
	local i=0
	while [ "$i" -lt "$WINDOWS" ]; do
		hl_spawn_wlbgeffect "rc$i" 300 "rc$i" >/dev/null
		hl_wait_client_count "$(( i + 2 ))" 200
		hl_dispatch toggle_floating 0.15
		hl_dispatch "move_window,$(( 20 + (i % 4) * 210 )),$(( 20 + (i / 4) * 200 ))" 0.15
		hl_dispatch resize_window,380,260 0.15
		# Half on each tag. A switch between a populated tag and an empty one
		# slides one population past nothing, and it is the two populations
		# moving at once that fragments the damage.
		[ $(( i % 2 )) -eq 1 ] && hl_dispatch "tag,2" 0.15
		i=$(( i + 1 ))
	done
	sleep 3
	hl_dispatch reset_avk_stats 0.5
	local c=0
	while [ "$c" -lt "$CYCLES" ]; do
		if [ $(( c % 2 )) -eq 0 ]; then hl_dispatch view,2; else hl_dispatch view,1; fi
		sleep 0.9
		c=$(( c + 1 ))
	done
	sleep 2
	STATS="$(hl_get "get avk-stats" | jq -r \
		'"\(.blur_damage_fallbacks) \(.blur_damage_rects_max) \(.blur_chains) \(.blur_fallback_area_before // 0) \(.blur_fallback_area_after // 0) \(.validation_errors) \(.cpu_sync_waits) \(.blur_nodes_clipped)"' 2>/dev/null)"
	# Settle to a known tag before capturing, or the two cohorts photograph
	# different desktops and the oracle compares tag 1 against tag 2.
	hl_dispatch view,1 1.5
	hl_dispatch capture_output 1
	sleep 1
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	hl_stop >/dev/null 2>&1
}
f() { echo "$2" | awk -v n="$1" '{print $n}'; }
diffpx() { python3 "$PPM" diff "$OUTDIR/$1.ppm" "$OUTDIR/$2.ppm" 2>/dev/null || echo "-1 -1"; }

echo "══ blur rectangle cap ══ ${W}x${H} scale $SCALE windows=$WINDOWS"
echo

echo "── 0. the premise: the DEFAULT cap actually falls back here ──────────"
capture cap20
S20="$STATS"
echo "  fallbacks=$(f 1 "$S20") rects_max=$(f 2 "$S20") chains=$(f 3 "$S20") clipped_nodes=$(f 8 "$S20")"
hl_assert_true "PREMISE: a capture was produced" \
	"$([ -s "$OUTDIR/cap20.ppm" ] && echo true || echo false)"
hl_assert_true "PREMISE: blur chains ran at all" \
	"$([ "$(f 3 "$S20")" -gt 0 ] && echo true || echo false)"
hl_assert_true "PREMISE: the default cap DOES collapse regions in this fixture" \
	"$([ "$(f 1 "$S20")" -gt 0 ] && echo true || echo false)"
hl_assert_true "PREMISE: and the region really exceeded 20 rectangles" \
	"$([ "$(f 2 "$S20")" -gt 20 ] && echo true || echo false)"

echo
echo "── 1. a raised cap stops the collapse ────────────────────────────────"
capture cap4096 4096
S40="$STATS"
echo "  fallbacks=$(f 1 "$S40") rects_max=$(f 2 "$S40") chains=$(f 3 "$S40")"
hl_assert "a cap of 4096 collapses nothing" "$(f 1 "$S40")" 0

echo
echo "── 2. THE ORACLE: every cap renders the same picture ─────────────────"
read -r D M <<<"$(diffpx cap20 cap4096)"
echo "  cap 20 vs cap 4096: $D px differ (worst channel $M)"
hl_assert "the rectangle cap changes NO pixel" "$D" 0

echo
echo "── 3. what the collapse costs, on this fixture ───────────────────────"
awk -v b="$(f 4 "$S20")" -v a="$(f 5 "$S20")" -v n="$(f 1 "$S20")" -v c="$(f 3 "$S20")" \
'BEGIN{
	if (n+0 == 0) { print "  no collapse -- nothing to price"; exit }
	printf "  %d of %d chains collapsed (%.1f%%)\n", n, c, 100*n/c;
	printf "  region %.2f Mpx -> bounding box %.2f Mpx   inflation %.2fx\n",
		b/1e6, a/1e6, (b>0 ? a/b : 0);
	printf "  avoidable fill: %.2f Mpx over %d collapses\n", (a-b)/1e6, n;
}'

echo
echo "── 4. neither setting is unsound ─────────────────────────────────────"
hl_assert "cap 20: validation errors"   "$(f 6 "$S20")" 0
hl_assert "cap 4096: validation errors" "$(f 6 "$S40")" 0
# NOT an absolute zero, and the reason is written down rather than tolerated.
# This fixture drives thirteen animating windows at roughly twelve blur chains
# per frame on a headless GPU that idles near 50MHz, so avk_cmd_ring_begin()
# blocks after running its three frames ahead of a GPU that cannot keep up.
# Measured: 104 stalls culled against 105 unculled, with the culled build
# waiting LESS (ring_wait p95 16.1ms vs 18.7ms). The live session reports zero.
# So the number here describes the fixture, and the meaningful assertion is that
# the two cap settings do not differ --- a cap change must not introduce stalls.
W20="$(f 7 "$S20")"; W40="$(f 7 "$S40")"
hl_assert_true "the cap does not add CPU stalls (cap20=$W20 cap4096=$W40)" \
	"$([ "$W40" -le $(( W20 + W20 / 4 + 8 )) ] && echo true || echo false)"

echo
echo "logs: $OUTDIR"
hl_summary
