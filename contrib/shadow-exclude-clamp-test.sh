#!/usr/bin/env bash
# shadow-exclude-clamp-test.sh — the darken clamp must stop at the hole.
#
# A shadow's backdrop blur excludes the window's own box from what it samples
# (otherwise the window blurs into its own shadow). The hole that leaves is
# then FILLED, by blur_exclude_from_source: one pixel row of the ring outside
# it stretched across the middle, and a mirror of the real content within the
# blur's reach of each edge. That fill is a fabrication -- a good enough one
# that nothing foreign bleeds out of the hole, which is all it has to be.
#
# The darken clamp (shadow-darken-test.sh) works by taking min() against the
# unblurred source, on the premise that the source IS the backdrop. Inside the
# hole that premise is false: there the "source" is the fill, and clamping
# against it drags the region down toward the fill's own darkest structure --
# measured here at nine levels through a mostly-transparent window, so far more
# than that in the backdrop itself. It shows as a dark, window-shaped patch,
# invisible behind an opaque window and plain through a translucent one.
#
# So: the clamp must change nothing inside the excluded box. That is a
# statement about two renderings of one scene, which no single capture can
# make, so the compositor is run twice over the same scene with
# FX_BLUR_NO_DARKEN_CLAMP toggling the clamp off for the second. Inside the
# hole the two must agree; below the window they must NOT, which is what proves
# the clamp was on at all and the first assertion is not passing vacuously.
#
# Vulkan only -- the clamp lives in the compute upsample pass.
#
# Calibrated against a build that clamps the whole region: 8.96 levels of
# difference inside the hole, 0.00 with the hole skipped.
set -u

REPO="${ASTEROIDZ_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# Window geometry is fixed by the rule below; open-floating centres it on the
# 1920x1080 headless output. Same numbers as shadow-darken-test.sh.
WX=610; WY=290; WW=700; WH=500

# ---- phase 2: one capture, invoked by the parent with the clamp on or off ---
if [ "${1:-}" = "--capture" ]; then
	OUTPNG="$2"
	WORK="$(mktemp -d /tmp/asteroidz-excl-XXXXXX)"
	export HL_RENDERER=vulkan
	# shellcheck disable=SC1091
	. "$REPO/contrib/lib/headless.sh"
	hl_start
	trap 'hl_stop; rm -rf "$WORK"' EXIT

	# Fine bright lines on black: mostly dark with sparse bright detail, the
	# structure of text without a terminal to produce it. A flat wallpaper
	# cannot show this -- a blur of a constant field is that field, and so is
	# min() of it with itself.
	magick -size "${HL_WIDTH}x${HL_HEIGHT}" xc:'#000000' \
		-fill '#ffffff' -draw "$(for y in $(seq 0 8 "$HL_HEIGHT"); do
			printf 'rectangle 0,%d %d,%d ' "$y" "$HL_WIDTH" "$((y + 1))"; done)" \
		"$WORK/lines.png"
	kill "$HL_SWAYBG_PID" 2>/dev/null
	swaybg -o '*' -i "$WORK/lines.png" -m fill >/dev/null 2>&1 &
	HL_SWAYBG_PID=$!
	sleep 2

	cat >> "$HL_CONFIG" <<EOF
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
window-rule { match title="EXCL"; open-floating #true; width $WW; height $WH }
EOF
	hl_dispatch "reload_config" 1

	# Barely-there background: the point is to SEE the shadow's backdrop
	# through the window, which is the only place the hole is observable.
	kitty --title EXCL -o background_opacity=0.15 -o background='#101010' \
		sh -c 'clear; exec sleep 300' > "$WORK/kitty.log" 2>&1 &
	HL_SPAWNED_PIDS+=($!)
	hl_wait_client_count 1
	sleep 3
	hl_screenshot excl
	cp "$HL_OUTDIR/excl.png" "$OUTPNG"
	exit 0
fi

# ---- phase 1: two runs of the above, then compare -------------------------
PASS=0; FAIL=0
ok()  { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }

OUTDIR="$(mktemp -d /tmp/asteroidz-exclcmp-XXXXXX)"
trap 'rm -rf "$OUTDIR"' EXIT

# Sequentially, never in parallel: two headless compositors at once contend
# for the GPU and produce failures that have nothing to do with the code.
echo "  ..   capturing with the darken clamp on"
HL_ENV="" bash "$0" --capture "$OUTDIR/on.png"  >/dev/null 2>&1
echo "  ..   capturing with the darken clamp off"
HL_ENV="FX_BLUR_NO_DARKEN_CLAMP=1" bash "$0" --capture "$OUTDIR/off.png" >/dev/null 2>&1

if [ ! -s "$OUTDIR/on.png" ] || [ ! -s "$OUTDIR/off.png" ]; then
	bad "both captures were produced"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
ok "the scene rendered both with and without the darken clamp"

RESULT="$(python3 - "$OUTDIR/on.png" "$OUTDIR/off.png" "$WX" "$WY" "$WW" "$WH" <<'MEASURE'
import sys
from PIL import Image

on_p, off_p = sys.argv[1], sys.argv[2]
wx, wy, ww, wh = (int(v) for v in sys.argv[3:7])

def mean(path, x0, y0, x1, y1, dark_only=False):
    im = Image.open(path).convert("RGB")
    px = im.load()
    scale = im.size[0] / 1920.0
    vals = []
    for y in range(int(y0 * scale), int(y1 * scale)):
        # Only the rows that are dark in the far field: the bright lines are
        # in both samples and would swamp the comparison.
        if dark_only and sum(px[40, y]) / 3 > 20:
            continue
        for x in range(int(x0 * scale), int(x1 * scale)):
            vals.append(sum(px[x, y]) / 3)
    return sum(vals) / len(vals) if vals else 0.0

# Inside the excluded box, inset past the titlebar, border and corner radius.
box = (wx + 40, wy + 60, wx + ww - 40, wy + wh - 40)
# The shadow band below the window, over wallpaper the clamp legitimately owns.
band = (wx + 60, wy + wh + 14, wx + ww - 60, wy + wh + 60)
print("OK %.2f %.2f %.2f %.2f" % (
    mean(on_p, *box), mean(off_p, *box),
    mean(on_p, *band, dark_only=True), mean(off_p, *band, dark_only=True)))
MEASURE
)"

set -- $RESULT
if [ "${1:-}" != "OK" ]; then
	bad "the captures could be measured ($RESULT)"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
IN_ON=$2; IN_OFF=$3; BAND_ON=$4; BAND_OFF=$5
echo "  ..   inside the hole: clamp on $IN_ON, off $IN_OFF"
echo "  ..   shadow band below: clamp on $BAND_ON, off $BAND_OFF"

# Premise first. If the clamp is not doing anything below the window then it is
# not running, and "the hole is unchanged" would pass for the wrong reason.
if python3 -c "import sys; sys.exit(0 if $BAND_ON < $BAND_OFF - 5.0 else 1)"; then
	ok "the clamp is active: it darkens the shadow band below the window"
else
	bad "the clamp is active: it darkens the shadow band below the window ($BAND_ON vs $BAND_OFF -- the premise of the next check does not hold)"
fi

# One level of slack for rounding. The measured failure is nine.
if python3 -c "import sys; sys.exit(0 if abs($IN_ON - $IN_OFF) <= 1.0 else 1)"; then
	ok "the clamp changes nothing inside the window box it excluded"
else
	bad "the clamp changes nothing inside the window box it excluded ($IN_ON vs $IN_OFF -- it is clamping against the synthetic fill)"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
