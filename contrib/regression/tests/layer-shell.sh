# layer-shell.sh — layer-shell edge cases via contrib/wllayer (a minimal
# configurable zwlr_layer_shell_v1 client -- kitty/wlvptr/wlvkbd can't reach
# any of this, none of them are layer-shell surfaces).
#
# Covers: exclusive-zone reservation shrinking the tiled area, multiple
# surfaces stacking across layers without breaking normal tiling, keyboard-
# interactivity modes not wedging the compositor, the DPMS layer-configure
# regression (arrangelayers() used to skip disabled/asleep outputs --
# e3ffe94), and the original need_output_flush shadow regression
# (resize-in-place leaving a stale shadow rect -- 221ff0e), pinned directly
# via a pixel sample instead of only through the waybar popup path.

hl_client_field() { hl_get "get all-clients" | jq -r ".clients[] | select(.title==\"$1\") | .$2"; }

test_exclusive_zone_reserves_space_from_tiling() {
	# top layer, anchored top+left+right, 80px tall, reserving that much
	# space (exclusive_zone == height, the common "bar" pattern)
	hl_spawn_wllayer top top,left,right 80 1920 80 none 4 "" bar >/dev/null
	sleep 1
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_assert_true "a tiled window starts below an 80px exclusive-zone bar" \
		"$([ "$(hl_client_field W1 y)" -ge 80 ] && echo true || echo false)"
}

test_background_layer_with_no_exclusive_zone_does_not_affect_tiling() {
	# compare against the SAME layout's baseline geometry rather than a
	# hardcoded y -- the shared test config's titlebar+gaps already inset a
	# lone tiled window well above 0, so "y==0" was never the right
	# baseline to assert (geometry.sh follows the same before/after pattern
	# for exactly this reason).
	hl_spawn_kitty Baseline >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local baseline_y; baseline_y="$(hl_client_field Baseline y)"
	hl_dispatch "kill_client,force"
	hl_wait_client_count 0

	hl_spawn_wllayer background none 0 1920 1080 none 4 "" bg >/dev/null
	sleep 1
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_assert_eq "a background layer with no exclusive zone doesn't change tiled geometry" \
		"$(hl_client_field W1 y)" "$baseline_y"
}

test_keyboard_interactivity_modes_do_not_wedge_the_compositor() {
	for kb in none on_demand exclusive; do
		local arg="$kb"
		[ "$kb" = "on_demand" ] && arg="ondemand"
		hl_spawn_wllayer overlay none 0 200 100 "$arg" 2 "" "kb-$kb" >/dev/null
		sleep 0.5
		hl_assert_true "compositor still responds to IPC with kb_interactivity=$kb" \
			"$([ -n "$(hl_get "get all-monitors")" ] && echo true || echo false)"
		sleep 2
	done
}

test_layer_surface_on_a_dpms_off_monitor_still_gets_configured() {
	# regression pin for e3ffe94: arrangelayers() used to skip disabled/
	# asleep outputs entirely, so a layer client got no configure event at
	# all and hung forever (GTK fell back to a bogus width). A bounded
	# timeout distinguishes "configure arrived" from "hung" -- an unbounded
	# wait would just block forever if the bug were reintroduced.
	hl_dispatch "dpms_off_monitor,$HL_MON"
	sleep 0.3
	timeout 4 "$HL_WLLAYER" top none 0 300 100 none 1 > "$HL_OUTDIR/dpms-layer.log" 2>&1
	local rc=$?
	hl_dispatch "dpms_on_monitor,$HL_MON"
	sleep 0.3
	hl_assert_true "layer surface on a DPMS'd-off monitor gets a configure event before timeout" \
		"$([ "$rc" -ne 124 ] && grep -q "wllayer: configure" "$HL_OUTDIR/dpms-layer.log" && echo true || echo false)"
}

test_resize_in_place_does_not_leave_a_stale_shadow() {
	# regression pin for 221ff0e: need_output_flush only got re-armed post-
	# map inside the layer_animations-gated branch, so with layer_animations
	# off (default), a content-fit surface resizing itself in place (no
	# animation involved at all) never redrew -- the shadow stayed frozen
	# at the surface's ORIGINAL (larger) extent. Sample a pixel that's
	# within the original 220px-tall footprint's shadow but well outside the
	# shrunk 60px-tall one: it should read back as plain background
	# (#3a5a7a, hl_start's flat wallpaper), not shadow-darkened.
	hl_dispatch "view,9"  # a tag with no other windows, keeps the frame clean
	hl_spawn_wllayer top top,left 0 300 220 none 6 "2,300,60" shrink >/dev/null
	sleep 2.5   # past the 2s resize_after, let the shrink + a settle frame land
	hl_screenshot shrink-after
	local rgb
	rgb="$(convert "$HL_OUTDIR/shrink-after.png" -crop 1x1+150+150 -format '%[pixel:p{0,0}]' info: 2>/dev/null)"
	# accept srgb(...)/srgba(...)/#hex forms across ImageMagick versions;
	# just check it's clearly background-toned, not shadow-darkened. The
	# background's own R channel is 0x3a=58; a lingering ~38%-opacity black
	# shadow over it would read well under half that.
	local r
	r="$(echo "$rgb" | grep -oP '(?<=srgba?\()[0-9]+' | head -1)"
	[ -z "$r" ] && r="$(echo "$rgb" | grep -oP '(?<=#)[0-9A-Fa-f]{2}' | head -1 | xargs -I{} printf '%d' 0x{})"
	hl_assert_true "no stale shadow below the shrunk surface (sampled R=$r, background R=58)" \
		"$([ -n "$r" ] && [ "$r" -ge 40 ] && echo true || echo false)"
}
