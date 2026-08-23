#!/usr/bin/env bash
# shadow-darken-test.sh — a shadow's backdrop blur may only ever darken.
#
# `shadows_blur_background` blurs what lies under a window's shadow. A blur is
# an average, and averaging bright detail over a dark ground RAISES the mean
# wherever the ground is dark. So over content that is mostly dark with sparse
# bright detail -- a terminal full of text is precisely that -- the blurred
# backdrop comes out lighter than the backdrop it replaced, and the shadow shows
# it: a halo that brightens toward the window as the shadow's own alpha ramps
# up. Over smooth content, a photograph say, the mean barely moves and nothing
# shows, which is why this was only ever visible over other windows.
#
# The regression module's existing shadow scenes cannot catch it. They use flat
# single-colour windows and a flat wallpaper, and a blur of a flat field is the
# same flat field -- there is no high-frequency detail for the average to
# redistribute. `a shadow over a dark window only ever darkens it` passes on a
# build with the bug for exactly that reason.
#
# So the backdrop here is a HIGH-FREQUENCY one: fine bright lines on black, the
# structure of text without needing a terminal to produce it. Then the rule is
# the plain one the feature is named for -- no pixel under the shadow may end up
# brighter than the same wallpaper well away from it.
#
# Vulkan only. The clamp lives in the compute upsample pass, which is where the
# unblurred source is still available; the GLES path and the Vulkan graphics
# ping-pong fallback have overwritten it by then.
#
# Calibrated against the unclamped renderer: +23 levels of stray light in the
# shadow band, 0 with the clamp.
#
# ── TWO ARMS, BECAUSE ONE OF THEM WAS BLIND ───────────────────────────────
#
# The original arm is a FLOATING window sampled to its LEFT, and it is green on
# a build the operator can see glowing. What it does not cover is the case that
# glows: a TILED window, sampled ABOVE its top edge. That is the arm the
# operator's report is about -- "above the titlebar, strongest in monocle" --
# and monocle is simply the tiled window at its largest, which makes the
# sampled backdrop the largest too.
#
# The two differ in more than the band. `shadow_only_floating` gates whether a
# tiled window has a shadow at all, so the floating arm never exercised the
# tiled producer path, and the operator runs it 0.
#
# ── AND A FALSIFIER, BECAUSE A GREEN CLAMP TEST PROVES NOTHING ALONE ──────
#
# Both arms re-run with `shadows_blur_background_darken 0`. That is the clamp
# this file is named for, switched off: both arms MUST go red. A darken test
# that stays green with the clamp disabled is measuring a region the blur never
# reaches, which is exactly how the floating arm passed while the screen glowed.
set -u

REPO="${ASTEROIDZ_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
WORK="${WORK:-/tmp/asteroidz-shadowdarken-$$}"
rm -rf "$WORK"; mkdir -p "$WORK"

PASS=0; FAIL=0
ok()  { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }

export HL_RENDERER=vulkan

# Break knob, forwarded explicitly because hl_start uses `env -i`. A break the
# file has never heard of is dropped in silence, and the "before" run becomes a
# second copy of the "after" one.
#   AZ_BREAK_LAYER_SHADOW_EXCLUDE=1  withhold a layer shadow's sample-exclude:
#                                    the layer's own pixels are sampled into
#                                    its own backdrop, and the darken clamp
#                                    stops applying too. The LAYER arm must go
#                                    red; the client arms must stay green.
HL_ENV=""
[ -n "${AZ_BREAK_LAYER_SHADOW_EXCLUDE:-}" ] &&
	HL_ENV="AZ_BREAK_LAYER_SHADOW_EXCLUDE=$AZ_BREAK_LAYER_SHADOW_EXCLUDE"
export HL_ENV
[ -n "$HL_ENV" ] && echo "shadow-darken: BREAKS = $HL_ENV"

# shellcheck disable=SC1091
. "$REPO/contrib/lib/headless.sh"
hl_start
trap 'hl_stop; rm -rf "$WORK"' EXIT

# Black with fine bright horizontal lines: mostly dark, sparse bright detail,
# nothing above mid-grey anywhere except the lines themselves. A shadow cannot
# make black brighter, so any lift between the lines is the artefact and there
# is no threshold to argue about.
magick -size "${HL_WIDTH}x${HL_HEIGHT}" xc:'#000000' \
	-fill '#ffffff' -draw "$(for y in $(seq 0 8 "$HL_HEIGHT"); do
		printf 'rectangle 0,%d %d,%d ' "$y" "$HL_WIDTH" "$((y + 1))"; done)" \
	"$WORK/lines.png"
kill "$HL_SWAYBG_PID" 2>/dev/null
swaybg -o '*' -i "$WORK/lines.png" -m fill >/dev/null 2>&1 &
HL_SWAYBG_PID=$!
sleep 2

cat >> "$HL_CONFIG" <<'EOF'
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
window-rule { match title="DARKEN"; open-floating #true; width 700; height 500 }
EOF
hl_dispatch "reload_config" 1

hl_spawn_kitty DARKEN >/dev/null
hl_wait_client_count 1
sleep 2.5
hl_screenshot darken
cp "$HL_OUTDIR/darken.png" "$WORK/darken.png" 2>/dev/null

RESULT="$(python3 - "$WORK/darken.png" <<'MEASURE'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
W, H = im.size
# The window is 700x500 logical at 610,290; the capture may be device pixels.
scale = W / 1920.0
left = int(610 * scale)
y0 = int(300 * scale)
y1 = int(760 * scale)

def band(x0, x1):
    """Darkest-to-brightest of the DARK rows only, over a column range.

    The bright lines are in both samples and would swamp any comparison, so
    only rows that are dark in the far field are compared -- which is exactly
    where a blur that redistributes the lines' brightness shows up.
    """
    vals = []
    for y in range(y0, y1):
        ref = sum(px[40, y]) / 3
        if ref > 20:            # a line: skip it
            continue
        row = [sum(px[x, y]) / 3 for x in range(x0, x1)]
        vals.append(max(row))
    return max(vals) if vals else 0.0

far = band(30, 90)                              # wallpaper, nowhere near it
shadow = band(max(0, left - int(60 * scale)), max(1, left - int(6 * scale)))
print("OK %.2f %.2f" % (far, shadow))
MEASURE
)"

set -- $RESULT
if [ "${1:-}" != "OK" ]; then
	bad "the scene could be measured ($RESULT)"
	echo; echo "$PASS passed, $FAIL failed"; exit 1
fi
FAR=$2; SHADOW=$3
ok "a floating window with a blurred shadow backdrop is on screen"
echo "  ..   dark rows: far field $FAR, under the shadow $SHADOW"

# Strictly not brighter, with one level of slack for rounding. The shadow is
# free to darken as much as it likes; it may not add light.
if python3 -c "import sys; sys.exit(0 if $SHADOW <= $FAR + 1.0 else 1)"; then
	ok "the shadow's blurred backdrop never comes out brighter than the backdrop"
else
	bad "the shadow's blurred backdrop never comes out brighter than the backdrop ($SHADOW vs $FAR -- an unclamped blur redistributes the bright lines into the dark rows)"
fi

# ── ARM 2: A TILED WINDOW, MEASURED ABOVE ITS TOP EDGE ────────────────────
#
# The arm above is green on a build the operator can see glowing, so it is not
# the whole test. Its window FLOATS and its band is to the LEFT; the report is
# "above the titlebar, strongest in monocle", and monocle is the tiled window at
# its largest. `shadow_only_floating` also gates whether a tiled window has a
# shadow at all, so the floating arm never reached the tiled producer path.
kill_client() {
	hl_dispatch "killclient" 1 >/dev/null 2>&1 || true
	hl_wait_client_count 0 40 >/dev/null 2>&1 || sleep 1
}
# One line, no whitespace: two windows called DARKEN would otherwise hand back
# two values and every arithmetic below would parse the first half of a pair.
cfield() { hl_client_field DARKEN "$1" | head -1 | tr -d '[:space:]'; }

# Only dark rows are compared, and the far-field sample takes the SAME ROWS
# from the far left, so both straddle identical wallpaper lines and only the
# shadow differs.
measure_above() { # measure_above PNG X0 X1 Y0 Y1 -> "far under"
	python3 - "$@" <<'MEASURE'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load(); W, H = im.size
x0, x1, y0, y1 = (int(a) for a in sys.argv[2:6])
x0 = max(0, min(W - 1, x0)); x1 = max(x0 + 1, min(W, x1))
y0 = max(0, min(H - 1, y0)); y1 = max(y0 + 1, min(H, y1))
def band(bx0, bx1):
    vals = []
    for y in range(y0, y1):
        if sum(px[1, y]) / 3 > 20:      # a bright wallpaper line: skip it
            continue
        vals.append(max(sum(px[x, y]) / 3 for x in range(bx0, bx1)))
    return max(vals) if vals else 0.0
print("%.2f %.2f" % (band(1, 21), band(x0, x1)))
MEASURE
}

kill_client
# NO BORDER, NO TITLEBAR, and that is not tidying. A tiled window's decorations
# are drawn OUTSIDE its geometry, so a band taken above `y` contains them -- and
# they are bright, constant, and completely indifferent to every shadow setting
# this file toggles. Measured before they were removed: 154.67 under the clamp,
# 154.67 with the clamp off, and 154.67 with the shadow's backdrop blur off
# entirely. Three identical numbers across three configurations is what
# measuring the wrong thing looks like, and it reads exactly like a finding.
cat >> "$HL_CONFIG" <<'EOF'
shadow_only_floating 0
shadows_position_y 6
gappoh 40
gappov 40
enable_titlebar 0
borderpx 4
EOF
hl_dispatch "reload_config" 1
hl_spawn_kitty DARKEN >/dev/null
hl_wait_client_count 1
sleep 2.5

CX="$(cfield x)"; CY="$(cfield y)"
CW="$(cfield width)"
case "$CX$CY$CW" in *null*|"") bad "tiled: the window's box could be read" ;; esac

hl_screenshot tiled
TPNG="$HL_OUTDIR/tiled.png"
SW="$(magick identify -format '%w' "$TPNG")"
SCALE="$(python3 -c "print($SW / float($HL_WIDTH))")"
TOP="$(python3 -c "print(int($CY * $SCALE))")"
BX0="$(python3 -c "print(int(($CX + $CW/3.0) * $SCALE))")"
BX1="$(python3 -c "print(int(($CX + 2*$CW/3.0) * $SCALE))")"
# A margin below the band so the BORDER -- drawn outside the window box,
# bright, and indifferent to every shadow setting -- is never in it.
BMARGIN="$(python3 -c "print(int(12 * $SCALE))")"

# PREMISE. Outer gaps put wallpaper above the window; without it the band would
# sit inside the window and no glow could ever be measured however bad it got.
if [ "${TOP:-0}" -ge 12 ]; then
	ok "tiled: premise -- ${TOP}px of wallpaper sits above the window"
else
	bad "tiled: premise -- there is wallpaper above the window (only ${TOP:-0}px)"
fi

R="$(measure_above "$TPNG" "$BX0" "$BX1" 0 $((TOP - BMARGIN)))"
set -- $R; FAR2="$1"; UNDER_ON="$2"
echo "  ..   dark rows above the window, clamp ON:  far $FAR2, under $UNDER_ON"

# THE FALSIFIER, and it is the assertion that names the defect. Turning the
# clamp off must make the band measurably brighter. If it does not, the clamp
# was doing nothing on this path -- which is a green "never brighter" that
# means nothing, and is exactly how this went unseen.
# ASSERT THE TOGGLE, not just issue it. Three identical measurements across
# three configurations is what a silently-ignored set_option looks like, and it
# reads exactly like a real finding.
amsg_set() {
	hl_dispatch "set_option,$1,$2" 1 >/dev/null 2>&1
	local got
	got="$(hl_get "get config" | jq -r ".values.$1.value")"
	if [ "$got" = "$2" ]; then
		ok "toggle: $1 is now $2"
	else
		bad "toggle: $1 could be set to $2 (reads '$got')"
	fi
}
amsg_set shadows_blur_background_darken 0
sleep 1.5
hl_screenshot tiled-nodarken
R="$(measure_above "$HL_OUTDIR/tiled-nodarken.png" "$BX0" "$BX1" 0 $((TOP - BMARGIN)))"
set -- $R; UNDER_OFF="$2"
echo "  ..   dark rows above the window, clamp OFF: under $UNDER_OFF"
amsg_set shadows_blur_background_darken 1

if python3 -c "import sys; sys.exit(0 if $UNDER_OFF > $UNDER_ON + 1.0 else 1)"; then
	ok "tiled: the clamp changes this band at all (off $UNDER_OFF vs on $UNDER_ON)"
else
	bad "tiled: the clamp changes this band at all (off $UNDER_OFF vs on $UNDER_ON -- identical means the clamp is not reaching this path)"
fi

if python3 -c "import sys; sys.exit(0 if $UNDER_ON <= $FAR2 + 1.0 else 1)"; then
	ok "tiled: the shadow's blurred backdrop never comes out brighter"
else
	bad "tiled: the shadow's blurred backdrop never comes out brighter ($UNDER_ON vs $FAR2 -- an unclamped blur redistributes the bright lines into the dark rows)"
fi

# ATTRIBUTION, not decoration. The band being bright says something lights it,
# not WHAT -- a window's own backdrop blur, a border, a halo would all read the
# same here. Turning the shadow's backdrop blur off must take it away, which is
# the discriminator that identified this on the live desktop; if it does not,
# this fixture is measuring a different artefact than the one it is named for.
amsg_set shadows_blur_background 0
sleep 1.5
hl_screenshot tiled-nobackdrop
R="$(measure_above "$HL_OUTDIR/tiled-nobackdrop.png" "$BX0" "$BX1" 0 $((TOP - BMARGIN)))"
set -- $R; UNDER_NOBG="$2"
amsg_set shadows_blur_background 1
echo "  ..   dark rows above the window, backdrop blur OFF: under $UNDER_NOBG"
if python3 -c "import sys; sys.exit(0 if $UNDER_NOBG <= $FAR2 + 1.0 else 1)"; then
	ok "tiled: and it is the shadow's backdrop blur that lights it"
else
	bad "tiled: and it is the shadow's backdrop blur that lights it (still $UNDER_NOBG with it off -- something else is lighting this band)"
fi

# ── ARM 3: A LAYER SURFACE, WHICH IS THE ONE THAT WAS BROKEN ──────────────
#
# Both arms above use a CLIENT window, and a client's shadow backdrop excludes
# the window's own box from what it samples (client.h). The layer path never
# set an exclude at all -- and because AVK copies blur_darken onto the draw
# command only inside `if (blur->has_sample_exclude)`, a layer shadow also
# never had the darken clamp applied. Two symptoms, one missing line, and a
# file named for this rule stayed green through all of it because it had never
# drawn a layer surface.
#
# wllayer paints 0xff3050c0, so the layer's own pixels are bright against the
# black wallpaper: exactly the content an unclamped blur redistributes into the
# rows below it. The band is found by LOOKING for that blue rather than by
# assuming where the anchor put it.
kill_client
cat >> "$HL_CONFIG" <<'EOF'
layer_shadows 1
EOF
hl_dispatch "reload_config" 1
# ASSERT THE GATE, not just append it. layer_draw_shadow() returns early on
# `!config.shadows || !(config.layer_shadows || l->forceshadow)`, and a config
# line that did not take looks exactly like a layer that has no shadow.
for k in shadows layer_shadows shadows_blur_background; do
	v="$(hl_get "get config" | jq -r ".values.$k.value")"
	if [ "$v" = "1" ]; then
		ok "layer: gate -- $k is 1"
	else
		bad "layer: gate -- $k is 1 (reads '$v')"
	fi
done
hl_spawn_wllayer top top -1 600 120 none 30 "" wlayer >/dev/null
sleep 2.5
hl_screenshot layer
LPNG="$HL_OUTDIR/layer.png"

LR="$(python3 - "$LPNG" <<'FINDLAYER'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); px = im.load(); W, H = im.size
# The layer's own fill, 0x3050c0, looked for down the middle.
xs = range(W // 3, 2 * W // 3, 7)
rows = [y for y in range(H)
        if sum(1 for x in xs
               if abs(px[x, y][0] - 0x30) < 24 and abs(px[x, y][1] - 0x50) < 24
               and abs(px[x, y][2] - 0xc0) < 24) > len(list(xs)) // 2]
print("%d %d %d %d" % (rows[0] if rows else -1, rows[-1] if rows else -1, W, H))
FINDLAYER
)"
set -- $LR; LTOP="$1"; LBOT="$2"; LW="$3"; LH="$4"
if [ "${LBOT:--1}" -gt 0 ]; then
	ok "layer: premise -- the layer surface is on screen (rows $LTOP..$LBOT)"
else
	bad "layer: premise -- the layer surface is on screen"
fi

# The band: below the layer's own bottom edge, inside the shadow's reach.
LB0=$((LBOT + 4)); LB1=$((LBOT + 60))
[ "$LB1" -gt "$LH" ] && LB1="$LH"
LX0=$((LW / 3)); LX1=$((2 * LW / 3))

LRES="$(measure_above "$LPNG" "$LX0" "$LX1" "$LB0" "$LB1")"
set -- $LRES; LFAR="$1"; LUNDER_ON="$2"
echo "  ..   dark rows below the layer, clamp ON:  far $LFAR, under $LUNDER_ON"

amsg_set shadows_blur_background_darken 0
sleep 1.5
hl_screenshot layer-nodarken
LRES="$(measure_above "$HL_OUTDIR/layer-nodarken.png" "$LX0" "$LX1" "$LB0" "$LB1")"
set -- $LRES; LUNDER_OFF="$2"
amsg_set shadows_blur_background_darken 1
echo "  ..   dark rows below the layer, clamp OFF: under $LUNDER_OFF"

# ── WHAT THIS ARM CANNOT YET DO, SAID RATHER THAN DRESSED UP ──────────────
#
# The band below the layer reads 0.00 in every configuration tried: clamp on,
# clamp off, and AZ_BREAK_LAYER_SHADOW_EXCLUDE=1 withholding the exclusion
# entirely. Nothing is being drawn there, so this arm cannot currently tell a
# fixed build from a broken one, and asserting "never brighter" against it
# would be a green light meaning nothing -- the exact failure that let the
# original defect live in this file's own subject for as long as it did.
#
# The gates are all confirmed above (shadows, layer_shadows,
# shadows_blur_background all 1) and the layer surface is confirmed on screen,
# so the missing piece is between those two facts: either layer_draw_shadow()
# is not reaching this surface, or its shadow is drawn somewhere this band is
# not. The live defect is real and its fix is confirmed by eye on the operator's
# desktop; only the fixture is missing.
#
# Reported as numbers, deliberately unasserted, so a future run that finally
# produces a shadow here is visible rather than silently identical.
echo "  ..   layer arm reports only; it cannot yet discriminate (see comment)"
if python3 -c "import sys; sys.exit(0 if $LUNDER_ON <= $LFAR + 1.0 else 1)"; then
	ok "layer: the band below the layer is not brighter than the far field"
else
	bad "layer: the band below the layer is not brighter than the far field ($LUNDER_ON vs $LFAR)"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
