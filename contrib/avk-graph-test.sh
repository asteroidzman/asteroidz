#!/usr/bin/env bash
# avk-graph-test.sh — M4E.1: the render graph, in a running compositor.
#
# WHAT THIS ADDS OVER tests/test-avk-graph.c. The unit suite builds graphs by
# hand and asserts on the barriers they emit. It cannot say whether the frame
# the COMPOSITOR builds is still one pass, whether the desktop still looks
# identical, or whether graph construction allocates on a scene the unit test
# never constructs. Those need a real compositor and, for the first two, they
# need the PREVIOUS BINARY to compare against.
#
# THE COMPARISON THAT MATTERS. "The pixels are unchanged" is not provable by a
# test that only ever ran against the new code -- it would pass equally well if
# the graph had broken every window in the same way the oracle expected. So
# this script renders the same fixture twice, once with a binary built from the
# last pre-graph commit and once with the current build, and compares the two
# framebuffers BYTE FOR BYTE. Anything less is an assertion about a
# reimplementation of the renderer rather than about the renderer.
#
#   ASTEROIDZ_PREGRAPH=<path>   the pre-M4E binary. Without it the pixel and
#                               cost comparisons SKIP rather than pass, because
#                               a comparison with nothing to compare against is
#                               not a result. Build one with:
#                                   git worktree add /tmp/pregraph b9d7115
#                                   meson setup /tmp/pregraph/build /tmp/pregraph
#                                   ninja -C /tmp/pregraph/build asteroidz
#
#   BREAK=graph-missing-write-read   drops the write->read dependency between
#                               passes. Has NO effect on the direct path, which
#                               has one pass and therefore no inter-pass edge --
#                               so it is deliberately NOT listed as a falsifier
#                               for this suite. Its coverage is in the unit
#                               suite and in the multipass fixture (M4E.4).
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-graph"
BREAK="${BREAK:-}"

OUTDIR="${HL_FIXED_OUTDIR:-${TMPDIR:-/tmp}/asteroidz-graph-$$}"
HL_OUTDIR="$OUTDIR"
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = graph-missing-write-read ] && HL_ENV="$HL_ENV AZ_GRAPH_NO_WRITE_READ=1"
export HL_OUTDIR HL_ENV

PREGRAPH="${ASTEROIDZ_PREGRAPH:-}"

mkdir -p "$OUTDIR"
trap 'hl_stop; [ -n "${HL_KEEP:-}" ] || rm -rf "$OUTDIR"' EXIT

echo "== avk graph (M4E.1) =="

# ── the fixture ────────────────────────────────────────────────────────────
#
# Three windows, one floated, so the frame contains a texture per client, a
# border annulus per client, a shadow, a gradient-free rect and the clear.
# That exercises every command type the direct path has -- which is what the
# graph has to declare uses for -- rather than the single rect a minimal
# fixture would produce.
#
# Deterministic on purpose: the same three windows in the same places, so the
# two binaries' framebuffers are comparable at all. A fixture with a clock, a
# cursor blink or an animation in it could not be.
fixture() {
	# Both of these make the capture reproducible, and BOTH were learned by
	# measuring the fixture against itself: the same binary, run twice,
	# differed in 3,213,397 of 6,220,800 bytes. Not one of those bytes was the
	# renderer's fault. See the note in docs/avk-effects.md.
	hl_reset_spawn_colors
	export HL_KITTY_EXTRA="-o cursor_blink_interval=0"
	hl_start
	hl_spawn_kitty graph-a >/dev/null
	hl_wait_client_count 1
	hl_spawn_kitty graph-b >/dev/null
	hl_wait_client_count 2
	hl_spawn_kitty graph-c >/dev/null
	hl_wait_client_count 3
	# Float the last one so a shadow exists: asteroidz shows shadows on
	# floating windows only, and a fixture that never floats is a fixture with
	# no shadow node in it at all.
	#
	# `toggle_floating`, not `togglefloating`. The IPC name and the C function
	# name are different, hl_dispatch reports nothing when a name is unknown,
	# and the first version of this used the C names -- so the window never
	# floated, no shadow was ever drawn, and the fixture measured a scene it did
	# not have. The shadow-draw premise below is what catches that.
	hl_dispatch "toggle_floating" 1
	hl_dispatch "move_window,exact 200 150" 1
	hl_dispatch "resize_window,exact 500 350" 1
	sleep 2
}

# ── 1. the direct path is still one pass ───────────────────────────────────
echo "-- topology --"
fixture
STATS="$(hl_get 'get avk-stats')"
PASSES=$(echo "$STATS" | jq -r '.graph_passes // "null"')
RESOURCES=$(echo "$STATS" | jq -r '.graph_resources // "null"')
BARRIERS=$(echo "$STATS" | jq -r '.graph_barriers // "null"')
USES=$(echo "$STATS" | jq -r '.graph_uses // "null"')
FRAMES=$(echo "$STATS" | jq -r '.graph_frames // 0')

# The premise. A topology assertion against a compositor that never composited
# a frame through the graph is an assertion about zero.
# Deliberately low. A settled headless desktop composites almost nothing --
# that is M4C.3H's idle-convergence invariant working -- so a threshold chosen
# to look impressive would fail on correct behaviour. What this rules out is
# zero.
hl_assert "the graph built frames at all" "$([ "${FRAMES:-0}" -gt 10 ] && echo yes || echo "no ($FRAMES)")" "yes"

# ONE pass. This is the hard requirement of M4E: a frame with no multipass
# effect must not have become a pipeline.
hl_assert "one pass in the last frame" "$PASSES" "1"
# Resources: the target plus one per distinct sampled image. More than one
# means textures are being declared, which is the point; the exact count
# depends on how many surfaces are visible, so this asserts the shape.
hl_assert "more than one resource (target + textures)" \
	"$([ "${RESOURCES:-0}" -ge 2 ] && echo yes || echo "no ($RESOURCES)")" "yes"
hl_assert "at least as many uses as resources" \
	"$([ "${USES:-0}" -ge "${RESOURCES:-999}" ] && echo yes || echo "no ($USES/$RESOURCES)")" "yes"
# TWO barrier calls on a real output: the acquire batch before the pass and the
# foreign release after it. Not three, not one per resource.
hl_assert "two barrier calls: acquire, then the foreign release" "$BARRIERS" "2"
# The fixture's own premise: a floating window with a shadow really is on
# screen. Without it every assertion above would hold just as well for a scene
# of three tiled terminals and nothing else.
SHADOWS=$(echo "$STATS" | jq -r '.shadow_draws // 0')
hl_assert "the fixture really has a shadow in it" \
	"$([ "${SHADOWS:-0}" -gt 0 ] && echo yes || echo "no ($SHADOWS)")" "yes"

echo "-- allocation --"
A1=$(echo "$STATS" | jq -r '.graph_allocs // 0')
# Force real frames. A settled headless desktop composites nothing, so the
# first version of this read the counter twice across an idle gap and asserted
# that an unchanging number had not changed -- 0 further frames, a vacuous
# pass. Moving the floating window damages the screen every time.
for i in 1 2 3 4 5 6; do
	hl_dispatch "move_window,exact $((200 + i * 20)) $((150 + i * 10))" 1
done
sleep 2
STATS2="$(hl_get 'get avk-stats')"
A2=$(echo "$STATS2" | jq -r '.graph_allocs // 0')
F2=$(echo "$STATS2" | jq -r '.graph_frames // 0')
BUILD=$(echo "$STATS2" | jq -r '.graph_build_ns_avg // 0')
echo "  ---- graph_allocs $A1 -> $A2 over $((F2 - FRAMES)) further frames"
echo "  ---- graph_build_ns_avg $BUILD"
# The premise. Asserting that a counter did not move across an interval in
# which nothing happened is not a test of anything.
hl_assert "frames were actually built between the two readings" \
	"$([ "$((F2 - FRAMES))" -gt 5 ] && echo yes || echo "no ($((F2 - FRAMES)))")" "yes"
hl_assert "graph construction stops allocating once the scene settles" \
	"$([ "$A1" = "$A2" ] && echo yes || echo "no ($A1 -> $A2)")" "yes"
# A ceiling, generous, on a machine running a compositor and three terminals.
hl_assert "graph build is under 50us a frame" \
	"$(awk -v b="$BUILD" 'BEGIN{print (b < 50000) ? "yes" : "no"}')" "yes"

echo "-- invariants --"
for k in cpu_sync_waits present_sync_failures target_state_violations \
		lifecycle_violations fallback_frames dmabuf_import_failures; do
	V=$(echo "$STATS2" | jq -r ".$k // 0")
	hl_assert "$k is zero" "$V" "0"
done

grim -o "$HL_MON" "$OUTDIR/new.png" 2>/dev/null
NEW_GPU=$(echo "$STATS2" | jq -r '.gpu_frame_us_avg // 0')
NEW_CPU=$(echo "$STATS2" | jq -r '.cpu_frame_us_p50 // 0')
NEW_P95=$(echo "$STATS2" | jq -r '.cpu_frame_us_p95 // 0')
# record_us_avg is M4E's own field and the pre-graph binary does not expose it,
# so it is reported for this build and compared on nothing.
NEW_REC=$(echo "$STATS2" | jq -r '.record_us_avg // "n/a"')
hl_stop

# ── 2. pixel equivalence, against a measured noise floor ───────────────────
#
# THE STRUCTURE, AND WHY IT IS NOT SIMPLY old-vs-new.
#
# The first version of this compared one capture from each binary and asserted
# byte equality. It failed by 3,214,556 of 6,220,800 bytes -- and the same
# binary compared against ITSELF failed by 3,213,397. The renderer was not
# involved in any of it. Two causes, both in the harness:
#
#   - hl_spawn_kitty walks a colour palette and the index is process-global, so
#     the second fixture in one script got colours 4,5,6 where the first got
#     1,2,3. Every window background differed;
#   - a terminal's text cursor blinks, so a "settled" desktop is not a still
#     image at all.
#
# Fixing both took it to ~1000 scattered pixels, which still varies run to run.
# So this measures that floor IN THE SAME RUN, by capturing the current binary
# twice, and then asks whether the pre-graph binary differs by MORE than the
# current one differs from itself. A constant tolerance written into the script
# would have been a number chosen to make the test pass.
echo "-- pixel equivalence --"

pngdiff() { # pngdiff A B -> "differing_bytes total worst"
	python3 - "$1" "$2" <<'PY'
import sys, zlib, struct

def read_png(path):
	data = open(path, 'rb').read()
	pos, idat, w, h, bpp = 8, b'', 0, 0, 4
	while pos < len(data):
		ln = struct.unpack('>I', data[pos:pos+4])[0]
		typ = data[pos+4:pos+8]
		if typ == b'IHDR':
			w, h, depth, ctype = struct.unpack('>IIBB', data[pos+8:pos+18])
			bpp = {0:1, 2:3, 4:2, 6:4}[ctype]
		elif typ == b'IDAT':
			idat += data[pos+8:pos+8+ln]
		pos += 12 + ln
	raw = zlib.decompress(idat)
	stride = w * bpp
	out = bytearray(w * h * bpp)
	prev = bytearray(stride)
	p = 0
	for y in range(h):
		f = raw[p]; p += 1
		line = bytearray(raw[p:p+stride]); p += stride
		if f == 1:
			for i in range(bpp, stride): line[i] = (line[i] + line[i-bpp]) & 255
		elif f == 2:
			for i in range(stride): line[i] = (line[i] + prev[i]) & 255
		elif f == 3:
			for i in range(stride):
				a = line[i-bpp] if i >= bpp else 0
				line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
		elif f == 4:
			for i in range(stride):
				a = line[i-bpp] if i >= bpp else 0
				c = prev[i-bpp] if i >= bpp else 0
				b = prev[i]
				pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
				pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
				line[i] = (line[i] + pr) & 255
		out[y*stride:(y+1)*stride] = line
		prev = line
	return w, h, bpp, bytes(out)

w1, h1, b1, a = read_png(sys.argv[1])
w2, h2, b2, b = read_png(sys.argv[2])
if (w1, h1, b1) != (w2, h2, b2):
	print(-1, 0, 255); sys.exit()
diff = 0; maxd = 0
for i in range(len(a)):
	d = abs(a[i] - b[i])
	if d:
		diff += 1
		if d > maxd: maxd = d
print(diff, len(a), maxd)
PY
}

# The noise floor: this binary, the same fixture, a second time.
fixture
sleep 3
grim -o "$HL_MON" "$OUTDIR/new2.png" 2>/dev/null
hl_stop
read -r FLOOR FTOTAL FMAX < <(pngdiff "$OUTDIR/new.png" "$OUTDIR/new2.png")
echo "  ---- noise floor (this binary vs itself): $FLOOR of $FTOTAL bytes, worst $FMAX"

if [ -z "$PREGRAPH" ] || [ ! -x "$PREGRAPH" ]; then
	hl_skip "no ASTEROIDZ_PREGRAPH binary: the pixel and cost comparison need one, and a comparison against nothing is not a result"
else
	HL_ASTEROIDZ="$PREGRAPH" fixture
	# THE PREMISE. hl_start resolves its binary from $ASTEROIDZ once, at source
	# time, so `ASTEROIDZ=<old> hl_start` does nothing -- which is what the
	# first version of this did, running the current build twice and reporting
	# a byte-identical framebuffer that said nothing about the old one.
	hl_assert "the comparison really used the pre-graph binary" \
		"$(hl_binary)" "$PREGRAPH"
	sleep 3
	OLD_STATS="$(hl_get 'get avk-stats')"
	grim -o "$HL_MON" "$OUTDIR/old.png" 2>/dev/null
	OLD_GPU=$(echo "$OLD_STATS" | jq -r '.gpu_frame_us_avg // 0')
	OLD_CPU=$(echo "$OLD_STATS" | jq -r '.cpu_frame_us_p50 // 0')
	OLD_P95=$(echo "$OLD_STATS" | jq -r '.cpu_frame_us_p95 // 0')
	hl_stop

	if [ ! -s "$OUTDIR/old.png" ] || [ ! -s "$OUTDIR/new.png" ]; then
		hl_skip "one of the captures is missing"
	else
		read -r DIFF TOTAL MAXD < <(pngdiff "$OUTDIR/old.png" "$OUTDIR/new.png")
		echo "  ---- pre-graph vs graph: $DIFF of $TOTAL bytes, worst $MAXD"
		# The premise, asserted rather than assumed: a floor that had come out
		# at zero would make the comparison below meaningful in a much stronger
		# way, and a floor larger than the whole framebuffer would make it
		# meaningless. Either way the test must say which it is.
		hl_assert "the fixture is reproducible enough to compare at all" \
			"$(awk -v f="$FLOOR" -v t="$FTOTAL" 'BEGIN{
				print (f < t / 100) ? "yes" : "no (" f "/" t ")" }')" "yes"
		# THE ACTUAL QUESTION. Not "are the pixels identical" -- the fixture
		# cannot support that claim -- but "did moving barrier decisions into a
		# graph change the picture by more than running the same binary twice
		# does". A missing barrier, a wrong layout or a dropped release shows up
		# as thousands to millions of bytes, not as hundreds.
		# SELF-CALIBRATING, with no tolerance written in. When the fixture is
		# perfectly reproducible the floor is 0 and this demands exact byte
		# equality; when it is not, it demands only that the graph be no worse
		# than the harness. A constant here would have been a number picked to
		# make the test pass.
		hl_assert "the graph changes the desktop no more than run-to-run noise" \
			"$(awk -v d="$DIFF" -v f="$FLOOR" 'BEGIN{
				print (d <= f) ? "yes" : "no (" d " vs floor " f ")" }')" "yes"

		echo "  ---- cpu_frame_us p50 $OLD_CPU -> $NEW_CPU   p95 $OLD_P95 -> $NEW_P95"
		echo "  ---- gpu_frame_us_avg $OLD_GPU -> $NEW_GPU"
		echo "  ---- record_us_avg    n/a -> $NEW_REC (M4E field; the old binary has none)"
		# No threshold picked in advance. Two compositor runs differ in damage,
		# client redraw and scheduling, so a tight bound here would be a
		# coin-flip. What is asserted is the ORDER OF MAGNITUDE -- a graph that
		# had doubled the cost of a frame would fail this, and a graph within
		# run-to-run noise cannot.
		hl_assert "CPU frame cost p50 is within 2x of the pre-graph renderer" \
			"$(awk -v o="$OLD_CPU" -v n="$NEW_CPU" 'BEGIN{
				if (o+0 <= 0) { print "no-baseline" }
				else { print (n <= o * 2) ? "yes" : "no (" n " vs " o ")" } }')" "yes"
		hl_assert "GPU frame cost is within 2x of the pre-graph renderer" \
			"$(awk -v o="$OLD_GPU" -v n="$NEW_GPU" 'BEGIN{
				if (o+0 <= 0) { print "no-baseline" }
				else { print (n <= o * 2) ? "yes" : "no (" n " vs " o ")" } }')" "yes"
	fi
fi

hl_summary
