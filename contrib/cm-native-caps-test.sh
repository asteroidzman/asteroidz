#!/usr/bin/env bash
# cm-native-caps-test.sh -- what a client learns at bind time, pinned.
#
# ── WHAT THIS USED TO BE, AND WHY IT COULD NOT STAY ──────────────────────
#
# It ran the same client against two implementations of wp_color_manager_v1 --
# wlroots' and this compositor's -- and diffed the advertisements. That was the
# right gate for a cutover: the change had to be invisible to clients, and a
# live A/B is the only way to assert that without hand-copying a list.
#
# The cutover has happened. wlroots' implementation is not created any more, so
# the reference arm cannot be run, and a fixture that keeps comparing native to
# native would be diffing a stream against itself -- passing unconditionally,
# for the rest of the project's life, while looking exactly as green as it does
# now.
#
# ── SO THE REFERENCE IS FROZEN INSTEAD ───────────────────────────────────
#
# The list below is the advertisement wlroots produced, recorded verbatim from
# the last run where both arms existed (the green baseline at 2b70d696). It is
# not what the native implementation happens to emit today -- copying today's
# output into the expectation is how a golden file stops being able to fail.
#
# ── COMPARED IN ARRIVAL ORDER, NOT AS A SET ──────────────────────────────
#
# Two implementations advertising the same values in a different order are not
# the same advertisement to a client that takes the first value it recognises,
# and a comparison that sorted first would call them equal.
#
# ── AND THE FIXTURE FALSIFIES ITSELF ─────────────────────────────────────
#
# A frozen expectation is only worth what its ability to fail is worth, and the
# old arrangement had a live second arm to keep it honest. This runs a third
# arm under AZ_BREAK_WPCM_EMPTY_CAPS -- the manager built with no transfer
# functions and no primaries, which is the real defect this file exists for:
# an empty capability list once meant NO CLIENT COULD ATTACH AN IMAGE
# DESCRIPTION AT ALL, silently, with mpv tone-mapping its own HDR10 to SDR
# before handing it over. That arm must come out RED, in this same run, or the
# green above it means nothing.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="cm-native-caps"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-cmcaps-$$"
mkdir -p "$OUTDIR"

BGE="$(cd "$(dirname "$0")" && pwd)/wlbgeffect/wlbgeffect"
[ -x "$BGE" ] || { echo "SKIP: wlbgeffect not built"; exit 77; }

# ── THE FROZEN ADVERTISEMENT ─────────────────────────────────────────────
# feature 1  = parametric image descriptions
# intent 0   = perceptual, the only one this compositor implements
# tf 14/2/1/11 = COMPOUND_POWER_2_4 (wlroots' sRGB), GAMMA22, BT1886, ST2084_PQ
# primaries 1/6 = SRGB (BT.709) and BT2020
# Order is AVK's decode-variant order (az_cm_caps.h), not sorted.
cat > "$OUTDIR/expected.caps" <<'EOF'
cmcap feature 1
cmcap intent 0
cmcap tf 14
cmcap tf 2
cmcap tf 1
cmcap tf 11
cmcap primaries 1
cmcap primaries 6
cmcap done
EOF

echo "══ the advertisement a client binds to has not moved ══"
echo

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

run_arm native ""
run_arm empty "AZ_BREAK_WPCM_EMPTY_CAPS=1"

N="$(wc -l < "$OUTDIR/native.caps" 2>/dev/null || echo 0)"
E="$(wc -l < "$OUTDIR/empty.caps" 2>/dev/null || echo 0)"
echo "  native advertised $N line(s):"
sed 's/^/    /' "$OUTDIR/native.caps" 2>/dev/null
echo "  under AZ_BREAK_WPCM_EMPTY_CAPS, $E line(s):"
sed 's/^/    /' "$OUTDIR/empty.caps" 2>/dev/null
echo

# ── PREMISES ─────────────────────────────────────────────────────────────
# An empty advertisement compares equal to an empty advertisement, so "it
# matches the golden" is satisfied by a compositor that said nothing and a
# golden file that was accidentally truncated. Both ends are pinned.
hl_assert_true "PREMISE: the client bound the manager and heard something ($N)" \
	"$([ "$N" -ge 4 ] && echo true || echo false)"
hl_assert_true "PREMISE: the frozen reference is not empty" \
	"$([ "$(wc -l < "$OUTDIR/expected.caps")" -eq 9 ] && echo true || echo false)"

# ── THE ASSERTION ────────────────────────────────────────────────────────
if diff -u "$OUTDIR/expected.caps" "$OUTDIR/native.caps" > "$OUTDIR/diff.txt" 2>&1; then
	hl_assert_true "the advertisement matches wlroots', in order" "true"
else
	echo "  difference (- frozen, + today):"
	sed 's/^/    /' "$OUTDIR/diff.txt" | head -20
	hl_assert_true "the advertisement matches wlroots', in order" "false"
fi

# ── THE FALSIFIER ────────────────────────────────────────────────────────
# Not "the break arm differs somehow" -- it must differ by having LOST the
# transfer functions specifically, which is the failure mode that was silent.
#
# The break suppresses the transfer-function list and NOTHING ELSE, so the
# primaries must still arrive. Asserting that too is the point: a break that
# quietly grew to knock out half the advertisement would still satisfy "the
# arm differs from the golden" while no longer reproducing the defect it was
# written for, and the falsifier would go on looking like it worked.
hl_assert_true "FALSIFIER: the empty-caps break names no transfer function" \
	"$(grep -q '^cmcap tf ' "$OUTDIR/empty.caps" && echo false || echo true)"
hl_assert_true "FALSIFIER: and knocks out ONLY those -- primaries still arrive" \
	"$(grep -q '^cmcap primaries ' "$OUTDIR/empty.caps" && echo true || echo false)"
hl_assert_true "FALSIFIER: so it does NOT match the frozen advertisement" \
	"$(cmp -s "$OUTDIR/expected.caps" "$OUTDIR/empty.caps" && echo false || echo true)"

echo
hl_summary
rc=$?
[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
exit $rc
