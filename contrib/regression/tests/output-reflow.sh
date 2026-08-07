# output-reflow.sh — changing an output's scale keeps the layout coherent.
#
# `x` is an absolute logical coordinate in monitors.kdl; an output's logical
# width is its mode divided by its scale. Those two do not survive each other.
# A 3840-wide panel is 2194 logical pixels at scale 1.75 and 2560 at 1.5, so an
# output sitting flush to its right belongs at a different x in each case --
# and before output_reflow() nothing revisited it. Changing the scale one way
# left the neighbour overlapping by 366 columns, the other way left 366 columns
# of dead gap, and every scale produced a different wrong number.
#
# The overlap is the half that shows: two outputs owning the same pixel is not
# a state anything downstream can render. Reported live as a doubled wallpaper
# with a seam down it, two bars drawing into the same strip, and blur artefacts
# where one output's cache sampled the other's contents.
#
# What is asserted is ADJACENCY, in both directions, plus the deliberate-gap
# case -- because the fix must not be "repack everything left to right", which
# would also make these first two pass while flattening an arrangement someone
# chose.
#
# harness: needs-second-monitor -- run.sh sorts this after every single-monitor
# module; the output it creates outlives it.

_orf_second=""

# Both outputs PINNED, with the one under test on the left.
#
# Two harness details make this necessary, and skipping either makes the whole
# module pass on the broken build. A headless output is added to the layout
# AUTOMATICALLY, and wlroots repacks an auto-arranged output on its own -- so a
# second monitor left as it was created moves correctly with no help from us.
# And set_output_position early-returns when the output is already at the
# coordinate asked for, so "pin it where it already is" pins nothing.
#
# Hence: place the second one first (which pushes the auto-arranged first one
# out of the way, giving it a coordinate to be moved OFF), then place the first
# one at 0,0 -- now a real move, so it becomes a fixed position too.
_orf_layout() { # _orf_layout <gap>   -> left at 0, right at width+gap, both fixed
	if [ "$(hl_get "get all-monitors" | jq '.monitors | length')" -lt 2 ]; then
		hl_dispatch "create_virtual_output" 1
	fi
	_orf_second="$(hl_get "get all-monitors" \
		| jq -r --arg m "$HL_MON" '[.monitors[] | select(.name != $m)][0].name // empty')"
	[ -n "$_orf_second" ] || return 1

	hl_dispatch "set_output_scale,$HL_MON,1" 1
	local w
	w="$(hl_get "get all-monitors" | jq -r --arg m "$HL_MON" \
		'.monitors[] | select(.name==$m) | .width')"
	# BOTH via a detour, so the final call is a real move in each case. Asking
	# for the coordinate an output already sits at is the one request
	# set_output_position refuses, and an output left auto-arranged is repacked
	# by wlroots itself -- which is what made these tests pass on the build that
	# has the bug, once per output until each was pinned for real.
	hl_dispatch "set_output_position,$_orf_second,$((w + $1 + 7)),0" 1
	hl_dispatch "set_output_position,$_orf_second,$((w + $1)),0" 1
	hl_dispatch "set_output_position,$HL_MON,1,0" 1
	hl_dispatch "set_output_position,$HL_MON,0,0" 1
}

_orf_box() { # _orf_box <name> -> "x width"
	hl_get "get all-monitors" \
		| jq -r --arg m "$1" '.monitors[] | select(.name==$m) | "\(.x) \(.width)"'
}

# The signed distance from the left output's right edge to the right output's
# left edge: 0 is flush, negative is overlap, positive is a gap.
_orf_seam() {
	local ax aw bx bw
	read -r ax aw <<< "$(_orf_box "$HL_MON")"
	read -r bx bw <<< "$(_orf_box "$_orf_second")"
	echo $(( bx - (ax + aw) ))
}

_orf_scale() { hl_dispatch "set_output_scale,$HL_MON,$1" 1; }

test_a_scale_change_keeps_neighbouring_outputs_flush() {
	if ! _orf_layout 0; then
		echo "  ..   skipped: no second output"
		return 0
	fi
	hl_assert_eq "the two outputs start flush" "$(_orf_seam)" "0"

	# Scaling UP makes the left output logically NARROWER, so the right one
	# has to come in with it. Before the fix it stayed put and left a gap.
	_orf_scale 1.5
	hl_assert_eq "still flush after scaling up (1 -> 1.5)" "$(_orf_seam)" "0"

	# ...and back down: the left output widens, so the right one has to move
	# out. Before the fix it stayed put and the two OVERLAPPED.
	_orf_scale 1
	hl_assert_eq "still flush after scaling back down (1.5 -> 1)" "$(_orf_seam)" "0"
}

test_a_scale_change_preserves_a_deliberate_gap() {
	if ! _orf_layout 300; then
		echo "  ..   skipped: no second output"
		return 0
	fi
	hl_assert_eq "a 300px gap is set up" "$(_orf_seam)" "300"

	# The relationship travels with the edge; the position is not recomputed
	# from scratch. A reflow that repacked the layout would close this to 0 and
	# still pass every other assertion in this file.
	_orf_scale 1.5
	hl_assert_eq "the gap survives a scale change" "$(_orf_seam)" "300"
}

test_outputs_never_overlap_after_a_scale_change() {
	if ! _orf_layout -400; then
		echo "  ..   skipped: no second output"
		return 0
	fi
	# The broken state forced directly: the second output parked part-way
	# inside the first, the way a stale `x` in a hand-edited monitors.kdl
	# leaves it. Whatever else the reflow decides, it may not leave two outputs
	# claiming the same pixels.
	hl_assert_true "the outputs really do overlap to begin with" \
		"$([ "$(_orf_seam)" -lt 0 ] && echo true || echo false)"

	# Scale DOWN, which makes the left output WIDER and drives it further into
	# its neighbour. Scaling up would have narrowed it out of the overlap on
	# its own and passed without any backstop at all.
	_orf_scale 0.5
	hl_assert_true "...and do not after a scale change (seam $(_orf_seam))" \
		"$([ "$(_orf_seam)" -ge 0 ] && echo true || echo false)"
}
