#!/usr/bin/env bash
# live-visual-tour.sh — a ONE-OFF, manually-driven visual tour of layouts/
# animations/layer-shell/shadows/blur against the CALLER's own live session,
# for recording + visual artifact inspection. Not part of the assertion-based
# regression suite (contrib/regression/) -- this makes no pass/fail claims,
# it just sequences real dispatches with deliberate dwell time so a screen
# recording actually shows each thing clearly.
#
# Safety notes (see docs/regression-testing.md "Live-session mode"):
#   - Requires HL_LIVE_MON=<real output name> and a valid
#     ASTEROIDZ_INSTANCE_SIGNATURE already in the environment.
#   - hl_dispatch (contrib/lib/headless.sh) already refuses to disable
#     animations in live mode -- this script never even tries.
#   - This script DOES intentionally change the target monitor's layout for
#     demonstration purposes (unlike hl_reset, which no longer touches real
#     monitor layout at all) -- it restores the original layout/view at the
#     end, best-effort.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# hl_click/hl_super_drag normalize against HL_WIDTH/HL_HEIGHT (default
# 1920x1080) -- must match the REAL target monitor's resolution or clicks
# land at the wrong absolute position. Override before sourcing the lib.
HL_WIDTH="${HL_WIDTH:-3840}"
HL_HEIGHT="${HL_HEIGHT:-2160}"
. "$REPO/contrib/lib/headless.sh"

: "${HL_LIVE_MON:?set HL_LIVE_MON=<real output name>}"
HL_OUTDIR="${HL_OUTDIR:-/tmp/asteroidz-tour-$$}"
mkdir -p "$HL_OUTDIR"
hl_start_live

# hl_shot LABEL -- a single screenshot_ui,rawhdr call (real 10-bit capture,
# see contrib/hdr-record.sh). NOT a poll loop: screenshot_ui freezes the
# target monitor and does a full frame readback per call -- looping this at
# ~1/sec previously made the whole real system nearly unresponsive
# (2026-07-19). One deliberate shot per transition is both cheaper and more
# useful (captures the state that actually matters instead of a random
# moment). Frames land in /tmp/asteroidz-rawhdr/frame_NNNNNN.raw in call
# order; assembled into one HDR10 video at the end of this script.
RAWDIR=/tmp/asteroidz-rawhdr
rm -rf "$RAWDIR"
mkdir -p "$RAWDIR"
hl_shot() {
	echo "  [hdr capture: $1]"
	env ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg dispatch screenshot_ui,rawhdr >/dev/null 2>&1
}

echo "=== capturing original state to restore at the end ==="
ORIG_ACTIVE_TAG="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .active_tags[0]")"
ORIG_TAG1_LAYOUT="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .tags[] | select(.index==1) | .layout")"
echo "original active tag: $ORIG_ACTIVE_TAG, tag1 layout: $ORIG_TAG1_LAYOUT"

echo "=== switching to tag 1 for the tour ==="
hl_dispatch "view,1" 1

echo "=== spawning 4 distinctly-colored windows ==="
kitty --title W1 -o background_opacity=1.0 -o background=#aa2222 sh -c "echo RED; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 1
kitty --title W2 -o background_opacity=1.0 -o background=#22aa22 sh -c "echo GREEN; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 1
kitty --title W3 -o background_opacity=1.0 -o background=#2222aa sh -c "echo BLUE; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 1
kitty --title W4 -o background_opacity=1.0 -o background=#aaaa22 sh -c "echo YELLOW; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
# NOT hl_wait_client_count here -- it checks the GLOBAL client count, which
# already includes your other real windows on the real monitor, so it would
# just spin to its own timeout uselessly. A fixed settle time instead --
# kitty has real startup latency; 2s here previously wasn't quite enough for
# all 4 to finish mapping before the first layout capture (2026-07-19: only
# 1 of 4 windows was visible in that capture, resolved itself a few captures
# later once startup caught up). 4s gives real margin.
sleep 4
hl_shot "4 windows spawned, before any layout forced"

echo "=== tile ==="
hl_dispatch "set_layout,tile" 3
hl_shot "tile"

echo "=== monocle ==="
hl_dispatch "set_layout,monocle" 3
hl_shot "monocle"

echo "=== scroller ==="
hl_dispatch "set_layout,scroller" 3
# NOTE (2026-07-19): earlier captures here showed zero visible panning no
# matter how many focus_direction dispatches ran. Root cause was NOT a
# compositor bug or a scroller_focus_center config nuance -- this script
# was dispatching the wrong IPC function name ("focusdir", the internal C
# function, instead of the actual dispatch name "focus_direction"), which
# amsg silently rejected with {"error":"unknown function"} every time (see
# hl_dispatch's >/dev/null 2>&1). A safe, isolated headless test with the
# correct name confirmed focus_direction works fine. Kept the narrower
# columns + extra windows here anyway since genuine overflow is still the
# clearest way to show panning on camera.
ORIG_PROPORTION="$(hl_get "get all-clients" | jq -r '[.clients[].scroller_proportion] | map(select(. != null)) | .[0] // empty')"
hl_dispatch "set_proportion,0.5" 1
kitty --title W7 -o background_opacity=1.0 -o background=#dd7700 sh -c "echo ORANGE; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 1
kitty --title W8 -o background_opacity=1.0 -o background=#00bbbb sh -c "echo TEAL; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 1
kitty --title W9 -o background_opacity=1.0 -o background=#bb00bb sh -c "echo PINK; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 3
hl_shot "scroller, 7 columns at 0.5 proportion -- more than fit on screen"
echo "--- panning right across columns (should now visibly shift) ---"
hl_dispatch "focus_direction,right" 1
hl_dispatch "focus_direction,right" 1
hl_dispatch "focus_direction,right" 1
hl_shot "scroller panned right"
echo "--- panning further right, off the initial screen entirely ---"
hl_dispatch "focus_direction,right" 1
hl_dispatch "focus_direction,right" 1
hl_shot "scroller panned further right"
echo "--- panning back left to the start ---"
hl_dispatch "focus_direction,left" 1
hl_dispatch "focus_direction,left" 1
hl_dispatch "focus_direction,left" 1
hl_dispatch "focus_direction,left" 1
hl_dispatch "focus_direction,left" 1
hl_shot "scroller panned back to start"
hl_dispatch "scroller_stack,left" 2
hl_shot "scroller after stack"
hl_dispatch "scroller_expel" 2
hl_shot "scroller after expel"
[ -n "$ORIG_PROPORTION" ] && hl_dispatch "set_proportion,$ORIG_PROPORTION" 1

echo "=== float ==="
hl_dispatch "set_layout,float" 3
hl_shot "float"

echo "=== back to tile, dwindle split demo (may no-op if dwindle_manual_split isn't on in your config) ==="
hl_dispatch "set_layout,tile" 2
hl_dispatch "dwindle_split_horizontal" 1
kitty --title W5 -o background_opacity=1.0 -o background=#aa22aa sh -c "echo MAGENTA; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 3
hl_shot "tile + dwindle_split_horizontal + new window"

echo "=== animations survey: open, close, minimize/restore, fullscreen, tag-switch ==="
echo "--- open ---"
kitty --title W6 -o background_opacity=1.0 -o background=#22aaaa sh -c "echo CYAN; exec sleep 300" >/dev/null 2>&1 &
NEWPID="$!"
HL_SPAWNED_PIDS+=("$NEWPID")
sleep 0.7
hl_shot "W6 mid-open (~0.7s after spawn, animation likely still in progress)"
sleep 1.5
hl_shot "W6 open, settled"

echo "--- minimize / restore ---"
hl_dispatch "minimize"
sleep 0.15
hl_shot "just dispatched minimize, mid-animation"
sleep 0.6
hl_shot "minimized, settled"
hl_dispatch "restore_minimized"
sleep 0.15
hl_shot "just dispatched restore_minimized, mid-animation"
sleep 0.6
hl_shot "restored, settled"

echo "--- fullscreen toggle ---"
hl_dispatch "toggle_fullscreen"
sleep 0.15
hl_shot "just toggled fullscreen on, mid-animation"
sleep 0.6
hl_shot "fullscreen, settled"
hl_dispatch "toggle_fullscreen"
sleep 0.15
hl_shot "just toggled fullscreen off, mid-animation"
sleep 0.6
hl_shot "fullscreen off, settled"

echo "--- tag-switch (to tag 2, which has real pre-existing content, and back) ---"
hl_dispatch "view,2"
sleep 0.15
hl_shot "just switched to tag 2, mid-transition"
sleep 0.6
hl_shot "tag 2, settled"
hl_dispatch "view,1"
sleep 0.15
hl_shot "just switched back to tag 1, mid-transition"
sleep 0.6
hl_shot "tag 1, settled"

echo "--- close ---"
NEWID="$(hl_get "get all-clients" | jq -r '.clients[] | select(.title=="W6") | .id')"
[ -n "$NEWID" ] && hl_dispatch "kill_client,force"
sleep 0.15
hl_shot "W6 mid-close, animation likely still in progress"
sleep 0.6
hl_shot "W6 closed, settled"

echo "=== layer-shell: exclusive-zone bar + overlay popup ==="
hl_spawn_wllayer top "top,left,right" 100 3840 100 none 6 "" bar >/dev/null
sleep 2
hl_shot "layer-shell exclusive-zone bar"
hl_spawn_wllayer overlay none 0 600 400 none 4 "" overlay >/dev/null
sleep 3
hl_shot "layer-shell overlay popup"

echo "=== overview toggle ==="
hl_dispatch "toggle_overview" 3
hl_shot "overview open"
hl_dispatch "toggle_overview" 2
hl_shot "overview closed"

echo "=== waybar popup: display module (coords found via WBTEST_DUMP_GEOM against the real bar) ==="
# zwlr_virtual_pointer_v1's motion_absolute (what hl_click ultimately calls)
# is normalized against the compositor's REAL global pointer space --
# spanning ALL outputs combined -- not any single monitor's own resolution.
# Passing DP-1's own 3840x2160 here previously landed a click meant for
# DP-1 onto HDMI-A-1 instead (sitting to its right at x=3840), which
# silently shifted focus there for the rest of the tour, since dispatches
# with no explicit monitor target always act on whatever's currently
# selmon -- confirmed live 2026-07-19 (waybar popup + the whole window-
# interaction section below captured HDMI-A-1's empty desktop instead).
ALL_MON="$(hl_get "get all-monitors")"
HL_WIDTH="$(jq -r '[.monitors[] | .x + .width] | max' <<<"$ALL_MON")"
HL_HEIGHT="$(jq -r '[.monitors[] | .y + .height] | max' <<<"$ALL_MON")"
echo "real pointer-space extent for clicks: ${HL_WIDTH}x${HL_HEIGHT}"

# HL_WAYBAR_DISPLAY_X/Y default to the display module's pill center on DP-1
# as found 2026-07-19 (x=3636,y=9,w=53,h=48 -> center 3662,33). Override via
# env if your bar layout differs or this is a different monitor.
WB_X="${HL_WAYBAR_DISPLAY_X:-3662}"
WB_Y="${HL_WAYBAR_DISPLAY_Y:-33}"
hl_click "$WB_X" "$WB_Y" click
sleep 2
hl_shot "waybar display popup open"
# NOT a second click on the same anchor -- GTK popovers close on an
# outside click or Escape, not a second click on the button that opened
# them; clicking the anchor again just re-triggers it (confirmed live
# 2026-07-19: the popup was still open several captures later). Escape is
# the reliable way to dismiss it.
"$HL_WLVKBD" press ESC
sleep 1
hl_shot "waybar display popup closed"

# Defense in depth regardless of whether the click above landed correctly:
# every dispatch below acts on whatever's currently selmon, so force it
# back to the intended target monitor before touching any windows.
hl_dispatch "focus_monitor,$HL_MON" 1

echo "=== window interaction: float one window + move/resize/exchange via IPC dispatch ==="
# NOT hl_super_drag: no Super+drag mousebind exists in this compositor's
# shipped default config or this user's own config.kdl -- that binding is
# hl_start's own synthetic test config, would be a silent no-op live.
# move_window/resize_window are relative when given a signed +/- value.
# toggle_floating alone (not set_layout,float) -- floats just the focused
# window within whatever tiled layout is currently active, matching how
# this is actually used day to day.
hl_dispatch "toggle_floating" 1
hl_shot "floating, before move"
# A newly-floated window inherits its last TILED geometry, which can be
# most of the screen -- a small +/- shift on that is easy to miss. Shrink
# it down to something clearly bounded first, THEN move it a large,
# unmistakable distance, so the motion actually reads on a 3840x2160
# screen instead of being a subtle nudge on an already-huge window.
# NOTE (2026-07-19): the original deltas here (-1400,-1200) over-shrunk a
# tiled window whose height was only ~1032 -- the resulting negative
# height got clamped to a ~5px sliver, which is why the "move" that
# followed looked like nothing happened (a near-invisible line, not a
# window). Confirmed headlessly by reading back get-all-clients geometry
# after each dispatch. These deltas are sized relative to a typical tiled
# half-screen window instead of guessed.
hl_dispatch "resize_window,-900,-500" 2
hl_shot "shrunk down to a clearly-bounded floating window"
hl_dispatch "move_window,+700,+500" 2
hl_shot "after a large move_window,+700,+500 (should be obviously in a new spot)"
hl_dispatch "resize_window,+300,+200" 2
hl_shot "after resize_window,+300,+200 (shadow/blur should track the new size, no stale rect)"
hl_dispatch "move_window,-700,-500" 2
hl_shot "after move back"
hl_dispatch "exchange_client,left" 2
hl_shot "after exchange_client,left"
hl_dispatch "toggle_floating" 1

echo "=== restoring original state ==="
case "$ORIG_TAG1_LAYOUT" in
	T) orig_layout_name=tile ;;
	M) orig_layout_name=monocle ;;
	S) orig_layout_name=scroller ;;
	F) orig_layout_name=float ;;
	*) orig_layout_name= ;;
esac
[ -n "$orig_layout_name" ] && hl_dispatch "set_layout,$orig_layout_name" 1
[ -n "$ORIG_ACTIVE_TAG" ] && hl_dispatch "view,$ORIG_ACTIVE_TAG" 1

hl_stop

echo "=== assembling captured frames into one HDR10 video ==="
COUNT="$(ls "$RAWDIR"/frame_*.raw 2>/dev/null | wc -l)"
if [ "$COUNT" -eq 0 ]; then
	echo "no frames captured, skipping assembly"
else
	MON_JSON="$(env ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg get all-monitors)"
	MON="$(jq -c ".monitors[] | select(.name==\"$HL_MON\")" <<<"$MON_JSON")"
	W="$(jq -r '.mode_width' <<<"$MON")"
	H="$(jq -r '.mode_height' <<<"$MON")"
	MAX_LUM="$(jq -r '.hdr_max_luminance' <<<"$MON")"
	MIN_LUM="$(jq -r '.hdr_min_luminance' <<<"$MON")"
	MAX_FALL="$(jq -r '.hdr_max_fall' <<<"$MON")"
	# master-display L() wants 0.0001 cd/m^2 units; max-cll wants PLAIN cd/m^2
	# (16-bit field -- the x10000-scaled value overflows mod 65536 and
	# silently corrupts it). colorprim/transfer/colormatrix inside
	# -x265-params is what actually lands in the bitstream VUI -- ffmpeg's
	# own -color_primaries/-color_trc flags didn't propagate there on their
	# own (confirmed: color_space took effect, the other two stayed
	# "unknown" without this, 2026-07-19).
	MAX_LUM_UNITS="$(awk -v v="$MAX_LUM" 'BEGIN{printf "%d", v*10000}')"
	MIN_LUM_UNITS="$(awk -v v="$MIN_LUM" 'BEGIN{printf "%d", v*10000}')"
	OUTFILE="${HL_OUTDIR}/tour-hdr.mp4"

	cat "$RAWDIR"/frame_*.raw > "$RAWDIR/combined.raw"
	ffmpeg -y -f rawvideo -pixel_format x2bgr10le -video_size "${W}x${H}" -framerate 1 \
		-i "$RAWDIR/combined.raw" \
		-c:v libx265 -preset medium -crf 18 -pix_fmt yuv420p10le \
		-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
		-x265-params "hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(${MAX_LUM_UNITS},${MIN_LUM_UNITS}):max-cll=${MAX_LUM},${MAX_FALL}" \
		"$OUTFILE" 2>"$HL_OUTDIR/ffmpeg.log"
	echo "$COUNT frames -> $OUTFILE"
fi
echo "=== tour finished ==="
