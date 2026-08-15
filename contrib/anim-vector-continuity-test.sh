#!/usr/bin/env bash
# anim-vector-continuity-test.sh -- does a retarget carry velocity on EVERY
# axis, or only along the direction it happens to be heading?
#
# P1. Before this, a retarget projected the outgoing velocity onto the new
# direction of travel and kept ONE scalar for x, y, width and height together.
# The component across that direction was dropped. This fixture drives a real
# retarget through a real compositor and measures, from the trace, whether each
# axis re-enters at the speed it left with.
#
# ── THE CONFIGURATION THAT DISCRIMINATES, AND THE TWO THAT DO NOT ─────────
#
# NOT a window moving +x redirected straight up. That is the obvious spelling
# of "90-degree retarget" and it proves nothing: the outgoing velocity is
# entirely in x, and the new segment has ZERO x span, so x cannot carry a
# velocity under ANY scheme -- its position is initial + 0*f(t) regardless.
# y had no outgoing velocity to preserve. Old and new implementations agree on
# every number and the fixture goes green against the broken code.
#
# NOT a window moving diagonally redirected diagonally. Then the projection is
# large and the scalar scheme looks nearly right.
#
# WHAT IS USED HERE: the window moves along +x ONLY, and is then redirected to
# a target that is both further right and further DOWN. So:
#
#   the outgoing velocity has NO y component whatsoever, and
#   the new segment has a large y span to spend one on.
#
# Per-axis seeding gives y a v0 of EXACTLY zero -- old_span_y is zero, so
# whatever the curve is doing, y's outgoing speed is zero and y starts from
# rest. The scalar scheme projects the x velocity onto a diagonal direction and
# then applies that one number to BOTH axes, so y is handed a velocity IT NEVER
# HAD and leaves the boundary already moving downward.
#
# That is the signal: not a velocity that vanishes, but one that appears out of
# nothing. It is measurable without knowing exactly where the window was when
# the retarget landed, which matters because a shell `sleep` cannot place the
# retarget precisely.
#
# ── WHY THIS FIXTURE DOES NOT RUN THE LIVE SPRING CONSTANTS ───────────────
#
# The live config is frequency 22 / damping 0.8 over a 500ms move, and that
# spring is FAST: it is 42% of the way to the target 30ms in, and one frame
# after a retarget it is already travelling at a third of its peak speed. Both
# schemes are then well away from rest at the first sample that exists, and the
# difference between "started from rest" and "started at the projected speed"
# shrinks to about 2.6x -- measurable, but close enough to sampling jitter to
# be an unconvincing gate.
#
# frequency 8 / damping 0.9 over 2000ms is the same physics with the same code
# path, sampled where the two answers are 13x apart instead. The property under
# test -- an axis with no outgoing velocity starts from rest -- is a property of
# the seeding arithmetic and holds at every setting; this one simply lets a
# 6ms sampling grid see it. tests/test-anim-spring.c sweeps the whole permitted
# damping and frequency range for the arithmetic itself.
set -u

# Never let amsg find the operator's real compositor. hl_dispatch sets the
# signature per call, but an inherited one in this shell is a live session one
# command away, and that has gone wrong before.
unset ASTEROIDZ_INSTANCE_SIGNATURE

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="anim-vector-continuity"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-anim-vc-$$"
mkdir -p "$OUTDIR"

# The geometry. Purely horizontal first leg; the retarget adds the y span.
START_X=200
START_Y=400
LEG1_X=1600          # same y: the first leg has NO vertical motion at all
RETARGET_X=1500
RETARGET_Y=900       # +500 of brand-new vertical span
# 120ms into a 2000ms move: u = 0.06, near the spring's peak speed.
RETARGET_AFTER=0.12

ANIM_CFG='animations {
	curve spring
	spring { damping 0.9; frequency 8 }
	animation_duration_move 2000
	window-open { type zoom; duration 200 }
	window-close { type fall; duration 350 }
}
layer_animations 0'

run_case() { # run_case NAME [EXTRA_ENV...] -> echoes the trace path
	local name="$1"; shift
	local dir="$OUTDIR/$name"
	mkdir -p "$dir"

	HL_OUTDIR="$dir"
	HL_OUTPUTS=1
	HL_HZ1=144
	# avk is the renderer the live session runs; the animation engine is shared
	# but there is no reason to measure the one nobody uses.
	HL_ENV="ASTEROIDZ_RENDERER=${VC_RENDERER:-avk} AZ_PACE=1 $*"
	export HL_OUTDIR HL_OUTPUTS HL_HZ1 HL_ENV

	hl_start "effects { blur { enable 0 }; shadow { enable 0 } }
$ANIM_CFG"

	# The hold is SECONDS, and it must outlast the whole workload. An empty
	# value here left a client that went away before the first dispatch, so
	# every move landed on nothing and the trace contained only the startup
	# animations -- which reads exactly like "the retarget never happened".
	local pid
	pid="$(hl_spawn_wlbgeffect "vc" 60 "w1")"
	hl_wait_client_count 1
	sleep 1

	hl_dispatch toggle_floating 1
	# Settle at the start position, so the first leg begins from rest and its
	# outgoing velocity is the spring's alone.
	hl_dispatch "move_window,$START_X,$START_Y" 1
	sleep 1

	local mark_ns
	mark_ns="$(python3 -c 'import time; print(time.clock_gettime_ns(time.CLOCK_MONOTONIC))')"

	# The retarget: leg one starts, and 120ms later a new target lands while
	# the window is still travelling.
	hl_dispatch "move_window,$LEG1_X,$START_Y" 0
	sleep "$RETARGET_AFTER"
	hl_dispatch "move_window,$RETARGET_X,$RETARGET_Y" 0
	sleep 3

	kill "$pid" 2>/dev/null || true
	local log="$dir/state/asteroidz/asteroidz.log"
	grep azpace "$log" > "$dir/trace.txt" 2>/dev/null || true
	echo "$mark_ns" > "$dir/mark_ns"
	hl_stop
	echo "$dir"
}

# ── the analyser ─────────────────────────────────────────────────────────
#
# Reads the trace, finds the retarget boundary, and finite-differences the
# REAL-VALUED ideal position on each side of it. `ideal` is used rather than
# `geom` deliberately: geom is rounded to whole pixels, and at a 6ms sample
# spacing the quotient of a rounded series is dominated by the rounding.
analyse() { # analyse DIR
	python3 - "$1" <<'PY'
import re, sys, json

d = sys.argv[1]
mark = int(open(d + "/mark_ns").read().strip())

TICK = re.compile(
    r"azpace anim tick c=(\S+) mon=(\S+) action=(\d+) dur=(\d+) t_ms=(-?\d+) "
    r"lin=(\S+) factor=(\S+) ideal=(\S+),(\S+),(\S+)x(\S+) "
    r"geom=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) t_ns=(\d+)")
START = re.compile(
    r"azpace anim start c=(\S+) action=(\d+) dur=(\d+) retarget=(\d+) "
    r"from=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) to=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) "
    r"cur=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) t_ns=(\d+)")

ticks, starts = [], []
for line in open(d + "/trace.txt", errors="replace"):
    m = TICK.search(line)
    if m:
        t_ns = int(m.group(16))
        if t_ns >= mark:
            ticks.append({"c": m.group(1), "t": t_ns,
                          "x": float(m.group(8)), "y": float(m.group(9))})
        continue
    m = START.search(line)
    if m:
        t_ns = int(m.group(17))
        if t_ns >= mark:
            starts.append({"c": m.group(1), "retarget": int(m.group(4)),
                           "t": t_ns,
                           "to_x": int(m.group(9)), "to_y": int(m.group(10))})

out = {"ticks": len(ticks), "starts": len(starts)}

rt = [s for s in starts if s["retarget"] == 1]
if not rt:
    out["error"] = "no retarget boundary in the trace"
    print(json.dumps(out)); sys.exit(0)
b = rt[0]
out["boundary_ns"] = b["t"]
out["boundary_to"] = [b["to_x"], b["to_y"]]

series = sorted([t for t in ticks if t["c"] == b["c"]], key=lambda t: t["t"])
pre = [t for t in series if t["t"] < b["t"]]
post = [t for t in series if t["t"] >= b["t"]]
out["pre_ticks"], out["post_ticks"] = len(pre), len(post)
if len(pre) < 3 or len(post) < 3:
    out["error"] = "too few samples either side of the boundary"
    print(json.dumps(out)); sys.exit(0)

def vel(a, b_):
    dt = (b_["t"] - a["t"]) / 1.0e6      # ms
    if dt <= 0:
        return None
    return ((b_["x"] - a["x"]) / dt, (b_["y"] - a["y"]) / dt)

# OUTGOING: the last complete step before the boundary.
vx_pre, vy_pre = vel(pre[-2], pre[-1])
# INCOMING: the first complete step after it. This is the instant that
# separates "started from rest" from "started at the projected speed"; a step
# taken later has had time for both to accelerate and tells you less.
vx_post, vy_post = vel(post[0], post[1])

out["v_pre"] = [vx_pre, vy_pre]
out["v_post"] = [vx_post, vy_post]
# The scale y motion eventually reaches, so "small" can be said relative to
# something the same fixture measured rather than to a constant.
vy_peak = max(abs(vel(post[i], post[i + 1])[1])
              for i in range(len(post) - 1))
out["vy_peak"] = vy_peak
out["vy_post_frac_of_peak"] = abs(vy_post) / vy_peak if vy_peak else None
out["vx_retained"] = abs(vx_post) / abs(vx_pre) if vx_pre else None
print(json.dumps(out))
PY
}


# ── the thresholds ───────────────────────────────────────────────────────
#
# y leaves the boundary FROM REST, so its first post-boundary step is a small
# fraction of the speed y later reaches under its own spring. Measured: 0.089
# with per-axis seeding, 0.777 with the scalar. 0.25 sits an order of magnitude
# clear of the first and well clear of the second.
VY_FRAC_MAX="${VY_FRAC_MAX:-0.25}"
# x re-enters at the speed it left with. Generous, because the two samples
# straddle a boundary the spring is curving through. Measured: 1.047.
VX_RETAIN_MIN="${VX_RETAIN_MIN:-0.70}"
VX_RETAIN_MAX="${VX_RETAIN_MAX:-1.30}"

# measure DIR -> sets M_VYFRAC, M_VXRET, M_PREMISE ("ok" or a reason)
measure() {
	local j; j="$(analyse "$1")"
	M_VYFRAC=""; M_VXRET=""; M_PREMISE=""
	eval "$(python3 - "$j" <<'PY'
import json, sys
j = json.loads(sys.argv[1])
def sh(k, v): print("%s=%s" % (k, v))
if "error" in j:
    sh("M_PREMISE", "'" + j["error"] + "'"); sys.exit(0)
vx_pre, vy_pre = j["v_pre"]
vx_post, vy_post = j["v_post"]
sys.stderr.write("    v_pre  = %.4f, %.4f px/ms\n" % (vx_pre, vy_pre))
sys.stderr.write("    v_post = %.4f, %.4f px/ms\n" % (vx_post, vy_post))
sys.stderr.write("    vy_peak= %.4f  vy_post/peak = %.4f  vx_retained = %.4f\n"
                 % (j["vy_peak"], j["vy_post_frac_of_peak"], j["vx_retained"]))
# THE PREMISES. Each one, if unmet, makes a pass meaningless rather than wrong.
if abs(vx_pre) < 0.2:
    sh("M_PREMISE", "'the window was barely moving before the retarget "
       "(vx_pre=%.4f)'" % vx_pre); sys.exit(0)
if abs(vy_pre) > 0.05 * abs(vx_pre):
    sh("M_PREMISE", "'the first leg had vertical motion (vy_pre=%.4f) -- not "
       "the axis-isolated case this fixture needs'" % vy_pre); sys.exit(0)
if j["vy_peak"] < 0.2:
    sh("M_PREMISE", "'y never moved after the retarget (peak %.4f px/ms)'"
       % j["vy_peak"]); sys.exit(0)
sh("M_PREMISE", "ok")
sh("M_VYFRAC", "%.6f" % j["vy_post_frac_of_peak"])
sh("M_VXRET", "%.6f" % j["vx_retained"])
PY
)"
}

# y_from_rest -> "true" if y left the boundary at rest
y_from_rest() {
	python3 -c "import sys; print('true' if $M_VYFRAC <= $VY_FRAC_MAX else 'false')"
}
x_continuous() {
	python3 -c "import sys; print('true' if $VX_RETAIN_MIN <= $M_VXRET <= $VX_RETAIN_MAX else 'false')"
}

MODE="${1:-both}"

# ── 1/2 the production build ─────────────────────────────────────────────
if [ "$MODE" = green ] || [ "$MODE" = both ]; then
	echo "=== per-axis seeding (production) -- must PASS ==="
	measure "$(run_case green)"
	hl_assert "PREMISE: the fixture measured a usable retarget" "$M_PREMISE" "ok"
	if [ "$M_PREMISE" = ok ]; then
		hl_assert_true "per-axis: y starts from rest ($M_VYFRAC <= $VY_FRAC_MAX)" "$(y_from_rest)"
		hl_assert_true "per-axis: x re-enters at its outgoing speed ($M_VXRET)" "$(x_continuous)"
	fi
fi

# ── 2/2 the falsifier ────────────────────────────────────────────────────
#
# A green run alone proves nothing: an assertion that cannot fail passes just
# as happily. The same fixture is run against AZ_BREAK_ANIM_SPRING_SCALAR_V0,
# which restores the projected scalar, and the y assertion MUST go red. If it
# does not, the assertion is not measuring what it claims and this suite fails
# even though nothing "broke".
if [ "$MODE" = break ] || [ "$MODE" = both ]; then
	echo
	echo "=== AZ_BREAK_ANIM_SPRING_SCALAR_V0 -- the y assertion must go RED ==="
	measure "$(run_case break AZ_BREAK_ANIM_SPRING_SCALAR_V0=1)"
	hl_assert "PREMISE: the broken arm also measured a usable retarget" "$M_PREMISE" "ok"
	if [ "$M_PREMISE" = ok ]; then
		# The break must NOT satisfy "y starts from rest".
		hl_assert_true "BREAK: the scalar seeding is DETECTED -- y left the boundary already moving ($M_VYFRAC > $VY_FRAC_MAX)" \
			"$([ "$(y_from_rest)" = false ] && echo true || echo false)"
	fi
fi

echo
echo "logs: $OUTDIR"
hl_summary
