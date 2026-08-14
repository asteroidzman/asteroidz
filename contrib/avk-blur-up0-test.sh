#!/usr/bin/env bash
# avk-blur-up0-test.sh — the up0-only scissor, correctness before performance.
#
# M4F.2D.2. AZ_BLUR_UP0_SCISSOR=1 restricts the FINAL full-resolution upsample
# to the region avk_blur_work_of() derived for it. Nothing else in the chain
# changes, and OFF is the existing baseline in the same binary.
#
# ── THREE-WAY, NOT TWO-WAY ────────────────────────────────────────────────
#
# Showing ON == OFF proves only that two runs agree. Each is also compared
# against a FORCED-FULL current-prefix render of the same scene, which is the
# oracle M4F has used throughout:
#
#     OFF vs FULL      the baseline is still right
#     ON  vs FULL      the optimisation is still right
#     OFF vs ON        and they are right in the same way
#
# All three must be 0 wrong pixels. No tolerance.
#
# ── AND THE OPTIMISATION MUST ACTUALLY DO SOMETHING ───────────────────────
#
# Every row prints the up0 area rendered with and without the scissor. If ON
# does not shade fewer fragments than OFF, the fixture is not exercising the
# thing being measured and no timing from it means anything -- so that is a
# premise, checked before any comparison is believed.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-up0"
PPM="$(cd "$(dirname "$0")" && pwd)/lib/ppm.py"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-up0-$$"
mkdir -p "$OUTDIR"
HL_OUTDIR="$OUTDIR"
HL_OUTPUTS=1
export HL_OUTDIR HL_OUTPUTS

LEVELS="${LEVELS:-3}"
RADIUS="${RADIUS:-5}"
SCALE="${SCALE:-1}"
# The output transform. up0's region is CAPTURE-LOCAL and the scissor is
# TARGET-LOCAL, and those are the same space for this pass -- but only a
# rotated output proves the claim rather than restating it.
RR="${RR:-0}"
KINDS="${KINDS:-small medium large move full twoblur}"

CFG="border_radius 0
effects { shadow { enable 0 }
  blur { enable 1; passes $LEVELS; radius $RADIUS } }"

# A ONE-PIXEL CHECKERBOARD backdrop. A blur of a flat field is that flat field,
# so on flat grey a wrongly scissored strip can be invisible; this is the
# content most sensitive to a missing fragment.
make_checker() {
	convert -size 2x2 xc:black -fill white \
		-draw 'point 0,0' -draw 'point 1,1' \
		-write mpr:tile +delete \
		-size 800x600 tile:mpr:tile "$OUTDIR/wallpaper.png" 2>/dev/null
	[ -s "$OUTDIR/wallpaper.png" ]
}

stats_of() {
	hl_get "get avk-stats" | jq -r '{c:.blur_chains, p:.blur_processed_pixels, r:.blur_required_work_pixels, u:.blur_removable_up0_pixels, w:.cpu_sync_waits, f:.fallback_frames, mc:.blur_max_chains_per_frame, gp:.graph_passes, gb:.graph_barriers, gu:.graph_uses, tc:.transient_creates, tr:.transient_reuses, ta:.transient_acquires, bp:.blur_passes} | "chains=\(.c) proc=\(.p) req=\(.r) rmup0=\(.u) waits=\(.w) fallback=\(.f) maxchains=\(.mc) gpasses=\(.gp) gbarriers=\(.gb) guses=\(.gu) tcreate=\(.tc) treuse=\(.tr) tacq=\(.ta) bpasses=\(.bp)"' 2>/dev/null
}
f() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

# run KIND NAME ON [EXTRA_ENV] -> capture + stats
run_case() {
	local kind="$1" name="$2" on="$3" extra="${4:-}"
	local cdir="$OUTDIR/cap-$name"
	mkdir -p "$cdir"
	local pulse=""
	case "$kind" in
	# A BOUNDED number of pulses: the scene has to SETTLE before the
	# forced-full oracle runs, or the two captures are of different desktops
	# and the baseline "fails" by exactly one pulse rectangle.
	small)  pulse="16x16@120,120:400:6" ;;
	medium) pulse="128x96@100,80:400:6" ;;
	large)  pulse="360x260@20,20:400:6" ;;
	esac
	HL_SCALE1="$SCALE"
	HL_RR1="$RR"
	HL_ENV="ASTEROIDZ_RENDERER=avk AZ_TRANSIENT_POISON=1 \
AZ_BLUR_UP0_SCISSOR=$on AZ_AVK_CAPTURE_DIR=$cdir $extra"
	export HL_ENV HL_SCALE1 HL_RR1
	hl_start "$CFG"
	if [ -n "$pulse" ]; then
		export WLBGEFFECT_PULSE="$pulse" WLBGEFFECT_NO_BLUR=1
		hl_spawn_wlbgeffect back 120 "back-$name" ff202020 >/dev/null
		hl_wait_client_count 1 60
		unset WLBGEFFECT_PULSE WLBGEFFECT_NO_BLUR
	fi
	hl_spawn_wlbgeffect front 120 "front-$name" >/dev/null
	hl_wait_client_count "$([ -n "$pulse" ] && echo 2 || echo 1)" 60
	if [ "$kind" = "toplevel" ]; then
		# A REAL COMPOSITOR-SIDE PRODUCER. wlbgeffect supplies its blur region
		# through ext-background-effect-v1; a kitty toplevel gets its blur node
		# from asteroidz itself (effects { blur { enable 1 } }), which is the
		# producer path M4F.2B corrected. Proving the optimisation on the
		# synthetic client only would leave that wiring untested.
		# A TERMINAL THAT DOES NOT ANIMATE. kitty's cursor blinks by default,
		# so the scene never settles and the forced-full oracle compares two
		# different desktops -- which showed up as ALL THREE comparisons
		# failing, including OFF vs FULL, and an optimisation that is switched
		# off cannot break the baseline.
		export HL_KITTY_EXTRA="-o cursor_blink_interval=0"
		hl_spawn_kitty real >/dev/null
		hl_wait_client_count 2 60
		hl_dispatch toggle_floating
		sleep 1
	fi
	if [ "$kind" = "twoblur" ]; then
		# TRANSITIVE DEMAND: a second blur whose prefix contains the first, so
		# the first must produce pixels the second samples even where they are
		# not themselves presented. If the scissor were derived from local
		# damage instead of backward demand, this is what would break.
		hl_spawn_wlbgeffect second 120 "second-$name" >/dev/null
		hl_wait_client_count 3 60
	fi
	sleep 4
	case "$kind" in
	move) hl_dispatch toggle_floating; sleep 0.5
	      for i in 1 2 3 4 5 6; do
	          hl_dispatch "move_window,$(( 100 + i * 30 )),$(( 80 + i * 20 ))"
	          sleep 0.4
	      done ;;
	full) hl_dispatch damage_all; sleep 1 ;;
	resize)
	      # THE SCISSOR MUST FOLLOW DEMAND FRAME TO FRAME. Odd and even sizes
	      # alternately, so a stale scissor from the previous size would leave
	      # a strip the forced-full oracle then reports.
	      hl_dispatch toggle_floating; sleep 0.5
	      for wh in "301x203" "260x180" "333x241" "280x200"; do
	          hl_dispatch "resize_window,exact ${wh%x*} ${wh#*x}" 2>/dev/null \
	              || hl_dispatch "move_window,${wh%x*},${wh#*x}"
	          sleep 0.5
	      done ;;
	*)    sleep 4 ;;      # 6 pulses at 400 ms, then settled
	esac
	sleep 2
	# The pool total BEFORE the measurement window opens. Creates are pool
	# state and survive the reset, so the only meaningful figure is this
	# subtracted from the one at the end -- see the assertion below.
	local pre_creates
	pre_creates="$(hl_get "get avk-stats" | jq -r '.transient_creates' \
		2>/dev/null)"
	hl_dispatch reset_avk_stats
	sleep 2
	hl_dispatch capture_output
	sleep 2
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name.ppm" 2>/dev/null || true
	# THE FORCED-FULL ORACLE, from the same compositor: same scene, redrawn
	# whole. Anything the partial path left stale shows up here.
	hl_dispatch damage_all
	sleep 2
	hl_dispatch capture_output
	sleep 2
	cp -f "$cdir/HEADLESS-1.ppm" "$OUTDIR/$name-full.ppm" 2>/dev/null || true
	# The coordinate mapping the renderer itself reported, so a non-zero
	# capture origin is visible rather than assumed.
	grep -m1 "up0 scissor:" "$HL_OUTDIR/state/asteroidz/asteroidz.log" \
		2>/dev/null | sed 's/.*avk blur: /    /' >&2 || true
	local st
	st="$(stats_of)"
	local post_creates
	post_creates="$(echo "$st" | tr ' ' '\n' | sed -n 's/^tcreate=//p' | head -1)"
	st="$st dcreate=$(( ${post_creates:-0} - ${pre_creates:-0} ))"
	hl_stop
	echo "$st"
}

diffpx() { python3 "$PPM" diff "$1" "$2" 2>/dev/null | awk '{print $1}'; }

echo
echo "── the up0-only scissor: OFF vs ON vs forced-full ────────────────────"
hl_assert "PREMISE: a checkerboard backdrop was generated" \
	"$(make_checker && echo true || echo false)" true

for KIND in $KINDS; do
	echo
	echo "  ── $KIND ──"
	OFF="$(run_case "$KIND" "$KIND-off" 0)"
	ON="$(run_case "$KIND" "$KIND-on" 1)"
	echo "    OFF $OFF"
	echo "    ON  $ON"

	# PREMISE: the optimisation actually removed fragments. `processed` counts
	# what will be shaded, so with the scissor on it must drop by about the
	# up0 removable figure.
	POFF="$(f proc "$OFF")"; PON="$(f proc "$ON")"
	RM="$(f rmup0 "$OFF")"
	if [ "${POFF:-0}" -gt 0 ] 2>/dev/null && [ "${PON:-0}" -gt 0 ] 2>/dev/null; then
		awk -v o="$POFF" -v n="$PON" -v r="$RM" 'BEGIN{
			printf "    up0 fragments: OFF %.0f  ON %.0f  removed %.0f (%.1f%%), predicted %.0f\n",
				o, n, o-n, (o>0? 100*(o-n)/o : 0), r }'
	fi
	hl_assert "$KIND: the scissor removes fragments (OFF $POFF -> ON $PON)" \
		"$([ "${PON:-0}" -lt "${POFF:-0}" ] 2>/dev/null && echo true || echo false)" true

	# THREE-WAY EQUIVALENCE.
	D1="$(diffpx "$OUTDIR/$KIND-off.ppm" "$OUTDIR/$KIND-off-full.ppm")"
	D2="$(diffpx "$OUTDIR/$KIND-on.ppm" "$OUTDIR/$KIND-on-full.ppm")"
	D3="$(diffpx "$OUTDIR/$KIND-off.ppm" "$OUTDIR/$KIND-on.ppm")"
	echo "    OFF vs FULL $D1 px    ON vs FULL $D2 px    OFF vs ON $D3 px"
	hl_assert "$KIND: OFF == forced-full ($D1 px)" \
		"$([ "${D1:-1}" = "0" ] && echo true || echo false)" true
	hl_assert "$KIND: ON == forced-full ($D2 px)" \
		"$([ "${D2:-1}" = "0" ] && echo true || echo false)" true
	hl_assert "$KIND: ON == OFF ($D3 px)" \
		"$([ "${D3:-1}" = "0" ] && echo true || echo false)" true
	hl_assert "$KIND: no CPU waits with the scissor on" "$(f waits "$ON")" 0
	hl_assert "$KIND: no fallback frames with the scissor on" \
		"$(f fallback "$ON")" 0

	# ── THE GRAPH MUST NOT HAVE CHANGED ────────────────────────────────────
	# A single dynamic scissor is a state change inside one pass. If passes,
	# barriers, graph uses or blur passes differ between OFF and ON, the
	# prototype did something other than what it claims and no timing from it
	# would mean anything.
	for FIELD in gpasses gbarriers guses bpasses; do
		hl_assert "$KIND: $FIELD unchanged by the scissor ($(f $FIELD "$OFF"))" \
			"$(f $FIELD "$ON")" "$(f $FIELD "$OFF")"
	done
	printf "    graph OFF passes=%s barriers=%s uses=%s blur_passes=%s | ON %s/%s/%s/%s\n" \
		"$(f gpasses "$OFF")" "$(f gbarriers "$OFF")" "$(f guses "$OFF")" \
		"$(f bpasses "$OFF")" "$(f gpasses "$ON")" "$(f gbarriers "$ON")" \
		"$(f guses "$ON")" "$(f bpasses "$ON")"
	printf "    transients OFF create=%s reuse=%s acq=%s | ON create=%s reuse=%s acq=%s\n" \
		"$(f tcreate "$OFF")" "$(f treuse "$OFF")" "$(f tacq "$OFF")" \
		"$(f tcreate "$ON")" "$(f treuse "$ON")" "$(f tacq "$ON")"
	# ── CREATES ARE POOL STATE, SO ONLY A DELTA MEANS ANYTHING ────────────
	#
	# transient_creates counts allocations for the life of the pool and is NOT
	# cleared by reset_avk_stats, because an image allocated before the reset
	# is still allocated after it. Comparing the two TOTALS across two
	# separately launched compositors therefore compares how each run warmed
	# up, not what the scissor did: it read 59 against 58 on a move fixture
	# whose per-frame graph passes, barriers, uses and blur passes were
	# identical. (The same mistake in the other direction -- asserting the
	# total is zero -- was an earlier error here.)
	#
	# avk-blur-walker-test already had this right, and its comment says why:
	# "reading the total after an idle window and calling it creates while idle
	# reported 55 here and meant nothing". So take the same in-process delta.
	# It is also the stronger claim: not "the two runs allocated equally" but
	# "neither run allocated anything once warm".
	printf "    creates during the measured window: OFF %s  ON %s (totals %s / %s)\n" \
		"$(f dcreate "$OFF")" "$(f dcreate "$ON")" \
		"$(f tcreate "$OFF")" "$(f tcreate "$ON")"
	hl_assert "$KIND: the warm pool allocates nothing more, scissor off" \
		"$(f dcreate "$OFF")" 0
	hl_assert "$KIND: nor with the scissor on" "$(f dcreate "$ON")" 0
	hl_assert "$KIND: nor transient acquires" \
		"$(f tacq "$ON")" "$(f tacq "$OFF")"
done

echo
echo "logs: $OUTDIR"
hl_summary
