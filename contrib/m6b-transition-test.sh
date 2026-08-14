#!/usr/bin/env bash
# m6b-transition-test.sh -- M6B gate G6: the colour path can be entered and left
# repeatedly without leaking, recompiling, or corrupting what it left behind.
#
# ── WHAT A "TRANSITION" IS HERE ──────────────────────────────────────────
#
# Attaching and detaching a matrix-shaper profile moves an 8-bit SDR output
# between Path A (composite through the scanout buffer's _SRGB view) and Path B
# (composite into a scene-linear FP16 intermediate, then encode with the
# display's measured curve). That is a COLOUR DOMAIN CHANGE with a resource
# lifecycle attached, and it is the same lifecycle an HDR toggle uses:
#
#   the encode intermediate    allocated on entering B, returned on leaving it
#   the LUT image             allocated on the profile, retired with the output
#   the blur cache            built in one domain, invalid in the other
#   the pipeline variants     keyed on (format, curve); must not recompile
#
# ── WHY NOT HDR↔SDR, WHICH IS WHAT G6 ORIGINALLY NAMED ───────────────────
#
# A headless output does not support BT.2020 + PQ, so it cannot enter HDR at
# all and an "HDR toggle" here would be twenty no-ops reporting success. The
# profile toggle exercises the same code with a transition that actually
# happens. The HDR half is live-only on DP-1 and is recorded as outstanding in
# docs/m7-next/status.md rather than simulated.
#
# ── WHAT IT ASSERTS, AFTER N CYCLES ──────────────────────────────────────
#
#   0. the transitions HAPPENED         (the premise: encode draws move)
#   1. no validation errors             (layer asserted present first)
#   2. no frames refused                fallback_frames == 0
#   3. no pipeline compile per cycle    compiles bounded, not linear in N
#   4. the intermediate is RETURNED     images back to 0 outside Path B
#   5. the blur cache is invalidated across the domain change, not reused
set -u

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="m6b-transition"
OUTDIR="${TMPDIR:-/tmp}/asteroidz-m6b-trans-$$"
mkdir -p "$OUTDIR"

PROFILE="${AZ_ICC_PROFILE:-/home/ralf/FI32U.icm}"
CYCLES="${AZ_CYCLES:-20}"

if [ ! -r "$PROFILE" ]; then
	echo "SKIP: $PROFILE not present -- G6's profile transition is display-specific"
	exit 77
fi

# BLUR ON, deliberately. The blur cache is one of the things a domain change
# must invalidate, and a fixture with effects off cannot see it.
# The blur-cache fixture's own configuration, copied rather than invented: the
# MONITOR_BACKGROUND role only engages with a shadow that blurs its backdrop AND
# optimized blur on, and a config that merely says `blur enable 1` leaves the
# cache reading 0 rebuilds / 0 hits forever -- an assertion on it would then be
# measuring nothing in either direction.
CFG="border_radius 12
borderpx 4
effects { shadow { enable 1; size 20; blur 12; only-floating 0; tiled-scale 1.0
    blur-background 1; blur-background-strength 0.55; blur-background-darken 1 }
  blur { enable 1; optimized 1; passes 2; radius 6 } }
layout { titlebar { enable 0 } }"

# WLBGEFFECT_SSD=1: without it the client draws its own decorations and the
# compositor's borders never render, which is what supplies the backdrop.
export WLBGEFFECT_SSD=1

JQ='{fallback:.fallback_frames, verr:.validation_errors, von:.validation_enabled,
     enc:.m5_encode_draws, compiles:.m5_encode_compiles,
     imgs:.m5_intermediate_images,
     bc:(.blur_cache_outputs[0].rebuilds // 0),
     bh:(.blur_cache_outputs[0].hits // 0)}'
v() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
snap() { hl_get "get avk-stats" | jq -r "$JQ | to_entries |
	map(\"\(.key)=\(.value)\") | join(\" \")" 2>/dev/null; }

HL_OUTDIR="$OUTDIR"; HL_WIDTH=1920; HL_HEIGHT=1080; HL_SCALE1=1
HL_ENV="ASTEROIDZ_RENDERER=avk ASTEROIDZ_VK_DEBUG=1"
export HL_OUTDIR HL_ENV HL_WIDTH HL_HEIGHT HL_SCALE1
hl_start "$CFG" >"$OUTDIR/hl_start.log" 2>&1
sleep 2

echo "══ M6B/G6 ══ $CYCLES profile on/off cycles (Path A <-> B, LUT1D <-> sRGB)"
echo

# A BACKGROUND LAYER FIRST, then a window over it.
#
# The M4I cache belongs to the monitor's OPTIMIZED BLUR node, and that node has
# nothing to blur unless something occupies the background layer -- on a real
# desktop that is the wallpaper. Without it the cache never engages at all and
# reads 0 rebuilds / 0 hits forever, which an assertion on "rebuilds moved"
# would satisfy in neither direction: it would simply be measuring nothing.
hl_spawn_wlbgeffect g6 300 g6 >/dev/null 2>&1
hl_wait_client_count 1 200 >/dev/null 2>&1
sleep 3
# Reset AFTER the window settles: the cold cache build happens on the first
# frames, and measuring from before it means `rebuilds` is already non-zero for
# a reason that has nothing to do with the transitions.
hl_dispatch reset_avk_stats 1 >/dev/null 2>&1

BASE="$(snap)"
echo "  before: $BASE"
hl_assert_true "PREMISE: the validation layer is loaded" "$(v von "$BASE")"

# ── THE CYCLES ───────────────────────────────────────────────────────────
# Settled between each: a transition half-applied when the next one starts
# would test a race rather than a lifecycle, and the lifecycle is what is
# unproven.
i=0
while [ "$i" -lt "$CYCLES" ]; do
	hl_dispatch "set_output_icc,$HL_MON,$PROFILE" 0 >/dev/null 2>&1
	sleep 0.35
	ON_SNAP="$(snap)"
	hl_dispatch "set_output_icc,$HL_MON," 0 >/dev/null 2>&1
	sleep 0.35
	OFF_SNAP="$(snap)"
	i=$(( i + 1 ))
done

AFTER="$(snap)"
echo "  after : $AFTER"
echo "  last on-cycle: $ON_SNAP"
echo

# ── 0. THE TRANSITIONS HAPPENED ──────────────────────────────────────────
# Without this every assertion below is satisfied by a compositor that ignored
# all $CYCLES dispatches: nothing recompiles, nothing leaks and nothing is
# refused when nothing happened.
hl_assert_true "PREMISE: the encode pass ran during the on-cycles ($(v enc "$ON_SNAP"))" \
	"$([ "$(v enc "$ON_SNAP")" -gt "$(v enc "$BASE")" ] && echo true || echo false)"
hl_assert_true "PREMISE: and an intermediate existed while on Path B" \
	"$([ "$(v imgs "$ON_SNAP")" -gt 0 ] && echo true || echo false)"

# ── 1-2. CLEAN ───────────────────────────────────────────────────────────
hl_assert "no validation errors across $CYCLES cycles" "$(v verr "$AFTER")" 0
hl_assert "no frames refused" "$(v fallback "$AFTER")" 0

# ── 3. NO RECOMPILE PER CYCLE ────────────────────────────────────────────
# A pipeline compile is a stall of milliseconds. Two variants exist here (sRGB
# and LUT1D) and both are built once; a count that scales with $CYCLES is a
# keying bug, and the bound is stated as a constant rather than as "small".
hl_assert_true "pipeline variants compiled at most 4 times, not $CYCLES ($(v compiles "$AFTER"))" \
	"$([ "$(v compiles "$AFTER")" -le 4 ] && echo true || echo false)"

# ── 4. THE INTERMEDIATE IS RETURNED ──────────────────────────────────────
# Path A has no encode pass and must hold no intermediate. Leaking one per cycle
# is 8 MB a time at this size and is exactly the kind of thing that only shows
# up after a long session.
hl_assert "the intermediate is returned when the profile comes off" \
	"$(v imgs "$AFTER")" 0

# ── 5. THE BLUR CACHE CROSSED THE DOMAIN CHANGE HONESTLY ─────────────────
# The cache holds a blurred backdrop in ONE colour domain. Entering Path B
# changes the domain, so a cache that survived unchanged would be serving a
# picture blurred in the other encoding -- the M5 defect, in transition form.
# Rebuilds must therefore MOVE with the cycles.
# AT LEAST ONE REBUILD PER TRANSITION, not merely "more than before". There are
# 2*CYCLES transitions and a cache that survived the domain change would show
# FEWER rebuilds than transitions -- "greater than zero" would be satisfied by
# ordinary damage and would say nothing about invalidation at all.
hl_assert_true "the blur cache rebuilt at least once per transition ($(v bc "$AFTER") >= $(( CYCLES * 2 )))" \
	"$([ "$(v bc "$AFTER")" -ge "$(( CYCLES * 2 ))" ] && echo true || echo false)"

echo
hl_stop >/dev/null 2>&1
hl_summary
rc=$?
[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
exit $rc
