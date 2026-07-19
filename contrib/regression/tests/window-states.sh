# window-states.sh — per-client boolean state toggles (floating, fullscreen,
# fake-fullscreen, maximize, minimize, pin).

hl_first_client_field() { # hl_first_client_field FIELD
	hl_get "get all-clients" | jq -r ".clients[0].$1"
}

test_toggle_floating() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_assert_false "freshly tiled window starts non-floating" "$(hl_first_client_field is_floating)"
	hl_dispatch "toggle_floating"
	hl_assert_true "toggle_floating sets is_floating" "$(hl_first_client_field is_floating)"
	hl_dispatch "toggle_floating"
	hl_assert_false "toggle_floating again clears is_floating" "$(hl_first_client_field is_floating)"
}

test_toggle_fullscreen() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "toggle_fullscreen"
	hl_assert_true "toggle_fullscreen sets is_fullscreen" "$(hl_first_client_field is_fullscreen)"
	hl_dispatch "toggle_fullscreen"
	hl_assert_false "toggle_fullscreen again clears is_fullscreen" "$(hl_first_client_field is_fullscreen)"
}

test_toggle_fake_fullscreen() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "toggle_fake_fullscreen"
	hl_assert_true "toggle_fake_fullscreen sets is_fakefullscreen" "$(hl_first_client_field is_fakefullscreen)"
	hl_dispatch "toggle_fake_fullscreen"
	hl_assert_false "toggle_fake_fullscreen again clears is_fakefullscreen" "$(hl_first_client_field is_fakefullscreen)"
}

test_toggle_maximize() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "toggle_maximize"
	hl_assert_true "toggle_maximize sets is_maximized" "$(hl_first_client_field is_maximized)"
	hl_dispatch "toggle_maximize"
	hl_assert_false "toggle_maximize again clears is_maximized" "$(hl_first_client_field is_maximized)"
}

test_minimize_restore() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "minimize"
	hl_assert_true "minimize sets is_minimized" "$(hl_first_client_field is_minimized)"
	hl_dispatch "restore_minimized"
	hl_assert_false "restore_minimized clears is_minimized" "$(hl_first_client_field is_minimized)"
}

test_pin() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "pin"
	hl_assert_true "pin sets pinned" "$(hl_first_client_field pinned)"
}

test_toggle_all_floating() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2
	hl_dispatch "toggle_all_floating"
	local n; n="$(hl_get "get all-clients" | jq '[.clients[] | select(.is_floating==true)] | length')"
	hl_assert_eq "toggle_all_floating floats every window on the tag" "$n" "2"
}
