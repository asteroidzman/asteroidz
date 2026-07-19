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

hl_tag_name() { hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"HEADLESS-1\") | .tags[] | select(.index==$1) | .name"; }

test_set_tag_name_renames_the_current_tag_only() {
	hl_dispatch "view,2"
	hl_dispatch "set_tag_name,scratch"
	hl_assert_eq "set_tag_name,scratch renames tag 2" "$(hl_tag_name 2)" "scratch"
	hl_assert_eq "...and leaves tag 1 alone" "$(hl_tag_name 1)" "t1"
	hl_dispatch "set_tag_name,t2"  # restore: don't leak the rename into later tests
}

test_set_tag_name_with_empty_value_clears_it() {
	hl_dispatch "view,2"
	hl_dispatch "set_tag_name,scratch"
	hl_dispatch "set_tag_name,"
	hl_assert_eq "set_tag_name with no value clears the custom name (falls back to the tag number)" \
		"$(hl_tag_name 2)" "2"
	hl_dispatch "set_tag_name,t2"  # restore
}
