#!/usr/bin/env bash
# avk-gradient-crossoutput-test.sh — M4C.4: one window, two outputs, one ramp.
#
# THE CLAIM
#
#   A window is ONE object. If it spans two outputs, its gradient spans them
#   with it. The output seam is not a gradient boundary.
#
# WHY IT CAN GO WRONG. Each output renders the window separately, and the
# gradient's coordinate is derived from the destination box in THAT output's
# space. If the box handed to the shader were clipped to the output the window
# is currently being drawn on, the ramp would restart at every seam -- each
# output showing a complete gradient of its own. That renders perfectly on a
# single-monitor desk and is wrong the moment a window is dragged across.
#
# HOW IT IS CHECKED. Not by eye and not by a colour count: the top border row
# is sampled straight across the window, and the per-pixel colour STEP is
# compared either side of the seam against the step everywhere else. A
# continuous ramp has a small, even step throughout; a ramp that restarts has
# one enormous discontinuity at exactly the seam.
#
# This fixture only became possible once M4C.3H landed. Before it, setting
# `border_gradient 1` starved the compositor's event loop within seconds and
# neither grim nor amsg would answer.
#
#   BREAK=gradient-noop-damage   the repaint storm; must FAIL (IPC dies)
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-gradient-crossoutput"
BREAK="${BREAK:-}"

REPAINT="$(dirname "$0")/wlrepaint/wlrepaint"
[ -x "$REPAINT" ] || { echo "not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

export HL_OUTPUTS=2
W1="${HL_WIDTH:-1920}"

gt() { ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" timeout 6 amsg $* 2>/dev/null; }

OUTDIR="${TMPDIR:-/tmp}/asteroidz-gradxo-$$"
HL_OUTDIR="$OUTDIR"
HL_ENV=""
[ "$BREAK" = gradient-noop-damage ] && HL_ENV="$HL_ENV AZ_GRADIENT_NOOP_DAMAGE=1"
export HL_OUTDIR HL_ENV

# borderpx 24 so the border is thick enough to sample a clean row through, and
# radius 0 so the corners cannot be confused with a seam artifact.
hl_start "border_radius 0
borderpx 24
focuscolor 0xff0000ff
bordercolor 0x2f6fd0ff
border_gradient 1
border_gradient_angle 0
border_gradient_color2 0x00ff00ff
animations 0
shadows 0
layer_shadows 0
effects { blur { enable 0 } }
layout {
    titlebar { enable 0 }
}"
sleep 2

hl_dispatch "view,4" 0.5
"$REPAINT" --title "wlxo" --size 900x500 --solid 202020 --frames 1 --ssd \
	--hold-ms 100 > "$OUTDIR/client.log" 2>&1 &
HL_SPAWNED_PIDS+=("$!")
for _ in $(seq 1 12); do
	[ "$(gt get all-clients | jq -r '.clients|length' 2>/dev/null)" = 1 ] && break
	sleep 1
done
sleep 2

# Float it and straddle the seam: the window's left half on HEADLESS-1, its
# right half on HEADLESS-2.
gt dispatch toggle_floating >/dev/null
sleep 1
gt dispatch "move_window,$((W1 - 450)),260" >/dev/null
sleep 2

GEOM=$(gt get focused-client | jq -r '"\(.x) \(.y) \(.width) \(.height)"')
echo "  window at $GEOM (seam at x=$W1)"
set -- $GEOM; GX=$1; GY=$2; GW=$3; GH=$4

hl_assert "the window really straddles the seam (premise: $GX..$((GX + GW)) vs $W1)" \
	"$([ "${GX:-0}" -lt "$W1" ] && [ "$((GX + GW))" -gt "$W1" ] && echo true || echo false)" "true"

DRAWS=$(gt get avk-stats | jq -r '.gradient_draws // 0')
hl_assert "a gradient was drawn (premise: $DRAWS draws)" \
	"$([ "${DRAWS:-0}" -gt 0 ] && echo true || echo false)" "true"

grim -o HEADLESS-1 "$OUTDIR/o1.png" 2>/dev/null
grim -o HEADLESS-2 "$OUTDIR/o2.png" 2>/dev/null

# Stitch the two captures side by side and walk the top border row across the
# whole window, reporting the largest per-pixel colour step and where it is.
read -r MAXSTEP AT MEDSTEP SAMPLES SPAN < <(python3 - \
	"$OUTDIR/o1.png" "$OUTDIR/o2.png" "$GX" "$GY" "$GW" "$W1" <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGB'); pa = a.load()
b = Image.open(sys.argv[2]).convert('RGB'); pb = b.load()
gx, gy, gw, seam = (int(v) for v in sys.argv[3:7])
AW, AH = a.size; BW, BH = b.size

def at(x, y):
    """One global x, resolved to whichever output owns it."""
    if x < seam:
        return pa[x, y] if 0 <= x < AW and 0 <= y < AH else None
    xb = x - seam
    return pb[xb, y] if 0 <= xb < BW and 0 <= y < BH else None

# A row a few pixels into the top border, clear of both the client and the
# outer edge.
y = gy + 6
steps = []
prev = None
first = last = None
for x in range(max(0, gx + 4), gx + gw - 4):
    c = at(x, y)
    if c is None:
        continue
    # only border pixels: the ramp runs red -> green, so blue stays low
    if c[2] > 120:
        prev = None
        continue
    if prev is not None:
        steps.append((sum(abs(c[i] - prev[i]) for i in range(3)), x))
    if first is None:
        first = c
    last = c
    prev = c
if not steps:
    print("0 0 0 0 0"); sys.exit(0)
mx, at_x = max(steps)
med = sorted(s for s, _ in steps)[len(steps) // 2]
# End-to-end span. A smooth 900px ramp between two stops a full channel apart
# steps by well under 1 per pixel, so "max step is small" is ALSO true of a
# flat border -- the continuity check cannot tell them apart and needs this
# beside it.
span = sum(abs(last[i] - first[i]) for i in range(3)) if first and last else 0
print(f"{mx} {at_x} {med} {len(steps)} {span}")
PY
)
echo "  border row: $SAMPLES samples, median step $MEDSTEP, max step $MAXSTEP at x=$AT, end-to-end span $SPAN"

hl_assert "the border row was actually sampled across the seam (premise: $SAMPLES samples)" \
	"$([ "${SAMPLES:-0}" -gt 400 ] && echo true || echo false)" "true"
# A ramp that restarted at the seam would jump most of a channel -- the two
# stops are a full 255 apart -- against a median step of ~1. 40 is far above
# any antialiasing or rounding and far below a restart.
hl_assert "no discontinuity at the seam (max step $MAXSTEP at x=$AT, median $MEDSTEP)" \
	"$([ "${MAXSTEP:-999}" -lt 40 ] && echo true || echo false)" "true"
# The premise the continuity check cannot supply. Two stops a full channel
# apart, swept across the window, must land far apart end to end.
hl_assert "and the row really is a RAMP, not a flat fill (end-to-end span $SPAN)" \
	"$([ "${SPAN:-0}" -gt 300 ] && echo true || echo false)" "true"

hl_stop
hl_summary
