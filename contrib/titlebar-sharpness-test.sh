#!/usr/bin/env bash
# titlebar-sharpness-test.sh — the titlebar is rasterised at the output's scale.
#
# asteroidz_tab_bar_node_update() takes a `scale`, and it decides two things at
# once: the pixel size of the surface it draws into (logical * scale) and the
# Pango resolution (96 * scale). The scene buffer is then displayed at the
# LOGICAL size. So the scale is what makes the difference between rasterising
# at the panel's real resolution and rasterising at logical resolution and
# letting the scene graph resample the result.
#
# Every caller but one passed a hardcoded 1.0, so on any output above scale 1
# the titlebar was exactly the right SIZE and visibly soft -- reported live at
# scale 1.5, and equally wrong at 1.75 or 2.
#
# Measured as the share of a glyph that is neither ink nor background. Text
# rasterised at native resolution is close to bimodal: a lot of ink, a lot of
# background, and antialiasing only along the true edges. Rasterise it small
# and scale it up and every one of those edges is smeared across several pixels
# instead, so the mid-tones multiply.
#
# Run at scale 2 rather than 1.5: an integer scale keeps the comparison about
# resampling alone, with no fractional-scale rounding in it.
set -u

REPO="${ASTEROIDZ_REPO:-$HOME/asteroidz}"
OUT="${TB_OUT:-$(mktemp -d)}"
mkdir -p "$OUT"

PASS=0
FAIL=0
ok() { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }

export HL_WIDTH=1920 HL_HEIGHT=1080
export HL_EXTRA_KDL='output HEADLESS-1 { scale 2 }
theme { font "Ubuntu 12"; bg-color 0x101418ff; fg-color 0xffffffff }'

# shellcheck disable=SC1091
. "$REPO/contrib/lib/headless.sh"
hl_start
trap 'hl_stop' EXIT
kill "$HL_SWAYBG_PID" 2>/dev/null

hl_dispatch "set_layout,tile"
hl_spawn_kitty TITLEBARTEST >/dev/null
hl_wait_client_count 1
sleep 2

W="$(hl_get "get all-clients" | jq -r '.clients[] | select(.title=="TITLEBARTEST") | "\(.x) \(.y) \(.width)"')"
if [ -z "$W" ]; then
	bad "a window with a titlebar is on screen"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
ok "a window with a titlebar is on screen ($W)"
read -r WX WY WW <<< "$W"

grim -o "$HL_MON" "$OUT/shot.png" 2>/dev/null
SHOT_W="$(python3 -c "from PIL import Image; print(Image.open('$OUT/shot.png').size[0])")"
# Against the output's LOGICAL width, which is what client geometry is in --
# not against the mode, which is the number the capture already comes back in.
LOG_W="$(hl_get "get all-monitors" | jq -r --arg m "$HL_MON" \
	'.monitors[] | select(.name==$m) | .width')"
SCALE=$(python3 -c "print($SHOT_W / $LOG_W)")
echo "  ..   capture is ${SHOT_W}px wide; device/logical = $SCALE"

# The titlebar sits directly ABOVE the client rect, which is why its height is
# what the client was pushed down by. Everything here is converted into the
# capture's own pixels.
RATIO="$(python3 - "$OUT/shot.png" "$WX" "$WY" "$WW" "$SCALE" <<'MEASURE'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("L")
px = im.load()
W, H = im.size
wx, wy, ww = (int(v) for v in sys.argv[2:5])
scale = float(sys.argv[5])

# The titlebar band: directly above the client rect, in capture pixels.
x0 = max(0, int(wx * scale))
x1 = min(W - 1, int((wx + ww) * scale))
y1 = min(H, int(wy * scale))
y0 = max(0, y1 - int(30 * scale))

# Edge STEEPNESS, not the amount of antialiasing. A glyph rasterised at the
# panel's own resolution goes from background to ink in one step; the same
# glyph rasterised small and resampled up arrives as a ramp across two or three
# pixels, because that is what interpolation does to an edge. Counting
# mid-tones alone does not separate the two -- the titlebar background, the
# borders and the icon supply plenty either way, and the first version of this
# measured 22.2 against 20.5 and could not tell the builds apart.
steep = ramps = 0
for y in range(y0, y1):
    for x in range(x0, x1 - 1):
        d = abs(px[x + 1, y] - px[x, y])
        if d >= 60:
            steep += 1
        elif d >= 15:
            ramps += 1

print("%.2f" % (ramps / steep) if steep else "999")
MEASURE
)"

echo "  ..   soft ramps per steep edge: $RATIO"

# Under 3, with a lot of headroom either side: rasterised at the output's scale
# this measures 0.8, and rasterised at 1.0 and resampled up it measures 15.7.
if python3 -c "import sys; sys.exit(0 if float('$RATIO') < 3.0 else 1)"; then
	ok "the titlebar is rasterised at the output's scale ($RATIO ramps per edge)"
else
	bad "the titlebar is rasterised at the output's scale ($RATIO ramps per edge -- native is ~0.8, resampled ~15.7)"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
