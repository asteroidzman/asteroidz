# dwindle.sh — dwindle_split_horizontal/vertical/toggle_split_direction.
#
# These only have an effect when config.dwindle_manual_split is enabled
# (default: 0/off) -- without it, the new-window split direction is always
# auto-decided from the target leaf's aspect ratio, silently ignoring
# custom_leaf_split_h. hl_start's own test config turns this on
# (dwindle_manual_split 1) specifically so these dispatches are
# observable; a fresh default config would make every test below a no-op,
# same class of surprise as set_master_factor/adjust_master_count in
# geometry.sh, except this one IS reachable, just opt-in.

hl_two_client_positions() { hl_get "get all-clients" | jq -c '[.clients[] | {x,y}]'; }

test_dwindle_split_horizontal_places_new_window_beside() {
	hl_skip_if_live_unrestorable_option "test_dwindle_split_horizontal_places_new_window_beside" dwindle_manual_split || return
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "dwindle_split_horizontal"
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2
	sleep 0.3
	local ys; ys="$(hl_get "get all-clients" | jq -c '[.clients[].y] | unique | length')"
	local xs; xs="$(hl_get "get all-clients" | jq -c '[.clients[].x] | unique | length')"
	hl_assert_true "dwindle_split_horizontal: new window lands beside (same y, different x)" \
		"$([ "$ys" = "1" ] && [ "$xs" = "2" ] && echo true || echo false)"
}

test_dwindle_split_vertical_places_new_window_below() {
	hl_skip_if_live_unrestorable_option "test_dwindle_split_vertical_places_new_window_below" dwindle_manual_split || return
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "dwindle_split_vertical"
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2
	sleep 0.3
	local ys; ys="$(hl_get "get all-clients" | jq -c '[.clients[].y] | unique | length')"
	local xs; xs="$(hl_get "get all-clients" | jq -c '[.clients[].x] | unique | length')"
	hl_assert_true "dwindle_split_vertical: new window lands below (same x, different y)" \
		"$([ "$xs" = "1" ] && [ "$ys" = "2" ] && echo true || echo false)"
}

test_dwindle_toggle_split_direction_flips_it() {
	hl_skip_if_live_unrestorable_option "test_dwindle_toggle_split_direction_flips_it" dwindle_manual_split || return
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "dwindle_split_horizontal"
	hl_dispatch "dwindle_toggle_split_direction"
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2
	sleep 0.3
	local xs; xs="$(hl_get "get all-clients" | jq -c '[.clients[].x] | unique | length')"
	hl_assert_eq "dwindle_toggle_split_direction flips horizontal to vertical (same x now)" "$xs" "1"
}
