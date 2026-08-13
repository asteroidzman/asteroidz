#!/usr/bin/env bash
# avk-cohort-test.sh — does the blur-active frame cohort actually exclude
# anything, and does the classification survive delayed readback?
#
# M4F.2D.2. gpu_frame is measured over every presented frame; gpu_blur_total
# only exists on frames that ran blur. On a sparse fixture those are different
# populations, and quoting a gpu_frame delta as a "blur frame speedup" compares
# a mixture against a mixture. gpu_frame_blur is the SAME FRAME_BEGIN ->
# FRAME_END timestamps restricted to blur-bearing frames -- a population
# filter, not a new measurement.
#
# ── WHY THIS TEST HAS TO EXIST ────────────────────────────────────────────
#
# GPU timestamp results are consumed several frames after the frame that wrote
# them. So the classification must travel WITH the frame -- stored in the query
# slot at build time -- and not be read off whatever the CPU happens to be
# doing when the result finally comes back. Both designs produce identical
# numbers on any fixture where every frame is blur-active, which is most of
# them; the single-chain control here reports ALL == BLUR == 25 under either.
#
# That is why the sparse fixture is mandatory and why AZ_TS_COHORT_WRONG=1
# exists: it deliberately classifies by the current CPU frame, and this test
# asserts that doing so CHANGES the answer. A cohort test that cannot fail
# proves nothing about the cohort.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-cohort"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-cohort-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

LEVELS="${LEVELS:-3}"
RADIUS="${RADIUS:-5}"
CYCLES="${CYCLES:-30}"

CFG="border_radius 0
effects { shadow { enable 0 }
  blur { enable 1; passes $LEVELS; radius $RADIUS } }"

KEYS='
  frames:.frames, waits:.cpu_sync_waits, chains:.blur_chains,
  frn:.gpu_frame_samples, fbn:.gpu_frame_blur_samples,
  bln:.gpu_blur_total_samples,
  cbf:.cohort_blur_frames, cif:.cohort_idle_frames,
  cbuilt:.cohort_frames_built, broken:.cohort_classifier_broken,
  fb50:.gpu_frame_blur_ns_p50, fr50:.gpu_frame_ns_p50
'

# run KIND TAG [EXTRA_ENV] -> one stats line
run() {
	local kind="$1" tag="$2" extra="${3:-}"
	HL_ENV="ASTEROIDZ_RENDERER=avk $extra"
	export HL_ENV
	hl_start "$CFG"
	if [ "$kind" = sparse ]; then
		# The pulse mutates a rectangle BEHIND the blur, so some frames carry
		# blur work and some do not. WLBGEFFECT_NO_BLUR keeps this client from
		# being a second blur producer, which would make every frame
		# blur-active and destroy the whole point of the fixture.
		export WLBGEFFECT_PULSE="128x96@100,80:300:40" WLBGEFFECT_NO_BLUR=1
		hl_spawn_wlbgeffect back 300 "back-$tag" ff202020 >/dev/null
		hl_wait_client_count 1 60
		unset WLBGEFFECT_PULSE WLBGEFFECT_NO_BLUR
		hl_spawn_wlbgeffect front 300 "front-$tag" >/dev/null
		hl_wait_client_count 2 60
	else
		hl_spawn_wlbgeffect front 300 "front-$tag" >/dev/null
		hl_wait_client_count 1 60
	fi
	sleep 5
	hl_dispatch reset_avk_stats
	local i=0
	while [ "$i" -lt "$CYCLES" ]; do
		hl_dispatch damage_all
		sleep 0.2
		i=$(( i + 1 ))
	done
	sleep 1
	local st
	st="$(hl_get "get avk-stats" | jq -r "{$KEYS} | to_entries | \
		map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null)"
	cp -f "$HL_OUTDIR/state/asteroidz/asteroidz.log" "$OUTDIR/$tag.log" \
		2>/dev/null || true
	hl_stop
	echo "$st"
}

v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

echo
echo "── 1. the degenerate case: every frame is blur-active ────────────────"
# Not evidence that the classifier works -- evidence that it is CONSISTENT
# with gpu_frame where the two populations are the same set. An alias of
# gpu_frame passes this too, which is exactly why it is not the real test.
CTL="$(run control ctl)"
echo "  $CTL"
hl_assert "control: some frames were measured" \
	"$([ "$(v frn "$CTL")" -gt 0 ] 2>/dev/null && echo true || echo false)" true
hl_assert "control: EVERY measured frame is blur-active (ALL == BLUR)" \
	"$(v frn "$CTL")" "$(v fbn "$CTL")"
hl_assert "control: no frame was classified idle" "$(v cif "$CTL")" 0

echo
echo "── 2. THE FALSIFIER: a sparse fixture with real idle frames ──────────"
SP="$(run sparse sparse "AZ_TS_TRACE=1")"
echo "  $SP"
FRN="$(v frn "$SP")"; FBN="$(v fbn "$SP")"; BLN="$(v bln "$SP")"
CBF="$(v cbf "$SP")"; CIF="$(v cif "$SP")"
printf "  frameALL %s   frameBLUR %s   blur spans %s\n" "$FRN" "$FBN" "$BLN"
printf "  slots classified: blur_active=1 %s   blur_active=0 %s   built %s\n" \
	"$CBF" "$CIF" "$(v cbuilt "$SP")"

hl_assert "sparse: the fixture really does contain idle frames" \
	"$([ "${CIF:-0}" -gt 0 ] 2>/dev/null && echo true || echo false)" true
hl_assert "sparse: the cohort EXCLUDES them (BLUR < ALL)" \
	"$([ "${FBN:-0}" -lt "${FRN:-0}" ] 2>/dev/null && echo true || echo false)" true
hl_assert "sparse: the cohort is not empty (BLUR > 0)" \
	"$([ "${FBN:-0}" -gt 0 ] 2>/dev/null && echo true || echo false)" true
# The histogram can only be SMALLER than what was classified -- results get
# dropped -- never larger. Larger would mean a frame joined the cohort that was
# never classified into it.
hl_assert "sparse: collected blur cohort <= classified blur frames" \
	"$([ "${FBN:-0}" -le "${CBF:-0}" ] 2>/dev/null && echo true || echo false)" true
# gpu_blur_total's marks are written by the blur passes themselves, so its
# sample count is the same population arrived at by a different mechanism: one
# is "avk_blur_declare returned true" recorded on the CPU, the other is "the
# BLUR_BEGIN/BLUR_END queries came back available". Not fully independent --
# they share a cause -- but they can disagree, and when they first did it was
# because the cohort was counting blur SLOTS instead of declared CHAINS: 75
# against 53 on this fixture, with 22 frames whose chains were all declined
# counted as blur-active.
hl_assert "sparse: the blur cohort agrees with the blur-span count" \
	"$FBN" "$BLN"
printf "  idle frames = %s built - %s declared-blur = %s\n" \
	"$(v cbuilt "$SP")" "$CBF" "$CIF"

echo
echo "── 3. one delayed sample, traced from build to readback ──────────────"
LOG="$OUTDIR/sparse.log"
python3 - "$LOG" <<'PY'
import re, sys
log = open(sys.argv[1], errors='replace').read().splitlines()
build = {}
reads = []
for l in log:
    m = re.search(r'cohort: BUILD frame=(\d+) slot=(\d+) blur_active=(\d)', l)
    if m:
        build[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
    m = re.search(r'cohort: READ  frame=(\d+) slot=(\d+) slot\.blur_active=(\d) '
                  r'cur\.blur_active=(\d) -> cohort=(\d) \(built (\d+) frames ago\)', l)
    if m:
        reads.append(tuple(int(g) for g in m.groups()))
if not reads:
    print("  NO TRACE LINES -- AZ_TS_TRACE did not reach the log"); sys.exit(1)

# every read must agree with the slot it came from, and with what was stored
# at build time for that frame id
bad = [r for r in reads if r[4] != r[2]]
mismatch = [r for r in reads if r[0] in build and build[r[0]][1] != r[2]]
delayed = [r for r in reads if r[5] > 0]
disagree = [r for r in reads if r[2] != r[3]]

def show(label, r):
    print("  %-22s frame=%d slot=%d slot.active=%d cur.active=%d cohort=%d "
          "delayed=%d frames" % (label, r[0], r[1], r[2], r[3], r[4], r[5]))

act = next((r for r in reads if r[2] == 1), None)
idle = next((r for r in reads if r[2] == 0), None)
if act:  show("blur-active frame:", act)
if idle: show("idle frame:", idle)
if disagree:
    print("  the case that distinguishes the two designs -- the originating")
    print("  frame and the frame being built at readback disagree:")
    show("  ", disagree[0])
print("  reads=%d  delayed(>0 frames)=%d  slot/cur disagreed=%d" %
      (len(reads), len(delayed), len(disagree)))
print("  classified by slot, not by current frame: %s" % ("YES" if not bad else "NO"))
print("  build-time value preserved to readback:   %s" % ("YES" if not mismatch else "NO"))
print("TRACE_OK=%d" % (1 if (not bad and not mismatch and act and idle) else 0))
PY
TRACE_OK="$(python3 - "$LOG" <<'PY'
import re, sys
log = open(sys.argv[1], errors='replace').read()
reads = re.findall(r'cohort: READ  frame=(\d+) slot=(\d+) slot\.blur_active=(\d) '
                   r'cur\.blur_active=(\d) -> cohort=(\d) \(built (\d+) frames ago\)', log)
reads = [tuple(int(x) for x in r) for r in reads]
ok = bool(reads) and all(r[4] == r[2] for r in reads) \
     and any(r[2] == 1 for r in reads) and any(r[2] == 0 for r in reads)
print("true" if ok else "false")
PY
)"
hl_assert "trace: every result was classified by ITS OWN slot, both cases seen" \
	"$TRACE_OK" true

echo
echo "── 4. the break: classify by the current frame instead ───────────────"
#
# ── WHY THE COHORT *SIZE* IS THE WRONG OBSERVABLE ─────────────────────────
#
# The obvious check -- does the broken build report a different number of
# blur-active frames -- does not work, and finding that out was worth more than
# the check would have been.
#
# At readback, `cur_blur_active` holds the value of the frame built one step
# ago, so the broken classifier is the correct sequence SHIFTED BY ONE. A
# shifted binary sequence has the same number of ones, so the cohort SIZE is
# nearly invariant: measured, 53 of 74 both ways. The medians barely move
# either (8.26 vs 8.24 ms), because blur-active frames arrive in runs and a
# one-frame shift only disturbs the run boundaries.
#
# So the break is asserted where it actually differs: PER FRAME. Every read
# logs both the slot's value and the value the broken build would use, and the
# correct build must never disagree with its slot while the broken one must.
WR="$(run sparse wrong "AZ_TS_COHORT_WRONG=1 AZ_TS_TRACE=1")"
echo "  $WR"
printf "  correct classifier: BLUR %s of %s frames\n" "$FBN" "$FRN"
printf "  broken  classifier: BLUR %s of %s frames  (size is shift-invariant)\n" \
	"$(v fbn "$WR")" "$(v frn "$WR")"
hl_assert "break: the broken build admits it is broken" \
	"$(v broken "$WR")" true

# reads where the cohort decision did NOT come from the originating slot
misclass() {
	python3 - "$1" <<'PY2'
import re, sys
log = open(sys.argv[1], errors='replace').read()
reads = re.findall(r'cohort: READ  frame=\d+ slot=\d+ slot\.blur_active=(\d) '
                   r'cur\.blur_active=(\d) -> cohort=(\d)', log)
print(sum(1 for slot, cur, coh in reads if coh != slot))
PY2
}
WRONGN="$(misclass "$OUTDIR/wrong.log")"
RIGHTN="$(misclass "$OUTDIR/sparse.log")"
printf "  frames classified against their own slot: broken %s, correct %s\n" \
	"$WRONGN" "$RIGHTN"
hl_assert "break: the broken build really does misclassify frames" \
	"$([ "${WRONGN:-0}" -gt 0 ] 2>/dev/null && echo true || echo false)" true
hl_assert "break: the correct build misclassifies none" "$RIGHTN" 0

echo
echo "logs: $OUTDIR"
hl_summary
