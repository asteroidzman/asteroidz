#!/usr/bin/env bash
# m6a-retarget-test.sh -- ADR-608: position continuity across a retarget.
#
# THE INVARIANT, and it is mandatory rather than nice: when a new target arrives
# while a move is still in flight, the window must carry on from WHERE IT IS.
# Seeding the new curve from the original start instead of the interpolated
# position makes the window jump backwards to where it set off, mid-motion, and
# then set off again -- the single most visible animation defect there is.
#
# The compositor's own comment at the retarget site says this is where it would
# go wrong: "whether `initial` is the interpolated position or the original
# one". Reading that line says it is currently right. This proves it, because a
# line of code is a claim and a measurement is evidence.
#
# HOW: drive a move, interrupt it partway with a different target, and sample
# the window's position across the seam. A discontinuity is a step far larger
# than one frame's worth of travel.
set -u
. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="m6a-retarget"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m6a-retarget-$$"
mkdir -p "$OUTDIR"

DUR="${DUR:-1200}"     # long, so there is plenty of flight to interrupt
CFG="borderpx 4
border_radius 8
animations 1
animation_duration_move $DUR
animation_duration_tag $DUR
animation_duration_open $DUR"

# Poll the window's PRESENTED x as fast as the IPC allows, one sample per line.
#
# anim_x, not x. `x` is the semantic target and jumps to it the instant a move
# is dispatched -- polling it during an animation gives a step function whose
# largest "step" is just the size of the move. The first version of this
# fixture did exactly that and reported a 1200px discontinuity that was really
# the retarget arriving. The premise assertion (the window must be MOVING
# between samples) is what caught it: typical step 0px.
sample_x() { # sample_x MILLISECONDS
	local ms="$1" end
	end=$(( $(date +%s%N) / 1000000 + ms ))
	while [ "$(( $(date +%s%N) / 1000000 ))" -lt "$end" ]; do
		hl_get "get all-clients" \
			| jq -r '.clients[0]? | select(.) | .anim_x' 2>/dev/null
	done
}

run() { # run TAG [env...]
	local tag="$1"; shift
	local dir="$OUTDIR/$tag"; mkdir -p "$dir"
	HL_OUTDIR="$dir"
	HL_ENV="ASTEROIDZ_RENDERER=avk $*"
	export HL_OUTDIR HL_ENV
	hl_start "$CFG" >/dev/null 2>&1
	sleep 2
	HL_KITTY_EXTRA="-o cursor_blink_interval=0" \
		hl_spawn_kitty "w-$tag" >/dev/null 2>&1
	hl_wait_client_count 1 40 >/dev/null 2>&1
	hl_dispatch toggle_floating 1 >/dev/null 2>&1
	hl_dispatch "move_window,100,100" 1 >/dev/null 2>&1
	sleep 1

	# Send it a long way, let it get properly under way, then change the target
	# while it is still flying.
	hl_dispatch "move_window,1400,100" 0 >/dev/null 2>&1
	sample_x 400 > "$dir/leg1.txt"
	hl_dispatch "move_window,200,100" 0 >/dev/null 2>&1
	sample_x 700 > "$dir/leg2.txt"
	hl_stop >/dev/null 2>&1

	cat "$dir/leg1.txt" "$dir/leg2.txt" > "$dir/all.txt"
	python3 - "$dir/all.txt" <<'PY'
import sys
xs = [int(l) for l in open(sys.argv[1]) if l.strip().lstrip('-').isdigit()]
if len(xs) < 8:
    print("0 0 0"); sys.exit()
steps = [abs(b - a) for a, b in zip(xs, xs[1:])]
moved = [d for d in steps if d]
# NOT the median. The poll runs at ~2.3ms while frames land at 16.7ms, so most
# consecutive samples are legitimately identical and the median step is 0 by
# construction -- an earlier version asserted on it and failed a healthy build
# for it. What the premise actually needs is that motion HAPPENED: how far in
# total, and across how many distinct steps.
#   max    largest single jump; a retarget that reset to the origin puts
#          hundreds of px here
#   moved  how many samples moved at all
#   total  cumulative travel
print(f"{max(steps)} {len(moved)} {sum(steps)}")
PY
}

echo "══ ADR-608: a retarget continues from where the window IS ══"
echo "  move 100->1400, interrupted at ~400ms with a new target of 200"
echo

read -r JUMP MOVED TOTAL <<<"$(run correct)"
echo "  correct build: largest step ${JUMP}px, ${MOVED} samples moved, ${TOTAL}px travelled"

hl_assert_true "PREMISE: the window actually travelled (${TOTAL}px > 500)" \
	"$([ "${TOTAL:-0}" -gt 500 ] && echo true || echo false)"
hl_assert_true "PREMISE: over many distinct steps (${MOVED} > 20)" \
	"$([ "${MOVED:-0}" -gt 20 ] && echo true || echo false)"
# A discontinuity would be a step back toward the origin -- hundreds of px.
# Ordinary travel between two IPC polls is tens.
hl_assert_true "no position discontinuity across the retarget (${JUMP}px < 200)" \
	"$([ "${JUMP:-9999}" -lt 200 ] && echo true || echo false)"


# ── AND THE BREAK MUST MAKE IT FAIL ──────────────────────────────────────
#
# Everything above passes on a compositor whose windows barely move, and on one
# whose retarget happens to be smooth for unrelated reasons. What makes the
# 42px mean "continuous" is that a build which deliberately restarts from the
# previous origin is caught. AZ_BREAK_ANIM_RETARGET_POSITION_RESET leaves
# animainit_geom holding the old start, so the second leg begins from x=100
# instead of from wherever the window had flown to -- a backwards jump of
# however far it had travelled, several hundred pixels here.
echo
echo "── BREAK: retarget restarts from the previous origin"
read -r BJUMP BMOVED BTOTAL <<<"$(run broken AZ_BREAK_ANIM_RETARGET_POSITION_RESET=1)"
echo "  broken build:  largest step ${BJUMP}px, ${BMOVED} samples moved, ${BTOTAL}px travelled"
hl_assert_true "PREMISE: the broken arm moved too (${BTOTAL}px > 500)" \
	"$([ "${BTOTAL:-0}" -gt 500 ] && echo true || echo false)"
hl_assert_true "BREAK: the discontinuity is DETECTED (${BJUMP}px >= 200)" \
	"$([ "${BJUMP:-0}" -ge 200 ] && echo true || echo false)"

echo
echo "logs: $OUTDIR"
hl_summary
STATUS=$?
exit "$STATUS"
