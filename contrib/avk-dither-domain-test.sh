#!/usr/bin/env bash
# avk-dither-domain-test.sh — which raster the dither noise is anchored to.
#
# M4F.2C.4e. The transform pixel oracle runs with AZ_SHADOW_DITHER_AMP=0, and
# the reason is a measurement: with dither on, the same logical desktop at 0 and
# at 180 degrees differed by 134389 pixels. That number was explained away as
# "device-position dither" and subtracted from the fixture, which is the wrong
# shape of answer -- a 134389-pixel population that nothing asserts on is a
# population that could change for a different reason tomorrow and still be
# called dither.
#
# So this file pins the contract instead.
#
# ── THE CONTRACT ──────────────────────────────────────────────────────────
#
# The dither field is a pure function of the ATTACHMENT pixel: az_dither_alpha()
# samples at az_frag_global(), which is gl_FragCoord plus the target's origin --
# physical pixels of the buffer being scanned out, with the regional-target
# offset applied so that a transient does not phase-shift it. There is no frame
# counter, no clock and no animation phase in it.
#
# Three consequences, and each is asserted below rather than described:
#
#   1. A PARTIAL REDRAW EQUALS A FULL ONE, dither included. The noise cannot
#      depend on what was damaged, so a repainted region can never be out of
#      step with the region beside it.
#
#   2. THE FIELD IS ANCHORED TO THE DEVICE GRID, NOT TO THE DESKTOP. Rotating
#      the output rotates the grid underneath the picture, so the same logical
#      desktop at two transforms MUST differ once the captures are
#      canonicalised. If it did not, the noise would be following the desktop
#      around -- which is the failure mode where a window dragged across the
#      screen carries its own texture with it.
#
#   3. AND THE DIFFERENCE IS DITHER-SIZED. Sub-step by construction: the
#      perturbation is a fraction of one 8-bit code. A difference of the right
#      SIZE but the wrong MAGNITUDE is geometry, not dither.
#
# Assertion 2 is the interesting one: it is the same measurement that was
# previously used to excuse a fixture, turned into the thing that would fail if
# the domain ever changed.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-dither-domain"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-dither-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

# SHADOWS ON, because the shadow is the only thing that dithers: it is the one
# effect quantised straight into an 8-bit attachment from an analytic ramp. A
# fixture with no shadow in it measures the dither of nothing at all, which is
# how a "dither is stable" assertion passes on a build with no dither.
SHADOWED="border_radius 12
effects { shadow { enable 1 }; blur { enable 0 } }"

AMP="${AMP:-}"   # empty = the shipped amplitude

# capture NAME TRANSFORM [EXTRA_ENV] -> $OUTDIR/NAME.ppm + -canon.ppm
capture() {
	# SEPARATE STATEMENTS: `local a="$1" b="...$a"` declares every name before
	# it assigns any of them, so under `set -u` the second initialiser reads an
	# unset variable and the function dies on its first line.
	local name="$1"
	local rr="$2"
	local extra="${3:-}"
	local cdir="$OUTDIR/cap-$name"
	mkdir -p "$cdir"
	HL_WIDTH=800 HL_HEIGHT=600 HL_RR1="$rr"
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_AVK_CAPTURE_DIR=$cdir $extra"
	[ -n "$AMP" ] && HL_ENV="$HL_ENV AZ_SHADOW_DITHER_AMP=$AMP"
	export HL_WIDTH HL_HEIGHT HL_RR1 HL_ENV
	hl_start "$SHADOWED"
	hl_spawn_wlbgeffect one 300 "w1-$name" ffb03020 >/dev/null
	hl_wait_client_count 1 60
	hl_spawn_wlbgeffect two 300 "w2-$name" ff30b040 >/dev/null
	hl_wait_client_count 2 60
	sleep 3
	hl_dispatch toggle_floating
	sleep 2
	hl_sync_pointer_extent && hl_move 300 400
	sleep 2
	hl_dispatch capture_output
	sleep 2
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	# ── the partial-vs-full pair, from the SAME compositor ───────────────
	# Captured second, after damage_all, so the two differ only in HOW the
	# frame was produced: one incrementally, one whole.
	hl_dispatch damage_all
	sleep 2
	hl_dispatch capture_output
	sleep 2
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name-full.ppm" 2>/dev/null || true
	hl_stop
	for f in "$name" "$name-full"; do
		[ -s "$OUTDIR/$f.ppm" ] && python3 "$PPM" canon "$OUTDIR/$f.ppm" \
			"$OUTDIR/$f-canon.ppm" "$rr" >/dev/null
	done
}

echo
echo "── 0. the premise: this fixture actually dithers ─────────────────────"
capture d0    0
capture flat0 0 "AZ_SHADOW_DITHER_AMP=0"
read -r DD DW <<<"$(python3 "$PPM" diff "$OUTDIR/d0.ppm" "$OUTDIR/flat0.ppm")"
echo "  dither on vs dither off, same transform: $DD px differ (worst $DW)"
hl_assert "PREMISE: the fixture produces dither at all ($DD px)" \
	"$([ "${DD:-0}" -gt 1000 ] && echo true || echo false)" true
hl_assert "PREMISE: and it is sub-step (worst $DW codes)" \
	"$([ "${DW:-99}" -le 8 ] && echo true || echo false)" true

echo
echo "── 1. a partial redraw equals a full one, dither included ────────────"
for N in d0 flat0; do
	read -r D W <<<"$(python3 "$PPM" diff "$OUTDIR/$N.ppm" "$OUTDIR/$N-full.ppm")"
	echo "  $N: incremental vs forced-full $D px (worst $W)"
	hl_assert "$N: a forced full repaint changes nothing ($D px)" \
		"$([ "${D:-1}" = "0" ] && echo true || echo false)" true
done

echo
echo "── 2. the field is anchored to the ATTACHMENT, not to the desktop ────"
# 180 degrees, because it keeps the extents and reverses both axes: the same
# logical pixel lands on a different DEVICE pixel, and on nothing else. If the
# canonicalised captures agreed, the noise would be following the desktop.
capture d180    2
capture flat180 2 "AZ_SHADOW_DITHER_AMP=0"
read -r FD FW <<<"$(python3 "$PPM" diff "$OUTDIR/flat0-canon.ppm" \
	"$OUTDIR/flat180-canon.ppm")"
echo "  with NO dither, 0 vs 180 differs $FD px (worst $FW)"
# THE GEOMETRY FLOOR, and it is NOT zero -- which is the whole reason the
# transform pixel oracle uses a flat fixture and this one cannot.
#
# This fixture must contain a shadow, because a shadow is the only thing that
# dithers. A shadow is also evaluated analytically in ATTACHMENT pixels, so at
# 180 degrees its antialiased edge samples at mirrored positions and rounds a
# code or two differently. Measured in M4F.2C.4d on a rounded+shadowed fixture
# with dither off: 1247 px, 1163 of them differing by exactly 1. So the control
# here is not "zero" -- it is "small, and sub-step", and it exists to be
# SUBTRACTED from the measurement below rather than to pass on its own.
# TWELVE, NOT EIGHT, AND THE CHANGE IS MEASURED RATHER THAN CONVENIENT.
#
# M6B/D5 made Path A the default, so a shadow's antialiased edge is now blended
# in LINEAR LIGHT. Its mirrored samples at 180 degrees therefore round slightly
# differently and the geometry floor moved by exactly one code:
#
#     AZ_M5_PATH_A=0   worst 8   (this threshold's original measurement)
#     default (on)     worst 9
#
# Confirmed by running this fixture both ways; the PIXEL COUNT assertion below
# is unchanged and still passes, so this is one outlier rounding differently
# rather than a broad shift. The bound exists to catch GEOMETRY MOVING
# WHOLESALE -- hundreds of codes, a picture in the wrong place -- and 12 leaves
# three codes of headroom so a further one-code drift does not reopen this.
hl_assert "CONTROL: the geometry-only residual is sub-step (worst $FW codes)" \
	"$([ "${FW:-99}" -le 12 ] && echo true || echo false)" true
hl_assert "CONTROL: and it is a small part of the frame ($FD px of 480000)" \
	"$([ "${FD:-999999}" -lt 20000 ] && echo true || echo false)" true

read -r RD RW <<<"$(python3 "$PPM" diff "$OUTDIR/d0-canon.ppm" \
	"$OUTDIR/d180-canon.ppm")"
echo "  with dither, 0 vs 180 differs $RD px (worst $RW)"
# THE DOMAIN, AS A DISCRIMINATOR, once the floor is accounted for.
#
#     presentation-anchored field  ->  RD == FD, the geometry floor and nothing
#                                      more: the noise would land on the same
#                                      logical pixel at both transforms
#     attachment-anchored field    ->  RD >> FD, because 180 degrees maps every
#                                      logical pixel onto a different DEVICE
#                                      pixel and therefore a different sample
#
# Measured: 2672 px of floor against 22747 with dither, on a fixture where
# dither touches 16598 px in the first place. The two hypotheses are not close.
EXCESS=$(( ${RD:-0} - ${FD:-0} ))
echo "  dither accounts for $EXCESS px beyond the geometry floor"
hl_assert "the dither field rotates WITH the device grid ($EXCESS px beyond the floor)" \
	"$([ "$EXCESS" -gt 5000 ] && echo true || echo false)" true
# AND IT IS STILL DITHER. A difference of the right SIZE but the wrong
# MAGNITUDE would be geometry moving, which is what this excludes.
# Same re-baseline, same reason: this residual contains the geometry floor
# above, so it cannot be tighter than it.
hl_assert "and the difference is sub-step, so it is dither (worst $RW codes)" \
	"$([ "${RW:-99}" -le 12 ] && echo true || echo false)" true

echo
echo "── 3. and it is stable at a rotated transform too ────────────────────"
for N in d180 flat180; do
	read -r D W <<<"$(python3 "$PPM" diff "$OUTDIR/$N.ppm" "$OUTDIR/$N-full.ppm")"
	echo "  $N: incremental vs forced-full $D px (worst $W)"
	hl_assert "$N at 180: a forced full repaint changes nothing ($D px)" \
		"$([ "${D:-1}" = "0" ] && echo true || echo false)" true
done

echo
echo "logs: $OUTDIR"
hl_summary
