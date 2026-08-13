#!/usr/bin/env bash
# avk-capture-layout-test.sh — the readback itself, at awkward extents.
#
# M4F.2C.4e. `amsg dispatch capture_output` is now a first-class correctness
# instrument: it is what found a 90-degree frame whose bottom 200 rows had never
# been written, after two milestones of tests that could not look at one. That
# makes its own layout load-bearing. A readback with a wrong row pitch, a wrong
# copy extent or a wrong channel order does not produce garbage -- it produces a
# picture, and a picture is exactly what every assertion built on top of it
# consumes.
#
# ── WHY AWKWARD WIDTHS ────────────────────────────────────────────────────
#
# Every real test so far ran at 800x600 and 600x800: even, and multiples of 16.
# A pitch assumption -- a row rounded up to 64 bytes, an extent rounded to a
# tile, a stride taken from the image instead of the buffer -- is invisible at
# those widths and wrong at 799. So: 799, 801, 1023 and 1365, crossed with an
# odd transform, where the copy extent and the presentation extent disagree.
#
# ── WHAT A PITCH BUG LOOKS LIKE ───────────────────────────────────────────
#
# It SHEARS. Every pixel is a real pixel and every colour is right; each row is
# simply displaced from the one above by a constant. The extents still match,
# the file still parses, and the difference against a reference is enormous but
# unstructured. So the assertion here is not "the sizes match" -- it is that a
# straight vertical edge in the desktop is still straight: over EVERY row that
# has one, the first colour transition must be in the same column. Measured:
# 12..12 on a correct readback, 84..383 with the skew break, which is exactly
# one pixel per row over the 300 rows the fixture's edge spans.
#
# AZ_AVK_CAPTURE_ROW_SKEW=1 is that assertion's falsifier: it reads each row one
# pixel further along than the last, which is precisely the bug described above.
# BREAK=skew runs it and requires the shear to be CAUGHT.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-capture-layout"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-caplayout-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

BREAK="${BREAK:-}"

FLAT="border_radius 0
effects { shadow { enable 0 }; blur { enable 0 } }"

# The sizes. Odd, prime-ish, and none of them a multiple of 16 or 64.
SIZES="${SIZES:-799x601 801x599 1023x767 1365x769}"
TRANSFORMS="${TRANSFORMS:-0 1}"

shot() { # shot W H RR -> NAME
	local w="$1"
	local h="$2"
	local rr="$3"
	local name="c-${w}x${h}-$rr"
	local cdir="$OUTDIR/cap-$name"
	mkdir -p "$cdir"
	local extra=""
	[ "$BREAK" = "skew" ] && extra="AZ_AVK_CAPTURE_ROW_SKEW=1"
	HL_WIDTH="$w" HL_HEIGHT="$h" HL_RR1="$rr"
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_SHADOW_DITHER_AMP=0 \
AZ_AVK_CAPTURE_DIR=$cdir $extra"
	export HL_WIDTH HL_HEIGHT HL_RR1 HL_ENV
	hl_start "$FLAT"
	# TWO tiled windows of distinct flat colours: one straight vertical edge
	# down the middle of the desktop, which is the whole measurement.
	hl_spawn_wlbgeffect one 300 "w1-$name" ffb03020 >/dev/null
	hl_wait_client_count 1 60
	hl_spawn_wlbgeffect two 300 "w2-$name" ff30b040 >/dev/null
	hl_wait_client_count 2 60
	sleep 3
	hl_sync_pointer_extent && hl_move 5 5
	sleep 2
	hl_dispatch capture_output
	sleep 2
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	cp -f "$HL_OUTDIR/state/asteroidz/asteroidz.log" "$OUTDIR/$name.log" \
		2>/dev/null || true
	hl_stop
	[ -s "$OUTDIR/$name.ppm" ] && python3 "$PPM" canon "$OUTDIR/$name.ppm" \
		"$OUTDIR/$name-canon.ppm" "$rr" >/dev/null
	echo "$name"
}

echo
echo "── the capture's own layout, at awkward extents ──────────────────────"
for SIZE in $SIZES; do
	W="${SIZE%x*}"; H="${SIZE#*x}"
	for RR in $TRANSFORMS; do
		NAME=$(shot "$W" "$H" "$RR")
		GOT="$(python3 "$PPM" info "$OUTDIR/$NAME.ppm" 2>/dev/null || echo '- -')"
		# What the renderer itself said it copied, from the capture log line.
		LINE="$(grep -m1 "^.*capture: HEADLESS-1 " "$OUTDIR/$NAME.log" \
			2>/dev/null | sed 's/.*capture: //')"
		printf "  %sx%s rr%s: file %-11s %s\n" "$W" "$H" "$RR" "$GOT" "$LINE"

		hl_assert "${W}x${H} rr$RR: the capture is the ATTACHMENT's extent" \
			"$GOT" "$W $H"
		# EXTENT=OK is the renderer comparing the image it copied against the
		# attachment extent it computed. It is cheap and it is the one line
		# that would have said, in M4F.2C.4d, that the two disagreed.
		hl_assert "${W}x${H} rr$RR: the renderer agrees the extents match" \
			"$(echo "$LINE" | grep -c 'extent=OK')" 1
		# TIGHTLY PACKED ROWS. The copy sets bufferRowLength = width, so the
		# buffer stride is width*4 with no alignment padding -- at every one of
		# these widths, none of which is a nice multiple of anything.
		hl_assert "${W}x${H} rr$RR: the readback row is width*4 bytes" \
			"$(echo "$LINE" | sed -n 's/.*row_bytes=\([0-9]*\).*/\1/p')" \
			"$(( W * 4 ))"

		# AND THE EDGE IS STILL STRAIGHT. Measured on the canonicalised
		# frame, where the desktop's vertical split is vertical at every
		# transform, and over EVERY row that has an edge rather than three
		# chosen ones: the fixture's windows do not reach the top and bottom of
		# every output shape, and a fixed sample row that lands on uniform
		# background reports "no edge" -- which is a premise failure dressed up
		# as a result. The first version of this test did exactly that.
		read -r EROWS EY0 EY1 ECMIN ECMAX <<<"$(python3 "$PPM" edgespan \
			"$OUTDIR/$NAME-canon.ppm" 2>/dev/null || echo "0 -1 -1 -1 -1")"
		printf "      edge on %s rows (y %s..%s), columns %s..%s\n" \
			"$EROWS" "$EY0" "$EY1" "$ECMIN" "$ECMAX"
		hl_assert "${W}x${H} rr$RR: PREMISE: there is an edge to measure ($EROWS rows)" \
			"$([ "${EROWS:-0}" -ge 100 ] && echo true || echo false)" true
		# One constant column over hundreds of rows. A pitch wrong by one pixel
		# per row spreads them by the row count: measured, 12..12 correct
		# against 84..383 under AZ_AVK_CAPTURE_ROW_SKEW=1.
		hl_assert "${W}x${H} rr$RR: the edge is straight -- no row-pitch shear ($ECMIN..$ECMAX)" \
			"$([ "$ECMIN" = "$ECMAX" ] && echo true || echo false)" true
	done
done

echo
echo "logs: $OUTDIR"
hl_summary
