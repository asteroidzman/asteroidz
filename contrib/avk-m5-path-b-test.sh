#!/usr/bin/env bash
# avk-m5-path-b-test.sh -- Path B on a real compositor: the scene composites
# into a scene-linear FP16 intermediate and one encode pass writes the frame.
#
# ── WHY THIS EXISTS BESIDE THE UNIT TEST ─────────────────────────────────
#
# tests/test-avk-render.c already drives the encode pass against C4's CPU
# reference and closes the SDR round trip at zero codes. What it cannot show is
# the INTEGRATION: that a compositor picks the FP16 renderer for the output,
# lends it an intermediate of the right extent, encodes into the scan-out
# buffer the display actually gets, and does all of it without a pipeline
# compile on the frame path or a validation error.
#
# Every one of those is a wiring question, and every one of them renders a
# plausible picture when it is wrong.
#
# ── AZ_M5_PATH_B=force, AND WHY THE FORCE IS THE POINT ───────────────────
#
# C3 puts an 8-bit output with a usable _SRGB view on Path A, so a headless
# ARGB8888 output would never reach Path B on its own. Forcing it there is what
# makes the comparison meaningful: the same output, the same scene, rendered
# once directly and once through the intermediate, so "identical" is a
# statement about the encode pass rather than about a format change. There is
# no pre-M5 10-bit picture for a 10-bit output to be compared against.
#
# ── WHAT IT ASSERTS ──────────────────────────────────────────────────────
#
#   0. the comparison can see a difference at all   (off vs off = 0 px)
#   1. Path B really engaged        encode draws > 0, an intermediate exists
#   2. the intermediate is the OUTPUT's extent and FP16, by arithmetic
#   3. no pipeline compile after the first frame    compiles == 1
#   4. a wallpaper-only frame is IDENTICAL through the intermediate
#   5. no validation errors -- ASSERTED WITH THE LAYER ON, see below
#
# (5) is not a formality. `validation_errors` only moves from the validation
# layer's callback, so a fixture that asserts it without ASTEROIDZ_VK_DEBUG is
# asserting a counter that cannot move -- which is how Path A shipped a
# milestone attaching an _SRGB view to pipelines declaring the UNORM twin,
# green everywhere. The premise is asserted first, from the compositor's own
# `validation_enabled` field.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-m5-path-b"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m5pathb-$$"
mkdir -p "$OUTDIR"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

W="${W:-1920}"; H="${H:-1080}"

# NO EFFECTS, and for the reason the Path A fixture states: Path B moves
# composition into linear light, so a BLENDED pixel is expected to differ --
# that is ADR-005 and it is the feature. A wallpaper-only frame has nothing
# blended in it, so the round trip must be exact and any difference is a
# defect rather than a judgement call.
CFG="shadows 0
layer_shadows 0
effects { blur { enable 0 } shadow { enable 0 } }
layout { titlebar { enable 0 } }"

JQ='{frames:.frames, dec:.m5_decode_draws, enc:.m5_encode_draws,
     encpx:.m5_encode_px, compiles:.m5_encode_compiles,
     imgs:.m5_intermediate_images, texel:.m5_intermediate_texel_bytes,
     req:.m5_intermediate_req_bytes,
     verr:.validation_errors, von:.validation_enabled}'
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
STATS=""

run() { # run NAME [EXTRA_ENV...]
	local name="$1"; shift
	local dir="$OUTDIR/$name" cdir="$OUTDIR/$name-cap"
	mkdir -p "$dir" "$cdir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1=1
	# ASTEROIDZ_VK_DEBUG=1 ON BOTH ARMS. Without it validation_errors is a
	# counter with nothing to increment it, and the assertion below is a
	# tautology. It is on the control arm too so the two are the same build in
	# the same mode, differing only in the thing being tested.
	HL_ENV="ASTEROIDZ_RENDERER=avk ASTEROIDZ_VK_DEBUG=1 AZ_SHADOW_DITHER_AMP=0 AZ_AVK_CAPTURE_DIR=$cdir $*"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	# TO A FILE, not to /dev/null. hl_start refuses to continue on a rejected
	# config or a missing helper and says which -- and sending that to
	# /dev/null turns every one of those into a fixture that prints its
	# banner and exits with no reason given.
	hl_start "$CFG" >"$dir/hl_start.log" 2>&1
	sleep 3
	STATS="$(hl_get "get avk-stats" | jq -r "$JQ | to_entries |
		map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null)"
	hl_dispatch capture_output 1
	# Wait for the arithmetic size rather than sleeping at it: a copy taken
	# mid-write is a truncated file, which ppm.py refuses and which reads as a
	# failed oracle rather than as a missing picture.
	local want=$(( W * H * 3 )) t=0
	while [ "$t" -lt 60 ]; do
		[ "$(stat -c %s "$cdir/HEADLESS-1.ppm" 2>/dev/null || echo 0)" -ge "$want" ] \
			&& break
		sleep 0.5
		t=$(( t + 1 ))
	done
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	hl_stop >/dev/null 2>&1
}

echo "══ M5 Path B ══ ${W}x${H}, wallpaper only"
echo

run off
OFF="$STATS"
run off2
OFF2="$STATS"
run on AZ_M5_PATH_B=force
ON="$STATS"

echo "  off: decode=$(v dec "$OFF") encode=$(v enc "$OFF") images=$(v imgs "$OFF")"
echo "  on : decode=$(v dec "$ON") encode=$(v enc "$ON")" \
	"px=$(v encpx "$ON") compiles=$(v compiles "$ON")"
echo "  intermediate: $(v imgs "$ON") image(s)" \
	"texel=$(v texel "$ON") req=$(v req "$ON")"
echo

# ── 0. THE PREMISE OF THE COMPARISON ─────────────────────────────────────
# Two identical runs must produce identical captures. Without this, "off and on
# agree" is also what a capture path that always returns the same bytes -- or
# always fails the same way -- would report.
read -r D0 M0 <<<"$(python3 "$PPM" diff "$OUTDIR/off.ppm" "$OUTDIR/off2.ppm" \
	2>/dev/null || echo "-1 -1")"
echo "  control: two identical runs differ in $D0 px (worst $M0)"
hl_assert "PREMISE: the comparison is repeatable (off vs off)" "$D0" 0

# ── 1. PATH B REALLY ENGAGED ─────────────────────────────────────────────
hl_assert "PREMISE: Path B off runs no encode pass" "$(v enc "$OFF")" 0
hl_assert "PREMISE: Path B off allocates no intermediate" "$(v imgs "$OFF")" 0
hl_assert_true "PREMISE: Path B on DOES encode ($(v enc "$ON") draws)" \
	"$([ "$(v enc "$ON")" -gt 0 ] && echo true || echo false)"
hl_assert_true "PREMISE: and DOES decode its sources ($(v dec "$ON"))" \
	"$([ "$(v dec "$ON")" -gt 0 ] && echo true || echo false)"
hl_assert "one intermediate, for the one output" "$(v imgs "$ON")" 1

echo
echo "── the intermediate is the output's own extent, in FP16 ─────────────"
# BY ARITHMETIC, not by trusting the allocator: a mismatched extent maps the
# encode pass's fullscreen triangle onto a different rectangle of the source,
# which is a plausible-looking rescale of the whole desktop.
hl_assert "texel bytes are width x height x 8" "$(v texel "$ON")" \
	"$(( W * H * 8 ))"

echo
echo "── no pipeline compile on the frame path ────────────────────────────"
# ONE compile, for one (format, curve) pair. A compile is a stall of
# milliseconds; a count that climbs with frames is a keying bug, and no timing
# percentile can tell it from a slow frame.
hl_assert "exactly one encode pipeline was built" "$(v compiles "$ON")" 1

echo
echo "── the round trip, on the real scanout buffer ───────────────────────"
read -r D M <<<"$(python3 "$PPM" diff "$OUTDIR/off.ppm" "$OUTDIR/on.ppm" 2>/dev/null \
	|| echo "-1 -1")"
echo "  direct vs Path B: $D px differ (worst channel $M)"
hl_assert "A WALLPAPER-ONLY FRAME IS IDENTICAL THROUGH THE INTERMEDIATE" "$D" 0

echo
echo "── validation, with the layer actually loaded ───────────────────────"
hl_assert "PREMISE: the validation layer is on (off arm)" "$(v von "$OFF")" true
hl_assert "PREMISE: the validation layer is on (on arm)" "$(v von "$ON")" true
hl_assert "off: validation errors" "$(v verr "$OFF")" 0
hl_assert "on:  validation errors" "$(v verr "$ON")" 0


# ── STAGE 2: A SOLID COLOUR THAT IS NOT A TEXTURE ────────────────────────
#
# The frame above is a wallpaper -- a client BUFFER, decoded by C7. A border is
# not: its colour comes from the config as an sRGB hex triple and reaches the
# renderer as a command field. On a linear path it has to be decoded like any
# other source (az_avk_scene_rgb), and before that existed an electrical 128
# came back as 188. This is the end-to-end half of that, with a walk in it; the
# renderer's half is in tests/test-avk-render.c.
#
# A PIXEL INSIDE THE BORDER RING, not a whole-frame compare. A window has
# antialiased corners and a blended edge, and those pixels are EXPECTED to move
# on a linear path -- that is ADR-005. The middle of a 12px border ring is
# solid colour and nothing else.
REPAINT="$(cd "$(dirname "$0")" && pwd)/wlrepaint/wlrepaint"
BW=12
FOCUS_COLOR="0xc66b25ff"
WIN_W=600; WIN_H=420; WIN_X=250; WIN_Y=160
# Left ring, vertically centred: clear of both corner arcs.
PX=$(( WIN_X + BW / 2 )); PY=$(( WIN_Y + WIN_H / 2 ))

BORDER_CFG="borderpx $BW
border_radius 0
focuscolor $FOCUS_COLOR
bordercolor $FOCUS_COLOR
shadows 0
layer_shadows 0
gappih 0
gappiv 0
gappoh 0
gappov 0
animations 0
smartgaps 0
effects { blur { enable 0 } shadow { enable 0 } }
layout { titlebar { enable 0 } }"

run_border() { # run_border NAME [EXTRA_ENV...]
	local name="$1"; shift
	local dir="$OUTDIR/$name" cdir="$OUTDIR/$name-cap"
	mkdir -p "$dir" "$cdir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1=1
	HL_ENV="ASTEROIDZ_RENDERER=avk ASTEROIDZ_VK_DEBUG=1 AZ_SHADOW_DITHER_AMP=0 AZ_AVK_CAPTURE_DIR=$cdir $*"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$BORDER_CFG" >"$dir/hl_start.log" 2>&1
	sleep 2
	# --ssd is not optional: a client that never binds xdg-decoration is CSD,
	# gets NO border, and the ring this probes is empty wallpaper.
	"$REPAINT" --title m5border --size "${WIN_W}x${WIN_H}" --solid 202020 \
		--frames 200 --ssd --hold-ms 100 > "$dir/wl.log" 2>&1 &
	HL_SPAWNED_PIDS+=("$!")
	hl_wait_client_count 1 40 >/dev/null 2>&1
	sleep 1
	hl_dispatch "toggle_floating" 0.5
	hl_dispatch "resize_window,$WIN_W,$WIN_H" 0.5
	hl_dispatch "move_window,$WIN_X,$WIN_Y" 1.5
	hl_dispatch capture_output 1
	local want=$(( W * H * 3 )) t=0
	while [ "$t" -lt 60 ]; do
		[ "$(stat -c %s "$cdir/HEADLESS-1.ppm" 2>/dev/null || echo 0)" -ge "$want" ] \
			&& break
		sleep 0.5
		t=$(( t + 1 ))
	done
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	hl_stop >/dev/null 2>&1
}

echo
echo "── a BORDER: a colour that never was a texture ──────────────────────"
run_border boff
run_border bon AZ_M5_PATH_B=force
BOFF="$(python3 "$PPM" px "$OUTDIR/boff.ppm" "$PX" "$PY" 2>/dev/null || echo "-1 -1 -1")"
BON="$(python3 "$PPM" px "$OUTDIR/bon.ppm" "$PX" "$PY" 2>/dev/null || echo "-1 -1 -1")"
echo "  border pixel at $PX,$PY -- direct [$BOFF]  Path B [$BON]"
# THE PREMISE: that pixel is the BORDER, not wallpaper. The harness wallpaper is
# a neutral #808080, so a probe that missed the ring would read 128 128 128 and
# the two arms would agree about nothing in particular.
hl_assert_true "PREMISE: the probe landed on the border, not the wallpaper" \
	"$([ "$BOFF" != "128 128 128" ] && [ "$BOFF" != "-1 -1 -1" ] \
		&& echo true || echo false)"
hl_assert "A BORDER'S COLOUR SURVIVES THE LINEAR PATH" "$BON" "$BOFF"

echo
echo "logs: $OUTDIR"
hl_summary
