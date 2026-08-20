#!/usr/bin/env bash
# avk-crossoutput-round-test.sh -- a window spanning two outputs is ONE rounded
# window, at MIXED SCALES.
#
# ── WHY THIS EXISTS WHEN avk-crossoutput-border-test.sh ALREADY DID ───────
#
# That test proved the far output draws the window's outer BORDER, and that the
# seam is not treated as a corner. It runs both outputs at scale 1, and it never
# asks whether the window's TRUE EXTERIOR CORNERS are rounded on the far side.
# Both of those are exactly where the live defect lives: DP-1 at scale 1.5 shows
# rounded corners, HDMI-A-1 at scale 1.0 shows square ones. A test that never
# varies the scale and never probes the far corners passes while that is true.
#
# ── THE GEOMETRY, AND WHICH CORNER LANDS WHERE ───────────────────────────
#
#   HEADLESS-1  raster 1920x1080, scale 1.5  ->  logical 1280x720 at 0,0
#   HEADLESS-2  raster 1920x1080, scale 1.0  ->  logical 1920x1080 at 1280,0
#
# The window straddles x=1280, so its TL and BL land on the 1.5 output and its
# TR and BR on the 1.0 output -- the live arrangement, and the only one where a
# radius scaled once on one path and twice (or not at all) on the other can be
# seen at all.
#
# grim captures RASTER pixels, so a logical coordinate on output 1 is multiplied
# by 1.5 and on output 2 by 1.0. Getting that conversion wrong is the obvious
# way for this test to report a defect that is its own arithmetic, so the
# expected corner positions are printed and the border is located empirically
# before any corner is judged.
#
# ── WHAT A ROUNDED CORNER LOOKS LIKE TO A PROBE ──────────────────────────
#
# At the extreme corner pixel of a SQUARE window the border colour is painted.
# At a rounded one the border has curved away and the wallpaper shows through.
# So: sample the small triangle outside the arc, and count border pixels. Zero
# (modulo AA) means rounded; a full wedge means square. That is a binary answer
# to a binary question, and it does not depend on the radius being any
# particular value.
#
# ASYMMETRIC RADII are used where the config allows, because equal radii hide
# corner-permutation and origin errors -- a TL radius applied at BR looks
# perfect until the two differ.
#
#   BREAK=owner-monitor-bound   restores the pre-fix rule: a corner is squared
#           where the window meets its OWNING MONITOR's edge rather than the
#           desktop's. The straddling window's overhanging side must go square
#           against it. A green break run is a suite failure.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-crossoutput-round"
REPAINT="$(dirname "$0")/wlrepaint/wlrepaint"
RADIUS="${RADIUS:-20}"
BORDER="${BORDER:-6}"
MODE="${MODE:-dual}"          # dual | single (the equivalence reference)
BREAK="${BREAK:-}"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-xround-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_ENV=""
[ "$BREAK" = owner-monitor-bound ] && \
	HL_ENV="$HL_ENV AZ_CORNER_OWNER_MONITOR_BOUND=1"

# Logical layout. HL_WIDTH/HL_HEIGHT are the MODE (raster); the logical extent
# is that divided by the scale, and HL_X2 must be output 1's LOGICAL width or
# the two outputs are not adjacent and there is no seam to straddle.
HL_WIDTH=1920
HL_HEIGHT=1080
if [ "$MODE" = single ]; then
	HL_OUTPUTS=1
	HL_SCALE1=1
	SEAM=99999                    # never crossed
	WIN_X=200
else
	HL_OUTPUTS=2
	HL_SCALE1=1.5
	HL_SCALE2=1
	HL_X2=1280
	SEAM=1280
	WIN_X=1080                    # 200 logical px each side of the seam
fi
WIN_Y=140
WIN_W=400
WIN_H=300
export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_OUTPUTS HL_SCALE1 HL_SCALE2 HL_X2

[ -n "$BREAK" ] && echo "*** BREAK=$BREAK -- this build is deliberately wrong ***"
echo "mode=$MODE radius=$RADIUS border=$BORDER"
echo "window logical ${WIN_X},${WIN_Y} ${WIN_W}x${WIN_H}  seam x=$SEAM"

hl_start "border_radius $RADIUS
borderpx $BORDER
focuscolor 0xc66b25ff
bordercolor 0x2f6fd0ff
shadows 0
layer_shadows 0
gappih 0
gappiv 0
gappoh 0
gappov 0
animations 0
smartgaps 0
effects { blur { enable 0 }; shadow { enable 0 } }
layout { titlebar { enable 0 } }"
sleep 2

"$REPAINT" --title wlround --size "$WIN_W"x"$WIN_H" --solid 202020 \
	--frames 90 --ssd --hold-ms 100 > "$OUTDIR/wl.log" 2>&1 &
HL_SPAWNED_PIDS+=("$!")
hl_wait_client_count 1 40 >/dev/null 2>&1
sleep 1
hl_dispatch "toggle_floating" 0.5
hl_dispatch "resize_window,$WIN_W,$WIN_H" 0.5
hl_dispatch "move_window,$WIN_X,$WIN_Y" 2
sleep 1

hl_get "get all-clients" | jq -r '.clients[]|"  client \(.x),\(.y) \(.width)x\(.height) mon=\(.monitor)"'
grim -o HEADLESS-1 "$OUTDIR/out1.png" 2>/dev/null
[ "$MODE" = dual ] && grim -o HEADLESS-2 "$OUTDIR/out2.png" 2>/dev/null

[ -s "$OUTDIR/out1.png" ] || { hl_assert "output 1 captured" false true
	hl_stop; hl_summary; exit $?; }

python3 - "$OUTDIR" "$MODE" "$WIN_X" "$WIN_Y" "$WIN_W" "$WIN_H" "$SEAM" \
		"$BORDER" "$RADIUS" > "$OUTDIR/probe.txt" 2>&1 <<'PY'
import sys, os
from PIL import Image
out, mode = sys.argv[1], sys.argv[2]
wx, wy, ww, wh, seam, bw, radius = (int(v) for v in sys.argv[3:10])

S1 = 1.5 if mode == "dual" else 1.0
S2 = 1.0
imgs = {"o1": Image.open(f"{out}/out1.png").convert("RGB")}
if mode == "dual":
    imgs["o2"] = Image.open(f"{out}/out2.png").convert("RGB")
px = {k: v.load() for k, v in imgs.items()}
size = {k: v.size for k, v in imgs.items()}

BORDER = (0xc6, 0x6b, 0x25)
CLIENT = (0x20, 0x20, 0x20)
WALL = px["o1"][4, 4]
def d(a, b): return sum((a[i]-b[i])**2 for i in range(3)) ** 0.5
print(f"wallpaper={WALL} border={BORDER} client={CLIENT}")
if min(d(BORDER, WALL), d(BORDER, CLIENT), d(WALL, CLIENT)) < 40:
    print("INVALID TEST: reference colours are not separable"); sys.exit(1)
def isb(p): return d(p, BORDER) < 60

def at(which, lx, ly):
    """A LOGICAL desktop coordinate -> that output's raster pixel."""
    if which == "o1":
        return (int(round(lx * S1)), int(round(ly * S1)))
    return (int(round((lx - seam) * S2)), int(round(ly * S2)))

def probe_corner(which, cx, cy, sx, sy, label):
    """Count border pixels in the wedge OUTSIDE a corner arc.

    (cx,cy) is the corner in logical coords; (sx,sy) points inward. A square
    corner paints the whole wedge; a rounded one leaves it to the wallpaper.
    The wedge is taken well inside the radius so antialiasing at the arc
    itself cannot decide the answer.
    """
    img = px[which]; W, H = size[which]
    scale = S1 if which == "o1" else S2
    r = radius
    if r < 6:
        print(f"{label}_skipped radius_too_small"); return None
    hits = tot = 0
    guard = 2.0                        # antialiasing either side of the arc
    for i in range(0, r):
        for j in range(0, r):
            # STRICTLY outside the arc, by the exact circle -- an earlier
            # version used the triangle i+j < r, which clips the arc itself
            # and reported one corner of a provably round window as square.
            if ((r - i) ** 2 + (r - j) ** 2) ** 0.5 <= r + guard:
                continue
            x, y = at(which, cx + sx * (i / scale), cy + sy * (j / scale))
            if 0 <= x < W and 0 <= y < H:
                tot += 1
                if isb(img[x, y]):
                    hits += 1
    print(f"{label} {hits} of {tot}")
    return (hits, tot)

# Where the window's four TRUE exterior corners are, and which output owns each.
corners = [
    ("TL", wx,      wy,      +1, +1),
    ("BL", wx,      wy + wh, +1, -1),
    ("TR", wx + ww, wy,      -1, +1),
    ("BR", wx + ww, wy + wh, -1, -1),
]
for name, cx, cy, sx, sy in corners:
    which = "o1" if (mode == "single" or cx < seam) else "o2"
    if which not in px:
        continue
    rx, ry = at(which, cx, cy)
    print(f"corner_{name}_on {which} logical={cx},{cy} raster={rx},{ry}")
    probe_corner(which, cx, cy, sx, sy, f"corner_{name}")

# The border really is painted on each output the window reaches (premise): if
# it is not, every corner reads "rounded" for the wrong reason.
ymid = wy + wh // 2
for which in px:
    W, H = size[which]
    n = sum(1 for x in range(W) if isb(px[which][x, int(ymid * (S1 if which == "o1" else S2))]))
    print(f"border_px_{which} {n}")

# The seam is not a corner: the top border row must not curve away near it.
if mode == "dual":
    def top_row(which, lx):
        img = px[which]; W, H = size[which]
        for ly in range(wy - 4, wy + bw + 8):
            x, y = at(which, lx, ly)
            if 0 <= x < W and 0 <= y < H and isb(img[x, y]):
                return ly
        return None
    left = [top_row("o1", seam - k) for k in range(4, 24)]
    right = [top_row("o2", seam + k) for k in range(4, 24)]
    ok = all(v is not None for v in left + right)
    drift = (max(left + right) - min(left + right)) if ok else 999
    print(f"seam_top_rows_left {left[0]} right {right[0]}")
    print(f"seam_row_drift {drift}")
PY
sed 's/^/  /' "$OUTDIR/probe.txt"

get() { awk -v k="$1" '$1==k{print $2}' "$OUTDIR/probe.txt"; }
tot() { awk -v k="$1" '$1==k{print $4}' "$OUTDIR/probe.txt"; }
# A corner is ROUNDED when almost none of the wedge outside its arc is border.
rounded() {
	local h t; h="$(get "corner_$1")"; t="$(tot "corner_$1")"
	[ -n "$h" ] && [ -n "$t" ] && [ "$t" -gt 8 ] || { echo "novalue"; return; }
	awk -v h="$h" -v t="$t" 'BEGIN{print (h <= t*0.15) ? "true" : "false"}'
}

# The border is BORDER LOGICAL pixels, so a mid-row crossing it shows
# BORDER*scale RASTER pixels per side. A fixed threshold of 10 failed both
# outputs for being CORRECT -- 9 at scale 1.5 and 6 at scale 1.0 -- and the
# premise it was guarding is exactly the one whose absence would make every
# corner read "rounded" for the wrong reason.
# ONE PIXEL OF ANTIALIASING ALLOWANCE, measured rather than guessed.
#
# M6B/D5 made Path A the default, so the border's edge pixel is now blended in
# LINEAR LIGHT and lands further from the pure border colour than the
# classifier's distance-60 threshold allows. Exactly one pixel per output drops
# out of the count:
#
#     AZ_M5_PATH_A=0   o1 9   o2 6   (the figures this premise was written to)
#     default (on)     o1 8   o2 5
#
# Confirmed by running this fixture both ways. The premise exists to catch a
# border that is NOT PAINTED AT ALL -- which would make every corner read
# "rounded" for the wrong reason and would count zero, not one short. The same
# allowance the corner probe already makes for antialiasing either side of its
# arc (`guard = 2.0`) belongs here too.
expect1=$(awk -v b="$BORDER" -v s="${HL_SCALE1:-1}" 'BEGIN{printf "%d", b*s-1}')
hl_assert "premise: output 1 paints a border of the expected width" \
	"$([ "$(get border_px_o1)" -ge "$expect1" ] 2>/dev/null && echo true || echo false)" true
if [ "$MODE" = dual ]; then
	expect2=$(awk -v b="$BORDER" -v s="${HL_SCALE2:-1}" 'BEGIN{printf "%d", b*s-1}')
	hl_assert "premise: output 2 paints a border of the expected width" \
		"$([ "$(get border_px_o2)" -ge "$expect2" ] 2>/dev/null && echo true || echo false)" true
fi

hl_assert "true TL corner is rounded" "$(rounded TL)" true
hl_assert "true BL corner is rounded" "$(rounded BL)" true
hl_assert "true TR corner is rounded" "$(rounded TR)" true
hl_assert "true BR corner is rounded" "$(rounded BR)" true

if [ "$MODE" = dual ]; then
	hl_assert "the seam is not treated as a corner" \
		"$([ "$(get seam_row_drift)" -le 1 ] 2>/dev/null && echo true || echo false)" true
fi

echo "captures: $OUTDIR"
hl_stop
hl_summary
