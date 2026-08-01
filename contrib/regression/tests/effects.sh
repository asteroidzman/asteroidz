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
# The one shadow setup all three scenes share, with the backdrop blur as the
# only variable. Appended rather than rewritten: later declarations win, so
# calling this twice with different arguments flips the one setting and leaves
# everything else -- geometry, colour, spread -- provably identical between the
# two frames of the differential below.
_shadow_scene_config() {
	cat >> "$HL_CONFIG" <<EOF
shadows 1
shadow_only_floating 1
shadows_size 72
shadows_blur 72
shadows_blur_background $1
shadows_blur_background_strength 0.55
shadowscolor 0x00000050
window-rule { match title="FB-FLOAT"; open-floating #true; width 700; height 500 }
EOF
	hl_dispatch "reload_config" 1
}

test_a_floating_windows_shadow_does_not_glow() {
	_shadow_scene_config 1

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

# The other direction, and the one that keeps the fix honest: "never brighter"
# is satisfied perfectly by a shadow that does nothing at all.
#
# This needs a genuinely FLOATING window with wallpaper around it, which is not
# what an earlier version of this test had: it spawned one tiled window and
# toggled it floating, so the window kept a near-fullscreen geometry and the
# "band beside it" was the five-pixel gap at the screen edge. It passed anyway,
# on whatever the blur was doing to those five pixels -- and then started
# failing on a correct renderer, which is the only reason it was looked at. A
# test measuring the wrong pixels is worse than no test: it reports on
# something, so nobody checks what.
test_a_floating_windows_shadow_still_darkens() {
	_shadow_scene_config 1

	local saved=("${HL_SPAWN_COLORS[@]}")
	local saved_idx="$HL_SPAWN_COLOR_IDX"
	HL_SPAWN_COLORS=('#22aa22')
	HL_SPAWN_COLOR_IDX=0
	hl_spawn_kitty FB-FLOAT >/dev/null
	hl_wait_client_count 1
	sleep 2
	hl_screenshot shadow-grey
	HL_SPAWN_COLORS=("${saved[@]}")
	HL_SPAWN_COLOR_IDX="$saved_idx"

	# The harness wallpaper is a flat mid-grey, so the shadow band beside the
	# window is the only thing that can be darker than it.
	hl_assert "the shadow darkens the wallpaper beside the window" \
		"$(python3 - "$HL_OUTDIR/shadow-grey.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
w, h = im.size
y = h // 2

green = [px[x, y][1] > 90 and px[x, y][0] < 90 for x in range(w)]
best = run = 0
edge = start = -1
for x in range(w + 1):
    if x < w and green[x]:
        if run == 0:
            start = x
        run += 1
    else:
        if run > best:
            best, edge = run, start
        run = 0
if best < 100 or edge < 200:
    print("no-float")
    raise SystemExit

band = [sum(px[x, y]) for x in range(edge - 64, edge - 6)]
bare = sum(px[edge - 300, y])           # wallpaper, well outside the spread
print("darker" if min(band) < bare - 20 else "not-darker-%d" % (bare - min(band)))
PY
)" "darker"
}

# The one the black wallpaper cannot catch, and the reason this file grew.
#
# The two tests above both measure a shadow against the WALLPAPER, and the
# first fix for the halo -- filling the window's hole in the blur source with
# the unblurred wallpaper snapshot -- passes both of them by construction: over
# bare wallpaper the substitute IS the truth. On a real desktop it was still
# wrong, because a floating window usually sits over another window rather than
# over the wallpaper, and there the substitute is lighter than what is actually
# beneath. It bled outward exactly as the window's own pixels had. Measured on
# the shipped build: 9 -> 17 approaching the edge.
#
# So this scene has no wallpaper in it at all: a dark window filling the screen
# with the floating one on top, and the measurement taken well inside the dark
# window. That makes the backdrop FLAT within the blur's reach, and a flat
# backdrop is what turns this into a physical invariant instead of a tuned
# number -- blurring a constant field returns the same constant field, so the
# only thing the backdrop blur can do beside the window is darken. Anything
# brighter than the no-blur baseline is, whatever its colour, something the
# blur invented.
#
# It is a DIFFERENTIAL against `shadows_blur_background 0`, not against a
# hardcoded value: same geometry, same shadow, same window, one setting apart.
# That isolates the feature and survives a change to the harness palette, the
# gaps or the shadow colour. And the flatness of the baseline band is asserted
# rather than assumed, because a band that had strayed onto the wallpaper would
# make the whole comparison meaningless while still reporting a pass.
test_a_shadow_over_a_dark_window_does_not_lighten_it() {
	local saved=("${HL_SPAWN_COLORS[@]}")
	local saved_idx="$HL_SPAWN_COLOR_IDX"

	_shadow_scene_config 0
	HL_SPAWN_COLORS=('#0a0a0a')
	HL_SPAWN_COLOR_IDX=0
	hl_spawn_kitty FB-DARK >/dev/null
	hl_wait_client_count 1
	sleep 1
	HL_SPAWN_COLORS=('#22aa22')
	HL_SPAWN_COLOR_IDX=0
	hl_spawn_kitty FB-FLOAT >/dev/null
	hl_wait_client_count 2
	sleep 2
	hl_screenshot shadow-noblur

	# Only the setting changes. The windows are never touched between the two
	# frames, so anything that differs is the backdrop blur and nothing else.
	_shadow_scene_config 1
	sleep 1.5
	hl_screenshot shadow-blur

	HL_SPAWN_COLORS=("${saved[@]}")
	HL_SPAWN_COLOR_IDX="$saved_idx"

	hl_assert "a shadow over a dark window only ever darkens it" \
		"$(python3 - "$HL_OUTDIR/shadow-noblur.png" "$HL_OUTDIR/shadow-blur.png" <<'PY'
import sys
from PIL import Image

base = Image.open(sys.argv[1]).convert("RGB").load()
shot = Image.open(sys.argv[2]).convert("RGB").load()
im = Image.open(sys.argv[2]).convert("RGB")
w, h = im.size
y = h // 2

# The LONGEST green run on the row, not the first green pixel: the tiled
# window's focus border is green too, and taking its leftmost pixel put the
# measurement band off the left of the screen (it reported "no-band", which
# is at least a refusal rather than a pass).
green = [shot[x, y][1] > 90 and shot[x, y][0] < 90 for x in range(w)]
best = run = 0
edge = start = -1
for x in range(w + 1):
    if x < w and green[x]:
        if run == 0:
            start = x
        run += 1
    else:
        if run > best:
            best, edge = run, start
        run = 0
if best < 100:
    print("no-float")
    raise SystemExit

# Left of the window, inside the blur's 72px reach, clear of the border.
lo, hi = max(0, edge - 64), max(0, edge - 6)
band = range(lo, hi)
if len(band) < 20:
    print("no-band")
    raise SystemExit

# The premise: flat backdrop. Without this the comparison below says nothing.
vals = [c for x in band for c in base[x, y]]
if max(vals) - min(vals) > 2:
    print("baseline-not-flat-%d" % (max(vals) - min(vals)))
    raise SystemExit

# ONE LEVEL, per channel -- not a tuned threshold. The flat #0a0a0a backdrop
# is rendered as 9 or 10 depending on where the dither lands, so a pixel can
# legitimately differ by one between two frames of an unchanged scene, and all
# three channels can do it together. Nothing else may move at all. For scale,
# the bug this catches measured +8 per channel on GLES and +28 on Vulkan.
worst = max(s - b for x in band
            for s, b in zip(shot[x, y], base[x, y]))
print("only-darker" if worst <= 1 else "brighter-by-%d" % worst)
PY
)" "only-darker"
}
