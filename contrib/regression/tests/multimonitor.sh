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
# disable_monitor/enable_monitor/dpms_{on,off,toggle}_monitor are now
# testable: build_monitor_json() in ipc.h gained "enabled"/"asleep" fields
# (there wasn't one at all before -- m->wlr_output->enabled/m->asleep were
# tracked internally but never surfaced over IPC).
#
# destroy_all_virtual_output is STILL deliberately not tested: it destroys
# every wlr_output_is_headless() output, which under a pure headless
# backend is ALL of them including the original HEADLESS-1 -- calling it
# here would kill the whole test compositor, not just the "virtual" one
# just created.
#
# harness: needs-second-monitor -- run.sh sorts this after every
# single-monitor module; the output it creates outlives it.

hl_monitor_names() { hl_get "get all-monitors" | jq -c '[.monitors[].name] | sort'; }
hl_monitor_field() { hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$1\") | .$2"; }
hl_active_monitor_name() { hl_get "get all-monitors" | jq -r '.monitors[] | select(.active==true) | .name'; }

# resolved once into HL_SECOND_MON -- everything below targets this instead
# of a hardcoded "HEADLESS-2". In real-monitor live mode (HL_LIVE_REAL_MON),
# prefer an already-connected REAL second display (confirmed live
# 2026-07-20: HDMI-A-1) over creating a virtual output: the old approach of
# hardcoding "HEADLESS-2" silently dispatched to a monitor that was never
# actually created in this mode -- hl_ensure_second_monitor's own
# count-based guard already saw >=2 monitors (HEADLESS-1 from an earlier
# module's leftover virtual output, plus the real second display) and
# skipped calling create_virtual_output, so "HEADLESS-2" never existed.
HL_SECOND_MON=""
hl_ensure_second_monitor() {
	[ -n "$HL_SECOND_MON" ] && return 0
	if [ "${HL_LIVE_REAL_MON:-0}" = "1" ]; then
		HL_SECOND_MON="$(hl_get "get all-monitors" | jq -r --arg m "$HL_MON" \
			'[.monitors[] | select(.name != $m) | select(.name | startswith("HEADLESS") | not)][0].name // empty')"
		[ -n "$HL_SECOND_MON" ]
		return $?
	fi
	if [ "$(hl_monitor_count)" -lt 2 ]; then
		hl_dispatch "create_virtual_output" 1
	fi
	HL_SECOND_MON="$(hl_get "get all-monitors" | jq -r --arg m "$HL_MON" '[.monitors[] | select(.name != $m)][0].name // empty')"
	[ -n "$HL_SECOND_MON" ]
}

test_create_virtual_output_adds_a_monitor() {
	# in real-monitor live mode there's no reason to ever create a headless
	# output here -- hl_ensure_second_monitor now prefers the real second
	# display instead (see above), and this dispatch used to fire
	# UNCONDITIONALLY before even checking monitor count, spawning a new
	# leftover headless output on the user's real session every single run
	# regardless of mode. Confirmed live 2026-07-20. Skip entirely here.
	if [ "${HL_LIVE_REAL_MON:-0}" = "1" ]; then
		hl_skip "test_create_virtual_output_adds_a_monitor: real-monitor live mode has no use for a virtual output here"
		return
	fi
	local before; before="$(hl_monitor_count)"
	if [ "$before" -ge 2 ]; then
		hl_skip "test_create_virtual_output_adds_a_monitor: already >=2 monitors from a prior test in this run"
		return
	fi
	hl_dispatch "create_virtual_output" 1
	local after; after="$(hl_monitor_count)"
	hl_assert_eq "create_virtual_output adds exactly one monitor" "$after" "$((before + 1))"
}

test_focus_monitor_by_name() {
	if ! hl_ensure_second_monitor; then
		hl_skip "test_focus_monitor_by_name: could not get a second monitor"
		return
	fi
	hl_dispatch "focus_monitor,$HL_MON" 0.3
	hl_assert_eq "focus_monitor,\$HL_MON makes it active" "$(hl_active_monitor_name)" "$HL_MON"
	hl_dispatch "focus_monitor,$HL_SECOND_MON" 0.3
	hl_assert_eq "focus_monitor,\$HL_SECOND_MON makes it active" "$(hl_active_monitor_name)" "$HL_SECOND_MON"
	hl_dispatch "focus_monitor,$HL_MON" 0.3  # restore focus to the primary test monitor
}

test_tag_cross_monitor_moves_a_client() {
	if ! hl_ensure_second_monitor; then
		hl_skip "test_tag_cross_monitor_moves_a_client: could not get a second monitor"
		return
	fi
	hl_dispatch "focus_monitor,$HL_MON" 0.3
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_assert_eq "freshly spawned window starts on \$HL_MON" \
		"$(hl_client_field W1 monitor)" "$HL_MON"
	hl_dispatch "tag_cross_monitor,1,$HL_SECOND_MON" 0.5
	hl_assert_eq "tag_cross_monitor,1,\$HL_SECOND_MON moves it there" \
		"$(hl_client_field W1 monitor)" "$HL_SECOND_MON"
}

test_view_cross_monitor_changes_the_other_monitors_tag() {
	if ! hl_ensure_second_monitor; then
		hl_skip "test_view_cross_monitor_changes_the_other_monitors_tag: could not get a second monitor"
		return
	fi
	hl_dispatch "focus_monitor,$HL_MON" 0.3
	hl_dispatch "view_cross_monitor,2,$HL_SECOND_MON" 0.5
	local tags; tags="$(hl_get "get all-monitors" | jq -c ".monitors[] | select(.name==\"$HL_SECOND_MON\") | .active_tags")"
	hl_assert_eq "view_cross_monitor,2,\$HL_SECOND_MON sets its active tag to 2" "$tags" "[2]"
}

test_disable_enable_monitor() {
	if ! hl_ensure_second_monitor; then
		hl_skip "test_disable_enable_monitor: could not get a second monitor"
		return
	fi
	if [ "${HL_LIVE_REAL_MON:-0}" = "1" ]; then
		hl_skip "test_disable_enable_monitor: \$HL_SECOND_MON is a real monitor -- disabling/re-enabling it risks a mode renegotiation to a different (lower) refresh rate, same class of issue confirmed live 2026-07-20 with DPMS-cycling DP-1"
		return
	fi
	hl_dispatch "disable_monitor,$HL_SECOND_MON" 1
	hl_assert_false "disable_monitor,\$HL_SECOND_MON disables it" "$(hl_monitor_field "$HL_SECOND_MON" enabled)"
	hl_assert_false "disable_monitor does NOT mark it asleep (a full disable, not DPMS)" \
		"$(hl_monitor_field "$HL_SECOND_MON" asleep)"
	hl_dispatch "enable_monitor,$HL_SECOND_MON" 1
	hl_assert_true "enable_monitor,\$HL_SECOND_MON re-enables it" "$(hl_monitor_field "$HL_SECOND_MON" enabled)"
}

test_dpms_off_on_monitor() {
	if ! hl_ensure_second_monitor; then
		hl_skip "test_dpms_off_on_monitor: could not get a second monitor"
		return
	fi
	if [ "${HL_LIVE_REAL_MON:-0}" = "1" ]; then
		hl_skip "test_dpms_off_on_monitor: \$HL_SECOND_MON is a real monitor -- DPMS-cycling it risks a mode renegotiation to a different (lower) refresh rate, confirmed live 2026-07-20 on DP-1"
		return
	fi
	hl_dispatch "dpms_off_monitor,$HL_SECOND_MON" 1
	hl_assert_false "dpms_off_monitor disables the output" "$(hl_monitor_field "$HL_SECOND_MON" enabled)"
	hl_assert_true "dpms_off_monitor marks it asleep (unlike disable_monitor)" \
		"$(hl_monitor_field "$HL_SECOND_MON" asleep)"
	hl_dispatch "dpms_on_monitor,$HL_SECOND_MON" 1
	hl_assert_true "dpms_on_monitor re-enables it" "$(hl_monitor_field "$HL_SECOND_MON" enabled)"
	hl_assert_false "dpms_on_monitor clears asleep" "$(hl_monitor_field "$HL_SECOND_MON" asleep)"
}

test_toggle_monitor() {
	if ! hl_ensure_second_monitor; then
		hl_skip "test_toggle_monitor: could not get a second monitor"
		return
	fi
	if [ "${HL_LIVE_REAL_MON:-0}" = "1" ]; then
		hl_skip "test_toggle_monitor: \$HL_SECOND_MON is a real monitor -- toggling it off/on risks a mode renegotiation to a different (lower) refresh rate, confirmed live 2026-07-20 on DP-1"
		return
	fi
	hl_assert_true "starts enabled" "$(hl_monitor_field "$HL_SECOND_MON" enabled)"
	hl_dispatch "toggle_monitor,$HL_SECOND_MON" 1
	hl_assert_false "toggle_monitor,\$HL_SECOND_MON disables it" "$(hl_monitor_field "$HL_SECOND_MON" enabled)"
	hl_dispatch "toggle_monitor,$HL_SECOND_MON" 1
	hl_assert_true "toggle_monitor,\$HL_SECOND_MON again re-enables it" "$(hl_monitor_field "$HL_SECOND_MON" enabled)"
}


test_focusing_another_monitor_is_announced() {
	hl_ensure_second_monitor || { hl_skip "needs a second monitor"; return 0; }
	# `active` on a monitor is `m == selmon`, and it is the only way a client
	# can learn which screen has the focus. The bar's wallpaper keybind acts on
	# that -- "change the wallpaper on the screen I am looking at" -- and it
	# kept changing the same one, because moving the pointer to the other
	# output assigned selmon directly and told nobody. focusmon() got away with
	# it by ending in focusclient(), which notifies for its own reasons.
	#
	# Driven by a CLICK on the other monitor, not by focus_monitor. The
	# dispatch was never the broken half -- it ends in focusclient(), which
	# notifies for its own reasons -- so a test using it would pass against the
	# bug it is here to catch.
	#
	# A click rather than a move, and that is a limitation worth stating: plain
	# MOTION also moves the focus (sloppyfocus), but motionnotify gates that on
	# `time`, and a virtual pointer reports time_msec 0. So the motion path
	# cannot be driven from here at all. The button path sets selmon through
	# the same helper, so this covers the notification; what it does not cover
	# is sloppy focus, which is how a person actually does it.
	# The pointer's absolute coordinates are a fraction of the whole layout's
	# bounding box, not of one output -- so with a second monitor attached the
	# extent has to be resynced or every coordinate lands at roughly half where
	# it was aimed, and the pointer never leaves the first screen.
	hl_sync_pointer_extent

	local other="$HL_SECOND_MON"
	local ox oy
	ox="$(hl_get "get all-monitors" | jq -r --arg m "$other" \
		'.monitors[] | select(.name==$m) | .x')"
	oy="$(hl_get "get all-monitors" | jq -r --arg m "$other" \
		'.monitors[] | select(.name==$m) | .y')"

	# Park on the first monitor, so the move to the second is a real change.
	hl_move 100 100; sleep 0.5
	hl_watch_start "watch all-monitors" wmon >/dev/null
	local before; before="$(hl_watch_line_count wmon)"

	hl_move $((ox + 200)) $((oy + 200)); sleep 0.3
	hl_click $((ox + 200)) $((oy + 200)); sleep 0.5
	hl_assert "clicking on the other monitor focuses it" \
		"$(hl_get "get all-monitors" | jq -r --arg m "$other" \
			'.monitors[] | select(.name==$m) | .active')" "true"
	hl_assert_true "...and a watcher was told" \
		"$(hl_wait_watch_grew wmon "$before" && echo true || echo false)"

	before="$(hl_watch_line_count wmon)"
	hl_move 100 100; sleep 0.3
	hl_click 100 100; sleep 0.5
	hl_assert_true "...and told again on the way back" \
		"$(hl_wait_watch_grew wmon "$before" && echo true || echo false)"
}
