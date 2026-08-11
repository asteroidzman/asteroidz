#!/usr/bin/env bash
# avk-cursor-owner-test.sh — the cursor has exactly one owner.
#
# M3.5E stage 1 made asteroidz own the cursor image: one image, chosen at the
# SHARPEST scale in the layout, pushed with wlr_cursor_set_buffer(..., scale)
# so wlroots divides it back to a logical size that is identical on every
# output. Seven call sites never got the memo and kept calling wlroots'
# wlr_cursor_set_xcursor() -- the compositor-driven move and resize cursors,
# the screenshot UI's crosshair, two "default" resets and the one inside
# reapply_cursor_style().
#
# That is a DIFFERENT ownership model, not a different spelling of the same
# one. wlroots picks a PER-OUTPUT image at each output's NATIVE scale and hands
# it to wlr_output_cursor_set_buffer(), which takes no scale argument at all.
# So after such a call, wlr_output_cursor describes wlroots' image while
# az_cursor still holds asteroidz's -- and az_avk_emit_cursors() draws
# az_cursor's PIXELS into wlroots' BOX.
#
# Two symptoms, one cause, both found on a real desktop and neither by this
# suite:
#
#   - dragging a window showed no "grab" cursor at all, because the shape
#     wlroots selected was never given to az_cursor;
#   - resizing a window on the coarser of two outputs made the arrow BIGGER:
#     asteroidz's 36px image at scale 1.5 is 24 output px, and wlroots' own
#     choice for a scale-1.0 output was ~28.
#
# WHY NOTHING CAUGHT IT
#
# It is invisible with a hardware cursor plane -- there wlroots' image is what
# reaches the plane and asteroidz's never enters the frame -- so the daily
# driver never showed it. It needs THREE things at once, and no existing test
# had more than two: forced software composition, two outputs at DIFFERENT
# scales, and a COMPOSITOR-DRIVEN move or resize. Every cursor test here drives
# shapes from clients, which take the az_cursor path and therefore agree with
# themselves.
#
# WHAT IS ASSERTED
#
# Not pixels. `cursor_geometry_mismatch` counts frames where wlr_output_cursor's
# box did not describe the image AVK was about to draw into it:
#
#     expected = image_size / az_cursor.scale * output_scale
#
# Zero is the only correct value. And `cursor_xcursor_sets` must MOVE when the
# compositor selects a shape, because a selection that does not reach az_cursor
# is precisely the bug.
#
#   BREAK=wlroots-move-resize   the SHIPPED defect exactly: only the move and
#                           resize shapes go to wlroots, so az_cursor still
#                           holds an image and AVK still composites -- and the
#                           disagreement appears as cursor_geometry_mismatch,
#                           a box that does not fit the image. MUST FAIL.
#
#   BREAK=wlroots-xcursor   the whole ownership model handed to wlroots. A
#                           louder, different failure: az_cursor ends up with
#                           no image at all and cursor_no_image fires. Kept
#                           because it covers the model rather than the seven
#                           particular call sites. MUST FAIL.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-cursor-owner"
BREAK="${BREAK:-}"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-cursor-owner-$$"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=2
HL_ENV="ASTEROIDZ_RENDERER=avk ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1"
[ "$BREAK" = wlroots-xcursor ] && HL_ENV="$HL_ENV AZ_CURSOR_WLROOTS_XCURSOR=1"
[ "$BREAK" = wlroots-move-resize ] && \
	HL_ENV="$HL_ENV AZ_CURSOR_WLROOTS_XCURSOR=moveresize"
# The live layout that found the bug: 1.5 and 1.0.
HL_SCALE1=1.5
HL_SCALE2=1.0
export HL_OUTDIR HL_OUTPUTS HL_ENV HL_SCALE1 HL_SCALE2

echo "binary under test: $HL_ASTEROIDZ"

# MIXED scales, and that is the whole point. At equal scales both owners
# compute the same box and the bug is invisible -- which is how a two-output
# suite can still be blind to it.
# cursor_size is PINNED, and not incidentally. The two ownership models only
# disagree numerically when the theme's nearest available size differs between
# base*sharpest_scale and base*output_scale. At the harness default they happen
# to coincide, both models produce the same pixel size, and the size half of
# this bug is invisible -- a test that passes because two wrong answers agree.
# 28 is the size the live session ran, and the size the bug was found at.
hl_start "cursor_hide_timeout 30
cursor_theme Adwaita
cursor_size 28"
sleep 2

stat() { hl_get "get avk-stats" | jq -r ".$1 // 0"; }

# The premise the whole test rests on. Two outputs at the SAME scale compute
# the same box under either owner, so the bug is invisible and every assertion
# below would pass on a broken build. Asserted, not assumed: a monitorrule
# block that fails to parse leaves both outputs at scale 1 and says nothing.
S1="$(hl_get "get all-monitors" | jq -r '.monitors[]|select(.name=="HEADLESS-1").scale')"
S2="$(hl_get "get all-monitors" | jq -r '.monitors[]|select(.name=="HEADLESS-2").scale')"
echo "-- output scales: HEADLESS-1=$S1  HEADLESS-2=$S2 --"
hl_assert "the two outputs really are at different scales" \
	"$([ "$S1" != "$S2" ] && echo true || echo false)" "true"

hl_spawn_kitty "owner" >/dev/null 2>&1
hl_wait_client_count 1 40 >/dev/null 2>&1
sleep 1

# Where the window actually IS. moveresize() resolves its target with
# xytonode(cursor->x, cursor->y): with the pointer over empty desktop it finds
# nothing, returns 0, and the IPC call still answers {"success":true}. A
# hard-coded coordinate therefore produces a test that passes by doing nothing.
CX="$(hl_get "get all-clients" | jq -r '.clients[0] | (.x + .width/2) | floor')"
CY="$(hl_get "get all-clients" | jq -r '.clients[0] | (.y + .height/2) | floor')"
echo "-- window centre at $CX,$CY --"
hl_assert "the test window has a resolvable centre" \
	"$([ -n "$CX" ] && [ "$CX" != "null" ] && echo true || echo false)" "true"

hl_move "$CX" "$CY"
sleep 0.5

echo
echo "-- the premise: AVK is compositing the cursor, not a plane --"
SW="$(stat software_cursor_frames)"
hl_assert "AVK composites the cursor ($SW frames)" \
	"$([ "$SW" -gt 0 ] && echo true || echo false)" "true"
hl_assert "the hardware plane is not carrying it" "$(stat hardware_cursor_frames)" "0"

# ── a compositor-driven shape must reach az_cursor ──────────────────────────
#
# begin_move_or_resize() selects "grab" / "<corner>-resize". If that selection
# goes to wlroots instead, this counter does not move -- and the cursor on
# screen keeps whatever shape it had, which is exactly what a real desktop
# showed.
#
# A release between the two, because moveresize() returns immediately unless
# cursor_mode is CurNormal: without it the second dispatch is a silent no-op
# and its assertion would be measuring the first one's result.
for mode in curmove curresize; do
	echo
	echo "-- move_resize,$mode --"
	hl_move "$CX" "$CY"
	sleep 0.3
	BEFORE="$(stat cursor_xcursor_sets)"
	hl_dispatch "move_resize,$mode" 0.6
	AFTER="$(stat cursor_xcursor_sets)"
	echo "  cursor_xcursor_sets $BEFORE -> $AFTER"
	hl_assert "[$mode] the cursor the compositor chose reached az_cursor" \
		"$([ "$AFTER" -gt "$BEFORE" ] && echo true || echo false)" "true"

	# Drag it across BOTH outputs while that cursor is selected, so the
	# composited box is computed at both scales. This is where a per-output
	# image at native scale disagrees with one image at the sharpest scale.
	for x in 400 900 1500 2100 2600 3000; do
		hl_move "$x" 500
		sleep 0.12
	done
	sleep 0.4
	hl_click "$CX" "$CY" >/dev/null 2>&1   # release: back to CurNormal
	sleep 0.4
done

echo
echo "-- the box AVK draws into must describe the image it draws --"
MM="$(stat cursor_geometry_mismatch)"
SWF="$(stat software_cursor_frames)"
echo "  cursor_geometry_mismatch $MM over $SWF composited cursor frames"
hl_assert "wlroots' cursor box always matched asteroidz's image" "$MM" "0"

hl_assert "no cursor went missing" "$(stat cursor_no_image)" "0"
hl_assert "no cursor import failed" "$(stat cursor_import_failures)" "0"

hl_stop
hl_summary
