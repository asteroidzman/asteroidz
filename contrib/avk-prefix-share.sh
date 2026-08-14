#!/usr/bin/env bash
# avk-prefix-share.sh -- how much of a SLOW frame is prefix replay, per frame.
#
# This exists because two percentiles of two different phases are not a
# decomposition of anything. gpu_blur_prefix p95 read 260-440us live and its p99
# read 2280us, and I used the first to reject an optimisation and then the
# second to un-reject it. Both readings are correct and neither says what
# fraction of THE SLOW FRAMES is prefix, because the frame at the 99th
# percentile of one distribution need not be the frame at the 99th percentile of
# the other.
#
# The AZ_TS_TRACE READ line carries gpu_frame, prefix, down, remainder, pre,
# post and the chain count FOR ONE FRAME, together. So the question is answered
# by sorting frames by their own cost and reading the prefix column of the
# expensive ones -- which is what this does, and it is the only form of the
# answer that cannot be a percentile artefact.
#
# HEADLESS ABSOLUTE MICROSECONDS ARE NOT BUDGET EVIDENCE. The fixture GPU idles
# at 47-60MHz, so every number here is inflated by roughly an order of
# magnitude. SHARES are what transfer: prefix/frame is a ratio of two spans
# measured on the same frame on the same clock.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-prefix-share"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-pfxshare-$$"
mkdir -p "$OUTDIR"

W="${W:-3840}"
H="${H:-2160}"
SCALE="${SCALE:-1.5}"
WINDOWS="${WINDOWS:-5}"
SWITCHES="${SWITCHES:-14}"

CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 20; blur_sigma 12; position_y 6 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

export WLBGEFFECT_SSD=1
HL_OUTDIR="$OUTDIR"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
HL_ENV="ASTEROIDZ_RENDERER=avk AZ_TS_TRACE=1"
export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1

echo "══ per-frame prefix share ══ ${W}x${H} scale $SCALE windows=$WINDOWS"
hl_start "$CFG" >/dev/null 2>&1
# Animations ON. The whole question is about transition frames, and a fixture
# with animations off never produces one.
i=0
while [ "$i" -lt "$WINDOWS" ]; do
	hl_spawn_wlbgeffect "px$i" 300 "px$i" >/dev/null
	hl_wait_client_count "$(( i + 1 ))" 160
	# Half on each tag, so a switch has both populations to composite --
	# a switch between a populated tag and an empty one is not the slow case.
	[ $(( i % 2 )) -eq 1 ] && hl_dispatch "tag,2" 0.3
	i=$(( i + 1 ))
done
sleep 3
hl_dispatch reset_avk_stats 1
c=0
while [ "$c" -lt "$SWITCHES" ]; do
	if [ $(( c % 2 )) -eq 0 ]; then hl_dispatch view,2; else hl_dispatch view,1; fi
	sleep 0.9
	c=$(( c + 1 ))
done
sleep 2
LOG="$OUTDIR/state/asteroidz/asteroidz.log"
cp -f "$LOG" "$OUTDIR/trace.log" 2>/dev/null || true
hl_stop >/dev/null 2>&1

python3 - "$OUTDIR/trace.log" <<'PY'
import re, sys
txt = open(sys.argv[1], errors='replace').read()
rows = re.findall(
    r'gpu_frame=([\d.]+) us chains=(\d+) single=\d+ blur_total_us=([\d.]+) '
    r'prefix_us=([\d.]+) down_us=([\d.]+) remainder_us=([\d.]+) '
    r'pre_us=([\d.]+) post_us=([\d.]+)', txt)
if not rows:
    print("  NO TRACE ROWS -- AZ_TS_TRACE produced nothing, this proves nothing")
    sys.exit(0)
f = [(float(a), int(b), float(c), float(d), float(e), float(g), float(h), float(i))
     for a, b, c, d, e, g, h, i in rows]
f.sort(key=lambda r: r[0])
n = len(f)
def band(lo, hi, label):
    s = f[int(n*lo):max(int(n*hi), int(n*lo)+1)]
    if not s: return
    m = len(s)
    # MEAN against MEAN. The first version divided a SUM of prefix by a MEAN
    # frame and printed shares of 352% and 219%, which is the sort of number
    # that gets read past rather than questioned.
    tot = sum(r[0] for r in s)/m
    pfx = sum(r[3] for r in s)/m
    print("  %-14s n=%-5d frame=%8.0f  blur=%8.0f  prefix=%8.0f (%4.1f%% of frame)"
          "  post=%8.0f  chains=%.2f"
          % (label, m, tot, sum(r[2] for r in s)/m, pfx,
             100*pfx/tot if tot else 0,
             sum(r[7] for r in s)/m, sum(r[1] for r in s)/m))
print("  frames with a timestamp read: %d" % n)
print()
band(0.00, 0.50, "cheapest 50%")
band(0.50, 0.90, "50-90%")
band(0.90, 0.99, "90-99%")
band(0.99, 1.00, "slowest 1%")
band(0.90, 1.00, "slowest 10%")
print()
# The number the whole argument turns on, stated once and unambiguously.
top = f[int(n*0.90):]
if top:
    tf = sum(r[0] for r in top); tp = sum(r[3] for r in top)
    print("  ON THE SLOWEST 10%% OF FRAMES, PREFIX IS %.1f%% OF FRAME TIME" %
          (100*tp/tf if tf else 0))
    print("  and those frames average %.2f blur chains against %.2f overall"
          % (sum(r[1] for r in top)/len(top), sum(r[1] for r in f)/n))
PY
echo
echo "logs: $OUTDIR"
