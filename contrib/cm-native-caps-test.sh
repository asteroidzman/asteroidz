#!/usr/bin/env bash
# cm-native-caps-test.sh -- the native wp-color-management manager must
# advertise EXACTLY what the wlroots one advertised.
#
# ── PHASE 1 OF NATIVE OWNERSHIP, AND ITS ONLY CLAIM ──────────────────────
#
# Two implementations of wp_color_manager_v1 exist in the tree and exactly one
# owns the global per session (AZ_NATIVE_CM chooses). The cutover must not be
# client-visible, so the whole of Phase 1 is one assertion: bind against each
# and compare the advertisement.
#
# ── COMPARED VERBATIM, IN ARRIVAL ORDER ──────────────────────────────────
#
# Not as a set. Two implementations advertising the same values in a different
# order are not the same advertisement to a client that takes the first value
# it recognises -- and a comparison that sorted first would call them equal.
# The observer prints each event as it arrives and this diffs the streams.
#
# ── WHY THIS IS THE RIGHT GATE FOR A PHASE THAT DOES NOTHING ─────────────
#
# Phase 1 implements the manager and refuses every object-creating request.
# There is no behaviour to assert. What CAN be asserted is that the thing a
# client learns at bind time is unchanged -- which is exactly the property the
# later phases will be free to break silently if nothing pins it now.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="cm-native-caps"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-cmcaps-$$"
mkdir -p "$OUTDIR"

BGE="$(cd "$(dirname "$0")" && pwd)/wlbgeffect/wlbgeffect"
[ -x "$BGE" ] || { echo "SKIP: wlbgeffect not built"; exit 77; }

echo "══ the native manager advertises what wlroots advertised ══"
echo

# One arm per implementation. Same everything else -- same renderer, same
# outputs, same client -- so the only difference is which code answers the bind.
run_arm() { # run_arm <name> <extra env>
	local name="$1" extra="$2"
	HL_OUTDIR="$OUTDIR/$name"; mkdir -p "$HL_OUTDIR"
	HL_ENV="ASTEROIDZ_RENDERER=avk $extra"
	export HL_OUTDIR HL_ENV
	export WLBGEFFECT_SSD=1
	hl_start "layout { titlebar { enable 0 } }" >"$HL_OUTDIR/start.log" 2>&1
	sleep 2
	hl_spawn_wlbgeffect "$name" 15 "$name" >/dev/null 2>&1
	sleep 4
	grep "^cmcap " "$HL_OUTDIR/$name.log" 2>/dev/null > "$OUTDIR/$name.caps"
	hl_stop >/dev/null 2>&1
	sleep 1
}

run_arm wlroots ""
run_arm native "AZ_NATIVE_CM=1"

W="$(wc -l < "$OUTDIR/wlroots.caps" 2>/dev/null || echo 0)"
N="$(wc -l < "$OUTDIR/native.caps" 2>/dev/null || echo 0)"
echo "  wlroots advertised $W line(s):"
sed 's/^/    /' "$OUTDIR/wlroots.caps" 2>/dev/null
echo "  native advertised $N line(s):"
sed 's/^/    /' "$OUTDIR/native.caps" 2>/dev/null
echo

# ── PREMISES ─────────────────────────────────────────────────────────────
# An empty advertisement compares equal to an empty advertisement, so "the two
# match" is satisfied by two compositors that said nothing. This is the same
# empty-list defect that once made every client unable to attach any image
# description at all -- and it was silent.
hl_assert_true "PREMISE: the wlroots arm advertised something ($W)" \
	"$([ "$W" -ge 4 ] && echo true || echo false)"
hl_assert_true "PREMISE: it named transfer functions" \
	"$(grep -q '^cmcap tf ' "$OUTDIR/wlroots.caps" && echo true || echo false)"
hl_assert_true "PREMISE: and primaries" \
	"$(grep -q '^cmcap primaries ' "$OUTDIR/wlroots.caps" && echo true || echo false)"

# ── THE ASSERTION ────────────────────────────────────────────────────────
if diff -u "$OUTDIR/wlroots.caps" "$OUTDIR/native.caps" > "$OUTDIR/diff.txt" 2>&1; then
	hl_assert_true "the two advertisements are identical, in order" "true"
else
	echo "  difference:"
	sed 's/^/    /' "$OUTDIR/diff.txt" | head -20
	hl_assert_true "the two advertisements are identical, in order" "false"
fi

echo
hl_summary
rc=$?
[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
exit $rc
