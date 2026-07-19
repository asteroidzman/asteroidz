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
# What IS worth pinning here: the graceful-refusal behavior itself (a
# headless output asked for HDR ends up NOT reporting hdr_enabled, rather
# than silently claiming success or crashing), and set_sdr_luminance
# (a plain float value, no capability dependency at all).

hl_mon_field() { hl_get "get all-monitors" | jq -r ".monitors[0].$1"; }

test_toggle_hdr_is_refused_gracefully_on_a_headless_output() {
	hl_assert_false "starts without HDR capability" "$(hl_mon_field hdr_capable)"
	hl_assert_false "starts with hdr_enabled false" "$(hl_mon_field hdr_enabled)"
	hl_dispatch "toggle_hdr" 1.5
	hl_assert_false "toggle_hdr on a non-HDR-capable output leaves hdr_enabled false" \
		"$(hl_mon_field hdr_enabled)"
	hl_assert_false "...and doesn't claim the committed hdr state either" "$(hl_mon_field hdr)"
}

test_set_sdr_luminance_absolute() {
	hl_dispatch "set_sdr_luminance,300" 0.3
	hl_assert_eq "set_sdr_luminance,300 sets it to exactly 300" "$(hl_mon_field sdr_luminance)" "300"
}

test_set_sdr_luminance_relative() {
	hl_dispatch "set_sdr_luminance,300" 0.3
	hl_dispatch "set_sdr_luminance,+50" 0.3
	hl_assert_eq "set_sdr_luminance,+50 adds to the current value" "$(hl_mon_field sdr_luminance)" "350"
	hl_dispatch "set_sdr_luminance,-100" 0.3
	hl_assert_eq "set_sdr_luminance,-100 subtracts from the current value" "$(hl_mon_field sdr_luminance)" "250"
}

test_set_sdr_luminance_is_clamped() {
	hl_dispatch "set_sdr_luminance,50" 0.3
	hl_assert_eq "set_sdr_luminance clamps below 80 up to 80" "$(hl_mon_field sdr_luminance)" "80"
	hl_dispatch "set_sdr_luminance,5000" 0.3
	hl_assert_eq "set_sdr_luminance clamps above 1000 down to 1000" "$(hl_mon_field sdr_luminance)" "1000"
}
