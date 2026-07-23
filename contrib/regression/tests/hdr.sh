# hdr.sh — toggle_hdr, set_sdr_luminance.
#
# What this CAN'T test: genuine HDR10 rendering (real PQ encode, BT.2020
# gamut, 10-bit scanout) -- wlroots' headless backend doesn't advertise
# wlr_output->supported_primaries/supported_transfer_functions at all (no
# real display, nothing to negotiate), so mon_state_apply_color() always
# hits its "output does not support HDR (BT.2020 + PQ)" refusal and
# reverts m->hdr back to 0 on the very next frame commit, regardless of
# what toggle_hdr just set it to. Confirmed this is the SAME refusal path
# contrib/render-matrix-test.sh's own "hdr" case already expects and
# explicitly excludes from its error count (see that script's grep -cvE
# pattern) -- that harness verifies the toggle is handled gracefully
# end-to-end (screenshots, no crash), not that real HDR was achieved.
# Pixel-level HDR10 correctness needs a real HDR-capable display; nothing
# headless can exercise that.
#
# The 0.17.2 global hdr-mode policy (misc/hdr-mode off|auto|on) and the
# per-window force_hdr rule are, for the same reason, not headlessly
# assertable: both only ever change the EFFECTIVE hdr state on an
# HDR-capable output (force_hdr flips an SDR output into HDR while a
# matching client is visible; hdr-mode gates whether that's allowed at
# all), and there's no IPC getter for the policy inputs themselves --
# only the resolved monitor hdr/hdr_enabled, which the headless backend
# pins to false. Exercising them needs a real HDR display (see
# reference_hdr10_rawhdr_capture / contrib/hdr-record.sh for the live path).
#
# What IS worth pinning here: the graceful-refusal behavior itself (a
# headless output asked for HDR ends up NOT reporting hdr_enabled, rather
# than silently claiming success or crashing), and set_sdr_luminance
# (a plain float value, no capability dependency at all).

hl_mon_field() { hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .$1"; }

test_toggle_hdr_is_refused_gracefully_on_a_headless_output() {
	if [ "$(hl_mon_field hdr_capable)" = "true" ]; then
		# $HL_MON is actually HDR-capable (a real monitor in live mode) --
		# this test's whole premise (graceful refusal on a NON-capable
		# output) doesn't apply, and dispatching toggle_hdr here would
		# really flip the user's real HDR state with no restore, since the
		# test was never written to undo a toggle that's expected to be a
		# no-op. Confirmed live 2026-07-19: left a real monitor in SDR after
		# a live-mode run.
		hl_skip "toggle_hdr graceful-refusal test needs a non-HDR-capable output ($HL_MON reports hdr_capable=true)"
		return
	fi
	hl_assert_false "starts without HDR capability" "$(hl_mon_field hdr_capable)"
	hl_assert_false "starts with hdr_enabled false" "$(hl_mon_field hdr_enabled)"
	hl_dispatch "toggle_hdr" 1.5
	hl_assert_false "toggle_hdr on a non-HDR-capable output leaves hdr_enabled false" \
		"$(hl_mon_field hdr_enabled)"
	hl_assert_false "...and doesn't claim the committed hdr state either" "$(hl_mon_field hdr)"
}

test_set_sdr_luminance_absolute() {
	local orig; orig="$(hl_mon_field sdr_luminance)"
	hl_dispatch "set_sdr_luminance,300" 0.3
	hl_assert_eq "set_sdr_luminance,300 sets it to exactly 300" "$(hl_mon_field sdr_luminance)" "300"
	hl_dispatch "set_sdr_luminance,$orig" 0.3  # restore -- this is a GLOBAL setting, not per-monitor
}

test_set_sdr_luminance_relative() {
	local orig; orig="$(hl_mon_field sdr_luminance)"
	hl_dispatch "set_sdr_luminance,300" 0.3
	hl_dispatch "set_sdr_luminance,+50" 0.3
	hl_assert_eq "set_sdr_luminance,+50 adds to the current value" "$(hl_mon_field sdr_luminance)" "350"
	hl_dispatch "set_sdr_luminance,-100" 0.3
	hl_assert_eq "set_sdr_luminance,-100 subtracts from the current value" "$(hl_mon_field sdr_luminance)" "250"
	hl_dispatch "set_sdr_luminance,$orig" 0.3  # restore -- this is a GLOBAL setting, not per-monitor
}

test_set_sdr_luminance_is_clamped() {
	local orig; orig="$(hl_mon_field sdr_luminance)"
	hl_dispatch "set_sdr_luminance,50" 0.3
	hl_assert_eq "set_sdr_luminance clamps below 80 up to 80" "$(hl_mon_field sdr_luminance)" "80"
	hl_dispatch "set_sdr_luminance,5000" 0.3
	hl_assert_eq "set_sdr_luminance clamps above 1000 down to 1000" "$(hl_mon_field sdr_luminance)" "1000"
	hl_dispatch "set_sdr_luminance,$orig" 0.3  # restore -- this is a GLOBAL setting, not per-monitor
}
