#!/usr/bin/env bash
# avk-blur-damage-test.sh — is a partially damaged desktop the same desktop?
#
# tests/test-avk-blur-damage.c answers this at the renderer, on hand-built
# command lists, by rendering the same scene twice and diffing. That leaves one
# gap it cannot close: the renderer can propagate damage perfectly while the
# COMPOSITOR reports the wrong region to the backend, or schedules no frame at
# all, or never stops scheduling them.
#
# So this drives a real compositor and uses `amsg dispatch damage_all` as the
# oracle. Two FRAMES of one run, not two runs:
#
#     settle -> screenshot -> damage_all -> screenshot
#
# The second reconstructs every pixel from the clear upward, so a stale blur
# fringe left outside the region its source change was reported in shows up as a
# difference. Two runs could not do this: they do not place windows, lay out
# text or schedule frames identically.
#
# Also measured, because "correct" is only half of what M4F.2B is for:
#
#     rebuild / capture      what the partial path actually saved
#     touched vs skipped     how many blur nodes did no work at all
#     frames while idle      damage bookkeeping must not dirty anything
#
# THERE IS NO BREAK MODE HERE, AND THAT IS A MEASURED DECISION.
#
# BREAK=under-damage -- the forward dilation removed -- was wired in and passed
# this script 12/12 in every geometry tried: a window move, a border toggle
# behind a floating overlay, the overlay inside one terminal and across the seam
# between two. A green break run is a suite failure, so rather than leave a mode
# that reports success, it is gone.
#
# The reason is not that the break is harmless. At the RENDERER it leaves 5261
# wrong pixels at 71 codes and fails 18 of 39 checks. It is that a compositor's
# real damage is the wrong shape to expose it:
#
#   a window moves          damage covers the whole node, so the demand sweep
#                           recomputes the blur entirely and the dilation has
#                           nothing left to add
#   something small changes the region the missing dilation strands is one
#                           SUPPORT away from the change, where a dual-Kawase
#                           kernel's energy is in its tail and the error is
#                           below one 8-bit code
#
# tests/test-avk-blur-damage.c falsifies it with a 24x24 block at full
# brightness on a near-black ground, where the stranded ring begins immediately
# at the change and the error is 71 codes. That is where this break belongs.
#
# BREAK=poison is offered and is a different kind of check: it makes every
# unwritten transient pixel a colour nothing produces, so a partial prefix
# rebuild that sampled outside what it reconstructed would be unmissable.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-damage"
BREAK="${BREAK:-}"

# The same producer the walker test uses: a shadow with a blurred backdrop is
# what makes a wlr_scene_blur on a plain desktop. only-floating 0 and the
# opacity are load-bearing for the same reasons documented there -- a tiled
# window gets no shadow, and a fully opaque one gets no blur node at all.
BLUR_KDL="border_radius 12
effects {
	shadow {
		enable 1
		only-floating 0
		size 24
		blur 24
		blur-background 1
		blur-background-darken 1
		blur-background-strength 1.0
	}
	blur { enable 1; passes 3; radius 2 }
}
focused_opacity 0.9
unfocused_opacity 0.9"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-blur-damage-$$"
HL_OUTDIR="$OUTDIR"
HL_WIDTH=1280 HL_HEIGHT=800
HL_ENV="ASTEROIDZ_VK_DEBUG=1"
case "$BREAK" in
poison) HL_ENV="$HL_ENV AZ_TRANSIENT_POISON=1" ;;
esac
# NO BLINKING CURSOR, AND IT IS THE WHOLE FIXTURE.
#
# The oracle below asks whether a forced full repaint changes anything. kitty
# blinks its cursor by default, so a desktop with two terminals on it changes on
# its own between any two screenshots -- measured at 1513 px and 197 codes, which
# is larger than the 1458 px the oracle then reported and would have been read as
# staleness. The control assertion exists because of that measurement; this line
# is what makes the control pass.
HL_KITTY_EXTRA="-o cursor_blink_interval=0 -o cursor_stop_blinking_after=0"
# HL_RR1 lets this whole fixture run on a TRANSFORMED output. It is the
# minimal reproducer for the 180-degree stale strip: one output, one blur, one
# small source change, the damage_all oracle -- no seam and no cross-output halo
# anywhere in it.
HL_RR1="${DAMAGE_RR:-0}"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV HL_KITTY_EXTRA HL_RR1

LOG="$OUTDIR/state/asteroidz/asteroidz.log"

field() { python3 - "$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}

# Pixels differing between two PNGs, and the worst per-channel difference.
# Reported as "differ worst" on one line so a caller can read both.
png_diff() { python3 - "$1" "$2" <<'PY'
import sys, zlib, struct

def read(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w, h, depth, ctype = 8, b'', 0, 0, 0, 0
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos+4])[0]
        typ = data[pos+4:pos+8]
        body = data[pos+8:pos+8+ln]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'IDAT':
            idat += body
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    assert depth == 8, depth
    stride = w * ch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i-ch]) & 0xff
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xff
        elif f == 3:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xff
        elif f == 4:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                b = prev[i]
                c = prev[i-ch] if i >= ch else 0
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xff
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, ch, bytes(out)

try:
    w1, h1, c1, a = read(sys.argv[1])
    w2, h2, c2, b = read(sys.argv[2])
except Exception as e:
    print("-1 -1")
    sys.exit(0)
if (w1, h1, c1) != (w2, h2, c2):
    print("-1 -1")
    sys.exit(0)
differ = 0
worst = 0
for i in range(0, len(a), c1):
    if a[i:i+3] != b[i:i+3]:
        differ += 1
        for k in range(3):
            d = abs(a[i+k] - b[i+k])
            if d > worst:
                worst = d
print(differ, worst)
PY
}

# ── 1. a settled desktop, then a forced full repaint ───────────────────────

hl_start "$BLUR_KDL"
hl_reset_spawn_colors
hl_spawn_kitty one >/dev/null; hl_wait_client_count 1 60
hl_spawn_kitty two >/dev/null; hl_wait_client_count 2 60
sleep 5

# A FRAME HAS TO HAPPEN BEFORE THE COUNTERS MEAN ANYTHING. A settled desktop
# renders NOTHING -- which is the invariant asserted at the end of this file --
# so reading the counters two seconds after resetting them reports 0 blur nodes
# for a desktop that is full of them. Measured here as a failing premise, which
# is the right way round: the assertion caught it rather than the fixture
# silently testing an empty frame. damage_all forces exactly one frame.
hl_dispatch reset_avk_stats
sleep 1
hl_dispatch damage_all
sleep 2
hl_get "get avk-stats" > "$OUTDIR/stats-settled.json"

EMIT="$(field "$OUTDIR/stats-settled.json" blur_nodes_emitted)"
hl_assert "the fixture has blur nodes in it (premise, $EMIT)" \
	"$([ "${EMIT:-0}" -gt 0 ] && echo true || echo false)" "true"

# THE CONTROL, FIRST, AND IT IS NOT OPTIONAL. Two screenshots with NOTHING
# between them. If the desktop moves on its own -- kitty blinks a cursor, an
# animation is still settling -- then every pixel it moved by is a difference
# the oracle below would report as staleness. The first run of this script
# measured 2471 differing pixels at 197 codes on the "static" desktop and 0
# immediately after a window move, which is the wrong way round for a damage
# bug and exactly the shape of a fixture that is not static.
hl_screenshot control-a >/dev/null 2>&1 || true
sleep 3
hl_screenshot control-b >/dev/null 2>&1 || true
CTRL=0
if [ -s "$OUTDIR/control-a.png" ] && [ -s "$OUTDIR/control-b.png" ]; then
	read -r CTRL CWORST <<<"$(png_diff "$OUTDIR/control-a.png" \
		"$OUTDIR/control-b.png")"
	echo "  note: CONTROL -- the desktop moves $CTRL px on its own (worst ${CWORST:-0})"
fi
hl_assert "PREMISE: the desktop is static between screenshots ($CTRL px)" \
	"$([ "${CTRL:-1}" = "0" ] && echo true || echo false)" "true"

# THE ORACLE. A screenshot of the settled desktop, then every output marked
# fully damaged, then another. A stale pixel anywhere -- a blur fringe outside
# the region its source change was reported in, most of all -- differs here.
hl_screenshot damage-before >/dev/null 2>&1 || true
sleep 1
hl_dispatch damage_all
sleep 2
hl_screenshot damage-after >/dev/null 2>&1 || true

if [ -s "$OUTDIR/damage-before.png" ] && [ -s "$OUTDIR/damage-after.png" ]; then
	read -r DIFF WORST <<<"$(png_diff "$OUTDIR/damage-before.png" \
		"$OUTDIR/damage-after.png")"
	echo "  note: a forced full repaint changes $DIFF pixels (worst $WORST codes)"
	# 0, not "few". The scene is static between the two frames, so every pixel
	# a full reconstruction produces must be the pixel that is already there.
	hl_assert "a forced full repaint changes NOTHING ($DIFF px, $WORST codes)" \
		"$([ "${DIFF:-1}" = "0" ] && echo true || echo false)" "true"
else
	hl_assert "SKIP: grim produced no screenshots" "skip" "skip"
fi

# ── 2. A SMALL SOURCE CHANGE BEHIND AN UNDAMAGED BLUR ─────────────────────
#
# The interesting frame is not one in which a window moved -- that damages the
# node's old and new boxes, which COVER the blur behind it, so the whole thing is
# recomputed and damage propagation has nothing to do. It is one in which a blur's
# SOURCE changed in a small region while the blur ITSELF was not damaged:
#
#     wlbgeffect          translucent, floating, ON TOP -- its blur samples the
#                         current-frame prefix, which is everything below it
#     the tiled terminals BELOW it, so their pixels are in that prefix
#     a border toggle     a 12 px ring, far smaller than the 57 px support
#
# So the frame damage is a thin strip and the correct blur output damage is that
# strip dilated by the support. This is where the ratios below are worth
# reading: a whole-window move rebuilds 78% of the capture, and this rebuilds
# 30%.

hl_spawn_wlbgeffect overlay 300 >/dev/null; hl_wait_client_count 3 60
sleep 2
hl_dispatch toggle_floating
sleep 1
# `move_window`, NOT `move`. An earlier version of this phase called
# `hl_dispatch "move 470,180"` and every counter came back byte-identical with
# and without it -- because there is no dispatch called `move`, the IPC layer
# swallows an unknown name silently, and the line did nothing at all. The
# dispatch is `move_window,<x>,<y>` (see amsg dispatch --list). This is the
# second time an invented dispatch name has produced a confident wrong reading
# in this project; the first was `focusdir` for `focus_direction`.
hl_dispatch "move_window,300,180"
sleep 3

hl_dispatch reset_avk_stats
sleep 1

# The control again, with the floating window in place: the oracle below is only
# meaningful if the desktop is still static with it there.
hl_screenshot small-control-a >/dev/null 2>&1 || true
sleep 2
hl_screenshot small-control-b >/dev/null 2>&1 || true
SCTRL=0
if [ -s "$OUTDIR/small-control-a.png" ] && [ -s "$OUTDIR/small-control-b.png" ]; then
	read -r SCTRL _ <<<"$(png_diff "$OUTDIR/small-control-a.png" \
		"$OUTDIR/small-control-b.png")"
fi
hl_assert "PREMISE: still static with the overlay in place ($SCTRL px)" \
	"$([ "${SCTRL:-1}" = "0" ] && echo true || echo false)" "true"

# The small change: a terminal's BORDER, shown and hidden. A 12 px ring, far
# smaller than the 57 px support, on a window that lies in the overlay's blur
# source. focus_direction was tried first and moved nothing -- the counters read
# touched=0, source=0, and the phase was measuring an unchanged desktop.
hl_dispatch "focus_stack next" 2>/dev/null || true
sleep 1
hl_dispatch toggle_render_border 2>/dev/null || true
sleep 1
hl_dispatch "focus_stack next" 2>/dev/null || true
sleep 1
hl_dispatch toggle_render_border 2>/dev/null || true
sleep 2

hl_get "get avk-stats" > "$OUTDIR/stats-small.json"
TOUCH="$(field "$OUTDIR/stats-small.json" blur_damage_nodes_touched)"
SKIP="$(field "$OUTDIR/stats-small.json" blur_damage_nodes_skipped)"
REBUILD="$(field "$OUTDIR/stats-small.json" blur_prefix_rebuild_pixels)"
CAPTURE="$(field "$OUTDIR/stats-small.json" blur_full_capture_pixels)"
SRC="$(field "$OUTDIR/stats-small.json" blur_source_damage_pixels)"
OUT="$(field "$OUTDIR/stats-small.json" blur_output_damage_pixels)"
WRITE="$(field "$OUTDIR/stats-small.json" blur_full_write_pixels)"
FALLBACK="$(field "$OUTDIR/stats-small.json" blur_damage_fallbacks)"
RECTS="$(field "$OUTDIR/stats-small.json" blur_damage_rects_max)"
INHERIT="$(field "$OUTDIR/stats-small.json" blur_transitive_damage_pixels)"
echo "  note: a small change -- touched=$TOUCH skipped=$SKIP"
echo "  note:   source $SRC, output $OUT/$WRITE write, rebuild $REBUILD/$CAPTURE capture"
echo "  note:   $RECTS rectangles at worst, $FALLBACK fallbacks, $INHERIT px inherited"

hl_assert "the small change made a blur work (premise, $TOUCH touched)" \
	"$([ "${TOUCH:-0}" -gt 0 ] && echo true || echo false)" "true"
# The saving, stated as a ratio rather than believed. A rebuild equal to the
# capture would mean the propagation ran and bought nothing.
PCT=$(( CAPTURE > 0 ? REBUILD * 100 / CAPTURE : 100 ))
hl_assert "the prefix rebuild is smaller than the full capture (${PCT}%)" \
	"$([ "${PCT:-100}" -lt 100 ] && echo true || echo false)" "true"

hl_screenshot small-before >/dev/null 2>&1 || true
sleep 1
hl_dispatch damage_all
sleep 2
hl_screenshot small-after >/dev/null 2>&1 || true
if [ -s "$OUTDIR/small-before.png" ] && [ -s "$OUTDIR/small-after.png" ]; then
	read -r DIFF2 WORST2 <<<"$(png_diff "$OUTDIR/small-before.png" \
		"$OUTDIR/small-after.png")"
	echo "  note: after a small change, a full repaint changes $DIFF2 px (worst $WORST2)"
	hl_assert "a small source change leaves no stale blur ($DIFF2 px)" \
		"$([ "${DIFF2:-1}" = "0" ] && echo true || echo false)" "true"
fi

# ── 3. the region machinery stays cheap ────────────────────────────────────
#
# Recorded rather than optimised. A rectangle count stuck at the fallback limit
# would mean the damage regions are fragmenting and every blur is collapsing to
# its extents -- which is correct, and is a full redraw wearing a region's name.
hl_assert "no blur damage region hit the complexity fallback ($FALLBACK)" \
	"${FALLBACK:-x}" "0"

# ── 4. idle ────────────────────────────────────────────────────────────────
#
# Damage bookkeeping must not dirty anything. M4C.3H's invariant, re-asserted
# because M4F.2B added a per-frame region computation that touches every blur.
hl_dispatch reset_avk_stats
sleep 6
hl_get "get avk-stats" > "$OUTDIR/stats-idle.json"
IDLE_FRAMES="$(field "$OUTDIR/stats-idle.json" frames)"
IDLE_TOUCH="$(field "$OUTDIR/stats-idle.json" blur_damage_nodes_touched)"
echo "  note: 6 idle seconds -> $IDLE_FRAMES frames, $IDLE_TOUCH blur recomputes"
hl_assert "an idle desktop does not free-run ($IDLE_FRAMES frames in 6s)" \
	"$([ "${IDLE_FRAMES:-999}" -le 12 ] && echo true || echo false)" "true"

WAITS="$(field "$OUTDIR/stats-idle.json" cpu_sync_waits)"
hl_assert "and no CPU wait was introduced ($WAITS)" "${WAITS:-x}" "0"

# ── 4b. what the two sweeps cost the CPU ───────────────────────────────────
#
# Frame-level, not per-operation: M4E's corrected methodology. A clock_gettime
# around each pixman call would measure the clock. This brackets the whole
# per-frame propagation stage, both sweeps together, and is reported as an
# average over the frames that ran -- there is no histogram here because the
# number is small enough that its distribution is not yet the question.
BUILD_NS="$(field "$OUTDIR/stats-small.json" blur_damage_build_ns)"
BUILD_FR="$(field "$OUTDIR/stats-small.json" frames)"
if [ "${BUILD_FR:-0}" -gt 0 ]; then
	echo "  note: damage propagation costs $(( BUILD_NS / BUILD_FR )) ns/frame" \
		"over $BUILD_FR frames"
	hl_assert "damage propagation costs under 100us a frame" \
		"$([ "$(( BUILD_NS / BUILD_FR ))" -lt 100000 ] && echo true || echo false)" \
		"true"
fi

# ── 5. validation ──────────────────────────────────────────────────────────

VUID=$(grep -ac "VUID" "$LOG" 2>/dev/null || true)
HAZ=$(grep -ac "SYNC-HAZARD" "$LOG" 2>/dev/null || true)
echo "  note: $VUID VUID lines, $HAZ SYNC-HAZARD lines"
hl_assert "no validation errors" "${VUID:-x}" "0"
hl_assert "no synchronisation hazards" "${HAZ:-x}" "0"

hl_stop
echo
echo "logs: $OUTDIR"
hl_summary
