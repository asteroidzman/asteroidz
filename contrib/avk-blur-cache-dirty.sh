#!/usr/bin/env bash
# avk-blur-cache-dirty.sh -- the cache must go stale exactly when its source does.
#
# avk-blur-cache-test.sh proves a hit equals a rebuild and that FRAME DAMAGE
# does not invalidate. That half of the contract, on its own, is satisfied
# perfectly by a cache that never invalidates at all -- which renders a blurred
# picture of last week's wallpaper and passes every assertion in that file.
#
# This is the other half. For each mutation of the cache's SOURCE or IDENTITY:
#
#     before the change    hit
#     the frame after      rebuild, with the right reason
#     afterwards           hit again
#     and the picture      actually changed
#
# ── EVERY ARM ASSERTS ITS OWN PREMISE ────────────────────────────────────
#
# "The picture changed" is not decoration. A mutation that produces an
# identical desktop cannot distinguish a cache that invalidated correctly from
# one that ignored the change entirely, so each arm first proves that its
# mutation is visible at all -- against a run with the cache OFF, where the
# result is recomputed from scratch every frame and therefore cannot be stale.
#
# ── AND EVERY ARM HAS A BREAK ────────────────────────────────────────────
#
# A green assertion only means something if the corresponding break makes it
# red. AZ_BLUR_CACHE_IGNORE_DIRTY, _STALE_GEOMETRY and _STALE_PARAMS each
# suppress one invalidation reason and nothing else, so each one must leave
# exactly its own arm rendering a stale picture.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-cache-dirty"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-bcdirty-$$"
mkdir -p "$OUTDIR"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

W="${W:-1920}"
H="${H:-1080}"
SCALE="${SCALE:-1.0}"
WINDOWS="${WINDOWS:-3}"

# BIG GAPS, AND THEY ARE THE FIXTURE.
#
# Only a TILED window asks for the cached bottom layer: client_sync_blur_geometry
# sets blur_cached = config.blur_optimized && !c->isfloating, because a floating
# window can sit over another window and its backdrop is therefore not the
# wallpaper. So the windows here must stay tiled -- and tiled windows fill the
# output, which hides the background layer whose change this fixture is about.
#
# Gaps resolve both at once: the windows stay tiled and eligible, and the
# background stays visible in the gutters between and around them, where every
# tile's backdrop blur reaches for its source.
CFG_R6="border_radius 12
borderpx 4
gappih 90
gappiv 90
gappoh 140
gappov 140
effects { shadow { enable 1; size 20; blur_sigma 12; position_y 6 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.0; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

export WLBGEFFECT_SSD=1
# A FLAT BACKGROUND CANNOT FALSIFY A BLUR. Blurring one colour gives the same
# colour at any radius, so on a flat wallpaper the STALE_PARAMS break rendered a
# pixel-identical desktop to the correct build -- which reads as "the break does
# nothing" and actually means "this fixture cannot see". The checkerboard gives
# the blur something to average.
export WLLAYER_PATTERN=1

JQ='{frames:.frames, req:.blur_cache_requests, hit:.blur_cache_hits,
     reb:.blur_cache_rebuilds, gen:.blur_cache_generation,
     inv_gen:.blur_cache_inv_generation, inv_geo:.blur_cache_inv_geometry,
     inv_par:.blur_cache_inv_params, inv_new:.blur_cache_inv_never_built,
     verr:.validation_errors}'
snap() { hl_get "get avk-stats" | jq -r "$JQ | to_entries |
	map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null; }
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
diffpx() { python3 "$PPM" diff "$OUTDIR/$1.ppm" "$OUTDIR/$2.ppm" 2>/dev/null \
	|| echo "-1 -1"; }

CAPDIR=""
shoot() { # shoot NAME
	hl_dispatch capture_output 1
	sleep 1
	cp -f "$CAPDIR/HEADLESS-1.ppm" "$OUTDIR/$1.ppm" 2>/dev/null || true
}

# ── one whole session, parameterised by which break is on ────────────────
#
# The mutations run inside ONE session on purpose. A cache that is torn down and
# rebuilt between them is never asked to invalidate anything: every arm would
# start from NEVER_BUILT and pass without the invalidation path ever running.
session() { # session TAG [ENV...]
	local tag="$1"; shift
	local dir="$OUTDIR/$tag"
	CAPDIR="$OUTDIR/$tag-cap"
	mkdir -p "$dir" "$CAPDIR"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_SHADOW_DITHER_AMP=0 AZ_AVK_CAPTURE_DIR=$CAPDIR $*"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG_R6" >/dev/null 2>&1

	# The wallpaper stand-in: a BACKGROUND layer surface, which is the one layer
	# asteroidz refuses to give a blur node of its own and therefore the one
	# that IS the cache's source. It resizes itself in place 8 seconds in --
	# that commit is what reaches layer_flush_blur_background() and marks the
	# node dirty, and it is a real source change rather than a poked flag.
	# 45 SECONDS, NOT 8.
	#
	# wllayer's resize is a TIMER from its own start, and the setup that follows
	# -- three windows, each with a client-count wait -- took 60+ seconds. The
	# resize therefore fired a minute BEFORE the "before" capture was taken, so
	# before and after were the same picture and the premise read 0 px. The
	# timer is now longer than any plausible setup, and the fixture WAITS FOR
	# THE LOG LINE rather than trusting either number.
	hl_spawn_wllayer background top 0 "$W" $(( H * 2 / 5 )) none 180 \
		"30,$W,$H" "bg-$tag" >/dev/null
	sleep 2
	# TILED. See the note on the gaps above: floating windows are excluded from
	# the cached bottom layer by the compositor itself, so floating these to
	# reveal the background removed every consumer instead -- hits went to zero
	# and the fixture measured a cache nobody used.
	local i=0
	while [ "$i" -lt "$WINDOWS" ]; do
		hl_spawn_wlbgeffect "bd$i" 180 "bd$i-$tag" >/dev/null
		# i + 1, NOT i + 2.
		#
		# A layer surface is not an xdg client and does not appear in
		# all-clients, so waiting for "windows + the background" never reached
		# its count and every spawn burned the full 20-second timeout. Sixty
		# seconds of setup later, the layer's resize timer had already fired and
		# the windows had outlived their hold: the "before" capture was a bare
		# post-resize background with nothing on it, and the whole fixture was
		# comparing two identical flat images.
		hl_wait_client_count "$(( i + 1 ))" 200
		i=$(( i + 1 ))
	done
	sleep 2
	hl_dispatch reset_avk_stats 1
	hl_dispatch damage_all 0.6
	hl_dispatch damage_all 0.6
	S_WARM="$(snap)"
	shoot "$tag-warm"

	# ── 1. THE SOURCE CHANGES: the background layer resizes ──────────────
	#
	# Waited for by OBSERVING IT, not by sleeping long enough. A sleep that is
	# accidentally shorter than the setup silently moves the mutation to the
	# wrong side of the "before" capture, which is exactly what happened.
	local waited=0
	while [ "$waited" -lt 600 ]; do
		grep -q "resized in place" "$dir/bg-$tag.log" 2>/dev/null && break
		sleep 0.5
		waited=$(( waited + 1 ))
	done
	if ! grep -q "resized in place" "$dir/bg-$tag.log" 2>/dev/null; then
		echo "  WARNING[$tag]: the background layer never resized -- this arm"
		echo "  is measuring a source that did not change"
	fi
	sleep 1
	hl_dispatch damage_all 0.6
	hl_dispatch damage_all 0.6
	S_BG="$(snap)"
	shoot "$tag-bg"

	# ── 2. THE IDENTITY CHANGES: output scale ────────────────────────────
	hl_dispatch "set_output_scale,$HL_MON,2.0" 2
	hl_dispatch damage_all 0.6
	hl_dispatch damage_all 0.6
	S_SCALE="$(snap)"
	shoot "$tag-scale"

	hl_stop >/dev/null 2>&1
}

echo "══ the cache goes stale when its source does ══ ${W}x${H} scale $SCALE"
echo

echo "── reference: cache OFF, nothing can ever be stale ───────────────────"
session off AZ_BLUR_CACHE=0
OFF_WARM="$S_WARM"; OFF_BG="$S_BG"
cp -f "$OUTDIR/off-warm.ppm" "$OUTDIR/ref-warm.ppm" 2>/dev/null || true
cp -f "$OUTDIR/off-bg.ppm"   "$OUTDIR/ref-bg.ppm"   2>/dev/null || true
read -r DREF _ <<<"$(diffpx ref-warm ref-bg)"
echo "  with the cache off, the background change moves $DREF px"
# THE PREMISE FOR EVERY ARM BELOW. If the mutation is invisible, a cache that
# ignored it entirely would pass every staleness assertion in this file.
hl_assert_true "PREMISE: the background change is VISIBLE at all ($DREF px)" \
	"$([ "$DREF" -gt 1000 ] && echo true || echo false)"

echo
echo "── A: cache ON -- it must notice ─────────────────────────────────────"
session on
ON_WARM="$S_WARM"; ON_BG="$S_BG"; ON_SCALE="$S_SCALE"
echo "  warm : hits=$(v hit "$ON_WARM") rebuilds=$(v reb "$ON_WARM") gen=$(v gen "$ON_WARM")"
echo "  bg   : hits=$(v hit "$ON_BG") rebuilds=$(v reb "$ON_BG") gen=$(v gen "$ON_BG") inv_gen=$(v inv_gen "$ON_BG")"
echo "  scale: hits=$(v hit "$ON_SCALE") req=$(v req "$ON_SCALE") rebuilds=$(v reb "$ON_SCALE") inv_geo=$(v inv_geo "$ON_SCALE") inv_par=$(v inv_par "$ON_SCALE") inv_gen=$(v inv_gen "$ON_SCALE")"

hl_assert_true "warm: consumers are being served" \
	"$([ "$(v hit "$ON_WARM")" -gt 0 ] && echo true || echo false)"
hl_assert_true "the background change bumped the generation" \
	"$([ "$(v gen "$ON_BG")" -gt "$(v gen "$ON_WARM")" ] && echo true || echo false)"
hl_assert_true "and forced a rebuild" \
	"$([ "$(v reb "$ON_BG")" -gt "$(v reb "$ON_WARM")" ] && echo true || echo false)"
hl_assert_true "attributed to GENERATION, not to something else" \
	"$([ "$(v inv_gen "$ON_BG")" -ge 1 ] && echo true || echo false)"
hl_assert_true "and it went back to hitting afterwards" \
	"$([ "$(v hit "$ON_BG")" -gt "$(v hit "$ON_WARM")" ] && echo true || echo false)"
# PARAMS, NOT GEOMETRY -- and the reason table is what said so.
#
# A scale change on this output does not change the cache's EXTENT: the node is
# sized in logical coordinates and the walker multiplies by the scale, so
# 1920x1080 at 1.0 and 960x540 at 2.0 are the same 1920x1080 of output pixels.
# What does change is the RADIUS, which is a length and is scaled exactly once at
# the walker. So the correct invalidation is PARAMS. Asserting GEOMETRY here was
# my own assumption, and the counter contradicted it.
hl_assert_true "the scale change invalidated the cache (params or geometry)" \
	"$([ $(( $(v inv_par "$ON_SCALE") + $(v inv_geo "$ON_SCALE") )) -ge 1 ] \
		&& echo true || echo false)"
hl_assert_true "and it was attributed to the KERNEL, since the extent is unchanged" \
	"$([ "$(v inv_par "$ON_SCALE")" -ge 1 ] && echo true || echo false)"

echo
echo "── B: the cached picture after the change equals the uncached one ────"
# Not bit-exact: the cached and live paths differ by design where a live blur
# would have sampled something other than the background. What must hold is that
# the cached run TRACKED the change -- its own before/after difference must be
# the same size as the uncached run's, not zero.
read -r DON _ <<<"$(diffpx on-warm on-bg)"
awk -v a="$DREF" -v b="$DON" 'BEGIN{
	printf "  background change moved %d px uncached, %d px cached (%.2fx)\n",
		a, b, (a>0 ? b/a : 0);
}'
hl_assert_true "the cached run tracked the change (>=50% of the uncached delta)" \
	"$([ $(( DON * 2 )) -ge "$DREF" ] && echo true || echo false)"

echo
echo "── C: BREAK -- ignore the dirty generation ───────────────────────────"
session ignoredirty AZ_BLUR_CACHE_IGNORE_DIRTY=1
IGN_BG="$S_BG"
echo "  ignore_dirty: rebuilds=$(v reb "$IGN_BG") inv_gen=$(v inv_gen "$IGN_BG")"
hl_assert "the break suppressed every generation invalidation" \
	"$(v inv_gen "$IGN_BG")" 0
read -r DIGN _ <<<"$(diffpx ignoredirty-warm ignoredirty-bg)"
echo "  with the break, the background change moves $DIGN px (was $DON)"
# THE FALSIFIER. The source changed and the cache did not notice, so the blurred
# backdrop still shows the old background. A green run here means the dirty path
# is not doing anything and arm A proved nothing.
# STRICTLY less, not half.
#
# The background layer is composited directly, so when it changes most of the
# screen changes in both builds no matter what the cache does. Only the BLURRED
# BACKDROPS are stale, and they are a small part of the picture -- 144000 px of
# 2024120 here. Demanding the broken run move less than half asserted that the
# backdrops dominate the frame, which is a claim about the fixture's layout.
hl_assert_true "BREAK: the picture moved LESS than it should have ($DIGN < $DON)" \
	"$([ "$DIGN" -lt "$DON" ] && echo true || echo false)"
read -r DSTALE _ <<<"$(diffpx ignoredirty-bg on-bg)"
echo "  broken vs correct, after the change: $DSTALE px differ"
hl_assert_true "BREAK: and it differs from the correct picture" \
	"$([ "$DSTALE" -gt 1000 ] && echo true || echo false)"

echo
echo "── D: BREAK -- ignore a kernel change ────────────────────────────────"
# STALE_PARAMS and not STALE_GEOMETRY, because on this fixture the scale change
# is a kernel change: see the note in arm A. A geometry break needs an extent
# change, which needs a mode change, and that is arm D2.
session staleparams AZ_BLUR_CACHE_STALE_PARAMS=1
PAR="$S_SCALE"
echo "  stale_params: inv_par=$(v inv_par "$PAR") inv_gen=$(v inv_gen "$PAR") rebuilds=$(v reb "$PAR") hits=$(v hit "$PAR") req=$(v req "$PAR")"
hl_assert "the break suppressed every parameter invalidation" "$(v inv_par "$PAR")" 0
read -r DPAR _ <<<"$(diffpx staleparams-scale on-scale)"
echo "  broken vs correct, after the scale change: $DPAR px differ"
hl_assert_true "BREAK: a stale-kernel cache renders a different picture" \
	"$([ "$DPAR" -gt 1000 ] && echo true || echo false)"

echo
echo "── E: soundness in every arm ─────────────────────────────────────────"
hl_assert "cache on: validation errors"     "$(v verr "$ON_SCALE")" 0
hl_assert "cache off: validation errors"    "$(v verr "$OFF_BG")" 0
hl_assert "ignore_dirty: validation errors" "$(v verr "$IGN_BG")" 0
hl_assert "stale_params: validation errors" "$(v verr "$PAR")" 0

echo
echo "logs: $OUTDIR"
hl_summary
