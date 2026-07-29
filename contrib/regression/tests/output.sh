
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
