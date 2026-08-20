#!/usr/bin/env bash
# avk-scale-transform-test.sh — fractional scale crossed with output transform.
#
# M4F.2C.4e. Scale and transform are applied one after the other on the same
# line: a node's logical box is multiplied by the output scale into the
# PRESENTATION raster, and then mapped by wlr_box_transform() into the
# ATTACHMENT. Each half has been tested on its own -- the fractional-scale
# rounding since M4C, the transform since M4F.2C.4d -- and their composition
# never has.
#
# It is exactly the composition that can go wrong. `az_avk_box_to_output()`
# rounds the FAR EDGE and subtracts, rather than rounding the width, so adjacent
# windows at 1.25x meet exactly instead of overlapping or leaving a hairline.
# That property is about the presentation raster. If the transform were applied
# before the scale -- or if the scale were applied to an attachment-space box --
# the rounding would happen against the wrong axis on a rotated output, and a
# seam would open along one edge and nowhere else.
#
# ── THE COMPARISON ────────────────────────────────────────────────────────
#
# The same trick as the transform oracle, at each scale: a rotated 800x600
# output has the same LOGICAL desktop as an unrotated 600x800 one, so the two
# are directly comparable once the rotated capture is canonicalised. Both go
# through the identical scale conversion in presentation space and differ only
# in the final transform, so the answer is exact -- not "close enough for a
# fractional scale".
#
#     mode 600x800 rr0 scale S    <- reference
#     mode 800x600 rr1 scale S       90
#     mode 800x600 rr3 scale S       270
#     mode 800x600 rr5 scale S       flipped-90
#
# No snapping, no tolerance: if a fractional scale needed one of those to pass,
# the geometry would be wrong and the test would be hiding it.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-scale-transform"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-scaletf-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

FLAT="border_radius 0
effects { shadow { enable 0 }; blur { enable 0 } }"

SCALES="${SCALES:-1 1.25 1.5 1.75 2}"
TRANSFORMS="${TRANSFORMS:-1 3 5}"
MODE="${MODE:-all}"

log_of() { echo "$HL_OUTDIR/state/asteroidz/asteroidz.log"; }
oracle_frames() { grep "oracle .* WRONG=" "$1" 2>/dev/null | wc -l; }
oracle_wrong()  { grep "FIRST DIVERGENCE" "$1" 2>/dev/null | wc -l; }

shot() { # shot NAME MODE_W MODE_H RR SCALE [EXTRA_ENV]
	local name="$1" mw="$2" mh="$3" rr="$4" sc="$5" extra="${6:-}"
	local cdir="$OUTDIR/cap-$name"
	mkdir -p "$cdir"
	HL_WIDTH="$mw" HL_HEIGHT="$mh" HL_RR1="$rr" HL_SCALE1="$sc"
	HL_ENV="AZ_SHADOW_DITHER_AMP=0 \
AZ_AVK_CAPTURE_DIR=$cdir $extra"
	export HL_WIDTH HL_HEIGHT HL_RR1 HL_SCALE1 HL_ENV
	hl_start "$FLAT"
	hl_spawn_wlbgeffect one   300 "w1-$name" ffb03020 >/dev/null
	hl_wait_client_count 1 60
	hl_spawn_wlbgeffect two   300 "w2-$name" ff30b040 >/dev/null
	hl_wait_client_count 2 60
	hl_spawn_wlbgeffect three 300 "w3-$name" ff2040c0 >/dev/null
	hl_wait_client_count 3 60
	sleep 3
	# A LAYOUT coordinate, which is the same logical place on every shape here.
	hl_sync_pointer_extent && hl_move 200 250
	sleep 2
	# THE CLIENTS ARE STILL THERE, checked again HERE and not only at spawn.
	#
	# hl_wait_client_count is satisfied the moment the windows appear; a client
	# that dies afterwards leaves a fixture with fewer windows in it, and the
	# comparison then reports a large, meaningless difference between two
	# different desktops instead of failing on its premise. That happened: seven
	# wlbgeffect clients were killed by SIGBUS when /tmp -- a tmpfs -- filled up,
	# and two whole scale rows measured nonsense before anything said so.
	local live
	live="$(hl_client_count 2>/dev/null || echo 0)"
	if [ "${live:-0}" -lt 3 ]; then
		echo "  note: only ${live:-0} of 3 fixture clients are alive at capture time"
		hl_assert "PREMISE: $name still has its three windows (${live:-0})" \
			"false" "true"
	fi
	hl_dispatch capture_output
	sleep 2
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	cp -f "$(log_of)" "$OUTDIR/$name.log" 2>/dev/null || true
	hl_stop
	[ -s "$OUTDIR/$name.ppm" ] && python3 "$PPM" canon "$OUTDIR/$name.ppm" \
		"$OUTDIR/$name-canon.ppm" "$rr" >/dev/null
}

if [ "$MODE" = "all" ] || [ "$MODE" = "pixel" ]; then
echo
echo "── every scale, crossed with the odd transforms ──────────────────────"
for S in $SCALES; do
	SN="${S//./_}"
	shot "ref-$SN" 600 800 0 "$S"
	REFINFO="$(python3 "$PPM" info "$OUTDIR/ref-$SN.ppm" 2>/dev/null)"
	hl_assert "scale $S: the reference attachment is the mode ($REFINFO)" \
		"$REFINFO" "600 800"
	for T in $TRANSFORMS; do
		shot "s$SN-rr$T" 800 600 "$T" "$S"
		read -r D W <<<"$(python3 "$PPM" diff "$OUTDIR/s$SN-rr$T-canon.ppm" \
			"$OUTDIR/ref-$SN-canon.ppm" 2>/dev/null || echo "-1 -1")"
		echo "  scale $S transform $T: $D px differ (worst $W)"
		if [ "${D:-1}" != "0" ] && [ "${D:-1}" != "-1" ]; then
			echo "    bbox: $(python3 "$PPM" bbox "$OUTDIR/s$SN-rr$T-canon.ppm" \
				"$OUTDIR/ref-$SN-canon.ppm")"
			python3 "$PPM" grid "$OUTDIR/s$SN-rr$T-canon.ppm" \
				"$OUTDIR/ref-$SN-canon.ppm" 20 | sed 's/^/    /'
		fi
		hl_assert "scale $S transform $T: exact against the unrotated reference ($D px)" \
			"$([ "${D:-1}" = "0" ] && echo true || echo false)" true
		# THE ATTACHMENT IS STILL THE MODE, at a fractional scale too: the
		# scale changes the LOGICAL size of the output and nothing else.
		hl_assert "scale $S transform $T: the attachment is still the mode" \
			"$(python3 "$PPM" info "$OUTDIR/s$SN-rr$T.ppm" 2>/dev/null)" "800 600"
	done
done
fi

if [ "$MODE" = "all" ] || [ "$MODE" = "oracle" ]; then
echo
echo "── partial == forced-full, at a fractional scale ─────────────────────"
# The pixel comparison above says the frames agree. This says they agree
# FRAME BY FRAME with a full render of the same snapshot -- which is what
# catches damage that is a rounded rectangle in one space and the raster it
# has to cover in another.
for S in 1.25 1.5; do
	for T in 0 1 5; do
		SN="${S//./_}"
		HL_WIDTH=800 HL_HEIGHT=600 HL_RR1="$T" HL_SCALE1="$S"
		HL_ENV="AZ_FRAME_ORACLE=1 AZ_SHADOW_DITHER_AMP=0"
		export HL_WIDTH HL_HEIGHT HL_RR1 HL_SCALE1 HL_ENV
		hl_start "$FLAT"
		hl_reset_spawn_colors 2>/dev/null || true
		hl_spawn_kitty a >/dev/null; hl_wait_client_count 1 60
		sleep 2
		hl_spawn_wlbgeffect w 300 "o-$SN-$T" >/dev/null
		hl_wait_client_count 2 60
		sleep 2
		hl_dispatch toggle_floating
		sleep 1
		# Odd pixel positions on purpose: at 1.25 and 1.5 an odd logical
		# coordinate is where the far-edge rounding actually differs from
		# rounding the width.
		for POS in "101,73" "0,151" "233,0" "197,199"; do
			hl_dispatch "move_window,$POS"
			sleep 2
		done
		hl_dispatch damage_all
		sleep 2
		OL=$(log_of)
		OF=$(oracle_frames "$OL"); OW=$(oracle_wrong "$OL")
		cp -f "$OL" "$OUTDIR/oracle-$SN-$T.log" 2>/dev/null || true
		hl_stop
		echo "  scale $S transform $T: $OF frames compared, $OW divergent"
		grep -m1 "FIRST DIVERGENCE" "$OUTDIR/oracle-$SN-$T.log" 2>/dev/null |
			sed 's/.*\] /    /'
		hl_assert "PREMISE: scale $S transform $T compared frames ($OF)" \
			"$([ "${OF:-0}" -gt 0 ] && echo true || echo false)" true
		hl_assert "scale $S transform $T: partial == forced-full on every frame" \
			"${OW:-1}" 0
	done
done
fi

echo
echo "logs: $OUTDIR"
hl_summary
