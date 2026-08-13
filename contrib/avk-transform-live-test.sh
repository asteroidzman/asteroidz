#!/usr/bin/env bash
# avk-transform-live-test.sh — changing an output's transform while it runs.
#
# M4F.2C.4e. Every transform test before this one set the transform in the
# CONFIG FILE and started a compositor with it. That never produces the frame
# this file is about: the one whose wlr_output_state carries a new transform
# while the wlr_output still reports the old one. On that frame the attachment
# extent and the presentation extent come from different structures, and
# reading either from the wrong one draws a frame half in the configuration the
# output is leaving.
#
# It is not hypothetical. az_avk_emit_cursors() read `output->transform` while
# the rest of the walk used the frame's pending transform; a modeset frame
# would have placed the cursor by the transform being left behind. And the
# mode/transform pair is exactly where M4F.2C.4d's defect lived.
#
# ── THE THREE PHASES ──────────────────────────────────────────────────────
#
#   1. ORACLE. The whole transition sequence under AZ_FRAME_ORACLE=1, which
#      renders every frame a second time from the same snapshot with full
#      damage and compares. A transform change that failed to expand damage,
#      left a stale region, or wrote outside the new attachment diverges on the
#      frame it happened, and the oracle names that frame. This assertion needs
#      no reference image and no layout convergence.
#
#   2. PIXEL. Arriving at transform T by rotating a running compositor must
#      produce the same frame as STARTING at transform T. This is the one that
#      would catch a transform applied to geometry but not to damage, or an
#      attachment that kept its old extents.
#
#   3. MODE + TRANSFORM, in one commit. 800x600 normal -> 1024x768 90 ->
#      600x800 270 -> 800x600 flipped-90: the mode changes, the presentation
#      extents swap, and on two of them both change at once. The attachment
#      must be the new mode and the presentation extent its transposition, on
#      the FIRST frame.
#
# MODE=oracle|pixel|modeset runs one phase; the default runs all three.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-transform-live"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-tflive-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

MODE="${MODE:-all}"

FLAT="border_radius 0
effects { shadow { enable 0 }; blur { enable 0 } }"

log_of() { echo "$HL_OUTDIR/state/asteroidz/asteroidz.log"; }

# `grep -c ... || echo 0` PRINTS TWO LINES when grep matches nothing: the count
# grep already wrote, and the fallback. An assertion then compares "0\n0"
# against "0" and fails a passing run. Piping through wc -l cannot do that.
oracle_frames()  { grep "oracle .* WRONG=" "$1" 2>/dev/null | wc -l; }
oracle_wrong()   { grep "FIRST DIVERGENCE" "$1" 2>/dev/null | wc -l; }
oracle_dropped() {
	grep -o "dropped=[0-9]*" "$1" 2>/dev/null | tail -1 | cut -d= -f2
}
oracle_invalid() {
	grep -o "invalidated=[0-9]*" "$1" 2>/dev/null | tail -1 | cut -d= -f2
}

# THE FIXTURE, twice over: two flat opaque toplevels of distinct colours. Flat
# because every comparison here is between RUNS or across a reconfiguration,
# and a terminal lays its glyphs out for the output it was told about -- 7183
# differing pixels of text, measured, none of them a transform defect.
spawn_fixture() {
	hl_spawn_wlbgeffect one 300 "w1-$1" ffb03020 >/dev/null
	hl_wait_client_count 1 60
	hl_spawn_wlbgeffect two 300 "w2-$1" ff30b040 >/dev/null
	hl_wait_client_count 2 60
	sleep 3
	hl_sync_pointer_extent && hl_move 300 400
	sleep 1
}

grab() { # grab CAPTUREDIR NAME TRANSFORM
	hl_dispatch capture_output
	sleep 2
	cp -f "$1/HEADLESS-1.ppm" "$OUTDIR/$2.ppm" 2>/dev/null || true
	if [ -s "$OUTDIR/$2.ppm" ]; then
		python3 "$PPM" canon "$OUTDIR/$2.ppm" "$OUTDIR/$2-canon.ppm" "$3" \
			>/dev/null
	fi
}

# ── 1. the transition sequence, under the frame oracle ─────────────────────

if [ "$MODE" = "all" ] || [ "$MODE" = "oracle" ]; then
	echo
	echo "── 1. every transition, under the frame oracle ───────────────────────"
	HL_WIDTH=800 HL_HEIGHT=600 HL_RR1=0
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_FRAME_ORACLE=1 AZ_SHADOW_DITHER_AMP=0"
	export HL_WIDTH HL_HEIGHT HL_RR1 HL_ENV
	hl_start "$FLAT"
	spawn_fixture osq
	# THE WHOLE CYCLE, rotations and mirrors, including the transitions
	# BETWEEN mirror families -- 270 -> flipped is where both the anchor and
	# the winding change at once.
	for T in 1 2 3 0 4 5 6 7 0 2 5 1; do
		hl_dispatch "set_output_transform,HEADLESS-1,$T"
		sleep 2
	done
	hl_dispatch damage_all
	sleep 2
	OLOG=$(log_of)
	OF=$(oracle_frames "$OLOG"); OW=$(oracle_wrong "$OLOG")
	OD=$(oracle_dropped "$OLOG"); OI=$(oracle_invalid "$OLOG")
	cp -f "$OLOG" "$OUTDIR/oracle.log" 2>/dev/null || true
	# EVERY TRANSFORM ACTUALLY HAPPENED, which is the premise: a dispatch that
	# silently did nothing would produce a clean oracle run and prove nothing.
	APPLIED=$(grep "output: HEADLESS-1 -> attachment" "$OUTDIR/oracle.log" \
		2>/dev/null | wc -l)
	hl_stop
	echo "  $OF frames compared, $OW divergent, dropped=${OD:-?} invalidated=${OI:-?}"
	echo "  $APPLIED transform commits applied"
	grep -m1 "FIRST DIVERGENCE" "$OUTDIR/oracle.log" 2>/dev/null |
		sed 's/.*\] /    /'
	hl_assert "PREMISE: every transform was actually committed ($APPLIED)" \
		"$([ "${APPLIED:-0}" -ge 12 ] && echo true || echo false)" true
	hl_assert "PREMISE: the oracle compared frames at all ($OF)" \
		"$([ "${OF:-0}" -gt 0 ] && echo true || echo false)" true
	hl_assert "no tap was dropped" "${OD:-x}" 0
	hl_assert "no comparison was invalidated" "${OI:-x}" 0
	hl_assert "no frame of any transition diverges from a full render" \
		"${OW:-1}" 0
fi

# ── 2. arriving at a transform == starting at it ───────────────────────────

if [ "$MODE" = "all" ] || [ "$MODE" = "pixel" ]; then
	echo
	echo "── 2. rotating into a transform == starting in it ────────────────────"
	# The live captures, all seven from ONE compositor: rotate, capture,
	# rotate, capture. That also makes the sequence cumulative, so transform 7
	# is reached after six earlier reconfigurations rather than from a clean
	# start -- if any of them leaves state behind, the last one carries it.
	LIVEDIR="$OUTDIR/cap-live"
	mkdir -p "$LIVEDIR"
	HL_WIDTH=800 HL_HEIGHT=600 HL_RR1=0
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_SHADOW_DITHER_AMP=0 \
AZ_AVK_CAPTURE_DIR=$LIVEDIR"
	export HL_WIDTH HL_HEIGHT HL_RR1 HL_ENV
	hl_start "$FLAT"
	spawn_fixture live
	for T in 1 2 3 4 5 6 7; do
		hl_dispatch "set_output_transform,HEADLESS-1,$T"
		sleep 3
		grab "$LIVEDIR" "live-$T" "$T"
	done
	hl_stop

	for T in 1 2 3 4 5 6 7; do
		SDIR="$OUTDIR/cap-static-$T"
		mkdir -p "$SDIR"
		HL_WIDTH=800 HL_HEIGHT=600 HL_RR1=$T
		HL_ENV="ASTEROIDZ_RENDERER=avk AZ_SHADOW_DITHER_AMP=0 \
AZ_AVK_CAPTURE_DIR=$SDIR"
		export HL_WIDTH HL_HEIGHT HL_RR1 HL_ENV
		hl_start "$FLAT"
		spawn_fixture "static$T"
		grab "$SDIR" "static-$T" "$T"
		hl_stop
	done

	for T in 1 2 3 4 5 6 7; do
		read -r D W <<<"$(python3 "$PPM" diff "$OUTDIR/live-$T-canon.ppm" \
			"$OUTDIR/static-$T-canon.ppm" 2>/dev/null || echo "-1 -1")"
		echo "  transform $T: rotated-into vs started-in differs $D px (worst $W)"
		if [ "${D:-1}" != "0" ] && [ "${D:-1}" != "-1" ]; then
			python3 "$PPM" grid "$OUTDIR/live-$T-canon.ppm" \
				"$OUTDIR/static-$T-canon.ppm" 20 | sed 's/^/    /'
		fi
		hl_assert "transform $T: rotating into it == starting in it ($D px)" \
			"$([ "${D:-1}" = "0" ] && echo true || echo false)" true
		# AND THE EXTENTS ARE THE NEW ONES. A capture whose attachment kept the
		# old shape would fail the comparison above too, but only this says why.
		hl_assert "transform $T: the attachment is still the mode" \
			"$(python3 "$PPM" info "$OUTDIR/live-$T.ppm" 2>/dev/null)" "800 600"
	done
fi

# ── 3. mode and transform, in one commit ───────────────────────────────────

if [ "$MODE" = "all" ] || [ "$MODE" = "modeset" ]; then
	echo
	echo "── 3. mode and transform changing together ───────────────────────────"
	MDIR="$OUTDIR/cap-modeset"
	mkdir -p "$MDIR"
	HL_WIDTH=800 HL_HEIGHT=600 HL_RR1=0
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_FRAME_ORACLE=1 AZ_SHADOW_DITHER_AMP=0 \
AZ_AVK_CAPTURE_DIR=$MDIR"
	export HL_WIDTH HL_HEIGHT HL_RR1 HL_ENV
	hl_start "$FLAT"
	spawn_fixture modeset
	# Every combination the brief names: the mode's extents swap, the
	# presentation extents swap, and both change at once.
	IDX=0
	for STEP in "1024x768 1" "2560x1440 0" "600x800 3" "800x600 5" \
			"1024x768 7" "800x600 0"; do
		# shellcheck disable=SC2086
		set -- $STEP
		hl_dispatch "set_output_mode_transform,HEADLESS-1,$1,$2"
		sleep 3
		grab "$MDIR" "ms-$IDX" "$2"
		WANT_ATT="${1%x*} ${1#*x}"
		GOT_ATT="$(python3 "$PPM" info "$OUTDIR/ms-$IDX.ppm" 2>/dev/null)"
		GOT_PRES="$(python3 "$PPM" info "$OUTDIR/ms-$IDX-canon.ppm" 2>/dev/null)"
		case "$2" in
		1|3|5|7) WANT_PRES="${1#*x} ${1%x*}" ;;
		*)       WANT_PRES="$WANT_ATT" ;;
		esac
		printf "  %s at transform %s: attachment %-9s presentation %s\n" \
			"$1" "$2" "$GOT_ATT" "$GOT_PRES"
		hl_assert "$1 rr$2: the attachment is the MODE ($GOT_ATT)" \
			"$GOT_ATT" "$WANT_ATT"
		hl_assert "$1 rr$2: the presentation raster is its transposition" \
			"$GOT_PRES" "$WANT_PRES"
		IDX=$(( IDX + 1 ))
	done
	# NO STALE PIXELS AFTER THE LAST ONE: a full repaint of the same scene must
	# change nothing. The oracle below says the same thing frame by frame; this
	# says it about the state the desktop was finally left in.
	hl_dispatch damage_all
	sleep 2
	grab "$MDIR" "ms-full" 0
	read -r D W <<<"$(python3 "$PPM" diff "$OUTDIR/ms-$(( IDX - 1 )).ppm" \
		"$OUTDIR/ms-full.ppm" 2>/dev/null || echo "-1 -1")"
	MLOG=$(log_of)
	MF=$(oracle_frames "$MLOG"); MW=$(oracle_wrong "$MLOG")
	cp -f "$MLOG" "$OUTDIR/modeset.log" 2>/dev/null || true
	hl_stop
	echo "  after the last modeset, a full repaint changes $D px (worst $W)"
	hl_assert "no stale pixels after six mode+transform changes ($D px)" \
		"$([ "${D:-1}" = "0" ] && echo true || echo false)" true
	echo "  $MF frames compared under the oracle, $MW divergent"
	grep -m1 "FIRST DIVERGENCE" "$OUTDIR/modeset.log" 2>/dev/null |
		sed 's/.*\] /    /'
	hl_assert "PREMISE: the oracle compared frames at all ($MF)" \
		"$([ "${MF:-0}" -gt 0 ] && echo true || echo false)" true
	hl_assert "no mode+transform frame diverges from a full render" "${MW:-1}" 0
fi

echo
echo "logs: $OUTDIR"
hl_summary
