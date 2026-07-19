# multimonitor.sh — focus_monitor, tag_cross_monitor, view_cross_monitor,
# create_virtual_output.
#
# SKIPS ENTIRELY under the default single-output topology (hl_monitor_count
# guard below) -- run this module on its own with a second monitor already
# present, or let it create one itself via create_virtual_output (which is
# how these tests actually get their second monitor; see below). Adding a
# permanent second `output` block to hl_start's own default config was
# tried and reverted: it changes which monitor is selmon by default and
# silently broke the overview tests (they dispatch against whatever
# monitor is focused, not always HEADLESS-1) -- multi-monitor topology
# needs to stay opt-in per-module, not global.
#
# A KDL `output HEADLESS-2 { ... }` config block does NOT create a second
# headless output by itself -- create_output() in asteroidz.c only ever
# creates ONE output automatically (a `bool *done` guard), config blocks
# for outputs that don't exist yet just sit unused. The dispatch function
# create_virtual_output is the actual mechanism (it calls the same
# create_output() again, so a headless backend adds another
# wlr_headless_add_output); that's what these tests use to get HEADLESS-2.
#
# NOT tested here: disable_monitor/enable_monitor/dpms_{on,off,toggle}_
# monitor -- there's no IPC-exposed field for output-enabled or DPMS/asleep
# state (grepped build_monitor_json in ipc.h; m->asleep isn't surfaced),
# so there's nothing reliable to assert against short of adding a new IPC
# field. destroy_all_virtual_output is deliberately not tested at all: it
# destroys every wlr_output_is_headless() output, which under a pure
# headless backend is ALL of them including the original HEADLESS-1 --
# calling it here would kill the whole test compositor, not just the
# "virtual" one just created.

hl_monitor_names() { hl_get "get all-monitors" | jq -c '[.monitors[].name] | sort'; }
hl_active_monitor_name() { hl_get "get all-monitors" | jq -r '.monitors[] | select(.active==true) | .name'; }

hl_ensure_second_monitor() { # idempotent: create HEADLESS-2 if not already present
	if [ "$(hl_monitor_count)" -lt 2 ]; then
		hl_dispatch "create_virtual_output" 1
	fi
}

test_create_virtual_output_adds_a_monitor() {
	local before; before="$(hl_monitor_count)"
	hl_dispatch "create_virtual_output" 1
	local after; after="$(hl_monitor_count)"
	if [ "$before" -ge 2 ]; then
		hl_skip "test_create_virtual_output_adds_a_monitor: already >=2 monitors from a prior test in this run"
		return
	fi
	hl_assert_eq "create_virtual_output adds exactly one monitor" "$after" "$((before + 1))"
}

test_focus_monitor_by_name() {
	hl_ensure_second_monitor
	if [ "$(hl_monitor_count)" -lt 2 ]; then
		hl_skip "test_focus_monitor_by_name: could not get a second monitor"
		return
	fi
	hl_dispatch "focus_monitor,HEADLESS-1" 0.3
	hl_assert_eq "focus_monitor,HEADLESS-1 makes it active" "$(hl_active_monitor_name)" "HEADLESS-1"
	hl_dispatch "focus_monitor,HEADLESS-2" 0.3
	hl_assert_eq "focus_monitor,HEADLESS-2 makes it active" "$(hl_active_monitor_name)" "HEADLESS-2"
}

test_tag_cross_monitor_moves_a_client() {
	hl_ensure_second_monitor
	if [ "$(hl_monitor_count)" -lt 2 ]; then
		hl_skip "test_tag_cross_monitor_moves_a_client: could not get a second monitor"
		return
	fi
	hl_dispatch "focus_monitor,HEADLESS-1" 0.3
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_assert_eq "freshly spawned window starts on HEADLESS-1" \
		"$(hl_get "get all-clients" | jq -r '.clients[0].monitor')" "HEADLESS-1"
	hl_dispatch "tag_cross_monitor,1,HEADLESS-2" 0.5
	hl_assert_eq "tag_cross_monitor,1,HEADLESS-2 moves it there" \
		"$(hl_get "get all-clients" | jq -r '.clients[0].monitor')" "HEADLESS-2"
}

test_view_cross_monitor_changes_the_other_monitors_tag() {
	hl_ensure_second_monitor
	if [ "$(hl_monitor_count)" -lt 2 ]; then
		hl_skip "test_view_cross_monitor_changes_the_other_monitors_tag: could not get a second monitor"
		return
	fi
	hl_dispatch "focus_monitor,HEADLESS-1" 0.3
	hl_dispatch "view_cross_monitor,2,HEADLESS-2" 0.5
	local tags; tags="$(hl_get "get all-monitors" | jq -c '.monitors[] | select(.name=="HEADLESS-2") | .active_tags')"
	hl_assert_eq "view_cross_monitor,2,HEADLESS-2 sets its active tag to 2" "$tags" "[2]"
}
