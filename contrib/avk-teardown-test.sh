#!/usr/bin/env bash
# avk-teardown-test.sh — the AVK renderer must be able to be destroyed.
#
# M3.5E crashed on the way OUT of a live session, twice, in two shapes:
#
#     az_avk_finish -> avk_dmabuf_importer_finish -> avk_retire_finish
#                   -> avk_image_destroy -> glibc abort
#     az_avk_finish -> avk_device_destroy -> vkDestroyDevice -> validation abort
#
# WHY THIS SCRIPT IS SHAPED THE WAY IT IS
#
# The first attempt at a teardown test ran ten headless cycles, reported every
# one of them clean, and was worthless: the binary it launched had no Vulkan in
# it, so `az_avk_finish()` returned at its first line and the thing being
# tested never executed. "No crash" is also what you get from code that never
# ran, and nothing in that harness could tell the two apart.
#
# Two consequences, both load-bearing here:
#
#   1. The binary is named explicitly and its identity is asserted. A test that
#      silently falls back to build/asteroidz tests whatever happens to be in
#      build/asteroidz.
#   2. Teardown execution is PROVEN per cycle, from markers the compositor
#      prints from inside the path itself:
#
#          CLEANUP_BEGIN            cleanup() was entered
#          AVK_TEARDOWN_BEGIN       az_avk_finish() was entered, and whether
#                                   there was anything for it to do
#          AVK_TEARDOWN_END         it returned having destroyed everything
#          CLEANUP_END              cleanup() returned
#
#      A cycle missing any of them is INVALID, not PASS, and is counted and
#      reported as such. `active=no` is a failure too: it means AVK was not
#      running, so the cycle tested nothing.
#
# The quit mechanism is SIGTERM, which is a real graceful exit here and not an
# abbreviation of one: handlesig() (asteroidz.c) routes SIGINT/SIGTERM to
# quit_now() -> wl_display_terminate() -> run() returns -> cleanup(). The
# dispatch `quit` deliberately does NOT take this path -- it raises the exit
# confirmation prompt and waits for a keystroke -- so a signal is both the
# correct thing to test and the only one a harness can use.
#
# RESOURCE CONFIGURATIONS
#
# Teardown only means something if there is something to tear down, and the
# crash was in the image/retire path, so each cycle populates it. CONFIG picks
# what:
#
#   bare              no clients past the harness baseline; the floor
#   shm               wlrotate: SHM images with partial-update generations
#   dmabuf            kitty: a GPU client, explicit-modifier import
#   mixed             both, plus pointer motion
#   cursor-xcursor    mixed + a themed cursor image
#   cursor-software   mixed + ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR: the cursor
#                     becomes an AVK-owned image composited by AVK
#   cursor-churn      cursor source switched repeatedly right up to the quit,
#                     so several cursor images are live or retired at exit
#
#   CONFIG=all runs every one of them.
#
# Hardware cursor planes are not represented: a headless output has no plane to
# put one on. That belongs to live acceptance and is not faked here.
#
#   BREAK=destroy-before-idle   AZ_AVK_NO_TEARDOWN_IDLE=1 removes the one
#                               device-idle wait that now precedes OUTPUT
#                               destruction -- which is the shipped ordering
#                               restored, and the ordering the aborts came
#                               from. It fails every cycle, in every
#                               configuration, with
#                               VUID-vkDestroySemaphore-semaphore-05149: the
#                               per-output present fence destroyed while a
#                               submitted batch still refers to it. MUST FAIL.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-teardown"
BREAK="${BREAK:-}"
CYCLES="${CYCLES:-10}"
CONFIG="${CONFIG:-mixed}"
# Short by construction: $HL_OUTDIR/xdg holds the Wayland socket, and
# sun_path is 108 bytes. A scratchpad path has already overflowed it once.
ROOT="${AVK_TEARDOWN_DIR:-/tmp/avk-teardown-$$}"

ALL_CONFIGS="bare shm dmabuf mixed cursor-xcursor cursor-software cursor-churn"
if [ "$CONFIG" = all ]; then
	CONFIGS="$ALL_CONFIGS"
else
	CONFIGS="$CONFIG"
fi

# The binary under test, named and checked. headless.sh resolves HL_ASTEROIDZ
# from $ASTEROIDZ at SOURCE time, so `ASTEROIDZ=x hl_start` -- a per-command
# assignment -- is far too late and silently runs build/asteroidz instead.
# That is precisely how the first version of this test came to prove nothing.
echo "binary under test: $HL_ASTEROIDZ"
if ! "$HL_ASTEROIDZ" -v 2>&1 | head -1; then
	echo "avk-teardown: cannot run $HL_ASTEROIDZ" >&2
	exit 1
fi

rm -rf "$ROOT"; mkdir -p "$ROOT"

INVALID=0
CYCLES_RUN=0

# grep -c, but always exactly one number on stdout even when the file is
# missing. Everything below feeds $(( )), which is unforgiving about the
# alternative.
cnt() {
	local n
	n=$(grep -c "$@" 2>/dev/null)
	echo "${n:-0}"
}

# ── one cycle ───────────────────────────────────────────────────────────────
#
# start -> exercise -> SIGTERM -> prove teardown ran -> prove it ran cleanly.
run_cycle() { # run_cycle CONFIG N
	local cfg="$1" n="$2"
	local dir="$ROOT/$cfg-$n"
	mkdir -p "$dir"

	HL_OUTDIR="$dir"
	HL_OUTPUTS=2
	HL_ENV="ASTEROIDZ_VK_DEBUG=1"
	[ "$cfg" = cursor-software ] && \
		HL_ENV="$HL_ENV ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1"
	[ "$BREAK" = destroy-before-idle ] && HL_ENV="$HL_ENV AZ_AVK_NO_TEARDOWN_IDLE=1"
	# ASan reports go to files, one per cycle, and do NOT abort: the point is
	# to capture the FIRST complaint, and an abort_on_error=1 run stops at
	# whichever one glibc noticed rather than whichever one came first.
	HL_ENV="$HL_ENV ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:log_path=$dir/asan"
	HL_ENV="$HL_ENV UBSAN_OPTIONS=print_stacktrace=1:log_path=$dir/ubsan"
	export HL_OUTDIR HL_OUTPUTS HL_ENV

	hl_start "cursor_hide_timeout 2" >/dev/null 2>&1
	sleep 2

	case "$cfg" in
	bare) ;;
	shm)
		"$HL_REPO/contrib/wlrotate/wlrotate" --buffers 4 --marks 6 \
			--size 300x200 --hold-ms 60 --idle-commits 6 \
			> "$dir/wlrotate.log" 2>&1 &
		HL_SPAWNED_PIDS+=("$!")
		sleep 2
		;;
	dmabuf)
		hl_spawn_kitty "td$n" >/dev/null 2>&1
		sleep 2
		;;
	mixed|cursor-xcursor|cursor-software|cursor-churn)
		hl_spawn_kitty "td$n" >/dev/null 2>&1
		"$HL_REPO/contrib/wlrotate/wlrotate" --buffers 4 --marks 6 \
			--size 300x200 --hold-ms 60 --idle-commits 6 \
			> "$dir/wlrotate.log" 2>&1 &
		HL_SPAWNED_PIDS+=("$!")
		sleep 2
		hl_move 400 400 >/dev/null 2>&1
		# Across the output boundary: a second output means a second target
		# and a second cursor image, both live at exit.
		hl_move 2200 500 >/dev/null 2>&1
		sleep 1
		;;
	esac

	if [ "$cfg" = cursor-churn ]; then
		# Several cursor images created, replaced and retired in quick
		# succession, with the last replacement as close to the quit as the
		# harness can put it. The idle hide (2s above) fires in here too, so
		# the hide/restore path contributes its own images.
		for i in 1 2 3 4 5 6; do
			hl_move $((300 + i * 90)) $((300 + i * 30)) >/dev/null 2>&1
			hl_move $((2100 + i * 40)) 400 >/dev/null 2>&1
		done
		sleep 0.2
	fi

	# The graceful exit under test.
	local pid="$HL_COMP_PID" rc=0
	kill -TERM "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null; rc=$?
	hl_stop >/dev/null 2>&1

	# ── did teardown actually run? ──────────────────────────────────────
	#
	# `grep -c` already prints 0 when it matches nothing; the obvious
	# `|| echo 0` on top of it prints a SECOND zero on every clean cycle,
	# which then reaches $(( )) as "0\n0" and turns the whole check into a
	# syntax error the run keeps going past.
	local log="$dir/state/asteroidz/asteroidz.log"
	local cb tb te ce active
	cb=$(cnt "CLEANUP_BEGIN" "$log")
	tb=$(cnt "AVK_TEARDOWN_BEGIN" "$log")
	te=$(cnt "AVK_TEARDOWN_END reason=complete" "$log")
	ce=$(cnt "CLEANUP_END" "$log")
	active=$(grep -o "AVK_TEARDOWN_BEGIN active=[a-z]*" "$log" 2>/dev/null \
		| head -1 | sed 's/.*active=//')

	if [ "$cb" -lt 1 ] || [ "$tb" -lt 1 ] || [ "${active:-no}" != yes ]; then
		echo "  cycle $cfg/$n: INVALID -- teardown not exercised" \
			"(cleanup=$cb avk_begin=$tb active=${active:-none})"
		INVALID=$((INVALID + 1))
		return
	fi
	CYCLES_RUN=$((CYCLES_RUN + 1))

	# ── did it run cleanly? ─────────────────────────────────────────────
	#
	# The ownership assertions come first, because they are the ones that fire
	# at the violation rather than at whatever the allocator noticed later. A
	# double-owned image reports itself here; a glibc abort reports itself
	# somewhere else entirely, or -- three times in four, on the desktop this
	# was found on -- not at all.
	local live_bad viol
	live_bad=$(cnt "NOT ZERO" "$log")
	viol=$(cnt -E "ownership violations|already queued for destruction|destroyed twice|was not idle before teardown" "$log")
	CY_LIVE=$live_bad; CY_VIOL=$viol

	local vuid sync abort asan ubsan
	vuid=$(cnt -E "Validation Error|VUID-" "$log")
	sync=$(cnt "SYNC-HAZARD" "$log")
	abort=$(cnt -E "free\(\):|corrupted|double free|munmap_chunk|Assertion .* failed" "$log")
	asan=$(cat "$dir"/asan.* 2>/dev/null | grep -c "ERROR: AddressSanitizer"); asan=${asan:-0}
	ubsan=$(cat "$dir"/ubsan.* 2>/dev/null | grep -c "runtime error"); ubsan=${ubsan:-0}

	CY_END=$te; CY_CE=$ce; CY_RC=$rc
	CY_VUID=$vuid; CY_SYNC=$sync; CY_ABORT=$abort; CY_ASAN=$asan; CY_UBSAN=$ubsan
	CY_DIR="$dir"
}

for cfg in $CONFIGS; do
	echo
	echo "=== $cfg: $CYCLES graceful teardown cycles ==="
	FAIL_END=0; FAIL_CE=0; FAIL_RC=0
	SUM_VUID=0; SUM_SYNC=0; SUM_ABORT=0; SUM_ASAN=0; SUM_UBSAN=0
	SUM_LIVE=0; SUM_VIOL=0
	FIRST_BAD=""
	for n in $(seq 1 "$CYCLES"); do
		CY_END=0; CY_CE=0; CY_RC=0; CY_LIVE=0; CY_VIOL=0
		CY_VUID=0; CY_SYNC=0; CY_ABORT=0; CY_ASAN=0; CY_UBSAN=0; CY_DIR=""
		run_cycle "$cfg" "$n"
		[ "$CY_END" -ge 1 ] || FAIL_END=$((FAIL_END + 1))
		[ "$CY_CE" -ge 1 ] || FAIL_CE=$((FAIL_CE + 1))
		[ "$CY_RC" -eq 0 ] || FAIL_RC=$((FAIL_RC + 1))
		SUM_VUID=$((SUM_VUID + CY_VUID)); SUM_SYNC=$((SUM_SYNC + CY_SYNC))
		SUM_ABORT=$((SUM_ABORT + CY_ABORT)); SUM_ASAN=$((SUM_ASAN + CY_ASAN))
		SUM_UBSAN=$((SUM_UBSAN + CY_UBSAN))
		SUM_LIVE=$((SUM_LIVE + CY_LIVE)); SUM_VIOL=$((SUM_VIOL + CY_VIOL))
		if [ -z "$FIRST_BAD" ] && [ -n "$CY_DIR" ]; then
			if [ "$CY_END" -lt 1 ] || [ "$CY_RC" -ne 0 ] || \
			   [ "$CY_VUID" -gt 0 ] || [ "$CY_ABORT" -gt 0 ] || \
			   [ "$CY_ASAN" -gt 0 ] || [ "$CY_LIVE" -gt 0 ] || \
			   [ "$CY_VIOL" -gt 0 ]; then
				FIRST_BAD="$CY_DIR"
			fi
		fi
	done

	hl_assert "[$cfg] az_avk_finish() completed in every cycle" \
		"$([ "$FAIL_END" -eq 0 ] && echo true || echo false)" "true"
	hl_assert "[$cfg] cleanup() returned in every cycle" \
		"$([ "$FAIL_CE" -eq 0 ] && echo true || echo false)" "true"
	hl_assert "[$cfg] the process exited normally in every cycle ($FAIL_RC bad)" \
		"$([ "$FAIL_RC" -eq 0 ] && echo true || echo false)" "true"
	hl_assert "[$cfg] no AVK ownership violation ($SUM_VIOL)" "$SUM_VIOL" "0"
	hl_assert "[$cfg] every AVK-owned device child was released before vkDestroyDevice ($SUM_LIVE)" \
		"$SUM_LIVE" "0"
	hl_assert "[$cfg] no glibc heap abort ($SUM_ABORT)" "$SUM_ABORT" "0"
	hl_assert "[$cfg] no AddressSanitizer report ($SUM_ASAN)" "$SUM_ASAN" "0"
	hl_assert "[$cfg] no UndefinedBehaviorSanitizer report ($SUM_UBSAN)" \
		"$SUM_UBSAN" "0"
	hl_assert "[$cfg] no Vulkan validation error ($SUM_VUID)" "$SUM_VUID" "0"
	hl_assert "[$cfg] no synchronization hazard ($SUM_SYNC)" "$SUM_SYNC" "0"

	if [ -n "$FIRST_BAD" ]; then
		echo "  -- first failing cycle: $FIRST_BAD"
		echo "  -- first complaint in it:"
		{ grep -m1 -E "NOT ZERO|ownership violation|already queued|destroyed twice|not idle before teardown|Validation Error|VUID-|SYNC-HAZARD|free\(\):|corrupted" \
			"$FIRST_BAD/state/asteroidz/asteroidz.log" 2>/dev/null
		  head -20 "$FIRST_BAD"/asan.* 2>/dev/null; } | sed 's/^/     /'
	fi
done

# An invalid cycle is not a passing one. Counted separately so it cannot be
# mistaken for either a pass or a product bug.
hl_assert "every cycle actually exercised AVK teardown ($INVALID invalid)" \
	"$INVALID" "0"
echo
echo "cycles that reached AVK teardown: $CYCLES_RUN"

hl_summary
