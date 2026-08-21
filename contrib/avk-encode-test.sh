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
		AVK_VIDEO_OUT="$WORK/seq.h265" AVK_MP4_OUT="$WORK/rec.mp4" \
		AVK_ODD_HEIF_OUT="$WORK/odd.heic" "$BIN" > "$WORK/unit.log" 2>&1; then
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
	# AN ODD-SIZED SELECTION, which is what a screenshot drag produces and
	# what every other size here is not. The conformance window removes an
	# even number of luma samples, so an odd width cannot be expressed and
	# libheif refuses the whole file. Asked of libheif, not of our own
	# assertions, because the refusal is libheif's.
	ODD="$(heif-info "$WORK/odd.heic" 2>&1)"
	case "$ODD" in
		*"688x502"*) ok "an odd-sized selection rounds to an even 688x502" ;;
		*) bad "an odd-sized selection rounds to an even 688x502" ;;
	esac
	if heif-convert "$WORK/odd.heic" "$WORK/odd.png" >/dev/null 2>&1; then
		ok "and libheif decodes it rather than refusing the size"
	else
		bad "and libheif decodes it rather than refusing the size"
	fi
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
# All-intra is the shipping mode. Inter prediction decodes to noise on
# detailed content and is an open defect; a fixture demanding P frames would be
# demanding the broken path.
case "$TYPES" in
	*P*) bad "every picture is a key frame -- all-intra ships (got: $TYPES)" ;;
	"I I I I I I I I I I "*) ok "every picture is a key frame (all-intra)" ;;
	*) bad "every picture is a key frame (got: $TYPES)" ;;
esac

# The source is a HIGH-FREQUENCY pattern that moves every frame, and the check
# is that consecutive decoded pictures DIFFER and keep their detail. Detail is
# the point: every test here used to encode a flat colour, and a flat picture
# has almost no residual for a syntax desync to corrupt -- which is how the
# suite scored 24/24 while a recording of a real desktop decoded green.
ffmpeg -v error -i "$WORK/seq.h265" -pix_fmt yuv420p10le -f rawvideo \
	"$WORK/seq.yuv" -y 2>/dev/null
RAMP="$(python3 - "$WORK/seq.yuv" <<'PY'
import sys, numpy as np
w, h = 1920, 1080
fsz = w * h * 3 // 2
d = np.fromfile(sys.argv[1], dtype='<u2')
n = d.size // fsz
differ = n > 1
for i in range(n - 1):
    a = d[i*fsz:i*fsz+w*h]
    b = d[(i+1)*fsz:(i+1)*fsz+w*h]
    if np.array_equal(a, b):
        differ = False
std = float(d[:w*h].std()) if n else 0.0
print("%d %s %.1f 0" % (n, "yes" if differ else "no", std))
PY
)"
set -- $RAMP
if [ "${2:-no}" = "yes" ] && [ "${1:-0}" -eq 10 ]; then
	ok "and every picture differs from the one before it"
else
	bad "and every picture differs from the one before it (frames ${1:-0}, all-differ ${2:-no})"
fi
# A desynced decode collapses the picture toward a flat field. The detail
# arriving intact is the assertion the flat tests could never make.
if python3 -c "import sys; sys.exit(0 if float('${3:-0}') > 50.0 else 1)"; then
	ok "and the detail survived the round trip (luma std $3)"
else
	bad "and the detail survived the round trip (luma std ${3:-0}, want > 50)"
fi

# ── the container ───────────────────────────────────────────────────────────
#
# A recording is the mp4, not the elementary stream: the timing exists only
# here, and so does the size a player will actually use.
MP4="$(ffprobe -v error -show_entries \
	stream=codec_name,width,height,pix_fmt,nb_frames,color_transfer,color_primaries \
	-of default=noprint_wrappers=1:nokey=1 "$WORK/rec.mp4" 2>/dev/null \
	| tr '\n' ' ')"
case "$MP4" in
	*hevc*) ok "the mp4 opens and holds an HEVC track" ;;
	*) bad "the mp4 opens and holds an HEVC track (got: $MP4)" ;;
esac
# THE CODED SIZE MUST NOT LEAK. 1080 is coded as 1088 because the encoder
# aligns to 16, and without a conformance window the file says 1088 and every
# player shows eight rows of padding. The elementary stream looks correct
# either way.
case "$MP4" in
	*"1920 1080"*) ok "and declares the DISPLAY size, not the coded one" ;;
	*) bad "and declares the DISPLAY size, not the coded one (got: $MP4)" ;;
esac
case "$MP4" in
	*smpte2084*bt2020*) ok "and carries its HDR colour in the container" ;;
	*) bad "and carries its HDR colour in the container (got: $MP4)" ;;
esac
MP4N="$(ffprobe -v error -count_frames -show_entries stream=nb_read_frames \
	-of csv=p=0 "$WORK/rec.mp4" 2>/dev/null | tr -d ',\r\n')"
if [ "${MP4N:-0}" = "10" ]; then
	ok "and decodes to all ten of its frames"
else
	bad "and decodes to all ten of its frames (got ${MP4N:-none})"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
