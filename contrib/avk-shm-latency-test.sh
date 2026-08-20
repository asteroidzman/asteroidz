#!/usr/bin/env bash
# avk-shm-latency-test.sh — does a wl_shm client still hold the event loop?
#
# THE BUG THIS MEASURES
#
# AVK copied a client's wl_shm buffer with a synchronous memcpy inside the
# surface-commit handler. On the operator's machine, with a 1637x1160 Qt window
# repainting, that was ~6.5 MB per commit, 26.5 GB in one session, memcpy at
# ~27% of all perf samples -- and, because the event loop is not in poll()
# while a handler runs, libinput reporting
#
#     USB Gaming Mouse: client bug: event processing lagging behind by 47-53ms
#     Cherry Corded Device: ... lagging behind by 30ms
#
# on the POINTER and the KEYBOARD, which is what rules out a device. The frame
# path was measured and exonerated: 4.8 ms worst-case handler, zero CPU sync
# waits, while input was 53 ms late.
#
# WHAT IS COMPARED, AND WHY IT IS ONE BINARY
#
# AZ_AVK_SYNC_UPLOAD=1 puts the copy back on the event loop. Same binary, same
# fixture, same client, same GPU: the only difference is which thread runs
# memcpy. A before/after taken across two builds would be comparing two
# compilations as much as two designs.
#
#     shm_commit_us_max    the longest one wl_surface commit held the loop.
#                          THE headline number.
#     shm_copy_us_total    main-thread time inside the copy, WHEREVER it ran.
#                          Kept separate on purpose: moving the block from the
#                          commit handler into the frame handler would improve
#                          the first number and change nothing about the lag,
#                          and this is what catches that.
#     shm_upload_bytes     the premise. Both arms must move a comparable
#                          volume, or the "faster" one is only doing less.
#
# THE CLIENT
#
# contrib/wlrepaint --churn repaints its whole surface and presents a FRESH
# wl_buffer every generation, which is what Qt/KDE clients do and what the perf
# trace showed: a rotated buffer pool warms the cache after two generations and
# every later commit is a cheap cache hit, so a fixture using one cannot reach
# the commit-handler copy at all. The rotating case is covered too, as its own
# scenario, because that is where the copy lands on the FRAME path instead --
# also the event loop, also input lag.
#
# Break tests, each of which MUST fail:
#
#   BREAK=sync-both     AZ_AVK_SYNC_UPLOAD=1 in BOTH arms. The comparison
#                       assertions must fail: if they pass with the fix
#                       disabled everywhere, they are not measuring the fix.
#   BREAK=async-both    the fix in both arms. The "the fixture can see the
#                       bug at all" assertion must fail -- that one asserts
#                       the PREMISE, that the synchronous arm really does
#                       block, and a fixture whose slow arm is not slow proves
#                       nothing about the fast one.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-shm-latency"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-shm-latency-test: needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "avk-shm-latency-test: needs python3-pillow"; exit 1; }
WLREPAINT="$(dirname "$0")/wlrepaint/wlrepaint"
[ -x "$WLREPAINT" ] || { echo "avk-shm-latency-test: wlrepaint not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

# Effects off: the thing under test is a CPU copy on the event loop, and a blur
# chain would put GPU work and its own frame cost into every number here.
SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

BASE="${TMPDIR:-/tmp}/asteroidz-avk-shmlat-$$"

field() { python3 - "$1" "$2" <<'PY'
import json, sys
try:
    v = json.load(open(sys.argv[1])).get(sys.argv[2], "x")
except Exception:
    v = "x"
# JSON spelling, not Python's. A bool printed as "True" compared against
# "true" is a failure that looks exactly like the feature being off, which
# cost a run to work out once already.
print("true" if v is True else "false" if v is False else v)
PY
}
count_colour() { python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
want = tuple(int(sys.argv[2][i:i+2], 16) for i in (1, 3, 5))
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(n for n, c in im.getcolors(1 << 24) if c == want))
PY
}

# ── one arm ────────────────────────────────────────────────────────────────
# NAME is the label; MODE is sync|async; the rest are wlrepaint arguments.
#
# The window is large on purpose: the bug is proportional to the bytes, and a
# 256x256 surface copies 256 KB in a time no counter can separate from noise.
# RUN_S is deliberately short -- a fixture holding the shared headless lock for
# minutes is a fixture nobody runs.
RUN_S="${RUN_S:-6}"
run_arm() { # run_arm NAME sync|async <wlrepaint args...>
	local name="$1" mode="$2"; shift 2
	HL_OUTDIR="$BASE/$name"
	HL_WIDTH=1920 HL_HEIGHT=1200
	# Validation on in every arm, not only a break arm: this change moves a
	# write to an image frames may still be sampling onto a different thread,
	# and no screenshot can see that hazard. Only the layer can.
	HL_ENV="ASTEROIDZ_VK_DEBUG=1"
	local want_sync=0
	[ "$mode" = sync ] && want_sync=1
	[ "$BREAK" = sync-both ] && want_sync=1
	[ "$BREAK" = async-both ] && want_sync=0
	[ "$want_sync" = 1 ] && HL_ENV="$HL_ENV AZ_AVK_SYNC_UPLOAD=1"
	export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV
	HL_SPAWN_COLOR_IDX=0

	hl_start "$SCENE_KDL"
	"$WLREPAINT" "$@" > "$HL_OUTDIR/wlrepaint.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	hl_wait_client_count 1 60
	sleep 1
	# Reset AFTER the window has mapped and settled, so the numbers describe
	# steady-state repainting and not the first-map storm.
	hl_dispatch reset_avk_stats
	sleep "$RUN_S"
	hl_screenshot frame
	hl_get "get avk-stats" > "$HL_OUTDIR/stats.json"
	kill "$pid" 2>/dev/null
	hl_stop
}

read_arm() { # read_arm NAME -> sets A_* globals
	local s="$BASE/$1/stats.json"
	A_ASYNC="$(field "$s" shm_async_upload)"
	A_COMMIT_MAX="$(field "$s" shm_commit_us_max)"
	A_COMMIT_AVG="$(field "$s" shm_commit_us_avg)"
	A_COMMIT_N="$(field "$s" shm_commit_samples)"
	A_OVER5="$(field "$s" shm_commit_over_5ms)"
	A_COPY_MAX="$(field "$s" shm_copy_us_max)"
	A_COPY_TOTAL="$(field "$s" shm_copy_us_total)"
	A_BYTES="$(field "$s" shm_upload_bytes)"
	A_JOBS="$(field "$s" shm_async_jobs)"
	A_SYNCCOPIES="$(field "$s" shm_sync_copies)"
	A_JOINWAITS="$(field "$s" shm_async_join_waits)"
	A_JOINMAX="$(field "$s" shm_async_join_us_max)"
	A_VERR="$(field "$s" validation_errors)"
	A_VON="$(field "$s" validation_enabled)"
	echo "  $1: async=$A_ASYNC commit_us max=$A_COMMIT_MAX avg=$A_COMMIT_AVG" \
		"n=$A_COMMIT_N over5ms=$A_OVER5 | copy_us max=$A_COPY_MAX" \
		"total=$A_COPY_TOTAL | bytes=$A_BYTES jobs=$A_JOBS sync=$A_SYNCCOPIES" \
		"joinwaits=$A_JOINWAITS joinmax=$A_JOINMAX"
}

# ── scenario 1: a client that presents a new buffer every commit ───────────
# The reported call chain. The copy lands INSIDE az_avk_surface_commit().
echo "== churning buffers: the copy is in the commit handler =="
run_arm churn-sync  sync  --churn --fixed --size 3200x2200 --hold-ms 0 --title churnA
run_arm churn-async async --churn --fixed --size 3200x2200 --hold-ms 0 --title churnB

read_arm churn-sync
S_COMMIT_MAX="$A_COMMIT_MAX"; S_COMMIT_AVG="$A_COMMIT_AVG"
S_COPY_TOTAL="$A_COPY_TOTAL"; S_BYTES="$A_BYTES"; S_N="$A_COMMIT_N"
S_OVER5="$A_OVER5"; S_VERR="$A_VERR"; S_VON="$A_VON"
read_arm churn-async
C_COMMIT_MAX="$A_COMMIT_MAX"; C_COMMIT_AVG="$A_COMMIT_AVG"
C_COPY_TOTAL="$A_COPY_TOTAL"; C_BYTES="$A_BYTES"; C_N="$A_COMMIT_N"
C_JOBS="$A_JOBS"; C_JOINMAX="$A_JOINMAX"; C_VERR="$A_VERR"; C_VON="$A_VON"

# ── the premise, before any comparison ─────────────────────────────────────
# A latency fixture that measured an idle compositor would measure nothing, and
# would say so by passing.
hl_assert "the client really committed (sync arm, $S_N commits)" \
	"$([ "${S_N:-0}" -gt 100 ] && echo true || echo false)" "true"
hl_assert "and so did the async arm ($C_N commits)" \
	"$([ "${C_N:-0}" -gt 100 ] && echo true || echo false)" "true"
hl_assert "the sync arm moved real bytes ($S_BYTES)" \
	"$([ "${S_BYTES:-0}" -gt 500000000 ] && echo true || echo false)" "true"
# THE PREMISE THAT MATTERS: the same work in both arms. A "fix" that halved
# the bytes would beat every timing assertion below while fixing nothing.
hl_assert "and the async arm moved a comparable volume ($C_BYTES vs $S_BYTES)" \
	"$(python3 -c "
s=${S_BYTES:-0}; c=${C_BYTES:-0}
print('true' if s>0 and c > 0.5*s and c < 2.0*s else 'false')")" "true"
# THE OTHER PREMISE: that the slow arm is actually slow. If the synchronous
# commit handler does not block here, this fixture cannot see the bug and
# nothing it says about the fix means anything.
#
# THE SIZE IS PART OF THIS ASSERTION. At 1600x1100 the synchronous arm used to
# block for 9916us -- but most of that was not the copy. A fresh wl_buffer got
# a fresh staging buffer, and the first memcpy into it took a page fault on
# every one of its pages; the warm-buffer cache on struct avk_device removed
# that, and the same arm then blocked for 636us, which is under the threshold
# below. The premise was resting on an allocation artefact.
#
# So the surface is sized to blow the threshold on BANDWIDTH alone: ~28MB per
# buffer at the ~13GB/s this machine copies at is ~2.2ms, comfortably past
# 1ms, and it stays that way however cheap the allocation becomes.
#
# --fixed, or the size is a fiction: without it wlrepaint follows its configure
# and paints its TILE, so --size sets only the first buffer and every one after
# it is the output's size. The first attempt at this raised --size alone, the
# arm went on copying 8.3MB, and the number moved by noise.
hl_assert "the synchronous arm really does block the loop (${S_COMMIT_MAX}us)" \
	"$([ "${S_COMMIT_MAX:-0}" -gt 1000 ] && echo true || echo false)" "true"

# ── the result ─────────────────────────────────────────────────────────────
hl_assert "the fix is on in the async arm and off in the sync arm" \
	"$A_ASYNC/$(field "$BASE/churn-sync/stats.json" shm_async_upload)" \
	"true/false"
hl_assert "the async arm posted copies to the worker ($C_JOBS)" \
	"$([ "${C_JOBS:-0}" -gt 50 ] && echo true || echo false)" "true"
hl_assert "worst-case commit collapses: ${C_COMMIT_MAX}us vs ${S_COMMIT_MAX}us" \
	"$(python3 -c "
s=${S_COMMIT_MAX:-0}; c=${C_COMMIT_MAX:-0}
print('true' if s>0 and c < s/4.0 else 'false')")" "true"
hl_assert "and so does the average: ${C_COMMIT_AVG}us vs ${S_COMMIT_AVG}us" \
	"$(python3 -c "
s=${S_COMMIT_AVG:-0}; c=${C_COMMIT_AVG:-0}
print('true' if s>0 and c < s/4.0 else 'false')")" "true"
# Not just moved elsewhere on the same thread.
hl_assert "main-thread copy time collapses too: ${C_COPY_TOTAL}us vs ${S_COPY_TOTAL}us" \
	"$(python3 -c "
s=${S_COPY_TOTAL:-0}; c=${C_COPY_TOTAL:-0}
print('true' if s>0 and c < s/4.0 else 'false')")" "true"
hl_assert "no commit exceeded 5ms in the async arm (sync arm: $S_OVER5)" \
	"$(field "$BASE/churn-async/stats.json" shm_commit_over_5ms)" "0"

# ── the pixels, which are the only reason any of this is allowed ───────────
# wlrepaint's palette: generation A is red/green, B is blue/yellow. A surface
# whose copy never landed shows none of the four.
INK=0
for c in ff0000 00ff00 0000ff ffff00; do
	INK=$(( INK + $(count_colour "$BASE/churn-async/frame.png" "#$c") ))
done
hl_assert "the async arm still put the client's pixels on screen ($INK px)" \
	"$([ "$INK" -gt 1000000 ] && echo true || echo false)" "true"

# ── the hazard no screenshot can see ───────────────────────────────────────
# The copy now happens on another thread. What keeps it from landing in an
# image a frame in flight is still sampling is a timeline wait and a barrier,
# recorded on the main thread in submission order -- unchanged by this work,
# and checkable only by asking the validation layer.
hl_assert "the validation layer was actually loaded (sync arm)" "$S_VON" "true"
hl_assert "the validation layer was actually loaded (async arm)" "$C_VON" "true"
hl_assert "no validation errors with the copy on the event loop" "${S_VERR:-x}" "0"
hl_assert "no validation errors with the copy on a worker thread" "${C_VERR:-x}" "0"

# ── scenario 2: a client that rotates a pool ───────────────────────────────
# No new buffers, so the commit handler is cheap either way and the copy lands
# on the FRAME path instead. Same event loop, same input lag -- which is why
# shm_copy_us_total, and not shm_commit_us_max, is the number that has to move
# here. A fix that only emptied the commit handler would pass scenario 1 and
# fail this.
echo
echo "== rotating pool: the copy is on the frame path =="
run_arm pool-sync  sync  --size 1600x1100 --hold-ms 0 --buffers 2 --title poolA
run_arm pool-async async --size 1600x1100 --hold-ms 0 --buffers 2 --title poolB
read_arm pool-sync
P_COPY_TOTAL="$A_COPY_TOTAL"; P_COPY_MAX="$A_COPY_MAX"; P_BYTES="$A_BYTES"
read_arm pool-async
Q_COPY_TOTAL="$A_COPY_TOTAL"; Q_COPY_MAX="$A_COPY_MAX"; Q_BYTES="$A_BYTES"
Q_JOINWAITS="$A_JOINWAITS"; Q_JOBS="$A_JOBS"

hl_assert "the pooled client also moved real bytes ($P_BYTES)" \
	"$([ "${P_BYTES:-0}" -gt 500000000 ] && echo true || echo false)" "true"
hl_assert "and both arms moved a comparable volume ($Q_BYTES vs $P_BYTES)" \
	"$(python3 -c "
s=${P_BYTES:-0}; c=${Q_BYTES:-0}
print('true' if s>0 and c > 0.5*s and c < 2.0*s else 'false')")" "true"
hl_assert "main-thread copy time collapses on the frame path too: ${Q_COPY_TOTAL}us vs ${P_COPY_TOTAL}us" \
	"$(python3 -c "
s=${P_COPY_TOTAL:-0}; c=${Q_COPY_TOTAL:-0}
print('true' if s>0 and c < s/4.0 else 'false')")" "true"
# What is left is the join: a frame that arrived before the worker finished.
# It is allowed to be nonzero -- a client committing in the same dispatch round
# as the frame gives the worker no time at all -- but it must be a small
# fraction of what the copy used to cost, or the copy is simply being waited
# for in a new place.
echo "  note: ${Q_JOINWAITS} of ${Q_JOBS} copies had to be waited for," \
	"worst ${Q_COPY_MAX}us (sync arm's worst single copy: ${P_COPY_MAX}us)"

echo
echo "captures and stats under: $BASE"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
