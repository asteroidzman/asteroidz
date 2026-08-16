#!/usr/bin/env bash
# avk-crossoutput-border-test.sh — M4B.1: a window that spans two outputs keeps
# its border on both of them.
#
# THE DEFECT THIS PINS DOWN
#
# A client's surface and its decorations were cropped to the owning monitor by
# two INDEPENDENT rules. The surface was cropped only for scroll-tiled windows
# and tag animations (clip_to_hide); the border, shadow, split indicator and
# blur backdrop were cropped ALWAYS, with a `c == grabc` escape hatch.
#
# For the case the cropping was written for -- a scroller column scrolled past
# its own monitor's edge, which must not paint onto a physically adjacent
# output (de0b5c5) -- the two rules agree.
#
# For an ORDINARY FLOATING WINDOW straddling a monitor seam they do not. The
# surface is not cropped, so the client is visible on both outputs; the border
# is, so the far output showed bare client with NO decoration at all -- not
# even the window's real right-hand outer edge, which lives over there. And
# because the escape hatch was `c == grabc`, the border was whole while the
# mouse button was down and lost its far half the instant you released, with no
# change in geometry whatsoever.
#
# WHAT IS ASSERTED, AND WHY THE RIGHT EDGE IS THE POINT
#
# The window is placed so its RIGHT OUTER BORDER falls on the second output.
# Everything else about this bug is arguable as a clipping preference; a window
# whose actual outer edge simply is not drawn is not. That assertion is the one
# that failed before the fix and the one BREAK=border-owner-monitor-clip
# restores.
#
# The monitor seam is NOT a window corner. Along the seam the top and bottom
# edges must run straight and continuous across both outputs, and no rounding
# may appear there -- a fix that clipped the ring per output and re-rounded it
# at the boundary would look plausible and be wrong.
#
#   BREAK=border-owner-monitor-clip   restores the old policy. The far-output
#           assertions must fail against it; a green run is a suite failure.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-crossoutput-border"
BREAK="${BREAK:-}"
RADIUS="${RADIUS:-40}"
BORDER="${BORDER:-6}"

REPAINT="$(dirname "$0")/wlrepaint/wlrepaint"
[ -x "$REPAINT" ] || { echo "not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

# HEADLESS-1 spans x 0..HL_WIDTH, HEADLESS-2 sits immediately to its right.
export HL_OUTPUTS=2
W1="${HL_WIDTH:-1920}"

# The window straddles the seam with its RIGHT outer edge on output 2.
WIN_W=900; WIN_H=620
WIN_X=$((W1 - 420))        # 420px on output 1 ...
WIN_Y=300
RIGHT_EDGE=$((WIN_X + WIN_W))          # ... and 480px on output 2
RIGHT_LOCAL=$((RIGHT_EDGE - W1))       # its right outer border, output-2 local

OUTDIR="${TMPDIR:-/tmp}/asteroidz-xout-$$"
HL_OUTDIR="$OUTDIR"
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = border-owner-monitor-clip ] && \
	HL_ENV="$HL_ENV AZ_BORDER_OWNER_MONITOR_CLIP=1"
export HL_OUTDIR HL_ENV

[ -n "$BREAK" ] && echo "*** BREAK=$BREAK -- this build is deliberately wrong ***"
echo "radius=$RADIUS border=$BORDER"
echo "outputs: HEADLESS-1 0..$W1, HEADLESS-2 $W1.."
echo "window: $WIN_X,$WIN_Y ${WIN_W}x${WIN_H} -> right outer edge at $RIGHT_EDGE (output-2 local $RIGHT_LOCAL)"

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
effects { blur { enable 0 } }
layout {
    titlebar { enable 0 }
}"
sleep 2

"$REPAINT" --title wlxout --size "$WIN_W"x"$WIN_H" --solid 202020 \
	--frames 90 --ssd --hold-ms 100 > "$OUTDIR/wl.log" 2>&1 &
HL_SPAWNED_PIDS+=("$!")
hl_wait_client_count 1 40 >/dev/null 2>&1
sleep 1
hl_dispatch "toggle_floating" 0.5
hl_dispatch "resize_window,$WIN_W,$WIN_H" 0.5
hl_dispatch "move_window,$WIN_X,$WIN_Y" 2

settled=false
for _ in $(seq 1 40); do
	grep -q "settled" "$OUTDIR/wl.log" 2>/dev/null && { settled=true; break; }
	sleep 0.25
done
sleep 1

hl_get "get all-clients" | jq -r '.clients[]|"  client \(.title) \(.x),\(.y) \(.width)x\(.height) mon=\(.monitor)"'
grim -o HEADLESS-1 "$OUTDIR/out1.png" 2>/dev/null
grim -o HEADLESS-2 "$OUTDIR/out2.png" 2>/dev/null

hl_assert "the client settled before the capture" "$settled" "true"
[ -s "$OUTDIR/out1.png" ] && [ -s "$OUTDIR/out2.png" ] || {
	hl_assert "both outputs captured" "false" "true"; hl_stop; hl_summary; exit $?; }

python3 - "$OUTDIR" "$WIN_X" "$WIN_Y" "$WIN_W" "$WIN_H" "$W1" "$BORDER" "$RADIUS" \
	> "$OUTDIR/probe.txt" 2>&1 <<'PY'
import sys
from PIL import Image
out = sys.argv[1]
wx, wy, ww, wh, w1, bw, radius = (int(v) for v in sys.argv[2:9])
o1 = Image.open(f"{out}/out1.png").convert('RGB').load()
o2 = Image.open(f"{out}/out2.png").convert('RGB').load()

def d(a, b): return sum((a[i]-b[i])**2 for i in range(3)) ** 0.5
WALL = o1[4, 4]
CLIENT = (0x20, 0x20, 0x20)
# The border colour is the CONFIGURED focuscolor, not a sample. Reading it off
# output 1 made the whole probe abort under the break -- which removes the
# border from exactly that output -- so every assertion failed together and
# said nothing about which one. With a fixed palette each side is judged on its
# own, and the break's signature becomes readable: client present, border gone.
BORDER = (0xc6, 0x6b, 0x25)
print(f"wallpaper={WALL} border={BORDER} client={CLIENT}")
if min(d(BORDER, WALL), d(BORDER, CLIENT), d(WALL, CLIENT)) < 40:
    print("INVALID TEST: the three reference colours are not separable")
    sys.exit(1)

def isb(p): return d(p, BORDER) < 40
def isc(p): return d(p, CLIENT) < 40

ymid = wy + wh // 2
painted = sum(1 for x in range(0, w1) if isb(o1[x, ymid])) + \
          sum(1 for x in range(0, w1) if isb(o2[x, ymid]))
print(f"border_painted_anywhere {painted}")
# ── output 1: the left outer border, and client running to the seam ──────────
left_border = sum(1 for x in range(wx, wx + bw) if isb(o1[x, ymid]))
client_to_seam = sum(1 for x in range(wx + bw + 2, w1) if isc(o1[x, ymid]))
print(f"o1_left_border {left_border} of {bw}")
print(f"o1_client_to_seam {client_to_seam} of {w1 - wx - bw - 2}")

# ── output 2: client continues, and THE RIGHT OUTER BORDER IS THERE ──────────
right_local = wx + ww - w1
client_from_seam = sum(1 for x in range(0, right_local - bw - 2) if isc(o2[x, ymid]))
right_border = sum(1 for x in range(right_local - bw, right_local) if isb(o2[x, ymid]))
print(f"o2_client_from_seam {client_from_seam} of {right_local - bw - 2}")
print(f"o2_right_border {right_border} of {bw}")

# ── the seam is not a corner: top and bottom edges continuous across it ──────
# Sample the top border a little inside each output's side of the seam; both
# must be border, and at the same distance from the window's top edge.
def top_edge_row(img, x):
    for y in range(wy - 3, wy + bw + 6):
        if isb(img[x, y]):
            return y
    return None
t_left = top_edge_row(o1, w1 - 30)
t_right = top_edge_row(o2, 30)
b_left = None
for y in range(wy + wh + 3, wy + wh - bw - 6, -1):
    if isb(o1[w1 - 30, y]): b_left = y; break
b_right = None
for y in range(wy + wh + 3, wy + wh - bw - 6, -1):
    if isb(o2[30, y]): b_right = y; break
print(f"seam_top_left {t_left} seam_top_right {t_right}")
print(f"seam_bottom_left {b_left} seam_bottom_right {b_right}")
same_top = (t_left is not None and t_right is not None and abs(t_left - t_right) <= 1)
same_bottom = (b_left is not None and b_right is not None and abs(b_left - b_right) <= 1)
print(f"seam_continuous {'true' if same_top and same_bottom else 'false'}")

# No artificial rounding AT the seam: along the last/first 20 columns either
# side of it, the top border row must not move (a corner would curve it away).
def row_drift(img, xs):
    rows = [top_edge_row(img, x) for x in xs]
    rows = [r for r in rows if r is not None]
    return (max(rows) - min(rows)) if len(rows) == len(list(xs)) else 999
drift = max(row_drift(o1, range(w1 - 20, w1)), row_drift(o2, range(0, 20)))
print(f"seam_row_drift {drift}")
PY
sed 's/^/  /' "$OUTDIR/probe.txt"

get() { awk -v k="$1" '$1==k{print $2}' "$OUTDIR/probe.txt"; }
ge() { v="${1:-}"; [ -n "$v" ] || v=0; [ "$v" -ge "$2" ] && echo true || echo false; }

hl_assert "a border is painted at all (premise)" \
	"$(ge "$(get border_painted_anywhere)" 1)" "true"
hl_assert "output 1 draws the left outer border" \
	"$(ge "$(get o1_left_border)" "$((BORDER - 1))")" "true"
hl_assert "output 1 shows the client right up to the seam" \
	"$(ge "$(get o1_client_to_seam)" 300)" "true"
hl_assert "output 2 shows the client continuing past the seam" \
	"$(ge "$(get o2_client_from_seam)" 300)" "true"
# THE OLD FAILURE.
hl_assert "output 2 draws the window's RIGHT outer border" \
	"$(ge "$(get o2_right_border)" "$((BORDER - 1))")" "true"
hl_assert "the top/bottom edges run continuous across the seam" \
	"$(get seam_continuous)" "true"
hl_assert "no rounding appears at the seam (it is not a corner)" \
	"$(ge 1 "$(get seam_row_drift)")" "true"

# ── DRAG / RELEASE: the border must not change when the button comes up ─────
#
# The old policy's escape hatch was `c == grabc`, so the ring was whole while
# the mouse was down and lost its far half the instant you let go -- with no
# geometry change at all. That is the invariant this pins: same geometry, same
# pixels, button or no button.
hl_sync_pointer_extent 2>/dev/null || true
PX=$((WIN_X + WIN_W / 2))
PY_=$((WIN_Y + 20))
hl_super_drag_hold "$PX" "$PY_" "$((PX + 4))" "$PY_" 2500 &
DRAGPID=$!
sleep 1.4
grim -o HEADLESS-1 "$OUTDIR/drag1.png" 2>/dev/null
grim -o HEADLESS-2 "$OUTDIR/drag2.png" 2>/dev/null
wait $DRAGPID 2>/dev/null || true
sleep 1.5
# Nudge a re-layout at the SAME geometry. Releasing the button does not by
# itself recompute the decoration -- apply_border() has to run again -- so a
# capture taken straight after the release shows the drag-time border on a
# broken build too, and the assertion passes for the wrong reason. It did:
# this pair was green against BREAK=border-owner-monitor-clip until the nudge
# was added. Geometry is unchanged, so the border must be unchanged.
REL_X=$(hl_get "get all-clients" | jq -r '.clients[]|select(.title=="wlxout")|.x')
REL_Y=$(hl_get "get all-clients" | jq -r '.clients[]|select(.title=="wlxout")|.y')
hl_dispatch "move_window,${REL_X:-$WIN_X},${REL_Y:-$WIN_Y}" 1.5
hl_get "get all-clients" | jq -r '.clients[]|"  after release+relayout \(.title) \(.x),\(.y) \(.width)x\(.height)"'
grim -o HEADLESS-1 "$OUTDIR/rel1.png" 2>/dev/null
grim -o HEADLESS-2 "$OUTDIR/rel2.png" 2>/dev/null

python3 - "$OUTDIR" > "$OUTDIR/drag.txt" 2>&1 <<'PY2'
import sys
from PIL import Image
out = sys.argv[1]
BORDER = (0xc6, 0x6b, 0x25)
def d(a, b): return sum((a[i]-b[i])**2 for i in range(3)) ** 0.5
def border_count(path):
    try:
        im = Image.open(path).convert('RGB'); px = im.load(); W, H = im.size
    except Exception:
        return None
    return sum(1 for y in range(0, H, 2) for x in range(0, W, 2)
               if d(px[x, y], BORDER) < 40)
for tag in ("drag", "rel"):
    a, b = border_count(f"{out}/{tag}1.png"), border_count(f"{out}/{tag}2.png")
    print(f"{tag}_border_o1 {a}")
    print(f"{tag}_border_o2 {b}")
PY2
sed 's/^/  /' "$OUTDIR/drag.txt"
d1=$(awk '$1=="drag_border_o1"{print $2}' "$OUTDIR/drag.txt")
d2=$(awk '$1=="drag_border_o2"{print $2}' "$OUTDIR/drag.txt")
r1=$(awk '$1=="rel_border_o1"{print $2}' "$OUTDIR/drag.txt")
r2=$(awk '$1=="rel_border_o2"{print $2}' "$OUTDIR/drag.txt")
# Premise: the drag capture caught a real held state with a border on BOTH
# outputs; otherwise "unchanged" could mean "nothing was there either time".
hl_assert "the held-drag capture caught a border on both outputs (premise)" \
	"$([ "${d1:-0}" -gt 50 ] && [ "${d2:-0}" -gt 50 ] && echo true || echo false)" "true"
within() { a=${1:-0}; b=${2:-0}; t=$3; dd=$((a-b)); [ $dd -lt 0 ] && dd=$((-dd));
	[ "$dd" -le "$t" ] && echo true || echo false; }
hl_assert "output 1's border is unchanged by releasing the drag" \
	"$(within "$d1" "$r1" 40)" "true"
hl_assert "output 2's border is unchanged by releasing the drag" \
	"$(within "$d2" "$r2" 40)" "true"

# ── DAMAGE: a cross-output border must not cost a full-output redraw ────────
XF=$(hl_get "get avk-stats" | jq -r '.full_redraw_frames')
XP=$(hl_get "get avk-stats" | jq -r '.partial_redraw_frames')
echo "  redraws so far: full=$XF partial=$XP"
hl_assert "a window spanning two outputs still redraws partially" \
	"$([ "${XP:-0}" -gt "${XF:-0}" ] && echo true || echo false)" "true"
hl_assert "no CPU sync wait was introduced" \
	"$(hl_get "get avk-stats" | jq -r '.cpu_sync_waits')" "0"
hl_assert "no fallback frame was introduced" \
	"$(hl_get "get avk-stats" | jq -r '.fallback_frames')" "0"
hl_assert "no target-state violation was introduced" \
	"$(hl_get "get avk-stats" | jq -r '.target_state_violations')" "0"

# ── SCROLLER PRESERVATION: the case the cropping was written for ────────────
#
# de0b5c5 clips a scroll-tiled window's blur (and the border before it) to its
# owning monitor because a scroller column scrolled past the edge would
# otherwise paint onto a physically adjacent output. The floating fix must not
# touch that: for a scroll-tiled overflow, surface AND decoration stay cropped.
#
# This is the assertion that makes the fix narrow rather than "stop clipping".
echo
echo "--- scroller overflow must STILL be cropped to its own output ---"
hl_dispatch "view,4" 0.5
hl_dispatch "set_layout,scroller" 1
for i in 1 2 3 4 5; do
	"$REPAINT" --title "wlsc$i" --size 700x500 --solid 202020 --frames 60 \
		--ssd --hold-ms 100 > "$OUTDIR/sc$i.log" 2>&1 &
	HL_SPAWNED_PIDS+=("$!")
	sleep 0.6
done
hl_wait_client_count 5 60 >/dev/null 2>&1
sleep 3
hl_get "get all-clients" | jq -r '.clients[]|select(.title|startswith("wlsc"))|"  \(.title) \(.x),\(.y) \(.width)x\(.height) mon=\(.monitor)"'
grim -o HEADLESS-1 "$OUTDIR/sc1.png" 2>/dev/null
grim -o HEADLESS-2 "$OUTDIR/sc2.png" 2>/dev/null

python3 - "$OUTDIR" "$W1" > "$OUTDIR/scroll.txt" 2>&1 <<'PY2'
import sys
from PIL import Image
out, w1 = sys.argv[1], int(sys.argv[2])
BORDER = (0xc6, 0x6b, 0x25); CLIENT = (0x20, 0x20, 0x20)
def d(a,b): return sum((a[i]-b[i])**2 for i in range(3))**0.5
def counts(path):
    im = Image.open(path).convert('RGB'); px = im.load(); W,H = im.size
    b = sum(1 for y in range(0,H,3) for x in range(0,W,3) if d(px[x,y],BORDER)<40)
    c = sum(1 for y in range(0,H,3) for x in range(0,W,3) if d(px[x,y],CLIENT)<40)
    return b, c
b1, c1 = counts(f"{out}/sc1.png")
b2, c2 = counts(f"{out}/sc2.png")
print(f"scroller_o1_border {b1}")
print(f"scroller_o1_client {c1}")
print(f"scroller_o2_border {b2}")
print(f"scroller_o2_client {c2}")
PY2
sed 's/^/  /' "$OUTDIR/scroll.txt"
# WHICH output owns the row is not assumed: the scroller lands wherever focus
# left it, and columns overflow whichever way the layout scrolls. Assuming
# output 1 owned it made this section fail against a CORRECT build -- the row
# was on output 2, overflowing leftwards, and output 1 was clean.
SC_MON=$(hl_get "get all-clients" | jq -r '.clients[]|select(.title=="wlsc1")|.monitor')
if [ "$SC_MON" = "HEADLESS-2" ]; then
	own_b=$(awk '$1=="scroller_o2_border"{print $2}' "$OUTDIR/scroll.txt")
	oth_b=$(awk '$1=="scroller_o1_border"{print $2}' "$OUTDIR/scroll.txt")
	oth_c=$(awk '$1=="scroller_o1_client"{print $2}' "$OUTDIR/scroll.txt")
else
	own_b=$(awk '$1=="scroller_o1_border"{print $2}' "$OUTDIR/scroll.txt")
	oth_b=$(awk '$1=="scroller_o2_border"{print $2}' "$OUTDIR/scroll.txt")
	oth_c=$(awk '$1=="scroller_o2_client"{print $2}' "$OUTDIR/scroll.txt")
fi
echo "  scroller owned by $SC_MON; own_border=$own_b other_border=$oth_b other_client=$oth_c"
hl_assert "the scroller row is drawn on its own output (premise)" \
	"$([ "${own_b:-0}" -gt 100 ] && echo true || echo false)" "true"
hl_assert "no scroller border bleeds onto the neighbouring output" \
	"$([ "${oth_b:-0}" -lt 20 ] && echo true || echo false)" "true"
hl_assert "no scroller client bleeds onto the neighbouring output" \
	"$([ "${oth_c:-0}" -lt 200 ] && echo true || echo false)" "true"

hl_stop
hl_summary
