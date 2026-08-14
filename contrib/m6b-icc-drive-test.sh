#!/usr/bin/env bash
# m6b-icc-drive-test.sh -- M6B gate G3b: a profiled output is DRIVEN BY AVK,
# and a profile AVK cannot express still falls back.
#
# ── WHY THIS EXISTS BESIDE G1, G2 AND G3a ────────────────────────────────
#
# G1 checks the ingest against lcms2 with no device. G2 checks the encode
# variant against that ingest on a GPU. G3a checks the decision table as a pure
# function. All three can be green while the COMPOSITOR never reaches any of
# them -- and that is not hypothetical: between G2 landing and this fixture
# being written, two separate paths loaded a profile and applied it to no
# pixels at all, because nothing re-derived the output's colour state. Both were
# found by hand, one of them live on the operator's own display.
#
# So this asserts the wiring and only the wiring:
#
#   1. no profile      -> A-direct-srgb / srgb          (the premise)
#   2. FI32U.icm       -> B-encode / lut1d, AVK DRIVING, 0 fallback frames
#   3. a cLUT profile  -> FALLBACK, and the log line says why
#
# ── (3) IS THE FALSIFIER, AND IT IS SYNTHESISED ──────────────────────────
#
# There is no cLUT profile on this machine -- the only one that exists is
# matrix-shaper, which is exactly why D2 refuses cLUT by classification rather
# than implementing it. A refusal that can never be exercised is not a tested
# behaviour, so contrib/icc-synth.c generates one, and it CARRIES COLORANTS AND
# TRCs: a cLUT profile without them would be refused by an implementation that
# merely failed to find a matrix, and would prove nothing about
# classification.
#
# ── AND (1) IS NOT DECORATION EITHER ─────────────────────────────────────
#
# Without it, "the profiled arm reports B-encode/lut1d" would also pass on a
# compositor that reported B-encode/lut1d for every output regardless. The three
# arms differ in one config line each and must land in three different states.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="m6b-icc-drive"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m6b-icc-$$"
mkdir -p "$OUTDIR"

PROFILE="${AZ_ICC_PROFILE:-/home/ralf/FI32U.icm}"
BUILD="$(cd "$(dirname "$0")/.." && pwd)/build"
SYNTH="$BUILD/icc-synth"

# DISPLAY-SPECIFIC, AND SAID OUT LOUD. This gate is about a real profile on a
# real desk; skipping quietly would let it rot into a fixture that checks
# nothing everywhere else.
if [ ! -r "$PROFILE" ]; then
	echo "SKIP: $PROFILE not present -- G3b is display-specific"
	echo "      (set AZ_ICC_PROFILE to a matrix-shaper display profile)"
	exit 77
fi
if [ ! -x "$SYNTH" ]; then
	echo "SKIP: $SYNTH not built -- run: ninja -C build icc-synth"
	exit 77
fi

CLUT="$OUTDIR/clut.icc"
"$SYNTH" clut "$CLUT" >/dev/null || { echo "could not synthesise a cLUT profile"; exit 1; }

# NO EFFECTS. This fixture asserts on STATE, not on pixels, and blur/shadow
# only add frames and time to every arm.
CFG_BASE="shadows 0
layer_shadows 0
effects { blur { enable 0 } shadow { enable 0 } }
layout { titlebar { enable 0 } }"

JQ_MON='.monitors[] | select(.name=="HEADLESS-1") |
        "path=\(.color_path) tf=\(.color_encode_tf) shaper=\(.icc_shaper) icc=\(.icc_profile)"'
JQ_AVK='"active=\(.active) fallback=\(.fallback_frames) enc=\(.m5_encode_draws)
         verr=\(.validation_errors) von=\(.validation_enabled)"'

v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

MON=""; AVK=""; LOG=""
run() { # run NAME [ICC_PATH]
	local name="$1" icc="${2:-}"
	local dir="$OUTDIR/$name"
	mkdir -p "$dir"
	local extra=""
	# A SECOND output BLOCK, merged with the harness's own. Monitor rules apply
	# in config order and later ones override per option (monitor_merge_rules),
	# so a rule carrying nothing but icc-profile composes with the geometry the
	# harness already wrote -- and the arms then differ in exactly one line.
	[ -n "$icc" ] && extra="output HEADLESS-1 { icc-profile \"$icc\" }"
	HL_OUTDIR="$dir"; HL_WIDTH=1920; HL_HEIGHT=1080; HL_SCALE1=1
	# ASTEROIDZ_VK_DEBUG=1: validation_errors is a counter nothing can
	# increment without the layer, so asserting it without this is asserting a
	# tautology (F14). The premise is checked from validation_enabled below.
	HL_ENV="ASTEROIDZ_RENDERER=avk ASTEROIDZ_VK_DEBUG=1"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG_BASE
$extra" >"$dir/hl_start.log" 2>&1
	sleep 3
	MON="$(hl_get "get all-monitors" | jq -r "$JQ_MON" 2>/dev/null)"
	AVK="$(hl_get "get avk-stats" | jq -r "$JQ_AVK" 2>/dev/null | tr -s ' \n' ' ')"
	LOG="$dir/state/asteroidz/asteroidz.log"
	[ -r "$LOG" ] || LOG="$(ls "$dir"/state/asteroidz/*.log 2>/dev/null | head -1)"
	cp -f "$LOG" "$dir/captured.log" 2>/dev/null || true
	LOG="$dir/captured.log"
	hl_stop >/dev/null 2>&1
}

echo "══ M6B/G3b ══ a profiled output must be driven by AVK"
echo "   profile: $PROFILE"
echo "   cLUT   : $CLUT (synthesised, with colorants)"
echo

# ── ARM 1: NO PROFILE ────────────────────────────────────────────────────
run none
NONE_MON="$MON"; NONE_AVK="$AVK"
echo "  none  : $NONE_MON"
echo "          $NONE_AVK"
hl_assert "PREMISE: an unprofiled output is A-direct-srgb" \
	"$(v path "$NONE_MON")" "A-direct-srgb"
hl_assert "PREMISE: and encodes with the analytic sRGB curve" \
	"$(v tf "$NONE_MON")" "srgb"
hl_assert "PREMISE: and has no shaper" "$(v shaper "$NONE_MON")" "false"
hl_assert_true "PREMISE: the validation layer is loaded, so verr can move" \
	"$(v von "$NONE_AVK")"

echo
# ── ARM 2: THE REAL PROFILE ──────────────────────────────────────────────
run shaper "$PROFILE"
S_MON="$MON"; S_AVK="$AVK"; S_LOG="$LOG"
echo "  shaper: $S_MON"
echo "          $S_AVK"
hl_assert "the profile is reduced to a matrix-shaper" \
	"$(v shaper "$S_MON")" "true"
hl_assert "a profiled SDR output takes Path B" "$(v path "$S_MON")" "B-encode"
hl_assert "and encodes with the display's MEASURED curve" \
	"$(v tf "$S_MON")" "lut1d"
# DRIVEN, not merely derived. The whole failure this gate exists for is a
# correct-looking colour state that no renderer acts on.
hl_assert_true "AVK is the renderer for it" "$(v active "$S_AVK")"
hl_assert "and refused no frames" "$(v fallback "$S_AVK")" 0
hl_assert_true "the encode pass actually drew ($(v enc "$S_AVK") draws)" \
	"$([ "$(v enc "$S_AVK")" -gt 0 ] && echo true || echo false)"
hl_assert "no validation errors" "$(v verr "$S_AVK")" 0
hl_assert_true "no SceneFX fallback message in the log" \
	"$(grep -qa "stays on the SceneFX path\|SceneFX drives this output" "$S_LOG" \
		&& echo false || echo true)"
hl_assert_true "the M5 colour line says B-encode/lut1d" \
	"$(grep -qa "M5 color: HEADLESS-1 path=B-encode tf=lut1d" "$S_LOG" \
		&& echo true || echo false)"

echo
# ── ARM 3: THE FALSIFIER ─────────────────────────────────────────────────
run clut "$CLUT"
C_MON="$MON"; C_AVK="$AVK"; C_LOG="$LOG"
echo "  clut  : $C_MON"
echo "          $C_AVK"
hl_assert "BREAK: a cLUT profile does NOT reduce, despite its colorants" \
	"$(v shaper "$C_MON")" "false"
hl_assert "BREAK: and the output falls back" "$(v path "$C_MON")" "fallback"
hl_assert "BREAK: with the analytic curve, not a measured one" \
	"$(v tf "$C_MON")" "srgb"
# THE PROFILE IS STILL LOADED. Falling back is not dropping the operator's
# calibration -- it is handing it to the renderer that can apply it.
hl_assert_true "BREAK: the profile is still attached for SceneFX to apply" \
	"$([ -n "$(v icc "$C_MON")" ] && echo true || echo false)"
hl_assert_true "BREAK: and the log names the reason" \
	"$(grep -qa "cannot express as a matrix and a curve" "$C_LOG" \
		&& echo true || echo false)"

echo
echo "── the three arms landed in three different states ──────────────────"
# Stated as its own assertion because every row above is an equality against a
# constant, and a compositor that reported one state for everything would
# satisfy a third of them by luck.
hl_assert_true "none != shaper != clut" \
	"$([ "$(v path "$NONE_MON")" != "$(v path "$S_MON")" ] \
	   && [ "$(v path "$S_MON")" != "$(v path "$C_MON")" ] \
	   && [ "$(v path "$NONE_MON")" != "$(v path "$C_MON")" ] \
	   && echo true || echo false)"

echo
hl_summary
rc=$?
[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
exit $rc
