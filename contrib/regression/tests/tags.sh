# tags.sh — tag/view/toggle_tag/toggle_view/tag_to_left/tag_to_right.

hl_active_tags_json() { hl_get "get all-monitors" | jq -c '.monitors[] | select(.name=="HEADLESS-1") | .active_tags'; }
hl_first_client_tags_json() { hl_get "get all-clients" | jq -c '.clients[0].tags'; }

test_view_switches_active_tag() {
	hl_dispatch "view,1"
	hl_dispatch "view,2"
	hl_assert_eq "view,2 makes tag 2 the active tag" "$(hl_active_tags_json)" "[2]"
}

test_tag_moves_client_to_new_tag() {
	hl_dispatch "view,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "tag,2"
	hl_assert_eq "tag,2 moves the focused client onto tag 2 only" "$(hl_first_client_tags_json)" "[2]"
}

test_toggle_tag_adds_a_second_tag() {
	hl_dispatch "view,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "toggle_tag,2"
	hl_assert_eq "toggle_tag,2 adds tag 2 alongside tag 1" "$(hl_first_client_tags_json)" "[1,2]"
}

test_toggle_view_adds_a_second_active_tag() {
	hl_dispatch "view,1"
	hl_dispatch "toggle_view,2"
	hl_assert_eq "toggle_view,2 makes both tag 1 and 2 active" "$(hl_active_tags_json)" "[1,2]"
}

test_combo_view_shows_only_the_given_tag() {
	hl_dispatch "view,1"
	hl_dispatch "toggle_view,2"
	hl_dispatch "combo_view,3"
	hl_assert_eq "combo_view,3 replaces the active set with just tag 3" "$(hl_active_tags_json)" "[3]"
}

test_tag_to_right_moves_client_up_one_tag() {
	hl_dispatch "view,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "tag_to_right,1"
	hl_assert_eq "tag_to_right,1 moves the client from tag 1 to tag 2" "$(hl_first_client_tags_json)" "[2]"
}

test_tag_to_left_moves_client_down_one_tag() {
	hl_dispatch "view,2"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "tag_to_left,1"
	hl_assert_eq "tag_to_left,1 moves the client from tag 2 to tag 1" "$(hl_first_client_tags_json)" "[1]"
}
