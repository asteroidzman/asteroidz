#!/usr/bin/env bash
# avk-blur-up0-ab.sh — does the fragment reduction become GPU time?
#
# M4F.2D.2. The correctness gate is closed: the up0 scissor removes exactly the
# fragments the derivation predicts, pixel-identically, at every transform. The
# only remaining question is whether those fragments cost enough GPU time to be
# worth removing.
#
# ── SAME BINARY, BALANCED ORDER ───────────────────────────────────────────
#
# AZ_BLUR_UP0_SCISSOR is a runtime toggle, so OFF and ON are the same
# executable, the same shaders, the same graph and the same fixture -- one
# scissor apart. Each workload runs
#
#     OFF  ON  ON  OFF
#
# and the delta is taken between the MEANS of the two pairs. A GPU that is
# warmer, or a driver whose caches are primed, favours whatever ran second; a
# balanced order costs one extra pass over the fixture and removes that bias
# instead of hoping it is small.
#
# Each mode is warmed independently before its statistics are reset, so setup
# frames -- first pipeline use, first transient, first full-damage frame --
# never enter the measured window.
#
# ── EVERY ROW PROVES ITS OWN PREMISES ─────────────────────────────────────
#
# A timing row is published only if: the blur ran, the topology matched between
# OFF and ON, no histogram saturated, there were no CPU waits or fallback
# frames, the graph is identical, and -- the strong one --
#
#     OFF processed - ON processed == predicted removable up0 pixels
#
# EXACTLY. That is deterministic rectangle arithmetic, not a measurement, so
# "within noise" is not a category it has.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-up0-ab"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-ab-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

LEVELS="${LEVELS:-3}"
RADIUS="${RADIUS:-5}"
SCALE="${SCALE:-1}"
CYCLES="${CYCLES:-30}"
WARMUP="${WARMUP:-5}"
KINDS="${KINDS:-control}"

CFG="border_radius 0
effects { shadow { enable 0 }
  blur { enable 1; passes $LEVELS; radius $RADIUS } }"

KEYS='
  frames:.frames, waits:.cpu_sync_waits, fallback:.fallback_frames,
  chains:.blur_chains, maxchains:.blur_max_chains_per_frame,
  bpasses:.blur_passes, gpasses:.graph_passes, gbarriers:.graph_barriers,
  guses:.graph_uses, tcreate:.transient_creates,
  proc:.blur_processed_pixels, req:.blur_required_work_pixels,
  rmup0:.blur_removable_up0_pixels, up0px:.blur_up0_pixels,
  cbf:.cohort_blur_frames, cif:.cohort_idle_frames,
  u050:.gpu_blur_up0_ns_p50, u095:.gpu_blur_up0_ns_p95,
  u099:.gpu_blur_up0_ns_p99, u0n:.gpu_blur_up0_samples,
  u0ovf:.gpu_blur_up0_overflow,
  bl50:.gpu_blur_total_ns_p50, bl95:.gpu_blur_total_ns_p95,
  bl99:.gpu_blur_total_ns_p99, bln:.gpu_blur_total_samples,
  blovf:.gpu_blur_total_overflow,
  fr50:.gpu_frame_ns_p50, fr95:.gpu_frame_ns_p95, fr99:.gpu_frame_ns_p99,
  frn:.gpu_frame_samples, frovf:.gpu_frame_overflow,
  rc50:.record_ns_p50, rc95:.record_ns_p95, rc99:.record_ns_p99,
  rcavg:.record_ns_avg, rcn:.record_samples, rcovf:.record_overflow,
  gbavg:.graph_build_ns_avg, rwavg:.ring_wait_ns_avg,
  rbavg:.blur_region_build_ns_avg,
  fb50:.gpu_frame_blur_ns_p50, fb95:.gpu_frame_blur_ns_p95,
  fb99:.gpu_frame_blur_ns_p99, fbn:.gpu_frame_blur_samples,
  fbovf:.gpu_frame_blur_overflow,
  rw50:.ring_wait_ns_p50, rw95:.ring_wait_ns_p95, rw99:.ring_wait_ns_p99,
  rwn:.ring_wait_samples,
  gb50:.graph_build_ns_p50, gb95:.graph_build_ns_p95, gb99:.graph_build_ns_p99,
  rb50:.blur_region_build_ns_p50, rb95:.blur_region_build_ns_p95,
  rb99:.blur_region_build_ns_p99, rbn:.blur_region_build_samples
'
read_stats() {
	hl_get "get avk-stats" | jq -r "{$KEYS} | to_entries | \
		map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null
}
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
us() { awk -v n="${1:-}" 'BEGIN{ if(n==""||n=="null"){print "-"} else {printf "%.1f", n/1000} }'; }

# ── THE THREE MODES ───────────────────────────────────────────────────────
#
#   A  true baseline      no scissor, and NO required-region derivation
#   B  instrumented base  no scissor, derivation runs (what M4F.2D.1 measured)
#   C  optimized          scissor on, derivation runs
#
# B is not a production baseline. avk_blur_work_of() runs today only because
# M4F.2D.1 wired it for accounting; a shipped renderer with the scissor OFF
# would not call it at all. Comparing B against C would therefore charge the
# optimisation nothing for the derivation it actually needs. A->C is the honest
# CPU cost of KEEPING this, A->B is what the instrumentation costs, and B->C is
# the scissor alone.
mode_env() {
	case "$1" in
	A) echo "AZ_BLUR_UP0_SCISSOR=0 AZ_BLUR_SKIP_WORK_DERIVATION=1" ;;
	B) echo "AZ_BLUR_UP0_SCISSOR=0" ;;
	C) echo "AZ_BLUR_UP0_SCISSOR=1" ;;
	esac
}

# one measured segment
segment() { # segment KIND MODE TAG
	local kind="$1" mode="$2" tag="$3"
	local pulse=""
	case "$kind" in
	small)  pulse="16x16@120,120:300:40" ;;
	medium) pulse="128x96@100,80:300:40" ;;
	large)  pulse="360x260@20,20:300:40" ;;
	esac
	HL_SCALE1="$SCALE"
	HL_ENV="$(mode_env "$mode")"
	export HL_ENV HL_SCALE1
	hl_start "$CFG"
	if [ -n "$pulse" ]; then
		export WLBGEFFECT_PULSE="$pulse" WLBGEFFECT_NO_BLUR=1
		hl_spawn_wlbgeffect back 300 "back-$tag" ff202020 >/dev/null
		hl_wait_client_count 1 60
		unset WLBGEFFECT_PULSE WLBGEFFECT_NO_BLUR
	fi
	if [ "$kind" = blur8 ]; then
		# EIGHT CHAINS. avk_blur_work_of() runs once per chain, so the CPU tax
		# it charges is per-chain while the frame it is charged against is not:
		# a derivation that is invisible at one chain can be material at eight.
		# The GPU rows from this fixture are not up0-isolatable (up0 needs a
		# single chain) and are not published as such.
		local b=0
		while [ "$b" -lt 8 ]; do
			hl_spawn_wlbgeffect "b$b" 300 "b$b-$tag" >/dev/null
			b=$(( b + 1 ))
		done
		hl_wait_client_count 8 90
	else
		hl_spawn_wlbgeffect front 300 "front-$tag" >/dev/null
		hl_wait_client_count "$([ -n "$pulse" ] && echo 2 || echo 1)" 60
	fi
	# ── WARM THIS MODE ─────────────────────────────────────────────────────
	sleep "$WARMUP"
	case "$kind" in
	control|full|blur8) hl_dispatch damage_all; sleep 1; hl_dispatch damage_all ;;
	move)
		# ANIMATIONS OFF, AND SAID OUT LOUD. A move animation runs at the
		# refresh rate for a wall-clock duration, so the number of blur chains
		# it produces is a function of how busy the machine was -- 581 in one
		# segment and 577 in the next. That is not noise to be averaged out:
		# it changes the denominator, so the pixel identity cannot be asserted
		# and the timing pairs are not comparable. The first run of this row
		# was correctly rejected as ROW INVALID rather than published. With
		# animations off each dispatched move settles in a bounded number of
		# frames and the two modes see the same topology.
		hl_dispatch set_option,animations,0
		hl_dispatch toggle_floating; sleep 0.5 ;;
	esac
	sleep 2
	# ── MEASURE ────────────────────────────────────────────────────────────
	hl_dispatch reset_avk_stats
	local i=0
	while [ "$i" -lt "$CYCLES" ]; do
		case "$kind" in
		move) hl_dispatch "move_window,$(( 100 + (i % 8) * 40 )),$(( 80 + (i % 5) * 40 ))" ;;
		*)    hl_dispatch damage_all ;;
		esac
		sleep 0.2
		i=$(( i + 1 ))
	done
	sleep 1
	local st
	st="$(read_stats)"
	hl_stop
	echo "$st"
}

# mean of two runs of one metric
mean2() { awk -v a="${1:-0}" -v b="${2:-0}" 'BEGIN{printf "%.0f", (a+b)/2}'; }

delta_row() { # delta_row LABEL OFFVAL ONVAL
	awk -v l="$1" -v o="${2:-0}" -v n="${3:-0}" 'BEGIN{
		if (o+0 == 0) { printf "  %-16s %10s %10s %10s %8s\n", l, "-", "-", "-", "-"; exit }
		printf "  %-16s %10.1f %10.1f %+10.1f %+7.1f%%\n", l, o/1000, n/1000,
			(n-o)/1000, 100*(n-o)/o }'
}

# SKIPAB=1 runs only the CPU control below. The two blocks measure different
# things and each costs six minutes; there is no reason to pay for both.
AB_KINDS="$KINDS"
[ "${SKIPAB:-0}" = "1" ] && AB_KINDS=""

for KIND in $AB_KINDS; do
	echo
	echo "══ $KIND ══ levels=$LEVELS radius=$RADIUS scale=$SCALE cycles=$CYCLES"
	echo "   run order: OFF ON ON OFF (delta from the mean of each pair)"
	O1="$(segment "$KIND" B "$KIND-o1")"
	N1="$(segment "$KIND" C "$KIND-n1")"
	N2="$(segment "$KIND" C "$KIND-n2")"
	O2="$(segment "$KIND" B "$KIND-o2")"

	printf "   topology OFF chains=%s/%s maxchains=%s bpasses=%s graph=%s/%s/%s\n" \
		"$(v chains "$O1")" "$(v chains "$O2")" "$(v maxchains "$O1")" \
		"$(v bpasses "$O1")" "$(v gpasses "$O1")" "$(v gbarriers "$O1")" \
		"$(v guses "$O1")"
	printf "   topology ON  chains=%s/%s maxchains=%s bpasses=%s graph=%s/%s/%s\n" \
		"$(v chains "$N1")" "$(v chains "$N2")" "$(v maxchains "$N1")" \
		"$(v bpasses "$N1")" "$(v gpasses "$N1")" "$(v gbarriers "$N1")" \
		"$(v guses "$N1")"

	# ── PREMISES ───────────────────────────────────────────────────────────
	BAD=""
	for R in "$O1" "$O2" "$N1" "$N2"; do
		[ "$(v waits "$R")" = "0" ] || BAD="$BAD cpu_waits"
		[ "$(v fallback "$R")" = "0" ] || BAD="$BAD fallback"
		[ "$(v blovf "$R")" = "0" ] || BAD="$BAD blur_overflow"
		[ "$(v frovf "$R")" = "0" ] || BAD="$BAD frame_overflow"
		[ "$(v rcovf "$R")" = "0" ] || BAD="$BAD record_overflow"
		[ "$(v chains "$R")" != "0" ] || BAD="$BAD no_blur"
		[ "$(v bln "$R")" != "0" ] || BAD="$BAD no_blur_samples"
		[ "$(v fbovf "$R")" = "0" ] || BAD="$BAD frameblur_overflow"
		[ "$(v fbn "$R")" != "0" ] || BAD="$BAD no_frameblur_samples"
		# ── THE COHORT PREMISE IS FIXTURE-AWARE ────────────────────────────
		#
		# The universal invariant is only 0 <= BLUR <= ALL. Demanding strict
		# inequality everywhere would fail the single-chain control for being
		# CORRECT: it damages the whole output every cycle, so every measured
		# frame really is blur-active and BLUR == ALL == 25 is the right
		# answer, not a broken classifier.
		#
		# So each fixture declares which it expects, and equality is only a
		# fault where the fixture was built to contain idle frames.
		[ "$(v fbn "$R")" -le "$(v frn "$R")" ] 2>/dev/null \
			|| BAD="$BAD frameblur_exceeds_frames"
		case "$KIND" in
		small|medium|large)
			# built to contain idle frames: the pulse damages a region that
			# does not always intersect the blur
			[ "$(v fbn "$R")" -lt "$(v frn "$R")" ] 2>/dev/null \
				|| BAD="$BAD cohort_excluded_nothing"
			[ "$(v cif "$R")" -gt 0 ] 2>/dev/null \
				|| BAD="$BAD no_idle_frames_classified" ;;
		control|full|move|blur8)
			# damage_all / bounded move: every measured frame carries blur
			[ "$(v fbn "$R")" = "$(v frn "$R")" ] \
				|| BAD="$BAD cohort_dropped_blur_frames" ;;
		esac
	done
	for F in gpasses gbarriers guses bpasses; do
		[ "$(v $F "$O1")" = "$(v $F "$N1")" ] || BAD="$BAD graph_$F"
	done
	# The exact-accounting premise, per matched pair.
	PROFF="$(mean2 "$(v proc "$O1")" "$(v proc "$O2")")"
	PRON="$(mean2 "$(v proc "$N1")" "$(v proc "$N2")")"
	PRED="$(mean2 "$(v rmup0 "$O1")" "$(v rmup0 "$O2")")"
	CHOFF="$(mean2 "$(v chains "$O1")" "$(v chains "$O2")")"
	CHON="$(mean2 "$(v chains "$N1")" "$(v chains "$N2")")"
	# THE MEASURED up0 PASS, on its own. Everything the prototype can affect
	# is in here; the chain totals above also contain four passes it never
	# touches, so a reduction quoted against them is the wrong denominator for
	# the gpu_blur_up0 span.
	U0OFF="$(mean2 "$(v up0px "$O1")" "$(v up0px "$O2")")"
	U0ON="$(mean2 "$(v up0px "$N1")" "$(v up0px "$N2")")"
	printf "   pixels OFF %s  ON %s  removed %s  predicted %s  (chains %s vs %s)\n" \
		"$PROFF" "$PRON" "$(( PROFF - PRON ))" "$PRED" "$CHOFF" "$CHON"
	awk -v a="$U0OFF" -v b="$U0ON" 'BEGIN{
		printf "   up0 pixels OFF %d  ON %d  removed %d  (%.1f%% of the up0 pass)\n",
			a, b, a-b, (a>0 ? 100*(a-b)/a : 0) }'
	# B->C must move the up0 pass and NOTHING ELSE: the whole-chain removal has
	# to be exactly the up0 removal, or some other pass changed too.
	if [ "$(( PROFF - PRON ))" != "$(( U0OFF - U0ON ))" ]; then
		BAD="$BAD removal_not_confined_to_up0"
	fi
	if [ "$CHOFF" != "$CHON" ]; then
		# Different chain counts mean different denominators; the pixel
		# identity cannot be asserted and neither can the timing be paired.
		echo "   NOTE: chain counts differ between modes -- pixel identity not asserted"
	elif [ "$(( PROFF - PRON ))" != "$PRED" ]; then
		BAD="$BAD removed!=predicted"
	fi
	if [ -n "$BAD" ]; then
		echo "   ROW INVALID:$BAD"
		hl_assert "$KIND: A/B premises hold" "false" "true"
		continue
	fi
	hl_assert "$KIND: A/B premises hold" "true" "true"

	echo "   metric              OFF µs      ON µs      delta   delta%"
	for M in u050:gpu_up0_p50 u095:gpu_up0_p95 u099:gpu_up0_p99 \
			bl50:gpu_blur_p50 bl95:gpu_blur_p95 bl99:gpu_blur_p99 \
			fb50:gpu_frameBLUR_p50 fb95:gpu_frameBLUR_p95 \
			fb99:gpu_frameBLUR_p99 \
			fr50:gpu_frameALL_p50 fr95:gpu_frameALL_p95 fr99:gpu_frameALL_p99 \
			rc50:cpu_record_p50 rc95:cpu_record_p95 rc99:cpu_record_p99 \
			rw50:ring_wait_p50 rw95:ring_wait_p95 rw99:ring_wait_p99 \
			gb50:graph_build_p50 gb95:graph_build_p95; do
		K="${M%%:*}"; L="${M#*:}"
		delta_row "$L" "$(mean2 "$(v $K "$O1")" "$(v $K "$O2")")" \
			"$(mean2 "$(v $K "$N1")" "$(v $K "$N2")")"
	done
	# THE COHORT SIZES ARE THE EVIDENCE, not decoration. gpu_frameALL is over
	# every presented frame; gpu_frameBLUR is the same timestamps restricted to
	# frames that actually ran a blur. When frame_all >> frame_blur the two are
	# different populations and only the second can be compared with gpu_blur.
	printf "   cohort frames ALL %s/%s vs %s/%s | BLUR %s/%s vs %s/%s\n" \
		"$(v frn "$O1")" "$(v frn "$O2")" "$(v frn "$N1")" "$(v frn "$N2")" \
		"$(v fbn "$O1")" "$(v fbn "$O2")" "$(v fbn "$N1")" "$(v fbn "$N2")"
	printf "   samples up0 OFF %s/%s ON %s/%s | blur %s/%s vs %s/%s | frame %s/%s vs %s/%s\n" \
		"$(v u0n "$O1")" "$(v u0n "$O2")" "$(v u0n "$N1")" "$(v u0n "$N2")" \
		"$(v bln "$O1")" "$(v bln "$O2")" "$(v bln "$N1")" "$(v bln "$N2")" \
		"$(v frn "$O1")" "$(v frn "$O2")" "$(v frn "$N1")" "$(v frn "$N2")"
	printf "   region build p50 %s ns (n=%s)  transient creates OFF %s/%s ON %s/%s\n" \
		"$(v rb50 "$N1")" "$(v rbn "$N1")" "$(v tcreate "$O1")" \
		"$(v tcreate "$O2")" "$(v tcreate "$N1")" "$(v tcreate "$N2")"
	if [ "$(v u0n "$N1")" = "0" ] || [ "$(v maxchains "$N1")" != "1" ]; then
		echo "   UP0_NOT_ISOLATABLE (maxchains=$(v maxchains "$N1"), samples=$(v u0n "$N1"))"
	else
		# ── DID THE PASS STILL RUN? ────────────────────────────────────────
		#
		# A 97% drop is what removing nearly every full-resolution fragment
		# looks like -- and it is ALSO what a collapsed timestamp interval or a
		# skipped pass looks like. These separate the two. The span must exist,
		# it must still fit inside the blur total it is part of, and the pass
		# must still have shaded pixels.
		U0BAD=""
		for R in "$N1" "$N2"; do
			[ "$(v u0n "$R")" -gt 0 ] 2>/dev/null || U0BAD="$U0BAD no_up0_span"
			[ "$(v up0px "$R")" -gt 0 ] 2>/dev/null \
				|| U0BAD="$U0BAD up0_rendered_nothing"
			[ "$(v u050 "$R")" -le "$(v bl50 "$R")" ] 2>/dev/null \
				|| U0BAD="$U0BAD up0_exceeds_blur_total"
			[ "$(v u0ovf "$R")" = "0" ] || U0BAD="$U0BAD up0_overflow"
		done
		if [ -n "$U0BAD" ]; then
			echo "   UP0 SANITY FAILED:$U0BAD"
		else
			printf "   up0 sanity: span present, up0 p50 %s ns <= blur p50 %s ns, %s px still shaded\n" \
				"$(v u050 "$N1")" "$(v bl50 "$N1")" "$(v up0px "$N1")"
		fi
		hl_assert "$KIND: up0 span is a real interval over a pass that ran" \
			"$([ -z "$U0BAD" ] && echo true || echo false)" true
	fi
	{ echo "OFF1 $O1"; echo "OFF2 $O2"; echo "ON1 $N1"; echo "ON2 $N2"; } \
		> "$OUTDIR/ab-$KIND.txt"
done

# ── THE THREE-MODE CPU CONTROL ────────────────────────────────────────────
#
# The GPU rows above compare B against C, which is the right comparison for
# "what does the scissor save". It is the WRONG comparison for "what does
# keeping the scissor cost the CPU", because B is already paying for the
# derivation that only C needs. This block runs all three.
if [ "${CPUCTL:-0}" = "1" ]; then
for KIND in $KINDS; do
	echo
	echo "══ CPU CONTROL: $KIND ══ levels=$LEVELS radius=$RADIUS cycles=$CYCLES"
	echo "   run order: A B C C B A (mean of each pair)"
	A1="$(segment "$KIND" A "$KIND-a1")"
	B1="$(segment "$KIND" B "$KIND-b1")"
	C1="$(segment "$KIND" C "$KIND-c1")"
	C2="$(segment "$KIND" C "$KIND-c2")"
	B2="$(segment "$KIND" B "$KIND-b2")"
	A2="$(segment "$KIND" A "$KIND-a2")"

	# ── PREMISES: the switch must do what it claims, and NOTHING else ──────
	CBAD=""
	# A must genuinely skip the derivation ...
	for R in "$A1" "$A2"; do
		[ "$(v req "$R")" = "0" ] || CBAD="$CBAD A_still_derived"
		[ "$(v rbn "$R")" = "0" ] || CBAD="$CBAD A_region_timer_ran"
	done
	# ... and B/C must genuinely run it.
	for R in "$B1" "$B2" "$C1" "$C2"; do
		[ "$(v req "$R")" != "0" ] || CBAD="$CBAD BC_no_derivation"
	done
	for R in "$A1" "$A2" "$B1" "$B2" "$C1" "$C2"; do
		[ "$(v waits "$R")" = "0" ] || CBAD="$CBAD cpu_waits"
		[ "$(v rcovf "$R")" = "0" ] || CBAD="$CBAD record_overflow"
	done
	# THE CORRECTNESS PREMISE. Skipping the derivation must change what is
	# COMPUTED, never what is RENDERED: with the scissor off, A and B must
	# shade exactly the same pixels. If they do not, the switch is not a
	# measurement control, it is a second code path, and no CPU number from it
	# means anything.
	PA="$(mean2 "$(v proc "$A1")" "$(v proc "$A2")")"
	PB="$(mean2 "$(v proc "$B1")" "$(v proc "$B2")")"
	PC="$(mean2 "$(v proc "$C1")" "$(v proc "$C2")")"
	UA="$(mean2 "$(v up0px "$A1")" "$(v up0px "$A2")")"
	UB="$(mean2 "$(v up0px "$B1")" "$(v up0px "$B2")")"
	UC="$(mean2 "$(v up0px "$C1")" "$(v up0px "$C2")")"
	printf "   rendered pixels A %s  B %s  C %s\n" "$PA" "$PB" "$PC"
	printf "   up0     pixels A %s  B %s  C %s\n" "$UA" "$UB" "$UC"
	# A vs B: CPU bookkeeping only. Every raster quantity must be identical,
	# or A is a second rendering path and no CPU number from it is comparable.
	[ "$PA" = "$PB" ] || CBAD="$CBAD A_B_render_differs"
	[ "$UA" = "$UB" ] || CBAD="$CBAD A_B_up0_differs"
	# B vs C: the up0 pass moves, and it is the ONLY thing that moves.
	[ "$(( PB - PC ))" = "$(( UB - UC ))" ] \
		|| CBAD="$CBAD BC_removal_not_confined_to_up0"
	printf "   topology A=%s B=%s C=%s chains (bpasses %s/%s/%s)\n" \
		"$(v chains "$A1")" "$(v chains "$B1")" "$(v chains "$C1")" \
		"$(v bpasses "$A1")" "$(v bpasses "$B1")" "$(v bpasses "$C1")"
	for F in gpasses gbarriers guses bpasses; do
		[ "$(v $F "$A1")" = "$(v $F "$B1")" ] || CBAD="$CBAD graph_$F"
	done
	if [ -n "$CBAD" ]; then
		echo "   CPU ROW INVALID:$CBAD"
		hl_assert "$KIND: CPU control premises hold" "false" "true"
		continue
	fi
	hl_assert "$KIND: CPU control premises hold" "true" "true"

	# ── RESOLUTION FIRST ──────────────────────────────────────────────────
	#
	# The percentiles below are read out of 20us histogram buckets. cpu_record
	# is about 100us in total, so ONE bucket is a fifth of the whole quantity:
	# any difference these rows show is +/-20us regardless of what the real
	# difference was, and a -20000 in the p50 column is quantization, not a
	# speedup. They are printed because a LARGE regression would still be
	# visible in them.
	#
	# The _avg rows are accumulated from raw nanoseconds and are exact. They
	# are the only rows here that can resolve this delta, and the region-build
	# average measures the derivation cost DIRECTLY instead of differencing two
	# ~100us numbers -- which is the better instrument for the same question.
	# ── EVERY RUN, NOT ONLY THE PAIR MEANS ────────────────────────────────
	#
	# The expected CPU difference is small enough that run-to-run variation
	# could be the same size as it. Averaging first would hide that, so the six
	# runs are printed before anything is differenced -- if A1 and A2 disagree
	# by as much as A and C do, the delta below is not a measurement.
	echo "   per run:      cpu_record avg/p50/p95/p99   graph_build avg   ring_wait avg"
	for RUN in A1:"$A1" B1:"$B1" C1:"$C1" C2:"$C2" B2:"$B2" A2:"$A2"; do
		RN="${RUN%%:*}"; RV="${RUN#*:}"
		printf "     %-3s %10s %8s %8s %8s   %10s %12s\n" "$RN" \
			"$(v rcavg "$RV")" "$(v rc50 "$RV")" "$(v rc95 "$RV")" \
			"$(v rc99 "$RV")" "$(v gbavg "$RV")" "$(v rwavg "$RV")"
	done
	echo "   resolution: percentiles are 20us-bucketed; only _avg can resolve this"
	echo "   metric              A ns       B ns       C ns     A->B     B->C     A->C"
	for M in rcavg:cpu_record_AVG gbavg:graph_build_AVG rwavg:ring_wait_AVG \
			rbavg:region_build_AVG \
			rc50:cpu_record_p50 rc95:cpu_record_p95 rc99:cpu_record_p99 \
			gb50:graph_build_p50 gb95:graph_build_p95 gb99:graph_build_p99 \
			rw50:ring_wait_p50 rw95:ring_wait_p95 rw99:ring_wait_p99; do
		K="${M%%:*}"; L="${M#*:}"
		awk -v l="$L" \
			-v a="$(mean2 "$(v $K "$A1")" "$(v $K "$A2")")" \
			-v b="$(mean2 "$(v $K "$B1")" "$(v $K "$B2")")" \
			-v c="$(mean2 "$(v $K "$C1")" "$(v $K "$C2")")" 'BEGIN{
			printf "  %-16s %9.0f %10.0f %10.0f %+8.0f %+8.0f %+8.0f\n",
				l, a, b, c, b-a, c-b, c-a }'
	done
	printf "   region build n:  A %s  B %s  C %s   |  record samples %s/%s/%s\n" \
		"$(v rbn "$A1")" "$(v rbn "$B1")" "$(v rbn "$C1")" \
		"$(v rcn "$A1")" "$(v rcn "$B1")" "$(v rcn "$C1")"
	# ── THE PRODUCTION-RELEVANT NUMBER ────────────────────────────────────
	#
	# A->C per FRAME, because that is what a frame budget is spent from.
	# Per chain is shown beside it only to say whether the tax scales with
	# chain count -- it must never be the headline, because normalizing by
	# chains would hide exactly the growth this fixture exists to find.
	awk -v a="$(mean2 "$(v rcavg "$A1")" "$(v rcavg "$A2")")" \
		-v c="$(mean2 "$(v rcavg "$C1")" "$(v rcavg "$C2")")" \
		-v ch="$(mean2 "$(v chains "$C1")" "$(v chains "$C2")")" \
		-v fr="$(mean2 "$(v rcn "$C1")" "$(v rcn "$C2")")" 'BEGIN{
		d = c - a;
		printf "   A->C cpu_record %+.0f ns per frame", d;
		if (fr > 0 && ch > 0) {
			printf "  (%.2f chains/frame, %+.0f ns per chain)", ch/fr, d/(ch/fr);
		}
		printf "\n" }' 
	{ echo "A1 $A1"; echo "A2 $A2"; echo "B1 $B1"; echo "B2 $B2"
	  echo "C1 $C1"; echo "C2 $C2"; } > "$OUTDIR/cpu-$KIND.txt"
done
fi

echo
echo "logs: $OUTDIR"
hl_summary
