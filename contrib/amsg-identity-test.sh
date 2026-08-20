#!/usr/bin/env bash
# amsg-identity-test.sh -- prove that a live gate can tell WHO answered.
#
# ── THE FAILURE THIS REPRODUCES ──────────────────────────────────────────
#
# An M6B live gate asserted `validation_enabled` as its precondition -- the
# guard against a vacuous `validation_errors: 0` -- and the assertion PASSED
# against a session that had no validation layer at all. amsg had fallen back
# to scanning XDG_RUNTIME_DIR and answered from a leftover headless test
# instance; every headless M6B fixture sets ASTEROIDZ_VK_DEBUG=1, so the wrong
# respondent reported exactly the value the precondition was looking for.
#
# The whole run's amsg-derived telemetry described a different compositor.
#
# ── WHY THIS FIXTURE AND NOT "KILL THE STRAYS FIRST" ─────────────────────
#
# Killing strays before a run would make the symptom go away and leave the
# defect in place, ready for the next run that forgets. The requirement is that
# a qualification harness can PROVE which instance answered, so this fixture
# builds the ambiguous condition on purpose and asserts behaviour in it.
#
# ── THE PREMISE THAT MAKES IT DISCRIMINATING ─────────────────────────────
#
# TWO instances must be alive at once, and they must DIFFER in something the
# check can see. Two identical compositors cannot distinguish "picked the right
# one" from "picked either one" -- the same coincidence trap that made a
# one-window frog test unable to see the wrong-display defect. Here the two
# differ by pid and by validation state, and the fixture asserts they differ
# before asserting anything else.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="amsg-identity"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-amsg-id-$$"
mkdir -p "$OUTDIR"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
AMSG="$REPO/build/amsg"
BIN="$REPO/build/asteroidz"
[ -x "$AMSG" ] || { echo "SKIP: build/amsg missing"; exit 77; }

echo "══ amsg instance identity ══ who answered, and can they see VUIDs"
echo

# ── instance A: the "target". validation ON. ─────────────────────────────
HL_OUTDIR="$OUTDIR/A"; mkdir -p "$HL_OUTDIR"
HL_ENV="ASTEROIDZ_VK_DEBUG=1"
export HL_OUTDIR HL_ENV
hl_start "" >"$OUTDIR/A.log" 2>&1
# THE HARNESS'S OWN SOCKET, IN THE HARNESS'S OWN RUNTIME DIR.
#
# The first version of this read $ASTEROIDZ_INSTANCE_SIGNATURE from the ambient
# environment and got /run/user/1000/asteroidz-2494.sock -- the operator's LIVE
# session. Every "pinned to the target" result below then came from amsg's
# fallback rather than from the pin, and the fixture written to prove the pin
# works was itself measuring the fallback. It also meant a fixture that
# dispatches could have reached the real desktop.
#
# hl_start already isolates: HL_XDG is its private XDG_RUNTIME_DIR and HL_SIG
# its socket inside it. Using those means /run/user/1000 is never scanned and
# the live session is unreachable from here by construction.
A_SOCK="$HL_SIG"
A_PID="$HL_COMP_PID"
RT="$HL_XDG"
sleep 1

# ── instance B: the "stray". validation OFF, started LATER, so the
#    mtime fallback prefers it -- which is precisely how the real failure
#    selected the wrong one. ─────────────────────────────────────────────
# Deliberately the SAME XDG_RUNTIME_DIR as A -- the harness's private one -- so
# both sockets are candidates for one scan. A separate runtime dir would make
# the fixture unable to reproduce the ambiguity at all.
B_LOG="$OUTDIR/B.log"
env -i PATH="$PATH" HOME="$HOME" \
	XDG_RUNTIME_DIR="$RT" \
	WLR_BACKENDS=headless WLR_RENDERER=pixman \
	\
	"$BIN" >"$B_LOG" 2>&1 &
B_PID=$!
sleep 4

cleanup() {
	kill "$B_PID" 2>/dev/null
	hl_stop >/dev/null 2>&1
	[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
}
trap cleanup EXIT

B_SOCK="$RT/asteroidz-$B_PID.sock"

# ── PREMISES ─────────────────────────────────────────────────────────────
hl_assert_true "PREMISE: the target instance is alive (pid $A_PID)" \
	"$(kill -0 "$A_PID" 2>/dev/null && echo true || echo false)"
hl_assert_true "PREMISE: a second instance is alive too (pid $B_PID)" \
	"$(kill -0 "$B_PID" 2>/dev/null && echo true || echo false)"
# Printed, because a wrong B_SOCK does NOT announce itself: amsg falls back to
# scanning and reaches B regardless, so every "pointed at the stray" assertion
# below would pass for a coincidental reason. The premise is what separates
# "the pin works" from "the fallback happened to agree with the pin".
echo "  A_SOCK $A_SOCK $([ -S "$A_SOCK" ] && echo '(socket)' || echo 'MISSING')"
echo "  B_SOCK $B_SOCK $([ -S "$B_SOCK" ] && echo '(socket)' || echo 'MISSING')"
ls -1 "$RT"/asteroidz-*.sock 2>/dev/null | sed 's/^/    /'
hl_assert_true "PREMISE: both sockets are in one XDG_RUNTIME_DIR" \
	"$([ -S "$A_SOCK" ] && [ -S "$B_SOCK" ] && echo true || echo false)"

N_SOCK=$(ls "$RT"/asteroidz-*.sock 2>/dev/null | wc -l)
echo "  $N_SOCK candidate sockets in $RT"
hl_assert_true "PREMISE: the scan is genuinely ambiguous ($N_SOCK >= 2)" \
	"$([ "$N_SOCK" -ge 2 ] && echo true || echo false)"

# THE DISCRIMINATOR. If both instances reported the same validation state, a
# passing check would prove nothing -- it could have reached either.
A_VAL="$(env ASTEROIDZ_INSTANCE_SIGNATURE="$A_SOCK" "$AMSG" --instance | awk '/^validation_enabled/{print $2}')"
B_VAL="$(env ASTEROIDZ_INSTANCE_SIGNATURE="$B_SOCK" "$AMSG" --instance | awk '/^validation_enabled/{print $2}')"
echo "  A validation=$A_VAL   B validation=$B_VAL"
hl_assert "PREMISE: the target has validation loaded" "$A_VAL" "true"
hl_assert "PREMISE: the stray does NOT -- so the check can discriminate" "$B_VAL" "false"

echo
echo "── the old selection: whichever socket is newest ─────────────────────"
# Reproduces the original defect exactly: no signature, no requirement, let
# the XDG_RUNTIME_DIR fallback choose. B was started later, so it wins.
OLD_PID="$(env -u ASTEROIDZ_INSTANCE_SIGNATURE XDG_RUNTIME_DIR="$RT" \
	"$AMSG" --instance 2>/dev/null | awk '/^pid/{print $2}')"
echo "  unpinned amsg reached pid $OLD_PID (target is $A_PID)"
hl_assert_true "the unpinned fallback reaches the STRAY, not the target" \
	"$([ "$OLD_PID" = "$B_PID" ] && echo true || echo false)"

# And this is the exact false pass: asking the unpinned endpoint whether
# validation is on gets `true` from a compositor that is not under test.
OLD_VAL="$(env -u ASTEROIDZ_INSTANCE_SIGNATURE XDG_RUNTIME_DIR="$RT" \
	"$AMSG" --instance 2>/dev/null | awk '/^validation_enabled/{print $2}')"
echo "  ...and reports validation_enabled=$OLD_VAL from it"

echo
echo "── the fixed selection: refuse, never guess ──────────────────────────"
# 1. Ambiguity is a failure, not a coin toss.
env -u ASTEROIDZ_INSTANCE_SIGNATURE XDG_RUNTIME_DIR="$RT" \
	"$AMSG" --require-pid="$A_PID" get instance >"$OUTDIR/amb.txt" 2>&1
AMB_RC=$?
echo "  ambiguous + --require-pid -> exit $AMB_RC: $(head -1 "$OUTDIR/amb.txt")"
hl_assert_true "an ambiguous target with a requirement REFUSES (non-zero)" \
	"$([ "$AMB_RC" -ne 0 ] && echo true || echo false)"
hl_assert_true "...and says so, rather than failing obscurely" \
	"$(grep -q "AMBIGUOUS TARGET" "$OUTDIR/amb.txt" && echo true || echo false)"

# 2. Pinned to the target: succeeds, and reaches the target.
PIN_PID="$(env ASTEROIDZ_INSTANCE_SIGNATURE="$A_SOCK" \
	"$AMSG" --require-pid="$A_PID" --instance | awk '/^pid/{print $2}')"
hl_assert "pinned to the target, the target answers" "$PIN_PID" "$A_PID"

# 3. Pinned to the target but pointed at the stray: REFUSES. This is the
#    assertion that matters -- it is the case that silently succeeded before.
env ASTEROIDZ_INSTANCE_SIGNATURE="$B_SOCK" \
	"$AMSG" --require-pid="$A_PID" get instance >"$OUTDIR/wrong.txt" 2>&1
WRONG_RC=$?
echo "  wrong instance -> exit $WRONG_RC: $(head -1 "$OUTDIR/wrong.txt")"
hl_assert_true "answering from the WRONG instance is refused" \
	"$([ "$WRONG_RC" -ne 0 ] && echo true || echo false)"
hl_assert_true "...naming it as a wrong-instance failure" \
	"$(grep -q "WRONG INSTANCE" "$OUTDIR/wrong.txt" && echo true || echo false)"

# 4. The validation requirement, against the instance that lacks it.
env ASTEROIDZ_INSTANCE_SIGNATURE="$B_SOCK" \
	"$AMSG" --require-validation get instance >"$OUTDIR/val.txt" 2>&1
VAL_RC=$?
hl_assert_true "--require-validation refuses an instance without the layer" \
	"$([ "$VAL_RC" -ne 0 ] && echo true || echo false)"
hl_assert_true "...naming the pid that answered" \
	"$(grep -q "VALIDATION NOT LOADED" "$OUTDIR/val.txt" && echo true || echo false)"

# 5. And it PASSES against the one that has it -- a requirement that can only
#    fail is not a check either.
env ASTEROIDZ_INSTANCE_SIGNATURE="$A_SOCK" \
	"$AMSG" --require-validation --require-pid="$A_PID" get instance \
	>"$OUTDIR/valok.txt" 2>&1
VALOK_RC=$?
hl_assert_true "--require-validation PASSES against the target" \
	"$([ "$VALOK_RC" -eq 0 ] && echo true || echo false)"

echo
echo "── build identity survives the binary being replaced ─────────────────"
# The reason `build` is the ELF build-id and not the path: a fresh install
# leaves a running compositor reading "/usr/bin/asteroidz (deleted)".
A_BUILD="$(env ASTEROIDZ_INSTANCE_SIGNATURE="$A_SOCK" "$AMSG" --instance | awk '/^build/{print $2}')"
echo "  target build-id $A_BUILD"
hl_assert_true "the instance reports a build id" \
	"$([ -n "$A_BUILD" ] && [ "$A_BUILD" != "?" ] && echo true || echo false)"
FILE_BUILD="$(file "$BIN" 2>/dev/null | grep -o 'BuildID\[sha1\]=[0-9a-f]*' | cut -d= -f2)"
hl_assert "and it matches the binary on disk" "$A_BUILD" "$FILE_BUILD"

env ASTEROIDZ_INSTANCE_SIGNATURE="$A_SOCK" \
	"$AMSG" --require-build=deadbeef get instance >"$OUTDIR/bld.txt" 2>&1
BLD_RC=$?
hl_assert_true "a wrong build id is refused" \
	"$([ "$BLD_RC" -ne 0 ] && echo true || echo false)"

echo
hl_summary
rc=$?
exit $rc
