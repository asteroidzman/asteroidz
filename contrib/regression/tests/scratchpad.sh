# scratchpad.sh — named scratchpad + toggle_scratchpad (visibility only) +
# special workspaces.
#
# toggle_scratchpad (the bare, un-named one) does NOT put an arbitrary
# window into the scratchpad -- switch_scratchpad_client_state() only has
# branches for a client whose is_in_scratchpad is ALREADY true; called on a
# normal window it just falls through and returns false, a no-op. Only
# toggle_named_scratchpad actually adds a client (via apply_named_scratchpad
# -> set_minimized + switch_scratchpad_client_state). So: use
# toggle_named_scratchpad to add a client to the scratchpad, then
# toggle_scratchpad to test show/hide of an EXISTING scratchpad client.
#
# toggle_named_scratchpad's first arg matches against the client's APPID
# (not its numeric IPC id, despite the name) via regex; the second arg
# matches the title. dispatch's comma-tokenizer collapses empty fields
# (strtok_r skips consecutive delimiters), so passing just one arg (the
# appid) is the reliable way to target "any kitty window" here.

hl_first_client_field() { hl_get "get all-clients" | jq -r ".clients[0].$1"; }
hl_mon_active_special() { hl_get "get all-monitors" | jq -r '.monitors[] | select(.name=="HEADLESS-1") | .active_special'; }

test_toggle_named_scratchpad_adds_the_client() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_assert_false "freshly spawned window isn't a named scratchpad client" "$(hl_first_client_field is_namedscratchpad)"
	hl_dispatch "toggle_named_scratchpad,kitty" 1
	hl_assert_true "toggle_named_scratchpad,kitty marks it as a named scratchpad client" \
		"$(hl_first_client_field is_namedscratchpad)"
}

test_toggle_scratchpad_hides_and_shows_an_existing_scratchpad_client() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "toggle_named_scratchpad,kitty" 1
	local minimized_after_add; minimized_after_add="$(hl_first_client_field is_minimized)"
	hl_dispatch "toggle_scratchpad" 0.5
	local minimized_after_toggle; minimized_after_toggle="$(hl_first_client_field is_minimized)"
	hl_assert_true "toggle_scratchpad flips visibility of an existing scratchpad client" \
		"$([ "$minimized_after_add" != "$minimized_after_toggle" ] && echo true || echo false)"
}

test_toggle_special_workspace_shows_and_hides() {
	hl_dispatch "toggle_special_workspace,scratch"
	hl_assert_eq "toggle_special_workspace,scratch opens it as the active special" "$(hl_mon_active_special)" "scratch"
	hl_dispatch "toggle_special_workspace,scratch"
	hl_assert_eq "toggle_special_workspace,scratch again closes it" "$(hl_mon_active_special)" ""
}

test_move_to_special_workspace() {
	hl_dispatch "view,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "move_to_special_workspace,scratch"
	hl_assert_eq "move_to_special_workspace,scratch tags the client with it" \
		"$(hl_first_client_field special_workspace)" "scratch"
	hl_dispatch "move_to_special_workspace,"
	hl_assert_eq "move_to_special_workspace with an empty name clears it" \
		"$(hl_first_client_field special_workspace)" ""
}
