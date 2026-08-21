#!/usr/bin/env bash
# avk-encode-test.sh — the encoder's output, judged by a decoder.
#
# tests/test-avk-encode.c asserts what it can see from inside: the session is
# created, the conversion writes the right P010, the bitstream starts with a
# start code, the first NAL unit is a VPS, an IDR follows, nine P frames
# follow that. It scored 24/24 on a stream that decoded to ONE FRAME.
#
# The reason is worth stating, because it is the whole argument for this
# fixture existing beside that one. The P frames were correctly typed and
# correctly ordered, and carried no short-term reference picture set -- so a
# decoder had no way to know which picture they predicted from and dropped all
# nine, reporting "zero refs for a frame with P or B slices". Every property
# the C test could reach was right. The stream was not.
#
# So the oracle here is an actual decoder. What it asserts is not the shape of
# the bitstream but the pictures that come out of it.
#
# Needs: a GPU with H.265 encode (skips otherwise) and ffmpeg.
set -u

REPO="${ASTEROIDZ_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
BIN="${AVK_ENCODE_BIN:-$REPO/build/test-avk-encode}"

PASS=0
FAIL=0
ok()  { echo "  ok   $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "$BIN" ]; then
	echo "  --   no $BIN; build it first (ninja -C build test-avk-encode)"
	echo "  --   INCONCLUSIVE, not a pass"
	exit 2
fi
if ! command -v ffprobe >/dev/null || ! command -v ffmpeg >/dev/null; then
	echo "  --   ffmpeg/ffprobe not installed; the decoder IS the oracle here"
	echo "  --   INCONCLUSIVE, not a pass"
	exit 2
fi

WORK="$(mktemp -d /tmp/asteroidz-encode-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# The unit test does the encoding; this reads what it wrote. Running it here
# rather than duplicating the setup keeps one description of how an encoder is
# driven, which is the thing most likely to drift.
if ! AVK_ENCODE_OUT="$WORK/still.h265" AVK_HEIF_OUT="$WORK/still.heic" \
		AVK_VIDEO_OUT="$WORK/seq.h265" "$BIN" > "$WORK/unit.log" 2>&1; then
	if grep -q "^SKIP" "$WORK/unit.log"; then
		sed -n 's/^SKIP: /  --   /p' "$WORK/unit.log"
		echo "  --   INCONCLUSIVE, not a pass"
		exit 2
	fi
	bad "the unit test itself passes"
	tail -5 "$WORK/unit.log" | sed 's/^/       /'
	echo; echo "$PASS passed, $FAIL failed"
	exit 1
fi
ok "the encoder ran and wrote its streams"

# ── the still ───────────────────────────────────────────────────────────────
DESC="$(ffprobe -v error -show_entries \
	stream=codec_name,profile,width,height,pix_fmt,color_transfer,color_primaries \
	-of default=noprint_wrappers=1:nokey=1 "$WORK/still.h265" 2>/dev/null \
	| tr '\n' ' ')"
case "$DESC" in
	*hevc*"Main 10"*3840*2160*yuv420p10le*smpte2084*bt2020*)
		ok "the still is HEVC Main 10, 4K, 10-bit, PQ and BT.2020" ;;
	*)  bad "the still is HEVC Main 10, 4K, 10-bit, PQ and BT.2020 (got: $DESC)" ;;
esac

# A flat colour in must be a flat colour out. This is the check that catches a
# wrong level, a wrong CTB size or a wrong transform depth: each of them makes
# a valid stream whose pictures are noise, and every metadata check above still
# passes on all of them.
ffmpeg -v error -i "$WORK/still.h265" -frames:v 1 -pix_fmt yuv420p10le \
	-f rawvideo "$WORK/still.yuv" -y 2>/dev/null
STILL="$(python3 - "$WORK/still.yuv" <<'PY'
import sys, numpy as np
w, h = 3840, 2160
d = np.fromfile(sys.argv[1], dtype='<u2')
if d.size < w * h * 3 // 2:
    print("SHORT"); raise SystemExit
y = d[:w*h]
n = w*h//4
u = d[w*h:w*h+n]; v = d[w*h+n:w*h+2*n]
print("%d %d %d %.3f" % (y.mean(), u.mean(), v.mean(), y.std()))
PY
)"
set -- $STILL
if [ "${1:-SHORT}" = "SHORT" ]; then
	bad "the still decodes to a full frame"
else
	# RGB(0.25,0.55,0.85) through BT.2020 non-constant luminance, full range.
	if [ "$1" -ge 496 ] && [ "$1" -le 504 ] \
			&& [ "$2" -ge 702 ] && [ "$2" -le 712 ] \
			&& [ "$3" -ge 342 ] && [ "$3" -le 350 ]; then
		ok "the still decodes to the colour that went in (Y=$1 Cb=$2 Cr=$3)"
	else
		bad "the still decodes to the colour that went in (Y=$1 Cb=$2 Cr=$3, want 500/708/346)"
	fi
	# A flat picture decoding to a flat picture. The first working encoder
	# produced std 158 across the full range here while every header read
	# correctly.
	if python3 -c "import sys; sys.exit(0 if float('$4') < 2.0 else 1)"; then
		ok "and it is flat, as the source was (luma std $4)"
	else
		bad "and it is flat, as the source was (luma std $4, want < 2)"
	fi
fi

# ── the HEIF ────────────────────────────────────────────────────────────────
if command -v heif-info >/dev/null; then
	INFO="$(heif-info "$WORK/still.heic" 2>&1)"
	case "$INFO" in
		*"3840x2160"*"bit depth: 10"*) ok "the HEIF is 3840x2160 at 10 bits" ;;
		*) bad "the HEIF is 3840x2160 at 10 bits" ;;
	esac
	case "$INFO" in
		*"MaxCLL"*) ok "and carries its HDR10 metadata as container boxes" ;;
		*) bad "and carries its HDR10 metadata as container boxes" ;;
	esac
else
	echo "  --   heif-info not installed; the container is not checked"
fi

# ── the sequence ────────────────────────────────────────────────────────────
TYPES="$(ffprobe -v error -show_frames -show_entries frame=pict_type \
	-of csv=p=0 "$WORK/seq.h265" 2>/dev/null | tr -d '\r' | tr '\n' ' ')"
COUNT="$(echo "$TYPES" | wc -w)"
if [ "$COUNT" -eq 10 ]; then
	ok "all 10 pictures of the sequence decode"
else
	bad "all 10 pictures of the sequence decode (got $COUNT: $TYPES)"
fi
case "$TYPES" in
	"I P P P P P P P P P "*) ok "one key frame followed by nine predicted" ;;
	*) bad "one key frame followed by nine predicted (got: $TYPES)" ;;
esac

# The encoded source ramps in luma, frame by frame. A decoder that lost the
# references produces one frame, or ten identical ones -- both of which the
# NAL-type check above would still have called correct.
ffmpeg -v error -i "$WORK/seq.h265" -pix_fmt yuv420p10le -f rawvideo \
	"$WORK/seq.yuv" -y 2>/dev/null
RAMP="$(python3 - "$WORK/seq.yuv" <<'PY'
import sys, numpy as np
w, h = 1920, 1080
fsz = w * h * 3 // 2
d = np.fromfile(sys.argv[1], dtype='<u2')
n = d.size // fsz
ys = [float(d[i*fsz:i*fsz+w*h].mean()) for i in range(n)]
mono = all(ys[i] < ys[i+1] for i in range(len(ys)-1)) if n > 1 else False
print("%d %s %.0f %.0f" % (n, "yes" if mono else "no", ys[0] if ys else 0,
                           ys[-1] if ys else 0))
PY
)"
set -- $RAMP
if [ "${2:-no}" = "yes" ] && [ "${1:-0}" -eq 10 ]; then
	ok "and each picture differs from the last, in the order encoded ($3 -> $4)"
else
	bad "and each picture differs from the last, in the order encoded (frames ${1:-0}, monotonic ${2:-no})"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
