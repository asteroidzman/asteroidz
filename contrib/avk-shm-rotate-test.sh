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
# commit 1 and applies only "mark 2" leaves mark 1 missing from its copy.
#
# Fixed in ef45327 by giving a commit's damage to every buffer in the
# surface's pool.
#
# WHAT THIS ASSERTS, AND WHY IT IS NOT THE OBVIOUS THING
#
# The obvious assertion -- every mark the client drew is on screen in every
# capture -- does not work. It passes against the pre-fix binary. The marks
# survive a screenshot on a build that visibly flickered a real desktop, so
# something restores the displayed content before a capture lands. That was
# measured, not assumed, and it is why this test was rewritten. The pixel
# assertions below are kept because they are true and cheap, but they are NOT
# the falsifier and must not be counted as coverage.
#
# The falsifier is the upload accounting, which measures the mechanism itself.
# Per-buffer stats are in `get avk-stats` -> shm_sources[]. Every commit that
# introduces a mark reports exactly one 24x24 rectangle, so upload bytes
# convert directly into "how many marks' worth of damage did this buffer
# receive". With B buffers and M marks the client attaches each buffer at M/B
# of the mark commits -- so if a buffer receives MORE marks' worth of damage
# than its own commits ever reported, it can only have got them from commits
# it did not carry. That is the fix, stated as a number:
#
#     pre-fix  a00a911:  1 mark  per buffer   (bound is 2)
#     post-fix ef45327:  4 marks per buffer   (bound is 2)
#
# and the bound is generous to the broken build: it counts the buffer's first
# mark commit too, which is always a full upload and so contributes no partial
# damage at all.
#
# Break tests, each of which MUST fail:
#
#   BREAK=one-buffer   run wlrotate with --buffers 1, so no rotation happens
#                      and the condition cannot arise. A single buffer carries
#                      every commit itself, so it can never receive more than
#                      it reported. This is a premise check on the TEST: if it
#                      still passes, the assertion is not measuring rotation.
#   BREAK=source-full  AZ_AVK_SOURCE_FULL=1 uploads every buffer whole, which
#                      is the sledgehammer that also fixes the bug. There is
#                      then no partial damage to account for at all, so the
#                      accumulation assertion has nothing to measure and
#                      fails -- correctly, because a compositor that never
#                      uses the damage path has not been shown to use it
#                      right.
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

# Four buffers and eight marks give every buffer exactly two mark-bearing
# commits of its own, one of which is its first sight and therefore a full
# upload. Anything above two marks' worth of partial damage came from
# somewhere else.
MARKS=8
BUFFERS=4
IDLE=8
[ "$BREAK" = one-buffer ] && BUFFERS=1

# Must match mark_box() in contrib/wlrotate/wlrotate.c.
MARK_BYTES=$((24 * 24 * 4))

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
	--hold-ms 150 --idle-commits "$IDLE" > "$OUTDIR/wlrotate.log" 2>&1 &
ROT_PID=$!
HL_SPAWNED_PIDS+=("$ROT_PID")
hl_wait_client_count 1 80
# Long enough for every mark to be committed and for the idle commits to
# rotate the pool several more times.
sleep 6

# ---------------------------------------------------------------------------
# THE assertion: a buffer received damage from commits it did not carry.
# ---------------------------------------------------------------------------

# What the client itself reported, straight out of its log: for each slot, how
# many of its commits introduced a mark. The largest of those is the most
# partial damage any buffer could legitimately owe itself.
OWN_MARKS="$(python3 - "$OUTDIR/wlrotate.log" "$MARKS" <<'PYEOF'
import re, sys
per = {}
for line in open(sys.argv[1]):
    m = re.match(r"wlrotate: commit (\d+) slot (\d+) marks (\d+)", line)
    if not m:
        continue
    commit, slot = int(m.group(1)), int(m.group(2))
    if commit <= int(sys.argv[2]):        # a commit that introduced a mark
        per[slot] = per.get(slot, 0) + 1
print(max(per.values()) if per else 0)
PYEOF
)"
BOUND=$((OWN_MARKS * MARK_BYTES))

hl_get "get avk-stats" > "$OUTDIR/avk-stats.json" 2>/dev/null
read -r NSRC BEST_BYTES WORST_BYTES <<<"$(python3 - "$OUTDIR/avk-stats.json" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
# The client's own buffers, by size. Anything else on the screen (the
# wallpaper) is a different shape and is not part of this measurement.
mine = [s for s in d.get("shm_sources", [])
        if s["width"] == 400 and s["height"] == 300]
if not mine:
    print("0 0 0"); raise SystemExit
# A full upload replaces the image outright and clears whatever damage was
# pending, so it says nothing about accumulation. Only the partial bytes do.
partial = [s["upload_bytes"] - s["full_uploads"] * 400 * 300 * 4 for s in mine]
print(f"{len(mine)} {max(partial)} {min(partial)}")
PYEOF
)"

echo "-- a buffer must receive the damage of commits it did not carry --"
echo "  note: buffers=$BUFFERS marks=$MARKS  sources seen=$NSRC"
echo "  note: most mark commits any one buffer carried: $OWN_MARKS" \
	"(bound $BOUND bytes)"
echo "  note: partial upload bytes per buffer: worst $WORST_BYTES," \
	"best $BEST_BYTES ($(python3 -c "print('%.2f' % ($BEST_BYTES/$MARK_BYTES))") marks)"

hl_assert "the client's buffers are all cached as sources" \
	"$NSRC" "$BUFFERS"

hl_assert "a buffer received more damage than its own commits reported" \
	"$([ "$BEST_BYTES" -gt "$BOUND" ] && echo true || echo false)" "true"

# Premise: the client really did rotate a pool, or the assertion above is not
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

# Premise: partial uploads were actually in use, so the bytes above came from
# the damage path and not from re-uploading everything.
PARTIAL="$(python3 -c \
	"import json;print(json.load(open('$OUTDIR/avk-stats.json')).get('shm_partial_uploads',0))" \
	2>/dev/null || echo 0)"
echo "  note: partial SHM uploads during the run: $PARTIAL"
if [ "$BREAK" = source-full ]; then
	hl_assert "BREAK=source-full really did stop partial uploads" \
		"$([ "$PARTIAL" -eq 0 ] && echo true || echo false)" "true"
else
	hl_assert "partial uploads were in use ($PARTIAL)" \
		"$([ "$PARTIAL" -gt 0 ] && echo true || echo false)" "true"
fi

# ---------------------------------------------------------------------------
# Pixels. True, cheap, and NOT the falsifier -- see the header.
# ---------------------------------------------------------------------------

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

echo "-- and the marks are on screen (true on the broken build too) --"
echo "  note: mark counts per capture:$TOTALS"

hl_assert "the client's marks reached the screen at all ($BEST seen)" \
	"$([ "$BEST" -gt 0 ] && echo true || echo false)" "true"

hl_assert "every capture shows all $MARKS marks (worst was $WORST)" \
	"$([ "$WORST" -eq "$MARKS" ] && echo true || echo false)" "true"

STABLE=true
for g in $TOTALS; do
	[ "$g" = "$BEST" ] || STABLE=false
done
hl_assert "the mark count does not change between captures" "$STABLE" "true"

kill "$ROT_PID" 2>/dev/null
hl_stop

echo
echo "screenshots: $OUTDIR/shot-*.png"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "Both breaks remove the CONDITION rather than the fix -- one-buffer"
	echo "stops the rotation, source-full stops partial uploads -- so both"
	echo "land on the accumulation assertion, which has nothing left to"
	echo "measure. The falsifier for the fix itself is the pre-ef45327"
	echo "binary, where the condition is present and the accumulation is"
	echo "not: 1 mark against a bound of 2."
fi
hl_summary
