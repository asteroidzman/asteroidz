#!/usr/bin/env bash
# xw-scale-test.sh — does an X11 window reach a fractional-scale output 1:1?
#
# ── THE PROBLEM THIS MEASURES ────────────────────────────────────────────
#
# X11 has no concept of a fractional output scale, and nothing in this tree
# ever gave it one: an X window is configured in LOGICAL units, commits a
# buffer of that many pixels, and the scene presents it at its buffer size --
# which the renderer then magnifies by the output scale. On a 1.25x output a
# fullscreen game renders 1536x864 and is stretched to 1920x1080. That is not
# a subtle loss; it is the entire reason `xwayland_force_scale_one` exists.
#
# ── WHY A CHECKERBOARD AND NOT A WINDOW ──────────────────────────────────
#
# Magnifying a flat colour produces the same flat colour. A fixture built on
# ordinary window content -- a terminal, a coloured rectangle -- cannot see
# this bug at all in its interior, and would report success against every
# possible implementation. contrib/x11check paints a ONE-PIXEL checkerboard
# instead, the highest frequency a raster can hold, so that:
#
#   presented 1:1        two colours, every neighbouring pixel differs
#   magnified, bilinear  greys appear (this is the blur the user sees)
#   magnified, nearest   greys do not, but duplicated columns/rows do
#
# contrib/lib/checker.py counts both symptoms, so neither filter mode can
# sneak a magnified window past as "native".
#
# ── THE THREE BOUNDARIES, ASSERTED SEPARATELY ────────────────────────────
#
#   configure   the size the compositor asks the X window for, read from the
#               client itself in X11's own units. Needs no renderer, so it
#               fails first and points straight at the configure path.
#   pixels      the capture verdict above: the presentation half.
#   input       where a click lands in the X window's own coordinates. This
#               is the half that has no visual symptom and so would never be
#               noticed: with the presentation scale applied and the input
#               transform missing, the picture is perfect and every click is
#               off by the scale factor.
#
# THE CLICK IS DELIBERATELY NOT AT THE ORIGIN OR THE CENTRE OF ANYTHING.
# 0 x 1.25 is still 0, so a probe at the window origin passes at every scale
# and against every implementation, correct or not. (900,500) logical maps to
# (1125,625) raw and to nothing else -- a wrong answer MUST differ.
#
# ── WHAT EACH ARM IS FOR ─────────────────────────────────────────────────
#
#   scale-1            THE PREMISE. At scale 1 there is nothing to correct, so
#                      the instrument must read `native`. If it does not, the
#                      capture path itself resamples and every other verdict
#                      here is meaningless.
#   1.25-off           THE FALSIFIER, and it is expected RED-shaped: this is
#                      the bug, measured. It stays in the suite after the fix
#                      as the standing proof that the oracle can still see
#                      the failure it was built for -- an oracle that reports
#                      `native` no matter what is not an oracle.
#   1.25-on            the fix. (Added in P2, with the option itself.)
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="xw-scale"
CHECKER="$(cd "$(dirname "$0")" && pwd)/lib/checker.py"

# SHORT PATH ON PURPOSE. The Wayland socket lives inside HL_OUTDIR and the
# whole path, plus a null terminator, must fit in 108 bytes -- a scratch
# directory a few levels deep silently produces "compositor did not create a
# socket" and no other clue.
OUTDIR="${TMPDIR:-/tmp}/az-xw-$$"
rm -rf "$OUTDIR"; mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
export HL_OUTDIR

# ── A WALLPAPER THE CLIENT CAN NEVER PRODUCE ─────────────────────────────
#
# hl_start paints a flat MID-GREY wallpaper, and a bilinearly magnified
# black/white checkerboard's most common colour is mid-grey. So with the
# default wallpaper a region that missed the window entirely and sampled the
# desktop would produce a `scaled` verdict indistinguishable from the real
# thing -- the falsifier arm would pass while measuring nothing.
#
# Pure red appears nowhere in an achromatic checkerboard, at any filter
# setting, so "zero red pixels in the region" is proof the region is client
# content. hl_start only creates its wallpaper if the file is absent, which is
# what lets this replace it.
WALLPAPER_HEX="ff0000"
convert -size 64x64 "xc:#$WALLPAPER_HEX" "$OUTDIR/wallpaper.png"

# Nothing decorative between the client's pixels and the capture. A border, a
# titlebar, a rounded corner or a shadow would all put non-checkerboard pixels
# inside the sampled region and read as resampling.
FLAT_BASE="border_radius 0
borderpx 0
shadows 0
layer_shadows 0
shadows_blur_background 0
effects { shadow { enable 0 }; blur { enable 0 } }
animations 0
layer_animations 0
layout { titlebar { enable 0 } }"

# The sampled region, in OUTPUT PIXELS. Well inside a fullscreen window on a
# 1920x1080 output at every scale tested, and far from the parked cursor.
RX0=400; RY0=300; RX1=1200; RY1=800

# Where the click goes, in LOGICAL layout coordinates.
CLICK_X=900; CLICK_Y=500

near() { # near ACTUAL EXPECTED TOL -> "yes"/"no"
	local a="$1" e="$2" t="$3"
	[ -n "$a" ] || { echo "no"; return; }
	local d=$((a - e)); [ "$d" -lt 0 ] && d=$((-d))
	[ "$d" -le "$t" ] && echo "yes" || echo "no"
}

arm() { # arm NAME SCALE EXTRA_KDL EXP_VERDICT EXP_W EXP_H EXP_BTN_X EXP_BTN_Y
	local name="$1" scale="$2" extra="$3"
	local exp_verdict="$4" exp_w="$5" exp_h="$6" exp_bx="$7" exp_by="$8"

	echo
	echo "── arm $name (output scale $scale) ──"

	HL_SCALE1="$scale"
	export HL_SCALE1
	hl_start "$FLAT_BASE
$extra" >/dev/null 2>&1
	echo "  binary: $(hl_binary)"

	local disp
	if ! disp="$(hl_xdisplay)"; then
		hl_assert "$name: Xwayland came up" "no" "yes"
		hl_stop >/dev/null 2>&1
		return
	fi
	echo "  DISPLAY=$disp"

	hl_spawn_x11check "xw$name" 40 "x11-$name" >/dev/null
	if ! hl_wait_client_count 1 80; then
		hl_assert "$name: the X11 client mapped" "no" "yes"
		hl_stop >/dev/null 2>&1
		return
	fi

	# Fullscreen: the one state in which the window's logical box is exactly
	# the output's, so the sampled region needs no arithmetic and the whole
	# capture is client pixels.
	hl_dispatch "toggle_fullscreen" 1

	# ── PREMISE: the window really does cover the output ─────────────────
	# Asserted, not assumed. If fullscreen silently failed, the region below
	# would be sampling the wallpaper -- a flat grey, which has no greys near
	# the threshold and no equal neighbours either, and would therefore read
	# as a perfect `native`. The most convincing possible false pass.
	local mw mh cw ch cx cy fs
	mw="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .width")"
	mh="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .height")"
	cw="$(hl_client_field "xw$name" width)"
	ch="$(hl_client_field "xw$name" height)"
	cx="$(hl_client_field "xw$name" x)"
	cy="$(hl_client_field "xw$name" y)"
	fs="$(hl_client_field "xw$name" is_fullscreen)"
	hl_assert "$name: premise -- the client is fullscreen" "$fs" "true"
	hl_assert "$name: premise -- it covers the output logically" \
		"$cx,$cy,${cw}x$ch" "0,0,${mw}x$mh"

	# ── BOUNDARY 1: the size the X window was actually given ─────────────
	hl_x11check_wait_configure "$exp_w" "$exp_h" "x11-$name" 50 || true
	hl_assert "$name: X window configured in raw pixels" \
		"$(hl_x11check_last_configure "x11-$name")" "$exp_w $exp_h"

	# ── BOUNDARY 3: input coordinates ────────────────────────────────────
	# Before the capture, so the pointer can then be parked out of the way.
	hl_move "$CLICK_X" "$CLICK_Y"
	sleep 0.3
	hl_click "$CLICK_X" "$CLICK_Y"
	sleep 0.5
	local btn bx by
	btn="$(hl_x11check_last_button "x11-$name")"
	bx="${btn%% *}"; by="${btn##* }"
	hl_assert "$name: click x lands at $exp_bx (got '${bx:-none}')" \
		"$(near "${bx:-}" "$exp_bx" 3)" "yes"
	hl_assert "$name: click y lands at $exp_by (got '${by:-none}')" \
		"$(near "${by:-}" "$exp_by" 3)" "yes"

	# Park the pointer far from the sampled region. grim does not capture the
	# cursor, but a compositor-drawn one would land inside the region and read
	# as resampling for a reason that has nothing to do with scaling.
	hl_move 20 20
	sleep 0.5

	# ── BOUNDARY 2: the pixels ───────────────────────────────────────────
	hl_screenshot_output "$HL_MON" "cap-$name"
	local shot="$HL_OUTDIR/cap-$name.png"
	local dims
	dims="$(python3 -c "
from PIL import Image
import sys
w, h = Image.open(sys.argv[1]).size
print(f'{w}x{h}')" "$shot" 2>/dev/null)"
	# The region indices above are pixels, so a capture of an unexpected size
	# would be sampling somewhere else entirely.
	hl_assert "$name: premise -- capture is the output's pixel size" \
		"$dims" "${HL_WIDTH}x${HL_HEIGHT}"

	# PREMISE: the region is client content, not the desktop behind it. See
	# the WALLPAPER_HEX note above -- without this the `scaled` verdict is
	# satisfied by literally anything, including a missed window.
	hl_assert "$name: premise -- no wallpaper inside the sampled region" \
		"$(python3 "$CHECKER" count "$shot" $RX0 $RY0 $RX1 $RY1 "$WALLPAPER_HEX")" "0"

	local verdict
	verdict="$(python3 "$CHECKER" verdict "$shot" $RX0 $RY0 $RX1 $RY1 2>&1)"
	echo "  checker: $verdict"
	hl_assert "$name: presentation is $exp_verdict" \
		"${verdict%% *}" "$exp_verdict"

	hl_stop >/dev/null 2>&1
}

# ── the premise arm ──────────────────────────────────────────────────────
# At scale 1 nothing needs correcting, so this says one thing only: the
# instrument can recognise a 1:1 presentation when it is given one. Without
# it, a `scaled` verdict below could just as easily mean the capture path
# resamples, the checkerboard never painted, or the region is off the window.
arm scale-1 1 "" native 1920 1080 900 500

# ── the falsifier arm ────────────────────────────────────────────────────
# The bug itself, measured. This arm must report `scaled`, a 1536x864
# configure and an untransformed click for as long as the option is off --
# an oracle whose failing case has stopped failing has stopped testing.
arm 1.25-off 1.25 "" scaled 1536 864 900 500

hl_summary
