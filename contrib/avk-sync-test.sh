#!/usr/bin/env bash
# avk-sync-test.sh — does a finished frame reach the display with a fence?
#
# M3.5C's acceptance test, and it exists because the answer used to be no. AVK
# submitted a frame, took the buffer straight to wlr_output_state_set_buffer()
# and committed it, with nothing anywhere telling the display engine that the
# GPU had not finished writing. It looked fine. It looked fine because the
# amdgpu kernel driver was inserting implicit fences on our behalf, which is
# a driver behaviour and not a guarantee -- and which the queue-family-foreign
# release AVK does to its scan-out buffers is entitled to break at any time.
#
# What is asserted here is not "the frame looks right". A frame handed over
# unsynchronised looks right nearly always, which is the entire problem, and
# it is why this file asserts on counters rather than on pixels.
#
# THE LIMIT OF THIS TEST, stated plainly because the last presentation bug
# shipped past a suite that passed throughout:
#
#   The headless backend has no DRM device (wlr_backend_get_drm_fd returns -1)
#   and rejects any output commit carrying a wait or signal timeline. So this
#   file exercises the DMA-BUF path only. The drm_syncobj timeline path -- the
#   one a real monitor uses -- is covered by test-avk-core's syncobj round trip
#   at the primitive level, and by nothing at all at the compositor level until
#   somebody runs it on a display and reads `amsg get avk-stats`.
#
#   Concretely: present_sync_timeline can only ever be 0 here, and a build that
#   broke the timeline path entirely would still pass this file.
#
# Break test, which MUST fail:
#
#   BREAK=presentsync   AZ_AVK_NO_PRESENT_SYNC=1 -- hand every frame over with
#                       no fence at all. This is the state the code was in
#                       before M3.5C, so a pass here means the assertions below
#                       are decorative.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-sync"
BREAK="${BREAK:-}"

# No effects: unimplemented in AVK, and a fallback frame is a frame that never
# reaches the presentation path at all, which would make the counters lie.
SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-sync-$$"
HL_OUTDIR="$OUTDIR"
HL_WIDTH=1280 HL_HEIGHT=720
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = presentsync ] && HL_ENV="$HL_ENV AZ_AVK_NO_PRESENT_SYNC=1"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

hl_start "$SCENE_KDL"
hl_spawn_kitty one >/dev/null; hl_wait_client_count 1 60
# Enough frames that a per-frame counter is unambiguously counting frames and
# not a one-off at startup.
sleep 3
hl_screenshot shot
hl_stop

LOG="$OUTDIR/state/asteroidz/asteroidz.log"
stat_field() { sed -n "s/.*\($1=[0-9]*\).*/\1/p" "$LOG" | tail -1 | cut -d= -f2; }

echo "-- the mechanism it chose --"
# Named in the log, once, at the point the decision is made. If this line is
# missing the output never reached the presentation path and every counter
# below is zero for the wrong reason.
hl_assert "AVK said how the frame's fence would travel" \
	"$(grep -cE "AVK: HEADLESS-1 (presents with drm_syncobj timelines|has no drm_syncobj timeline support)" "$LOG")" "1"
hl_assert "and it is the dma-buf path, because headless has no DRM device" \
	"$(grep -c "the frame's fence travels on the target's dma-buf" "$LOG")" "1"

echo "-- every frame left with a fence --"
FRAMES="$(stat_field 'avk\.frames')"
DMABUF="$(stat_field 'avk\.present_sync_dmabuf')"
TIMELINE="$(stat_field 'avk\.present_sync_timeline')"
NONE="$(stat_field 'avk\.present_sync_none')"
FAILS="$(stat_field 'avk\.present_sync_fails')"
FALLBACKS="$(stat_field 'avk\.fallback_frames')"

hl_assert "AVK composited frames at all" \
	"$([ "${FRAMES:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "no frame fell back to SceneFX" "${FALLBACKS:-x}" "0"
# The assertion. Not "most frames" and not "at least one": every frame AVK
# composited was handed over with a fence attached, so the two must be equal.
hl_assert "every composited frame carried a fence ($FRAMES frames)" \
	"$(( ${DMABUF:-0} + ${TIMELINE:-0} ))" "${FRAMES:-x}"
hl_assert "no frame was handed over unsynchronised" "${NONE:-x}" "0"
hl_assert "no fence failed to attach" "${FAILS:-x}" "0"

echo "-- and the CPU never waited for it --"
# The whole point of a fence is that somebody else does the waiting. A nonzero
# value in either of these means the frame path blocked, which is the failure
# mode explicit synchronisation exists to avoid rather than to introduce.
hl_assert "no CPU sync waits on the frame path" \
	"$(stat_field 'avk\.cpu_sync_waits' || echo x)" "0"
hl_assert "no target was re-acquired before it was released" \
	"$(stat_field 'avk\.target_state_violations' || echo x)" "0"

echo "-- the desktop still works --"
# A synchronisation change that produced a black screen would satisfy every
# counter above. One pixel of wallpaper is enough to say it did not.
if command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null; then
	WP="$(python3 - "$OUTDIR/shot.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(n for n, c in im.getcolors(1 << 24) if c == (0x80, 0x80, 0x80)))
PY
)"
	hl_assert "the frame has a wallpaper in it" \
		"$([ "${WP:-0}" -gt 10000 ] && echo true || echo false)" "true"
else
	echo "  (no python3-pillow -- skipping the pixel check)"
fi

echo
echo "screenshot: $OUTDIR/shot.png"
echo "log:        $LOG"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
