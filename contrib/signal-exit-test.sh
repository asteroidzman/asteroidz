#!/usr/bin/env bash
# signal-exit-test.sh — SIGTERM and SIGINT exit the compositor, immediately.
#
# `quit` asks before exiting. A SIGNAL must not: a session manager shutting the
# machine down is not asking, and there is nobody at the keyboard to answer.
# handlesig() called quit() -- the ASKING one -- for a while, so every SIGTERM
# raised a prompt and then waited, and the process only died when whatever sent
# the signal gave up and sent SIGKILL. On this machine that meant 90s stalls on
# logout and a regression run that leaked its compositor on every single module.
#
# Not part of contrib/regression/run.sh: every test in there shares one
# compositor, and the whole point of this one is to kill it. Run it on its own,
# and never alongside another headless suite (two compositors competing for the
# same GPU produce failures that have nothing to do with the code).
#
# Usage: contrib/signal-exit-test.sh
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ASTEROIDZ:-$REPO/build/asteroidz}"
[ -x "$BIN" ] || { echo "no asteroidz binary at $BIN -- build first" >&2; exit 1; }

# Never talk to the live session by accident. amsg and the compositor both read
# this, and a stale one in the shell is how a "headless" test reaches the real
# instance.
unset ASTEROIDZ_INSTANCE_SIGNATURE

PASS=0
FAIL=0
ok()  { echo "  ok - $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL - $1"; FAIL=$((FAIL + 1)); }

# one throwaway instance, killed with $1, asserted gone inside $2 seconds
check_signal() { # check_signal SIGNAL DEADLINE_S
	local sig="$1" deadline="$2"
	local dir; dir="$(mktemp -d /tmp/asteroidz-sig-XXXXXX)"
	local cfg="$dir/config.kdl" xdg="$dir/xdg" state="$dir/state"
	mkdir -p "$xdg" "$state"; chmod 700 "$xdg"
	cat > "$cfg" <<EOF
output HEADLESS-1 { width 1280; height 720; refresh 60 }
EOF

	env -i HOME="$HOME" PATH="$PATH" XDG_RUNTIME_DIR="$xdg" \
		XDG_STATE_HOME="$state" \
		WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=gles2 \
		"$BIN" -c "$cfg" > "$dir/comp.log" 2>&1 &
	local pid=$!

	# Up and rendering before the signal: killing a compositor that has not
	# finished starting proves nothing about the handler, and would pass
	# against a build where the handler is never installed at all.
	local i sock=""
	for i in $(seq 1 50); do
		sleep 0.2
		sock="$(ls "$xdg"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1)"
		[ -n "$sock" ] && break
	done
	if [ -z "$sock" ]; then
		bad "$sig: the compositor never came up (see $dir/comp.log)"
		kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
		return
	fi

	kill -"$sig" "$pid" 2>/dev/null
	local waited=0 limit=$((deadline * 10))
	while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$limit" ]; do
		sleep 0.1
		waited=$((waited + 1))
	done

	if kill -0 "$pid" 2>/dev/null; then
		bad "$sig did not exit within ${deadline}s (it is waiting for an answer)"
		kill -9 "$pid" 2>/dev/null
	else
		ok "$sig exits (${waited}00ms)"
	fi
	wait "$pid" 2>/dev/null
	rm -rf "$dir"
}

echo "=== signal-exit ==="
check_signal TERM 5
check_signal INT 5

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
