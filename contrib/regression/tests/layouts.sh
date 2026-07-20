# layouts.sh — set_layout + per-tag layout state (tile/monocle/scroller/float).
#
# "get all-monitors" -> monitors[].tags[].layout is the layout SYMBOL, not
# name (there's no "layout name" field over IPC at all) -- see layout.h:
#   {"T", dwindle, "tile", ...}  {"S", scroller, "scroller", ...}
#   {"M", monocle, "monocle", ...}  {"F", floatlayout, "float", ...}

# the CURRENTLY ACTIVE tag's layout symbol, not hardcoded tag 1 -- set_layout
# with no explicit target acts on selmon's current tag, which in live mode
# can be anything (leftover from a previous test module). Hardcoding index
# 1 here meant these tests were silently checking the wrong tag whenever the
# live session wasn't already on tag 1 -- confirmed live 2026-07-20.
hl_current_layout_symbol() {
	local idx; idx="$(hl_current_tag_index)"
	hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .tags[] | select(.index==$idx) | .layout"
}

test_set_layout_tile() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "set_layout,tile"
	hl_assert_eq "set_layout,tile -> tag layout symbol is T" "$(hl_current_layout_symbol)" "T"
}

test_set_layout_monocle() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "set_layout,monocle"
	hl_assert_eq "set_layout,monocle -> tag layout symbol is M" "$(hl_current_layout_symbol)" "M"
}

test_set_layout_scroller() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "set_layout,scroller"
	hl_assert_eq "set_layout,scroller -> tag layout symbol is S" "$(hl_current_layout_symbol)" "S"
}

test_set_layout_float() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "set_layout,float"
	hl_assert_eq "set_layout,float -> tag layout symbol is F" "$(hl_current_layout_symbol)" "F"
}

test_switch_layout_cycles() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "set_layout,tile"
	local before; before="$(hl_current_layout_symbol)"
	hl_dispatch "switch_layout"
	local after; after="$(hl_current_layout_symbol)"
	hl_assert_true "switch_layout changes the tag's layout" "$([ "$before" != "$after" ] && echo true || echo false)"
}

test_tile_arranges_two_windows_side_by_side() {
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_spawn_kitty W2 >/dev/null
	hl_spawn_kitty W3 >/dev/null
	hl_spawn_kitty W4 >/dev/null
	hl_wait_client_count 4
	sleep 0.5
	# filtered to our own four spawned windows specifically -- checking
	# ALL clients' x positions on the tag (the old approach) counts real
	# pre-existing windows too, in live mode.
	#
	# tile here is a plain equal-width horizontal split, not dwm-style
	# master+stack: confirmed empirically (all 4 windows get distinct x,
	# identical y). Generalizes directly from the original 2-window check.
	local x1 x2 x3 x4
	x1="$(hl_client_field W1 x)"; x2="$(hl_client_field W2 x)"
	x3="$(hl_client_field W3 x)"; x4="$(hl_client_field W4 x)"
	hl_assert_true "tile layout: all four windows get distinct x positions" \
		"$([ "$x1" != "$x2" ] && [ "$x1" != "$x3" ] && [ "$x1" != "$x4" ] && \
		   [ "$x2" != "$x3" ] && [ "$x2" != "$x4" ] && [ "$x3" != "$x4" ] && \
		   echo true || echo false)"
}

test_monocle_stacks_windows_at_same_position() {
	hl_dispatch "set_layout,monocle"
	hl_spawn_kitty W1 >/dev/null
	hl_spawn_kitty W2 >/dev/null
	hl_spawn_kitty W3 >/dev/null
	hl_spawn_kitty W4 >/dev/null
	hl_wait_client_count 4
	sleep 0.5
	# filtered to our own four spawned windows -- real pre-existing windows
	# on the tag would otherwise inflate "unique x" past 1 in live mode.
	local x1 x2 x3 x4
	x1="$(hl_client_field W1 x)"; x2="$(hl_client_field W2 x)"
	x3="$(hl_client_field W3 x)"; x4="$(hl_client_field W4 x)"
	hl_assert_true "monocle layout: all four windows share the same x" \
		"$([ "$x1" = "$x2" ] && [ "$x2" = "$x3" ] && [ "$x3" = "$x4" ] && echo true || echo false)"
}
