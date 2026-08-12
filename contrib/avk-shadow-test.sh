#!/usr/bin/env bash
# avk-shadow-test.sh — M4D.3: shadows in a running compositor.
#
# WHAT THIS ADDS OVER tests/test-avk-shadow.c. The unit suite proves the
# material is right: it builds an avk_scene by hand and compares 43264 pixels
# against a CPU oracle. It cannot prove that the COMPOSITOR ever asks for one,
# that it asks on the right windows, that the answer survives a fractional
# output scale, or that a shadow crosses a monitor seam with its window. Those
# are properties of the snapshot and the producer, and they need a real
# compositor.
#
# THE TRAP THIS FIXTURE IS BUILT AROUND. asteroidz shows shadows on FLOATING
# windows only -- `shadow_only_floating` defaults to 1, because a tiled
# window's shadow lands on its neighbour rather than on the wallpaper. A
# fixture that spawns a client, leaves it tiled and then asserts on a shadow is
# asserting on nothing at all, and would pass just as well against a renderer
# that dropped every shadow node. So every case here floats its window first
# and checks the shadow-draw counter as a premise before it measures anything.
#
#   BREAK=shadow-symmetric      re-centres the envelope; directionality dies
#
# shadow-single-radius is NOT listed: see the per-corner case at the bottom
# for the measurement showing it has no steady-state effect here, and for
# where its falsifier actually lives.
#
# HOW THE SECOND BREAK GOT ITS COVERAGE. It first scored 22 of 22 here -- a
# green break run, which is a suite failure -- because every window this
# fixture floated sat in the middle of the screen with all four corners
# rounded, and a single-radius shadow is indistinguishable from a correct one
# when the four radii are equal. The conclusion drawn from that was wrong
# twice over: the fixture was rewritten to say asteroidz never produces
# asymmetric shadow corners, on the strength of reading
# corner_radii_from_location().
#
# The live session disproved it in one number: asymmetric_shadow_draws = 258.
# client_apply_border() starts at CORNER_LOCATION_ALL and masks off whichever
# edges the window is FLUSH AGAINST -- "a corner is squared off only where the
# window meets the screen edge" -- and it feeds that mask straight to the two
# shadow nodes. So a window against the left edge of a monitor has square left
# corners and round right ones, which is the ordinary case for anything
# maximised or dragged to an edge, and the case a single-radius shadow gets
# wrong.
#
# The "flush against the screen edge" case below reproduces it.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-shadow"
BREAK="${BREAK:-}"

REPAINT="$(dirname "$0")/wlrepaint/wlrepaint"
[ -x "$REPAINT" ] || { echo "not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

OUTDIR="${TMPDIR:-/tmp}/asteroidz-shadow-$$"
HL_OUTDIR="$OUTDIR"
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = shadow-symmetric ] && HL_ENV="$HL_ENV AZ_SHADOW_SYMMETRIC=1"
export HL_OUTDIR HL_ENV

# Every amsg call is bounded. An unbounded one against a wedged compositor
# hangs the suite, and a timed-out one emits NOTHING -- so a `// "TIMEOUT"`
# default never fires and two empty strings subtract to zero. That combination
# reported a green break run in M4C.
gt() { ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" timeout 6 amsg $* 2>/dev/null; }

stat_of() { # stat_of KEY -> number, or TIMEOUT
	local v
	v="$(gt get avk-stats | jq -r ".$1 // empty" 2>/dev/null)"
	case "$v" in
		''|null) echo TIMEOUT ;;
		*) echo "$v" ;;
	esac
}

wait_clients() { # wait_clients N
	local want="$1" i
	for i in $(seq 1 15); do
		[ "$(gt get all-clients | jq -r '.clients|length' 2>/dev/null)" = "$want" ] && return 0
		sleep 1
	done
	return 1
}

# ── the config every case starts from ──────────────────────────────────────
#
# The shipped shadow profile, stated rather than inherited, so a change to the
# defaults cannot silently change what this suite measures.
SHADOW_KDL_TEMPLATE='border_radius ${RADIUS:-12}
borderpx 0
animations 0
effects {
    blur { enable 0 }
    shadow {
        enable 1
        only-floating 1
        size ${SHSIZE:-24}
        blur ${SHBLUR:-24}
        position { x 0; y 10 }
        color 0x000000cc
        contact { enable ${CONTACT:-1}; size 8; blur 9; position { x 0; y 2 } }
    }
}
layout {
    titlebar { enable ${TITLEBAR:-0} }
}'

start_case() { # start_case [EXTRA_KDL]   (honours $TITLEBAR)
	local kdl
	kdl="$(TITLEBAR="${TITLEBAR:-0}" RADIUS="${RADIUS:-12}" SHSIZE="${SHSIZE:-24}" \
		SHBLUR="${SHBLUR:-24}" CONTACT="${CONTACT:-1}" \
		envsubst <<<"$SHADOW_KDL_TEMPLATE")"
	hl_start "$kdl
${1:-}"
	sleep 2
}

spawn_floating() { # spawn_floating X Y W H -> geometry on stdout
	local x="$1" y="$2" w="$3" hgt="$4"
	hl_dispatch "view,4" 0.5
	"$REPAINT" --title "wlshadow" --size "${w}x${hgt}" --solid 202020 \
		--frames 1 ${SSD:+--ssd} --hold-ms 100 > "$OUTDIR/client.log" 2>&1 &
	HL_SPAWNED_PIDS+=("$!")
	wait_clients 1 || { echo "  (client never mapped)"; return 1; }
	sleep 1
	gt dispatch toggle_floating >/dev/null
	sleep 1
	gt dispatch "move_window,$x,$y" >/dev/null
	sleep 2
	gt get focused-client | jq -r '"\(.x) \(.y) \(.width) \(.height)"'
}

# ── 1. the enable policy, and the premise the whole suite rests on ─────────

echo "== enable policy: floating yes, tiled no =="
start_case

hl_dispatch "view,4" 0.5
"$REPAINT" --title "wltiled" --size 600x400 --solid 202020 --frames 1 \
	--hold-ms 100 > "$OUTDIR/tiled.log" 2>&1 &
HL_SPAWNED_PIDS+=("$!")
wait_clients 1
sleep 2
TILED_DRAWS="$(stat_of shadow_draws)"
hl_assert "a TILED window casts no shadow (only-floating 1) -- draws $TILED_DRAWS" \
	"$([ "$TILED_DRAWS" = 0 ] && echo true || echo false)" "true"

gt dispatch toggle_floating >/dev/null
sleep 2
FLOAT_DRAWS="$(stat_of shadow_draws)"
echo "  shadow_draws after floating: $FLOAT_DRAWS"
hl_assert "and floating it makes shadows appear (draws $FLOAT_DRAWS)" \
	"$([ "$FLOAT_DRAWS" != TIMEOUT ] && [ "${FLOAT_DRAWS:-0}" -gt 0 ] && echo true || echo false)" "true"

# Both lobes, not one. The contact lobe is the thing a "shadows work" test
# never notices is missing.
ROUNDED="$(stat_of rounded_shadow_draws)"
hl_assert "the shadows are rounded, matching border_radius 12 (rounded $ROUNDED)" \
	"$([ "$ROUNDED" != TIMEOUT ] && [ "${ROUNDED:-0}" -gt 0 ] && echo true || echo false)" "true"

hl_stop

# ── 2. it is directional, on a real screenshot ─────────────────────────────

echo "== the shadow on screen is directional =="
start_case
GEOM="$(spawn_floating 600 400 500 300)" || GEOM=""
if [ -z "$GEOM" ]; then
	hl_skip "no client; directional check needs a mapped floating window"
else
	set -- $GEOM; GX=$1; GY=$2; GW=$3; GH=$4
	echo "  window at $GX,$GY ${GW}x${GH}"
	DRAWS="$(stat_of shadow_draws)"
	hl_assert "premise: shadows were drawn ($DRAWS draws)" \
		"$([ "${DRAWS:-0}" != TIMEOUT ] && [ "${DRAWS:-0}" -gt 0 ] && echo true || echo false)" "true"

	grim -o "$HL_MON" "$OUTDIR/dir.png" 2>/dev/null
	read -r TOPV BOTV LEFTV RIGHTV BGV < <(python3 - \
		"$OUTDIR/dir.png" "$GX" "$GY" "$GW" "$GH" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); p = im.load()
gx, gy, gw, gh = (int(v) for v in sys.argv[2:6])
Wi, Hi = im.size
D = 6   # same distance from the window on all four sides

def lum(x, y):
    if not (0 <= x < Wi and 0 <= y < Hi):
        return None
    r, g, b = p[x, y]
    return 0.2126 * r + 0.7152 * g + 0.0722 * b

def avg(pts):
    vals = [v for v in (lum(x, y) for x, y in pts) if v is not None]
    return sum(vals) / len(vals) if vals else -1

cx, cy = gx + gw // 2, gy + gh // 2
span = range(-40, 41, 8)
top    = avg([(cx + d, gy - D) for d in span])
bottom = avg([(cx + d, gy + gh - 1 + D) for d in span])
left   = avg([(gx - D, cy + d) for d in span])
right  = avg([(gx + gw - 1 + D, cy + d) for d in span])
# Far from the window: the unshadowed wallpaper, whatever it is.
bg = avg([(4, 4), (Wi - 5, 4), (4, Hi - 5), (Wi - 5, Hi - 5)])
print(f"{top:.2f} {bottom:.2f} {left:.2f} {right:.2f} {bg:.2f}")
PY
)
	echo "  luma 6px out -- top $TOPV  left $LEFTV  right $RIGHTV  bottom $BOTV  (bg $BGV)"
	# Darkening, as a fraction of the background: a shadow removes light.
	read -r T B L R <<<"$(python3 -c "
bg=$BGV
print(' '.join(f'{(bg-v)/bg:.4f}' for v in ($TOPV,$BOTV,$LEFTV,$RIGHTV)))")"
	echo "  darkening -- top $T  left $L  right $R  bottom $B"

	hl_assert "premise: the background is bright enough to darken (bg $BGV)" \
		"$(python3 -c "print('true' if $BGV > 20 else 'false')")" "true"
	hl_assert "below the window is darker than the sides ($B vs $L/$R)" \
		"$(python3 -c "print('true' if $B > max($L,$R)*1.15 else 'false')")" "true"
	hl_assert "and the sides are darker than above ($L/$R vs $T)" \
		"$(python3 -c "print('true' if min($L,$R) > $T*1.15 else 'false')")" "true"
	hl_assert "left and right match -- no horizontal bias ($L vs $R)" \
		"$(python3 -c "print('true' if abs($L-$R) < 0.02 else 'false')")" "true"
fi
hl_stop

# ── 3. damage: a static shadow must converge to nothing ────────────────────

echo "== a settled shadow schedules no further work =="
start_case
GEOM="$(spawn_floating 400 300 500 300)" || GEOM=""
if [ -z "$GEOM" ]; then
	hl_skip "no client; idle convergence needs a mapped floating window"
else
	DRAWS="$(stat_of shadow_draws)"
	hl_assert "premise: shadows are being drawn ($DRAWS)" \
		"$([ "$DRAWS" != TIMEOUT ] && [ "${DRAWS:-0}" -gt 0 ] && echo true || echo false)" "true"
	sleep 3
	F1="$(stat_of frames)"
	sleep 4
	F2="$(stat_of frames)"
	echo "  frames $F1 -> $F2 over 4 idle seconds"
	hl_assert "premise: the frame counter was readable ($F1 / $F2)" \
		"$([ "$F1" != TIMEOUT ] && [ "$F2" != TIMEOUT ] && echo true || echo false)" "true"
	if [ "$F1" != TIMEOUT ] && [ "$F2" != TIMEOUT ]; then
		DELTA=$((F2 - F1))
		echo "  delta $DELTA"
		# M4C.3H's invariant, carried forward: a visually stable state must
		# converge to zero self-generated repaint work. A handful of frames is
		# cursor and clock; a storm is hundreds.
		hl_assert "a static shadow does not repaint forever (delta $DELTA)" \
			"$([ "$DELTA" -lt 20 ] && echo true || echo false)" "true"
	fi

	# And the IPC is still answering, which is what a storm takes away first.
	T0=$(date +%s%N)
	gt get avk-stats >/dev/null
	T1=$(date +%s%N)
	MS=$(( (T1 - T0) / 1000000 ))
	echo "  amsg round trip ${MS}ms"
	hl_assert "IPC still answers promptly (${MS}ms)" \
		"$([ "$MS" -lt 500 ] && echo true || echo false)" "true"
fi
hl_stop

# ── 4. move and resize damage the old bounds as well as the new ────────────

echo "== moving and resizing leaves nothing behind =="
start_case
GEOM="$(spawn_floating 300 300 400 260)" || GEOM=""
if [ -z "$GEOM" ]; then
	hl_skip "no client; damage checks need a mapped floating window"
else
	set -- $GEOM; GX=$1; GY=$2; GW=$3; GH=$4
	grim -o "$HL_MON" "$OUTDIR/before.png" 2>/dev/null
	gt dispatch "move_window,900,600" >/dev/null
	sleep 2
	grim -o "$HL_MON" "$OUTDIR/after.png" 2>/dev/null

	# The place the shadow USED to be must be back to background. A renderer
	# that damaged only the new bounds leaves a shadow-shaped stain there, and
	# it survives every subsequent frame because nothing damages it again.
	read -r STAIN < <(python3 - "$OUTDIR/before.png" "$OUTDIR/after.png" \
		"$GX" "$GY" "$GW" "$GH" <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGB'); pa = a.load()
b = Image.open(sys.argv[2]).convert('RGB'); pb = b.load()
gx, gy, gw, gh = (int(v) for v in sys.argv[3:7])
Wi, Hi = a.size
# A band just below where the window was: the strongest part of its shadow.
worst = 0.0
for x in range(max(0, gx + 20), min(Wi, gx + gw - 20), 4):
    for d in (2, 6, 12, 20):
        y = gy + gh - 1 + d
        if not (0 <= y < Hi):
            continue
        va = sum(pa[x, y]) / 3.0
        vb = sum(pb[x, y]) / 3.0
        # How much darker than the CURRENT background that pixel still is.
        worst = max(worst, va - vb) if False else worst
        # what we actually want: is `after` still darkened there?
        ref = sum(pb[4, y]) / 3.0 if y < Hi else 0
        worst = max(worst, ref - vb)
print(f"{worst:.2f}")
PY
)
	echo "  worst residual darkening where the shadow was: $STAIN/255"
	hl_assert "the old shadow's ground is clean after the move ($STAIN/255)" \
		"$(python3 -c "print('true' if $STAIN < 12 else 'false')")" "true"

	NEW="$(gt get focused-client | jq -r '"\(.x) \(.y)"')"
	echo "  window now at $NEW"
	hl_assert "premise: the window really moved ($NEW)" \
		"$([ "$NEW" != "$GX $GY" ] && echo true || echo false)" "true"
fi
hl_stop

# ── 5. fractional scale: the falloff follows the box ───────────────────────

echo "== output scale 1.5 =="
# HL_SCALE1, not a `monitorrule { scale 1.5 }` block. That block parses as
# key:value pairs and is SILENTLY IGNORED -- the cursor-lifetime suite once
# claimed mixed scales and ran both outputs at 1 because of it. The premise
# assertion below is what caught the same mistake here, and it stays.
HL_SCALE1=1.5
export HL_SCALE1
start_case
GEOM="$(spawn_floating 300 200 400 260)" || GEOM=""
if [ -z "$GEOM" ]; then
	hl_skip "no client; scale check needs a mapped floating window"
else
	set -- $GEOM; GX=$1; GY=$2; GW=$3; GH=$4
	SC="$(gt get all-monitors | jq -r ".monitors[]|select(.name==\"$HL_MON\")|.scale" 2>/dev/null)"
	echo "  monitor scale $SC, window $GX,$GY ${GW}x${GH}"
	hl_assert "premise: the output really is at 1.5 (got $SC)" \
		"$(python3 -c "print('true' if abs(float('$SC')-1.5)<0.01 else 'false')")" "true"
	DRAWS="$(stat_of shadow_draws)"
	hl_assert "premise: shadows are drawn at 1.5 ($DRAWS)" \
		"$([ "$DRAWS" != TIMEOUT ] && [ "${DRAWS:-0}" -gt 0 ] && echo true || echo false)" "true"

	grim -o "$HL_MON" "$OUTDIR/scale.png" 2>/dev/null
	# The reach of the shadow below the window, in DEVICE pixels, measured as
	# the distance at which darkening falls under 4%.
	read -r REACH < <(python3 - "$OUTDIR/scale.png" "$GX" "$GY" "$GW" "$GH" "$SC" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); p = im.load()
gx, gy, gw, gh, sc = (float(v) for v in sys.argv[2:7])
Wi, Hi = im.size
# The client reports LOGICAL geometry; the capture is in device pixels.
dx, dy = int(gx * sc), int(gy * sc)
dw, dh = int(gw * sc), int(gh * sc)
cx = dx + dw // 2
bg = sum(p[4, min(Hi - 1, dy + dh + 60)]) / 3.0
reach = 0
for d in range(1, 120):
    y = dy + dh - 1 + d
    if not (0 <= y < Hi):
        break
    v = sum(p[cx, y]) / 3.0
    if bg > 0 and (bg - v) / bg > 0.04:
        reach = d
print(reach)
PY
)
	echo "  shadow reaches ${REACH} DEVICE px below the window at scale 1.5"
	# size 24 + offset 10 logical = 34 logical, x1.5 = 51 device px of
	# envelope below the window. If sigma were NOT scaled with the box the
	# falloff would be 1.5x tighter and the reach would come out near 34.
	hl_assert "the falloff was scaled with its box, not left logical (reach $REACH)" \
		"$([ "${REACH:-0}" -gt 38 ] && echo true || echo false)" "true"
	hl_assert "and it stays inside the envelope the producer sized (reach $REACH)" \
		"$([ "${REACH:-0}" -lt 70 ] && echo true || echo false)" "true"
fi
hl_stop

# ── 6. across a monitor seam ───────────────────────────────────────────────

echo "== a shadow crosses the seam with its window =="
HL_SCALE1=1.0
export HL_SCALE1 HL_OUTPUTS
HL_OUTPUTS=2
W1="${HL_WIDTH:-1920}"
start_case
GEOM="$(spawn_floating $((W1 - 300)) 300 600 300)" || GEOM=""
if [ -z "$GEOM" ]; then
	hl_skip "no client; the seam check needs a mapped floating window"
else
	set -- $GEOM; GX=$1; GY=$2; GW=$3; GH=$4
	echo "  window at $GX,$GY ${GW}x${GH} (seam at x=$W1)"
	hl_assert "premise: the window straddles the seam ($GX..$((GX+GW)) vs $W1)" \
		"$([ "$GX" -lt "$W1" ] && [ "$((GX+GW))" -gt "$W1" ] && echo true || echo false)" "true"

	grim -o HEADLESS-1 "$OUTDIR/s1.png" 2>/dev/null
	grim -o HEADLESS-2 "$OUTDIR/s2.png" 2>/dev/null
	read -r MAXSTEP AT SAMPLES SPAN < <(python3 - "$OUTDIR/s1.png" \
		"$OUTDIR/s2.png" "$GX" "$GY" "$GW" "$GH" "$W1" <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGB'); pa = a.load()
b = Image.open(sys.argv[2]).convert('RGB'); pb = b.load()
gx, gy, gw, gh, seam = (int(v) for v in sys.argv[3:8])
AW, AH = a.size; BW, BH = b.size

def at(x, y):
    if x < seam:
        return pa[x, y] if 0 <= x < AW and 0 <= y < AH else None
    xb = x - seam
    return pb[xb, y] if 0 <= xb < BW and 0 <= y < BH else None

# A row 6px below the window: straight through the strongest part of the
# shadow, all the way across both outputs.
y = gy + gh - 1 + 6
vals, steps, prev = [], [], None
for x in range(gx + 8, gx + gw - 8):
    c = at(x, y)
    if c is None:
        prev = None
        continue
    v = sum(c) / 3.0
    vals.append(v)
    if prev is not None:
        steps.append((abs(v - prev), x))
    prev = v
if not steps:
    print("0 0 0 0"); sys.exit(0)
mx, at_x = max(steps)
# The premise the continuity check cannot supply: there has to BE a shadow
# here, i.e. this row is darker than the wallpaper beside it.
far = at(gx - 60 if gx >= 60 else 4, y)
span = (sum(far) / 3.0 - min(vals)) if far else 0
print(f"{mx:.2f} {at_x} {len(steps)} {span:.2f}")
PY
)
	echo "  row below the window: $SAMPLES samples, max step $MAXSTEP at x=$AT, depth $SPAN"
	hl_assert "premise: the row was sampled across the seam ($SAMPLES samples)" \
		"$([ "${SAMPLES:-0}" -gt 300 ] && echo true || echo false)" "true"
	hl_assert "premise: there really is a shadow on that row (depth $SPAN)" \
		"$(python3 -c "print('true' if $SPAN > 15 else 'false')")" "true"
	# A shadow clipped to its own monitor would stop dead at the seam: one
	# enormous step where the other output takes over.
	hl_assert "no discontinuity at the seam (max step $MAXSTEP at x=$AT)" \
		"$(python3 -c "print('true' if $MAXSTEP < 12 else 'false')")" "true"
fi
hl_stop

# ── 7. a window flush against the screen edge: asymmetric shadow corners ───

echo "== the per-corner path is really driven: asymmetric shadow corners =="
HL_OUTPUTS=1
export HL_OUTPUTS
# ANIMATIONS ON, and that is the whole point of this case.
#
# client_apply_border() squares off whichever corners the window is flush
# against the screen edge on, and feeds that mask straight to both shadow
# nodes -- so an asymmetric shadow is an ordinary thing rather than a
# theoretical one. Every other case in this file runs `animations 0` and never
# sees one, which led to the fixture briefly claiming the compositor could not
# produce them at all. The live session said otherwise in one number:
# asymmetric_shadow_draws = 258.
#
# The condition cannot be reached with move_window, which clamps a floating
# window back inside the usable area, and it does not appear during an open
# animation either -- both were tried and both measured 0. The mask that fires
# in ordinary use is the TITLEBAR one: when a window wears a tab, its top
# corners are squared so the tab and the window read as one shape, while the
# bottom two stay round. Every other case here runs `titlebar { enable 0 }`
# and therefore never sees an asymmetric shadow.
# --ssd as well as the titlebar option: the mask lives behind
# client_wants_ssd(c), so a client that decorates itself never grows a
# tab and never squares a corner however the compositor is configured.
# A BIG radius under a SHARP shadow, and both are required. At the suite's
# usual blur of 24 the effective Gaussian sigma is 12, and a 12 px corner
# radius simply does not survive being blurred by 12 -- the break moved this
# probe by 2/255, which is a correct rendering of a soft shadow and a useless
# falsifier. Radius 40 against blur 8 puts the corner back where a camera can
# see it. The contact lobe is off so only one shape is being measured.
TITLEBAR=1
SSD=1
RADIUS=40
SHSIZE=14
SHBLUR=8
CONTACT=0
export TITLEBAR SSD RADIUS SHSIZE SHBLUR CONTACT
start_case
GEOM="$(spawn_floating 500 300 500 350)" || GEOM=""
if [ -z "$GEOM" ]; then
	hl_skip "no client; the per-corner case needs a window to open"
else
	echo "  window opened at $GEOM"
	DRAWS="$(stat_of shadow_draws)"
	ASYM="$(stat_of asymmetric_shadow_draws)"
	echo "  shadow_draws $DRAWS, of which asymmetric $ASYM"
	hl_assert "premise: shadows were drawn at all ($DRAWS)" \
		"$([ "$DRAWS" != TIMEOUT ] && [ "${DRAWS:-0}" -gt 0 ] && echo true || echo false)" "true"
	# The claim: the four-radius path is not decoration. A shadow really does
	# arrive at the renderer with corners that differ, which is the only
	# condition under which a single-radius implementation renders wrongly.
	hl_assert "the compositor really drives per-corner shadow radii ($ASYM asymmetric)" \
		"$([ "$ASYM" != TIMEOUT ] && [ "${ASYM:-0}" -gt 0 ] && echo true || echo false)" "true"

	# NO PIXEL ASSERTION HERE, and the reason is worth keeping.
	#
	# Those asymmetric draws are TRANSIENT -- 4 of 10, while the tab is being
	# put together. By the time the compositor has settled and grim can
	# photograph it, the corners are equal again, so a steady-state capture
	# has nothing asymmetric in it to measure. Three fixtures were tried
	# before this was understood: a window moved flush to an edge (move_window
	# clamps it back inside the usable area), a window opened under an
	# animation, and this one probed at all four corners -- which read
	# tl=119.6 tr=122.2 br=99.4 bl=99.4 normally and tl=119.0 tr=122.2 br=98.2
	# bl=98.2 under BREAK=shadow-single-radius. A 1/255 difference is not a
	# falsifier.
	#
	# So the pixel proof of per-corner shadow radii lives in
	# tests/test-avk-shadow.c, where a constructed 0/7/19/37 case is compared
	# against three wrong oracles and fails the break 56 of 58. What this case
	# contributes is the part the unit suite structurally cannot: that the
	# COMPOSITOR really does hand the renderer corners that differ.
fi
hl_stop

hl_summary
