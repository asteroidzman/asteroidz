#!/usr/bin/env bash
# avk-damage-test.sh — does redrawing less produce the same picture?
#
# M3.5D's acceptance test. AVK used to redraw the whole output every frame,
# which is slow but cannot be wrong. It now redraws only the damaged region,
# which is fast and can be wrong in a specific and nasty way: the pixels it did
# NOT touch have to already be correct, and they come from whatever that
# particular swapchain buffer held the last time it was on screen. There are up
# to four of those in rotation (WLR_SWAPCHAIN_CAP), so "the last frame" is the
# wrong mental model and any code written to it is broken three frames out of
# four.
#
# So the test runs the same scene twice -- once redrawing only the damage, once
# with AZ_AVK_FULL_DAMAGE=1 forcing the old behaviour -- and asserts the two
# pictures agree. The full-damage run is the reference precisely because it
# cannot be wrong.
#
# The premise is asserted too, and it matters more than usual here. If the
# partial run had quietly redrawn everything, the two runs would agree
# perfectly and the comparison would prove nothing at all. So the run has to
# show damage_ratio < 1 and partial_redraw_frames > 0 before its agreement with
# the reference counts for anything.
#
# What provides the damage is a terminal's blinking cursor: a few hundred
# pixels changing a couple of times a second, which is small enough that a
# broken preserve is obvious everywhere else on the screen. The comparison is
# on the wallpaper and the window decorations -- regions that never change and
# are therefore never redrawn -- so what is being measured really is the
# untouched part of the buffer.
#
# Break tests, each of which MUST fail:
#
#   BREAK=preserve   AVK_NO_LOAD_PRESERVE=1 on the partial run: the render
#                    pass loads the target with DONT_CARE instead of LOAD, so
#                    everything outside the damage becomes undefined.
#   BREAK=stale      AZ_AVK_FULL_DAMAGE=1 on BOTH runs, which makes the two
#                    runs identical by construction. This one breaks the TEST
#                    rather than the code, and it must fail on the premise
#                    assertions -- if it does not, the premise assertions are
#                    not doing their job and every other result here is
#                    unearned.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-damage"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-damage-test: needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "avk-damage-test: needs python3-pillow"; exit 1; }

SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

run_backend() { # run_backend MODE OUTDIR
	local mode="$1" outdir="$2"
	HL_OUTDIR="$outdir"
	HL_WIDTH=1280 HL_HEIGHT=720
	HL_ENV="ASTEROIDZ_RENDERER=avk"
	if [ "$mode" = full ] || [ "$BREAK" = stale ]; then
		HL_ENV="$HL_ENV AZ_AVK_FULL_DAMAGE=1"
	fi
	if [ "$mode" = partial ] && [ "$BREAK" = preserve ]; then
		HL_ENV="$HL_ENV AVK_NO_LOAD_PRESERVE=1"
	fi
	export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

	# The palette cursor is shell state and both runs live in one shell. Left
	# alone the second run spawns different colours and every comparison fails
	# for a reason damage had nothing to do with.
	HL_SPAWN_COLOR_IDX=0

	hl_start "$SCENE_KDL"
	hl_spawn_kitty one >/dev/null; hl_wait_client_count 1 60
	hl_spawn_kitty two >/dev/null; hl_wait_client_count 2 60
	# Long enough for the open animations to settle and then for the cursor to
	# blink often enough that every buffer in the swapchain has been rendered
	# into several times. Four buffers; a blink is roughly half a second.
	sleep 6
	hl_get "get avk-stats" > "$outdir/stats-run.json"
	# Zero the counters immediately before the capture, so what gets read back
	# afterwards describes only the frames the screenshot actually came from.
	# Without this the numbers are dominated by the window-open animations,
	# which damage most of the screen -- and a screenshot taken just after a
	# full redraw would look correct no matter how badly preservation was
	# broken. That is the difference between measuring preservation and
	# happening not to need it.
	hl_dispatch reset_avk_stats
	hl_screenshot shot
	hl_get "get avk-stats" > "$outdir/stats-shot.json"
	hl_stop
}

field() { # field JSON NAME
	python3 - "$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}

count_colour() { # count_colour PNG "#rrggbb" -> pixel count
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
want = tuple(int(sys.argv[2][i:i+2], 16) for i in (1, 3, 5))
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(n for n, c in im.getcolors(1 << 24) if c == want))
PY
}

BASE="${TMPDIR:-/tmp}/asteroidz-avk-damage-$$"
echo "== full-damage reference run =="
run_backend full "$BASE/full"
echo "== partial-damage run =="
run_backend partial "$BASE/partial"

FULL_PNG="$BASE/full/shot.png"
PART_PNG="$BASE/partial/shot.png"

echo "-- the premise: the runs really did redraw different amounts --"
FULL_FRAMES="$(field "$BASE/full/stats-run.json" frames)"
PART_FRAMES="$(field "$BASE/partial/stats-run.json" frames)"
FULL_FULLREDRAW="$(field "$BASE/full/stats-run.json" full_redraw_frames)"
PART_PARTIAL="$(field "$BASE/partial/stats-run.json" partial_redraw_frames)"
PART_RATIO="$(field "$BASE/partial/stats-run.json" damage_ratio)"
PART_RATIO="$(python3 -c "print('%.3f' % $PART_RATIO)" 2>/dev/null || echo x)"

hl_assert "both runs composited frames" \
	"$([ "${FULL_FRAMES:-0}" -gt 0 ] && [ "${PART_FRAMES:-0}" -gt 0 ] \
		&& echo true || echo false)" "true"
# The reference has to be a reference: every one of its frames a full redraw.
hl_assert "the reference redrew the whole output every frame" \
	"${FULL_FULLREDRAW:-x}" "${FULL_FRAMES:-y}"
# And the run under test has to actually be doing the thing under test.
hl_assert "the partial run redrew less than the whole output on some frames" \
	"$([ "${PART_PARTIAL:-0}" -gt 0 ] && echo true || echo false)" "true"
echo "  note: over the whole run the partial pass redrew $PART_RATIO of the desktop"
hl_assert "and redrew substantially less overall (ratio $PART_RATIO)" \
	"$(python3 -c "print('true' if $PART_RATIO < 0.9 else 'false')" 2>/dev/null \
		|| echo x)" "true"

# The sharper premise, and the one the whole comparison rests on: the frames
# the SCREENSHOT came from were partial redraws. A capture taken right after a
# full redraw looks correct however badly preservation is broken, so without
# this the pixel assertions below could pass on a build with no preservation at
# all.
SHOT_FULL="$(field "$BASE/partial/stats-shot.json" full_redraw_frames)"
SHOT_PARTIAL="$(field "$BASE/partial/stats-shot.json" partial_redraw_frames)"
echo "  note: the captured frames were $SHOT_PARTIAL partial, $SHOT_FULL full"
hl_assert "the captured frame itself was a partial redraw" \
	"$([ "${SHOT_PARTIAL:-0}" -gt 0 ] && [ "${SHOT_FULL:-1}" -eq 0 ] \
		&& echo true || echo false)" "true"

echo "-- the untouched parts of the buffer are still right --"
# Regions that never change, and are therefore never redrawn after the first
# frame. If a stale buffer's contents were not preserved, this is where it
# shows: the numbers are exact pixel counts, so a single wrong pixel fails.
WP="$(count_colour "$FULL_PNG" "#808080")"
hl_assert "the reference has a wallpaper at all" \
	"$([ "${WP:-0}" -gt 10000 ] && echo true || echo false)" "true"
hl_assert "the partial run has the same wallpaper area ($WP px)" \
	"$(count_colour "$PART_PNG" "#808080")" "$WP"

for colour in "#2a6fd6" "#c66b25" "#eb441e"; do
	ref="$(count_colour "$FULL_PNG" "$colour")"
	hl_assert "the partial run covers the same area in $colour ($ref px)" \
		"$(count_colour "$PART_PNG" "$colour")" "$ref"
done

echo "-- and the window contents survived too --"
# Sampled at the centre of each window, well inside the border and below the
# titlebar -- a region redrawn once at open and preserved from then on.
for spec in "left 320 400 #aa2222" "right 960 400 #22aa22"; do
	set -- $spec
	name="$1" x="$2" y="$3" want="$4"
	got="$(python3 - "$PART_PNG" "$x" "$y" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
print("#%02x%02x%02x" % im.getpixel((int(sys.argv[2]), int(sys.argv[3]))))
PY
)"
	hl_assert "the $name window still shows its own colour" "$got" "$want"
done

echo
echo "reference: $FULL_PNG"
echo "partial:   $PART_PNG"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
