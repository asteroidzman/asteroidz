# scroller.sh — scroller_stack, scroller_consume, scroller_expel,
# set_proportion, switch_proportion_preset.
#
# All of these early-return no-ops unless is_scroller_layout(selmon) is true
# for the CURRENT tag -- hl_start's config gives tag 4 a scroller layout
# specifically for this module (view,4 first, every test below).
# switch_proportion_preset additionally needs config.scroller_proportion_
# preset_count > 0 (default 0/off, same class of surprise as dwindle_manual_
# split) -- hl_start's config sets `scroller { preset 0.3,0.5,0.8 }`.

hl_client_field() { hl_get "get all-clients" | jq -r ".clients[] | select(.title==\"$1\") | .$2"; }
# scroller_proportion round-trips through a 32-bit float, so e.g. 0.6 comes
# back as 0.600000023841858 -- round before comparing.
hl_client_field_rounded() { hl_client_field "$1" "$2" | awk '{printf "%.1f", $1}'; }

test_scroller_stack_merges_into_one_column() {
	hl_dispatch "view,4"
	hl_dispatch "set_layout,scroller"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2
	sleep 0.3
	# filtered to our own two spawned windows -- checking unique x/y across
	# ALL clients on the tag (the old approach) counts real pre-existing
	# windows too, in live mode.
	local x1_0 x2_0
	x1_0="$(hl_client_field W1 x)"; x2_0="$(hl_client_field W2 x)"
	hl_assert_true "before stacking: two separate columns (different x)" \
		"$([ "$x1_0" != "$x2_0" ] && echo true || echo false)"

	hl_dispatch "scroller_stack,left"
	sleep 0.3
	local x1_1 x2_1 y1_1 y2_1
	x1_1="$(hl_client_field W1 x)"; x2_1="$(hl_client_field W2 x)"
	y1_1="$(hl_client_field W1 y)"; y2_1="$(hl_client_field W2 y)"
	hl_assert_true "scroller_stack,left: now share one column (same x, different y)" \
		"$([ "$x1_1" = "$x2_1" ] && [ "$y1_1" != "$y2_1" ] && echo true || echo false)"
}

test_scroller_expel_splits_the_stack_back_out() {
	hl_dispatch "view,4"
	hl_dispatch "set_layout,scroller"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2
	sleep 0.3
	hl_dispatch "scroller_stack,left"
	sleep 0.3
	hl_dispatch "scroller_expel"
	sleep 0.3
	local x1 x2
	x1="$(hl_client_field W1 x)"; x2="$(hl_client_field W2 x)"
	hl_assert_true "scroller_expel: back to two separate columns" \
		"$([ "$x1" != "$x2" ] && echo true || echo false)"
}

test_scroller_consume_is_a_no_op_with_nothing_to_pull_in() {
	hl_dispatch "view,4"
	hl_dispatch "set_layout,scroller"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	# a single column, nothing after it to consume -- pins the no-op rather
	# than asserting a fake effect (same pattern as the zero-client overview
	# refusal in overview.sh)
	hl_dispatch "scroller_consume"
	sleep 0.3
	hl_assert_eq "scroller_consume with only one column: still one client" \
		"$(hl_client_count)" "1"
}

test_set_proportion_absolute() {
	# scroller_ignore_proportion_single has no IPC getter, and its compile-
	# time default is actually 1 (not 0 -- this test used to "restore" to 0
	# at the end, which was always wrong, it just never mattered headlessly
	# since the whole instance gets discarded). No safe way to capture the
	# live session's real current value, so skip there rather than risk
	# permanently flipping the user's real config to the wrong value.
	hl_skip_if_live_unrestorable_option "test_set_proportion_absolute" scroller_ignore_proportion_single || return
	hl_dispatch "view,4"
	hl_dispatch "set_layout,scroller"
	hl_dispatch "set_option,scroller_ignore_proportion_single,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "set_proportion,0.6"
	sleep 0.3
	hl_assert_eq "set_proportion,0.6 sets scroller_proportion" \
		"$(hl_client_field_rounded W1 scroller_proportion)" "0.6"
	hl_dispatch "set_option,scroller_ignore_proportion_single,1"  # restore actual compile-time default (not 0)
}

test_switch_proportion_preset_cycles_through_configured_presets() {
	hl_skip_if_live_unrestorable_option "test_switch_proportion_preset_cycles_through_configured_presets" scroller_ignore_proportion_single || return
	hl_dispatch "view,4"
	hl_dispatch "set_layout,scroller"
	hl_dispatch "set_option,scroller_ignore_proportion_single,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "set_proportion,0.3"
	sleep 0.3
	hl_dispatch "switch_proportion_preset"
	sleep 0.3
	hl_assert_eq "switch_proportion_preset from 0.3 advances to the next preset (0.5)" \
		"$(hl_client_field W1 scroller_proportion)" "0.5"
	hl_dispatch "switch_proportion_preset,prev"
	sleep 0.3
	hl_assert_eq "switch_proportion_preset,prev goes back to 0.3" \
		"$(hl_client_field_rounded W1 scroller_proportion)" "0.3"
	hl_dispatch "set_option,scroller_ignore_proportion_single,1"  # restore actual compile-time default (not 0)
}
