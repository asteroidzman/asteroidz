#!/usr/bin/env bash
# avk-cursor-test.sh — does a client's own cursor image reach the screen, and
# is it in the right place?
#
# M3.5E's first acceptance test. Three cursor sources exist and they do not
# share a path (docs/vulkan-native-architecture.md §5.4g):
#
#   xcursor        wlr_xcursor_image_get_buffer() -> a wlr_buffer
#   cursor-shape   mapped onto xcursor, so the same
#   client surface wlr_surface_get_texture()      -> a wlr_texture
#
# The third one broke when AVK stopped giving wl_compositor a renderer.
# wlr_surface_get_texture() reads surface->buffer, the wlr_client_buffer
# wrapper that only exists when the compositor has a renderer; with none it is
# NULL, output_cursor_set_texture() disables the cursor outright, and NOTHING
# LOGS IT. Not a slow cursor -- no cursor, hardware plane and software alike.
#
# The whole suite ran green through that, because no other client in contrib/
# sets a cursor. contrib/wlcursor exists for this and nothing else.
#
# TWO THINGS MAKE THIS OBSERVABLE AT ALL
#
#   1. grim asks screencopy for overlay_cursor = 0, so its output never
#      contains a cursor and every assertion on one would be vacuous.
#      contrib/wlshot asks for the cursor.
#
#   2. The headless backend implements set_cursor() as `return true;` and does
#      nothing, so it believes it has a hardware cursor plane and puts every
#      cursor on a plane that does not exist. Asking screencopy for the cursor
#      calls wlr_output_lock_software_cursors(), which is what forces the
#      cursor to be composited into the frame instead.
#
# So the two captures differ in exactly one thing, and the pair is the test: a
# capture without the cursor must NOT contain the cursor colour, and a capture
# with it must. Asserting only the second would pass against a client whose
# window happened to be green.
#
# Break tests, each of which MUST fail:
#
#   BREAK=cursor-texture   AZ_CURSOR_LEGACY_SURFACE=1 -- restore the original
#                          wlr_cursor_set_surface() call. Verified: the cursor
#                          vanishes and every image assertion fails, with
#                          wlr_surface_get_texture() returning NULL exactly as
#                          the audit predicted.
#   BREAK=cursor-command   AZ_AVK_NO_CURSOR=1 -- AVK emits no cursor draw.
#                          Nothing else is drawing into this frame, so the
#                          pointer disappears. This is what separates "AVK
#                          draws the cursor" from "a cursor appeared and nobody
#                          asked who put it there" -- which was true of every
#                          frame before, when the whole output fell back to
#                          SceneFX the moment a software cursor existed.
#   BREAK=cursor-upload    AZ_AVK_UPLOAD_ON_LOOKUP=1 -- re-upload every buffer
#                          the frame looks at, including the cursor's. Moving
#                          the pointer then costs a copy per motion.
#   BREAK=cursor-damage    AZ_AVK_FULL_DAMAGE=1 -- repaint everything, every
#                          frame. Honest about its scope: it breaks damage
#                          globally rather than only for the cursor, and it is
#                          here because the per-frame damage assertion has to
#                          be falsifiable by something.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-cursor"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-cursor-test: needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "avk-cursor-test: needs python3-pillow"; exit 1; }

WLCURSOR="$(dirname "$0")/wlcursor/wlcursor"
WLSHOT="$(dirname "$0")/wlshot/wlshot"
WLVPTR="$(dirname "$0")/wlvptr/wlvptr"
for tool in "$WLCURSOR" "$WLSHOT" "$WLVPTR"; do
	[ -x "$tool" ] || {
		echo "avk-cursor-test: $tool not built -- run: cd $(dirname "$tool") && make" >&2
		exit 1
	}
done

SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-cursor-$$"
HL_OUTDIR="$OUTDIR"
HL_WIDTH=1280 HL_HEIGHT=720
HL_ENV=""
[ "$BREAK" = cursor-texture ] && HL_ENV="$HL_ENV AZ_CURSOR_LEGACY_SURFACE=1"
[ "$BREAK" = cursor-command ] && HL_ENV="$HL_ENV AZ_AVK_NO_CURSOR=1"
[ "$BREAK" = cursor-damage ] && HL_ENV="$HL_ENV AZ_AVK_FULL_DAMAGE=1"
# BOTH switches, and the second is not redundant. AZ_AVK_UPLOAD_ON_LOOKUP
# restores the unconditional upload CALL, but D.1 Phase 2 made the copy itself
# damage-driven, so the call finds no pending damage and copies nothing --
# az_avk_upload_shm() returns early at rect_count == 0. AZ_AVK_SOURCE_FULL is
# what makes it copy the whole buffer again. Either alone leaves this test
# passing, which is a fact worth knowing about the D.1 switches generally.
[ "$BREAK" = cursor-upload ] && HL_ENV="$HL_ENV AZ_AVK_UPLOAD_ON_LOOKUP=1 AZ_AVK_SOURCE_FULL=1"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

# Where the pointer is put, and therefore where the hotspot must land.
PX=640
PY=360
CURSOR_COLOUR="00ff00"
HOTSPOT_X=4
HOTSPOT_Y=4
CURSOR_SIZE=32

count_colour() { # count_colour PNG "#rrggbb" -> pixel count
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
want = tuple(int(sys.argv[2][i:i+2], 16) for i in (1, 3, 5))
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(n for n, c in im.getcolors(1 << 24) if c == want))
PY
}

# The bounding box of a colour, plus where the single black marker pixel is.
# wlcursor paints one opaque-black pixel at its hotspot precisely so this can
# assert position and not merely presence.
cursor_geometry() { # cursor_geometry PNG "#rrggbb" -> "minx miny maxx maxy markx marky"
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
want = tuple(int(sys.argv[2][i:i+2], 16) for i in (1, 3, 5))
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
xs, ys, mark = [], [], (-1, -1)
for y in range(im.size[1]):
    for x in range(im.size[0]):
        c = px[x, y]
        if c == want:
            xs.append(x); ys.append(y)
        elif c == (0, 0, 0):
            mark = (x, y)
if not xs:
    print("-1 -1 -1 -1 %d %d" % mark)
else:
    print("%d %d %d %d %d %d" % (min(xs), min(ys), max(xs), max(ys), mark[0], mark[1]))
PY
}

hl_start "$SCENE_KDL"

"$WLCURSOR" --size 800x600 --colour 202020 \
	--cursor-size "${CURSOR_SIZE}x${CURSOR_SIZE}" \
	--cursor-colour "$CURSOR_COLOUR" \
	--hotspot "$HOTSPOT_X,$HOTSPOT_Y" > "$OUTDIR/wlcursor.log" 2>&1 &
WLCURSOR_PID=$!
HL_SPAWNED_PIDS+=("$WLCURSOR_PID")
hl_wait_client_count 1 60
sleep 2

# A client may only set a cursor from inside a pointer enter, so the pointer
# has to be moved onto the window first. The headless seat has no pointer at
# all until this creates a virtual one, which is why wlcursor waits for the
# capability rather than requiring it at startup.
"$WLVPTR" "$PX" "$PY" "$HL_WIDTH" "$HL_HEIGHT"
sleep 1

# ── the pair of captures ───────────────────────────────────────────────────
# Order matters, and not for a subtle reason: wlroots deliberately does NOT
# re-enable the hardware cursor the instant a software-cursor lock is dropped
# ("a recorder is likely to lock software cursors for the next frame again",
# output/cursor.c:75). So a cursorless capture taken AFTER a cursored one
# still has the cursor composited in, and the control would silently become a
# copy of the experiment. Cursorless first.
"$WLSHOT" --no-cursor "$OUTDIR/without.png" 2>>"$OUTDIR/wlshot.log"
"$WLSHOT" --cursor "$OUTDIR/with.png" 2>>"$OUTDIR/wlshot.log"

if [ ! -f "$OUTDIR/with.png" ] || [ ! -f "$OUTDIR/without.png" ]; then
	hl_stop
	echo "avk-cursor-test: a capture failed -- see $OUTDIR/wlshot.log" >&2
	exit 1
fi

WITHOUT="$(count_colour "$OUTDIR/without.png" "#$CURSOR_COLOUR")"
WITH="$(count_colour "$OUTDIR/with.png" "#$CURSOR_COLOUR")"
read -r MINX MINY MAXX MAXY MARKX MARKY \
	<<<"$(cursor_geometry "$OUTDIR/with.png" "#$CURSOR_COLOUR")"

echo "-- a client's own cursor image reaches the screen --"
echo "  note: cursor colour px without=$WITHOUT with=$WITH; box ${MINX},${MINY}..${MAXX},${MAXY}; hotspot marker ${MARKX},${MARKY}"

# The premise. If the cursor colour were already on screen for some other
# reason -- the window, the wallpaper -- everything below would pass without
# a cursor existing at all.
hl_assert "the scene does not contain the cursor colour on its own" \
	"$([ "${WITHOUT:-1}" -eq 0 ] && echo true || echo false)" "true"

# The assertion this milestone exists for. 32x32 minus the one marker pixel.
EXPECT=$(( CURSOR_SIZE * CURSOR_SIZE - 1 ))
hl_assert "the client's cursor is composited, whole ($WITH of $EXPECT px)" \
	"$([ "${WITH:-0}" -eq "$EXPECT" ] && echo true || echo false)" "true"

# Presence is not placement. A compositor that ignores the hotspot draws the
# entire image, correctly, one hotspot away from where it belongs -- and passes
# every count-based assertion. The marker pixel is the only thing that can tell
# the difference.
hl_assert "its hotspot sits exactly on the pointer (${MARKX},${MARKY} vs ${PX},${PY})" \
	"$([ "${MARKX:--1}" -eq "$PX" ] && [ "${MARKY:--1}" -eq "$PY" ] \
		&& echo true || echo false)" "true"

# And the image is placed relative to the hotspot rather than merely centred on
# it or aligned to it, which the box origin is what proves.
hl_assert "and the image starts hotspot-many pixels before it ($MINX,$MINY)" \
	"$([ "${MINX:--1}" -eq $(( PX - HOTSPOT_X )) ] \
		&& [ "${MINY:--1}" -eq $(( PY - HOTSPOT_Y )) ] \
		&& echo true || echo false)" "true"

# ── who drew it, and how much did it cost ──────────────────────────────────
# Everything above would pass identically if the frame had fallen back to
# SceneFX, which is exactly what happened before AVK drew cursors itself. The
# counters are the only thing that can tell the two apart.
#
# The capture above left software cursors on: wlroots does not re-enable the
# hardware cursor the moment a lock drops. So the pointer can now be moved
# around with the software path live, which is both the ownership measurement
# and the damage one.
echo "-- AVK composites it, and moving it does not repaint the screen --"
hl_dispatch reset_avk_stats
for i in 1 2 3 4 5 6 7 8; do
	"$WLVPTR" $(( 300 + i * 40 )) $(( 200 + i * 30 )) "$HL_WIDTH" "$HL_HEIGHT"
	sleep 0.2
done
sleep 1
hl_get "get avk-stats" > "$OUTDIR/stats-move.json"

field() { python3 - "$OUTDIR/stats-move.json" "$1" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}
FRAMES="$(field frames)"
FALLBACK="$(field fallback_frames)"
SW="$(field software_cursor_frames)"
NOIMG="$(field cursor_no_image)"
IMPFAIL="$(field cursor_import_failures)"
DMG="$(field damage_pixels)"
UPBYTES="$(( $(field shm_upload_bytes) ))"
PERFRAME="$(python3 -c "print(int($DMG/$FRAMES) if '$FRAMES'.isdigit() and $FRAMES else -1)" 2>/dev/null || echo -1)"
echo "  note: $FRAMES frames ($FALLBACK fallback), $SW with an AVK cursor, "\
"$NOIMG no-image, $IMPFAIL import failures, ${PERFRAME} damaged px/frame"

# Premise: the pointer really did move and produce frames.
hl_assert "moving the pointer produced frames" \
	"$([ "${FRAMES:-0}" -gt 0 ] && echo true || echo false)" "true"
# Ownership. AVK, not SceneFX, and not a plane.
hl_assert "AVK composited the cursor on those frames ($SW of $FRAMES)" \
	"$([ "${SW:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "without falling back to SceneFX ($FALLBACK)" \
	"$([ "${FALLBACK:-1}" -eq 0 ] && echo true || echo false)" "true"
# The regression's fingerprint: wlroots says visible, asteroidz has no picture.
hl_assert "and never ran out of a cursor image to draw" \
	"$([ "${NOIMG:-1}" -eq 0 ] && [ "${IMPFAIL:-1}" -eq 0 ] \
		&& echo true || echo false)" "true"
# Cost. A moving software cursor damages the rectangle it left and the one it
# entered -- about two 32x32 boxes. A compositor that repaints the whole output
# for every pointer motion is correct and unusable, and only this can tell.
FULL=$(( HL_WIDTH * HL_HEIGHT ))
hl_assert "repainting a small fraction of the output ($PERFRAME of $FULL px)" \
	"$([ "${PERFRAME:--1}" -ge 0 ] \
		&& [ "${PERFRAME:-0}" -lt $(( FULL / 10 )) ] \
		&& echo true || echo false)" "true"
# Moving a cursor changes where its pixels go, not what they are. The image is
# already on the GPU and must stay there: re-uploading a 32x32 cursor on every
# motion event is small in isolation and is exactly the shape of the bug D.1
# removed for wallpapers, at pointer rates.
hl_assert "and re-uploading nothing to move it (${UPBYTES} B)" \
	"$([ "${UPBYTES:-999999}" -lt 8192 ] && echo true || echo false)" "true"

kill "$WLCURSOR_PID" 2>/dev/null
hl_stop

echo
echo "screenshots: $OUTDIR/with.png $OUTDIR/without.png"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
