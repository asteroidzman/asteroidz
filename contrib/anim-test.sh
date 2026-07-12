#!/usr/bin/env bash
# anim-test.sh — headless visual test harness for asteroidz animations/effects.
#
# Runs a SECOND asteroidz instance on a virtual (headless) output with its own
# Wayland socket — it never touches your real session — sets a patterned
# wallpaper, opens a translucent terminal, then records an open+close of a
# second window and extracts the frames. Use it to *see* (and diff) rendering
# changes without a real display, e.g. to catch a one-frame flash on window
# open/close.
#
# Usage:
#   contrib/anim-test.sh [CONFIG] [LABEL]
#     CONFIG  KDL config to run (default: a generated minimal blur+animations
#             config at $OUTDIR/config.kdl). Keep it free of spawn-at-startup /
#             DMS so it can't disturb your session.
#     LABEL   output basename (default: anim)
#
# Env:
#   OUTDIR      work dir (default: /tmp/asteroidz-anim)
#   ASTEROIDZ   binary to test (default: /usr/bin/asteroidz)
#   WALLPAPER   image for the backdrop (default: assets or Pictures wallpaper)
#   OPEN_KDL    window-open KDL line (override to test settings)
#   FPS         capture/extract fps (default: 60)
#
# Outputs (under $OUTDIR):
#   rec.mp4            the recording
#   fr/f_*.png         extracted frames
#   <LABEL>.png        a montage scan of the whole clip
#   prints the brightest frames (a full-window colour "flash" reads as a
#   markedly brighter frame than the ~steady state).
#
# Requires: grim (unused directly but handy), wf-recorder, ffmpeg,
#           ImageMagick (montage/convert), mpvpaper, kitty.

CONFIG="${1:-}"
LABEL="${2:-anim}"
OUTDIR="${OUTDIR:-/tmp/asteroidz-anim}"
ASTEROIDZ="${ASTEROIDZ:-/usr/bin/asteroidz}"
FPS="${FPS:-60}"
OPEN_KDL="${OPEN_KDL:-window-open { type fade; duration 180; fade-begin-opacity 0.6; }}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WALLPAPER="${WALLPAPER:-}"
if [ -z "$WALLPAPER" ]; then
	for c in "$REPO/assets/asteroidz-battle-4k.png" "$HOME/Pictures/asteroidz-battle-4k.png" \
			 "$REPO/assets/wallpaper.png"; do
		[ -f "$c" ] && WALLPAPER="$c" && break
	done
fi

for t in wf-recorder ffmpeg montage convert mpvpaper kitty; do
	command -v "$t" >/dev/null || { echo "missing tool: $t" >&2; exit 1; }
done
[ -x "$ASTEROIDZ" ] || { echo "no asteroidz binary at $ASTEROIDZ" >&2; exit 1; }

mkdir -p "$OUTDIR"
if [ -z "$CONFIG" ]; then
	CONFIG="$OUTDIR/config.kdl"
	cat > "$CONFIG" <<EOF
border_radius 9
borderpx 4
animations {
    curve spring
    spring { damping 0.8; frequency 22; }
    $OPEN_KDL
    window-close { type fade; duration 300; fade-begin-opacity 1.0; }
}
effects {
    blur { enable 1; optimized 1; passes 2; radius 6; transparency-threshold 0.5;
        params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2; } }
    shadow { enable 1; size 5; blur 4; color 0x00000030; }
}
EOF
fi

# Run the test compositor in a DEDICATED, wiped runtime dir so it's fully
# isolated from your real session (own socket, no stale-socket confusion) and
# any leftover from a prior run is gone.
pkill -f "asteroidz -c $CONFIG" 2>/dev/null
sleep 1
TESTRT="$OUTDIR/xdg"
rm -rf "$TESTRT"; mkdir -p "$TESTRT"; chmod 700 "$TESTRT"

# launch the test compositor headless (force headless backend, no input devices)
env -i HOME="$HOME" PATH="$PATH" XDG_RUNTIME_DIR="$TESTRT" \
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
	"$ASTEROIDZ" -c "$CONFIG" > "$OUTDIR/log.txt" 2>&1 &

# wait (up to 8s) for it to create its socket in the dedicated dir
SOCK=""
for _ in $(seq 1 40); do
	sleep 0.2
	SOCK="$(ls "$TESTRT"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1 | xargs -r basename)"
	[ -n "$SOCK" ] && break
done
[ -n "$SOCK" ] || { echo "test compositor did not create a socket; see $OUTDIR/log.txt" >&2; cat "$OUTDIR/log.txt" >&2; exit 1; }
# all subsequent clients (mpvpaper/kitty/wf-recorder) use the test session
export XDG_RUNTIME_DIR="$TESTRT" WAYLAND_DISPLAY="$SOCK"
OUT="$(wayland-info 2>/dev/null | awk '/name: .HEADLESS/{gsub(/[^A-Za-z0-9-]/,"",$2); print $2; exit}')"
OUT="${OUT:-HEADLESS-1}"
echo "test compositor socket=$SOCK output=$OUT"

[ -n "$WALLPAPER" ] && mpvpaper -o "no-audio --loop-file=inf --image-display-duration=inf" \
	"$OUT" "$WALLPAPER" >/dev/null 2>&1 & MPV=$!
sleep 1.5
kitty -o background_opacity=0.65 -o background=#101418 --hold sh -c 'echo win1' >/dev/null 2>&1 & K1=$!
sleep 2

rm -f "$OUTDIR/rec.mp4"
wf-recorder -o "$OUT" -r "$FPS" -f "$OUTDIR/rec.mp4" >/dev/null 2>&1 & WF=$!
sleep 1
kitty -o background_opacity=0.65 -o background=#181014 --hold sh -c 'echo win2' >/dev/null 2>&1 & K2=$!
sleep 2
kill "$K2" 2>/dev/null      # close win2 -> close animation
sleep 2
kill -INT "$WF" 2>/dev/null; WF=""
sleep 1

rm -rf "$OUTDIR/fr" && mkdir -p "$OUTDIR/fr"
ffmpeg -y -i "$OUTDIR/rec.mp4" -vf "fps=$FPS" "$OUTDIR/fr/f_%03d.png" >/dev/null 2>&1
montage $(ls "$OUTDIR"/fr/f_*.png | sed -n '1~2p') -tile 10x -geometry 192x108+1+1 \
	"$OUTDIR/$LABEL.png" 2>/dev/null

# tear down the test instance + clients (identified by the unique config path)
kill "${MPV:-0}" "${WF:-0}" "${K1:-0}" 2>/dev/null
pkill -f "asteroidz -c $CONFIG" 2>/dev/null

echo "montage: $OUTDIR/$LABEL.png ($(ls "$OUTDIR"/fr | wc -l) frames)"
echo "brightest frames (a full-window colour flash reads as a bright outlier):"
for f in "$OUTDIR"/fr/f_*.png; do
	printf '%s %s\n' "$(convert "$f" -resize 1x1 -format '%[fx:mean]' info: 2>/dev/null)" "$f"
done | sort -rn | head -4
