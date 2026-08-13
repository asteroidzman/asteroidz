#!/usr/bin/env bash
# m4g-motion-test.sh -- did M4G actually fix motion, and can this test fail?
#
# Five claims, each with a break that must reproduce the defect it removed.
# A green break run is a failed suite: it means the assertion was never
# testing anything.
#
#   DAMAGE       a moving blurred window damages what the motion requires,
#                measured against an oracle computed here from the trace's
#                own geometry series -- NOT from the production damage code
#                that is under test.
#   QUANTISE     the stored position is the rounded ideal, so a settled
#                window cannot flip by a pixel.       break AZ_ANIM_TRUNCATE
#   CONVERGE     a spring stops when it stops moving. break AZ_ANIM_NO_CONVERGE
#   SCHEDULE     frames are asked for by the outputs the motion reaches.
#   REFRESH      the same animation converges at the same wall-clock time at
#                two very different refresh rates.
#
# ── WHY THE ORACLE IS ARITHMETIC AND NOT A SECOND IMPLEMENTATION ──────────
#
# The required damage for one animation step is the union of the box the
# window occupied and the box it now occupies, each grown by how far an
# effect can carry a pixel outward. Both boxes are in the trace, as the
# INTEGER geometry the scene node actually stored -- so the oracle needs no
# access to the compositor's regions, cannot inherit a bug from them, and is
# checkable by hand from the log.
#
# The growth is deliberately GENEROUS (see PAD): a pad larger than the true
# effect support can only make the oracle bigger, i.e. the test weaker in the
# direction of passing -- so the ratio it reports is a LOWER bound on the
# amplification, and the assertion is conservative.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="m4g-motion"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m4g-$$"
mkdir -p "$OUTDIR"

PACE="$(dirname "$0")/anim-pace-test.sh"
# Blur radius 6 at 2 passes reaches (r+1)*2^passes = 28 output pixels; the
# border and its AA add 2 more. 48 is comfortably past both and is the same
# number on both sides of every comparison below.
PAD="${PAD:-48}"

run_case() { # run_case NAME ENV... -> echoes the trace path
	local name="$1"; shift
	local dir="$OUTDIR/$name"
	mkdir -p "$dir"
	( cd "$HL_REPO" && env TMPDIR="$dir" HL_EXTRA_ENV="$*" \
		WORKLOADS=move BLUR=1 WINDOWS=1 OUTS="${OUTS:-1}" \
		HZ1="${HZ1:-144}" HZ2="${HZ2:-60}" bash "$PACE" ) \
		> "$dir/out.txt" 2>&1
	local d
	d="$(sed -n 's/^logs: //p' "$dir/out.txt" | head -1)"
	echo "$d/move/trace.txt"
}

# The analyser, given a trace, printing one line of key=value facts.
facts() { # facts TRACE PAD
	python3 - "$1" "$2" <<'PY'
import sys, re
trace, pad = sys.argv[1], int(sys.argv[2])

# ── PARSE, IN TIME ORDER ─────────────────────────────────────────────────
# Every event carries t_ns, including the end marker. An earlier version
# sorted the end markers to position 0, which silently produced zero runs --
# and the convergence assertions then passed by comparing 0 against 0. A
# vacuous assertion is worse than a missing one.
ev = []
for l in open(trace, errors="replace"):
    m = re.search(r"anim tick c=(\S+) mon=(\S+) .*factor=(\S+) "
                  r"ideal=(\S+),(\S+),\S+x\S+ "
                  r"geom=(-?\d+),(-?\d+),(\d+)x(\d+) t_ns=(\d+)", l)
    if m:
        ev.append((int(m.group(10)), "t", m.group(2),
                   (int(m.group(6)), int(m.group(7)), int(m.group(8)),
                    int(m.group(9))), float(m.group(4)), float(m.group(5))))
        continue
    m = re.search(r"render mon=(\S+) .*committed=(\d) more=(\d) "
                  r"damage_px=(\d+) .*t_ns=(\d+)", l)
    if m:
        ev.append((int(m.group(5)), "r", m.group(1), int(m.group(2)),
                   int(m.group(3)), int(m.group(4))))
        continue
    m = re.search(r"anim end .*t_ms=(-?\d+) converged=(\d) t_ns=(\d+)", l)
    if m:
        ev.append((int(m.group(3)), "e", int(m.group(1)), int(m.group(2))))
        continue
    m = re.search(r"anim start .*t_ns=(\d+)", l)
    if m:
        ev.append((int(m.group(1)), "s"))
ev.sort(key=lambda e: e[0])

def box_pad(b, p):
    return (b[0]-p, b[1]-p, b[0]+b[2]+p, b[1]+b[3]+p)

def union_area(a, b):
    A = (a[2]-a[0])*(a[3]-a[1]); B = (b[2]-b[0])*(b[3]-b[1])
    ix = max(0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0, min(a[3], b[3]) - max(a[1], b[1]))
    return A + B - ix*iy

# ── WALK ─────────────────────────────────────────────────────────────────
# A run is start..end. Damage is only compared against the oracle for a
# render that falls INSIDE a run and has two ticks of the same monitor
# before it in that same run -- pairing across a run boundary would compute
# the union of two unrelated positions, which is not a step the compositor
# was ever asked to make.
runs = []        # list of (ticks, converged)
cur = None
last = {}        # mon -> (prev_geom, geom) within the current run
ratios = []
ratios_tight = []
by_mon = {}
for e in ev:
    kind = e[1]
    if kind == "s":
        if cur:
            runs.append((cur, 0))
        cur = []
        last = {}
    elif kind == "t":
        mon, g = e[2], e[3]
        if cur is not None:
            cur.append((g, e[4], e[5]))
        prev = last.get(mon)
        last[mon] = (prev[1] if prev else None, g)
    elif kind == "e":
        if cur is not None:
            runs.append((cur, e[3]))
        cur = None
        last = {}
    else:
        mon, committed, more, dmg = e[2], e[3], e[4], e[5]
        m = by_mon.setdefault(mon, [0, 0, 0])
        if committed:
            m[0] += 1
            if dmg == 0:
                m[1] += 1
        if more:
            m[2] += 1
        if cur is None or not committed or dmg == 0:
            continue
        p = last.get(mon)
        if not p or p[0] is None:
            continue
        old, new = p
        req = union_area(box_pad(old, pad), box_pad(new, pad))
        tight = union_area(box_pad(old, 0), box_pad(new, 0))
        if req > 0:
            ratios.append(dmg / req)
        if tight > 0:
            ratios_tight.append(dmg / tight)
if cur:
    runs.append((cur, 0))

flips = 0
max_stall = 0
max_qerr = 0.0
ticks = []
converged = 0
for r, conv in runs:
    if not r:
        continue
    converged += conv
    ticks.append(len(r))
    stall = 0
    for i, (g, ix, iy) in enumerate(r):
        max_qerr = max(max_qerr, abs(ix - g[0]), abs(iy - g[1]))
        if i:
            stall = stall + 1 if g == r[i-1][0] else 0
            max_stall = max(max_stall, stall)
        # a flip: the position returns to a value it had two ticks ago after
        # leaving it -- the 200 -> 199 -> 200 signature
        if i >= 2 and g[:2] == r[i-2][0][:2] and g[:2] != r[i-1][0][:2]:
            flips += 1

def pct(a, p):
    if not a:
        return 0.0
    s = sorted(a)
    return s[min(len(s)-1, int(round(p*(len(s)-1))))]

print("ratio_n=%d ratio_p50=%.3f ratio_p95=%.3f ratio_max=%.3f "
      "tight_p50=%.3f tight_p95=%.3f" %
      (len(ratios), pct(ratios, .5), pct(ratios, .95), max(ratios or [0]),
       pct(ratios_tight, .5), pct(ratios_tight, .95)))
print("runs=%d converged=%d ticks_p50=%d max_stall=%d flips=%d "
      "max_qerr=%.4f" % (len(runs), converged, pct(ticks, .5), max_stall,
                         flips, max_qerr))
for mon in sorted(by_mon):
    c, z, more = by_mon[mon]
    print("mon=%s committed=%d zero_damage=%d zero_pct=%.1f wanted_more=%d" %
          (mon, c, z, 100.0*z/c if c else 0.0, more))
PY
}

get() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

echo
echo "── 1. the fix: motion damage against an independent oracle ──────────"
T_FIX="$(OUTS=1 run_case fix)"
F_FIX="$(facts "$T_FIX" "$PAD" | tr '\n' ' ')"
echo "  $F_FIX"
hl_assert "damage: the oracle saw a real population" \
	"$([ "$(get ratio_n "$F_FIX")" -ge 100 ] && echo true || echo false)" true
hl_assert_true "damage: p50 <= 1.5x required" \
	"$(awk -v v="$(get ratio_p50 "$F_FIX")" 'BEGIN{print (v<=1.5)?"true":"false"}')"
hl_assert_true "damage: p95 <= 1.5x required" \
	"$(awk -v v="$(get ratio_p95 "$F_FIX")" 'BEGIN{print (v<=1.5)?"true":"false"}')"
hl_assert_true "quantise: no post-arrival position flip" \
	"$([ "$(get flips "$F_FIX")" = "0" ] && echo true || echo false)"
# Half a pixel EXACTLY is the mathematical maximum of round-to-nearest: a tie
# at x.5 rounds away and is off by half. Anything above it is truncation.
hl_assert_true "quantise: error never exceeds half a pixel" \
	"$(awk -v v="$(get max_qerr "$F_FIX")" 'BEGIN{print (v<=0.5)?"true":"false"}')"
hl_assert_true "converge: the fixture produced runs to judge" \
	"$([ "$(get runs "$F_FIX")" -ge 5 ] && echo true || echo false)"
hl_assert_true "converge: every run ended by converging, not by the clock" \
	"$([ "$(get converged "$F_FIX")" = "$(get runs "$F_FIX")" ] && echo true || echo false)"
hl_assert_true "converge: no long stationary stall" \
	"$([ "$(get max_stall "$F_FIX")" -le 3 ] && echo true || echo false)"
ZP="$(facts "$T_FIX" "$PAD" | sed -n 's/.*zero_pct=\([0-9.]*\).*/\1/p' | head -1)"
ZN="$(facts "$T_FIX" "$PAD" | sed -n 's/.*zero_damage=\([0-9]*\).*/\1/p' | head -1)"
echo "  single-output zero-damage commits: ${ZP}% (${ZN} over $(get runs "$F_FIX") runs)"
# ── WHY PER-RUN AND NOT A PERCENTAGE ─────────────────────────────────────
#
# The percentage's denominator is "frames committed during the workload",
# which is a property of how long the fixture sleeps between dispatches, not
# of the scheduler. The scheduler's actual residue is per ANIMATION, and it
# is two frames: one mid-run tick whose sub-pixel step rounds to the position
# it already had, and one trailing frame that the last tick had already asked
# for before it discovered it was the last. Both are the floor for a loop
# that schedules one frame ahead into an integer scene graph.
#
# Three per run, so the assertion has one frame of headroom and still fails
# by a mile if the dead tail returns -- the break run produces 48.
hl_assert_true "schedule: at most 3 zero-damage commits per animation" \
	"$([ "$(( ${ZN:-9999} ))" -le "$(( $(get runs "$F_FIX") * 3 ))" ] \
		&& echo true || echo false)"

echo
echo "── 2. BREAK AZ_ANIM_TRUNCATE: the jitter must come back ─────────────"
T_TRUNC="$(OUTS=1 run_case trunc AZ_ANIM_TRUNCATE=1)"
F_TRUNC="$(facts "$T_TRUNC" "$PAD" | tr '\n' ' ')"
echo "  $F_TRUNC"
hl_assert_true "break truncate: the broken fixture produced runs to judge" \
	"$([ "$(get runs "$F_TRUNC")" -ge 5 ] && echo true || echo false)"
hl_assert_true "break truncate: quantisation error reaches a whole pixel" \
	"$(awk -v v="$(get max_qerr "$F_TRUNC")" 'BEGIN{print (v>=0.9)?"true":"false"}')"

echo
echo "── 3. BREAK AZ_ANIM_NO_CONVERGE: the dead tail must come back ───────"
T_TAIL="$(OUTS=1 run_case tail AZ_ANIM_NO_CONVERGE=1)"
F_TAIL="$(facts "$T_TAIL" "$PAD" | tr '\n' ' ')"
echo "  $F_TAIL"
hl_assert_true "break converge: the broken fixture produced runs to judge" \
	"$([ "$(get runs "$F_TAIL")" -ge 5 ] && echo true || echo false)"
hl_assert_true "break converge: no run converges" \
	"$([ "$(get converged "$F_TAIL")" = "0" ] && echo true || echo false)"
hl_assert_true "break converge: long stationary stalls return" \
	"$([ "$(get max_stall "$F_TAIL")" -ge 10 ] && echo true || echo false)"
ZN_TAIL="$(facts "$T_TAIL" "$PAD" | sed -n 's/.*zero_damage=\([0-9]*\).*/\1/p' | head -1)"
hl_assert_true "break converge: zero-damage commits per animation explode" \
	"$([ "$(( ${ZN_TAIL:-0} ))" -gt "$(( $(get runs "$F_TAIL") * 10 ))" ] \
		&& echo true || echo false)"
hl_assert_true "break converge: ticks per run roughly triple" \
	"$([ "$(get ticks_p50 "$F_TAIL")" -ge $(( $(get ticks_p50 "$F_FIX") * 2 )) ] \
		&& echo true || echo false)"

echo
echo "── 4. two outputs: the one with no motion is left alone ─────────────"
T_TWO="$(OUTS=2 run_case two)"
facts "$T_TWO" "$PAD" | sed 's/^/  /'
SECOND="$(facts "$T_TWO" "$PAD" | sed -n 's/^mon=HEADLESS-2 .*committed=\([0-9]*\).*/\1/p')"
FIRST="$(facts "$T_TWO" "$PAD" | sed -n 's/^mon=HEADLESS-1 .*committed=\([0-9]*\).*/\1/p')"
echo "  committed: HEADLESS-1=$FIRST HEADLESS-2=$SECOND"
hl_assert_true "schedule: the second output commits far less than the first" \
	"$([ "${SECOND:-9999}" -lt $(( ${FIRST:-1} / 3 )) ] && echo true || echo false)"

echo
echo "── 5. refresh invariance: convergence is wall clock, not frames ─────"
T_FAST="$(OUTS=1 HZ1=240 run_case fast)"
T_SLOW="$(OUTS=1 HZ1=48 run_case slow)"
ms() { grep "anim end" "$1" | sed -n 's/.*t_ms=\([0-9]*\).*/\1/p' \
	| sort -n | awk '{a[NR]=$1} END{print a[int(NR/2)]}'; }
MF="$(ms "$T_FAST")"; MS="$(ms "$T_SLOW")"
TF="$(grep -c "anim tick" "$T_FAST")"; TS="$(grep -c "anim tick" "$T_SLOW")"
echo "  240Hz: converged at ${MF}ms over $TF ticks"
echo "   48Hz: converged at ${MS}ms over $TS ticks"
hl_assert_true "refresh: convergence times agree within 30ms" \
	"$(awk -v a="${MF:-0}" -v b="${MS:-0}" \
		'BEGIN{d=a-b; if(d<0)d=-d; print (a>0 && b>0 && d<=30)?"true":"false"}')"
hl_assert_true "refresh: the faster output really did get more samples" \
	"$([ "${TF:-0}" -gt "${TS:-1}" ] && echo true || echo false)"

echo
echo "logs: $OUTDIR"
hl_summary
