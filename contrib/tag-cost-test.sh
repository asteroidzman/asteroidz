#!/usr/bin/env bash
# tag-cost-test.sh -- what does a tag slide actually cost, with a blurred
# window on it?
#
# P4's first half. Before any lever is pulled, the cost is measured: per
# TRANSITION, not averaged over a session, because a tag slide is a burst and
# an average over idle frames hides it completely.
#
# The number the lever exists to move is `blur_rebuild_px`: prefix pixels the
# blur chain re-rendered during the slide. A blurred window moving across a
# backdrop makes the chain rebuild that backdrop every frame, and this says how
# much.
#
# ── THE PREMISE THIS FIXTURE MUST ASSERT ─────────────────────────────────
#
# That blur is actually involved. A run with no blurred window rebuilds nothing
# and reports a beautiful zero, which would read as "tag slides are already
# cheap" when it means "this fixture measured the wrong scene". So it runs the
# same slide TWICE -- once with blur off, once on -- and requires the blurred
# run to cost materially more. If it does not, the fixture is blind and says so
# rather than reporting a number.
set -u

unset ASTEROIDZ_INSTANCE_SIGNATURE
. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="tag-cost"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-tagcost-$$"
mkdir -p "$OUTDIR"

ANIM_CFG='animations {
	curve spring
	spring { damping 0.8; frequency 22 }
	animation_duration_tag 400
	window-open { type zoom; duration 200 }
	window-close { type fade; duration 200 }
}
layer_animations 0'

blur_cfg() {
	if [ "$1" = 0 ]; then
		echo 'effects { blur { enable 0 }; shadow { enable 0 } }'
	else
		echo 'effects {
			blur { enable 1; optimized 1; passes 2; radius 6
				params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } }
			shadow { enable 0 } }'
	fi
}

run_case() { # run_case NAME BLUR -> echoes the dir
	local name="$1" blur="$2"
	local dir="$OUTDIR/$name"
	mkdir -p "$dir"

	HL_OUTDIR="$dir"
	HL_OUTPUTS=1
	HL_HZ1=144
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_PACE=1"
	export HL_OUTDIR HL_OUTPUTS HL_HZ1 HL_ENV

	hl_start "$(blur_cfg "$blur")
$ANIM_CFG"

	local pid
	pid="$(hl_spawn_wlbgeffect "tag" 60 "w1")"
	hl_wait_client_count 1
	sleep 1

	# Slide between two tags, repeatedly. Each `view` is one transition.
	local i
	for i in 1 2 3 4 5 6; do
		hl_dispatch view,2 1
		hl_dispatch view,1 1
	done
	sleep 1

	kill "$pid" 2>/dev/null || true
	grep azpace "$dir/state/asteroidz/asteroidz.log" > "$dir/trace.txt" \
		2>/dev/null || true
	hl_stop
	echo "$dir"
}

# Every completed transition in the trace, summarised.
analyse() { # analyse DIR
	python3 - "$1" <<'PY'
import re, sys, json

TC = re.compile(
    r"azpace tag cost dur_ms=(\S+) frames=(\d+) committed=(\d+) "
    r"blur_rebuild_px=(\d+) damage_px=(\d+) p50_ms=(\S+) p95_ms=(\S+) "
    r"samples=(\d+) truncated=(\d) n=(\d+)")

rows = []
for line in open(sys.argv[1] + "/trace.txt", errors="replace"):
    m = TC.search(line)
    if m:
        rows.append({"dur_ms": float(m.group(1)), "frames": int(m.group(2)),
                     "committed": int(m.group(3)),
                     "blur_px": int(m.group(4)), "damage_px": int(m.group(5)),
                     "p50": float(m.group(6)), "p95": float(m.group(7)),
                     "samples": int(m.group(8)),
                     "truncated": int(m.group(9))})

out = {"transitions": len(rows)}
if rows:
    # The first transition is the one that also pays for whatever the tag
    # switch touches for the first time; report the median of the rest, which
    # is what a slide costs in the steady state.
    body = rows[1:] if len(rows) > 1 else rows
    body.sort(key=lambda r: r["blur_px"])
    mid = body[len(body) // 2]
    out["median_blur_px"] = mid["blur_px"]
    out["median_damage_px"] = mid["damage_px"]
    out["median_frames"] = mid["frames"]
    out["median_p50_ms"] = mid["p50"]
    out["median_p95_ms"] = mid["p95"]
    out["total_blur_px"] = sum(r["blur_px"] for r in rows)
    out["any_truncated"] = any(r["truncated"] for r in rows)
print(json.dumps(out))
PY
}

echo "=== tag slide, blur OFF ==="
J0="$(analyse "$(run_case noblur 0)")"
echo "  $J0"
echo "=== tag slide, blur ON ==="
J1="$(analyse "$(run_case blur 1)")"
echo "  $J1"

pred() { J="$1" python3 -c '
import json, os, sys
j = json.loads(os.environ["J"])
print("true" if eval(sys.argv[1]) else "false")' "$2"; }

hl_assert_true "PREMISE: transitions were measured at all" \
	"$(pred "$J1" 'j.get("transitions",0) >= 4')"
hl_assert_true "PREMISE: the percentiles are of complete samples, not a prefix" \
	"$(pred "$J1" 'not j.get("any_truncated", True)')"

# THE PREMISE THAT MAKES THE NUMBER MEAN ANYTHING. With blur off the chain
# rebuilds nothing; with it on, a slide over a blurred backdrop rebuilds a lot.
# If the two are close, this fixture is not measuring blur.
B0="$(python3 -c "import json;print(json.loads('''$J0''').get('median_blur_px',0))")"
B1="$(python3 -c "import json;print(json.loads('''$J1''').get('median_blur_px',0))")"
echo "  median blur_rebuild_px: off=$B0 on=$B1"
hl_assert_true "PREMISE: blur is what this fixture is measuring (on >> off)" \
	"$(python3 -c "print('true' if $B1 > $B0 * 4 + 100000 else 'false')")"

echo
echo "logs: $OUTDIR"
hl_summary
