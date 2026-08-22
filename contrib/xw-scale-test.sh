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

# ── BREAK KNOBS ──────────────────────────────────────────────────────────
#
# Forwarded EXPLICITLY. hl_start launches the compositor under `env -i`, so an
# exported variable does not reach it -- a break set the obvious way is simply
# not applied, and the run comes back green while claiming to have been
# falsified. That has happened here before with MESA_VK_TRACE.
#
#   AZ_BREAK_X11_VIEW_SCALE=1   size the window in pixels but never tell the
#                               scene: the pixel gate must go red.
#   AZ_BREAK_X11_INPUT_SCALE=1  present correctly but drop the input
#                               conversion: the click gates must go red, and
#                               the pixel gate must stay green -- that pairing
#                               is the whole point, because the defect it
#                               models is invisible on screen.
#   AZ_BREAK_X11_ROOT_SIZE=1    let Xwayland see xdg-output again, so its
#                               X screen goes back to the LOGICAL desktop:
#                               the root-size gate and the click outside
#                               the old screen must go red, and every gate
#                               inside the old screen must stay green.
BREAKS=""
for v in AZ_BREAK_X11_VIEW_SCALE AZ_BREAK_X11_INPUT_SCALE AZ_BREAK_X11_ROOT_SIZE; do
	eval "val=\${$v:-}"
	[ -n "$val" ] && BREAKS="$BREAKS $v=$val"
done
[ -n "$BREAKS" ] && echo "xw-scale: BREAKS =$BREAKS"

# ARMS filters which arms run, so a falsification run costs one compositor
# instead of three. Empty means all of them.
ARMS="${ARMS:-}"
wanted() {
	[ -z "$ARMS" ] && return 0
	case " $ARMS " in *" $1 "*) return 0 ;; esac
	return 1
}

near() { # near ACTUAL EXPECTED TOL -> "yes"/"no"
	local a="$1" e="$2" t="$3"
	[ -n "$a" ] || { echo "no"; return; }
	local d=$((a - e)); [ "$d" -lt 0 ] && d=$((-d))
	[ "$d" -le "$t" ] && echo "yes" || echo "no"
}

arm() { # arm NAME SCALE EXTRA_KDL EXP_VERDICT EXP_W EXP_H EXP_BTN_X EXP_BTN_Y [POST]
	local name="$1" scale="$2" extra="$3"
	local exp_verdict="$4" exp_w="$5" exp_h="$6" exp_bx="$7" exp_by="$8"
	# POST runs once the window is fullscreen and before anything is measured.
	# It is how an arm asks "does the window come BACK from this correctly",
	# which is a different question from "is it right when nothing has
	# happened to it" and is where a stale clip or a stale dest-size hides.
	local post="${9:-}"

	wanted "$name" || return 0

	echo
	echo "── arm $name (output scale $scale) ──"

	HL_SCALE1="$scale"
	HL_ENV="$BREAKS"
	export HL_SCALE1 HL_ENV
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

	if [ -n "$post" ]; then
		eval "$post"
	fi

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

	# What the X SCREEN measures, for the log only. It comes from Xwayland's
	# own reading of the wl_outputs, not from anything asteroidz configures,
	# and an X11 app that sizes itself from the screen rather than from its
	# configure will use it. Reported so the gap is visible when it matters;
	# not asserted, because it is not this compositor's number to set.
	echo "  X screen: $(grep '^screen ' "$HL_OUTDIR/x11-$name.log" | head -1)"

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
#
# It sets the option to 0 EXPLICITLY rather than leaving the config empty and
# taking whatever the default is. It did rely on the default, and the default
# then flipped to 1: this arm would have started reporting `native`, failed,
# and read as a regression in the feature it was written to measure. What this
# arm means is "with the option off", which is not the same statement as "with
# the option unmentioned" and must not be spelled the same way.
arm 1.25-off 1.25 "xwayland_force_scale_one 0" scaled 1536 864 900 500

# ── the fix ──────────────────────────────────────────────────────────────
# Same output, same client, one option. The X window is asked for the real
# pixel count, the scene presents that buffer across 1536 logical pixels --
# which is 1920 device pixels -- and the click that was aimed at logical
# (900,500) arrives at raw (1125,625). All three boundaries move together;
# any one of them left behind shows up as its own failed assertion rather
# than as a vague "it still looks wrong".
arm 1.25-on 1.25 "xwayland_force_scale_one 1" native 1920 1080 1125 625

# ── a window that is NOT fullscreen ──────────────────────────────────────
#
# Everything above runs fullscreen, at the origin, with no border. That is the
# case the option is for, and it is also the easiest one: the surface clip
# covers the whole window and both edges land on whole device pixels.
#
# A TILED window has a border, sits at a non-zero origin, and is clipped to
# less than the surface. The clip is the part that has no separate boundary of
# its own -- it is expressed in SURFACE coordinates, which for these windows
# are raw pixels, while every clip box in the compositor is computed from
# c->geom and is logical. Left unconverted the window is cropped to 1/scale of
# itself, and what shows through the missing fifth is the desktop.
#
# So this arm asserts the crop rather than the sampler: ZERO wallpaper pixels
# anywhere inside the window's content box. It deliberately does NOT assert
# `native`. At 1.25x a window whose logical edges fall between device pixels
# can come out one pixel wider or narrower than its buffer -- the caveat
# documented on the option itself -- and demanding bit-exactness here would be
# asserting something the implementation does not claim.
tiled_arm() { # tiled_arm NAME SCALE EXTRA_KDL [POST] [SKIP_CONFIGURE]
	local name="$1" scale="$2" extra="$3" post="${4:-}" skip_cfg="${5:-}"
	local bw=2

	wanted "$name" || return 0

	echo
	echo "── arm $name (output scale $scale, tiled, ${bw}px border) ──"

	HL_SCALE1="$scale"
	HL_ENV="$BREAKS"
	export HL_SCALE1 HL_ENV
	hl_start "$FLAT_BASE
borderpx $bw
$extra" >/dev/null 2>&1

	hl_xdisplay >/dev/null || {
		hl_assert "$name: Xwayland came up" "no" "yes"
		hl_stop >/dev/null 2>&1
		return
	}
	hl_spawn_x11check "xw$name" 40 "x11-$name" >/dev/null
	if ! hl_wait_client_count 1 80; then
		hl_assert "$name: the X11 client mapped" "no" "yes"
		hl_stop >/dev/null 2>&1
		return
	fi
	sleep 1

	if [ -n "$post" ]; then
		eval "$post"
	fi

	local cx cy cw ch
	cx="$(hl_client_field "xw$name" x)"; cy="$(hl_client_field "xw$name" y)"
	cw="$(hl_client_field "xw$name" width)"; ch="$(hl_client_field "xw$name" height)"

	# PREMISE: it really is tiled somewhere other than the origin, with a
	# border. Fullscreen or an origin of 0,0 would make this arm a slower copy
	# of the ones above.
	hl_assert "$name: premise -- fullscreen is off" \
		"$(hl_client_field "xw$name" is_fullscreen)" "false"
	hl_assert "$name: premise -- the window is not at the layout origin" \
		"$([ "$cx" -gt 0 ] && [ "$cy" -gt 0 ] && echo yes || echo no)" "yes"

	# The expectation is derived from the window's own logical box, so this
	# measures the CONVERSION and not the layout's gap arithmetic.
	local exp
	exp="$(python3 -c "
import sys
cw, ch, bw, s = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4])
print(round((cw - 2*bw) * s), round((ch - 2*bw) * s))" "$cw" "$ch" "$bw" "$scale")"
	if [ -n "$skip_cfg" ]; then
		# ── WHY THIS ARM DOES NOT COMPARE THOSE TWO NUMBERS ──────────────
		#
		# The assertion below ties the size the X client was last given to the
		# window's logical box as the compositor reports it. After an overview
		# round trip those two genuinely disagree: the compositor's own
		# bookkeeping is left holding the overview-scaled box (1221x683) while
		# the client was last configured for the ordinary tiled one
		# (1890x1050, which is exactly 1516x844 logical x 1.25). Both numbers
		# are internally consistent -- the conversion is right in each -- they
		# are just measurements of different moments.
		#
		# That divergence is not this feature's: exiting overview leaves a
		# window un-fullscreened and its geometry un-restored for a WAYLAND
		# client too, at scale 1, with the option off. Asserting the pair here
		# would be a test of overview's bookkeeping wearing this fixture's
		# name, and it would fail whatever the scaling code did.
		#
		# What IS this feature's -- that the surface is not cropped and the
		# window is not showing desktop through itself -- is asserted below
		# and is unaffected.
		echo "  configure: $(hl_x11check_last_configure "x11-$name") (not asserted, see the comment)"
	else
		# Polled, not slept on: an X11 window is configured several times on
		# the way to its final size, and a fixed wait reads whichever
		# intermediate size the race happened to leave behind.
		hl_x11check_wait_configure "${exp% *}" "${exp#* }" "x11-$name" 50 || true
		hl_assert "$name: X window configured to round(logical x $scale)" \
			"$(hl_x11check_last_configure "x11-$name")" "$exp"
	fi

	hl_move 20 20
	sleep 0.5
	hl_screenshot_output "$HL_MON" "cap-$name"

	# The content box in OUTPUT PIXELS, inset by two pixels on every side so
	# the assertion is about the crop and not about the one-pixel edge
	# remainder the option's own documentation admits to.
	local box
	box="$(python3 -c "
import sys
cx, cy, cw, ch, bw = (int(v) for v in sys.argv[1:6])
s = float(sys.argv[6])
x0 = round((cx + bw) * s) + 2
y0 = round((cy + bw) * s) + 2
x1 = round((cx + cw - bw) * s) - 2
y1 = round((cy + ch - bw) * s) - 2
print(x0, y0, x1, y1)" "$cx" "$cy" "$cw" "$ch" "$bw" "$scale")"
	echo "  content box (px): $box"

	hl_assert "$name: no desktop shows through the window's content box" \
		"$(python3 "$CHECKER" count "$HL_OUTDIR/cap-$name.png" $box "$WALLPAPER_HEX")" "0"

	# And the content really is the client's, not a coincidence: a box full of
	# wallpaper would also report zero RED if the wallpaper had been repainted
	# by something else, so say what it IS as well as what it is not.
	local m
	m="$(python3 "$CHECKER" measure "$HL_OUTDIR/cap-$name.png" $box)"
	echo "  content box: $m"
	hl_assert "$name: premise -- the content box holds a checkerboard" \
		"$(python3 -c "
import sys
d = dict(kv.split('=') for kv in sys.argv[1:])
# A checkerboard, magnified or not, is a long way from a flat fill: fewer than
# nine tenths of its neighbours are equal. A flat region has essentially all
# of them, which is what an empty or wallpapered box would look like.
print('yes' if int(d['dup_h']) < 0.9 * int(d['px']) else 'no')" $m)" "yes"

	hl_stop >/dev/null 2>&1
}

tiled_arm 1.25-tiled 1.25 "xwayland_force_scale_one 1"

# ── with animations, and after a round trip through overview ─────────────
#
# Both of these override the scene buffer's destination size themselves, on
# every drawn frame, from numbers that are a mix of the surface's own size
# (pixels) and the window's logical box. The view scale lives on the scene
# surface and is re-applied on every commit; a per-frame dest-size written
# from the wrong space would win over it, and it would win only while the
# animation or the overview is running -- so a fixture that measures a
# perfectly still desktop cannot see it.
#
# The animation arm measures after the open animation has SETTLED, which is
# the state a stale dest-size survives into. The overview arm goes in and
# comes back out: overview is the one place that deliberately shrinks a live
# surface, and if it restores the wrong number the window stays a thumbnail
# or comes back cropped.
arm 1.25-anim 1.25 "xwayland_force_scale_one 1
animations 1" native 1920 1080 1125 625

# THE OVERVIEW ARM IS A TILED ARM, AND THAT IS NOT A COMPROMISE.
#
# It was written as a fullscreen arm first, and failed: after
# overview-in-overview-out the window was at 157,120 1221x683 and no longer
# fullscreen. The obvious reading is that this feature broke the restore. It
# did not. The same round trip was run at scale 1, with the option off, and
# with a WAYLAND client instead of an X11 one, and all three come back exactly
# the same way -- a window that was fullscreen before overview is tiled after
# it, for every client this compositor has. That is pre-existing overview
# behaviour and nothing to do with scaling.
#
# So the arm asserts what is actually interesting: the window comes back in a
# state where the conversion still holds -- configured to round(logical x
# scale) for its NEW box, with no desktop showing through it. A stale clip or
# a stale dest-size left behind by overview would land exactly there.
tiled_arm 1.25-overview 1.25 "xwayland_force_scale_one 1" \
	'hl_dispatch "toggle_overview" 1.5; hl_dispatch "toggle_overview" 1.5; sleep 1' \
	skip-configure

# ── THE X SCREEN IS A FIFTH BOUNDARY, AND IT IS ONE WE CAN MOVE ──────────
#
# Xwayland sizes its X screen from what it is told the outputs are: 1536x864
# for a 1920x1080 display at 1.25. This option sizes windows in DEVICE PIXELS,
# so a fullscreen window used to be 1920x1080 inside a 1536x864 screen and
# overflowed it by exactly the scale factor. X11 requires the pointer to be
# inside the root window, so every position beyond the screen was clamped to
# its edge before the client was told, and the client was told window-relative
# coordinates derived from that clamped position:
#
#     logical (900,500)  -> 1125,625    correct
#     logical (1450,800) -> 1535,863    CLAMPED (should be 1812,1000)
#
# For a long time this arm asserted those clamped values, on the reasoning
# that Xwayland derives the screen from the outputs' logical geometry and
# wlroots 0.20 exposes no way to tell it otherwise. The second half of that
# was wrong. RandR really is refused -- `xrandr --fb` against Xwayland returns
# success and changes nothing -- but the screen size is not read from one
# fixed source. xwayland-output.c writes xwl_output->width/height from
# xdg-output's logical_size when that protocol is present and from
# wl_output.mode when it is not, and output_get_new_size() reads nothing else;
# wl_output.scale never enters the calculation. So hiding xdg-output from the
# Xwayland client alone -- see xdg_output_visible_to() in
# src/ext-protocol/modern.h -- gives it an X screen in device pixels, the same
# unit its windows were already sized and positioned in, and takes fractional
# scaling away from nobody.
#
# THE ASSERTIONS ARE THE UNCLAMPED VALUES, and the arm carries its own
# falsifier: AZ_BREAK_X11_ROOT_SIZE=1 puts xdg-output back and restores the
# clamp exactly, which is what the break arm below asserts. Without that pair
# a green run here would be indistinguishable from an arm that stopped
# reaching the overflow band at all.
screen_arm() { # screen_arm NAME SCALE WANT_SCREEN WANT_FAR SCREEN_IS FAR_IS
	local name="$1" scale="$2" want_screen="$3" want_far="$4"
	# Named by the caller so a GREEN break arm still reads as the clamp it is.
	# With one fixed wording the falsifier would print "sized in device pixels"
	# while asserting the logical desktop, which is worse than no label.
	local screen_is="$5" far_is="$6"
	wanted "$name" || return 0

	echo
	echo "── arm $name (output scale $scale) ──"
	HL_SCALE1="$scale"; HL_ENV="$BREAKS"; export HL_SCALE1 HL_ENV
	hl_start "$FLAT_BASE
xwayland_force_scale_one 1" >/dev/null 2>&1
	hl_xdisplay >/dev/null || {
		hl_assert "$name: Xwayland came up" "no" "yes"; hl_stop >/dev/null 2>&1; return; }
	hl_spawn_x11check "xw$name" 40 "x11-$name" >/dev/null
	hl_wait_client_count 1 80 || {
		hl_assert "$name: the X11 client mapped" "no" "yes"; hl_stop >/dev/null 2>&1; return; }
	hl_dispatch "toggle_fullscreen" 1
	hl_x11check_wait_configure 1920 1080 "x11-$name" 50 || true

	local scr
	scr="$(grep '^screen ' "$HL_OUTDIR/x11-$name.log" | head -1 | awk '{print $2, $3}')"
	hl_assert "$name: the X screen is $screen_is" "$scr" "$want_screen"
	# PREMISE. Every click assertion below is about a window sized in pixels;
	# if the window came out logical the far probe would land inside it by
	# accident and the arm would pass while measuring nothing.
	hl_assert "$name: premise -- the window is sized in device pixels" \
		"$(hl_x11check_last_configure "x11-$name")" "1920 1080"

	# Inside the OLD screen: exact either way, so this one gate must stay
	# green even under the break. It separates "the root got bigger" from
	# "the input path broke".
	hl_move 900 500; sleep 0.3; hl_click 900 500; sleep 0.5
	hl_assert "$name: inside the old X screen, the click is exact" \
		"$(hl_x11check_last_button "x11-$name")" "1125 625"

	# Past the old screen edge: 1450 x 1.25 = 1812, 800 x 1.25 = 1000. This is
	# the band that used to clamp to 1535 863.
	hl_move 1450 800; sleep 0.3; hl_click 1450 800; sleep 0.5
	hl_assert "$name: past the old X screen edge, the click $far_is" \
		"$(hl_x11check_last_button "x11-$name")" "$want_far"

	hl_stop >/dev/null 2>&1
}

screen_arm 1.25-screen 1.25 "1920 1080" "1812 1000" \
	"sized in device pixels" "still lands"

# THE FALSIFIER FOR THE ARM ABOVE. Same scale, same option, same probes --
# only xdg-output is visible to Xwayland again, which is the state the clamp
# was discovered in. It must report the LOGICAL screen and the clamped click,
# and its inside-the-old-screen gate must still pass: an arm that went red
# everywhere would prove the break broke the input path rather than the root
# size. Run this against a build without xdg_output_visible_to() and it is the
# arm above that is red instead.
if [ -z "${AZ_BREAK_X11_ROOT_SIZE:-}" ]; then
	( export AZ_BREAK_X11_ROOT_SIZE=1
	  BREAKS="$BREAKS AZ_BREAK_X11_ROOT_SIZE=1"
	  screen_arm 1.25-screen-clamped 1.25 "1536 864" "1535 863" \
		"back to the logical desktop" "clamps to the edge again" )
fi

# ── THE OPT-OUT, WHICH IS NO LONGER ABOUT THE CLAMP ───────────────────────
#
# This arm was written when the clamp above had no fix and taking one window
# out of pixel sizing was the only way to reach its bottom third. The X screen
# is device-sized now, so that is not what the option is for any more: it is
# for a window that is better off in logical units whatever the screen size --
# one that reads the X screen's DPI and sizes its own UI from it, or one whose
# fonts a user simply prefers larger.
#
# It still has to keep working, and under a device-sized screen it is the
# window rather than the screen that has changed shape: sized logically at
# 1536x864 inside a 1920x1080 root, with no edge to clamp against from either
# direction. THE ASSERTIONS ARE THE LOGICAL COORDINATES, unchanged, and they
# are the same pointer positions the arm above probes in pixels -- which is
# what makes the pair meaningful: identical input, two different correct
# answers, decided by the rule alone. Run against a build without the
# per-window override and this arm reports 1812 1000 and fails.
optout_arm() { # optout_arm NAME SCALE
	local name="$1" scale="$2"
	wanted "$name" || return 0

	echo
	echo "── arm $name (output scale $scale) ──"
	HL_SCALE1="$scale"; HL_ENV="$BREAKS"; export HL_SCALE1 HL_ENV
	hl_start "$FLAT_BASE
xwayland_force_scale_one 1
window-rule { match title=xw$name; xwayland-scale-one 0 }" >/dev/null 2>&1
	hl_xdisplay >/dev/null || {
		hl_assert "$name: Xwayland came up" "no" "yes"; hl_stop >/dev/null 2>&1; return; }
	hl_spawn_x11check "xw$name" 40 "x11-$name" >/dev/null
	hl_wait_client_count 1 80 || {
		hl_assert "$name: the X11 client mapped" "no" "yes"; hl_stop >/dev/null 2>&1; return; }
	hl_dispatch "toggle_fullscreen" 1
	hl_x11check_wait_configure 1536 864 "x11-$name" 50 || true

	# PREMISE, and the whole point of the arm: the rule won over the global, so
	# the window is sized in LOGICAL units and fits the X screen. Without this
	# the click assertions below could pass for the wrong reason -- a window
	# that never overflowed because the arm failed to go fullscreen at all.
	hl_assert "$name: premise -- the rule beat the global; sized logically" \
		"$(hl_x11check_last_configure "x11-$name")" "1536 864"

	hl_move 900 500; sleep 0.3; hl_click 900 500; sleep 0.5
	hl_assert "$name: inside the old screen, still exact" \
		"$(hl_x11check_last_button "x11-$name")" "900 500"

	# The position that clamped to 1535 863 with the option on.
	hl_move 1450 800; sleep 0.3; hl_click 1450 800; sleep 0.5
	hl_assert "$name: and where it used to clamp, the click now lands" \
		"$(hl_x11check_last_button "x11-$name")" "1450 800"

	hl_stop >/dev/null 2>&1
}

optout_arm 1.25-scale-one-optout 1.25

hl_summary
