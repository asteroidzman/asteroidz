#!/usr/bin/env bash
# avk-blur-cost.sh — what the uncached blur actually costs.
#
# M4F.2D. This is a MEASUREMENT harness, not an assertion suite: its output is
# a table, and the few assertions it does make are the ones that would
# invalidate the table if they failed (no CPU waits, the direct path still
# direct, the fixture actually produced blur).
#
# ── WHAT EACH NUMBER MEANS ────────────────────────────────────────────────
#
#   graph_build_ns   CPU. avk_graph_execute() minus the time spent inside each
#                    pass's own record callback. Barrier derivation and pass
#                    ordering, nothing else.
#   record_ns        CPU. The whole of avk_render_frame(): walking, declaring,
#                    recording every pass, and submitting. INCLUDES
#                    graph_build_ns.
#   gpu_frame_ns     GPU. FRAME_BEGIN -> FRAME_END, the frame's own execution.
#   gpu_blur_total   GPU. BLUR_BEGIN -> BLUR_END: every prefix replay and every
#                    down/up chain in the frame. EXCLUDES the composite draws,
#                    which are interleaved with content in the output segment
#                    and cannot be bracketed honestly.
#   gpu_blur_prefix  GPU. The FIRST chain's prefix replay.
#   gpu_blur_down    GPU. The FIRST chain's downsample passes.
#   gpu_blur_up      GPU. The upsamples -- sampled only on frames with exactly
#                    ONE chain, where that span contains nothing else.
#
# Percentiles come from 20us-bucket histograms and are the UPPER EDGE of the
# bucket the percentile falls in: an over-estimate by at most 20us. At the
# magnitudes involved that is coarse for graph_build and fine for gpu_frame,
# which is why the mean is printed beside them.
#
# ── HOW A RUN IS MADE COMPARABLE ──────────────────────────────────────────
#
# Every run: start, spawn the fixture, WARM UP, reset_avk_stats, then drive a
# fixed number of identical changes, then read. The reset is what makes the
# numbers about the steady state rather than about pipeline creation, first
# transient allocation and first-frame full damage.
#
# MODE=baseline   blur OFF, four scenes -- the direct path, same build
# MODE=matrix     1 / 2 / 4 / 8 blurred windows
# MODE=params     one blur: levels, radius, region size, scale
# MODE=workloads  static / small / medium / large / move / full: the two
#                 efficiency ratios and what each candidate strategy would win
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-cost"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-cost-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

MODE="${MODE:-baseline}"
CYCLES="${CYCLES:-24}"
WARMUP="${WARMUP:-4}"

# jq expression -> one flat line of key=value, so a row is assembled in shell
# without twelve subprocesses per column.
STATS_KEYS='
  frames: .frames,
  cpu_sync_waits: .cpu_sync_waits,
  fallback_frames: .fallback_frames,
  gb50: .graph_build_ns_p50, gb95: .graph_build_ns_p95, gb99: .graph_build_ns_p99,
  gbavg: .graph_build_ns_avg,
  rec50: .record_ns_p50, rec95: .record_ns_p95, rec99: .record_ns_p99,
  recavg: .record_us_avg, recn: .record_samples,
  gf50: .gpu_frame_ns_p50, gf95: .gpu_frame_ns_p95, gf99: .gpu_frame_ns_p99,
  gfavg: .gpu_frame_ns_avg, gfn: .gpu_frame_samples,
  bt50: .gpu_blur_total_ns_p50, bt95: .gpu_blur_total_ns_p95,
  bt99: .gpu_blur_total_ns_p99, btavg: .gpu_blur_total_ns_avg,
  btn: .gpu_blur_total_samples,
  bp50: .gpu_blur_prefix_ns_p50, bpavg: .gpu_blur_prefix_ns_avg,
  bd50: .gpu_blur_down_ns_p50, bdavg: .gpu_blur_down_ns_avg,
  bu50: .gpu_blur_up_ns_p50, buavg: .gpu_blur_up_ns_avg, bun: .gpu_blur_up_samples,
  chains: .blur_chains, bpasses: .blur_passes,
  replays: .blur_prefix_replays, pcmds: .blur_prefix_commands,
  ppx: .blur_prefix_pixels, prpx: .blur_prefix_rebuild_pixels,
  cappx: .blur_capture_pixels, respx: .blur_result_pixels,
  procpx: .blur_processed_pixels, fullcap: .blur_full_capture_pixels,
  tacq: .transient_acquires, tcre: .transient_creates, treu: .transient_reuses,
  tpeak: .transient_peak_bytes,
  gpasses: .graph_passes, gbarr: .graph_barriers, gallocs: .graph_allocs,
  reqpx: .blur_required_work_pixels,
  rmup0: .blur_removable_up0_pixels, rmup01: .blur_removable_up01_pixels,
  rmup: .blur_removable_up_pixels, rmall: .blur_removable_all_pixels,
  srcdmg: .blur_source_damage_pixels, touched: .blur_damage_nodes_touched,
  rb50: .blur_region_build_ns_p50, rb95: .blur_region_build_ns_p95,
  rb99: .blur_region_build_ns_p99, rbn: .blur_region_build_samples,
  up050: .gpu_blur_up0_ns_p50, up0avg: .gpu_blur_up0_ns_avg,
  up0n: .gpu_blur_up0_samples,
  ovf_gf: .gpu_frame_overflow, ovf_bt: .gpu_blur_total_overflow,
  ovf_rec: .record_overflow,
  btmax: .gpu_blur_total_ns_max, gfmax: .gpu_frame_ns_max,
  maxchains: .blur_max_chains_per_frame, bun: .gpu_blur_up_samples,
  rbund: .blur_region_build_underflow,
  rw95: .ring_wait_ns_p95, rwmax: .ring_wait_ns_max, ovf_rw: .ring_wait_overflow
'

read_stats() {
	hl_get "get avk-stats" | jq -r "{$STATS_KEYS} | to_entries | \
		map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null
}

# num FIELD LINE -> the value, or "-"
val() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

us() { # ns -> us with one decimal, tolerant of null
	local v="$1"
	case "$v" in ''|null|-) echo "-" ;; *) awk -v n="$v" 'BEGIN{printf "%.1f", n/1000}' ;; esac
}

# A blur config with knobs. Levels/radius are the SCENE's kernel; every node's
# own strength can only reduce it, so this bounds every chain in the run.
blur_cfg() { # blur_cfg LEVELS RADIUS
	printf 'border_radius 0\neffects { shadow { enable 0 }\n  blur { enable 1; passes %s; radius %s } }\n' \
		"$1" "$2"
}
noblur_cfg() {
	printf 'border_radius 0\neffects { shadow { enable %s }; blur { enable 0 } }\n' "${1:-0}"
}

# drive N identical changes, so every run does the same amount of work
drive_moves() { # drive_moves N
	local i=0
	while [ "$i" -lt "$1" ]; do
		hl_dispatch "move_window,$(( 100 + (i % 8) * 40 )),$(( 80 + (i % 5) * 40 ))"
		sleep 0.25
		i=$(( i + 1 ))
	done
}
drive_full() { # drive_full N -- whole-output damage each time
	local i=0
	while [ "$i" -lt "$1" ]; do
		hl_dispatch damage_all
		sleep 0.2
		i=$(( i + 1 ))
	done
}

# EVERY PUBLISHED ROW PROVES ITS OWN PREMISES. A benchmark that prints a
# number it cannot stand behind is worse than one that prints nothing: the
# first version of this harness published a "small update" row measured on a
# desktop where nothing changed, as processed=0 result=0.
premises_ok() { # premises_ok LABEL STATSLINE WANT_BLUR
	local label="$1" L="$2" want="$3" bad=""
	[ "$(val cpu_sync_waits "$L")" = "0" ] || bad="$bad cpu_waits"
	[ "$(val fallback_frames "$L")" = "0" ] || bad="$bad fallback"
	# SATURATION and ABSENCE are different faults and get different names.
	[ "$(val ovf_gf "$L")" = "0" ] || bad="$bad gpu_frame_OVERFLOW"
	[ "$(val ovf_bt "$L")" = "0" ] || bad="$bad blur_OVERFLOW"
	[ "$(val ovf_rec "$L")" = "0" ] || bad="$bad record_OVERFLOW"
	[ "$(val btn "$L")" != "0" ] || bad="$bad no_blur_gpu_samples"
	[ "$(val gfn "$L")" != "0" ] || bad="$bad no_gpu_samples"
	if [ "$want" = "blur" ]; then
		[ "$(val chains "$L")" != "0" ] || bad="$bad no_blur"
		[ "$(val reqpx "$L")" != "0" ] || bad="$bad no_required"
		[ "$(val procpx "$L")" -ge "$(val reqpx "$L")" ] 2>/dev/null \
			|| bad="$bad actual<required"
	fi
	# IMPOSSIBILITIES, not just absences. Per frame the blur span is contained
	# in the frame span, so a blur MAX above the frame MAX means the marks are
	# being paired wrongly. (p50s may cross legitimately: they are medians of
	# different populations -- blur frames vs all frames -- and a run with
	# cheap no-blur frames drags the frame median down.)
	local bmax fmax
	bmax="$(val btmax "$L")"; fmax="$(val gfmax "$L")"
	if [ "${bmax:-0}" != "null" ] && [ "${fmax:-0}" != "null" ]; then
		[ "${bmax:-0}" -le "${fmax:-0}" ] 2>/dev/null \
			|| bad="$bad blur_max>frame_max"
	fi
	if [ -n "$bad" ]; then
		echo "  ROW INVALID [$label ]:$bad"
		# The values behind the verdict, so the next step is reading rather
		# than re-running.
		printf "    frames=%s gpu_samples=%s blur_samples=%s up_samples=%s \
up0_samples=%s chains=%s maxchains=%s ovf(gf/bt/rec)=%s/%s/%s\n" \
			"$(val frames "$L")" "$(val gfn "$L")" "$(val btn "$L")" \
			"$(val bun "$L")" "$(val up0n "$L")" "$(val chains "$L")" \
			"$(val maxchains "$L")" "$(val ovf_gf "$L")" \
			"$(val ovf_bt "$L")" "$(val ovf_rec "$L")"
		hl_assert "$label: premises hold" "false" "true"
		return 1
	fi
	return 0
}

# The strategy table: what each candidate would remove, from the SAME derived
# regions. Pixel work only -- not a GPU time estimate.
strategy_row() { # strategy_row LABEL STATSLINE
	local L="$2"
	local proc rmall rmup0 rmup01 rmup
	proc="$(val procpx "$L")"; rmall="$(val rmall "$L")"
	rmup0="$(val rmup0 "$L")"; rmup01="$(val rmup01 "$L")"
	rmup="$(val rmup "$L")"
	awk -v l="$1" -v p="$proc" -v a="$rmall" -v u0="$rmup0" -v u01="$rmup01" \
		-v uc="$rmup" 'BEGIN {
		if (p+0 == 0) { printf "  %-20s (no filter work)\n", l; exit }
		printf "  %-20s baseline %.0f px\n", l, p;
		printf "  %-20s   up0 only   removes %12.0f  %5.1f%%  captures %5.1f%% of full\n", "", u0, 100*u0/p, (a>0? 100*u0/a : 0);
		printf "  %-20s   up0+up1    removes %12.0f  %5.1f%%  captures %5.1f%% of full\n", "", u01, 100*u01/p, (a>0? 100*u01/a : 0);
		printf "  %-20s   whole up   removes %12.0f  %5.1f%%  captures %5.1f%% of full\n", "", uc, 100*uc/p, (a>0? 100*uc/a : 0);
		printf "  %-20s   all levels removes %12.0f  %5.1f%%  captures %5.1f%% of full\n", "", a, 100*a/p, 100.0;
	}'
}

# The two ratios, never combined: they are different optimisation problems.
ratio_row() { # ratio_row LABEL STATSLINE
	local L="$2"
	awk -v l="$1" -v pr="$(val prpx "$L")" -v fc="$(val fullcap "$L")" \
		-v ap="$(val procpx "$L")" -v rq="$(val reqpx "$L")" \
		-v sd="$(val srcdmg "$L")" 'BEGIN {
		printf "  %-20s PREFIX  rebuild %.0f / capture %.0f = %.4f\n", l, pr, fc, (fc>0? pr/fc : 0);
		printf "  %-20s FILTER  actual %.0f / required %.0f = %.2fx  (required/actual %.4f)\n", "", ap, rq, (rq>0? ap/rq : 0), (ap>0? rq/ap : 0);
		printf "  %-20s source damage %.0f px\n", "", sd;
	}'
}

hdr_printed=0
row() { # row LABEL STATSLINE
	local label="$1" L="$2"
	if [ "$hdr_printed" = 0 ]; then
		printf "%-22s %7s %7s %7s  %7s %7s %7s  %7s %7s %7s  %6s %6s %6s\n" \
			"scene" "gb_p50" "gb_p95" "gb_avg" "rec_p50" "rec_p95" "rec_avg" \
			"gpu_p50" "gpu_p95" "gpu_avg" "blur50" "blur95" "blravg"
		hdr_printed=1
	fi
	printf "%-22s %7s %7s %7s  %7s %7s %7s  %7s %7s %7s  %6s %6s %6s\n" \
		"$label" \
		"$(us "$(val gb50 "$L")")" "$(us "$(val gb95 "$L")")" \
		"$(us "$(val gbavg "$L")")" \
		"$(us "$(val rec50 "$L")")" "$(us "$(val rec95 "$L")")" \
		"$(awk -v v="$(val recavg "$L")" 'BEGIN{if(v=="null"||v==""){print "-"}else{printf "%.1f", v}}')" \
		"$(us "$(val gf50 "$L")")" "$(us "$(val gf95 "$L")")" \
		"$(us "$(val gfavg "$L")")" \
		"$(us "$(val bt50 "$L")")" "$(us "$(val bt95 "$L")")" \
		"$(us "$(val btavg "$L")")"
}

work_row() { # work_row LABEL STATSLINE
	local label="$1" L="$2"
	printf "  %-20s chains=%s passes=%s replays=%s cmds=%s prefix_px=%s rebuild_px=%s\n" \
		"$label" "$(val chains "$L")" "$(val bpasses "$L")" \
		"$(val replays "$L")" "$(val pcmds "$L")" "$(val ppx "$L")" \
		"$(val prpx "$L")"
	printf "  %-20s capture_px=%s result_px=%s processed_px=%s frames=%s\n" \
		"" "$(val cappx "$L")" "$(val respx "$L")" "$(val procpx "$L")" \
		"$(val frames "$L")"
	printf "  %-20s transients acq=%s create=%s reuse=%s peak=%sKiB  graph passes=%s barriers=%s allocs=%s\n" \
		"" "$(val tacq "$L")" "$(val tcre "$L")" "$(val treu "$L")" \
		"$(( $(val tpeak "$L") / 1024 ))" "$(val gpasses "$L")" \
		"$(val gbarr "$L")" "$(val gallocs "$L")"
}

# ── baseline: the direct path, this build ──────────────────────────────────

if [ "$MODE" = "baseline" ]; then
	echo
	echo "── no blur, four scenes, same build ──────────────────────────────────"
	for SCENE in simple many gradient shadow; do
		case "$SCENE" in
		shadow) CFG="$(noblur_cfg 1)" ;;
		*)      CFG="$(noblur_cfg 0)" ;;
		esac
		HL_ENV=""
		export HL_ENV
		hl_start "$CFG"
		hl_reset_spawn_colors 2>/dev/null || true
		case "$SCENE" in
		simple)   hl_spawn_kitty a >/dev/null; hl_wait_client_count 1 60 ;;
		many)     for i in $(seq 1 8); do hl_spawn_kitty "m$i" >/dev/null; done
		          hl_wait_client_count 8 120 ;;
		gradient) for i in $(seq 1 4); do hl_spawn_kitty "g$i" >/dev/null; done
		          hl_wait_client_count 4 120 ;;
		shadow)   for i in $(seq 1 3); do hl_spawn_kitty "s$i" >/dev/null
		              hl_wait_client_count "$i" 60; hl_dispatch toggle_floating
		              sleep 0.3; done ;;
		esac
		sleep "$WARMUP"
		hl_dispatch reset_avk_stats
		drive_full "$CYCLES"
		sleep 1
		L="$(read_stats)"
		hl_stop
		row "$SCENE (no blur)" "$L"
		echo "$L" > "$OUTDIR/baseline-$SCENE.txt"
		# THE DIRECT PATH IS STILL DIRECT. Not a performance claim -- a
		# structural one, and the reason the baseline is a baseline.
		hl_assert "$SCENE: no prefix captures" "$(val replays "$L")" 0
		hl_assert "$SCENE: no blur passes" "$(val bpasses "$L")" 0
		hl_assert "$SCENE: no blur chains" "$(val chains "$L")" 0
		hl_assert "$SCENE: no CPU waits" "$(val cpu_sync_waits "$L")" 0
		hl_assert "$SCENE: no fallback frames" "$(val fallback_frames "$L")" 0
	done
fi

# ── matrix: blur count ─────────────────────────────────────────────────────

if [ "$MODE" = "matrix" ]; then
	echo
	echo "── uncached blur, by blur count ──────────────────────────────────────"
	for N in ${COUNTS:-1 2 4 8}; do
		HL_ENV=""
		export HL_ENV
		hl_start "$(blur_cfg "${LEVELS:-3}" "${RADIUS:-5}")"
		for i in $(seq 1 "$N"); do
			hl_spawn_wlbgeffect "b$i" 300 "w$i-$N" >/dev/null
			hl_wait_client_count "$i" 60
		done
		sleep "$WARMUP"
		hl_dispatch reset_avk_stats
		drive_full "$CYCLES"
		sleep 1
		L="$(read_stats)"
		hl_stop
		row "$N blur window(s)" "$L"
		work_row "$N blur" "$L"
		echo "$L" > "$OUTDIR/matrix-$N.txt"
		hl_assert "$N blurs: the fixture actually blurred" \
			"$([ "$(val chains "$L")" -gt 0 ] && echo true || echo false)" true
		hl_assert "$N blurs: no CPU waits" "$(val cpu_sync_waits "$L")" 0
	done
fi

# ── params: levels, radius, scale ──────────────────────────────────────────

if [ "$MODE" = "params" ]; then
	echo
	echo "── one blur, varying the kernel and the scale ────────────────────────"
	for SPEC in "1 5 1" "2 5 1" "3 5 1" "4 5 1" "3 1 1" "3 12 1" "3 5 1.5"; do
		# shellcheck disable=SC2086
		set -- $SPEC
		LV="$1"; RD="$2"; SC="$3"
		HL_ENV=""
		HL_SCALE1="$SC"
		export HL_ENV HL_SCALE1
		hl_start "$(blur_cfg "$LV" "$RD")"
		hl_spawn_wlbgeffect one 300 "p-$LV-$RD-$SC" >/dev/null
		hl_wait_client_count 1 60
		sleep "$WARMUP"
		hl_dispatch reset_avk_stats
		drive_full "$CYCLES"
		sleep 1
		L="$(read_stats)"
		hl_stop
		row "levels=$LV r=$RD s=$SC" "$L"
		printf "  %-20s prefix=%s down=%s up=%s (us, p50; up sampled on %s frames)\n" \
			"" "$(us "$(val bp50 "$L")")" "$(us "$(val bd50 "$L")")" \
			"$(us "$(val bu50 "$L")")" "$(val bun "$L")"
		work_row "levels=$LV r=$RD" "$L"
		echo "$L" > "$OUTDIR/params-$LV-$RD-$SC.txt"
	done
fi

# ── workloads: the corrected matrix ────────────────────────────────────────
#
# ── THE FIXTURE BUG THIS EXISTS TO NOT REPEAT ─────────────────────────────
#
# The first version pulsed the BLUR PRODUCER'S OWN SURFACE. A blur samples what
# is BEHIND it -- the commands before it in scene order -- so changing the
# blurred window's own pixels is not a source change at all. Every SMALL and
# MEDIUM row came back with blur_chains=0 and source_damage=0, and the STATIC
# row passed trivially because an inert fixture satisfies "no work happened".
#
# So the pulsing surface is now a SEPARATE window BEHIND the blur producer, and
# the fixture asserts the intersection rather than assuming it.
#
#   back    a wlbgeffect window with WLBGEFFECT_PULSE: two buffers differing in
#           exactly one rectangle, damaging exactly that rectangle
#   front   the blur producer, overlapping it
#
# Every row proves the whole chain before it is timed:
#   pulse enabled -> source damage -> blur node touched -> result region ->
#   prefix rebuild -> filter work -> required work.

if [ "$MODE" = "workloads" ]; then
	echo
	echo "── workloads: prefix and filter efficiency, and what each strategy wins ──"
	for KIND in ${KINDS:-static small medium large move full}; do
		PULSE_GEOM=""
		case "$KIND" in
		static) ;;                                   # nothing changes at all
		small)  PULSE_GEOM="16x16@120,120:400" ;;
		medium) PULSE_GEOM="128x96@100,80:400" ;;
		large)  PULSE_GEOM="360x260@20,20:400" ;;
		esac
		HL_ENV=""
		export HL_ENV
		hl_start "$(blur_cfg "${LEVELS:-3}" "${RADIUS:-5}")"

		# THE PULSING WINDOW GOES FIRST, so it is BEHIND in scene order and is
		# therefore part of the blur's source. WLBGEFFECT_PULSE is exported for
		# this spawn only.
		if [ -n "$PULSE_GEOM" ]; then
			# SOURCE ONLY, no blur region of its own: a second chain in the
			# frame makes the up-phase and final-upsample timestamps
			# unattributable, which is why gpu_blur_up0 collected 0 samples.
			export WLBGEFFECT_PULSE="$PULSE_GEOM" WLBGEFFECT_NO_BLUR=1
			hl_spawn_wlbgeffect back 300 "back-$KIND" ff202020 >/dev/null
			hl_wait_client_count 1 60
			unset WLBGEFFECT_PULSE WLBGEFFECT_NO_BLUR
		fi
		hl_spawn_wlbgeffect front 300 "front-$KIND" >/dev/null
		hl_wait_client_count "$([ -n "$PULSE_GEOM" ] && echo 2 || echo 1)" 60
		sleep "$WARMUP"

		# ── PHASE A: QUALIFY THE FIXTURE ───────────────────────────────────
		# Counters over a short observation window, BEFORE the measured one, so
		# qualification frames never enter the performance distributions.
		hl_dispatch reset_avk_stats
		case "$KIND" in
		move)   hl_dispatch toggle_floating; sleep 0.5; drive_moves 4 ;;
		full)   drive_full 4 ;;
		*)      sleep 3 ;;
		esac
		Q="$(read_stats)"
		# The client's own record of what it did. `grep -c` alone would print
		# nothing on no-match under some shells, so the count is normalised.
		PULSES="$(grep "^wlbgeffect: pulse [01] " \
			"$OUTDIR/back-$KIND.log" 2>/dev/null | wc -l)"
		PULSE_LINE="$(grep -m1 "^wlbgeffect: pulse enabled" \
			"$OUTDIR/back-$KIND.log" 2>/dev/null || echo "(none)")"
		if [ "$KIND" != "static" ]; then
			WHY=""
			[ -n "$PULSE_GEOM" ] && [ "${PULSES:-0}" = "0" ] \
				&& WHY="$WHY pulse_not_observed"
			[ "$(val srcdmg "$Q")" != "0" ] || WHY="$WHY source_damage_zero"
			[ "$(val chains "$Q")" != "0" ] || WHY="$WHY blur_chain_zero"
			[ "$(val reqpx "$Q")" != "0" ] || WHY="$WHY required_zero"
			[ "$(val prpx "$Q")" != "0" ] || WHY="$WHY prefix_rebuild_zero"
			[ "$(val respx "$Q")" != "0" ] || WHY="$WHY result_zero"
			[ "$(val procpx "$Q")" != "0" ] || WHY="$WHY filter_work_zero"
			if [ -n "$WHY" ]; then
				echo
				echo "  ── $KIND ──"
				echo "  ROW INVALID:$WHY"
				echo "    (pulses=$PULSES srcdmg=$(val srcdmg "$Q") \
chains=$(val chains "$Q") req=$(val reqpx "$Q") \
rebuild=$(val prpx "$Q") res=$(val respx "$Q") proc=$(val procpx "$Q"))"
				hl_assert "$KIND: the fixture drove real blur work" "false" "true"
				hl_stop
				continue
			fi
		fi

		# ── PHASE B: PROFILE ───────────────────────────────────────────────
		hl_dispatch reset_avk_stats
		case "$KIND" in
		move)   drive_moves "$CYCLES" ;;
		full)   drive_full "$CYCLES" ;;
		*)      sleep $(( CYCLES / 3 )) ;;
		esac
		sleep 1
		L="$(read_stats)"
		hl_stop
		echo
		echo "  ── $KIND ──"
		if [ "$KIND" = "static" ]; then
			# AN ASSERTION, NOT A MEASUREMENT. With required = actual = 0 a
			# ratio would be a number about nothing.
			echo "  STATIC -- NO BLUR WORK: chains=$(val chains "$L") \
processed=$(val procpx "$L") frames=$(val frames "$L")"
			hl_assert "static: a settled desktop runs no blur filter work" \
				"$(val procpx "$L")" 0
			continue
		fi
		echo "  fixture: $PULSES pulse events; client said: ${PULSE_LINE:-(n/a)}"
		premises_ok "$KIND" "$L" blur || { hl_stop 2>/dev/null; continue; }
		row "$KIND" "$L"
		ratio_row "$KIND" "$L"
		strategy_row "$KIND" "$L"
		# UP0 ATTRIBUTION IS CONDITIONAL. The span between the penultimate
		# upsample's end and BLUR_END is the final upsample ONLY on a frame
		# with exactly one chain; with two it also contains the other chain.
		# Say so rather than print a number that is not what its name says.
		if [ "$(val maxchains "$L")" = "1" ] && [ "$(val up0n "$L")" != "0" ]; then
			printf "  %-20s up0 gpu p50 %s us over %s samples (single-chain frames)\n" \
				"" "$(us "$(val up050 "$L")")" "$(val up0n "$L")"
		else
			printf "  %-20s up0 UP0_NOT_ISOLATABLE (max chains/frame=%s, samples=%s)\n" \
				"" "$(val maxchains "$L")" "$(val up0n "$L")"
		fi
		printf "  %-20s up phase %s us over %s samples; region build p50 %s ns (%s below resolution); ring wait p95 %s us\n" \
			"" "$(us "$(val bu50 "$L")")" "$(val bun "$L")" \
			"$(val rb50 "$L")" "$(val rbund "$L")" "$(us "$(val rw95 "$L")")"
		echo "$L" > "$OUTDIR/workload-$KIND.txt"
	done
fi

echo
echo "logs: $OUTDIR"
hl_summary
