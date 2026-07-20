#!/usr/bin/env bash
# hdr-record.sh — records a genuine HDR10 (PQ/BT.2020, 10-bit) video of a
# real monitor, using the compositor's own new `screenshot_ui,rawhdr` raw
# capture mode instead of any external screen-capture pipeline (portal/
# pipewire capture forces the output down to SDR to produce something the
# capture pipeline can consume -- confirmed live, 2026-07-19).
#
# Usage: contrib/hdr-record.sh DURATION_SECONDS [INTERVAL_SECONDS] [OUTFILE]
#   DURATION_SECONDS   how long to capture
#   INTERVAL_SECONDS   seconds between frames (default 1) -- screenshot_ui
#                      freezes the output and does a full frame readback
#                      per call; a ~1/sec poll loop of the (cheaper, no
#                      encode) PNG mode already made a real system nearly
#                      unresponsive once. Do NOT go faster than this
#                      default without a good reason and without watching
#                      what happens.
#   OUTFILE            output video path (default ./hdr-recording.mp4)
#
# Env: ASTEROIDZ_INSTANCE_SIGNATURE must already point at the target
# compositor. HL_MON, if set, selects which monitor's metadata to tag the
# video with (default: whichever monitor currently has hdr_enabled=true).
#
# LIMITATION: mastering-display primaries (the G/B/R/WP chromaticity
# points x265's HDR10 SEI wants) aren't exposed over this project's IPC --
# only luminance figures are (hdr_max_luminance/hdr_min_luminance/
# hdr_max_fall). This script falls back to the standard BT.2020 reference
# primaries, which is the normal thing to do when a display's own EDID
# primaries aren't available -- it does NOT invalidate the luminance
# metadata (which IS read from the real monitor), just means the
# primaries in the SEI are a standard reference, not this exact panel's.
set -u

DURATION="${1:?usage: hdr-record.sh DURATION_SECONDS [INTERVAL_SECONDS] [OUTFILE]}"
INTERVAL="${2:-1}"
OUTFILE="${3:-./hdr-recording.mp4}"
RAWDIR=/tmp/asteroidz-rawhdr

: "${ASTEROIDZ_INSTANCE_SIGNATURE:?set ASTEROIDZ_INSTANCE_SIGNATURE first}"
[ -S "$ASTEROIDZ_INSTANCE_SIGNATURE" ] || { echo "hdr-record.sh: not a valid socket: $ASTEROIDZ_INSTANCE_SIGNATURE" >&2; exit 1; }

echo "=== finding the HDR-enabled monitor to tag metadata from ==="
MON_JSON="$(amsg get all-monitors)"
if [ -n "${HL_MON:-}" ]; then
	MON="$(jq -c ".monitors[] | select(.name==\"$HL_MON\")" <<<"$MON_JSON")"
else
	MON="$(jq -c '.monitors[] | select(.hdr_enabled==true)' <<<"$MON_JSON" | head -1)"
fi
if [ -z "$MON" ]; then
	echo "hdr-record.sh: no HDR-enabled monitor found (set HL_MON=<name> to force one)" >&2
	exit 1
fi
MON_NAME="$(jq -r '.name' <<<"$MON")"
W="$(jq -r '.mode_width' <<<"$MON")"
H="$(jq -r '.mode_height' <<<"$MON")"
MAX_LUM="$(jq -r '.hdr_max_luminance' <<<"$MON")"
MIN_LUM="$(jq -r '.hdr_min_luminance' <<<"$MON")"
MAX_FALL="$(jq -r '.hdr_max_fall' <<<"$MON")"
echo "target: $MON_NAME (${W}x${H}), max_luminance=$MAX_LUM min_luminance=$MIN_LUM max_fall=$MAX_FALL"

echo "=== clearing $RAWDIR ==="
rm -rf "$RAWDIR"
mkdir -p "$RAWDIR"

FRAMES=$((DURATION / INTERVAL))
echo "=== capturing $FRAMES raw frames, 1 every ${INTERVAL}s ==="
for i in $(seq 1 "$FRAMES"); do
	amsg dispatch screenshot_ui,rawhdr >/dev/null 2>&1
	sleep "$INTERVAL"
done

COUNT="$(ls "$RAWDIR"/frame_*.raw 2>/dev/null | wc -l)"
if [ "$COUNT" -eq 0 ]; then
	echo "hdr-record.sh: no frames captured -- is the compositor build new enough for screenshot_ui,rawhdr?" >&2
	exit 1
fi
echo "=== captured $COUNT frames, assembling ==="

cat "$RAWDIR"/frame_*.raw > "$RAWDIR/combined.raw"

# master-display L() wants luminance in 0.0001 cd/m^2 units; max-cll wants
# PLAIN cd/m^2 (it's a 16-bit field, max 65535 -- passing the x10000-scaled
# value here overflows mod 65536 and silently corrupts it, e.g. 418 cd/m^2
# came out as max_content=51232 the first time this was tried, 2026-07-19).
MAX_LUM_UNITS="$(awk -v v="$MAX_LUM" 'BEGIN{printf "%d", v*10000}')"
MIN_LUM_UNITS="$(awk -v v="$MIN_LUM" 'BEGIN{printf "%d", v*10000}')"

FPS="$(awk -v i="$INTERVAL" 'BEGIN{printf "%.6f", 1/i}')"

# ffmpeg's own -color_primaries/-color_trc flags don't reliably propagate
# into libx265's actual VUI bitstream settings (confirmed: color_space took
# effect, color_primaries/color_transfer stayed "unknown" without this) --
# colorprim/transfer/colormatrix inside -x265-params is what actually lands
# in the encoded stream, so both are set here (redundant but harmless).
ffmpeg -y -f rawvideo -pixel_format x2bgr10le -video_size "${W}x${H}" -framerate "$FPS" \
	-i "$RAWDIR/combined.raw" \
	-c:v libx265 -preset medium -crf 18 -pix_fmt yuv420p10le \
	-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
	-x265-params "hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(${MAX_LUM_UNITS},${MIN_LUM_UNITS}):max-cll=${MAX_LUM},${MAX_FALL}" \
	"$OUTFILE"

echo "=== done: $OUTFILE ==="
echo "verify with: ffprobe -show_entries stream=color_primaries,color_transfer,color_space -of default=noprint_wrappers=1 $OUTFILE"
echo "view with an HDR-aware player (e.g. mpv) on an HDR-capable display for it to mean anything."
