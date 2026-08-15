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
# per-window force_hdr rule can't have their EFFECT asserted here, for the
# same reason: both only ever change the effective hdr state on an
# HDR-capable output, and the headless backend pins that to false.
# Exercising the effect needs a real HDR display (see
# reference_hdr10_rawhdr_capture / contrib/hdr-record.sh for the live path).
#
# Their INPUT is a different matter, and it is assertable here. `hdr_configured`
# is the per-output baseline the resolver reads, as a tri-state: -1 nobody has
# spoken for this output, 0 explicitly off, 1 explicitly on. It is independent
# of whether the output can commit HDR at all, so a headless run can pin what
# was ASKED FOR even where it can never pin what happened.
#
# That distinction is not theoretical. setmon once assigned the tri-state
# straight into the effective m->hdr, so every unconfigured output ran as HDR
# (-1 is truthy) -- PQ encode, Path B, first frames refused. It reached a
# 50-fixture closure run, where it moved twelve of them, because the only
# observable was a BOOL that reported the broken value as `true`. The
# tri-state assertions below are the cheap guard the expensive one replaced.
#
# What IS worth pinning here: the graceful-refusal behavior itself (a
# headless output asked for HDR ends up NOT reporting hdr_enabled, rather
# than silently claiming success or crashing), and set_sdr_luminance
# (a plain float value, no capability dependency at all).

hl_mon_field() { hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .$1"; }

test_aaa_an_unconfigured_output_has_no_hdr_baseline() {
	# MUST RUN FIRST, hence the name -- modules run their test_* functions in
	# definition order, and -1 is a WRITE-ONCE observation. set_output_hdr
	# takes a boolean, so nothing can put an output back to "never mentioned"
	# once any test has written a baseline; the first version of this test sat
	# below the toggle test and read that test's leftover 1, failing on the
	# broken build for a reason that had nothing to do with the bug -- and it
	# would have failed identically on the fixed one.
	#
	# The premise is therefore asserted, not assumed. If a later reordering
	# puts a baseline write above this, THIS line goes red and names why,
	# rather than the assertion below going red and pointing at the compositor.
	local base; base="$(hl_mon_field hdr_configured)"
	hl_assert_eq "PREMISE: nothing has written a baseline yet" "$base" "-1"

	# THE GUARD. An output nobody configured must not come up in HDR. Under
	# the tri-state leak the baseline was still correctly -1 while the derived
	# state was truthy, so the intent looked right and only the derived value
	# was wrong -- which is exactly why both halves are pinned here.
	hl_assert_false "an unconfigured output's effective state is off, not truthy -1" \
		"$(hl_mon_field hdr_enabled)"
}

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

test_set_output_hdr_writes_an_explicit_baseline() {
	if [ "$(hl_mon_field hdr_capable)" = "true" ]; then
		hl_skip "writes a real baseline on an HDR-capable output ($HL_MON reports hdr_capable=true)"
		return
	fi
	# 0 must be distinguishable from -1: "the operator turned this off" is not
	# "the operator never mentioned it". Collapsing them is what made
	# `hdr-mode on` outrank every per-output request, so the dispatch reported
	# success while the resolver undid it in the same call.
	hl_dispatch "set_output_hdr,$HL_MON,1" 1.5
	hl_assert_eq "set_output_hdr,1 records an explicit ON baseline" \
		"$(hl_mon_field hdr_configured)" "1"
	hl_dispatch "set_output_hdr,$HL_MON,0" 1.5
	hl_assert_eq "set_output_hdr,0 records an explicit OFF, not absence" \
		"$(hl_mon_field hdr_configured)" "0"
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
