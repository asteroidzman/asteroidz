#!/usr/bin/env bash
# avk-cursor-hide-test.sh — what is underneath a cursor when it goes away?
#
# "Hiding a cursor" sounds like it has one failure mode (it stays visible) and
# actually has four, three of which look fine in a screenshot taken a moment
# too late:
#
#   GHOST          the image is gone from the compositor's state but the
#                  pixels are still in the output buffer, because nothing
#                  damaged the rectangle it used to occupy. Under partial
#                  damage that stale rectangle survives until something else
#                  happens to overlap it.
#
#   OVER-DAMAGE    the rectangle is cleaned up by repainting the entire
#                  output. Correct, invisible in any pixel comparison, and
#                  ruinous at pointer rates.
#
#   COLLATERAL     the rectangle is repainted with something other than what
#                  was underneath it.
#
#   STALE IMAGE    the cursor comes back, or changes to a different one, and
#                  the OLD picture is what appears -- the M3.5D.1 failure, on
#                  the cursor.
#
# So the assertions here are all differential: capture the same scene with the
# cursor, without it, and with it again, and require the frames to differ ONLY
# where they should. A single capture cannot express any of this.
#
# Three sequences, in one compositor run, all driven from one client:
#
#   visible -> hidden           the old rectangle must be restored EXACTLY,
#                               and nothing else may change
#   hidden  -> visible again    the same image, in the same place
#   A -> NULL -> B              a different cursor afterwards, not the old one
#
# The pointer has to stay alive throughout. contrib/wlvptr normally moves the
# pointer and exits, which destroys the virtual pointer device and with it the
# seat's pointer capability -- and a set_cursor request carries an enter serial
# the compositor checks against the currently focused pointer client, which by
# then is nobody. `hold:<ms>` keeps it present.
#
# ON OLD-POSITION DAMAGE, STATED HONESTLY
#
# AVK does not compute it. When a software cursor is hidden or moves, wlroots
# emits wlr_output.events.damage for the rectangle it vacated and scenefx feeds
# that into the same ring AVK rotates out of. AVK consumes that damage; it does
# not own the calculation. There is therefore NO BREAK=cursor-old-damage here,
# because a switch that disabled it would be disabling wlroots' work through a
# hole punched in AVK for the purpose -- a test of a fabrication rather than of
# the compositor. The assertions below observe the RESULT, which is the honest
# thing this test can do.
#
# Break tests, each of which MUST fail:
#
#   BREAK=cursor-command   AZ_AVK_NO_CURSOR=1 -- draw no cursor ever. The
#                          "visible" captures stop containing one, so the
#                          premise assertions fail and nothing below them
#                          means anything.
#   BREAK=cursor-generation  AZ_AVK_CACHE_BY_IDENTITY=1 -- cache on the buffer
#                          pointer. The A -> NULL -> B sequence comes back
#                          showing A.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-cursor-hide"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-cursor-hide-test: needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "avk-cursor-hide-test: needs python3-pillow"; exit 1; }

WLCURSOR="$(dirname "$0")/wlcursor/wlcursor"
WLSHOT="$(dirname "$0")/wlshot/wlshot"
WLVPTR="$(dirname "$0")/wlvptr/wlvptr"
for tool in "$WLCURSOR" "$WLSHOT" "$WLVPTR"; do
	[ -x "$tool" ] || {
		echo "avk-cursor-hide-test: $tool not built -- run: cd $(dirname "$tool") && make" >&2
		exit 1
	}
done

SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-cursorhide-$$"
HL_OUTDIR="$OUTDIR"
HL_WIDTH=1280 HL_HEIGHT=720
HL_ENV=""
[ "$BREAK" = cursor-command ] && HL_ENV="$HL_ENV AZ_AVK_NO_CURSOR=1"
[ "$BREAK" = cursor-generation ] && HL_ENV="$HL_ENV AZ_AVK_CACHE_BY_IDENTITY=1"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

PX=640
PY=360
COLOUR_A="00ff00"
COLOUR_B="ff00ff"
HOTSPOT=4
CURSOR_SIZE=32

count_colour() { python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
want = tuple(int(sys.argv[2][i:i+2], 16) for i in (0, 2, 4))
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(n for n, c in im.getcolors(1 << 24) if c == want))
PY
}

# How many pixels differ between two captures, and the bounding box of the
# difference. This is the measurement the whole test is built on: "the cursor
# went away" is only correct if the pixels that changed are exactly the ones
# the cursor covered.
diff_box() { # diff_box A.png B.png -> "count minx miny maxx maxy"
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
pa, pb = a.load(), b.load()
xs, ys, n = [], [], 0
for y in range(a.size[1]):
    for x in range(a.size[0]):
        if pa[x, y] != pb[x, y]:
            xs.append(x); ys.append(y); n += 1
print("%d %d %d %d %d" % (n, min(xs) if xs else -1, min(ys) if ys else -1,
    max(xs) if xs else -1, max(ys) if ys else -1))
PY
}

field() { python3 - "$OUTDIR/$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}

hl_start "$SCENE_KDL"

# Timings are all relative to the first pointer enter, which is when the client
# starts its clock. Generous gaps so a capture is never racing a transition.
"$WLCURSOR" --size 800x600 --colour 202020 \
	--cursor-size "${CURSOR_SIZE}x${CURSOR_SIZE}" \
	--cursor-colour "$COLOUR_A" --cursor-colour "$COLOUR_B" \
	--hotspot "$HOTSPOT,$HOTSPOT" \
	--hide-after-ms 3000 --reshow-after-ms 6000 --swap-after-ms 12000 \
	> "$OUTDIR/wlcursor.log" 2>&1 &
WLCURSOR_PID=$!
HL_SPAWNED_PIDS+=("$WLCURSOR_PID")
hl_wait_client_count 1 60
sleep 2

# Move the pointer in and KEEP IT THERE for the whole sequence.
"$WLVPTR" "$PX" "$PY" "$HL_WIDTH" "$HL_HEIGHT" hold:20000 &
WLVPTR_PID=$!
HL_SPAWNED_PIDS+=("$WLVPTR_PID")
sleep 1

# Force software cursors and keep them forced: the first cursored capture
# switches the output over, and wlroots does not switch back on its own
# (output/cursor.c:75).
"$WLSHOT" --cursor "$OUTDIR/warm.png" 2>>"$OUTDIR/wlshot.log"
sleep 1

hl_dispatch reset_avk_stats
"$WLSHOT" --cursor "$OUTDIR/a-visible.png" 2>>"$OUTDIR/wlshot.log"   # ~t+2s
sleep 3
"$WLSHOT" --cursor "$OUTDIR/b-hidden.png" 2>>"$OUTDIR/wlshot.log"    # ~t+5s
hl_get "get avk-stats" > "$OUTDIR/stats-hide.json"
sleep 3
"$WLSHOT" --cursor "$OUTDIR/c-reshown.png" 2>>"$OUTDIR/wlshot.log"   # ~t+8s
sleep 6
"$WLSHOT" --cursor "$OUTDIR/d-swapped.png" 2>>"$OUTDIR/wlshot.log"   # ~t+14s
hl_get "get avk-stats" > "$OUTDIR/stats-end.json"

for f in a-visible b-hidden c-reshown d-swapped; do
	[ -f "$OUTDIR/$f.png" ] || { hl_stop; echo "capture $f failed" >&2; exit 1; }
done

A_ON="$(count_colour "$OUTDIR/a-visible.png" "$COLOUR_A")"
B_ON="$(count_colour "$OUTDIR/b-hidden.png" "$COLOUR_A")"
C_ON="$(count_colour "$OUTDIR/c-reshown.png" "$COLOUR_A")"
D_A="$(count_colour "$OUTDIR/d-swapped.png" "$COLOUR_A")"
D_B="$(count_colour "$OUTDIR/d-swapped.png" "$COLOUR_B")"
EXPECT=$(( CURSOR_SIZE * CURSOR_SIZE - 1 ))

# ── visible -> hidden ──────────────────────────────────────────────────────
echo "-- hiding a cursor removes exactly the cursor --"
read -r DN DMINX DMINY DMAXX DMAXY <<<"$(diff_box "$OUTDIR/a-visible.png" "$OUTDIR/b-hidden.png")"
DW=$(( DMAXX - DMINX + 1 ))
DH=$(( DMAXY - DMINY + 1 ))
echo "  note: cursor px $A_ON -> $B_ON; ${DN} px differ, box ${DMINX},${DMINY} ${DW}x${DH}"

hl_assert "the cursor was on screen to begin with ($A_ON of $EXPECT)" \
	"$([ "${A_ON:-0}" -eq "$EXPECT" ] && echo true || echo false)" "true"
hl_assert "and is completely gone after set_cursor(NULL) ($B_ON)" \
	"$([ "${B_ON:-1}" -eq 0 ] && echo true || echo false)" "true"
# The ghost test. Everything the cursor covered must change back, and nothing
# else may change at all -- so the difference between the two frames is exactly
# the cursor's own rectangle.
hl_assert "and the pixels that changed are exactly its rectangle (${DW}x${DH})" \
	"$([ "${DW:-0}" -eq "$CURSOR_SIZE" ] && [ "${DH:-0}" -eq "$CURSOR_SIZE" ] \
		&& echo true || echo false)" "true"
hl_assert "at exactly where it was (${DMINX},${DMINY})" \
	"$([ "${DMINX:--1}" -eq $(( PX - HOTSPOT )) ] \
		&& [ "${DMINY:--1}" -eq $(( PY - HOTSPOT )) ] \
		&& echo true || echo false)" "true"
# And no more than the cursor's own area differs, which rules out the frame
# having been repainted with something subtly different elsewhere.
hl_assert "with nothing else on screen disturbed (${DN} px)" \
	"$([ "${DN:-999999}" -le $(( CURSOR_SIZE * CURSOR_SIZE )) ] \
		&& echo true || echo false)" "true"

# Cost. Removing a 32x32 cursor must not repaint 921,600 pixels.
FRAMES="$(field stats-hide.json frames)"
DMG="$(field stats-hide.json damage_pixels)"
FULLREDRAW="$(field stats-hide.json full_redraw_frames)"
echo "  note: $FRAMES frames, $DMG damaged px, $FULLREDRAW full redraws"
hl_assert "and no full-output redraw was needed to do it ($FULLREDRAW)" \
	"$([ "${FULLREDRAW:-1}" -eq 0 ] && echo true || echo false)" "true"

# ── hidden -> visible again ────────────────────────────────────────────────
echo "-- and it comes back, the same, in the same place --"
read -r RN RMINX RMINY _ _ <<<"$(diff_box "$OUTDIR/b-hidden.png" "$OUTDIR/c-reshown.png")"
echo "  note: cursor px $C_ON; ${RN} px differ from hidden at ${RMINX},${RMINY}"
hl_assert "the same image returns whole ($C_ON of $EXPECT)" \
	"$([ "${C_ON:-0}" -eq "$EXPECT" ] && echo true || echo false)" "true"
hl_assert "in the same place (${RMINX},${RMINY})" \
	"$([ "${RMINX:--1}" -eq $(( PX - HOTSPOT )) ] \
		&& [ "${RMINY:--1}" -eq $(( PY - HOTSPOT )) ] \
		&& echo true || echo false)" "true"
# The strongest statement available: after hide-then-show the frame is
# byte-identical to before the hide. Any ghost, any collateral repaint, any
# half-restored rectangle shows up here.
read -r AN _ _ _ _ <<<"$(diff_box "$OUTDIR/a-visible.png" "$OUTDIR/c-reshown.png")"
hl_assert "and the frame is identical to before it was hidden ($AN px differ)" \
	"$([ "${AN:-1}" -eq 0 ] && echo true || echo false)" "true"

# ── A -> NULL -> B ─────────────────────────────────────────────────────────
echo "-- and a different cursor afterwards is the new one, not the old --"
echo "  note: after swap, $D_A px of $COLOUR_A and $D_B px of $COLOUR_B"
hl_assert "the second cursor is on screen ($D_B of $EXPECT)" \
	"$([ "${D_B:-0}" -eq "$EXPECT" ] && echo true || echo false)" "true"
hl_assert "and none of the first one survives ($D_A)" \
	"$([ "${D_A:-1}" -eq 0 ] && echo true || echo false)" "true"

# ── the invariant that outlives all of it ──────────────────────────────────
# Hiding and showing a cursor moves it around the screen; it does not change
# what the cursor looks like. The image is uploaded per generation, never per
# appearance.
UPLOADS="$(field stats-end.json cursor_source_uploads)"
UPBYTES="$(field stats-end.json cursor_source_upload_bytes)"
NOIMG="$(field stats-end.json cursor_no_image)"
echo "  note: cursor source uploads=$UPLOADS bytes=$UPBYTES cursor_no_image=$NOIMG"
hl_assert "hiding and showing never re-uploaded a cursor image ($UPLOADS)" \
	"$([ "${UPLOADS:-99}" -le 2 ] && echo true || echo false)" "true"
hl_assert "and AVK never lacked an image while one was wanted ($NOIMG)" \
	"$([ "${NOIMG:-1}" -eq 0 ] && echo true || echo false)" "true"

kill "$WLCURSOR_PID" "$WLVPTR_PID" 2>/dev/null
hl_stop

echo
echo "screenshots: $OUTDIR/{a-visible,b-hidden,c-reshown,d-swapped}.png"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
