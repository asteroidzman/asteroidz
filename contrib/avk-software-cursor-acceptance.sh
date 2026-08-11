#!/usr/bin/env bash
# avk-software-cursor-acceptance.sh — the M3.5E acceptance run that headless
# testing cannot stand in for.
#
# A headless output is blind to the difference this test exists to measure.
# The headless backend implements `output_set_cursor()` as `return true;`, so
# it believes it always has a hardware plane and every cursor is composited
# anyway — forced software and hardware-planed are the same code path there,
# and `contrib/avk-cursor-*-test.sh` can no more tell them apart than they can
# tell a real monitor from a buffer. On real KMS the two are genuinely
# different: one hands 64x64 to a plane, the other puts the compositor in the
# frame path for every pointer motion, at pointer rates, forever.
#
# So this runs against the user's own session, with the user driving the
# pointer and watching the screen, and it only READS. The single write is an
# optional `dispatch reset_avk_stats`, which zeroes counters and touches
# nothing else; percentiles come from lifetime histograms and cannot be
# deltaed, so without it the frame-time numbers describe the whole session
# rather than the 30 seconds that matter.
#
# Requires the session to have been started with
# ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1 -- there is no runtime toggle, by
# design: promotion and demotion are startup decisions and a switch would let
# a test claim a transition the compositor never makes.
#
#   bash contrib/avk-software-cursor-acceptance.sh
#
# Every phase prints what to do and waits. Nothing is timed against you.
set -u

command -v amsg >/dev/null || { echo "needs amsg"; exit 1; }
command -v jq   >/dev/null || { echo "needs jq"; exit 1; }

PASS=0; FAIL=0
declare -a FAILURES=()
declare -a EYES=()

ok()   { PASS=$((PASS + 1)); printf '  ok   - %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); FAILURES+=("$1"); printf '  FAIL - %s\n' "$1"; }
note() { printf '         %s\n' "$1"; }

assert() { # assert <description> <true|false>
	if [ "$2" = true ]; then ok "$1"; else bad "$1"; fi
}

# A question only a pair of eyes can answer. Recorded, asked at the end, and
# counted -- an acceptance run that quietly drops the visual half is not an
# acceptance run.
eyes() { EYES+=("$1"); }

pause() { # pause <instruction>
	echo
	printf '>> %s\n' "$1"
	printf '>> press ENTER when done '
	read -r _
}

stats() { amsg get avk-stats 2>/dev/null; }

# Delta of one counter between two captured snapshots.
d() { # d <before.json> <after.json> <key>
	local b a
	b="$(jq -r ".$3 // 0" "$1")"
	a="$(jq -r ".$3 // 0" "$2")"
	echo $((a - b))
}
now() { jq -r ".$2 // 0" "$1"; }

WORK="${TMPDIR:-/tmp}/avk-swcursor-$$"
mkdir -p "$WORK"

# ---------------------------------------------------------------------------
# Preconditions. Every one of these has been got wrong at least once.
# ---------------------------------------------------------------------------

echo "== preconditions =="

FIRST="$(stats)"
if [ -z "$FIRST" ] || ! echo "$FIRST" | jq -e . >/dev/null 2>&1; then
	echo "avk-stats returned nothing. Either this is not an AVK session," >&2
	echo "or ASTEROIDZ_INSTANCE_SIGNATURE in this shell points somewhere" >&2
	echo "other than the running compositor." >&2
	exit 1
fi

# Ask the process, not the counters: the counters cannot distinguish "forced
# software" from "software because something demoted it this second".
COMP_PID="$(pgrep -x asteroidz | head -1)"
FORCED=false
if [ -n "$COMP_PID" ] && [ -r "/proc/$COMP_PID/environ" ]; then
	if tr '\0' '\n' < "/proc/$COMP_PID/environ" |
			grep -qx "ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1"; then
		FORCED=true
	fi
fi
assert "the session was started with ASTEROIDZ_AVK_FORCE_SOFTWARE_CURSOR=1" \
	"$FORCED"
if [ "$FORCED" != true ]; then
	echo
	echo "Start a session from the forced-software entry and run this again."
	echo "Without it this measures the hardware plane and proves nothing."
	exit 1
fi

MONS="$(amsg get all-monitors 2>/dev/null | jq -r '[.monitors[].name] | join(" ")')"
note "monitors: $MONS"
NMON="$(echo "$MONS" | wc -w)"
assert "more than one output is attached (crossings are testable)" \
	"$([ "$NMON" -gt 1 ] && echo true || echo false)"

echo
echo "The next step zeroes the AVK counters (dispatch reset_avk_stats). It"
echo "changes no rendering or window state -- it only sets counters to 0 --"
echo "but it IS a dispatch against your live session, so it is asked for."
printf ">> reset counters? [y/N] "
read -r ans
if [ "$ans" = y ] || [ "$ans" = Y ]; then
	amsg dispatch reset_avk_stats >/dev/null 2>&1
	note "counters reset"
else
	note "not reset -- percentiles describe the whole session, not this run"
fi

# ---------------------------------------------------------------------------
# Phase 1 — the invariant. A moving cursor over a still screen.
# ---------------------------------------------------------------------------

echo
echo "== phase 1: 30 seconds of pointer motion over static content =="
echo
echo "This is the one that decides whether software cursor is viable. Nothing"
echo "on screen may be animating: no video, no terminal cursor blinking under"
echo "the pointer, no clock ticking in view if you can help it. Move the"
echo "pointer continuously, over one output, for about half a minute."
echo
echo "We do not need software cursor to be free. We need it to be sane,"
echo "localized, asynchronous, and comfortably inside the frame budget."

stats > "$WORK/p1-before.json"
pause "move the pointer continuously for ~30 seconds over a still screen"
stats > "$WORK/p1-after.json"

FRAMES="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" frames)"
SW="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" software_cursor_frames)"
HW="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" hardware_cursor_frames)"
MOVES="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" cursor_moves)"
CURPX="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" cursor_damage_pixels)"
FULL="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" full_redraw_frames)"
PART="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" partial_redraw_frames)"
WAITS="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" cpu_sync_waits)"
FAILS="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" cursor_import_failures)"
NOIMG="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" cursor_no_image)"
FALLBACK="$(d "$WORK/p1-before.json" "$WORK/p1-after.json" fallback_frames)"
P50="$(now "$WORK/p1-after.json" cpu_frame_us_p50)"
P95="$(now "$WORK/p1-after.json" cpu_frame_us_p95)"
P99="$(now "$WORK/p1-after.json" cpu_frame_us_p99)"
DR95="$(now "$WORK/p1-after.json" damage_ratio_p95)"

echo
note "frames $FRAMES  software-cursor $SW  hardware-cursor $HW  moves $MOVES"
note "redraws: partial $PART full $FULL   cpu frame us: p50 $P50 p95 $P95 p99 $P99"
note "damage ratio p95 $DR95"

assert "the compositor drew the cursor itself ($SW software cursor frames)" \
	"$([ "$SW" -gt 0 ] && echo true || echo false)"
assert "the hardware plane was never used ($HW hardware cursor frames)" \
	"$([ "$HW" -eq 0 ] && echo true || echo false)"
assert "the pointer actually moved ($MOVES moves)" \
	"$([ "$MOVES" -gt 100 ] && echo true || echo false)"

# Localized. A cursor move must repaint a cursor, not an output. Two cursor
# rectangles per move (leave the old place, arrive at the new one) is correct;
# a 4K output frame is 8.3M pixels, so anything in that region is a full
# repaint wearing a cursor's name.
if [ "$SW" -gt 0 ]; then
	PERFRAME=$((CURPX / SW))
	note "cursor damage per software cursor frame: $PERFRAME px"
	assert "cursor damage stays local (${PERFRAME} px/frame, well under an output)" \
		"$([ "$PERFRAME" -lt 65536 ] && echo true || echo false)"
fi

# A cursor move must not force a full output redraw.
if [ "$FRAMES" -gt 0 ]; then
	assert "pointer motion did not force full output redraws ($FULL of $FRAMES)" \
		"$([ $((FULL * 100)) -lt "$FRAMES" ] && echo true || echo false)"
fi

# Asynchronous. Non-negotiable, milestone-wide.
assert "no CPU waits appeared in the frame path ($WAITS)" \
	"$([ "$WAITS" -eq 0 ] && echo true || echo false)"
assert "no frame fell back off the AVK path ($FALLBACK)" \
	"$([ "$FALLBACK" -eq 0 ] && echo true || echo false)"
assert "no cursor import failed ($FAILS)" \
	"$([ "$FAILS" -eq 0 ] && echo true || echo false)"
assert "the cursor was never missing an image ($NOIMG)" \
	"$([ "$NOIMG" -eq 0 ] && echo true || echo false)"

# Within budget. 8000us is generous even for 120Hz (8333us); the point of the
# assertion is to catch a path that has become synchronous, not to grade it.
assert "p95 CPU frame time is inside a frame budget (${P95}us)" \
	"$([ "$P95" -lt 8000 ] && echo true || echo false)"
assert "p99 CPU frame time has no catastrophic tail (${P99}us)" \
	"$([ "$P99" -lt 16000 ] && echo true || echo false)"

eyes "phase 1: the cursor tracked your hand smoothly, with no lag, tearing, \
trail or stutter"

# ---------------------------------------------------------------------------
# Phase 2 — shapes. Every one the desktop can produce.
# ---------------------------------------------------------------------------

echo
echo "== phase 2: cursor shapes =="
echo
echo "Produce as many different shapes as you can: a text I-beam over a text"
echo "field, a resize arrow on a window edge and a corner, a hand over a link,"
echo "a busy/wait cursor if anything offers one, and the plain arrow between"
echo "each. Watch that each one is the right shape, sharp, and correctly"
echo "positioned -- the hotspot is where the click lands, not where the"
echo "picture is."

stats > "$WORK/p2-before.json"
pause "cycle through as many cursor shapes as you can reach"
stats > "$WORK/p2-after.json"

SHAPES="$(d "$WORK/p2-before.json" "$WORK/p2-after.json" cursor_shape_sets)"
XSETS="$(d "$WORK/p2-before.json" "$WORK/p2-after.json" cursor_xcursor_sets)"
CSETS="$(d "$WORK/p2-before.json" "$WORK/p2-after.json" cursor_client_surface_sets)"
REIMP="$(d "$WORK/p2-before.json" "$WORK/p2-after.json" cursor_forced_reimports)"
FAILS2="$(d "$WORK/p2-before.json" "$WORK/p2-after.json" cursor_import_failures)"
NOIMG2="$(d "$WORK/p2-before.json" "$WORK/p2-after.json" cursor_no_image)"
NOBUF="$(d "$WORK/p2-before.json" "$WORK/p2-after.json" cursor_client_no_buffer)"

echo
note "shape sets $SHAPES  xcursor sets $XSETS  client surface sets $CSETS"
note "forced reimports $REIMP  no-buffer $NOBUF"

assert "the shape genuinely changed more than a couple of times ($((SHAPES + CSETS)))" \
	"$([ $((SHAPES + CSETS)) -gt 3 ] && echo true || echo false)"
assert "every shape change produced an image ($XSETS xcursor loads, $FAILS2 failures)" \
	"$([ "$FAILS2" -eq 0 ] && echo true || echo false)"
assert "no shape change left the cursor imageless ($NOIMG2)" \
	"$([ "$NOIMG2" -eq 0 ] && echo true || echo false)"
assert "no client cursor surface arrived without a buffer ($NOBUF)" \
	"$([ "$NOBUF" -eq 0 ] && echo true || echo false)"

eyes "phase 2: every shape you produced was the right one, drawn sharply, \
with the click landing where the hotspot points"

# ---------------------------------------------------------------------------
# Phase 3 — crossing outputs. Different scales, different planes, no plane.
# ---------------------------------------------------------------------------

echo
echo "== phase 3: crossing between outputs =="
echo
echo "Outputs: $MONS"
echo "Cross between them several times, slowly enough to watch the moment of"
echo "transit, then quickly a few times. Watch for: the cursor vanishing at"
echo "the seam, a copy left behind on the output you left, a size jump if the"
echo "outputs are at different scales, or the shape resetting for no reason."

stats > "$WORK/p3-before.json"
pause "cross between outputs several times, slowly and then quickly"
stats > "$WORK/p3-after.json"

CULLED="$(d "$WORK/p3-before.json" "$WORK/p3-after.json" cursor_culled)"
SW3="$(d "$WORK/p3-before.json" "$WORK/p3-after.json" software_cursor_frames)"
HW3="$(d "$WORK/p3-before.json" "$WORK/p3-after.json" hardware_cursor_frames)"
FAILS3="$(d "$WORK/p3-before.json" "$WORK/p3-after.json" cursor_import_failures)"
REIMP3="$(d "$WORK/p3-before.json" "$WORK/p3-after.json" cursor_forced_reimports)"

echo
note "software cursor frames $SW3  hardware $HW3  culled $CULLED  reimports $REIMP3"

assert "crossings stayed on the software path ($HW3 hardware frames)" \
	"$([ "$HW3" -eq 0 ] && echo true || echo false)"
assert "the cursor was culled from the output it was not on ($CULLED)" \
	"$([ "$CULLED" -gt 0 ] && echo true || echo false)"
assert "no import failed while crossing ($FAILS3)" \
	"$([ "$FAILS3" -eq 0 ] && echo true || echo false)"

eyes "phase 3: the cursor crossed cleanly -- never vanished at the seam, left \
no copy behind, and changed size only where the outputs differ in scale"

# ---------------------------------------------------------------------------
# Phase 4 — the client matrix. Toolkits disagree about cursors.
# ---------------------------------------------------------------------------

echo
echo "== phase 4: clients =="
echo
echo "Move the pointer over each of these in turn, and over their chrome as"
echo "well as their content:"
echo "    Firefox        (its own cursor handling, wp_cursor_shape_v1)"
echo "    Chromium or an Electron app"
echo "    a GTK application"
echo "    a Qt6 / KDE application"
echo "    an XWayland application"
echo "    a terminal"
echo
echo "XWayland is the one to watch: it has its own cursor pipeline and has"
echo "already produced two bugs in this milestone."

stats > "$WORK/p4-before.json"
pause "hover each client in the list, including its chrome"
stats > "$WORK/p4-after.json"

FAILS4="$(d "$WORK/p4-before.json" "$WORK/p4-after.json" cursor_import_failures)"
NOIMG4="$(d "$WORK/p4-before.json" "$WORK/p4-after.json" cursor_no_image)"
NOBUF4="$(d "$WORK/p4-before.json" "$WORK/p4-after.json" cursor_client_no_buffer)"
HW4="$(d "$WORK/p4-before.json" "$WORK/p4-after.json" hardware_cursor_frames)"
SETS4="$(d "$WORK/p4-before.json" "$WORK/p4-after.json" cursor_shape_sets)"
CSETS4="$(d "$WORK/p4-before.json" "$WORK/p4-after.json" cursor_client_surface_sets)"

echo
note "shape sets $SETS4  client surface sets $CSETS4  no-buffer $NOBUF4"

assert "clients changed the cursor ($((SETS4 + CSETS4)) changes)" \
	"$([ $((SETS4 + CSETS4)) -gt 0 ] && echo true || echo false)"
assert "no client's cursor failed to import ($FAILS4)" \
	"$([ "$FAILS4" -eq 0 ] && echo true || echo false)"
assert "no client left the cursor imageless ($NOIMG4)" \
	"$([ "$NOIMG4" -eq 0 ] && echo true || echo false)"
assert "no client surface arrived without a buffer ($NOBUF4)" \
	"$([ "$NOBUF4" -eq 0 ] && echo true || echo false)"
assert "still no hardware plane ($HW4)" \
	"$([ "$HW4" -eq 0 ] && echo true || echo false)"

eyes "phase 4: every client showed a correct cursor, XWayland included, with \
no window showing the X server's own 'X' or a stale shape from the last window"

# ---------------------------------------------------------------------------
# Phase 5 — hide and restore. Three of its four failure modes survive a
# screenshot taken a moment too late, so this half is the user's eyes.
# ---------------------------------------------------------------------------

echo
echo "== phase 5: hiding and restoring =="
echo
echo "Type into a terminal until the cursor hides (or wait out"
echo "cursor_hide_timeout), then move the pointer to bring it back. Do it a"
echo "few times, and once with the pointer over a client that sets its own"
echo "cursor. Watch for a ghost left where it was, and for the wrong shape"
echo "coming back."

stats > "$WORK/p5-before.json"
pause "hide the cursor and bring it back, a few times"
stats > "$WORK/p5-after.json"

UNSETS="$(d "$WORK/p5-before.json" "$WORK/p5-after.json" cursor_unsets)"
FAILS5="$(d "$WORK/p5-before.json" "$WORK/p5-after.json" cursor_import_failures)"
FULL5="$(d "$WORK/p5-before.json" "$WORK/p5-after.json" full_redraw_frames)"
FRAMES5="$(d "$WORK/p5-before.json" "$WORK/p5-after.json" frames)"

echo
note "unsets $UNSETS  full redraws $FULL5 of $FRAMES5 frames"

assert "the cursor was actually hidden ($UNSETS unsets)" \
	"$([ "$UNSETS" -gt 0 ] && echo true || echo false)"
assert "hiding did not repaint whole outputs ($FULL5 of $FRAMES5)" \
	"$([ "$FRAMES5" -eq 0 ] || [ $((FULL5 * 20)) -lt "$FRAMES5" ] && echo true || echo false)"
assert "no import failed around a hide ($FAILS5)" \
	"$([ "$FAILS5" -eq 0 ] && echo true || echo false)"

eyes "phase 5: the cursor disappeared completely with no ghost left behind, \
and came back in the right shape in the right place"

# ---------------------------------------------------------------------------
# The half a script cannot measure.
# ---------------------------------------------------------------------------

echo
echo "== what only you saw =="
echo
echo "The counters above cannot see a wrong-looking cursor, only a missing"
echo "one. Answer each of these; a no is a milestone failure, not a note."
for q in "${EYES[@]}"; do
	echo
	printf '?? %s\n' "$q"
	printf '?? yes / no [y/N] '
	read -r a
	if [ "$a" = y ] || [ "$a" = Y ]; then
		ok "confirmed: ${q:0:60}..."
	else
		bad "NOT confirmed: $q"
	fi
done

echo
echo "----"
echo "snapshots: $WORK"
echo "$PASS/$((PASS + FAIL)) checks passed"
if [ "$FAIL" -gt 0 ]; then
	echo "failures:"
	for f in "${FAILURES[@]}"; do echo "  $f"; done
	exit 1
fi
