#!/usr/bin/env bash
# avk-decoration-cost.sh -- what the 7ms after the blur is actually spent on.
#
# The frame decomposition stops at three terms: pre, blur, post. On an
# overlapping-tag frame post is 7048us against 3467us on an ordinary one, and
# "post" contains client content, shadows, borders, rounded coverage, gradients,
# blending and the final composite all at once. A conclusion about transition
# architecture drawn on top of that bucket is a conclusion about an unread
# number.
#
# ── WHY NOT `shadow { enable 0 }` ────────────────────────────────────────
#
# The obvious A/B is to turn each effect off in the config. It measures the
# wrong thing. Removing a shadow node removes a scene node, which changes the
# damage, which changes every blur source region downstream of it, which changes
# how many chains run. The difference between the two runs is then a geometry
# difference wearing the shadow's name, and it is impossible to tell the two
# apart after the fact.
#
# AZ_AVK_SKIP_DRAW suppresses the vkCmdDraw for one primitive CLASS and changes
# nothing else: same scene, same damage, same chains, same transients, same
# barriers. The delta is fragment cost and can be nothing else. It renders a
# visibly broken desktop, which is correct -- it is a measuring instrument.
#
# ── WHY THE FIXTURE HOLDS STILL ──────────────────────────────────────────
#
# The expensive live frames are the ones where both tags are on screen at once.
# Reproducing that by running an actual tag animation makes every cohort a
# different number of frames with a different chain count, so the cohorts are
# not comparable -- that is exactly the trap that produced a retracted
# cold/warm GPU finding earlier in this milestone.
#
# So the population is reproduced STATICALLY: 2N floating windows, the count an
# overlapping transition puts on screen, animations off, driven by damage_all so
# every cohort renders an identical frame an identical number of times. `--tag`
# additionally runs the real switch as a confirmation that the ranking holds,
# but the attribution comes from the static run.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-decoration-cost"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-decor-$$"
mkdir -p "$OUTDIR"

CYCLES="${CYCLES:-24}"
WARMUP="${WARMUP:-4}"
WINDOWS="${WINDOWS:-6}"
W="${W:-3840}"
H="${H:-2160}"
SCALE="${SCALE:-1.5}"
# A, B, ... H in the brief's order, plus the two background layers, which the
# draw ledger showed are each a full output of fill under an opaque wallpaper.
# "-" is the full production cohort.
COHORTS="${COHORTS:-- shadow border round shadow,border shadow,border,round clear rect clear,rect blur}"

# SERVER-SIDE DECORATION, and the matrix is worthless without it. asteroidz
# draws no border for a client that never negotiates decorations -- it still
# reserves borderpx, so the window looks bordered and the ring is empty. The
# first run of this audit recorded border_draws = 0 against borderpx 4 and would
# have reported borders as free.
export WLBGEFFECT_SSD=1

# Shadows are drawn on FLOATING windows only, so a fixture that leaves its
# clients tiled measures a decoration that was never there. hl_dispatch
# toggle_floating below is not optional.
CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 20; blur_sigma 12; position_y 6 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

KEYS='
 frames:.frames, draws:.draws, chains:.blur_chains,
 maxch:.blur_max_chains_per_frame,
 fb50:.gpu_frame_blur_ns_p50, fb95:.gpu_frame_blur_ns_p95,
 fb_n:.gpu_frame_blur_samples,
 bl50:.gpu_blur_total_ns_p50, post50:.gpu_frame_postblur_ns_p50,
 pre50:.gpu_frame_preblur_ns_p50,
 o_clear:.px_out_clear, o_content:.px_out_content, o_shadow:.px_out_shadow,
 o_shadow_env:.px_out_shadow_env, o_border:.px_out_border,
 o_border_outer:.px_out_border_outer, o_blur:.px_out_blur_comp,
 o_rect:.px_out_rect, o_grad:.px_out_gradient, o_target:.px_out_target,
 p_clear:.px_prefix_clear, p_content:.px_prefix_content,
 p_shadow:.px_prefix_shadow, p_border:.px_prefix_border,
 p_blur:.px_prefix_blur_comp, p_rect:.px_prefix_rect,
 p_grad:.px_prefix_gradient, p_target:.px_prefix_target,
 sh_draws:.shadow_draws, bo_draws:.border_draws, bl_draws:.blur_draws,
 surf:.surfaces,
 waits:.cpu_sync_waits, verr:.validation_errors
'
read_stats() { hl_get "get avk-stats" | jq -r "{$KEYS} | to_entries | \
	map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null; }
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

# cohort SKIPSPEC MODE -> stats line
cohort() {
	local skip="$1" mode="$2"
	local dir="$OUTDIR/${skip//,/+}-$mode"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	HL_ENV="ASTEROIDZ_RENDERER=avk"
	[ "$skip" = "-" ] || HL_ENV="$HL_ENV AZ_AVK_SKIP_DRAW=$skip"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1

	hl_dispatch set_option,animations,0 1
	# ONE AT A TIME, floated and placed before the next is spawned. There is no
	# focus-by-title dispatch, and the only client whose identity is known
	# without one is the one that just mapped -- so the loop has to interleave.
	# Spreading them matters: a stack of identical tiled rectangles has one
	# visible border and would price the class at a sixth of its real coverage.
	local i=0
	while [ "$i" -lt "$WINDOWS" ]; do
		hl_spawn_wlbgeffect "dc$i" 300 "dc$i" >/dev/null
		hl_wait_client_count "$(( i + 1 ))" 160
		hl_dispatch toggle_floating 0.2
		hl_dispatch "move_window,$(( 60 + i * 300 )),$(( 40 + (i % 3) * 260 ))" 0.2
		hl_dispatch resize_window,900,620 0.2
		i=$(( i + 1 ))
	done
	sleep "$WARMUP"
	hl_dispatch damage_all; sleep 1; hl_dispatch damage_all; sleep 1

	hl_dispatch reset_avk_stats
	local c=0
	while [ "$c" -lt "$CYCLES" ]; do
		case "$mode" in
		static) hl_dispatch damage_all ;;
		tag)    if [ $(( c % 2 )) -eq 0 ]; then hl_dispatch view,2
		        else hl_dispatch view,1; fi ;;
		esac
		sleep 0.25
		c=$(( c + 1 ))
	done
	sleep 1
	local st; st="$(read_stats)"
	hl_stop >/dev/null 2>&1
	echo "$st"
}

MODE="static"
[ "${1:-}" = "--tag" ] && MODE="tag"

echo "══ decoration cost ══ ${W}x${H} scale $SCALE windows=$WINDOWS cycles=$CYCLES mode=$MODE"
echo

BASE=""
printf "  %-24s %8s %8s %8s %8s   %s\n" cohort frame_us blur_us post_us delta_us note
for sp in $COHORTS; do
	R="$(cohort "$sp" "$MODE")"
	fb="$(v fb50 "$R")"; bl="$(v bl50 "$R")"; po="$(v post50 "$R")"
	[ -n "$BASE" ] || BASE="$fb"
	if [ -z "$fb" ] || [ "$fb" = "null" ]; then
		printf "  %-24s NO BLUR-FRAME SAMPLES -- cohort invalid\n" "$sp"
		continue
	fi
	note=""
	[ "$(v waits "$R")" = "0" ] || note="$note cpu_waits"
	[ "$(v verr "$R")" = "0" ] || note="$note validation"
	[ "$(v chains "$R")" != "0" ] || note="$note NO_BLUR"
	# The premise every cohort below depends on: the fixture actually drew the
	# decorations whose cost it is pricing. A cohort that scores well because
	# no shadow node ever existed is the failure mode here.
	[ "$sp" = "-" ] && { [ "$(v sh_draws "$R")" != "0" ] || note="$note NO_SHADOWS"; \
		[ "$(v bo_draws "$R")" != "0" ] || note="$note NO_BORDERS"; }
	awk -v s="$sp" -v fb="$fb" -v bl="$bl" -v po="$po" -v b="$BASE" -v n="$note" \
	'BEGIN{ printf "  %-24s %8.0f %8.0f %8.0f %8.0f  %s\n",
		s, fb/1000, bl/1000, po/1000, (fb-b)/1000, n }'
	echo "$sp|$R" >> "$OUTDIR/rows"
done

echo
echo "── fragment area, production cohort ──"
echo "   OUT = the output composite.  PREFIX = scene replay into blur capture targets."
echo "   A decoration appearing in both is rasterised more than once per frame."
R="$(grep '^-|' "$OUTDIR/rows" | head -1 | cut -d'|' -f2-)"
if [ -n "$R" ]; then
	awk -v ocl="$(v o_clear "$R")" -v oco="$(v o_content "$R")" \
	    -v osh="$(v o_shadow "$R")" -v ose="$(v o_shadow_env "$R")" \
	    -v obo="$(v o_border "$R")" -v obx="$(v o_border_outer "$R")" \
	    -v obu="$(v o_blur "$R")" -v ore="$(v o_rect "$R")" \
	    -v ogr="$(v o_grad "$R")" -v oou="$(v o_target "$R")" \
	    -v pcl="$(v p_clear "$R")" -v pco="$(v p_content "$R")" \
	    -v psh="$(v p_shadow "$R")" -v pbo="$(v p_border "$R")" \
	    -v pbu="$(v p_blur "$R")" -v pre="$(v p_rect "$R")" \
	    -v pgr="$(v p_grad "$R")" -v pou="$(v p_target "$R")" \
	'function mp(x) { return x/1e6 }
	BEGIN{
		if (oou+0 == 0) { print "  no output segment composed -- nothing to divide by"; exit }
		otot = ocl+oco+osh+obo+obu+ore;
		ptot = pcl+pco+psh+pbo+pbu+pre;
		printf "  %-16s %11s %9s %11s %11s\n",
			"class", "OUT Mpx", "x output", "PREFIX Mpx", "prefix/out";
		fmt = "  %-16s %11.1f %9.3f %11.1f %11s\n";
		split("clear content shadow border blur_composite other_rect", nm, " ");
		o[1]=ocl; o[2]=oco; o[3]=osh; o[4]=obo; o[5]=obu; o[6]=ore;
		p[1]=pcl; p[2]=pco; p[3]=psh; p[4]=pbo; p[5]=pbu; p[6]=pre;
		for (i = 1; i <= 6; i++) {
			r = (o[i]+0 > 0) ? sprintf("%.3f", p[i]/o[i]) : "-";
			printf fmt, nm[i], mp(o[i]), o[i]/oou, mp(p[i]), r;
		}
		printf fmt, "gradient(of)", mp(ogr), ogr/oou, mp(pgr), "-";
		printf "  %-16s %11.1f %9.3f %11.1f %11.3f  TOTAL\n",
			"", mp(otot), otot/oou, mp(ptot), (otot>0 ? ptot/otot : 0);
		printf "  prefix target area %.1f Mpx against an output of %.1f Mpx\n",
			mp(pou), mp(oou);
		print "";
		# The two geometry questions the brief asks, as ratios.
		if (ose+0 > 0)
			printf "  shadow  drawn/envelope   %.3f   (1.000 = the caster is not cut out)\n", osh/ose;
		else
			print  "  shadow  drawn/envelope   n/a -- NO SHADOW WAS DRAWN, this row proves nothing";
		if (obx+0 > 0)
			printf "  border  drawn/outer rect %.3f   (1.000 = the ring shades the whole window)\n", obo/obx;
		else
			print  "  border  drawn/outer rect n/a -- no border carried an interior cut-out;\n" \
			       "                               any border drawn is in `other_rect` as a FULL rectangle";
	}'
fi

echo
echo "logs: $OUTDIR"
