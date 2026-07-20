#!/usr/bin/env bash
# live-visual-tour.sh — a ONE-OFF, manually-driven visual tour of layouts/
# animations/layer-shell/shadows/blur against the CALLER's own live session,
# for recording + visual artifact inspection. Not part of the assertion-based
# regression suite (contrib/regression/) -- this makes no pass/fail claims,
# it just sequences real dispatches with deliberate dwell time so a screen
# recording actually shows each thing clearly.
#
# Safety notes (see docs/regression-testing.md "Live-session mode"):
#   - Requires HL_LIVE_MON=<real output name> and a valid
#     ASTEROIDZ_INSTANCE_SIGNATURE already in the environment.
#   - hl_dispatch (contrib/lib/headless.sh) already refuses to disable
#     animations in live mode -- this script never even tries.
#   - This script DOES intentionally change the target monitor's layout for
#     demonstration purposes (unlike hl_reset, which no longer touches real
#     monitor layout at all) -- it restores the original layout/view at the
#     end, best-effort.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$REPO/contrib/lib/headless.sh"

: "${HL_LIVE_MON:?set HL_LIVE_MON=<real output name>}"
HL_OUTDIR="${HL_OUTDIR:-/tmp/asteroidz-tour-$$}"
mkdir -p "$HL_OUTDIR"
hl_start_live

# hl_shot LABEL -- a single screenshot_ui,rawhdr call (real 10-bit capture,
# see contrib/hdr-record.sh). NOT a poll loop: screenshot_ui freezes the
# target monitor and does a full frame readback per call -- looping this at
# ~1/sec previously made the whole real system nearly unresponsive
# (2026-07-19). One deliberate shot per transition is both cheaper and more
# useful (captures the state that actually matters instead of a random
# moment). Frames land in /tmp/asteroidz-rawhdr/frame_NNNNNN.raw in call
# order; assembled into one HDR10 video at the end of this script.
RAWDIR=/tmp/asteroidz-rawhdr
rm -rf "$RAWDIR"
mkdir -p "$RAWDIR"
hl_shot() {
	echo "  [hdr capture: $1]"
	env ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg dispatch screenshot_ui,rawhdr >/dev/null 2>&1
}

echo "=== capturing original state to restore at the end ==="
ORIG_ACTIVE_TAG="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .active_tags[0]")"
ORIG_TAG1_LAYOUT="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .tags[] | select(.index==1) | .layout")"
echo "original active tag: $ORIG_ACTIVE_TAG, tag1 layout: $ORIG_TAG1_LAYOUT"

echo "=== switching to tag 1 for the tour ==="
hl_dispatch "view,1" 1

echo "=== spawning 4 distinctly-colored windows ==="
kitty --title W1 -o background_opacity=1.0 -o background=#aa2222 sh -c "echo RED; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 1
kitty --title W2 -o background_opacity=1.0 -o background=#22aa22 sh -c "echo GREEN; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 1
kitty --title W3 -o background_opacity=1.0 -o background=#2222aa sh -c "echo BLUE; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 1
kitty --title W4 -o background_opacity=1.0 -o background=#aaaa22 sh -c "echo YELLOW; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
# NOT hl_wait_client_count here -- it checks the GLOBAL client count, which
# already includes your other real windows on the real monitor, so it would
# just spin to its own timeout uselessly. A fixed settle time instead.
sleep 2
hl_shot "4 windows spawned, before any layout forced"

echo "=== tile ==="
hl_dispatch "set_layout,tile" 3
hl_shot "tile"

echo "=== monocle ==="
hl_dispatch "set_layout,monocle" 3
hl_shot "monocle"

echo "=== scroller ==="
hl_dispatch "set_layout,scroller" 3
hl_shot "scroller"
hl_dispatch "scroller_stack,left" 2
hl_shot "scroller after stack"
hl_dispatch "scroller_expel" 2
hl_shot "scroller after expel"

echo "=== float ==="
hl_dispatch "set_layout,float" 3
hl_shot "float"

echo "=== back to tile, dwindle split demo (may no-op if dwindle_manual_split isn't on in your config) ==="
hl_dispatch "set_layout,tile" 2
hl_dispatch "dwindle_split_horizontal" 1
kitty --title W5 -o background_opacity=1.0 -o background=#aa22aa sh -c "echo MAGENTA; exec sleep 300" >/dev/null 2>&1 &
HL_SPAWNED_PIDS+=("$!")
sleep 3
hl_shot "tile + dwindle_split_horizontal + new window"

echo "=== open/close animation: one more window, then close it ==="
kitty --title W6 -o background_opacity=1.0 -o background=#22aaaa sh -c "echo CYAN; exec sleep 300" >/dev/null 2>&1 &
NEWPID="$!"
HL_SPAWNED_PIDS+=("$NEWPID")
sleep 2
hl_shot "just after W6 opened (open animation should be settled by now)"
NEWID="$(hl_get "get all-clients" | jq -r '.clients[] | select(.title=="W6") | .id')"
[ -n "$NEWID" ] && hl_dispatch "kill_client,force" 2
sleep 1
hl_shot "just after W6 closed (close animation should be settled by now)"

echo "=== layer-shell: exclusive-zone bar + overlay popup ==="
hl_spawn_wllayer top "top,left,right" 100 3840 100 none 6 "" bar >/dev/null
sleep 2
hl_shot "layer-shell exclusive-zone bar"
hl_spawn_wllayer overlay none 0 600 400 none 4 "" overlay >/dev/null
sleep 3
hl_shot "layer-shell overlay popup"

echo "=== overview toggle ==="
hl_dispatch "toggle_overview" 3
hl_shot "overview open"
hl_dispatch "toggle_overview" 2
hl_shot "overview closed"

echo "=== restoring original state ==="
case "$ORIG_TAG1_LAYOUT" in
	T) orig_layout_name=tile ;;
	M) orig_layout_name=monocle ;;
	S) orig_layout_name=scroller ;;
	F) orig_layout_name=float ;;
	*) orig_layout_name= ;;
esac
[ -n "$orig_layout_name" ] && hl_dispatch "set_layout,$orig_layout_name" 1
[ -n "$ORIG_ACTIVE_TAG" ] && hl_dispatch "view,$ORIG_ACTIVE_TAG" 1

hl_stop

echo "=== assembling captured frames into one HDR10 video ==="
COUNT="$(ls "$RAWDIR"/frame_*.raw 2>/dev/null | wc -l)"
if [ "$COUNT" -eq 0 ]; then
	echo "no frames captured, skipping assembly"
else
	MON_JSON="$(env ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg get all-monitors)"
	MON="$(jq -c ".monitors[] | select(.name==\"$HL_MON\")" <<<"$MON_JSON")"
	W="$(jq -r '.mode_width' <<<"$MON")"
	H="$(jq -r '.mode_height' <<<"$MON")"
	MAX_LUM="$(jq -r '.hdr_max_luminance' <<<"$MON")"
	MIN_LUM="$(jq -r '.hdr_min_luminance' <<<"$MON")"
	MAX_FALL="$(jq -r '.hdr_max_fall' <<<"$MON")"
	# master-display L() wants 0.0001 cd/m^2 units; max-cll wants PLAIN cd/m^2
	# (16-bit field -- the x10000-scaled value overflows mod 65536 and
	# silently corrupts it). colorprim/transfer/colormatrix inside
	# -x265-params is what actually lands in the bitstream VUI -- ffmpeg's
	# own -color_primaries/-color_trc flags didn't propagate there on their
	# own (confirmed: color_space took effect, the other two stayed
	# "unknown" without this, 2026-07-19).
	MAX_LUM_UNITS="$(awk -v v="$MAX_LUM" 'BEGIN{printf "%d", v*10000}')"
	MIN_LUM_UNITS="$(awk -v v="$MIN_LUM" 'BEGIN{printf "%d", v*10000}')"
	OUTFILE="${HL_OUTDIR}/tour-hdr.mp4"

	cat "$RAWDIR"/frame_*.raw > "$RAWDIR/combined.raw"
	ffmpeg -y -f rawvideo -pixel_format x2bgr10le -video_size "${W}x${H}" -framerate 1 \
		-i "$RAWDIR/combined.raw" \
		-c:v libx265 -preset medium -crf 18 -pix_fmt yuv420p10le \
		-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
		-x265-params "hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(${MAX_LUM_UNITS},${MIN_LUM_UNITS}):max-cll=${MAX_LUM},${MAX_FALL}" \
		"$OUTFILE" 2>"$HL_OUTDIR/ffmpeg.log"
	echo "$COUNT frames -> $OUTFILE"
fi
echo "=== tour finished ==="
