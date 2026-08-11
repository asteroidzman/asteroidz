#!/usr/bin/env bash
# avk-shm-rotate-test.sh — the damage a commit reports is not the damage a
# BUFFER is owed.
#
# wl_surface.damage_buffer states what changed since the previous COMMIT of the
# surface. A renderer that caches one image per wl_buffer needs something else:
# what changed since it last saw THAT buffer. While a client reuses a single
# buffer the two are identical, which is why contrib/wlreuse -- one buffer for
# its whole life -- cannot tell them apart, and why avk-shm-partial passed
# 23/23 against a desktop where every KDE application flickered.
#
# contrib/wlrotate rotates a pool the way real toolkits do:
#
#     commit 1  buffer A  marks 0            damage = mark 0
#     commit 2  buffer B  marks 0,1          damage = mark 1
#     commit 3  buffer A  marks 0,1,2        damage = mark 2
#
# Every commit is correct Wayland. But a compositor that last uploaded A at
# commit 1 and applies only "mark 2" leaves mark 1 missing from its copy, and
# presenting A and B alternately blinks the client's own recent pixels.
#
# So the assertion is simply: every mark drawn is on screen, in every capture,
# whichever buffer happens to be showing.
#
# Fixed in ef45327 by giving a commit's damage to every buffer in the
# surface's pool.
#
# STATUS: NOT YET VALID COVERAGE.
#
# This test PASSES against the pre-fix binary (a00a911), so it does not
# currently falsify anything. The client rotates a real pool and partial
# uploads are demonstrably in use (22 of them), yet all four marks survive --
# so something is making the compositor upload those buffers whole anyway.
# The likeliest candidate is the scene-content observer reporting NULL damage
# when the wl_buffer OBJECT changes, which sets pending_full and hides the
# accumulation bug from this particular client while real toolkits still hit
# it. Until that is chased down and the test is shown to fail without the fix,
# it must not be counted as coverage for ef45327.
#
# Break tests, each of which MUST fail:
#
#   BREAK=one-buffer   run wlrotate with --buffers 1, so no rotation happens
#                      and the condition cannot arise. This one is a premise
#                      check on the TEST, not on the compositor: if it still
#                      passes the assertions below, they are not measuring
#                      rotation.
#   BREAK=source-full  AZ_AVK_SOURCE_FULL=1 uploads every buffer whole, which
#                      is the sledgehammer that also fixes the bug. It must
#                      make the test pass, so it is asserted the other way
#                      round -- see the note at the bottom.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-shm-rotate"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "needs python3-pillow"; exit 1; }

WLSHOT="$(dirname "$0")/wlshot/wlshot"
WLROTATE="$(dirname "$0")/wlrotate/wlrotate"
for t in "$WLSHOT" "$WLROTATE"; do
	[ -x "$t" ] || { echo "avk-shm-rotate: $t not built" >&2; exit 1; }
done

MARKS=4
BUFFERS=2
[ "$BREAK" = one-buffer ] && BUFFERS=1

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-rotate-$$"
HL_OUTDIR="$OUTDIR"
HL_WIDTH=1280 HL_HEIGHT=720
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = source-full ] && HL_ENV="$HL_ENV AZ_AVK_SOURCE_FULL=1"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

hl_start "shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }
cursor_hide_timeout 0
animations 0"
sleep 2

"$WLROTATE" --buffers "$BUFFERS" --marks "$MARKS" --size 400x300 \
	--hold-ms 120 --idle-commits 20 > "$OUTDIR/wlrotate.log" 2>&1 &
ROT_PID=$!
HL_SPAWNED_PIDS+=("$ROT_PID")
hl_wait_client_count 1 80
# Long enough for every mark to be committed and for the idle commits to
# rotate the pool several more times.
sleep 6

# Count the mark-coloured pixels in a capture. The marks are 24x24 and
# 0xff4040; nothing else in the scene is red.
count_marks() {
	python3 - "$1" <<'PYEOF'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(int)
red = (a[:, :, 0] > 180) & (a[:, :, 1] < 110) & (a[:, :, 2] < 110)
if not red.any():
    print("0 0"); raise SystemExit
# How many separate marks, by column clustering: they sit in a row 40px apart.
cols = np.nonzero(red.any(axis=0))[0]
groups = 1
for i in range(1, len(cols)):
    if cols[i] - cols[i - 1] > 4:
        groups += 1
print(f"{groups} {int(red.sum())}")
PYEOF
}

# Several captures across time: a stale cache shows as the mark count changing
# from frame to frame, because which buffer is on screen changes.
BEST=99; WORST=99; TOTALS=""
for i in 1 2 3 4 5 6; do
	"$WLSHOT" "$OUTDIR/shot-$i.png" 2>>"$OUTDIR/wlshot.log"
	read -r G PX <<<"$(count_marks "$OUTDIR/shot-$i.png")"
	TOTALS="$TOTALS $G"
	[ "$G" -lt "$WORST" ] && WORST="$G"
	[ "$BEST" -eq 99 ] && BEST="$G"
	[ "$G" -gt "$BEST" ] && BEST="$G"
	sleep 0.4
done

echo "-- every mark the client drew must be on screen, in every frame --"
echo "  note: buffers=$BUFFERS marks drawn=$MARKS  mark counts per capture:$TOTALS"

hl_assert "the client's marks reached the screen at all ($BEST seen)" \
	"$([ "$BEST" -gt 0 ] && echo true || echo false)" "true"

# THE assertion. Every capture must show every mark.
hl_assert "every capture shows all $MARKS marks (worst was $WORST)" \
	"$([ "$WORST" -eq "$MARKS" ] && echo true || echo false)" "true"

# And they must not vary between captures: a count that changes over time is
# the flicker itself, caught as a number.
STABLE=true
for g in $TOTALS; do
	[ "$g" = "$BEST" ] || STABLE=false
done
hl_assert "the mark count does not change between captures" "$STABLE" "true"

# Premise: the client really did rotate a pool, or none of the above is
# testing what it claims. wlrotate logs the slot it used per commit.
SLOTS="$(grep -c "slot 1" "$OUTDIR/wlrotate.log" || true)"
echo "  note: commits that used the second buffer: $SLOTS"
if [ "$BREAK" = one-buffer ]; then
	hl_assert "BREAK=one-buffer really did stop the rotation" \
		"$([ "$SLOTS" -eq 0 ] && echo true || echo false)" "true"
else
	hl_assert "the client really did rotate a pool ($SLOTS commits on buffer 2)" \
		"$([ "$SLOTS" -gt 2 ] && echo true || echo false)" "true"
fi

# Premise: partial uploads were actually in use, so the assertions above were
# exercising the damage path and not a full re-upload of everything.
PARTIAL="$(hl_get "get avk-stats" | python3 -c \
	"import json,sys; print(json.load(sys.stdin).get('shm_partial_uploads',0))" 2>/dev/null || echo 0)"
echo "  note: partial SHM uploads during the run: $PARTIAL"
if [ "$BREAK" = source-full ]; then
	hl_assert "BREAK=source-full really did stop partial uploads" \
		"$([ "$PARTIAL" -eq 0 ] && echo true || echo false)" "true"
else
	hl_assert "partial uploads were in use ($PARTIAL)" \
		"$([ "$PARTIAL" -gt 0 ] && echo true || echo false)" "true"
fi

kill "$ROT_PID" 2>/dev/null
hl_stop

echo
echo "screenshots: $OUTDIR/shot-*.png"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "Both breaks work by removing the CONDITION rather than the fix:"
	echo "one-buffer stops the rotation, source-full stops partial uploads."
	echo "Each therefore fails its own premise assertion above while the"
	echo "pixel assertions pass -- which is the honest way to show that this"
	echo "test depends on both, and neither is a falsifier for the fix"
	echo "itself. The falsifier for the fix is the pre-ef45327 binary."
fi
hl_summary
