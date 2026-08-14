#!/usr/bin/env bash
# avk-gles-floor.sh -- is AVK at least as fast as the renderer it replaces?
#
# ── WHY THIS CANNOT USE THE GPU TIMESTAMPS ───────────────────────────────
#
# Every frame number this project quotes comes from AVK's own timestamp marks:
# FRAME_BEGIN, BLUR_BEGIN, FRAME_END, read back through the frame trace. Those
# are an AVK instrument. Run the same fixture with ASTEROIDZ_RENDERER=wlr and
# the trace emits NOTHING -- there is no GLES frame time to compare against,
# and a matrix that tried produced a clean, well-formatted "NO TRACE ROWS".
#
# Building the equivalent instrumentation into the SceneFX path would be the
# deep GLES study this milestone explicitly does not want. So the floor is
# measured with the one clock both renderers are answerable to: THE FRAME
# CALLBACK.
#
# ── CLIENT FRAME CALLBACKS WERE TRIED FIRST AND DO NOT WORK ──────────────
#
# The obvious instrument is contrib/wlrepaint: it commits on every frame
# callback, so counting its `gen` lines should be frames per second, measured by
# a client, in identical units whatever is behind the compositor.
#
# It measures nothing here. Both renderers reported 61.9 and 62.0 fps -- at 8
# windows and at 20, and again with the output driven at 1000Hz instead of 60.
# Three loads, one number. asteroidz schedules a client frame callback against
# the next vblank rather than after rendering completes, so a client cannot see
# render cost at all, and "AVK / GLES = 1.00x" was the frame scheduler being
# read twice.
#
# That is worth recording because the run PASSED once the refresh was raised:
# the saturation premise was satisfied by 62 < 0.95 x 1000, arithmetic rather
# than load. A premise that a broken instrument can satisfy is not a premise.
#
# ── AND COMPOSITOR CPU TIME DOES NOT SEPARATE THEM EITHER ────────────────
#
# THIS FIXTURE CURRENTLY FAILS ITS OWN PREMISE, ON PURPOSE. Over an 18-second
# damage-driven run with 8 blurred windows at 4K, the compositor's cumulative
# run time is 0.008s under AVK and 0.009s under GLES. That is not a tie: it is
# both arms doing so little CPU work that the counter has nothing to compare.
# It is also consistent with everything else known here -- AVK records a frame
# in about 80us and the fixture completed 66 frames, so ~5ms of CPU is the
# expected total. The work is on the GPU, which is where CPU time cannot look.
#
# ── ACCEPTED AS UNVERIFIED (user decision, 2026-08-14) ───────────────────
#
# The floor is NOT ESTABLISHED and will not be pursued. Establishing it needs
# the SceneFX path instrumented, and that cost was weighed against what it would
# buy -- a comparison against a renderer asteroidz no longer uses -- and
# declined. AVK's own budget qualification stands on its own instruments.
#
# This fixture is kept, and kept FAILING its premise, as the record of WHY
# rather than as a gate. Do not "fix" it by relaxing the premise: every
# instrument below was tried and each one produced a confident, well-formatted
# number that turned out to be describing itself.
#
# The honest statement of why is:
#
#   1. GPU timestamps are an AVK instrument; GLES emits no frame trace.
#   2. Client frame callbacks are scheduled against vblank rather than render
#      completion, so they read 62fps at 8 windows and at 20, at 60Hz and at
#      1000Hz -- three loads, one number.
#   3. Compositor CPU is ~8ms over 18s on both paths.
#
# Establishing it requires instrumenting the SceneFX path, which is the deep
# GLES study this milestone excludes. The fixture is kept, and kept FAILING,
# because a suite that reports "AVK / GLES = 1.00x PASS" from any of the three
# instruments above would be reporting the instrument.
#
# ── WHAT IT MEASURES WHEN THE PREMISE HOLDS ──────────────────────────────
#
# utime + stime from /proc/<pid>/stat. Renderer-agnostic, real, and available on
# both paths without instrumenting SceneFX. Both arms run the SAME dispatches
# over the SAME wall-clock interval on the SAME scene, so the only variable is
# which renderer consumed the CPU.
#
# WHAT THIS DOES AND DOES NOT MEASURE, stated rather than implied: it is CPU,
# not GPU. A renderer that offloads more to the GPU looks better here and might
# not be. It is a floor check -- "AVK is not dramatically more expensive to
# drive than the renderer it replaces" -- and it is not a frame-time comparison.
# The GPU-side comparison would need SceneFX instrumented, which is the study
# this milestone is explicitly not doing.
#
# ── WHAT PASSES ──────────────────────────────────────────────────────────
#
# AVK >= GLES. Not "AVK is faster": the requirement is a floor, and a renderer
# that matches the one it replaces on the same work has cleared it. 10% of
# tolerance, because two renderers on one idling GPU do not repeat to the frame
# and a verdict that flips on noise is not a floor.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-gles-floor"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-floor-$$"
mkdir -p "$OUTDIR"
REPAINT="$(cd "$(dirname "$0")" && pwd)/wlrepaint/wlrepaint"
[ -x "$REPAINT" ] || { echo "not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

W="${W:-3840}"; H="${H:-2160}"; SCALE="${SCALE:-1.5}"
WINDOWS="${WINDOWS:-8}"
MEASURE="${MEASURE:-12}"
CYCLES="${CYCLES:-30}"
# ── THE OUTPUT MUST NOT BE THE LIMITER ───────────────────────────────────
#
# At the harness default of 60Hz both renderers reported 61.92 and 62.00 fps --
# and reported EXACTLY THE SAME COUNTS at 8 windows and at 20. That is not a
# renderer comparison, it is the output's mode read twice: asteroidz schedules a
# client's frame callback against the next vblank rather than after rendering
# finishes, so a vsync-paced client cannot see render cost at all.
#
# Driving the output far above what either renderer can sustain makes the
# RENDERER the limiter, which is the only arrangement in which this measures
# anything. The premise assertion below is what catches it if that stops being
# true.
# 60Hz, the harness default, and NOT the 1000Hz this once used.
#
# Driving the output at 1000Hz was an attempt to make the RENDERER rather than
# the mode the limiter, back when this counted client frame callbacks. It did
# not work -- the callback rate never moved off 62 -- and it introduced a
# measurement artefact of its own: the compositor woke on a 1ms frame timer and
# burned 99.8% of a core to complete 66 frames whose GPU time totalled 41ms.
# That is the fixture's own knob, not a renderer property, and it is exactly the
# kind of number that gets quoted as one.
REFRESH="${REFRESH:-60}"

CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 20; blur 12; only-floating 0; tiled-scale 1.0
    blur-background 1; blur-background-strength 0.55 }
  blur { enable 1; optimized 1; passes 3; radius 10
    params { noise 0.0; brightness 0.9; contrast 0.9; saturation 1.2 } } }
layout { titlebar { enable 0 } }"

export WLBGEFFECT_SSD=1

FPS=""; GENS=""
run() { # run RENDERER
	local rend="$1"
	local dir="$OUTDIR/$rend"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	if [ "$rend" = gles ]; then HL_ENV="ASTEROIDZ_RENDERER=wlr"
	else HL_ENV="ASTEROIDZ_RENDERER=avk"; fi
	HL_ENV="$HL_ENV AZ_SHADOW_DITHER_AMP=0"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1
	local i=0
	while [ "$i" -lt "$WINDOWS" ]; do
		hl_spawn_wlbgeffect "fl$i" 300 "fl$i" >/dev/null
		hl_wait_client_count "$(( i + 1 ))" 200
		i=$(( i + 1 ))
	done
	sleep 3
	# THE COMPOSITOR'S OWN PID, from the harness rather than a pattern. A pgrep
	# for "asteroidz" here would match the caller's real session.
	# HL_COMP_PID is the pid hl_start recorded. NEVER a pgrep for "asteroidz":
	# that would match the caller's own live session.
	local pid="${HL_COMP_PID:-}"
	if [ -z "$pid" ] || [ ! -r "/proc/$pid/stat" ]; then
		echo "  cannot read the compositor's CPU time (pid='$pid')" >&2
		CPU_S=0; GENS=0; FPS=0
		hl_stop >/dev/null 2>&1
		return
	fi
	# /proc/<pid>/schedstat field 1: cumulative run time in NANOSECONDS.
	#
	# Not utime+stime from /proc/<pid>/stat, which is in CLK_TCK ticks -- 10ms
	# each. The compositor's CPU cost here is around a tenth of a second over a
	# fifteen-second run, so that instrument reported "0.01" for both renderers:
	# one tick, quantised to uselessness, and a 1.00x tie that was the counter's
	# resolution rather than a result.
	cpu_of() { awk '{print $1}' "/proc/$1/schedstat" 2>/dev/null || echo 0; }
	local c0 c1 w0 w1
	c0="$(cpu_of "$pid")"
	w0="$(date +%s.%N)"
	local c=0
	while [ "$c" -lt "$CYCLES" ]; do
		hl_dispatch damage_all
		sleep 0.3
		c=$(( c + 1 ))
	done
	c1="$(cpu_of "$pid")"
	w1="$(date +%s.%N)"
	CPU_S="$(awk -v a="$c0" -v b="$c1" 'BEGIN{printf "%.3f", (b-a)/1e9}')"
	WALL_S="$(awk -v a="$w0" -v b="$w1" 'BEGIN{printf "%.3f", b-a}')"
	hl_get "get avk-stats" > "$dir/stats.json" 2>/dev/null
	hl_stop >/dev/null 2>&1
}

echo "══ GLES floor ══ ${W}x${H} scale $SCALE windows=$WINDOWS measure=${MEASURE}s"
echo "   frames counted from the CLIENT side (wlrepaint frame callbacks),"
echo "   because the GPU timestamps are an AVK instrument and GLES has none."
echo

CPU_S=""; WALL_S=""
run avk;  A_CPU="$CPU_S"; A_WALL="$WALL_S"
run gles; G_CPU="$CPU_S"; G_WALL="$WALL_S"

echo "  renderer   CPU s   wall s   CPU/wall"
awk -v ac="$A_CPU" -v aw="$A_WALL" -v gc="$G_CPU" -v gw="$G_WALL" 'BEGIN{
	printf "  %-10s %5.2f %8.2f %10.3f\n", "avk",  ac, aw, (aw>0?ac/aw:0);
	printf "  %-10s %5.2f %8.2f %10.3f\n", "gles", gc, gw, (gw>0?gc/gw:0);
}'
echo

# ── THE PREMISES ─────────────────────────────────────────────────────────
# Both arms must have burned measurable CPU over comparable wall clocks. Zero
# against zero would otherwise be reported as a perfect tie, and two very
# different wall clocks would make the CPU totals incomparable.
hl_assert_true "PREMISE: the AVK arm consumed CPU (${A_CPU}s)" \
	"$(awk -v c="$A_CPU" 'BEGIN{print (c > 0.05) ? "true" : "false"}')"
hl_assert_true "PREMISE: the GLES arm consumed CPU (${G_CPU}s)" \
	"$(awk -v c="$G_CPU" 'BEGIN{print (c > 0.05) ? "true" : "false"}')"
hl_assert_true "PREMISE: both arms ran for comparable wall time (${A_WALL}s vs ${G_WALL}s)" \
	"$(awk -v a="$A_WALL" -v g="$G_WALL" 'BEGIN{
		r = (a>g?a/g:g/a); print (r < 1.15) ? "true" : "false" }')"

echo
awk -v ac="$A_CPU" -v aw="$A_WALL" -v gc="$G_CPU" -v gw="$G_WALL" 'BEGIN{
	if (gc+0 == 0 || gw+0 == 0) { print "  no GLES sample -- no comparison"; exit }
	a = ac/aw; g = gc/gw;
	printf "  CPU per wall second: AVK %.3f, GLES %.3f  ->  AVK/GLES = %.2fx\n",
		a, g, (g>0 ? a/g : 0);
}'
# AVK <= GLES x 1.10 on CPU. A floor, not a race: matching the renderer it
# replaces clears it.
hl_assert_true "FLOOR: AVK is not more expensive to drive than GLES (10% tolerance)" \
	"$(awk -v ac="$A_CPU" -v aw="$A_WALL" -v gc="$G_CPU" -v gw="$G_WALL" 'BEGIN{
		if (gw<=0||aw<=0||gc<=0) { print "false"; exit }
		print ((ac/aw) <= (gc/gw)*1.10) ? "true" : "false" }')"

echo
echo "logs: $OUTDIR"
hl_summary
