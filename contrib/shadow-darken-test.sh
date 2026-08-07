#!/usr/bin/env bash
# shadow-darken-test.sh — a shadow's backdrop blur may only ever darken.
#
# `shadows_blur_background` blurs what lies under a window's shadow. A blur is
# an average, and averaging bright detail over a dark ground RAISES the mean
# wherever the ground is dark. So over content that is mostly dark with sparse
# bright detail -- a terminal full of text is precisely that -- the blurred
# backdrop comes out lighter than the backdrop it replaced, and the shadow shows
# it: a halo that brightens toward the window as the shadow's own alpha ramps
# up. Over smooth content, a photograph say, the mean barely moves and nothing
# shows, which is why this was only ever visible over other windows.
#
# The regression module's existing shadow scenes cannot catch it. They use flat
# single-colour windows and a flat wallpaper, and a blur of a flat field is the
# same flat field -- there is no high-frequency detail for the average to
# redistribute. `a shadow over a dark window only ever darkens it` passes on a
# build with the bug for exactly that reason.
#
# So the backdrop here is a HIGH-FREQUENCY one: fine bright lines on black, the
# structure of text without needing a terminal to produce it. Then the rule is
# the plain one the feature is named for -- no pixel under the shadow may end up
# brighter than the same wallpaper well away from it.
#
# Vulkan only. The clamp lives in the compute upsample pass, which is where the
# unblurred source is still available; the GLES path and the Vulkan graphics
# ping-pong fallback have overwritten it by then.
#
# Calibrated against the unclamped renderer: +23 levels of stray light in the
# shadow band, 0 with the clamp.
set -u

REPO="${ASTEROIDZ_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
WORK="${WORK:-/tmp/asteroidz-shadowdarken-$$}"
rm -rf "$WORK"; mkdir -p "$WORK"

PASS=0; FAIL=0
ok()  { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }

export HL_RENDERER=vulkan

# shellcheck disable=SC1091
. "$REPO/contrib/lib/headless.sh"
hl_start
trap 'hl_stop; rm -rf "$WORK"' EXIT

# Black with fine bright horizontal lines: mostly dark, sparse bright detail,
# nothing above mid-grey anywhere except the lines themselves. A shadow cannot
# make black brighter, so any lift between the lines is the artefact and there
# is no threshold to argue about.
magick -size "${HL_WIDTH}x${HL_HEIGHT}" xc:'#000000' \
	-fill '#ffffff' -draw "$(for y in $(seq 0 8 "$HL_HEIGHT"); do
		printf 'rectangle 0,%d %d,%d ' "$y" "$HL_WIDTH" "$((y + 1))"; done)" \
	"$WORK/lines.png"
kill "$HL_SWAYBG_PID" 2>/dev/null
swaybg -o '*' -i "$WORK/lines.png" -m fill >/dev/null 2>&1 &
HL_SWAYBG_PID=$!
sleep 2

cat >> "$HL_CONFIG" <<'EOF'
animations 0
blur 1
blur_layer 0
blur_optimized 0
blur_passes 2
blur_radius 6
shadows 1
shadow_only_floating 1
shadows_size 72
shadows_blur 72
shadows_blur_background 1
shadows_blur_background_strength 0.55
shadowscolor 0x00000050
shadows_position_y 18
window-rule { match title="DARKEN"; open-floating #true; width 700; height 500 }
EOF
hl_dispatch "reload_config" 1

hl_spawn_kitty DARKEN >/dev/null
hl_wait_client_count 1
sleep 2.5
hl_screenshot darken
cp "$HL_OUTDIR/darken.png" "$WORK/darken.png" 2>/dev/null

RESULT="$(python3 - "$WORK/darken.png" <<'MEASURE'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
W, H = im.size
# The window is 700x500 logical at 610,290; the capture may be device pixels.
scale = W / 1920.0
left = int(610 * scale)
y0 = int(300 * scale)
y1 = int(760 * scale)

def band(x0, x1):
    """Darkest-to-brightest of the DARK rows only, over a column range.

    The bright lines are in both samples and would swamp any comparison, so
    only rows that are dark in the far field are compared -- which is exactly
    where a blur that redistributes the lines' brightness shows up.
    """
    vals = []
    for y in range(y0, y1):
        ref = sum(px[40, y]) / 3
        if ref > 20:            # a line: skip it
            continue
        row = [sum(px[x, y]) / 3 for x in range(x0, x1)]
        vals.append(max(row))
    return max(vals) if vals else 0.0

far = band(30, 90)                              # wallpaper, nowhere near it
shadow = band(max(0, left - int(60 * scale)), max(1, left - int(6 * scale)))
print("OK %.2f %.2f" % (far, shadow))
MEASURE
)"

set -- $RESULT
if [ "${1:-}" != "OK" ]; then
	bad "the scene could be measured ($RESULT)"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
FAR=$2; SHADOW=$3
ok "a floating window with a blurred shadow backdrop is on screen"
echo "  ..   dark rows: far field $FAR, under the shadow $SHADOW"

# Strictly not brighter, with one level of slack for rounding. The shadow is
# free to darken as much as it likes; it may not add light.
if python3 -c "import sys; sys.exit(0 if $SHADOW <= $FAR + 1.0 else 1)"; then
	ok "the shadow's blurred backdrop never comes out brighter than the backdrop"
else
	bad "the shadow's blurred backdrop never comes out brighter than the backdrop ($SHADOW vs $FAR -- an unclamped blur redistributes the bright lines into the dark rows)"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
