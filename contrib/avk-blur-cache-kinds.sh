#!/usr/bin/env bash
# avk-blur-cache-kinds.sh -- ONE cache, TWO images, and each must be validated
# against the source IT was built from.
#
# The monitor background cache holds two pictures of the same background: the
# plain one a window's backdrop samples, and the darkened one a shadow's
# backdrop samples (see avk_blur_cache_kind -- serving either from the other is
# a visible defect). They are built INDEPENDENTLY: a kind is only rebuilt when
# something asked for it that frame, and "asked for it" is gated on the frame
# damage reaching a consumer of that kind. An ordinary frame rebuilds one and
# leaves the other exactly as it was.
#
# avk-blur-cache-dirty.sh proves the cache notices when its source changes. It
# cannot see this, because every arm in it damages the whole output and both
# kinds are therefore rebuilt together on every mutation. What it leaves
# untested is the frame where only ONE of them is rebuilt -- which is the
# common frame, not the exotic one.
#
# ── THE RULE ─────────────────────────────────────────────────────────────
#
# If the background changes while nothing is asking for kind K, then the next
# frame that DOES ask for kind K must rebuild it. Not hit it.
#
# The shipped defect is that it hits: the identity (generation, source digest,
# extent, format) was one record shared by both images and stamped whenever
# EITHER kind was ready, so the kind that rebuilt certified the kind that did
# not. The stale image then compared equal on every field for the rest of the
# session -- a blurred backdrop of a wallpaper several rotations old, beside a
# correct sharp one.
#
# ── WHY THIS NEEDS TWO INSTRUMENTS, AND WHAT WAS TRIED FIRST ─────────────
#
# STARVING A KIND. `set_blur_cache_starve,2` makes the DARK consumers count as
# undamaged: not checked, not rebuilt, not served. Everything else is
# untouched.
#
# The first version of this file starved DARK by turning
# `shadows_blur_background` off, which really does remove every DARK consumer
# -- and it PASSED ON THE BROKEN BUILD. A config change reaches
# layer_flush_blur_background() and MOVES THE GENERATION, so when the kind came
# back the check found a generation disagreement, returned GENERATION, and
# rebuilt for a reason that had nothing to do with the rule. avk_blur_cache_
# check() returns at the FIRST disagreement, so any lever that disturbs the
# generation makes every later rule unreachable. That is the same trap
# AZ_BLUR_CACHE_IGNORE_DIRTY fell into one milestone earlier, by a different
# road. The starve instrument touches only the demand, so the generation is
# left exactly where the wallpaper change put it.
#
# THE BREAK. `AZ_BLUR_CACHE_SHARED_IDENTITY=1` restores the shipped rule:
# validate both kinds against the cache-wide record. It is the only thing that
# can make this file red, so a run in which the BREAK arm stays green is a
# broken fixture, not a passing build.
#
# EVERY PREMISE IS ASSERTED, because three of the four ways this file has been
# wrong so far were silent: a scene with no DARK consumer at all (tiled windows
# get no shadow unless `only-floating 0`), a lever that moved the generation,
# and a pixel threshold that measured the cached/live gap rather than staleness.
set -u

# NEVER THE OPERATOR'S SESSION. amsg resolves its socket from this variable, so
# a fixture that inherits it dispatches damage_all, set_blur_cache_starve and
# capture_output at the real desktop instead of at its own compositor. Cleared
# before the harness is sourced, because that is where the instance signature is
# decided.
unset ASTEROIDZ_INSTANCE_SIGNATURE WAYLAND_DISPLAY

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-cache-kinds"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-bckinds-$$"
mkdir -p "$OUTDIR"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

W="${W:-1920}"
H="${H:-1080}"
SCALE="${SCALE:-1.0}"
WINDOWS="${WINDOWS:-2}"

# Gaps, and they are the fixture -- the same argument as avk-blur-cache-dirty.sh.
# Only a TILED window is a cache consumer, tiled windows fill the output, and the
# background this is about is then invisible. The gutters are where every tile's
# backdrop and every shadow's halo reach for their source.
#
# `only-floating 0` IS LOAD-BEARING, and leaving it out cost a whole A/B cycle.
# It defaults to 1: tiled windows get no shadow, therefore no shadow_blur node,
# therefore NO DARK CONSUMER -- and a floating window cannot stand in, because
# the compositor excludes floating windows from the cache entirely
# (blur_cached = blur_optimized && !c->isfloating). That run read `dark h/r=0/0`
# at every step and reported "the DARK image was not rebuilt", which was true
# and meant nothing.
#
# `size 28` makes the shadow halo big enough that a stale one is a readable
# number of pixels rather than a rounding error.
#
# THE KEYS ARE THE KDL PATHS FROM config-schema.h, not the flat option names.
# `effects/shadow/blur` is the shadow's softness and `effects/shadow/size` its
# reach; a fixture that writes `blur_sigma` here (as an older one does) is
# silently configuring nothing and running on the defaults.
#
# NOTHING BELOW THIS LINE IS SHELL. It is the literal text of a KDL file, and a
# `#` line inside it is a KDL parse error, not a comment -- which is how an
# earlier note here spent a cycle being rejected by the parser. Comments about
# the config go above the quotes.
CFG='border_radius 12
borderpx 4
gappih 110
gappiv 110
gappoh 150
gappov 150
effects { shadow { enable 1; only-floating 0; size 28; blur 14
    blur-background 1; blur-background-darken 1; blur-background-strength 1.0 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.0; brightness 0.9; contrast 0.9; saturation 1.2 } } }'

export WLBGEFFECT_SSD=1
# A FLAT BACKGROUND CANNOT FALSIFY A BLUR -- see wllayer.c. The checkerboard is
# what makes a stale backdrop differ from a fresh one at all.
export WLLAYER_PATTERN=1

JQ='{frames:.frames, req:.blur_cache_requests, hit:.blur_cache_hits,
     reb:.blur_cache_rebuilds, gen:.blur_cache_generation,
     ph:.blur_cache_plain_hits, pr:.blur_cache_plain_rebuilds,
     dh:.blur_cache_dark_hits,  dr:.blur_cache_dark_rebuilds,
     inv_gen:.blur_cache_inv_generation, inv_src:.blur_cache_inv_source,
     inv_new:.blur_cache_inv_never_built,
     verr:.validation_errors}'
snap() { hl_get "get avk-stats" | jq -r "$JQ | to_entries |
	map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null; }
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
diffpx() { python3 "$PPM" diff "$OUTDIR/$1.ppm" "$OUTDIR/$2.ppm" 2>/dev/null \
	|| echo "-1 -1"; }

CAPDIR=""
# A PPM IS ONLY A PICTURE WHEN IT IS COMPLETE. capture_output writes the file
# from the Vulkan attachment while this script is already reading it; a short
# read is a truncated image that diffs as "everything changed". Wait for the
# size the header promises instead of sleeping and hoping.
shoot() { # shoot NAME
	local src="$CAPDIR/HEADLESS-1.ppm" want i
	rm -f "$src"
	hl_dispatch capture_output 1
	want=$(( W * H * 3 ))
	for i in $(seq 1 60); do
		[ -f "$src" ] && [ "$(stat -c %s "$src" 2>/dev/null || echo 0)" \
			-ge "$want" ] && break
		sleep 0.25
	done
	cp -f "$src" "$OUTDIR/$1.ppm" 2>/dev/null || true
}

# ── one whole session, parameterised by the cache env ────────────────────
#
# ONE session for the whole sequence, deliberately. A cache torn down between
# the steps starts every step from NEVER_BUILT and passes without the
# invalidation path ever running -- which is the way this file could most
# easily be green and blind.
session() { # session TAG [ENV...]
	local tag="$1"; shift
	local dir="$OUTDIR/$tag"
	CAPDIR="$OUTDIR/$tag-cap"
	mkdir -p "$dir" "$CAPDIR"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	HL_ENV="AZ_SHADOW_DITHER_AMP=0 AZ_AVK_CAPTURE_DIR=$CAPDIR $*"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1

	# The wallpaper stand-in: a BACKGROUND layer surface, the one layer
	# asteroidz refuses to give a blur node of its own and therefore the one
	# that IS the cache's source. Its in-place resize is the wallpaper change,
	# and 90 seconds is longer than any plausible setup -- the fixture then
	# WAITS FOR THE LOG LINE rather than trusting the number.
	hl_spawn_wllayer background top 0 "$W" $(( H * 2 / 5 )) none 300 \
		"90,$W,$H" "bg-$tag" >/dev/null
	sleep 2
	local i=0
	while [ "$i" -lt "$WINDOWS" ]; do
		hl_spawn_wlbgeffect "bd$i" 300 "bd$i-$tag" >/dev/null
		hl_wait_client_count "$(( i + 1 ))" 200
		i=$(( i + 1 ))
	done
	sleep 2
	hl_dispatch reset_avk_stats 1
	hl_dispatch damage_all 0.6
	hl_dispatch damage_all 0.6
	S_WARM="$(snap)"
	shoot "$tag-warm"

	# ── 1. STARVE THE DARK KIND ──────────────────────────────────────────
	# 2 == dark. Demand only: no config is touched, so the generation stays
	# exactly where it is and the rule under test stays reachable.
	hl_dispatch "set_blur_cache_starve,2" 2
	hl_dispatch damage_all 0.6
	hl_dispatch damage_all 0.6
	S_STARVED="$(snap)"

	# ── 2. THE BACKGROUND CHANGES WHILE DARK IS STARVED ──────────────────
	local waited=0
	while [ "$waited" -lt 900 ]; do
		grep -q "resized in place" "$dir/bg-$tag.log" 2>/dev/null && break
		sleep 0.5
		waited=$(( waited + 1 ))
	done
	if ! grep -q "resized in place" "$dir/bg-$tag.log" 2>/dev/null; then
		echo "  WARNING[$tag]: the background layer never resized -- this run"
		echo "  is measuring a source that did not change"
	fi
	sleep 1
	hl_dispatch damage_all 0.6
	hl_dispatch damage_all 0.6
	S_CHANGED="$(snap)"

	# ── 3. THE DARK KIND IS ASKED FOR AGAIN ──────────────────────────────
	hl_dispatch "set_blur_cache_starve,0" 2
	hl_dispatch damage_all 0.6
	hl_dispatch damage_all 0.6
	S_BACK="$(snap)"
	shoot "$tag-back"

	hl_stop >/dev/null 2>&1
}

echo "══ each cached kind is validated against its own source ══ ${W}x${H} scale $SCALE"
# WHICH BINARY. A run that silently used the wrong one proves nothing -- see the
# note on HL_ASTEROIDZ in lib/headless.sh.
echo "   binary: $HL_ASTEROIDZ"
echo

echo "── reference: cache OFF, nothing can ever be stale ───────────────────"
session off AZ_BLUR_CACHE=0
OFF_WARM="$S_WARM"; OFF_BACK="$S_BACK"
read -r DREF _ <<<"$(diffpx off-warm off-back)"
echo "  with the cache off, the whole sequence moves $DREF px"
# THE PREMISE FOR EVERY PIXEL ASSERTION BELOW. A mutation that produces an
# identical desktop cannot distinguish a cache that tracked it from one that
# ignored it entirely.
hl_assert_true "PREMISE: the background change is VISIBLE at all ($DREF px)" \
	"$([ "$DREF" -gt 1000 ] && echo true || echo false)"

echo
echo "── A: cache ON, per-kind identity ───────────────────────────────────"
session on
ON_WARM="$S_WARM"; ON_STARVED="$S_STARVED"; ON_CHANGED="$S_CHANGED"
ON_BACK="$S_BACK"
echo "  warm    : plain h/r=$(v ph "$ON_WARM")/$(v pr "$ON_WARM") dark h/r=$(v dh "$ON_WARM")/$(v dr "$ON_WARM") gen=$(v gen "$ON_WARM")"
echo "  starved : plain h/r=$(v ph "$ON_STARVED")/$(v pr "$ON_STARVED") dark h/r=$(v dh "$ON_STARVED")/$(v dr "$ON_STARVED")"
echo "  changed : plain h/r=$(v ph "$ON_CHANGED")/$(v pr "$ON_CHANGED") dark h/r=$(v dh "$ON_CHANGED")/$(v dr "$ON_CHANGED") gen=$(v gen "$ON_CHANGED") inv_gen=$(v inv_gen "$ON_CHANGED")"
echo "  back    : plain h/r=$(v ph "$ON_BACK")/$(v pr "$ON_BACK") dark h/r=$(v dh "$ON_BACK")/$(v dr "$ON_BACK") gen=$(v gen "$ON_BACK") inv_gen=$(v inv_gen "$ON_BACK")"

# ── every premise this file's conclusion rests on ────────────────────────
hl_assert_true "PREMISE: BOTH kinds were in play while warm" \
	"$([ "$(v ph "$ON_WARM")" -gt 0 ] && [ "$(v dh "$ON_WARM")" -gt 0 ] \
		&& echo true || echo false)"
hl_assert_true "PREMISE: the starve really stopped the DARK consumers" \
	"$([ "$(v dh "$ON_CHANGED")" -eq "$(v dh "$ON_STARVED")" ] \
		&& echo true || echo false)"
hl_assert_true "PREMISE: PLAIN kept being served through the starve" \
	"$([ "$(v ph "$ON_CHANGED")" -gt "$(v ph "$ON_STARVED")" ] \
		&& echo true || echo false)"
# THE IDENTITY MOVED. Without this the cache never learned about the new
# background at all and the rule below is vacuous -- a kind that was never
# certified against a new source cannot be wrongly certified against it.
hl_assert_true "PREMISE: the source change rebuilt PLAIN while DARK was starved" \
	"$([ "$(v pr "$ON_CHANGED")" -gt "$(v pr "$ON_STARVED")" ] \
		&& echo true || echo false)"
hl_assert_true "PREMISE: and the DARK image survived the starve unrebuilt" \
	"$([ "$(v dr "$ON_CHANGED")" -eq "$(v dr "$ON_STARVED")" ] \
		&& echo true || echo false)"

echo
echo "── B: THE RULE -- a kind that missed a source change must rebuild ────"
echo "  dark rebuilds: $(v dr "$ON_CHANGED") before the kind came back, $(v dr "$ON_BACK") after"
hl_assert_true "the DARK consumers are being served again at all" \
	"$([ "$(v dh "$ON_BACK")" -gt "$(v dh "$ON_CHANGED")" ] \
		&& echo true || echo false)"
hl_assert_true "THE RULE: the DARK image was REBUILT, not served stale" \
	"$([ "$(v dr "$ON_BACK")" -gt "$(v dr "$ON_CHANGED")" ] \
		&& echo true || echo false)"

echo
echo "── C: BREAK -- validate both kinds against one shared record ─────────"
# THE SHIPPED DEFECT, restored. Without this arm every assertion above is
# satisfied by a build that simply rebuilds everything all the time, and by the
# broken build too if the lever happens to disturb something else.
session broken AZ_BLUR_CACHE_SHARED_IDENTITY=1
BRK_CHANGED="$S_CHANGED"; BRK_BACK="$S_BACK"
echo "  dark rebuilds: $(v dr "$BRK_CHANGED") before, $(v dr "$BRK_BACK") after"
hl_assert_true "PREMISE: the break's DARK consumers are being served again" \
	"$([ "$(v dh "$BRK_BACK")" -gt "$(v dh "$BRK_CHANGED")" ] \
		&& echo true || echo false)"
hl_assert "BREAK: the stale DARK image was served, not rebuilt" \
	"$(v dr "$BRK_BACK")" "$(v dr "$BRK_CHANGED")"

echo
echo "── D: and the picture says the same thing ────────────────────────────"
# SELF-CALIBRATING, and the earlier version of this arm was not.
#
# A cached picture and a live one are not bit-equal by design -- a cached
# shadow backdrop draws the blurred WALLPAPER whatever is really beneath it.
# That gap also GROWS as the wallpaper covers more of the screen, so an
# absolute threshold measured the wallpaper's size and not staleness: it failed
# identically on the correct and the broken build.
#
# Both numbers here are a CACHED run against the SAME live reference at the
# SAME point in the sequence, so the intrinsic gap is common to both and what
# is left is the staleness.
read -r DON _  <<<"$(diffpx on-back off-back)"
read -r DBRK _ <<<"$(diffpx broken-back off-back)"
awk -v a="$DON" -v b="$DBRK" 'BEGIN{
	printf "  distance from the live truth: correct %d px, broken %d px (%.2fx)\n",
		a, b, (a>0 ? b/a : 0);
}'
hl_assert_true "BREAK: the stale desktop is much further from the truth ($DBRK > 2x $DON)" \
	"$([ "$DBRK" -gt $(( DON * 2 )) ] && echo true || echo false)"

echo
echo "── E: soundness ─────────────────────────────────────────────────────"
hl_assert "cache on: validation errors"  "$(v verr "$ON_BACK")" 0
hl_assert "cache off: validation errors" "$(v verr "$OFF_BACK")" 0
hl_assert "break: validation errors"     "$(v verr "$BRK_BACK")" 0

echo
echo "logs: $OUTDIR"
hl_summary
