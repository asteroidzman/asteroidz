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
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TESTS_DIR="$REPO/contrib/regression/tests"
. "$REPO/contrib/lib/headless.sh"

MODULES=("$@")
if [ "${#MODULES[@]}" -eq 0 ]; then
	MODULES=()
	for f in "$TESTS_DIR"/*.sh; do
		MODULES+=("$(basename "$f" .sh)")
	done
fi

hl_start
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
