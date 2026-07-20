# window-states.sh — per-client boolean state toggles (floating, fullscreen,
# fake-fullscreen, maximize, minimize, pin).

hl_first_client_field() { # hl_first_client_field FIELD -- our own spawned W1,
	# not .clients[0] (which in live mode can just as easily be a real
	# pre-existing window as the test's own spawned one)
	hl_client_field W1 "$1"
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
	hl_spawn_kitty W3 >/dev/null
	hl_spawn_kitty W4 >/dev/null
	hl_wait_client_count 4
	hl_dispatch "toggle_all_floating"
	# checked via our own four spawned windows specifically -- counting ALL
	# floating clients on the tag (the old approach) counts real
	# pre-existing windows too, in live mode.
	hl_assert_true "toggle_all_floating floats every window on the tag" \
		"$([ "$(hl_client_field W1 is_floating)" = "true" ] && [ "$(hl_client_field W2 is_floating)" = "true" ] && \
		   [ "$(hl_client_field W3 is_floating)" = "true" ] && [ "$(hl_client_field W4 is_floating)" = "true" ] && \
		   echo true || echo false)"
}
