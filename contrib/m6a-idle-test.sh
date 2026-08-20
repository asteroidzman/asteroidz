#!/usr/bin/env bash
# m6a-idle-test.sh -- ADR-610, falsifier I5: idle stays idle.
#
# THE CONSTRAINT: a settled desktop presents nothing. No prediction timer, no
# phase-maintenance tick, no feedback poll. A predictor is a standing
# temptation to add "just one" refresh timer, and the cost of that temptation
# is paid by every idle machine, all the time, forever.
#
# The guarantee is structural rather than behavioural -- the presenter owns no
# wl_event_source, so there is no object through which it could wake the loop.
# A structural guarantee is worth exactly as much as the test that tries to
# violate it, which is why AZ_BREAK_PRESENT_IDLE_WAKE adds the thing the design
# says does not exist.
#
# Counting PRESENTS rather than parsing a trace: a frame that was scheduled and
# then coalesced away costs nothing and is not a violation. What matters is
# whether the display was made to do work.
set -u
. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="m6a-idle"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m6a-idle-$$"
mkdir -p "$OUTDIR"

SETTLE="${SETTLE:-4}"      # seconds to let the desktop go quiet
WATCH="${WATCH:-12}"       # seconds of observation

CFG="borderpx 4
border_radius 8
animations 1"

# presents observed across WATCH seconds of doing nothing at all.
watch_idle() { # watch_idle TAG [env...] -> "<presented> <frames>"
	local tag="$1"; shift
	local dir="$OUTDIR/$tag"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"
	HL_ENV="$*"
	export HL_OUTDIR HL_ENV
	hl_start "$CFG" >/dev/null 2>&1
	sleep 2
	# A window, because an empty desktop is a weaker claim: the interesting
	# case is a compositor with something to draw that correctly decides not
	# to redraw it.
	#
	# WITH ITS CURSOR BLINK OFF, and that is the whole difficulty of this
	# fixture. A terminal blinks its cursor about once a second, each blink is
	# a client commit, and a commit MUST be presented -- so the first version
	# of this test measured 12 presents in 12s and called the compositor
	# non-idle. It was not: an empty desktop presented 0 in 10s while the same
	# desktop with a blinking kitty presented 11. The client was awake, which
	# is correct behaviour and not what ADR-610 is about.
	HL_KITTY_EXTRA="-o cursor_blink_interval=0" \
		hl_spawn_kitty "w-$tag" >/dev/null 2>&1
	hl_wait_client_count 1 40 >/dev/null 2>&1
	# Let everything settle: map animations, the first blur build, the shell.
	sleep "$SETTLE"

	hl_dispatch reset_presentation 0 >/dev/null 2>&1
	local f0
	f0=$(hl_get "get avk-stats" | jq -r '.frames')
	sleep "$WATCH"
	local pres f1
	pres=$(hl_get "get presentation" | jq -r '[.outputs[].presented]|add // 0')
	f1=$(hl_get "get avk-stats" | jq -r '.frames')
	hl_stop >/dev/null 2>&1
	echo "$pres $(( f1 - f0 ))"
}

echo "══ ADR-610: a settled desktop presents nothing ══"
echo "  settle ${SETTLE}s, then watch ${WATCH}s"
echo

read -r OK_PRES OK_FRAMES <<<"$(watch_idle quiet)"
echo "  healthy: $OK_PRES presents, $OK_FRAMES frames rendered over ${WATCH}s"
hl_assert "idle presents nothing" "${OK_PRES:-x}" 0
hl_assert "idle renders nothing" "${OK_FRAMES:-x}" 0

echo
read -r BRK_PRES BRK_FRAMES <<<"$(watch_idle waker AZ_BREAK_PRESENT_IDLE_WAKE=1)"
echo "  broken:  $BRK_PRES presents, $BRK_FRAMES frames rendered over ${WATCH}s"
# ~one per second per output. Demanding a specific count would be asserting the
# break's implementation; demanding "more than a couple" asserts that idle
# stopped being idle, which is the property.
hl_assert_true "BREAK: a wake timer is DETECTED ($BRK_PRES presents > 2)" \
	"$([ "${BRK_PRES:-0}" -gt 2 ] && echo true || echo false)"

echo
echo "logs: $OUTDIR"
hl_summary
STATUS=$?
exit "$STATUS"
