#!/usr/bin/env bash
# blur-sync-validation.sh — are the optimized-blur image copies synchronised?
#
# The live report: blur is correct when a session starts, and corrupts into
# flat dark polygons the moment the WALLPAPER changes. `blur { optimized 0 }`
# avoids it, and it is worse when the other monitor is in HDR.
#
# That trio says "unsynchronised GPU work" rather than "wrong pixels". A
# wallpaper change is what calls wlr_scene_optimized_blur_mark_dirty, so it is
# what makes the per-monitor cache RE-render mid-session, with a queue that is
# already busy -- as opposed to the first fill on a quiet startup, which is
# where the hazard happens to win. An HDR sibling adds work and shifts the
# timing further. `optimized 0` removes the cache, and with it the copies.
#
# The copies live in AVK's per-monitor blur cache: plain vkCmdCopyImage between
# images the compositor owns. A copy confers no ordering of its own, so each one
# needs barriers on both sides, against three different stages -- the graphics
# pass that wrote the scene image, the compute dispatches that blur it, and the
# fragment shaders that sample the result.
#
# The same class of bug has been found here twice. The report above is the fx_vk
# one; AVK's own optimized-blur corruption (fixed in 0.20.5) was an
# unsynchronised vkCmdCopyImage in this cache, and scale, bit depth and the
# dirty flag were all chased first. This is the instrument that would have named
# it directly.
#
# Rather than argue about which of those are covered, ask Vulkan. The
# synchronization validation layer reports read-after-write and write-after-
# write hazards directly, and it does not care how busy the queue happens to
# be, so it finds them on a headless instance that never shows the artifact.
#
# RESULT, on fx_vk's copies as they stood when this was written: 4 hazards, 2
# READ-AFTER-WRITE and 2 WRITE-AFTER-WRITE, one naming vkCmdCopyImage outright --
#
#   vkQueueSubmit2KHR(): WRITE_AFTER_WRITE hazard detected. vkCmdCopyImage
#   ... writes to VkImage 0x78, which was previously written during an image
#   layout transition initiated by vkCmdPipelineBarrier
#
# -- and 0 after adding barriers on both sides of both copy helpers. That is
# the whole argument for the fix, and re-running this is how to keep it true.
#
# Usage: contrib/blur-sync-validation.sh   (registered `manual` in avk-suite.sh:
#        it needs vulkan-validation-layers installed and gates nothing)
# Exit:  0 no hazards, 1 hazards found, 2 could not run (no layer / no vulkan)
set -u

REPO="${ASTEROIDZ_REPO:-$HOME/asteroidz}"
export HL_RENDERER=vulkan

# ONE switch, and it is not the one you would guess. The renderer decides at
# instance creation whether to request the validation layer at all, so setting
# VK_INSTANCE_LAYERS from outside does nothing -- the layer loads and then has
# no work. Sync validation is a validation FEATURE rather than the layer itself,
# but AVK turns it on for you: with validation enabled it sets `validate_sync`
# through VkLayerSettingEXT (avk_instance.c), so VK_LAYER_VALIDATE_SYNC=1 is not
# needed here and its absence is not a hole. fx_vk did not do that, which is why
# earlier revisions of this script set it by hand.
# SYNC_ENV= runs the same scene with validation OFF, which must reach the
# INCONCLUSIVE exit below and not a pass. That is the break for the guard:
# the previous one could not be exercised because it never held.
export HL_ENV="${SYNC_ENV-ASTEROIDZ_VK_DEBUG=1}"

if [ ! -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ]; then
	echo "  --   VK_LAYER_KHRONOS_validation is not installed (vulkan-validation-layers)"
	exit 2
fi

# shellcheck disable=SC1091
. "$REPO/contrib/lib/headless.sh"

hl_start
trap 'hl_stop' EXIT

WORK="$HL_OUTDIR"
# The layer writes to stderr -- and asteroidz dup2's its own stderr to the
# state log, so the file the harness redirects the process into is EMPTY. Read
# the state log, not comp-stdout.log.
COMPLOG="$WORK/state/asteroidz/asteroidz.log"

cat >> "$HL_CONFIG" <<'EOF'
effects {
	blend-space "srgb"
	blur { enable 1; layer 0; optimized 1; passes 2; radius 6
		transparency-threshold 0.5
		params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } }
}
EOF
hl_dispatch "reload_config" 1

# A second output, because the live arrangement has one and the cache is
# per-monitor: two of them means twice the copies and twice the chances.
hl_dispatch "create_virtual_output" 2
hl_dispatch "set_output_position,HEADLESS-2,$HL_WIDTH,0" 1

# Something translucent, so blur is actually consumed rather than merely built.
kitty --title BLURWIN -o background_opacity=0.35 -o background='#101010' \
	sh -c 'echo BLURWIN; exec sleep 600' > "$WORK/kitty.log" 2>&1 &
sleep 4

# THE TRIGGER. Changing the background layer surface is what marks the
# optimized-blur node dirty, and re-rendering the cache mid-session is the
# case that breaks live. Do it several times: the hazard is a race, and the
# validation layer reports the first instance of each distinct one.
kill "$HL_SWAYBG_PID" 2>/dev/null
sleep 0.3
for i in 1 2 3; do
	magick -size "${HL_WIDTH}x${HL_HEIGHT}" plasma:fractal -blur 0x2 "$WORK/wall$i.png"
	swaybg -o '*' -i "$WORK/wall$i.png" -m fill > "$WORK/swaybg$i.log" 2>&1 &
	SWAYBG_PID=$!
	sleep 3
	kill "$SWAYBG_PID" 2>/dev/null
	sleep 0.5
done
sleep 2

echo
# Did SYNC validation actually come up? A run where it silently failed reports
# zero hazards and looks exactly like a pass, which is the one result this
# script must never produce by accident. Assert the feature, not the layer:
# validation can be on with `validate_sync` off, and then every hazard below is
# invisible. avk_instance_log_caps prints the state of both.
#
# The string this used to look for -- "validation layer enabled" -- was never
# logged by anything, so this guard fired on every run and the script could
# only ever exit 2. It was inert for as long as it was in the tree.
if ! grep -q "sync_validation=on" "$COMPLOG" 2>/dev/null; then
	echo "  --   sync validation never came up:"
	grep -o "debug_utils=.*gpu_assisted=[a-z]*" "$COMPLOG" 2>/dev/null | tail -1 | sed 's/^/       /'
	echo "  --   INCONCLUSIVE, not a pass"
	exit 2
fi

# No `|| echo 0`: grep -c already PRINTS 0 when it matches nothing, and exits
# 1 while doing it, so the fallback appended a second zero and the count came
# out as "0\n0" -- which then failed the numeric test below.
HAZARDS="$(grep -c "SYNC-HAZARD" "$COMPLOG" 2>/dev/null)"
echo "SYNC-HAZARD reports: $HAZARDS"
if [ "$HAZARDS" -gt 0 ]; then
	echo
	echo "distinct hazards:"
	grep -o "SYNC-HAZARD[A-Z_-]*" "$COMPLOG" | sort | uniq -c | sed 's/^/  /'
	echo
	echo "first report:"
	grep -m1 -A6 "SYNC-HAZARD" "$COMPLOG" | sed 's/^/  /'
	exit 1
fi

echo "  ok   no synchronisation hazards across $((3)) wallpaper changes"
exit 0
