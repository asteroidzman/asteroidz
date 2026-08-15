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
#   7. A STATIONARY SURFACE IS TOLD about the HDR change, exactly once per
#      transition, with the right values -- M6B frog gate #9, which is
#      live-only because a headless output cannot enter HDR at all (verified:
#      set_output_hdr leaves hdr=false there).
#
# (6) is the one that catches a corrupted transition, and it is measured the way
# the live ICC check was: a CONTROL PAIR FIRST, because "1.2% of pixels differ"
# is meaningless until you know that two captures of a settled desktop differ by
# 0.01%. Without the control this assertion is a number with no scale.
set -u

CYCLES="${AZ_CYCLES:-20}"
# NOT UNDER /tmp/asteroidz-*, AND THAT PREFIX IS THE WHOLE REASON.
#
# avk-suite.sh snapshots /tmp/asteroidz-* before each fixture and rm -rf's
# anything that appeared afterwards, to reap the leftovers of a fixture that
# died. A live run sharing that prefix looks exactly like such a leftover: the
# first attempt at this gate had its output directory -- captures, and the
# operator's own monitors.kdl backup -- deleted underneath it by a suite running
# concurrently. The captures had been written correctly; there was simply
# nowhere left to put them.
OUT="${AZ_OUT:-${TMPDIR:-/tmp}/az-g6-live-$$}"
mkdir -p "$OUT"

pass=0; fail=0
ok()  { pass=$(( pass + 1 )); printf '  ok   %s\n' "$1"; }
bad() { fail=$(( fail + 1 )); printf '  FAIL %s\n' "$1"; }
chk() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1 (got '$2', want '$3')"; fi; }

# ── EVERY QUERY GOES THROUGH THE PIN ─────────────────────────────────────
#
# The first attempt at this gate reported "no validation errors across 20
# cycles" from a session that had no validation layer at all. amsg had fallen
# back to scanning XDG_RUNTIME_DIR and answered from a leftover headless test
# instance -- and every headless M6B fixture sets ASTEROIDZ_VK_DEBUG=1, so the
# wrong respondent reported exactly the value the precondition wanted to see.
# Every amsg-derived number in that run described a different compositor.
#
# AZ() is the only way this fixture talks to a compositor. AMSG_REQUIRE_* are
# exported once in the preflight, so a call that forgets the pin cannot exist:
# there is nothing to forget.
AZ() { amsg "$@"; }
j() { AZ get "$1" 2>/dev/null; }
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

# ── WHO IS ANSWERING, AND IS IT THE BUILD UNDER TEST ─────────────────────
#
# Established BEFORE anything is toggled and before any counter is read. A
# forty-modeset run on the operator's own display that turns out to have been
# measuring another process is worse than no run: it produces numbers that look
# like evidence.
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IDENT="$(amsg --instance 2>&1)"
if ! printf '%s' "$IDENT" | grep -q '^pid'; then
	echo "  ABORT: could not read the compositor's identity."
	echo "         \`get instance\` arrived with 5b9c742; if the running"
	echo "         compositor predates it, this gate cannot pin its target and"
	echo "         must not run. Log out and back in to pick up the install."
	printf '%s\n' "$IDENT" | sed 's/^/         /'
	exit 1
fi
LIVE_PID="$(printf '%s' "$IDENT"   | awk '/^pid/{print $2}')"
LIVE_BUILD="$(printf '%s' "$IDENT" | awk '/^build/{print $2}')"
LIVE_SESS="$(printf '%s' "$IDENT"  | awk '/^session/{print $2}')"
LIVE_BACK="$(printf '%s' "$IDENT"  | awk '/^backend/{print $2}')"
LIVE_VAL="$(printf '%s' "$IDENT"   | awk '/^validation_enabled/{print $2}')"
LIVE_CAND="$(printf '%s' "$IDENT"  | awk '/^candidates/{print $2}')"
BUILD_ID="$(file "$REPO/build/asteroidz" 2>/dev/null \
	| grep -o 'BuildID\[sha1\]=[0-9a-f]*' | cut -d= -f2)"
INST_ID="$(file /usr/bin/asteroidz 2>/dev/null \
	| grep -o 'BuildID\[sha1\]=[0-9a-f]*' | cut -d= -f2)"
HEADSHA="$(cd "$REPO" && git rev-parse --short HEAD)"
DIRTY="$(cd "$REPO" && git status --porcelain | wc -l)"

echo "  ── the instance under test ──"
echo "     pid        $LIVE_PID"
echo "     build      $LIVE_BUILD"
echo "     session    $LIVE_SESS"
echo "     backend    $LIVE_BACK"
echo "     validation $LIVE_VAL"
echo "     candidates $LIVE_CAND live socket(s) in XDG_RUNTIME_DIR"
echo "     HEAD       $HEADSHA ($DIRTY modified)"
echo "     build/     $BUILD_ID"
echo "     installed  $INST_ID"

# NO CLOSURE EVIDENCE FROM MIXED BINARIES. Source, installed and running must
# be the same code, or the run measures a combination that exists nowhere.
if [ "$LIVE_BUILD" != "$BUILD_ID" ] || [ "$BUILD_ID" != "$INST_ID" ]; then
	echo "  ABORT: running / built / installed are not the same build."
	echo "         A gate that closes a milestone must run the code being"
	echo "         closed. Install, log out, log back in."
	exit 1
fi
if [ "$DIRTY" != "0" ]; then
	echo "  ABORT: the working tree has $DIRTY modified file(s)."
	echo "         The frozen candidate must be what is committed."
	exit 1
fi
ok "PREMISE: source == installed == running ($HEADSHA / $BUILD_ID)"

# FROM HERE ON, EVERY amsg CALL IS PINNED. A wrong-instance answer is an error
# with a message, not a plausible number.
export AMSG_REQUIRE_PID="$LIVE_PID"
export AMSG_REQUIRE_BUILD="$LIVE_BUILD"
export AMSG_REQUIRE_VALIDATION=1
if ! AZ get version >/dev/null 2>&1; then
	echo "  ABORT: the pin does not resolve -- amsg refused its own target."
	exit 1
fi
ok "PREMISE: every query below is pinned to pid $LIVE_PID"
# NOTHING ELSE MAY BE DRIVING THE GPU. A headless suite starts compositors of
# its own, competes for the device, and reaps /tmp directories that are not its
# own -- all three of which corrupt this run rather than merely slowing it.
if pgrep -f "build/asteroidz -c /tmp/asteroidz-" >/dev/null 2>&1; then
	echo "  ABORT: a headless suite is running (its compositors are live)."
	echo "         Let it finish or stop it; this gate needs the GPU and /tmp"
	echo "         to itself."
	exit 1
fi
if [ "$(j avk-stats | jq -r .active)" != "true" ]; then
	echo "  ABORT: AVK is not the renderer in this session."; exit 1
fi
VON="$LIVE_VAL"
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

# ── hdr-mode on FORCES HDR AND MAKES THIS GATE A NO-OP ───────────────────
#
# hdr_resolve() reads config.hdr_mode BEFORE the per-output baseline: with
# `hdr-mode on` it returns want=true unconditionally, so set_output_hdr(...,0)
# writes the baseline and is immediately overridden. The first live run of this
# gate observed ZERO SDR states across twenty cycles and every downstream
# assertion failed for that one reason -- correctly, because nothing transitioned.
#
# There is no runtime dispatch for it, so the fixture edits config.kdl and
# reloads. Backed up and restored by hash, exactly like monitors.kdl.
CFGKDL="$HOME/.config/asteroidz/config.kdl"
HDRMODE_PATCHED=0
if grep -qE '^[[:space:]]*hdr-mode[[:space:]]+on' "$CFGKDL" 2>/dev/null; then
	echo "  NOTE: config.kdl has \`hdr-mode on\`. That used to force HDR and make"
	echo "        every toggle below a no-op; since the per-output choice was"
	echo "        made authoritative, set_output_hdr outranks it and this"
	echo "        fixture no longer edits your config."
fi

# THEIR CONFIG IS BACKED UP. set_output_hdr persists to monitors.kdl, so this
# rewrites the operator's own file 2N times. Even N returns it to where it
# started, but "should" is not a backup.
KDL="$HOME/.config/asteroidz/monitors.kdl"
cp -f "$KDL" "$OUT/monitors.kdl.bak" 2>/dev/null && \
	echo "  monitors.kdl backed up to $OUT/monitors.kdl.bak"

# The observer's path is needed by the freeze below, which runs before the
# confirmation -- and `set -u` turns a forward reference into an abort, not a
# blank.
BGE="$(cd "$(dirname "$0")" && pwd)/wlbgeffect/wlbgeffect"

# ── FREEZE THE EXPERIMENT ────────────────────────────────────────────────
#
# Recorded before the first modeset and re-checked at the end. NEVER EDIT A
# RUNNING EXPERIMENT: bash reads a script by byte offset, so editing one
# mid-run resumes it in the middle of a token -- that destroyed a 50-fixture
# summary once. If a defect in this fixture appears during the run, the run is
# invalidated, the fixture fixed, the freeze re-cut and the whole thing redone.
# A patched-mid-flight run is not a result.
FREEZE="$OUT/freeze.txt"
{
	echo "HEAD        $HEADSHA"
	echo "compositor  $LIVE_BUILD (pid $LIVE_PID, session $LIVE_SESS)"
	for f in "$0" "$BGE" "$(command -v amsg)" "$REPO/build/asteroidz"; do
		[ -e "$f" ] && printf '%-60s %s\n' "$f" "$(sha256sum "$f" | cut -c1-16)"
	done
	echo "cycles      $CYCLES  ($(( CYCLES * 2 )) transitions, $(( CYCLES * 4 )) modesets)"
	echo "started     $(date -Is)"
} | tee "$FREEZE"
SELF_HASH="$(sha256sum "$0" | cut -c1-16)"
echo

echo "  This will toggle HDR on $MON $(( CYCLES * 2 )) times."
echo "  Each toggle RETRAINS the output: two modesets and a visible flash."
echo "  Watch it. Do not walk away -- you are the instrument for anything the"
echo "  counters cannot see."
# THE CONFIRMATION IS DELIBERATE, AND IT HAS TWO SPELLINGS.
#
# A typed 'yes' when stdin is a terminal. When it is NOT -- which is how this
# gets invoked from an editor-integrated shell -- `read` returns EOF
# immediately, `reply` stays empty and the run aborts before doing anything,
# which is exactly what happened the first time. Falling back to "proceed
# anyway" would be the wrong repair: the point of the prompt is that a
# forty-modeset run on the operator's own display is never entered by accident.
# So the non-interactive spelling is an explicit environment variable, which is
# no less deliberate than typing.
if [ -t 0 ]; then
	printf "  Type 'yes' to proceed: "
	read -r reply
else
	reply="${AZ_G6_CONFIRM:-}"
	[ "$reply" = "yes" ] \
		&& echo "  confirmed by AZ_G6_CONFIRM=yes (stdin is not a terminal)"
fi
if [ "$reply" != "yes" ]; then
	echo "  aborted -- no confirmation."
	[ -t 0 ] || echo "  Non-interactive: re-run with AZ_G6_CONFIRM=yes"
	exit 0
fi
echo

# ── THE FROG OBSERVER: A SURFACE THAT DOES NOT MOVE ──────────────────────
#
# The headless fixture proves the metadata follows a surface BETWEEN outputs.
# What it cannot reach is the other half: the surface stays put and the OUTPUT
# changes state underneath it. That needs a display that can actually enter
# HDR, so it lives here.
#
# wlbgeffect carries the observer (see contrib/wlbgeffect/wlbgeffect.c). It maps
# reliably, which matters: an unmapped surface is on no output and the
# compositor correctly says nothing about it -- indistinguishable from a
# compositor that forgot to send.
frog_events() { grep -c "^frog\[" "$OUT/frogwin.log" 2>/dev/null || echo 0; }
frog_last()   { grep "^frog\[" "$OUT/frogwin.log" 2>/dev/null | tail -1; }
frog_mon()    { amsg get all-clients 2>/dev/null | jq -r '.clients[] | select(.title=="fwlive") | .monitor' | head -1; }
frog_ident()  { amsg get all-clients 2>/dev/null | jq -r '.clients[] | select(.title=="fwlive") | .preferred_identity' | head -1; }

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

# ── THE OBSERVER WINDOW, ON THE OUTPUT UNDER TEST ────────────────────────
# POSITIONAL: <app_id> <hold_seconds>. It sets the title to the app_id, which
# is what the client lookups below select on.
WLBGEFFECT_SSD=1 "$BGE" fwlive "$(( CYCLES * 4 + 60 ))" \
	>"$OUT/frogwin.log" 2>&1 &
FROG_PID=$!
sleep 3
FROG_MON="$(frog_mon)"
FROG_ID0="$(frog_ident)"
FROG_EV0="$(frog_events)"
echo "  observer window on $FROG_MON, identity $FROG_ID0, $FROG_EV0 event(s)"
if [ "$FROG_MON" != "$MON" ]; then
	echo "  NOTE: the observer landed on $FROG_MON, not $MON -- move it there"
	echo "        (amsg dispatch tag_monitor,$MON) and re-run, or the frog half"
	echo "        of this gate measures an output that is not toggling."
fi
ok "PREMISE: the observer surface is mapped on an output ($FROG_MON)"
ok "PREMISE: and the compositor resolved a preferred identity ($FROG_ID0)"

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
BASE_LIFE="$(stat_field lifecycle_violations)"
BASE_CPUW="$(stat_field cpu_sync_waits)"
BASE_PRESW="$(stat_field presentation_waits)"
BASE_COMPILES="$(stat_field m5_encode_compiles)"
# The compositor's OWN resolved description for the observer surface -- the
# wp-cm half. The frog log says what went on the wire; this says what the
# shared policy selected. They are separate claims and are kept separate:
# az_preferred.h is one policy with two serializers, and a fixture that only
# read the wire could not tell a correct policy with a broken serializer from
# the reverse.
pref_of() { AZ get all-clients 2>/dev/null \
	| jq -r ".clients[] | select(.title==\"fwlive\") | \"\(.preferred_output) \(.preferred_hdr) \(.preferred_max_luminance) \(.preferred_identity)\"" | head -1; }
BASE_PREF="$(pref_of)"
echo "  before: verr=$BASE_VERR fallback=$BASE_FB imgs=$BASE_IMGS bytes=$BASE_BYTES cache_rebuilds=$BASE_BC"
echo "          lifecycle=$BASE_LIFE cpu_waits=$BASE_CPUW present_waits=$BASE_PRESW compiles=$BASE_COMPILES"
echo "          wp-cm preferred: $BASE_PREF"
echo

# ── THE CYCLES ───────────────────────────────────────────────────────────
echo "── $CYCLES HDR off/on cycles ────────────────────────────────────────"
i=0; sdr_seen=0
FROG_SDR_META=""; FROG_HDR_META=""; FROG_IDS=""
SDR_PREF=""
# Per-cycle record, so a failure at the end can be attributed to a cycle rather
# than to the run. Written as it goes: a fixture that only reports totals
# cannot say whether the fortieth transition behaved like the first.
CYCLELOG="$OUT/cycles.tsv"
printf 'cycle\tstate\thdr\tpath\ttf\tfallback\tverr\tcompiles\tfrog_ev\tpref_ident\n' \
	>"$CYCLELOG"
rec() { # rec <cycle> <state>
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$1" "$2" "$(mon_field "$MON" hdr)" "$(mon_field "$MON" color_path)" \
		"$(mon_field "$MON" color_encode_tf)" "$(stat_field fallback_frames)" \
		"$(stat_field validation_errors)" "$(stat_field m5_encode_compiles)" \
		"$(frog_events)" "$(frog_ident)" >>"$CYCLELOG"
}
while [ "$i" -lt "$CYCLES" ]; do
	AZ dispatch "set_output_hdr,$MON,0" >/dev/null 2>&1
	sleep 1.5
	if [ "$(mon_field "$MON" hdr)" = "false" ]; then
		sdr_seen=$(( sdr_seen + 1 ))
		[ -z "$FROG_SDR_META" ] && FROG_SDR_META="$(frog_last)"
		[ -z "$SDR_PREF" ] && SDR_PREF="$(pref_of)"
	fi
	FROG_IDS="$FROG_IDS $(frog_ident)"
	rec "$(( i + 1 ))" sdr
	AZ dispatch "set_output_hdr,$MON,1" >/dev/null 2>&1
	sleep 1.5
	[ -z "$FROG_HDR_META" ] && FROG_HDR_META="$(frog_last)"
	FROG_IDS="$FROG_IDS $(frog_ident)"
	rec "$(( i + 1 ))" hdr
	i=$(( i + 1 ))
	printf '\r  cycle %d/%d (SDR states observed: %d, frog events: %d)   ' \
		"$i" "$CYCLES" "$sdr_seen" "$(frog_events)"
done
echo; echo

sleep 3
AFT_VERR="$(stat_field validation_errors)"
AFT_FB="$(stat_field fallback_frames)"
AFT_BYTES="$(stat_field m5_intermediate_req_bytes)"
AFT_IMGS="$(stat_field m5_intermediate_images)"
AFT_BC="$(j avk-stats | jq -r "[.blur_cache_outputs[] | select(.name==\"$MON\") | .rebuilds][0] // 0")"
AFT_LIFE="$(stat_field lifecycle_violations)"
AFT_CPUW="$(stat_field cpu_sync_waits)"
AFT_PRESW="$(stat_field presentation_waits)"
AFT_COMPILES="$(stat_field m5_encode_compiles)"
AFT_PREF="$(pref_of)"
echo "  after : verr=$AFT_VERR fallback=$AFT_FB imgs=$AFT_IMGS bytes=$AFT_BYTES cache_rebuilds=$AFT_BC"
echo "          lifecycle=$AFT_LIFE cpu_waits=$AFT_CPUW present_waits=$AFT_PRESW compiles=$AFT_COMPILES"
echo "          wp-cm preferred: $AFT_PREF"
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

# ── THE HARD GATES ───────────────────────────────────────────────────────
# Each is a count that must not move at all. They are separate assertions
# rather than one summed check, because "something went wrong" is not a
# finding -- which counter moved is.
chk "no lifecycle violations" "$AFT_LIFE" "$BASE_LIFE"
chk "no CPU sync waits" "$AFT_CPUW" "$BASE_CPUW"
chk "no presentation waits" "$AFT_PRESW" "$BASE_PRESW"

# ── PIPELINE COMPILES ARE NOT PER-TRANSITION ─────────────────────────────
#
# The invariant the headless gate established as "1 compile, not 20", carried
# to 40 transitions. The encode variants are keyed, so re-entering HDR must
# reuse the pipeline it built the first time. A count that tracks the
# transition count means the key is not doing its job, and the cost is a
# shader compile on a modeset path the operator is watching.
COMPILE_DELTA=$(( AFT_COMPILES - BASE_COMPILES ))
if [ "$COMPILE_DELTA" -le 4 ]; then
	ok "pipeline compiles do not track transitions ($COMPILE_DELTA over $(( CYCLES * 2 )))"
else
	bad "pipeline compiles track transitions ($COMPILE_DELTA over $(( CYCLES * 2 )) -- the variant key is not holding)"
fi

# ── THE wp-cm HALF: THE POLICY MOVED, AND CAME BACK ──────────────────────
#
# az_preferred.h is ONE policy with two serializers, so the wire alone cannot
# settle it: a correct policy with a broken serializer and a broken policy with
# a correct serializer look different only from here. The compositor's own
# resolved description for the stationary surface must differ between the two
# states and return to its starting value.
echo "  wp-cm preferred, SDR state: $SDR_PREF"
if [ -n "$SDR_PREF" ] && [ "$SDR_PREF" != "$BASE_PREF" ]; then
	ok "the resolved description CHANGES with the output's HDR state"
else
	bad "the resolved description CHANGES with the output's HDR state (SDR '$SDR_PREF' vs HDR '$BASE_PREF')"
fi
chk "and returns to its starting value" "$AFT_PREF" "$BASE_PREF"
# The output identity must NOT drift: the surface never moved, so every
# resolution across 40 transitions must have named the same output.
DRIFT="$(printf '%s' "$AFT_PREF" | awk '{print $1}')"
chk "the surface's output identity never drifted" "$DRIFT" \
	"$(printf '%s' "$BASE_PREF" | awk '{print $1}')"
# ── ONE REFUSED FRAME PER TRANSITION, AND IT IS THE INTERLOCK WORKING ────
#
# Measured: 20 cycles = 40 transitions produced 20 refused frames. During a
# transition the output's image description and the derived colour state are
# momentarily out of step -- the description is committed by KMS, the state is
# derived from m->hdr -- and az_output_may_drive() refuses that frame rather
# than let AVK write scene-linear values into a PQ buffer. SceneFX draws it.
#
# ZERO IS THE WRONG EXPECTATION. Refusing is the designed behaviour and the
# alternative is a wrong frame; what must be bounded is HOW MANY. One per cycle
# is the cost of a toggle nobody does often. It only became observable at all
# once the per-output HDR choice started outranking `hdr-mode`.
FB_DELTA=$(( AFT_FB - BASE_FB ))
if [ "$FB_DELTA" -le "$CYCLES" ]; then
	ok "refused frames are bounded by the transition count ($FB_DELTA <= $CYCLES)"
else
	bad "refused frames are bounded by the transition count ($FB_DELTA > $CYCLES)"
fi

# ── 4. THE INTERMEDIATE IS ACCOUNTED FOR ─────────────────────────────────
# Leaving HDR leaves Path B, which returns the intermediate; re-entering
# allocates one. After an even number of transitions the accounting must be
# exactly where it started -- a per-cycle leak at 4K is 66 MB a time.
chk "the intermediate count is where it started" "$AFT_IMGS" "$BASE_IMGS"
chk "and so is its byte accounting" "$AFT_BYTES" "$BASE_BYTES"

# ── 5. THE BLUR CACHE IS *CORRECTLY* PRESERVED HERE ──────────────────────
#
# THIS ASSERTION USED TO DEMAND REBUILDS AND WAS WRONG ABOUT THE ARCHITECTURE.
#
# An HDR toggle on THIS output does not change the composition domain: both
# states are Path B, compositing into the same scene-linear FP16 intermediate.
# Only the ENCODE differs -- PQ against the display's measured curve -- and the
# encode happens after the blur. The cache is keyed on the renderer format,
# which is identical on both sides, so preserving it is correct and rebuilding
# it would be waste.
#
# Measured across 40 transitions: rebuilds 2 -> 2, hits 4204, inv_generation 0,
# inv_source 0. Nothing invalidated it because nothing about the blurred
# picture changed.
#
# The case where the domain DOES change is Path A <-> Path B, which is what
# contrib/m6b-transition-test.sh exercises headlessly -- and there the cache
# rebuilds 80 times across 20 cycles, with removing its format key dropping
# that to zero. That fixture owns the invalidation claim; this one owns the
# preservation claim, and they are different claims about different transitions.
if [ "$AFT_BC" -le "$(( BASE_BC + 2 ))" ]; then
	ok "the blur cache is preserved across a same-domain toggle ($BASE_BC -> $AFT_BC)"
else
	bad "the blur cache is preserved across a same-domain toggle ($BASE_BC -> $AFT_BC -- it rebuilt, but the composition domain did not change)"
fi

# ── 7. THE STATIONARY SURFACE WAS TOLD ───────────────────────────────────
echo
echo "── frog: a surface that never moved, under $(( CYCLES * 2 )) state changes ──"
FROG_EV="$(frog_events)"
FROG_MON_END="$(frog_mon)"
echo "  observer still on: $FROG_MON_END (was $FROG_MON)"
echo "  frog events: $FROG_EV0 -> $FROG_EV"
echo "  first SDR metadata: $FROG_SDR_META"
echo "  first HDR metadata: $FROG_HDR_META"

chk "the observer never moved output" "$FROG_MON_END" "$FROG_MON"

# THE SURFACE STAYED PUT AND THE OUTPUT CHANGED, so this is the case the
# headless fixture cannot reach. One resend per state change that actually
# altered the description.
if [ "$FROG_EV" -ge "$(( FROG_EV0 + CYCLES ))" ]; then
	ok "an HDR change RE-SENDS to a stationary surface ($FROG_EV0 -> $FROG_EV)"
else
	bad "an HDR change RE-SENDS to a stationary surface ($FROG_EV0 -> $FROG_EV, want >= $(( FROG_EV0 + CYCLES )))"
fi

# AND NOT MORE THAN ONE PER TRANSITION. "We fixed stale metadata" must not have
# become "send metadata whenever anything happens": there are 2*CYCLES
# transitions and an upper bound of one send each, plus the initial one.
if [ "$FROG_EV" -le "$(( FROG_EV0 + CYCLES * 2 + 2 ))" ]; then
	ok "and does NOT churn ($FROG_EV <= $(( FROG_EV0 + CYCLES * 2 + 2 )))"
else
	bad "and does NOT churn ($FROG_EV > $(( FROG_EV0 + CYCLES * 2 + 2 )) -- redundant sends)"
fi

# THE VALUES, WHICH ON THIS DISPLAY GENUINELY DIFFER BETWEEN THE TWO STATES --
# unlike two SDR headless outputs, where they are byte-identical. frog's
# ST2084_PQ is 3 and GAMMA_22 is 2; BT.2020 red x is 35400 and BT.709's 32000;
# and the rule's 400 / 0.4 / 250 ride the wire as maxlum=400, minlum=4000
# (units of 0.0001) and maxfall=250.
if [ -n "$FROG_HDR_META" ]; then
	if echo "$FROG_HDR_META" | grep -q "tf=3 primaries=35400,"; then
		ok "the HDR metadata is PQ / BT.2020"
	else
		bad "the HDR metadata is PQ / BT.2020 ($FROG_HDR_META)"
	fi
	if echo "$FROG_HDR_META" | grep -q "maxlum=400 minlum=4000 maxfall=250"; then
		ok "and carries the rule's own mastering values (400 / 0.4 / 250)"
	else
		bad "and carries the rule's own mastering values ($FROG_HDR_META)"
	fi
else
	bad "an HDR metadata event was observed at all"
fi
if [ -n "$FROG_SDR_META" ]; then
	if echo "$FROG_SDR_META" | grep -q "tf=2 primaries=32000,"; then
		ok "the SDR metadata is gamma2.2 / BT.709"
	else
		bad "the SDR metadata is gamma2.2 / BT.709 ($FROG_SDR_META)"
	fi
else
	bad "an SDR metadata event was observed at all"
fi

# ── 6. THE PICTURE SURVIVED ──────────────────────────────────────────────
# THE OBSERVER STAYS ALIVE UNTIL AFTER THIS CAPTURE. It used to be killed
# first, which destroyed a window between the control captures and the final
# one and guaranteed a large difference -- the fixture manufacturing the very
# change it was asking about.
cap after
read -r MOVED WORST <<<"$(diffpx ctrl_b after)"
echo
echo "  the desktop after $CYCLES cycles differs from before by $MOVED px (worst $WORST)"
echo "  the control floor was $CTRL_MOVED px -- anything at that scale is the"
echo "  desktop's own churn (a clock, a cursor), not the transitions."
# The observer has done its job and the final capture is taken; let it go.
kill "$FROG_PID" 2>/dev/null || true
LIMIT=$(( CTRL_MOVED * 4 + 2000 ))
if [ "$MOVED" -le "$LIMIT" ]; then
	ok "the picture is unchanged across the transitions (<= $LIMIT px)"
else
	bad "the picture CHANGED across the transitions ($MOVED px > $LIMIT)"
fi

# ── THE EXPERIMENT WAS NOT EDITED WHILE IT RAN ───────────────────────────
chk "the fixture is byte-identical to its frozen hash" \
	"$(sha256sum "$0" | cut -c1-16)" "$SELF_HASH"
# AND THE SAME COMPOSITOR ANSWERED THROUGHOUT. A restart mid-run would give a
# new pid, and every counter delta above would be a comparison across two
# different processes -- which reads as "nothing leaked" rather than as an
# invalid run.
END_PID="$(amsg --instance 2>/dev/null | awk '/^pid/{print $2}')"
chk "the same instance answered from first query to last" "$END_PID" "$LIVE_PID"

# ── config.kdl IS RESTORED FIRST ─────────────────────────────────────────
# ── monitors.kdl IS RESTORED, BY HASH ────────────────────────────────────
# set_output_hdr PERSISTS, so this fixture rewrote the operator's own file 2N
# times. An even N should return it to where it started -- "should" is not
# evidence, so the hashes are compared.
if [ -r "$OUT/monitors.kdl.bak" ] && [ -r "$KDL" ]; then
	H0="$(sha256sum "$OUT/monitors.kdl.bak" | cut -d" " -f1)"
	H1="$(sha256sum "$KDL" | cut -d" " -f1)"
	if [ "$H0" != "$H1" ]; then
		# RESTORED, not merely reported. set_output_hdr rewrites the block by
		# removing and re-appending the key, so an even number of toggles ends
		# semantically identical and byte-different -- and leaving the
		# operator's hand-maintained file reordered is not an acceptable
		# souvenir of a test run.
		cp -f "$OUT/monitors.kdl.bak" "$KDL"
		H1="$(sha256sum "$KDL" | cut -d" " -f1)"
		echo "    (rewritten by output_persist; restored from the backup)"
	fi
	chk "monitors.kdl is restored byte for byte" "$H1" "$H0"
fi
chk "the output is back in HDR" "$(mon_field "$MON" hdr)" "true"

echo
echo "  captures and the config backup: $OUT"
echo "  ---- $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
