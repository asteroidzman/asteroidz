#!/usr/bin/env bash
# avk-blur-required-test.sh — is the derived required region actually required?
#
# M4F.2D. avk_blur_work_of() derives, per blur pass, the region that pass would
# have to render for the demanded result to be correct. That derivation is the
# DENOMINATOR of the per-level scissor decision, and a denominator nobody has
# tried to break is an opinion.
#
# ── THE EXPERIMENT ────────────────────────────────────────────────────────
#
# AZ_BLUR_REQ_SCISSOR=<pass>[,<shrink>] scissors one blur pass to its derived
# required region. AZ_TRANSIENT_POISON=1 fills every transient with garbage
# first. So everything that pass does NOT render is poison, and the final image
# says whether that mattered:
#
#   shrink=0   the whole derived region is rendered.
#              The frame must be IDENTICAL to the unscissored one.
#              => the region is SUFFICIENT.
#
#   shrink=N   N pixels are removed from each edge of the derived region, and
#              poison takes their place.
#              The frame must DIFFER.
#              => the region is NECESSARY at that boundary.
#
# Either half alone proves nothing. A region that is merely large enough passes
# the first; a region that is arbitrary passes neither. Both together are what
# lets the report say "required" rather than "the number we happened to
# compute".
#
# ── WHY A CHECKERBOARD ────────────────────────────────────────────────────
#
# A blur of a flat field is that flat field, so on a flat backdrop a missing
# strip of a lower level can be completely invisible -- and the falsifier would
# report "no difference" for a region that was genuinely too small. The
# wallpaper here is a ONE-PIXEL checkerboard: the highest spatial frequency the
# raster can hold, and the content most sensitive to a missing tap.
#
# PRODUCTION IS UNTOUCHED. With AZ_BLUR_REQ_SCISSOR unset every pass renders
# its whole level exactly as it always has. M4F.2D has not implemented regional
# filter execution; this is how the decision's input is checked.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-required"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-required-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

LEVELS="${LEVELS:-3}"
RADIUS="${RADIUS:-5}"
SHRINK="${SHRINK:-1}"
PASSES="${PASSES:-down1 up0 up1}"

CFG="border_radius 0
effects { shadow { enable 0 }
  blur { enable 1; passes $LEVELS; radius $RADIUS } }"

# A ONE-PIXEL CHECKERBOARD, written before hl_start so the harness uses it
# instead of generating its flat grey. Flat grey is the worst possible backdrop
# for this test: a blur of it is itself, so a missing strip is invisible.
make_checker() {
	convert -size 2x2 xc:black -fill white \
		-draw 'point 0,0' -draw 'point 1,1' \
		-write mpr:tile +delete \
		-size 800x600 tile:mpr:tile "$OUTDIR/wallpaper.png" 2>/dev/null
	[ -s "$OUTDIR/wallpaper.png" ]
}

# shot NAME [EXTRA_ENV] -> capture + the stats that say the fixture was real
shot() {
	local name="$1"
	local extra="${2:-}"
	local cdir="$OUTDIR/cap-$name"
	mkdir -p "$cdir"
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_TRANSIENT_POISON=1 \
AZ_AVK_CAPTURE_DIR=$cdir $extra"
	export HL_ENV
	hl_start "$CFG"
	hl_spawn_wlbgeffect blur 120 "w-$name" >/dev/null
	hl_wait_client_count 1 60
	sleep 4
	hl_dispatch reset_avk_stats
	hl_dispatch damage_all
	sleep 2
	hl_dispatch capture_output
	sleep 2
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	# ONE LINE. A backslash-newline inside a single-quoted jq program is a
	# literal backslash, not a continuation: the first version of this produced
	# an empty stats line and every premise assertion failed for a reason that
	# had nothing to do with the fixture.
	STATS="$(hl_get "get avk-stats" | jq -r '{c:.blur_chains, p:.blur_processed_pixels, r:.blur_required_work_pixels, s:.blur_result_pixels, w:.cpu_sync_waits} | "chains=\(.c) proc=\(.p) req=\(.r) res=\(.s) waits=\(.w)"' 2>/dev/null)"
	cp -f "$HL_OUTDIR/state/asteroidz/asteroidz.log" "$OUTDIR/$name.log" \
		2>/dev/null || true
	hl_stop
	echo "$STATS"
}

f() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

echo
echo "── 0. the fixture: high-frequency backdrop, real blur ────────────────"
hl_assert "PREMISE: a checkerboard wallpaper was generated" \
	"$(make_checker && echo true || echo false)" true

REF="$(shot ref)"
echo "  reference (no scissor): $REF"
hl_assert "PREMISE: the fixture actually blurred" \
	"$([ "$(f chains "$REF")" -gt 0 ] 2>/dev/null && echo true || echo false)" true
hl_assert "PREMISE: required work was derived" \
	"$([ "$(f req "$REF")" -gt 0 ] 2>/dev/null && echo true || echo false)" true
hl_assert "PREMISE: actual work >= required work" \
	"$([ "$(f proc "$REF")" -ge "$(f req "$REF")" ] 2>/dev/null && echo true \
		|| echo false)" true
hl_assert "PREMISE: no CPU waits" "$(f waits "$REF")" 0

for P in $PASSES; do
	echo
	echo "── $P ────────────────────────────────────────────────────────────────"
	SUF="$(shot "suf-$P" "AZ_BLUR_REQ_SCISSOR=$P,0")"
	read -r D W <<<"$(python3 "$PPM" diff "$OUTDIR/suf-$P.ppm" \
		"$OUTDIR/ref.ppm" 2>/dev/null || echo "-1 -1")"
	echo "  rendering ONLY the derived region: $D px differ (worst $W)"
	hl_assert "$P: the derived region is SUFFICIENT ($D px)" \
		"$([ "${D:-1}" = "0" ] && echo true || echo false)" true

	NEC="$(shot "nec-$P" "AZ_BLUR_REQ_SCISSOR=$P,$SHRINK")"
	read -r ND NW <<<"$(python3 "$PPM" diff "$OUTDIR/nec-$P.ppm" \
		"$OUTDIR/ref.ppm" 2>/dev/null || echo "-1 -1")"
	BB="$(python3 "$PPM" bbox "$OUTDIR/nec-$P.ppm" "$OUTDIR/ref.ppm" \
		2>/dev/null || echo '- - - -')"
	echo "  shrunk by $SHRINK px per edge:      $ND px differ (worst $NW) bbox $BB"
	# NECESSARY, not merely sufficient. If this passes, the derived region has
	# slack at this boundary and the honest word for it is CONSERVATIVE, not
	# minimal -- which is what the report must then say.
	hl_assert "$P: the derived region is NECESSARY -- shrinking it breaks the frame ($ND px)" \
		"$([ "${ND:-0}" -gt 0 ] 2>/dev/null && echo true || echo false)" true
done

echo
echo "logs: $OUTDIR"
hl_summary
