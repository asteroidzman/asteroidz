# vrr.sh — per-client VRR state exposed in client JSON after 0.17.0
# (get focused-client / get client / get all-clients now carry "vrr" and
# "vrr_only_fullscreen"). See build_client_json in src/ipc/ipc.h.
#
# VRR is an output-wide hardware state, so a client's "vrr" is the committed
# adaptive-sync status of its monitor -- it must agree with that monitor's
# own "vrr" field. The headless output is not adaptive-sync-capable, so both
# read false here; the point of the test is the wiring/consistency, not the
# specific value.

hl_vrr_mon_field() { hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .$1"; }

test_client_json_carries_vrr_fields() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.2
	local vrr vof
	vrr="$(hl_client_field W1 vrr)"
	vof="$(hl_client_field W1 vrr_only_fullscreen)"
	hl_assert_true "client JSON has a boolean 'vrr' field" \
		"$([ "$vrr" = "true" ] || [ "$vrr" = "false" ] && echo true || echo false)"
	hl_assert_true "client JSON has a boolean 'vrr_only_fullscreen' field" \
		"$([ "$vof" = "true" ] || [ "$vof" = "false" ] && echo true || echo false)"
	# no force_hdr/vrr_only_fullscreen rule in the harness config -> default off
	hl_assert_false "vrr_only_fullscreen defaults false with no matching rule" "$vof"
}

test_client_vrr_matches_its_monitor() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.2
	local client_vrr mon_vrr
	client_vrr="$(hl_client_field W1 vrr)"
	mon_vrr="$(hl_vrr_mon_field vrr)"
	hl_assert_eq "a client's 'vrr' equals its monitor's committed 'vrr'" \
		"$client_vrr" "$mon_vrr"
}

test_focused_client_carries_vrr_fields() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.2
	local vrr
	vrr="$(hl_get "get focused-client" | jq -r '.vrr')"
	hl_assert_true "get focused-client also reports a boolean 'vrr'" \
		"$([ "$vrr" = "true" ] || [ "$vrr" = "false" ] && echo true || echo false)"
}
