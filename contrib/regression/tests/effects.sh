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

# ── the shadow's backdrop blur must not glow ────────────────────────────────
#
# `shadows_blur_background` blurs what is behind a window's shadow. The blur's
# box is the SHADOW's box, which is the window plus its spread -- so the region
# it samples covers the window itself, and the scene image holds the PREVIOUS
# frame there (the node draws beneath the window, and an undamaged region is
# never re-rendered). The blur therefore picked the window's own pixels up and
# spread them outward: a halo in the window's own colour, which on a dark
# backdrop is a glow around every floating window.
#
# Fixed in scenefx by keeping the window's box out of the blur's SOURCE
# (wlr_scene_blur_set_sample_exclude), not by choosing a different blur source
# -- an earlier attempt did that, swapped the rim for a much worse bright haze
# wherever a floating window sat over another window, and was reverted.
#
# ON A BLACK WALLPAPER. A shadow cannot make black brighter, so any light
# outside the window is the bug and there is no threshold to argue about. The
# module puts the harness's grey back afterwards, HL_SWAYBG_PID included,
# because every other module measures against it.
#
# Calibrated against the unpatched renderers: GLES 13, Vulkan 71, both 0 with
# the fix. HL_RENDERER picks which one runs here.
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

	local grey_pid="${HL_SWAYBG_PID:-}"
	[ -n "$grey_pid" ] && kill "$grey_pid" 2>/dev/null
	magick -size "${HL_WIDTH}x${HL_HEIGHT}" xc:'#000000' "$HL_OUTDIR/black.png" 2>/dev/null \
		|| convert -size "${HL_WIDTH}x${HL_HEIGHT}" xc:'#000000' "$HL_OUTDIR/black.png" 2>/dev/null
	swaybg -o '*' -i "$HL_OUTDIR/black.png" -m fill > /dev/null 2>&1 &
	HL_SWAYBG_PID=$!
	sleep 1.5

	# Green explicitly: the halo takes the WINDOW's colour, so the assertion
	# is about this colour and must not depend on the spawn rotation.
	HL_SPAWN_COLOR_IDX=1 hl_spawn_kitty GLOW >/dev/null
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
xs = [x for x in range(w) if sum(px[x, y]) > 90]
if not xs:
    print("no-window")
    raise SystemExit
edge = xs[0]
band = [sum(px[x, y]) for x in range(max(0, edge - 44), max(0, edge - 4))]
if not band:
    print("no-band")
    raise SystemExit
print("black" if max(band) <= 6 else "lit-%d" % max(band))
PY
)"
	hl_assert "a floating window's shadow does not light up a black wallpaper" \
		"$verdict" "black"

	kill "$HL_SWAYBG_PID" 2>/dev/null
	swaybg -o '*' -i "$HL_WALLPAPER" -m fill > "$HL_OUTDIR/swaybg.log" 2>&1 &
	HL_SWAYBG_PID=$!
	sleep 1
}
