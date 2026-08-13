#!/usr/bin/env bash
# avk-frame-phase-profile.sh -- where does a blur-bearing frame actually go?
#
# The performance question is no longer "is the blur slow". It is "which part
# of the 7-10ms frame is removable", and that cannot be answered from a
# decomposition whose largest term is unnamed. At one chain the measured split
# was gpu_blur 1032us inside a gpu_frameBLUR of 2370us -- 56% of the frame with
# no attribution at all.
#
# ── THE PARTITION ────────────────────────────────────────────────────────
#
#   PRE      FRAME_BEGIN      -> BLUR_BEGIN      everything before any blur
#   PREFIX   BLUR_BEGIN       -> PREFIX_END      chain 1's scene-prefix replay
#   DOWN     PREFIX_END       -> DOWN_END        chain 1's downsample pyramid
#   REST     DOWN_END         -> BLUR_END        chain 1's upsamples, plus
#                                                chains 2..N entire
#   POST     BLUR_END         -> FRAME_END       final composite and after
#
# PRE + BLUR + POST == the frame, exactly. Within BLUR, PREFIX + DOWN + REST
# == blur_total, exactly.
#
# REST IS NOT "UP". At N > 1 the PREFIX_END and DOWN_END marks belong to the
# FIRST chain only, so everything after chain 1's downsamples -- including
# every later chain's prefix, pyramid and upsamples -- lands in one bucket.
# Reading it as an upsample cost is precisely how a prefix-replay problem gets
# mistaken for a filter problem. gpu_blur_up and gpu_blur_up0 are collected
# only on single-chain frames and are reported only for N=1.
#
# ── WHY THE FIXTURES ARE WHAT THEY ARE ───────────────────────────────────
#
# Each workload forces a bounded, repeatable amount of work with animations
# OFF, because an animation runs for a wall-clock duration at whatever rate the
# machine manages and so changes the number of chains between runs -- which
# changes the denominator and makes two rows incomparable.
#
# SCALE 1.5 AND A 4K-CLASS OUTPUT are the point. The live budget is 6.944ms at
# 3840x2160; a 1920x1080 fixture at scale 1 measures a quarter of the fill and
# would rank the phases by their small-output behaviour.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-frame-phase"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-phase-$$"
mkdir -p "$OUTDIR"

CYCLES="${CYCLES:-20}"
WARMUP="${WARMUP:-4}"
LEVELS="${LEVELS:-2}"
RADIUS="${RADIUS:-6}"
WORKLOADS="${WORKLOADS:-one five tag move resize}"
# 3840x2160 at scale 1.5 -> 2560x1440 logical, the live DP-1 geometry.
W="${W:-3840}"
H="${H:-2160}"
SCALE="${SCALE:-1.5}"

CFG="border_radius 9
borderpx 2
effects { shadow { enable 0 }
  blur { enable 1; optimized 1; passes $LEVELS; radius $RADIUS
    params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

KEYS='
 frames:.frames, blurframes:.cohort_blur_frames, chains:.blur_chains,
 maxch:.blur_max_chains_per_frame,
 pre50:.gpu_frame_preblur_ns_p50, pre95:.gpu_frame_preblur_ns_p95,
 pre_n:.gpu_frame_preblur_samples,
 px50:.gpu_blur_prefix_ns_p50, px95:.gpu_blur_prefix_ns_p95,
 dn50:.gpu_blur_down_ns_p50, dn95:.gpu_blur_down_ns_p95,
 up50:.gpu_blur_up_ns_p50, up0_50:.gpu_blur_up0_ns_p50, up_n:.gpu_blur_up_samples,
 bl50:.gpu_blur_total_ns_p50, bl95:.gpu_blur_total_ns_p95,
 bl99:.gpu_blur_total_ns_p99,
 post50:.gpu_frame_postblur_ns_p50, post95:.gpu_frame_postblur_ns_p95,
 fb50:.gpu_frame_blur_ns_p50, fb95:.gpu_frame_blur_ns_p95,
 fb99:.gpu_frame_blur_ns_p99, fb_n:.gpu_frame_blur_samples,
 falls:.blur_damage_fallbacks, dmg50:.damage_ratio_p50, dmg95:.damage_ratio_p95,
 proc:.blur_processed_pixels, prefpx:.blur_prefix_pixels,
 waits:.cpu_sync_waits, fb_frames:.fallback_frames, verr:.validation_errors
'
read_stats() { hl_get "get avk-stats" | jq -r "{$KEYS} | to_entries | \
	map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null; }
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

segment() { # segment NAME NWINDOWS WORKLOAD
	local name="$1" n="$2" work="$3"
	local dir="$OUTDIR/$name"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	HL_ENV="ASTEROIDZ_RENDERER=avk"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1
	local i=0
	while [ "$i" -lt "$n" ]; do
		hl_spawn_wlbgeffect "ph$i" 300 "ph$i-$name" >/dev/null
		i=$(( i + 1 ))
	done
	hl_wait_client_count "$n" 120
	hl_dispatch set_option,animations,0 1
	[ "$work" = move ] || [ "$work" = resize ] && hl_dispatch toggle_floating 1
	sleep "$WARMUP"
	hl_dispatch damage_all; sleep 1; hl_dispatch damage_all; sleep 1

	hl_dispatch reset_avk_stats
	local c=0
	while [ "$c" -lt "$CYCLES" ]; do
		case "$work" in
		damage) hl_dispatch damage_all ;;
		tag)    if [ $(( c % 2 )) -eq 0 ]; then hl_dispatch view,2
		        else hl_dispatch view,1; fi ;;
		move)   hl_dispatch "move_window,$(( 200 + (c % 6) * 120 )),$(( 150 + (c % 4) * 90 ))" ;;
		resize) if [ $(( c % 2 )) -eq 0 ]; then hl_dispatch resize_window,1400,900
		        else hl_dispatch resize_window,700,500; fi ;;
		esac
		sleep 0.25
		c=$(( c + 1 ))
	done
	sleep 1
	local st; st="$(read_stats)"
	hl_stop >/dev/null 2>&1
	echo "$st"
}

row() { # row LABEL STATS
	local L="$1" R="$2"
	awk -v l="$L" \
		-v pre="$(v pre50 "$R")" -v px="$(v px50 "$R")" -v dn="$(v dn50 "$R")" \
		-v bl="$(v bl50 "$R")" -v post="$(v post50 "$R")" -v fb="$(v fb50 "$R")" \
		-v up="$(v up50 "$R")" -v up0="$(v up0_50 "$R")" -v upn="$(v up_n "$R")" \
	'BEGIN{
		if (bl=="" || bl=="null") { printf "  %-8s NO BLUR SAMPLES\n", l; exit }
		rest = bl - px - dn; if (rest < 0) rest = 0;
		printf "  %-8s pre=%7.0f  prefix=%7.0f  down=%7.0f  rest=%7.0f  |blur=%7.0f|  post=%7.0f  frame=%7.0f\n",
			l, pre/1000, px/1000, dn/1000, rest/1000, bl/1000, post/1000, fb/1000;
		printf "  %-8s        %5.1f%%          %5.1f%%         %5.1f%%        %5.1f%%    (%5.1f%%)         %5.1f%%   of the blur frame\n",
			"", 100*pre/fb, 100*px/fb, 100*dn/fb, 100*rest/fb, 100*bl/fb, 100*post/fb;
		if (upn+0 > 0)
			printf "  %-8s        single-chain only: up=%.0fus  up0=%.0fus\n", "", up/1000, up0/1000;
	}'
}

echo "══ frame phase profile ══ ${W}x${H} scale $SCALE levels=$LEVELS radius=$RADIUS cycles=$CYCLES"
echo "   pre + blur + post == frame;  prefix + down + rest == blur"
echo
for wl in $WORKLOADS; do
	case "$wl" in
	one)    R="$(segment one 1 damage)";    L="1blur" ;;
	five)   R="$(segment five 5 damage)";   L="5blur" ;;
	tag)    R="$(segment tag 3 tag)";       L="tag" ;;
	move)   R="$(segment move 1 move)";     L="move" ;;
	resize) R="$(segment resize 1 resize)"; L="resize" ;;
	*) echo "unknown workload $wl" >&2; continue ;;
	esac
	echo "── $L ──  frames=$(v frames "$R") blurframes=$(v blurframes "$R") chains=$(v chains "$R") maxch=$(v maxch "$R")"
	echo "         damage p50=$(v dmg50 "$R") p95=$(v dmg95 "$R")  fallbacks=$(v falls "$R") of $(v chains "$R") chains"
	row "$L" "$R"
	BAD=""
	[ "$(v waits "$R")" = "0" ] || BAD="$BAD cpu_waits"
	[ "$(v fb_frames "$R")" = "0" ] || BAD="$BAD fallback_frames"
	[ "$(v verr "$R")" = "0" ] || BAD="$BAD validation"
	[ "$(v chains "$R")" != "0" ] || BAD="$BAD no_blur"
	[ -n "$BAD" ] && echo "         ROW INVALID:$BAD"
	echo
done
echo "logs: $OUTDIR"
