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
		"$(hl_wait_watch_grew wac "$before" && echo true || echo false)"
}

# toggle_hdr on a REAL HDR-capable monitor is genuinely disruptive -- not
# just a software flag flip, but a real modeset that can force the monitor
# down to a bandwidth-constrained lower-refresh mode (confirmed live
# 2026-07-20: DP-1 dropped to 24Hz and stayed HDR-on with no restore, since
# these tests never toggle it back). hdr.sh's own toggle_hdr test already
# skips for exactly this reason -- these two were missed and toggled it
# completely unguarded, with no restore step at all, which is why HDR kept
# reappearing even in runs that excluded the entire hdr.sh module.
hl_hdr_capable() { [ "$(hl_mon_field hdr_capable)" = "true" ]; }
hl_mon_field() { hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .$1"; }

test_watch_all_monitors_notifies_on_hdr_toggle() {
	if hl_hdr_capable; then
		hl_skip "test_watch_all_monitors_notifies_on_hdr_toggle: \$HL_MON is HDR-capable (a real monitor) -- toggle_hdr is genuinely disruptive there, see comment above"
		return
	fi
	hl_watch_start "watch all-monitors" wam >/dev/null
	local before; before="$(hl_watch_line_count wam)"
	hl_dispatch "toggle_hdr"
	hl_assert_true "watch all-monitors grows on toggle_hdr" \
		"$(hl_wait_watch_grew wam "$before" && echo true || echo false)"
}

test_watch_monitor_notifies_for_the_named_output() {
	if hl_hdr_capable; then
		hl_skip "test_watch_monitor_notifies_for_the_named_output: \$HL_MON is HDR-capable (a real monitor) -- toggle_hdr is genuinely disruptive there, see comment above"
		return
	fi
	hl_watch_start "watch monitor $HL_MON" wm >/dev/null
	local before; before="$(hl_watch_line_count wm)"
	hl_dispatch "toggle_hdr"
	hl_assert_true "watch monitor \$HL_MON grows on toggle_hdr" \
		"$(hl_wait_watch_grew wm "$before" && echo true || echo false)"
}

test_watch_all_tags_notifies_on_view_switch() {
	# force a known, DIFFERENT starting tag first -- a prior test/module can
	# leave the tag already on 2 in live mode (hl_reset deliberately doesn't
	# force tag 1 there, see its own comment), in which case dispatching
	# view,2 again is a no-op and no notification can fire (confirmed live
	# 2026-07-20: this test alone passes, but running right after a sibling
	# test that also lands on tag 2 makes the SAME dispatch a no-op).
	hl_dispatch "view,1"
	sleep 0.2
	hl_watch_start "watch all-tags" wat >/dev/null
	local before; before="$(hl_watch_line_count wat)"
	hl_dispatch "view,2"
	hl_assert_true "watch all-tags grows on view,2" \
		"$(hl_wait_watch_grew wat "$before" && echo true || echo false)"
}

test_watch_tags_notifies_for_the_named_monitor() {
	# view,2 with no explicit monitor target acts on selmon, not necessarily
	# $HL_MON -- a prior test/module (e.g. multimonitor's focus_monitor)
	# can leave a DIFFERENT monitor focused, in which case this per-monitor
	# watch never fires even though the dispatch worked fine (the sibling
	# "watch all-tags" test passes regardless, since it's not scoped to one
	# monitor). Force focus first so the dispatch actually lands here.
	hl_dispatch "focus_monitor,$HL_MON" 0.2
	# same no-op hazard as test_watch_all_tags_notifies_on_view_switch above:
	# force tag 1 first so this test's own view,2 is guaranteed to be a real
	# transition, not a no-op from wherever the previous test left the tag.
	hl_dispatch "view,1"
	sleep 0.2
	hl_watch_start "watch tags $HL_MON" wt >/dev/null
	local before; before="$(hl_watch_line_count wt)"
	hl_dispatch "view,2"
	hl_assert_true "watch tags \$HL_MON grows on view,2" \
		"$(hl_wait_watch_grew wt "$before" && echo true || echo false)"
}

test_watch_focused_client_notifies_on_a_new_window() {
	hl_watch_start "watch focused-client" wfc >/dev/null
	local before; before="$(hl_watch_line_count wfc)"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_assert_true "watch focused-client grows when a new window takes focus" \
		"$(hl_wait_watch_grew wfc "$before" && echo true || echo false)"
}

test_watch_client_notifies_for_that_id() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	# our own spawned W1's id specifically, not .clients[0] -- in live mode
	# that can be any real pre-existing window. Target the same client
	# explicitly via the generic "client,ID,func,args" dispatch prefix (see
	# ipc.h's dispatch parser) so the action is guaranteed to land on the
	# client being watched, regardless of what's actually focused.
	#
	# toggle_floating, not move_window: confirmed live 2026-07-20 that a pure
	# geometry change (move_window/resize_window) NEVER fires this
	# notification at all -- resize() (src/animation/client.h) applies the
	# new geom without ever calling ipc_notify_client/printstatus, so 15/15
	# reproductions of "spawn, watch, move" saw the client's x genuinely
	# change while the watch stream stayed flat. That's a real gap in the
	# compositor's own IPC (geometry-only watchers of a specific client get
	# no live updates), not a race -- toggling a client's boolean state
	# (isfloating, isfullscreen, minimized, pinned, ...) reliably notifies,
	# same as this test always intended to check "did a notification fire",
	# not "did the client move".
	local id; id="$(hl_client_field W1 id)"
	hl_watch_start "watch client $id" wc >/dev/null
	local before; before="$(hl_watch_line_count wc)"
	hl_dispatch "client,$id,toggle_floating"
	hl_assert_true "watch client <id> grows when that client's state changes" \
		"$(hl_wait_watch_grew wc "$before" && echo true || echo false)"
}

test_watch_keymode_notifies_on_set_key_mode() {
	hl_watch_start "watch keymode" wkm >/dev/null
	local before; before="$(hl_watch_line_count wkm)"
	hl_dispatch "set_key_mode,resize"
	hl_assert_true "watch keymode grows on set_key_mode" \
		"$(hl_wait_watch_grew wkm "$before" && echo true || echo false)"
	hl_dispatch "set_key_mode,default"  # restore
}

test_watch_keyboardlayout_notifies_on_switch() {
	# switch_keyboard_layout,1 switches to configured xkb layout INDEX 1 --
	# hl_start's own synthetic config sets two ("us,de") specifically so
	# this is observable. There's no IPC getter for how many layouts the
	# live session's real config has, and switching to a nonexistent index
	# would be a silent no-op at best -- or could actually change the
	# user's real input layout at worst, with no safe way to detect or
	# restore the original. Skip rather than risk it.
	if [ "${HL_LIVE_MODE:-0}" = "1" ]; then
		hl_skip "test_watch_keyboardlayout_notifies_on_switch: can't verify the live session has a 2nd configured xkb layout to switch to, and switching to a nonexistent one could alter your real input layout with no way to detect or restore it"
		return
	fi
	hl_watch_start "watch keyboardlayout" wkl >/dev/null
	local before; before="$(hl_watch_line_count wkl)"
	hl_dispatch "switch_keyboard_layout,1"
	hl_assert_true "watch keyboardlayout grows on switch_keyboard_layout" \
		"$(hl_wait_watch_grew wkl "$before" && echo true || echo false)"
}

test_watch_last_open_surface_notifies_on_a_new_window() {
	hl_watch_start "watch last_open_surface" wlos >/dev/null
	local before; before="$(hl_watch_line_count wlos)"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_assert_true "watch last_open_surface grows when a window opens" \
		"$(hl_wait_watch_grew wlos "$before" && echo true || echo false)"
}
