#!/usr/bin/env bash
# xw-mixed-test.sh — one X11 window, two displays, two different scales.
#
# ── WHY A SECOND OUTPUT IS NOT OPTIONAL HERE ─────────────────────────────
#
# With one output there is only one scale, so every wrong answer coincides
# with the right one: a per-client scale, a global scale, the sharpest scale
# in the layout and "whatever selmon happens to be" all produce the same
# number, and a single-output fixture would pass against all four. The whole
# question this feature raises -- which display's pixels is this window being
# measured in -- only exists once there are two answers.
#
# So the layout is deliberately mismatched in BOTH resolution and scale:
#
#   HEADLESS-1   1920x1080 at 1.25   logical 1536x864    layout x = 0
#   HEADLESS-2   2880x1620 at 1.5    logical 1920x1080   layout x = 1536
#
# A fullscreen X window must be configured 1920x1080 on the first and
# 2880x1620 on the second. Those differ in both axes and neither is derivable
# from the other, so an implementation that shared one scale across the layout
# fails on whichever output it is not tracking.
#
# ── WHY THESE EXACT NUMBERS ──────────────────────────────────────────────
#
# Both are chosen so that logical x scale is a whole number of pixels:
# 1536 x 1.25 = 1920 and 1920 x 1.5 = 2880, exactly. That is not to make the
# feature look better than it is -- it is so this fixture measures monitor
# ATTRIBUTION rather than rounding. A mode like 2560x1440 at 1.5 has a logical
# width of 1706.67, which wlroots rounds to 1707, and 1707 x 1.5 is 2560.5:
# the configure comes back one pixel wide and the assertion fails for a reason
# that has nothing to do with which monitor was picked. The rounding remainder
# is real and is documented as a caveat on the option itself; it is not what
# this file is for.
#
# ── THE COORDINATES ARE MONITOR-RELATIVE ─────────────────────────────────
#
# Every geometry assertion below subtracts the monitor's own layout origin.
# A test that assumes a window starts at 0,0 passes headlessly on a single
# output and fails the moment there is a second one -- which is exactly the
# case here, where the second output starts at logical x = 1536.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="xw-mixed"
CHECKER="$(cd "$(dirname "$0")" && pwd)/lib/checker.py"

# Short: the Wayland socket path has a 108-byte cap and lives in here.
OUTDIR="${TMPDIR:-/tmp}/az-xwm-$$"
rm -rf "$OUTDIR"; mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
export HL_OUTDIR

# See contrib/xw-scale-test.sh for why the wallpaper is repainted: a region
# that missed the window has to be distinguishable from one that did not.
WALLPAPER_HEX="ff0000"
convert -size 64x64 "xc:#$WALLPAPER_HEX" "$OUTDIR/wallpaper.png" 2>/dev/null

HL_OUTPUTS=2
HL_WIDTH=1920;  HL_HEIGHT=1080;  HL_SCALE1=1.25
HL_WIDTH2=2880; HL_HEIGHT2=1620; HL_SCALE2=1.5
# The second output's LAYOUT x is LOGICAL, so it is the first output's logical
# width (1920/1.25), not its pixel width. Left at the default of HL_WIDTH the
# two outputs would sit 384 logical pixels apart with a gap between them.
HL_X2=1536
export HL_OUTPUTS HL_WIDTH HL_HEIGHT HL_WIDTH2 HL_HEIGHT2 HL_SCALE1 HL_SCALE2 HL_X2

# Break knob, forwarded explicitly because hl_start uses `env -i`:
#   AZ_BREAK_X11_MON_MIGRATE=1  never re-evaluate the scale when a window
#                               changes monitor. The window keeps the scale of
#                               the display it opened on, which is the single
#                               most likely way to get this wrong -- and on
#                               one output it is indistinguishable from
#                               correct.
BREAKS=""
[ -n "${AZ_BREAK_X11_MON_MIGRATE:-}" ] &&
	BREAKS="AZ_BREAK_X11_MON_MIGRATE=$AZ_BREAK_X11_MON_MIGRATE"
[ -n "$BREAKS" ] && echo "xw-mixed: BREAKS = $BREAKS"

HL_ENV="$BREAKS"
export HL_ENV

near() { # near ACTUAL EXPECTED TOL -> yes/no
	local a="$1" e="$2" t="$3"
	[ -n "$a" ] || { echo "no"; return; }
	local d=$((a - e)); [ "$d" -lt 0 ] && d=$((-d))
	[ "$d" -le "$t" ] && echo "yes" || echo "no"
}

hl_start "border_radius 0
borderpx 0
shadows 0
layer_shadows 0
effects { shadow { enable 0 }; blur { enable 0 } }
animations 0
layer_animations 0
layout { titlebar { enable 0 } }
xwayland_force_scale_one 1" >/dev/null 2>&1

echo "binary: $(hl_binary)"
trap 'hl_stop >/dev/null 2>&1' EXIT

hl_assert "two outputs are up" "$(hl_monitor_count)" "2"

# PREMISE: the scales actually took. `monitorrule { scale ... }` is parsed,
# ignored and forgotten, so a fixture can claim mixed scales for weeks while
# running at 1,1 -- in which case every configure below is 1920x1080 on both
# outputs and the whole file proves nothing.
m1w="$(hl_get "get all-monitors" | jq -r '.monitors[] | select(.name=="HEADLESS-1") | .width')"
m2w="$(hl_get "get all-monitors" | jq -r '.monitors[] | select(.name=="HEADLESS-2") | .width')"
m2x="$(hl_get "get all-monitors" | jq -r '.monitors[] | select(.name=="HEADLESS-2") | .x')"
hl_assert "premise -- HEADLESS-1 is 1536 logical wide (scale 1.25 applied)" "$m1w" "1536"
hl_assert "premise -- HEADLESS-2 is 1920 logical wide (scale 1.5 applied)" "$m2w" "1920"
hl_assert "premise -- the outputs are adjacent, not overlapping or gapped" "$m2x" "1536"

disp="$(hl_xdisplay)" || { hl_assert "Xwayland came up" "no" "yes"; hl_summary; exit; }
echo "DISPLAY=$disp"

hl_spawn_x11check xwmix 60 x11 >/dev/null
hl_wait_client_count 1 80 || hl_assert "the X11 client mapped" "no" "yes"

# WHICH OUTPUT THE WINDOW OPENS ON IS NOT THIS TEST'S TO ASSUME. The first run
# of this file assumed HEADLESS-1 and got HEADLESS-2 -- whichever one selmon
# happens to be at startup -- so the "on mon1" half measured a window that was
# never there and the "moved to mon2" half measured a move that never
# happened. Both halves reported on the same monitor and the migration this
# file exists for was not exercised at all. Placed explicitly instead.
hl_dispatch "tag_monitor,HEADLESS-1" 1
hl_dispatch "toggle_fullscreen" 1
sleep 1
hl_assert "premise -- the window starts on HEADLESS-1" \
	"$(hl_client_field xwmix x)" "0"

# ── on HEADLESS-1, at 1.25 ───────────────────────────────────────────────
hl_x11check_wait_configure 1920 1080 x11 50 || true
hl_assert "mon1: configured in HEADLESS-1's pixels" \
	"$(hl_x11check_last_configure x11)" "1920 1080"

cx="$(hl_client_field xwmix x)"
hl_assert "mon1: the window sits at the monitor's own origin" \
	"$((cx - 0))" "0"

hl_move 900 500; sleep 0.3
hl_click 900 500; sleep 0.5
btn="$(hl_x11check_last_button x11)"
hl_assert "mon1: click x -> 1125 at scale 1.25 (got '${btn%% *}')" \
	"$(near "${btn%% *}" 1125 3)" "yes"
hl_assert "mon1: click y -> 625 at scale 1.25 (got '${btn##* }')" \
	"$(near "${btn##* }" 625 3)" "yes"

hl_move 20 20; sleep 0.5
hl_screenshot_output HEADLESS-1 cap-mon1
hl_assert "mon1: premise -- no wallpaper inside the sampled region" \
	"$(python3 "$CHECKER" count "$HL_OUTDIR/cap-mon1.png" 400 300 1200 800 "$WALLPAPER_HEX")" "0"
v1="$(python3 "$CHECKER" verdict "$HL_OUTDIR/cap-mon1.png" 400 300 1200 800 2>&1)"
echo "  mon1 checker: $v1"
hl_assert "mon1: presented 1:1" "${v1%% *}" "native"

# ── move it to HEADLESS-2, at 1.5 ────────────────────────────────────────
#
# THIS IS THE ASSERTION THE SECOND OUTPUT EXISTS FOR. 2880x1620 is not
# 1920x1080 in either axis, and it is not the first output's number scaled by
# anything -- so a shared or stale scale cannot produce it.
hl_dispatch "tag_monitor,HEADLESS-2" 1
sleep 1
hl_x11check_wait_configure 2880 1620 x11 60 || true
hl_assert "mon2: reconfigured in HEADLESS-2's pixels" \
	"$(hl_x11check_last_configure x11)" "2880 1620"

cx2="$(hl_client_field xwmix x)"
hl_assert "mon2: the window sits at the monitor's own origin" \
	"$((cx2 - m2x))" "0"

# Monitor-relative again: 600 logical INTO the second output is layout 2136.
#
# 600 AND NOT 900, AND THE REASON MATTERS. The X screen is the whole LOGICAL
# desktop, 3456 wide; this window sits at X 2304 (1536 logical x 1.5) and is
# 2880 wide, so it runs off the right of the X screen at local x = 1152. X11
# clamps the pointer to the root window, so a probe past that point reports the
# screen edge no matter what the compositor sent -- 900 logical did exactly
# that on the first run of this file, reporting 1151 where 1350 was expected,
# and it reads as a broken input transform rather than as what it is. See the
# 1.25-screen-clamp arm in contrib/xw-scale-test.sh, which asserts that
# limitation deliberately. 600 logical is 900 raw, comfortably inside.
hl_move $((1536 + 600)) 400; sleep 0.3
hl_click $((1536 + 600)) 400; sleep 0.5
btn2="$(hl_x11check_last_button x11)"
hl_assert "mon2: click x -> 900 at scale 1.5 (got '${btn2%% *}')" \
	"$(near "${btn2%% *}" 900 3)" "yes"
hl_assert "mon2: click y -> 600 at scale 1.5 (got '${btn2##* }')" \
	"$(near "${btn2##* }" 600 3)" "yes"

hl_move 20 20; sleep 0.5
hl_screenshot_output HEADLESS-2 cap-mon2
hl_assert "mon2: premise -- no wallpaper inside the sampled region" \
	"$(python3 "$CHECKER" count "$HL_OUTDIR/cap-mon2.png" 600 400 1600 1000 "$WALLPAPER_HEX")" "0"
v2="$(python3 "$CHECKER" verdict "$HL_OUTDIR/cap-mon2.png" 600 400 1600 1000 2>&1)"
echo "  mon2 checker: $v2"
hl_assert "mon2: presented 1:1 at the OTHER scale" "${v2%% *}" "native"

hl_summary
