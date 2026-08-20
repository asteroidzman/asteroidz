#!/usr/bin/env bash
# avk-gradient-border-test.sh -- the BORDER gradient, which had no coverage.
#
# ── WHY THIS IS THE ONLY GRADIENT FIXTURE IN contrib/ ────────────────────
#
# asteroidz has exactly two gradient consumers: the overview vignette and
# client_set_border_fill(). The vignette had a fixture of its own,
# avk-gradient-test.sh, which compared it against the same scene rendered by
# SceneFX -- and it never once scored anything, because a headless overview
# does not create its vignettes (guarded by m->ov_main_wp, overview.h;
# gradient_draws reads 0 with the overview open). With SceneFX gone it had no
# reference arm either, so it was deleted rather than given an oracle for a
# scene that cannot be produced. See docs/regression-testing.md.
#
# That leaves the border gradient -- the one an actual desktop turns on -- and
# it went untested for a different reason: `border_gradient 1` used to make
# the compositor unresponsive within seconds. wlr_scene_rect_set_gradient()
# had no dirty check while being called from the per-frame path, so a focused
# window with a gradient border re-damaged itself forever: ~54 MB/s of heap
# and 100% of a core. That is fixed in subprojects/asteroidz-scenefx now, the
# border gradient became testable, and nobody went back for it until this.
#
# Nothing here needs a second renderer: the control is the SAME build with the
# gradient turned off. tests/test-avk-gradient.c carries the arithmetic, a CPU
# model of gradient.frag at every count, angle, origin and mode.
#
# ── WHAT IS ASSERTED ─────────────────────────────────────────────────────
#
# 1. the configured gradient REACHES THE RENDERER      gradient_draws > 0
# 2. it is the LINEAR path with the right stop count   linear_draws, colors
# 3. it is VISIBLE, and where the border is            pixel comparison
# 4. it does not livelock                              frame count is bounded
# 5. a break that corrupts it fails the pixel oracle   AZ_GRADIENT_FIRST_COLOR
#
# 4 is not decoration. The defect that kept this untested was a runaway frame
# loop, and a suite that proves the gradient draws while saying nothing about
# how often would have passed on the broken build too -- enthusiastically.
#
# HEADLESS ABSOLUTE MICROSECONDS ARE NOT BUDGET EVIDENCE. Counts and pixels are.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-gradient-border"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-gradborder-$$"
mkdir -p "$OUTDIR"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

W="${W:-1920}"; H="${H:-1080}"
# A FAT BORDER. The gradient lives in the border annulus and nowhere else, so at
# the desktop's 2px it is 2 pixels wide at scale 1 -- a difference that a
# tolerance would swallow whole. 16 gives the ramp room to be wrong in.
BORDER="${BORDER:-16}"
SETTLE="${SETTLE:-4}"

# color2 is deliberately FAR from the theme's border colour. A gradient between
# two similar colours is a solid fill to within any tolerance worth having, and
# an oracle that cannot tell those apart is an oracle that passes on a build
# with no gradient at all.
grad_cfg() { # grad_cfg <enable>
	echo "border_radius 0
borderpx $BORDER
effects { shadow { enable 0 } blur { enable 0 } }
layout { border { width $BORDER; gradient { enable $1; angle 45
      color2 0xff00ffff } } }"
}

export WLBGEFFECT_SSD=1

JQ='{frames:.frames, draws:.gradient_draws, lin:.gradient_linear_draws,
     conic:.gradient_conic_draws, colors:.gradient_colors_processed,
     uploads:.gradient_buffer_uploads, grows:.gradient_buffer_grows,
     verr:.validation_errors, waits:.cpu_sync_waits}'
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
STATS=""

run() { # run NAME ENABLE [EXTRA_ENV...]
	local name="$1" en="$2"; shift 2
	local dir="$OUTDIR/$name" cdir="$OUTDIR/$name-cap"
	mkdir -p "$dir" "$cdir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"
	HL_ENV="AZ_AVK_CAPTURE_DIR=$cdir $*"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT
	hl_start "$(grad_cfg "$en")" >/dev/null 2>&1
	hl_spawn_wlbgeffect gb 300 gb >/dev/null
	hl_wait_client_count 1 200
	# SETTLE FIRST, THEN RESET, THEN COUNT. The focus animation runs on open and
	# legitimately re-damages the border every tick, so a window counted from the
	# moment it maps cannot distinguish the animation from a runaway loop -- and
	# the runaway loop is one of the things being tested for.
	sleep "$SETTLE"
	hl_dispatch reset_avk_stats 1
	# ── READING 1: IDLE ──────────────────────────────────────────────────
	# Nothing moves, nothing is damaged. A healthy compositor renders nothing.
	sleep "$SETTLE"
	IDLE="$(hl_get "get avk-stats" | jq -r "$JQ | to_entries |
		map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null)"
	# ── READING 2: DAMAGED ───────────────────────────────────────────────
	# The same counters, over an interval where frames are DEMANDED. Without
	# this pair, "idle drew nothing" is indistinguishable from "the counter is
	# dead", and an assertion against a dead counter passes on every build.
	local d=0
	while [ "$d" -lt 3 ]; do hl_dispatch damage_all; sleep 0.4; d=$(( d + 1 )); done
	sleep 1
	STATS="$(hl_get "get avk-stats" | jq -r "$JQ | to_entries |
		map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null)"
	hl_dispatch capture_output 1
	sleep 1
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	hl_stop >/dev/null 2>&1
}
diffpx() { python3 "$PPM" diff "$OUTDIR/$1.ppm" "$OUTDIR/$2.ppm" 2>/dev/null \
	|| echo "-1 -1"; }

echo "══ border gradient ══ ${W}x${H} border ${BORDER}px angle 45"
echo

echo "── OFF: the control ──────────────────────────────────────────────────"
run off 0
OFF="$STATS"; OFF_IDLE="$IDLE"
echo "  frames=$(v frames "$OFF") gradient draws=$(v draws "$OFF")"
# THE PREMISE, AND IT IS THE WHOLE POINT OF THE CONTROL. If the OFF arm already
# draws gradients then this fixture is measuring something else -- the overview
# vignette, say -- and every assertion below would pass without the border ever
# being involved.
hl_assert "PREMISE: with the gradient off, nothing draws one" "$(v draws "$OFF")" 0

echo
echo "── ON: does the configured gradient reach the renderer? ──────────────"
run on 1
ON="$STATS"; ON_IDLE="$IDLE"
echo "  frames=$(v frames "$ON") draws=$(v draws "$ON") linear=$(v lin "$ON")" \
	"conic=$(v conic "$ON") colors=$(v colors "$ON")"
echo "  buffer: uploads=$(v uploads "$ON") grows=$(v grows "$ON")"
hl_assert_true "THE CONFIGURED GRADIENT DRAWS (draws=$(v draws "$ON") > 0)" \
	"$([ "$(v draws "$ON")" -gt 0 ] && echo true || echo false)"
# The border gradient is 2 stops, linear, blended. A conic draw here would mean
# the wrong path ran and produced a picture that happens to look plausible.
hl_assert "every draw took the LINEAR path" "$(v draws "$ON")" "$(v lin "$ON")"
hl_assert "and none took the conic path" "$(v conic "$ON")" 0
# 2 colours per draw, exactly. A count that is not 2x the draws means the stop
# list reaching the shader is not the one client_set_border_fill() built.
hl_assert "two stops per draw" "$(v colors "$ON")" "$(( $(v draws "$ON") * 2 ))"

echo
echo "── is it VISIBLE, and only in the border? ────────────────────────────"
read -r D M <<<"$(diffpx off on)"
awk -v d="$D" -v m="$M" -v w="$W" -v h="$H" -v b="$BORDER" 'BEGIN{
	if (d < 0) { print "  capture missing"; exit }
	printf "  gradient vs solid border: %d px differ (%.3f%% of the output), worst channel %d\n",
		d, 100*d/(w*h), m;
}'
hl_assert_true "the gradient CHANGES THE PICTURE ($D px)" \
	"$([ "${D:-0}" -gt 0 ] && echo true || echo false)"
# A ramp between two far-apart colours must move a channel a long way somewhere.
# Without this, one quantisation step in a corner would satisfy "it changed".
hl_assert_true "and changes it by a lot somewhere (worst channel $M)" \
	"$([ "${M:-0}" -ge 32 ] && echo true || echo false)"

echo
echo "── IT MUST NOT LIVELOCK ──────────────────────────────────────────────"
#
# THE DEFECT THAT KEPT THIS FEATURE UNTESTED, AND WHY IT NEEDS TWO READINGS.
#
# wlr_scene_rect_set_gradient() had no dirty check while being called from the
# per-frame path, so a focused window with a gradient border re-damaged itself
# forever: ~54 MB/s of heap, 100% of a core, IPC never answering again. The
# scenefx fix compares the whole gradient and returns on an identical write.
#
# An idle interval alone cannot test this, because "0 frames" is also what a
# dead counter reads. So both arms are measured twice: once idle, where a
# healthy build renders nothing, and once with damage demanded, where it must
# render. The second reading is what makes the first one mean something.
FOFF_I="$(v frames "$OFF_IDLE")"; FON_I="$(v frames "$ON_IDLE")"
FOFF_A="$(v frames "$OFF")";      FON_A="$(v frames "$ON")"
GON_I="$(v draws "$ON_IDLE")";    GON_A="$(v draws "$ON")"
echo "  frames  idle(${SETTLE}s) -> damaged:  off $FOFF_I -> $FOFF_A   on $FON_I -> $FON_A"
echo "  gradient draws                        on $GON_I -> $GON_A"
# THE INSTRUMENT IS ALIVE. Both must move when frames are demanded, or every
# zero above is a zero from a counter nobody is writing.
hl_assert_true "PREMISE: demanding damage DOES produce frames (off $FOFF_I->$FOFF_A)" \
	"$([ "$FOFF_A" -gt "$FOFF_I" ] && echo true || echo false)"
hl_assert_true "PREMISE: and gradient draws move with them ($GON_I->$GON_A)" \
	"$([ "$GON_A" -gt "$GON_I" ] && echo true || echo false)"
# AND THE IDLE INTERVAL IS QUIET. On the broken build this was hundreds of
# frames of a window nobody touched.
hl_assert "a gradient border repaints NOTHING while idle" "$FON_I" 0
hl_assert "and draws no gradient while idle" "$GON_I" 0

# ── AND THE ASSERTION IS FALSIFIABLE ─────────────────────────────────────
# AZ_GRADIENT_NOOP_DAMAGE=1 removes scenefx's identical-write check and
# restores the original bug exactly. Run by default rather than behind a BREAK=
# flag, because "the livelock is fixed" is a claim about a thing that already
# shipped broken once, and an unrun falsifier is a comment.
#
# The arm is bounded so a wedged compositor cannot hang the suite: the run
# function's IPC calls carry their own timeouts and a hung instance simply
# reports nothing, which fails the premise below rather than blocking.
run storm 1 AZ_GRADIENT_NOOP_DAMAGE=1
STORM_I="$IDLE"
echo "  BREAK idle frames: $(v frames "$STORM_I") (correct build: $FON_I)"
hl_assert_true "BREAK: an identical gradient write repaints forever ($(v frames "$STORM_I") idle frames)" \
	"$([ "$(v frames "$STORM_I")" -gt 30 ] && echo true || echo false)"

echo
echo "── BREAK: corrupt the ramp ───────────────────────────────────────────"
# AZ_GRADIENT_FIRST_COLOR makes every gradient a flat fill of its first stop.
# The pixel oracle must fail on it -- otherwise "the gradient changed the
# picture" was measuring the existence of a border, not the ramp inside it.
run break 1 AZ_GRADIENT_FIRST_COLOR=1
BRK="$STATS"
echo "  break: draws=$(v draws "$BRK") linear=$(v lin "$BRK")"
hl_assert_true "PREMISE: the break still DREW gradients" \
	"$([ "$(v draws "$BRK")" -gt 0 ] && echo true || echo false)"
read -r DB MB <<<"$(diffpx on break)"
echo "  correct vs flattened ramp: $DB px differ (worst channel $MB)"
hl_assert_true "BREAK: a flattened ramp renders a different picture ($DB px)" \
	"$([ "${DB:-0}" -gt 0 ] && echo true || echo false)"

echo
echo "── soundness ─────────────────────────────────────────────────────────"
hl_assert "gradient on: validation errors" "$(v verr "$ON")" 0
hl_assert "gradient off: validation errors" "$(v verr "$OFF")" 0
hl_assert "gradient on: CPU sync waits" "$(v waits "$ON")" 0

echo
echo "logs: $OUTDIR"
hl_summary
