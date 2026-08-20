#!/usr/bin/env bash
# avk-live-matrix.sh -- the qualification matrix, on the real display.
#
# The headless matrix measures shape. This measures the BUDGET, which is the
# only place the budget exists: a headless GPU idles near 50MHz and its
# microseconds mean nothing against a 6944us frame interval.
#
# ── IT RUNS AGAINST THE USER'S OWN SESSION ───────────────────────────────
#
# Tags switch on the real screen, a scratch window appears and is moved and
# resized, and the user must be WATCHING. That is not a formality: live-mode
# testing in this project has produced two crashes and a full logout, and a
# backgrounded live test is how each of them started.
#
# ── WHAT IT WILL NOT TOUCH ───────────────────────────────────────────────
#
# The user's own windows. `move` and `resize` need a floating window, and
# toggling one of theirs would rearrange a desktop they are working in -- so
# the fixture spawns its own kitty, floats THAT, and kills it afterwards. The
# session is returned to the tag it started on.
#
# ── PER OUTPUT, ALWAYS ───────────────────────────────────────────────────
#
# DP-1 and HDMI-A-1 have different budgets (6944us against 16667us) and mixing
# them into one distribution has sent this investigation the wrong way three
# times. Nothing here aggregates across outputs.
set -u

MON="${MON:-DP-1}"
BUDGET_US="${BUDGET_US:-6944}"
TAG_A="${TAG_A:-2}"
TAG_B="${TAG_B:-1}"
REPS="${REPS:-10}"
LOG="$HOME/.local/state/asteroidz/asteroidz.log"
OUT="${TMPDIR:-/tmp}/avk-live-matrix-$$"
mkdir -p "$OUT"

command -v amsg >/dev/null || { echo "no amsg"; exit 1; }
PID="$(pgrep -x asteroidz | head -1)"
[ -n "$PID" ] || { echo "no running asteroidz"; exit 1; }

START_TAG="$(amsg get all-monitors | jq -r \
	".monitors[] | select(.name==\"$MON\") | .active_tags[0]")"
# ── IS ANYONE WATCHING FOR VUIDs? ─────────────────────────────────────────
#
# The VUID column below is `validation_errors`, which only ever increments from
# the Vulkan validation layer's callback. There are two AVK session files and
# only one loads it:
#
#   asteroidz-avk.desktop        ... asteroidz
#   asteroidz-avk-debug.desktop  ... ASTEROIDZ_VK_DEBUG=1 asteroidz
#
# An earlier run of this matrix reported "0 VUID" from the PLAIN session, where
# the counter could not have moved -- the same defect that let Path A emit
# twenty VUIDs a run unseen. So the answer is printed here, at the top, and the
# table says UNWATCHED rather than 0 when nothing is looking.
VALIDATION="$(amsg get avk-stats 2>/dev/null | jq -r '.validation_enabled')"
echo "══ live matrix ══ pid=$PID monitor=$MON budget=${BUDGET_US}us"
if [ "$VALIDATION" = "true" ]; then
	echo "   validation layer: ON -- the VUID column is a real result."
	echo "   NOTE: validation inflates CPU-side timing by ~100x. The gpu_frame"
	echo "   percentiles below are GPU timestamp spans and stay meaningful, but"
	echo "   this session cannot pace like the plain one, so the SCENARIO is not"
	echo "   the same workload. Do not compare these percentiles to a run made"
	echo "   without the layer."
else
	echo "   validation layer: OFF -- the VUID column CANNOT MOVE and means"
	echo "   nothing. Log into asteroidz-avk-debug for a real answer."
fi
echo "   the display WILL switch tags and a scratch window WILL appear."
echo "   starting tag on $MON: $START_TAG (restored at the end)"
echo

SCRATCH=""
SCRATCH_CLIENT_PID=""
# THE PID THE COMPOSITOR KNOWS, not the shell job.
#
# `kitty ... &` gives the job's pid, and killing that left the terminal on the
# user's desktop after the run: the process the compositor has a surface for is
# a different one. Asking the compositor which pid owns the window is the only
# identity that is actually the window -- and it is a recorded pid rather than
# a pattern, so this can never reach one of the user's own terminals.
cleanup() {
	amsg dispatch set_frame_trace,0 >/dev/null 2>&1
	if [ -n "$SCRATCH_CLIENT_PID" ]; then
		kill "$SCRATCH_CLIENT_PID" 2>/dev/null
	fi
	[ -n "$SCRATCH" ] && kill "$SCRATCH" 2>/dev/null
	amsg dispatch "view,$START_TAG" >/dev/null 2>&1
}
trap cleanup EXIT INT TERM

scenario() { # scenario NAME BODY...
	local name="$1"; shift
	local start
	start=$(wc -l < "$LOG")
	amsg dispatch reset_avk_stats >/dev/null 2>&1
	sleep 1
	amsg dispatch set_frame_trace,1 >/dev/null 2>&1
	"$@"
	amsg dispatch set_frame_trace,0 >/dev/null 2>&1
	sleep 0.5
	tail -n +"$(( start + 1 ))" "$LOG" > "$OUT/$name.log"
	amsg get avk-stats > "$OUT/$name.json" 2>/dev/null
	printf "  %-10s %6s trace lines\n" "$name" "$(wc -l < "$OUT/$name.log")"
}

do_idle() {
	local i=0
	while [ "$i" -lt "$REPS" ]; do
		amsg dispatch damage_all >/dev/null 2>&1
		sleep 0.4
		i=$(( i + 1 ))
	done
}
do_tag() {
	local i=0
	while [ "$i" -lt "$REPS" ]; do
		amsg dispatch "view,$TAG_B" >/dev/null 2>&1; sleep 1.2
		amsg dispatch "view,$TAG_A" >/dev/null 2>&1; sleep 1.2
		i=$(( i + 1 ))
	done
}
do_move() {
	local i=0
	while [ "$i" -lt "$(( REPS * 2 ))" ]; do
		amsg dispatch "move_window,$(( 200 + (i % 5) * 180 )),$(( 120 + (i % 4) * 140 ))" >/dev/null 2>&1
		sleep 0.35
		i=$(( i + 1 ))
	done
}
do_resize() {
	local i=0
	while [ "$i" -lt "$(( REPS * 2 ))" ]; do
		amsg dispatch "resize_window,$(( 800 + (i % 5) * 150 )),$(( 500 + (i % 4) * 130 ))" >/dev/null 2>&1
		sleep 0.35
		i=$(( i + 1 ))
	done
}

scenario idle do_idle
scenario tag  do_tag

# A window of OUR OWN for move/resize. Floating one of the user's would
# rearrange a desktop they are working in, and restoring a layout exactly is
# not something this fixture should be trusted to do.
echo "  spawning a scratch window for move/resize ..."
BEFORE_KITTY="$(amsg get all-clients 2>/dev/null | jq -r \
	'[.clients[] | select(.appid=="kitty") | .pid] | join(",")')"
kitty --title az-live-matrix-scratch >/dev/null 2>&1 &
SCRATCH=$!
sleep 3
# The kitty that is NEW since the line above -- so a user with terminals of
# their own open loses none of them.
SCRATCH_CLIENT_PID="$(amsg get all-clients 2>/dev/null | jq -r --arg b "$BEFORE_KITTY" \
	'.clients[] | select(.appid=="kitty") | .pid | tostring
	 | select(($b | split(",") | index(.)) == null)' | tail -1)"
amsg dispatch toggle_floating >/dev/null 2>&1
sleep 1
scenario move   do_move
scenario resize do_resize
[ -n "$SCRATCH_CLIENT_PID" ] && kill "$SCRATCH_CLIENT_PID" 2>/dev/null
kill "$SCRATCH" 2>/dev/null; SCRATCH=""; SCRATCH_CLIENT_PID=""
sleep 1

echo
python3 - "$OUT" "$MON" "$BUDGET_US" "$VALIDATION" <<'PY'
import os, re, sys, json
out, mon, budget = sys.argv[1], sys.argv[2], float(sys.argv[3])
watched = len(sys.argv) > 4 and sys.argv[4] == "true"
RX = re.compile(r'READ\s+out=(\S+).*?gpu_frame=([\d.]+) us')

def frames(name):
    p = os.path.join(out, name + ".log")
    if not os.path.exists(p):
        return []
    return [float(m.group(2)) for m in RX.finditer(open(p, errors='replace').read())
            if m.group(1) == mon]

def pct(v, q):
    if not v: return 0.0
    v = sorted(v); return v[min(int(len(v) * q), len(v) - 1)]

def longest(v, mult):
    best = cur = 0
    for x in v:
        cur = cur + 1 if x > budget * mult else 0
        best = max(best, cur)
    return best

def stat(name, key):
    try:
        return json.load(open(os.path.join(out, name + ".json"))).get(key)
    except Exception:
        return None

print("  THE BUDGET IS %.0fus. These microseconds are the real ones." % budget)
if len(sys.argv) > 4 and sys.argv[4] != "true":
    print("  VUID reads UNWATCHED: no validation layer in this session.")
print()
hdr = ("  %-8s %6s %8s %8s %8s %8s %6s %4s %4s %4s %6s %6s"
       % ("scenario","n","p50","p95","p99","max",">bud","1x","2x","3x","VUID","waits"))
print(hdr); print("  " + "-" * (len(hdr) - 2))
for name in ("idle", "tag", "move", "resize"):
    v = frames(name)
    if not v:
        print("  %-8s %6d  NO TRACE ROWS -- this measured nothing" % (name, 0))
        continue
    print("  %-8s %6d %8.0f %8.0f %8.0f %8.0f %6d %4d %4d %4d %6s %6s"
          % (name, len(v), pct(v,.50), pct(v,.95), pct(v,.99), max(v),
             sum(1 for x in v if x > budget),
             longest(v,1), longest(v,2), longest(v,3),
             stat(name, "validation_errors") if watched else "UNWATCH",
             stat(name, "cpu_sync_waits")))
PY
echo
echo "logs: $OUT"
