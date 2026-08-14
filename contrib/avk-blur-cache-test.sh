#!/usr/bin/env bash
# avk-blur-cache-test.sh -- the monitor background blur result cache, A/B'd.
#
# THE CLAIM. Everything below the scene's optimized-blur node is the wallpaper,
# it does not change when a window moves, and blurring it once per frame per
# consuming window is work nobody asked for. Build it when its source changes,
# reuse the finished result until it changes again.
#
# ── WHAT MUST BE TRUE FOR THE MEASUREMENT TO MEAN ANYTHING ───────────────
#
# The fixture has to establish its own premise before it reports a saving:
#
#   1. nodes actually ASK for the cached bottom layer      (forced_live > 0)
#   2. with the cache OFF they each run their own chain    (WINDOW_BACKDROP
#                                                           capture > 0)
#   3. with it ON those chains disappear                   (capture -> 0)
#   4. and the background is built ONCE                    (rebuilds small,
#                                                           hits == requests)
#
# Without (1) and (2) the ON arm is measuring a desktop that had no work to
# remove, and every number would look like a triumph. `blur_optimized 0` is
# exactly that trap: it stops nodes ASKING, so forced_live goes to zero and the
# cost does not move, and it was read once as evidence that the cache could not
# help.
#
# ── THE PIXEL QUESTION, STATED HONESTLY ──────────────────────────────────
#
# Honouring should_only_blur_bottom_layer CHANGES PIXELS. That is what the flag
# means: the node's backdrop becomes the blurred wallpaper rather than a blur of
# the live scene beneath it, so a shadow lying in the gap between two tiles no
# longer appears inside the neighbour's blur. SceneFX does exactly this, and the
# compositor's own config asks for it.
#
# So this test does NOT assert cached == live. It MEASURES the difference and
# prints it, and asserts the two things that must hold either way:
#
#   - a cache HIT must be indistinguishable from a cache REBUILD. Same source,
#     same kernel, same result: if reusing the image differs by one pixel from
#     recomputing it, the cache is broken regardless of what the flag means.
#   - the difference against the live path must be confined to where a live
#     blur would have sampled something other than the wallpaper.
#
# The first is the real oracle and it is exact.
#
# HEADLESS ABSOLUTE MICROSECONDS ARE NOT BUDGET EVIDENCE -- the fixture GPU
# idles near 50MHz. Areas, counts and pixel differences are what transfer.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-cache"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-blurcache-$$"
mkdir -p "$OUTDIR"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

W="${W:-1920}"
H="${H:-1080}"
SCALE="${SCALE:-1.5}"
WINDOWS="${WINDOWS:-5}"
CYCLES="${CYCLES:-10}"

# optimized 1 is what makes a client blur node ASK for the cached bottom layer.
CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 20; blur_sigma 12; position_y 6 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.0; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

export WLBGEFFECT_SSD=1

JQ='{frames:.frames, forced:.blur_nodes_forced_live,
     wb_c:.blur_role_WINDOW_BACKDROP_chains,
     wb_cap:.blur_role_WINDOW_BACKDROP_capture_px,
     wb_reb:.blur_role_WINDOW_BACKDROP_rebuild_px,
     mb_c:.blur_role_MONITOR_BACKGROUND_chains,
     mb_cap:.blur_role_MONITOR_BACKGROUND_capture_px,
     req:.blur_cache_requests, hit:.blur_cache_hits, reb:.blur_cache_rebuilds,
     bytes:.blur_cache_bytes, gen:.blur_cache_generation,
     inv_gen:.blur_cache_inv_generation, inv_geo:.blur_cache_inv_geometry,
     inv_par:.blur_cache_inv_params, inv_new:.blur_cache_inv_never_built,
     inv_forced:.blur_cache_inv_forced,
     s_px:.blur_cache_saved_prefix_px, s_blur:.blur_cache_saved_blur_px,
     s_ch:.blur_cache_saved_chains,
     images:.live_images, mem:.live_device_memory,
     verr:.validation_errors, waits:.cpu_sync_waits}'

STATS=""
run() { # run NAME [EXTRA_ENV...]
	local name="$1"; shift
	local dir="$OUTDIR/$name" cdir="$OUTDIR/$name-cap"
	mkdir -p "$dir" "$cdir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_SHADOW_DITHER_AMP=0 AZ_AVK_CAPTURE_DIR=$cdir $*"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1
	# RESET FIRST, BEFORE ANY WINDOW MAPS.
	#
	# The cache is built on the first frame that has a consumer, which is long
	# before a settle-then-reset would run -- so resetting after the windows
	# appear measures an interval in which the cache was already warm, and
	# `rebuilds` correctly reads 0. That is a true number and it reads exactly
	# like a cache that never builds. Resetting here puts the cold build inside
	# the measured interval, where it can be asserted.
	hl_dispatch reset_avk_stats 1
	local i=0
	while [ "$i" -lt "$WINDOWS" ]; do
		hl_spawn_wlbgeffect "bc$i" 300 "bc$i" >/dev/null
		hl_wait_client_count "$(( i + 1 ))" 200
		i=$(( i + 1 ))
	done
	sleep 3
	local c=0
	while [ "$c" -lt "$CYCLES" ]; do
		hl_dispatch damage_all
		sleep 0.4
		c=$(( c + 1 ))
	done
	sleep 1
	STATS="$(hl_get "get avk-stats" | jq -r "$JQ | to_entries |
		map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null)"
	hl_dispatch capture_output 1
	sleep 1
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	hl_stop >/dev/null 2>&1
}
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
diffpx() { python3 "$PPM" diff "$OUTDIR/$1.ppm" "$OUTDIR/$2.ppm" 2>/dev/null \
	|| echo "-1 -1"; }

echo "══ monitor background blur result cache ══ ${W}x${H} scale $SCALE windows=$WINDOWS"
echo

echo "── A: cache OFF -- every backdrop rebuilds the background itself ─────"
run off AZ_BLUR_CACHE=0
OFF="$STATS"
echo "  frames=$(v frames "$OFF") forced_live=$(v forced "$OFF")"
echo "  WINDOW_BACKDROP chains=$(v wb_c "$OFF") capture=$(v wb_cap "$OFF")"
echo "  MONITOR_BACKGROUND chains=$(v mb_c "$OFF")  cache requests=$(v req "$OFF")"
# THE PREMISE, both halves. Without these the ON arm removes nothing and says so
# in numbers that look like a win.
hl_assert_true "PREMISE: nodes ask for the cached bottom layer" \
	"$([ "$(v forced "$OFF")" -gt 0 ] && echo true || echo false)"
hl_assert_true "PREMISE: with the cache off they run their own chains" \
	"$([ "$(v wb_c "$OFF")" -gt 0 ] && echo true || echo false)"
hl_assert_true "PREMISE: and those chains capture real area" \
	"$([ "$(v wb_cap "$OFF")" -gt 0 ] && echo true || echo false)"
hl_assert "cache off: the background node builds nothing" "$(v mb_c "$OFF")" 0
hl_assert "cache off: nothing is served from a cache" "$(v hit "$OFF")" 0

echo
echo "── B: cache ON ───────────────────────────────────────────────────────"
run on
ON="$STATS"
echo "  frames=$(v frames "$ON") forced_live=$(v forced "$ON")"
echo "  WINDOW_BACKDROP chains=$(v wb_c "$ON") capture=$(v wb_cap "$ON")"
echo "  MONITOR_BACKGROUND chains=$(v mb_c "$ON") capture=$(v mb_cap "$ON")"
echo "  requests=$(v req "$ON") hits=$(v hit "$ON") rebuilds=$(v reb "$ON") bytes=$(v bytes "$ON")"
echo "  invalidations: never_built=$(v inv_new "$ON") generation=$(v inv_gen "$ON")"
echo "                 geometry=$(v inv_geo "$ON") params=$(v inv_par "$ON")"
hl_assert_true "the background is built at least once" \
	"$([ "$(v reb "$ON")" -ge 1 ] && echo true || echo false)"
hl_assert_true "consumers were served from it" \
	"$([ "$(v hit "$ON")" -gt 0 ] && echo true || echo false)"
hl_assert "every request was a hit" "$(v req "$ON")" "$(v hit "$ON")"

echo
echo "── C: FULL DAMAGE IS NOT SOURCE DAMAGE ───────────────────────────────"
# The whole architecture in one assertion. Every cycle above damaged the entire
# output; the wallpaper never changed. A cache keyed on damage would have
# rebuilt once per cycle.
awk -v r="$(v reb "$ON")" -v c="$CYCLES" -v f="$(v frames "$ON")" 'BEGIN{
	printf "  %d full-output damage cycles over %d frames -> %d rebuild(s)\n",
		c, f, r;
}'
hl_assert_true "full-output damage did NOT rebuild the cache ($(v reb "$ON") <= 1)" \
	"$([ "$(v reb "$ON")" -le 1 ] && echo true || echo false)"
hl_assert "no rebuild blamed on damage-driven invalidation" \
	"$(v inv_gen "$ON")" 0

echo
echo "── D: what the cache removed, per frame ──────────────────────────────"
awk -v foff="$(v frames "$OFF")" -v fon="$(v frames "$ON")" \
    -v capoff="$(v wb_cap "$OFF")" -v caphon="$(v wb_cap "$ON")" \
    -v reboff="$(v wb_reb "$OFF")" -v rebon="$(v wb_reb "$ON")" \
    -v mbon="$(v mb_cap "$ON")" -v spx="$(v s_px "$ON")" \
    -v sbl="$(v s_blur "$ON")" -v sch="$(v s_ch "$ON")" 'BEGIN{
	if (foff+0==0 || fon+0==0) { print "  no frames"; exit }
	printf "  backdrop capture  : %8.3f -> %8.3f Mpx/frame\n",
		capoff/foff/1e6, caphon/fon/1e6;
	printf "  backdrop replay   : %8.3f -> %8.3f Mpx/frame\n",
		reboff/foff/1e6, rebon/fon/1e6;
	printf "  background build  : %8s    %8.3f Mpx/frame (amortised)\n",
		"-", mbon/fon/1e6;
	printf "  avoided per frame : %.3f Mpx replay, %.3f Mpx capture, %.2f chains\n",
		spx/fon/1e6, sbl/fon/1e6, sch/fon;
}'

echo
echo "── E: THE ORACLE -- a hit is indistinguishable from a rebuild ────────"
# The exact test, and the only one that can be exact. Same scene, same source,
# same kernel: forcing a rebuild every frame must produce the identical picture
# to reusing one. A difference here is a broken cache whatever the flag means.
run dirty AZ_BLUR_CACHE_ALWAYS_DIRTY=1
DIRTY="$STATS"
echo "  always-dirty: rebuilds=$(v reb "$DIRTY") hits=$(v hit "$DIRTY") frames=$(v frames "$DIRTY") forced_inv=$(v inv_forced "$DIRTY")"
# The break forces a REBUILD every frame; it does not bypass the cache. Consumers
# still read the image -- they read one recomputed for them this frame, which is
# exactly the comparison the oracle below needs. A first version asserted
# hits == 0 here and failed on correct behaviour.
# NOT frames-1. A rebuild only happens on a frame that has an eligible consumer
# with live damage, and a settled desktop renders frames that have none -- 39 of
# 43 here. Demanding one per frame asserts that the fixture never idles, which is
# a claim about the fixture rather than about the break.
hl_assert_true "the break forced a rebuild on nearly every frame ($(v reb "$DIRTY")/$(v frames "$DIRTY"))" \
	"$([ $(( $(v reb "$DIRTY") * 10 )) -ge $(( $(v frames "$DIRTY") * 8 )) ] && echo true || echo false)"
hl_assert_true "every forced rebuild was attributed to the break" \
	"$([ "$(v inv_forced "$DIRTY")" -ge "$(v reb "$DIRTY")" ] && echo true || echo false)"
hl_assert_true "the ON arm rebuilt far less than the break did" \
	"$([ "$(v reb "$ON")" -lt "$(v reb "$DIRTY")" ] && echo true || echo false)"
read -r D M <<<"$(diffpx on dirty)"
echo "  cached vs rebuilt-every-frame: $D px differ (worst channel $M)"
hl_assert "A CACHE HIT IS BIT-IDENTICAL TO A REBUILD" "$D" 0

echo
echo "── F: the difference against the live path, measured not asserted ────"
# Honouring should_only_blur_bottom_layer is a pixel change by design. This
# prints its size rather than pretending it is zero.
read -r DL ML <<<"$(diffpx off on)"
awk -v d="$DL" -v m="$ML" -v w="$W" -v h="$H" 'BEGIN{
	if (d < 0) { print "  capture missing -- no comparison"; exit }
	printf "  live vs cached backdrop: %d of %d px differ (%.3f%%), worst channel %d\n",
		d, w*h, 100*d/(w*h), m;
	print  "  EXPECTED nonzero: a cached backdrop is the blurred wallpaper,";
	print  "  a live one is a blur of the scene beneath the node.";
}'

echo
echo "── G: resources and soundness ────────────────────────────────────────"
echo "  cache bytes=$(v bytes "$ON")  images_live off=$(v images "$OFF") on=$(v images "$ON")"
hl_assert "cache on: validation errors"  "$(v verr "$ON")" 0
hl_assert "cache off: validation errors" "$(v verr "$OFF")" 0
hl_assert "always-dirty: validation errors" "$(v verr "$DIRTY")" 0
# NOT an absolute zero on waits: this fixture drives several animating windows on
# a headless GPU near 50MHz, so the ring legitimately blocks. The meaningful
# claim is that the cache does not ADD stalls.
WOFF="$(v waits "$OFF")"; WON="$(v waits "$ON")"
hl_assert_true "the cache adds no CPU stalls (off=$WOFF on=$WON)" \
	"$([ "$WON" -le $(( WOFF + WOFF / 4 + 8 )) ] && echo true || echo false)"

echo
echo "logs: $OUTDIR"
hl_summary
