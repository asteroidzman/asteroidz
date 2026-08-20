#!/usr/bin/env bash
# avk-occlusion-test.sh -- the culled frame must be the SAME frame.
#
# M4H added top-down opaque occlusion culling: a command's draw region has every
# opaque command above it in the same segment subtracted from it. The draw
# ledger that motivated it showed an ordinary frame filling the output three
# times over before a single window was drawn -- the scene clear, the background
# rect and an opaque wallpaper, two of which nothing can ever see.
#
# ── WHY THE ASSERTION IS EQUALITY AND NOT "LOOKS RIGHT" ──────────────────
#
# Occlusion culling is the one optimisation whose failure mode is missing
# pixels. It has no visual budget: a build that culls correctly and a build that
# does not cull at all must agree on EVERY pixel, so the test is a pixel oracle
# against AZ_AVK_NO_OCCLUSION=1 rather than an inspection.
#
# Two things make that assertion mean something.
#
#   THE PREMISE. Two builds that both cull nothing also agree on every pixel.
#   So the culled run must be shown to have culled: px_out_clear + px_out_rect
#   drop, measurably, against the same scene rendered without it.
#
#   THE BREAK. AZ_AVK_OCCLUDE_ALL=1 treats every command as an occluder,
#   including the translucent surfaces, the shadows and the rounded corners
#   whose exclusion is the entire correctness argument. The oracle must catch
#   it. An oracle that cannot fail has not passed.
#
# ── THE FIXTURE HAS TO CONTAIN THE HARD CASES ────────────────────────────
#
# An all-opaque, all-square desktop is culled correctly by a rule that is
# recklessly wrong: "everything occludes". The windows here are translucent
# (wlbgeffect is 0x80 alpha on purpose), rounded, bordered, shadowed and
# blurred, because those are the five shapes a careless occluder erases.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-occlusion"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-occl-$$"
mkdir -p "$OUTDIR"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

W="${W:-1920}"
H="${H:-1080}"
SCALE="${SCALE:-1.5}"

# Rounded, bordered, shadowed, blurred. Every one of them is a shape that is
# NOT opaque over its own bounding box.
CFG="border_radius 14
borderpx 5
animations 0
effects { shadow { enable 1; size 18; blur_sigma 10; position_y 5 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.0; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

export WLBGEFFECT_SSD=1

STATS=""
capture() { # capture NAME [EXTRA_ENV]
	local name="$1" extra="${2:-}"
	local dir="$OUTDIR/$name" cdir="$OUTDIR/$name-cap"
	mkdir -p "$dir" "$cdir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	# The dither is noise keyed to fragment position and would differ between
	# two runs for reasons that have nothing to do with occlusion. Off, in both
	# cohorts, so a difference is a difference.
	HL_ENV="AZ_SHADOW_DITHER_AMP=0 \
AZ_AVK_CAPTURE_DIR=$cdir $extra"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1
	hl_dispatch set_option,animations,0 1
	local i=0
	while [ "$i" -lt 3 ]; do
		hl_spawn_wlbgeffect "oc$i" 200 "oc$i" >/dev/null
		hl_wait_client_count "$(( i + 1 ))" 160
		hl_dispatch toggle_floating 0.2
		hl_dispatch "move_window,$(( 40 + i * 210 )),$(( 30 + i * 120 ))" 0.2
		hl_dispatch resize_window,520,360 0.2
		i=$(( i + 1 ))
	done
	sleep 3
	hl_dispatch reset_avk_stats 0.5
	hl_dispatch damage_all 1.5
	STATS="$(hl_get "get avk-stats" | jq -r \
		'"\(.px_out_clear) \(.px_out_rect) \(.px_out_content) \(.px_out_target) \(.shadow_draws) \(.border_draws) \(.blur_draws) \(.validation_errors)"' \
		2>/dev/null)"
	hl_dispatch capture_output 1
	sleep 1
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	hl_stop >/dev/null 2>&1
}

fld() { echo "$2" | awk -v n="$1" '{print $n}'; }
diffpx() {
	python3 "$PPM" diff "$OUTDIR/$1.ppm" "$OUTDIR/$2.ppm" 2>/dev/null \
		|| echo "-1 -1"
}

echo "══ occlusion oracle ══ ${W}x${H} scale $SCALE"
echo

echo "── 0. the premise: the fixture is reproducible at all ────────────────"
capture base
BASE_STATS="$STATS"
capture base2
read -r D M <<<"$(diffpx base base2)"
echo "  two runs of the identical build differ by $D px (worst $M)"
hl_assert "PREMISE: a capture was produced" \
	"$([ -s "$OUTDIR/base.ppm" ] && echo yes || echo no)" yes
hl_assert "PREMISE: the fixture is reproducible between runs" "$D" 0

echo
echo "── 1. the premise: the fixture contains the hard shapes ──────────────"
echo "  shadow_draws=$(fld 5 "$BASE_STATS") border_draws=$(fld 6 "$BASE_STATS") blur_draws=$(fld 7 "$BASE_STATS")"
hl_assert_true "PREMISE: shadows were drawn" \
	"$([ "$(fld 5 "$BASE_STATS")" -gt 0 ] && echo true || echo false)"
hl_assert_true "PREMISE: borders were drawn" \
	"$([ "$(fld 6 "$BASE_STATS")" -gt 0 ] && echo true || echo false)"
hl_assert_true "PREMISE: blur composites were drawn" \
	"$([ "$(fld 7 "$BASE_STATS")" -gt 0 ] && echo true || echo false)"

echo
echo "── 2. the premise: culling actually culled ───────────────────────────"
capture nocull "AZ_AVK_NO_OCCLUSION=1"
NC_STATS="$STATS"
B_HIDDEN=$(( $(fld 1 "$BASE_STATS") + $(fld 2 "$BASE_STATS") ))
N_HIDDEN=$(( $(fld 1 "$NC_STATS") + $(fld 2 "$NC_STATS") ))
TARGET=$(fld 4 "$NC_STATS")
echo "  clear+background fill: culled $B_HIDDEN px, unculled $N_HIDDEN px"
echo "  output target $TARGET px -- the unculled pair is $(awk -v a="$N_HIDDEN" -v b="$TARGET" 'BEGIN{printf "%.2f", (b>0? a/b : 0)}')x the screen"
hl_assert_true "PREMISE: the unculled build filled the screen twice over" \
	"$([ "$TARGET" -gt 0 ] && [ "$N_HIDDEN" -ge $(( TARGET * 3 / 2 )) ] && echo true || echo false)"
hl_assert_true "PREMISE: culling removed a majority of that fill" \
	"$([ "$N_HIDDEN" -gt 0 ] && [ "$B_HIDDEN" -lt $(( N_HIDDEN / 2 )) ] && echo true || echo false)"

echo
echo "── 3. THE ORACLE: culled and unculled are the same picture ───────────"
read -r D M <<<"$(diffpx base nocull)"
echo "  culled vs unculled: $D px differ (worst channel $M)"
hl_assert "culling changes NO pixel" "$D" 0

echo
echo "── 4. the oracle can fail: over-culling is caught ────────────────────"
capture overcull "AZ_AVK_OCCLUDE_ALL=1"
read -r D M <<<"$(diffpx base overcull)"
echo "  every-command-occludes vs production: $D px differ (worst $M)"
hl_assert_true "BREAK: treating translucent/shadow/rounded as opaque IS caught" \
	"$([ "$D" -gt 1000 ] && echo true || echo false)"

echo
echo "── 5. no validation error in any cohort ──────────────────────────────"
hl_assert "production cohort: validation errors" "$(fld 8 "$BASE_STATS")" 0
hl_assert "unculled cohort: validation errors" "$(fld 8 "$NC_STATS")" 0

echo
echo "logs: $OUTDIR"
hl_summary
