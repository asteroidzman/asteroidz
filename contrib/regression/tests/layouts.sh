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
	# "tile" here is dwindle: a BSP-style recursive split that inserts each
	# new window by splitting whichever node currently has FOCUS (see
	# dwindle_insert in src/layout/dwindle.h). That means the exact shape
	# (equal columns vs. a nested quad split) depends on focus-follows-spawn
	# timing at insert time, not just window count -- confirmed live
	# 2026-07-20: 4 rapidly-spawned windows produced a nested split (two
	# windows sharing an x, at different y) instead of 4 equal columns, while
	# the same test spawned headlessly produced 4 equal columns. Asserting a
	# specific shape ("all x distinct") is asserting a timing artifact, not a
	# real invariant of the layout. The one thing that IS a real invariant of
	# any correct tiling arrangement, regardless of exact split shape: no two
	# of the four windows overlap.
	local x1 y1 w1 h1 x2 y2 w2 h2 x3 y3 w3 h3 x4 y4 w4 h4
	x1="$(hl_client_field W1 x)"; y1="$(hl_client_field W1 y)"; w1="$(hl_client_field W1 width)"; h1="$(hl_client_field W1 height)"
	x2="$(hl_client_field W2 x)"; y2="$(hl_client_field W2 y)"; w2="$(hl_client_field W2 width)"; h2="$(hl_client_field W2 height)"
	x3="$(hl_client_field W3 x)"; y3="$(hl_client_field W3 y)"; w3="$(hl_client_field W3 width)"; h3="$(hl_client_field W3 height)"
	x4="$(hl_client_field W4 x)"; y4="$(hl_client_field W4 y)"; w4="$(hl_client_field W4 width)"; h4="$(hl_client_field W4 height)"
	local overlaps
	overlaps="$(jq -n \
		--argjson a "$(jq -n --arg x "$x1" --arg y "$y1" --arg w "$w1" --arg h "$h1" '{x:($x|tonumber),y:($y|tonumber),w:($w|tonumber),h:($h|tonumber)}')" \
		--argjson b "$(jq -n --arg x "$x2" --arg y "$y2" --arg w "$w2" --arg h "$h2" '{x:($x|tonumber),y:($y|tonumber),w:($w|tonumber),h:($h|tonumber)}')" \
		--argjson c "$(jq -n --arg x "$x3" --arg y "$y3" --arg w "$w3" --arg h "$h3" '{x:($x|tonumber),y:($y|tonumber),w:($w|tonumber),h:($h|tonumber)}')" \
		--argjson d "$(jq -n --arg x "$x4" --arg y "$y4" --arg w "$w4" --arg h "$h4" '{x:($x|tonumber),y:($y|tonumber),w:($w|tonumber),h:($h|tonumber)}')" \
		'def overlaps(r1; r2): (r1.x < (r2.x + r2.w)) and (r2.x < (r1.x + r1.w)) and (r1.y < (r2.y + r2.h)) and (r2.y < (r1.y + r1.h));
		 [overlaps($a;$b), overlaps($a;$c), overlaps($a;$d), overlaps($b;$c), overlaps($b;$d), overlaps($c;$d)] | any')"
	hl_assert_true "tile layout: no two of the four windows overlap" \
		"$([ "$overlaps" = "false" ] && echo true || echo false)"
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
