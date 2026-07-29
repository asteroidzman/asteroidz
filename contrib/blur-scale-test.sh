#!/usr/bin/env bash
# blur-scale-test.sh — does window blur survive a FRACTIONAL output scale?
#
# Written to chase a report of broken blur on a 1920x1080 output running
# `scale 0.75`: a translucent terminal came up with large flat black polygons
# and RGB fringing instead of a blurred wallpaper, while the scale-1 output
# beside it was fine. That looked like sampling a blur texture outside the
# region actually written, and scale < 1 is the awkward direction nobody tests
# -- the LOGICAL size is then BIGGER than the physical mode (1920/0.75 = 2560),
# so a buffer sized in physical pixels but indexed in logical ones is read past
# its end. Fractional scaling is usually only exercised at 1.25/1.5, where
# logical is smaller and that bug would hide.
#
# IT DID NOT REPRODUCE. Blur is intact at 0.75, 1.25 and 1.5, on a single
# output and on a second output at a non-zero origin, under GLES2. So this is
# NOT a fractional-scale bug, and the file stays as the regression guard that
# says so -- the next person to suspect scale can spend one command instead of
# an afternoon.
#
# What this harness CANNOT reach is the remaining difference on the machine
# that reported it: the working output there is HDR, and a headless output is
# not HDR-capable (toggle_hdr is refused -- see regression/tests/hdr.sh), so
# "an SDR output beside an HDR one" has no headless equivalent at all.
#
# Captured at scale 1 first as the control, then at each scale under test on the
# SAME output with the same window, so the only variable is the scale.
#
# Usage: contrib/blur-scale-test.sh
#   MULTI=1   put the window on a SECOND output at x=$HL_WIDTH
#   SCALES    scales to try (default "0.75 1.25 1.5")
set -u

REPO="${ASTEROIDZ_REPO:-$HOME/asteroidz}"
SCALES="${SCALES:-0.75 1.25 1.5}"

PASS=0
FAIL=0
ok() { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }

# shellcheck disable=SC1091
. "$REPO/contrib/lib/headless.sh"

hl_start
trap 'hl_stop' EXIT

WORK="$HL_OUTDIR"

# Blur has to be ON, and `optimized 1` matters: that is the shared per-monitor
# cache (wlr_scene_optimized_blur), sized from the monitor's LOGICAL box, which
# is exactly the thing a fractional scale changes.
cat >> "$HL_CONFIG" <<'EOF'
effects {
	blend-space "srgb"
	blur { enable 1; layer 0; optimized 1; passes 2; radius 6
		transparency-threshold 0.5
		params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } }
}
EOF
hl_dispatch "reload_config" 1

# Something worth blurring. A flat colour blurs to itself, so a broken blur and
# a working one would look identical -- this is high-frequency and colourful, so
# a correct blur is a smooth gradient and a broken one is not.
kill "$HL_SWAYBG_PID" 2>/dev/null
sleep 0.3
magick -size "${HL_WIDTH}x${HL_HEIGHT}" plasma:fractal -blur 0x2 "$WORK/wall.png"
swaybg -o '*' -i "$WORK/wall.png" -m fill > "$WORK/swaybg.log" 2>&1 &
SWAYBG=$!
sleep 2

# A SECOND output, if asked for, because the reported failure is on one: an
# output at a non-zero origin, beside another one, is the arrangement the live
# machine has and a single headless output cannot represent it. The per-monitor
# blur cache is positioned at the monitor's logical origin, so an output that
# does not start at 0,0 is a different code path from the one above.
if [ "${MULTI:-0}" = "1" ]; then
	hl_dispatch "create_virtual_output" 2
	SECOND="$(hl_get "get all-monitors" | python3 -c '
import json, sys
first = sys.argv[1]
for m in json.load(sys.stdin)["monitors"]:
    if m["name"] != first:
        print(m["name"]); break
' "$HL_MON")"
	if [ -z "$SECOND" ]; then
		bad "a second output was created"
		echo; echo "$PASS passed, $FAIL failed"; exit 1
	fi
	ok "a second output was created ($SECOND)"
	# Beside the first, not on top of it -- the live arrangement is side by side
	# and the origin is the whole point.
	hl_dispatch "set_output_position,$SECOND,$HL_WIDTH,0" 2
	# Focused BEFORE the window is spawned, so it simply opens there -- moving
	# an existing client between outputs is a second mechanism to get wrong.
	hl_dispatch "focus_monitor,$SECOND" 1
	HL_MON="$SECOND"
	sleep 1
fi

# A TRANSLUCENT window: hl_spawn_kitty forces background_opacity=1.0, and an
# opaque window is never blurred at all (transparency-threshold), so it would
# pass this test on a completely broken renderer.
kitty --title BLURWIN -o background_opacity=0.35 -o background='#101010' \
	sh -c 'echo BLURWIN; exec sleep 600' > "$WORK/kitty.log" 2>&1 &
KITTY=$!
sleep 4

# How varied is the window's interior?
#
# A working blur of a plasma wallpaper is smooth but COLOURFUL -- many distinct
# values. The reported failure is large flat polygons, so the count of distinct
# colours collapses and near-black takes over. Both are measured: either alone
# has an innocent explanation (a dark wallpaper region; a low-contrast blur).
window_stats() { # window_stats <shot> <x> <y> <w> <h>
	python3 - "$WORK/$1.png" "$2" "$3" "$4" "$5" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x, y, w, h = (int(v) for v in sys.argv[2:6])
W, H = im.size
# well inside the frame, clear of the border and titlebar
x0, y0 = max(0, x + 20), max(0, y + 40)
x1, y1 = min(W, x + w - 20), min(H, y + h - 20)
if x1 <= x0 or y1 <= y0:
    print("0 0"); raise SystemExit
px = im.load()
seen, dark, total = set(), 0, 0
for yy in range(y0, y1, 2):
    for xx in range(x0, x1, 2):
        c = px[xx, yy]
        seen.add(c)
        if sum(c) < 40:
            dark += 1
        total += 1
print(len(seen), round(100.0 * dark / total) if total else 0)
PY
}

# The window's box IN THE CAPTURED OUTPUT'S OWN COORDINATES.
#
# `get all-clients` reports layout-global x/y, and `grim -o` captures a single
# output whose image starts at that output's origin -- so on a second monitor at
# x=1920 the window reported at 1930 is at 10 in the shot. Indexing the image
# with the global figure reads off the end and reports a perfectly black window,
# which is indistinguishable from the bug being looked for.
geom() { # geom -> "x y w h" relative to $HL_MON
	local mx my
	read -r mx my <<<"$(hl_get "get all-monitors" | python3 -c '
import json, sys
name = sys.argv[1]
for m in json.load(sys.stdin)["monitors"]:
    if m["name"] == name:
        print(m.get("x", 0), m.get("y", 0)); break
else:
    print(0, 0)
' "$HL_MON")"
	hl_get "get all-clients" | python3 -c '
import json, sys
mx, my = int(sys.argv[1]), int(sys.argv[2])
for c in json.load(sys.stdin).get("clients", []):
    if "BLURWIN" in (c.get("title") or ""):
        print(c["x"] - mx, c["y"] - my, c["width"], c["height"]); break
' "$mx" "$my"
}

read -r WX WY WW WH <<<"$(geom)"
if [ -z "${WH:-}" ]; then
	bad "the translucent test window mapped"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
ok "the translucent test window mapped (${WW}x${WH} at ${WX},${WY})"

grim -o "$HL_MON" "$WORK/scale1.png" 2>/dev/null
read -r BASE_COLORS BASE_DARK <<<"$(window_stats scale1 "$WX" "$WY" "$WW" "$WH")"
echo "  ..   scale 1.00: $BASE_COLORS distinct colours, $BASE_DARK% near-black"

if [ "$BASE_COLORS" -lt 200 ]; then
	bad "the control is a real blur to compare against ($BASE_COLORS colours)"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
ok "the control is a real blur to compare against ($BASE_COLORS colours)"

for s in $SCALES; do
	hl_dispatch "set_output_scale,$HL_MON,$s" 2
	sleep 2
	read -r WX WY WW WH <<<"$(geom)"
	grim -o "$HL_MON" "$WORK/scale-$s.png" 2>/dev/null
	read -r C D <<<"$(window_stats "scale-$s" "$WX" "$WY" "$WW" "$WH")"
	echo "  ..   scale $s: $C distinct colours, $D% near-black"

	# A tenth of the control's variety, or a third of the window gone black, is
	# not a blur that merely looks different at another scale.
	if [ "$C" -gt $((BASE_COLORS / 10)) ] && [ "$D" -lt 33 ]; then
		ok "blur survives scale $s ($C colours, $D% near-black)"
	else
		bad "blur survives scale $s ($C colours, $D% near-black)"
	fi
done

kill "$KITTY" "$SWAYBG" 2>/dev/null

echo
echo "$PASS passed, $FAIL failed"
echo "shots: $WORK/scale1.png $WORK/scale-*.png"
[ "$FAIL" = 0 ]
