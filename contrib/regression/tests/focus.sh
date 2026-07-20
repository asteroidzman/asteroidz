# focus.sh — focus_stack, focus_direction, kill_client, exchange_client.

test_focus_stack_next_cycles_focus() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null; hl_wait_client_count 1
	hl_spawn_kitty W2 >/dev/null; hl_wait_client_count 2
	hl_spawn_kitty W3 >/dev/null; hl_wait_client_count 3
	hl_spawn_kitty W4 >/dev/null; hl_wait_client_count 4
	local before; before="$(hl_focused_title)"
	hl_dispatch "focus_stack,next"
	local after; after="$(hl_focused_title)"
	hl_assert_true "focus_stack,next moves focus to a different client" \
		"$([ "$before" != "$after" ] && echo true || echo false)"
}

test_focus_direction_left_right() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null; hl_wait_client_count 1
	hl_spawn_kitty W2 >/dev/null; hl_wait_client_count 2
	hl_spawn_kitty W3 >/dev/null; hl_wait_client_count 3
	hl_spawn_kitty W4 >/dev/null; hl_wait_client_count 4
	sleep 0.3
	hl_dispatch "focus_direction,left"
	local left_focus; left_focus="$(hl_focused_title)"
	hl_dispatch "focus_direction,right"
	local right_focus; right_focus="$(hl_focused_title)"
	hl_assert_true "focus_direction,left then ,right lands on a different client" \
		"$([ "$left_focus" != "$right_focus" ] && echo true || echo false)"
}

test_kill_client_removes_the_window() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	# kill_client (graceful) does send xdg_toplevel.close() correctly --
	# confirmed via WAYLAND_DEBUG=1, kitty destroys the toplevel ~2.2s later
	# -- but the kitty PROCESS this harness spawns never actually exits
	# afterward even given 12+s (its `sh -c "...; exec sleep 300"` child
	# apparently doesn't get a SIGHUP that kills it, likely a process-group
	# artifact of how this harness backgrounds kitty from bash, not a
	# compositor bug). Use the FORCE variant here instead, which sends
	# SIGKILL directly to the client's pid and is instant/reliable --
	# this test is about the compositor's dispatch mechanism, not kitty's
	# own graceful-shutdown latency.
	#
	# kill_client has no by-ID targeting -- it always force-kills whatever's
	# focused, so this is routed through hl_kill_focused_or_skip rather than
	# dispatched blind (see its definition for why: it killed a real window
	# once already).
	if hl_kill_focused_or_skip W1 "kill_client,force removes the window"; then
		hl_assert_true "kill_client,force removes the window" \
			"$(hl_wait_client_count 0 30 && echo true || echo false)"
	fi
}

test_exchange_client_swaps_positions() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null; hl_wait_client_count 1
	hl_spawn_kitty W2 >/dev/null; hl_wait_client_count 2
	hl_spawn_kitty W3 >/dev/null; hl_wait_client_count 3
	hl_spawn_kitty W4 >/dev/null; hl_wait_client_count 4
	sleep 0.3
	local before; before="$(hl_get "get all-clients" | jq -c '[.clients[] | {title,x}] | sort_by(.title)')"
	hl_dispatch "exchange_client,left"
	local after; after="$(hl_get "get all-clients" | jq -c '[.clients[] | {title,x}] | sort_by(.title)')"
	hl_assert_true "exchange_client,left changes window x-positions" \
		"$([ "$before" != "$after" ] && echo true || echo false)"
}
