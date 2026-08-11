#!/usr/bin/env bash
# avk-cursor-scale-test.sh — the two things that only go wrong away from scale 1.
#
# Both of these shipped, were found on a real desktop, and were invisible to
# every other test in this suite. Both are cheap to check and neither had
# anything watching it.
#
#   THE THEME IS NOT LOADED AT EVERY SCALE ANYTHING ASKS FOR
#
#   When asteroidz took over choosing cursor images it started loading the
#   theme at ONE scale -- the sharpest output's. wlroots used to load per
#   output, as a side effect of choosing a per-output image.
#
#   wlr_xcursor_manager_get_xcursor() returns NULL for a scale that was never
#   loaded; it does not load on demand. xwaylandready() asks for "default" at
#   scale 1 regardless of what any output runs at, so on a layout whose
#   sharpest output is 1.5 the lookup returned NULL, wlr_xwayland_set_cursor()
#   was never called, and every X11 window showed the X server's own 'X' root
#   cursor. Nothing logged it on either side.
#
#   THE RE-SELECTION GUARD IS A TRAP WHEN THE CURSOR IS HIDDEN
#
#   az_cursor_set_xcursor() returns early when asked for the cursor it already
#   chose, so that pointer motion does not restart an animation. Hiding the
#   cursor clears the image out of wlroots but not the record of which xcursor
#   was chosen -- so the first thing to ask for that same cursor again matches
#   the guard, returns, and the pointer never comes back.
#
#   handlecursoractivity() does exactly that: it replays last_cursor.shape,
#   which is normally the shape that was showing when the cursor was hidden.
#
# WHY NO OTHER TEST COULD SEE EITHER
#
# Every other cursor test runs a single output at scale 1 and never lets the
# cursor be hidden. At scale 1 the loaded scale and the requested scale are the
# same number, so the first bug cannot exist; and with no hide there is nothing
# for the guard to trap.
#
# Break tests, each of which MUST fail:
#
#   BREAK=xcursor-one-scale  AZ_CURSOR_ONE_SCALE=1 -- load only the sharpest
#                            output's scale, as the shipped bug did.
#   BREAK=xcursor-guard      AZ_CURSOR_NO_PUSH_CHECK=1 -- drop the
#                            image_pushed term from the re-selection guard,
#                            restoring the trap.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-cursor-scale"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-cursor-scale-test: needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "avk-cursor-scale-test: needs python3-pillow"; exit 1; }

WLSHOT="$(dirname "$0")/wlshot/wlshot"
WLVPTR="$(dirname "$0")/wlvptr/wlvptr"
WLCURSOR="$(dirname "$0")/wlcursor/wlcursor"
for tool in "$WLSHOT" "$WLVPTR" "$WLCURSOR"; do
	[ -x "$tool" ] || { echo "avk-cursor-scale-test: $tool not built" >&2; exit 1; }
done

# The output runs at 1.5, so "the scale the theme was loaded at" and "the scale
# something asks for" are different numbers. That difference IS the test.
# Everything goes in ONE string. hl_start takes `${1:-${HL_EXTRA_KDL:-}}`, so
# passing a scene argument makes HL_EXTRA_KDL be ignored entirely rather than
# merged -- which silently dropped both the scale and the timeout, and left the
# XWayland assertion below passing for the trivial reason that the output was
# at scale 1 after all. That is what the scale premise assertion is for.
SCALE=1.5
SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }
cursor_hide_timeout 3
output HEADLESS-1 { scale $SCALE }"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-cursorscale-$$"
HL_OUTDIR="$OUTDIR"
HL_WIDTH=1280 HL_HEIGHT=720
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = xcursor-one-scale ] && HL_ENV="$HL_ENV AZ_CURSOR_ONE_SCALE=1"
[ "$BREAK" = xcursor-guard ] && HL_ENV="$HL_ENV AZ_CURSOR_NO_PUSH_CHECK=1"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

# How many pixels differ between two captures.
#
# Counting "non-background pixels" does not work here: the shape-setting client
# needs a window, and a 600x400 window at scale 1.5 is 540,000 pixels that swamp
# a cursor's ~500. Everything in the scene is static except the cursor, so a
# difference between two frames IS the cursor.
diff_count() {
	python3 - "$1" "$2" <<'PYEOF'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
pa, pb = a.load(), b.load()
n = 0
for y in range(a.size[1]):
    for x in range(a.size[0]):
        if pa[x, y] != pb[x, y]:
            n += 1
print(n)
PYEOF
}

hl_start "$SCENE_KDL"
LOG="$(ls "$OUTDIR"/state/asteroidz/*.log 2>/dev/null | head -1)"
sleep 3

# ── the theme, at a scale nothing on screen runs at ────────────────────────
# XWayland asks for "default" at scale 1 while the only output is at 1.5. The
# compositor now says so out loud when it cannot find one, which is the whole
# assertion: the failure used to be silent on both sides.
# THE premise. Everything below is about a scale mismatch, and at scale 1 there
# is no mismatch to have: the loaded scale and the requested scale are the same
# number and the bug cannot exist. An earlier version of this test lost its
# scale config to hl_start's argument handling and passed on an output running
# at 1.0, asserting nothing whatsoever.
ACTUAL_SCALE="$(hl_get "get all-monitors" | python3 -c "
import json,sys
print(json.load(sys.stdin)['monitors'][0]['scale'])" 2>/dev/null || echo 0)"
echo "-- a cursor exists at scale 1 even though no output runs at scale 1 --"
echo "  note: HEADLESS-1 scale is $ACTUAL_SCALE (must not be 1)"
hl_assert "the output really is running at a scale other than 1" \
	"$([ "$ACTUAL_SCALE" = "$SCALE" ] && echo true || echo false)" "true"
XWARN=0
if [ -n "$LOG" ] && grep -q "X11 windows will show the X server" "$LOG"; then
	XWARN=1
fi
echo "  note: xwayland cursor-lookup failures logged: $XWARN"
hl_assert "XWayland was given a cursor to use" \
	"$([ "$XWARN" -eq 0 ] && echo true || echo false)" "true"

# ── hidden, then asked for the same cursor again ───────────────────────────
# Put the pointer somewhere over the wallpaper so the cursor is the theme's
# "default", hold it there, and let the hide timeout expire. Moving again
# replays the same shape -- which is exactly the request the guard used to
# swallow.
echo "-- a hidden cursor comes back when the same shape is asked for again --"
# A client that uses wp_cursor_shape_v1, because that is the path the bug is
# on. handlecursoractivity() replays `last_cursor.shape` when a client has
# requested one and falls through to az_cursor_show() when none has -- and
# az_cursor_show() bypasses the re-selection guard entirely. With no
# shape-setting client the trap is unreachable and the break test is inert,
# which is exactly how the first version of this test came back green with the
# bug fully present.
"$WLCURSOR" --size 600x400 --colour 202020 --shape default \
	> "$OUTDIR/wlcursor.log" 2>&1 &
WLCURSOR_PID=$!
HL_SPAWNED_PIDS+=("$WLCURSOR_PID")
hl_wait_client_count 1 60
sleep 2
# Onto the client's own window, so it is the one holding pointer focus and its
# requested shape is what gets replayed.
"$WLVPTR" 400 300 "$HL_WIDTH" "$HL_HEIGHT" hold:1000
sleep 1
"$WLSHOT" --cursor "$OUTDIR/before-hide.png" 2>>"$OUTDIR/wlshot.log"

# Past the 3s idle timeout with no input at all.
sleep 5
"$WLSHOT" --cursor "$OUTDIR/hidden.png" 2>>"$OUTDIR/wlshot.log"

# Any motion counts as activity; handlecursoractivity() replays the shape.
# Nudged away and then put back exactly where it started, so the final capture
# can be compared to the first one pixel for pixel -- a cursor two pixels along
# is a different picture and would fail an identity assertion for a reason that
# has nothing to do with hiding.
"$WLVPTR" 402 302 "$HL_WIDTH" "$HL_HEIGHT" hold:300
"$WLVPTR" 400 300 "$HL_WIDTH" "$HL_HEIGHT" hold:1000
sleep 1
"$WLSHOT" --cursor "$OUTDIR/after.png" 2>>"$OUTDIR/wlshot.log"

VANISHED="$(diff_count "$OUTDIR/before-hide.png" "$OUTDIR/hidden.png")"
RETURNED="$(diff_count "$OUTDIR/hidden.png" "$OUTDIR/after.png")"
SAME="$(diff_count "$OUTDIR/before-hide.png" "$OUTDIR/after.png")"
echo "  note: px changed by hiding=$VANISHED, by reappearing=$RETURNED, before vs after=$SAME"

# Premise: the idle timeout has to have actually removed something, or "it is
# visible at the end" is true for reasons unrelated to the fix.
hl_assert "the idle timeout removed a cursor-sized region ($VANISHED px)" \
	"$([ "${VANISHED:-0}" -gt 20 ] && echo true || echo false)" "true"
# The assertion: activity replays the client's requested shape, and the
# re-selection guard must not swallow it.
hl_assert "and the next movement brought it back ($RETURNED px)" \
	"$([ "${RETURNED:-0}" -gt 20 ] && echo true || echo false)" "true"
hl_assert "as exactly the same picture in the same place ($SAME px differ)" \
	"$([ "${SAME:-1}" -eq 0 ] && echo true || echo false)" "true"

kill "$WLCURSOR_PID" 2>/dev/null
hl_stop

echo
echo "screenshots: $OUTDIR/{before-hide,hidden,after}.png"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
