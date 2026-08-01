# screenshot-ui.sh — the screenshot overlay owns the pointer while it is up.
#
# The overlay freezes ONE output and clamps its selection to that output, so a
# pointer free to walk onto the next screen drops the rectangle against an
# invisible wall while the crosshair keeps going, over pixels that are not part
# of the shot. Confinement is what makes the far edge reachable too:
# overshooting is how anyone selects the last column, and on a multi-monitor
# layout overshooting used to mean landing on the neighbour.
#
# Needs two monitors to say anything at all -- on one output every position is
# trivially "on the captured monitor" and every assertion below passes
# vacuously. So this SKIPS unless a second one exists or can be created, the
# same guard multimonitor.sh uses and for the same reason.
#
# Nothing here confirms a shot. Confirming writes a PNG to ~/Pictures and puts
# it on the clipboard, which is not a side effect a regression run should have
# on the machine it runs on; Escape exercises the same teardown.
#
# harness: needs-second-monitor -- run.sh sorts this after every
# single-monitor module; the output it creates outlives it.

_su_second_mon=""
_su_setup() {
	[ -n "$_su_second_mon" ] && return 0
	if [ "$(hl_monitor_count)" -lt 2 ]; then
		hl_dispatch "create_virtual_output" 1
	fi
	_su_second_mon="$(hl_get "get all-monitors" | jq -r --arg m "$HL_MON" \
		'[.monitors[] | select(.name != $m)][0].name // empty')"
	[ -n "$_su_second_mon" ] || return 1
	# The virtual output changed the layout's bounding box, and every
	# hl_move below is an ABSOLUTE motion scaled against that box.
	hl_sync_pointer_extent
	return 0
}

_su_mon_field() { hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$1\") | .$2"; }
_su_cursor_mon() { hl_get "get cursorpos" | jq -r '.monitor // ""'; }
_su_cursor_x() { hl_get "get cursorpos" | jq -r '.x'; }
_su_escape() { "$HL_WLVKBD" press ESC >/dev/null 2>&1; sleep 0.5; }
# active_tags is [0] exactly while the overview is open (monitor_active_tags in
# ipc.h), which is the only handle on overview state the socket offers.
_su_in_overview() { [ "$(hl_current_tag_index)" = "0" ] && echo true || echo false; }

# Open the overlay on HL_MON. The dispatcher only ARMS the capture; the frame
# is taken by the next render of that output, so nothing is active until one
# has happened -- hence the settle, and hence pointing the pointer at HL_MON
# first, since the overlay follows selmon and selmon follows the pointer.
_su_open() { # _su_open [region|window]
	hl_move "$(( $(_su_mon_field "$HL_MON" x) + 40 ))" \
		"$(( $(_su_mon_field "$HL_MON" y) + 40 ))"
	sleep 0.3
	hl_dispatch "screenshot_ui,${1:-region}" 1
}

test_screenshot_ui_confines_the_cursor_to_the_captured_output() {
	if ! _su_setup; then
		hl_skip "test_screenshot_ui_confines_the_cursor_to_the_captured_output: no second monitor"
		return
	fi

	local sx sy
	sx="$(_su_mon_field "$_su_second_mon" x)"
	sy="$(_su_mon_field "$_su_second_mon" y)"

	# Control first: with no overlay up, that same move DOES cross over. An
	# assertion that the pointer stayed put means nothing until the move it
	# is refusing is known to work.
	hl_move "$(( sx + 200 ))" "$(( sy + 200 ))"
	sleep 0.3
	hl_assert "the pointer can reach the second monitor with no overlay up" \
		"$(_su_cursor_mon)" "$_su_second_mon"

	_su_open region
	hl_move "$(( sx + 200 ))" "$(( sy + 200 ))"
	sleep 0.3
	hl_assert "...and cannot while the overlay is up" \
		"$(_su_cursor_mon)" "$HL_MON"

	_su_escape
}

test_the_captured_outputs_far_edge_stays_reachable() {
	if ! _su_setup; then
		hl_skip "test_the_captured_outputs_far_edge_stays_reachable: no second monitor"
		return
	fi

	# Confining by clamping to the monitor's width would put the last
	# selectable column one pixel short of the screen, which is exactly the
	# column a person aims for by shoving the pointer at the edge.
	local mx mw sx last
	mx="$(_su_mon_field "$HL_MON" x)"
	mw="$(_su_mon_field "$HL_MON" width)"
	sx="$(_su_mon_field "$_su_second_mon" x)"

	_su_open region
	hl_move "$(( sx + 400 ))" "$(( $(_su_mon_field "$HL_MON" y) + 300 ))"
	sleep 0.3
	last="$(_su_cursor_x)"
	# Within the last pixel of HL_MON: at or past width-1, and still short of
	# the edge itself (at exactly mx+mw the pointer is on the next output).
	hl_assert "the pointer is pinned to the captured monitor's last column" \
		"$(python3 -c "print('yes' if $mx + $mw - 1 <= $last < $mx + $mw else 'no ($last)')")" \
		"yes"

	_su_escape
}

test_the_pointer_is_free_again_once_the_overlay_is_gone() {
	if ! _su_setup; then
		hl_skip "test_the_pointer_is_free_again_once_the_overlay_is_gone: no second monitor"
		return
	fi

	local sx sy
	sx="$(_su_mon_field "$_su_second_mon" x)"
	sy="$(_su_mon_field "$_su_second_mon" y)"

	_su_open region
	_su_escape

	hl_move "$(( sx + 200 ))" "$(( sy + 200 ))"
	sleep 0.3
	hl_assert "Escape gives the pointer back to the whole layout" \
		"$(_su_cursor_mon)" "$_su_second_mon"
}

# F7 opens the overlay. Bound at runtime and reloaded rather than set through
# HL_EXTRA_KDL, which run.sh reads before it sources this file -- see the same
# note in quit-confirm.sh, where doing it the other way made every assertion
# pass against a key that was bound to nothing.
_su_bind_done=""
_su_bind() {
	[ -z "$_su_bind_done" ] || return 0
	cat >> "$HL_CONFIG" <<'EOF'
binds {
	NONE+F7 { screenshot_ui region; }
}
EOF
	hl_dispatch "reload_config" 1
	_su_bind_done=1
}

# "Is the overlay up?" has no IPC answer, so it is measured by the thing this
# module already established: while it is up the pointer cannot leave the
# captured output. Every use pairs it with the same move made when the overlay
# is NOT up, so a pointer that has simply stopped working cannot read as a pass.
_su_pointer_is_captive() { # -> true/false
	local sx sy
	sx="$(_su_mon_field "$_su_second_mon" x)"
	sy="$(_su_mon_field "$_su_second_mon" y)"
	hl_move "$(( sx + 200 ))" "$(( sy + 200 ))"
	sleep 0.3
	[ "$(_su_cursor_mon)" = "$HL_MON" ] && echo true || echo false
}

test_the_overview_does_not_swallow_the_screenshot_bind() {
	if ! _su_setup; then
		hl_skip "test_the_overview_does_not_swallow_the_screenshot_bind: no second monitor"
		return
	fi
	_su_bind

	# toggle_overview refuses to enter with nothing to preview, so without a
	# window every assertion below would be about a monitor that never left
	# the desktop -- see the same note at the top of overview.sh.
	hl_spawn_kitty SU1 >/dev/null
	hl_wait_client_count 1

	# The overview is modal by design -- it drops every bind that is not one
	# of its own -- so this has to go through the KEYBOARD. An IPC dispatch
	# never reaches that filter and would pass whether or not the bind works.
	hl_move "$(( $(_su_mon_field "$HL_MON" x) + 40 ))" \
		"$(( $(_su_mon_field "$HL_MON" y) + 40 ))"
	sleep 0.3
	hl_dispatch "toggle_overview" 1.5
	hl_assert_true "the overview is open before the bind is judged" \
		"$(_su_in_overview)"

	hl_assert "the pointer roams freely in the overview with no overlay up" \
		"$(_su_pointer_is_captive)" "false"

	hl_move "$(( $(_su_mon_field "$HL_MON" x) + 40 ))" \
		"$(( $(_su_mon_field "$HL_MON" y) + 40 ))"
	sleep 0.3
	"$HL_WLVKBD" press F7 >/dev/null 2>&1
	sleep 1

	hl_assert "the screenshot bind still opens the overlay from the overview" \
		"$(_su_pointer_is_captive)" "true"

	# Escape belongs to the overlay while it is up -- the overview's own
	# Escape must not also fire, or cancelling a screenshot would throw away
	# the view being photographed.
	_su_escape
	hl_assert_true "cancelling the overlay leaves the overview open" \
		"$(_su_in_overview)"

	# ,1 forces the close: a bare toggle just cycles focus while ov_tab_mode
	# is on, which is the default.
	hl_dispatch "toggle_overview,1" 1.5
	hl_assert_false "and the overview still closes afterwards" \
		"$(_su_in_overview)"
}

test_window_mode_confines_the_cursor_too() {
	if ! _su_setup; then
		hl_skip "test_window_mode_confines_the_cursor_too: no second monitor"
		return
	fi

	# Window mode hit-tests the pointer against a frozen list of windows on
	# ONE monitor, so a pointer on the neighbour resolves to nothing at all
	# and the click confirms an empty selection.
	local sx sy
	sx="$(_su_mon_field "$_su_second_mon" x)"
	sy="$(_su_mon_field "$_su_second_mon" y)"

	_su_open window
	hl_move "$(( sx + 200 ))" "$(( sy + 200 ))"
	sleep 0.3
	hl_assert "window mode confines the pointer as well" \
		"$(_su_cursor_mon)" "$HL_MON"

	_su_escape
}
