#!/usr/bin/env bash
# m6a-mixed-refresh-test.sh -- ADR-607 statement 3, and falsifier I4.
#
# THE STATEMENT: a semantic animation completes at the same WALL-CLOCK time on
# every output, whatever each output's refresh rate is. One trajectory X(t),
# sampled per output at that output's own instant -- so 144Hz shows ~2.4x more
# intermediate positions than 60Hz and both finish together.
#
# WHY THIS IS THE TEST WORTH HAVING. An animation that secretly advanced by
# frame COUNT rather than by elapsed time renders perfectly at one refresh rate
# and is wrong at every other -- and the desktop it was developed on has one
# rate, so it looks right forever.
#
# AND WHY IT COMPARES TOTAL RATES, NOT WHICH OUTPUT IS FASTER. rendermon walks
# EVERY client on EVERY output's pass, so a frame-stepped animation advances at
# the SUM of the outputs' refresh rates. Swapping 144/60 for 60/144 leaves that
# sum at 204Hz either way -- an earlier version of this file did exactly that
# and could not have detected the defect it was named after. The arms therefore
# differ in total tick rate: 144+60 against 60+60.
#
# It is also the one mixed-refresh statement that does NOT need a vblank
# sequence, which matters: the headless backend honours a custom refresh in its
# frame timer (144Hz and 60Hz outputs presented 2305 and 874 frames, a 2.64
# ratio) but never fills ev->seq, so period and cadence read 0 there by
# absence. Wall-clock completion needs neither.
set -u
. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="m6a-mixed-refresh"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m6a-mixed-$$"
mkdir -p "$OUTDIR"

DUR="${DUR:-700}"          # animation duration, ms
CFG="borderpx 4
border_radius 8
animations 1
animation_duration_move $DUR
animation_duration_tag $DUR
animation_duration_open $DUR
animation_duration_close $DUR"

# How long a tag animation actually takes, measured from the compositor's own
# report that it still needs frames. Not from a screenshot: this is a question
# about time, and a pixel comparison would answer a different one.
measure() { # measure HZ1 HZ2 [env...] -> milliseconds
	local hz1="$1" hz2="$2"; shift 2
	local dir="$OUTDIR/hz$hz1-$hz2"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"; HL_OUTPUTS=2; HL_HZ1="$hz1"; HL_HZ2="$hz2"
	HL_ENV="ASTEROIDZ_RENDERER=avk $*"
	export HL_OUTDIR HL_OUTPUTS HL_HZ1 HL_HZ2 HL_ENV
	hl_start "$CFG" >/dev/null 2>&1
	sleep 2
	hl_spawn_kitty "w-$hz1" >/dev/null 2>&1
	hl_wait_client_count 1 40 >/dev/null 2>&1
	sleep 1

	# Three runs, median reported: a single run picks up whatever the machine
	# was doing, and this is a millisecond-scale measurement.
	local out=""
	local r
	for r in 1 2 3; do
		local t0 t1
		t0=$(date +%s%N)
		hl_dispatch "view,2" 0 >/dev/null 2>&1
		# Poll the animation's own liveness rather than sleeping a fixed time,
		# which would measure the sleep.
		local waited=0
		while [ "$waited" -lt 4000 ]; do
			if [ "$(hl_get 'get all-clients' \
					| jq -r '[.clients[]?|select(.animating==true)]|length' \
					2>/dev/null || echo 0)" = "0" ]; then
				break
			fi
			sleep 0.01
			waited=$(( waited + 10 ))
		done
		t1=$(date +%s%N)
		out="$out $(( (t1 - t0) / 1000000 ))"
		hl_dispatch "view,1" 0.5 >/dev/null 2>&1
		sleep 0.4
	done
	hl_stop >/dev/null 2>&1
	# median of three
	echo $out | tr ' ' '\n' | grep -v '^$' | sort -n | sed -n 2p
}

echo "══ ADR-607 statement 3: completion is wall-clock, not frame-count ══"
echo "  configured animation duration: ${DUR}ms"
echo

A=$(measure 144 60)
echo "  144Hz + 60Hz (204Hz of ticks): ${A}ms"
B=$(measure 60 60)
echo "   60Hz +  60Hz (120Hz of ticks): ${B}ms"
echo

# PREMISE FIRST. If the animation never ran, both numbers are the poll's floor
# and their agreement means nothing at all.
hl_assert_true "PREMISE: the animation actually took time at 204Hz (${A}ms)" \
	"$([ "${A:-0}" -ge 200 ] && echo true || echo false)"
hl_assert_true "PREMISE: and at 120Hz (${B}ms)" \
	"$([ "${B:-0}" -ge 200 ] && echo true || echo false)"

# The two must agree within one 60Hz period plus slack for the 10ms poll.
# Frame stepping at 2.4x the rate would put them a factor apart, not a few
# milliseconds -- so this tolerance is wide and the defect is still caught.
DIFF=$(( A > B ? A - B : B - A ))
echo "  difference: ${DIFF}ms"
hl_assert_true "completion time is refresh-independent (${DIFF}ms <= 60ms)" \
	"$([ "$DIFF" -le 60 ] && echo true || echo false)"

# And near the configured duration, so a run in which BOTH were wrong by the
# same factor -- which a frame-count defect at equal rates would produce --
# cannot pass.
for pair in "204:$A" "120:$B"; do
	hz=${pair%%:*}; ms=${pair##*:}
	off=$(( ms > DUR ? ms - DUR : DUR - ms ))
	hl_assert_true "${hz}Hz of ticks completes near ${DUR}ms (${ms}ms)" \
		"$([ "$off" -le 250 ] && echo true || echo false)"
done


# ── AND THE BREAK MUST MAKE IT FAIL ──────────────────────────────────────
#
# Everything above passes on a compositor that never animated at all, and it
# passes on one whose durations happen to be right for other reasons. The only
# thing that makes the numbers mean "wall-clock, not frame-count" is that a
# frame-counting build is caught. AZ_BREAK_ANIM_FRAME_STEP derives progress
# from a count of ticks rather than elapsed time, so the 204Hz arm should reach
# the end in roughly 120/204 of the wall time the 120Hz arm needs.
echo
echo "── BREAK: advance by frame count instead of elapsed time"
BA=$(measure 144 60 AZ_BREAK_ANIM_FRAME_STEP=1)
BB=$(measure 60 60 AZ_BREAK_ANIM_FRAME_STEP=1)
BDIFF=$(( BA > BB ? BA - BB : BB - BA ))
echo "  broken: 204Hz ${BA}ms vs 120Hz ${BB}ms  (difference ${BDIFF}ms)"
hl_assert_true "BREAK: frame stepping makes completion depend on refresh (${BDIFF}ms > 60ms)" \
	"$([ "$BDIFF" -gt 60 ] && echo true || echo false)"

echo
echo "logs: $OUTDIR"
hl_summary
STATUS=$?
exit "$STATUS"
