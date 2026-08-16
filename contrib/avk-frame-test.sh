#!/usr/bin/env bash
# avk-frame-test.sh — the desktop, composited by AVK, checked against its own
# geometry.
#
# M3's acceptance test. It boots a real compositor, puts two tiled terminals on
# it, and asserts that the resulting frame IS a desktop: each window's own
# colour fills exactly the rectangle the compositor says that window occupies,
# the trim around it is trim, and everything else is wallpaper.
#
# ── WHAT THIS USED TO BE, AND WHY IT CHANGED ─────────────────────────────
#
# This fixture used to run the scene TWICE -- once on the SceneFX/wlroots path,
# once on AVK -- and assert the two frames agreed. That was the right oracle
# while there were two renderers. There is one now: SceneFX is gone, AVK
# composites every frame and aborts rather than falling back, and the reference
# arm degenerated into a second copy of the thing under test that could not
# draw. Its last act was to fail two assertions by rendering every window as a
# flat block of titlebar colour, which is precisely the symptom this fixture
# was written to catch -- reported against the ORACLE rather than the subject.
#
# So the oracle is now ARITHMETIC, computed from the compositor's own reported
# geometry rather than from a second renderer. `get all-clients` gives each
# window's box; borderpx is pinned in the config below; from those two the
# surface rectangle and the border ring follow exactly, and every assertion
# here is either an exact pixel count or an exact zero. There is no tolerance
# anywhere in this file, and there does not need to be: at border_radius 0 with
# effects off, every edge in the frame is on an integer coordinate.
#
# The claims, in the order they are made:
#
#   own colour fills its surface       -- the window is drawn, at the size and
#                                         place the compositor says it is
#   own colour appears nowhere else    -- and only there
#   the ring is entirely painted       -- the border exists, all the way round
#   no wallpaper inside the ring       -- and has no gaps in it
#   no trim colour inside the surface  -- THE ORIGINAL DEFECT. A border in this
#                                         compositor is ONE rect with the
#                                         window's interior clipped out of it,
#                                         and the border sits ABOVE the surface
#                                         in the scene. A renderer that ignores
#                                         the clip paints the whole window in
#                                         the border colour and the window
#                                         vanishes behind it. That used to be
#                                         caught by comparing a pixel COUNT
#                                         against SceneFX; it is now stated
#                                         directly.
#   the wallpaper covers everything    -- swaybg paints once, commits once and
#   outside every window                  releases its buffer, so it is the
#                                         sharpest test of whether AVK OWNS
#                                         client content or merely borrows it.
#                                         A partially-drawn wallpaper leaves
#                                         non-grey pixels in a region that can
#                                         contain nothing else, and fails as
#                                         loudly as a missing one.
#
# Effects stay off (shadows, blur, rounded corners). Not because a second
# renderer cannot do them -- because each of them paints OUTSIDE a window's box
# and would turn "is this pixel wallpaper" into a judgement call. They have
# their own fixtures.
#
# Break tests, each of which MUST fail. Every pixel claim above has one, and
# which claim each break reaches is written down, because a break that fires on
# a neighbouring assertion proves nothing about the one it was meant for:
#
#   BREAK=border    ignore the border rect's clipped-out interior, so the
#                   border fills the whole window. Falsifies the
#                   surface-coverage and trim-inside-surface claims.
#
#                   IT REACHES THE UNFOCUSED WINDOW ONLY, and that is a
#                   property of the scene rather than of this fixture. The
#                   scene walk emits the unfocused window's border rect AFTER
#                   its content and the focused window's BEFORE, so with the
#                   clip gone the first fills over its surface and the second
#                   fills under it, invisibly. Verified from AVK_SCENE_DUMP,
#                   not inferred: node 10,40 628x670 is dumped after the
#                   left window's buffers, node 643,40 627x670 before the
#                   right window's. Both windows are asserted on identically;
#                   only one of them can be falsified this way.
#   BREAK=noborder  suppress the border primitive's draw outright
#                   (AZ_AVK_SKIP_DRAW=border). Falsifies the ring claims --
#                   the ring is then wallpaper, on BOTH windows.
#   BREAK=hole      withhold damage over a strip that can only ever be
#                   wallpaper, so it is acknowledged and never drawn.
#                   Falsifies the wallpaper claim.
#   BREAK=wrapper   give the wl_compositor a renderer in AVK mode, restoring
#                   the wlr_client_buffer/wlr_texture topology. What catches
#                   it is avk.late_imports: the scene then hands AVK a wrapper
#                   it has never seen, so content is discovered at draw time
#                   instead of owned from commit. The pixels may still come out
#                   right -- that is the point. Ownership is not visible in a
#                   screenshot, so it needs a counter to be testable at all.
#
# ONE ASSERTION HAS NO FALSIFIER AND CLAIMS NO COVERAGE: "the window's own
# colour appears nowhere outside it". AZ_BREAK_AVK_QUAD_SWAP_CORNERS was tried
# for it and the run came back 20/20 -- the quad break only bites where a quad's
# four corners are placed independently, which at rest they are not. A break
# that leaves the suite green is worse than no break, so it is not wired up.
# The claim stays because it is exact and free, but the coverage it appears to
# add is not real, and that is written down rather than left to be rediscovered.
#
# There is a second switch, AVK_NO_FOREIGN_ACQUIRE=1, which disables the
# queue-family ownership transfer on imported buffers. It is NOT a break test
# HERE, and that is the interesting part: this suite passes with it set, and a
# real display comes up flat white. Nothing scans a headless buffer out, so the
# release half of that transfer -- the half that hands the finished frame back
# to KMS -- is invisible to every assertion below. Presentation is not covered
# by this file and cannot be; it takes a monitor.
#
# contrib/avk-sync-test.sh covers as much of the handover as a headless run can:
# whether every frame leaves with a fence attached at all. It cannot cover which
# fence, or whether the display honoured it, for the same reason.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-frame"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-frame-test: needs python3"; exit 1; }
python3 -c "import PIL, numpy" 2>/dev/null || { echo "avk-frame-test: needs python3-pillow and python3-numpy"; exit 1; }

# BORDERPX IS PINNED HERE AND NOT INHERITED. The surface rectangle is the
# window box inset by exactly this much, so a change to the harness default
# would silently move every rectangle this file measures -- and the assertions
# would then fail against a perfectly correct compositor.
BORDERPX=2

# No effects: each of shadow, blur and a corner radius paints outside a
# window's box, and every assertion below is an exact count over a rectangle.
#
# border_gradient is PINNED OFF rather than inherited, for the same reason as
# borderpx. It is off by default today; if it were ever turned on, the trim
# would be a ramp, the two colours sampled off the ring's edges would be its
# ends, and the intermediate stops -- most of the ring -- would go unnamed.
# Gradient borders have their own fixture (avk-gradient-border-test.sh).
SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
borderpx $BORDERPX
border_gradient 0
effects { blur { enable 0 } }"

# The harness wallpaper. Flat, and a colour no client here emits.
WALLPAPER="#808080"

BASE="${TMPDIR:-/tmp}/asteroidz-avk-frame-$$"
HL_OUTDIR="$BASE/avk"
HL_WIDTH=1280 HL_HEIGHT=720
HL_ENV="ASTEROIDZ_RENDERER=avk"
case "$BREAK" in
	border)   HL_ENV="$HL_ENV AVK_NO_BORDER_CLIP=1" ;;
	noborder) HL_ENV="$HL_ENV AZ_AVK_SKIP_DRAW=border" ;;
	# The bottom strip of the output. Below every window's box, so it is
	# wallpaper and can be nothing else -- which is what makes it a falsifier
	# for the wallpaper claim SPECIFICALLY, rather than for whichever
	# assertion happens to overlap a hole punched somewhere more interesting.
	hole)     HL_ENV="$HL_ENV AZ_AVK_DAMAGE_HOLE=0,$((HL_HEIGHT - 10)),$HL_WIDTH,10" ;;
	wrapper)  HL_ENV="$HL_ENV AZ_AVK_COMPOSITOR_RENDERER=1" ;;
	"")       ;;
	*) echo "avk-frame-test: unknown BREAK=$BREAK" >&2; exit 1 ;;
esac
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

hl_start "$SCENE_KDL"
# The palette cursor decides what colour each terminal paints itself, and the
# oracle below is keyed on it by name. Reset it so this fixture always gets
# colours 1 and 2 whatever ran before it in the same shell.
hl_reset_spawn_colors
LEFT_HEX="${HL_SPAWN_COLORS[0]}"
RIGHT_HEX="${HL_SPAWN_COLORS[1]}"
hl_spawn_kitty one >/dev/null; hl_wait_client_count 1 60
hl_spawn_kitty two >/dev/null; hl_wait_client_count 2 60
# Let both terminals finish their first real paint. Asserting on a frame caught
# mid-open animation measures a scene that is still moving.
sleep 3

# THE ORACLE'S INPUT: the compositor's own geometry, read over IPC, not the
# tiling arithmetic re-derived here. Re-deriving it would be a second chance to
# get the layout wrong, and it would agree with the compositor when both were.
GEOM="$(hl_get "get all-clients" | jq -c --arg l "$LEFT_HEX" --arg r "$RIGHT_HEX" \
	'[.clients[] | select(.title=="one" or .title=="two") |
	  {name: (if .title=="one" then "left" else "right" end),
	   hex: (if .title=="one" then $l else $r end),
	   x, y, w: .width, h: .height}] | sort_by(.x)')"
echo "geometry: $GEOM"

hl_screenshot shot
hl_stop

AVK_PNG="$HL_OUTDIR/shot.png"
AVK_LOG="$HL_OUTDIR/state/asteroidz/asteroidz.log"

# ── the engine came up, and said so unambiguously ──────────────────────────
echo "-- startup --"
hl_assert "AVK announced itself as the rendering backend" \
	"$(grep -c 'Asteroidz rendering backend: AVK native Vulkan' "$AVK_LOG")" "1"
# Deliberately NOT pinned to a renderer name. wlroots still needs a renderer
# for shm formats, allocation and screencopy; which one that is has no bearing
# on whether AVK builds the frame, and pinning it made this line a hostage to
# an unrelated default.
hl_assert "wlroots' renderer is named as a compatibility renderer only" \
	"$(grep -c 'wlroots compatibility renderer:' "$AVK_LOG")" "1"

# ── AVK actually built the frames ──────────────────────────────────────────
stat_field() { sed -n "s/.*\($1=[0-9]*\).*/\1/p" "$AVK_LOG" | tail -1 | cut -d= -f2; }
FRAMES="$(stat_field 'avk\.frames')"
FALLBACKS="$(stat_field 'avk\.fallback_frames')"
SURFACES="$(stat_field 'avk\.surfaces')"
WAITS="$(stat_field 'avk\.cpu_sync_waits')"

echo "-- instrumentation --"
hl_assert "AVK composited frames" "$([ "${FRAMES:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "no frame was refused" "${FALLBACKS:-x}" "0"
hl_assert "AVK drew client surfaces" \
	"$([ "${SURFACES:-0}" -gt 0 ] && echo true || echo false)" "true"
# The rule the whole synchronisation design exists to keep. A nonzero value
# here means the frame path is blocking on the GPU somewhere.
hl_assert "no CPU sync waits on the frame path" "${WAITS:-x}" "0"

# Ownership: AVK must take a client's content at the commit that produced it,
# not discover it at some later frame and hope it is still readable.
COMMITS="$(stat_field 'avk\.commit_imports')"
LATE="$(stat_field 'avk\.late_imports')"
hl_assert "AVK took client content at commit" \
	"$([ "${COMMITS:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "no surface buffer arrived late at a frame" "${LATE:-x}" "0"

# ── the frame itself ───────────────────────────────────────────────────────
python3 - "$AVK_PNG" "$GEOM" "$BORDERPX" "$WALLPAPER" > "$BASE/probe.txt" 2>&1 <<'PY'
import sys, json
import numpy as np
from PIL import Image

shot, wins, bw, wall = sys.argv[1], json.loads(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
a = np.asarray(Image.open(shot).convert('RGB'), dtype=np.uint8)
H, W, _ = a.shape

def hx(h):
    h = h.lstrip('#')
    return np.array([int(h[i:i+2], 16) for i in (0, 2, 4)], dtype=np.uint8)

def mask(c):
    return (a == c).all(axis=2)

wallm = mask(hx(wall))

# THE WALLPAPER-ONLY REGION, and why it is shaped like this. A window's own
# box is known exactly; its TITLEBAR is not -- the compositor derives the
# height from the font, so it is not a number this file may assume. The band
# ABOVE each window, over that window's own columns, is therefore excluded
# outright: whatever the titlebar's height turns out to be, it lives in there.
# What is left over is a region that can contain nothing but wallpaper, and
# that is a claim strong enough to fail on a single undrawn pixel.
hull = np.zeros((H, W), dtype=bool)
for w in wins:
    hull[0:min(H, w['y'] + w['h']), max(0, w['x']):min(W, w['x'] + w['w'])] = True
print(f"outside_hull_area {int((~hull).sum())}")
print(f"outside_hull_nonwallpaper {int((~hull & ~wallm).sum())}")

for w in wins:
    n = w['name']
    sx, sy, sw, sh = w['x'] + bw, w['y'] + bw, w['w'] - 2 * bw, w['h'] - 2 * bw
    S = np.zeros((H, W), dtype=bool); S[sy:sy + sh, sx:sx + sw] = True
    G = np.zeros((H, W), dtype=bool); G[w['y']:w['y'] + w['h'], w['x']:w['x'] + w['w']] = True
    R = G & ~S                                  # the border ring, exactly
    own = mask(hx(w['hex']))
    print(f"{n}_surface_area {sw * sh}")
    print(f"{n}_own_in_surface {int((own & S).sum())}")
    print(f"{n}_own_outside_surface {int((own & ~S).sum())}")
    print(f"{n}_ring_area {int(R.sum())}")
    print(f"{n}_ring_wallpaper {int((R & wallm).sum())}")
    print(f"{n}_ring_painted {int((R & ~wallm & ~own).sum())}")
    # The trim colours are SAMPLED off the middle of the left and right edges,
    # where the ring is a straight run. Two samples, not one: the focused
    # border is a gradient, so its two sides are different colours and
    # hardcoding either would leave half the ring unnamed.
    ymid = w['y'] + w['h'] // 2
    lc = tuple(int(v) for v in a[ymid, w['x'] + bw // 2])
    rc = tuple(int(v) for v in a[ymid, w['x'] + w['w'] - 1 - bw // 2])
    print(f"{n}_trim_left #%02x%02x%02x" % lc)
    print(f"{n}_trim_right #%02x%02x%02x" % rc)
    trim = mask(np.array(lc, dtype=np.uint8)) | mask(np.array(rc, dtype=np.uint8))
    print(f"{n}_trim_inside_surface {int((trim & S).sum())}")
PY
sed 's/^/  /' "$BASE/probe.txt"
get() { awk -v k="$1" '$1==k{print $2}' "$BASE/probe.txt"; }

echo "-- window contents --"
for name in left right; do
	area="$(get "${name}_surface_area")"
	own="$(get "${name}_own_in_surface")"
	# PREMISE. Everything after this is a zero, and a zero is what an empty
	# region reports too. The window has to be THERE, filling the rectangle the
	# compositor says it fills, before "and nothing else is in it" means
	# anything. Not 100%: the terminal prints its own name, and a few hundred
	# glyph pixels are the client's content, not a rendering error.
	pct="$(python3 -c "print(f'{100.0*${own:-0}/${area:-1}:.2f}')")"
	hl_assert "the $name window's own colour fills its surface rect ($own of $area, $pct%)" \
		"$(python3 -c "print('true' if ${own:-0} >= 0.99 * ${area:-1} else 'false')")" "true"
	hl_assert "the $name window's own colour appears nowhere outside it" \
		"$(get "${name}_own_outside_surface")" "0"
done

# ── the border is a border, not a fill ─────────────────────────────────────
echo "-- borders --"
for name in left right; do
	hl_assert "the $name window's border ring is painted all the way round" \
		"$(get "${name}_ring_painted")" "$(get "${name}_ring_area")"
	hl_assert "no wallpaper shows through the $name window's ring" \
		"$(get "${name}_ring_wallpaper")" "0"
	# THE ORIGINAL DEFECT, stated directly instead of as a pixel count copied
	# off another renderer.
	hl_assert "no trim colour ($(get "${name}_trim_left")/$(get "${name}_trim_right")) is painted inside the $name window" \
		"$(get "${name}_trim_inside_surface")" "0"
done

# ── the wallpaper ──────────────────────────────────────────────────────────
echo "-- wallpaper --"
WP_AREA="$(get outside_hull_area)"
hl_assert "there is a wallpaper-only region to judge (premise: $WP_AREA px)" \
	"$([ "${WP_AREA:-0}" -gt 10000 ] && echo true || echo false)" "true"
hl_assert "every pixel of it is the wallpaper, exactly" \
	"$(get outside_hull_nonwallpaper)" "0"

echo
echo "frame: $AVK_PNG"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
