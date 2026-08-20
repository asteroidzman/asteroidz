#!/usr/bin/env bash
#
# A LATE COPY OWES A REPAINT TO EVERY OUTPUT IT WAS DRAWN ON.
#
# When a wl_shm client's copy has not finished, AVK draws the newest generation
# it already has and records that the frame owes a repaint once the copy lands.
# The debt was recorded in `entry->stale_output`, ONE wlr_output pointer -- so a
# surface straddling a monitor edge, which draws stale on both outputs in the
# same frame cycle, had its first output overwritten by its second. That output
# was never repainted and kept showing what it had, indefinitely, until
# something unrelated damaged it.
#
# Live symptom: a wallpaper covering both monitors came back on ONE of them
# after a wallpaper change and stayed there until a window was closed over it.
# The content was never wrong -- it was simply never asked for again -- which is
# why every stale draw in the trace read "0 behind" and why the content-side
# theories were all dead ends.
#
# THE PREMISE MATTERS MORE THAN THE CLAIM HERE. `shm_stale_multi_output_repaints`
# can only be non-zero if a spanning surface ACTUALLY drew stale, and a copy that
# always finishes in time makes this whole fixture vacuous. So the stale path
# firing at all is asserted first, and the test fails if it did not.
#
# Falsifier: BREAK=stale-one-output restores the single-output payment. It must
# drive the counter to 0 while the premise stays true.
set -u

unset ASTEROIDZ_INSTANCE_SIGNATURE WAYLAND_DISPLAY

. "$(dirname "$0")/lib/headless.sh"

CURRENT_TEST="avk-stale-multioutput"
BREAK="${BREAK:-}"

REPAINT="$(dirname "$0")/wlrepaint/wlrepaint"
[ -x "$REPAINT" ] || { echo "not built -- run: cd contrib/wlrepaint && make" >&2; exit 1; }

export HL_OUTPUTS=2
W1="${HL_WIDTH:-1920}"

# Straddles the seam, and big enough that the copy cannot land within a frame.
# --churn gives every generation a FRESH wl_buffer, which is what a client with
# no reusable sibling image looks like -- asteroidzbg allocates its wallpaper
# exactly this way, one wl_shm pool per buffer, destroyed immediately.
# Sized so a single generation cannot be copied inside a frame. 6.4MB packs in
# under a millisecond and NEVER goes late -- the first version of this fixture
# was vacuous for exactly that reason, and its premise assertion is what said so
# (402 commits, 0 stale frames).
WIN_W=3600; WIN_H=1900
WIN_X=$((W1 - 1800))
WIN_Y=0

OUTDIR="${TMPDIR:-/tmp}/asteroidz-stale-mo-$$"
HL_OUTDIR="$OUTDIR"
# ── WHY THE STALL IS THIS LARGE ────────────────────────────────────────────
#
# 4000us was enough for as long as a client copy was the whole buffer. It is
# not any more: AVK clips a copy to what can be SEEN of the buffer, and the
# spanning window this fixture builds is 1900 rows tall on two 1080-row
# outputs, so more than a third of every generation is below both screens and
# is never copied at all. The copies stopped outranning the frames and the
# whole run produced ONE stale frame -- the premise scraped through and the
# claim then depended on that single frame happening to land on the spanning
# surface, which it did not.
#
# Measured: 0.25.2 gave 1193 stale frames and 207 multi-output repaints at
# 4000us; the same fixture on the clipped build gave 1 and 0. At 30000us the
# clipped build gives 1468 and 175. The mechanism is intact -- the fixture had
# stopped being able to reach it, which is not the same thing and is worth the
# distinction being written down rather than the number quietly changed.
HL_ENV="AZ_AVK_SLOW_UPLOAD_US=${SLOW_US:-30000}"
[ "$BREAK" = stale-one-output ] && \
	HL_ENV="$HL_ENV AZ_BREAK_STALE_ONE_OUTPUT=1"
export HL_OUTDIR HL_ENV
mkdir -p "$OUTDIR"

[ -n "$BREAK" ] && echo "*** BREAK=$BREAK -- this build is deliberately wrong ***"
echo "outputs: HEADLESS-1 0..$W1, HEADLESS-2 $W1.."
echo "window:  $WIN_X,$WIN_Y ${WIN_W}x${WIN_H} (straddles the seam)"
echo

hl_start "borderpx 0
shadows 0
layer_shadows 0
blur 0
layout { titlebar { enable 0 } }" >"$OUTDIR/start.log" 2>&1 || {
	echo "  harness failed to start:"; tail -5 "$OUTDIR/start.log"; exit 2; }

"$REPAINT" --title spanner --size "${WIN_W}x${WIN_H}" --solid 3080c0 \
	--frames 400 --hold-ms 0 --churn >"$OUTDIR/repaint.log" 2>&1 &
HL_SPAWNED_PIDS+=("$!")
hl_wait_client_count 1 60 >/dev/null 2>&1

# Floating, then placed across the seam in LAYOUT coordinates. BEFORE the load
# clients exist: these dispatches act on the FOCUSED window, and a later spawn
# would silently steal them -- placing a loader and leaving the spanner on one
# output, which is a fixture that tests nothing while looking fine.
hl_dispatch "toggle_floating" 0.5
hl_dispatch "resize_window,${WIN_W},${WIN_H}" 0.5
hl_dispatch "move_window,${WIN_X},${WIN_Y}" 2

# PREMISE, and the reason this fixture was green-on-nothing once already: these
# are IPC dispatch NAMES, not C function names, and hl_dispatch swallows an
# unknown one without a word. The first version said "togglefloating" and
# "move:X,Y" -- neither exists -- so the window never left its tile, never
# spanned anything, and 624 stale draws produced a debt on one output because
# there only ever WAS one output. Read the geometry back and prove it straddles.
GEO="$(hl_get "get all-clients" | jq -r \
	'.clients[] | select(.title=="spanner") | "\(.x) \(.y) \(.width) \(.height)"' \
	| head -1)"
GX="$(echo "$GEO" | awk '{print $1}')"; GW="$(echo "$GEO" | awk '{print $3}')"
GX="${GX:-0}"; GW="${GW:-0}"
echo "  spanner geometry     : ${GEO:-<not found>}  (seam at $W1)"
hl_assert_true "PREMISE: the window really straddles the seam ($GX..$((GX+GW)) vs $W1)" \
	"$([ "$GX" -lt "$W1" ] && [ $((GX + GW)) -gt "$W1" ] && echo true || echo false)"

# Load, so the upload worker is genuinely behind rather than merely busy. The
# defect only appears once a copy misses its frame, and a lone client here is
# copied faster than the compositor can ask for it.
"$REPAINT" --title load1 --size 3400x1800 --solid 802050 \
	--frames 400 --hold-ms 0 --churn >"$OUTDIR/load1.log" 2>&1 &
HL_SPAWNED_PIDS+=("$!")
"$REPAINT" --title load2 --size 3400x1800 --solid 208050 \
	--frames 400 --hold-ms 0 --churn >"$OUTDIR/load2.log" 2>&1 &
HL_SPAWNED_PIDS+=("$!")
hl_wait_client_count 3 60 >/dev/null 2>&1
sleep 10

S="$(hl_get "get avk-stats")"
g() { echo "$S" | jq -r ".$1 // 0"; }

STALE="$(g shm_stale_frames)"
MULTI="$(g shm_stale_multi_output_repaints)"
COMMITS="$(g shm_commits)"
FALLBACK="$(g fallback_frames)"

echo "  shm commits          : $COMMITS"
echo "  stale frames         : $STALE"
echo "  multi-output repaints: $MULTI"
echo

# ── PREMISES ────────────────────────────────────────────────────────────────
# Without these the claim below is green on a scene that never exercised it.
hl_assert_true "PREMISE: the client committed shm at all (commits=$COMMITS)" \
	"$([ "$COMMITS" -gt 0 ] && echo true || echo false)"
hl_assert_true "PREMISE: a copy outran a frame, so a debt existed (stale=$STALE)" \
	"$([ "$STALE" -gt 0 ] && echo true || echo false)"

# ── THE CLAIM ───────────────────────────────────────────────────────────────
if [ "$BREAK" = stale-one-output ]; then
	hl_assert "BREAK pays only one output" "$MULTI" "0"
else
	hl_assert_true "a spanning surface's debt is paid on BOTH outputs (multi=$MULTI)" \
		"$([ "$MULTI" -gt 0 ] && echo true || echo false)"
fi

hl_assert "no fallback frame was introduced" "$FALLBACK" "0"

ALIVE="$(kill -0 "${HL_COMP_PID:-0}" 2>/dev/null && echo yes || echo no)"
hl_assert "compositor alive" "$ALIVE" "yes"

hl_stop >/dev/null 2>&1
echo
hl_summary
rc=$?
[ "${AZ_KEEP:-0}" = 1 ] || rm -rf "$OUTDIR"
exit $rc
