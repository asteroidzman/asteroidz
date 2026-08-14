#!/usr/bin/env bash
# avk-tag-ledger.sh -- what GROWS during a push, when the visible area does not.
#
# AZ_TAGTRACE established that an ordinary tag transition exposes a flat ~0.95
# screenfuls throughout: the outgoing strip plus the incoming strip is exactly
# one screen, the same visible content a static frame carries. So the transition
# is not expensive because of area, and the question becomes which execution
# COUNT grows while the area stays put.
#
# This runs the identical scene twice --- once holding still, once pushing ---
# and diffs the counters. Everything that is equal between the two is not the
# mechanism; everything that grows is a candidate.
#
# ── WHY BOTH COHORTS ARE DAMAGE-DRIVEN ───────────────────────────────────
#
# A static tag left alone damages almost nothing and runs almost no frames, so
# comparing it directly against a transition compares "did work" with "did
# nothing" and every counter grows. That says only that the transition rendered.
# The static cohort therefore drives damage_all, which redraws the whole output
# every cycle with the same windows visible: same area, same windows, same
# effects, no motion. The difference that survives is motion.
#
# PER-FRAME, NOT PER-RUN. The two cohorts render different numbers of frames,
# so every counter is divided by that cohort's own frame count before it is
# compared --- the trap that has already produced two wrong answers this
# milestone, both times by comparing totals over unequal populations.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-tag-ledger"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-tagledger-$$"
mkdir -p "$OUTDIR"

W="${W:-3840}"
H="${H:-2160}"
SCALE="${SCALE:-1.5}"
WINDOWS="${WINDOWS:-5}"
CYCLES="${CYCLES:-14}"

CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 20; blur_sigma 12; position_y 6 }
  blur { enable 1; optimized 1; passes 2; radius 6
    params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2 } } }"

export WLBGEFFECT_SSD=1

KEYS='
 frames:.frames, chains:.blur_chains, maxch:.blur_max_chains_per_frame,
 nodes:.blur_nodes_emitted, replays:.blur_prefix_replays,
 draws:.draws, surfaces:.surfaces, rects:.rects,
 shadow_draws:.shadow_draws, border_draws:.border_draws, blur_draws:.blur_draws,
 px_content:.px_out_content, px_blur:.px_out_blur_comp,
 px_shadow:.px_out_shadow, px_border:.px_out_border, px_target:.px_out_target,
 pfx_target:.px_prefix_target, pfx_content:.px_prefix_content,
 f50:.gpu_frame_blur_ns_p50, f95:.gpu_frame_blur_ns_p95,
 blur50:.gpu_blur_total_ns_p50, post50:.gpu_frame_postblur_ns_p50,
 barriers:.barriers, waits:.cpu_sync_waits, verr:.validation_errors
'
read_stats() { hl_get "get avk-stats" | jq -r "{$KEYS} | to_entries | \
	map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null; }
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

cohort() { # cohort MODE -> stats
	local mode="$1"
	local dir="$OUTDIR/$mode"
	mkdir -p "$dir"
	HL_OUTDIR="$dir"; HL_WIDTH="$W"; HL_HEIGHT="$H"; HL_SCALE1="$SCALE"
	HL_ENV="ASTEROIDZ_RENDERER=avk"
	export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
	hl_start "$CFG" >/dev/null 2>&1
	local i=0
	while [ "$i" -lt "$WINDOWS" ]; do
		hl_spawn_wlbgeffect "tl$i" 300 "tl$i" >/dev/null
		hl_wait_client_count "$(( i + 1 ))" 200
		# Half on the second tag, so a push has two populations to move. The
		# STATIC cohort keeps the identical split and simply never switches,
		# so both cohorts have the same windows in the same places.
		[ $(( i % 2 )) -eq 1 ] && hl_dispatch "tag,2" 0.3
		i=$(( i + 1 ))
	done
	sleep 3
	hl_dispatch reset_avk_stats 1
	local c=0
	while [ "$c" -lt "$CYCLES" ]; do
		case "$mode" in
		static) hl_dispatch damage_all ;;
		push)   if [ $(( c % 2 )) -eq 0 ]; then hl_dispatch view,2
		        else hl_dispatch view,1; fi ;;
		esac
		sleep 0.9
		c=$(( c + 1 ))
	done
	sleep 2
	local st; st="$(read_stats)"
	hl_stop >/dev/null 2>&1
	echo "$st"
}

echo "══ static vs push execution ledger ══ ${W}x${H} scale $SCALE windows=$WINDOWS"
echo "   visible area is ~0.95 screens in BOTH. what grows is the mechanism."
echo
S="$(cohort static)"
P="$(cohort push)"

FS="$(v frames "$S")"; FP="$(v frames "$P")"
echo "  frames: static=$FS push=$FP   (every row below is PER FRAME)"
echo
printf "  %-22s %12s %12s %10s\n" counter "static/frame" "push/frame" "ratio"
row() {
	awk -v n="$1" -v a="$(v "$2" "$S")" -v b="$(v "$2" "$P")" \
	    -v fa="$FS" -v fb="$FP" -v sc="${3:-1}" 'BEGIN{
		if (fa+0==0 || fb+0==0) { printf "  %-22s no frames\n", n; exit }
		x=a/fa/sc; y=b/fb/sc;
		printf "  %-22s %12.3f %12.3f %10s\n", n, x, y,
			(x>0 ? sprintf("%.2fx", y/x) : "-");
	}'
}
row "blur chains"       chains
row "blur nodes"        nodes
row "prefix replays"    replays
row "draws"             draws
row "surfaces"          surfaces
row "shadow draws"      shadow_draws
row "border draws"      border_draws
row "blur composites"   blur_draws
row "barriers"          barriers
echo
row "content Mpx"       px_content 1000000
row "blur-composite Mpx" px_blur    1000000
row "shadow Mpx"        px_shadow  1000000
row "border Mpx"        px_border  1000000
row "target Mpx"        px_target  1000000
row "prefix target Mpx" pfx_target 1000000
echo
# NULL-SAFE. These percentiles are null wherever the device cannot write
# timestamps, and `$(( null / 1000 ))` aborts the whole fixture under `set -u`
# -- so a machine with no GPU timing lost the entire ledger, which is the part
# that does not need a GPU at all.
us() { local x; x="$(v "$1" "$2")"; case "$x" in ''|null) echo "-" ;;
	*) echo $(( x / 1000 )) ;; esac; }
echo "  GPU (p50, us):  static frame=$(us f50 "$S") blur=$(us blur50 "$S") post=$(us post50 "$S")"
echo "                  push   frame=$(us f50 "$P") blur=$(us blur50 "$P") post=$(us post50 "$P")"
echo
BAD=""
[ "$(v waits "$S")" = "0" ] && [ "$(v waits "$P")" = "0" ] || BAD="$BAD cpu_waits"
[ "$(v verr "$S")" = "0" ] && [ "$(v verr "$P")" = "0" ] || BAD="$BAD validation"
[ "$(v chains "$P")" != "0" ] || BAD="$BAD no_blur_in_push"
[ -n "$BAD" ] && echo "  LEDGER SUSPECT:$BAD"
echo "logs: $OUTDIR"
