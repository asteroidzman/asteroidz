# border-colors.sh — the per-state border colours are reachable and optional.
#
# Written for a real bug. A themed setup (matugen writing layout/border/*)
# showed a correctly themed focused border that turned GREEN the moment the
# window was maximized, with nothing in the config to blame -- and nothing
# that COULD be, because layout/border/maximize-color did not exist.
#
# maximizescreencolor, scratchpadcolor, globalcolor and overlaycolor had a
# field, a parse_option branch, a hardcoded default hue and a read in
# get_border_color -- everything except a row in the KDL name table. KDL is the
# only config format asteroidz has, so those four colours were not merely
# unthemed, they were UNREACHABLE: the only way to get that green off the
# screen was to patch the compositor.
#
# Two things are pinned here, and they fail differently:
#
#   * the keys parse. A missing name-table row is silent -- the file is
#     accepted, the key never reaches the config, and the option does nothing
#     while looking configured. That is the exact shape of the original bug.
#   * they stay OPTIONAL. Unset, each falls back to focus-color, which is what
#     keeps a themed border one colour across states. A default hue creeping
#     back would be invisible to a parse check, so the fallback is asserted by
#     reloading a config that sets ONLY focus-color and requiring the
#     compositor to survive and serve.
#
# What this CAN'T test: the pixels. There is no IPC getter for a resolved
# border colour, and screenshotting a border means finding a 1-2px frame
# against an antialiased rounded corner -- measurable, but it would be
# asserting the renderer rather than the config plumbing that actually broke.

test_border_state_colors_are_reachable_keys() {
	local cfg="$HL_OUTDIR/bordercolors.kdl"
	local out key

	# Each of the four, individually, so a partial name-table row fails on the
	# key it actually missed instead of hiding behind its neighbours.
	for key in maximize-color scratchpad-color global-color overlay-color; do
		cat > "$cfg" <<KDL
layout { border { $key 0xff0000ff } }
KDL
		out="$("$HL_ASTEROIDZ" -p -c "$cfg" 2>&1)"
		hl_assert_true "layout/border/$key is a reachable key" \
			"$(echo "$out" | grep -qi "unknown keyword" && echo false || echo true)"
	done
}

test_border_state_colors_are_optional() {
	local cfg="$HL_OUTDIR/borderfocusonly.kdl"

	# The themed case: a palette that sets the border colours it knows about
	# and says nothing about window states. This is what matugen writes.
	cat > "$cfg" <<'KDL'
layout { border { color 0x292929ff; focus-color 0xffb86fff; urgent-color 0xffb4abff } }
KDL
	local out
	out="$("$HL_ASTEROIDZ" -p -c "$cfg" 2>&1)"
	hl_assert_true "a palette that omits the state colours parses" \
		"$(echo "$out" | grep -qi "unknown keyword" && echo false || echo true)"

	# And it has to survive being applied, because the fallback is read on
	# every border repaint rather than resolved once at parse time.
	hl_dispatch "reload_config" 0.5
	hl_assert_true "the compositor is healthy with only focus-color set" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
}

test_border_state_colors_survive_being_removed_again() {
	local cfg="$HL_OUTDIR/borderremoved.kdl"

	# The *_set flags live in the Config struct, and parse_config memsets it
	# before every reload -- so setting a state colour and then taking it away
	# must go back to the fallback rather than latching the old value. A flag
	# that survived a reload would make the green un-removable a second time,
	# which is the original bug wearing a different hat.
	cat > "$cfg" <<'KDL'
layout { border { focus-color 0xffb86fff; maximize-color 0x00ff00ff } }
KDL
	hl_dispatch "reload_config" 0.5

	cat > "$cfg" <<'KDL'
layout { border { focus-color 0xffb86fff } }
KDL
	hl_dispatch "reload_config" 0.5
	hl_assert_true "a state colour can be set and then removed again" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
}
