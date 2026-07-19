# layouts.sh — set_layout + per-tag layout state (tile/monocle/scroller/float).
#
# "get all-monitors" -> monitors[].tags[].layout is the layout SYMBOL, not
# name (there's no "layout name" field over IPC at all) -- see layout.h:
#   {"T", dwindle, "tile", ...}  {"S", scroller, "scroller", ...}
#   {"M", monocle, "monocle", ...}  {"F", floatlayout, "float", ...}

hl_tag1_layout_symbol() {
	hl_get "get all-monitors" | jq -r '.monitors[] | select(.name=="HEADLESS-1") | .tags[] | select(.index==1) | .layout'
}

test_set_layout_tile() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "set_layout,tile"
	hl_assert_eq "set_layout,tile -> tag layout symbol is T" "$(hl_tag1_layout_symbol)" "T"
}

test_set_layout_monocle() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "set_layout,monocle"
	hl_assert_eq "set_layout,monocle -> tag layout symbol is M" "$(hl_tag1_layout_symbol)" "M"
}

test_set_layout_scroller() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "set_layout,scroller"
	hl_assert_eq "set_layout,scroller -> tag layout symbol is S" "$(hl_tag1_layout_symbol)" "S"
}

test_set_layout_float() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "set_layout,float"
	hl_assert_eq "set_layout,float -> tag layout symbol is F" "$(hl_tag1_layout_symbol)" "F"
}

test_switch_layout_cycles() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "set_layout,tile"
	local before; before="$(hl_tag1_layout_symbol)"
	hl_dispatch "switch_layout"
	local after; after="$(hl_tag1_layout_symbol)"
	hl_assert_true "switch_layout changes the tag's layout" "$([ "$before" != "$after" ] && echo true || echo false)"
}

test_tile_arranges_two_windows_side_by_side() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2
	sleep 0.5
	local xs; xs="$(hl_get "get all-clients" | jq -r '[.clients[].x] | sort | join(",")')"
	local x1 x2
	x1="$(cut -d, -f1 <<<"$xs")"; x2="$(cut -d, -f2 <<<"$xs")"
	hl_assert_true "tile layout: two windows get distinct x positions" \
		"$([ "$x1" != "$x2" ] && [ -n "$x2" ] && echo true || echo false)"
}

test_monocle_stacks_windows_at_same_position() {
	hl_dispatch "set_layout,monocle"
	hl_spawn_kitty W1 >/dev/null
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2
	sleep 0.5
	local xs; xs="$(hl_get "get all-clients" | jq -r '[.clients[].x] | unique | length')"
	hl_assert_eq "monocle layout: all windows share the same x" "$xs" "1"
}
