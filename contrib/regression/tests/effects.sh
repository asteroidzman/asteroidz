# effects.sh — the effects{} config block: blend space.
#
# What this CAN'T test: the pixels. blend-space only changes anything on the
# Vulkan renderer, and this harness pins WLR_RENDERER=gles2 (see
# contrib/lib/headless.sh) because that's the one renderer available on every
# machine that runs the suite -- on GLES the setting is a deliberate no-op, so
# a screenshot here would be identical either way and would assert nothing.
# The measured linear-vs-srgb values are in docs/visuals/effects.md; they came
# from a Vulkan run over a fixed backdrop, which is the only way to get them.
#
# What IS worth pinning: the wiring. A render setting has a field, a
# parse_option branch and a row in the KDL name table, and those drift apart
# silently -- a missing table row means the file is accepted, the key never
# reaches the config, and the option does nothing while looking configured.
# That's the failure that reads as a bug in the renderer rather than in the
# config plumbing. Plus: flipping it at runtime goes through reset_blur_params
# on every reload, so a reload must survive it.

test_effects_blend_space_is_reachable_and_reloadable() {
	local cfg="$HL_OUTDIR/blendspace.kdl"
	local out

	# Only the KEY can be checked this way -- `-p` reports unknown keywords, not
	# unknown values, and there's no IPC getter for the resolved blend space, so
	# "srgb" vs "linear" is indistinguishable from here. Both are run anyway so
	# a future value-level check has somewhere obvious to hang.
	for space in linear srgb; do
		cat > "$cfg" <<KDL
effects { blend-space "$space" }
KDL
		out="$("$HL_ASTEROIDZ" -p -c "$cfg" 2>&1)"
		hl_assert_true "effects/blend-space \"$space\" is a reachable key" \
			"$(echo "$out" | grep -qi "unknown keyword" && echo false || echo true)"
	done

	# The setter is re-pushed from reset_blur_params, i.e. on every reload, so
	# that the space can be flipped and judged live. Reloading is the path a
	# user actually takes to compare the two, and it must not take the
	# compositor with it.
	hl_dispatch "reload_config" 0.5
	hl_dispatch "reload_config" 0.5
	hl_assert_true "the compositor is healthy after reloads with a blend space set" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
}

# ── shadows_blur_background: NOT TESTED, and why ────────────────────────────
#
# There was an assertion here for one day. It rendered a floating window on a
# BLACK wallpaper and required that nothing outside it be lit, which caught the
# live-blur artefact -- a shadow's footprint hugs its window, a live blur
# samples the framebuffer, so it picks up the window's own pixels and smears
# them outward: 0, 5, 12, 20 approaching the edge instead of black, in the
# window's own colour.
#
# It passed against the change that made the feature WORSE. Forcing the cached
# wallpaper snapshot for floating windows removes that rim, and this test's
# arrangement -- one window, over bare wallpaper -- is the single case where
# the snapshot IS what is beneath. On a real desktop a floating window sits
# over another window, and the snapshot then paints the blurred wallpaper over
# whatever is really there: over a dark window on a bright wallpaper, a broad
# bright haze. Reported within minutes of installing it.
#
# So the test asserted the configuration the fix had just been measured in,
# which is no test at all. What a real one has to require is both at once:
#
#   1. on a black wallpaper, no light outside the window   (rules out live)
#   2. over a DARK window on a BRIGHT wallpaper, the shadow band is darker
#      than that window                                    (rules out cached)
#
# Neither path satisfies both, so writing it now would only add a red line to
# every run. The fix belongs in the sampling -- the blur wants what is beneath
# the window minus the window itself, which scenefx cannot express today -- and
# the assertion belongs with it. `shadows_blur_background` is off by default
# and a plain shadow has neither artefact.
