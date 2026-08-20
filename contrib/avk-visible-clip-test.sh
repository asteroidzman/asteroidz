#!/usr/bin/env bash
# avk-visible-clip-test.sh — does AVK stop copying what nothing can see, and is
# it still right when something can?
#
# A client's wl_shm buffer is copied to the GPU whenever it changes. Copying the
# part of it that an opaque window covers is bytes across the bus and a page
# fault per page, for pixels no draw will sample. Firefox's decoration frame is
# the extreme case: 5128x2788 of wl_shm under its own opaque dma-buf
# subsurface, 54.5 MB per redraw to show a 20px border.
#
# The optimisation is a HINT and is allowed to be wrong. What is not allowed is
# a texel being sampled that was never copied, and the whole design rests on the
# frame's visibility pass catching that before the draw. So this suite is in two
# halves, and only the second one is about correctness:
#
#   covered     a 1024x1024 shm client, fully hidden by an opaque window,
#               commits five more generations. The copy must collapse.
#   uncovered   the same client goes QUIET and the window above it is closed.
#               What is on screen must be the generation the client last
#               committed -- which it committed while nothing could see it.
#   churn       a client that allocates a FRESH wl_buffer for every redraw, so
#               every clipped copy is a partial write into an image that has
#               never been written. The copy path refuses exactly that -- it
#               has to, because leaving UNDEFINED may discard the whole image
#               -- unless the plan says there is nothing there to lose. This is
#               not an edge case: Firefox is a churning client at 54.5MB a
#               redraw, so it is every copy the subsystem exists to shrink.
#   frame       THE SHAPE THIS EXISTS FOR, with nothing covering the window at
#               all: a 1024x1024 shm parent with an opaque subsurface over most
#               of it. Nothing is hidden, the client is fully on screen, and
#               most of every copy is still waste. This is a browser's
#               decoration frame in miniature, and it is the case a fixture
#               built only out of overlapping windows would miss.
#
# The second is the one that matters. A build that clips copies and never
# repairs them passes every cost assertion in this file and shows the wrong
# picture, which is why BREAK=no-repair exists and must fail.
#
# Break tests, each of which MUST fail:
#
#   BREAK=no-repair   AZ_AVK_NO_VISIBLE_REPAIR=1 -- clip the copies, decline to
#                     repair what the clip got wrong. The cost assertions still
#                     pass. The uncovered window shows the last colour it
#                     committed while VISIBLE, which is a different colour, and
#                     that is the whole point of the fixture.
#   BREAK=refuse      AZ_AVK_REFUSE_UNDEFINED_PARTIAL=1 -- the pre-fix build in
#                     one switch: the copy path declines every clipped FIRST
#                     write. A churning client then saves nothing and says
#                     nothing about it beyond a line in the log, which is how
#                     this survived a session of looking at counters that were
#                     all going the right way.
#   BREAK=no-clip     AZ_AVK_NO_VISIBLE_CLIP=1 -- the control arm. Nothing is
#                     clipped, so the saving assertions fail and every pixel
#                     assertion passes. It is also run unconditionally below as
#                     the premise: without it the "we saved bytes" numbers have
#                     nothing to be smaller than.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-visible-clip"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-visible-clip-test: needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "avk-visible-clip-test: needs python3-pillow"; exit 1; }
WLREUSE="$(dirname "$0")/wlreuse/wlreuse"
[ -x "$WLREUSE" ] || { echo "avk-visible-clip-test: wlreuse not built -- run: cd contrib/wlreuse && make" >&2; exit 1; }
WLREPAINT="$(dirname "$0")/wlrepaint/wlrepaint"
[ -x "$WLREPAINT" ] || { echo "avk-visible-clip-test: wlrepaint not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

# No blur, no shadow, no rounding: each of them is a reason az_cmd_opaque_region
# declines to treat a window as an occluder, and a fixture in which nothing
# occludes anything would pass this file by measuring nothing.
SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

BASE="${TMPDIR:-/tmp}/asteroidz-avk-visclip-$$"

field() { python3 - "$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}
count_colour() { python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
want = tuple(int(sys.argv[2][i:i+2], 16) for i in (1, 3, 5))
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(n for n, c in im.getcolors(1 << 24) if c == want))
PY
}

# ── the fixture ────────────────────────────────────────────────────────────
#
# bottom   1024x1024 ARGB shm, six generations alternating #0000ff and #ff00ff,
#          then quiet. Generation 6 is #ff00ff and is committed while covered.
# top      an OPAQUE window (XRGB plus a whole-surface opaque region -- an ARGB
#          surface that says nothing is never an occluder, however solid it
#          looks), floated and grown past every edge of the output.
#
# The colours alternate so that "the last generation" and "the last generation
# anything could see" are different colours rather than the same one. A fixture
# using one colour cannot tell a repaired image from an unrepaired one.
BOTTOM_EARLY="#0000ff"    # generation 1, committed while visible
BOTTOM_FINAL="#ff00ff"    # generation 6, committed while covered

run_case() { # run_case NAME
	local name="$1"
	HL_OUTDIR="$BASE/$name"
	HL_WIDTH=1280 HL_HEIGHT=720
	HL_ENV="ASTEROIDZ_RENDERER=avk"
	[ "$name" = noclip ] && HL_ENV="$HL_ENV AZ_AVK_NO_VISIBLE_CLIP=1"
	[ "$BREAK" = no-repair ] && HL_ENV="$HL_ENV AZ_AVK_NO_VISIBLE_REPAIR=1"
	[ "$BREAK" = no-clip ] && HL_ENV="$HL_ENV AZ_AVK_NO_VISIBLE_CLIP=1"
	[ "$BREAK" = refuse ] && HL_ENV="$HL_ENV AZ_AVK_REFUSE_UNDEFINED_PARTIAL=1"
	export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV
	HL_SPAWN_COLOR_IDX=0

	hl_start "$SCENE_KDL"
	hl_dispatch set_option,animations,0 1

	# Eight generations at 1.2s. The window is covered somewhere around the
	# third, so the last four are committed to nobody -- and the eighth is the
	# one the screen has to be showing at the end.
	"$WLREUSE" --title bottom --size 1024x1024 \
		--colour 0000ff --colour ff00ff --hold-ms 1200 --generations 8 \
		> "$HL_OUTDIR/bottom.log" 2>&1 &
	local bottom=$!
	HL_SPAWNED_PIDS+=("$bottom")
	hl_wait_client_count 1 60
	sleep 0.5
	# The uncovered picture, before anything hides it. The premise for
	# everything below: this fixture does put a window on the screen.
	hl_screenshot visible

	# Bigger than the output, by construction rather than by resize_window:
	# wlreuse holds ONE buffer for its whole life and ignores the configure,
	# so a window told to grow simply does not. That is worth knowing here --
	# a fixture that asks for a covering window and gets a 512x512 one covers
	# a third of the screen and fails a premise instead of a claim.
	"$WLREUSE" --title top --opaque --size 1400x820 \
		--colour 202020 --hold-ms 600000 \
		> "$HL_OUTDIR/top.log" 2>&1 &
	local top=$!
	HL_SPAWNED_PIDS+=("$top")
	hl_wait_client_count 2 60
	# FULLSCREEN, not floated-and-moved. A floating window is placed by the
	# compositor and clamped to the usable area, which left a strip of the
	# window below it showing -- and a fixture that covers 88% of a window is
	# testing a different thing quietly. Fullscreen puts it at the origin, and
	# its buffer is larger than the output, so it covers every pixel.
	hl_dispatch toggle_fullscreen 0.5
	sleep 1
	hl_screenshot covered

	# From here on the numbers belong to the covered period alone.
	hl_dispatch reset_avk_stats 0.3
	# Whatever is left of the eight generations, all of them hidden, and then
	# the client is quiet for good.
	sleep 7
	hl_get "get avk-stats" > "$HL_OUTDIR/covered.json"
	grep -c 'generation' "$HL_OUTDIR/bottom.log" > "$HL_OUTDIR/gens" 2>/dev/null || true

	# UNCOVER. The client will never commit again, so what appears is entirely
	# the compositor's own doing.
	kill "$top" 2>/dev/null
	sleep 2
	hl_screenshot uncovered
	hl_get "get avk-stats" > "$HL_OUTDIR/after.json"
	kill "$bottom" 2>/dev/null
	hl_stop
}

# ── the decoration-frame shape ─────────────────────────────────────────────
#
# One client, nothing above it. The parent's buffer is 1024x1024; the window it
# is drawn into is 1024x696, and a 960x600 opaque child sits at +32+32 inside
# that. So of 1048576 buffer pixels, 136704 can be seen -- 13% -- and a copy
# that ignores both the child and the part of the buffer the window does not
# reach carries all of it.
run_frame_case() { # run_frame_case NAME
	local name="$1"
	HL_OUTDIR="$BASE/$name"
	HL_WIDTH=1280 HL_HEIGHT=720
	HL_ENV="ASTEROIDZ_RENDERER=avk"
	[ "$name" = frame-noclip ] && HL_ENV="$HL_ENV AZ_AVK_NO_VISIBLE_CLIP=1"
	[ "$BREAK" = no-repair ] && HL_ENV="$HL_ENV AZ_AVK_NO_VISIBLE_REPAIR=1"
	[ "$BREAK" = no-clip ] && HL_ENV="$HL_ENV AZ_AVK_NO_VISIBLE_CLIP=1"
	[ "$BREAK" = refuse ] && HL_ENV="$HL_ENV AZ_AVK_REFUSE_UNDEFINED_PARTIAL=1"
	export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV
	HL_SPAWN_COLOR_IDX=0

	hl_start "$SCENE_KDL"
	hl_dispatch set_option,animations,0 1
	"$WLREUSE" --title framed --size 1024x1024 --child 960x600+32+32 \
		--colour 0000ff --colour ff00ff --hold-ms 700 \
		> "$HL_OUTDIR/framed.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	hl_wait_client_count 1 60
	sleep 2
	hl_dispatch reset_avk_stats 0.3
	sleep 4
	hl_get "get avk-stats" > "$HL_OUTDIR/stats.json"
	hl_screenshot framed
	kill "$pid" 2>/dev/null
	hl_stop
}

# ── a client that never presents the same buffer twice ─────────────────────
#
# wlrepaint --churn allocates a fresh wl_buffer per generation, so AVK has a
# brand-new VkImage every time and every clipped copy is a partial write into an
# image whose layout is still UNDEFINED. That is refused by the copy path unless
# the plan states there is nothing outside the rectangles to lose.
#
# NOTHING COVERS IT. The buffer is 1024x2048 and the window it is drawn into is
# about 700 rows tall, so two thirds of every generation is off the bottom of
# its own window and can never be sampled. An occluder was the obvious way to
# build this and it does not work: a fully hidden window gets no frame
# callbacks, so a callback-driven client stops committing and BOTH arms copy
# nothing. Overflow keeps the client drawing while most of what it draws stays
# invisible, which is the same question asked in a way the fixture can answer.
run_churn_case() { # run_churn_case NAME
	local name="$1"
	HL_OUTDIR="$BASE/$name"
	HL_WIDTH=1280 HL_HEIGHT=720
	HL_ENV="ASTEROIDZ_RENDERER=avk"
	[ "$name" = churn-noclip ] && HL_ENV="$HL_ENV AZ_AVK_NO_VISIBLE_CLIP=1"
	[ "$BREAK" = no-repair ] && HL_ENV="$HL_ENV AZ_AVK_NO_VISIBLE_REPAIR=1"
	[ "$BREAK" = no-clip ] && HL_ENV="$HL_ENV AZ_AVK_NO_VISIBLE_CLIP=1"
	[ "$BREAK" = refuse ] && HL_ENV="$HL_ENV AZ_AVK_REFUSE_UNDEFINED_PARTIAL=1"
	export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV
	HL_SPAWN_COLOR_IDX=0

	hl_start "$SCENE_KDL"
	hl_dispatch set_option,animations,0 1
	"$WLREPAINT" --title churner --size 1024x2048 --fixed --churn \
		--hold-ms 120 > "$HL_OUTDIR/churner.log" 2>&1 &
	local ch=$!
	HL_SPAWNED_PIDS+=("$ch")
	hl_wait_client_count 1 60
	sleep 2
	hl_dispatch reset_avk_stats 0.3
	sleep 5
	hl_get "get avk-stats" > "$HL_OUTDIR/stats.json"
	hl_screenshot churn
	kill "$ch" 2>/dev/null
	hl_stop
}

echo "== production =="
run_case prod
echo "== control: AZ_AVK_NO_VISIBLE_CLIP=1 =="
run_case noclip

P="$BASE/prod/covered.json"
N="$BASE/noclip/covered.json"
PB="$(field "$P" shm_upload_bytes)"
NB="$(field "$N" shm_upload_bytes)"
PSAVE="$(field "$P" visible_saved_px)"
PCLIP="$(field "$P" visible_clipped)"
PREP="$(field "$P" visible_repairs)"
PREPPX="$(field "$P" visible_repair_px)"
PFAIL="$(field "$P" visible_repair_failed)"
AFTREP="$(field "$BASE/prod/after.json" visible_repairs)"

echo
echo "── 1. the premise: the fixture copies, and covers ────────────────────"
echo "  control copied $NB B while the client was hidden"
hl_assert "PREMISE: the hidden client's generations were copied at all" \
	"$([ "${NB:-0}" -gt 2000000 ] && echo true || echo false)" "true"
VIS_EARLY="$(( $(count_colour "$BASE/prod/visible.png" "$BOTTOM_EARLY") \
	+ $(count_colour "$BASE/prod/visible.png" "$BOTTOM_FINAL") ))"
COV_EARLY="$(( $(count_colour "$BASE/prod/covered.png" "$BOTTOM_EARLY") \
	+ $(count_colour "$BASE/prod/covered.png" "$BOTTOM_FINAL") ))"
echo "  bottom window on screen: $VIS_EARLY px before, $COV_EARLY px once covered"
hl_assert "PREMISE: the bottom window was on screen to begin with" \
	"$([ "${VIS_EARLY:-0}" -gt 100000 ] && echo true || echo false)" "true"
hl_assert "PREMISE: and the opaque window then hid all of it" \
	"$([ "${COV_EARLY:-0}" -eq 0 ] && echo true || echo false)" "true"

echo
echo "── 2. the claim: a covered client is not copied ──────────────────────"
echo "  production $PB B vs control $NB B; $PCLIP plans clipped, $PSAVE px saved"
hl_assert "the copy collapsed against the control" \
	"$([ "${PB:-999999999}" -lt $(( ${NB:-0} / 4 )) ] && echo true || echo false)" "true"
hl_assert "and the clip is what did it" \
	"$([ "${PSAVE:-0}" -gt 1000000 ] && echo true || echo false)" "true"

echo
echo "── 3. THE ORACLE: uncovering shows what the client last sent ─────────"
for c in prod noclip; do
	EARLY="$(count_colour "$BASE/$c/uncovered.png" "$BOTTOM_EARLY")"
	FINAL="$(count_colour "$BASE/$c/uncovered.png" "$BOTTOM_FINAL")"
	echo "  $c: $FINAL px of the final generation, $EARLY px of the earlier one"
	hl_assert "$c: the uncovered window shows generation 6, not generation 1" \
		"$([ "${FINAL:-0}" -gt 500000 ] && [ "${EARLY:-0}" -lt 1000 ] \
			&& echo true || echo false)" "true"
done
echo "  repairs: $PREP ($PREPPX px) during the covered period, $AFTREP after uncovering"
hl_assert "the repair path is what carried it, and none of them failed" \
	"$([ "${PFAIL:-1}" -eq 0 ] && [ "${AFTREP:-0}" -gt 0 ] && echo true || echo false)" "true"

echo
echo "── 4. the shape it exists for: an opaque child over most of a frame ──"
echo "== framed =="
run_frame_case frame
echo "== framed control: AZ_AVK_NO_VISIBLE_CLIP=1 =="
run_frame_case frame-noclip

FB="$(field "$BASE/frame/stats.json" shm_upload_bytes)"
FNB="$(field "$BASE/frame-noclip/stats.json" shm_upload_bytes)"
FSAVE="$(field "$BASE/frame/stats.json" visible_saved_px)"
FREPF="$(field "$BASE/frame/stats.json" visible_repair_failed)"
CHILD="$(count_colour "$BASE/frame/framed.png" "#202020")"
MARGIN_E="$(count_colour "$BASE/frame/framed.png" "$BOTTOM_EARLY")"
MARGIN_F="$(count_colour "$BASE/frame/framed.png" "$BOTTOM_FINAL")"
echo "  control $FNB B vs production $FB B, $FSAVE px saved"
echo "  on screen: child $CHILD px, visible frame margin $(( MARGIN_E + MARGIN_F )) px"
hl_assert "PREMISE: the child really is drawn over the parent" \
	"$([ "${CHILD:-0}" -gt 100000 ] && echo true || echo false)" "true"
hl_assert "PREMISE: and the parent's margin really is still showing" \
	"$([ $(( MARGIN_E + MARGIN_F )) -gt 3000 ] && echo true || echo false)" "true"
hl_assert "PREMISE: the control copied the whole frame every generation" \
	"$([ "${FNB:-0}" -gt 8000000 ] && echo true || echo false)" "true"
hl_assert "the copy shrank to the part that is not covered" \
	"$([ "${FB:-999999999}" -lt $(( ${FNB:-0} / 2 )) ] && echo true || echo false)" "true"
hl_assert "and nothing had to be repaired after the fact" \
	"$([ "${FREPF:-1}" -eq 0 ] && echo true || echo false)" "true"

echo
echo "── 5. a client that never presents the same buffer twice ─────────────"
echo "== churn =="
run_churn_case churn
echo "== churn control: AZ_AVK_NO_VISIBLE_CLIP=1 =="
run_churn_case churn-noclip

CB="$(field "$BASE/churn/stats.json" shm_upload_bytes)"
CNB="$(field "$BASE/churn-noclip/stats.json" shm_upload_bytes)"
CSUB="$(field "$BASE/churn/stats.json" shm_submit_failed)"
CSAVE="$(field "$BASE/churn/stats.json" visible_saved_px)"
echo "  control $CNB B vs production $CB B, $CSAVE px saved, $CSUB submit failures"
hl_assert "PREMISE: the churning client copied a great deal unclipped" \
	"$([ "${CNB:-0}" -gt 40000000 ] && echo true || echo false)" "true"
# Two thirds of the buffer is below its own window, so a copy that stops at
# what can be seen is about a third of one that does not.
hl_assert "a fresh image on every redraw is still clipped" \
	"$([ "${CB:-999999999}" -lt $(( ${CNB:-0} / 2 )) ] && echo true || echo false)" "true"
hl_assert "and the copy path refused none of them" \
	"$([ "${CSUB:-1}" -eq 0 ] && echo true || echo false)" "true"
# The point of the whole cohort: a partial first write must still SHOW.
CHURN_BG="$(count_colour "$BASE/churn/churn.png" "#808080")"
CHURN_BG_N="$(count_colour "$BASE/churn-noclip/churn.png" "#808080")"
echo "  wallpaper still showing: $CHURN_BG px clipped, $CHURN_BG_N px unclipped"
hl_assert "and the window is on screen, not a hole where one should be" \
	"$([ "${CHURN_BG:-0}" -le $(( ${CHURN_BG_N:-0} + 2000 )) ] \
		&& echo true || echo false)" "true"

echo
echo "logs: $BASE"
hl_summary
