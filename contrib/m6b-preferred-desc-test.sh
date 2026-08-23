#!/usr/bin/env bash
# m6b-preferred-desc-test.sh -- M6B gate G4: the compositor tells a wp-cm
# surface what its output prefers.
#
# ── WHY A CLIENT AND NOT AN ASSERTION IN THE COMPOSITOR ──────────────────
#
# `wlr_color_manager_v1_set_surface_preferred_image_description` had no callers
# in the tree, and nothing inside the compositor could tell: no error, no log,
# no counter. The answer lives in a protocol object, and only a client holds
# one. contrib/wlcm exists for exactly this and nothing else.
#
# ── THE THREE ANSWERS ARE DIFFERENT FINDINGS ─────────────────────────────
#
#   preferred: set ...      the compositor stated a description
#   preferred: none         no preferred_changed event ever arrived  (pre-D6)
#   preferred: no-protocol  wp-cm is not advertised at all
#
# Collapsing them would be the whole bug over again: a client that renders SDR
# because it was told SDR and a client that renders SDR because it was told
# NOTHING look identical on screen and are unrelated defects.
#
# ── WHAT THIS DOES *NOT* COVER, SAID HERE RATHER THAN IMPLIED ────────────
#
# THE HDR ARM. A headless output does not support BT.2020 + PQ -- the backend
# says so and the compositor logs it -- so it cannot present an HDR image
# description and there is nothing for the PQ branch to be read from. That arm
# is live-only, on DP-1, and is recorded as such in docs/history.md
# rather than faked here with a forced description that no output is in.
#
# What IS covered headlessly is the part that was actually broken: whether the
# compositor says anything at all, and whether the break restores the silence.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="m6b-preferred-desc"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m6b-pref-$$"
mkdir -p "$OUTDIR"

WLCM="$(cd "$(dirname "$0")" && pwd)/wlcm/wlcm"
[ -x "$WLCM" ] || { echo "SKIP: wlcm not built -- run: cd contrib/wlcm && make"; exit 77; }

CFG="shadows 0
layer_shadows 0
effects { blur { enable 0 } shadow { enable 0 } }
layout { titlebar { enable 0 } }"

OUT=""
run() { # run NAME [EXTRA_ENV]
	local name="$1" extra_env="${2:-}"
	local dir="$OUTDIR/$name"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"; HL_WIDTH=1920; HL_HEIGHT=1080; HL_SCALE1=1
	HL_ENV="$extra_env"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >"$dir/hl_start.log" 2>&1
	sleep 2
	# The client runs INSIDE the session: hl_start exports WAYLAND_DISPLAY and
	# XDG_RUNTIME_DIR, and wlcm is short-lived -- it maps, reads, prints, exits.
	OUT="$("$WLCM" 3000 2>"$dir/wlcm.err" | head -2)"
	echo "$OUT" >"$dir/wlcm.out"
	hl_stop >/dev/null 2>&1
}

echo "══ M6B/G4 ══ the preferred image description reaches a client"
echo

run wired
WIRED="$OUT"
echo "  wired : $WIRED"

run broken "AZ_BREAK_CM_NO_PREFERRED=1"
BROKEN="$OUT"
echo "  broken: $BROKEN"
echo

# ── THE PROTOCOL IS THERE AT ALL ─────────────────────────────────────────
# Asserted first: without it, "none" from the break arm would be satisfied by a
# compositor that never advertised wp-cm, and the gate would pass on a session
# with no colour management whatsoever.
hl_assert_true "PREMISE: wp-cm is advertised (not 'no-protocol')" \
	"$(echo "$WIRED" | grep -q "no-protocol" && echo false || echo true)"

# ── D6: SOMETHING IS SAID ────────────────────────────────────────────────
hl_assert_true "the compositor states a preferred description" \
	"$(echo "$WIRED" | grep -q "^preferred: set" && echo true || echo false)"

# AND IT IS THE OUTPUT'S ACTUAL COLOUR. A headless output is SDR, so sRGB is
# the true answer -- and stating it is not the same as staying silent, which is
# the whole point of the break below.
#
# tf=14 IS sRGB, and the number is worth the sentence it takes to explain.
# wlroots maps its own WLR_COLOR_TRANSFER_FUNCTION_SRGB to the protocol's
# COMPOUND_POWER_2_4 (14), NOT to the protocol's own SRGB (9) -- and _to_wlr()
# aborts on 9, so a compositor that sent the obvious constant would be killed by
# the first client that asked. Asserting 14 here is asserting that the value
# went out through wlroots' own mapping rather than being written by hand.
hl_assert_true "and it is sRGB/sRGB, which is what this output is" \
	"$(echo "$WIRED" | grep -q "tf=14 primaries=1" && echo true || echo false)"

# THE have MASK. A zero in a value field could mean "the compositor said zero"
# or "that event never arrived"; the mask distinguishes them, and tf/primaries
# must both have been genuinely sent.
hl_assert_true "the tf and primaries events genuinely arrived (have mask)" \
	"$(echo "$WIRED" | grep -qE "have=11[01][01][01]" && echo true || echo false)"

echo
# ── THE FALSIFIER ────────────────────────────────────────────────────────
hl_assert_true "BREAK: with AZ_BREAK_CM_NO_PREFERRED the client is told NOTHING" \
	"$(echo "$BROKEN" | grep -q "^preferred: none" && echo true || echo false)"
hl_assert_true "BREAK: and that is a DIFFERENT answer from the wired arm" \
	"$([ "$WIRED" != "$BROKEN" ] && echo true || echo false)"

echo
hl_summary
rc=$?
[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
exit $rc
