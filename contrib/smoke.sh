#!/usr/bin/env bash
# smoke.sh -- does the compositor survive real clients? Thirty seconds, no oracle.
#
# ── WHY THIS EXISTS, AND WHY IT IS NOT A FIXTURE ─────────────────────────
#
# An async shm upload shipped to the operator's machine and aborted the
# compositor on the first kitty window:
#
#     types/buffer/buffer.c:85: wlr_buffer_begin_data_ptr_access:
#         Assertion `!buffer->accessing_data_ptr' failed.
#
# It had a before/after performance measurement, and that measurement was
# taken with contrib/wlrepaint -- which never provoked the assert. No fixture
# would have been cheaper than this: the compositor died in under a second
# with a real toolkit client on screen. What was missing was not an oracle.
# It was starting the thing and looking.
#
# So this asserts NOTHING about pixels, geometry, colour or damage. Those cost
# fixtures, and fixtures are worth their price only where a wrong result still
# looks right. This is for the other class -- lifetime, threading, buffers,
# protocol -- where the failure is loud and the only question is whether
# anybody ran it.
#
# ── RUN IT UNCHANGED ─────────────────────────────────────────────────────
#
# The point is that it is never re-authored. A change does not get its own
# smoke test; it runs this one. The moment somebody edits it to suit a
# particular change, it stops being the thing every change shares.
#
#     flock -w 1800 /tmp/asteroidz-headless.lock ./contrib/smoke.sh
#
# ── WHAT IT ASSERTS ──────────────────────────────────────────────────────
#
#   1. the compositor was alive after the clients ran      (the liveness claim)
#   2. no assertion fired                                  (an abort can look
#   3. no abort/segfault was logged                         like a clean exit
#   4. no Vulkan validation error                           to a script)
#   5. it shut down on request rather than being killed
#
# and asserts as PREMISES that the clients actually mapped -- a smoke test
# whose clients never appeared proves the compositor survives nothing.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="smoke"
SECONDS_OF_LOAD="${SMOKE_SECONDS:-10}"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-smoke-$$"
mkdir -p "$OUTDIR"
LOG="$OUTDIR/state/asteroidz/asteroidz.log"
HL_OUTDIR="$OUTDIR"
HL_ENV="ASTEROIDZ_RENDERER=avk"
export HL_OUTDIR HL_ENV

REPAINT="$(cd "$(dirname "$0")" && pwd)/wlrepaint/wlrepaint"

echo "══ does the compositor survive real clients ══"
echo

# hl_start exits the script when a harness helper is missing, which is an
# ENVIRONMENT failure and must not be reported as "the compositor died". Say so
# and stop, rather than leaving the caller to read an empty transcript.
if ! hl_start "layout { titlebar { enable 0 } }" >"$OUTDIR/start.log" 2>&1; then
	echo "  SMOKE COULD NOT START -- this is the harness, not the compositor:"
	tail -3 "$OUTDIR/start.log" | sed 's/^/      /'
	echo "      (build the helpers: for d in contrib/*/; do make -C \"$d\"; done)"
	exit 2
fi

# A real toolkit client. kitty is the one that crashed it: it is what makes the
# scene call wlr_buffer_is_opaque() on a buffer somebody else may be holding.
hl_spawn_kitty smoke1 >/dev/null 2>&1
hl_wait_client_count 1 60 >/dev/null 2>&1

# A client committing a FRESH wl_buffer every generation -- the Qt/Dolphin
# behaviour, and the only shape that reaches the commit-handler upload path.
# A pool-rotating client does not: its copy lands on the frame path instead.
if [ -x "$REPAINT" ]; then
	"$REPAINT" --title smoke-shm --size 800x600 --solid 3060a0 \
		--frames 400 --hold-ms 10 --churn >"$OUTDIR/repaint.log" 2>&1 &
	HL_SPAWNED_PIDS+=("$!")
	hl_wait_client_count 2 60 >/dev/null 2>&1
fi

# A second toolkit client, so more than one surface is committing at once.
hl_spawn_kitty smoke2 >/dev/null 2>&1
hl_wait_client_count 3 60 >/dev/null 2>&1

sleep "$SECONDS_OF_LOAD"

N="$(hl_get "get all-clients" | jq '[.clients[]?]|length' 2>/dev/null || echo 0)"
echo "  clients that mapped: ${N:-0}"

# ── PREMISES ─────────────────────────────────────────────────────────────
hl_assert_true "PREMISE: clients actually mapped (${N:-0})" \
	"$([ "${N:-0}" -ge 2 ] && echo true || echo false)"

ALIVE="$(kill -0 "${HL_COMP_PID:-0}" 2>/dev/null && echo yes || echo no)"
hl_assert "PREMISE: the compositor was still running under load" "$ALIVE" "yes"

# ── THE CLAIMS ───────────────────────────────────────────────────────────
# Greps, because an abort is a message in the log and an exit code the harness
# may never see. Each is counted rather than tested for emptiness so a failure
# prints how many.
# grep -c PRINTS 0 and EXITS 1 when it matches nothing, so `|| echo 0`
# appends a second zero and every comparison below reads "0\n0". Default the
# empty case instead; that is the only case (a missing log) worth defaulting.
A="$(grep -ac 'Assertion' "$LOG" 2>/dev/null)"; A="${A:-0}"
[ "$A" -gt 0 ] && grep -a 'Assertion' "$LOG" | head -3 | sed 's/^/      /'
hl_assert "no assertion fired" "$A" "0"

B="$(grep -acE 'Segmentation fault|SIGSEGV|SIGABRT|core dumped|terminate called' "$LOG" 2>/dev/null)"; B="${B:-0}"
hl_assert "no abort or segfault logged" "$B" "0"

V="$(grep -acE 'VUID-|SYNC-HAZARD' "$LOG" 2>/dev/null)"; V="${V:-0}"
[ "$V" -gt 0 ] && grep -aE 'VUID-|SYNC-HAZARD' "$LOG" | head -3 | sed 's/^/      /'
hl_assert "no Vulkan validation error" "$V" "0"

hl_stop >/dev/null 2>&1
sleep 1

# 5. it went down because it was asked to, not because it fell over.
C="$(grep -ac 'CLEANUP_END' "$LOG" 2>/dev/null)"; C="${C:-0}"
hl_assert "shut down cleanly on request" \
	"$([ "$C" -ge 1 ] && echo yes || echo no)" "yes"

echo
hl_summary
rc=$?
[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
exit $rc
