#!/usr/bin/env bash
# avk-transform-test.sh — the pixel oracle for a rotated output.
#
# M4F.2C.4d. Until now nothing had ever LOOKED at a 90 or 270 degree frame: grim
# captures nothing at all from a rotated output on the headless backend, so every
# pixel assertion at those transforms was skipped with a stated reason.
# `amsg dispatch capture_output` reads the Vulkan attachment itself -- after
# compositing, before presentation -- and the very first look found the bottom
# 200 rows of a 90-degree frame had never been written at all.
#
# ── WHAT CAN AND CANNOT BE COMPARED ───────────────────────────────────────
#
# A rotated output has a different LOGICAL SHAPE, so the desktop laid out on it
# is a different desktop. What IS comparable is a rotation against an unrotated
# output of the SAME LOGICAL SHAPE:
#
#     mode 800x600 rr 0    logical 800x600   <- reference A
#     mode 800x600 rr 2    logical 800x600      compared against A
#
#     mode 600x800 rr 0    logical 600x800   <- reference B
#     mode 800x600 rr 1    logical 600x800      compared against B
#     mode 800x600 rr 3    logical 600x800      compared against B
#
# Each capture is inverse-transformed into presentation orientation first
# (contrib/lib/ppm.py canon). Comparing raw attachments would compare rotated
# buffers, which is the mistake this exercise exists to avoid.
#
# ── THE ORACLE IS INDEPENDENT OF THE RENDERER ─────────────────────────────
#
# Two ways, and both matter:
#
#   THE REFERENCE IS A DIFFERENT RENDER. rr90 is not compared against a
#   rotation of its own output; it is compared against a SEPARATELY RENDERED
#   600x800 desktop at transform 0. Nothing the transform code does can make
#   both agree by being wrong in the same way.
#
#   THE CANONICALISATION IS CHECKED BY DATA. contrib/lib/ppm.py implements
#   wl_output_transform from its definition in Python, sharing no code with
#   wlroots or with AVK -- and phase 2b then canonicalises each capture through
#   ALL EIGHT candidates and requires the expected one to be the UNIQUE zero.
#   A transcription error in either direction loses that election.
#
# ── ALL EIGHT TRANSFORMS ──────────────────────────────────────────────────
#
# The four rotations do not cover the mirrors. flipped-90 and 90 have the same
# extents and different anchors; a sign error in one is invisible in the other.
# Even and odd transforms are compared against different references, because
# the desktop's shape follows the transform's parity:
#
#     0, 180, flipped, flipped-180        logical 800x600   -> ref-a
#     90, 270, flipped-90, flipped-270    logical 600x800   -> ref-b
#
# ── THE FIXTURE HAS TO SURVIVE BEING RUN TWICE ────────────────────────────
#
# Every comparison here is BETWEEN RUNS, so three things that are not transform
# semantics had to be removed from the fixture first, each of them measured:
#
#   text        a terminal lays its glyphs out for the output it was told
#               about: 7183 differing pixels, every one inside a text row.
#               Flat surfaces of distinct colours have nothing to lay out.
#   the cursor  its position is a property of the pointer, not the transform.
#               Parked at a fixed LAYOUT coordinate, which is the same logical
#               place on both outputs.
#   dither      AZ_SHADOW_DITHER_AMP=0. The dither is a hash of the DEVICE
#               pixel position, so it legitimately differs when the device grid
#               rotates -- 134389 pixels of it at 180 degrees, none of them a
#               defect.
#
# What is left is exact: 0 differing pixels at every rotation.
#
# ── AND THE ORACLE MUST BE ABLE TO FAIL ───────────────────────────────────
#
# Phase 3 renders each transform with AZ_AVK_DAMAGE_HOLE -- a rectangle
# subtracted from every frame's damage after the ring has rotated, so it is
# never redrawn -- and requires the comparison to CATCH it at every rotation.
# An oracle that has never reported a difference is not evidence.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-transform"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-transform-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

# NO ROUNDING, NO SHADOW, NO BLUR, and that is the point of this fixture rather
# than a limitation of it. Every one of those is evaluated analytically in
# ATTACHMENT pixels, so its antialiased edge samples at mirrored positions when
# the device grid rotates and rounds differently by a code or two. Measured with
# effects on and dither off: 1247 differing pixels, 1163 of them differing by
# exactly 1, and the four largest all single samples on a corner arc. That is
# raster semantics, not geometry, and mixing it into the geometric oracle would
# mean choosing a tolerance instead of asserting equality. Effects under
# transform are covered by their own suites and by the partial-vs-full oracle.
FLAT="border_radius 0
effects { shadow { enable 0 }; blur { enable 0 } }"

# ── AND THE WINDOWS MUST NOT BE FLAT ──────────────────────────────────────
#
# A SOLID COLOUR ROTATED BY ANY AMOUNT IS THE SAME SOLID COLOUR. This fixture
# was three flat windows, and it reported 0 differing pixels at all eight
# transforms while every texture on a 90 or 270 degree output was being drawn
# ROTATED 180 DEGREES INSIDE ITS OWN BOX -- a third of the screen wrong, and
# geometrically invisible because the geometry was right and only the sampling
# was not.
#
# WLBGEFFECT_QUAD=1 paints four quadrants instead, which is the smallest
# fixture that is still deterministic between runs (no text, no client layout,
# no font rendering) and can tell all eight orientations apart. With it, the
# defect measures 167400 of 480000 pixels at 90 and 270 and exactly 0 at the
# other six.
#
# QUAD=0 restores the flat fixture, and is kept only to reproduce that result.
[ "${QUAD:-1}" = "1" ] && export WLBGEFFECT_QUAD=1

# capture NAME MODE_W MODE_H TRANSFORM [EXTRA_ENV]
capture() {
	local name="$1" mw="$2" mh="$3" rr="$4" extra="${5:-}"
	local cdir="$OUTDIR/cap-$name"
	mkdir -p "$cdir"
	HL_WIDTH="$mw" HL_HEIGHT="$mh" HL_RR1="$rr"
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_SHADOW_DITHER_AMP=0 \
AZ_AVK_CAPTURE_DIR=$cdir $extra"
	export HL_WIDTH HL_HEIGHT HL_RR1 HL_ENV
	hl_start "$FLAT"
	# Three opaque toplevels of distinct colours, tiled. Distinct colours in
	# distinct places is what makes a rotation distinguishable from its mirror;
	# a symmetric fixture cannot tell those apart at all.
	hl_spawn_wlbgeffect one   300 w1 ffb03020 >/dev/null
	hl_wait_client_count 1 60
	hl_spawn_wlbgeffect two   300 w2 ff30b040 >/dev/null
	hl_wait_client_count 2 60
	hl_spawn_wlbgeffect three 300 w3 ff2040c0 >/dev/null
	hl_wait_client_count 3 60
	sleep 3
	# A fixed LAYOUT coordinate, which is the same LOGICAL place on every
	# output shape here, so the cursor is a constant rather than a variable.
	hl_sync_pointer_extent && hl_move 300 400
	sleep 2
	hl_dispatch capture_output
	sleep 2
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	hl_stop
	if [ -s "$OUTDIR/$name.ppm" ]; then
		python3 "$PPM" canon "$OUTDIR/$name.ppm" "$OUTDIR/$name-canon.ppm" \
			"$rr" >/dev/null
	fi
}

canon_diff() {
	python3 "$PPM" diff "$OUTDIR/$1-canon.ppm" "$OUTDIR/$2-canon.ppm" \
		2>/dev/null || echo "-1 -1"
}

echo
echo "── 0. the premise: is the fixture reproducible between runs? ─────────"
capture ref-a  800 600 0
capture ref-a2 800 600 0
read -r D W <<<"$(canon_diff ref-a ref-a2)"
echo "  two runs of the identical configuration differ by $D px (worst $W)"
hl_assert "PREMISE: a capture was produced at all" \
	"$([ -s "$OUTDIR/ref-a.ppm" ] && echo true || echo false)" true
hl_assert "PREMISE: the fixture is reproducible between runs ($D px)" \
	"$([ "${D:-1}" = "0" ] && echo true || echo false)" true

echo
echo "── 1. attachment geometry ────────────────────────────────────────────"
capture ref-b  600 800 0
for RR in 1 2 3 4 5 6 7; do
	capture "rr$RR" 800 600 "$RR"
done
for n in ref-a ref-b rr1 rr2 rr3 rr4 rr5 rr6 rr7; do
	printf "  %-7s attachment %-9s presentation %s\n" "$n" \
		"$(python3 "$PPM" info "$OUTDIR/$n.ppm" 2>/dev/null || echo '-')" \
		"$(python3 "$PPM" info "$OUTDIR/$n-canon.ppm" 2>/dev/null || echo '-')"
done
# THE ATTACHMENT IS THE MODE, at every transform, and this is the regression
# canary for the defect this phase found. AVK allocated its swapchain from
# wlr_output_transformed_resolution(), so a 90-degree output got a 600x800
# attachment for an 800x600 mode and then mapped its geometry into an 800x600
# space: the right 200 columns of every node were clipped and the bottom 200
# rows of the attachment were never written. wlr_scene asserts the buffer
# matches output_pending_resolution(); on real KMS the other shape is a
# rejected commit rather than a rotated picture.
for RR in 1 3 5 7; do
	hl_assert "a transform-$RR attachment is the MODE size, not the transposed one" \
		"$(python3 "$PPM" info "$OUTDIR/rr$RR.ppm" 2>/dev/null)" "800 600"
	hl_assert "and its presentation raster is the transposed one" \
		"$(python3 "$PPM" info "$OUTDIR/rr$RR-canon.ppm" 2>/dev/null)" "600 800"
done
for RR in 2 4 6; do
	hl_assert "a transform-$RR attachment is 800x600" \
		"$(python3 "$PPM" info "$OUTDIR/rr$RR.ppm" 2>/dev/null)" "800 600"
done
hl_assert "the unrotated 600x800 reference has the transposed shape" \
	"$(python3 "$PPM" info "$OUTDIR/ref-b-canon.ppm" 2>/dev/null)" "600 800"

echo
echo "── 2. the pixel oracle, at all eight transforms ──────────────────────"
for RR in 1 2 3 4 5 6 7; do
	case "$RR" in
	1|3|5|7) REF=ref-b ;;
	*)       REF=ref-a ;;
	esac
	read -r D W <<<"$(canon_diff "rr$RR" "$REF")"
	echo "  rr$RR vs $REF: $D px differ (worst $W)"
	if [ "${D:-1}" != "0" ] && [ "${D:-1}" != "-1" ]; then
		# THE WHOLE REPORT, on failure only: both extents, both image hashes,
		# the wrong-pixel count, the first mismatch coordinate, the bounding box
		# -- and a difference image on disk to look at. A rotated-output defect
		# that has to be re-derived from a single number is how M4F.2C.4d spent
		# two milestones.
		python3 "$PPM" report "$OUTDIR/rr$RR-canon.ppm" "$OUTDIR/$REF-canon.ppm" \
			"$OUTDIR/diff-rr$RR.ppm" | sed 's/^/    /'
		python3 "$PPM" grid "$OUTDIR/rr$RR-canon.ppm" "$OUTDIR/$REF-canon.ppm" 20 |
			sed 's/^/    /'
	fi
	hl_assert "transform $RR renders the same desktop as $REF ($D px)" \
		"$([ "${D:-1}" = "0" ] && echo true || echo false)" true
done

echo
echo "── 2b. the canonicalisation is the right one, by election ────────────"
# EIGHT CANDIDATES, ONE WINNER. If ppm.py's transform were transcribed
# backwards -- 90 where 270 belongs -- the comparisons above would fail loudly
# for a rotation, but a MIRROR pair can be transcribed backwards and still look
# plausible on a fixture whose content happens to be near-symmetric. So each
# capture is canonicalised through every candidate and the expected one must be
# the unique zero. That also proves the fixture is asymmetric enough to tell the
# eight apart at all, which no amount of "0 px" on its own can say.
for RR in 1 2 4 5; do
	case "$RR" in
	1|3|5|7) REF=ref-b ;;
	*)       REF=ref-a ;;
	esac
	ELECT=$(python3 "$PPM" verify "$OUTDIR/rr$RR.ppm" "$OUTDIR/$REF-canon.ppm" "$RR")
	echo "$ELECT" | sed 's/^/    /'
	ZEROS=$(echo "$ELECT" | awk '$2 == "diff" && $3 == 0 { print $1 }' | wc -l)
	WINNER=$(echo "$ELECT" | awk '$2 == "diff" && $3 == 0 { print $1 }' | head -1)
	hl_assert "transform $RR canonicalises correctly under exactly one candidate ($ZEROS)" \
		"$ZEROS" 1
	hl_assert "and that candidate is $RR" "${WINNER:--}" "$RR"
done

echo
echo "── 3. the oracle can fail, at every transform ────────────────────────"
# The hole is in ATTACHMENT coordinates and has to lie inside the attachment at
# every transform, so one rectangle serves all nine cases: 800x600 and 600x800
# both contain 240,180 200x160.
#
# EXACTLY THE HOLE'S AREA, not merely "more than zero". The canonicalisation is
# a bijection, so a rectangle of attachment pixels that is never redrawn must
# show up as the same NUMBER of differing presentation pixels at every
# transform. A count that came out smaller would mean part of the hole was
# redrawn anyway; larger, that something else moved as well. Either way the
# oracle would be measuring something other than the hole.
HOLE="AZ_AVK_DAMAGE_HOLE=240,180,200,160"
HOLE_AREA=32000
capture hole-a   800 600 0 "$HOLE"
capture hole-b   600 800 0 "$HOLE"
for RR in 1 2 3 4 5 6 7; do
	capture "hole$RR" 800 600 "$RR" "$HOLE"
done
read -r D W <<<"$(canon_diff hole-a ref-a)"
echo "  transform 0: a deliberate damage hole changes $D px (worst $W)"
hl_assert "the oracle CATCHES a damage hole at transform 0 ($D px)" \
	"${D:-0}" "$HOLE_AREA"
read -r D W <<<"$(canon_diff hole-b ref-b)"
echo "  transform 0 (600x800): a deliberate damage hole changes $D px (worst $W)"
hl_assert "the oracle CATCHES a damage hole on the tall reference ($D px)" \
	"${D:-0}" "$HOLE_AREA"
for RR in 1 2 3 4 5 6 7; do
	case "$RR" in
	1|3|5|7) REF=ref-b ;;
	*)       REF=ref-a ;;
	esac
	read -r D W <<<"$(canon_diff "hole$RR" "$REF")"
	echo "  transform $RR: a deliberate damage hole changes $D px (worst $W)"
	hl_assert "the oracle CATCHES a damage hole at transform $RR ($D px)" \
		"${D:-0}" "$HOLE_AREA"
done

echo
echo "logs: $OUTDIR"
hl_summary
