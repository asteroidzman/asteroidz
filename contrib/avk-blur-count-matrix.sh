#!/usr/bin/env bash
# avk-blur-count-matrix.sh -- what does the Nth blurred window cost?
#
# The live desktop shows five overlapping blurred terminals producing a
# strongly bimodal gpu_frameBLUR (p50 0.52ms, p95 13.20ms) against a 6.944ms
# budget. Two very different things would produce that shape:
#
#   (a) a per-frame cost that scales badly with the number of chains, so the
#       expensive frames are expensive because five chains ran; or
#   (b) a per-frame cost that is fine, with the tail being the frames where
#       the blur had to rebuild at all -- damaged frames among undamaged ones.
#
# (a) is an architecture problem and (b) is a damage problem, and they have
# nothing in common except the histogram. So this measures the cost as a
# function of chain count under a damage cadence that is FORCED and identical
# at every N, and separately keeps the per-frame series (AZ_TS_TRACE) so the
# fast and slow populations can be told apart rather than averaged.
#
# ── WHY damage_all AND NOT A MOVE ─────────────────────────────────────────
#
# A move animation runs for a wall-clock duration at whatever rate the machine
# manages, so the number of blur chains it produces is a function of how busy
# the machine was -- which changes the denominator between runs and makes the
# per-chain figures incomparable. damage_all forces every chain to rebuild, is
# bounded, and is the same work at every N.
#
# ── WHAT THE PHASE SPLIT CAN AND CANNOT SAY ───────────────────────────────
#
# The BLUR_PREFIX_END and BLUR_DOWN_END marks are written by the FIRST chain
# only. At N > 1 the span after DOWN_END is chain 1's upsample plus chains
# 2..N entire, so it is reported as `remainder` and never as "up". The prefix
# COMMAND and PIXEL counts are exact at every N and carry no such caveat --
# they are counters, not spans.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-count-matrix"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-blur-count-$$"
mkdir -p "$OUTDIR"

COUNTS="${COUNTS:-1 2 4 5 8}"
CYCLES="${CYCLES:-25}"
WARMUP="${WARMUP:-4}"
LEVELS="${LEVELS:-2}"
RADIUS="${RADIUS:-6}"

CFG="border_radius 8
effects { shadow { enable 0 }
  blur { enable 1; optimized 1; passes $LEVELS; radius $RADIUS
    params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

KEYS='
 frames:.frames, gframes:.graph_frames, waits:.cpu_sync_waits,
 fallback:.fallback_frames, chains:.blur_chains,
 maxchains:.blur_max_chains_per_frame, bpasses:.blur_passes,
 gpasses:.graph_passes, gbarriers:.graph_barriers, guses:.graph_uses,
 gres:.graph_resources,
 pcmd:.blur_prefix_commands, preplay:.blur_prefix_replays,
 ppx:.blur_prefix_pixels, prebuild:.blur_prefix_rebuild_pixels,
 cap:.blur_capture_pixels, proc:.blur_processed_pixels,
 res:.blur_result_pixels, req:.blur_required_work_pixels,
 tcreate:.transient_creates, tbytes:.transient_peak_bytes,
 rc50:.record_ns_p50, rc95:.record_ns_p95, rc99:.record_ns_p99,
 gb50:.graph_build_ns_p50, gb95:.graph_build_ns_p95, gb99:.graph_build_ns_p99,
 rb50:.blur_region_build_ns_p50, rb95:.blur_region_build_ns_p95,
 rw50:.ring_wait_ns_p50, rw99:.ring_wait_ns_p99, rwn:.ring_wait_samples,
 px50:.gpu_blur_prefix_ns_p50, px95:.gpu_blur_prefix_ns_p95,
 px99:.gpu_blur_prefix_ns_p99, pxn:.gpu_blur_prefix_samples,
 dn50:.gpu_blur_down_ns_p50, dn95:.gpu_blur_down_ns_p95,
 dn99:.gpu_blur_down_ns_p99,
 bl50:.gpu_blur_total_ns_p50, bl95:.gpu_blur_total_ns_p95,
 bl99:.gpu_blur_total_ns_p99, bln:.gpu_blur_total_samples,
 blovf:.gpu_blur_total_overflow,
 fb50:.gpu_frame_blur_ns_p50, fb95:.gpu_frame_blur_ns_p95,
 fb99:.gpu_frame_blur_ns_p99, fbn:.gpu_frame_blur_samples,
 fbovf:.gpu_frame_blur_overflow,
 fr50:.gpu_frame_ns_p50, fr95:.gpu_frame_ns_p95, fr99:.gpu_frame_ns_p99,
 frn:.gpu_frame_samples, straddle:.gpu_results_straddled,
 verr:.validation_errors
'
read_stats() {
	hl_get "get avk-stats" | jq -r "{$KEYS} | to_entries | \
		map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null
}
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
us() { awk -v n="${1:-}" 'BEGIN{ if(n==""||n=="null"){printf "%8s","-"} \
	else {printf "%8.1f", n/1000} }'; }

segment() { # segment N
	# Two statements, not one `local n=... dir=...`: `local` declares n before
	# the later word is expanded, so `$n` there is an unset LOCAL and set -u
	# kills the function -- silently, at every N, leaving every stat empty and
	# every premise "failing".
	local n="$1"
	local dir="$OUTDIR/n$n"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"
	HL_ENV="AZ_TS_TRACE=1 ${HL_EXTRA_ENV:-}"
	export HL_OUTDIR HL_ENV
	hl_start "$CFG" >/dev/null 2>&1

	# CASCADED, NOT TILED. Overlapping blurred windows are the case where one
	# blur's source contains another blur's result, which is the only case
	# where a prefix has anything to replay. A tiled fixture would have N
	# independent chains and would answer a different question.
	local i=0
	while [ "$i" -lt "$n" ]; do
		hl_spawn_wlbgeffect "bc$i" 300 "bc$i-n$n" >/dev/null
		i=$(( i + 1 ))
	done
	hl_wait_client_count "$n" 120
	hl_dispatch set_option,animations,0 1
	hl_dispatch toggle_all_floating 1
	sleep "$WARMUP"
	hl_dispatch damage_all
	sleep 1
	hl_dispatch damage_all
	sleep 1

	hl_dispatch reset_avk_stats
	local c=0
	while [ "$c" -lt "$CYCLES" ]; do
		hl_dispatch damage_all
		sleep 0.2
		c=$(( c + 1 ))
	done
	sleep 1
	local st
	st="$(read_stats)"
	grep "avk cohort: READ" "$dir/state/asteroidz/asteroidz.log" \
		> "$dir/frames.txt" 2>/dev/null || true
	hl_stop >/dev/null 2>&1
	echo "$st"
}

echo "══ blur-count matrix ══ levels=$LEVELS radius=$RADIUS cycles=$CYCLES"
echo
printf "%3s %7s %7s %8s %8s %9s %9s %10s %12s %12s\n" \
	"N" "frames" "chains" "maxch" "passes" "prefcmd" "prefrep" "prefpx" "capturepx" "processedpx"
RESULTS=""
for N in $COUNTS; do
	R="$(segment "$N")"
	RESULTS="$RESULTS
$N|$R"
	printf "%3s %7s %7s %8s %8s %9s %9s %10s %12s %12s\n" "$N" \
		"$(v frames "$R")" "$(v chains "$R")" "$(v maxchains "$R")" \
		"$(v bpasses "$R")" "$(v pcmd "$R")" "$(v preplay "$R")" \
		"$(v ppx "$R")" "$(v cap "$R")" "$(v proc "$R")"
done

echo
echo "── graph topology ──"
printf "%3s %9s %10s %8s %9s %9s %12s\n" \
	"N" "passes" "barriers" "res" "uses" "transients" "peak_bytes"
echo "$RESULTS" | while IFS='|' read -r N R; do
	[ -n "$N" ] || continue
	printf "%3s %9s %10s %8s %9s %9s %12s\n" "$N" "$(v gpasses "$R")" \
		"$(v gbarriers "$R")" "$(v gres "$R")" "$(v guses "$R")" \
		"$(v tcreate "$R")" "$(v tbytes "$R")"
done

echo
echo "── CPU (us) ──"
printf "%3s %8s %8s %8s | %8s %8s | %8s %8s | %8s\n" \
	"N" "rec50" "rec95" "rec99" "gbld50" "gbld95" "rgn50" "rgn95" "ringw99"
echo "$RESULTS" | while IFS='|' read -r N R; do
	[ -n "$N" ] || continue
	printf "%3s" "$N"
	us "$(v rc50 "$R")"; us "$(v rc95 "$R")"; us "$(v rc99 "$R")"
	printf " |"; us "$(v gb50 "$R")"; us "$(v gb95 "$R")"
	printf " |"; us "$(v rb50 "$R")"; us "$(v rb95 "$R")"
	printf " |"; us "$(v rw99 "$R")"; echo
done

echo
echo "── GPU (us) ──"
printf "%3s %8s %8s | %8s %8s | %8s %8s %8s | %8s %8s %8s\n" \
	"N" "pref50" "pref95" "down50" "down95" "blur50" "blur95" "blur99" \
	"fBLUR50" "fBLUR95" "fBLUR99"
echo "$RESULTS" | while IFS='|' read -r N R; do
	[ -n "$N" ] || continue
	printf "%3s" "$N"
	us "$(v px50 "$R")"; us "$(v px95 "$R")"
	printf " |"; us "$(v dn50 "$R")"; us "$(v dn95 "$R")"
	printf " |"; us "$(v bl50 "$R")"; us "$(v bl95 "$R")"; us "$(v bl99 "$R")"
	printf " |"; us "$(v fb50 "$R")"; us "$(v fb95 "$R")"; us "$(v fb99 "$R")"
	echo
done

echo
echo "── premises ──"
echo "$RESULTS" | while IFS='|' read -r N R; do
	[ -n "$N" ] || continue
	BAD=""
	[ "$(v waits "$R")" = "0" ] || BAD="$BAD cpu_waits"
	[ "$(v fallback "$R")" = "0" ] || BAD="$BAD fallback_frames"
	[ "$(v blovf "$R")" = "0" ] || BAD="$BAD blur_hist_overflow"
	[ "$(v fbovf "$R")" = "0" ] || BAD="$BAD frameblur_hist_overflow"
	[ "$(v verr "$R")" = "0" ] || BAD="$BAD validation_errors"
	# Straddled results are the RESET WORKING, not a fault: the frames in
	# flight when reset_avk_stats ran carry results from the previous window
	# and are dropped on arrival. What would be a fault is more of them than
	# the pipeline can hold, which would mean the generation stamp is not
	# separating the windows at all.
	[ "$(v straddle "$R")" -le 4 ] 2>/dev/null \
		|| BAD="$BAD straddled=$(v straddle "$R")"
	[ "$(v chains "$R")" != "0" ] || BAD="$BAD no_blur"
	# The fixture asks every chain to rebuild, so max chains per frame must
	# reach N. If it does not, the scene never had N simultaneous chains and
	# every per-chain figure in the row is against the wrong denominator.
	[ "$(v maxchains "$R")" = "$N" ] || BAD="$BAD maxchains!=$N"
	if [ -n "$BAD" ]; then echo "  N=$N ROW INVALID:$BAD"; else echo "  N=$N ok"; fi
done

echo
echo "per-frame traces: $OUTDIR/n*/frames.txt"
echo "logs: $OUTDIR"
