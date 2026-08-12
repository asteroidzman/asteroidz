#!/usr/bin/env bash
# avk-blur-seam-test.sh — a blur whose source crosses an output boundary.
#
# Every other effect can be rendered per output after intersecting with that
# output. Blur cannot. A blur pixel presented ten pixels inside output A's right
# edge samples source up to one filter SUPPORT further right — scene content
# that belongs to output B. Clip the source to A and that content is replaced by
# the capture's edge-clamped colour, and every window spanning the join grows a
# visible seam down it.
#
# So AVK separates two things a single-output renderer never has to:
#
#     presentation bounds   pixels this output can put on a screen
#     source bounds         scene it can RECONSTRUCT, = presentation + halo
#
# and reconstructs the halo from the GLOBAL scene into THIS output's device
# grid. Output A never samples output B's framebuffer — that would couple
# format, scale, presentation history and (in M5) the colour domain.
#
# WHAT IS ASSERTED HERE
#
#   continuity   a logical line crossing the seam through a blurred window has
#                no step in it. This is the falsifier for the break: damage_all
#                cannot catch a source-clipping bug, because a partial frame and
#                a full one would both use the clipped source and agree.
#
#   retention    commands outside an output's bounds are KEPT when they are
#                inside its halo, and still culled when they are not.
#
#   routing      damage on B, inside A's halo, reaches A. Damage on B outside
#                A's halo does not.
#
#   the oracle   damage_all on each output leaves 0 stale pixels.
#
# Break tests, which MUST fail:
#
#   BREAK=source-clip   AZ_BLUR_SOURCE_OUTPUT_CLIP=1 -- the source is clamped
#                       to the presenting output. The seam step must exceed the
#                       tolerance the good build meets.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-seam"
BREAK="${BREAK:-}"

# EQUAL SCALES FOR THE CONTINUITY METRIC, and that is a deliberate limitation
# rather than an oversight. Comparing "the last device column of A" with "the
# first device column of B" is only a comparison of adjacent LOGICAL positions
# when both outputs have the same density; at 1.5 against 1.0 the two columns
# are 2/3 of a logical pixel apart horizontally and sample different logical
# rows vertically, and any threshold would be measuring the resampling. Mixed
# scale is exercised in phase 4 against the damage_all oracle and the counters,
# which do not need corresponding pixels.
HL_WIDTH=800 HL_HEIGHT=600
HL_OUTPUTS=2
HL_SCALE1="${SEAM_SCALE1:-1}"
HL_SCALE2="${SEAM_SCALE2:-1}"
HL_RR1="${SEAM_RR1:-0}"
HL_RR2="${SEAM_RR2:-0}"
# The second output starts where the first one ENDS, logically. With scale 1
# that is HL_WIDTH; the default would leave a gap at any other scale.
HL_X2="${SEAM_X2:-800}"
HL_ENV="ASTEROIDZ_RENDERER=avk ASTEROIDZ_VK_DEBUG=1"
case "$BREAK" in
source-clip) HL_ENV="$HL_ENV AZ_BLUR_SOURCE_OUTPUT_CLIP=1" ;;
poison)      HL_ENV="$HL_ENV AZ_TRANSIENT_POISON=1" ;;
esac
HL_KITTY_EXTRA="-o cursor_blink_interval=0 -o cursor_stop_blinking_after=0"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-blur-seam-$$"
HL_OUTDIR="$OUTDIR"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV HL_OUTPUTS HL_SCALE1 HL_SCALE2 \
	HL_X2 HL_KITTY_EXTRA HL_RR1 HL_RR2
echo "  note: scales $HL_SCALE1/$HL_SCALE2, transforms $HL_RR1/$HL_RR2, seam at x=$HL_X2"

# A blur with a WIDE kernel, so its halo is tens of pixels and the seam it would
# leave is impossible to miss. passes 3 / radius 4 gives a support around 110
# device pixels at scale 1.
BLUR_KDL="border_radius 12
effects {
	shadow {
		enable 1
		only-floating 0
		size 24
		blur 24
		blur-background 1
		blur-background-darken 1
		blur-background-strength 1.0
	}
	blur { enable 1; passes 3; radius 4 }
}
focused_opacity 0.9
unfocused_opacity 0.9"

# hl_start WIPES the state directory, so this path only ever holds the CURRENT
# instance's output and the validation count at the end would cover the last
# phase alone. Every phase that ends copies it out; the count sums the copies.
LOG="$OUTDIR/state/asteroidz/asteroidz.log"
keep_log() { cp -f "$LOG" "$OUTDIR/log-$1.txt" 2>/dev/null || true; }

field() { python3 - "$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}

# ── PNG helpers ────────────────────────────────────────────────────────────
#
# One decoder, two questions. `col` prints a single column's luminance, one
# value per line; `diff` prints "differing worst" for two whole images.
PNGPY="$OUTDIR/png.py"

mkdir -p "$OUTDIR"
cat > "$PNGPY" <<'PY'
import sys, zlib, struct

def read(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w, h, depth, ctype = 8, b'', 0, 0, 0, 0
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos+4])[0]
        typ = data[pos+4:pos+8]
        body = data[pos+8:pos+8+ln]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'IDAT':
            idat += body
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    assert depth == 8, depth
    stride = w * ch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i-ch]) & 0xff
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xff
        elif f == 3:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xff
        elif f == 4:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                b = prev[i]
                c = prev[i-ch] if i >= ch else 0
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xff
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, ch, bytes(out)

mode = sys.argv[1]
if mode == 'diff':
    try:
        w1, h1, c1, a = read(sys.argv[2])
        w2, h2, c2, b = read(sys.argv[3])
    except Exception:
        print("-1 -1"); sys.exit(0)
    if (w1, h1, c1) != (w2, h2, c2):
        print("-1 -1"); sys.exit(0)
    differ = worst = 0
    for i in range(0, len(a), c1):
        if a[i:i+3] != b[i:i+3]:
            differ += 1
            for k in range(3):
                d = abs(a[i+k] - b[i+k])
                if d > worst:
                    worst = d
    print(differ, worst)
elif mode == 'bbox':
    w1, h1, c1, a = read(sys.argv[2])
    w2, h2, c2, b = read(sys.argv[3])
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for y in range(h1):
        row = y * w1 * c1
        for x in range(w1):
            i = row + x * c1
            if a[i:i+3] != b[i:i+3]:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    print(x0, y0, x1, y1)
elif mode == 'col':
    # luminance of one column, one value per line
    w, h, c, px = read(sys.argv[2])
    x = int(sys.argv[3])
    if x < 0:
        x += w
    for y in range(h):
        i = (y * w + x) * c
        print((px[i] + px[i+1] + px[i+2]) // 3)
PY

png_diff() { python3 "$PNGPY" diff "$1" "$2"; }

# The STEP across the seam: |A's last column - B's first column|, averaged over
# the rows where the blurred window actually is. Reported as an integer in 8-bit
# codes so a threshold can be read as "how visible".
seam_step() { python3 - "$PNGPY" "$1" "$2" "$3" "$4" <<'PY'
import subprocess, sys
pngpy, a, b, y0, y1 = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
def col(path, x):
    out = subprocess.run([sys.executable, pngpy, 'col', path, str(x)],
                         capture_output=True, text=True).stdout.split()
    return [int(v) for v in out]
try:
    left = col(a, -1)     # A's rightmost column
    right = col(b, 0)     # B's leftmost column
except Exception:
    print(-1); sys.exit(0)
n = min(len(left), len(right), y1) - y0
if n <= 0:
    print(-1); sys.exit(0)
total = sum(abs(left[y0+i] - right[y0+i]) for i in range(n))
print(total // n)
PY
}

# ── 1. a blurred window straddling the seam ────────────────────────────────

hl_start "$BLUR_KDL"
hl_reset_spawn_colors
# Content on BOTH sides, so the blur has real scene to sample either way and a
# clipped source has something to be wrong about.
hl_spawn_kitty left >/dev/null;  hl_wait_client_count 1 60
hl_spawn_kitty right >/dev/null; hl_wait_client_count 2 60
sleep 3

# The blur-bearing window: a real toplevel with a real ext-background-effect
# region, so this exercises the producer M4F.2B corrected rather than only the
# compositor's own shadow-backdrop blur.
hl_spawn_wlbgeffect spanner 300 >/dev/null; hl_wait_client_count 3 60
sleep 2
hl_dispatch toggle_floating
sleep 1
# 400 px wide, placed so it crosses the seam at logical x=800 with roughly half
# on each side. `move_window`, not `move`: there is no dispatch called `move`
# and the IPC layer swallows an unknown name in silence.
# CENTRED ON THE SEAM, WHEREVER IT IS. The window is 400 logical pixels wide,
# so this puts 200 on each side. Hardcoding 600 worked only while the seam was
# at 800: rotating an output changes its LOGICAL width, and at 270 degrees the
# window landed entirely on B, the fixture stopped spanning anything, and the
# cross-output routing assertion correctly reported 0 for a scene that had no
# cross-output dependency in it.
# SEAM_PLACE=far puts the window entirely inside output A instead of across the
# join. It exists to classify the 180-degree stale strip: if that strip survives
# with no cross-output dependency in the scene, the halo path is innocent.
if [ "${SEAM_PLACE:-seam}" = "far" ]; then
	hl_dispatch "move_window,40,150"
else
	hl_dispatch "move_window,$(( HL_X2 - 200 )),150"
fi
sleep 3

hl_dispatch reset_avk_stats
sleep 1
hl_dispatch damage_all
sleep 2
hl_get "get avk-stats" > "$OUTDIR/stats-seam.json"

HALO="$(field "$OUTDIR/stats-seam.json" blur_halo_px)"
RETAIN="$(field "$OUTDIR/stats-seam.json" nodes_retained_for_halo)"
HALOPX="$(field "$OUTDIR/stats-seam.json" blur_halo_pixels)"
EMIT="$(field "$OUTDIR/stats-seam.json" blur_nodes_emitted)"
echo "  note: halo ${HALO}px, $RETAIN commands retained for it, $HALOPX halo source px, $EMIT blur nodes"

hl_assert "the scene's kernel produces a halo (premise, ${HALO}px)" \
	"$([ "${HALO:-0}" -gt 20 ] && echo true || echo false)" "true"
hl_assert "blur nodes reached the walker (premise, $EMIT)" \
	"$([ "${EMIT:-0}" -gt 0 ] && echo true || echo false)" "true"
# The halo is only worth having if something is actually kept by it. 0 here
# would mean the seam-spanning fixture is not spanning anything.
hl_assert "commands outside an output are retained for its halo ($RETAIN)" \
	"$([ "${RETAIN:-0}" -gt 0 ] && echo true || echo false)" "true"

# ── 2. the seam has no step in it ──────────────────────────────────────────

hl_screenshot_output HEADLESS-1 seam-a >/dev/null 2>&1 || true
hl_screenshot_output HEADLESS-2 seam-b >/dev/null 2>&1 || true
STEP=-1
if [ -s "$OUTDIR/seam-a.png" ] && [ -s "$OUTDIR/seam-b.png" ]; then
	# Rows 150..450 -- where the 400x300 window is. Outside it the two columns
	# show different windows and a step there means nothing.
	STEP="$(seam_step "$OUTDIR/seam-a.png" "$OUTDIR/seam-b.png" 150 450)"
fi
echo "  note: mean seam step through the blurred window: $STEP codes"
# THE CONTINUITY METRIC IS ONLY MEANINGFUL AT EQUAL SCALES AND NO TRANSFORM.
#
# It compares A's last device column with B's first, which are adjacent LOGICAL
# positions only while both outputs have the same density and orientation. At
# 1.5 against 1.0 the two columns are 2/3 of a logical pixel apart horizontally
# and sample different logical rows vertically; under a 90-degree transform the
# "last column" is not even the edge nearest the seam. Any threshold there would
# be measuring the resampling rather than the blur, so those runs skip it and
# rely on the damage_all oracle and the counters, which need no correspondence
# between the two images.
if [ "${SEAM_MODE:-equal}" = "equal" ]; then
hl_assert "the seam could be measured (premise)" \
	"$([ "${STEP:-(-1)}" -ge 0 ] && echo true || echo false)" "true"
# 4 codes, and the threshold is set BY THE TWO MEASUREMENTS rather than chosen:
#
#     correct source     0 codes
#     BREAK=source-clip  14 codes
#
# The two columns are adjacent logical positions over continuous content, so a
# correct blur leaves only what two neighbouring pixels of a blurred picture
# differ by -- which on this fixture is nothing at all. Clipping the source at
# the output replaces one side's neighbourhood with the capture's edge-clamped
# colour, and the join stops being continuous.
hl_assert "and no step across it ($STEP codes)" \
	"$([ "${STEP:-99}" -le 4 ] && echo true || echo false)" "true"

# AND THE SOURCE REALLY WAS RECONSTRUCTED OUTSIDE THE OUTPUT. A second
# falsifier for the same break that does not depend on a threshold at all:
# blur_halo_pixels is the source area reconstructed beyond this output's own
# bounds, and the break makes it exactly 0.
hl_assert "source is reconstructed beyond the output's bounds ($HALOPX px)" \
	"$([ "${HALOPX:-0}" -gt 0 ] && echo true || echo false)" "true"
else
	echo "  note: SEAM_MODE=${SEAM_MODE} -- continuity metric skipped (see above)"
	hl_assert "source is reconstructed beyond the output's bounds ($HALOPX px)" \
		"$([ "${HALOPX:-0}" -gt 0 ] && echo true || echo false)" "true"
fi

# ── 3. the damage oracle, on each output ───────────────────────────────────

hl_screenshot_output HEADLESS-1 pre-a >/dev/null 2>&1 || true
hl_screenshot_output HEADLESS-2 pre-b >/dev/null 2>&1 || true
sleep 2
hl_screenshot_output HEADLESS-1 ctl-a >/dev/null 2>&1 || true
hl_screenshot_output HEADLESS-2 ctl-b >/dev/null 2>&1 || true
# CAN THE OUTPUTS BE CAPTURED AT ALL?
#
# grim produces NOTHING for a 90- or 270-degree rotated output on this backend:
# a transform=1 run left no HEADLESS-1 png on disk while HEADLESS-2's captures
# were all present. That is a screenshot limitation and not a compositor one, so
# the pixel comparisons below are SKIPPED with a reason rather than reported as
# failures -- and every counter-based assertion still runs, including the
# cross-output damage routing and validation. 180 degrees captures fine and is
# the transform case that gets the full pixel treatment.
PIXELS=1
for f in pre-a ctl-a pre-b ctl-b; do
	[ -s "$OUTDIR/$f.png" ] || PIXELS=0
done
if [ "$PIXELS" = "0" ]; then
	echo "  SKIP: grim captured no image for a rotated output" \
		"(HL_RR1=$HL_RR1) -- pixel comparisons skipped, counters still asserted"
fi

if [ "$PIXELS" = "1" ]; then
read -r CA _ <<<"$(png_diff "$OUTDIR/pre-a.png" "$OUTDIR/ctl-a.png")"
read -r CB _ <<<"$(png_diff "$OUTDIR/pre-b.png" "$OUTDIR/ctl-b.png")"
hl_assert "PREMISE: both outputs are static ($CA / $CB px)" \
	"$([ "${CA:-1}" = "0" ] && [ "${CB:-1}" = "0" ] && echo true || echo false)" \
	"true"
fi

hl_dispatch damage_all
sleep 2
hl_screenshot_output HEADLESS-1 post-a >/dev/null 2>&1 || true
hl_screenshot_output HEADLESS-2 post-b >/dev/null 2>&1 || true
if [ "$PIXELS" = "1" ]; then
read -r DA WA <<<"$(png_diff "$OUTDIR/ctl-a.png" "$OUTDIR/post-a.png")"
read -r DB WB <<<"$(png_diff "$OUTDIR/ctl-b.png" "$OUTDIR/post-b.png")"
echo "  note: forced full repaint changes A $DA px (worst $WA), B $DB px (worst $WB)"
hl_assert "output A: a full repaint changes nothing ($DA px)" \
	"$([ "${DA:-1}" = "0" ] && echo true || echo false)" "true"
hl_assert "output B: a full repaint changes nothing ($DB px)" \
	"$([ "${DB:-1}" = "0" ] && echo true || echo false)" "true"
fi

# ── 4. cross-output damage routing ─────────────────────────────────────────
#
# A change on B, inside A's halo, must reach A. The border of the window that
# straddles the seam is the smallest deterministic change available, and part of
# it lies on B within one support of the join.

# A NEW TILED WINDOW ON OUTPUT B, whose left edge IS the seam.
#
# `toggle_render_border` was tried first and routed nothing: it acts on the
# focused window, which the layout had put entirely on A, so no damage landed
# within A's halo and the phase was measuring an unchanged neighbour. A window
# appearing on B damages B from its very first column, which is one pixel from
# the join and therefore unambiguously inside A's halo.
hl_dispatch reset_avk_stats
sleep 1
hl_dispatch "focus_monitor,HEADLESS-2" 2>/dev/null || \
	hl_dispatch "focus_monitor HEADLESS-2" 2>/dev/null || true
sleep 1
hl_spawn_kitty onb >/dev/null; hl_wait_client_count 4 60
sleep 3
hl_get "get avk-stats" > "$OUTDIR/stats-cross.json"
HALOFR="$(field "$OUTDIR/stats-cross.json" blur_halo_damage_frames)"
INHERIT="$(field "$OUTDIR/stats-cross.json" blur_transitive_damage_pixels)"
TOUCH="$(field "$OUTDIR/stats-cross.json" blur_damage_nodes_touched)"
RECS="$(field "$OUTDIR/stats-cross.json" blur_halo_damage_records)"
echo "  note: $HALOFR frames took damage from outside their own output ($RECS recorded); $TOUCH blur recomputes"
hl_assert "a near-seam change routes damage across the boundary ($HALOFR)" \
	"$([ "${HALOFR:-0}" -gt 0 ] && echo true || echo false)" "true"

hl_screenshot_output HEADLESS-1 cross-a >/dev/null 2>&1 || true
hl_screenshot_output HEADLESS-2 cross-b >/dev/null 2>&1 || true
sleep 1
hl_dispatch damage_all
sleep 2
hl_screenshot_output HEADLESS-1 crossfull-a >/dev/null 2>&1 || true
hl_screenshot_output HEADLESS-2 crossfull-b >/dev/null 2>&1 || true
if [ "$PIXELS" = "1" ]; then
read -r XA _ <<<"$(png_diff "$OUTDIR/cross-a.png" "$OUTDIR/crossfull-a.png")"
read -r XB _ <<<"$(png_diff "$OUTDIR/cross-b.png" "$OUTDIR/crossfull-b.png")"
echo "  note: after a cross-seam change, full repaint differs A $XA px, B $XB px"
if [ "${XA:-0}" != "0" ]; then
	echo "  note: A's stale bbox (x0 y0 x1 y1): $(python3 "$PNGPY" bbox "$OUTDIR/cross-a.png" "$OUTDIR/crossfull-a.png")"
fi
hl_assert "no stale blur on A after a change routed from B ($XA px)" \
	"$([ "${XA:-1}" = "0" ] && echo true || echo false)" "true"
hl_assert "and none on B ($XB px)" \
	"$([ "${XB:-1}" = "0" ] && echo true || echo false)" "true"
fi

# ── 5. the direct path is untouched ────────────────────────────────────────

keep_log seam
hl_stop
HL_ENV="ASTEROIDZ_RENDERER=avk ASTEROIDZ_VK_DEBUG=1"
export HL_ENV
hl_start "effects { shadow { blur-background 0 }; blur { enable 0 } }"
hl_spawn_kitty plain >/dev/null; hl_wait_client_count 1 60
sleep 3
hl_dispatch reset_avk_stats
sleep 1
hl_dispatch damage_all
sleep 2
hl_get "get avk-stats" > "$OUTDIR/stats-direct.json"
D_HALO="$(field "$OUTDIR/stats-direct.json" blur_halo_px)"
D_RETAIN="$(field "$OUTDIR/stats-direct.json" nodes_retained_for_halo)"
D_HALOFR="$(field "$OUTDIR/stats-direct.json" blur_halo_damage_frames)"
D_REPLAY="$(field "$OUTDIR/stats-direct.json" blur_prefix_replays)"
echo "  note: direct path -- halo=$D_HALO retained=$D_RETAIN halo-frames=$D_HALOFR replays=$D_REPLAY"
hl_assert "with no blur there is no halo" "${D_HALO:-x}" "0"
hl_assert "no command is retained for one" "${D_RETAIN:-x}" "0"
hl_assert "no frame takes damage from another output" "${D_HALOFR:-x}" "0"
hl_assert "and no prefix capture runs" "${D_REPLAY:-x}" "0"

WAITS="$(field "$OUTDIR/stats-direct.json" cpu_sync_waits)"
hl_assert "no CPU wait anywhere ($WAITS)" "${WAITS:-x}" "0"

keep_log direct
VUID=0; HAZ=0
for f in "$OUTDIR"/log-*.txt; do
	[ -s "$f" ] || continue
	VUID=$(( VUID + $(grep -ac "VUID" "$f" || true) ))
	HAZ=$(( HAZ + $(grep -ac "SYNC-HAZARD" "$f" || true) ))
done
echo "  note: $VUID VUID lines, $HAZ SYNC-HAZARD lines"
hl_assert "no validation errors" "${VUID:-x}" "0"
hl_assert "no synchronisation hazards" "${HAZ:-x}" "0"

hl_stop
echo
echo "logs: $OUTDIR"
hl_summary
