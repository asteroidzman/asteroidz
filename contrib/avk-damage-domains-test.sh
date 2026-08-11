#!/usr/bin/env bash
# avk-damage-domains-test.sh — are source damage and scene damage really two
# different things?
#
# The claim M3.5D.1 rests on is that these are separate domains:
#
#     source damage   what the client redrew          -> controls SHM upload
#     scene damage    what the output must repaint    -> controls composition
#
# The clearest proof is a window that moves without changing a pixel. Dragging
# a static terminal across the desktop damages an enormous amount of output --
# the whole path it sweeps, twice over, once uncovering and once covering --
# and changes nothing whatsoever in the client's buffer. If moving it uploads
# anything, the two domains are still conflated somewhere.
#
# The second thing here is the output cull. The scene walk starts at the scene
# ROOT, so it visits every node for every output, including nodes belonging to
# a monitor thousands of pixels away. Resolving those costs an import or a copy
# and produces a command the renderer then scissors away to nothing. Measured
# on the live desktop, each output frame was resolving BOTH monitors'
# full-screen wallpapers. With two outputs side by side, a node parked entirely
# on one of them must be culled before its buffer is resolved.
#
# Break test:
#
#   BREAK=no-cull   AZ_AVK_NO_OUTPUT_CULL=1 -- resolve buffers before testing
#                   whether the node can touch this output at all.
#
#                   *** NOT A FALSIFIER. DO NOT READ ITS PASS AS COVERAGE. ***
#
#                   This harness runs ONE output, and a node can only be culled
#                   for being entirely on some other one. Measured with the
#                   switch on and off, `nodes_output_culled_before_resolve` is
#                   0 both ways and `shm_upload_bytes` is 0 both ways: there is
#                   nothing for the switch to stop doing, so the run comes back
#                   4/4 either way.
#
#                   It is left here rather than deleted because the cull is
#                   real and is measured live -- 2970 of 8010 nodes on the
#                   dual-monitor desktop. Making it falsifiable needs a second
#                   headless output (WLR_HEADLESS_OUTPUTS=2) placed beside the
#                   first, which is a harness change, not a test change.
#
#                   The assertions below are about the DAMAGE DOMAINS, and
#                   those are falsifiable and are what this script is for. The
#                   cull is described in the commentary because it was found
#                   here; it is not asserted here.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-damage-domains"
BREAK="${BREAK:-}"

SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-domains-$$"
HL_OUTDIR="$OUTDIR"
HL_WIDTH=1280 HL_HEIGHT=720
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = no-cull ] && HL_ENV="$HL_ENV AZ_AVK_NO_OUTPUT_CULL=1"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

field() { python3 - "$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}

hl_start "$SCENE_KDL"
hl_spawn_kitty one >/dev/null; hl_wait_client_count 1 60
# Let it finish painting and settle. A terminal that is still drawing its first
# frames would supply real source damage and make the whole measurement
# meaningless.
sleep 4

# ── moving a static window ─────────────────────────────────────────────────
echo "-- moving a window that has not changed a pixel --"
hl_dispatch reset_avk_stats
# Float it so it can actually be moved around, then walk it across the desktop.
hl_dispatch togglefloating 0.4
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
	hl_dispatch "moveresize,$(( (i % 6) * 90 + 40 )) $(( (i % 4) * 70 + 40 )) 500 380" 0.25
done
sleep 1
hl_get "get avk-stats" > "$OUTDIR/stats-move.json"

FRAMES="$(field "$OUTDIR/stats-move.json" frames)"
DMG="$(field "$OUTDIR/stats-move.json" damage_pixels)"
UPLOADS="$(( $(field "$OUTDIR/stats-move.json" shm_full_uploads) + $(field "$OUTDIR/stats-move.json" shm_partial_uploads) ))"
BYTES="$(field "$OUTDIR/stats-move.json" shm_upload_bytes)"
SKIPS="$(field "$OUTDIR/stats-move.json" shm_upload_skips)"
echo "  note: $FRAMES frames, $DMG damaged output px, $UPLOADS uploads, $BYTES upload bytes, $SKIPS skips"

# Premise, both halves. Moving must actually have produced frames and output
# damage, or "no uploads" is the right answer to a question nobody asked.
hl_assert "moving the window produced frames" \
	"$([ "${FRAMES:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "and a lot of output damage" \
	"$([ "${DMG:-0}" -gt 500000 ] && echo true || echo false)" "true"
hl_assert "and the surface was looked up on those frames" \
	"$([ "${SKIPS:-0}" -gt 0 ] && echo true || echo false)" "true"
# The assertion. Not "a bit less" -- a window whose pixels did not change must
# cost exactly nothing on the upload path, however far it travelled. A blinking
# cursor can add a little, so this allows a small terminal-sized budget rather
# than demanding a hard zero.
hl_assert "but the copy cost stayed negligible (${BYTES} B over $FRAMES frames)" \
	"$([ "${BYTES:-999999999}" -lt 400000 ] && echo true || echo false)" "true"

hl_stop

echo
echo "stats: $OUTDIR/stats-move.json"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
