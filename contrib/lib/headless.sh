# headless.sh — shared library for headless asteroidz test harnesses.
# Source this, don't execute it: `. "$(dirname "$0")/../lib/headless.sh"`
#
# Provides a fully isolated compositor instance (own XDG_RUNTIME_DIR, own
# Wayland socket, own IPC instance signature) so tests never touch a live
# session, plus small helpers for dispatching compositor functions, reading
# IPC state, spawning/tracking throwaway test windows, screenshotting, and
# TAP-style pass/fail assertions.
#
# Conventions used by contrib/regression/run.sh (but usable standalone too):
#   - hl_start must be called once before anything else.
#   - hl_reset should be called between independent test cases sharing one
#     compositor instance (kills test-spawned windows, returns to tag 1 /
#     tile layout) so cases don't leak state into each other.
#   - hl_stop must be called on exit (the runner traps this for you).
#
# Env knobs:
#   ASTEROIDZ   compositor binary (default: build/asteroidz next to this
#               repo, falling back to /usr/bin/asteroidz)
#   HL_OUTDIR   work dir (default /tmp/asteroidz-hl-<random>)
#   HL_WIDTH / HL_HEIGHT   output size (default 1920x1080)
#   HL_OUTPUTS             how many headless outputs (default 1). 2 creates
#                          HEADLESS-2 immediately to the RIGHT of HEADLESS-1,
#                          which is what makes "this node belongs to the other
#                          monitor" a real condition rather than a hypothetical
#                          -- several AVK counters (nodes_output_culled_before_
#                          resolve, cursor_culled) can only ever read 0 with a
#                          single output, and a break test that flips them is
#                          inert until a second one exists. Costs a second
#                          compositor output's worth of frames and GPU memory,
#                          so it is not the default.
#   HL_EXTRA_KDL           extra config appended verbatim (e.g. a second
#                          `output HEADLESS-2 { ... }` block for
#                          multi-monitor tests)
#   HL_ENV                 space-separated NAME=VALUE pairs passed through to
#                          the compositor. The launch below uses `env -i` so a
#                          test instance cannot inherit the caller's session,
#                          which also means an exported variable does NOT reach
#                          it -- MESA_VK_TRACE and FX_VK_VALIDATION both looked
#                          like they were being ignored by the driver when in
#                          fact they never arrived.
#   HL_RENDERER            gles2 (default) or vulkan. gles2 is the default
#                          because it is the one renderer present on every
#                          machine that runs this suite, so results are
#                          comparable; set vulkan to exercise the fx_vk path
#                          (which is what a real session here actually uses,
#                          and the only way to reach its GPU instrumentation).
#                          Editing the launch line by hand instead does NOT
#                          work once this file is sourced -- the function body
#                          is already parsed, which silently produced a second
#                          gles2 run labelled as vulkan.
set -u

HL_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The compositor binary. Resolved HERE, at source time, from $ASTEROIDZ -- so a
# fixture that wants to run two different builds must set HL_ASTEROIDZ directly
# before each hl_start, NOT $ASTEROIDZ, which is read once and never again.
#
# That is not a style note. Two M4E fixtures set `ASTEROIDZ=<old> hl_start`,
# both ran the CURRENT build twice, and both reported a clean comparison: a
# byte-identical framebuffer and a matching cost table, neither of which said
# anything about the old binary. hl_start now logs the path it starts and
# hl_binary() reports it, so a fixture can assert it got the build it asked for.
HL_ASTEROIDZ="${ASTEROIDZ:-$HL_REPO/build/asteroidz}"
[ -x "$HL_ASTEROIDZ" ] || HL_ASTEROIDZ=/usr/bin/asteroidz

# The binary the most recent hl_start actually launched.
hl_binary() { echo "${HL_STARTED_BINARY:-}"; }
HL_WLVPTR="$HL_REPO/contrib/wlvptr/wlvptr"
HL_WLVKBD="$HL_REPO/contrib/wlvkbd/wlvkbd"
HL_WLLAYER="$HL_REPO/contrib/wllayer/wllayer"
HL_WLKEYS="$HL_REPO/contrib/wlkeys/wlkeys"
HL_WLSTATES="$HL_REPO/contrib/wlstates/wlstates"
HL_WLSANDBOX="$HL_REPO/contrib/wlsandbox/wlsandbox"
HL_WLBGEFFECT="$HL_REPO/contrib/wlbgeffect/wlbgeffect"
HL_WIDTH="${HL_WIDTH:-1920}"
# How many headless outputs the backend creates. One unless a test says
# otherwise; see the HL_OUTPUTS note in the header for why a second one is not
# free.
HL_OUTPUTS="${HL_OUTPUTS:-1}"
HL_HEIGHT="${HL_HEIGHT:-1080}"
# wlvptr's absolute-pointer extent -- equal to HL_WIDTH/HL_HEIGHT except in
# hl_start_live's HL_LIVE_MON branch, which overrides these to the full
# multi-monitor layout bounding box (see the comment there for why).
HL_PTR_EXTENT_W="$HL_WIDTH"
HL_PTR_EXTENT_H="$HL_HEIGHT"
HL_MON="${HL_MON:-HEADLESS-1}"   # the monitor every test targets; see hl_start_live

ASSERT_COUNT=0
ASSERT_PASS=0
declare -a ASSERT_FAILURES=()
declare -a HL_SPAWNED_PIDS=()
CURRENT_TEST="${CURRENT_TEST:-(no test)}"

# ─── lifecycle ────────────────────────────────────────────────────────────

# The private session bus started alongside the compositor, by recorded PID.
# Never by pattern: a pattern that matched the user's real dbus-daemon would
# take their whole desktop down with it.
hl_dbus_stop() {
	[ -n "${HL_DBUS_PID:-}" ] && kill "$HL_DBUS_PID" 2>/dev/null
	HL_DBUS_PID=""
	HL_DBUS_ADDR=""
}

# Run a busctl call against the test compositor's own bus. Empty output and a
# non-zero status when there is no bus, which is what lets a portal module skip
# itself rather than fail.
hl_busctl() { # hl_busctl ARGS...
	[ -n "${HL_DBUS_ADDR:-}" ] || return 1
	busctl --address="$HL_DBUS_ADDR" "$@" 2>&1
}

hl_start() { # hl_start [EXTRA_KDL]
	HL_OUTDIR="${HL_OUTDIR:-/tmp/asteroidz-hl-$$}"
	mkdir -p "$HL_OUTDIR"
	local extra="${1:-${HL_EXTRA_KDL:-}}"

	for t in grim swaybg jq; do
		command -v "$t" >/dev/null || { echo "hl_start: missing tool: $t" >&2; exit 1; }
	done
	[ -x "$HL_ASTEROIDZ" ] || { echo "hl_start: no asteroidz binary at $HL_ASTEROIDZ" >&2; exit 1; }
	HL_STARTED_BINARY="$HL_ASTEROIDZ"
	[ -x "$HL_WLVPTR" ] || { echo "hl_start: wlvptr not built -- run: cd contrib/wlvptr && make" >&2; exit 1; }
	[ -x "$HL_WLVKBD" ] || { echo "hl_start: wlvkbd not built -- run: cd contrib/wlvkbd && make" >&2; exit 1; }
	[ -x "$HL_WLLAYER" ] || { echo "hl_start: wllayer not built -- run: cd contrib/wllayer && make" >&2; exit 1; }
	[ -x "$HL_WLKEYS" ] || { echo "hl_start: wlkeys not built -- run: cd contrib/wlkeys && make" >&2; exit 1; }

	HL_CONFIG="$HL_OUTDIR/config.kdl"

	# A second output, placed to the RIGHT of the first with no overlap.
	#
	# Side by side matters more than it looks. A pair of outputs stacked at the
	# same layout origin makes every node touch both of them, so the very
	# thing a second output is usually added to test -- that work belonging to
	# one monitor is not done for the other -- is silently untestable. The
	# offset is HL_WIDTH exactly, so a node at x >= HL_WIDTH is on the second
	# output and nowhere else.
	#
	# HL_SCALE1 / HL_SCALE2 set the outputs' scales. They belong in the
	# `output` block and NOWHERE ELSE: `monitorrule` parses colon-separated
	# key:value pairs, so the KDL-child form `monitorrule { scale 1.5 }` is
	# accepted, ignored, and leaves the output at scale 1 without a word. Two
	# tests here claimed to run at mixed scales for weeks on that basis.
	#
	# Mixed scales are not a detail: at equal scales, a cursor sized by
	# wlroots per-output and one sized by asteroidz at the sharpest scale come
	# out identical, so a whole class of ownership bug is invisible.
	local scale1="" scale2=""
	[ -n "${HL_SCALE1:-}" ] && scale1="scale $HL_SCALE1; "
	[ -n "${HL_SCALE2:-}" ] && scale2="scale $HL_SCALE2; "
	# HL_RR1 / HL_RR2 set the outputs' TRANSFORMS (0-7, the wl_output_transform
	# enumeration). Like scale they belong in the `output` block; a rotated
	# output swaps the buffer's width and height, which is the cheapest way for
	# a caller to check the setting took at all.
	local rr1="" rr2=""
	[ -n "${HL_RR1:-}" ] && rr1="rr $HL_RR1; "
	[ -n "${HL_RR2:-}" ] && rr2="rr $HL_RR2; "

	# HL_X2 is the second output's LAYOUT x, which is LOGICAL. The default of
	# HL_WIDTH is right only while the first output is at scale 1: at scale 1.5
	# its logical width is HL_WIDTH/1.5, so the default leaves a GAP between the
	# two outputs and there is no seam to test. A seam fixture must set this to
	# the first output's logical width.
	local x2="${HL_X2:-$HL_WIDTH}"
	local secondary_output=""
	if [ "${HL_OUTPUTS:-1}" -ge 2 ]; then
		secondary_output="output HEADLESS-2 { ${scale2}${rr2}x $x2; y 0; width $HL_WIDTH; height $HL_HEIGHT; refresh 60 }"
	fi

	cat > "$HL_CONFIG" <<EOF
border_radius 8
borderpx 2
shadows 1
layer_shadows 1
shadows_size 24
shadows_blur 24
shadowscolor 0x00000060
shadows_blur_background 1
shadows_blur_background_strength 0.5
effects {
	blur { enable 1; optimized 1; passes 2; radius 6;
		params { noise 0.02; brightness 0.9; contrast 0.9; saturation 1.2; } }
}
theme { bg-color 0x2a6fd6ff; fg-color 0xffffffff; focus-bg-color 0x2a6fd6ff; focus-fg-color 0xffffffff }
input { keyboard { xkb { layout "us,de" } } }
output $HL_MON { ${scale1}${rr1}x 0; y 0; width $HL_WIDTH; height $HL_HEIGHT; refresh 60 }
$secondary_output
layout {
	titlebar { enable 1 }
	scroller { preset 0.3,0.5,0.8 }
}
dwindle_manual_split 1
mousebind SUPER,BTN_LEFT,move_resize,curmove
mousebind SUPER,BTN_RIGHT,move_resize,curresize
binds {
	F11 { combo_view 2; }
	F12 { combo_view 3; }
}
tag 1 { layout tile; name t1 }
tag 2 { layout tile; name t2 }
tag 3 { layout tile; name t3 }
tag 4 { layout scroller; name t4 }
$extra
EOF

	# Own state dir, so a test instance's log does not append to the live
	# session's -- they share HOME (deliberately: fonts, icons, the user's
	# themes) and the log used to be keyed on HOME alone.
	HL_STATE="$HL_OUTDIR/state"
	rm -rf "$HL_STATE"; mkdir -p "$HL_STATE"
	HL_XDG="$HL_OUTDIR/xdg"
	# Before the rm, not only in hl_stop. A run killed with ^C or timed out
	# never reaches its trap, so the mounts from that one are still there --
	# and `rm -rf` over a live mount fails with EBUSY, which aborts the next
	# run with "no IPC socket" and no hint about why.
	hl_unmount_xdg
	rm -rf "$HL_XDG"; mkdir -p "$HL_XDG"; chmod 700 "$HL_XDG"

	# A session bus of its very own.
	#
	# The compositor's portal backends (global shortcuts, inhibit) live on
	# D-Bus, so without one they are simply not there to test. The real user
	# bus is the wrong answer twice over: the live session already owns
	# org.freedesktop.impl.portal.desktop.asteroidz, so a test instance could
	# never take the name -- and if it somehow did, it would be answering
	# Discord's push-to-talk and mpv's inhibit requests from a headless
	# compositor with no screen. A private daemon is isolated by construction.
	#
	# Optional: nothing outside the portal modules needs it, and the rest of
	# the suite must still run on a machine without dbus-daemon.
	hl_dbus_stop
	HL_DBUS_ADDR=""
	if command -v dbus-daemon >/dev/null; then
		exec 9>"$HL_OUTDIR/dbus.pid"
		HL_DBUS_ADDR="$(dbus-daemon --session --fork --print-address --print-pid=9 2>/dev/null)"
		exec 9>&-
		HL_DBUS_PID="$(cat "$HL_OUTDIR/dbus.pid" 2>/dev/null)"
		[ -n "$HL_DBUS_ADDR" ] || echo "hl_start: dbus-daemon failed; portal tests will skip" >&2
	fi

	# shellcheck disable=SC2086 -- HL_ENV is intentionally word-split
	env -i HOME="$HOME" PATH="$PATH" XDG_RUNTIME_DIR="$HL_XDG" \
		XDG_STATE_HOME="$HL_STATE" \
		${HL_DBUS_ADDR:+DBUS_SESSION_BUS_ADDRESS="$HL_DBUS_ADDR"} \
		WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
		WLR_HEADLESS_OUTPUTS="$HL_OUTPUTS" \
		WLR_RENDERER="${HL_RENDERER:-gles2}" \
		${HL_ENV:-} \
		"$HL_ASTEROIDZ" -c "$HL_CONFIG" > "$HL_OUTDIR/comp-stdout.log" 2>&1 &
	HL_COMP_PID=$!

	HL_SOCK=""
	local i
	for i in $(seq 1 50); do
		sleep 0.2
		HL_SOCK="$(ls "$HL_XDG"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1 | xargs -r basename)"
		[ -n "$HL_SOCK" ] && break
	done
	if [ -z "$HL_SOCK" ]; then
		echo "hl_start: compositor did not create a socket; see $HL_OUTDIR/comp-stdout.log" >&2
		kill "$HL_COMP_PID" 2>/dev/null
		exit 1
	fi
	export XDG_RUNTIME_DIR="$HL_XDG" WAYLAND_DISPLAY="$HL_SOCK"
	HL_SIG="$(ls "$HL_XDG"/asteroidz-*.sock 2>/dev/null | head -1)"
	HL_BASELINE_CLIENTS=0 # fresh isolated instance, always starts at zero

	# flat mid-grey wallpaper: plenty of contrast for shadow/blur checks
	# without needing per-test image generation, and easy to spot rendering
	# artifacts against (a genuinely neutral grey, not a tinted color --
	# #808080 was previously mislabeled "grey" while actually being a muted
	# blue, #3a5a7a). swaybg not mpvpaper (a continuously-updating video
	# wallpaper silently defeats shadow rendering -- verified headlessly,
	# see the shadow_blur commit history).
	HL_WALLPAPER="$HL_OUTDIR/wallpaper.png"
	[ -f "$HL_WALLPAPER" ] || convert -size "${HL_WIDTH}x${HL_HEIGHT}" xc:'#808080' "$HL_WALLPAPER" 2>/dev/null
	swaybg -o '*' -i "$HL_WALLPAPER" -m fill > "$HL_OUTDIR/swaybg.log" 2>&1 &
	HL_SWAYBG_PID=$!
	sleep 0.5

	# The virtual pointer's coordinate space is the LAYOUT bounding box, not
	# one monitor. Until this ran, HL_PTR_EXTENT_W/H kept their defaults of
	# HL_WIDTH/HL_HEIGHT -- so on a two-output layout every hl_move past the
	# first output's width was scaled into the first output and landed
	# nowhere near its target. The function existed and nothing called it,
	# which is worse than not having it: `hl_move 2880 555` looked like it
	# aimed at the second monitor and silently did not, so a test could drive
	# a window that was never under the pointer and report a pass.
	hl_sync_pointer_extent || echo "hl_start: could not read the layout extent; pointer coordinates will be wrong on a multi-output layout" >&2
}

# hl_start_live — attach to the CALLER'S OWN already-running compositor
# instead of launching an isolated one. Uses the existing
# ASTEROIDZ_INSTANCE_SIGNATURE from the environment (must already be valid --
# this deliberately does NOT scan for a socket the way waybar-asteroidz-
# workspaces' resolve_socket() does, since blind newest-mtime scanning is
# exactly the bug that hijacked that plugin the first time this was tried:
# a second/test instance's newer socket outranked the real one).
#
# HL_LIVE_MON, if set, names a REAL, physically-connected output (e.g.
# DP-1/HDMI-A-1) to run every test against directly -- this WILL move,
# focus, spawn on top of, and otherwise disturb whatever is really on that
# screen (tag/view changes, test windows mixed with real ones, HDR/SDR
# luminance flips, the works). Unset (the default), this creates a fresh
# virtual/headless output via create_virtual_output instead and confines
# everything there, leaving every real output untouched -- the safer but
# NOT what "test on my real setup" means; only use HL_LIVE_MON when the
# user has explicitly asked for real-monitor disruption, not just live
# process attachment.
hl_start_live() {
	if [ -z "${ASTEROIDZ_INSTANCE_SIGNATURE:-}" ] || [ ! -S "$ASTEROIDZ_INSTANCE_SIGNATURE" ]; then
		echo "hl_start_live: ASTEROIDZ_INSTANCE_SIGNATURE is not set to a valid socket in this shell" >&2
		exit 1
	fi
	HL_SIG="$ASTEROIDZ_INSTANCE_SIGNATURE"
	HL_LIVE_MODE=1
	HL_OUTDIR="${HL_OUTDIR:-/tmp/asteroidz-hl-live-$$}"
	mkdir -p "$HL_OUTDIR"

	# get all-clients is global, not scoped to any one monitor/tag -- a live
	# session normally has real windows open already (confirmed live
	# 2026-07-20: assertions written for a clean count like "1" or "0" saw
	# the user's actual open windows instead and failed for the wrong
	# reason). Captured once, right here, before any test window exists;
	# hl_wait_client_count below adds this back so tests can keep asserting
	# small, human-readable counts without needing to know it's a live
	# session or query the user's real windows themselves.
	HL_BASELINE_CLIENTS="$(hl_get "get all-clients" | jq '.clients | length')"

	if [ -n "${HL_LIVE_MON:-}" ]; then
		local real_names
		real_names="$(hl_get "get all-monitors" | jq -r '[.monitors[].name] | join(",")')"
		case ",$real_names," in
			*",$HL_LIVE_MON,"*) ;;
			*)
				echo "hl_start_live: HL_LIVE_MON=$HL_LIVE_MON is not a currently attached monitor (have: $real_names)" >&2
				exit 1
				;;
		esac
		HL_MON="$HL_LIVE_MON"
		HL_LIVE_REAL_MON=1
		# HL_WIDTH/HL_HEIGHT default to the synthetic headless size (1920x1080)
		# and are used verbatim as wlvptr's absolute-pointer extent (see
		# contrib/wlvptr/wlvptr.c) and as the "output center" for geometry
		# assertions -- both silently wrong on a real monitor of a different
		# size. Confirmed live 2026-07-20: on a 3840x2160 real monitor,
		# center_window's target was computed from the WRONG (1920x1080)
		# center, and any wlvptr-based click/drag would land at roughly half
		# the intended real pixel position (motion_absolute is a fraction of
		# the given extent, so extent/2 off scales every coordinate by 2x).
		local real_w real_h
		real_w="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .width")"
		real_h="$(hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .height")"
		if [ -n "$real_w" ] && [ -n "$real_h" ] && [ "$real_w" != "null" ] && [ "$real_h" != "null" ]; then
			HL_WIDTH="$real_w"
			HL_HEIGHT="$real_h"
		fi
		# wlr-virtual-pointer's motion_absolute is a fraction not of the
		# TARGET monitor's own size but of the whole wlr_output_layout's
		# bounding box across every connected output (confirmed live
		# 2026-07-20: on a DP-1 + HDMI-A-1 setup, asking for x=200 out of a
		# 3840-wide extent landed the real cursor at x=333 -- the compositor
		# normalized against the 6400px-wide two-monitor layout instead).
		# Using HL_WIDTH/HL_HEIGHT (the target monitor's OWN size) as the
		# wlvptr extent silently overshoots any multi-monitor live session --
		# harmless when the target point is near a window's middle (still
		# lands inside it by luck), but a grab point near an edge (e.g. a
		# resize test's bottom-right corner, 5px inset) misses the window
		# entirely. HL_PTR_EXTENT_W/H is the true layout bounding box, used
		# only by the wlvptr-driving helpers below; HL_WIDTH/HL_HEIGHT keep
		# meaning "the target monitor's own size" for geometry math (e.g.
		# center_window's expected output-center) elsewhere -- correct as
		# long as HL_MON sits at layout offset (0,0), true for this setup,
		# not asserted in general.
		local layout_w layout_h
		layout_w="$(hl_get "get all-monitors" | jq '[.monitors[] | .x + .width] | max')"
		layout_h="$(hl_get "get all-monitors" | jq '[.monitors[] | .y + .height] | max')"
		if [ -n "$layout_w" ] && [ -n "$layout_h" ] && [ "$layout_w" != "null" ] && [ "$layout_h" != "null" ]; then
			HL_PTR_EXTENT_W="$layout_w"
			HL_PTR_EXTENT_H="$layout_h"
		fi
		echo "hl_start_live: attached to live session, testing DIRECTLY against real monitor $HL_MON (${HL_WIDTH}x${HL_HEIGHT}, pointer layout extent ${HL_PTR_EXTENT_W}x${HL_PTR_EXTENT_H})" >&2
		hl_notify "asteroidz live regression: running on $HL_MON" "Testing your REAL monitor directly -- expect window/tag churn on that screen for the duration."
		return
	fi

	hl_notify "asteroidz live regression: starting" "Attaching to your running compositor -- creating a virtual monitor now."

	local before after new
	before="$(hl_get "get all-monitors" | jq -r '[.monitors[].name] | sort | join(",")')"
	hl_dispatch "create_virtual_output" 1
	after="$(hl_get "get all-monitors" | jq -r '[.monitors[].name] | sort | join(",")')"
	new="$(comm -13 <(tr ',' '\n' <<<"$before" | sort) <(tr ',' '\n' <<<"$after" | sort) | head -1)"
	if [ -z "$new" ]; then
		echo "hl_start_live: create_virtual_output didn't add a new monitor (before=$before after=$after)" >&2
		hl_notify "asteroidz live regression: FAILED to start" "create_virtual_output didn't produce a new monitor -- aborting."
		exit 1
	fi
	HL_MON="$new"
	echo "hl_start_live: attached to live session, testing against virtual monitor $HL_MON" >&2
	hl_notify "asteroidz live regression: running" "Confined to virtual monitor $HL_MON. This is still your real compositor process -- a crash there affects your whole desktop."
}

# hl_notify SUMMARY [BODY] -- desktop notification via notify-send, LIVE MODE
# ONLY (a no-op in plain headless runs, and if notify-send isn't installed).
# The point is to keep the user able to see, in real time, what a live-attach
# run is doing to their own compositor process -- see feedback memory on the
# 2026-07-19 live-mode segfault incident for why this exists.
hl_notify() {
	[ "${HL_LIVE_MODE:-0}" = "1" ] || return 0
	command -v notify-send >/dev/null 2>&1 || return 0
	notify-send -a "asteroidz regression" -- "$1" "${2:-}" 2>/dev/null
}

# Unmount the fuse filesystems the portals mount inside our XDG_RUNTIME_DIR.
#
# Anything that starts xdg-document-portal or gvfsd under our runtime dir leaves
# `doc` and `gvfs` mounted at $HL_XDG when the test ends -- and a mount holds its
# directory, so the next run's `rm -rf "$HL_XDG"` fails, the run aborts with no
# IPC socket, and the mount survives to do it again. They accumulate: this landed
# with 136 of them on one machine, 68 runs' worth, each still holding a
# gvfsd-fuse and a fusermount3 process.
#
# SCOPE IS THE WHOLE SAFETY ARGUMENT HERE. The user's own session has
# /run/user/1000/doc and /run/user/1000/gvfs, which are the same two filesystems
# from the same two programs and are indistinguishable in /proc/mounts except by
# path. Unmounting those takes out their file dialogs and every gvfs mount they
# have open. So this only ever touches paths that are a prefix match under
# $HL_XDG, and refuses outright if that is empty or looks like a real runtime
# dir -- rather than matching on `fuse.portal`, which would find both.
hl_unmount_xdg() {
	local base="${HL_XDG:-}"
	[ -n "$base" ] || return 0
	case "$base" in
		/run/user/* | / | /tmp | "") return 0 ;;
	esac
	local m
	# Deepest first: a nested mount holds its parent busy, so unmounting the
	# outer one first fails and leaves both.
	while read -r m; do
		[ -n "$m" ] || continue
		fusermount3 -u "$m" 2>/dev/null \
			|| fusermount3 -uz "$m" 2>/dev/null \
			|| umount -l "$m" 2>/dev/null
	done < <(awk -v b="$base/" 'index($2, b) == 1 { print $2 }' /proc/mounts \
			| sort -r)
}

# hl_restart -- bring up a fresh compositor after a test deliberately exited
# the shared one.
#
# Exactly one test does that (quit-confirm's "Enter exits", which cannot be
# written any other way), and every module alphabetically after it used to fail
# with an empty reply because the instance they all share was gone -- 60-odd
# assertions reported as broken code by a run that had simply lost its
# compositor. Refuses in live mode, where the compositor is the user's own and
# is not ours to restart.
#
# The new instance gets hl_start's PRISTINE config: a module that appended its
# own binds must re-append them after calling this.
hl_restart() {
	if [ "${HL_LIVE_MODE:-0}" = "1" ]; then
		echo "hl_restart: refusing in live mode" >&2
		return 1
	fi
	for pid in "${HL_SPAWNED_PIDS[@]:-}"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null; done
	HL_SPAWNED_PIDS=()
	[ -n "${HL_SWAYBG_PID:-}" ] && kill "$HL_SWAYBG_PID" 2>/dev/null
	if [ -n "${HL_COMP_PID:-}" ]; then
		kill "$HL_COMP_PID" 2>/dev/null
		wait "$HL_COMP_PID" 2>/dev/null
	fi
	hl_start
}

hl_stop() {
	for pid in "${HL_SPAWNED_PIDS[@]:-}"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null; done
	if [ "${HL_LIVE_MODE:-0}" = "1" ]; then
		# never kill the caller's own live compositor. In real-monitor mode
		# there's no virtual output to remove (HL_MON IS the real output);
		# destroy_all_virtual_output only ever targets wlr_output_is_headless()
		# outputs regardless, so it's a harmless no-op there either way.
		if [ "${HL_LIVE_REAL_MON:-0}" = "1" ]; then
			hl_notify "asteroidz live regression: finished" "Done testing $HL_MON."
		else
			hl_notify "asteroidz live regression: finished" "Cleaning up the virtual monitor."
			hl_dispatch "destroy_all_virtual_output" 0.5
		fi
		return
	fi
	[ -n "${HL_SWAYBG_PID:-}" ] && kill "$HL_SWAYBG_PID" 2>/dev/null
	[ -n "${HL_COMP_PID:-}" ] && kill "$HL_COMP_PID" 2>/dev/null
	# Bounded, and loud about it. This used to be a bare `wait`, which is
	# correct exactly as long as the compositor honours SIGTERM -- and when it
	# stopped doing so (handlesig() raised the exit-confirmation prompt instead
	# of exiting, so a signal waited for a keystroke nobody was there to press)
	# every single run hung until its outer timeout and left its compositor
	# alive. Twenty-eight of them accumulated before anyone noticed, because a
	# run that has already printed its summary looks finished.
	if [ -n "${HL_COMP_PID:-}" ]; then
		local waited=0
		while kill -0 "$HL_COMP_PID" 2>/dev/null && [ "$waited" -lt 50 ]; do
			sleep 0.1
			waited=$((waited + 1))
		done
		if kill -0 "$HL_COMP_PID" 2>/dev/null; then
			echo "hl_stop: the compositor ignored SIGTERM for 5s -- SIGKILLing it." >&2
			echo "hl_stop: that is a COMPOSITOR bug, not a harness one: a signal must exit." >&2
			kill -9 "$HL_COMP_PID" 2>/dev/null
		fi
		wait "$HL_COMP_PID" 2>/dev/null
	fi
	# After the compositor is down, not before: a client still holding a file
	# under the mount keeps it busy and turns the unmount into a lazy one.
	hl_unmount_xdg
	hl_dbus_stop
}

# kill test-spawned windows and return to a known-clean state, WITHOUT
# tearing down the compositor -- call between independent test cases that
# share one instance so they can't leak state into each other.
hl_reset() {
	for pid in "${HL_SPAWNED_PIDS[@]:-}"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null; done
	HL_SPAWNED_PIDS=()
	sleep 0.3
	# a multi-monitor test (contrib/regression/tests/multimonitor.sh) can
	# leave a second output created AND focused -- every other module
	# assumes $HL_MON is selmon (dispatch with no explicit monitor target
	# always acts on selmon), so refocus it unconditionally. Harmless/no-op
	# when there's only ever been one monitor.
	hl_dispatch "focus_monitor,$HL_MON" 0.1
	if [ "${HL_LIVE_REAL_MON:-0}" = "1" ]; then
		# Real-monitor live mode: do NOT force view/set_layout here. Forcing
		# tag 1 + tile on a real output re-arranges whatever real windows are
		# actually there -- with animations off (as most modules set at some
		# point) that re-arrange, any per-window shadow/titlebar re-render it
		# triggers, and a freshly-mapped test window's own first render all
		# have to happen INSTANTLY, in one shot, instead of spread over the
		# ~200-300ms an animated transition would normally take. On a real
		# HDR/high-refresh output that's a burst of synchronous GPU work far
		# outside anything a human doing the same layout switch by hand ever
		# produces (they always have animations on), and is suspected to be
		# what froze the whole display in a 2026-07-19 incident. Tests that
		# assume a controlled tag-1/tile baseline are unreliable in this mode
		# as a result -- real-monitor mode is for IPC/responsiveness-style
		# checks, not baseline-dependent assertions.
		sleep 0.2
		return
	fi
	hl_dispatch "view,1" 0.1
	hl_dispatch "set_layout,tile" 0.1
	sleep 0.2
}

# ─── IPC ──────────────────────────────────────────────────────────────────

hl_dispatch() { # hl_dispatch "func,arg1,arg2" [settle_seconds]
	# Live mode never disables animations, regardless of what a test asks
	# for. A real user always has animations on -- disabling them is purely
	# a headless determinism trick (it makes geometry settle instantly so a
	# dispatch-then-poll assertion doesn't need to wait out an animation).
	# Applied to a real, busy monitor, that same "instant" settling forces
	# a burst of synchronous shadow/titlebar/geometry work for every
	# affected window into a single frame instead of spreading it over the
	# ~200-300ms an animated transition normally takes -- suspected cause of
	# a full display freeze on 2026-07-19. So: mimic real usage instead of
	# the synthetic-instance shortcut once we're live, and just skip the
	# dispatch (the test's own responsiveness assertions still run against
	# whatever config.animations already was).
	if [ "${HL_LIVE_MODE:-0}" = "1" ]; then
		case "$1" in
			set_option,animations,0|set_option,layer_animations,0)
				echo "hl_dispatch: skipping '$1' in live mode (animations stay on)" >&2
				sleep "${2:-0.3}"
				return
				;;
		esac
	fi
	ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg dispatch "$1" >/dev/null 2>&1
	sleep "${2:-0.3}"
}

hl_get() { # hl_get "get all-clients" -> raw JSON on stdout
	ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg $1 2>/dev/null
}

hl_watch_start() { # hl_watch_start "watch monitor HEADLESS-1" LOGNAME -> pid (tracked for hl_reset/hl_stop)
	ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg $1 > "$HL_OUTDIR/$2.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	sleep 0.3   # let the subscribe land before the caller triggers a change
	echo "$pid"
}
hl_watch_line_count() { wc -l < "$HL_OUTDIR/$1.log" 2>/dev/null || echo 0; }

# hl_wait_watch_grew LOGNAME BEFORE_COUNT [timeout_tenths=20] -- poll until a
# watch stream's line count exceeds BEFORE_COUNT instead of a fixed sleep
# then single check. hl_watch_start's own subscribe-settle sleep (0.3s) is
# a reasonable default but not a hard guarantee under a busier live session
# (more background IPC traffic, more real windows) -- confirmed live
# 2026-07-20: a manual reproduction with a 0.5s gap between subscribe and
# dispatch worked reliably every time, while the harness's 0.3s sometimes
# missed it. Polling removes the exact margin as a variable; a genuinely
# broken notification still times out and fails same as before.
hl_wait_watch_grew() {
	local logname="$1" before="$2" timeout="${3:-20}" i n
	for i in $(seq 1 "$timeout"); do
		n="$(hl_watch_line_count "$logname")"
		[ "$n" -gt "$before" ] && return 0
		sleep 0.1
	done
	return 1
}

# hl_move X Y -- move the pointer without pressing anything, which is what
# hover behaviour (tooltips) needs and what hl_click cannot express.
# Recompute the virtual pointer's coordinate space from the CURRENT layout.
#
# zwlr_virtual_pointer_v1.motion_absolute maps x/extent_w onto the layout's
# bounding box, so an extent that no longer matches the layout scales every
# coordinate. hl_start sets it from the single output it created, and cannot know
# what a test is about to add -- hl_start_live already does this for the real
# session for exactly the same reason.
#
# Any test that creates, destroys or repositions an output must call this
# afterwards. It went unnoticed for a long time because multimonitor.sh places
# its second output BESIDE the first, which leaves the layout height alone.
# Stacking one BELOW doubled the layout height, so a click aimed at a bar pill
# 33px down was delivered at 66px -- below the bar entirely -- and the test
# failed as "the panel did not open", which points nowhere near the pointer.
hl_sync_pointer_extent() {
	local dims
	dims="$(hl_get "get all-monitors" | python3 -c '
import json, sys
try:
    ms = json.load(sys.stdin)["monitors"]
except Exception:
    raise SystemExit(1)
if not ms:
    raise SystemExit(1)
x0 = min(m["x"] for m in ms); y0 = min(m["y"] for m in ms)
x1 = max(m["x"] + m["width"] for m in ms)
y1 = max(m["y"] + m["height"] for m in ms)
print(max(1, x1 - x0), max(1, y1 - y0))
' 2>/dev/null)"
	[ -n "$dims" ] || return 1
	read -r HL_PTR_EXTENT_W HL_PTR_EXTENT_H <<<"$dims"
	return 0
}

hl_move() { # hl_move X Y
	"$HL_WLVPTR" "$1" "$2" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H"
}

hl_click() { # hl_click X Y [click|rclick|mclick]
	"$HL_WLVPTR" "$1" "$2" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H" "${3:-click}"
}

# hl_wheel X Y N -- scroll N discrete notches at (X,Y), positive = down. This
# is what a real mouse wheel sends: a continuous delta AND a notch count.
hl_wheel() { # hl_wheel X Y N
	"$HL_WLVPTR" "$1" "$2" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H" "wheel:${3:-1}"
}

# hl_scroll X Y AMT -- continuous scroll only, no notch, which is what a
# TRACKPAD sends. Kept distinct from hl_wheel because handlers that key off
# the discrete count alone work under one and silently do nothing under the
# other.
hl_scroll() { # hl_scroll X Y AMT
	"$HL_WLVPTR" "$1" "$2" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H" "scroll:${3:-1}"
}

# hl_super_drag X1 Y1 X2 Y2 -- press Super, left-drag from (X1,Y1) to
# (X2,Y2), release Super. For testing a real Super+drag mouse binding (not
# an IPC dispatch) -- needs hl_start's own test config to actually bind one
# (mousebind SUPER,BTN_LEFT,move_resize,curmove), a compositor default
# can't be assumed.
# A plain press-move-release, no modifier. hl_super_drag is for the
# compositor's own Super+drag bindings; this is for dragging something inside a
# client, which is a different gesture entirely -- holding Super would move the
# WINDOW instead.
hl_drag() { # hl_drag <from-x> <from-y> <to-x> <to-y>
	# Move FIRST, as its own call. wlvptr presses wherever the pointer already
	# is -- the x,y it takes only seeds the interpolation for the motion that
	# follows -- so a drag issued without positioning the pointer starts from
	# wherever the last test left it, and every event lands on some other
	# widget. hl_click gets away with it because callers here always hl_move
	# first; this reads as one gesture, so it does its own.
	"$HL_WLVPTR" "$1" "$2" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H"
	sleep 0.3
	"$HL_WLVPTR" "$1" "$2" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H" "drag:$3,$4"
}

hl_super_drag() {
	"$HL_WLVKBD" hold LEFTMETA -- "$HL_WLVPTR" "$1" "$2" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H" "drag:$3,$4"
}
# hl_super_drag_hold X1 Y1 X2 Y2 MS -- drag and WAIT with the button still
# down. Backgrounded by the caller, which then captures the held state before
# the release lands. The only way to ask whether a decoration changes when the
# button comes up while the geometry does not.
hl_super_rdrag_hold() {
	"$HL_WLVKBD" hold LEFTMETA -- "$HL_WLVPTR" "$1" "$2" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H" "rdraghold:$3,$4,$5"
}
hl_super_drag_hold() {
	"$HL_WLVKBD" hold LEFTMETA -- "$HL_WLVPTR" "$1" "$2" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H" "draghold:$3,$4,$5"
}
# hl_super_rdrag X1 Y1 X2 Y2 -- same, right button (resize binding).
hl_super_rdrag() {
	"$HL_WLVKBD" hold LEFTMETA -- "$HL_WLVPTR" "$1" "$2" "$HL_PTR_EXTENT_W" "$HL_PTR_EXTENT_H" "rdrag:$3,$4"
}

# ─── test windows ─────────────────────────────────────────────────────────

# same palette as contrib/live-visual-tour.sh, so spawned test windows are
# visually distinct in a recording/screenshot -- cycles if more than 9 are
# spawned in one run.
HL_SPAWN_COLORS=(
	'#aa2222' # red
	'#22aa22' # green
	'#2222aa' # blue
	'#aaaa22' # yellow
	'#aa22aa' # magenta
	'#22aaaa' # cyan
	'#dd7700' # orange
	'#00bbbb' # teal
	'#bb00bb' # pink
)
HL_SPAWN_COLOR_IDX=0

# Restart the colour cycle. A fixture that runs TWICE in one script process --
# to compare two binaries, say -- gets colours 1,2,3 then 4,5,6 without this,
# and every window's background differs between the two captures. That is half
# a framebuffer's worth of difference caused entirely by the harness, and it
# reads exactly like a renderer regression.
hl_reset_spawn_colors() { HL_SPAWN_COLOR_IDX=0; }

hl_spawn_kitty() { # hl_spawn_kitty TITLE -> pid (also tracked for hl_reset/hl_stop)
	local title="$1"
	local color="${HL_SPAWN_COLORS[$((HL_SPAWN_COLOR_IDX % ${#HL_SPAWN_COLORS[@]}))]}"
	HL_SPAWN_COLOR_IDX=$((HL_SPAWN_COLOR_IDX + 1))
	# NOT --hold: a held (finished-process) window needs a keypress to
	# actually close even on a compositor-issued close request, which stalls
	# kill_client tests headlessly (no input device to dismiss it with) --
	# a real long-lived foreground process closes cleanly instead.
	# HL_KITTY_EXTRA appends further -o options. It exists for fixtures that
	# have to compare two CAPTURES of the same scene: a terminal's text cursor
	# blinks, so a screenshot of a settled desktop is not reproducible unless
	# `-o cursor_blink_interval=0` turns that off. Empty by default, because a
	# blinking cursor is what a real terminal does and most fixtures should see
	# it.
	# shellcheck disable=SC2086
	kitty --title "$title" -o background_opacity=1.0 -o "background=$color" \
		${HL_KITTY_EXTRA:-} \
		sh -c "echo $title; exec sleep 300" > "$HL_OUTDIR/kitty-$title.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	echo "$pid"
}

hl_spawn_wlkeys() { # hl_spawn_wlkeys APPID HOLD_S [LOGNAME] -> pid
	# Logs one line per keyboard event to $HL_OUTDIR/$logname.log -- the only
	# client here that reports what the compositor SENT it, which is the only
	# way to assert on wl_keyboard.enter's held-key array.
	local appid="$1" hold="$2" logname="${3:-wlkeys}"
	"$HL_WLKEYS" "$appid" "$hold" > "$HL_OUTDIR/$logname.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	echo "$pid"
}

# Matching lines in a log, as a bare number even when there are none.
# `grep -c` prints 0 AND exits 1 on no match, so the obvious `|| echo 0`
# emits "0\n0" and every comparison against it fails confusingly.
hl_count_lines() { # hl_count_lines PATTERN FILE
	local n
	n="$(grep -c "$1" "$2" 2>/dev/null || true)"
	echo "${n:-0}"
}

# The held-key array from the LAST wl_keyboard.enter this client was sent,
# comma-separated evdev codes ("" when it was told nothing is held).
hl_wlkeys_last_enter() { # hl_wlkeys_last_enter [LOGNAME]
	local logname="${1:-wlkeys}"
	grep '^enter keys=' "$HL_OUTDIR/$logname.log" 2>/dev/null | tail -1 |
		sed 's/^enter keys=//'
}

hl_spawn_wlstates() { # hl_spawn_wlstates APPID HOLD_S [LOGNAME] -> pid
	# Logs one line per xdg_toplevel.configure to $HL_OUTDIR/$logname.log with
	# the state array spelled out. Binds xdg_wm_base v6, so it is the only
	# client here that can be sent -- and can therefore observe -- `suspended`.
	local appid="$1" hold="$2" logname="${3:-wlstates}"
	"$HL_WLSTATES" "$appid" "$hold" > "$HL_OUTDIR/$logname.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	echo "$pid"
}

hl_spawn_wlbgeffect() { # hl_spawn_wlbgeffect APPID HOLD_S [LOGNAME] -> pid
	# A TRANSLUCENT toplevel that supplies a real ext-background-effect-v1 blur
	# region: TWO separated rectangles, so a consumer that preserved the region's
	# shape can be told from one that took its bounding box.
	#
	# Nothing else here sets one, and a plain window's blur node carries no clip
	# at all (`clipped_region_get_default()` is an empty box) -- so without this
	# client a "clip_region works" assertion is made against nodes that have no
	# clip. See contrib/wlbgeffect/wlbgeffect.c.
	#
	# A fourth argument is an ARGB hex colour. The transform pixel oracle needs
	# fixtures that are reproducible between two RUNS of the compositor, and a
	# terminal is not one: the same logical desktop at 0 and at 90 degrees left
	# 7183 differing pixels, every one of them inside a TEXT row, because the
	# client lays its glyphs out for the output it was told about. Flat
	# surfaces of distinct colours have nothing to lay out.
	#
	# BUT A FLAT SURFACE IS BLIND TO ORIENTATION. A solid colour rotated by any
	# amount is the same solid colour, so a fixture built out of flat windows
	# reports 0 differing pixels whether the sampler is oriented correctly or
	# not -- and that is exactly how a texture transform that was wrong at 90
	# and 270 degrees survived a pixel-exact eight-transform oracle, with a
	# third of the screen drawn upside down inside its own windows.
	#
	# WLBGEFFECT_QUAD=1 paints four quadrants instead: still no text, still no
	# client-side layout, still reproducible between runs, and able to tell all
	# eight orientations apart. contrib/avk-transform-test.sh sets it by
	# default. Anything comparing a rotated output should.
	local appid="$1" hold="$2" logname="${3:-wlbgeffect}" argb="${4:-}"
	# shellcheck disable=SC2086
	"$HL_WLBGEFFECT" "$appid" "$hold" $argb > "$HL_OUTDIR/$logname.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	echo "$pid"
}

# The state list from the LAST configure this client was sent ("-" when the
# compositor sent an empty array).
hl_wlstates_last() { # hl_wlstates_last [LOGNAME]
	local logname="${1:-wlstates}"
	grep '^configure ' "$HL_OUTDIR/$logname.log" 2>/dev/null | tail -1 |
		sed 's/^.* states=//'
}

# Interfaces a client connecting through security-context-v1 is shown, one per
# line. Runs to completion (no hold), so this blocks rather than backgrounding.
hl_sandbox_globals() { # hl_sandbox_globals -> interface names on stdout
	"$HL_WLSANDBOX" "hl-sandbox-$$" 2>"$HL_OUTDIR/wlsandbox.err" |
		sed -n 's/^sandboxed \([a-z_0-9]*\) .*/\1/p'
}

hl_spawn_wllayer() { # hl_spawn_wllayer LAYER ANCHOR EXCL_ZONE W H KB HOLD_S [RESIZE_SPEC] LOGNAME -> pid
	local layer="$1" anchor="$2" zone="$3" w="$4" h="$5" kb="$6" hold="$7" resize="$8" logname="$9"
	logname="${logname:-wllayer}"
	"$HL_WLLAYER" "$layer" "$anchor" "$zone" "$w" "$h" "$kb" "$hold" $resize \
		> "$HL_OUTDIR/$logname.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	echo "$pid"
}

# current client count with HL_BASELINE_CLIENTS (whatever was already open
# when this instance attached -- see hl_start_live) subtracted back out, so
# callers can keep asserting small, test-relative counts like "1" or "2"
# regardless of a live session's own pre-existing windows.
hl_client_count() { echo $(($(hl_get "get all-clients" | jq '.clients | length') - ${HL_BASELINE_CLIENTS:-0})); }

hl_wait_client_count() { # hl_wait_client_count N [timeout_tenths=30]
	local want="$1" timeout="${2:-30}" i n
	for i in $(seq 1 "$timeout"); do
		n="$(hl_client_count)"
		[ "$n" = "$want" ] && return 0
		sleep 0.1
	done
	return 1
}

# hl_client_field TITLE FIELD -- looks up a field on the specific client with
# that title, never positional (.clients[0] picks whatever's FIRST in the
# array -- in live mode that's just as likely to be a real pre-existing
# window as the test's own spawned one). Centralizes what several test files
# used to define locally and identically.
hl_client_field() { hl_get "get all-clients" | jq -r ".clients[] | select(.title==\"$1\") | .$2"; }

# the currently active tag's index on $HL_MON -- for reading per-tag state
# (like the layout symbol) without assuming tag 1 is active, which only
# holds in a fresh isolated instance. In live mode the active tag can be
# anything, including leftover state from a previous test module (real-
# monitor mode deliberately doesn't force view/layout between modules --
# see hl_reset -- to avoid a synchronous-redraw freeze risk).
hl_current_tag_index() { hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$HL_MON\") | .active_tags[0]"; }

# some config options (dwindle_manual_split, scroller_proportion_preset, ...)
# have a runtime SETTER (set_option) but no IPC GETTER at all -- there's no
# way to read the live session's actual current value before overriding it,
# so a test that sets one and "restores" to a hardcoded literal at the end
# risks silently and permanently changing the user's real config to the
# wrong value if their real value ever differed from that literal (this
# already happened once this session: scroller.sh's set_proportion_absolute
# test flips scroller_ignore_proportion_single 1->0 assuming 0 is the
# default, but the compile-time default is actually 1). Tests that need one
# of these should skip in live mode via this guard instead of forcing it.
hl_skip_if_live_unrestorable_option() { # hl_skip_if_live_unrestorable_option TEST_NAME OPTION_NAME
	if [ "${HL_LIVE_MODE:-0}" = "1" ]; then
		hl_skip "$1: needs $2 set to a specific value, which has no IPC getter to safely restore afterward in live mode -- skipping rather than risk permanently changing your real config"
		return 1
	fi
	return 0
}

# hl_wait_field_eq TITLE FIELD EXPECTED [timeout_tenths=20] -- poll until a
# client's field reaches an expected value instead of a fixed sleep. Live
# mode never disables animations (see hl_dispatch), so a move/resize/center
# dispatch settles over a real ~200-300ms animated transition rather than
# instantly -- a fixed short sleep can read the position mid-animation and
# see the pre-dispatch value, which looks identical to "the dispatch had no
# effect" from the assertion's point of view. Polling for the actual target
# tells those two cases apart and is a no-op cost in headless mode (returns
# on the very first successful poll there, since geometry settles instantly
# with animations off).
hl_wait_field_eq() {
	local title="$1" field="$2" want="$3" timeout="${4:-20}" i got
	for i in $(seq 1 "$timeout"); do
		got="$(hl_client_field "$title" "$field")"
		[ "$got" = "$want" ] && return 0
		sleep 0.1
	done
	return 1
}

hl_screenshot() { grim "$HL_OUTDIR/$1.png" 2>/dev/null; }
# ONE OUTPUT, by name. grim with no -o captures the whole LAYOUT and resamples
# every output into one image at a single scale -- which silently destroys the
# thing a mixed-scale fixture is trying to measure, and puts a resampling filter
# between the test and the pixels the compositor actually presented. A seam test
# must compare each output's own buffer.
hl_screenshot_output() { grim -o "$1" "$HL_OUTDIR/$2.png" 2>/dev/null; }

hl_focused_title() { hl_get "get focused-client" | jq -r .title; }

# kill_client has no by-ID targeting over IPC -- bind_define.h's dispatch
# always operates on `selmon->sel` (arg->tc is never set by a bare dispatch
# string), so it force-kills WHATEVER is currently focused, not necessarily
# the test's own spawned window. Confirmed live 2026-07-20: a test that
# assumed its just-spawned window had focus instead killed the user's real
# tmux-hosting kitty terminal when that assumption didn't hold. Always route
# kill_client,force through this instead of dispatching it directly.
hl_kill_focused_or_skip() { # hl_kill_focused_or_skip EXPECTED_TITLE DESC
	local expected="$1" desc="$2" got
	got="$(hl_focused_title)"
	if [ "$got" != "$expected" ]; then
		hl_skip "$desc: focused client is '$got', not '$expected' -- refusing to force-kill an unexpected window"
		return 1
	fi
	hl_dispatch "kill_client,force" 0.2
	return 0
}

# ─── assertions (TAP-ish: "ok"/"FAIL" lines, tallied globally) ────────────

hl_assert() { # hl_assert "description" "$actual" "$expected"
	local desc="$1" actual="$2" expected="$3"
	ASSERT_COUNT=$((ASSERT_COUNT + 1))
	if [ "$actual" = "$expected" ]; then
		ASSERT_PASS=$((ASSERT_PASS + 1))
		echo "  ok - $desc"
	else
		ASSERT_FAILURES+=("$CURRENT_TEST: $desc (got '$actual', want '$expected')")
		echo "  FAIL - $desc (got '$actual', want '$expected')"
	fi
}
hl_assert_eq() { hl_assert "$1" "$2" "$3"; }
hl_assert_true() { hl_assert "$1" "$2" "true"; }
hl_assert_false() { hl_assert "$1" "$2" "false"; }

# For tests whose PRECONDITION isn't met by the current instance topology
# (e.g. a multi-monitor test running against the default single-output
# config) -- doesn't count as pass or fail, just notes why it didn't run.
hl_skip() { echo "  skip - $1"; }
hl_monitor_count() { hl_get "get all-monitors" | jq '.monitors | length'; }

hl_summary() { # prints totals, returns 1 if anything failed
	echo "----"
	echo "$ASSERT_PASS/$ASSERT_COUNT assertions passed"
	if [ "${#ASSERT_FAILURES[@]}" -gt 0 ]; then
		echo "failures:"
		printf '  %s\n' "${ASSERT_FAILURES[@]}"
		hl_notify "asteroidz live regression: $ASSERT_PASS/$ASSERT_COUNT passed" "${#ASSERT_FAILURES[@]} failure(s) -- see terminal output."
		return 1
	fi
	hl_notify "asteroidz live regression: $ASSERT_PASS/$ASSERT_COUNT passed" "All assertions passed."
	return 0
}

# ─── screenshot analysis ──────────────────────────────────────────────────
#
# Assertions about what is actually ON SCREEN. Layout arithmetic can agree
# with itself and still be wrong -- the bar's spacing shipped wrong three
# times over on assertions that never looked at a pixel -- so these read the
# rendered frame back and measure it.
#
# All of them need python3 with PIL; a test using one should skip when it is
# missing rather than fail.

# Count pixels in a region that are not the harness's flat grey wallpaper.
# A popover panel covers it; nothing else in these tests does.
hl_region_ink() { # hl_region_ink PNG X0 Y0 X1 Y1
	python3 - "$@" <<'PY'
import sys
from PIL import Image
png, x0, y0, x1, y1 = sys.argv[1], *map(int, sys.argv[2:6])
im = Image.open(png).convert("RGB"); px = im.load(); W, H = im.size
x1, y1 = min(x1, W), min(y1, H)
n = 0
for x in range(x0, x1):
    for y in range(y0, y1):
        r, g, b = px[x, y]
        if abs(r - 0x80) + abs(g - 0x80) + abs(b - 0x80) > 30:
            n += 1
print(n)
PY
}

# Centre x of the rightmost run of non-wallpaper pixels in a horizontal band.
hl_rightmost_ink_x() { # hl_rightmost_ink_x PNG Y0 Y1
	python3 - "$@" <<'PY'
import sys
from PIL import Image
png, y0, y1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
im = Image.open(png).convert("RGB"); px = im.load(); W, H = im.size
y1 = min(y1, H)
runs, s = [], None
for x in range(W):
    hit = any(abs(px[x, y][0] - 0x80) + abs(px[x, y][1] - 0x80) +
              abs(px[x, y][2] - 0x80) > 30 for y in range(y0, y1))
    if hit and s is None:
        s = x
    elif not hit and s is not None:
        runs.append((s, x - 1)); s = None
if s is not None:
    runs.append((s, W - 1))
runs = [r for r in runs if r[1] - r[0] + 1 >= 3]
if runs:
    print((runs[-1][0] + runs[-1][1]) // 2)
PY
}

# Pixels differing between two screenshots inside a region. Used to assert a
# popover's CONTENT changed (scrolled, drilled in, gained a cursor) without
# needing to know what it says.
hl_region_diff() { # hl_region_diff PNG_A PNG_B X0 Y0 X1 Y1
	python3 - "$@" <<'PY'
import sys
from PIL import Image
a, b = Image.open(sys.argv[1]).convert("RGB"), Image.open(sys.argv[2]).convert("RGB")
x0, y0, x1, y1 = map(int, sys.argv[3:7])
pa, pb = a.load(), b.load()
W, H = a.size
x1, y1 = min(x1, W), min(y1, H)
n = 0
for x in range(x0, x1):
    for y in range(y0, y1):
        if pa[x, y] != pb[x, y]:
            n += 1
print(n)
PY
}

# Centres of the monitor tiles on the arrange canvas, within a horizontal band.
# The tiles are drawn in the theme colour; the popover behind them is near
# black and the desktop is grey, so a tile is whatever is neither. Sampled
# rather than counted: the tiles cover enough of the band to BE one of its most
# common colours.
hl_canvas_tiles() { # hl_canvas_tiles PNG Y0 Y1 -> "x1 x2 y"
	python3 - "$@" <<'PY'
import sys
from PIL import Image
png, y0, y1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
im = Image.open(png).convert("RGB"); px = im.load(); W, H = im.size
y1 = min(y1, H)
bg = [px[5, 5], px[W - 20, y1 - 4]]
def tile(p):
    return all(abs(p[0]-b[0]) + abs(p[1]-b[1]) + abs(p[2]-b[2]) > 60 for b in bg)
best = None
for y in range(y0, y1):
    runs, s = [], None
    for x in range(W):
        if tile(px[x, y]) and s is None:
            s = x
        elif not tile(px[x, y]) and s is not None:
            runs.append((s, x - 1)); s = None
    if s is not None:
        runs.append((s, W - 1))
    runs = [r for r in runs if r[1] - r[0] + 1 >= 20]
    span = sum(r[1] - r[0] for r in runs)
    if runs and (best is None or span > best[2]):
        best = (runs, y, span)
if best:
    runs, y, _ = best
    if len(runs) == 1:      # two tiles butted together read as one run
        a, b = runs[0]
        print(a + (b - a) // 4, b - (b - a) // 4, y)
    else:
        print((runs[0][0]+runs[0][1])//2, (runs[-1][0]+runs[-1][1])//2, y)
PY
}
