#!/usr/bin/env bash
# avk-frame-test.sh — the desktop, composited by AVK, compared against SceneFX.
#
# M3's acceptance test. It boots a real compositor twice with the same config,
# the same clients and the same layout -- once on the SceneFX/wlroots path and
# once on asteroidz's own Vulkan engine -- and asserts the two frames agree.
#
# The comparison is the point. A test that only asserted "AVK produced a frame"
# would have passed on the build this test was written against, where every
# window rendered as a flat block of its own border colour. That is a frame. It
# is not a desktop. So the assertions here are on named colours at named
# places: this window is red, that window is green, and the border covers a few
# thousand pixels of trim rather than half the screen.
#
# The critical pairing is WLR_RENDERER=gles2 with ASTEROIDZ_RENDERER=avk. If
# those two were coupled, this test would prove nothing -- it would just be the
# Vulkan renderer compared against itself. Composited by Vulkan while wlroots
# holds GLES2 is the whole claim.
#
# Two things are deliberately NOT asserted equal, and both are logged by the
# compositor as AVK-mode limitations rather than hidden:
#
#   - the wallpaper. swaybg draws once and lets go of its buffer, and wlroots
#     has by then uploaded it into a wlr_texture that this renderer may not
#     touch. See "SHM surfaces and the wlr_client_buffer problem" in
#     docs/vulkan-native-architecture.md.
#   - shadows, blur and rounded corners, which are M4. The config below turns
#     them off so the two paths are comparable at all.
#
# Break test: BREAK=border reinstates the bug this test was written for, by
# telling the compositor to ignore the border's clipped-out interior. That run
# MUST fail; a pass there means the assertions are measuring nothing.
#
# There is a second switch, AVK_NO_FOREIGN_ACQUIRE=1, which disables the
# queue-family ownership transfer on imported buffers. It is NOT a break test
# HERE, and that is the interesting part: this suite passes with it set, and a
# real display comes up flat white. Nothing scans a headless buffer out, so the
# release half of that transfer -- the half that hands the finished frame back
# to KMS -- is invisible to every assertion below. Presentation is not covered
# by this file and cannot be; it takes a monitor.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-frame"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-frame-test: needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "avk-frame-test: needs python3-pillow"; exit 1; }

# No effects: AVK does not implement them yet, and a comparison against a path
# that does would be measuring the gap rather than the frame.
SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

# ── run the same scene on one backend ──────────────────────────────────────
# Leaves the screenshot at $HL_OUTDIR/shot.png and the log where run_backend's
# caller can read it.
run_backend() { # run_backend BACKEND OUTDIR
	local backend="$1" outdir="$2"
	HL_OUTDIR="$outdir"
	HL_WIDTH=1280 HL_HEIGHT=720
	HL_ENV="ASTEROIDZ_RENDERER=$backend"
	if [ "$BREAK" = border ] && [ "$backend" = avk ]; then
		HL_ENV="$HL_ENV AVK_NO_BORDER_CLIP=1"
	fi
	export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

	# The palette cursor is shell state, and both runs live in one shell. Left
	# alone, the second run spawns the THIRD and FOURTH colours and every
	# comparison fails on a difference the renderers had nothing to do with.
	HL_SPAWN_COLOR_IDX=0

	hl_start "$SCENE_KDL"
	hl_spawn_kitty one >/dev/null; hl_wait_client_count 1 60
	hl_spawn_kitty two >/dev/null; hl_wait_client_count 2 60
	# Let both terminals finish their first real paint. Asserting on a frame
	# caught mid-open animation compares two different scenes and fails for a
	# reason that has nothing to do with the renderer.
	sleep 2
	hl_screenshot shot
	hl_stop
}

pixel() { # pixel PNG X Y -> "#rrggbb"
	python3 - "$1" "$2" "$3" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
print("#%02x%02x%02x" % im.getpixel((int(sys.argv[2]), int(sys.argv[3]))))
PY
}

count_colour() { # count_colour PNG "#rrggbb" -> pixel count
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
want = tuple(int(sys.argv[2][i:i+2], 16) for i in (1, 3, 5))
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(n for n, c in im.getcolors(1 << 24) if c == want))
PY
}

# ── the two runs, one after the other ──────────────────────────────────────
# Sequentially, never in parallel: two headless compositors at once contend for
# the GPU and produce failures that look like rendering bugs.
BASE="${TMPDIR:-/tmp}/asteroidz-avk-frame-$$"
echo "== SceneFX reference run =="
run_backend wlr "$BASE/wlr"
echo "== AVK run =="
run_backend avk "$BASE/avk"

WLR_PNG="$BASE/wlr/shot.png"
AVK_PNG="$BASE/avk/shot.png"
AVK_LOG="$BASE/avk/state/asteroidz/asteroidz.log"

# ── the engine came up, and said so unambiguously ──────────────────────────
echo "-- startup --"
hl_assert "AVK announced itself as the rendering backend" \
	"$(grep -c 'Asteroidz rendering backend: AVK native Vulkan' "$AVK_LOG")" "1"
hl_assert "wlroots' renderer is named as a compatibility renderer only" \
	"$(grep -c 'wlroots compatibility renderer: gles2 -- protocols' "$AVK_LOG")" "1"

# ── AVK actually built the frames ──────────────────────────────────────────
stat_field() { sed -n "s/.*\($1=[0-9]*\).*/\1/p" "$AVK_LOG" | tail -1 | cut -d= -f2; }
FRAMES="$(stat_field 'avk\.frames')"
FALLBACKS="$(stat_field 'avk\.fallback_frames')"
SURFACES="$(stat_field 'avk\.surfaces')"
WAITS="$(stat_field 'avk\.cpu_sync_waits')"

echo "-- instrumentation --"
hl_assert "AVK composited frames" "$([ "${FRAMES:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "no frame fell back to SceneFX" "${FALLBACKS:-x}" "0"
hl_assert "AVK drew client surfaces" \
	"$([ "${SURFACES:-0}" -gt 0 ] && echo true || echo false)" "true"
# The rule the whole synchronisation design exists to keep. A nonzero value
# here means the frame path is blocking on the GPU somewhere.
hl_assert "no CPU sync waits on the frame path" "${WAITS:-x}" "0"

# ── the frame itself ───────────────────────────────────────────────────────
# Two tiled terminals, spawned in the harness's palette order: red on the left,
# green on the right. Sampled at the centre of each window, well inside the
# border and below the titlebar.
echo "-- window contents --"
for spec in "left 320 400 #aa2222" "right 960 400 #22aa22"; do
	set -- $spec
	name="$1" x="$2" y="$3" want="$4"
	hl_assert "SceneFX draws the $name window's own colour" \
		"$(pixel "$WLR_PNG" "$x" "$y")" "$want"
	hl_assert "AVK draws the $name window's own colour" \
		"$(pixel "$AVK_PNG" "$x" "$y")" "$want"
done

# ── the border is a border, not a fill ─────────────────────────────────────
# SceneFX draws a window border as ONE rect with the window's interior clipped
# out of it. A renderer that ignores the clip paints the whole window in the
# border colour -- and because the border sits ABOVE the surface in the scene,
# the window vanishes behind it. Comparing the border colour's pixel COUNT is
# what catches that: a few thousand pixels of trim versus half the screen.
echo "-- borders --"
for colour in "#2a6fd6" "#c66b25" "#eb441e"; do
	ref="$(count_colour "$WLR_PNG" "$colour")"
	got="$(count_colour "$AVK_PNG" "$colour")"
	# Exact, not approximate. These are flat-filled decorations at integer
	# coordinates; if the two paths disagree by even one pixel, the geometry
	# has drifted and that is worth knowing.
	hl_assert "AVK covers the same area in $colour as SceneFX ($ref px)" \
		"$got" "$ref"
done

echo
echo "reference: $WLR_PNG"
echo "avk:       $AVK_PNG"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
