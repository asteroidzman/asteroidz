#!/usr/bin/env bash
# avk-oracle-runs.sh — the 180-degree cross-seam defect, N times, under the
# first-divergence oracle.
#
# M4F.2C.4c. The defect fails about two runs in three, so three consecutive
# runs cannot tell determinism from luck -- that mistake was already made once,
# and the "persistent" reading it produced had to be retracted. This runs the
# minimal trigger repeatedly and records, per run:
#
#     stale       what a forced full repaint changes AFTER the interaction
#     first       the first frame where the production render stopped equalling
#                 a full render of the same snapshot
#     class       PREFIX / BLUR / OUTPUT, from the per-blur boundary taps
#     buf         which scan-out buffer that frame drew into, and how many the
#                 output has used -- buffer identity IS damage-history identity,
#                 because wlr_damage_ring keys on the wlr_buffer
#     halo_rec    the output's running count of damage recorded from outside its
#                 own bounds, so "the change never routed here" is separable
#                 from "it routed here and was lost afterwards"
#
# THE MINIMAL TRIGGER, and it is smaller than the seam test's:
#
#     two adjacent outputs, the first rotated 180
#     a blurred window straddling the seam
#     a new window appearing on the SECOND output, at the join
#
# The last step is the one that matters. It changes scene content that lies
# inside output A's blur halo and nowhere inside output A.
#
# RUNS=n            how many (default 6; the brief's characterisation wants 30)
# ORACLE_RR1=t      output A's transform (default 2 = 180)
# ORACLE_PLACE=far  put the blurred window entirely inside A, so the scene has
#                   no cross-output dependency at all. The control.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-oracle-runs"
RUNS="${RUNS:-6}"

HL_WIDTH=800 HL_HEIGHT=600
HL_OUTPUTS=2
HL_RR1="${ORACLE_RR1:-2}"
HL_RR2="${ORACLE_RR2:-0}"
HL_X2=800
HL_KITTY_EXTRA="-o cursor_blink_interval=0 -o cursor_stop_blinking_after=0"
HL_ENV="ASTEROIDZ_RENDERER=avk AZ_FRAME_ORACLE=1 ${ORACLE_EXTRA_ENV:-}"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-oracle-runs-$$"
HL_OUTDIR="$OUTDIR"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV HL_OUTPUTS HL_RR1 HL_RR2 HL_X2 \
	HL_KITTY_EXTRA

BLUR_KDL="border_radius 12
effects {
	shadow { enable 1; only-floating 0; size 24; blur 24; blur-background 1 }
	blur { enable 1; passes 3; radius 4 }
}
focused_opacity 0.9
unfocused_opacity 0.9"

LOG="$OUTDIR/state/asteroidz/asteroidz.log"
PNGPY="$OUTDIR/png.py"

# The seam test's PNG reader, verbatim -- a second decoder written from
# scratch produced an IndexError on the first row and a -1 that read
# exactly like 'the outputs are different sizes'.
png_setup() { cat > "$PNGPY" <<'PY'
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
}

# One run. Prints a single row.
one_run() {
	local n="$1"
	hl_start "$BLUR_KDL"
	hl_reset_spawn_colors
	hl_spawn_kitty left >/dev/null;  hl_wait_client_count 1 60
	hl_spawn_kitty right >/dev/null; hl_wait_client_count 2 60
	sleep 3
	hl_spawn_wlbgeffect spanner 300 >/dev/null; hl_wait_client_count 3 60
	sleep 2
	hl_dispatch toggle_floating
	sleep 1
	if [ "${ORACLE_PLACE:-seam}" = "far" ]; then
		hl_dispatch "move_window,40,150"
	else
		hl_dispatch "move_window,$(( HL_X2 - 200 )),150"
	fi
	sleep 3

	# THE TRIGGER: a window appearing on B, its left edge at the join.
	hl_dispatch "focus_monitor,HEADLESS-2" 2>/dev/null || true
	sleep 1
	hl_spawn_kitty onb >/dev/null; hl_wait_client_count 4 60
	sleep 3

	# The static control, then the postmortem. Without the control a moving
	# desktop reads exactly like a stale one.
	hl_screenshot_output HEADLESS-1 "ctl1-$n" >/dev/null 2>&1 || true
	sleep 2
	hl_screenshot_output HEADLESS-1 "ctl2-$n" >/dev/null 2>&1 || true
	hl_dispatch damage_all
	sleep 2
	hl_screenshot_output HEADLESS-1 "full-$n" >/dev/null 2>&1 || true

	local ctl stale
	read -r ctl _ <<<"$(python3 "$PNGPY" diff "$OUTDIR/ctl1-$n.png" \
		"$OUTDIR/ctl2-$n.png" 2>/dev/null || echo "-1 -1")"
	read -r stale _ <<<"$(python3 "$PNGPY" diff "$OUTDIR/ctl2-$n.png" \
		"$OUTDIR/full-$n.png" 2>/dev/null || echo "-1 -1")"

	cp -f "$LOG" "$OUTDIR/run-$n.log" 2>/dev/null || true
	hl_stop

	local first buf halo cls
	first=$(grep -m1 "FIRST DIVERGENCE" "$OUTDIR/run-$n.log" 2>/dev/null || true)
	if [ -n "$first" ]; then
		buf=$(echo "$first" | grep -o "buf=[0-9]*/[0-9]*" | cut -d= -f2)
		halo=$(echo "$first" | grep -o "halo_rec=[0-9]*" | cut -d= -f2)
		# The classification comes from the blur boundary lines that follow the
		# first divergence: a prefix that already differs is a source
		# reconstruction defect, a prefix that matches is not.
		if grep -A 20 "FIRST DIVERGENCE" "$OUTDIR/run-$n.log" |
				grep -q "PREFIX wrong=[1-9]"; then
			cls=PREFIX
		elif grep -A 20 "FIRST DIVERGENCE" "$OUTDIR/run-$n.log" |
				grep -q "BLUR wrong=[1-9]"; then
			cls=BLUR
		else
			cls=OUTPUT
		fi
		printf "%-4s %-7s %-7s %-9s %-6s %-9s %s\n" "$n" "$stale" "$ctl" \
			"$(echo "$first" | grep -o 'frame=[0-9]*' | cut -d= -f2)" \
			"$buf" "$halo" "$cls"
	else
		printf "%-4s %-7s %-7s %-9s %-6s %-9s %s\n" "$n" "$stale" "$ctl" \
			"-" "-" "-" "none"
	fi
}

mkdir -p "$OUTDIR"
png_setup
echo "  transform $HL_RR1/$HL_RR2, seam at x=$HL_X2, place=${ORACLE_PLACE:-seam}"
echo
printf "%-4s %-7s %-7s %-9s %-6s %-9s %s\n" \
	run stale ctl first buf halo_rec class
i=1
while [ "$i" -le "$RUNS" ]; do
	one_run "$i"
	i=$(( i + 1 ))
done
echo
echo "logs: $OUTDIR"
