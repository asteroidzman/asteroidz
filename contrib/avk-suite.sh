#!/usr/bin/env bash
# avk-suite.sh -- the register of AVK test suites, and the audit that keeps it
# honest.
#
# ── WHY THIS EXISTS ──────────────────────────────────────────────────────
#
# contrib/avk-blur-required-test.sh shipped without its executable bit and
# nobody noticed, because nothing enumerates these suites: they are run one at a
# time, by name, by whoever remembers them. A suite that cannot execute is
# indistinguishable from a suite that was not run, and both look exactly like a
# clean milestone.
#
# So this is a REGISTER with a DISPOSITION for every file, plus one check:
#
#   - every registered suite must exist and be executable, or the audit FAILS
#   - every contrib/avk-*.sh must be registered, or the audit FAILS
#
# The second half is the part that keeps working. A static list decays the
# moment somebody adds a file; requiring a disposition for every discovered
# suite means a new one cannot arrive silently unclassified.
#
# This is deliberately not a test framework. It runs nothing by default and
# holds no result state. `--run required` exists because a register that cannot
# execute what it registers is the same problem one level up.
#
# ── THE DISPOSITIONS ─────────────────────────────────────────────────────
#
#   required   correctness. Must pass before a milestone closes.
#   perf       measures rather than asserts. Prints numbers, has no verdict, so
#              a green run of the required set does not depend on it.
#   live       needs the real session and the user watching. NEVER run from a
#              batch: see the standing rule about live-mode testing.
#   manual     an investigation kept because it can be re-run, not because it
#              is part of any gate.
set -u

cd "$(dirname "$0")" || exit 1

# name                             disposition
REGISTER="
avk-blur-cache-dirty.sh            required
avk-blur-cache-multi.sh            required
avk-blur-cache-test.sh             required
avk-blur-damage-test.sh            required
avk-blur-required-test.sh          required
avk-blur-seam-test.sh              required
avk-blur-walker-test.sh            required
avk-border-test.sh                 required
avk-capture-layout-test.sh         required
avk-clip-policy-test.sh            required
avk-crossoutput-border-test.sh     required
avk-crossoutput-round-test.sh      required
avk-cursor-content-test.sh         required
avk-cursor-hide-test.sh            required
avk-cursor-lifetime-test.sh        required
avk-cursor-owner-test.sh           required
avk-cursor-scale-test.sh           required
avk-cursor-test.sh                 required
avk-cursor-transform-test.sh       required
avk-damage-domains-test.sh         required
avk-damage-test.sh                 required
avk-dither-domain-test.sh          required
avk-dmabuf-feedback-test.sh        required
avk-frame-test.sh                  required
avk-gradient-border-test.sh        required
avk-gradient-crossoutput-test.sh   required
avk-gradient-test.sh               required
avk-graph-test.sh                  required
avk-occlusion-test.sh              required
avk-oracle-test.sh                 required
avk-rounded-alpha-test.sh          required
avk-rounded-persist-test.sh        required
avk-rounded-test.sh                required
avk-scale-transform-test.sh        required
avk-shadow-test.sh                 required
avk-shm-cache-test.sh              required
avk-shm-partial-test.sh            required
avk-shm-rotate-test.sh             required
avk-sync-test.sh                   required
avk-teardown-test.sh               required
avk-transform-test.sh              required
avk-blur-role-split.sh             perf
avk-blur-cost.sh                   perf
avk-blur-count-matrix.sh           perf
avk-blur-up0-ab.sh                 perf
avk-blur-up0-test.sh               perf
avk-cohort-test.sh                 perf
avk-decoration-cost.sh             perf
avk-frame-phase-profile.sh         perf
avk-graph-perf.sh                  perf
avk-idle-convergence-test.sh       perf
avk-prefix-share.sh                perf
avk-rect-cap-test.sh               perf
avk-tag-ledger.sh                  perf
avk-blur-cache-live-ab.sh          live
avk-software-cursor-acceptance.sh  live
avk-transform-live-test.sh         live
avk-oracle-runs.sh                 manual
avk-transform-classify.sh          manual
"

WANT="${1:---audit}"
FILTER="${2:-required}"

reg_names() { echo "$REGISTER" | awk 'NF {print $1}'; }
reg_disp()  { echo "$REGISTER" | awk -v n="$1" '$1==n {print $2}'; }

FAIL=0
echo "══ AVK suite register ══"
echo

# ── 1. every registered suite exists and can execute ──────────────────────
MISSING=""; NOEXEC=""
for n in $(reg_names); do
	if [ ! -f "$n" ]; then
		MISSING="$MISSING $n"
	elif [ ! -x "$n" ]; then
		NOEXEC="$NOEXEC $n"
	fi
done

# ── 2. every discovered suite is registered ───────────────────────────────
# The half that keeps this file honest as the tree changes. A new avk-*.sh with
# no disposition is not "probably fine": it is a suite nobody has decided
# whether to gate on.
UNREG=""
for f in avk-*.sh; do
	[ "$f" = "avk-suite.sh" ] && continue
	[ -n "$(reg_disp "$f")" ] || UNREG="$UNREG $f"
done

for d in required perf live manual; do
	c=$(echo "$REGISTER" | awk -v d="$d" '$2==d' | wc -l)
	printf "  %-9s %2d\n" "$d" "$c"
done
echo "  discovered $(ls avk-*.sh | grep -cv '^avk-suite.sh$')"
echo

if [ -n "$MISSING" ]; then
	echo "  REGISTERED BUT ABSENT:"
	for n in $MISSING; do echo "    $n"; done
	FAIL=1
fi
if [ -n "$NOEXEC" ]; then
	# THE ORIGINAL DEFECT. A suite without its executable bit is not a suite
	# that fails -- it is a suite that never runs, which reads as silence.
	echo "  PRESENT BUT NOT EXECUTABLE (chmod +x):"
	for n in $NOEXEC; do echo "    $n"; done
	FAIL=1
fi
if [ -n "$UNREG" ]; then
	echo "  DISCOVERED BUT UNREGISTERED (give each a disposition above):"
	for n in $UNREG; do echo "    $n"; done
	FAIL=1
fi

if [ "$WANT" = "--audit" ]; then
	if [ "$FAIL" = 0 ]; then
		echo "  every suite exists, executes, and has a disposition."
	fi
	exit "$FAIL"
fi

if [ "$WANT" != "--run" ]; then
	echo "usage: $0 [--audit | --run [required|perf|manual]]" >&2
	exit 2
fi
if [ "$FAIL" != 0 ]; then
	echo "  refusing to run: fix the audit first." >&2
	exit 1
fi
# `live` is not runnable from here, ever. Those need the real session with the
# user watching it, which is not something a batch runner can arrange.
if [ "$FILTER" = live ]; then
	echo "  live suites are run by hand, with the user watching. Not from here." >&2
	exit 2
fi

echo
PASS=0; FAILED=""
for n in $(reg_names); do
	[ "$(reg_disp "$n")" = "$FILTER" ] || continue
	echo "── $n ──"
	if ./"$n"; then
		PASS=$(( PASS + 1 ))
	else
		FAILED="$FAILED $n"
	fi
	echo
done
echo "══ $PASS passed ══"
if [ -n "$FAILED" ]; then
	echo "  FAILED:"
	for n in $FAILED; do echo "    $n"; done
	exit 1
fi
