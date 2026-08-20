#!/usr/bin/env bash
# avk-shm-partial-test.sh — when a client changes a little, does AVK copy a
# little?
#
# M3.5D.1 Phase 2's acceptance test. Phase 1 stopped re-uploading a CPU buffer
# on every frame; this asserts that a changed generation copies only the
# rectangles the client actually reported, and that everything outside them
# survives untouched.
#
# Every scenario drives contrib/wlreuse, which reuses ONE wl_buffer for its
# whole life. That matters twice over: it is the only client in the tree whose
# buffer identity is a lie, and it is the only one that can change a small
# rectangle of an EXISTING generation, which is exactly the case a partial
# upload has to get right.
#
# Scenarios:
#
#   small     one 64x64 rectangle changes in a 512x512 buffer. Uploaded bytes
#             must be a small fraction of the buffer, and the other 99% of the
#             pixels must still hold the previous colour.
#   two       two widely separated rectangles. A bounding-box implementation
#             passes the "both rectangles changed" check and fails the "the gap
#             between them did not" check, which is why the gap is sampled.
#   stride    the same, on a buffer whose rows are padded well beyond
#             width * 4. A copy computing the source row as y * width * bpp,
#             or treating a rectangle as one contiguous block, shears here and
#             nowhere else.
#   many      more damage rectangles than the packed-copy path takes. Damage
#             it cannot represent must become ONE full upload for that
#             generation -- correct, and still nothing like one per frame.
#   nodamage  a new generation with no damage reported at all. Per the protocol
#             that means nothing changed, so the correct answer is to upload
#             nothing -- which is also what wlroots' own texture path does. It
#             is here because "we honour what the client said" and "we re-upload
#             whenever we are unsure" are different behaviours and only one of
#             them is right.
#
# Break tests, each of which MUST fail:
#
#   BREAK=source-full   AZ_AVK_SOURCE_FULL=1 -- ignore source damage and upload
#                       the whole buffer for every generation. The byte-count
#                       assertions fail; the pixels stay correct, which is the
#                       point: this is a cost regression, not a visual one.
#   BREAK=omit-region   AZ_AVK_OMIT_REGION=1 -- drop one damage rectangle. The
#                       pixel assertions fail and the byte counts look better
#                       than ever, which is why bytes alone are not enough.
#   BREAK=unsafe-reuse  AZ_AVK_UNSAFE_REUSE=1 AVK_UNSAFE_REUSE=1 -- overwrite an
#                       image with no GPU ordering against the frames still
#                       sampling it: no timeline wait AND no read dependency in
#                       the barrier.
#
#                       THIS BREAK DOES NOT FAIL, and that is reported rather
#                       than hidden. With both protections removed, and with
#                       the validation layer demonstrably loaded and running,
#                       synchronisation validation reports nothing -- including
#                       under the rapid case below, where 221 generations and
#                       82 partial uploads genuinely overlap frames. The likely
#                       reason is that uploads and frames are submitted to the
#                       same VkQueue, each upload ordered after the frame that
#                       read the image, so no concurrency exists for validation
#                       to object to.
#
#                       The protections are kept: they cost nothing, they are
#                       correct, and they are what will matter the moment
#                       uploads move to the dedicated transfer queue the
#                       capability table already records. But this suite cannot
#                       currently prove they are load-bearing, and saying so is
#                       better than a break switch that passes and looks like
#                       coverage.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-shm-partial"
BREAK="${BREAK:-}"

command -v python3 >/dev/null || { echo "avk-shm-partial-test: needs python3"; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "avk-shm-partial-test: needs python3-pillow"; exit 1; }
WLREUSE="$(dirname "$0")/wlreuse/wlreuse"
[ -x "$WLREUSE" ] || { echo "avk-shm-partial-test: wlreuse not built -- run: cd contrib/wlreuse && make" >&2; exit 1; }

SCENE_KDL="shadows 0
layer_shadows 0
border_radius 0
effects { blur { enable 0 } }"

BASE="${TMPDIR:-/tmp}/asteroidz-avk-partial-$$"

field() { python3 - "$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}
count_colour() { python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
want = tuple(int(sys.argv[2][i:i+2], 16) for i in (1, 3, 5))
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(n for n, c in im.getcolors(1 << 24) if c == want))
PY
}

# ── one scenario ───────────────────────────────────────────────────────────
# Boots a compositor, runs wlreuse with the given arguments, captures the
# first generation and then a later one, and leaves the stats beside them.
run_case() { # run_case NAME <wlreuse args...>
	local name="$1"; shift
	HL_OUTDIR="$BASE/$name"
	HL_WIDTH=1280 HL_HEIGHT=720
	# Validation on for every run, not only the break run. A partial upload
	# writes an image that frames in flight are still sampling, and that
	# hazard is invisible to a screenshot -- synchronisation validation is the
	# only thing in the suite that can see it at all.
	HL_ENV="ASTEROIDZ_VK_DEBUG=1"
	[ "$BREAK" = source-full ] && HL_ENV="$HL_ENV AZ_AVK_SOURCE_FULL=1"
	[ "$BREAK" = omit-region ] && HL_ENV="$HL_ENV AZ_AVK_OMIT_REGION=1"
	[ "$BREAK" = unsafe-reuse ] && HL_ENV="$HL_ENV AZ_AVK_UNSAFE_REUSE=1 AVK_UNSAFE_REUSE=1"
	export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV
	HL_SPAWN_COLOR_IDX=0

	hl_start "$SCENE_KDL"
	"$WLREUSE" "$@" > "$HL_OUTDIR/wlreuse.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	hl_wait_client_count 1 60
	sleep 1
	hl_screenshot gen1
	hl_dispatch reset_avk_stats
	# Past the hold, so the client has committed its second generation into
	# the same buffer with only the damaged rectangles changed.
	sleep 3
	hl_screenshot gen2
	hl_get "get avk-stats" > "$HL_OUTDIR/stats.json"
	kill "$pid" 2>/dev/null
	hl_stop
}

# ── small rectangle ────────────────────────────────────────────────────────
# 512x512 = 262144 px. One 64x64 = 4096 px changes, 1.6% of the buffer.
echo "== small rectangle =="
run_case small --size 512x512 --colour 000000 --colour ffffff \
	--damage 64x64+100+100 --hold-ms 2200

S="$BASE/small/stats.json"
BYTES="$(field "$S" shm_upload_bytes)"
PARTIAL="$(field "$S" shm_partial_uploads)"
FULLUP="$(field "$S" shm_full_uploads)"
BLACK1="$(count_colour "$BASE/small/gen1.png" "#000000")"
BLACK2="$(count_colour "$BASE/small/gen2.png" "#000000")"
WHITE2="$(count_colour "$BASE/small/gen2.png" "#ffffff")"
echo "  note: $PARTIAL partial / $FULLUP full uploads, $BYTES bytes; black $BLACK1 -> $BLACK2, white $WHITE2"

# Premise: the first generation really did paint the whole buffer black.
hl_assert "the first generation filled the buffer" \
	"$([ "${BLACK1:-0}" -gt 200000 ] && echo true || echo false)" "true"
# The changed rectangle arrived.
hl_assert "the damaged rectangle changed colour" \
	"$([ "${WHITE2:-0}" -ge 4000 ] && [ "${WHITE2:-0}" -le 4300 ] \
		&& echo true || echo false)" "true"
# And nothing else did. This is what a bounding box or a full-buffer clear
# would break.
hl_assert "and everything outside it kept the old colour" \
	"$([ "${BLACK2:-0}" -ge $(( BLACK1 - 4300 )) ] && echo true || echo false)" "true"
# The cost assertion: a 64x64 ARGB rectangle is 16 KB. Anything approaching the
# 1 MB buffer means source damage is being ignored.
hl_assert "and the copy was a small fraction of the buffer ($BYTES B)" \
	"$([ "${BYTES:-999999999}" -lt 200000 ] && echo true || echo false)" "true"
hl_assert "carried by a partial upload rather than a full one" \
	"$([ "${PARTIAL:-0}" -gt 0 ] && echo true || echo false)" "true"

# ── two separated regions ──────────────────────────────────────────────────
echo "== two separated regions =="
run_case two --size 512x512 --colour 000000 --colour ffffff \
	--damage 64x64+16+16 --two-regions --hold-ms 2200

S="$BASE/two/stats.json"
BYTES="$(field "$S" shm_upload_bytes)"
WHITE2="$(count_colour "$BASE/two/gen2.png" "#ffffff")"
echo "  note: $BYTES bytes, white $WHITE2"
# Two 64x64 rectangles = 8192 px. A bounding box spanning them would be
# 432x432 = 186624 px, which is unmistakably different.
hl_assert "both rectangles changed and nothing between them did" \
	"$([ "${WHITE2:-0}" -ge 8000 ] && [ "${WHITE2:-0}" -le 8600 ] \
		&& echo true || echo false)" "true"
hl_assert "and the copy stayed small ($BYTES B)" \
	"$([ "${BYTES:-999999999}" -lt 200000 ] && echo true || echo false)" "true"

# ── padded stride ──────────────────────────────────────────────────────────
echo "== padded stride =="
run_case stride --size 512x512 --stride-pad 512 --colour 000000 --colour ffffff \
	--damage 64x64+100+100 --hold-ms 2200

S="$BASE/stride/stats.json"
BYTES="$(field "$S" shm_upload_bytes)"
WHITE2="$(count_colour "$BASE/stride/gen2.png" "#ffffff")"
BLACK2="$(count_colour "$BASE/stride/gen2.png" "#000000")"
echo "  note: $BYTES bytes, white $WHITE2, black $BLACK2"
# The exact same numbers as the unpadded case. A source row computed as
# y * width * bpp instead of y * stride walks diagonally through the buffer and
# produces a sheared smear, which changes both counts.
hl_assert "a padded buffer damages exactly the same area" \
	"$([ "${WHITE2:-0}" -ge 4000 ] && [ "${WHITE2:-0}" -le 4300 ] \
		&& echo true || echo false)" "true"
hl_assert "with the rest of it undisturbed" \
	"$([ "${BLACK2:-0}" -gt 200000 ] && echo true || echo false)" "true"

# ── damage that cannot be represented as regions ───────────────────────────
# 40 rectangles, past the 32 the packed-copy path takes. The fallback is a full
# upload, which is correct; doing it once is the part that matters.
echo "== more regions than the copy path packs =="
run_case many --size 512x512 --colour 000000 --colour ffffff \
	--many-regions 40 --hold-ms 2200

S="$BASE/many/stats.json"
FULLUP="$(field "$S" shm_full_uploads)"
PARTIAL="$(field "$S" shm_partial_uploads)"
SKIPS="$(field "$S" shm_upload_skips)"
WHITE2="$(count_colour "$BASE/many/gen2.png" "#ffffff")"
echo "  note: $FULLUP full / $PARTIAL partial uploads, $SKIPS skips, white $WHITE2"
# ONE COPY FOR THE GENERATION, whichever shape it takes.
#
# This used to assert specifically that the fallback was a FULL upload, which
# was the only correct answer while a plan could not be narrowed. It can be now:
# with a visibility hint, damage the packer cannot represent collapses to the
# BOUNDING BOX of the part that is visible, which is a superset of what is owed
# and no larger than the buffer. That is a correct partial, so the assertion is
# on the thing that was actually being protected -- one copy for the generation
# rather than one per frame -- and the pixel check below is what would catch a
# copy that dropped a rectangle.
UPLOADS=$(( ${FULLUP:-0} + ${PARTIAL:-0} ))
hl_assert "unrepresentable damage still produced a copy" \
	"$([ "$UPLOADS" -ge 1 ] && echo true || echo false)" "true"
hl_assert "but only once for that generation, not once per frame" \
	"$([ "$UPLOADS" -le 2 ] && echo true || echo false)" "true"
hl_assert "and later frames reused the image" \
	"$([ "${SKIPS:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "with the pixels correct either way" \
	"$([ "${WHITE2:-0}" -gt 1500 ] && echo true || echo false)" "true"

# ── a generation with no damage reported ───────────────────────────────────
# Not the same thing as "damage unknown". A client that attaches and commits
# without a damage request has SAID that nothing changed, and the correct
# response is to believe it -- which is also what wlroots' own texture path
# does. The client here scribbles anyway; the screen must not follow.
echo "== no damage reported =="
run_case nodamage --size 512x512 --colour 000000 --colour ffffff \
	--no-damage --hold-ms 2200

S="$BASE/nodamage/stats.json"
FULLUP="$(field "$S" shm_full_uploads)"
PARTIAL="$(field "$S" shm_partial_uploads)"
BLACK2="$(count_colour "$BASE/nodamage/gen2.png" "#000000")"
echo "  note: $FULLUP full / $PARTIAL partial uploads, black $BLACK2"
hl_assert "a client reporting no damage triggers no upload" \
	"$(( ${FULLUP:-9} + ${PARTIAL:-9} ))" "0"
hl_assert "and the screen keeps what the client last reported" \
	"$([ "${BLACK2:-0}" -gt 200000 ] && echo true || echo false)" "true"

# ── a client fast enough to race the compositor ────────────────────────────
# The lifetime hazard this guards against is an upload overwriting an image
# while a frame that samples it is still in flight. At a two-second hold there
# is no such frame -- the GPU has been idle for an age -- so none of the cases
# above can produce the race at all, however broken the ordering is. Measured:
# with both ordering protections removed, all five stayed clean.
#
# This one commits every few milliseconds, so uploads and frames genuinely
# overlap.
echo "== rapid updates, overlapping frames =="
run_case rapid --size 512x512 --colour 000000 --colour ffffff \
	--damage 128x128+64+64 --hold-ms 6

S="$BASE/rapid/stats.json"
RGEN="$(field "$S" shm_commits)"
RUP="$(field "$S" shm_partial_uploads)"
echo "  note: $RGEN generations, $RUP partial uploads"
hl_assert "the rapid client really did commit many generations" \
	"$([ "${RGEN:-0}" -gt 50 ] && echo true || echo false)" "true"
hl_assert "and they were uploaded" \
	"$([ "${RUP:-0}" -gt 0 ] && echo true || echo false)" "true"

# ── validation, which is the only thing that sees a lifetime race ──────────
# Updating an image while a frame in flight is still sampling it produces no
# visible artefact most of the time, and an occasional torn surface that looks
# like a client bug the rest of the time. The GPU-side ordering that prevents
# it -- a timeline wait on the image's last use -- cannot be checked by looking
# at pixels, so this is checked by asking the validation layer.
echo "== synchronisation validation =="
for case in small two stride many nodamage rapid; do
	VERR="$(field "$BASE/$case/stats.json" validation_errors)"
	hl_assert "no validation errors during the $case case" "${VERR:-x}" "0"
done

echo
echo "captures under: $BASE"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
	echo "A pass here means the assertions are not measuring what they claim."
fi
hl_summary
