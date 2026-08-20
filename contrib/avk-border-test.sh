#!/usr/bin/env bash
# avk-border-test.sh — M4B: the border is an annulus, and it has to close.
#
# WHAT THIS EXISTS TO CATCH
#
# A window border in the scene graph is ONE filled rect with the window's
# interior cut out of it (wlr_scene_rect_set_clipped_region, a fork extension
# that outlived the SceneFX renderer), and BOTH edges of that ring are rounded:
# the outside at the
# window's radius, the inside at the radius apply_border() computed for the
# client underneath. AVK carried the inner box but not its radii, so the ring's
# outside was an arc and its inside was a square. On each corner's diagonal the
# square hole cut away border that the outer arc had already curved away from,
# and nobody painted the wedge in between -- 104 pixels of wallpaper per corner,
# which is the artifact M4A classified and handed here.
#
# So the assertion is not "is there a border" but "is the ring CONTINUOUS":
# walking outward from a corner's arc centre the colours must run
#
#     client ... border ... wallpaper
#
# and wallpaper must never appear before border. A wedge is exactly a run of
# wallpaper on the wrong side of the ring.
#
# WHY THE PROBE IS COORDINATE-FREE
#
# The window's box is DETECTED in the captured image rather than mapped from
# its logical geometry, and the radii are MEASURED off the diagonal rather than
# assumed. That is what lets the same probe judge a rotated output and a
# fractional-scale one: a transform that permutes the outer corners but not the
# inner ones shows up as a corner whose ring does not close, with no need for
# this script to know which physical corner a logical one became. Reimplementing
# wlr_box_transform() in the test would just be a second chance to get the
# permutation wrong, and it would agree with the renderer when both were wrong.
#
# WHAT EACH CASE IS FOR
#
#   base        radius 40 / border 6 -- the original 104-pixel gap regression
#   square      radius 0: the ring is a plain frame and must still close
#   bw1 bw2 bw4 thin borders, where a one-pixel error is the whole border
#   near        border width close to the radius
#   over        border WIDER than the radius: pathological, must be stable
#   small big   radius 9 and radius 120
#   titlebar    TL squared by the titlebar rule, the other three rounded --
#               a real compositor state, and the one a single inner radius
#               renders wrong while looking almost right
#   scale15     fractional output scale
#   scale15tb   fractional scale AND asymmetric corners
#   rr90 rr180 rr270   rotated outputs, with asymmetric corners, so that
#               permuting the outer radii and not the inner ones is visible
#   opacity     window opacity < 1: the ring must not fringe against the
#               wallpaper showing through it
#   unfocused   the inactive border colour
#
#   BREAK=border-square-inner   restores the defect: rounded outer edge, SQUARE
#               inner cut. Every rounded case must fail against it, and a green
#               run is a suite failure -- WITH ONE RECORDED EXCEPTION. `near`
#               (radius 20, border 18) has an inner radius of 1, where square
#               and round are the same shape to within a pixel, so it stays
#               green under the break and gives ZERO coverage against it. That
#               is written down rather than left to be rediscovered as "the
#               break must be broken".
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-border"
BREAK="${BREAK:-}"
CASES="${CASES:-base square bw1 bw2 bw4 near over small big titlebar scale15 scale15tb rr90 rr180 rr270 opacity unfocused}"

REPAINT="$(dirname "$0")/wlrepaint/wlrepaint"
[ -x "$REPAINT" ] || { echo "not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

# 0x202020 client on the harness's flat grey wallpaper, with a border colour
# far from both so a three-way classification is unambiguous.
CLIENT_HEX=202020
# LOGICAL geometry, and it must fit on the smallest logical output any case
# produces. At scale 1.5 the 1920x1080 headless output is 1280x720 logical, so
# the 700x500-at-500,300 box this started with hung 80px off the bottom -- the
# edge rule then SQUARED the two bottom corners and the fixture read a correct
# compositor as a broken renderer. Clear of every edge by more than the largest
# radius under test, at both scales.
WIN_W=600; WIN_H=420; WIN_X=250; WIN_Y=160
FOCUS_COLOR="0xc66b25ff"
UNFOCUS_COLOR="0x2f6fd0ff"

# case            radius bw  scale rr titlebar opacity
case_params() {
	case "$1" in
		base)      echo "40  6 1   0 0 1.0" ;;
		square)    echo " 0  6 1   0 0 1.0" ;;
		bw1)       echo "40  1 1   0 0 1.0" ;;
		bw2)       echo "40  2 1   0 0 1.0" ;;
		bw4)       echo "40  4 1   0 0 1.0" ;;
		near)      echo "20 18 1   0 0 1.0" ;;
		over)      echo "12 20 1   0 0 1.0" ;;
		small)     echo " 9  3 1   0 0 1.0" ;;
		big)       echo "120 8 1   0 0 1.0" ;;
		titlebar)  echo "40  6 1   0 1 1.0" ;;
		scale15)   echo "40  6 1.5 0 0 1.0" ;;
		scale15tb) echo "40  6 1.5 0 1 1.0" ;;
		rr90)      echo "40  6 1   1 1 1.0" ;;
		rr180)     echo "40  6 1   2 1 1.0" ;;
		rr270)     echo "40  6 1   3 1 1.0" ;;
		opacity)   echo "40  6 1   0 0 0.75" ;;
		unfocused) echo "40  6 1   0 0 1.0" ;;
		*) return 1 ;;
	esac
}

[ -n "$BREAK" ] && echo "*** BREAK=$BREAK -- this build is deliberately wrong ***"
echo "cases: $CASES"

run_case() {
	local name="$1"
	set -- $(case_params "$name") || { echo "unknown case $name" >&2; return 1; }
	local radius="$1" bw="$2" scale="$3" rr="$4" titlebar="$5" opacity="$6"

	OUTDIR="${TMPDIR:-/tmp}/asteroidz-border-$name-$$"
	HL_OUTDIR="$OUTDIR"
	HL_SCALE1="$scale"
	HL_ENV=""
	[ "$BREAK" = border-square-inner ] && HL_ENV="$HL_ENV AZ_AVK_BORDER_SQUARE_INNER=1"
	[ -n "${VKDEBUG:-}" ] && HL_ENV="$HL_ENV ASTEROIDZ_VK_DEBUG=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
	[ "${FULLDRAW:-0}" = 1 ] && HL_ENV="$HL_ENV AZ_AVK_FULL_DAMAGE=1"
	[ -n "${BORDER_DEBUG:-}" ] && HL_ENV="$HL_ENV AZ_AVK_BORDER_DEBUG=1"
	export HL_OUTDIR HL_ENV HL_SCALE1

	echo
	echo "--- $name: radius=$radius border=$bw scale=$scale rr=$rr titlebar=$titlebar opacity=$opacity ---"

	# Shadows and blur off: both paint outside the window's box and would turn
	# "is this pixel wallpaper" into a judgement call right where the ring's
	# outer edge is being measured.
	hl_start "border_radius $radius
borderpx $bw
focuscolor $FOCUS_COLOR
bordercolor $UNFOCUS_COLOR
focused_opacity $opacity
unfocused_opacity $opacity
shadows 0
layer_shadows 0
gappih 0
gappiv 0
gappoh 0
gappov 0
animations 0
smartgaps 0
output $HL_MON { rr $rr }
effects { blur { enable 0 } }
layout {
    titlebar { enable $titlebar }
}"
	sleep 2

	# --ssd is not optional. A client that never binds xdg-decoration is CSD,
	# check_hit_no_border() gives it NO border, and apply_border() still insets
	# its surface by borderpx -- so the window looks bordered and the ring is
	# empty wallpaper. Measured without this, even a correct renderer "fails".
	"$REPAINT" --title wlborder --size "$WIN_W"x"$WIN_H" --solid "$CLIENT_HEX" \
		--frames 60 --ssd --hold-ms 100 > "$OUTDIR/wl.log" 2>&1 &
	HL_SPAWNED_PIDS+=("$!")
	hl_wait_client_count 1 40 >/dev/null 2>&1
	sleep 1

	# Floating and well clear of every screen edge: set_client_corner_location()
	# SQUARES any corner within border_radius of the monitor edge, so a window
	# touching an edge has no arc there to measure and the fixture would report
	# the compositor's correct behaviour as a renderer bug.
	hl_dispatch "toggle_floating" 0.5
	hl_dispatch "resize_window,$WIN_W,$WIN_H" 0.5
	hl_dispatch "move_window,$WIN_X,$WIN_Y" 1.5

	if [ "$name" = unfocused ]; then
		# A second window takes focus, so the first paints its INACTIVE colour.
		"$REPAINT" --title wlother --size 200x160 --solid 505050 --frames 30 \
			--ssd --hold-ms 100 > "$OUTDIR/wl2.log" 2>&1 &
		HL_SPAWNED_PIDS+=("$!")
		hl_wait_client_count 2 40 >/dev/null 2>&1
		hl_dispatch "toggle_floating" 0.5
		# Clear of the window under test, which now sits at WIN_X,WIN_Y: an
		# overlapping focus-stealer would occlude the very ring being measured.
		hl_dispatch "move_window,$((WIN_X + WIN_W + 120)),$((WIN_Y + WIN_H + 80))" 1.5
	fi
	# WAIT FOR THE CLIENT TO SETTLE, and treat not settling as an INVALID TEST.
	# wlrepaint reconfigures its buffers on every xdg configure, so between the
	# resize and the client catching up the border is drawn at the NEW size
	# while the surface is still the OLD one -- the ring is then genuinely open,
	# for a frame, for reasons that have nothing to do with its geometry.
	# Captured mid-flight this reads as a renderer bug, and it reads as a
	# DIFFERENT one each run: the measured corner radii swung between 30 and 57
	# on identical inputs before this wait existed.
	settled=false
	for _ in $(seq 1 40); do
		if grep -q "settled" "$OUTDIR/wl.log" 2>/dev/null; then settled=true; break; fi
		sleep 0.25
	done
	sleep 1

	hl_screenshot "shot"
	hl_assert "$name: the client settled before the capture" "$settled" "true"

	# PREMISE from the renderer's own counters, not from pixels: the ROUNDED
	# border path has to have been taken. Without this a build that quietly
	# stopped rounding the inner edge would score a perfect zero wedge, because
	# a square ring on a square hole has no seam to open.
	# Only where an inner ARC is actually expected. apply_border() computes the
	# inner radius as max(border_radius - bw - 1, 0), so a border WIDER than the
	# radius legitimately gets a square inner cut and rounded_border_draws stays
	# 0 -- that is the reference behaviour (GEZERO clamps; there is no unsigned
	# underflow to worry about), not a renderer that stopped rounding.
	if [ $((radius - bw - 1)) -gt 0 ]; then
		rb=$(hl_get "get avk-stats" | jq -r '.rounded_border_draws // 0')
		hl_assert "$name: the rounded-border path was taken ($rb draws)" \
			"$([ "${rb:-0}" -gt 0 ] && echo true || echo false)" "true"
		if [ "$titlebar" = 1 ]; then
			ab=$(hl_get "get avk-stats" | jq -r '.asymmetric_border_draws // 0')
			hl_assert "$name: per-corner inner radii reached the shader ($ab draws)" \
				"$([ "${ab:-0}" -gt 0 ] && echo true || echo false)" "true"
		fi
	fi

	python3 - "$OUTDIR/shot.png" "$radius" "$bw" "$scale" "$name" \
		> "$OUTDIR/probe.txt" 2>&1 <<'PY'
import sys, math
from PIL import Image

path, radius, bw, scale, name = (sys.argv[1], int(sys.argv[2]),
	int(sys.argv[3]), float(sys.argv[4]), sys.argv[5])
im = Image.open(path).convert('RGB')
W, H = im.size
px = im.load()
bw_out = max(1, int(round(bw * scale)))

def dist(a, b):
	return sum((a[i] - b[i]) ** 2 for i in range(3)) ** 0.5

# REFERENCE COLOURS ARE SAMPLED, NOT ASSUMED. Window opacity blends the client
# and the border with the wallpaper behind them, and the inactive border is a
# different colour again; hardcoding either would make the classifier disagree
# with the very cases it exists to judge.
wallpaper = px[4, 4]

# The client fill is the one colour known a priori (wlrepaint --solid), which is
# what makes finding the window independent of the border being right.
CLIENT = (0x20, 0x20, 0x20)
xs, ys = [], []
step = 2
for y in range(0, H, step):
	for x in range(0, W, step):
		p = px[x, y]
		# accept a blend of the client with the wallpaper (opacity < 1)
		if dist(p, CLIENT) < 40 or (0.55 < (p[0] - wallpaper[0]) / (CLIENT[0] - wallpaper[0] or 1) < 1.45
				and max(p) - min(p) < 24 and dist(p, wallpaper) > 30):
			xs.append(x); ys.append(y)
if len(xs) < 500:
	print("INVALID TEST: no client fill found in the capture")
	sys.exit(1)
cx0, cx1, cy0, cy1 = min(xs), max(xs), min(ys), max(ys)
client_mid = ((cx0 + cx1) // 2, (cy0 + cy1) // 2)
client = px[client_mid]

# Grow from the client rect to the OUTER box by walking until wallpaper. The
# inner rect is deliberately NOT assumed to be a symmetric inset of the outer
# one -- apply_border() compensates it against the monitor edge -- so the outer
# box is measured on all four sides independently.
def walk(x, y, dx, dy):
	n = 0
	while 0 <= x < W and 0 <= y < H and dist(px[x, y], wallpaper) > 26 and n < 400:
		x += dx; y += dy; n += 1
	return x - dx, y - dy

left  = walk(cx0, client_mid[1], -1, 0)[0]
right = walk(cx1, client_mid[1],  1, 0)[0]
top   = walk(client_mid[0], cy0, 0, -1)[1]
bot   = walk(client_mid[0], cy1, 0,  1)[1]
print(f"outerbox {left},{top} {right - left + 1}x{bot - top + 1} "
	f"client {cx0},{cy0} {cx1 - cx0 + 1}x{cy1 - cy0 + 1}")

# The border colour is read off the middle of the LEFT edge, where the ring is
# a straight run and antialiasing has nothing to do.
border = px[min(W - 1, left + bw_out // 2), client_mid[1]]
print(f"colours wallpaper={wallpaper} border={border} client={client}")

# PREMISE: the three references have to be separable, or every classification
# below is a coin flip.
sep = min(dist(border, wallpaper), dist(client, wallpaper), dist(border, client))
if sep < 30:
	print(f"INVALID TEST: reference colours are only {sep:.0f} apart")
	sys.exit(1)

def classify(p):
	d = [(dist(p, wallpaper), 'W'), (dist(p, border), 'B'), (dist(p, client), 'C')]
	d.sort()
	# Anything far from all three is the titlebar or an antialiased blend; it is
	# not evidence either way and must not be counted as wallpaper.
	return d[0][1] if d[0][0] < 26 else '?'

corners = [('TL', left, top, 1, 1), ('TR', right, top, -1, 1),
	('BR', right, bot, -1, -1), ('BL', left, bot, 1, -1)]

total_wedge = total_spill = ring_px = closed = 0
radii = []
for cname, cxp, cyp, sx, sy in corners:
	# MEASURE the outer radius on the diagonal: the arc crosses it at
	# r*(sqrt2 - 1) from the corner, so a square corner reads as 0.
	t = 0.0
	while t < 400:
		# round, do not truncate: int() biases toward the origin, which on the
		# two corners walking in -x/-y reads the arc several pixels early and
		# reports an asymmetry the pixels do not have.
		x = int(round(cxp + sx * t / 1.4142))
		y = int(round(cyp + sy * t / 1.4142))
		if not (0 <= x < W and 0 <= y < H):
			break
		if dist(px[x, y], wallpaper) > 26:
			break
		t += 0.5
	r_meas = t / (math.sqrt(2) - 1)
	radii.append(round(r_meas))

	# Rays outward from a point INSIDE the window. The invariant is independent
	# of where that point is: any outward ray must run client, then border,
	# then wallpaper, and must never come back. So this needs no arc centre, no
	# corner permutation and no transform arithmetic to be correct.
	inset = max(r_meas, bw_out) + 6
	ox, oy = cxp + sx * inset, cyp + sy * inset
	bad_w = bad_b = 0
	for deg in range(0, 91):
		a = math.radians(deg)
		dx, dy = -sx * math.cos(a), -sy * math.sin(a)
		seq = []
		t = 0.0
		while t < inset * 1.6 + bw_out + 10:
			x, y = int(ox + dx * t), int(oy + dy * t)
			if not (0 <= x < W and 0 <= y < H):
				break
			seq.append(classify(px[x, y]))
			t += 0.5
		if 'B' not in seq:
			continue
		ring_px += seq.count('B')
		last_b = len(seq) - 1 - seq[::-1].index('B')
		first_w = seq.index('W') if 'W' in seq else None
		if first_w is not None and first_w < last_b:
			# Wallpaper on the INSIDE of the ring: the wedge.
			bad_w += sum(1 for c in seq[:last_b] if c == 'W')
			# ...and equivalently, paint on the far side of the wallpaper.
			bad_b += sum(1 for c in seq[first_w:] if c in 'BC')
	if bad_w == 0 and bad_b == 0:
		closed += 1
	else:
		print(f"open {cname} r={r_meas:.1f} wedge={bad_w} spill={bad_b}")
	total_wedge += bad_w
	total_spill += bad_b

print(f"radii {'/'.join(str(r) for r in radii)} (tl/tr/br/bl, expected ~"
	f"{round(radius * scale)} where rounded, 0 where squared)")
print(f"ringpx {ring_px}")
print(f"wedge {total_wedge}")
print(f"spill {total_spill}")
print(f"closed {closed}")
PY
	local rc=$?
	sed 's/^/  /' "$OUTDIR/probe.txt"

	if [ ! -s "$OUTDIR/shot.png" ]; then
		# grim returns nothing for a 90/270-rotated headless output. That is a
		# CAPTURE limitation, not a render result, so it is skipped out loud
		# rather than counted either way. rr180 does capture and carries the
		# transform coverage: its window is titlebar-asymmetric, so a build that
		# permuted the outer radii and not the inner ones would open the ring.
		hl_skip "$name: no capture -- grim does not read back a rotated headless output"
		hl_stop
		return 0
	fi
	if [ $rc -ne 0 ]; then
		hl_assert "$name: the probe could measure the window at all" "false" "true"
		hl_stop
		return 0
	fi

	local wedge ring closed rmeas
	wedge=$(awk '/^wedge /{print $2}' "$OUTDIR/probe.txt")
	spill=$(awk '/^spill /{print $2}' "$OUTDIR/probe.txt")
	ring=$(awk '/^ringpx /{print $2}' "$OUTDIR/probe.txt")
	closed=$(awk '/^closed /{print $2}' "$OUTDIR/probe.txt")
	rmeas=$(awk '/^radii /{print $2}' "$OUTDIR/probe.txt")

	# PREMISE. Without these, "0 wedge pixels" only says the detector never
	# fired -- a window with no border at all scores a perfect zero.
	hl_assert "$name: a border is actually painted" \
		"$([ "${ring:-0}" -gt 200 ] && echo true || echo false)" "true"
	hl_assert "$name: the corners really are cut" \
		"$([ "${rmeas:-none}" != none ] && echo true || echo false)" "true"

	hl_assert "$name: no wallpaper inside the ring" "${wedge:-x}" "0"
	hl_assert "$name: no border outside the ring" "${spill:-x}" "0"
	hl_assert "$name: every corner's ring closes" "${closed:-x}" "4"

	hl_stop
}

# ── border damage ───────────────────────────────────────────────────────────
#
# A border extends the visible geometry of a window, so moving, resizing or
# recolouring one has to remove the OLD ring as well as paint the new one. The
# question is not "is the new border right" -- every case above answers that --
# but "is anything left where the old one was", and it must be answered without
# a full-output redraw.
run_damage() {
	OUTDIR="${TMPDIR:-/tmp}/asteroidz-border-damage-$$"
	HL_OUTDIR="$OUTDIR"
	HL_SCALE1=1
	HL_ENV=""
	[ "$BREAK" = border-square-inner ] && HL_ENV="$HL_ENV AZ_AVK_BORDER_SQUARE_INNER=1"
	# THESE FOUR ASSERTIONS ARE NOT FALSIFIED, and that is recorded rather than
	# glossed. The obvious break -- AZ_AVK_DAMAGE_HOLE over the band the window
	# vacates -- does NOT make them fail, and the reason is instructive: the
	# hole is applied from compositor start, so the region is frozen before the
	# window ever paints a border into it. "No stale border survives" then comes
	# out true because no border was ever there. A break that makes a test pass
	# for the wrong reason is worse than no break, so it is not wired up.
	#
	# A real falsifier has to withhold damage only AFTER the move. Until one
	# exists these four claim no coverage of their own; move-damage correctness
	# is carried by avk-rounded-alpha-test.sh ("movement leaves no square
	# ghost") and avk-damage-test.sh, which do have proven falsifiers.
	export HL_OUTDIR HL_ENV HL_SCALE1

	echo
	echo "--- damage: move, resize and focus colour, all under partial redraw ---"
	hl_start "border_radius 40
borderpx 6
focuscolor $FOCUS_COLOR
bordercolor $UNFOCUS_COLOR
shadows 0
layer_shadows 0
gappih 0
gappiv 0
gappoh 0
gappov 0
animations 0
smartgaps 0
effects { blur { enable 0 } }
layout {
    titlebar { enable 0 }
}"
	sleep 2
	"$REPAINT" --title wlborder --size "$WIN_W"x"$WIN_H" --solid "$CLIENT_HEX" \
		--frames 200 --ssd --hold-ms 100 > "$OUTDIR/wl.log" 2>&1 &
	HL_SPAWNED_PIDS+=("$!")
	hl_wait_client_count 1 40 >/dev/null 2>&1
	sleep 1
	hl_dispatch "toggle_floating" 0.5
	hl_dispatch "resize_window,$WIN_W,$WIN_H" 0.5
	hl_dispatch "move_window,$WIN_X,$WIN_Y" 2
	sleep 2
	hl_screenshot "d_before"

	full_before=$(hl_get "get avk-stats" | jq -r '.full_redraw_frames')
	hl_dispatch "move_window,$((WIN_X + 220)),$((WIN_Y + 140))" 2
	sleep 2
	hl_screenshot "d_moved"
	hl_dispatch "resize_window,$((WIN_W - 140)),$((WIN_H - 100))" 2
	sleep 2
	hl_screenshot "d_resized"

	# A second window steals focus: the first must repaint its ring in the
	# INACTIVE colour, everywhere, including the corners.
	"$REPAINT" --title wlother --size 200x160 --solid 505050 --frames 60 \
		--ssd --hold-ms 100 > "$OUTDIR/wl2.log" 2>&1 &
	HL_SPAWNED_PIDS+=("$!")
	hl_wait_client_count 2 40 >/dev/null 2>&1
	hl_dispatch "toggle_floating" 0.5
	# Clear of BOTH the original box and the moved one: this window is the
	# FOCUSED one, so it wears the active colour, and parked over the vacated
	# band its perfectly correct border reads as 70 pixels of stale paint.
	hl_dispatch "move_window,$((WIN_X + WIN_W + 460)),$((WIN_Y + WIN_H + 260))" 2
	sleep 2
	hl_screenshot "d_unfocused"

	python3 - "$OUTDIR" "$WIN_X" "$WIN_Y" "$WIN_W" "$WIN_H" \
		> "$OUTDIR/damage.txt" 2>&1 <<'PY2'
import sys
from PIL import Image
out, x, y, w, h = sys.argv[1], *(int(v) for v in sys.argv[2:6])
WALL = (128, 128, 128)
FOCUS = (198, 107, 37)
def load(n): return Image.open(f"{out}/{n}.png").convert('RGB').load()
def near(p, q, t=30):
    return sum((p[i] - q[i]) ** 2 for i in range(3)) ** 0.5 < t

# The box the window VACATED must be back to wallpaper -- no ring left behind.
for shot in ("d_moved", "d_resized", "d_unfocused"):
    px = load(shot)
    stale = 0
    for yy in range(y - 2, y + 12):
        for xx in range(x - 2, x + w + 2):
            if near(px[xx, yy], FOCUS):
                stale += 1
    print(f"vacated_{shot} {stale}")

# The window that lost focus must carry no active-colour pixel ANYWHERE ON
# ITSELF. Scanned over its own box only -- the window that TOOK focus is
# correctly wearing the active colour, and a screen-wide scan counts that as
# 870 pixels of stale border.
px = load("d_unfocused")
mx, my = x + 220, y + 140            # after the move
mw, mh = w - 140, h - 100            # after the resize
active = sum(1 for yy in range(my - 2, my + mh + 2)
             for xx in range(mx - 2, mx + mw + 2)
             if near(px[xx, yy], FOCUS))
print(f"active_after_unfocus {active}")
PY2
	sed 's/^/  /' "$OUTDIR/damage.txt"
	full_after=$(hl_get "get avk-stats" | jq -r '.full_redraw_frames')
	part_after=$(hl_get "get avk-stats" | jq -r '.partial_redraw_frames')
	echo "  full redraws across the sequence: $((full_after - full_before)); partial total $part_after"
	hl_get "get avk-stats" | jq -r '"  cpu_frame_us p50=\(.cpu_frame_us_p50) p95=\(.cpu_frame_us_p95) p99=\(.cpu_frame_us_p99) max=\(.cpu_frame_us_max)  border_draws=\(.border_draws) rounded=\(.rounded_border_draws) asym=\(.asymmetric_border_draws)"'
	hl_assert "damage: no CPU sync wait was introduced" \
		"$(hl_get "get avk-stats" | jq -r '.cpu_sync_waits')" "0"
	hl_assert "damage: no fallback frame was introduced" \
		"$(hl_get "get avk-stats" | jq -r '.fallback_frames')" "0"

	for k in d_moved d_resized d_unfocused; do
		v=$(awk -v k="vacated_$k" '$1==k{print $2}' "$OUTDIR/damage.txt")
		hl_assert "damage: nothing of the old border survives $k" "${v:-x}" "0"
	done
	a=$(awk '$1=="active_after_unfocus"{print $2}' "$OUTDIR/damage.txt")
	hl_assert "damage: no active-colour border survives losing focus" "${a:-x}" "0"
	hl_stop
}

for c in $CASES; do
	run_case "$c"
done
[ "${DAMAGE:-1}" = 1 ] && run_damage

hl_summary
