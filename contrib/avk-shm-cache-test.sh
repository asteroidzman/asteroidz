#!/usr/bin/env bash
# avk-shm-cache-test.sh — does a CPU buffer get uploaded because its pixels
# changed, or because somebody looked at it?
#
# M3.5D.1's acceptance test. AVK used to re-upload every SHM-backed surface on
# every frame that drew it, because az_avk_image_for_buffer() treated "this
# buffer was encountered while walking the scene" as "this buffer has new
# pixels". On the live dual-monitor desktop that was 41.5 MB of CPU->GPU copy
# per output frame and 924 GB over one session, for a wallpaper that had not
# changed since it was drawn once.
#
# There are two ways to get this wrong and they fail in opposite directions,
# so both are tested here:
#
#   TOO MUCH   upload on every lookup. Correct pixels, ruinous cost. Caught by
#              the static-wallpaper assertions: thousands of lookups, zero
#              uploads after the first.
#
#   TOO LITTLE cache on buffer IDENTITY -- "I have already uploaded this
#              wlr_buffer, skip it". Free, and permanently wrong for any client
#              that reuses a wl_buffer, which the protocol explicitly allows.
#              Caught by contrib/wlreuse, which reuses ONE buffer for every
#              commit and changes only the bytes.
#
# The second is the important one. Every ordinary toolkit rotates through a
# pool of two or three buffers, so a pointer-identity cache LOOKS correct
# against kitty, against swaybg, and against every other client in this suite:
# each new pointer is a cache miss and the pixels get through anyway. wlreuse
# exists because nothing else in the tree makes buffer identity a lie.
#
# Break tests, each of which MUST fail:
#
#   BREAK=lookup     AZ_AVK_UPLOAD_ON_LOOKUP=1 -- restore the old unconditional
#                    upload. The static-wallpaper upload count must blow up.
#   BREAK=identity   AZ_AVK_CACHE_BY_IDENTITY=1 -- ignore content generations
#                    and cache on the buffer pointer. wlreuse must freeze on
#                    its first colour.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-shm-cache"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-shm-cache-test: needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "avk-shm-cache-test: needs python3-pillow"; exit 1; }

WLREUSE="$(dirname "$0")/wlreuse/wlreuse"
[ -x "$WLREUSE" ] || { echo "avk-shm-cache-test: wlreuse not built -- run: cd contrib/wlreuse && make" >&2; exit 1; }

SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-shmcache-$$"
HL_OUTDIR="$OUTDIR"
HL_WIDTH=1280 HL_HEIGHT=720
HL_ENV="ASTEROIDZ_RENDERER=avk"
[ "$BREAK" = lookup ] && HL_ENV="$HL_ENV AZ_AVK_UPLOAD_ON_LOOKUP=1"
[ "$BREAK" = identity ] && HL_ENV="$HL_ENV AZ_AVK_CACHE_BY_IDENTITY=1"
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

field() { # field JSON NAME
	python3 - "$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}
count_colour() { # count_colour PNG "#rrggbb" -> pixel count
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
want = tuple(int(sys.argv[2][i:i+2], 16) for i in (1, 3, 5))
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(n for n, c in im.getcolors(1 << 24) if c == want))
PY
}

hl_start "$SCENE_KDL"

# ── the static case ────────────────────────────────────────────────────────
# The harness wallpaper is a flat SHM layer surface drawn once and never again
# -- exactly the buffer that was costing 33 MB a frame on the live desktop.
#
# It needs a reason for frames to keep happening, though. A genuinely idle
# desktop now produces ZERO frames (that is M3.5D working), and zero uploads
# across zero frames proves nothing at all. A terminal's blinking cursor is
# enough: a few hundred pixels of real client change per blink, while the
# wallpaper underneath it never changes and is looked up on every single frame.
hl_spawn_kitty one >/dev/null; hl_wait_client_count 1 60
sleep 3
hl_dispatch reset_avk_stats
sleep 6
hl_get "get avk-stats" > "$OUTDIR/stats-idle.json"

echo "-- a static SHM surface is uploaded once, not once per frame --"
IDLE_FRAMES="$(field "$OUTDIR/stats-idle.json" frames)"
IDLE_UPLOADS="$(field "$OUTDIR/stats-idle.json" shm_full_uploads)"
IDLE_SKIPS="$(field "$OUTDIR/stats-idle.json" shm_upload_skips)"
IDLE_BYTES="$(field "$OUTDIR/stats-idle.json" shm_upload_bytes)"
PER_FRAME="$(python3 -c "print(int($IDLE_BYTES/$IDLE_FRAMES) if $IDLE_FRAMES else -1)" 2>/dev/null || echo -1)"
echo "  note: $IDLE_FRAMES frames, $IDLE_UPLOADS uploads, $IDLE_SKIPS skips, ${PER_FRAME} bytes/frame"

# The premise, in two parts, because zero uploads is the right answer to the
# wrong question if either fails: the compositor really was drawing, and it
# really was looking SHM buffers up and finding them already current.
hl_assert "the compositor kept drawing frames" \
	"$([ "${IDLE_FRAMES:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "and kept finding SHM buffers already on the GPU" \
	"$([ "${IDLE_SKIPS:-0}" -gt 0 ] && echo true || echo false)" "true"
# The assertion. The wallpaper alone is 1280x720x4 = 3.7 MB, so upload-on-
# lookup cannot possibly come in under a megabyte a frame. Anything near zero
# means the wallpaper is being reused rather than recopied.
hl_assert "and copied far less than a wallpaper per frame (${PER_FRAME} B)" \
	"$([ "${PER_FRAME:--1}" -ge 0 ] && [ "${PER_FRAME:-0}" -lt 1000000 ] \
		&& echo true || echo false)" "true"
# Lookups must vastly outnumber uploads: that ratio IS the fix.
hl_assert "with lookups far outnumbering uploads ($IDLE_SKIPS vs $IDLE_UPLOADS)" \
	"$([ "${IDLE_SKIPS:-0}" -gt $(( ${IDLE_UPLOADS:-0} * 2 )) ] \
		&& echo true || echo false)" "true"

# ── the same-buffer case ───────────────────────────────────────────────────
# wlreuse commits one wl_buffer over and over with different bytes. A cache
# keyed on the buffer pointer shows the first colour forever.
echo "-- a reused buffer's new contents still reach the screen --"
hl_dispatch reset_avk_stats
"$WLREUSE" --colour ff0000 --colour 0000ff --hold-ms 2500 --size 400x300 \
	> "$OUTDIR/wlreuse.log" 2>&1 &
WLREUSE_PID=$!
HL_SPAWNED_PIDS+=("$WLREUSE_PID")
hl_wait_client_count 2 60
sleep 1

# Counted, not sampled at a fixed point: wlreuse never resizes its buffer (that
# is the whole point of it), so the compositor places a 400x300 surface inside
# whatever tile it was given and a hardcoded centre coordinate lands on the
# wallpaper instead. The pixel COUNT does not care where it ended up.
hl_screenshot gen1
R1="$(count_colour "$OUTDIR/gen1.png" "#ff0000")"
B1="$(count_colour "$OUTDIR/gen1.png" "#0000ff")"
hl_get "get avk-stats" > "$OUTDIR/s1.json"
UP1="$(field "$OUTDIR/s1.json" shm_full_uploads)"

# Wait past the hold so the client has committed the SECOND colour into the
# SAME buffer.
sleep 3
hl_screenshot gen2
R2="$(count_colour "$OUTDIR/gen2.png" "#ff0000")"
B2="$(count_colour "$OUTDIR/gen2.png" "#0000ff")"
hl_get "get avk-stats" > "$OUTDIR/s2.json"
UP2="$(field "$OUTDIR/s2.json" shm_full_uploads)"

echo "  note: red/blue px was $R1/$B1, then $R2/$B2; uploads $UP1 -> $UP2"
kill "$WLREUSE_PID" 2>/dev/null

# The premise: the client really did get on screen with its first colour.
hl_assert "the reusing client's first generation is on screen" \
	"$([ "${R1:-0}" -gt 1000 ] && [ "${B1:-0}" -eq 0 ] && echo true || echo false)" "true"
# The assertion. Same wl_buffer, different bytes -- and the screen has to
# follow the bytes. An identity cache freezes on red and fails here.
hl_assert "and its second generation replaced it on screen" \
	"$([ "${B2:-0}" -gt 1000 ] && [ "${R2:-0}" -eq 0 ] && echo true || echo false)" "true"
hl_assert "which took at least one more upload" \
	"$([ "${UP2:-0}" -gt "${UP1:-0}" ] && echo true || echo false)" "true"

hl_stop

echo
echo "screenshots: $OUTDIR/gen1.png $OUTDIR/gen2.png"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
