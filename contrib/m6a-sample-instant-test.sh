#!/usr/bin/env bash
# m6a-sample-instant-test.sh -- falsifier I1 (ADR-606).
#
# THE INVARIANT: every animated object in a pass samples the instant that pass
# was ARMED with -- the output's predicted presentation time -- and not a clock
# it read itself.
#
# Before M6A this was false in five places at once, and the symptom was
# nothing: a frame showed the state it had when the CPU walked it rather than
# the state it should have when it lit up, which looks like a correct animation
# running slightly behind. There is no pixel to assert on, so the invariant is
# asserted directly -- sample_ns == target_ns, counted.
#
# AND THE BREAK MUST MAKE IT FAIL. AZ_BREAK_PRESENT_SAMPLE_NOW restores exactly
# the old behaviour. A run in which the break leaves this green is a test that
# has stopped looking, not a compositor that is right.
set -u
. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="m6a-sample-instant"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m6a-sample-$$"
mkdir -p "$OUTDIR"

CFG="borderpx 4
border_radius 8
animations 1"

# One arm, parameterised by whether the break is on.
arm() { # arm TAG [env...]
	local tag="$1"; shift
	local dir="$OUTDIR/$tag"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"
	HL_ENV="$*"
	export HL_OUTDIR HL_ENV
	hl_start "$CFG" >/dev/null 2>&1
	sleep 2
	# Something must actually animate, or the pass count stays at whatever
	# idle produces and the arm proves nothing.
	hl_spawn_kitty "w1-$tag" >/dev/null 2>&1
	hl_wait_client_count 1 40 >/dev/null 2>&1
	local i
	for i in 1 2 3 4; do
		hl_dispatch "view,2" 0.6
		hl_dispatch "view,1" 0.6
	done
	sleep 1
	hl_get "get presentation" \
		| jq -r '"\(.sample_passes) \(.sample_not_target)"'
	hl_stop >/dev/null 2>&1
}

echo "══ I1: the sampled instant IS the armed target ══"
echo

read -r OK_PASSES OK_BAD <<<"$(arm correct)"
echo "  correct build: $OK_PASSES passes, $OK_BAD sampled something else"

# THE PREMISE. A run in which nothing sampled anything would report 0 bad and
# pass while testing nothing at all -- the failure mode this project keeps
# meeting. Assert that the instrument saw work before trusting its zero.
hl_assert_true "PREMISE: passes actually sampled an instant ($OK_PASSES)" \
	"$([ "${OK_PASSES:-0}" -gt 100 ] && echo true || echo false)"
hl_assert "every pass sampled the armed target" "${OK_BAD:-x}" 0

echo
read -r BRK_PASSES BRK_BAD <<<"$(arm broken AZ_BREAK_PRESENT_SAMPLE_NOW=1)"
echo "  broken build:  $BRK_PASSES passes, $BRK_BAD sampled something else"
hl_assert_true "PREMISE: the broken arm sampled too ($BRK_PASSES)" \
	"$([ "${BRK_PASSES:-0}" -gt 100 ] && echo true || echo false)"
hl_assert_true "BREAK: sampling CPU-now is DETECTED ($BRK_BAD > 0)" \
	"$([ "${BRK_BAD:-0}" -gt 0 ] && echo true || echo false)"

echo
echo "logs: $OUTDIR"
hl_summary
STATUS=$?
exit "$STATUS"
