#!/usr/bin/env bash
# avk-blur-walker-test.sh — do REAL wlr_scene blur nodes reach AVK, in the
# right place in the command stream?
#
# Everything up to M4F.2A.2 proved things about the RENDERER, driven by
# hand-built command lists. That leaves one gap the renderer tests cannot close:
# the compositor's own scene walker could be correct about every field and still
# emit a blur node at the wrong POSITION -- and a blur's position IS its
# semantics, because what it samples is the commands before it.
#
# So this drives a real compositor with the real producer
# (effects/shadow/blur-background), and asserts on the EMITTED COMMAND STREAM
# rather than on pixels. AVK_SCENE_DUMP logs one line per command with the index
# it landed at, and the index is exactly the k the renderer replays [0, k) for.
#
#   the wallpaper's texture command   BEFORE the blur   -- it is in the source
#   the blur command                  at some index k
#   the window's own texture command  AFTER the blur    -- it is NOT in the
#                                                          source
#
# That is A · BLUR · B and A · B · BLUR at once, in one stream, from the real
# walker: the same window is on both sides of a blur -- the one below it and the
# one above it -- and the two must land on opposite sides.
#
# THE DIRECT PATH is measured with the same fixture and blur turned off. A
# compositor that merely learned how to emit blur nodes must cost nothing when
# there are none.
#
# Break tests, which MUST fail:
#
#   BREAK=scene-after   AZ_BLUR_SCENE_AFTER=1 -- a blur's source becomes the
#                       WHOLE scene rather than the prefix behind it. It changes
#                       no INDEX at all: the blur command stays exactly where it
#                       is and only its source RANGE grows, so the falsifier is
#                       the range count, not the stream order.
#
#   BREAK=no-darken     AZ_BLUR_IGNORE_DARKEN=1 -- drops the clamp. Caught here
#                       as "the producer's darken never reached the renderer",
#                       which is the end-to-end claim; what the clamp does to
#                       pixels is test-avk-blur-material's question.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-blur-walker"
BREAK="${BREAK:-}"

# Shadows ON with a blurred backdrop: that is the producer that creates a
# wlr_scene_blur, sets sample_exclude, darken and edge_softness, and places the
# node below its own window. Nothing else in asteroidz makes one on a plain
# desktop.
#
# only-floating 0 IS LOAD-BEARING. Its default is 1 -- a tiled window shares
# every edge with a neighbour, so a shadow there would be drawn onto another
# window -- and with it on, a tiled window gets no shadow, hence no backdrop
# blur, hence no blur node for the walker to meet. The first version of this
# script measured 0 blur nodes for exactly that reason and read it as a broken
# walker. The premise assertions below exist because of it.
#
# TRANSLUCENT WINDOWS are load-bearing for the same reason. client_update_blur()
# refuses a blur node behind a FULLY OPAQUE surface -- it could never show its
# backdrop, and a CSD window whose buffer did not cover the node showed the
# refusal as a glowing blurred band. kitty is opaque, so without the opacity
# below the only blur node in the scene is the shadow's backdrop, and the
# window's OWN blur node -- the one that carries a clipped_region, and therefore
# the only thing that exercises the walker's node-local clip conversion -- never
# exists at all.
#
# (These are KDL, and KDL has no `#` comment. A comment inside the string is a
# parse error, the config is rejected, and the run comes back with no windows in
# it -- which was measured here, not guessed.)
BLUR_KDL="border_radius 12
effects {
	shadow {
		enable 1
		only-floating 0
		size 24
		blur 24
		blur-background 1
		blur-background-darken 1
		blur-background-strength 1.0
	}
	blur { enable 1; passes 3; radius 2 }
}
focused_opacity 0.9
unfocused_opacity 0.9"
NOBLUR_KDL="border_radius 12
effects {
	shadow { enable 1; only-floating 0; size 24; blur 24; blur-background 0 }
	blur { enable 0 }
}"

OUTDIR="${TMPDIR:-/tmp}/asteroidz-avk-blur-walker-$$"
HL_OUTDIR="$OUTDIR"
HL_WIDTH=1280 HL_HEIGHT=800
# ── AZ_BLUR_CACHE=0, BECAUSE EVERY CLAIM BELOW IS ABOUT THE LIVE CHAIN ─────
#
# What this fixture pins is the PREFIX ARCHITECTURE: that a blur's source is
# a replay of [0, k) and not the whole scene, that every recomputed node gets
# its own replay, and that the producer's darken reaches the renderer. M4I's
# monitor background cache bypasses all of it by design -- a served consumer
# runs no chain and replays no prefix -- so with the cache on these read 0 and
# the fixture failed while nothing was wrong.
#
# The cache is off rather than the assertions being broadened to
# "replays OR cache hits", and that is the whole point: a disjunction like that
# is satisfied by the cache even if prefix replay were completely broken, which
# is precisely how a break stops breaking. The claims stay strict; the cache's
# own correctness has three fixtures of its own (avk-blur-cache-test,
# -dirty, -multi).
HL_ENV="ASTEROIDZ_RENDERER=avk ASTEROIDZ_VK_DEBUG=1 AZ_BLUR_CACHE=0"
case "$BREAK" in
scene-after) HL_ENV="$HL_ENV AZ_BLUR_SCENE_AFTER=1" ;;
no-darken)   HL_ENV="$HL_ENV AZ_BLUR_IGNORE_DARKEN=1" ;;
esac
export HL_OUTDIR HL_WIDTH HL_HEIGHT HL_ENV

# hl_start WIPES the state directory, so the file at this path only ever holds
# the CURRENT instance's output. Every phase that ends copies it out; the
# validation count at the end sums the copies rather than reading a log three
# quarters of which has been deleted.
LOG="$OUTDIR/state/asteroidz/asteroidz.log"
keep_log() { cp -f "$LOG" "$OUTDIR/log-$1.txt" 2>/dev/null || true; }

field() { python3 - "$1" "$2" <<'PY'
import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], "x"))
except Exception:
    print("x")
PY
}

# ── 1. the producer runs, and the walker emits ─────────────────────────────

hl_start "$BLUR_KDL"
hl_reset_spawn_colors
hl_spawn_kitty one >/dev/null; hl_wait_client_count 1 60
# And a client that supplies a real ext-background-effect-v1 blur region -- TWO
# separated rectangles. It is the only thing in the suite that gives a blur node
# a clip_region at all: clipped_region_get_default() is an empty box, so an
# ordinary window's blur node carries no clip and a "the clip works" assertion
# made against it would be an assertion about nothing. avk.blur_nodes_clipped
# read 0 on the first fixture that claimed to test this.
hl_spawn_wlbgeffect bgeffect 300 >/dev/null; hl_wait_client_count 2 60
sleep 4

hl_dispatch reset_avk_stats
sleep 2
hl_get "get avk-stats" > "$OUTDIR/stats-blur.json"

SEEN="$(field "$OUTDIR/stats-blur.json" blur_nodes_seen)"
EMIT="$(field "$OUTDIR/stats-blur.json" blur_nodes_emitted)"
CULL="$(field "$OUTDIR/stats-blur.json" blur_nodes_culled)"
LIVE="$(field "$OUTDIR/stats-blur.json" blur_nodes_forced_live)"
echo "  note: blur nodes seen=$SEEN emitted=$EMIT culled=$CULL forced_live=$LIVE"

# THE PREMISE. Every assertion after this one is vacuous if the producer never
# made a blur node -- and "0 emitted" is also what a completely broken walker
# reports, so the two must be told apart before anything else is claimed.
hl_assert "the walk MET real blur nodes (premise)" \
	"$([ "${SEEN:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "and emitted AVK commands for them ($EMIT)" \
	"$([ "${EMIT:-0}" -gt 0 ] && echo true || echo false)" "true"

REPLAYS="$(field "$OUTDIR/stats-blur.json" blur_prefix_replays)"
PXLS="$(field "$OUTDIR/stats-blur.json" blur_prefix_pixels)"
CMDS="$(field "$OUTDIR/stats-blur.json" blur_prefix_commands)"
echo "  note: $REPLAYS prefix replays, $CMDS commands replayed, $PXLS pixels"
hl_assert "each emitted blur ran a prefix capture ($REPLAYS replays)" \
	"$([ "${REPLAYS:-0}" -gt 0 ] && echo true || echo false)" "true"

WAITS="$(field "$OUTDIR/stats-blur.json" cpu_sync_waits)"
hl_assert "and no CPU wait was introduced ($WAITS)" "${WAITS:-x}" "0"

# ── 1b. the REAL producer's material, end to end ───────────────────────────
#
# test-avk-blur-material proves what the renderer does with darken and
# edge_softness given commands that carry them. This is the other half: that the
# actual shadow-backdrop producer sets them, the walker snapshots them, and the
# renderer receives them -- a chain of three that a renderer test cannot see and
# in which any link can be missing while the picture still looks plausible.
#
# Counted AT THE DRAW and at the chain, so these say what the material DID, not
# what the scene asked for.
DARKEN="$(field "$OUTDIR/stats-blur.json" blur_darken_chains)"
SOFT="$(field "$OUTDIR/stats-blur.json" blur_soft_edge_draws)"
DRAWS="$(field "$OUTDIR/stats-blur.json" blur_draws)"
echo "  note: $DRAWS blur composites, $SOFT of them soft-edged, $DARKEN darken chains"
hl_assert "the producer's darken reached the renderer ($DARKEN chains)" \
	"$([ "${DARKEN:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "and its edge_softness took the soft-edge pipeline ($SOFT draws)" \
	"$([ "${SOFT:-0}" -gt 0 ] && echo true || echo false)" "true"
# Two producers, two kinds of blur node: the shadow's backdrop (soft-edged, with
# darken and sample_exclude) and the window's own (hard-edged, carrying a
# clipped_region). The second is the only one that exercises the walker's
# node-local clip conversion at all, so its presence is a premise and not a
# nicety -- and it exists only because the fixture makes windows translucent.
hl_assert "both producers are present: some composites are NOT soft-edged" \
	"$([ "${DRAWS:-0}" -gt "${SOFT:-0}" ] && echo true || echo false)" "true"
# And the walker's node-local clip conversion actually RAN. Without this, "the
# clip works" would be a claim about nodes that carry no clip.
CLIPPED="$(field "$OUTDIR/stats-blur.json" blur_nodes_clipped)"
echo "  note: $CLIPPED of $EMIT emitted nodes carried a clip region"
hl_assert "the walker converted a node-local clip ($CLIPPED nodes)" \
	"$([ "${CLIPPED:-0}" -gt 0 ] && echo true || echo false)" "true"

# ── 2. WHERE the blur landed in the stream ─────────────────────────────────
#
# ARMED, NOT SCHEDULED. AVK_SCENE_DUMP names a frame NUMBER, and a test cannot
# know which frame its window will be on: too low and the dump fires before the
# client exists, too high and an idle compositor never gets there. Both produce
# an empty dump, which reads exactly like "the walker emitted no blur" -- a
# completely different diagnosis. Both were measured here before `dump_scene`
# existed. So the dump is asked for once the scene is ready, and lands on the
# next frame.

DUMP="$OUTDIR/cmddump.txt"
hl_dispatch dump_scene 0.3
hl_dispatch "moveresize,140 120 520 400" 0.8
grep -a 'cmd\[' "$LOG" > "$DUMP" 2>/dev/null || true
NCMD="$(wc -l < "$DUMP")"
echo "  note: $NCMD command lines dumped"
sed -n '1,40p' "$DUMP" | sed 's/^/    /'

hl_assert "the dump produced a command stream (premise)" \
	"$([ "${NCMD:-0}" -gt 3 ] && echo true || echo false)" "true"

# Indices, from the one dumped frame. The first BLUR, the first TEXTURE (the
# wallpaper, drawn under everything), and the LAST TEXTURE (the window itself,
# drawn over its own backdrop blur).
IDX="$(python3 - "$DUMP" <<'PY'
import re, sys
rows = []
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"cmd\[(\d+)\] (\w+)", line)
    if m:
        rows.append((int(m.group(1)), m.group(2)))
# One frame only: the dump fires once, but the log may hold a partial second
# copy if the run raced. Take the longest ascending run starting at 0.
frame, best = [], []
for idx, kind in rows:
    if idx == 0:
        if len(frame) > len(best):
            best = frame
        frame = []
    frame.append((idx, kind))
if len(frame) > len(best):
    best = frame
blur = [i for i, k in best if k == "BLUR"]
tex  = [i for i, k in best if k == "TEXTURE"]
print(blur[0] if blur else -1,
      tex[0] if tex else -1,
      tex[-1] if tex else -1,
      len(best))
PY
)"
set -- $IDX
BLUR_AT=$1 FIRST_TEX=$2 LAST_TEX=$3 NTOTAL=$4
echo "  note: $NTOTAL commands; first BLUR at $BLUR_AT, first TEXTURE at $FIRST_TEX, last TEXTURE at $LAST_TEX"

hl_assert "a BLUR command is in the stream" \
	"$([ "${BLUR_AT:--1}" -ge 0 ] && echo true || echo false)" "true"
# A · BLUR: the wallpaper is emitted before the blur, so it IS in the source.
hl_assert "the wallpaper (first texture, $FIRST_TEX) comes BEFORE the blur ($BLUR_AT)" \
	"$([ "${FIRST_TEX:-99}" -lt "${BLUR_AT:-0}" ] && echo true || echo false)" "true"
# BLUR · B: the window is emitted after its own backdrop blur, so it is NOT.
hl_assert "the window (last texture, $LAST_TEX) comes AFTER the blur ($BLUR_AT)" \
	"$([ "${LAST_TEX:--1}" -gt "${BLUR_AT:-99}" ] && echo true || echo false)" "true"

# THE SHAPE THE CLIP ARRIVES IN, from a real client region.
#
# BLUR LINES ONLY. Measured over the whole dump this read 4 and passed -- from a
# SHADOW's window-shaped hole, which is the standard two-rectangle cross and has
# nothing whatever to do with a blur's clip. Right answer, wrong command.
#
# AND IT IS 2, END TO END. wlbgeffect supplies two separated rectangles with a
# 24 px gap. This assertion read 1 until M4F.2B.0: client_update_blur() took
# pixman_region32_extents() of the client's region and handed the bounding box to
# wlr_scene_blur_set_clipped_region(), while the two OTHER producers of the same
# protocol data -- layer_update_blur and popup_update_blur -- had always passed
# the region itself to wlr_scene_blur_set_region(). A toplevel was the only
# surface kind whose shape was discarded. Now all three agree, so the gap in a
# client's region is a gap in the command's clip.
#
# 2, not ">= 1". A bounding box is one rectangle and would satisfy ">= 1"
# forever, which is exactly how the collapse survived the previous milestone.
MAXRECTS="$(grep -a 'BLUR' "$DUMP" | grep -o 'clip=[0-9]*rects' | grep -o '[0-9]*' | sort -n | tail -1)"
echo "  note: the most rectangles a BLUR command's clip arrived in: ${MAXRECTS:-0}"
hl_assert "a client's two-rectangle blur region reaches the command with its gap" \
	"${MAXRECTS:-0}" "2"

# ── 2b. and the RANGE it replays is [0, k), not the whole scene ────────────
#
# The index checks above cannot see the one thing BREAK=scene-after changes: the
# blur command stays exactly where it is and only its SOURCE RANGE grows. That
# range is counted, so it is checked directly rather than inferred from a
# picture.
#
# One blur node at index k replays exactly k commands. The break replays the
# whole stream instead, which for this fixture is NTOTAL.

hl_dispatch reset_avk_stats
hl_dispatch "moveresize,120 120 520 400" 0.4
sleep 2
hl_get "get avk-stats" > "$OUTDIR/stats-range.json"
R_REP="$(field "$OUTDIR/stats-range.json" blur_prefix_replays)"
R_CMD="$(field "$OUTDIR/stats-range.json" blur_prefix_commands)"
AVG=$(( R_REP > 0 ? R_CMD / R_REP : -1 ))
echo "  note: $R_CMD commands over $R_REP replays -- $AVG per blur, of $NTOTAL in the frame"
hl_assert "a blur replayed something (premise, $R_REP replays)" \
	"$([ "${R_REP:-0}" -gt 0 ] && echo true || echo false)" "true"
hl_assert "and its source range is a PREFIX ($AVG < $NTOTAL commands)" \
	"$([ "${AVG:--1}" -ge 0 ] && [ "${AVG:--1}" -lt "${NTOTAL:-0}" ] && echo true || echo false)" "true"

# ── 3. two windows: two blur nodes, and the later one sees the earlier ─────

hl_spawn_kitty two >/dev/null; hl_wait_client_count 3 60
sleep 4
hl_dispatch reset_avk_stats
sleep 2
hl_get "get avk-stats" > "$OUTDIR/stats-two.json"
SEEN2="$(field "$OUTDIR/stats-two.json" blur_nodes_seen)"
EMIT2="$(field "$OUTDIR/stats-two.json" blur_nodes_emitted)"
FR2="$(field "$OUTDIR/stats-two.json" frames)"
REP2="$(field "$OUTDIR/stats-two.json" blur_prefix_replays)"
echo "  note: two windows -- seen=$SEEN2 emitted=$EMIT2 over $FR2 frames, $REP2 replays"
# Per frame rather than in total, because the counters accumulate.
PERFRAME=$(( FR2 > 0 ? EMIT2 / FR2 : 0 ))
hl_assert "three windows emit several blur nodes per frame ($PERFRAME)" \
	"$([ "${PERFRAME:-0}" -ge 4 ] && echo true || echo false)" "true"
# EVERY BLUR THAT HAD TO BE RECOMPUTED GOT ITS OWN REPLAY -- which is not the
# same claim as "every blur node got one", and stopped being the same claim in
# M4F.2B. A blur whose source did not change and whose result nobody needs is
# SKIPPED: no capture, no chain, no composite. This assertion read
# "replays >= emitted" and failed at 9 >= 18 the moment damage propagation
# landed, correctly reporting that half the blur nodes in an idle three-window
# desktop now do no work at all.
#
# touched + skipped == emitted is the identity that says none of them was
# simply forgotten.
TOUCH2="$(field "$OUTDIR/stats-two.json" blur_damage_nodes_touched)"
SKIP2="$(field "$OUTDIR/stats-two.json" blur_damage_nodes_skipped)"
echo "  note: $TOUCH2 recomputed, $SKIP2 skipped, of $EMIT2 emitted"
hl_assert "every blur is either recomputed or deliberately skipped" \
	"$(( TOUCH2 + SKIP2 ))" "$EMIT2"
hl_assert "and each recomputed one replays its own prefix ($REP2 >= $TOUCH2)" \
	"$([ "${REP2:-0}" -ge "${TOUCH2:-0}" ] && echo true || echo false)" "true"
hl_assert "an idle desktop skips some blur work entirely ($SKIP2)" \
	"$([ "${SKIP2:-0}" -gt 0 ] && echo true || echo false)" "true"

# ── 4. static idle ─────────────────────────────────────────────────────────
#
# The blur now reaches the real compositor path, so the question "does it make
# its own work" has to be asked of the whole system rather than of the renderer.
# A blur that fed anything back into damage would keep the compositor rendering
# forever on a desktop where nothing moves.

sleep 3
hl_dispatch reset_avk_stats
# BEFORE and AFTER, because the transient pool's counters are LIFETIME totals
# and reset_avk_stats does not clear them -- deliberately, since a pool entry's
# reuse history spans the reset. Reading the total after an idle window and
# calling it "creates while idle" reported 55 here and meant nothing.
hl_get "get avk-stats" > "$OUTDIR/stats-idle-before.json"
BEFORE_CREATES="$(field "$OUTDIR/stats-idle-before.json" transient_creates)"
sleep 6
hl_get "get avk-stats" > "$OUTDIR/stats-idle.json"
IDLE_FRAMES="$(field "$OUTDIR/stats-idle.json" frames)"
AFTER_CREATES="$(field "$OUTDIR/stats-idle.json" transient_creates)"
IDLE_CREATES=$(( AFTER_CREATES - BEFORE_CREATES ))
echo "  note: 6 idle seconds -> $IDLE_FRAMES frames, $IDLE_CREATES new transients (${BEFORE_CREATES} -> ${AFTER_CREATES})"
# A terminal has a blinking cursor, so this is not zero -- but it must be a
# cursor's worth of frames and not a free-running loop at the refresh rate.
hl_assert "an idle desktop does not free-run ($IDLE_FRAMES frames in 6s)" \
	"$([ "${IDLE_FRAMES:-9999}" -lt 60 ] && echo true || echo false)" "true"
hl_assert "and the transient pool creates NOTHING new after warmup ($IDLE_CREATES)" \
	"$([ "${IDLE_CREATES:-9999}" -eq 0 ] && echo true || echo false)" "true"

# ── 5. a frame, kept for eyes ──────────────────────────────────────────────
#
# Not asserted on: this script's claims are about the command stream and the
# counters, which say things a screenshot cannot. It is captured because a
# failing run is much easier to diagnose with the picture beside the numbers.

hl_screenshot blur-on >/dev/null 2>&1 || true
keep_log blur
hl_stop

# ── 6. the direct path ─────────────────────────────────────────────────────

HL_ENV="ASTEROIDZ_RENDERER=avk ASTEROIDZ_VK_DEBUG=1 AZ_BLUR_CACHE=0"
export HL_ENV
hl_start "$NOBLUR_KDL"
hl_reset_spawn_colors
hl_spawn_kitty one >/dev/null; hl_wait_client_count 1 60
sleep 4
hl_dispatch reset_avk_stats
sleep 3
hl_get "get avk-stats" > "$OUTDIR/stats-direct.json"

D_SEEN="$(field "$OUTDIR/stats-direct.json" blur_nodes_seen)"
D_EMIT="$(field "$OUTDIR/stats-direct.json" blur_nodes_emitted)"
D_REPLAY="$(field "$OUTDIR/stats-direct.json" blur_prefix_replays)"
D_ACQ="$(field "$OUTDIR/stats-direct.json" transient_acquires)"
D_WAIT="$(field "$OUTDIR/stats-direct.json" cpu_sync_waits)"
echo "  note: direct path -- seen=$D_SEEN emitted=$D_EMIT replays=$D_REPLAY acquires=$D_ACQ"

hl_assert "with blur off the walker emits no blur commands" "${D_EMIT:-x}" "0"
hl_assert "no prefix capture runs" "${D_REPLAY:-x}" "0"
hl_assert "no transient is acquired" "${D_ACQ:-x}" "0"
hl_assert "and still no CPU wait" "${D_WAIT:-x}" "0"

keep_log direct
hl_stop

# ── 7. validation ──────────────────────────────────────────────────────────

# Across EVERY phase's log. Reading only the file at $LOG would report on the
# last instance alone, because hl_start wiped the two before it -- a validation
# claim covering a quarter of the run and saying so nowhere.
#
# `grep -hc` prints 0 and exits 1 when nothing matches, so `|| true` rather than
# `|| echo 0`: the latter appended a SECOND zero and the comparison saw "0\n0".
VUID="$(cat "$OUTDIR"/log-*.txt 2>/dev/null | grep -ac 'VUID-' || true)"
HAZ="$(cat "$OUTDIR"/log-*.txt 2>/dev/null | grep -ac 'SYNC-HAZARD' || true)"
NLOGS="$(ls "$OUTDIR"/log-*.txt 2>/dev/null | wc -l)"
echo "  note: $VUID VUID lines, $HAZ SYNC-HAZARD lines across $NLOGS phase logs"
hl_assert "every phase's log was kept (premise)" \
	"$([ "${NLOGS:-0}" -ge 2 ] && echo true || echo false)" "true"
hl_assert "no validation errors across every phase" "${VUID:-x}" "0"
hl_assert "no synchronisation hazards" "${HAZ:-x}" "0"

# ── THE STATUS HAS TO SURVIVE WHAT COMES AFTER IT ─────────────────────────
#
# hl_summary returns 1 when an assertion failed, and this was followed by three
# more commands -- so the script's exit status was the trailing `fi`, which is
# always 0. The fixture printed its own failures and reported SUCCESS to
# anything reading its exit code, which is how avk-suite.sh ran it, saw 0, and
# left it out of the FAILED list while five of its assertions were failing.
#
# Worse than the missing executable bit the register was built for: that one at
# least did nothing visible. This one does the work, prints the failure, and
# then says it passed.
hl_summary
STATUS=$?

echo
echo "logs: $OUTDIR"
if [ -n "$BREAK" ]; then
	echo
	echo "BREAK=$BREAK was set: this run is EXPECTED TO FAIL."
fi
exit "$STATUS"
