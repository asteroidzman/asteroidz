#!/usr/bin/env bash
# anim-pace-test.sh -- is the animation engine slow, or is the renderer?
#
# Runs the SAME production animations under a trace (AZ_PACE=1, see
# src/common/pace.h) across a matrix of refresh rates and scene loads, and
# hands each log to contrib/pace-analyse.py.
#
# ── WHAT THE FIXTURE HAS TO GET RIGHT ─────────────────────────────────────
#
# THE ENGINE'S CONFIGURATION IS PART OF THE MEASUREMENT. The interpolator is
# a spring evaluated over NORMALISED time, so `spring frequency` and
# `animation_duration_move` are not independent: the frequency says how many
# radians the spring travels per configured duration, not per second. A
# fixture running the schema defaults would be measuring a different engine
# from the one the user is complaining about. So the animation block is copied
# from the live config, and the harness prints it.
#
# TWO REFRESH RATES, NOT ONE. At equal rates the frame budget, the present
# interval and the tick interval all coincide, so an animation that secretly
# counted frames would finish in the same wall-clock time as one that read a
# clock. They have to differ for the question to have an answer.
#
# THE NOMINAL RATE IS NOT THE OBSERVED RATE. The headless backend's frame
# timer is whole milliseconds, so 144 free-runs at 1000/6 = 166.7Hz and 60 at
# 1000/16 = 62.5Hz. The analyser classifies against the observed period for
# exactly this reason; the numbers here are still two clearly different
# cadences, which is all the refresh-independence question needs.
#
# BLUR IS A SEPARATE AXIS FROM ANIMATION. If the no-blur run is already uneven
# the scheduler is implicated; if only the blurred runs are, the renderer is.
# One fixture cannot tell them apart, so there are three.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="anim-pace"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-anim-pace-$$"
mkdir -p "$OUTDIR"

ANALYSE="$(dirname "$0")/pace-analyse.py"
WINDOWS="${WINDOWS:-1}"
BLUR="${BLUR:-0}"
# `optimized` caches the bottom layer's blur. It is ON in the live config, and
# it changes what a moving window damages -- a cached layer that has to be
# rebuilt is a different damage shape from one blurred in place. Whether that
# is the amplifier is a question the fixture has to be able to ask.
OPTIMIZED="${OPTIMIZED:-1}"
HZ1="${HZ1:-144}"
HZ2="${HZ2:-60}"
OUTS="${OUTS:-2}"
HOLD="${HOLD:-60}"
# render-late is ON in the live config and is an adaptive controller with a
# documented failure mode (a frac small enough that it stops arming and can
# never climb out). A pacing investigation that left it at the schema default
# would be measuring a scheduler the user does not run.
RENDER_LATE="${RENDER_LATE:-0}"
KEEP="${KEEP:-0}"

# Copied verbatim from the live ~/.config/asteroidz/config.kdl. A duration
# here that disagrees with the one the user runs makes every number below
# describe a different compositor.
ANIM_CFG='animations {
	curve spring
	spring { damping 0.8; frequency 22 }
	window-open { type zoom; duration 200; fade-begin-opacity 1.0 }
	window-close { type fall; duration 350; fade-begin-opacity 1.0 }
}
zoom_initial_ratio 0.92
layer_animations 0'

blur_cfg() {
	if [ "$BLUR" = "0" ]; then
		echo 'effects { blur { enable 0 }; shadow { enable 0 } }'
	else
		echo 'effects {
			blur { enable 1; optimized '"$OPTIMIZED"'; passes 2; radius 6
				params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } }
			shadow { enable 0 } }'
	fi
}

run_case() { # run_case NAME WORKLOAD_FN
	local name="$1" workload="$2"
	local dir="$OUTDIR/$name"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"
	HL_OUTPUTS="$OUTS"
	HL_HZ1="$HZ1"
	HL_HZ2="$HZ2"
	# ASTEROIDZ_RENDERER=avk, ALWAYS. Without it the compositor composites
	# through scenefx/GLES, which has an entirely different blur damage path
	# (apply_blur_region and the saved-pixels compensation) from the one the
	# live session runs. A pacing run on the wrong renderer measures a
	# compositor the user does not have; the first pass of this investigation
	# did exactly that and its damage figures were discarded.
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_PACE=1 ${HL_EXTRA_ENV:-}"
	export HL_OUTDIR HL_OUTPUTS HL_HZ1 HL_HZ2 HL_ENV

	hl_start "$(blur_cfg)
$ANIM_CFG
render-late $RENDER_LATE
render-late-margin-us 3000"

	local pids=() i
	for i in $(seq 1 "$WINDOWS"); do
		pids+=("$(hl_spawn_wlbgeffect "pace$i" "$HOLD" "w$i")")
		hl_wait_client_count "$i"
	done
	sleep 1

	# The trace is only meaningful from here: everything above is startup.
	echo "=== $name (windows=$WINDOWS blur=$BLUR hz=$HZ1/$HZ2 outs=$OUTS render_late=$RENDER_LATE) ===" >&2
	# CLOCK_MONOTONIC, not `date +%s%N`. The trace's t_ns is monotonic (see
	# az_pace_now_ns), and a realtime mark is ~55 years larger, so --since
	# would silently discard the entire trace and print three empty sections
	# that look exactly like "the compositor never animated".
	local mark_ns
	mark_ns="$(python3 -c 'import time; print(time.clock_gettime_ns(time.CLOCK_MONOTONIC))')"
	"$workload"
	sleep 1

	local log="$dir/state/asteroidz/asteroidz.log"
	grep azpace "$log" > "$dir/trace.txt" 2>/dev/null || true
	# --since drops startup: every window's OPEN animation, the first
	# full-damage frame, the layer arrivals. None of that is the workload.
	python3 "$ANALYSE" "$dir/trace.txt" "--since=$mark_ns"
	python3 "$ANALYSE" "$dir/trace.txt" "--since=$mark_ns" --json \
		> "$dir/report.json"

	for i in "${pids[@]}"; do kill "$i" 2>/dev/null || true; done
	hl_stop
}

# ── workloads ────────────────────────────────────────────────────────────
wl_move() {
	hl_dispatch toggle_floating 1
	local n
	for n in 1 2 3 4 5; do
		hl_dispatch move_window,1200,600 1
		hl_dispatch move_window,200,100 1
	done
}

wl_resize() {
	hl_dispatch toggle_floating 1
	local n
	for n in 1 2 3; do
		hl_dispatch resize_window,1400,900 1
		hl_dispatch resize_window,500,400 1
	done
}

wl_tag() {
	local n
	for n in 1 2 3 4 5; do
		hl_dispatch view,2 1
		hl_dispatch view,1 1
	done
}

wl_focus() {
	local n
	for n in $(seq 1 10); do
		hl_dispatch focus_stack,next 1
	done
}

# Interrupt: a new target lands 60ms into a 500ms move, five times over.
wl_retarget() {
	hl_dispatch toggle_floating 1
	local n
	for n in 1 2 3 4 5; do
		hl_dispatch move_window,1200,600 0
		sleep 0.06
		hl_dispatch move_window,200,100 0
		sleep 0.06
		hl_dispatch move_window,900,300 1
	done
}

# The overview open/close chrome fade, which is driven from render_monitor
# (m->ov_anim_running) rather than from a client animation, so it exercises a
# second scheduler path entirely.
wl_overview() {
	local n
	for n in 1 2 3 4 5; do
		hl_dispatch toggle_overview 1
		hl_dispatch toggle_overview 1
	done
}

# Cross-output: the window is dragged from output 1 into output 2's half of
# the layout and back. Only meaningful with OUTS=2.
wl_cross() {
	hl_dispatch toggle_floating 1
	local n
	for n in 1 2 3; do
		hl_dispatch move_window,2400,300 1
		hl_dispatch move_window,300,300 1
	done
}

WORKLOADS="${WORKLOADS:-move resize tag focus retarget cross overview}"

for w in $WORKLOADS; do
	case "$w" in
	move) run_case "move" wl_move ;;
	resize) run_case "resize" wl_resize ;;
	tag) run_case "tag" wl_tag ;;
	focus) run_case "focus" wl_focus ;;
	retarget) run_case "retarget" wl_retarget ;;
	cross) run_case "cross" wl_cross ;;
	overview) run_case "overview" wl_overview ;;
	*) echo "unknown workload: $w" >&2; exit 1 ;;
	esac
done

echo
echo "logs: $OUTDIR"
[ "$KEEP" = "1" ] || true
