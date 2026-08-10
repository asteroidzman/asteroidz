#!/usr/bin/env bash
# shadow-tiled-neighbour-test.sh — a tiled window's shadow may not touch its
# neighbour.
#
# A shadow used to live at the bottom of its own client's scene tree, which is
# only "below" that one window. A shadow is the window's box plus its spread,
# so on a tiled layout it reaches into whatever is beside it -- and that
# neighbour is a sibling tree under LyrTile, not something the shadow is above.
# Whichever window was raised last had its shadow drawn over the other one: a
# dark edge that moved from window to window as focus changed, and a backdrop
# blur that sampled the neighbour's own pixels and smeared them along the seam.
#
# Reported as "the blended shadow currently will clip through and affect
# adjacent windows".
#
# The fix is stacking: every tiled shadow goes on LyrTileShadow, beneath every
# tiled window, so no shadow can reach another window's pixels whatever order
# the windows are in. What survives is the gap between tiles and the outer edge
# of the layout, which is where there is actually something to cast onto.
#
# Asserted as an invariant rather than as a look: turning shadows off may not
# change a single pixel INSIDE any window's own box. A window's own shadow is
# already clipped out of that box, so anything that does change there came from
# a neighbour -- which is the whole bug. The gap between the two windows must
# change, or the scene had no shadows in it and the first check proves nothing.
#
# Two windows, TILED, with gaps: a shadow needs somewhere to be visible, and
# with gappih 0 there is no lit ground between the tiles at all.
#
# Vulkan only.
set -u

REPO="${ASTEROIDZ_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [ "${1:-}" = "--capture" ]; then
	OUTPNG="$2"; SHADOWS="$3"; GEOMOUT="${4:-}"
	WORK="$(mktemp -d /tmp/asteroidz-tilesh-XXXXXX)"
	export HL_RENDERER=vulkan
	# shellcheck disable=SC1091
	. "$REPO/contrib/lib/headless.sh"
	hl_start
	trap 'hl_stop; rm -rf "$WORK"' EXIT

	# Detail everywhere, for the same reason the hole test needs it: a blur or
	# a tint over a flat field is that field again, and a region with no
	# structure cannot show whether anything was drawn into it.
	magick -size 48x48 xc:'#141414' -fill '#d0d0d0' \
		-draw 'polygon 0,0 14,0 48,34 48,48 34,48 0,14' \
		-fill '#2460b0' -draw 'polygon 26,0 34,0 48,14 48,22' "$WORK/tile.png"
	magick -size "${HL_WIDTH}x${HL_HEIGHT}" "tile:$WORK/tile.png" "$WORK/wp.png"
	kill "$HL_SWAYBG_PID" 2>/dev/null
	swaybg -o '*' -i "$WORK/wp.png" -m fill >/dev/null 2>&1 &
	HL_SWAYBG_PID=$!
	sleep 2

	# shadow_only_floating 0 is the point of the test: these windows are tiled.
	# The shadow is deliberately larger than the gap, which is the condition
	# under which it used to reach the neighbour -- a shadow that fits inside
	# the gap could not fail even on the broken build.
	cat >> "$HL_CONFIG" <<EOF
animations 0
blur 0
blur_optimized 0
shadows $SHADOWS
shadow_only_floating 0
shadows_tiled_scale 1.0
shadows_size 60
shadows_blur 60
shadows_position_x 0
shadows_position_y 0
shadowscolor 0x000000c0
shadows_contact 0
shadows_blur_background 1
shadows_blur_background_strength 0.6
border_radius 6
borderpx 0
gappih 26
gappiv 26
gappoh 26
gappov 26
smartgaps 0
window-rule { match title="FLOATY"; open-floating #true; width 600; height 400 }
EOF
	hl_dispatch "reload_config" 1

	kitty --title TILEA -o background_opacity=1.0 -o background='#b8b8b8' \
		sh -c 'clear; exec sleep 300' > "$WORK/a.log" 2>&1 &
	HL_SPAWNED_PIDS+=($!)
	hl_wait_client_count 1
	kitty --title TILEB -o background_opacity=1.0 -o background='#b8b8b8' \
		sh -c 'clear; exec sleep 300' > "$WORK/b.log" 2>&1 &
	HL_SPAWNED_PIDS+=($!)
	hl_wait_client_count 2
	sleep 3

	# The geometry is READ, never assumed: which of the two is master and how
	# the layout splits is the layout's business, and a hardcoded box would
	# make this test a test of the default layout instead.
	if [ -n "$GEOMOUT" ]; then
		hl_get "get all-clients" | python3 -c '
import json, sys
cs = [c for c in json.load(sys.stdin).get("clients", [])
      if c.get("title") in ("TILEA", "TILEB")]
cs.sort(key=lambda c: c["x"])
for c in cs:
    print(c["x"], c["y"], c["width"], c["height"])
' > "$GEOMOUT"
	fi

	hl_screenshot tiles
	cp "$HL_OUTDIR/tiles.png" "$OUTPNG"

	# ...and again with a FLOATING window over the pair.
	#
	# The other half of the rule, and the one a stacking change can quietly
	# break: a floating window really is above the tiles, so its shadow really
	# does fall on them. Put every shadow on LyrTileShadow and this one would
	# be drawn under the very windows it is supposed to darken -- a floating
	# window with no shadow at all over anything but the wallpaper, which is
	# the sort of regression that looks like "the shadows went away" and has
	# nothing to do with tiling.
	kitty --title FLOATY -o background_opacity=1.0 -o background='#303030' \
		sh -c 'clear; exec sleep 300' > "$WORK/f.log" 2>&1 &
	HL_SPAWNED_PIDS+=($!)
	hl_wait_client_count 3
	sleep 2
	if [ -n "$GEOMOUT" ]; then
		hl_get "get all-clients" | python3 -c '
import json, sys
for c in json.load(sys.stdin).get("clients", []):
    if c.get("title") == "FLOATY":
        print(c["x"], c["y"], c["width"], c["height"]); break
' >> "$GEOMOUT"
	fi
	hl_screenshot floaty
	cp "$HL_OUTDIR/floaty.png" "${OUTPNG%.png}-floaty.png"

	# ...and again with everything on a tag nobody is looking at (tag 9, empty).
	#
	# A tiled window's shadow tree is a SIBLING of its scene tree, so hiding
	# the window no longer hides the shadow through the parent -- every place
	# that hides a window has to hide the shadow too. Miss one and the shadows
	# of the tag you left stay painted on the wallpaper, which is a worse
	# artefact than the one this whole change is about.
	hl_dispatch "view,9" 2
	hl_screenshot empty
	cp "$HL_OUTDIR/empty.png" "${OUTPNG%.png}-empty.png"
	exit 0
fi

PASS=0; FAIL=0
ok()  { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }

OUTDIR="$(mktemp -d /tmp/asteroidz-tileshcmp-XXXXXX)"
trap 'rm -rf "$OUTDIR"' EXIT

echo
echo "tiled shadows and their neighbours"
echo "  ..   capturing two tiled windows with shadows on"
bash "$0" --capture "$OUTDIR/on.png"  1 "$OUTDIR/geom.txt" >/dev/null 2>&1
echo "  ..   capturing the same scene with shadows off"
bash "$0" --capture "$OUTDIR/off.png" 0 >/dev/null 2>&1

if [ ! -s "$OUTDIR/on.png" ] || [ ! -s "$OUTDIR/off.png" ]; then
	bad "both captures were produced"
	echo; echo "  $PASS passed, $FAIL failed"; exit 1
fi
if [ "$(wc -l < "$OUTDIR/geom.txt")" != "3" ]; then
	bad "two tiled windows and a floating one came up (got $(wc -l < "$OUTDIR/geom.txt"))"
	echo; echo "  $PASS passed, $FAIL failed"; exit 1
fi
ok "two tiled windows rendered, with shadows on and off"

RESULT="$(python3 - "$OUTDIR/on.png" "$OUTDIR/off.png" "$OUTDIR/geom.txt" <<'MEASURE'
import sys
from PIL import Image

ON = Image.open(sys.argv[1]).convert("RGB")
OFF = Image.open(sys.argv[2]).convert("RGB")
rows = [tuple(int(v) for v in line.split())
        for line in open(sys.argv[3]) if line.strip()]
boxes = rows[:2]   # the two tiled windows, left to right

S = ON.size[0] / 1920.0
a, b = ON.load(), OFF.load()
W, H = ON.size

def mad(x0, y0, x1, y1):
    """Mean absolute difference between the two captures over a box."""
    t = n = 0
    for y in range(max(0, int(y0 * S)), min(H, int(y1 * S)), 2):
        for x in range(max(0, int(x0 * S)), min(W, int(x1 * S)), 2):
            t += abs(sum(a[x, y]) - sum(b[x, y])) / 3.0
            n += 1
    return t / n if n else -1.0

(lx, ly, lw, lh), (rx, ry, rw, rh) = boxes
top, bot = max(ly, ry) + 40, min(ly + lh, ry + rh) - 40

# A NARROW strip just inside each window's inner edge -- the 40 pixels facing
# the neighbour, which is as far as a 60px shadow reaches past a 26px gap.
#
# Averaging over the whole window was the first measurement here and it is
# useless: the band is 40px of a 921px-wide window, so a 15-level darkening
# along the edge -- plainly visible, and exactly the reported fault -- came out
# as 0.11 levels once spread over the interior, and the test passed against a
# build that had the bug.
inner = max(
    mad(lx + lw - 44, top, lx + lw - 4, bot),   # left window, right edge
    mad(rx + 4, top, rx + 44, bot),             # right window, left edge
)
# The middle of each window, where nothing should ever change either. Kept
# separate so a failure says WHERE: an edge band is a neighbour's shadow, a
# whole-window change is something else entirely.
middle = max(mad(x + w // 3, y + 40, x + 2 * w // 3, y + h - 40)
             for x, y, w, h in boxes)

# The gap BETWEEN them: from the right edge of the left window to the left
# edge of the right one, over the vertical range they share.
gap = mad(lx + lw + 2, top, rx - 2, bot)

print("OK %.3f %.3f %.3f %d" % (inner, middle, gap, rx - (lx + lw)))
MEASURE
)"

set -- $RESULT
if [ "${1:-}" != "OK" ]; then
	bad "the captures could be measured ($RESULT)"
	echo; echo "  $PASS passed, $FAIL failed"; exit 1
fi
INNER=$2; MIDDLE=$3; GAP=$4; GAPW=$5
echo "  ..   turning shadows off changes: inner edge $INNER, window middle $MIDDLE, ${GAPW}px gap $GAP"

# Premise, twice over. There must BE a gap -- with none, there is nowhere for a
# shadow to show and the scene cannot fail -- and the shadows must be visible
# in it.
if [ "$GAPW" -gt 8 ]; then
	ok "the two windows are tiled with a gap between them (${GAPW}px)"
else
	bad "the two windows are tiled with a gap between them (${GAPW}px -- nothing below can fail)"
	echo; echo "  $PASS passed, $FAIL failed"; exit 1
fi
if python3 -c "import sys; sys.exit(0 if $GAP > 3.0 else 1)"; then
	ok "the shadows are visible in the gap between them"
else
	bad "the shadows are visible in the gap between them ($GAP -- they never drew, so the next check proves nothing)"
fi

# The assertion. A window's own shadow is clipped out of its own box, so
# anything that changes along the edge FACING the neighbour came from the
# neighbour's shadow being drawn over this window. Measured at 8.7 levels
# before the fix and 0.0 after.
if python3 -c "import sys; sys.exit(0 if $INNER < 1.0 else 1)"; then
	ok "no shadow reaches across the gap into the next window ($INNER)"
else
	bad "no shadow reaches across the gap into the next window ($INNER levels of change along the inner edge -- a neighbour's shadow is being composited over a tiled window)"
fi

if python3 -c "import sys; sys.exit(0 if $MIDDLE < 1.0 else 1)"; then
	ok "...and nothing changes in the middle of either window ($MIDDLE)"
else
	bad "...and nothing changes in the middle of either window ($MIDDLE)"
fi

# The converse, and the regression a stacking change invites: a floating window
# IS above the tiles, so its shadow must still land on them. Measured on the
# band just below it, which lies on tiled window content rather than wallpaper.
FLOATY="$(python3 - "$OUTDIR/on-floaty.png" "$OUTDIR/off-floaty.png" "$OUTDIR/geom.txt" <<'MEASURE'
import sys
from PIL import Image

A = Image.open(sys.argv[1]).convert("RGB")
B = Image.open(sys.argv[2]).convert("RGB")
rows = [tuple(int(v) for v in line.split())
        for line in open(sys.argv[3]) if line.strip()]
fx, fy, fw, fh = rows[2]

S = A.size[0] / 1920.0
a, b = A.load(), B.load()
W, H = A.size
t = n = 0
for y in range(int((fy + fh + 4) * S), min(H, int((fy + fh + 44) * S)), 2):
    for x in range(int((fx + 60) * S), min(W, int((fx + fw - 60) * S)), 2):
        t += abs(sum(a[x, y]) - sum(b[x, y])) / 3.0
        n += 1
print("%.3f" % (t / n if n else -1.0))
MEASURE
)"
if python3 -c "import sys; sys.exit(0 if $FLOATY > 3.0 else 1)"; then
	ok "a floating window still casts its shadow onto the tiles below ($FLOATY)"
else
	bad "a floating window still casts its shadow onto the tiles below ($FLOATY -- its shadow is being drawn beneath the windows it should darken)"
fi

# And the shadows leave with the windows. Over the WHOLE screen, because a
# shadow left behind on an empty tag could be anywhere -- there is no window to
# measure relative to any more.
EMPTY="$(python3 - "$OUTDIR/on-empty.png" "$OUTDIR/off-empty.png" <<'MEASURE'
import sys
from PIL import Image

A = Image.open(sys.argv[1]).convert("RGB").load()
B = Image.open(sys.argv[2]).convert("RGB")
W, H = B.size
b = B.load()
t = n = 0
for y in range(0, H, 4):
    for x in range(0, W, 4):
        t += abs(sum(A[x, y]) - sum(b[x, y])) / 3.0
        n += 1
print("%.3f" % (t / n if n else -1.0))
MEASURE
)"
if python3 -c "import sys; sys.exit(0 if 0 <= $EMPTY < 1.0 else 1)"; then
	ok "switching to an empty tag takes the shadows with it ($EMPTY)"
else
	bad "switching to an empty tag takes the shadows with it ($EMPTY levels of change on an empty tag -- a hidden window's shadow is still being drawn)"
fi

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
