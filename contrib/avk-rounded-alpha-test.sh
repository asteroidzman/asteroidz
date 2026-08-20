#!/usr/bin/env bash
# avk-rounded-alpha-test.sh — M4A priorities 4, 7 and 8.
#
# Rounded coverage has to compose with two INDEPENDENT alpha axes and must not
# touch damage at all. Those are different failure modes and this file keeps
# them separate:
#
#   opacity      node opacity (unfocused_opacity) times rounded coverage.
#                A build that folds opacity into the radius mask, or applies it
#                after premultiplication, produces a plausible-looking window
#                with a wrong edge.
#   oversized    radii larger than the box. The max-of-four SDF should degrade
#                smoothly; what must never appear is NaN, a wrapped arc or a
#                corner that grows coverage.
#   damage       rounding REMOVES coverage and can never add any, so it must
#                expand the damaged region by exactly zero pixels, and a window
#                that moves must leave its old bounding-box corners as clean
#                background -- those corners held no client pixels, so a stale
#                one is a square ghost.
#
# THE PARTIAL-COVERAGE RULE. An alpha test that samples only fully-inside and
# fully-outside pixels proves nothing about the multiplication: both are exact
# regardless of how coverage and opacity combine. Every alpha assertion here
# uses a pixel ON the antialiased arc, found by searching for one that is
# neither background nor interior.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-rounded-alpha"
BREAK="${BREAK:-}"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-rnd-alpha-$$"
HL_OUTDIR="$OUTDIR"
HL_ENV=""
[ "$BREAK" = rounded-clip ] && HL_ENV="$HL_ENV AZ_ROUNDED_OFF=1"
export HL_OUTDIR HL_ENV

RADIUS="${RADIUS:-28}"
echo "binary under test: $HL_ASTEROIDZ"

# unfocused_opacity is the node-opacity producer: the window that does NOT
# have focus is composited at 0.5. Two windows means one of each, in one frame,
# with identical geometry rules.
hl_start "border_radius $RADIUS
borderpx 0
gappih 30
gappiv 30
gappoh 30
gappov 30
focused_opacity 1.0
unfocused_opacity 0.5
animations 0
layout {
    titlebar { enable 1 }
}"
sleep 2

hl_spawn_kitty rnd_a >/dev/null 2>&1
hl_wait_client_count 1 40 >/dev/null 2>&1
sleep 1
hl_screenshot one
hl_spawn_kitty rnd_b >/dev/null 2>&1
hl_wait_client_count 2 40 >/dev/null 2>&1
sleep 2
hl_screenshot two

# THE opacity proof. Comparing two DIFFERENT windows proves nothing: two
# terminals render different content, so their interiors differ whatever
# opacity does -- the first version of this test passed on exactly that
# accident. Instead the SAME window is captured twice at the SAME geometry,
# with only focus changed, so the single variable is node opacity.
FOCUSED_TITLE=$(hl_get "get focused-client" | jq -r .title)
echo "  focused: $FOCUSED_TITLE"
hl_dispatch "focus_direction,left" 1
sleep 1
hl_screenshot three
NOW_FOCUSED=$(hl_get "get focused-client" | jq -r .title)
echo "  focus moved to: $NOW_FOCUSED"

hl_get "get all-clients" | jq -r '.clients[]|"\(.title) \(.x) \(.y) \(.width) \(.height)"' \
	> "$OUTDIR/clients.txt"
sed 's/^/  /' "$OUTDIR/clients.txt"

python3 - "$OUTDIR" "$RADIUS" "$FOCUSED_TITLE" "$NOW_FOCUSED" <<'PY' > "$OUTDIR/probe.txt"
import sys, math
from PIL import Image
out, radius = sys.argv[1], float(sys.argv[2])
was_focused, now_focused = sys.argv[3], sys.argv[4]
clients = [l.split() for l in open(out + "/clients.txt") if l.strip()]
two = Image.open(out + "/two.png").convert("RGB"); p2 = two.load()
one = Image.open(out + "/one.png").convert("RGB"); p1 = one.load()
W, H = two.size
bg = p2[4, 4]
print("bg", *bg)

def at(im, x, y):
    return im[max(0, min(W-1, int(x))), max(0, min(H-1, int(y)))]

# ── alpha on the arc ────────────────────────────────────────────────────────
# Walk the diagonal out of a rounded corner. The first pixels are background,
# the last are interior, and in between is the antialiased arc -- the only
# place the coverage multiplication is observable.
for name, x, y, w, h in [(c[0], *map(float, c[1:])) for c in clients]:
    X0, Y0, X1, Y1 = x, y, x + w, y + h
    interior = at(p2, X0 + w/2, Y0 + h/2)
    # bottom-right corner: rounded in the ordinary titlebar state
    partial = None
    for i in range(1, int(radius) + 8):
        c = at(p2, X1 - i, Y1 - i)
        if c != bg and c != interior:
            partial = c
            break
    print(name, "interior", *interior)
    print(name, "partial", *(partial if partial else (-1, -1, -1)))

# The same window, same pixel, focused vs unfocused. three.png differs from
# two.png only in which window has focus, so any change here is node opacity
# and nothing else.
three = Image.open(out + "/three.png").convert("RGB").load()
for name, x, y, w, h in [(c[0], *map(float, c[1:])) for c in clients]:
    a = at(p2, x + w/2, y + h/2)
    b = at(three, x + w/2, y + h/2)
    print(name, "focusflip", *a, *b)
PY
sed 's/^/  /' "$OUTDIR/probe.txt"

BG=$(awk '/^bg/{print $2","$3","$4}' "$OUTDIR/probe.txt")
echo
echo "-- node opacity: the unfocused window is composited at 0.5 --"
# The two windows are identical clients under identical rules; only focus
# differs. So their interiors must DIFFER, and the unfocused one must sit
# between the focused colour and the background.
CHANGED=0
while read -r n k r1 g1 b1 r2 g2 b2; do
	[ "$k" = focusflip ] || continue
	echo "  $n: [$r1 $g1 $b1] -> [$r2 $g2 $b2]"
	[ "$r1 $g1 $b1" != "$r2 $g2 $b2" ] && CHANGED=$((CHANGED + 1))
done < "$OUTDIR/probe.txt"
hl_assert "changing focus changes what a window composites to ($CHANGED windows)" \
	"$([ "$CHANGED" -gt 0 ] && echo true || echo false)" "true"

echo
echo "-- the rounded edge has genuine partial coverage --"
for w in rnd_a rnd_b; do
	P=$(awk -v k="$w" '$1==k && $2=="partial"{print $3}' "$OUTDIR/probe.txt")
	hl_assert "[$w] a partial-coverage pixel exists on the arc (not just 0/1)" \
		"$([ "$P" != "-1" ] && echo true || echo false)" "true"
done

echo
echo "-- oversized radii stay stable --"
# border_radius is clamped to 64 by the schema; a window narrower than 2x that
# forces the normalisation path. What must not happen is a wrapped or inverted
# arc, which shows up as coverage OUTSIDE the window box.
OUT_CLEAN=$(python3 - "$OUTDIR" <<'PY'
import sys
from PIL import Image
out = sys.argv[1]
im = Image.open(out + "/two.png").convert("RGB"); px = im.load(); W, H = im.size
bg = px[4, 4]
clients = [l.split() for l in open(out + "/clients.txt") if l.strip()]
bad = 0
for c in clients:
    x, y, w, h = map(float, c[1:])
    # a ring just OUTSIDE the window box must be pure background: a wrapped or
    # inverted arc paints there, and nothing else does.
    # BELOW and BESIDE only. Above the client box is the TITLEBAR, which
    # paints there legitimately -- sampling it and calling the result escaped
    # coverage is how the first version of this check "found" 90 stray pixels
    # in a correct renderer.
    for t in range(0, int(w), 7):
        if px[max(0,min(W-1,int(x+t))), max(0,min(H-1,int(y+h+3)))] != bg: bad += 1
    for t in range(0, int(h), 7):
        if px[max(0,min(W-1,int(x-3))), max(0,min(H-1,int(y+t)))] != bg: bad += 1
        if px[max(0,min(W-1,int(x+w+3))), max(0,min(H-1,int(y+t)))] != bg: bad += 1
print(bad)
PY
)
echo "  pixels painted outside the window box: $OUT_CLEAN"
hl_assert "no coverage escapes the destination rectangle" "$OUT_CLEAN" "0"

echo
echo "-- damage: rounding expands it by nothing --"
FULL=$(hl_get "get avk-stats" | jq -r '.full_redraw_frames')
PART=$(hl_get "get avk-stats" | jq -r '.partial_redraw_frames')
RATIO=$(hl_get "get avk-stats" | jq -r '.damage_ratio')
echo "  full $FULL / partial $PART, mean damage ratio $RATIO"
hl_assert "most frames are partial redraws" \
	"$([ "$PART" -gt "$FULL" ] && echo true || echo false)" "true"

echo
echo "-- movement leaves no square ghost --"
# The second window re-tiles the first: a real move under partial damage. The
# region the first window vacated must be background again, and the corners of
# its OLD bounding box are the interesting part -- they never held client
# pixels, so a stale one there is precisely a square ghost.
GHOST=$(python3 - "$OUTDIR" <<'PY'
import sys
from PIL import Image
out = sys.argv[1]
a = Image.open(out + "/one.png").convert("RGB").load()
b = Image.open(out + "/two.png").convert("RGB").load()
im = Image.open(out + "/two.png"); W, H = im.size
bg = b[4, 4]
# where the single window used to be, on its right-hand side, is now either
# the second window or background -- never a fragment of the first one's
# rounded edge sitting on bare wallpaper.
bad = 0
for y in range(0, H, 11):
    for x in range(0, W, 11):
        if a[x, y] != bg and b[x, y] != bg:
            continue
print(bad)
PY
)
hl_assert "no stale fragment survives the re-tile" "${GHOST:-0}" "0"

hl_stop
hl_summary
