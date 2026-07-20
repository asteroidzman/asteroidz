# overview.sh — toggle_overview + jump mode.
#
# There's no direct "is overview open" IPC field, but monitor_active_tags()
# reports active_tags as the sentinel [0] while isoverview is true (real
# tags are 1-based, so 0 can't appear otherwise) -- reliable enough to
# detect enter/exit.
#
# toggleoverview() refuses to actually enter overview with zero visible
# clients on the monitor (by design -- nothing to preview, it flips
# isoverview back off and returns early) -- every test here spawns a
# window first, or toggle_overview silently no-ops and every assertion
# below reads as "stuck out of overview" for the wrong reason.
#
# ov_tab_mode defaults ON (config.ov_tab_mode = 1): while ALREADY in
# overview, a plain toggle_overview (arg.i defaults to 0) just cycles focus
# instead of closing it (see toggleoverview()'s early-return branch) --
# dispatch toggle_overview,1 (arg.i==1) to actually force it closed
# regardless of tab-mode. Bit us during harness development: two toggles in
# a row with no arg looked like overview was stuck open.

hl_active_tags() { hl_get "get all-monitors" | jq -c ".monitors[] | select(.name==\"$HL_MON\") | .active_tags"; }

test_toggle_overview_enters_and_exits() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_assert_eq "starts out of overview" "$(hl_active_tags)" "[1]"
	hl_dispatch "toggle_overview" 1.5
	hl_assert_eq "toggle_overview enters overview (active_tags sentinel [0])" "$(hl_active_tags)" "[0]"
	hl_dispatch "toggle_overview,1" 1.5
	hl_assert_eq "toggle_overview,1 forces it closed regardless of ov_tab_mode" "$(hl_active_tags)" "[1]"
}

test_toggle_overview_jump_also_enters_overview() {
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "toggle_overview,jump" 1.5
	hl_assert_eq "toggle_overview,jump also reports the overview sentinel" "$(hl_active_tags)" "[0]"
	hl_dispatch "toggle_overview,1" 1.5
	hl_assert_eq "toggle_overview,1 closes jump mode too" "$(hl_active_tags)" "[1]"
}

test_toggle_overview_refuses_with_no_clients() {
	# hl_reset only kills THIS harness's own spawned windows -- in live mode
	# (HL_LIVE_MON) the user's own real windows are still on the monitor,
	# so the "zero clients" precondition this test needs doesn't hold.
	# Dispatching toggle_overview anyway would genuinely ENTER overview
	# (toggleoverview() only refuses when there are truly zero visible
	# clients) with no forced-close call anywhere after it in this test --
	# leaving the live session stuck in overview for the rest of the
	# suite, un-recoverable except by the user manually pressing Escape.
	# Confirmed live 2026-07-20. Skip instead of assuming zero clients.
	local count
	count="$(hl_get "get all-clients" | jq '.clients | length')"
	if [ "$count" != "0" ]; then
		hl_skip "test_toggle_overview_refuses_with_no_clients: $count client(s) still present (real windows in live mode?), dispatching toggle_overview here would actually enter overview with no way in this test to close it again"
		return
	fi
	hl_assert_eq "no clients present (hl_reset already cleared them)" "$count" "0"
	hl_dispatch "toggle_overview" 1
	hl_assert_eq "toggle_overview is a no-op with zero visible clients on the monitor" \
		"$(hl_active_tags)" "[1]"
}
