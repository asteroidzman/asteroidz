#!/usr/bin/env bash
# m6b-hdr-transition-live.sh -- M6B gate G6, the half that only a real display
# can run: N HDR<->SDR transitions on a connected HDR output.
#
# ══ THIS IS A LIVE-SESSION FIXTURE. READ BEFORE RUNNING. ═════════════════
#
# It toggles HDR on your actual display 2N times. Every toggle RETRAINS the
# output -- two modesets and a visible flash, because the fold-into-frame path
# on DP-1 never succeeds and always falls back to a retrain. Twenty cycles is
# forty of them, and it looks alarming. That is expected and is not what the
# fixture is measuring.
#
# It must be run BY THE OPERATOR, WATCHING, in the foreground. Never from a
# batch and never backgrounded; that is why its disposition in avk-suite.sh is
# `live` and why the register refuses to run it.
#
# ══ WHY IT NEEDS THE -debug SESSION, NOT A RESTART ═══════════════════════
#
# The gate's first assertion is that there were no validation errors, and
# `validation_errors` is a counter NOTHING CAN INCREMENT without the validation
# layer -- so asserting it in the plain session is asserting a tautology (F14).
# The layer is enabled by ASTEROIDZ_VK_DEBUG, which is set by the session file,
# and `restart` re-execs with the same environ and cannot add one. So:
#
#     log out  ->  choose "Asteroidz (AVK + Vulkan validation)"  ->  run this
#
# The session is slower than plain AVK by a large factor (validation costs
# ~99x CPU per frame). That is fine here: this fixture measures transitions,
# not frame time.
#
# ══ WHAT IT ASSERTS ══════════════════════════════════════════════════════
#
#   0. the preconditions, EACH ONE, before anything is toggled
#   1. the transitions actually happened          (hdr really changed state)
#   2. no validation errors across all cycles     (premise asserted first)
#   3. no frames refused                          fallback_frames flat
#   4. the encode intermediate is accounted for   bytes back where they started
#   5. the blur cache was invalidated across the domain change
#   6. the desktop is UNCHANGED at the end        vs a control-pair churn floor
#
# (6) is the one that catches a corrupted transition, and it is measured the way
# the live ICC check was: a CONTROL PAIR FIRST, because "1.2% of pixels differ"
# is meaningless until you know that two captures of a settled desktop differ by
# 0.01%. Without the control this assertion is a number with no scale.
set -u

CYCLES="${AZ_CYCLES:-20}"
OUT="${AZ_OUT:-${TMPDIR:-/tmp}/asteroidz-g6-live-$$}"
mkdir -p "$OUT"

pass=0; fail=0
ok()  { pass=$(( pass + 1 )); printf '  ok   %s\n' "$1"; }
bad() { fail=$(( fail + 1 )); printf '  FAIL %s\n' "$1"; }
chk() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1 (got '$2', want '$3')"; fi; }

j() { amsg get "$1" 2>/dev/null; }
mon_field() { j all-monitors | jq -r ".monitors[] | select(.name==\"$1\") | .$2"; }
stat_field() { j avk-stats | jq -r ".$1"; }

echo "══ M6B/G6 (live) ══ HDR transitions on a real display"
echo

# ── 0. PRECONDITIONS, EVERY ONE, BEFORE ANYTHING MOVES ───────────────────
# A fixture that starts toggling and THEN discovers it cannot measure anything
# has cost the operator forty modesets for nothing.
MON="${AZ_MON:-DP-1}"
echo "  target output: $MON"

if ! command -v jq >/dev/null; then echo "  need jq"; exit 1; fi
if [ "$(j avk-stats | jq -r .active)" != "true" ]; then
	echo "  ABORT: AVK is not the renderer in this session."; exit 1
fi
VON="$(stat_field validation_enabled)"
if [ "$VON" != "true" ]; then
	echo "  ABORT: the validation layer is NOT loaded."
	echo "         validation_errors cannot move, so asserting it would be a"
	echo "         tautology (F14). Log out and choose the session named"
	echo "         \"Asteroidz (AVK + Vulkan validation)\", then run this again."
	exit 1
fi
ok "PREMISE: the validation layer is loaded, so verr can move"

HDR0="$(mon_field "$MON" hdr)"
if [ "$HDR0" != "true" ]; then
	echo "  ABORT: $MON is not presenting HDR right now, so there is no"
	echo "         transition to make. Turn HDR on for it first."
	exit 1
fi
ok "PREMISE: $MON is presenting HDR to begin with"
chk "PREMISE: and is on Path B with the PQ encode" \
	"$(mon_field "$MON" color_path)/$(mon_field "$MON" color_encode_tf)" "B-encode/pq"

# THEIR CONFIG IS BACKED UP. set_output_hdr persists to monitors.kdl, so this
# rewrites the operator's own file 2N times. Even N returns it to where it
# started, but "should" is not a backup.
KDL="$HOME/.config/asteroidz/monitors.kdl"
cp -f "$KDL" "$OUT/monitors.kdl.bak" 2>/dev/null && \
	echo "  monitors.kdl backed up to $OUT/monitors.kdl.bak"

echo
echo "  This will toggle HDR on $MON $(( CYCLES * 2 )) times."
echo "  Each toggle RETRAINS the output: two modesets and a visible flash."
echo "  Watch it. Do not walk away -- you are the instrument for anything the"
echo "  counters cannot see."
printf "  Type 'yes' to proceed: "
read -r reply
[ "$reply" = "yes" ] || { echo "  aborted"; exit 0; }
echo

cap() { # cap NAME -> writes $OUT/NAME.ppm
	rm -f "/tmp/$MON.ppm"
	amsg dispatch capture_output >/dev/null 2>&1
	local t=0
	# 4K is ~21 MB and takes a couple of seconds; wait for the size to settle
	# rather than sleeping at it, or a truncated copy reads as a corrupt frame.
	while [ "$t" -lt 60 ]; do
		local a b
		a=$(stat -c %s "/tmp/$MON.ppm" 2>/dev/null || echo 0)
		sleep 0.5
		b=$(stat -c %s "/tmp/$MON.ppm" 2>/dev/null || echo 0)
		[ "$a" -gt 1000 ] && [ "$a" = "$b" ] && break
		t=$(( t + 1 ))
	done
	cp -f "/tmp/$MON.ppm" "$OUT/$1.ppm" 2>/dev/null
}

diffpx() { # diffpx A B -> "moved worst"
	python3 - "$OUT/$1.ppm" "$OUT/$2.ppm" <<'PY'
import sys
def load(p):
    f=open(p,'rb'); f.readline(); l=f.readline()
    while l.startswith(b'#'): l=f.readline()
    w,h=map(int,l.split()); f.readline()
    return f.read(w*h*3)
try:
    a=load(sys.argv[1]); b=load(sys.argv[2])
except Exception:
    print("-1 -1"); raise SystemExit
n=min(len(a),len(b)); m=0; worst=0
for i in range(0,n,3):
    d=max(abs(a[i]-b[i]),abs(a[i+1]-b[i+1]),abs(a[i+2]-b[i+2]))
    if d>1: m+=1
    if d>worst: worst=d
print(m, worst)
PY
}

# ── THE CONTROL PAIR, FIRST ──────────────────────────────────────────────
echo "── control: two captures, nothing toggled ──────────────────────────"
cap ctrl_a
cap ctrl_b
read -r CTRL_MOVED CTRL_WORST <<<"$(diffpx ctrl_a ctrl_b)"
echo "  a settled desktop differs from itself in $CTRL_MOVED px (worst $CTRL_WORST)"
if [ "$CTRL_MOVED" -lt 0 ]; then
	bad "the capture path works at all"; echo; exit 1
fi
ok "PREMISE: the capture path produces comparable frames"

BASE_VERR="$(stat_field validation_errors)"
BASE_FB="$(stat_field fallback_frames)"
BASE_BYTES="$(stat_field m5_intermediate_req_bytes)"
BASE_IMGS="$(stat_field m5_intermediate_images)"
BASE_BC="$(j avk-stats | jq -r "[.blur_cache_outputs[] | select(.name==\"$MON\") | .rebuilds][0] // 0")"
echo "  before: verr=$BASE_VERR fallback=$BASE_FB imgs=$BASE_IMGS bytes=$BASE_BYTES cache_rebuilds=$BASE_BC"
echo

# ── THE CYCLES ───────────────────────────────────────────────────────────
echo "── $CYCLES HDR off/on cycles ────────────────────────────────────────"
i=0; sdr_seen=0
while [ "$i" -lt "$CYCLES" ]; do
	amsg dispatch "set_output_hdr,$MON,0" >/dev/null 2>&1
	sleep 1.5
	[ "$(mon_field "$MON" hdr)" = "false" ] && sdr_seen=$(( sdr_seen + 1 ))
	amsg dispatch "set_output_hdr,$MON,1" >/dev/null 2>&1
	sleep 1.5
	i=$(( i + 1 ))
	printf '\r  cycle %d/%d (SDR states observed: %d)   ' "$i" "$CYCLES" "$sdr_seen"
done
echo; echo

sleep 3
AFT_VERR="$(stat_field validation_errors)"
AFT_FB="$(stat_field fallback_frames)"
AFT_BYTES="$(stat_field m5_intermediate_req_bytes)"
AFT_IMGS="$(stat_field m5_intermediate_images)"
AFT_BC="$(j avk-stats | jq -r "[.blur_cache_outputs[] | select(.name==\"$MON\") | .rebuilds][0] // 0")"
echo "  after : verr=$AFT_VERR fallback=$AFT_FB imgs=$AFT_IMGS bytes=$AFT_BYTES cache_rebuilds=$AFT_BC"
echo

# ── 1. THE TRANSITIONS HAPPENED ──────────────────────────────────────────
# Without this every assertion below is satisfied by a compositor that ignored
# all 2N dispatches: nothing leaks, nothing errors and nothing is refused when
# nothing happened.
chk "PREMISE: the output really entered SDR on every cycle" "$sdr_seen" "$CYCLES"
chk "and it is back in HDR at the end" "$(mon_field "$MON" hdr)" "true"
chk "on Path B with the PQ encode, as it started" \
	"$(mon_field "$MON" color_path)/$(mon_field "$MON" color_encode_tf)" "B-encode/pq"

# ── 2-3. CLEAN ───────────────────────────────────────────────────────────
chk "no validation errors across $CYCLES cycles" "$AFT_VERR" "$BASE_VERR"
chk "no frames refused" "$AFT_FB" "$BASE_FB"

# ── 4. THE INTERMEDIATE IS ACCOUNTED FOR ─────────────────────────────────
# Leaving HDR leaves Path B, which returns the intermediate; re-entering
# allocates one. After an even number of transitions the accounting must be
# exactly where it started -- a per-cycle leak at 4K is 66 MB a time.
chk "the intermediate count is where it started" "$AFT_IMGS" "$BASE_IMGS"
chk "and so is its byte accounting" "$AFT_BYTES" "$BASE_BYTES"

# ── 5. THE BLUR CACHE CROSSED THE DOMAIN CHANGE ──────────────────────────
# The cache holds a blurred backdrop in ONE colour domain, and HDR changes it.
# A cache that survived would serve a backdrop blurred in the other encoding.
if [ "$AFT_BC" -ge "$(( BASE_BC + CYCLES ))" ]; then
	ok "the blur cache rebuilt at least once per cycle ($BASE_BC -> $AFT_BC)"
else
	bad "the blur cache rebuilt at least once per cycle ($BASE_BC -> $AFT_BC, want >= $(( BASE_BC + CYCLES )))"
fi

# ── 6. THE PICTURE SURVIVED ──────────────────────────────────────────────
cap after
read -r MOVED WORST <<<"$(diffpx ctrl_b after)"
echo
echo "  the desktop after $CYCLES cycles differs from before by $MOVED px (worst $WORST)"
echo "  the control floor was $CTRL_MOVED px -- anything at that scale is the"
echo "  desktop's own churn (a clock, a cursor), not the transitions."
LIMIT=$(( CTRL_MOVED * 4 + 2000 ))
if [ "$MOVED" -le "$LIMIT" ]; then
	ok "the picture is unchanged across the transitions (<= $LIMIT px)"
else
	bad "the picture CHANGED across the transitions ($MOVED px > $LIMIT)"
fi

echo
echo "  captures and the config backup: $OUT"
echo "  ---- $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
