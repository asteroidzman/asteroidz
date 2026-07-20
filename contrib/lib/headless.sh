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
#   HL_EXTRA_KDL           extra config appended verbatim (e.g. a second
#                          `output HEADLESS-2 { ... }` block for
#                          multi-monitor tests)
set -u

HL_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HL_ASTEROIDZ="${ASTEROIDZ:-$HL_REPO/build/asteroidz}"
[ -x "$HL_ASTEROIDZ" ] || HL_ASTEROIDZ=/usr/bin/asteroidz
HL_WLVPTR="$HL_REPO/contrib/wlvptr/wlvptr"
HL_WLVKBD="$HL_REPO/contrib/wlvkbd/wlvkbd"
HL_WLLAYER="$HL_REPO/contrib/wllayer/wllayer"
HL_WIDTH="${HL_WIDTH:-1920}"
HL_HEIGHT="${HL_HEIGHT:-1080}"
HL_MON="${HL_MON:-HEADLESS-1}"   # the monitor every test targets; see hl_start_live

ASSERT_COUNT=0
ASSERT_PASS=0
declare -a ASSERT_FAILURES=()
declare -a HL_SPAWNED_PIDS=()
CURRENT_TEST="${CURRENT_TEST:-(no test)}"

# ─── lifecycle ────────────────────────────────────────────────────────────

hl_start() { # hl_start [EXTRA_KDL]
	HL_OUTDIR="${HL_OUTDIR:-/tmp/asteroidz-hl-$$}"
	mkdir -p "$HL_OUTDIR"
	local extra="${1:-${HL_EXTRA_KDL:-}}"

	for t in grim swaybg jq; do
		command -v "$t" >/dev/null || { echo "hl_start: missing tool: $t" >&2; exit 1; }
	done
	[ -x "$HL_ASTEROIDZ" ] || { echo "hl_start: no asteroidz binary at $HL_ASTEROIDZ" >&2; exit 1; }
	[ -x "$HL_WLVPTR" ] || { echo "hl_start: wlvptr not built -- run: cd contrib/wlvptr && make" >&2; exit 1; }
	[ -x "$HL_WLVKBD" ] || { echo "hl_start: wlvkbd not built -- run: cd contrib/wlvkbd && make" >&2; exit 1; }
	[ -x "$HL_WLLAYER" ] || { echo "hl_start: wllayer not built -- run: cd contrib/wllayer && make" >&2; exit 1; }

	HL_CONFIG="$HL_OUTDIR/config.kdl"
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
output $HL_MON { width $HL_WIDTH; height $HL_HEIGHT; refresh 60 }
layout {
	titlebar { enable 1; height 28 }
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

	HL_XDG="$HL_OUTDIR/xdg"
	rm -rf "$HL_XDG"; mkdir -p "$HL_XDG"; chmod 700 "$HL_XDG"

	env -i HOME="$HOME" PATH="$PATH" XDG_RUNTIME_DIR="$HL_XDG" \
		WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=gles2 \
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
		echo "hl_start_live: attached to live session, testing DIRECTLY against real monitor $HL_MON" >&2
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
	wait "${HL_COMP_PID:-}" 2>/dev/null
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

hl_click() { # hl_click X Y [click|rclick|mclick]
	"$HL_WLVPTR" "$1" "$2" "$HL_WIDTH" "$HL_HEIGHT" "${3:-click}"
}

# hl_super_drag X1 Y1 X2 Y2 -- press Super, left-drag from (X1,Y1) to
# (X2,Y2), release Super. For testing a real Super+drag mouse binding (not
# an IPC dispatch) -- needs hl_start's own test config to actually bind one
# (mousebind SUPER,BTN_LEFT,move_resize,curmove), a compositor default
# can't be assumed.
hl_super_drag() {
	"$HL_WLVKBD" hold LEFTMETA -- "$HL_WLVPTR" "$1" "$2" "$HL_WIDTH" "$HL_HEIGHT" "drag:$3,$4"
}
# hl_super_rdrag X1 Y1 X2 Y2 -- same, right button (resize binding).
hl_super_rdrag() {
	"$HL_WLVKBD" hold LEFTMETA -- "$HL_WLVPTR" "$1" "$2" "$HL_WIDTH" "$HL_HEIGHT" "rdrag:$3,$4"
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

hl_spawn_kitty() { # hl_spawn_kitty TITLE -> pid (also tracked for hl_reset/hl_stop)
	local title="$1"
	local color="${HL_SPAWN_COLORS[$((HL_SPAWN_COLOR_IDX % ${#HL_SPAWN_COLORS[@]}))]}"
	HL_SPAWN_COLOR_IDX=$((HL_SPAWN_COLOR_IDX + 1))
	# NOT --hold: a held (finished-process) window needs a keypress to
	# actually close even on a compositor-issued close request, which stalls
	# kill_client tests headlessly (no input device to dismiss it with) --
	# a real long-lived foreground process closes cleanly instead.
	kitty --title "$title" -o background_opacity=1.0 -o "background=$color" \
		sh -c "echo $title; exec sleep 300" > "$HL_OUTDIR/kitty-$title.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	echo "$pid"
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
