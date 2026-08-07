#!/usr/bin/env bash
# blur-exclusion-test.sh — a shadow's blur never samples the window it is under.
#
# A window's shadow is drawn beneath the window, and its backdrop blur samples
# the scene image over the shadow's whole footprint -- which contains the
# window. The scene image holds the PREVIOUS frame there (the window is drawn
# after the shadow, and an undamaged region is never re-rendered), so without
# help the blur picks the window's own pixels up and spreads them outward as a
# halo in the window's own colour.
#
# scenefx keeps them out by patching the window's box out of the blur's SOURCE
# before blurring: each side's strip of ring content is stretched inward, so the
# hole holds a continuation of the surroundings rather than the window. This
# asserts that invariant DIRECTLY, on the source image itself, rather than
# hoping the consequence is visible on screen -- because whether it is visible
# depends on the window's size, its colour against the backdrop and the blur's
# reach, and the two scenes in contrib/regression/tests/effects.sh happen to sit
# where it is not.
#
# Uses FX_BLUR_DUMP (render/fx_renderer/vulkan/blur_debug.c), which writes the
# staged source to a .pam. Vulkan only: the dump is a Vulkan facility, and the
# GLES path patches the hole in a shader with no equivalent image to read back.
#
# Calibrated against the unfixed renderer: 30800 of 30800 hole pixels within
# tolerance of the window's colour on a 220x140 window, 0 after.
set -u

REPO="${ASTEROIDZ_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
WORK="${WORK:-/tmp/asteroidz-blurex-$$}"
rm -rf "$WORK"; mkdir -p "$WORK"

PASS=0; FAIL=0
ok()  { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }

export HL_RENDERER=vulkan
export HL_ENV="FX_BLUR_DUMP=$WORK/d FX_BLUR_DUMP_FRAMES=3"

# shellcheck disable=SC1091
. "$REPO/contrib/lib/headless.sh"
hl_start
trap 'hl_stop; rm -rf "$WORK"' EXIT

# SMALL, deliberately. The fill used to cover a quarter from each side and
# leave the middle, so on a big window the untouched middle sat further from
# every edge than the blur could reach and nothing showed. A window whose
# quarter is under the blur's reach is where it matters, and is also the shape
# of every dialog and notification card.
WIN_W=220
WIN_H=140

cat >> "$HL_CONFIG" <<EOF
animations 0
blur 1
blur_layer 0
blur_optimized 0
blur_passes 2
blur_radius 6
shadows 1
shadow_only_floating 1
shadows_size 72
shadows_blur 72
shadows_blur_background 1
shadows_blur_background_strength 0.55
shadowscolor 0x00000050
shadows_position_y 18
window-rule { match title="BLUREX"; open-floating #true; width $WIN_W; height $WIN_H }
EOF
hl_dispatch "reload_config" 1

# A tiled window under the floating one, so the backdrop the fill is supposed to
# extend is a strong flat colour that is NOT the wallpaper and NOT the floating
# window -- three distinguishable colours, so "what is in the hole" has an
# unambiguous answer.
HL_SPAWN_COLOR_IDX=3 hl_spawn_kitty BACKDROP >/dev/null   # yellow
hl_wait_client_count 1
sleep 1.5
HL_SPAWN_COLOR_IDX=1 hl_spawn_kitty BLUREX >/dev/null     # green
hl_wait_client_count 2
sleep 2.5

DUMP="$(ls "$WORK"/d-*-patched.pam 2>/dev/null | tail -1)"
SIDE="${DUMP%.pam}.txt"
if [ -z "$DUMP" ] || [ ! -f "$SIDE" ]; then
	bad "the blur source was dumped (is this a Vulkan build?)"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
ok "the blur source was dumped ($(basename "$DUMP"))"

RESULT="$(python3 - "$DUMP" "$SIDE" <<'MEASURE'
import sys

path, side = sys.argv[1], sys.argv[2]
with open(path, "rb") as f:
    data = f.read()
end = data.index(b"ENDHDR\n") + 7
hdr = {}
for line in data[:end].split(b"\n"):
    p = line.split()
    if len(p) == 2:
        hdr[p[0].decode()] = p[1].decode()
w, h, depth = int(hdr["WIDTH"]), int(hdr["HEIGHT"]), int(hdr["DEPTH"])
buf = data[end:]

box = {}
for line in open(side):
    p = line.split()
    if p and p[0] in ("region", "exclude"):
        box[p[0]] = [int(v) for v in p[1:5]]
rx, ry = box["region"][0], box["region"][1]
ex, ey, ew, eh = box["exclude"]
hx, hy = ex - rx, ey - ry


def px(x, y):
    o = (y * w + x) * depth
    return buf[o:o + 3]


# The floating window's colour, read from the STAGED dump of the same frame --
# the copy taken before the hole was patched, where the hole is the window. Read
# rather than hardcoded, because it lands here in the pass's blend space and
# premultiplied, not as the #22aa22 the terminal was told to use.
staged = path.replace("-patched.pam", "-staged.pam")
with open(staged, "rb") as f:
    sdata = f.read()
send = sdata.index(b"ENDHDR\n") + 7
sbuf = sdata[send:]
so = ((hy + eh // 2) * w + hx + ew // 2) * depth
win = sbuf[so:so + 3]

hit = 0
for y in range(hy, hy + eh):
    for x in range(hx, hx + ew):
        if all(abs(a - b) <= 25 for a, b in zip(px(x, y), win)):
            hit += 1
print("OK %d %d %d %d %d" % (hit, ew * eh, win[0], win[1], win[2]))
MEASURE
)"

set -- $RESULT
if [ "${1:-}" != "OK" ]; then
	bad "the dump could be measured ($RESULT)"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
HIT=$2; TOTAL=$3
echo "  ..   window colour in the blur source is ($4,$5,$6); hole is $TOTAL px"

# Zero, not a fraction. The fill either covers the hole or it does not, and a
# threshold here would just record how much leakage was tolerated on the day.
if [ "$HIT" -eq 0 ]; then
	ok "no pixel of the window survives in its own shadow's blur source"
else
	bad "no pixel of the window survives in its own shadow's blur source ($HIT of $TOTAL still hold it)"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
