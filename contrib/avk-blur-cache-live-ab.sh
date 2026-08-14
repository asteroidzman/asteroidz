#!/usr/bin/env bash
# avk-blur-cache-live-ab.sh -- the monitor background blur cache, on the real
# display, same binary, same session, same windows.
#
# ── WHY NOT TWO RESTARTS ─────────────────────────────────────────────────
#
# Because two restarts are two different measurements. A restart re-execs with
# the same environ (so an env-only knob cannot be flipped at all), starts a cold
# GPU, and rebuilds the whole client set -- and this milestone has already
# produced one "2.6x warm GPU" result that was nothing but unequal animation
# counts between two cohorts. `amsg dispatch set_blur_cache,<0|1>` flips the arm
# inside one running session, so the two arms differ in exactly one thing.
#
# ── OFF / ON / ON / OFF ──────────────────────────────────────────────────
#
# Interleaved, not two blocks. A GPU that warms up over the run would give the
# second block an advantage no matter which arm it was, and the ordering here
# cancels a monotonic drift to first order. Equal animation counts per arm, and
# the per-animation figures are reported beside the aggregate so a percentile
# cannot hide behind a different population size.
#
# ── PER OUTPUT, ALWAYS ───────────────────────────────────────────────────
#
# Every percentile taken before the outputs were separated mixed DP-1 (4K,
# 144Hz, 6944us budget) with HDMI-A-1 (1080p, 60Hz, 16667us) and sent this
# investigation the wrong way three times. The trace line carries the output
# name, and nothing here aggregates across outputs.
#
# THIS RUNS ON THE LIVE SESSION AND SWITCHES TAGS ON THE REAL DISPLAY. The user
# must be watching it: that is the whole point of a live test.
set -u

MON="${MON:-DP-1}"
TAG_A="${TAG_A:-2}"
TAG_B="${TAG_B:-1}"
SWITCHES="${SWITCHES:-10}"
DWELL="${DWELL:-1.2}"
LOG="$HOME/.local/state/asteroidz/asteroidz.log"
OUT="${TMPDIR:-/tmp}/avk-live-ab-$$"
mkdir -p "$OUT"

# DERIVED, NEVER HARDCODED. A pid noted earlier in a session goes stale in
# silence and every dispatch then lands nowhere, which reads as "the cache did
# nothing".
PID="$(pgrep -x asteroidz | head -1)"
if [ -z "$PID" ]; then
	echo "no running asteroidz"; exit 1
fi
echo "══ live A/B ══ pid=$PID monitor=$MON switches=$SWITCHES/arm"
echo "   the display WILL switch tags $((SWITCHES * 2 * 4)) times; watch it."
echo

arm() { # arm NAME 0|1
	local name="$1" on="$2"
	local start
	start=$(wc -l < "$LOG")
	amsg dispatch "set_blur_cache,$on" >/dev/null
	amsg dispatch reset_avk_stats >/dev/null
	sleep 1
	amsg dispatch set_frame_trace,1 >/dev/null
	local i=0
	while [ "$i" -lt "$SWITCHES" ]; do
		amsg dispatch "view,$TAG_B" >/dev/null; sleep "$DWELL"
		amsg dispatch "view,$TAG_A" >/dev/null; sleep "$DWELL"
		i=$(( i + 1 ))
	done
	amsg dispatch set_frame_trace,0 >/dev/null
	sleep 0.5
	tail -n +"$(( start + 1 ))" "$LOG" > "$OUT/$name.log"
	amsg get avk-stats > "$OUT/$name.json" 2>/dev/null
	echo "  arm $name (cache=$on): $(wc -l < "$OUT/$name.log") log lines"
}

arm off1 0
arm on1  1
arm on2  1
arm off2 0

echo
python3 - "$OUT" "$MON" <<'PY'
import re, sys, os, json, statistics as st
out, mon = sys.argv[1], sys.argv[2]

# The READ line, per frame, per output. Same-frame attribution: gpu_frame,
# prefix, blur and post all come from ONE frame's marks, so a share is never a
# ratio of two percentiles taken over two different frames.
RX = re.compile(
    r'READ\s+out=(\S+).*?gpu_frame=([\d.]+) us chains=(\d+).*?'
    r'blur_total_us=([\d.]+) prefix_us=([\d.]+) down_us=([\d.]+) '
    r'remainder_us=([\d.]+) pre_us=([\d.]+) post_us=([\d.]+)')

def load(name):
    rows = []
    p = os.path.join(out, name + ".log")
    if not os.path.exists(p): return rows
    for m in RX.finditer(open(p, errors='replace').read()):
        if m.group(1) != mon: continue
        rows.append(tuple(float(x) for x in m.group(2,4,5,6,7,8,9)) + (int(m.group(3)),))
    return rows

def pct(v, q):
    if not v: return 0.0
    v = sorted(v); k = min(int(len(v) * q), len(v) - 1)
    return v[k]

BUDGET = 6944.0
arms = {n: load(n) for n in ("off1", "on1", "on2", "off2")}
for n, r in arms.items():
    print(f"  {n:5s} frames={len(r)}")
if not any(arms.values()):
    print("  NO TRACE ROWS for", mon, "-- this measured nothing")
    raise SystemExit(0)

def group(names):
    r = []
    for n in names: r += arms[n]
    return r

print()
print(f"  {'arm':10s} {'n':>6s} {'p50':>8s} {'p95':>8s} {'p99':>8s} {'max':>8s}"
      f" {'>6944':>7s} {'2x':>4s} {'blur p50':>9s} {'prefix p50':>11s} {'post p50':>9s}")
for label, names in (("CACHE OFF", ("off1", "off2")), ("CACHE ON", ("on1", "on2"))):
    r = group(names)
    if not r: continue
    f = [x[0] for x in r]
    over = sum(1 for x in f if x > BUDGET)
    over2 = sum(1 for x in f if x > 2 * BUDGET)
    print(f"  {label:10s} {len(r):6d} {pct(f,.50):8.0f} {pct(f,.95):8.0f} "
          f"{pct(f,.99):8.0f} {max(f):8.0f} {over:7d} {over2:4d} "
          f"{pct([x[1] for x in r],.50):9.0f} {pct([x[2] for x in r],.50):11.0f} "
          f"{pct([x[6] for x in r],.50):9.0f}")

print()
print("  per arm, in run order (a monotonic GPU drift would show here):")
for n in ("off1", "on1", "on2", "off2"):
    r = arms[n]
    if not r: continue
    f = [x[0] for x in r]
    print(f"    {n:5s} n={len(f):5d} p50={pct(f,.50):7.0f} p95={pct(f,.95):7.0f} "
          f"p99={pct(f,.99):7.0f} max={max(f):7.0f} over={sum(1 for x in f if x>BUDGET)}")

off, on = group(("off1","off2")), group(("on1","on2"))
if off and on:
    print()
    fo = [x[0] for x in off]; fn = [x[0] for x in on]
    for q, nm in ((.50,"p50"), (.95,"p95"), (.99,"p99")):
        a, b = pct(fo,q), pct(fn,q)
        print(f"  {nm}: {a:7.0f} -> {b:7.0f} us   ({(b-a)/a*100:+.1f}%)")
    print(f"  over budget: {sum(1 for x in fo if x>BUDGET)}/{len(fo)} -> "
          f"{sum(1 for x in fn if x>BUDGET)}/{len(fn)}")

for n in ("off1","on1","on2","off2"):
    p = os.path.join(out, n + ".json")
    if not os.path.exists(p): continue
    d = json.load(open(p))
    print(f"  {n:5s} cache req={d.get('blur_cache_requests')} "
          f"hits={d.get('blur_cache_hits')} rebuilds={d.get('blur_cache_rebuilds')} "
          f"saved_cap={d.get('blur_cache_saved_blur_px',0)/1e6:.1f}Mpx "
          f"saved_replay={d.get('blur_cache_saved_prefix_px',0)/1e6:.1f}Mpx "
          f"waits={d.get('cpu_sync_waits')} verr={d.get('validation_errors')}")
PY
echo
echo "logs: $OUT"
