#!/usr/bin/env bash
# avk-oracle-test.sh — the first-divergence oracle, and proof it can fail.
#
# M4F.2C.4c. Everything known about the 180-degree stale strip was a postmortem:
# run the interaction, force a repaint afterwards, count what changed. That says
# some frame was wrong. It cannot say WHICH, because by the time the measurement
# is taken the frame that stranded the pixels is gone.
#
# AZ_FRAME_ORACLE=1 renders every frame twice against the same immutable
# snapshot -- the production partial render into the real scan-out buffer, and
# an independent full render into an image nothing presents -- reads both back
# on the GPU, and reports the FIRST frame where they differ, together with a
# per-blur comparison at three boundaries:
#
#     PREFIX   the reconstructed scene prefix, before the chain samples it
#     BLUR     the same image after the chain
#     OUTPUT   the scan-out target, after compositing and before presentation
#
# so a divergence classifies itself:
#
#     A  PREFIX WRONG                        segmented source reconstruction
#     B  PREFIX RIGHT / BLUR WRONG           the chain's coordinate mapping
#     C  PREFIX+BLUR RIGHT / OUTPUT WRONG    damage, scissor, buffer history
#
# WHAT IS ASSERTED HERE
#
#   premise      a build with a deliberate damage hole MUST be caught. An
#                oracle that has never reported a divergence is not evidence.
#   control      a clean single-output frame reports none, so a divergence
#                means something.
#   coverage     the taps are actually recorded -- dropped=0, and the number of
#                compared frames is above zero.
#
# MODE=fixture runs the 180-degree cross-seam interaction under the oracle and
# asserts it never diverges.
#
# MODE=transforms runs the damage matrix -- nine positions, from the interior
# to a one-pixel column -- at all eight wl_output_transforms, and requires
# partial == forced-full on every frame of every one. A damage bug that only
# shows at one edge is exactly what a transform permutes into a different edge.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-oracle"
MODE="${MODE:-assert}"

HL_WIDTH=800 HL_HEIGHT=600
HL_KITTY_EXTRA="-o cursor_blink_interval=0 -o cursor_stop_blinking_after=0"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-oracle-$$"
HL_OUTDIR="$OUTDIR"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_KITTY_EXTRA

BLUR_CFG="border_radius 12
effects {
	shadow { enable 1; only-floating 0; size 24; blur 24; blur-background 1 }
	blur { enable 1; passes 3; radius 4 }
}
focused_opacity 0.9
unfocused_opacity 0.9"

# ── reading the oracle's log ──────────────────────────────────────────────
# Every frame logs one `oracle <output> frame=N ... WRONG=n` line; the first
# divergence additionally carries a marker and is followed by the per-blur
# boundary lines.
# `grep -c` EXITS 1 ON ZERO MATCHES, so `grep -c ... || echo 0` prints "0\n0"
# and every comparison against it fails on a two-line string. Count with wc
# instead, which has one exit status and one line of output.
oracle_frames()  { grep "oracle HEADLESS" "$1" 2>/dev/null | wc -l; }
oracle_wrong()   { grep "WRONG=[1-9]" "$1" 2>/dev/null | wc -l; }
oracle_dropped() { grep -o "dropped=[0-9]*" "$1" 2>/dev/null | tail -1 |
	cut -d= -f2; }
# A tap that was dropped, or a readback buffer that grew while slots were live,
# means the run compared less than it claimed to -- or compared freed memory.
# Either way the result is not evidence, so both are asserted at zero.
oracle_invalid() { grep -o "invalidated=[0-9]*" "$1" 2>/dev/null | tail -1 |
	cut -d= -f2; }
oracle_first()   { grep -m1 "FIRST DIVERGENCE" "$1" 2>/dev/null; }

# asteroidz dup2s its own stderr to the state log, so the file the harness
# redirects the PROCESS into is empty. Read the state log.
log_of() { echo "$HL_OUTDIR/state/asteroidz/asteroidz.log"; }

# One settled desktop with a blurred window, then a small change and a forced
# repaint. Everything the oracle needs to have something to compare.
exercise() {
	hl_reset_spawn_colors
	hl_spawn_kitty a >/dev/null; hl_wait_client_count 1 60
	sleep 2
	hl_spawn_wlbgeffect w 300 >/dev/null; hl_wait_client_count 2 60
	sleep 2
	hl_dispatch toggle_floating
	sleep 1
	hl_dispatch "move_window,${1:-120},150"
	sleep 2
	hl_dispatch damage_all
	sleep 2
}

echo
echo "── premise: the oracle can fail ──────────────────────────────────────"
# A build that strands a rectangle on purpose. AZ_AVK_DAMAGE_HOLE subtracts it
# AFTER the ring has rotated, so it is never redrawn and never re-damaged --
# precisely a region wrongly believed to be up to date. The production frame
# must therefore differ from a full render of the same snapshot.
HL_OUTPUTS=1
HL_ENV="ASTEROIDZ_RENDERER=avk AZ_FRAME_ORACLE=1 AZ_AVK_DAMAGE_HOLE=300,200,120,90"
export HL_OUTPUTS HL_ENV
hl_start "$BLUR_CFG"
exercise 120
BROKEN_LOG=$(log_of)
BROKEN_FRAMES=$(oracle_frames "$BROKEN_LOG")
BROKEN_WRONG=$(oracle_wrong "$BROKEN_LOG")
hl_stop
echo "  damage-hole build: $BROKEN_FRAMES frames compared, $BROKEN_WRONG wrong"
oracle_first "$BROKEN_LOG" | sed 's/^/    /'
hl_assert "the oracle compares frames at all" \
	"$([ "${BROKEN_FRAMES:-0}" -gt 0 ] && echo true || echo false)" true
hl_assert "a deliberate damage hole is CAUGHT" \
	"$([ "${BROKEN_WRONG:-0}" -gt 0 ] && echo true || echo false)" true

echo
echo "── control: a clean single output ────────────────────────────────────"
# ── AZ_BLUR_CACHE=0, AND WHAT IT COST TO FIND OUT ──────────────────────────
#
# THIS ARM IS THE DAMAGE ORACLE. It asserts that a partial render of a frame
# equals a full render of the same snapshot -- i.e. that damage tracking left
# nothing stale. Nothing else.
#
# But the oracle's reference render is issued AFTER avk_render_frame(), by which
# point az_avk.h has already taken the M4I blur cache back (`blur_cache = NULL`,
# lent per frame because it is per output). So the reference reconstructs every
# blur LIVE while production served it from the cache, and the comparison was
# quietly cached-versus-live rather than partial-versus-full.
#
# Measured: with the cache on, 16 of 20 frames diverge, 245745 px at up to 47
# codes, bbox covering nearly the whole output. With it off, 0 of 19. Same
# result on the pre-M5 tree, so this is not new -- it is an instrument that has
# been answering a different question than its assertion claims, and failing.
#
# The cached-versus-live delta is a REAL and separate question and it is written
# down as one (docs/vulkan-native-architecture.md, the M4I cache section). It is
# not this fixture's, and leaving it here made this fixture permanently red
# while measuring nothing about damage.
HL_ENV="ASTEROIDZ_RENDERER=avk AZ_FRAME_ORACLE=1 AZ_BLUR_CACHE=0"
export HL_ENV
hl_start "$BLUR_CFG"
exercise 120
GOOD_LOG=$(log_of)
GOOD_FRAMES=$(oracle_frames "$GOOD_LOG")
GOOD_WRONG=$(oracle_wrong "$GOOD_LOG")
GOOD_DROP=$(oracle_dropped "$GOOD_LOG")
GOOD_INVALID=$(oracle_invalid "$GOOD_LOG")
hl_stop
echo "  clean build: $GOOD_FRAMES frames compared, $GOOD_WRONG wrong, " \
	"dropped=${GOOD_DROP:-?}"
oracle_first "$GOOD_LOG" | sed 's/^/    /'
hl_assert "the clean build compares frames" \
	"$([ "${GOOD_FRAMES:-0}" -gt 0 ] && echo true || echo false)" true
hl_assert "no tap was dropped" "${GOOD_DROP:-0}" 0
hl_assert "no comparison was invalidated" "${GOOD_INVALID:-0}" 0
hl_assert "a clean single output diverges on no frame" "${GOOD_WRONG:-0}" 0

if [ "$MODE" = "fixture" ]; then
	echo
	echo "── the 180-degree cross-seam fixture, under the oracle ───────────────"
	# Two outputs, the second adjacent, output 1 rotated 180, and a blurred
	# window straddling the seam so the source halo is live.
	HL_OUTPUTS=2 HL_RR1="${ORACLE_RR1:-2}" HL_RR2="${ORACLE_RR2:-0}" HL_X2=800
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_FRAME_ORACLE=1 ${ORACLE_EXTRA_ENV:-}"
	export HL_OUTPUTS HL_RR1 HL_RR2 HL_X2 HL_ENV
	hl_start "$BLUR_CFG"
	exercise 600
	FIX_LOG=$(log_of)
	echo "  frames compared: $(oracle_frames "$FIX_LOG")"
	echo "  divergent:       $(oracle_wrong "$FIX_LOG")"
	echo "  dropped:         $(oracle_dropped "$FIX_LOG")"
	echo "  invalidated:     $(oracle_invalid "$FIX_LOG")"
	FIX_FRAMES=$(oracle_frames "$FIX_LOG")
	FIX_WRONG=$(oracle_wrong "$FIX_LOG")
	echo
	grep -A 12 "FIRST DIVERGENCE" "$FIX_LOG" | sed 's/^/  /' | head -30
	hl_stop
	# ASSERTED, not merely printed. This is the configuration M4F.2C.4 left
	# open, and it is the one the fix has to hold.
	hl_assert "the 180-degree fixture compares frames" \
		"$([ "${FIX_FRAMES:-0}" -gt 0 ] && echo true || echo false)" true
	hl_assert "no tap was dropped at 180 degrees" \
		"$(oracle_dropped "$FIX_LOG")" 0
	hl_assert "no comparison was invalidated at 180 degrees" \
		"$(oracle_invalid "$FIX_LOG")" 0
	hl_assert "a 180-degree cross-seam frame never diverges from a full render" \
		"${FIX_WRONG:-1}" 0
fi

if [ "$MODE" = "transforms" ]; then
	echo
	echo "── partial == forced-full at every transform ─────────────────────────"
	# THE DAMAGE MATRIX, at all EIGHT wl_output_transforms and nine positions.
	#
	# Nine because this is where half-open rectangle errors live: a bug that
	# only shows along one edge is exactly what a transform permutes into a
	# different edge, and a centre-only fixture would find it at one transform
	# and miss it at seven. The positions are interior, each of the four edges,
	# a corner, a box straddling the corner the transform maps FROM, a box
	# almost entirely off the output, and a one-pixel column.
	#
	# Eight because the four rotations do not exercise the mirror transforms:
	# flipped-90 and 90 have the same extents and different anchors, and a sign
	# error in one of them is invisible in the other.
	#
	# The oracle compares EVERY frame of the sequence against a full render of
	# the same snapshot, so this asserts the whole interaction rather than its
	# final state.
	HL_OUTPUTS=1
	for RR in 0 1 2 3 4 5 6 7; do
		# The PRESENTATION extent transposes on transform parity -- 1, 3, 5 and
		# 7 -- so the positions are computed from it rather than from the mode.
		case "$RR" in
		1|3|5|7) LW=600; LH=800 ;;
		*)       LW=800; LH=600 ;;
		esac
		HL_RR1=$RR
		HL_ENV="ASTEROIDZ_RENDERER=avk AZ_FRAME_ORACLE=1 ${ORACLE_EXTRA_ENV:-}"
		export HL_OUTPUTS HL_RR1 HL_ENV
		hl_start "$BLUR_CFG"
		hl_reset_spawn_colors
		hl_spawn_kitty a >/dev/null; hl_wait_client_count 1 60
		sleep 2
		hl_spawn_wlbgeffect w 300 >/dev/null; hl_wait_client_count 2 60
		sleep 2
		hl_dispatch toggle_floating
		sleep 1
		for POS in "$(( LW / 2 - 150 )),$(( LH / 2 - 150 ))" \
				"0,$(( LH / 2 - 150 ))" "$(( LW - 300 )),$(( LH / 2 - 150 ))" \
				"$(( LW / 2 - 150 )),0" "$(( LW / 2 - 150 )),$(( LH - 300 ))" \
				"0,0" "$(( LW - 150 )),$(( LH - 150 ))" \
				"$(( LW - 60 )),$(( LH / 2 - 150 ))" \
				"$(( LW - 1 )),$(( LH / 2 - 150 ))"; do
			hl_dispatch "move_window,$POS"
			sleep 2
		done
		hl_dispatch damage_all
		sleep 2
		TLOG=$(log_of)
		TF=$(oracle_frames "$TLOG")
		TW=$(oracle_wrong "$TLOG")
		TD=$(oracle_dropped "$TLOG")
		TI=$(oracle_invalid "$TLOG")
		cp -f "$TLOG" "$HL_OUTDIR/rr$RR.log" 2>/dev/null || true
		hl_stop
		echo "  transform $RR: $TF frames compared, $TW divergent, " \
			"dropped=${TD:-?} invalidated=${TI:-?}"
		grep -m1 "FIRST DIVERGENCE" "$HL_OUTDIR/rr$RR.log" 2>/dev/null |
			sed 's/.*\] /    /'
		hl_assert "transform $RR compares frames at all" \
			"$([ "${TF:-0}" -gt 0 ] && echo true || echo false)" true
		hl_assert "transform $RR drops no tap" "${TD:-x}" 0
		hl_assert "transform $RR invalidates no comparison" "${TI:-x}" 0
		hl_assert "transform $RR: partial == forced-full on every frame" \
			"${TW:-1}" 0
	done
fi

echo
echo "logs: $OUTDIR"
hl_summary
