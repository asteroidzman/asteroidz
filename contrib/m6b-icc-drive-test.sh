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
#   3. a cLUT profile  -> B-encode / clut3d, AVK DRIVING, 0 fallback frames
#
# ── (3) CHANGED SIDES IN M6C, AND THAT IS THE POINT OF KEEPING IT ────────
#
# It used to assert the OPPOSITE: a cLUT profile derived FALLBACK and SceneFX
# drove the output. With SceneFX gone a refusal is an abort, so a user who
# profiles their display with a colorimeter -- which is what produces a cLUT
# profile -- could not start the compositor. This arm now asserts that AVK
# carries it, and the row it replaced is the reason to keep reading the file.
#
# It is SYNTHESISED, because there is no cLUT profile on this machine -- the
# only one that exists is matrix-shaper. contrib/icc-synth.c generates one, and
# it CARRIES COLORANTS AND TRCs: a cLUT profile without them would be handled
# by an implementation that merely fell back to the matrix, and would prove
# nothing. Arm 2 and arm 3 must therefore land in DIFFERENT encode curves --
# lut1d against clut3d -- which is what says the classification still
# discriminates now that both outcomes are "AVK drives it".
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
        "path=\(.color_path) tf=\(.color_encode_tf) shaper=\(.icc_shaper) clut=\(.icc_clut) icc=\(.icc_profile)"'
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

echo "══ M6B/G3b + M6C ══ a profiled output must be driven by AVK"
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
hl_assert "PREMISE: and no cube either" "$(v clut "$NONE_MON")" "false"
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
# AND NOT ALSO A CUBE. Building both would be 274625 lcms2 evaluations and 2 MB
# spent on an answer nothing consults -- and would mean the two forms could
# disagree with nothing to say which was in force.
hl_assert "and NOT also carried as a cube" "$(v clut "$S_MON")" "false"
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
# ── ARM 3: THE cLUT PROFILE, WHICH AVK NOW DRIVES (M6C) ──────────────────
run clut "$CLUT"
C_MON="$MON"; C_AVK="$AVK"; C_LOG="$LOG"
echo "  clut  : $C_MON"
echo "          $C_AVK"
hl_assert "a cLUT profile does NOT reduce, despite its colorants" \
	"$(v shaper "$C_MON")" "false"
hl_assert "so it is carried as a 3D table instead" "$(v clut "$C_MON")" "true"
hl_assert "and the output takes Path B" "$(v path "$C_MON")" "B-encode"
hl_assert "encoding through the cube, not the analytic curve" \
	"$(v tf "$C_MON")" "clut3d"
# DRIVEN. The whole reason this arm changed sides: a refusal is now an abort,
# so "the profile is safely with the other renderer" is no longer an outcome.
hl_assert_true "AVK is the renderer for it" "$(v active "$C_AVK")"
hl_assert "and refused no frames" "$(v fallback "$C_AVK")" 0
hl_assert_true "the encode pass actually drew ($(v enc "$C_AVK") draws)" \
	"$([ "$(v enc "$C_AVK")" -gt 0 ] && echo true || echo false)"
# A 3D IMAGE IS A NEW KIND OF DESCRIPTOR IN THIS RENDERER, and binding a view
# whose type does not match the sampler is a VUID the layer reports and a
# picture the eye does not. Asserting zero here is only meaningful because the
# premise above established the layer is loaded.
hl_assert "no validation errors" "$(v verr "$C_AVK")" 0
hl_assert_true "the profile is still attached" \
	"$([ -n "$(v icc "$C_MON")" ] && echo true || echo false)"
hl_assert_true "and nothing was handed back to SceneFX" \
	"$(grep -qa "stays on the SceneFX path\|could reduce to neither" "$C_LOG" \
		&& echo false || echo true)"
hl_assert_true "the M5 colour line says B-encode/clut3d" \
	"$(grep -qa "M5 color: HEADLESS-1 path=B-encode tf=clut3d" "$C_LOG" \
		&& echo true || echo false)"

echo
echo "── the three arms landed in three different states ──────────────────"
# Stated as its own assertion because every row above is an equality against a
# constant, and a compositor that reported one state for everything would
# satisfy a third of them by luck.
#
# ON (path, tf) AND NOT ON path ALONE, since M6C: two of the three arms are now
# B-encode and only the CURVE tells them apart. Comparing paths would have gone
# on passing while the compositor treated a cLUT profile exactly like a
# matrix-shaper one, which is the one confusion this whole milestone is about.
n_st="$(v path "$NONE_MON")/$(v tf "$NONE_MON")"
s_st="$(v path "$S_MON")/$(v tf "$S_MON")"
c_st="$(v path "$C_MON")/$(v tf "$C_MON")"
echo "   none=$n_st  shaper=$s_st  clut=$c_st"
hl_assert_true "none != shaper != clut, as (path, curve)" \
	"$([ "$n_st" != "$s_st" ] && [ "$s_st" != "$c_st" ] \
	   && [ "$n_st" != "$c_st" ] && echo true || echo false)"

echo
hl_summary
rc=$?
[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
exit $rc
