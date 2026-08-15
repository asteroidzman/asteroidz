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
#   - every REQUIRED suite must be able to report a failure, or the audit FAILS
#
# The third was added after avk-blur-walker-test.sh was found running five
# failing assertions, printing them, and exiting 0 -- because it called
# hl_summary and then ran three more commands, so its status was the trailing
# `fi`. This runner read that 0 and left it out of the FAILED list. Strictly
# worse than the executable bit: that one was silent, this one lied.
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
m6a-sample-instant-test.sh         required
m6a-mixed-refresh-test.sh          required
m6a-idle-test.sh                   required
m6a-retarget-test.sh               required
anim-vector-continuity-test.sh     required
m6b-icc-drive-test.sh              required
m6b-preferred-desc-test.sh         required
m6b-transition-test.sh             required
m6b-frog-metadata-test.sh          required
amsg-identity-test.sh              required
cm-two-writer-test.sh              required
cm-native-caps-test.sh             required
m6b-hdr-transition-live.sh         live
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
avk-m5-path-a-test.sh              required
avk-m5-path-b-test.sh              required
avk-live-matrix.sh                 live
avk-gles-floor.sh                  manual
avk-final-matrix.sh                perf
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
xw-scale-test.sh                   required
xw-mixed-test.sh                   required
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
# The half that keeps this file honest as the tree changes. A discovered suite
# with no disposition is not "probably fine": it is a suite nobody has decided
# whether to gate on.
#
# The glob covers every fixture FAMILY, not just avk-*. It used to be avk-*.sh
# alone, which meant the m6a-*, m6b-* and amsg-* fixtures could be added and go
# unregistered indefinitely -- decay in exactly the half written to prevent it,
# and invisible for the same reason everything else here is: an unregistered
# suite and a suite that passes look identical from outside.
UNREG=""
for f in avk-*.sh m6a-*.sh m6b-*.sh amsg-*.sh cm-*.sh xw-*.sh; do
	[ "$f" = "avk-suite.sh" ] && continue
	[ -e "$f" ] || continue
	[ -n "$(reg_disp "$f")" ] || UNREG="$UNREG $f"
done

# ── 3. every REQUIRED suite can actually report a failure ─────────────────
#
# hl_summary returns 1 when an assertion failed. A script that calls it and then
# runs anything else exits with THAT command's status instead -- so the fixture
# prints its own failures and reports success to whoever reads its exit code.
#
# avk-blur-walker-test.sh did exactly this. It ended with an `if ... fi` after
# hl_summary, so its status was always 0: this runner executed it, saw success,
# and left it out of the FAILED list below while five of its assertions were
# failing. That is strictly worse than the missing executable bit this file was
# built for -- that one was silent, this one lies.
#
# So a required suite must both CALL hl_summary and end in a way that carries
# its status: `hl_summary` as the last effective line, or an explicit `exit`.
# perf, live and manual suites measure rather than assert and are exempt by
# disposition, which is what the disposition is for.
NOSTATUS=""
for n in $(reg_names); do
	[ "$(reg_disp "$n")" = required ] || continue
	[ -f "$n" ] || continue
	grep -q 'hl_summary' "$n" || { NOSTATUS="$NOSTATUS $n(no-summary)"; continue; }
	last="$(grep -vE '^[[:space:]]*(#|$)' "$n" | tail -1)"
	case "$last" in
		*hl_summary*|exit*|*"exit \"\$"*) ;;
		*) NOSTATUS="$NOSTATUS $n";;
	esac
done

for d in required perf live manual; do
	c=$(echo "$REGISTER" | awk -v d="$d" '$2==d' | wc -l)
	printf "  %-9s %2d\n" "$d" "$c"
done
echo "  discovered $(ls avk-*.sh m6a-*.sh m6b-*.sh amsg-*.sh cm-*.sh 2>/dev/null | grep -cv '^avk-suite.sh$')"
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
if [ -n "$NOSTATUS" ]; then
	# A suite that cannot fail is a suite that is not being run, one level up.
	echo "  REQUIRED BUT CANNOT REPORT FAILURE (end with hl_summary or exit \$?):"
	for n in $NOSTATUS; do echo "    $n"; done
	FAIL=1
fi

if [ "$WANT" = "--audit" ]; then
	if [ "$FAIL" = 0 ]; then
		echo "  every suite exists, executes, has a disposition, and can fail."
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

# ── /tmp IS A 24GB tmpfs AND THE SUITES FILL IT ──────────────────────────
#
# Every fixture writes captures to /tmp/asteroidz-<name>-<pid>/ and nothing ever
# removed them. A 4K PPM is 24MB and a rawhdr frame is 33MB, so a full required
# run exhausts the tmpfs partway through -- and what that looks like is NOT a
# disk error. Screenshots come back zero bytes so PIL reports "cannot identify
# image file", premises fail for no visible reason, and eventually every command
# that writes anything exits 1. The run that hit this reported four failures in
# avk-cursor-scale that were nothing but an unwritable /tmp.
#
# So each suite's output is reclaimed WHEN IT PASSES. A failure keeps its
# directory, because that is the evidence -- which is also why this cannot just
# rm the lot at the end.
free_mb() { df -Pm /tmp | awk 'NR==2 {print $4}'; }

SPACE_START="$(free_mb)"
if [ "$SPACE_START" -lt 4096 ]; then
	echo "  /tmp has only ${SPACE_START}MB free; a full run needs several GB." >&2
	echo "  Clear /tmp/asteroidz-* first." >&2
	exit 1
fi
echo "  /tmp: ${SPACE_START}MB free"

echo
PASS=0; FAILED=""
for n in $(reg_names); do
	[ "$(reg_disp "$n")" = "$FILTER" ] || continue
	echo "── $n ──"
	before="$(ls -d /tmp/asteroidz-* 2>/dev/null | sort)"
	if ./"$n"; then
		PASS=$(( PASS + 1 ))
		# Reclaim only what THIS suite created, and only because it passed.
		after="$(ls -d /tmp/asteroidz-* 2>/dev/null | sort)"
		comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after") \
			| while read -r d; do
				[ -n "$d" ] && rm -rf -- "$d"
			done
	else
		FAILED="$FAILED $n"
		echo "  (output kept in /tmp for diagnosis)"
	fi
	# A suite that leaves a compositor behind starves every one after it, and
	# the symptom is the NEXT suite failing. Reported rather than killed: this
	# runner does not own those processes and a pattern kill here would be one
	# typo away from the user's own session.
	# `pgrep -c` prints 0 AND exits 1 when nothing matches, so `|| echo 0`
	# appends a second zero and the comparison below says "integer expected".
	stray="$(pgrep -c -f 'build/asteroidz -c /tmp/asteroidz-' 2>/dev/null)"
	[ "${stray:-0}" -gt 0 ] 2>/dev/null && echo "  WARNING: $stray headless compositor(s) still running"
	echo "  /tmp: $(free_mb)MB free"
	echo
done
echo "══ $PASS passed ══"
if [ -n "$FAILED" ]; then
	echo "  FAILED:"
	for n in $FAILED; do echo "    $n"; done
	exit 1
fi
