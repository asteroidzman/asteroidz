#!/usr/bin/env bash
# blur-bitdepth-test.sh — does a 10-BIT SIBLING output corrupt blur on the
# 8-bit output beside it?
#
# The live report is: translucent windows on a 1920x1080 SDR output draw large
# flat dark polygons with colour fringing instead of a blurred backdrop, but
# ONLY while the OTHER monitor is in HDR. Turn HDR off on the other one and the
# blur is correct. `blur { optimized 0 }` also fixes it, which places the fault
# in the shared per-monitor optimized-blur cache rather than the per-window
# blur path.
#
# contrib/blur-scale-test.sh chased the fractional scale and cleared it, and
# its header records why it stopped: "an SDR output beside a colour-transformed
# one" had no headless equivalent, because a headless output is not HDR-capable.
#
# It has one after all. HDR is not the only thing that flips a render format:
#
#     if ((m->bitdepth == 10 || m->hdr) && render_format != XRGB2101010)
#             wlr_output_state_set_render_format(state, XRGB2101010);
#
# `bit-depth 10` is a per-output config knob that reaches the SAME line without
# needing an HDR-capable display or an ICC profile.
#
# IT DID NOT REPRODUCE — and this time the trigger was definitely armed. The
# headless backend accepted the 10-bit framebuffer (the run reports "HEADLESS-2
# is 10-bit, HEADLESS-1 is 8-bit"), and blur on the 8-bit output beside it came
# back with 110869 distinct colours and 1% near-black, i.e. perfect.
#
# So the 10-bit render format is NOT what corrupts the neighbour. What is left
# of the live difference is the colour TRANSFORM itself -- BT.2020/PQ plus the
# ICC profile on DP-1 -- not the framebuffer format it happens to imply. That
# is a much narrower target than "HDR", and it is why this file stays: it costs
# one command to re-confirm the format is innocent.
#
# The test aborts with exit 2 and says INCONCLUSIVE if the backend ever refuses
# the 10-bit framebuffer, because a green run that never armed the trigger
# would be worse than no run at all.
#
# Run under the renderer the SESSION uses. The live machine is Vulkan, and the
# optimized-blur cache is renderer-specific code, so this defaults to vulkan
# rather than the harness default of gles2.
#
# Usage: contrib/blur-bitdepth-test.sh
#   DEPTH=8    make the sibling 8-bit too (the CONTROL -- should pass)
set -u

REPO="${ASTEROIDZ_REPO:-$HOME/asteroidz}"
DEPTH="${DEPTH:-10}"
export HL_RENDERER="${HL_RENDERER:-vulkan}"

PASS=0
FAIL=0
ok() { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }
note() { echo "  --   $1"; }

# shellcheck disable=SC1091
. "$REPO/contrib/lib/headless.sh"

hl_start
trap 'hl_stop' EXIT

WORK="$HL_OUTDIR"
echo "renderer: $HL_RENDERER, sibling bit-depth: $DEPTH"

# `optimized 1` is the whole point: that is the shared per-monitor cache.
cat >> "$HL_CONFIG" <<'EOF'
effects {
	blend-space "srgb"
	blur { enable 1; layer 0; optimized 1; passes 2; radius 6
		transparency-threshold 0.5
		params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } }
}
EOF
hl_dispatch "reload_config" 1

# Something worth blurring: a flat colour blurs to itself, so broken and
# working would look identical. Plasma is high-frequency and colourful, so a
# correct blur is a smooth spread of many values and a broken one is not.
kill "$HL_SWAYBG_PID" 2>/dev/null
sleep 0.3
magick -size "${HL_WIDTH}x${HL_HEIGHT}" plasma:fractal -blur 0x2 "$WORK/wall.png"
swaybg -o '*' -i "$WORK/wall.png" -m fill > "$WORK/swaybg.log" 2>&1 &
sleep 2

# The SIBLING. The window under test stays on HL_MON (8-bit); this second
# output is the one whose render format is changed out from under it.
hl_dispatch "create_virtual_output" 2
SIBLING="$(hl_get "get all-monitors" | python3 -c '
import json, sys
first = sys.argv[1]
for m in json.load(sys.stdin)["monitors"]:
    if m["name"] != first:
        print(m["name"]); break
' "$HL_MON")"
if [ -z "$SIBLING" ]; then
	bad "a sibling output was created"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
ok "a sibling output was created ($SIBLING)"
hl_dispatch "set_output_position,$SIBLING,$HL_WIDTH,0" 2

# Force its framebuffer depth. There is no set_output_bitdepth dispatch -- it
# is a config rule -- so this goes through the file and a reload.
cat >> "$HL_CONFIG" <<EOF
output $SIBLING { bit-depth $DEPTH }
EOF
hl_dispatch "reload_config" 3

depth_of() { # depth_of NAME -> reported framebuffer bit depth
	hl_get "get all-monitors" | python3 -c '
import json, sys
name = sys.argv[1]
for m in json.load(sys.stdin)["monitors"]:
    if m["name"] == name:
        print(m.get("bitdepth", "?")); break
else:
    print("?")
' "$1"
}

SIB_DEPTH="$(depth_of "$SIBLING")"
MON_DEPTH="$(depth_of "$HL_MON")"
note "$SIBLING is $SIB_DEPTH-bit, $HL_MON is $MON_DEPTH-bit"

# An honest inconclusive. The headless backend may simply refuse a 10-bit
# framebuffer, in which case asteroidz logs "10-bit framebuffer not supported"
# and falls back to 8 -- and then this test has NOT set up the condition it
# claims to test. Saying so is the point; a green run that never armed the
# trigger is worse than no run.
if [ "$DEPTH" = "10" ] && [ "$SIB_DEPTH" != "10" ]; then
	note "the headless backend refused a 10-bit framebuffer (got $SIB_DEPTH)"
	note "this configuration cannot reproduce the live trigger -- INCONCLUSIVE"
	echo; echo "$PASS passed, $FAIL failed (inconclusive: 10-bit unavailable)"
	exit 2
fi

# The window goes on the 8-bit output -- the one that is broken live.
hl_dispatch "focus_monitor,$HL_MON" 1
kitty --title BLURWIN -o background_opacity=0.35 -o background='#101010' \
	sh -c 'echo BLURWIN; exec sleep 600' > "$WORK/kitty.log" 2>&1 &
sleep 4

window_stats() { # window_stats <shot> <x> <y> <w> <h> -> "distinct_colours pct_dark"
	python3 - "$WORK/$1.png" "$2" "$3" "$4" "$5" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x, y, w, h = (int(v) for v in sys.argv[2:6])
W, H = im.size
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

# Window box in the CAPTURED OUTPUT's own coordinates: `get all-clients` is
# layout-global, `grim -o` starts at the output's origin.
geom() {
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
	bad "the translucent window was found on $HL_MON"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
ok "the translucent window is on $HL_MON at ${WW}x${WH}+${WX}+${WY}"

grim -o "$HL_MON" "$WORK/blur.png" 2>/dev/null
read -r COLOURS DARKPCT <<<"$(window_stats blur "$WX" "$WY" "$WW" "$WH")"
note "window interior: $COLOURS distinct colours, ${DARKPCT}% near-black"

# Both measures, because either alone has an innocent reading: few colours can
# mean a low-contrast blur, and darkness can mean a dark patch of wallpaper.
# The reported failure is BOTH at once -- flat AND dark.
if [ "$COLOURS" -lt 50 ] && [ "$DARKPCT" -gt 50 ]; then
	bad "blur on the 8-bit output survives a ${SIB_DEPTH}-bit sibling"
	note "REPRODUCED: flat and dark, which is the live artifact"
elif [ "$COLOURS" -lt 50 ]; then
	bad "blur on the 8-bit output has more than 50 distinct colours"
	note "flat but not dark -- related, but not the reported artifact"
else
	ok "blur on the 8-bit output survives a ${SIB_DEPTH}-bit sibling"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
