#!/usr/bin/env bash
# anim-shatter-test.sh -- does the shatter close cost what it should, and does
# it finish at the same moment on two very different outputs?
#
# P3's two COMPOSITOR-level gates. The other three -- purity replay, gravity
# recovered from the trajectory, and the speed bound -- are statements about
# arithmetic and live in tests/test-anim-shatter.c, where they can be swept
# without a display. These two cannot: they are about what the compositor does
# with that arithmetic.
#
#   REFRESH INDEPENDENCE (I4's pattern). The same close, on two outputs at very
#   different refresh rates, must take the same WALL-CLOCK time. A break-up
#   that advanced once per frame instead of reading the clock would finish in
#   proportion to how fast -- and how often -- it was ticked, and rendermon
#   ticks every client on EVERY output's pass, so a two-output layout steps it
#   at the sum of the two rates.
#
#   DAMAGE BOUNDED BY THE CLOUD (I14's pattern, ADR-613). Every frame's damage
#   must fit inside the fragment cloud's bounding box, with a margin, and must
#   never be the whole output. Blur move damage is already amplified 6.7-10x on
#   this compositor; a close animation that quietly repainted the screen 30
#   times would be invisible on screen and expensive everywhere.
#
# ── WHAT THE FIXTURE HAS TO GET RIGHT ────────────────────────────────────
#
# THE RENDERER. shatter needs AVK: its fragments rotate, and rotation is an
# AVK_CMD_TEXTURE_QUAD, which the SceneFX path does not have. On that path the
# close silently becomes `fall` -- the same pixels, no rotation -- so a run on
# the wrong renderer measures a DIFFERENT ANIMATION and passes for the wrong
# reason. ASTEROIDZ_RENDERER=avk, and the premise below asserts a shatter
# actually ran by looking for its trace lines.
#
# THE NOMINAL RATE IS NOT THE OBSERVED RATE. The headless backend's frame timer
# is whole milliseconds, so 144 free-runs at 1000/6 = 166.7Hz and 60 at
# 1000/16 = 62.5Hz. That does not matter here -- the two are still clearly
# different cadences, which is all a refresh-independence question needs -- but
# it is why nothing below compares against 144 or 60 as numbers.
set -u

# hl_dispatch sets the signature per call, but an inherited one in this shell
# is one command away from the operator's real session.
unset ASTEROIDZ_INSTANCE_SIGNATURE

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="anim-shatter"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-shatter-$$"
mkdir -p "$OUTDIR"

# A long close, so there are plenty of ticks to measure on both outputs.
ANIM_CFG='animations {
	curve spring
	spring { damping 0.9; frequency 8 }
	window-open { type zoom; duration 200 }
	window-close { type shatter; duration 700; shatter-fragments 6 }
}
layer_animations 0'

run_case() { # run_case NAME [EXTRA_ENV...] -> echoes the log dir
	local name="$1"; shift
	local dir="$OUTDIR/$name"
	mkdir -p "$dir"

	HL_OUTDIR="$dir"
	HL_OUTPUTS="${SHATTER_OUTS:-2}"
	HL_HZ1=144
	HL_HZ2=60
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_PACE=1 $*"
	export HL_OUTDIR HL_OUTPUTS HL_HZ1 HL_HZ2 HL_ENV

	hl_start "effects { blur { enable 0 }; shadow { enable 0 } }
$ANIM_CFG"

	local pid
	pid="$(hl_spawn_wlbgeffect "shat" 60 "w1")"
	hl_wait_client_count 1
	sleep 1

	# ── THE WINDOW MUST NOT FILL ITS OUTPUT ──────────────────────────────
	#
	# The first version of this fixture left the window TILED, so it covered
	# HEADLESS-2 entirely and its fragment cloud was the whole monitor from the
	# first tick. "Damage fits inside the cloud" was then true and meaningless:
	# the cloud WAS the output, and a full-output repaint satisfied it. The
	# assertion passed 3/3 while the trace showed damage_px exactly 2073600 --
	# a gate that could not fail.
	#
	# Floating and small is what makes the claim falsifiable: the cloud is a
	# fraction of the screen, so a full-output repaint is visibly outside it.
	hl_dispatch toggle_floating 0.5
	hl_dispatch resize_window,600,400 0.5
	hl_dispatch move_window,600,300 1

	local mark_ns
	mark_ns="$(python3 -c 'import time; print(time.clock_gettime_ns(time.CLOCK_MONOTONIC))')"
	echo "$mark_ns" > "$dir/mark_ns"

	# Close it, and let the whole animation play out.
	hl_dispatch kill_client 0
	sleep 3

	kill "$pid" 2>/dev/null || true
	grep azpace "$dir/state/asteroidz/asteroidz.log" > "$dir/trace.txt" \
		2>/dev/null || true
	hl_stop
	echo "$dir"
}

analyse() { # analyse DIR -> one JSON line
	python3 - "$1" <<'PY'
import re, sys, json

d = sys.argv[1]
mark = int(open(d + "/mark_ns").read().strip())

SHAT = re.compile(
    r"azpace shatter tick c=(\S+) t=(\S+) live=(\d+)/(\d+) "
    r"aabb=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) opacity=(\S+) t_ns=(\d+)")
RENDER = re.compile(
    r"azpace render mon=(\S+) dur_us=(\d+) needed=(\d+) committed=(\d+) "
    r"more=(\d+) damage_px=(\d+) damage_rects=(\d+) "
    r"damage_ext=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) t_ns=(\d+)")

ticks, renders = [], []
for line in open(d + "/trace.txt", errors="replace"):
    m = SHAT.search(line)
    if m:
        t_ns = int(m.group(10))
        if t_ns >= mark:
            ticks.append({"t": float(m.group(2)), "live": int(m.group(3)),
                          "n": int(m.group(4)),
                          "aabb": [int(m.group(5)), int(m.group(6)),
                                   int(m.group(7)), int(m.group(8))],
                          "t_ns": t_ns})
        continue
    m = RENDER.search(line)
    if m:
        renders.append({"mon": m.group(1), "damage_px": int(m.group(6)),
                        "t_ns": int(m.group(12)),
                        "ext": [int(m.group(8)), int(m.group(9)),
                                int(m.group(10)), int(m.group(11))]})

out = {"ticks": len(ticks), "renders": len(renders)}
if not ticks:
    out["error"] = "no shatter tick in the trace -- did the close run at all?"
    print(json.dumps(out)); sys.exit(0)

out["first_ns"] = ticks[0]["t_ns"]
out["last_ns"] = ticks[-1]["t_ns"]
out["wall_ms"] = (ticks[-1]["t_ns"] - ticks[0]["t_ns"]) / 1.0e6
out["t_last"] = ticks[-1]["t"]
out["live_first"] = ticks[0]["live"]
out["live_last"] = ticks[-1]["live"]
out["nfrags"] = ticks[0]["n"]

# The largest cloud AABB seen, in pixels: the ceiling every frame's damage is
# measured against. Taken from the trace's own numbers rather than recomputed,
# so the oracle cannot inherit a bug from the damage code it is checking.
big = 0
for t in ticks:
    big = max(big, t["aabb"][2] * t["aabb"][3])
out["max_aabb_px"] = big

# ── DAMAGE DURING THE SHATTER, AND ONLY DURING IT ────────────────────────
#
# Bounded by the FIRST and LAST shatter tick. The first version took every
# frame after the mark, which swept in the startup repaint and the teardown --
# both legitimately full-output, and neither anything to do with this
# animation. Those frames were what made HEADLESS-1 (which the window was not
# even on) report 2073600px of "shatter" damage.
lo, hi = ticks[0]["t_ns"], ticks[-1]["t_ns"]
per = {}
for r in renders:
    if r["damage_px"] <= 0 or r["t_ns"] < lo or r["t_ns"] > hi:
        continue
    per.setdefault(r["mon"], []).append(r["damage_px"])
out["damage_max"] = {k: max(v) for k, v in per.items()}
out["damage_frames"] = {k: len(v) for k, v in per.items()}

# The monitor the cloud is actually on: the one carrying the most damaged
# frames during the window. The other output should be nearly idle, and if it
# is not, that is itself worth seeing.
out["busiest"] = max(out["damage_frames"], key=out["damage_frames"].get) \
    if out["damage_frames"] else None
out["output_px"] = 1920 * 1080
print(json.dumps(out))
PY
}

PASS_LABEL=""
report() { # report JSON LABEL
	python3 - "$1" "$2" <<'PY'
import json, sys
j = json.loads(sys.argv[1])
print("  %s: ticks=%s renders=%s" % (sys.argv[2], j.get("ticks"),
                                     j.get("renders")))
if "error" in j:
    print("    ERROR " + j["error"]); sys.exit(0)
print("    wall=%.1fms t_last=%.3f live %s->%s of %s  max_aabb=%dpx"
      % (j["wall_ms"], j["t_last"], j["live_first"], j["live_last"],
         j["nfrags"], j["max_aabb_px"]))
print("    damage_max=%s frames=%s" % (j["damage_max"], j["damage_frames"]))
PY
}

# ── the run ──────────────────────────────────────────────────────────────
#
# CONFIGURED DURATION, which the refresh gate compares against. A shatter that
# advanced once per frame would finish in a wall time set by how many ticks it
# received -- and with two outputs it receives the SUM of their rates.
DURATION_MS=700
MODE="${1:-both}"

# A python predicate over the analysis JSON, echoing true/false for hl_assert.
pred() { # pred JSON PYEXPR
	J="$1" python3 -c '
import json, os, sys
j = json.loads(os.environ["J"])
worst = max(j["damage_max"].values()) if j.get("damage_max") else 0
big = j.get("max_aabb_px", 0)
out = j.get("output_px", 1920 * 1080)
wall = j.get("wall_ms", 0.0)
dur = float(sys.argv[2])
print("true" if eval(sys.argv[1]) else "false")
' "$2" "$DURATION_MS"
}

check_green() { # check_green JSON LABEL
	local j="$1" label="$2"
	report "$j" "$label"

	hl_assert_true "PREMISE: a shatter actually ran (AVK, not the fall fallback)" \
		"$(pred "$j" 'j.get("ticks", 0) > 5')"

	# PREMISE: the cloud is genuinely smaller than the output. Without this the
	# damage gate below is the vacuous one the first version of this fixture
	# shipped -- a maximised window's cloud IS the screen, so a full-output
	# repaint sits happily inside it and the gate cannot fail.
	hl_assert_true "PREMISE: the cloud is smaller than the output (else the damage gate is vacuous)" \
		"$(pred "$j" 'big < out')"

	# (d) REFRESH INDEPENDENCE. Two outputs, 144 and 60, both ticking the same
	# client; the close still takes its configured wall-clock time. The last
	# tick is the last one BEFORE the animation ended, so the measured span is
	# short by up to a frame; 15% covers that and a headless backend's
	# scheduling jitter. A frame-stepped run is out by multiples, not percent.
	hl_assert_true "the close takes its configured wall time despite two refresh rates" \
		"$(pred "$j" 'abs(wall - dur) <= dur * 0.15')"

	# (e) DAMAGE BOUND, against BOTH the cloud and the output. 2x the cloud
	# because while the marker moves the scene damages old box UNION new box,
	# which can legitimately be about twice one box.
	hl_assert_true "damage stays within the fragment cloud" \
		"$(pred "$j" 'worst <= big * 2 + 4096')"
	hl_assert_true "damage is never the whole output (ADR-613)" \
		"$(pred "$j" 'worst < out')"
}

if [ "$MODE" = green ] || [ "$MODE" = both ]; then
	echo "=== shatter, two outputs at 144/60 -- must PASS ==="
	check_green "$(analyse "$(run_case green)")" "green"
fi

if [ "$MODE" = break-damage ] || [ "$MODE" = both ]; then
	echo
	echo "=== AZ_BREAK_SHATTER_DAMAGE_FULL -- the damage gate must go RED ==="
	JD="$(analyse "$(run_case breakdmg AZ_BREAK_SHATTER_DAMAGE_FULL=1)")"
	report "$JD" "break-damage"
	# The green gate demands worst < output_px; the break must violate it.
	hl_assert_true "BREAK: full-output damage is DETECTED" \
		"$(pred "$JD" 'worst >= out')"
fi

if [ "$MODE" = break-step ] || [ "$MODE" = both ]; then
	echo
	echo "=== AZ_BREAK_SHATTER_FRAME_STEP -- the refresh gate must go RED ==="
	JS="$(analyse "$(run_case breakstep AZ_BREAK_SHATTER_FRAME_STEP=1)")"
	report "$JS" "break-step"
	hl_assert_true "BREAK: frame-stepped progress is DETECTED (wall time is wrong)" \
		"$(pred "$JS" 'abs(wall - dur) > dur * 0.15')"
fi

echo
echo "logs: $OUTDIR"
hl_summary
