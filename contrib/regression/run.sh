#!/usr/bin/env bash
# run.sh — regression test runner for asteroidz's window-management/IPC
# surface. Boots ONE isolated headless compositor instance and runs every
# test_* function from contrib/regression/tests/*.sh against it (hl_reset
# between each so they can't leak state into one another), then prints a
# TAP-ish summary and exits non-zero if anything failed.
#
# Usage: contrib/regression/run.sh [module...]
#   module   one or more test file basenames without .sh (default: all of
#            contrib/regression/tests/*.sh), e.g. `run.sh layouts tags`
# Env: see contrib/lib/headless.sh (ASTEROIDZ, HL_OUTDIR, HL_WIDTH/HL_HEIGHT)
#   HL_LIVE=1   attach to the CALLER's own already-running compositor
#               instead of launching an isolated instance (see hl_start_live
#               in headless.sh). By default every dispatch is confined to a
#               fresh virtual monitor this creates, never a real output --
#               set HL_LIVE_MON=<name> (e.g. DP-1) to instead run directly
#               against that REAL, physically-connected monitor, disturbing
#               whatever's actually on it for the duration.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TESTS_DIR="$REPO/contrib/regression/tests"
. "$REPO/contrib/lib/headless.sh"

# Default order is alphabetical, with one exception: a module that declares
#
#     # harness: needs-second-monitor
#
# runs at the END, after every single-monitor module.
#
# All modules share one compositor, and create_virtual_output has no
# counterpart that removes just the output it made (destroy_all_virtual_output
# takes HEADLESS-1 with it under a headless backend). So a two-monitor module
# leaves a second monitor behind for everything that follows, and modules that
# assume one output start failing in ways that point nowhere near themselves:
# adding fullscreen-bleed put a monitor in place at "f", and keybind-combo and
# layer-shell -- untouched, six modules later -- began reporting a view that
# would not switch and a bar that reserved no space. Sorting them last means
# only modules that already tolerate two monitors ever see one.
#
# An explicit module list on the command line is run exactly as given: if you
# name them, you meant that order.
MODULES=("$@")
if [ "${#MODULES[@]}" -eq 0 ]; then
	MODULES=()
	LAST=()
	for f in "$TESTS_DIR"/*.sh; do
		mod="$(basename "$f" .sh)"
		if grep -q '^# harness: needs-second-monitor' "$f"; then
			LAST+=("$mod")
		else
			MODULES+=("$mod")
		fi
	done
	MODULES+=("${LAST[@]:-}")
	# ${LAST[@]:-} contributes an empty element when nothing is marked
	[ -n "${MODULES[-1]}" ] || unset 'MODULES[-1]'
fi

if [ "${HL_LIVE:-0}" = "1" ]; then
	hl_start_live
else
	hl_start
fi
trap hl_stop EXIT

for mod in "${MODULES[@]}"; do
	file="$TESTS_DIR/$mod.sh"
	if [ ! -f "$file" ]; then
		echo "run.sh: no such test module: $mod ($file)" >&2
		ASSERT_FAILURES+=("(harness): missing test module $mod")
		ASSERT_COUNT=$((ASSERT_COUNT + 1))
		continue
	fi
	echo "=== $mod ==="
	hl_notify "asteroidz live regression: module $mod" ""
	# shellcheck disable=SC1090
	. "$file"
	# test_* functions in FILE ORDER (declare -F would sort alphabetically)
	while IFS= read -r fn; do
		[ -n "$fn" ] || continue
		CURRENT_TEST="$mod:$fn"
		echo "-- $fn"
		hl_reset
		"$fn"
	done < <(grep -oE '^test_[a-zA-Z0-9_]+' "$file")
done

hl_summary
exit $?
