#!/usr/bin/env bash
# Session autostart, spawned once by asteroidz.
#
# One script rather than a column of `spawn-at-startup` lines, because most of
# what a session needs to start is conditional -- "if it is installed", "if it is
# not already running" -- and KDL has no way to say either. A config line that
# launches something absent produces a shell error into the log and nothing else;
# a hundred of them produce a log nobody reads.
#
# Copied to ~/.config/asteroidz/autostart.sh and edited there. The shipped copy
# stays as the fallback: `config.kdl` runs your copy if it exists and this one
# otherwise, so a fresh install has a working session before you have touched
# anything.
#
# EVERYTHING HERE IS OPTIONAL except the bar. Each block checks that its program
# exists and is not already running, so this script is safe to run twice and safe
# to run on a machine that has none of them.

set -uo pipefail

CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/asteroidz"

have() { command -v "$1" >/dev/null 2>&1; }

# ── the palette has to exist ────────────────────────────────────────────────
#
# config.kdl sources colors.kdl, and asteroidz treats a `source` it cannot open
# as a FATAL error rather than a warning. matugen rewrites that file on every
# wallpaper change, and a matugen run that fails part-way can leave it missing --
# at which point the next reload takes the whole config down.
#
# An empty file is a valid KDL document that sets nothing, so the compositor
# falls back to its compiled-in colours instead of refusing to start. Belt and
# braces, and cheap.
[ -f "$CONFIG_DIR/colors.kdl" ] || : > "$CONFIG_DIR/colors.kdl" 2>/dev/null || true

# ── the shell ───────────────────────────────────────────────────────────────
#
# asteroidz-bar draws the bar AND the wallpaper -- they share one process and one
# Wayland connection, so there is nothing else to start for either.
#
# The guard matches the QML PATH, not a process name. The launcher execs
# quickshell, so `pgrep -x asteroidz-bar` never matches anything and the check
# would silently do nothing. It is anchored for the opposite reason: an
# unanchored `pgrep -f` also matches any shell that merely mentions the string --
# including a terminal running this very script, which answered "already running"
# and left the session with no bar.
if have asteroidz-bar; then
	pgrep -f "^/usr/bin/qs -p /usr/share/asteroidz-bar" >/dev/null 2>&1 \
		|| asteroidz-bar >/tmp/asteroidz-bar-session.log 2>&1 &
fi

# ── clipboard history ───────────────────────────────────────────────────────
if have wl-paste && have wl-copy; then
	pgrep -f "wl-paste -pw wl-copy" >/dev/null 2>&1 || wl-paste -pw wl-copy &
fi
if have wl-paste && have cliphist; then
	pgrep -f "wl-paste --watch cliphist" >/dev/null 2>&1 \
		|| wl-paste --watch cliphist store &
fi

# ── notifications ───────────────────────────────────────────────────────────
#
# The bar's notification module talks to swaync over its own D-Bus name, so
# without a daemon that pill is simply absent rather than broken.
if have swaync; then
	pgrep -x swaync >/dev/null 2>&1 || swaync &
fi

# ── idle handling ───────────────────────────────────────────────────────────
#
# No lock command is configured here on purpose: which locker to use and how long
# to wait are choices, and guessing them would either lock a machine somebody did
# not want locked or look broken when it did nothing. `swayidle -w` with no rules
# is a no-op that costs nothing and is there to add rules to.
if have swayidle; then
	pgrep -x swayidle >/dev/null 2>&1 || swayidle -w &
fi

# ── XWayland resources ──────────────────────────────────────────────────────
#
# Only if there is something to load. `xrdb -load` on a missing file is an error
# on every login.
if have xrdb && [ -f "$HOME/.Xresources" ]; then
	xrdb -load "$HOME/.Xresources" &
fi

wait
