#!/usr/bin/env bash
# avk-cursor-content-test.sh — a cursor is a surface, and surfaces change.
#
# contrib/avk-cursor-test.sh asks whether a client's cursor reaches the screen
# at all. This one asks the three questions that only arise once it does:
#
#   SCALE       a cursor surface has its own buffer scale. A 64x64 buffer at
#               scale 2 is a 32x32 cursor with a hotspot in 32x32 units, and
#               getting that wrong gives a pointer that is twice the size it
#               should be and offset by half of itself.
#
#   CONTENT     a cursor that animates by rewriting ONE wl_buffer must keep
#               updating. This is M3.5D.1's content model applied to the
#               cursor, and it has two independent places to go wrong:
#               AVK's own image cache, and wlr_cursor_set_buffer(), which
#               early-returns when the buffer pointer, hotspot and scale all
#               match (wlr_cursor.c:520) and would otherwise freeze the
#               hardware plane on the first image forever.
#
#   HANDOVER    the hardware plane is the fast path and must stay it, so the
#               same cursor has to work on the plane and composited by AVK,
#               and cross between them.
#
# One compositor run covers all three because one client can exhibit all three:
# a scaled, animating, buffer-reusing cursor.
#
# WHAT THIS CANNOT SEE, AND SAYS SO
#
# The forced re-import exists for the HARDWARE PLANE -- it is what stops
# wlr_cursor_set_buffer()'s early return from freezing a plane-carried cursor.
# A headless output has no plane (backend/headless/output.c:82 is
# `return true;` and does nothing), so its effect is unobservable in pixels
# here and only the counter can be asserted. Whether the plane itself keeps up
# is a live-only observation.
#
# Break tests, each of which MUST fail:
#
#   BREAK=cursor-generation  AZ_AVK_CACHE_BY_IDENTITY=1 -- cache the cursor
#                            image on the buffer pointer. The client rewrites
#                            one buffer, so the cursor freezes on its first
#                            colour while the client goes on committing.
#   BREAK=cursor-hotspot     AZ_AVK_NO_CURSOR_HOTSPOT=1 -- draw at the pointer
#                            rather than at the pointer minus the hotspot. The
#                            whole image is still there and still the right
#                            size, so only the placement assertion can catch
#                            it.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-cursor-content"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-cursor-content-test: needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "avk-cursor-content-test: needs python3-pillow"; exit 1; }

WLCURSOR="$(dirname "$0")/wlcursor/wlcursor"
WLSHOT="$(dirname "$0")/wlshot/wlshot"
WLVPTR="$(dirname "$0")/wlvptr/wlvptr"
for tool in "$WLCURSOR" "$WLSHOT" "$WLVPTR"; do
	[ -x "$tool" ] || {
		echo "avk-cursor-content-test: $tool not built -- run: cd $(dirname "$tool") && make" >&2
		exit 1
	}
done

SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-cursorcontent-$$"
HL_OUTDIR="$OUTDIR"
HL_WIDTH=1280 HL_HEIGHT=720
HL_ENV=""
[ "$BREAK" = cursor-generation ] && HL_ENV="$HL_ENV AZ_AVK_CACHE_BY_IDENTITY=1"
[ "$BREAK" = cursor-hotspot ] && HL_ENV="$HL_ENV AZ_AVK_NO_CURSOR_HOTSPOT=1"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

PX=640
PY=360
# A 64x64 buffer at scale 2 is a 32x32 cursor. The hotspot is stated in
# SURFACE-local units, so 8,8 here means 16,16 in the buffer.
CURSOR_BUF=64
CURSOR_SCALE=2
EXPECT_SIZE=$(( CURSOR_BUF / CURSOR_SCALE ))
HOTSPOT=8
COLOUR_A="ff0000"
COLOUR_B="0000ff"

field() { python3 - "$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}

# The bounding box of anything close to a colour, and how many such pixels.
#
# Tolerant on purpose: a 64x64 buffer drawn into a 32x32 box is filtered, so
# the edge pixels blend toward whatever is behind them and an exact-match count
# would measure the filter rather than the geometry. The interior stays the
# flat colour, which is what carries the assertion.
colour_box() { # colour_box PNG "rrggbb" -> "count minx miny maxx maxy"
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
want = tuple(int(sys.argv[2][i:i+2], 16) for i in (0, 2, 4))
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
xs, ys, n = [], [], 0
for y in range(im.size[1]):
    for x in range(im.size[0]):
        c = px[x, y]
        if max(abs(c[i] - want[i]) for i in range(3)) <= 40:
            xs.append(x); ys.append(y); n += 1
print("%d %d %d %d %d" % (n, min(xs) if xs else -1, min(ys) if ys else -1,
    max(xs) if xs else -1, max(ys) if ys else -1))
PY
}

hl_start "$SCENE_KDL"

"$WLCURSOR" --size 800x600 --colour 202020 \
	--cursor-size "${CURSOR_BUF}x${CURSOR_BUF}" \
	--cursor-scale "$CURSOR_SCALE" \
	--hotspot "$HOTSPOT,$HOTSPOT" \
	--cursor-colour "$COLOUR_A" --cursor-colour "$COLOUR_B" \
	--reuse-buffer --animate-ms 2000 --animate-once > "$OUTDIR/wlcursor.log" 2>&1 &
WLCURSOR_PID=$!
HL_SPAWNED_PIDS+=("$WLCURSOR_PID")
hl_wait_client_count 1 60
sleep 2

"$WLVPTR" "$PX" "$PY" "$HL_WIDTH" "$HL_HEIGHT"
sleep 1

# ── the plane, before anything forces software ─────────────────────────────
# Mapping the window and painting the wallpaper produced frames, and through
# all of them the cursor was on the (notional) hardware plane. Read this
# BEFORE the first cursored capture, because that capture is what switches the
# output to software cursors and it never switches back on its own.
hl_get "get avk-stats" > "$OUTDIR/stats-hw.json"
HW="$(field "$OUTDIR/stats-hw.json" hardware_cursor_frames)"
SW_BEFORE="$(field "$OUTDIR/stats-hw.json" software_cursor_frames)"

echo "-- the hardware plane carries it until something needs it composited --"
echo "  note: hardware_cursor_frames=$HW software_cursor_frames=$SW_BEFORE"
hl_assert "the plane carried the cursor on earlier frames ($HW)" \
	"$([ "${HW:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "and AVK composited none of them ($SW_BEFORE)" \
	"$([ "${SW_BEFORE:-1}" -eq 0 ] && echo true || echo false)" "true"

# ── scale ──────────────────────────────────────────────────────────────────
"$WLSHOT" --cursor "$OUTDIR/gen1.png" 2>>"$OUTDIR/wlshot.log"
[ -f "$OUTDIR/gen1.png" ] || { hl_stop; echo "capture failed" >&2; exit 1; }

read -r N1 MINX MINY MAXX MAXY <<<"$(colour_box "$OUTDIR/gen1.png" "$COLOUR_A")"
read -r N1B _ _ _ _ <<<"$(colour_box "$OUTDIR/gen1.png" "$COLOUR_B")"
W1=$(( MAXX - MINX + 1 ))
H1=$(( MAXY - MINY + 1 ))
echo "-- a scale-2 cursor surface is half the size of its buffer --"
echo "  note: ${N1}px of $COLOUR_A, box ${MINX},${MINY} ${W1}x${H1}"

hl_assert "the first cursor generation is on screen" \
	"$([ "${N1:-0}" -gt 100 ] && echo true || echo false)" "true"
# The substantive scale claim: a 64x64 buffer at scale 2 occupies 32x32.
# Drawn at the buffer's own size it would be 64x64 -- not a subtle difference.
hl_assert "it occupies ${EXPECT_SIZE}x${EXPECT_SIZE}, not ${CURSOR_BUF}x${CURSOR_BUF} (${W1}x${H1})" \
	"$([ "${W1:-0}" -ge $(( EXPECT_SIZE - 1 )) ] && [ "${W1:-0}" -le $(( EXPECT_SIZE + 1 )) ] \
		&& [ "${H1:-0}" -ge $(( EXPECT_SIZE - 1 )) ] && [ "${H1:-0}" -le $(( EXPECT_SIZE + 1 )) ] \
		&& echo true || echo false)" "true"
# And the hotspot is in surface units too: 8,8 of a 32x32 cursor, not 8,8 of
# the 64x64 buffer and not 16,16 of anything.
hl_assert "with its hotspot ${HOTSPOT} surface px in (origin ${MINX},${MINY})" \
	"$([ "${MINX:--1}" -ge $(( PX - HOTSPOT - 1 )) ] && [ "${MINX:-0}" -le $(( PX - HOTSPOT + 1 )) ] \
		&& [ "${MINY:--1}" -ge $(( PY - HOTSPOT - 1 )) ] && [ "${MINY:-0}" -le $(( PY - HOTSPOT + 1 )) ] \
		&& echo true || echo false)" "true"

# ── content, under one unchanging wl_buffer ────────────────────────────────
# Past the animation period the client has rewritten the SAME buffer with the
# second colour. Nothing about the buffer identity changed -- not the pointer,
# not the pool, not the fd -- so anything caching on identity is now showing a
# picture the client stopped drawing seconds ago.
sleep 3
"$WLSHOT" --cursor "$OUTDIR/gen2.png" 2>>"$OUTDIR/wlshot.log"
read -r N2A _ _ _ _ <<<"$(colour_box "$OUTDIR/gen2.png" "$COLOUR_A")"
read -r N2B _ _ _ _ <<<"$(colour_box "$OUTDIR/gen2.png" "$COLOUR_B")"
hl_get "get avk-stats" > "$OUTDIR/stats-sw.json"
SW="$(field "$OUTDIR/stats-sw.json" software_cursor_frames)"
NOIMG="$(field "$OUTDIR/stats-sw.json" cursor_no_image)"

echo "-- a cursor that rewrites one buffer still changes on screen --"
echo "  note: capture 1 had ${N1}px $COLOUR_A / ${N1B}px $COLOUR_B; capture 2 has ${N2A}px / ${N2B}px"

# Stated as a CHANGE between the two captures, not as "the second one is blue".
#
# That distinction is not pedantry. Run against BREAK=cursor-generation the
# cursor freezes on whatever colour happened to be current when the image was
# first uploaded -- which, with an animation already running, was blue. "The
# second capture is blue" passed perfectly while the cursor had been frozen
# from the start and the client's commits were going nowhere. An assertion
# about a change has to name both endpoints.
hl_assert "capture 1 was $COLOUR_A and not $COLOUR_B" \
	"$([ "${N1:-0}" -gt 100 ] && [ "${N1B:-1}" -lt 100 ] && echo true || echo false)" "true"
hl_assert "and capture 2 is $COLOUR_B and not $COLOUR_A" \
	"$([ "${N2B:-0}" -gt 100 ] && [ "${N2A:-1}" -lt 100 ] && echo true || echo false)" "true"

# ── handover ───────────────────────────────────────────────────────────────
echo "-- and AVK took it over when the plane was locked out --"
echo "  note: software_cursor_frames=$SW cursor_no_image=$NOIMG"
hl_assert "AVK composited the cursor once software was forced ($SW)" \
	"$([ "${SW:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "and never lacked an image to draw ($NOIMG)" \
	"$([ "${NOIMG:-1}" -eq 0 ] && echo true || echo false)" "true"

kill "$WLCURSOR_PID" 2>/dev/null
hl_stop

echo
echo "screenshots: $OUTDIR/gen1.png $OUTDIR/gen2.png"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
