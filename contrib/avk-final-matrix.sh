#!/usr/bin/env bash
# avk-final-matrix.sh -- the qualification matrix, and the GLES floor.
#
# One fixture, six scenarios, two renderers. Everything else in contrib/
# measures a mechanism; this measures whether the compositor makes its deadline
# doing the things a desktop actually does, and whether AVK is at least as fast
# as the renderer it replaces on the same work.
#
# ── WHAT A BUDGET NUMBER MEANS HERE, AND WHERE IT DOES NOT ───────────────
#
# HEADLESS ABSOLUTE MICROSECONDS ARE NOT BUDGET EVIDENCE. The fixture GPU idles
# near 50MHz and a frame that takes 15ms here takes a fraction of that on a
# clocked display. So the headless run reports two things that DO transfer:
#
#   - the AVK/GLES RATIO, which is two renderers on one machine at one clock
#   - the SHAPE: p99/p50, and whether the tail is continuous or bimodal
#
# The over-budget columns are printed headless too, and they are labelled as
# meaningless there. They mean something only in live mode (HL_LIVE=1), where
# the budget is the real refresh interval.
#
# ── WHY EVERY SCENARIO CONTROLS ITS OWN POPULATION ───────────────────────
#
# A percentile taken over one scenario and compared against a percentile taken
# over another is not a comparison, and this milestone has produced two
# retracted results that way -- once by mixing two outputs with different
# budgets into one distribution, once by comparing cohorts with unequal
# animation counts. So: every scenario runs the same number of actions in both
# renderers, each renderer's frames are counted separately, and nothing is
# aggregated across scenarios.
#
# ── CONSECUTIVE MISSES, NOT JUST A COUNT ─────────────────────────────────
#
# One frame over budget is a hitch nobody sees. Three in a row is a visible
# stutter. The 1x/2x/3x columns are the longest RUN of consecutive over-budget
# frames, because a count alone cannot tell those apart.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-final-matrix"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-matrix-$$"
mkdir -p "$OUTDIR"

W="${W:-3840}"; H="${H:-2160}"; SCALE="${SCALE:-1.5}"
WINDOWS="${WINDOWS:-5}"
ACTIONS="${ACTIONS:-16}"
# The DP-1 frame interval. Live mode should override it to the real one.
BUDGET_US="${BUDGET_US:-6944}"
SCENARIOS="${SCENARIOS:-idle move resize tag multiblur}"
RENDERERS="${RENDERERS:-avk gles}"

# titlebar off: its text does not render identically run to run, which is worth
# 2,574 px of noise to any capture comparison. Nothing here compares captures,
# but a scenario that redraws a nondeterministic strip also damages
# nondeterministically, and that is a timing population this cannot control.
CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 20; blur 12; only-floating 0; tiled-scale 1.0
    blur-background 1; blur-background-strength 0.55 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.0; brightness 0.9; contrast 0.9; saturation 1.2 } } }
layout { titlebar { enable 0 } }"

export WLBGEFFECT_SSD=1
LOGDIR="$OUTDIR"

# ── the scenarios ─────────────────────────────────────────────────────────
#
# Each does ACTIONS actions with the same pacing in both renderers. `idle` is
# damage-driven rather than genuinely idle: a truly idle compositor renders
# nothing, and a percentile over zero frames is not a measurement.
act_idle()     { hl_dispatch damage_all; sleep 0.25; }
act_tag()      { hl_dispatch "view,$1" ; sleep 0.9; }
act_move()     { hl_dispatch "move_window,$1,$2"; sleep 0.35; }
act_resize()   { hl_dispatch "resize_window,$1,$2"; sleep 0.35; }

run_scenario() { # run_scenario RENDERER SCENARIO
	local rend="$1" sc="$2"
	local dir="$OUTDIR/$rend-$sc"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	if [ "$rend" = gles ]; then HL_ENV="ASTEROIDZ_RENDERER=wlr"
	else HL_ENV="ASTEROIDZ_RENDERER=avk"; fi
	HL_ENV="$HL_ENV AZ_SHADOW_DITHER_AMP=0"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1

	local n="$WINDOWS"
	[ "$sc" = multiblur ] && n=$(( WINDOWS * 2 ))
	local i=0
	while [ "$i" -lt "$n" ]; do
		hl_spawn_wlbgeffect "fm$i" 300 "fm$i" >/dev/null
		hl_wait_client_count "$(( i + 1 ))" 200
		# HALF ON TAG 2, so `tag` has two populated tags to push between. A
		# transition between a populated tag and an empty one is a different
		# scene, and the expensive live frames are the ones with both on screen.
		[ "$sc" = tag ] && [ $(( i % 2 )) -eq 1 ] && hl_dispatch "tag,2" 0.3
		i=$(( i + 1 ))
	done
	[ "$sc" = tag ] && hl_dispatch "view,1" 1
	# move_window/resize_window act on the FOCUSED window and a tiled window is
	# placed by the layout, so without this they are accepted and do nothing --
	# a scenario that measures an idle desktop under the name "move".
	case "$sc" in
	move|resize) hl_dispatch toggle_floating 1 ;;
	esac
	sleep 3
	hl_dispatch reset_avk_stats 1
	hl_dispatch set_frame_trace,1 0.2

	local a=0
	while [ "$a" -lt "$ACTIONS" ]; do
		case "$sc" in
		idle|multiblur) act_idle ;;
		tag)   if [ $(( a % 2 )) -eq 0 ]; then act_tag 2; else act_tag 1; fi ;;
		move)  act_move $(( 200 + (a % 4) * 150 )) $(( 150 + (a % 3) * 120 )) ;;
		resize) act_resize $(( 700 + (a % 4) * 130 )) $(( 500 + (a % 3) * 110 )) ;;
		esac
		a=$(( a + 1 ))
	done
	hl_dispatch set_frame_trace,0 0.5
	hl_get "get avk-stats" > "$dir/stats.json" 2>/dev/null
	cp -f "$dir/state/asteroidz/asteroidz.log" "$OUTDIR/$rend-$sc.log" 2>/dev/null || true
	hl_stop >/dev/null 2>&1
}

for r in $RENDERERS; do
	for s in $SCENARIOS; do
		echo "  running $r/$s ..." >&2
		run_scenario "$r" "$s"
	done
done

echo
python3 - "$OUTDIR" "$BUDGET_US" "$SCENARIOS" "$RENDERERS" "${HL_MON:-HEADLESS-1}" <<'PY'
import os, re, sys, json
out, budget, scen, rends, mon = sys.argv[1], float(sys.argv[2]), \
    sys.argv[3].split(), sys.argv[4].split(), sys.argv[5]

# The READ line, per frame, per output. Same-frame attribution: nothing here is
# a ratio of two percentiles taken over two different frames.
RX = re.compile(r'READ\s+out=(\S+).*?gpu_frame=([\d.]+) us')

def frames(path):
    v = []
    if not os.path.exists(path):
        return v
    for m in RX.finditer(open(path, errors='replace').read()):
        if m.group(1) != mon:
            continue
        v.append(float(m.group(2)))
    return v

def pct(v, q):
    if not v: return 0.0
    v = sorted(v); return v[min(int(len(v) * q), len(v) - 1)]

def longest_over(v, mult):
    best = cur = 0
    for x in v:
        cur = cur + 1 if x > budget * mult else 0
        best = max(best, cur)
    return best

def stat(path, key):
    try:
        return json.load(open(path)).get(key)
    except Exception:
        return None

print("  HEADLESS ABSOLUTE MICROSECONDS ARE NOT BUDGET EVIDENCE.")
print("  The >budget and consecutive columns are meaningful only in live mode.")
print()
hdr = ("  %-10s %-6s %6s %8s %8s %8s %8s %7s %4s %4s %4s  %5s %5s %6s"
       % ("scenario","rend","n","p50","p95","p99","max",">bud","1x","2x","3x",
          "VUID","sync","waits"))
print(hdr); print("  " + "-" * (len(hdr) - 2))
table = {}
for s in scen:
    for r in rends:
        v = frames(os.path.join(out, "%s-%s.log" % (r, s)))
        sp = os.path.join(out, "%s-%s" % (r, s), "stats.json")
        verr = stat(sp, "validation_errors")
        waits = stat(sp, "cpu_sync_waits")
        table[(s, r)] = v
        if not v:
            print("  %-10s %-6s %6s  NO TRACE ROWS -- this measured nothing"
                  % (s, r, 0))
            continue
        over = sum(1 for x in v if x > budget)
        print("  %-10s %-6s %6d %8.0f %8.0f %8.0f %8.0f %7d %4d %4d %4d  %5s %5s %6s"
              % (s, r, len(v), pct(v,.50), pct(v,.95), pct(v,.99), max(v), over,
                 longest_over(v,1), longest_over(v,2), longest_over(v,3),
                 verr if verr is not None else "-",
                 verr if verr is not None else "-",
                 waits if waits is not None else "-"))
    print()

if len(rends) == 2:
    print("  ── THE FLOOR: AVK against GLES, same machine, same workload ──")
    print("  %-10s %10s %10s %8s   %s" % ("scenario","AVK p50","GLES p50","ratio","verdict"))
    allpass = True
    for s in scen:
        a, g = table.get((s,"avk"), []), table.get((s,"gles"), [])
        if not a or not g:
            print("  %-10s %10s %10s %8s   %s" % (s,"-","-","-","NO DATA"))
            allpass = False
            continue
        pa, pg = pct(a,.50), pct(g,.50)
        # AVK >= GLES means AVK is not SLOWER. 1.10 of tolerance, because two
        # renderers on one idling GPU do not repeat to the microsecond and a
        # verdict that flips on noise is not a floor.
        ok = pa <= pg * 1.10
        allpass = allpass and ok
        print("  %-10s %10.0f %10.0f %8.2fx   %s"
              % (s, pa, pg, (pa/pg if pg else 0), "PASS" if ok else "FAIL"))
    print()
    print("  GLES FLOOR: %s" % ("PASS" if allpass else "FAIL"))
PY
echo
echo "logs: $OUTDIR"
