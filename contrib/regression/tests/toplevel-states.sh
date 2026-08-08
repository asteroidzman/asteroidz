# toplevel-states.sh — xdg_toplevel state array: suspended on tag hide/show.
#
# Uses wlstates rather than kitty because the state array's LENGTH is unchanged
# by a tag hide (the window loses `activated` exactly as it gains `suspended`),
# so WAYLAND_DEBUG's "array[20]" cannot tell the two apart. Only a client that
# unpacks the array can assert this.

test_suspended_set_when_tag_hidden() {
	hl_spawn_wlstates SUSP 20 susp >/dev/null
	hl_wait_client_count 1
	sleep 0.5

	hl_assert_true "visible window is not suspended" \
		"$(case "$(hl_wlstates_last susp)" in *suspended*) echo false;; *) echo true;; esac)"

	hl_dispatch "view,2" 0.6
	hl_assert_true "window on a hidden tag is configured suspended" \
		"$(case "$(hl_wlstates_last susp)" in *suspended*) echo true;; *) echo false;; esac)"
}

test_suspended_cleared_when_tag_shown_again() {
	hl_spawn_wlstates SUSP 20 susp2 >/dev/null
	hl_wait_client_count 1
	sleep 0.5

	hl_dispatch "view,2" 0.6
	hl_assert_true "premise: window is suspended while its tag is hidden" \
		"$(case "$(hl_wlstates_last susp2)" in *suspended*) echo true;; *) echo false;; esac)"

	hl_dispatch "view,1" 0.6
	hl_assert_true "returning to the tag clears suspended" \
		"$(case "$(hl_wlstates_last susp2)" in *suspended*) echo false;; *) echo true;; esac)"
}

# The same transition seen from the compositor instead of the client. Worth
# having both: wlstates proves the state reached the wire, is_suspended proves
# the compositor's own bookkeeping agrees with what it sent. A drift between
# them is exactly the bug that would make the IPC field lie.
test_is_suspended_ipc_field_tracks_visibility() {
	hl_spawn_wlstates SUSP 20 susp3 >/dev/null
	hl_wait_client_count 1
	sleep 0.5

	hl_assert_eq "is_suspended false while the window is visible" \
		"$(hl_get "get all-clients" | jq -r '.clients[0].is_suspended')" "false"

	hl_dispatch "view,2" 0.6
	hl_assert_eq "is_suspended true once its tag is hidden" \
		"$(hl_get "get all-clients" | jq -r '.clients[0].is_suspended')" "true"

	# agrees with what the client was actually sent
	hl_assert_true "IPC field agrees with the client's own configure" \
		"$(case "$(hl_wlstates_last susp3)" in *suspended*) echo true;; *) echo false;; esac)"

	hl_dispatch "view,1" 0.6
	hl_assert_eq "is_suspended false again after returning to the tag" \
		"$(hl_get "get all-clients" | jq -r '.clients[0].is_suspended')" "false"
}
