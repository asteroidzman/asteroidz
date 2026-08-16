#!/usr/bin/env bash
# avk-graph-perf.sh — M4E.5: what the graph costs on scenes that are not the
# minimum.
#
# WHY SEPARATE FROM avk-graph-test.sh. That suite asserts correctness on one
# fixture. This one asks a different question -- "how much does the
# infrastructure cost when no multipass effect is active" -- and it has to ask
# it on more than one scene, because the answer depends on how many resources a
# frame declares. A frame with one window declares two; a frame with sixteen
# declares seventeen, and the graph's per-frame work is linear in that.
#
# Every scene is run on BOTH binaries in the same invocation, alternating, so
# that machine state, thermal drift and background load land on both rather than
# on whichever went second.
#
#   ASTEROIDZ_PREGRAPH=<path>   required. Without it there is nothing to compare
#                               against and the script says so rather than
#                               printing one column and calling it a result.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-graph-perf"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-graphperf-$$"
HL_OUTDIR="$OUTDIR"
HL_ENV="ASTEROIDZ_RENDERER=avk"
export HL_OUTDIR HL_ENV

PREGRAPH="${ASTEROIDZ_PREGRAPH:-}"
mkdir -p "$OUTDIR"
trap 'hl_stop; rm -rf "$OUTDIR"' EXIT

echo "== avk graph perf (M4E.5) =="

if [ -z "$PREGRAPH" ] || [ ! -x "$PREGRAPH" ]; then
	echo "ASTEROIDZ_PREGRAPH is not set to an executable." >&2
	echo "Nothing to compare against; refusing to print one column." >&2
	exit 1
fi

# ── scenes ─────────────────────────────────────────────────────────────────
#
# Four, chosen because they stress different parts of the frame:
#
#   idle           one window, nothing moving -- the floor
#   many           16 windows -- resource declarations, the graph's linear part
#   gradient       gradient borders -- the storage-buffer path. NOTE that
#                  `border_gradient 1` triggers a known repaint storm
#                  (client_set_border_fill() has no dirty check, so it damages
#                  the border node every tick); it reproduces on both renderers
#                  and on pre-M4C builds. It is used here anyway,
#                  because it is the only way to get gradients into the
#                  renderer headlessly -- which makes this scene a stress case
#                  rather than a calm one, and its absolute numbers should be
#                  read as such. The comparison is still valid: both binaries
#                  storm identically.
#   shadow         floating windows with shadows -- fragment-bound work
scene_idle() {
	hl_spawn_kitty perf-1 >/dev/null
	hl_wait_client_count 1
}

scene_many() {
	local i
	for i in $(seq 1 16); do
		hl_spawn_kitty "perf-$i" >/dev/null
	done
	hl_wait_client_count 16 120
}

scene_gradient() {
	local i
	for i in $(seq 1 8); do
		hl_spawn_kitty "perf-$i" >/dev/null
	done
	hl_wait_client_count 8 120
}

scene_shadow() {
	local i
	for i in $(seq 1 6); do
		hl_spawn_kitty "perf-$i" >/dev/null
		hl_wait_client_count "$i" 60
		hl_dispatch "toggle_floating" 0.3
		hl_dispatch "move_window,exact $((60 + i * 90)) $((40 + i * 70))" 0.3
	done
}

# Gradient borders need config; the others use the harness default.
kdl_for() {
	case "$1" in
	gradient) printf 'border_gradient 1\nborder_gradient_degree 45\n' ;;
	shadow)   printf 'shadows 1\nshadows_size 48\nshadows_blur 48\nshadow_only_floating 1\n' ;;
	*)        printf '' ;;
	esac
}

# One measured run. Prints "record_us cpu_p50 cpu_p95 gpu_us passes resources barriers build_ns".
run_scene() { # run_scene BINARY SCENE
	local bin="$1" scene="$2"
	hl_reset_spawn_colors
	export HL_KITTY_EXTRA="-o cursor_blink_interval=0"
	HL_ASTEROIDZ="$bin" HL_EXTRA_KDL="$(kdl_for "$scene")" hl_start >/dev/null 2>&1
	if [ "$(hl_binary)" != "$bin" ]; then
		echo "run_scene: asked for $bin, started $(hl_binary)" >&2
		exit 1
	fi
	"scene_$scene"
	sleep 2
	# NOT reset_avk_stats. The first version zeroed the counters and then
	# measured an eight-dispatch interval -- which produced too few frames for
	# the 20us-bucket histogram to report anything but 0, in both columns, so
	# the comparison was between two zeroes. The run-long average includes
	# startup, which is the same for both binaries and therefore cancels.
	local i
	for i in 1 2 3 4 5 6 7 8; do
		hl_dispatch "focusstack,1" 0.25 >/dev/null 2>&1 || true
	done
	sleep 2
	local s
	s="$(hl_get 'get avk-stats')"
	hl_stop >/dev/null 2>&1
	echo "$s" | jq -r '[(.record_us_avg // 0), (.cpu_frame_us_p50 // 0),
		(.cpu_frame_us_p95 // 0), (.gpu_frame_us_avg // 0),
		(.graph_passes // 0), (.graph_resources // 0), (.graph_barriers // 0),
		(.graph_build_ns_avg // 0), (.frames // 0)] | @tsv'
}

printf '\n%-10s %-6s %9s %8s %8s %10s %7s %6s %5s %10s\n' \
	scene build record_us p50 p95 gpu_us passes res barr build_ns

FAIL=0
for scene in idle many gradient shadow; do
	# Alternating, not old-then-new: whichever runs second inherits whatever the
	# machine was doing during the first, and a comparison that always puts one
	# binary there is measuring the order.
	read -r O_REC O_P50 O_P95 O_GPU O_PASS O_RES O_BAR O_BUILD O_FR \
		< <(run_scene "$PREGRAPH" "$scene")
	read -r N_REC N_P50 N_P95 N_GPU N_PASS N_RES N_BAR N_BUILD N_FR \
		< <(run_scene "$HL_REPO/build/asteroidz" "$scene")

	printf '%-10s %-6s %9.1f %8s %8s %10.1f %7s %6s %5s %10s\n' \
		"$scene" pre "$O_REC" "$O_P50" "$O_P95" "$O_GPU" "-" "-" "-" "-"
	printf '%-10s %-6s %9.1f %8s %8s %10.1f %7s %6s %5s %10.0f\n' \
		"" graph "$N_REC" "$N_P50" "$N_P95" "$N_GPU" "$N_PASS" "$N_RES" \
		"$N_BAR" "$N_BUILD"

	# ONE pass on every scene. This is the requirement -- a frame with no
	# multipass effect must not have become a pipeline, however many windows it
	# contains.
	hl_assert "$scene: still one pass" "$N_PASS" "1"
	hl_assert "$scene: still two barrier calls" "$N_BAR" "2"
	# No threshold was picked before the numbers. What is asserted is that graph
	# construction stays microseconds -- a scene-dependent cost that had become
	# a millisecond would show here and nowhere else.
	hl_assert "$scene: graph build under 50us" \
		"$(awk -v b="$N_BUILD" 'BEGIN{print (b < 50000) ? "yes" : "no (" b ")"}')" "yes"
	# Compared on keys BOTH binaries expose. record_us_avg is an M4E field and
	# the pre-graph build has none, so it is reported in the table for the new
	# column and compared against nothing -- saying "no-baseline" out loud
	# rather than treating a missing key as a zero and declaring a win.
	hl_assert "$scene: CPU frame p50 within 2x of pre-graph" \
		"$(awk -v o="$O_P50" -v n="$N_P50" 'BEGIN{
			if (o+0 <= 0) print "no-baseline";
			else print (n <= o * 2) ? "yes" : "no (" n " vs " o ")" }')" "yes"
	hl_assert "$scene: GPU frame within 2x of pre-graph" \
		"$(awk -v o="$O_GPU" -v n="$N_GPU" 'BEGIN{
			if (o+0 <= 0) print "no-baseline";
			else print (n <= o * 2) ? "yes" : "no (" n " vs " o ")" }')" "yes"
done

hl_summary
