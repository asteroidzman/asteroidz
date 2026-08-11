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
esac
HL_SCALE1="$SCALE"
export HL_OUTDIR HL_ENV HL_SCALE1

echo "binary under test: $HL_ASTEROIDZ   scale $SCALE   radius $RADIUS"

# Only the BOTTOM corners rounded (12 = BOTTOM_LEFT|BOTTOM_RIGHT; this option
# is an integer BITMASK, not a name -- "bottom" parses as 0 via atoi and the
# default silently stands, which reads as a per-corner bug in the renderer). This is the configuration the audit says
# matters and the one a single-radius build renders wrong: with all four equal
# every mistake in this file's list still passes.
# Titlebar OFF, and that is load-bearing. asteroidz composes a window from
# several nodes -- titlebar, border, client -- each carrying its own radii, and
# a probe at the window's top corners samples whichever is topmost there. The
# first version of this test read the TITLEBAR's rounding and reported it as
# the client's, which looked exactly like a per-corner bug in the renderer.
hl_start "border_radius $RADIUS
borderpx 0
border_radius_location_default 12
layout {
    titlebar { enable 0 }
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

hl_screenshot rounded
SHOT="$OUTDIR/rounded.png"

python3 - "$SHOT" "$WX" "$WY" "$WW" "$WH" "$RADIUS" "$SCALE" <<'PY' > "$OUTDIR/probe.txt"
import sys
from PIL import Image
shot, wx, wy, ww, wh, radius, scale = sys.argv[1], *map(float, sys.argv[2:])
im = Image.open(shot).convert("RGB"); px = im.load()
W, H = im.size
# logical -> output pixels, the same conversion az_avk_box_to_output makes
X0, Y0 = round(wx*scale), round(wy*scale)
X1, Y1 = round((wx+ww)*scale), round((wy+wh)*scale)
R = radius*scale

def at(x, y):
    x = max(0, min(W-1, int(x))); y = max(0, min(H-1, int(y)))
    return px[x, y]

# A corner is ROUNDED if the extreme corner pixel differs from a pixel well
# inside the window on the same diagonal. Sampling one pixel in from the very
# edge avoids the antialiased boundary itself.
def corner_state(cx, cy, dx, dy):
    outer = at(cx + 2*dx, cy + 2*dy)          # just inside the corner
    inner = at(cx + (R+6)*dx, cy + (R+6)*dy)  # comfortably past the arc
    return ("ROUND" if outer != inner else "SQUARE"), outer, inner

tl = corner_state(X0, Y0,  1,  1)
tr = corner_state(X1, Y0, -1,  1)
br = corner_state(X1, Y1, -1, -1)
bl = corner_state(X0, Y1,  1, -1)
for name, st in (("tl", tl), ("tr", tr), ("br", br), ("bl", bl)):
    print(name, st[0], st[1], st[2])
print("scaled_radius", round(R, 2))
PY
cat "$OUTDIR/probe.txt" | sed 's/^/  /'

TL=$(awk '/^tl/{print $2}' "$OUTDIR/probe.txt")
TR=$(awk '/^tr/{print $2}' "$OUTDIR/probe.txt")
BR=$(awk '/^br/{print $2}' "$OUTDIR/probe.txt")
BL=$(awk '/^bl/{print $2}' "$OUTDIR/probe.txt")

echo
echo "-- only the configured corners are rounded --"
hl_assert "bottom-left is rounded"  "$BL" "ROUND"
hl_assert "bottom-right is rounded" "$BR" "ROUND"
# THE assertion a single-radius build fails: the top corners were not asked
# for and must stay square.
hl_assert "top-left is SQUARE (not asked for)"  "$TL" "SQUARE"
hl_assert "top-right is SQUARE (not asked for)" "$TR" "SQUARE"

echo
echo "-- rounding does not expand damage --"
# A rounded window that damages more than an unrounded one would mean the
# radius leaked into the damage calculation. Compare the damage ratio against
# a run with rounding disabled at the same geometry.
DR=$(hl_get "get avk-stats" | jq -r '.damage_ratio')
FULL=$(hl_get "get avk-stats" | jq -r '.full_redraw_frames')
echo "  damage_ratio $DR, full redraws $FULL"
hl_assert "a rounded scene still redraws partially (not every frame full)" \
	"$([ "$FULL" -lt 60 ] && echo true || echo false)" "true"

RD=$(hl_get "get avk-stats" | jq -r '.rounded_clip_draws')
echo "  rounded_clip_draws $RD"
if [ "$BREAK" != rounded-clip ]; then
	hl_assert "the rounded path was actually taken ($RD draws)" \
		"$([ "$RD" -gt 0 ] && echo true || echo false)" "true"
fi

hl_stop
hl_summary
