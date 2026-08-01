# fullscreen-bleed.sh — leaving fullscreen must not leave the window drawn on
# the monitor next door.
#
# Reported live 2026-08-01: a window was fullscreened and then unfullscreened,
# and its contents kept being painted across the boundary onto the adjacent
# output. Fullscreening it again cleaned it up, so the state was persistent
# rather than a one-frame race.
#
# Nothing about it is visible over IPC -- the geometry can be perfectly correct
# while the SURFACE is drawn past it -- so these look at pixels: what the
# neighbour output shows before anything happens, and what it shows afterwards.
# Test windows are opaque solid colours (hl_spawn_kitty) against a flat grey
# wallpaper, which makes "something that is not the wallpaper" a decidable
# question.
#
# SKIPS on one output, like multimonitor.sh -- with no neighbour there is
# nothing to bleed onto and every assertion here would pass by default.
#
# harness: needs-second-monitor -- run.sh sorts this after every
# single-monitor module; the output it creates outlives it.

_fb_second_mon=""
_fb_setup() {
	[ -n "$_fb_second_mon" ] && return 0
	if [ "$(hl_monitor_count)" -lt 2 ]; then
		hl_dispatch "create_virtual_output" 1
	fi
	_fb_second_mon="$(hl_get "get all-monitors" | jq -r --arg m "$HL_MON" \
		'[.monitors[] | select(.name != $m)][0].name // empty')"
	[ -n "$_fb_second_mon" ] || return 1
	hl_sync_pointer_extent
	return 0
}

# Pixels on the neighbour that differ from the reference shot of it, counted
# coarsely. A window bleeding across is thousands; encoding noise is single
# digits.
_fb_changed_px() { # _fb_changed_px REF PNG
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
if a.size != b.size:
    print(-1); raise SystemExit
pa, pb = a.load(), b.load()
w, h = a.size
n = 0
for y in range(0, h, 4):
    for x in range(0, w, 4):
        r1, g1, b1 = pa[x, y]
        r2, g2, b2 = pb[x, y]
        if abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2) > 40:
            n += 1
print(n)
PY
}

_fb_shot() { grim -o "$_fb_second_mon" "$HL_OUTDIR/$1.png" 2>/dev/null; }

# One window on HL_MON, fullscreened and released, with the neighbour
# photographed before and after. $1 names the layout to do it in.
_fb_run_layout() { # _fb_run_layout LAYOUT
	local layout="$1" changed

	hl_dispatch "focus_monitor,$HL_MON" 0.3
	hl_dispatch "set_layout,$layout" 0.5
	# Three, not one. A single window fits its monitor whatever happens to
	# it; the report is about a layout whose columns already run past the
	# screen edge, and in the scroller that needs more than one window to
	# exist at all.
	hl_spawn_kitty "FB-$layout-1" >/dev/null
	hl_spawn_kitty "FB-$layout-2" >/dev/null
	hl_spawn_kitty "FB-$layout-3" >/dev/null
	hl_wait_client_count 3
	sleep 0.7

	# The reference: the neighbour with the window on the OTHER monitor,
	# windowed. Everything below is measured against this, so a wallpaper or
	# a bar that happens to be there is not counted as bleed.
	_fb_shot "fb-$layout-before"

	hl_dispatch "toggle_fullscreen" 1
	hl_dispatch "toggle_fullscreen" 1
	sleep 0.7
	_fb_shot "fb-$layout-after"

	changed="$(_fb_changed_px "$HL_OUTDIR/fb-$layout-before.png" \
		"$HL_OUTDIR/fb-$layout-after.png")"
	echo "$changed"
}

test_leaving_fullscreen_does_not_bleed_onto_the_next_monitor_tiled() {
	if ! _fb_setup; then
		hl_skip "test_leaving_fullscreen_does_not_bleed_onto_the_next_monitor_tiled: no second monitor"
		return
	fi
	local changed; changed="$(_fb_run_layout tile)"
	hl_assert "tile: the neighbour is unchanged after a fullscreen round trip" \
		"$([ "${changed:-0}" -ge 0 ] && [ "${changed:-0}" -lt 50 ] && echo clean || echo "dirty ($changed px)")" \
		"clean"
}

test_leaving_fullscreen_does_not_bleed_onto_the_next_monitor_scroller() {
	if ! _fb_setup; then
		hl_skip "test_leaving_fullscreen_does_not_bleed_onto_the_next_monitor_scroller: no second monitor"
		return
	fi
	# The scroller is the layout that positions windows past the screen edge
	# on purpose, so it is the one where a missing clip shows up first -- and
	# it is what the report came from.
	local changed; changed="$(_fb_run_layout scroller)"
	hl_assert "scroller: the neighbour is unchanged after a fullscreen round trip" \
		"$([ "${changed:-0}" -ge 0 ] && [ "${changed:-0}" -lt 50 ] && echo clean || echo "dirty ($changed px)")" \
		"clean"
}

test_a_windows_geometry_stays_on_its_own_monitor_after_fullscreen() {
	if ! _fb_setup; then
		hl_skip "test_a_windows_geometry_stays_on_its_own_monitor_after_fullscreen: no second monitor"
		return
	fi
	# The cheap half of the same question, and the one that says WHICH is
	# wrong when both fail: geometry, or only what is drawn from it.
	hl_dispatch "focus_monitor,$HL_MON" 0.3
	hl_dispatch "set_layout,scroller" 0.5
	hl_spawn_kitty FB-GEOM-1 >/dev/null
	hl_spawn_kitty FB-GEOM-2 >/dev/null
	hl_spawn_kitty FB-GEOM >/dev/null
	hl_wait_client_count 3
	hl_dispatch "toggle_fullscreen" 1
	hl_dispatch "toggle_fullscreen" 1
	sleep 0.5

	local mx mw x w
	mx="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .x")"
	mw="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .width")"
	x="$(hl_client_field FB-GEOM x)"
	w="$(hl_client_field FB-GEOM width)"
	hl_assert "the window's right edge is still on its own monitor" \
		"$(python3 -c "print('yes' if $x + $w <= $mx + $mw else 'no (right edge $(( x + w )), monitor ends $(( mx + mw )))')")" \
		"yes"
}

test_a_floating_window_does_not_bleed_after_fullscreen() {
	if ! _fb_setup; then
		hl_skip "test_a_floating_window_does_not_bleed_after_fullscreen: no second monitor"
		return
	fi
	# Floating is the case with no layout to put it back: nothing re-arranges
	# it on the way out of fullscreen, so whatever geometry it lands on is
	# what gets drawn -- and a floating window is not clipped to its monitor
	# the way a scroller column is.
	hl_dispatch "focus_monitor,$HL_MON" 0.3
	hl_dispatch "set_layout,tile" 0.5
	hl_spawn_kitty FB-FLOAT >/dev/null
	hl_wait_client_count 1
	hl_dispatch "toggle_floating" 0.7
	sleep 0.5
	_fb_shot "fb-float-before"

	hl_dispatch "toggle_fullscreen" 1
	hl_dispatch "toggle_fullscreen" 1
	sleep 0.7
	_fb_shot "fb-float-after"

	local changed
	changed="$(_fb_changed_px "$HL_OUTDIR/fb-float-before.png" \
		"$HL_OUTDIR/fb-float-after.png")"
	hl_assert "floating: the neighbour is unchanged after a fullscreen round trip" \
		"$([ "${changed:-0}" -ge 0 ] && [ "${changed:-0}" -lt 50 ] && echo clean || echo "dirty ($changed px)")" \
		"clean"
}

# The reported sequence, exactly: a window opened FULLSCREEN BY RULE onto a
# scroller tag, another window opened beside it, then a tag switch.
#
# What makes this one different from the round trips above is that the window
# never leaves fullscreen as far as its own state is concerned.
# clear_fullscreen_flag() returns early for a non-floating client on a SCROLLER
# tag (asteroidz.c), so the new window makes the scroller re-arrange it while
# `isfullscreen` stays set and its border stays zeroed -- a monitor-sized
# surface being laid out as a column. The tag switch then animates it off the
# edge, which is where it showed up on the neighbouring output.
_fb_rule_done=""
_fb_rule() {
	[ -z "$_fb_rule_done" ] || return 0
	cat >> "$HL_CONFIG" <<'EOF'
window-rule {
	match title="FB-FS"
	open-fullscreen #true
	no-animation #true
}
EOF
	hl_dispatch "reload_config" 1
	_fb_rule_done=1
}

test_a_rule_fullscreened_window_does_not_bleed_when_the_tag_changes() {
	if ! _fb_setup; then
		hl_skip "test_a_rule_fullscreened_window_does_not_bleed_when_the_tag_changes: no second monitor"
		return
	fi
	_fb_rule

	hl_dispatch "focus_monitor,$HL_MON" 0.3
	# A TILE tag, which is what the report came from -- and the detail that
	# decides the whole thing. clear_fullscreen_flag() returns early for a
	# non-floating client on a scroller tag, so there the window never leaves
	# fullscreen and set_arrange_hidden's fullscreen guard hides it outright.
	# On a tile tag it really is unfullscreened, so the tag switch slides it.
	hl_dispatch "view,3" 0.5
	sleep 0.5
	grim -o "$_fb_second_mon" "$HL_OUTDIR/fb-rule-before.png" 2>/dev/null

	hl_spawn_kitty FB-FS >/dev/null
	hl_wait_client_count 1
	sleep 1
	hl_assert_true "the rule really did fullscreen it" \
		"$(hl_client_field FB-FS is_fullscreen)"

	hl_spawn_kitty FB-SECOND >/dev/null
	hl_wait_client_count 2
	sleep 1

	hl_dispatch "view,1" 1.5
	sleep 1.5
	grim -o "$_fb_second_mon" "$HL_OUTDIR/fb-rule-after.png" 2>/dev/null

	local changed
	changed="$(_fb_changed_px "$HL_OUTDIR/fb-rule-before.png" \
		"$HL_OUTDIR/fb-rule-after.png")"
	hl_assert "the neighbour is unchanged after the tag switch" \
		"$([ "${changed:-0}" -ge 0 ] && [ "${changed:-0}" -lt 50 ] && echo clean || echo "dirty ($changed px)")" \
		"clean"

	hl_dispatch "view,1" 0.3
}
