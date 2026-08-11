#!/usr/bin/env bash
# avk-rounded-persist-test.sh — M4A: what a rounded corner leaves behind.
#
# THE GAP THIS FILLS
#
# Every earlier M4A test asks "is this corner clipped correctly in this frame?"
# A large-radius artifact found on a real desktop is not that question. It
# flickers, which means it is about what PERSISTS between frames: a pixel the
# window does not cover, whose background stops being repainted.
#
# The movement test did not cover it either -- that moves the window and checks
# the vacated area, which damages everything. Here the window is STATIONARY and
# the content BEHIND its transparent corner changes. If the corner region is
# excluded from damage, it keeps showing the old content while everything
# around it updates, and the diff is exact.
#
# WHY THE OPAQUE REGION IS NOT THE SUSPECT
#
# SceneFX's create_corner_location_region() subtracts a full r-by-r SQUARE at
# each corner from the opaque region, and the quarter-disc AVK renders is
# strictly inside that square. So it under-claims opacity: the background gets
# MORE damage than needed, never less, and normalisation only shrinks radii so
# the arc can never escape the square. That disagreement runs the safe way and
# cannot strand a pixel.
#
# BORDER ISOLATION is therefore the discriminator this script exists for:
# BORDER=0 tests rounded CLIENT coverage alone (M4A's own responsibility);
# BORDER=6 adds the border, whose inner cut-out is still a square subtraction
# (M4B). Run both. If only the second fails, the defect is not M4A's.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-rounded-persist"
BREAK="${BREAK:-}"
RADIUS="${RADIUS:-40}"
BORDER="${BORDER:-0}"
FULLDRAW="${FULLDRAW:-0}"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-persist-$$"
HL_OUTDIR="$OUTDIR"
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = rounded-clip ] && HL_ENV="$HL_ENV AZ_ROUNDED_OFF=1"
# Diagnostic only, never a fix: if the artifact disappears when every frame is
# redrawn whole, the defect is damage persistence rather than geometry.
[ "$FULLDRAW" = 1 ] && HL_ENV="$HL_ENV AZ_AVK_FULL_DAMAGE=1"
export HL_OUTDIR HL_ENV

echo "binary: $HL_ASTEROIDZ   radius=$RADIUS border=$BORDER fulldraw=$FULLDRAW"

hl_start "border_radius $RADIUS
borderpx $BORDER
gappih 0
gappiv 0
gappoh 0
gappov 0
animations 0
layout {
    titlebar { enable 0 }
}"
sleep 2

# The BACKGROUND window has to actually REPAINT. A plain terminal does not --
# the first version of this test spawned one and measured 164 changed pixels
# across the whole screen, so there was nothing for a stale corner to be stale
# against and the premise assertion caught it. contrib/wlrotate commits a new
# generation continuously, which is the whole reason it exists.
"$HL_REPO/contrib/wlrotate/wlrotate" --buffers 4 --marks 40 --size 900x1000 \
	--hold-ms 120 --idle-commits 400 > "$OUTDIR/wlrotate.log" 2>&1 &
HL_SPAWNED_PIDS+=("$!")
hl_wait_client_count 1 40 >/dev/null 2>&1
sleep 2

# The FOREGROUND window: stationary, rounded, tiled beside the first so its
# left corners sit over the boundary between them.
hl_spawn_kitty rounded >/dev/null 2>&1
hl_wait_client_count 2 40 >/dev/null 2>&1
sleep 2

hl_get "get all-clients" | jq -r '.clients[]|"  \(.title) \(.x) \(.y) \(.width) \(.height)"' \
	> "$OUTDIR/clients.txt"
cat "$OUTDIR/clients.txt"

# Two captures with the background driven to a different colour in between.
# Anything not covered by the rounded window must follow.
hl_screenshot f1
sleep 2.0
hl_screenshot f2

python3 - "$OUTDIR" "$RADIUS" <<'PY' > "$OUTDIR/probe.txt"
import sys
from PIL import Image
out, radius = sys.argv[1], int(float(sys.argv[2]))
a = Image.open(out + "/f1.png").convert("RGB"); pa = a.load()
b = Image.open(out + "/f2.png").convert("RGB"); pb = b.load()
W, H = a.size
changed = sum(1 for y in range(0, H, 3) for x in range(0, W, 3)
              if pa[x, y] != pb[x, y])
print("sampled_changed", changed)

# The corner boxes of the SECOND window: the r-by-r squares whose outer part is
# transparent. Report how many pixels there differ between the frames and how
# many are identical, per corner, so a stranded strip shows as "identical while
# its surroundings moved".
rows = [l.split() for l in open(out + "/clients.txt") if l.strip()]
for name, x, y, w, h in [(r[0], *map(int, r[1:])) for r in rows]:
    for cn, (cx, cy) in {"TL": (x, y), "TR": (x + w - radius, y),
                          "BR": (x + w - radius, y + h - radius),
                          "BL": (x, y + h - radius)}.items():
        same = diff = 0
        for j in range(radius):
            for i in range(radius):
                px, py = cx + i, cy + j
                if 0 <= px < W and 0 <= py < H:
                    if pa[px, py] == pb[px, py]: same += 1
                    else: diff += 1
        print(f"{name} {cn} same={same} diff={diff}")
PY
sed 's/^/  /' "$OUTDIR/probe.txt"

CHANGED=$(awk '/^sampled_changed/{print $2}' "$OUTDIR/probe.txt")
echo
hl_assert "the background actually changed between the two frames ($CHANGED px)" \
	"$([ "$CHANGED" -gt 200 ] && echo true || echo false)" "true"

echo "  (a corner whose pixels are ALL identical while the scene moved is the"
echo "   stranded region this test exists to find)"
hl_stop
hl_summary
