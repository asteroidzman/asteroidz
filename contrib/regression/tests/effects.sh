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

# ── the shadow's backdrop blur is a SHADOW, not a glow ──────────────────────
#
# `shadows_blur_background` blurs what is behind a window's shadow so the
# shadow blends with it rather than sitting on it as a flat smear. It used to
# take the live re-blur path for FLOATING windows -- the same rule regular
# window blur uses, on the reasoning that a floating window can sit over
# anything and its shadow should blend with what is really under it.
#
# A live blur samples the framebuffer, and a shadow's footprint hugs its
# window: what it picked up was the window's own pixels, smeared outward. On a
# dark wallpaper that is a coloured glow around every floating window, which is
# how it was reported.
#
# ON A BLACK WALLPAPER, which is why this module swaps one in and puts the grey
# back afterwards. A shadow cannot make black brighter, so any light at all
# outside the window is the bug, and the measurement needs no threshold to
# argue about. The first version of this test measured the halo's HUE on the
# harness's grey instead -- a green window should not have a green-tinted
# shadow -- and passed against the broken build, because over mid-grey the
# cast is a couple of levels. Reported values on black: 0, 5, 12, 20
# approaching the edge, and green when the window was green.
test_a_floating_windows_shadow_does_not_glow() {
	cat >> "$HL_CONFIG" <<'EOF'
shadows 1
shadow_only_floating 1
shadows_size 72
shadows_blur 72
shadows_blur_background 1
shadows_blur_background_strength 0.55
shadowscolor 0x00000050
EOF
	hl_dispatch "reload_config" 1

	# Black wallpaper for the duration. Every other module measures against
	# the harness's flat grey, so this has to go back exactly as it was --
	# including HL_SWAYBG_PID, which hl_stop kills.
	local grey_pid="${HL_SWAYBG_PID:-}"
	[ -n "$grey_pid" ] && kill "$grey_pid" 2>/dev/null
	magick -size "${HL_WIDTH}x${HL_HEIGHT}" xc:'#000000' "$HL_OUTDIR/black.png" 2>/dev/null \
		|| convert -size "${HL_WIDTH}x${HL_HEIGHT}" xc:'#000000' "$HL_OUTDIR/black.png" 2>/dev/null
	swaybg -o '*' -i "$HL_OUTDIR/black.png" -m fill > /dev/null 2>&1 &
	HL_SWAYBG_PID=$!
	sleep 1.5

	HL_SPAWN_COLOR_IDX=1 hl_spawn_kitty GLOW >/dev/null   # '#22aa22'
	hl_wait_client_count 1
	hl_dispatch "toggle_floating" 1
	sleep 1.5
	hl_screenshot shadow-glow

	local verdict
	verdict="$(python3 - "$HL_OUTDIR/shadow-glow.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
w, h = im.size
y = h // 2
# The window is the first bright thing at this height; its shadow is the
# 40px of wallpaper just left of it.
xs = [x for x in range(w) if sum(px[x, y]) > 90]
if not xs:
    print("no-window")
    raise SystemExit
edge = xs[0]
band = [px[x, y] for x in range(max(0, edge - 44), max(0, edge - 4))]
if not band:
    print("no-band")
    raise SystemExit
print("black" if max(sum(p) for p in band) <= 6 else
      "lit-%d" % max(sum(p) for p in band))
PY
)"
	hl_assert "a floating window's shadow does not light up a black wallpaper" \
		"$verdict" "black"

	# Grey back, whatever happened above.
	kill "$HL_SWAYBG_PID" 2>/dev/null
	swaybg -o '*' -i "$HL_WALLPAPER" -m fill > "$HL_OUTDIR/swaybg.log" 2>&1 &
	HL_SWAYBG_PID=$!
	sleep 1
}
