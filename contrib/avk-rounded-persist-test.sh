#!/usr/bin/env bash
# avk-rounded-persist-test.sh — M4A: what a rounded corner leaves behind.
#
# THE GAP THIS FILLS
#
# Every earlier M4A test asks "is this corner clipped correctly in this frame?"
# A large-radius artifact found on a real desktop is not that question. It
# FLICKERS, which means it is about what PERSISTS between frames: a pixel the
# window does not cover, whose background stops being repainted.
#
# The movement test does not cover it either -- that moves the window and checks
# the vacated area, which damages everything. Here the window is STATIONARY, is
# quiet after it settles, and the content BEHIND its transparent corner changes
# continuously. If the corner region is excluded from damage it keeps showing
# the previous frame while everything around it moves on.
#
# WHY THE PALETTE IS THE MEASUREMENT
#
# contrib/wlrepaint alternates every pixel between two four-colour checkers:
#
#     generation even ("A")   red    green
#     generation odd  ("B")   blue   yellow
#
# One background surface means one buffer means ONE parity visible at a time
# across the whole background. So a stale pixel does not need two screenshots
# and a theory about when the flip happened -- in a SINGLE capture, a background
# pixel of the wrong parity is displaying an older frame, and there is no other
# way for that colour to be there. Every bad pixel is then classified rather
# than counted: STALE, WRONG-FOREGROUND, or AA.
#
# WHY THE OPAQUE REGION IS NOT THE SUSPECT
#
# SceneFX's create_corner_location_region() subtracts a full r-by-r SQUARE at
# each corner from the opaque region, and the quarter-disc AVK renders is
# strictly inside that square. It under-claims opacity: the background gets MORE
# damage than needed, never less, and normalisation only shrinks radii so the
# arc can never escape the square. That disagreement runs the safe way and
# cannot strand a pixel. Disproven at the source; do not re-chase it.
#
# WHAT IT FOUND
#
#   AVK  border 0    clean       GLES border 0    clean
#   AVK  border 6    104 px      GLES border 6    clean
#
# 104 background pixels per corner INSIDE the outer arc, all of them showing the
# CURRENT generation, under genuinely partial damage (5 full frames to 191
# partial) and unchanged under forced full redraw. So: not persistence,
# not shared with SceneFX, and not the rounded client path. AVK cuts the
# border's inner edge as a square (ignoring clipped_region.corners, which the
# compositor fills in per corner) where SceneFX cuts it rounded, and the wedge
# between the two arcs is painted by nobody. That is the live artifact, and it
# belongs to M4B.
#
# WHAT THE KNOBS ARE FOR
#
#   BORDER=0    rounded CLIENT coverage alone -- M4A's own responsibility.
#               This is the default and it passes.
#   BORDER=6    adds the border. FAILS TODAY, deliberately: it is the first M4B
#               regression and is not part of the green suite. Do not make it
#               pass by weakening what it expects.
#   FULLDRAW=1  redraw every frame whole. A DIAGNOSTIC, never a fix: it
#               separates "the geometry is wrong" from "the damage is wrong".
#   ENGINE=gles the SceneFX/GLES path, to decide whether any of this predates
#               AVK at all.
#   BREAK=damage-hole   AZ_AVK_DAMAGE_HOLE over the window's top-left corner:
#               a region acknowledged and never redrawn. This fixture MUST fail
#               against it, and a green run of it is a suite failure.
#   BREAK=rounded-clip  AZ_ROUNDED_OFF: no rounding at all, so the corner boxes
#               hold no background and the arc premise must fail.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-rounded-persist"
BREAK="${BREAK:-}"
RADIUS="${RADIUS:-40}"
BORDER="${BORDER:-0}"
FULLDRAW="${FULLDRAW:-0}"
ENGINE="${ENGINE:-avk}"
SHOTS="${SHOTS:-6}"
FG_W="${FG_W:-700}"
FG_H="${FG_H:-500}"
FG_X="${FG_X:-500}"
FG_Y="${FG_Y:-260}"
# The background must strictly contain the foreground plus one radius of margin
# on every side -- that margin is where the scene's current generation is read
# from -- and must be strictly smaller than the output, so its damage is
# partial. 400,200 1000x700 contains 500,260 700x500 with 60px to spare.
BG_W="${BG_W:-1000}"
BG_H="${BG_H:-700}"
BG_X="${BG_X:-400}"
BG_Y="${BG_Y:-200}"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-persist-$$"
HL_OUTDIR="$OUTDIR"
if [ "$ENGINE" = gles ]; then
	HL_ENV="ASTEROIDZ_RENDERER=wlr"
else
	HL_ENV="ASTEROIDZ_RENDERER=avk"
fi
case "$BREAK" in
	rounded-clip)
		HL_ENV="$HL_ENV AZ_ROUNDED_OFF=1" ;;
	# The break this fixture exists to be checked against. It punches the
	# window's top-left corner box out of every frame's damage, so that region
	# is acknowledged and never redrawn -- a region wrongly believed current,
	# which is the exact shape of the bug being looked for. Without a run
	# against this, "0 stale pixels" only says the detector never fired.
	damage-hole)
		HL_ENV="$HL_ENV AZ_AVK_DAMAGE_HOLE=$FG_X,$FG_Y,$RADIUS,$RADIUS" ;;
esac
[ "$FULLDRAW" = 1 ] && HL_ENV="$HL_ENV AZ_AVK_FULL_DAMAGE=1"
export HL_OUTDIR HL_ENV
[ -n "$BREAK" ] && echo "*** BREAK=$BREAK -- this build is deliberately wrong ***"

REPAINT="$HL_REPO/contrib/wlrepaint/wlrepaint"
[ -x "$REPAINT" ] || { echo "not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

echo "binary: $HL_ASTEROIDZ"
echo "engine=$ENGINE radius=$RADIUS border=$BORDER fulldraw=$FULLDRAW break=${BREAK:-none}"
if [ "$FULLDRAW" = 1 ] && [ "$ENGINE" = gles ]; then
	echo "  note: AZ_AVK_FULL_DAMAGE is an AVK switch and does nothing here"
fi

# Shadows and blur OFF, deliberately. Both paint OUTSIDE and AROUND a window's
# box, so either one turns "which pixels near the corner are background" into a
# judgement call -- and the GLES arm would render them where AVK skips them,
# making the two engines incomparable for reasons that have nothing to do with
# rounding. The titlebar is off so the top corners belong to the CLIENT node,
# which is the node under test.
hl_start "border_radius $RADIUS
borderpx $BORDER
shadows 0
layer_shadows 0
gappih 0
gappiv 0
gappoh 0
gappov 0
animations 0
effects { blur { enable 0 } }
layout {
    titlebar { enable 0 }
}"
sleep 2

# BACKGROUND: floating and SMALLER THAN THE OUTPUT, which is not a detail.
# Tiled, it filled the screen, so its whole-surface damage was whole-OUTPUT
# damage and every frame counted as a full redraw -- the corner then survives
# because the scene is redrawn entire, which says nothing about partial damage
# and is the one thing this fixture is not allowed to conclude. At 1000x700 the
# per-frame damage is about a third of the output and the frames are genuinely
# partial, while the whole area under test still sits over live content.
"$REPAINT" --title wlbg --size "$BG_W"x"$BG_H" --cell 16 --hold-ms 100 \
	> "$OUTDIR/wlbg.log" 2>&1 &
HL_SPAWNED_PIDS+=("$!")
hl_wait_client_count 1 40 >/dev/null 2>&1
sleep 1.5
hl_dispatch "toggle_floating" 0.5
hl_dispatch "resize_window,$BG_W,$BG_H" 0.5
hl_dispatch "move_window,$BG_X,$BG_Y" 1

# FOREGROUND: flat, stationary, and QUIET once placed. --frames 40 at 100ms
# gives it four seconds to be floated, resized and moved; after that it stops
# committing, so it damages nothing and the only thing changing in the scene is
# the background underneath its rounded corners.
#
# --ssd is not optional at BORDER > 0. A client that never binds xdg-decoration
# is CSD by default, and check_hit_no_border() then gives it NO border while
# apply_border() still insets its surface by borderpx -- so the window looks
# bordered and the ring is empty. Measured without this, BOTH engines "failed"
# BORDER=6 with background where the border should be, which would have been
# reported as a shared SceneFX border defect. It was the fixture's client.
"$REPAINT" --title wlfg --size "$FG_W"x"$FG_H" --solid 202020 --frames 40 \
	--ssd --hold-ms 100 > "$OUTDIR/wlfg.log" 2>&1 &
HL_SPAWNED_PIDS+=("$!")
hl_wait_client_count 2 40 >/dev/null 2>&1
sleep 1

# Floating and clear of every screen edge, because set_client_corner_location()
# SQUARES any corner within border_radius of the monitor edge: a window touching
# an edge has no rounded corner there to test, and a fixture that does not know
# that measures a square corner and calls the renderer broken.
hl_dispatch "toggle_floating" 0.5
hl_dispatch "resize_window,$FG_W,$FG_H" 0.5
hl_dispatch "move_window,$FG_X,$FG_Y" 1
sleep 4

grep -c "settled" "$OUTDIR/wlfg.log" > /dev/null 2>&1 \
	&& echo "  foreground: $(grep -m1 settled "$OUTDIR/wlfg.log")" \
	|| echo "  foreground: still committing (frames not exhausted)"

hl_get "get all-clients" \
	| jq -r '.clients[]|"\(.title) \(.x) \(.y) \(.width) \(.height) \(.isfloating)"' \
	> "$OUTDIR/clients.txt"
sed 's/^/  /' "$OUTDIR/clients.txt"

FG=$(awk '$1=="wlfg"{print $2, $3, $4, $5}' "$OUTDIR/clients.txt")
[ -n "$FG" ] || { echo "no foreground client"; hl_stop; exit 1; }

# A run of captures. The background flips ~10 times a second, so shots 300ms
# apart land on different generations and any pixel can be followed across the
# sequence: current, one frame late, or not background at all.
GEN_BEFORE=$(grep -c "^wlrepaint: gen" "$OUTDIR/wlbg.log")
for i in $(seq 1 "$SHOTS"); do
	hl_screenshot "shot$i"
	sleep 0.3
done
GEN_AFTER=$(grep -c "^wlrepaint: gen" "$OUTDIR/wlbg.log")
GEN_DELTA=$((GEN_AFTER - GEN_BEFORE))
FG_AFTER=$(grep -c "^wlrepaint: gen" "$OUTDIR/wlfg.log")

python3 - "$OUTDIR" "$RADIUS" "$BORDER" "$SHOTS" $FG <<'PY' > "$OUTDIR/probe.txt"
import sys
from PIL import Image

out, radius, border, shots = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
x, y, w, h = (int(v) for v in sys.argv[5:9])

# wlrepaint's palette, and the only interface between the client and this
# analysis. A: red, green. B: blue, yellow.
PARITY = {(255, 0, 0): 'A', (0, 255, 0): 'A', (0, 0, 255): 'B', (255, 255, 0): 'B'}
TOL = 40

def classify(px):
    """Which generation this pixel is showing, or None if it is not background.

    Tight tolerance on purpose: a pixel where the arc's antialiasing has mixed
    background with the dark foreground is NOT background, and counting it as
    a stale one would turn every correctly rendered edge into a defect."""
    best, bestd = None, 10 ** 9
    for ref, gen in PARITY.items():
        d = sum(abs(a - b) for a, b in zip(px, ref))
        if d < bestd:
            best, bestd = gen, d
    return best if bestd <= TOL else None

rows = []
prev_scene = None
for s in range(1, shots + 1):
    im = Image.open(f"{out}/shot{s}.png").convert("RGB")
    p = im.load()
    W, H = im.size

    # The scene's current generation, read from the ring of background around
    # the window -- inside the background client, outside the foreground. Not
    # from the screen edge: the background client is deliberately smaller than
    # the output now, so the edge is wallpaper and would vote for nothing.
    votes = {'A': 0, 'B': 0}
    for yy in range(max(0, y - radius), min(H, y + h + radius)):
        for xx in range(max(0, x - radius), min(W, x + w + radius)):
            if x <= xx < x + w and y <= yy < y + h:
                continue
            g = classify(p[xx, yy])
            if g:
                votes[g] += 1
    scene = 'A' if votes['A'] >= votes['B'] else 'B'
    stale_scene = 'B' if scene == 'A' else 'A'
    print(f"shot {s} scene {scene} votes A={votes['A']} B={votes['B']}")

    # PREMISE: the background immediately around the window must be materially
    # different between two shots that caught DIFFERENT generations. Sampled in
    # a ring one radius out from each side, so it measures the background the
    # corners actually sit on.
    #
    # Only parity-differing pairs count. Two captures can land on the same
    # generation -- the client flips ~10 times a second and grim is not
    # synchronised to it -- and that is a sampling coincidence, not a background
    # that stopped repainting. The producer side is asserted separately from the
    # client's own generation log, which cannot be faked by capture timing.
    if s > 1 and scene != prev_scene:
        prev = Image.open(f"{out}/shot{s-1}.png").convert("RGB").load()
        same = diff = 0
        for yy in range(max(0, y - radius), min(H, y + h + radius)):
            for xx in range(max(0, x - radius), min(W, x + w + radius)):
                if x <= xx < x + w and y <= yy < y + h:
                    continue
                if p[xx, yy] == prev[xx, yy]:
                    same += 1
                else:
                    diff += 1
        total = same + diff
        pct = (100.0 * diff / total) if total else 0.0
        print(f"premise {s} sampled {total} changed {diff} pct {pct:.1f}")
    prev_scene = scene

    # The four corner boxes, classified AGAINST THE GEOMETRY rather than merely
    # counted. Each corner box is r-by-r with an arc of radius r centred one
    # radius inside it; that splits the box into three populations with three
    # different expectations:
    #
    #   outside the arc   background, and specifically the CURRENT generation
    #   inside the arc    the window (or its border): not background at all
    #   on the arc        antialiasing, which is a blend and is not judged
    #
    # Counting "how much background is in this box" cannot tell a stale pixel
    # from a correct one, and cannot tell an unpainted pixel from either. The
    # distinction matters because in motion they look identical, which is what
    # "flicker" means in the report that started this.
    corners = {"TL": (x, y), "TR": (x + w - radius, y),
               "BR": (x + w - radius, y + h - radius),
               "BL": (x, y + h - radius)}
    centres = {"TL": (x + radius, y + radius),
               "TR": (x + w - radius, y + radius),
               "BR": (x + w - radius, y + h - radius),
               "BL": (x + radius, y + h - radius)}
    AA = 1.5
    for cn, (cx, cy) in corners.items():
        ox, oy = centres[cn]
        stale = unpainted = covered = gap = ok = aa = 0
        gap_cur = gap_old = 0
        first_bad = None
        for j in range(radius):
            for i in range(radius):
                px, py = cx + i, cy + j
                if not (0 <= px < W and 0 <= py < H):
                    continue
                # +0.5 for the pixel centre; the arc passes through the box
                # corner, so the sample point matters at this radius.
                d = ((px + 0.5 - ox) ** 2 + (py + 0.5 - oy) ** 2) ** 0.5
                g = classify(p[px, py])
                if d > radius + AA:
                    # Must be background, and must be THIS generation.
                    if g == scene:
                        ok += 1
                    elif g == stale_scene:
                        stale += 1
                        first_bad = first_bad or (px, py, "STALE", p[px, py])
                    else:
                        # Neither generation: an old frame from before the
                        # background client existed, the clear colour, or the
                        # window painted where it has no business painting.
                        # Either way this pixel is not the background it should
                        # be showing.
                        unpainted += 1
                        first_bad = first_bad or (px, py, "UNPAINTED", p[px, py])
                elif d < radius - AA:
                    # Must be the window or its border, never background.
                    if g is None:
                        covered += 1
                    else:
                        gap += 1
                        # Which generation the gap shows separates the two
                        # explanations for it: the CURRENT one means nobody ever
                        # painted here and the background is simply visible
                        # through a hole in the window's own coverage --
                        # geometry. An OLDER one would mean the hole is in the
                        # damage instead.
                        if g == scene:
                            gap_cur += 1
                        else:
                            gap_old += 1
                        first_bad = first_bad or (px, py, "COVERAGE-GAP", p[px, py])
                else:
                    aa += 1
        print(f"corner {s} {cn} ok {ok} stale {stale} unpainted {unpainted} "
              f"covered {covered} gap {gap} gap_current {gap_cur} "
              f"gap_older {gap_old} aa {aa}")
        if first_bad:
            bx, by, kind, col = first_bad
            print(f"firstbad {s} {cn} {kind} at {bx},{by} rgb {col[0]},{col[1]},{col[2]}")
        rows.append((s, cn, stale, unpainted, gap, ok))

# PREMISE for any BORDER > 0 run: there is actually a border being painted.
# Sampled mid-edge, far from both corners, where nothing but the border can be.
if border > 0:
    p = Image.open(f"{out}/shot1.png").convert("RGB").load()
    bx, by = x + border // 2, y + h // 2
    col = p[bx, by]
    kind = ("background" if classify(col) else
            "foreground" if sum(abs(a - b) for a, b in zip(col, (32, 32, 32))) < 40
            else "border")
    print(f"borderprobe at {bx},{by} rgb {col[0]},{col[1]},{col[2]} is {kind}")

print("worst_corner_stale", max((r[2] for r in rows), default=0))
print("worst_corner_unpainted", max((r[3] for r in rows), default=0))
print("worst_corner_gap", max((r[4] for r in rows), default=0))
print("least_corner_ok", min((r[5] for r in rows), default=0))
PY
sed 's/^/  /' "$OUTDIR/probe.txt"

echo
echo "-- premise: the background under the window really is repainting --"
# The check both earlier versions of this fixture failed. 164 and then 72
# changed pixels across a whole screen is not a changing background, and
# without this assertion both runs reported a clean corner and passed.
#
# Two independent halves, because either alone can be satisfied by an accident:
# the PRODUCER really committed new generations (its own log, immune to capture
# timing), and those generations really reached the screen around the window
# (the pixels, immune to a client that logs without presenting).
echo "  background generations committed during capture: $GEN_DELTA"
echo "  foreground generations total (settled = stopped): $FG_AFTER"
hl_assert "the background client committed new generations while capturing" \
	"$([ "$GEN_DELTA" -ge "$SHOTS" ] && echo true || echo false)" "true"

PAIRS=$(grep -c "^premise" "$OUTDIR/probe.txt")
WORST_PCT=$(awk '/^premise/{ if (n=="" || $8+0 < n) n=$8+0 } END{ printf "%d", (n=="" ? -1 : n) }' \
	"$OUTDIR/probe.txt")
echo "  generation-differing capture pairs: $PAIRS, smallest change ${WORST_PCT}%"
hl_assert "at least two captures caught different background generations" \
	"$([ "$PAIRS" -ge 2 ] && echo true || echo false)" "true"
hl_assert "a generation flip changes >= 75% of the background around the window" \
	"$([ "$WORST_PCT" -ge 75 ] && echo true || echo false)" "true"
if [ "$PAIRS" -lt 2 ] || [ "$WORST_PCT" -lt 75 ] || [ "$GEN_DELTA" -lt "$SHOTS" ]; then
	echo "  INVALID TEST -- a persistence result means nothing without this."
	hl_stop
	hl_summary
	exit 1
fi

if [ "$BORDER" -gt 0 ]; then
	echo
	echo "-- premise: a border is actually being drawn --"
	BKIND=$(awk '/^borderprobe/{print $NF}' "$OUTDIR/probe.txt")
	grep "^borderprobe" "$OUTDIR/probe.txt" | sed 's/^/  /'
	hl_assert "the window has a painted border, not just reserved space" \
		"$BKIND" "border"
	if [ "$BKIND" != border ]; then
		echo "  INVALID TEST -- a border result means nothing without this."
		hl_stop
		hl_summary
		exit 1
	fi
fi

if [ "$ENGINE" != gles ]; then
	echo
	echo "-- premise: these frames were PARTIAL redraws --"
	# Load-bearing for what this fixture is allowed to conclude. A corner that
	# survives a scene redrawn whole every frame proves nothing about damage,
	# and "the corner is clean under partial damage" is the entire claim.
	FULL=$(hl_get "get avk-stats" | jq -r '.full_redraw_frames')
	PART=$(hl_get "get avk-stats" | jq -r '.partial_redraw_frames')
	echo "  full $FULL / partial $PART"
	if [ "$FULLDRAW" = 1 ]; then
		hl_assert "FULLDRAW=1 really did force whole-frame redraws" \
			"$([ "$PART" -eq 0 ] && echo true || echo false)" "true"
	else
		hl_assert "most frames were partial redraws" \
			"$([ "$PART" -gt "$FULL" ] && echo true || echo false)" "true"
	fi
fi

echo
echo "-- persistence: every pixel outside the arc is the CURRENT background --"
STALE=$(awk '/^worst_corner_stale/{print $2}' "$OUTDIR/probe.txt")
UNPAINTED=$(awk '/^worst_corner_unpainted/{print $2}' "$OUTDIR/probe.txt")
GAP=$(awk '/^worst_corner_gap/{print $2}' "$OUTDIR/probe.txt")
echo "  worst corner, any shot: $STALE stale, $UNPAINTED unpainted, $GAP coverage-gap"
grep "^firstbad" "$OUTDIR/probe.txt" | head -4 | sed 's/^/  /'
# STALE and UNPAINTED are both "this pixel is not showing the background it
# should be", and they are reported apart because they mean different things: a
# stale pixel is one generation behind, an unpainted one is older than the
# background client itself (the clear colour, or a frame from before it mapped).
# In motion they are the same flicker, which is why the live report could not
# distinguish them.
hl_assert "no rounded corner strands a pixel of the previous background" \
	"$STALE" "0"
hl_assert "no rounded corner shows content older than the background client" \
	"$UNPAINTED" "0"
hl_assert "nothing inside the arc is background showing through" "$GAP" "0"

echo
echo "-- the corners really are cut (or this proves nothing) --"
# A window whose corners are NOT rounded has no background outside its arc at
# all, so "no stale pixels" would pass trivially -- BREAK=rounded-clip is
# exactly that build. The geometric count is r-squared times (1 - pi/4) minus
# the antialiasing band; 0.7 of the ideal area leaves room for the band without
# leaving room for a square corner.
EXPECT=$(python3 -c "import math;print(int($RADIUS*$RADIUS*(1-math.pi/4)*0.7))")
LEAST_OK=$(awk '/^least_corner_ok/{print $2}' "$OUTDIR/probe.txt")
echo "  smallest per-corner correct-background count: $LEAST_OK (expect >= $EXPECT)"
hl_assert "every corner box shows background through a real arc" \
	"$([ "$LEAST_OK" -ge "$EXPECT" ] && echo true || echo false)" "true"

hl_stop
hl_summary
