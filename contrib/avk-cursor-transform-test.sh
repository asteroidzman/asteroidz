#!/usr/bin/env bash
# avk-cursor-transform-test.sh — the cursor, at every output transform.
#
# M4F.2C.4e. The transform pixel oracle deliberately PARKED the cursor at a
# fixed layout coordinate and left it there, because a pointer that moved
# between two runs would have shown up as a difference that was not a transform
# defect. That made the cursor the one part of the frame the oracle never
# examined -- and the cursor is drawn through its own call to
# wlr_box_transform(), on its own line, with its own source space and its own
# hotspot subtraction. Every one of those can be wrong independently of the
# scene walk.
#
# It was: az_avk_emit_cursors() mapped the cursor with `output->transform`
# while the rest of the walk used the frame's PENDING transform. On a modeset
# frame those differ, and the cursor would have been placed by the transform
# the output was leaving. Found by the M4F.2C.4e variable-reuse audit and fixed
# there; this is the test that would have found it.
#
# ── WHAT IS ASSERTED ──────────────────────────────────────────────────────
#
#   POSITION AND HOTSPOT. The cursor's bounding box in PRESENTATION
#   coordinates -- found by differencing against a frame with the pointer
#   elsewhere -- must start at the pointer minus the hotspot. The hotspot of
#   the default arrow is at its tip, so a compositor that ignored it would draw
#   a complete, correct cursor a few pixels down and to the right; only a
#   position assertion can tell those apart. AZ_AVK_NO_CURSOR_HOTSPOT is that
#   assertion's falsifier and phase 3 runs it.
#
#   ORIENTATION AND CLIP. The whole canonicalised frame at transform T must
#   equal the whole canonicalised frame at transform 0 with the pointer in the
#   same logical place. That covers the cursor's own orientation, its clip at
#   an edge, and the damage it leaves behind, without needing a separate
#   assertion for each.
#
#   SIX POSITIONS: the four corners, the centre, and one deliberately half off
#   the right-hand edge, which is where a clip in the wrong space stops being
#   a no-op.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-cursor-transform"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-cursor-tf-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

# The same flat fixture the transform oracle uses, and for the same reason: the
# comparisons here are between RUNS, so anything that lays itself out for the
# output it was told about (text) or hashes the device pixel grid (dither) is a
# difference that is not a transform defect. Measured, not assumed: 7183 px of
# text and 134389 px of dither at 180 degrees.
FLAT="border_radius 0
effects { shadow { enable 0 }; blur { enable 0 } }"

# The default arrow's hotspot is at its tip. Read from the frame rather than
# assumed: phase 1 measures it at transform 0 and every other transform is
# checked against that measurement.
HOTSPOT_X=0
HOTSPOT_Y=0
CURSOR_W=0
CURSOR_H=0

TRANSFORMS="${TRANSFORMS:-0 1 2 3 4 5 6 7}"
BREAK="${BREAK:-}"

# run_transform RR  -> writes $OUTDIR/c-RR-<pos>.ppm, canonicalised
# run_transform RR [MODE_W MODE_H NAME]
# run_transform RR [MODE_W MODE_H NAME EXTRA_ENV]
run_transform() {
	# TWO `local` STATEMENTS, not one. `local a="$1" b="...$a"` declares every
	# name before it assigns any of them, so under `set -u` the second
	# initialiser reads an unset variable and the function dies before it has
	# started -- which is what happened, silently, to a whole matrix run.
	local rr="$1"
	local mw="${2:-800}" mh="${3:-600}"
	local tag="${4:-$rr}"
	local envextra="${5:-}"
	local cdir="$OUTDIR/cap-$tag"
	mkdir -p "$cdir"
	local extra=""
	[ "$BREAK" = "no-hotspot" ] && extra="AZ_AVK_NO_CURSOR_HOTSPOT=1"
	HL_WIDTH="$mw" HL_HEIGHT="$mh" HL_RR1="$rr"
	# THE HEADLESS BACKEND BELIEVES IT HAS A CURSOR PLANE. set_cursor() is
	# `return true;` and does nothing, so every cursor goes onto a plane that
	# does not exist and NOTHING reaches the frame -- a capture with the pointer
	# in seven different places is seven identical files, and a position
	# assertion built on them measures nothing at all. That is exactly what the
	# first run of this test did.
	#
	# ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1 takes the same lock screencopy's
	# overlay_cursor takes, so AVK composites the cursor into every frame.
	HL_ENV="AZ_SHADOW_DITHER_AMP=0 \
ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1 AZ_AVK_CAPTURE_DIR=$cdir $extra $envextra"
	export HL_WIDTH HL_HEIGHT HL_RR1 HL_ENV
	hl_start "$FLAT"
	hl_spawn_wlbgeffect one   300 "w1-$tag" ffb03020 >/dev/null
	hl_wait_client_count 1 60
	hl_spawn_wlbgeffect two   300 "w2-$tag" ff30b040 >/dev/null
	hl_wait_client_count 2 60
	sleep 3
	hl_sync_pointer_extent || return 1
	# LAYOUT coordinates, which for a single output at 0,0 are also this
	# output's PRESENTATION coordinates. The desktop transposes at odd
	# transforms, so the positions are computed from the presentation extent
	# rather than from the mode -- a corner of the mode is not a corner of the
	# desktop at 90 degrees.
	local lw="$mw" lh="$mh"
	case "$rr" in 1|3|5|7) lw="$mh"; lh="$mw" ;; esac
	local i=0 name
	for pos in "40,40" "$(( lw - 40 )),40" "40,$(( lh - 40 ))" \
			"$(( lw - 40 )),$(( lh - 40 ))" "$(( lw / 2 )),$(( lh / 2 ))" \
			"$(( lw - 4 )),$(( lh / 2 ))" "$(( lw / 2 )),$(( lh - 4 ))"; do
		name="c-$tag-$i"
		# shellcheck disable=SC2086
		hl_move ${pos//,/ }
		sleep 1
		hl_dispatch capture_output
		sleep 2
		cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
		[ -s "$OUTDIR/$name.ppm" ] && python3 "$PPM" canon "$OUTDIR/$name.ppm" \
			"$OUTDIR/$name-canon.ppm" "$rr" >/dev/null
		i=$(( i + 1 ))
	done
	hl_stop
}

# The seven positions, named, in PRESENTATION coordinates for each shape.
pos_of() { # pos_of RR INDEX -> "X Y"
	local rr="$1"
	local i="$2"
	local lw=800 lh=600
	case "$rr" in 1|3|5|7|tall) lw=600; lh=800 ;; esac
	case "$i" in
	0) echo "40 40" ;;
	1) echo "$(( lw - 40 )) 40" ;;
	2) echo "40 $(( lh - 40 ))" ;;
	3) echo "$(( lw - 40 )) $(( lh - 40 ))" ;;
	4) echo "$(( lw / 2 )) $(( lh / 2 ))" ;;
	5) echo "$(( lw - 4 )) $(( lh / 2 ))" ;;
	6) echo "$(( lw / 2 )) $(( lh - 4 ))" ;;
	esac
}
POS_NAMES=(top-left top-right bottom-left bottom-right centre \
	right-edge-clipped bottom-edge-clipped)

echo
echo "── 0. every transform, seven pointer positions ───────────────────────"
# THE TALL REFERENCE, an unrotated 600x800 output. The desktop's shape follows
# the transform's parity, so an odd transform has no 800x600 reference to be
# compared against and would otherwise be checked only against other odd
# transforms -- four conventions agreeing with each other and with nothing.
run_transform 0 600 800 tall
run_transform 0 600 800 ntall "AZ_AVK_NO_CURSOR=1"
printf "  reference (600x800 at 0): %s captures\n" \
	"$(ls "$OUTDIR"/c-tall-*-canon.ppm 2>/dev/null | wc -l)"
for RR in $TRANSFORMS; do
	run_transform "$RR"
	# THE SAME DESKTOP WITH NO CURSOR IN IT, which is what makes the position
	# measurement exact rather than a guess about which corner of a union
	# belongs to which pointer.
	run_transform "$RR" 800 600 "n$RR" "AZ_AVK_NO_CURSOR=1"
	printf "  transform %s: %s\n" "$RR" \
		"$(ls "$OUTDIR"/c-"$RR"-*-canon.ppm 2>/dev/null | wc -l) captures"
done
hl_assert "PREMISE: a capture was produced at transform 0" \
	"$([ -s "$OUTDIR/c-0-0.ppm" ] && echo true || echo false)" true

echo
echo "── 1. the cursor is where the pointer is, minus the hotspot ──────────"
# WHERE THE CURSOR IS, EXACTLY: each frame is differenced against a frame of
# the same desktop rendered with AZ_AVK_NO_CURSOR=1, so the bounding box of the
# difference IS the cursor -- its origin and its size, with nothing else in it.
#
# The first version differenced two frames that both had a cursor in them and
# then guessed which corner of the union belonged to which, using an assumed
# 24-pixel cursor. It reported the top-left position exactly and every other
# position off by the amount the guess was wrong, which is a measurement
# pretending to be a result.
for RR in $TRANSFORMS; do
	# THE CENTRE FIRST, because it is where the hotspot and the cursor's size
	# are measured and every other position is checked against that
	# measurement. Ascending order put four positions before their own
	# calibration, which then compared them against a hotspot of 0,0 and a
	# cursor of 0x0 -- eight failures that were entirely the test's own
	# ordering. Nothing is clipped at the centre, which is the other reason it
	# is the one to calibrate on.
	for I in 4 0 1 2 3 5 6; do
		read -r PX PY <<<"$(pos_of "$RR" "$I")"
		BB=$(python3 "$PPM" bbox "$OUTDIR/c-$RR-$I-canon.ppm" \
			"$OUTDIR/c-n$RR-$I-canon.ppm" 2>/dev/null)
		read -r X0 Y0 X1 Y1 <<<"$BB"
		if [ "$I" = 4 ]; then
			hl_assert "rr$RR PREMISE: the cursor is IN the frame at all" \
				"$([ "${X1:--1}" -gt 0 ] && [ "${X1:-0}" -lt 100000 ] && \
					echo true || echo false)" true
		fi
		CW=$(( X1 - X0 )); CH=$(( Y1 - Y0 ))
		if [ "$RR" = "$(echo "$TRANSFORMS" | awk '{print $1}')" ] && \
				[ "$I" = 4 ]; then
			# CALIBRATION, once, in the middle of the output where nothing is
			# clipped: the hotspot is a property of the cursor THEME, not of the
			# compositor, so it is measured and every transform is then required
			# to agree with the measurement.
			HOTSPOT_X=$(( PX - X0 ))
			HOTSPOT_Y=$(( PY - Y0 ))
			CURSOR_W=$CW
			CURSOR_H=$CH
			echo "  measured at the centre: hotspot ${HOTSPOT_X},${HOTSPOT_Y}, cursor ${CW}x${CH}"
		fi
		DX=$(( PX - X0 - HOTSPOT_X ))
		DY=$(( PY - Y0 - HOTSPOT_Y ))
		printf "  rr%s %-19s pointer %s,%s cursor %s,%s %sx%s (off by %s,%s)\n" \
			"$RR" "${POS_NAMES[$I]}" "$PX" "$PY" "$X0" "$Y0" "$CW" "$CH" \
			"$DX" "$DY"
		hl_assert "rr$RR ${POS_NAMES[$I]}: the cursor is at the pointer ($DX,$DY off)" \
			"$([ "${DX#-}" -le 1 ] && [ "${DY#-}" -le 1 ] && echo true || \
				echo false)" true
		case "$I" in
		5|6)
			# CLIPPED ON PURPOSE. The pointer is four pixels from the edge, so
			# most of the cursor is outside the output and what remains must be
			# clipped in the ATTACHMENT: a clip applied in the wrong space
			# either removes too much (a cursor that vanishes early) or nothing
			# at all (one that wraps to the far side).
			hl_assert "rr$RR ${POS_NAMES[$I]}: clipped, not whole or missing ($CW x $CH)" \
				"$([ "$CW" -gt 0 ] && [ "$CH" -gt 0 ] && \
					{ [ "$CW" -lt "${CURSOR_W:-99}" ] || \
					  [ "$CH" -lt "${CURSOR_H:-99}" ]; } && echo true || \
					echo false)" true
			;;
		*)
			hl_assert "rr$RR ${POS_NAMES[$I]}: the whole cursor is drawn (${CW}x${CH})" \
				"$CW $CH" "${CURSOR_W:-?} ${CURSOR_H:-?}"
			;;
		esac
	done
done

echo
echo "── 2. the same desktop, at every transform ───────────────────────────"
# ORIENTATION, CLIP AND DAMAGE IN ONE ASSERTION. The canonicalised frame at
# transform T must equal the canonicalised frame at transform 0 with the
# pointer in the same logical place -- which for odd transforms means the
# transposed reference, because the desktop's shape follows the transform's
# parity. Position 5 and 6 are the clipped ones: a cursor half off the edge has
# to be clipped in the ATTACHMENT, and a clip applied in the wrong space either
# removes too much (a cursor that vanishes early) or nothing at all.
for RR in $TRANSFORMS; do
	[ "$RR" = 0 ] && continue
	case "$RR" in
	1|3|5|7) REF=tall ;;
	*)       REF=0 ;;
	esac
	for I in 0 1 2 3 4 5 6; do
		read -r D W <<<"$(python3 "$PPM" diff "$OUTDIR/c-$RR-$I-canon.ppm" \
			"$OUTDIR/c-$REF-$I-canon.ppm" 2>/dev/null || echo "-1 -1")"
		printf "  rr%s %-19s %s px differ from ref-%s (worst %s)\n" "$RR" \
			"${POS_NAMES[$I]}" "$D" "$REF" "$W"
		hl_assert "rr$RR ${POS_NAMES[$I]}: identical to the unrotated reference ($D px)" \
			"$([ "${D:-1}" = "0" ] && echo true || echo false)" true
	done
done

echo
echo "logs: $OUTDIR"
hl_summary
