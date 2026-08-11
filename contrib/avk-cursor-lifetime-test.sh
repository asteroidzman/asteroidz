#!/usr/bin/env bash
# avk-cursor-lifetime-test.sh — a borrowed cursor may not outlive its owner.
#
# `wlr_xcursor_manager_get_xcursor()` returns memory the manager owns.
# `wlr_xcursor_manager_destroy()` frees every cursor, image and pixel buffer in
# it (wlr_xcursor.c:38-47, wlr_xcursor_manager.c:19-31). asteroidz destroys and
# rebuilds that manager in reapply_cursor_style() on ANY live config change --
# a settings slider, set-config, a reload.
#
# M3.5E stage 1 kept the resolved `struct wlr_xcursor *` as durable cursor
# identity. Two paths replayed it with no re-resolve: az_cursor_show(), which
# restores the cursor after an idle hide, and az_cursor_xcursor_tick(), the
# animation timer. Either one, after a config change, read a freed
# wlr_xcursor_image and handed wlroots a wild wlr_buffer.
#
# That killed a live desktop. The core dump is unambiguous about the mechanism:
#
#     hotspot_x = -2147483648        (INT32_MIN)
#     hotspot_y = 14617
#     image_pushed = false           (the cursor was hidden: the restore path)
#
# INT32_MIN is what `(int)roundf()` saturates to when handed a NaN, and the NaN
# came from `image->hotspot_x / scale` reading freed memory -- so the image was
# already dead before the buffer was derived from it.
#
# WHY THIS TEST IS NOT A CRASH TEST
#
# The original SIGSEGV was never reproduced headlessly: ~110 iterations across
# four reproducer shapes, under ASan, never faulted and never tripped the
# sanitizer. Freed heap usually stays mapped and usually still holds
# plausible-looking bytes, so the old code mostly read garbage quietly and only
# occasionally hit an unmapped address. Waiting for undefined behaviour to
# happen to fault is not coverage.
#
# So the invariant is checked instead, one step before the dereference:
#
#     a borrowed xcursor may only be used while the manager generation that
#     produced it is still current
#
# `cursor_mgr_generation` increments on every manager rebuild, and
# `cursor_stale_xcursor` counts borrowed resolutions used after theirs expired.
# Zero is the only correct value.
#
# NOTE: wlr_xcursor_manager_load() is NOT an invalidation. It appends a scaled
# theme and frees nothing, so pointers handed out earlier survive it. Only
# destruction invalidates. The first theory of this crash blamed mixed-scale
# loading and was wrong; the scales are still exercised below because they
# shaped the live sequence, but they are not the ownership violation.
#
#   BREAK=cursor-stale-xcursor   AZ_CURSOR_STALE_XCURSOR=1 restores the shipped
#                                model: keep the borrowed pointer and replay it
#                                instead of re-resolving. MUST FAIL.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-cursor-lifetime"
BREAK="${BREAK:-}"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-cursor-life-$$"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=2
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = cursor-stale-xcursor ] && HL_ENV="$HL_ENV AZ_CURSOR_STALE_XCURSOR=1"
export HL_OUTDIR HL_OUTPUTS HL_ENV

# Mixed scales, because the live layout was mixed and the per-scale themes are
# what make the manager hold more than one thing to free.
hl_start "cursor_hide_timeout 1
monitorrule {
    name \"HEADLESS-1\"
    scale 1.0
}
monitorrule {
    name \"HEADLESS-2\"
    scale 2.0
}"
sleep 2

gen() { hl_get "get avk-stats" | jq -r '.cursor_mgr_generation // 0'; }
stale() { hl_get "get avk-stats" | jq -r '.cursor_stale_xcursor // 0'; }
xsets() { hl_get "get avk-stats" | jq -r '.cursor_xcursor_sets // 0'; }

# Destroy and rebuild the xcursor manager. Any live config change does this;
# borderpx is chosen because it is real, cheap and unrelated to cursors, which
# is the point -- nothing about it warns you that it frees the cursor theme.
rebuild_manager() {
	printf '%s' "{\"changes\":[{\"key\":\"borderpx\",\"value\":\"$1\"}],\"persist\":false}" \
		| ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" "$HL_REPO/build/amsg" set-config @- \
		>/dev/null 2>&1
}

GEN0="$(gen)"
echo "-- the manager generation moves when the manager is replaced --"
hl_assert "the compositor reports a manager generation ($GEN0)" \
	"$([ "$GEN0" -ge 1 ] && echo true || echo false)" "true"

# Select an xcursor: this is what the old code cached.
"$HL_WLVPTR" move:300,300 >/dev/null 2>&1
sleep 0.3
SETS_BEFORE="$(xsets)"
hl_assert "an xcursor was selected before the rebuild ($SETS_BEFORE)" \
	"$([ "$SETS_BEFORE" -gt 0 ] && echo true || echo false)" "true"

rebuild_manager 3
sleep 0.5
GEN1="$(gen)"
echo "  note: generation $GEN0 -> $GEN1"
hl_assert "rebuilding the manager advanced the generation" \
	"$([ "$GEN1" -gt "$GEN0" ] && echo true || echo false)" "true"

echo
echo "-- replaying a cursor across a manager rebuild --"

# The live sequence, driven deterministically: resolve, invalidate, replay.
# Each iteration crosses both scales, lets the idle timeout hide the cursor so
# the az_cursor_show() restore path runs, and rebuilds the manager underneath.
for i in 1 2 3 4 5 6 7 8; do
	"$HL_WLVPTR" "move:$((250 + i * 40)),300" >/dev/null 2>&1
	"$HL_WLVPTR" "move:$((1400 + i * 20)),400" >/dev/null 2>&1
	rebuild_manager $((2 + i % 5))
	sleep 1.3          # idle hide, with the manager now replaced
	"$HL_WLVPTR" "move:$((300 + i * 30)),350" >/dev/null 2>&1
	sleep 0.2
done

STALE="$(stale)"
GEN2="$(gen)"
NOIMG="$(hl_get "get avk-stats" | jq -r '.cursor_no_image // 0')"
FAILS="$(hl_get "get avk-stats" | jq -r '.cursor_import_failures // 0')"
echo "  note: generations $GEN0 -> $GEN2, stale borrowed uses: $STALE"

hl_assert "no borrowed xcursor was used after its manager was replaced ($STALE)" \
	"$STALE" "0"
hl_assert "the cursor was never left without an image ($NOIMG)" \
	"$NOIMG" "0"
hl_assert "no cursor image failed to import ($FAILS)" \
	"$FAILS" "0"

echo
echo "-- re-selection is not needed, and must not be forced --"

# A rebuild does not oblige the cursor to change: the buffer asteroidz locked
# is still valid across it, because readonly_data_buffer_drop() copies the
# pixels into saved_data whenever locks are held (readonly_data.c:66-88). So
# the same name at the same scale must NOT re-push -- doing so every time the
# manager is rebuilt would be a redundant import on every settings change.
#
# NOT ASSERTED HERE: that a *different* name re-selects because identity is
# compared by name rather than by pointer. The failure it guards against needs
# the allocator to hand back the same address for a different cursor, and a
# test cannot make that happen on demand; asserting it without controlling the
# allocator would be a test that passes for the wrong reason. The name-based
# comparison is exercised by avk-cursor-content, which changes shapes.
SETS_A="$(xsets)"
"$HL_WLVPTR" move:400,300 >/dev/null 2>&1
sleep 0.3
rebuild_manager 5
sleep 0.4
"$HL_WLVPTR" move:420,320 >/dev/null 2>&1
sleep 0.3
SETS_B="$(xsets)"
echo "  note: xcursor selections $SETS_A -> $SETS_B"
hl_assert "an unchanged cursor is not re-imported across a rebuild" \
	"$SETS_B" "$SETS_A"

ALIVE=false
kill -0 "$HL_COMP_PID" 2>/dev/null && ALIVE=true
hl_assert "the compositor survived every rebuild" "$ALIVE" "true"

hl_stop

if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "It restores the durable borrowed pointer the fix removed, so a"
	echo "replay after a rebuild reaches a generation that no longer exists."
	echo "The stale counter is the falsifier; the original bug's SIGSEGV was"
	echo "never reproducible on demand, which is exactly why the invariant is"
	echo "checked instead of the crash."
fi
hl_summary
