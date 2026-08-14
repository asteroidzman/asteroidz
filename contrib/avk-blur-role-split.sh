#!/usr/bin/env bash
# avk-blur-role-split.sh -- who actually pays for the background blur.
#
# The accepted root cause of the transition tail was "a monitor-sized optimized
# blur node is recomputed live every frame". Read from the source, that cannot
# be what happens in AVK: az_avk.h skips WLR_SCENE_NODE_OPTIMIZED_BLUR outright,
# so the monitor node costs nothing at all. Reading the source is not evidence
# about a running renderer, though, so this measures it.
#
# ── THE TWO CLAIMS, SEPARATED ────────────────────────────────────────────
#
# 1. The monitor node runs no chain.       -> MONITOR_BACKGROUND chains == 0
# 2. The work went somewhere else.         -> WINDOW_BACKDROP chains > 0 and
#                                             carry most of the capture area
#
# A blur node whose producer set should_only_blur_bottom_layer has DECLARED
# that its source is the monitor background. AVK ignores the declaration and
# gives each one a private prefix replay and a private dual-Kawase chain, so a
# desktop with n such nodes recomputes the same background blur n times a frame.
# That is the redundancy, and its size is what this prints.
#
# ── WHY THE FIXTURE VARIES THE WINDOW COUNT ──────────────────────────────
#
# If the cost is one shared background being recomputed per consumer, then
# capture area per FRAME grows with the number of consumers while the visible
# area does not. A fixture at a single window count cannot tell that apart from
# "more windows cost more", which is true of every renderer ever written. Two
# counts, same output, same wallpaper: the per-frame capture area is the answer.
#
# HEADLESS ABSOLUTE MICROSECONDS ARE NOT BUDGET EVIDENCE -- the fixture GPU
# idles near 50MHz. Areas and ratios are what transfer.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-role-split"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-roles-$$"
mkdir -p "$OUTDIR"

W="${W:-3840}"
H="${H:-2160}"
SCALE="${SCALE:-1.5}"
CYCLES="${CYCLES:-10}"

# optimized 1 is what makes a client blur node ASK for the cached bottom layer.
# With optimized 0 nothing asks, every role is LIVE, and the fixture measures
# nothing -- which is exactly why `blur_optimized 0` moved forced_live to zero
# and moved no frame time: it stops the request, not the work.
CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 20; blur_sigma 12; position_y 6 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

export WLBGEFFECT_SSD=1

ROLES="WINDOW_BACKDROP LIVE MONITOR_BACKGROUND"
JQ='{frames:.frames, chains:.blur_chains, emitted:.blur_nodes_emitted,
     forced:.blur_nodes_forced_live, cap:.blur_capture_pixels,
     rebuild:.blur_prefix_rebuild_pixels,
     wb_c:.blur_role_WINDOW_BACKDROP_chains,
     wb_cap:.blur_role_WINDOW_BACKDROP_capture_px,
     wb_reb:.blur_role_WINDOW_BACKDROP_rebuild_px,
     wb_cmd:.blur_role_WINDOW_BACKDROP_prefix_cmds,
     lv_c:.blur_role_LIVE_chains,
     lv_cap:.blur_role_LIVE_capture_px,
     lv_reb:.blur_role_LIVE_rebuild_px,
     mb_c:.blur_role_MONITOR_BACKGROUND_chains,
     mb_cap:.blur_role_MONITOR_BACKGROUND_capture_px,
     ch_req:.blur_cache_requests, ch_hit:.blur_cache_hits,
     ch_reb:.blur_cache_rebuilds, ch_bytes:.blur_cache_bytes,
     ch_gen:.blur_cache_generation,
     ch_spx:.blur_cache_saved_prefix_px, ch_sbl:.blur_cache_saved_blur_px,
     ch_sch:.blur_cache_saved_chains,
     verr:.validation_errors, waits:.cpu_sync_waits}'

STATS=""
cohort() { # cohort NWINDOWS
	local n="$1"
	local dir="$OUTDIR/w$n"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	HL_ENV="ASTEROIDZ_RENDERER=avk"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1
	local i=0
	while [ "$i" -lt "$n" ]; do
		hl_spawn_wlbgeffect "rs$i" 300 "rs$i" >/dev/null
		hl_wait_client_count "$(( i + 1 ))" 200
		i=$(( i + 1 ))
	done
	sleep 3
	hl_dispatch reset_avk_stats 1
	# damage_all, not a transition: the question is about a STATIC desktop, and
	# a static desktop is where a background blur has the least excuse to be
	# recomputed. A transition would let "things moved" explain the cost.
	local c=0
	while [ "$c" -lt "$CYCLES" ]; do
		hl_dispatch damage_all
		sleep 0.4
		c=$(( c + 1 ))
	done
	sleep 1
	STATS="$(hl_get "get avk-stats" | jq -r "$JQ | to_entries |
		map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null)"
	hl_stop >/dev/null 2>&1
}
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

echo "══ blur chains by role ══ ${W}x${H} scale $SCALE  (output = 8.29 Mpx)"
echo

cohort 2; S2="$STATS"
cohort 6; S6="$STATS"

report() { # report LABEL STATS
	local L="$1" S="$2" f; f="$(v frames "$S")"
	echo "── $L ── frames=$f"
	awk -v f="$f" -v wc="$(v wb_c "$S")" -v wcap="$(v wb_cap "$S")" \
	    -v wreb="$(v wb_reb "$S")" -v wcmd="$(v wb_cmd "$S")" \
	    -v lc="$(v lv_c "$S")" -v lcap="$(v lv_cap "$S")" -v lreb="$(v lv_reb "$S")" \
	    -v mc="$(v mb_c "$S")" -v mcap="$(v mb_cap "$S")" 'BEGIN{
		if (f+0 == 0) { print "  NO FRAMES -- this fixture measured nothing"; exit }
		printf "  %-20s %8s %12s %12s %10s\n",
			"role", "chains/fr", "capture Mpx/fr", "rebuild Mpx/fr", "cmds/chain";
		printf "  %-20s %8.2f %12.3f %12.3f %10.2f\n", "WINDOW_BACKDROP",
			wc/f, wcap/f/1e6, wreb/f/1e6, (wc>0 ? wcmd/wc : 0);
		printf "  %-20s %8.2f %12.3f %12.3f %10s\n", "LIVE",
			lc/f, lcap/f/1e6, lreb/f/1e6, "-";
		printf "  %-20s %8.2f %12.3f %12s %10s\n", "MONITOR_BACKGROUND",
			mc/f, mcap/f/1e6, "-", "-";
	}'
	echo
}
report "2 windows" "$S2"
report "6 windows" "$S6"

echo "── the two claims ────────────────────────────────────────────────────"
hl_assert "CLAIM 1: the monitor node runs no chain (2 windows)" "$(v mb_c "$S2")" 0
hl_assert "CLAIM 1: the monitor node runs no chain (6 windows)" "$(v mb_c "$S6")" 0
hl_assert_true "CLAIM 2: window backdrops DO run chains" \
	"$([ "$(v wb_c "$S6")" -gt 0 ] && echo true || echo false)"
# PREMISE. If nothing asked for the cached bottom layer the split is vacuous and
# every assertion above passes by describing an empty set.
hl_assert_true "PREMISE: nodes actually asked for the cached bottom layer" \
	"$([ "$(v forced "$S6")" -gt 0 ] && echo true || echo false)"

echo
echo "── does the SAME background get recomputed once per consumer? ────────"
# Same output, same wallpaper, same visible area per frame. If the capture area
# per frame tracks the window count, each consumer is paying for its own copy.
awk -v a="$(v wb_cap "$S2")" -v fa="$(v frames "$S2")" \
    -v b="$(v wb_cap "$S6")" -v fb="$(v frames "$S6")" \
    -v ca="$(v wb_c "$S2")" -v cb="$(v wb_c "$S6")" 'BEGIN{
	if (fa+0==0 || fb+0==0) { print "  no frames"; exit }
	x=a/fa/1e6; y=b/fb/1e6;
	printf "  WINDOW_BACKDROP capture: %.2f Mpx/frame at 2 windows, %.2f at 6  (%.2fx)\n",
		x, y, (x>0 ? y/x : 0);
	printf "  chains/frame: %.2f -> %.2f\n", ca/fa, cb/fb;
	printf "  the output is 8.29 Mpx; at 6 windows the renderer captures %.2f screenfuls per frame\n",
		y/8.294;
}'

echo
echo "── the cache ─────────────────────────────────────────────────────────"
for L in "2:$S2" "6:$S6"; do
	N="${L%%:*}"; S="${L#*:}"
	printf "  %d windows: requests=%s hits=%s rebuilds=%s gen=%s bytes=%s\n" \
		"$N" "$(v ch_req "$S")" "$(v ch_hit "$S")" "$(v ch_reb "$S")" \
		"$(v ch_gen "$S")" "$(v ch_bytes "$S")"
	awk -v f="$(v frames "$S")" -v spx="$(v ch_spx "$S")" \
	    -v sbl="$(v ch_sbl "$S")" -v sch="$(v ch_sch "$S")" 'BEGIN{
		if (f+0==0) exit;
		printf "             avoided per frame: %.2f Mpx replay, %.2f Mpx capture, %.2f chains\n",
			spx/f/1e6, sbl/f/1e6, sch/f;
	}'
done

echo
hl_assert "no validation errors (2)" "$(v verr "$S2")" 0
hl_assert "no validation errors (6)" "$(v verr "$S6")" 0
echo
echo "logs: $OUTDIR"
hl_summary
