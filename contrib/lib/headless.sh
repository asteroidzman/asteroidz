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
HL_WIDTH="${HL_WIDTH:-1920}"
HL_HEIGHT="${HL_HEIGHT:-1080}"

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
output HEADLESS-1 { width $HL_WIDTH; height $HL_HEIGHT; refresh 60 }
layout {
	titlebar { enable 1; height 28 }
	scroller { preset 0.3,0.5,0.8 }
}
dwindle_manual_split 1
mousebind SUPER,BTN_LEFT,move_resize,curmove
mousebind SUPER,BTN_RIGHT,move_resize,curresize
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

	# flat mid-grey wallpaper: plenty of contrast for shadow/blur checks
	# without needing per-test image generation; swaybg not mpvpaper (a
	# continuously-updating video wallpaper silently defeats shadow
	# rendering -- verified headlessly, see the shadow_blur commit history).
	HL_WALLPAPER="$HL_OUTDIR/wallpaper.png"
	[ -f "$HL_WALLPAPER" ] || convert -size "${HL_WIDTH}x${HL_HEIGHT}" xc:'#3a5a7a' "$HL_WALLPAPER" 2>/dev/null
	swaybg -o '*' -i "$HL_WALLPAPER" -m fill > "$HL_OUTDIR/swaybg.log" 2>&1 &
	HL_SWAYBG_PID=$!
	sleep 0.5
}

hl_stop() {
	for pid in "${HL_SPAWNED_PIDS[@]:-}"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null; done
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
	# assumes HEADLESS-1 is selmon (dispatch with no explicit monitor target
	# always acts on selmon), so refocus it unconditionally. Harmless/no-op
	# when there's only ever been one monitor.
	hl_dispatch "focus_monitor,HEADLESS-1" 0.1
	hl_dispatch "view,1" 0.1
	hl_dispatch "set_layout,tile" 0.1
	sleep 0.2
}

# ─── IPC ──────────────────────────────────────────────────────────────────

hl_dispatch() { # hl_dispatch "func,arg1,arg2" [settle_seconds]
	ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg dispatch "$1" >/dev/null 2>&1
	sleep "${2:-0.3}"
}

hl_get() { # hl_get "get all-clients" -> raw JSON on stdout
	ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg $1 2>/dev/null
}

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

hl_spawn_kitty() { # hl_spawn_kitty TITLE -> pid (also tracked for hl_reset/hl_stop)
	local title="$1"
	# NOT --hold: a held (finished-process) window needs a keypress to
	# actually close even on a compositor-issued close request, which stalls
	# kill_client tests headlessly (no input device to dismiss it with) --
	# a real long-lived foreground process closes cleanly instead.
	kitty --title "$title" -o background_opacity=1.0 -o background=#181818 \
		sh -c "echo $title; exec sleep 300" > "$HL_OUTDIR/kitty-$title.log" 2>&1 &
	local pid=$!
	HL_SPAWNED_PIDS+=("$pid")
	echo "$pid"
}

hl_wait_client_count() { # hl_wait_client_count N [timeout_tenths=30]
	local want="$1" timeout="${2:-30}" i n
	for i in $(seq 1 "$timeout"); do
		n="$(hl_get "get all-clients" | jq '.clients | length')"
		[ "$n" = "$want" ] && return 0
		sleep 0.1
	done
	return 1
}

hl_screenshot() { grim "$HL_OUTDIR/$1.png" 2>/dev/null; }

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
		return 1
	fi
	return 0
}
