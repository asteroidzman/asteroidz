#!/usr/bin/env bash
# m6b-frog-metadata-test.sh -- M6B/D6: the preferred colour description is
# PER SURFACE, and it is re-sent when it changes.
#
# ── THE TWO DEFECTS THIS CLOSES ──────────────────────────────────────────
#
#   WRONG DISPLAY   frog resolved its output as "the first enabled HDR output,
#                   else the focused one", so a window living entirely on one
#                   panel was handed another panel's description and told to
#                   tone-map for a display it was not on.
#
#   SENT ONCE       the metadata went out when the client created the object
#                   and never again, so a client that connected before an
#                   output change kept tone-mapping for the state it was told
#                   about at startup, forever.
#
# ── TWO WITNESSES, BECAUSE ONE CANNOT SETTLE IT ──────────────────────────
#
# On two SDR outputs the serialized metadata is IDENTICAL, so a client-side
# observation cannot say which output was described. The fixture therefore reads
# both:
#
#   COMPOSITOR   `preferred_output` / `preferred_identity` per client, which is
#                what az_preferred_resolve() actually selected.
#   CLIENT       the frog preferred_metadata events wlbgeffect received.
#
# Neither alone is sufficient. Together they pin the selection AND the wire.
#
# ── WHY TWO WINDOWS ──────────────────────────────────────────────────────
#
# THE DISCRIMINATOR IS THAT THEY MUST DISAGREE. Under the old policy every
# surface resolves to the SAME output (the first HDR one, or the focused one),
# so two windows on two different outputs report the same description. Under the
# per-surface policy they report their own. One window cannot tell those apart;
# two can, and it needs no HDR output to do it -- which matters because a
# headless output cannot enter HDR at all (verified: set_output_hdr leaves
# hdr=false), so the HDR-flavoured version of this case is live-only.
#
# ── WHY wlbgeffect AND NOT A PURPOSE-BUILT CLIENT ────────────────────────
#
# An earlier version used a new minimal client whose toplevel never mapped. An
# unmapped surface is on no output, so the compositor correctly says nothing
# about it -- which reads exactly like a compositor that forgot to send.
# wlbgeffect already maps reliably and is already trusted by the blur fixtures,
# so it carries the observer. THE FIXTURE ASSERTS THE MAPPING FIRST; without
# that premise every result below is unfalsifiable.
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="m6b-frog-metadata"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m6b-frog-$$"
mkdir -p "$OUTDIR"

BGE="$(cd "$(dirname "$0")" && pwd)/wlbgeffect/wlbgeffect"
[ -x "$BGE" ] || { echo "SKIP: wlbgeffect not built -- run: cd contrib/wlbgeffect && make"; exit 77; }

CFG="layout { titlebar { enable 0 } }"

# WLBGEFFECT_SSD=1: without it the client draws its own decorations and the
# compositor's borders never render. Set for consistency with the other
# fixtures that use this client.
export WLBGEFFECT_SSD=1

HL_OUTPUTS=2
HL_OUTDIR="$OUTDIR"; HL_WIDTH=1920; HL_HEIGHT=1080
HL_ENV="ASTEROIDZ_RENDERER=avk ${AZ_FROG_BREAK:-}"
export HL_OUTPUTS HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT
hl_start "$CFG" >"$OUTDIR/hl_start.log" 2>&1
sleep 2

echo "══ M6B/D6 ══ the preferred description is per surface, and it is live"
[ -n "${AZ_FROG_BREAK:-}" ] && echo "   BREAK: $AZ_FROG_BREAK"
echo

cl() { hl_get "get all-clients" | jq -r ".clients[] | select(.title==\"$1\") | .$2" 2>/dev/null | head -1; }
ev() { grep -c "^frog\[" "$OUTDIR/$1.log" 2>/dev/null || echo 0; }
last() { grep "^frog\[" "$OUTDIR/$1.log" 2>/dev/null | tail -1; }

hl_spawn_wlbgeffect fwA 90 fwA >/dev/null 2>&1
sleep 3
hl_spawn_wlbgeffect fwB 90 fwB >/dev/null 2>&1
sleep 4

# ── PREMISES. IF ANY FAILS, NOTHING BELOW MEANS ANYTHING ─────────────────
A_MON="$(cl fwA monitor)"; B_MON="$(cl fwB monitor)"
hl_assert_true "PREMISE: fwA is a mapped client on an output ($A_MON)" \
	"$([ -n "$A_MON" ] && echo true || echo false)"
hl_assert_true "PREMISE: fwB is a mapped client on an output ($B_MON)" \
	"$([ -n "$B_MON" ] && echo true || echo false)"
hl_assert_true "PREMISE: fwA's frog object received metadata ($(ev fwA))" \
	"$([ "$(ev fwA)" -ge 1 ] && echo true || echo false)"

# Move fwB to the other output. BY NAME, not by direction: `tag_monitor,+1`
# parses as neither a direction nor a monitor name, does nothing, and
# hl_dispatch reports success either way.
hl_dispatch "tag_monitor,$HL_MON" 3 >/dev/null 2>&1
sleep 3
A_MON="$(cl fwA monitor)"; B_MON="$(cl fwB monitor)"
echo "  fwA on $A_MON, fwB on $B_MON"
hl_assert_true "PREMISE: the two windows are on DIFFERENT outputs" \
	"$([ -n "$A_MON" ] && [ -n "$B_MON" ] && [ "$A_MON" != "$B_MON" ] \
	   && echo true || echo false)"

echo
echo "── the wrong-display defect ──────────────────────────────────────────"
# THE DISCRIMINATING ASSERTION. Each surface must resolve to ITS OWN output.
# Under the old policy both resolve to the same one, which is what the break
# reinstates.
A_PREF="$(cl fwA preferred_output)"; B_PREF="$(cl fwB preferred_output)"
echo "  fwA: layout=$A_MON policy=$A_PREF"
echo "  fwB: layout=$B_MON policy=$B_PREF"
hl_assert "fwA's colour policy agrees with its layout" "$A_PREF" "$A_MON"
hl_assert "fwB's colour policy agrees with its layout" "$B_PREF" "$B_MON"
hl_assert_true "and the two surfaces resolve to DIFFERENT outputs" \
	"$([ "$A_PREF" != "$B_PREF" ] && echo true || echo false)"

# AND THE IDENTITIES DIFFER, which is what makes a move a change even when two
# outputs happen to describe the same colour.
A_ID="$(cl fwA preferred_identity)"; B_ID="$(cl fwB preferred_identity)"
hl_assert_true "their preferred identities differ ($A_ID vs $B_ID)" \
	"$([ -n "$A_ID" ] && [ "$A_ID" != "0" ] && [ "$A_ID" != "$B_ID" ] \
	   && echo true || echo false)"

echo
echo "── the values on the wire ────────────────────────────────────────────"
# The CLIENT's view, checked against what the compositor resolved. Both outputs
# are SDR here, so: frog's GAMMA_22 is 2, BT.709 red x is 32000 in the
# protocol's 1/50000 CIE units, and the luminances are az_preferred's SDR
# answer -- the scene reference, a 0.2 cd/m2 floor in units of 0.0001, and max
# FALL equal to max.
echo "  fwB last: $(last fwB)"
hl_assert_true "the wire carries this output's description (SDR, BT.709)" \
	"$(last fwB | grep -q "tf=2 primaries=32000,16500" && echo true || echo false)"
hl_assert_true "with the resolved luminances (203 / 0.2 / 203)" \
	"$(last fwB | grep -q "maxlum=203 minlum=2000 maxfall=203" && echo true || echo false)"
hl_assert "and the compositor agrees on the max luminance" \
	"$(cl fwB preferred_max_luminance)" "203"

echo
echo "── the send-once defect ──────────────────────────────────────────────"
# fwB MOVED, so its identity changed and its metadata must have been restated.
# The count is the assertion: the values are identical between two SDR outputs,
# so only the event separates "re-sent" from "never re-sent".
B_EV="$(ev fwB)"
echo "  fwB frog events after its move: $B_EV"
hl_assert_true "moving a surface RE-SENDS its metadata ($B_EV >= 2)" \
	"$([ "$B_EV" -ge 2 ] && echo true || echo false)"

echo
echo "── and nothing else churns ───────────────────────────────────────────"
# fwA did not move and its output did not change, so it must have heard nothing
# further. Without the identity gate, every layout change on any monitor would
# re-announce to every surface.
A_EV_BEFORE="$(ev fwA)"
hl_dispatch "tag_monitor,$HL_MON" 3 >/dev/null 2>&1
sleep 3
A_EV="$(ev fwA)"
echo "  fwA events: $A_EV_BEFORE -> $A_EV (it never moved)"
hl_assert "an unrelated surface moving sends NOTHING to fwA" "$A_EV" "$A_EV_BEFORE"

hl_stop >/dev/null 2>&1
echo
hl_summary
rc=$?
[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
exit $rc
