#!/usr/bin/env bash
# avk-m5-path-a-test.sh -- Path A on a real compositor, and the one-line
# decision that makes it correct.
#
# ── WHY THIS IS NOT A UNIT TEST ──────────────────────────────────────────
#
# The thing being asserted lives in the scene walk: how a surface's declared
# transfer function becomes a luminance domain. That code reads scenefx structs
# and cannot be exercised without a compositor and a client -- and the input
# that matters is not one this project produces. It comes from scenefx's
# surface adapter, which initialises the transfer function to GAMMA22 and
# overwrites it only for a surface carrying an image description.
#
# So a surface that has declared NOTHING arrives indistinguishable from one
# that declared a 2.2 power curve. ADR-004 says untagged is piecewise-sRGB, and
# F12 records what happened when that was not honoured: Path A's encode is the
# hardware's _SRGB attachment conversion and cannot be selected, so a 2.2
# decode could not round-trip through it and the WHOLE DISPLAY came back one
# code high.
#
# That fix is one case in one switch. Nothing else in the tree would notice it
# being reverted, and the symptom -- every pixel off by one -- is the kind that
# reads as rounding. Hence this.
#
# ── WHAT IT ASSERTS ──────────────────────────────────────────────────────
#
#   1. Path A really engaged            decode draws > 0, srgb segments > 0
#   2. every source took the sRGB curve  decode_gamma22 == 0   <- the F12 fix
#   3. a wallpaper-only frame is IDENTICAL with Path A on and off
#   4. no validation errors on either arm
#
# (3) is the round trip: decode then encode, on a real output, through the real
# scanout buffer. Zero, not a tolerance -- at 8 bits the hardware's sRGB
# conversions are exact inverses, and a tolerance here would hide exactly the
# curve mismatch this exists to catch.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-m5-path-a"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m5patha-$$"
mkdir -p "$OUTDIR"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

W="${W:-1920}"; H="${H:-1080}"

# NO EFFECTS, and that is the point of this fixture rather than a shortcut.
#
# Path A changes where composition happens, so a BLENDED pixel is expected to
# come out different -- that is ADR-005 and it is the feature. A wallpaper-only
# frame has nothing blended in it, so the round trip must be exact, and any
# difference at all is a real defect rather than a judgement call about how
# much linear compositing should move a pixel.
CFG="shadows 0
layer_shadows 0
effects { blur { enable 0 } shadow { enable 0 } }
layout { titlebar { enable 0 } }"

JQ='{frames:.frames, srgb:.m5_decode_srgb, g22:.m5_decode_gamma22,
     bt1886:.m5_decode_bt1886, dec:.m5_decode_draws,
     segs:.m5_srgb_attach_segments, verr:.validation_errors,
     von:.validation_enabled}'
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
STATS=""

run() { # run NAME [EXTRA_ENV...]
	local name="$1"; shift
	local dir="$OUTDIR/$name" cdir="$OUTDIR/$name-cap"
	mkdir -p "$dir" "$cdir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1=1
	# ASTEROIDZ_VK_DEBUG=1, WITHOUT WHICH THE LAST TWO ASSERTIONS ARE EMPTY.
	# `validation_errors` only increments from the validation layer's callback,
	# so with no layer loaded it reads 0 whatever the frame did -- and this
	# fixture asserted it that way for a whole milestone while Path A attached
	# an _SRGB view to pipelines declaring the UNORM twin, twenty VUIDs a run.
	HL_ENV="ASTEROIDZ_VK_DEBUG=1 AZ_SHADOW_DITHER_AMP=0 AZ_AVK_CAPTURE_DIR=$cdir $*"
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
	# mid-write is a truncated file, which ppm.py refuses and which reads in the
	# output as a failed oracle rather than as a missing picture.
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

echo "══ M5 Path A ══ ${W}x${H}, wallpaper only"
echo

# M6B/D5 INVERTED THE DEFAULT: unset now means ON, so the control arm has to
# ask for OFF explicitly. It used to be spelled `run off` with no environment at
# all, and after the promotion that arm was quietly running the SAME
# configuration as the `on` arm -- which showed up here as the premises failing
# ("Path A off decodes nothing" reading 2 decodes) rather than as the gate
# silently comparing a thing to itself. That is the only reason this fixture
# noticed the promotion at all.
run off AZ_M5_PATH_A=0
OFF="$STATS"
run on AZ_M5_PATH_A=1
ON="$STATS"

echo "  off: decode=$(v dec "$OFF") srgb_segments=$(v segs "$OFF")"
echo "  on : decode=$(v dec "$ON") srgb_segments=$(v segs "$ON")" \
	"[srgb=$(v srgb "$ON") gamma22=$(v g22 "$ON") bt1886=$(v bt1886 "$ON")]"
echo

# ── 1. THE PREMISE: Path A actually engaged ──────────────────────────────
# Off must do neither and on must do both. Without the second half, a frame
# that came back identical would be reported as a perfect round trip when what
# actually happened is that nothing was switched on.
hl_assert "PREMISE: Path A off decodes nothing" "$(v dec "$OFF")" 0
hl_assert "PREMISE: Path A off takes no _SRGB attachment" "$(v segs "$OFF")" 0
hl_assert_true "PREMISE: Path A on DOES decode ($(v dec "$ON"))" \
	"$([ "$(v dec "$ON")" -gt 0 ] && echo true || echo false)"
hl_assert_true "PREMISE: and DOES take the _SRGB attachment ($(v segs "$ON"))" \
	"$([ "$(v segs "$ON")" -gt 0 ] && echo true || echo false)"

echo
echo "── the F12 decision: untagged is sRGB, not 2.2 ──────────────────────"
# scenefx hands every untagged surface over as GAMMA22. If this reads nonzero,
# the adapter is honouring that literally again and the whole display will be
# one code high -- see F12.
hl_assert "no source took the 2.2 curve" "$(v g22 "$ON")" 0
hl_assert "no source took BT.1886" "$(v bt1886 "$ON")" 0
hl_assert "every decode was sRGB" "$(v srgb "$ON")" "$(v dec "$ON")"

echo
echo "── the round trip, on the real scanout buffer ───────────────────────"
read -r D M <<<"$(python3 "$PPM" diff "$OUTDIR/off.ppm" "$OUTDIR/on.ppm" 2>/dev/null \
	|| echo "-1 -1")"
echo "  Path A off vs on: $D px differ (worst channel $M)"
hl_assert "A WALLPAPER-ONLY FRAME IS IDENTICAL THROUGH PATH A" "$D" 0

echo
# THE PREMISE FIRST. A count of zero from a counter nothing can increment is
# not a result; see the note beside HL_ENV above for what it cost.
hl_assert "PREMISE: the validation layer is on (off arm)" "$(v von "$OFF")" true
hl_assert "PREMISE: the validation layer is on (on arm)" "$(v von "$ON")" true
hl_assert "off: validation errors" "$(v verr "$OFF")" 0
hl_assert "on:  validation errors" "$(v verr "$ON")" 0

echo
echo "logs: $OUTDIR"
hl_summary
