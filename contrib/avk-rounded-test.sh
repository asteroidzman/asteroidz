#!/usr/bin/env bash
# avk-rounded-test.sh — M4A: per-corner rounded clipping, verified in pixels.
#
# WHAT THIS HAS TO CATCH THAT A SCREENSHOT DOES NOT
#
# A rounded corner looks right to a human the moment ANY rounding happens. The
# three ways to get it wrong that still look plausible are:
#
#   1. one radius applied to all four corners. SceneFX stores four
#      (struct fx_corner_radii) and asteroidz uses them -- a window joined to
#      its titlebar is rounded on two corners and square on the other two -- so
#      a single-scalar implementation renders a real configuration wrong while
#      looking like a rounded window.
#   2. the output scale applied twice, or not at all. At scale 1.0 both
#      mistakes are invisible; only a fractional-scale output separates them.
#   3. rounding that expands damage. Rounding REMOVES coverage and can never
#      add any, so a radius must not grow the damaged region by one pixel.
#
# So the assertions are geometric, not "does it look round": the corner pixel
# must be background, a pixel just inside the arc must be window, and the two
# must differ per corner when the radii differ.
#
#   BREAK=rounded-clip            AZ_ROUNDED_OFF -- no rounding at all
#   BREAK=rounded-single-radius   AZ_ROUNDED_SINGLE_RADIUS -- all four corners
#                                 take the first, i.e. mistake (1)
#   BREAK=rounded-double-scale    AZ_ROUNDED_DOUBLE_SCALE -- mistake (2)
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-rounded"
BREAK="${BREAK:-}"
RADIUS="${RADIUS:-28}"
SCALE="${HL_SCALE1:-1.0}"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-rounded-$$"
HL_OUTDIR="$OUTDIR"
HL_ENV="ASTEROIDZ_RENDERER=avk"
case "$BREAK" in
	rounded-clip)          HL_ENV="$HL_ENV AZ_ROUNDED_OFF=1" ;;
	rounded-single-radius) HL_ENV="$HL_ENV AZ_ROUNDED_SINGLE_RADIUS=1" ;;
	rounded-double-scale)  HL_ENV="$HL_ENV AZ_ROUNDED_DOUBLE_SCALE=$SCALE" ;;
	rounded-bottom-swap)   HL_ENV="$HL_ENV AZ_ROUNDED_BOTTOM_SWAP=1" ;;
esac
HL_SCALE1="$SCALE"
export HL_OUTDIR HL_ENV HL_SCALE1

echo "binary under test: $HL_ASTEROIDZ   scale $SCALE   radius $RADIUS"

# THE PRODUCER OF ASYMMETRIC RADII, found by tracing rather than guessing:
# set_client_corner_location() in src/animation/client.h. Corners are squared
# by LAYOUT, not by configuration --
#
#   window edge meets the monitor edge   -> that side's two corners squared
#   per-window titlebar tab              -> TOP-LEFT ONLY squared, because the
#                                           close button owns that corner and
#                                           the two pieces must read as one
#   fullscreen / no_radius_when_single   -> all four squared
#
# So the natural asymmetric states are reachable from ordinary window
# management, and this test uses them rather than injecting numbers.
#
# SCENE A (titlebar, window clear of the screen edges): TL square, other three
# rounded. One corner different from the rest -- a single-radius build rounds
# all four and fails.
#
# SCENE B (no gaps, tiled against the left edge): LEFT squared by the edge rule
# AND TOP-LEFT by the titlebar, plus top/bottom where it meets those edges.
# The point of B is that BR and BL end up DIFFERENT, which is what makes a
# bottom-corner swap detectable at all -- in scene A they are equal and a swap
# is invisible.
#
# An earlier version of this test asserted "bottom corners rounded" from
# `border_radius_location_default bottom`. That option is an integer bitmask
# read with atoi(), so the name parses as 0 and the default silently stands;
# the test then reported the compositor's CORRECT titlebar behaviour as a
# per-corner bug. Both traps are why the expectations below come from the
# producer's own rules.
# NO SCENE B, and the reason is worth recording. To get BR != BL -- the only
# arrangement in which a bottom-corner swap is visible -- one side must be
# squared by the edge rule, and that rule is
#
#     target_geom.x + config.border_radius <= bnd_x
#
# i.e. the window must extend PAST the monitor edge by at least the radius, so
# that the arc would be off-screen entirely. A window merely touching the edge
# keeps its rounding, which is correct and is why removing the outer gap does
# nothing. Tiling never produces that state; dragging a floating window off the
# edge does, so BREAK=rounded-bottom-swap is falsifiable live and is recorded
# as NOT TESTED headlessly rather than counted as coverage.
GAPOH=40
hl_start "border_radius $RADIUS
borderpx 0
gappih 40
gappiv 40
gappoh $GAPOH
gappov 40
layout {
    titlebar { enable 1 }
}"
sleep 2

hl_spawn_kitty rnd >/dev/null 2>&1
hl_wait_client_count 1 40 >/dev/null 2>&1
sleep 1.5

read -r WX WY WW WH <<<"$(hl_get "get all-clients" \
	| jq -r '.clients[0]|"\(.x) \(.y) \(.width) \(.height)"')"
echo "window logical $WW x $WH at $WX,$WY"
hl_assert "the test window has geometry" \
	"$([ -n "$WW" ] && [ "$WW" != null ] && echo true || echo false)" "true"

probe() { # probe LABEL
	hl_screenshot "$1"
	python3 - "$OUTDIR/$1.png" "$WX" "$WY" "$WW" "$WH" "$RADIUS" "$SCALE" <<'PY'
import sys
from PIL import Image
shot, wx, wy, ww, wh, radius, scale = sys.argv[1], *map(float, sys.argv[2:])
im = Image.open(shot).convert("RGB"); px = im.load(); W, H = im.size
X0, Y0 = round(wx*scale), round(wy*scale)
X1, Y1 = round((wx+ww)*scale), round((wy+wh)*scale)
R = radius*scale
def at(x, y):
    return px[max(0,min(W-1,int(x))), max(0,min(H-1,int(y)))]
def state(cx, cy, dx, dy):
    # just inside the bounding-box corner vs comfortably past the arc
    return "ROUND" if at(cx+2*dx, cy+2*dy) != at(cx+(R+6)*dx, cy+(R+6)*dy) else "SQUARE"
print("tl", state(X0, Y0,  1,  1))
print("tr", state(X1, Y0, -1,  1))
print("br", state(X1, Y1, -1, -1))
print("bl", state(X0, Y1,  1, -1))

# MEASURE the radius, do not merely classify the corner.
#
# "Is it round?" is true of a 28px arc and a 63px one alike, so a boolean
# probe cannot see a radius scaled twice -- which is exactly the mistake the
# double-scale break injects, and why that break passed a boolean test at both
# scales. The area of the cut-out corner is r^2 (1 - pi/4), so counting
# background pixels in a generous corner box recovers r without needing to
# find the arc.
import math
def measure(cx, cy, dx, dy, span=72):
    bg = at(X1 + 8*1, Y0 - 8)          # outside the window entirely
    n = 0
    for i in range(span):
        for j in range(span):
            if at(cx + (i if dx > 0 else -i-1), cy + (j if dy > 0 else -j-1)) == bg:
                n += 1
    if n == 0:
        return 0.0
    return math.sqrt(n / (1.0 - math.pi/4.0))
print("r_tr", round(measure(X1, Y0, -1, 1), 1))
print("r_br", round(measure(X1, Y1, -1, -1), 1))
PY
}

probe sceneA > "$OUTDIR/a.txt"
sed 's/^/  A /' "$OUTDIR/a.txt"
A_TL=$(awk '/^tl/{print $2}' "$OUTDIR/a.txt"); A_TR=$(awk '/^tr/{print $2}' "$OUTDIR/a.txt")
A_BR=$(awk '/^br/{print $2}' "$OUTDIR/a.txt"); A_BL=$(awk '/^bl/{print $2}' "$OUTDIR/a.txt")

echo
echo "-- the titlebar squares the TOP-LEFT and nothing else --"
hl_assert "top-left is square (the titlebar tab owns that corner)" "$A_TL" "SQUARE"
hl_assert "top-right is rounded"    "$A_TR" "ROUND"
hl_assert "bottom-right is rounded" "$A_BR" "ROUND"
hl_assert "bottom-left is rounded"  "$A_BL" "ROUND"

R_TR=$(awk '/^r_tr/{print $2}' "$OUTDIR/a.txt")
R_BR=$(awk '/^r_br/{print $2}' "$OUTDIR/a.txt")
EXPECT=$(python3 -c "print(round($RADIUS*$SCALE,1))")
echo
echo "-- the radius is the configured one, scaled ONCE --"
echo "  measured top-right $R_TR, bottom-right $R_BR, expected ~$EXPECT physical px"
# +-25%: the estimator is an area integral over an antialiased arc, and the
# border/titlebar can bite into the box. Wide enough to be stable, far tighter
# than the 1.5x a double-scale mistake introduces.
hl_assert "top-right radius is ~${EXPECT}px (scaled once)" \
	"$(python3 -c "print('true' if abs($R_TR-$EXPECT) <= 0.25*$EXPECT else 'false')")" "true"
hl_assert "bottom-right radius is ~${EXPECT}px (scaled once)" \
	"$(python3 -c "print('true' if abs($R_BR-$EXPECT) <= 0.25*$EXPECT else 'false')")" "true"

echo
echo "-- rounding does not expand damage --"
FULL=$(hl_get "get avk-stats" | jq -r '.full_redraw_frames')
DR=$(hl_get "get avk-stats" | jq -r '.damage_ratio')
RD=$(hl_get "get avk-stats" | jq -r '.rounded_clip_draws')
ASYM=$(hl_get "get avk-stats" | jq -r '.rounded_asymmetric_draws')
echo "  damage_ratio $DR, full redraws $FULL, rounded draws $RD, asymmetric $ASYM"
hl_assert "a rounded scene still redraws partially" \
	"$([ "$FULL" -lt 60 ] && echo true || echo false)" "true"
if [ "$BREAK" != rounded-clip ]; then
	hl_assert "the rounded path was taken ($RD draws)" \
		"$([ "$RD" -gt 0 ] && echo true || echo false)" "true"
	hl_assert "asymmetric radii genuinely reached the shader ($ASYM draws)" \
		"$([ "$ASYM" -gt 0 ] && echo true || echo false)" "true"
fi

hl_stop
hl_summary
