#!/usr/bin/env bash
# cm-two-writer-test.sh -- ONE policy may author the preferred description.
#
# ── THE DEFECT ───────────────────────────────────────────────────────────
#
# TWO writers were sending wp-color-management preferred descriptions for the
# same surface, with DIFFERENT policies:
#
#   asteroidz   src/render/az_preferred.h -- the surface's own output (c->mon),
#               the single policy M6B built so frog and wp-cm could not drift
#               apart. Sends WLR_COLOR_TRANSFER_FUNCTION_SRGB, which wlroots
#               maps to the protocol's COMPOUND_POWER_2_4 = 14.
#
#   scenefx     types/scene/surface.c:164 -- "max preference across every
#               output this surface touches", fired from surface_reconfigure on
#               scene-graph output enter/leave and scale changes. Defaults to
#               WLR_COLOR_TRANSFER_FUNCTION_GAMMA22 = protocol 2.
#
# Both called wlr_color_manager_v1_set_surface_preferred_image_description on
# the same surface. Whichever fired last won.
#
# ── WHY IT IS NOT A MULTI-MONITOR EDGE CASE ──────────────────────────────
#
# The first reading of this defect -- mine and the audit's -- was that the two
# policies "only disagree for a surface straddling outputs in different colour
# states", making it rare and live-only. THAT WAS WRONG, and the numbers above
# are why: 2 and 14 are different on an ORDINARY SDR SURFACE ON ONE OUTPUT.
# Every SDR window was subject to it, and which answer the client got depended
# on event ordering.
#
# It is also why contrib/m6b-preferred-desc-test.sh passed throughout: it
# asserts tf=14 and reads once, at a moment when asteroidz's writer had fired
# last. A fixture that reads once cannot see a race; this one reads again after
# deliberately provoking the other writer.
#
# ── THE PROVOCATION ──────────────────────────────────────────────────────
#
# scenefx's writer fires from surface_reconfigure, so the fixture moves the
# window between outputs -- a scene-graph output leave and enter -- and reads
# the description again. Without the move this fixture would assert only the
# initial ordering, which is the thing that already looked fine.
# ── WHAT THIS FIXTURE DOES NOT COVER ─────────────────────────────────────
#
# It settles WHICH WRITER authored the description, not WHICH OUTPUT was
# chosen. Verified: with AZ_BREAK_FROG_FIRST_HDR_OUTPUT=1 -- the break that
# makes az_preferred resolve the wrong monitor -- this fixture stays GREEN,
# because two headless SDR outputs describe identical colour and a wrong-output
# answer is byte-identical to a right one. A headless output cannot enter HDR,
# so nothing here can make them differ.
#
# The which-output claim belongs to contrib/m6b-frog-metadata-test.sh, which
# reads the compositor's own resolved `preferred_output` per client and does
# not depend on the two outputs describing different colour. Two fixtures, two
# claims; neither is a substitute for the other.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="cm-two-writer"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-cm2w-$$"
mkdir -p "$OUTDIR"

BGE="$(cd "$(dirname "$0")" && pwd)/wlbgeffect/wlbgeffect"
[ -x "$BGE" ] || { echo "SKIP: wlbgeffect not built -- run: cd contrib/wlbgeffect && make"; exit 77; }

# The protocol's own numbers, named once so the assertions read as claims about
# policy rather than about magic integers.
TF_COMPOUND_POWER_2_4=14   # what wlroots maps its own SRGB to -- az_preferred
TF_GAMMA22=2               # scenefx's default -- the other writer

export WLBGEFFECT_SSD=1
HL_OUTPUTS=2
HL_OUTDIR="$OUTDIR"; HL_WIDTH=1920; HL_HEIGHT=1080
HL_ENV="${AZ_CM_BREAK:-}"
export HL_OUTPUTS HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT
hl_start "layout { titlebar { enable 0 } }" >"$OUTDIR/hl_start.log" 2>&1
sleep 2

echo "══ one policy authors the preferred description ══"
[ -n "${AZ_CM_BREAK:-}" ] && echo "   BREAK: $AZ_CM_BREAK"
echo

wp_events() { grep -c "^wpcm\[" "$OUTDIR/cm2w.log" 2>/dev/null || echo 0; }
# ONLY the wpcm lines. The first version grepped `tf=` across the whole log,
# which also matches the frog observer's output in the same file -- and frog's
# tf=2 is frog's OWN GAMMA_22 enum, an unrelated numbering. That fixture was
# reading frog's transfer function and asserting about wp-cm's, so both its red
# and its green were statements about the wrong protocol.
wp_tfs()    { grep "^wpcm\[" "$OUTDIR/cm2w.log" 2>/dev/null \
	| grep -o "tf=[0-9]*" | cut -d= -f2 | tr '\n' ' '; }
wp_bound()  { grep -o "^wpcm_bound [01]" "$OUTDIR/cm2w.log" 2>/dev/null | awk '{print $2}' | head -1; }

hl_spawn_wlbgeffect cm2w 60 cm2w >/dev/null 2>&1
sleep 4

# ── PREMISES ─────────────────────────────────────────────────────────────
hl_assert "PREMISE: the client bound wp-color-management" "$(wp_bound)" "1"
MON="$(hl_get "get all-clients" | jq -r '.clients[]|select(.title=="cm2w")|.monitor' 2>/dev/null | head -1)"
hl_assert_true "PREMISE: the observer is mapped on an output ($MON)" \
	"$([ -n "$MON" ] && echo true || echo false)"
E0="$(wp_events)"
hl_assert_true "PREMISE: it read a preferred description at all ($E0)" \
	"$([ "$E0" -ge 1 ] && echo true || echo false)"

echo "  initial: $(wp_tfs)"

# ── PROVOKE THE OTHER WRITER ─────────────────────────────────────────────
# A move between outputs is a scene-graph leave+enter, which is exactly what
# surface_reconfigure -- and therefore scenefx's writer -- runs on.
# BY NAME, both ways. `tag_monitor,+1` parses as neither a direction nor a
# monitor name, does nothing, and hl_dispatch reports success either way -- so
# a provocation spelled that way would leave the window where it was and the
# assertion below would be about nothing.
OTHER="$(hl_get "get all-monitors" | jq -r ".monitors[].name" 2>/dev/null \
	| grep -v "^$MON$" | head -1)"
hl_assert_true "PREMISE: there is a second output to move to ($OTHER)" \
	"$([ -n "$OTHER" ] && echo true || echo false)"
hl_dispatch "tag_monitor,$OTHER" 3 >/dev/null 2>&1
sleep 3
hl_dispatch "tag_monitor,$MON" 3 >/dev/null 2>&1
sleep 3

E1="$(wp_events)"
TFS="$(wp_tfs)"
echo "  after two output moves: $E1 event(s), tf values: $TFS"

# ── THE ASSERTION ────────────────────────────────────────────────────────
# Not "the last value is right" -- a race is a claim about EVERY value. One
# gamma22 anywhere in the log means the other writer reached the client.
BAD=0
for t in $TFS; do
	[ "$t" = "$TF_GAMMA22" ] && BAD=$(( BAD + 1 ))
done
hl_assert "no description came from the max-preference policy (tf=$TF_GAMMA22)" \
	"$BAD" "0"

GOOD=0
for t in $TFS; do
	[ "$t" = "$TF_COMPOUND_POWER_2_4" ] && GOOD=$(( GOOD + 1 ))
done
hl_assert_true "every description came from az_preferred (tf=$TF_COMPOUND_POWER_2_4 x$GOOD)" \
	"$([ "$GOOD" -ge 1 ] && [ "$GOOD" -eq "$(echo $TFS | wc -w)" ] && echo true || echo false)"

hl_stop >/dev/null 2>&1
echo
hl_summary
rc=$?
[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
exit $rc
