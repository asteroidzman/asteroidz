#!/usr/bin/env bash
# tag-cost-test.sh -- what does a tag slide actually cost, with a blurred
# window on it?
#
# P4's first half. Before any lever is pulled, the cost is measured: per
# TRANSITION, not averaged over a session, because a tag slide is a burst and
# an average over idle frames hides it completely.
#
# TWO NUMBERS, AND THE DIFFERENCE BETWEEN THEM IS THE POINT.
#
#   blur_prefix_px  the prefix area the blur chain PRICED. Accumulated for
#                   every slot whether or not its chain runs, because a skipped
#                   blur's saving is only meaningful against what it would
#                   otherwise have cost. A price list.
#   blur_rebuilds   the background blur actually re-rendered. The invoice.
#
# P4's lever was to be justified by the first and is refuted by the second: the
# priced area is ~1.2M pixels per transition, and the rebuild count is ZERO,
# because the M4I background-blur cache already serves the whole slide from a
# single build. Reading the price list as the invoice is what made a lever look
# necessary; this fixture now reports both so it cannot happen again.
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
	HL_ENV="AZ_PACE=1"
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
    r"blur_prefix_px=(\d+) blur_rebuilds=(\d+) blur_hits=(\d+) "
    r"damage_px=(\d+) p50_ms=(\S+) p95_ms=(\S+) "
    r"samples=(\d+) truncated=(\d) n=(\d+)")

rows = []
for line in open(sys.argv[1] + "/trace.txt", errors="replace"):
    m = TC.search(line)
    if m:
        rows.append({"dur_ms": float(m.group(1)), "frames": int(m.group(2)),
                     "committed": int(m.group(3)),
                     "blur_px": int(m.group(4)),
                     "rebuilds": int(m.group(5)), "hits": int(m.group(6)),
                     "damage_px": int(m.group(7)),
                     "p50": float(m.group(8)), "p95": float(m.group(9)),
                     "samples": int(m.group(10)),
                     "truncated": int(m.group(11))})

out = {"transitions": len(rows)}
if rows:
    # The first transition is the one that also pays for whatever the tag
    # switch touches for the first time; report the median of the rest, which
    # is what a slide costs in the steady state.
    body = rows[1:] if len(rows) > 1 else rows
    body.sort(key=lambda r: r["blur_px"])
    mid = body[len(body) // 2]
    out["median_blur_px"] = mid["blur_px"]
    out["median_rebuilds"] = mid["rebuilds"]
    out["median_hits"] = mid["hits"]
    out["total_rebuilds"] = sum(r["rebuilds"] for r in rows)
    out["total_hits"] = sum(r["hits"] for r in rows)
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

pred_num() { J="$1" python3 -c '
import json, os, sys
j = json.loads(os.environ["J"])
print(eval(sys.argv[1]))' "$2"; }

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
echo "  median blur_prefix_px (PRICED, not work): off=$B0 on=$B1"
hl_assert_true "PREMISE: blur is what this fixture is measuring (on >> off)" \
	"$(python3 -c "print('true' if $B1 > $B0 * 4 + 100000 else 'false')")"

# ── AND THE FINDING THAT MADE P4's LEVER UNNECESSARY ─────────────────────
#
# The priced area above is NOT work done. The M4I background-blur cache already
# serves a whole tag slide from one build -- measured 106 requests, 106 hits,
# 1 rebuild across twelve transitions -- so there is nothing left for a
# "freeze the blur during a slide" lever to avoid.
#
# Asserted rather than merely recorded, so that if some future change starts
# rebuilding the blur mid-slide it fails HERE, in the fixture that knows what
# the number means, instead of as a slide nobody can explain.
R="$(pred_num "$J1" 'j.get("total_rebuilds", -1)')"
H="$(pred_num "$J1" 'j.get("total_hits", 0)')"
echo "  blur cache during slides (blur on): rebuilds=$R hits=$H"
hl_assert_true "a tag slide rebuilds no background blur -- it is served from the M4I cache" \
	"$(python3 -c "print('true' if $R == 0 else 'false')")"
hl_assert_true "PREMISE: the cache was actually exercised (hits > 0), so zero rebuilds means served not skipped" \
	"$(python3 -c "print('true' if $H > 0 else 'false')")"

echo
echo "logs: $OUTDIR"
hl_summary
