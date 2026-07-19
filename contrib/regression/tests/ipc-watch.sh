# ipc-watch.sh — amsg's persistent "watch" streams (IPC_WATCH_* in ipc.h).
#
# Each watch type gets: subscribe (hl_watch_start backgrounds `amsg watch
# ...`, gives the subscribe a moment to land), record the line count, do
# something that SHOULD produce a notification, then assert the stream grew.
# This is a "did a notification fire at all" check, not "was its content
# correct" (the get-based modules already cover the content of every one of
# these views).

test_watch_all_clients_notifies_on_a_new_window() {
	hl_watch_start "watch all-clients" wac >/dev/null
	local before; before="$(hl_watch_line_count wac)"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_assert_true "watch all-clients grows when a window maps" \
		"$([ "$(hl_watch_line_count wac)" -gt "$before" ] && echo true || echo false)"
}

test_watch_all_monitors_notifies_on_hdr_toggle() {
	hl_watch_start "watch all-monitors" wam >/dev/null
	local before; before="$(hl_watch_line_count wam)"
	hl_dispatch "toggle_hdr"
	hl_assert_true "watch all-monitors grows on toggle_hdr" \
		"$([ "$(hl_watch_line_count wam)" -gt "$before" ] && echo true || echo false)"
}

test_watch_monitor_notifies_for_the_named_output() {
	hl_watch_start "watch monitor $HL_MON" wm >/dev/null
	local before; before="$(hl_watch_line_count wm)"
	hl_dispatch "toggle_hdr"
	hl_assert_true "watch monitor \$HL_MON grows on toggle_hdr" \
		"$([ "$(hl_watch_line_count wm)" -gt "$before" ] && echo true || echo false)"
}

test_watch_all_tags_notifies_on_view_switch() {
	hl_watch_start "watch all-tags" wat >/dev/null
	local before; before="$(hl_watch_line_count wat)"
	hl_dispatch "view,2"
	hl_assert_true "watch all-tags grows on view,2" \
		"$([ "$(hl_watch_line_count wat)" -gt "$before" ] && echo true || echo false)"
}

test_watch_tags_notifies_for_the_named_monitor() {
	hl_watch_start "watch tags $HL_MON" wt >/dev/null
	local before; before="$(hl_watch_line_count wt)"
	hl_dispatch "view,2"
	hl_assert_true "watch tags \$HL_MON grows on view,2" \
		"$([ "$(hl_watch_line_count wt)" -gt "$before" ] && echo true || echo false)"
}

test_watch_focused_client_notifies_on_a_new_window() {
	hl_watch_start "watch focused-client" wfc >/dev/null
	local before; before="$(hl_watch_line_count wfc)"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_assert_true "watch focused-client grows when a new window takes focus" \
		"$([ "$(hl_watch_line_count wfc)" -gt "$before" ] && echo true || echo false)"
}

test_watch_client_notifies_for_that_id() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local id; id="$(hl_get "get all-clients" | jq -r '.clients[0].id')"
	hl_watch_start "watch client $id" wc >/dev/null
	local before; before="$(hl_watch_line_count wc)"
	hl_dispatch "move_window,+20,+20"
	hl_assert_true "watch client <id> grows when that client moves" \
		"$([ "$(hl_watch_line_count wc)" -gt "$before" ] && echo true || echo false)"
}

test_watch_keymode_notifies_on_set_key_mode() {
	hl_watch_start "watch keymode" wkm >/dev/null
	local before; before="$(hl_watch_line_count wkm)"
	hl_dispatch "set_key_mode,resize"
	hl_assert_true "watch keymode grows on set_key_mode" \
		"$([ "$(hl_watch_line_count wkm)" -gt "$before" ] && echo true || echo false)"
	hl_dispatch "set_key_mode,default"  # restore
}

test_watch_keyboardlayout_notifies_on_switch() {
	hl_watch_start "watch keyboardlayout" wkl >/dev/null
	local before; before="$(hl_watch_line_count wkl)"
	hl_dispatch "switch_keyboard_layout,1"
	hl_assert_true "watch keyboardlayout grows on switch_keyboard_layout" \
		"$([ "$(hl_watch_line_count wkl)" -gt "$before" ] && echo true || echo false)"
}

test_watch_last_open_surface_notifies_on_a_new_window() {
	hl_watch_start "watch last_open_surface" wlos >/dev/null
	local before; before="$(hl_watch_line_count wlos)"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_assert_true "watch last_open_surface grows when a window opens" \
		"$([ "$(hl_watch_line_count wlos)" -gt "$before" ] && echo true || echo false)"
}
