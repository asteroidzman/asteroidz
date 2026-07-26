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

# ─── the bar's monitor arrange canvas ─────────────────────────────────────
#
# This is a BAR test living in the multi-monitor module, on purpose. It needs
# a second output, and creating one is not something a test can undo here:
# destroy_all_virtual_output destroys every headless output, which under a
# pure headless backend is all of them including HEADLESS-1 (see the note at
# the top of this file). So the output it creates outlives it, and a leftover
# second monitor changes what a pointer coordinate means -- wlvptr maps its
# absolute axis onto the whole layout -- and displaces .monitors[0] for every
# module that reads it. Run early, that broke four unrelated assertions in
# mousebind and keybind-combo. Run here, everything that assumes one monitor
# has already been and gone.

test_bar_arrange_canvas_drags_a_monitor() {
	declare -F bar_set >/dev/null || {
		echo "  (skip: needs the bar module's config helpers -- run with 'bar' too)"
		return 0; }
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	if [ "$(hl_get "get all-monitors" | jq '.monitors | length')" -lt 2 ]; then
		hl_dispatch "create_virtual_output" 1
		sleep 1
	fi
	[ "$(hl_get "get all-monitors" | jq '.monitors | length')" -ge 2 ] || {
		echo "  (skip: could not get a second monitor)"; return 0; }
	local second
	second="$(hl_get "get all-monitors" | jq -r --arg m "$HL_MON" \
		'[.monitors[] | select(.name != $m) | .name][0]')"

	# The canvas is the only part of the bar that is not a list of rows, and
	# the only one driven by a press-move-release rather than a click. Both
	# halves of that are worth pinning: the drag has to end up applying a real
	# layout change, and dismissing the panel afterwards has to not crash --
	# the first version double-freed every tile, because the popover's teardown
	# released them AFTER destroying the tree that owned them.
	#
	# wlvptr maps its absolute axis onto the whole LAYOUT, so a second monitor
	# changes what a coordinate means. Set for the test, restored before it
	# returns, or every later test clicks in the wrong place.
	local saved_w="$HL_PTR_EXTENT_W" saved_h="$HL_PTR_EXTENT_H"
	HL_PTR_EXTENT_W="$(hl_get "get all-monitors" | jq '[.monitors[] | .x + .width] | max')"
	HL_PTR_EXTENT_H="$(hl_get "get all-monitors" | jq '[.monitors[] | .y + .height] | max')"
	local mx oy
	mx="$(hl_get "get all-monitors" | jq --arg m "$HL_MON" '[.monitors[] | select(.name==$m) | .x][0]')"
	oy="$(hl_get "get all-monitors" | jq --arg m "$HL_MON" '[.monitors[] | select(.name==$m) | .y][0]')"

	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; panel { enable false }; show-logo false; tag-icons 0; modules-left "tags"; modules-right "display" }'
	sleep 1

	grim -o "$HL_MON" "$HL_OUTDIR/arr0.png" 2>/dev/null
	local dx; dx="$(hl_rightmost_ink_x "$HL_OUTDIR/arr0.png" 6 32)"
	[ -n "$dx" ] || { echo "  (skip: could not locate the display pill)"
		HL_PTR_EXTENT_W="$saved_w"; HL_PTR_EXTENT_H="$saved_h"; bar_off; return 0; }
	hl_click "$((mx + dx))" 19
	sleep 0.8

	# outputs popover: one row per monitor, then "Arrange displays". Rows start
	# at margin(4) + height(30) + popover gap(6) + panel padding(6) = 46.
	local nmon; nmon="$(hl_get "get all-monitors" | jq '.monitors | length')"
	local arrange_y=$((46 + nmon * 36 + 17))
	hl_click "$((mx + HL_WIDTH - 180))" "$arrange_y"
	sleep 0.8

	grim -o "$HL_MON" "$HL_OUTDIR/arr1.png" 2>/dev/null
	local t1 t2 ty
	read -r t1 t2 ty <<<"$(hl_canvas_tiles "$HL_OUTDIR/arr1.png" 46 182)"
	[ -n "${t1:-}" ] || { echo "  (skip: no canvas tiles found -- popover may not have opened)"
		hl_click "$((mx + 400))" 600
		HL_PTR_EXTENT_W="$saved_w"; HL_PTR_EXTENT_H="$saved_h"; bar_off; return 0; }
	hl_assert_true "the arrange canvas draws a tile per monitor ($t1, $t2)" \
		"$([ "$t1" -ne "$t2" ] && echo true || echo false)"

	local before; before="$(hl_get "get all-monitors" | jq -c '[.monitors[] | {name, x}] | sort_by(.name)')"
	"$HL_WLVPTR" "$((mx + t1))" "$ty" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H" \
		"drag:$((mx + t2 + 120)),$ty"
	sleep 1.2
	local after; after="$(hl_get "get all-monitors" | jq -c '[.monitors[] | {name, x}] | sort_by(.name)')"
	hl_assert_true "dragging a tile moves the output it stands for" \
		"$([ "$before" != "$after" ] && echo true || echo false)"

	# Save it. This writes to a real config file, so the assertion is on the
	# file: the harness config carries an `output HEADLESS-1 { ... }` block,
	# and saving must put the live position into THAT block rather than
	# inventing one somewhere else. Rows begin below the canvas -- 46 + four
	# 34px rows + spacing -- and "Save arrangement" is the second of them,
	# after the hint line.
	local newx
	newx="$(hl_get "get all-monitors" | jq --arg m "$HL_MON" '[.monitors[] | select(.name==$m) | .x][0]')"
	hl_click "$((mx + HL_WIDTH - 180))" $((46 + 138 + 36 + 17))
	sleep 0.8
	hl_assert_true "Save arrangement writes the position into the config ($HL_MON x=$newx)" \
		"$(grep -qE "output[[:space:]]+$HL_MON[^\n]*x[[:space:]]+$newx" "$HL_CONFIG" && echo true || echo false)"

	# the double-free regression: dismiss the panel the drag happened in
	hl_click "$((mx + 400))" 600
	sleep 0.6
	hl_assert_true "dismissing the arrange popover leaves the compositor alive" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"

	# PUT IT BACK. hl_reset does NOT remove the virtual output between tests but has
	# no idea the shared monitor was moved, and every later test computes its
	# click coordinates from an origin this would have silently shifted --
	# which is exactly how this first ran: four unrelated mousebind and
	# keybind assertions failed because their clicks were landing off-screen.
	sed -i "s|^output $HL_MON {.*|output $HL_MON { width $HL_WIDTH; height $HL_HEIGHT; refresh 60; x $mx; y $oy }|" "$HL_CONFIG"
	hl_dispatch "reload_config" 1
	sleep 0.5
	local restored
	restored="$(hl_get "get all-monitors" | jq --arg m "$HL_MON" '[.monitors[] | select(.name==$m) | .x][0]')"
	hl_assert_eq "the monitor is put back where the suite expects it" \
		"$restored" "$mx"

	# And take the second monitor back out of the layout. hl_reset does not
	# (destroy_all_virtual_output would take HEADLESS-1 with it under a pure
	# headless backend), and a leftover second output changes what a pointer
	# coordinate MEANS for every module that runs after this one -- wlvptr
	# maps its absolute axis onto the whole layout.
	hl_dispatch "disable_monitor,$second" 0.5

	HL_PTR_EXTENT_W="$saved_w"
	HL_PTR_EXTENT_H="$saved_h"
	bar_off
}
