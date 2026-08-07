#!/usr/bin/env bash
# shadow-hole-visible-test.sh — nothing fabricated may show through a window.
#
# A shadow's backdrop blur excludes the shadowed window's own box from what it
# samples, and fills the hole that leaves so the window cannot bleed out of its
# own shadow: a one-pixel strip stretched across the middle, a mirror of the
# real content within the blur's reach of each edge. That fill is a deliberate
# fabrication and it was never meant to be looked at.
#
# It is looked at. A window at `background_opacity 0.98` shows two percent of
# whatever is composited beneath it, and two percent of a fabrication is still
# a fabrication -- soft saturated rectangles following the stretch, and hard
# black blocks during a move where the frame's damage never wrote. Both were
# reported from a real desktop before anything here could see them, because
# every shadow scene in the suite used opaque windows.
#
# So the composite is clipped out of that box, and this asserts it directly:
# the same scene is rendered twice, once with `shadows_blur_background` on and
# once off, and INSIDE the window the two must be identical -- the shadow's
# blur contributes nothing there, so turning it off may not change a pixel.
# Outside, in the shadow band, they must differ, which is what proves the
# feature was on and the first check is not passing vacuously.
#
# Correlating against the bare wallpaper was tried first and is confounded: a
# translucent window has its OWN backdrop blur, so what shows through is a
# blurred wallpaper and does not track a sharp one. It scored 0.76 on a correct
# build against 0.57 on a broken one -- a real difference, far too narrow to
# assert on.
#
# The corner arcs are deliberately NOT clipped: past a rounded corner the
# backdrop genuinely is visible, so the blur still belongs there. The sample
# region below stays well clear of them.
#
# The window is at 0.85 rather than the 0.98 the fault was reported at, purely
# for signal: at 0.98 the backdrop arrives as 2% of an 8-bit value and the
# measurement is mostly quantisation noise. Same mechanism, more of it.
#
# Vulkan only.
set -u

REPO="${ASTEROIDZ_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
WX=510; WY=230; WW=900; WH=620

if [ "${1:-}" = "--capture" ]; then
	OUTPNG="$2"; BLURBG="$3"
	WORK="$(mktemp -d /tmp/asteroidz-hole-XXXXXX)"
	export HL_RENDERER=vulkan
	# shellcheck disable=SC1091
	. "$REPO/contrib/lib/headless.sh"
	hl_start
	trap 'hl_stop; rm -rf "$WORK"' EXIT

	# Structure EVERYWHERE, not just in the middle. A blur of a flat field is
	# that field, so any region that is flat cannot show whether the blur ran
	# -- the first version put its detail in the centre, left the shadow band
	# over bare backdrop, and the premise check below correctly refused to
	# certify a scene that could not have failed.
	magick -size 48x48 xc:'#141414' -fill '#d0d0d0' \
		-draw 'polygon 0,0 14,0 48,34 48,48 34,48 0,14' \
		-fill '#b02424' -draw 'polygon 26,0 34,0 48,14 48,22' "$WORK/tile.png"
	magick -size "${HL_WIDTH}x${HL_HEIGHT}" "tile:$WORK/tile.png" "$WORK/wp.png"
	kill "$HL_SWAYBG_PID" 2>/dev/null
	swaybg -o '*' -i "$WORK/wp.png" -m fill >/dev/null 2>&1 &
	HL_SWAYBG_PID=$!
	sleep 2

	cat >> "$HL_CONFIG" <<EOF
animations 0
blur 1
blur_layer 0
blur_optimized 1
blur_params_radius 6
blur_params_num_passes 2
shadows 1
shadow_only_floating 1
shadows_size 72
shadows_blur 72
shadows_position_y 18
shadowscolor 0x00000050
shadows_blur_background $BLURBG
shadows_blur_background_strength 0.55
border_radius 9
window-rule { match title="HOLE"; open-floating #true; width $WW; height $WH }
EOF
	hl_dispatch "reload_config" 1

	kitty --title HOLE -o background_opacity=0.85 -o background='#101010' \
		sh -c 'clear; exec sleep 300' > "$WORK/kitty.log" 2>&1 &
	HL_SPAWNED_PIDS+=($!)
	hl_wait_client_count 1
	sleep 3
	hl_screenshot hole
	cp "$HL_OUTDIR/hole.png" "$OUTPNG"
	exit 0
fi

PASS=0; FAIL=0
ok()  { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }

OUTDIR="$(mktemp -d /tmp/asteroidz-holecmp-XXXXXX)"
trap 'rm -rf "$OUTDIR"' EXIT

echo "  ..   capturing with the shadow's backdrop blur on"
bash "$0" --capture "$OUTDIR/on.png"  1 >/dev/null 2>&1
echo "  ..   capturing with it off"
bash "$0" --capture "$OUTDIR/off.png" 0 >/dev/null 2>&1

if [ ! -s "$OUTDIR/on.png" ] || [ ! -s "$OUTDIR/off.png" ]; then
	bad "both captures were produced"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
ok "the scene rendered both with and without the shadow's backdrop blur"

RESULT="$(python3 - "$OUTDIR/on.png" "$OUTDIR/off.png" "$WX" "$WY" "$WW" "$WH" <<'MEASURE'
import sys
from PIL import Image

on_p, off_p = sys.argv[1], sys.argv[2]
wx, wy, ww, wh = (int(v) for v in sys.argv[3:7])

ON = Image.open(on_p).convert("RGB")
OFF = Image.open(off_p).convert("RGB")
S = ON.size[0] / 1920.0
a, b = ON.load(), OFF.load()

def mad(x0, y0, x1, y1):
    """Mean absolute difference between the two captures over a box."""
    t = n = 0
    for y in range(int(y0 * S), int(y1 * S), 2):
        for x in range(int(x0 * S), int(x1 * S), 2):
            t += abs(sum(a[x, y]) - sum(b[x, y])) / 3.0
            n += 1
    return t / n if n else 0.0

# Well inside the window, clear of the titlebar, the border, and the corner
# arcs -- which are deliberately still blurred.
inside = mad(wx + 60, wy + 80, wx + ww - 60, wy + wh - 60)
# The shadow band below the window, which the blur legitimately owns.
band = mad(wx + 80, wy + wh + 16, wx + ww - 80, wy + wh + 62)
print("OK %.3f %.3f" % (inside, band))
MEASURE
)"

set -- $RESULT
if [ "${1:-}" != "OK" ]; then
	bad "the captures could be measured ($RESULT)"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
INSIDE=$2; BAND=$3
echo "  ..   turning the blur off changes: inside the window $INSIDE, shadow band $BAND"

# Premise: the feature is on and visible where it belongs. Without this the
# next check would pass on a build where the blur never ran at all.
if python3 -c "import sys; sys.exit(0 if $BAND > 2.0 else 1)"; then
	ok "the shadow's backdrop blur is active in the band around the window"
else
	bad "the shadow's backdrop blur is active in the band around the window ($BAND -- it is not running, so the next check proves nothing)"
fi

if python3 -c "import sys; sys.exit(0 if $INSIDE < 1.0 else 1)"; then
	ok "the shadow's backdrop blur draws nothing inside the window's own box"
else
	bad "the shadow's backdrop blur draws nothing inside the window's own box ($INSIDE levels of change -- the substitute fill is being composited where it can be seen through)"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
