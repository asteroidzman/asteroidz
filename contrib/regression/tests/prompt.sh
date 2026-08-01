# prompt.sh — what the compositor's modal prompt DRAWS, on every output.
#
# quit-confirm.sh covers the keyboard contract: who may answer, what is
# swallowed, what Escape means. None of it looks at a pixel, so all of it
# passed while the prompt dimmed one monitor out of two and left the other
# showing a desktop that was no longer taking input.
#
# That is the failure worth pinning. Both prompts that use this overlay -- the
# exit confirmation and the global-shortcuts key picker -- take the keyboard
# away from everything, and a screen that does not say so is a screen someone
# will keep typing at. On a multi-head desk the question can be asked on the
# monitor nobody is looking at.
#
# Measured against the harness wallpaper rather than a window: it is a flat
# grey, so "dimmed" is a number rather than a judgement, and no client has to
# be spawned and waited for on a second output.
#
# harness: needs-second-monitor -- run.sh sorts this after every
# single-monitor module; the output it creates outlives it.

# NEVER in live mode. This presses the real quit bind: it puts a modal prompt
# on the user's own screen and takes their keyboard, and if the Escape below
# missed, the only remaining answer is the one that ends the session.
_pr_live_skip() { # _pr_live_skip NAME -> 0 if the test should be skipped
	if [ "${HL_LIVE_MODE:-0}" = "1" ]; then
		hl_skip "$1: live mode -- this module presses the real quit bind"
		return 0
	fi
	return 1
}

_pr_setup_done=""
_pr_setup() {
	[ -z "$_pr_setup_done" ] || return 0
	if [ "$(hl_monitor_count)" -lt 2 ]; then
		hl_dispatch "create_virtual_output" 1
	fi
	[ "$(hl_monitor_count)" -ge 2 ] || return 1
	cat >> "$HL_CONFIG" <<'EOF'
binds {
	NONE+F9 { quit; }
}
EOF
	hl_dispatch "reload_config" 1
	_pr_setup_done=1
	return 0
}

# Held past the repeat delay, the way a person's own quit bind is pressed.
_pr_open() { "$HL_WLVKBD" hold F9 -- sleep 1.2 >/dev/null 2>&1; sleep 1.0; }
_pr_close() { "$HL_WLVKBD" press ESC >/dev/null 2>&1; sleep 0.8; }

# Every monitor's geometry, as "x y w h" lines, written to a FILE.
#
# grim composites the whole layout into one image, so each monitor is a
# rectangle inside it -- offset by the layout's own origin, which is NOT 0,0
# once a virtual output has been placed beside a real one.
#
# A file rather than a pipe because the readers below are `python3 - <<PY`,
# which is already using stdin for the program itself. Piping geometry in as
# well silently delivered nothing, and the test reported "need-two-monitors"
# against a compositor that had two.
_pr_geometry() { # _pr_geometry PATH
	hl_get "get all-monitors" \
		| jq -r '.monitors[] | "\(.x) \(.y) \(.width) \(.height)"' > "$1"
}

test_the_exit_prompt_covers_every_output() {
	_pr_live_skip test_the_exit_prompt_covers_every_output && return
	if ! _pr_setup; then
		hl_skip "test_the_exit_prompt_covers_every_output: no second monitor"
		return
	fi

	hl_screenshot prompt-before
	_pr_open
	hl_screenshot prompt-during

	local verdict
	_pr_geometry "$HL_OUTDIR/prompt-mons.txt"
	verdict="$(python3 - "$HL_OUTDIR/prompt-before.png" \
		"$HL_OUTDIR/prompt-during.png" "$HL_OUTDIR/prompt-mons.txt" <<'PY'
import sys
from PIL import Image

mons = []
for line in open(sys.argv[3]):
    line = line.split()
    if len(line) == 4:
        mons.append(tuple(int(v) for v in line))
if len(mons) < 2:
    print("need-two-monitors")
    raise SystemExit

before = Image.open(sys.argv[1]).convert("RGB")
during = Image.open(sys.argv[2]).convert("RGB")
b, d = before.load(), during.load()
iw, ih = during.size

# The layout's own origin. A virtual output is placed beside the real one and
# the pair need not start at 0,0 -- a test that assumed it did would pass
# headless and read the wrong rectangle live.
ox = min(m[0] for m in mons)
oy = min(m[1] for m in mons)

problems = []
for (mx, my, mw, mh) in mons:
    x0, y0 = mx - ox, my - oy
    if x0 + mw > iw or y0 + mh > ih:
        problems.append("monitor-outside-capture")
        continue

    # A patch near the monitor's own corner, clear of anything centred.
    def patch(px, sx, sy):
        return [sum(px[sx + i, sy + j]) / 3.0
                for i in range(0, 24, 4) for j in range(0, 24, 4)]

    was = sum(patch(b, x0 + 12, y0 + 12)) / 36.0
    now = sum(patch(d, x0 + 12, y0 + 12)) / 36.0
    # 0.55 black over the harness grey is a little over half of it. Anything
    # near "unchanged" is an output the prompt never reached.
    if now > was * 0.8:
        problems.append("not-dimmed@%d,%d(%d->%d)" % (mx, my, was, now))

    # And the prompt itself: white text on a near-black panel, in the middle.
    cx, cy = x0 + mw // 2, y0 + mh // 2
    half_w, half_h = min(300, mw // 2 - 1), min(90, mh // 2 - 1)
    brightest = 0
    for i in range(-half_w, half_w, 3):
        for j in range(-half_h, half_h, 3):
            brightest = max(brightest, max(d[cx + i, cy + j]))
    if brightest < 200:
        problems.append("no-label@%d,%d(max=%d)" % (mx, my, brightest))

print("every-output" if not problems else ",".join(problems))
PY
)"
	hl_assert "the exit prompt dims and draws on every output" \
		"$verdict" "every-output"

	_pr_close
}

# And it all goes away again. The dim is a scene node over everything; one left
# behind is a desktop that looks switched off and still takes input, which is
# the same lie as the one above told the other way round.
test_dismissing_the_prompt_restores_every_output() {
	_pr_live_skip test_dismissing_the_prompt_restores_every_output && return
	if ! _pr_setup; then
		hl_skip "test_dismissing_the_prompt_restores_every_output: no second monitor"
		return
	fi

	hl_screenshot prompt-clean
	_pr_open
	_pr_close
	hl_screenshot prompt-after

	_pr_geometry "$HL_OUTDIR/prompt-mons.txt"
	hl_assert "Escape takes the dim off every output" \
		"$(python3 - "$HL_OUTDIR/prompt-clean.png" \
			"$HL_OUTDIR/prompt-after.png" "$HL_OUTDIR/prompt-mons.txt" <<'PY'
import sys
from PIL import Image

mons = []
for line in open(sys.argv[3]):
    line = line.split()
    if len(line) == 4:
        mons.append(tuple(int(v) for v in line))
if not mons:
    print("no-monitors")
    raise SystemExit

clean = Image.open(sys.argv[1]).convert("RGB").load()
after_im = Image.open(sys.argv[2]).convert("RGB")
after = after_im.load()
iw, ih = after_im.size
ox = min(m[0] for m in mons)
oy = min(m[1] for m in mons)

problems = []
for (mx, my, mw, mh) in mons:
    x0, y0 = mx - ox, my - oy
    if x0 + mw > iw or y0 + mh > ih:
        problems.append("monitor-outside-capture")
        continue
    was = sum(sum(clean[x0 + 12 + i, y0 + 12 + j]) / 3.0
              for i in range(0, 24, 4) for j in range(0, 24, 4)) / 36.0
    now = sum(sum(after[x0 + 12 + i, y0 + 12 + j]) / 3.0
              for i in range(0, 24, 4) for j in range(0, 24, 4)) / 36.0
    if abs(now - was) > 8:
        problems.append("still-changed@%d,%d(%d->%d)" % (mx, my, was, now))

print("restored" if not problems else ",".join(problems))
PY
)" "restored"
}
