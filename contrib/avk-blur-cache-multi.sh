#!/usr/bin/env bash
# avk-blur-cache-multi.sh -- the background blur cache on two unequal outputs.
#
# ── WHY THIS CANNOT BE FOLDED INTO avk-blur-cache-test.sh ────────────────
#
# Everything the single-output suite proves is true of a cache that is secretly
# shared between monitors. One image, built from one output's wallpaper at one
# output's resolution with one output's kernel, hit by every consumer on the
# machine, would pass all 26 of those assertions -- because there is only one
# output there to be wrong about.
#
# ── THE TWO OUTPUTS ARE DELIBERATELY UNEQUAL IN BOTH AXES ────────────────
#
# HEADLESS-1  1920x1080  scale 1.5
# HEADLESS-2  1280x720   scale 1.0
#
# Resolution AND scale, because they falsify different things:
#
#   resolution -- the cached IMAGE is sized in output pixels. Two outputs at
#                 one resolution can share one image and still render
#                 correctly, so an equal-resolution fixture cannot see an
#                 alias at all. This mirrors the real desktop, where DP-1 is
#                 3840x2160 beside HDMI-A-1 at 1920x1080.
#   scale      -- the blur KERNEL is the configured radius times the output
#                 scale, and M4I already learned the hard way that a scale
#                 change invalidates on PARAMS rather than GEOMETRY. Equal
#                 scales would let one kernel serve both and look right.
#
# ── WHAT MAKES AN ALIAS VISIBLE ──────────────────────────────────────────
#
# blur_cache_images and blur_cache_texel_bytes are summed over outputs, and
# texel_bytes is arithmetic rather than a measurement: it is exactly
# sum over images of (width x height x 4). With two outputs each holding a
# plain and a dark image there is exactly ONE value it can take, and it names
# both resolutions. A shared cache cannot produce it -- not by coincidence and
# not by rounding.
#
# HEADLESS ABSOLUTE MICROSECONDS ARE NOT BUDGET EVIDENCE. Counts, extents and
# byte arithmetic are what transfer.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-cache-multi"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-bcmulti-$$"
mkdir -p "$OUTDIR"

W1="${W1:-1920}"; H1="${H1:-1080}"; S1="${S1:-1.5}"
W2="${W2:-1280}"; H2="${H2:-720}";  S2="${S2:-1.0}"
CYCLES="${CYCLES:-8}"

# only-floating 0 and blur-background 1: without them every window here is
# tiled, so no shadow exists, so no DARK image is ever built and this measures
# one kind on two outputs instead of two on two.
CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 20; blur 12; only-floating 0; tiled-scale 1.0
    position { y 6 }
    blur-background 1; blur-background-strength 0.55; blur-background-darken 1 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.0; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

export WLBGEFFECT_SSD=1

JQ='{frames:.frames, forced:.blur_nodes_forced_live,
     req:.blur_cache_requests, hit:.blur_cache_hits, reb:.blur_cache_rebuilds,
     plain_hit:.blur_cache_plain_hits, dark_hit:.blur_cache_dark_hits,
     plain_reb:.blur_cache_plain_rebuilds, dark_reb:.blur_cache_dark_rebuilds,
     images:.blur_cache_images, cw:.blur_cache_width, ch:.blur_cache_height,
     texel:.blur_cache_texel_bytes, reqb:.blur_cache_req_bytes,
     bytes:.blur_cache_bytes,
     inv_geo:.blur_cache_inv_geometry, inv_par:.blur_cache_inv_params,
     inv_gen:.blur_cache_inv_generation, inv_new:.blur_cache_inv_never_built,
     verr:.validation_errors, waits:.cpu_sync_waits}'
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

STATS=""; MONS=""; CLIENTS=""; LOG=""
run() { # run NAME [EXTRA_ENV...]
	local name="$1"; shift
	HL_OUTDIR="$OUTDIR/$name"; mkdir -p "$HL_OUTDIR"
	HL_OUTPUTS=2
	HL_WIDTH="$W1";  HL_HEIGHT="$H1";  HL_SCALE1="$S1"
	HL_WIDTH2="$W2"; HL_HEIGHT2="$H2"; HL_SCALE2="$S2"
	# The second output's layout x is LOGICAL, and output 1 at scale 1.5 is only
	# W1/1.5 wide in logical space. The harness default of HL_WIDTH would leave a
	# gap, and a gap is a layout with no seam in it.
	HL_X2=$(( W1 * 10 / 15 ))
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_SHADOW_DITHER_AMP=0 AZ_BLUR_CHAIN_TRACE=1 $*"
	export HL_OUTDIR HL_OUTPUTS HL_WIDTH HL_HEIGHT HL_SCALE1 HL_WIDTH2 \
		HL_HEIGHT2 HL_SCALE2 HL_X2 HL_ENV

	hl_start "$CFG" >/dev/null 2>&1
	# RESET BEFORE ANY WINDOW MAPS -- the cache is built on the first frame that
	# has a consumer, so a settle-then-reset measures an interval in which it was
	# already warm and reports rebuilds=0, which reads exactly like a cache that
	# never builds.
	hl_dispatch reset_avk_stats 1

	# ONE WINDOW PER OUTPUT, each spawned while that output is focused.
	#
	# FOCUS HEADLESS-1 EXPLICITLY FIRST. The focused monitor after startup is not
	# HEADLESS-1 -- it is whichever came up last -- so a fixture that only
	# switches before the SECOND window puts both of them on the same display and
	# then reports a perfectly consistent two-image cache describing one monitor.
	hl_dispatch "focus_monitor,HEADLESS-1" 1
	hl_spawn_wlbgeffect m1 300 m1 >/dev/null; hl_wait_client_count 1 200
	hl_dispatch "focus_monitor,HEADLESS-2" 1
	hl_spawn_wlbgeffect m2 300 m2 >/dev/null; hl_wait_client_count 2 200
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
	MONS="$(hl_get "get all-monitors")"
	CLIENTS="$(hl_get "get all-clients")"
	hl_screenshot_output HEADLESS-1 "$name-out1" >/dev/null 2>&1 || true
	hl_screenshot_output HEADLESS-2 "$name-out2" >/dev/null 2>&1 || true
	LOG="$HL_OUTDIR/state/asteroidz/asteroidz.log"
	hl_stop >/dev/null 2>&1
}

echo "══ background blur cache, two unequal outputs ══"
echo "   HEADLESS-1 ${W1}x${H1} scale $S1     HEADLESS-2 ${W2}x${H2} scale $S2"
echo

run ok
OKSTATS="$STATS"; OKLOG="$LOG"

echo "── the layout the compositor actually built ──────────────────────────"
# `width`/`height` over IPC are LOGICAL, and that is not a detail here: at
# 1920x1080 scale 1.5 and 1280x720 scale 1.0 both outputs report 1280x720, so a
# premise written against the reported width would have declared two identical
# outputs and passed anyway. Raster is logical x scale, and raster is what the
# cache is sized in.
echo "$MONS" | jq -r '.monitors[] | "  \(.name) logical \(.width)x\(.height) scale \(.scale) -> raster \(.width*.scale|floor)x\(.height*.scale|floor) at \(.x),\(.y)"'
NMON="$(echo "$MONS" | jq '.monitors | length')"
NRASTER="$(echo "$MONS" | jq -r '[.monitors[] | "\(.width*.scale|floor)x\(.height*.scale|floor)"] | unique | length')"
# THE PREMISE. Everything below is about two outputs; with one it is vacuous.
hl_assert "PREMISE: two outputs exist" "$NMON" 2
hl_assert "PREMISE: and they differ in RASTER resolution" "$NRASTER" 2
hl_assert_true "PREMISE: and in scale" \
	"$([ "$(echo "$MONS" | jq -r '[.monitors[].scale] | unique | length')" -eq 2 ] \
		&& echo true || echo false)"
echo "  clients:"
echo "$CLIENTS" | jq -r '.clients[] | "    \(.title) on \(.monitor // .mon // "?")"'
# A CONSUMER ON EACH. Without this every per-output assertion below is satisfied
# by an output that renders nothing worth caching -- and the first run of this
# fixture did exactly that, putting both windows on one monitor and reporting a
# tidy two-image cache that described one display.
NCMON="$(echo "$CLIENTS" | jq -r '[.clients[].monitor] | unique | length')"
hl_assert "PREMISE: the windows are on DIFFERENT monitors" "$NCMON" 2

echo
echo "── every output built its own cache ──────────────────────────────────"
echo "  frames=$(v frames "$STATS") forced_live=$(v forced "$STATS")"
echo "  requests=$(v req "$STATS") hits=$(v hit "$STATS") rebuilds=$(v reb "$STATS")"
echo "  plain: hits=$(v plain_hit "$STATS") rebuilds=$(v plain_reb "$STATS")"
echo "  dark:  hits=$(v dark_hit "$STATS") rebuilds=$(v dark_reb "$STATS")"
echo "  images=$(v images "$STATS") largest=$(v cw "$STATS")x$(v ch "$STATS")"
echo "  texel_bytes=$(v texel "$STATS") req_bytes=$(v reqb "$STATS")"
hl_assert_true "consumers on both outputs were served" \
	"$([ "$(v hit "$STATS")" -gt 0 ] && echo true || echo false)"
hl_assert_true "THE PLAIN CONSUMER RAN" \
	"$([ "$(v plain_hit "$STATS")" -gt 0 ] && echo true || echo false)"
hl_assert_true "THE DARK CONSUMER RAN" \
	"$([ "$(v dark_hit "$STATS")" -gt 0 ] && echo true || echo false)"

echo
echo "── NO CROSS-OUTPUT ALIAS ─────────────────────────────────────────────"
# FOUR images: a plain and a dark per output. Not "at least four" -- a fifth
# would mean an output rebuilt at a second extent, and three would mean one
# output is serving a consumer from a neighbour's image.
hl_assert "four cache images: plain and dark, per output" "$(v images "$STATS")" 4
# The arithmetic. There is exactly one value this can take if each output holds
# its own pair at its own resolution, and it names both resolutions at once.
EXPECT=$(( 2 * W1 * H1 * 4 + 2 * W2 * H2 * 4 ))
echo "  expected texel bytes for 2x(${W1}x${H1}) + 2x(${W2}x${H2}) = $EXPECT"
hl_assert "texel bytes name BOTH resolutions" "$(v texel "$STATS")" "$EXPECT"
# The reported extent is the LARGEST across outputs, so it must be output 1's.
# If a shared cache had been built at output 2's size this reads 1280x720.
hl_assert "the largest cached extent is output 1's" \
	"$(v cw "$STATS")x$(v ch "$STATS")" "${W1}x${H1}"
hl_assert_true "the driver asked for at least the texels" \
	"$([ "$(v reqb "$STATS")" -ge "$(v texel "$STATS")" ] && echo true || echo false)"

echo
echo "── both extents appear in the trace, and only those two ──────────────"
# The aggregate above is a sum; this is the per-frame reading that produced it.
# AZ_BLUR_CHAIN_TRACE prints tgt=<render target> and cache=<extent>@<origin> per
# output per frame.
#
# ── THE ORIGIN IS THE PART THAT MATTERS, NOT THE EXTENT ──────────────────
#
# An extent-only comparison passes on the broken build by coincidence, and that
# is not a hypothetical either: monitor 2's node is 1280x720 LOGICAL, and seen
# from output 1 at scale 1.5 that is 1920x1080 -- exactly output 1's raster. The
# wrong node and the right node then have identical extents and only their
# POSITION differs. A correct build puts every output's node at its own origin,
# so the origin is 0,0 on every output and any other value names a neighbour.
pairs_of() { grep -o 'tgt=[0-9]*x[0-9]* cache=[0-9]*x[0-9]*@-\?[0-9]*,-\?[0-9]*' "$1" \
	2>/dev/null | sort -u; }
# Returns the number of trace lines whose cache does not describe its own output:
# a different extent, or an origin other than 0,0.
alien_of() {
	local n=0 line t c o
	while read -r line; do
		[ -n "$line" ] || continue
		t="${line#tgt=}"; t="${t%% *}"
		c="${line#*cache=}"; o="${c#*@}"; c="${c%@*}"
		{ [ "$t" = "$c" ] && [ "$o" = "0,0" ]; } || n=$(( n + 1 ))
	done <<<"$1"
	echo "$n"
}
if [ -f "$LOG" ]; then
	PAIRS="$(pairs_of "$LOG")"
	echo "$PAIRS" | sed 's/^/  /'
	MISMATCH="$(alien_of "$PAIRS")"
	hl_assert "every cache describes ITS OWN output (extent AND origin)" \
		"$MISMATCH" 0
	hl_assert_true "both outputs appear in the trace" \
		"$([ "$(echo "$PAIRS" | grep -c "cache=${W1}x${H1}")" -ge 1 ] \
			&& [ "$(echo "$PAIRS" | grep -c "cache=${W2}x${H2}")" -ge 1 ] \
			&& echo true || echo false)"
else
	hl_skip "no compositor log -- the trace assertions could not run"
fi

echo
echo "── damage is still not source dirt, on either output ─────────────────"
awk -v r="$(v reb "$STATS")" -v c="$CYCLES" 'BEGIN{
	printf "  %d full-output damage cycles -> %d rebuild(s) across both outputs\n", c, r;
}'
# Four: one per image. The bound is the number of IMAGES, not a tolerance.
hl_assert_true "full-output damage did NOT rebuild anything ($(v reb "$STATS") <= 4)" \
	"$([ "$(v reb "$STATS")" -le 4 ] && echo true || echo false)"
hl_assert "no rebuild blamed on a generation bump" "$(v inv_gen "$STATS")" 0
hl_assert "no rebuild blamed on geometry" "$(v inv_geo "$STATS")" 0
hl_assert "no rebuild blamed on the kernel" "$(v inv_par "$STATS")" 0

echo
echo "── soundness ─────────────────────────────────────────────────────────"
hl_assert "validation errors" "$(v verr "$STATS")" 0
hl_assert "CPU sync waits" "$(v waits "$STATS")" 0

echo
echo "── BREAK: let an output use another monitor's node ───────────────────"
#
# ── WHY THIS BREAK EXISTS ────────────────────────────────────────────────
#
# It is not hypothetical. layers[LyrBlur] is one global layer holding every
# monitor's background node, the walk covers the whole scene, and until the
# identity check went in each output simply kept whichever node it reached
# last. On this exact layout that was the OTHER display's: HEADLESS-2 built its
# cache at bounds (-1280,0) -- monitor 1's area expressed in monitor 2's pixels
# -- and every backdrop on that output sampled the wrong background.
#
# The single-output suite cannot see this at any number of assertions, because
# with one output the wrong node and the right node are the same node.
run wrong AZ_BLUR_CACHE_WRONG_OUTPUT=1
BSTATS="$STATS"; BLOG="$LOG"
echo "  images=$(v images "$BSTATS") texel_bytes=$(v texel "$BSTATS")" \
	"largest=$(v cw "$BSTATS")x$(v ch "$BSTATS")"
BPAIRS="$(pairs_of "$BLOG")"
echo "$BPAIRS" | sed 's/^/  /'
BMIS="$(alien_of "$BPAIRS")"
# THE BREAK MUST BREAK SOMETHING. Either an output takes a node that is not its
# own (extent or origin) or the two outputs collapse onto one node and the byte
# arithmetic stops naming both resolutions. The disjunction is asserted rather
# than one specific symptom, because which one shows depends on scene child
# order, and a break that fires in only one ordering is a break that passes for
# the wrong reason in the other.
echo "  alien caches with the break: $BMIS (correct build had $MISMATCH)"
hl_assert_true "BREAK: an output's cache no longer describes that output" \
	"$([ "$BMIS" -gt 0 ] || [ "$(v texel "$BSTATS")" -ne "$EXPECT" ] \
		&& echo true || echo false)"
# And the control, in the same breath: the correct build was clean on the same
# assertion. Without this the break above proves only that something is wrong
# somewhere.
hl_assert "CONTROL: the correct build had no mismatch" "$MISMATCH" 0

echo
echo "logs: $OUTDIR"
hl_summary
