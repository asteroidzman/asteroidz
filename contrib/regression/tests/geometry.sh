# geometry.sh — gaps, master factor/count, move/resize/center.
#
# move_window/resize_window use a sign-PREFIX convention (see movewin/
# resizewin in dispatch/bind_define.h, parse_num_type): "+30"/"-30" are
# relative deltas, but a plain "30" (no sign) is an ABSOLUTE value -- pass
# the '+' explicitly to test delta behavior, or the assertions silently
# check the wrong thing (a bare "30" SETS x to 30, it doesn't add 30).

# our own spawned W1, not .clients[0] (which in live mode can just as
# easily be a real pre-existing window as the test's own spawned one)
hl_first_client_json() { hl_get "get all-clients" | jq -c '.clients[] | select(.title=="W1") | {x,y,width,height}'; }

test_toggle_gaps_changes_tiled_geometry() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local before; before="$(hl_first_client_json)"
	hl_dispatch "toggle_gaps"
	local after; after="$(hl_first_client_json)"
	hl_assert_true "toggle_gaps changes the tiled window's geometry" \
		"$([ "$before" != "$after" ] && echo true || echo false)"
	hl_dispatch "toggle_gaps"  # restore: enablegaps defaults to 1, don't leak state to later tests
}

test_adjust_gaps_changes_tiled_geometry() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2
	sleep 0.3
	local before; before="$(hl_get "get all-clients" | jq -c '[.clients[].x] | sort')"
	hl_dispatch "adjust_gaps,40"
	local after; after="$(hl_get "get all-clients" | jq -c '[.clients[].x] | sort')"
	hl_assert_true "adjust_gaps,40 changes the tiled windows' x positions" \
		"$([ "$before" != "$after" ] && echo true || echo false)"
	hl_dispatch "adjust_gaps,-40"  # restore
}

# set_master_factor/adjust_master_count are DEAD CODE against every layout
# asteroidz currently registers: they write to selmon->pertag->{mfacts,
# nmasters}[curtag], but grepping dwindle.h/scroll.h/floating.h (the "tile"/
# "scroller"/"float" arrange functions) turns up zero reads of either.
# arrange_special() (the named-scratchpad/special-workspace tiler) DOES
# read nmaster/mfact, but from config.default_nmaster/default_mfact
# (a separate global), never the per-tag values these dispatch functions
# actually write. So: no currently-reachable code path makes these do
# anything visible. These two tests pin that CURRENT (surprising) behavior
# rather than assert a real effect -- if this ever changes (someone wires
# a layout up to read the per-tag values), these will start failing, which
# is the point: a maintainer should notice and update the test, not have it
# silently keep passing either way.
test_set_master_factor_currently_has_no_layout_effect() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null; hl_wait_client_count 1
	hl_spawn_kitty W2 >/dev/null; hl_wait_client_count 2
	sleep 0.3
	local before; before="$(hl_get "get all-clients" | jq -c '[.clients[].width] | sort')"
	hl_dispatch "set_master_factor,0.05"
	local after; after="$(hl_get "get all-clients" | jq -c '[.clients[].width] | sort')"
	hl_assert_eq "set_master_factor is not wired into any current layout (pinning known behavior)" "$after" "$before"
}

test_adjust_master_count_currently_has_no_layout_effect() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null; hl_wait_client_count 1
	hl_spawn_kitty W2 >/dev/null; hl_wait_client_count 2
	hl_spawn_kitty W3 >/dev/null; hl_wait_client_count 3
	sleep 0.3
	local before; before="$(hl_get "get all-clients" | jq -c '[.clients[].width] | sort')"
	hl_dispatch "adjust_master_count,1"
	local after; after="$(hl_get "get all-clients" | jq -c '[.clients[].width] | sort')"
	hl_assert_eq "adjust_master_count is not wired into any current layout (pinning known behavior)" "$after" "$before"
}

test_move_window_shifts_position() {
	hl_dispatch "set_layout,float"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local before_x; before_x="$(hl_client_field W1 x)"
	local want_x=$((before_x + 30))
	hl_dispatch "move_window,+30,+30"
	# poll for the target instead of a fixed sleep -- live mode animates the
	# move over ~200-300ms, so reading too early sees the pre-move position
	hl_wait_field_eq W1 x "$want_x" 20
	local after_x; after_x="$(hl_client_field W1 x)"
	hl_assert_eq "move_window,+30,+30 shifts x by +30 (relative -- bare '30' with no sign SETS it instead)" \
		"$after_x" "$want_x"
}

test_resize_window_changes_size() {
	hl_dispatch "set_layout,float"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local before_w; before_w="$(hl_client_field W1 width)"
	local want_w=$((before_w + 50))
	hl_dispatch "resize_window,+50,+0"
	hl_wait_field_eq W1 width "$want_w" 20
	local after_w; after_w="$(hl_client_field W1 width)"
	hl_assert_eq "resize_window,+50,+0 grows width by +50 (relative)" "$after_w" "$want_w"
}

test_center_window_centers_a_floating_client() {
	hl_dispatch "set_layout,float"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "move_window,300,300"
	hl_wait_field_eq W1 x 300 20
	hl_dispatch "center_window"
	sleep 0.5  # center_window's target isn't known ahead of time to poll for
	local cx cy w h
	cx="$(hl_client_field W1 x)"; cy="$(hl_client_field W1 y)"
	w="$(hl_client_field W1 width)"; h="$(hl_client_field W1 height)"
	local mid_x=$((cx + w / 2)) mid_y=$((cy + h / 2))
	local out_mid_x=$((HL_WIDTH / 2)) out_mid_y=$((HL_HEIGHT / 2))
	local dx=$((mid_x - out_mid_x)); dx=${dx#-}
	local dy=$((mid_y - out_mid_y)); dy=${dy#-}
	hl_assert_true "center_window puts the window's midpoint near the output's center" \
		"$([ "$dx" -lt 40 ] && [ "$dy" -lt 40 ] && echo true || echo false)"
}
