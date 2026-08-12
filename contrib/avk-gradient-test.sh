#!/usr/bin/env bash
# avk-gradient-test.sh — M4C: AVK's gradients against the GLES reference.
#
# WHY THIS EXISTS BESIDE tests/test-avk-gradient.c, WHICH IS STRICTER
#
# The unit test compares AVK against an independent C transcription of
# SceneFX's gradient.frag, at every count, angle, origin and mode. That is the
# tighter check by a wide margin -- but it compares AVK to a READING of the
# reference, and a reading can be wrong in the same way twice. This compares
# AVK to the reference ITSELF, running in the same compositor, on the same
# scene, through its own renderer.
#
# WHAT IT USES, AND WHY IT IS NOT THE BORDER
#
# asteroidz has exactly two gradient consumers:
#
#   client_set_border_fill()   2 stops, linear, blend, origin {0.5, 0.5}
#   overview vignettes         5 stops, linear, blend, origin {0.5, 0.5},
#                              degrees 90 and 0, corner radii 18
#
# This uses the VIGNETTE. The border cannot be captured at all: setting
# `border_gradient 1` makes the compositor unresponsive within seconds --
# client_set_border_fill() calls wlr_scene_rect_set_gradient() with no dirty
# check, which damages the border node every tick, which schedules another
# frame, which ticks again. The event loop never returns to its clients: grim
# and amsg both time out, and the heap grows about 54 MB/s. It reproduces on
# BOTH renderers and on a pre-M4C build, so it is not a gradient-rendering bug
# and it is not fixed here -- see docs/avk-effects.md.
#
# The vignette is the better fixture anyway: five stops rather than two, two
# angles, and rounded corners composed on top.
#
# Both consumers are LINEAR and INTERPOLATED. Nothing in the compositor draws a
# conic gradient, a banded one, or an off-centre origin, so those have no GLES
# reference here and are carried by the unit test alone. That is a limit of the
# fixture, stated rather than papered over.
#
# THE CONTROL IS THE POINT. Two renderers do not agree pixel for pixel on an
# arbitrary scene. Each engine is captured twice -- overview closed (wallpaper
# only, no gradient anywhere) and overview open (wallpaper plus vignettes). The
# closed pair measures what the two renderers disagree about on the same
# wallpaper blit; the open pair may not exceed that by more than a tolerance.
# Without the control, a tolerance large enough to survive the noise floor
# would also hide a wrong ramp.
#
#   bash contrib/avk-gradient-test.sh
#   BREAK=gradient-first-color bash contrib/avk-gradient-test.sh   # must FAIL
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-gradient"
BREAK="${BREAK:-}"

REPAINT="$(dirname "$0")/wlrepaint/wlrepaint"
[ -x "$REPAINT" ] || { echo "not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

start() { # start <engine>
	OUTDIR="${TMPDIR:-/tmp}/asteroidz-vig-$1-$$"
	HL_OUTDIR="$OUTDIR"
	if [ "$1" = gles ]; then HL_ENV="ASTEROIDZ_RENDERER=wlr"
	else HL_ENV="ASTEROIDZ_RENDERER=avk"; fi
	case "$BREAK" in
		gradient-first-color)   HL_ENV="$HL_ENV AZ_GRADIENT_FIRST_COLOR=1" ;;
		gradient-blend-swap)    HL_ENV="$HL_ENV AZ_GRADIENT_BLEND_SWAP=1" ;;
		gradient-center-origin) HL_ENV="$HL_ENV AZ_GRADIENT_CENTER_ORIGIN=1" ;;
		gradient-color-offset)  HL_ENV="$HL_ENV AZ_GRADIENT_COLOR_OFFSET=1" ;;
		"") ;;
		*) echo "unknown BREAK=$BREAK" >&2; exit 1 ;;
	esac
	export HL_OUTDIR HL_ENV
	# No clients and no blur. Blur is M4F -- AVK does not implement it, so the
	# overview's chrome strip would differ between the engines by the whole
	# effect and swamp the number this test is about. With no windows the frame
	# is wallpaper, vignettes and chrome, which is what makes the comparison
	# almost entirely the gradient.
	hl_start "shadows 0
layer_shadows 0
animations 0
effects { blur { enable 0 } }
layout {
    titlebar { enable 0 }
}"
	sleep 2
}

capture() { # capture <engine> <overview 0|1> <outfile>
	start "$1"
	# ONE client, because the overview only arranges -- and only creates its
	# vignettes -- when the monitor has a visible window. With none,
	# toggleoverview() flips the flag, nothing is laid out, and the "open" and
	# "closed" captures come back byte-identical: max 0 across 1.5M channels,
	# which reads exactly like a perfect match.
	"$REPAINT" --title "wlvig" --size 700x500 --solid 202020 --frames 4000 \
		--hold-ms 100 > "$OUTDIR/client.log" 2>&1 &
	HL_SPAWNED_PIDS+=("$!")
	hl_wait_client_count 1 60 >/dev/null 2>&1
	sleep 2
	if [ "$2" = 1 ]; then
		hl_dispatch "toggle_overview" 2
	fi
	sleep 2
	grim -o HEADLESS-1 "$3" 2>/dev/null
	if [ "$1" = avk ]; then
		hl_get "get avk-stats" | jq -r \
			'"\(.gradient_draws // 0) \(.gradient_linear_draws // 0) \(.gradient_colors_processed // 0)"' \
			> "$3.stats"
	fi
	hl_stop
}

# Whole-frame comparison, reported as max and mean channel error. With no
# windows on screen the frame is wallpaper plus vignette, and the wallpaper is
# the same 1:1 blit in both engines -- so what is left is the gradient.
compare() { # compare <a.png> <b.png> -> "max mean outside n wx wy"
	python3 - "$1" "$2" <<'PYEOF'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGB'); pa = a.load()
b = Image.open(sys.argv[2]).convert('RGB'); pb = b.load()
if a.size != b.size:
    print("0 0 0 0 0 0"); sys.exit(0)
W, H = a.size
mx = tot = n = outside = 0; wx = wy = 0
for y in range(0, H, 2):
    for x in range(0, W, 2):
        ca, cb = pa[x, y], pb[x, y]
        for i in range(3):
            e = abs(ca[i] - cb[i]); tot += e; n += 1
            if e > mx: mx, wx, wy = e, x, y
            if e > 2: outside += 1
print(f"{mx} {tot/n if n else 0:.4f} {outside} {n} {wx} {wy}")
PYEOF
}

# How far the vignette darkens the wallpaper: the largest per-pixel drop
# between the overview-closed and overview-open captures of the SAME engine.
# This is the premise -- a gradient that did not draw, or drew flat, has
# nothing to say here, and the comparison would then be scored on two
# identical wallpapers.
darkening() { # darkening <closed.png> <open.png>
	python3 - "$1" "$2" <<'PYEOF'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGB'); pa = a.load()
b = Image.open(sys.argv[2]).convert('RGB'); pb = b.load()
if a.size != b.size:
    print("0 0"); sys.exit(0)
W, H = a.size
# Sample the far LEFT and the CENTRE of the same row band. The horizontal
# vignette is a symmetric dark -> clear -> dark ramp, so the edge must be
# darkened and the middle must not be -- which a flat fill cannot produce.
def mean(x0, x1):
    t = c = 0
    for y in range(H // 3, 2 * H // 3, 2):
        for x in range(x0, x1, 2):
            t += sum(pb[x, y]) - sum(pa[x, y]); c += 3
    return t / c if c else 0
print(f"{-mean(0, max(2, W // 20)):.1f} {-mean(W // 2 - W // 40, W // 2 + W // 40):.1f}")
PYEOF
}

echo
echo "--- overview vignette (5 stops, linear, interpolated): AVK vs GLES ---"
BASE="${TMPDIR:-/tmp}/asteroidz-vigcmp-$$"
capture avk  0 "$BASE-closed-avk.png"
capture gles 0 "$BASE-closed-gles.png"
capture avk  1 "$BASE-open-avk.png"
capture gles 1 "$BASE-open-gles.png"

# PREMISE 1, AND IT IS CURRENTLY UNMET. gradient_draws is the only thing that
# can say a gradient reached the renderer, and a headless overview does not
# create its vignettes: they are guarded by `m->ov_main_wp && ...->node.enabled`
# (overview.h), the overview wallpaper node, which this environment does not
# produce. Instrumented and confirmed -- with the overview open, rects go 41 ->
# 153 and NOT ONE of them carries has_gradient.
#
# This is stopped here rather than scored, because the numbers below are
# perfectly capable of looking like a pass. The edge-darkening premise that
# follows measured 21.0 on both engines with no gradient anywhere on screen:
# the overview dims its own background, that dimming is a plain rect, and both
# renderers agree about it exactly. A premise that a NON-gradient satisfies is
# not a premise.
read -r DRAWS LIN COLS < "$BASE-open-avk.png.stats"
read -r CDRAWS _ _ < "$BASE-closed-avk.png.stats"
echo "  avk gradient stats: overview open $DRAWS draws ($LIN linear, $COLS colours); closed $CDRAWS"
if [ "${DRAWS:-0}" -eq 0 ]; then
	hl_skip "no gradient reached the renderer -- a headless overview creates no vignettes, and border_gradient hangs the compositor (see the header). NO COVERAGE IS CLAIMED."
	hl_summary
	exit 0
fi
hl_assert "AVK drew the vignette gradients ($DRAWS draws, $COLS colours)" \
	"$([ "${LIN:-0}" -eq "${DRAWS:-0}" ] && echo true || echo false)" "true"
hl_assert "and drew none with the overview closed (premise: $CDRAWS)" \
	"$([ "${CDRAWS:-1}" -eq 0 ] && echo true || echo false)" "true"

# PREMISE 2: the vignette is a RAMP -- it darkens the edge and not the middle.
# A flat fill, or a gradient collapsed to one stop, fails this and would
# otherwise sail through the comparison below.
read -r EDGE_A MID_A < <(darkening "$BASE-closed-avk.png" "$BASE-open-avk.png")
read -r EDGE_G MID_G < <(darkening "$BASE-closed-gles.png" "$BASE-open-gles.png")
echo "  darkening (edge / centre): avk $EDGE_A / $MID_A   gles $EDGE_G / $MID_G"
hl_assert "the AVK vignette darkens the edge (${EDGE_A}) far more than the centre (${MID_A})" \
	"$(python3 -c "import sys; print('true' if float('$EDGE_A') > 8 and float('$EDGE_A') > 3*abs(float('$MID_A')) else 'false')")" "true"
hl_assert "the GLES vignette does the same (${EDGE_G} / ${MID_G})" \
	"$(python3 -c "import sys; print('true' if float('$EDGE_G') > 8 and float('$EDGE_G') > 3*abs(float('$MID_G')) else 'false')")" "true"

# The control: what the two engines disagree about with NO gradient on screen.
read -r C_MAX C_MEAN C_OUT C_N C_X C_Y < <(compare \
	"$BASE-closed-avk.png" "$BASE-closed-gles.png")
echo "  control (overview closed): max $C_MAX  mean $C_MEAN  outside(>2) $C_OUT/$C_N"
read -r G_MAX G_MEAN G_OUT G_N G_X G_Y < <(compare \
	"$BASE-open-avk.png" "$BASE-open-gles.png")
echo "  with vignettes:            max $G_MAX  mean $G_MEAN  outside(>2) $G_OUT/$G_N  (worst at $G_X,$G_Y)"

LIMIT=$((C_MAX + 2))
hl_assert "AVK matches the GLES reference (max $G_MAX <= control+2 = $LIMIT)" \
	"$([ "${G_MAX:-999}" -le "$LIMIT" ] && echo true || echo false)" "true"
hl_assert "and does so almost everywhere ($G_OUT of $G_N channels outside 2)" \
	"$([ "${G_OUT:-999}" -le $((C_OUT + G_N / 100)) ] && echo true || echo false)" "true"

hl_summary
