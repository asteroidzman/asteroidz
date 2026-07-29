
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
