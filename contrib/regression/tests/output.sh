
test_output_dispatches_configure_an_output() {
	command -v jq >/dev/null || { echo "  (skip: jq not available)"; return 0; }

	# The native bar's display popover could change a mode, a scale or a
	# position because it ran INSIDE the compositor. A bar in another process
	# cannot, so those apply paths became dispatches -- same test-then-commit,
	# same retrain safety net, reachable from anything.
	local name; name="$(hl_get "get all-monitors" | jq -r '.monitors[0].name')"

	hl_dispatch "set_output_scale,$name,1.5" 1
	hl_assert_eq "a scale is applied" "1.5" \
		"$(hl_get "get all-monitors" | jq -r '.monitors[0].scale')"
	# and it is the LOGICAL size that changes, which is the part a client sees
	hl_assert_eq "and the logical width follows it" "1280" \
		"$(hl_get "get all-monitors" | jq -r '.monitors[0].width')"

	hl_dispatch "set_output_position,$name,300,200" 1
	hl_assert_eq "a position is applied" "300,200" \
		"$(hl_get "get all-monitors" | jq -r '.monitors[0] | "\(.x),\(.y)"')"

	# Refusals matter more than applications here: a picker that can black out
	# a display is worse than one that cannot pick.
	hl_dispatch "set_output_scale,$name,0" 1
	hl_assert_eq "a zero scale is refused, leaving the output alone" "1.5" \
		"$(hl_get "get all-monitors" | jq -r '.monitors[0].scale')"

	hl_dispatch "set_output_mode,$name,999x999@1" 1
	hl_assert_eq "a mode the output does not have is refused" "1280" \
		"$(hl_get "get all-monitors" | jq -r '.monitors[0].width')"

	hl_dispatch "set_output_scale,$name,1" 1
	hl_dispatch "set_output_position,$name,0,0" 1
}

test_output_settings_are_written_back_to_the_config() {
	command -v jq >/dev/null || { echo "  (skip: jq not available)"; return 0; }

	# Applying a setting and REMEMBERING it are two different features, and
	# only the first one existed: every set_output_* took effect immediately
	# and was gone at the next reload. The panel driving them looked like it
	# was configuring a display and was really only nudging it.
	# $HL_MON, not monitors[0]: earlier modules create and destroy virtual
	# outputs, so "the first monitor" is whichever one happens to be first by
	# the time this runs -- and a virtual output has no block in the config to
	# write into. Passed alone this test went green and only failed in a full
	# run, which is the worst way for a test to be wrong.
	local name="$HL_MON"

	# The harness already declares this output, so the block is EDITED rather
	# than a second one appended -- two blocks for one output is a different
	# test (the last one wins on reload) and it silently hid whether the write
	# had landed. A comment goes in beside it: the edit is textual precisely so
	# a hand-maintained file survives being written to, and a comment is the
	# first thing a regenerating writer would destroy.
	sed -i "s|^output $name .*|// kept by hand: this comment must survive every write below\noutput $name { width $HL_WIDTH; height $HL_HEIGHT; refresh 60; scale 1; x 0; y 0; }|" \
		"$HL_CONFIG"
	hl_dispatch "reload_config" 1

	hl_dispatch "set_output_scale,$name,1.5" 1
	hl_assert_true "a scale is written back to the config" \
		"$(grep -q "scale 1.5" "$HL_CONFIG" && echo true || echo false)"

	hl_dispatch "set_output_position,$name,300,200" 1
	hl_assert_true "a position is written back to the config" \
		"$(grep -q "x 300" "$HL_CONFIG" && grep -q "y 200" "$HL_CONFIG" &&
		   echo true || echo false)"

	# The whole reason the writer is textual rather than a KDL round-trip.
	hl_assert_true "the hand-written comment survives being written around" \
		"$(grep -q "kept by hand" "$HL_CONFIG" && echo true || echo false)"

	# A key the block never had has to be APPENDED, and appended in a way that
	# still parses -- a bare append after an unterminated entry turns it into
	# an extra argument, which is a config that means something else.
	hl_dispatch "set_output_vrr,$name,1" 1
	hl_assert_true "a setting the block never had is appended" \
		"$(grep -q "vrr 1" "$HL_CONFIG" && echo true || echo false)"

	# ...and the proof that it still parses is that the compositor can read it
	# back without complaint and is still answering afterwards.
	hl_dispatch "reload_config" 1
	hl_assert_true "the rewritten config still loads" \
		"$([ -n "$(hl_get "get all-monitors")" ] && echo true || echo false)"
	hl_assert_eq "and the scale it wrote is the scale it reloads" "1.5" \
		"$(hl_get "get all-monitors" \
		   | jq -r --arg n "$name" '.monitors[] | select(.name==$n) | .scale')"
}

test_set_output_hdr_writes_the_baseline_not_the_resolved_state() {
	command -v jq >/dev/null || { echo "  (skip: jq not available)"; return 0; }

	# set_output_hdr writes hdr_configured -- the INPUT to hdr_resolve -- and
	# persists it. It must NOT write m->hdr, which is derived: assigning that
	# was the old toggle_hdr bug, and the next resolve pass threw it away.
	#
	# A headless output is not HDR-capable, so the resolver will refuse to
	# actually turn HDR on here. That is the interesting half: the baseline
	# still has to be REMEMBERED, so it takes effect on hardware that can.
	local name="$HL_MON"

	sed -i "s|^output $name .*|output $name { width $HL_WIDTH; height $HL_HEIGHT; refresh 60; }|" \
		"$HL_CONFIG"
	hl_dispatch "reload_config" 1

	hl_dispatch "set_output_hdr,$name,1" 1
	hl_assert_true "an HDR baseline is written to the config" \
		"$(grep -q "hdr 1" "$HL_CONFIG" && echo true || echo false)"

	# Written as `hdr 1`, never a bare `hdr`: the parser reads it with
	# atoi(val), so the value form is the one that round-trips.
	hl_assert_true "it is written as a value, not a bare flag" \
		"$(grep -qE "hdr 1" "$HL_CONFIG" && echo true || echo false)"

	hl_dispatch "reload_config" 1
	hl_assert_true "the compositor is still healthy with the baseline set" \
		"$([ -n "$(hl_get "get all-monitors")" ] && echo true || echo false)"

	# ...and turning it off REMOVES the key rather than writing `hdr 0`: absent
	# is the default, and an explicit 0 is a second way to say the same thing.
	hl_dispatch "set_output_hdr,$name,0" 1
	hl_assert_true "turning it off removes the key" \
		"$(grep -q "hdr 1" "$HL_CONFIG" && echo false || echo true)"
}

test_an_output_with_no_block_is_applied_but_not_saved() {
	command -v jq >/dev/null || { echo "  (skip: jq not available)"; return 0; }

	# Inventing a block means choosing a file and a place in it, and guessing
	# that about a hand-maintained config is worse than leaving it alone. What
	# must NOT happen is a corrupted file or a refused dispatch.
	local before; before="$(wc -c < "$HL_CONFIG")"

	hl_dispatch "set_output_scale,NOSUCH-9,1.25" 1
	hl_assert_eq "a dispatch for an unknown output leaves the config alone" \
		"$before" "$(wc -c < "$HL_CONFIG")"
	hl_assert_true "and the compositor is still answering" \
		"$([ -n "$(hl_get "get all-monitors")" ] && echo true || echo false)"
}

# The presenter derives its timing model -- nominal_period_ns, and the VRR-or-
# fixed regime -- from the output, ONCE, when its epoch is reset. Anything that
# reconfigures the output without ending that epoch leaves the predictor pacing
# against a display that no longer exists.
#
# The wlr-output-management handler (wlr-randr, DMS) always reset. The
# compositor's OWN dispatches did not, in two separate places: set_output_mode /
# set_output_scale through output_apply_change(), and the adaptive-sync toggle
# in commit_vrr_state(). Both were found the same night, from a stale
# present_regime that reported "vrr" while the hardware reported adaptive sync
# disabled.
#
# Written against the defect: on the build before the fix, epoch stays 1 here.
test_a_scale_change_ends_the_presenter_timing_epoch() {
	command -v jq >/dev/null || { echo "  (skip: jq not available)"; return 0; }
	local name="$HL_MON" was e0 e1 m0 m1

	_epoch() { hl_get "get surface-intent" \
		| jq -r ".outputs[] | select(.name==\"$name\") | .epoch // 0"; }
	# `// 0` because a reason absent from the histogram has never fired, which
	# is not the same as the field being missing and must not read as null.
	_mode_resets() { hl_get "get surface-intent" \
		| jq -r ".outputs[] | select(.name==\"$name\") | .resets.mode // 0"; }

	was="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$name\") | .scale")"
	e0="$(_epoch)"; m0="$(_mode_resets)"

	# Scale rather than mode: it reaches the same output_apply_change() and a
	# headless output has no mode list for set_output_mode to pick from.
	hl_dispatch "set_output_scale,$name,2" 1
	e1="$(_epoch)"; m1="$(_mode_resets)"

	hl_assert_true "a scale change opens a new presenter epoch" \
		"$([ "${e1:-0}" -gt "${e0:-0}" ] && echo true || echo false)"
	hl_assert_true "and is counted against the 'mode' reset reason" \
		"$([ "${m1:-0}" -gt "${m0:-0}" ] && echo true || echo false)"

	hl_dispatch "set_output_scale,$name,$was" 1
}

# The instrument itself: three counters added the same night, all of which
# answer "how often did this change" rather than "what is it now". A dump that
# cannot express the difference is what let an intermittent blank go
# unattributed for a whole session.
test_the_output_dump_carries_its_change_counters() {
	command -v jq >/dev/null || { echo "  (skip: jq not available)"; return 0; }
	local o
	o="$(hl_get "get surface-intent" | jq -r ".outputs[] | select(.name==\"$HL_MON\")")"

	hl_assert_true "outputs[] carries scanout_changes" \
		"$(echo "$o" | jq -r 'has("scanout_changes")')"
	hl_assert_true "outputs[] carries hdr_state_commits" \
		"$(echo "$o" | jq -r 'has("hdr_state_commits")')"
	hl_assert_true "outputs[] carries the reset histogram" \
		"$(echo "$o" | jq -r 'has("resets")')"
	# create is raised when the output's presenter is first built, so any
	# running output has at least one. A histogram that is merely present and
	# always empty would pass the check above and mean nothing.
	hl_assert_true "and the histogram has recorded the output's own creation" \
		"$(echo "$o" | jq -r '(.resets.create // 0) > 0')"
}

# ── THE VRR OFF-DEBOUNCE, AS FAR AS A HEADLESS OUTPUT CAN SEE IT ──────────
#
# WHAT THIS CANNOT TEST, SAID PLAINLY. The debounce only engages once VRR is
# actually ON: enable_adaptive_sync() sets is_vrr_opening only when
# wlr_output_test_state() accepts adaptive sync, and the headless backend does
# not -- tests/vrr.sh asserts both the client and monitor fields read false
# here. So the held answer, the cancel and the timer are NOT exercised by this
# suite and have to be verified on real hardware. The counters below are how
# that is done, because they are the difference between "no modesets happened"
# and "no modesets were needed".
#
# What this does test is that the counters exist and that an output which never
# had VRR on has never deferred turning it off -- the half that disappears
# silently in a refactor.
test_the_output_dump_carries_the_vrr_debounce_counters() {
	command -v jq >/dev/null || { echo "  (skip: jq not available)"; return 0; }
	local o
	o="$(hl_get "get surface-intent" | jq -r ".outputs[] | select(.name==\"$HL_MON\")")"

	hl_assert_true "outputs[] carries vrr_off_deferred" \
		"$(echo "$o" | jq -r 'has("vrr_off_deferred")')"
	hl_assert_true "outputs[] carries vrr_off_cancelled" \
		"$(echo "$o" | jq -r 'has("vrr_off_cancelled")')"
	# An output that never had VRR on can never have held an answer about
	# turning it off. Zero here is a fact, not a default.
	hl_assert "and nothing was deferred on an output that never had VRR" \
		"$(echo "$o" | jq -r '.vrr_off_deferred')" "0"
}
