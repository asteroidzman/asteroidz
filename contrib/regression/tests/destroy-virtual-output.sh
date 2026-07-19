# destroy-virtual-output.sh — destroy_all_virtual_output.
#
# Gated behind HL_ALLOW_DESTRUCTIVE=1 and SKIPPED otherwise, even when this
# module is run on its own: destroy_all_virtual_output destroys every
# wlr_output_is_headless() output, which under this test harness's pure
# headless backend is ALL of them, including the original HEADLESS-1 --
# running it corrupts "HEADLESS-1" as an assumed-stable name for the rest
# of a shared run.sh instance (every other module dispatches against it
# unconditionally). Verified in isolation that the compositor actually
# recovers gracefully (0 monitors is survivable, and a fresh
# create_virtual_output afterward works and clients map to it fine) --
# just under a NEW auto-incremented name (HEADLESS-3, not HEADLESS-1), so
# this can never be folded into the default shared-instance run.
#
# Run standalone: HL_ALLOW_DESTRUCTIVE=1 bash contrib/regression/run.sh destroy-virtual-output

test_destroy_all_virtual_output_and_recover() {
	if [ "${HL_ALLOW_DESTRUCTIVE:-0}" != "1" ]; then
		hl_skip "test_destroy_all_virtual_output_and_recover: needs HL_ALLOW_DESTRUCTIVE=1 (destroys HEADLESS-1, corrupting the rest of a shared run)"
		return
	fi

	hl_dispatch "create_virtual_output" 1
	local before; before="$(hl_monitor_count)"
	hl_assert_eq "starts with 2 monitors" "$before" "2"

	hl_dispatch "destroy_all_virtual_output" 1
	hl_assert_eq "destroy_all_virtual_output removes every headless output" "$(hl_monitor_count)" "0"

	hl_dispatch "create_virtual_output" 1
	hl_assert_eq "the compositor recovers: a fresh create_virtual_output works" "$(hl_monitor_count)" "1"

	hl_spawn_kitty W1 >/dev/null
	hl_assert_true "a client can map to the recovered output" \
		"$(hl_wait_client_count 1 && echo true || echo false)"
}
