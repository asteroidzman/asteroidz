# mousebind.sh — real Super+drag mouse-binding gestures (not IPC dispatch).
#
# Unlike every other module here, this exercises the ACTUAL input path:
# a held modifier key (via contrib/wlvkbd) plus a real button-press-then-
# motion-then-release pointer sequence (via contrib/wlvptr's drag: action),
# matching how a user's Super+drag move/resize actually reaches the
# compositor (handle_buttonpress reading the seat's current keyboard
# modifiers, then matching config.mouse_bindings). hl_start's own test
# config defines the bindings needed
# (mousebind SUPER,BTN_LEFT,move_resize,curmove /
#  mousebind SUPER,BTN_RIGHT,move_resize,curresize) -- there's no compiled-
# in default, mouse_bindings_count starts at 0 and only grows from KDL
# mousebind entries, so a real user's Super+drag depends entirely on
# having this (or equivalent) in their own config.
#
# Every test below pins a known starting geometry (move_window/
# resize_window with bare, un-prefixed numbers -- an ABSOLUTE set, see
# geometry.sh's comment on the sign-prefix convention) right after spawn,
# rather than trusting whatever size the floating layout gave the window.
# Found the hard way: kitty's floating default can come out close to full
# output height, and a resize test that grabs the bottom-right corner
# (y = window bottom) then drags further down pushes the target point
# past the output's bottom edge -- an out-of-range coordinate for
# wlr-virtual-pointer's motion_absolute (extent-relative), which silently
# produced a no-op resize instead of an error.

# our own spawned W1, not .clients[0] -- in live mode that's whatever real
# window the compositor's list happens to put first (confirmed live
# 2026-07-20: a real Vivaldi window), not the test's own drag target, which
# made every assertion below compare the wrong window's before/after geometry.
hl_client_geom() { hl_get "get all-clients" | jq -c '.clients[] | select(.title=="W1") | {x,y,width,height}'; }

test_super_left_drag_moves_a_floating_window() {
	hl_dispatch "set_layout,float"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "move_window,100,100"
	hl_dispatch "resize_window,300,300"
	sleep 0.3
	local before; before="$(hl_client_geom)"
	local bx by; bx="$(jq -r .x <<<"$before")"; by="$(jq -r .y <<<"$before")"
	hl_super_drag "$((bx + 30))" "$((by + 10))" "$((bx + 230))" "$((by + 210))"
	sleep 0.3
	local after; after="$(hl_client_geom)"
	local ax ay; ax="$(jq -r .x <<<"$after")"; ay="$(jq -r .y <<<"$after")"
	hl_assert_true "Super+left-drag moves the window (x grew)" "$([ "$ax" -gt "$bx" ] && echo true || echo false)"
	hl_assert_true "Super+left-drag moves the window (y grew)" "$([ "$ay" -gt "$by" ] && echo true || echo false)"
}

test_super_right_drag_resizes_a_floating_window() {
	hl_dispatch "set_layout,float"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "move_window,100,100"
	hl_dispatch "resize_window,300,300"
	sleep 0.3
	local before; before="$(hl_client_geom)"
	local bx by bw bh
	bx="$(jq -r .x <<<"$before")"; by="$(jq -r .y <<<"$before")"
	bw="$(jq -r .width <<<"$before")"; bh="$(jq -r .height <<<"$before")"
	local ex=$((bx + bw - 5)) ey=$((by + bh - 5))
	hl_super_rdrag "$ex" "$ey" "$((ex + 150))" "$((ey + 150))"
	sleep 0.3
	local after; after="$(hl_client_geom)"
	local aw ah; aw="$(jq -r .width <<<"$after")"; ah="$(jq -r .height <<<"$after")"
	hl_assert_true "Super+right-drag resizes the window (width grew)" "$([ "$aw" -gt "$bw" ] && echo true || echo false)"
	hl_assert_true "Super+right-drag resizes the window (height grew)" "$([ "$ah" -gt "$bh" ] && echo true || echo false)"
}
