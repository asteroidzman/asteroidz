#!/usr/bin/env bash
# avk-transform-classify.sh — WHICH defect is it, before anything is changed.
#
# M4F.2C left two transform failures open. Both were observed with blur on, a
# seam, and cross-output halo routing all active at once, so neither is yet
# attributable to anything. This script does nothing but classify:
#
#   A  no blur at all          is the output even capable of reaching idle?
#   B  blur, no seam           does blur alone do it?
#   C  blur crossing the seam  does the cross-output source path do it?
#
# times four transforms, reporting per-FRAME numbers rather than run totals. The
# 270-degree case reported "894 blur nodes"; 894 over ~89 frames is ten per
# frame and a frame-scheduling defect, while 894 in one walk is a traversal
# defect. Those are different investigations and the totals cannot tell them
# apart.
#
# It asserts nothing. It prints a table.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-transform-classify"

HL_WIDTH=800 HL_HEIGHT=600
HL_OUTPUTS=2
HL_KITTY_EXTRA="-o cursor_blink_interval=0 -o cursor_stop_blinking_after=0"
HL_ENV="ASTEROIDZ_RENDERER=avk ASTEROIDZ_VK_DEBUG=1"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-tfclass-$$"
HL_OUTDIR="$OUTDIR"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV HL_OUTPUTS HL_KITTY_EXTRA

BLUR_ON="border_radius 12
effects {
	shadow { enable 1; only-floating 0; size 24; blur 24; blur-background 1 }
	blur { enable 1; passes 3; radius 4 }
}
focused_opacity 0.9
unfocused_opacity 0.9"
BLUR_OFF="border_radius 12
effects {
	shadow { enable 1; only-floating 0; size 24; blur 24; blur-background 0 }
	blur { enable 0 }
}"

field() { python3 - "$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], 0))
except Exception:
    print(0)
PY
}

printf "%-4s %-10s %8s %8s %8s %8s %8s %8s %8s\n" \
	rr control frames seen emit per-fr replays rec cons

for RR in 0 1 2 3; do
	case $RR in
	1|3) X2=600 ;;
	*)   X2=800 ;;
	esac
	for CTL in none far seam; do
		HL_RR1=$RR HL_X2=$X2
		export HL_RR1 HL_X2
		if [ "$CTL" = "none" ]; then
			hl_start "$BLUR_OFF"
		else
			hl_start "$BLUR_ON"
		fi
		hl_reset_spawn_colors
		hl_spawn_kitty a >/dev/null; hl_wait_client_count 1 60
		hl_spawn_kitty b >/dev/null; hl_wait_client_count 2 60
		sleep 3
		if [ "$CTL" != "none" ]; then
			hl_spawn_wlbgeffect w 300 >/dev/null; hl_wait_client_count 3 60
			sleep 2
			hl_dispatch toggle_floating
			sleep 1
			if [ "$CTL" = "seam" ]; then
				# straddling the seam: half the window on each output
				hl_dispatch "move_window,$(( X2 - 200 )),150"
			else
				# FAR from the seam -- entirely inside output A, so a blur here
				# has no cross-output source at all. This is what separates
				# "blur" from "blur across a boundary".
				hl_dispatch "move_window,40,150"
			fi
			sleep 3
		fi

		# SETTLE, then measure a STATIC scene. Anything counted from here is
		# work the compositor generated for itself.
		sleep 3
		hl_dispatch reset_avk_stats
		sleep 6
		hl_get "get avk-stats" > "$OUTDIR/s.json"
		FR=$(field "$OUTDIR/s.json" frames)
		SEEN=$(field "$OUTDIR/s.json" blur_nodes_seen)
		EMIT=$(field "$OUTDIR/s.json" blur_nodes_emitted)
		REP=$(field "$OUTDIR/s.json" blur_prefix_replays)
		REC=$(field "$OUTDIR/s.json" blur_halo_damage_records)
		CONS=$(field "$OUTDIR/s.json" blur_halo_damage_frames)
		PER=0
		[ "${FR:-0}" -gt 0 ] && PER=$(( EMIT / FR ))
		printf "%-4s %-10s %8s %8s %8s %8s %8s %8s %8s\n" \
			"$RR" "$CTL" "$FR" "$SEEN" "$EMIT" "$PER" "$REP" "$REC" "$CONS"
		hl_stop
	done
done

echo
echo "logs: $OUTDIR"
echo "READ: 'frames' over 6 STATIC seconds. Anything above ~0 is self-generated."
