#!/usr/bin/env bash
# avk-opaque-noblend-ab.sh -- is blending an opaque draw costing anything?
#
# ── THE ONE CANDIDATE THE POST AUDIT FOUND ───────────────────────────────
#
# AZ_BLEND_REPLACE existed in AVK for exactly two consumers, the blur's down and
# up passes. Every content and rect draw in the output pass ran with
# blendEnable = VK_TRUE, INCLUDING the ones the renderer already knows are
# opaque -- it computes that predicate every frame for occlusion culling.
#
# AZ_AVK_OPAQUE_NOBLEND=1 sends draws that are opaque over their WHOLE footprint
# to a blend-free pipeline. That is bit-exact rather than an approximation:
# premultiplied OVER is (ONE, ONE_MINUS_SRC_ALPHA), so at alpha 1 the
# destination factor is zero and the blend already resolves to src. What it
# removes is the ROP's read of a destination it was about to discard.
#
# ── WHAT THIS DECIDES ────────────────────────────────────────────────────
#
# The keep threshold for a simple optimisation is a ~5% live frame improvement
# with correctness unchanged. This measures the share; the pixel oracle checks
# the correctness half; and if the share is not there the experiment is REJECTED
# and the code comes out. It is not kept because it is tidy.
#
# ── WHY THE PIXEL ORACLE IS THE STRICT KIND ──────────────────────────────
#
# Not "close enough": ZERO differing pixels. A blend-free write of a partially
# covered fragment is a hard edge, and a hard edge one pixel wide down the side
# of a rounded window is exactly the artefact a tolerance would absorb.
#
# HEADLESS ABSOLUTE MICROSECONDS ARE NOT BUDGET EVIDENCE -- the fixture GPU
# idles near 50MHz. The SHARE of post is what transfers.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-opaque-noblend"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-noblend-$$"
mkdir -p "$OUTDIR"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

W="${W:-3840}"; H="${H:-2160}"; SCALE="${SCALE:-1.5}"
WINDOWS="${WINDOWS:-6}"; CYCLES="${CYCLES:-24}"

CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 20; blur 12; only-floating 0; tiled-scale 1.0
    blur-background 1; blur-background-strength 0.55 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.0; brightness 0.9; contrast 0.9; saturation 1.2 } } }
layout { titlebar { enable 0 } }"

# ── THE TITLEBAR IS OFF AND THAT IS NOT COSMETIC ─────────────────────────
#
# contrib/lib/headless.sh's base config sets `titlebar { enable 1 }`, and the
# titlebar's TEXT does not render identically from one run to the next. Two runs
# of this fixture with byte-identical environments differed by 2,574 pixels in a
# 429x6 strip at (2565,60) -- the title text present in one capture and absent
# from the other, so the strip read as the theme blue underneath.
#
# That is enough to fail a zero-pixel oracle, and it did: the first run of this
# A/B reported "skipping the blend is not bit-exact" and the difference had
# nothing to do with blending. With the titlebar off, two identical runs differ
# by 0 px. Any fixture here that compares captures and leaves the default
# titlebar on is carrying ~2,574 px of noise under its oracle.

export WLBGEFFECT_SSD=1

JQ='{frames:.frames, nb:.opaque_noblend_draws, surfaces:.surfaces,
     rects:.rects, content:.px_out_content, target:.px_out_target,
     f50:.gpu_frame_ns_p50, blur50:.gpu_blur_total_ns_p50,
     post50:.gpu_frame_postblur_ns_p50,
     verr:.validation_errors, waits:.cpu_sync_waits}'
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
us() { local x; x="$(v "$1" "$2")"; case "$x" in ''|null) echo 0 ;;
	*) echo $(( x / 1000 )) ;; esac; }
STATS=""

run() { # run NAME [EXTRA_ENV...]
	local name="$1"; shift
	local dir="$OUTDIR/$name" cdir="$OUTDIR/$name-cap"
	mkdir -p "$dir" "$cdir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_SHADOW_DITHER_AMP=0 AZ_AVK_CAPTURE_DIR=$cdir $*"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1
	local i=0
	while [ "$i" -lt "$WINDOWS" ]; do
		hl_spawn_wlbgeffect "nb$i" 300 "nb$i" >/dev/null
		hl_wait_client_count "$(( i + 1 ))" 200
		i=$(( i + 1 ))
	done
	sleep 3
	hl_dispatch reset_avk_stats 1
	# STATIC, DAMAGE-DRIVEN. Both arms render the same scene the same number of
	# times with the same chains -- the only difference is which pipeline shades
	# an opaque quad. A moving fixture would make the arms different populations,
	# which is the trap that has produced two retracted results in this project.
	local c=0
	while [ "$c" -lt "$CYCLES" ]; do
		hl_dispatch damage_all
		sleep 0.3
		c=$(( c + 1 ))
	done
	sleep 1
	STATS="$(hl_get "get avk-stats" | jq -r "$JQ | to_entries |
		map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null)"
	hl_dispatch capture_output 1
	# WAIT FOR THE FILE, DO NOT SLEEP AT IT.
	#
	# A 3840x2160 PPM is 24.9 MB and the compositor is still writing it a second
	# after the dispatch returns. Copying on a fixed sleep produced a TRUNCATED
	# capture -- ppm.py then refuses to read it and diffpx returns -1, which
	# reads in the output as "the pixel oracle failed" rather than "there was no
	# picture to compare". The expected size is arithmetic, so wait for it.
	local want=$(( W * H * 3 ))
	local t=0
	while [ "$t" -lt 60 ]; do
		local have
		have="$(stat -c %s "$cdir/HEADLESS-1.ppm" 2>/dev/null || echo 0)"
		[ "$have" -ge "$want" ] && break
		sleep 0.5
		t=$(( t + 1 ))
	done
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	# AND ASSERT IT ARRIVED. A missing or short capture must not be silently
	# carried into the oracle below as a -1.
	local got
	got="$(stat -c %s "$OUTDIR/$name.ppm" 2>/dev/null || echo 0)"
	if [ "$got" -lt "$want" ]; then
		echo "  CAPTURE INCOMPLETE for $name: $got bytes, wanted >= $want" >&2
	fi
	hl_stop >/dev/null 2>&1
}
diffpx() { python3 "$PPM" diff "$OUTDIR/$1.ppm" "$OUTDIR/$2.ppm" 2>/dev/null \
	|| echo "-1 -1"; }

echo "══ opaque draws: blend vs straight write ══ ${W}x${H} scale $SCALE windows=$WINDOWS"
echo

run blend
A="$STATS"
# THE CONTROL ARM: the same build, the same env, again. Its diff against `blend`
# is the fixture's own noise floor, and the cross-arm comparison below is only
# worth reading if that floor is zero. Costs one compositor run and is the
# difference between an oracle and a coin.
run blend2
run noblend AZ_AVK_OPAQUE_NOBLEND=1
B="$STATS"

echo "  arm       frames  noblend_draws  surfaces  rects  frame_us  blur_us  post_us"
printf "  %-9s %6s %14s %9s %6s %9s %8s %8s\n" blend \
	"$(v frames "$A")" "$(v nb "$A")" "$(v surfaces "$A")" "$(v rects "$A")" \
	"$(us f50 "$A")" "$(us blur50 "$A")" "$(us post50 "$A")"
printf "  %-9s %6s %14s %9s %6s %9s %8s %8s\n" noblend \
	"$(v frames "$B")" "$(v nb "$B")" "$(v surfaces "$B")" "$(v rects "$B")" \
	"$(us f50 "$B")" "$(us blur50 "$B")" "$(us post50 "$B")"
echo

# ── THE PREMISE, BOTH HALVES ─────────────────────────────────────────────
# Off must match nothing and on must match something. Without the second, a
# timing delta of zero is what a predicate that never fired would also report --
# and it would be written up as "blending is free".
hl_assert "PREMISE: with the experiment off, no draw skips blending" "$(v nb "$A")" 0
hl_assert_true "PREMISE: with it on, some draws DO skip blending ($(v nb "$B"))" \
	"$([ "$(v nb "$B")" -gt 0 ] && echo true || echo false)"
# Same scene, same work. If the arms drew different numbers of primitives they
# are not comparable and the delta below is a scene difference.
hl_assert "the two arms drew the same surfaces" "$(v surfaces "$A")" "$(v surfaces "$B")"
hl_assert "and the same rects" "$(v rects "$A")" "$(v rects "$B")"

echo
echo "── CORRECTNESS: it must not change a pixel ───────────────────────────"
read -r DC MC <<<"$(diffpx blend blend2)"
echo "  NOISE FLOOR (same build twice): $DC px differ (worst channel $MC)"
hl_assert "CONTROL: this fixture is deterministic" "$DC" 0
read -r D M <<<"$(diffpx blend noblend)"
echo "  blend vs noblend: $D px differ (worst channel $M)"
hl_assert "BIT-EXACT: skipping the blend changes nothing" "$D" 0

echo
echo "── IS IT WORTH ANYTHING? ─────────────────────────────────────────────"
PA="$(us post50 "$A")"; PB="$(us post50 "$B")"
FA="$(us f50 "$A")";    FB="$(us f50 "$B")"
awk -v pa="$PA" -v pb="$PB" -v fa="$FA" -v fb="$FB" -v n="$(v nb "$B")" \
    -v f="$(v frames "$B")" 'BEGIN{
	if (pa+0==0 || fa+0==0) { print "  no GPU timing on this device"; exit }
	printf "  post  %d -> %d us  (%+.1f%%)\n", pa, pb, 100*(pb-pa)/pa;
	printf "  frame %d -> %d us  (%+.1f%%)\n", fa, fb, 100*(fb-fa)/fa;
	printf "  %.1f blend-free draws per frame\n", (f>0 ? n/f : 0);
	d = 100*(fa-fb)/fa;
	printf "\n  KEEP THRESHOLD for a simple optimisation is ~5%% of the frame.\n";
	printf "  measured: %+.1f%%  -> %s\n", d, (d >= 5 ? "KEEP" : "REJECT");
}'

echo
echo "── soundness ─────────────────────────────────────────────────────────"
hl_assert "blend: validation errors"   "$(v verr "$A")" 0
hl_assert "noblend: validation errors" "$(v verr "$B")" 0

echo
echo "logs: $OUTDIR"
hl_summary
