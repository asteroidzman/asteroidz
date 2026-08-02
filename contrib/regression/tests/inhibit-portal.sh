# inhibit-portal.sh — org.freedesktop.impl.portal.Inhibit really inhibits.
#
# The portal an application uses to say "not now". A sandboxed video player has
# no surface to hang a Wayland idle-inhibitor on, so it asks over D-Bus
# instead; before this backend existed, xdg-desktop-portal answered those
# requests with "no such interface" and the screen blanked halfway through the
# film anyway.
#
# The property under test is not "the method returns" -- an empty stub returns
# too, and that is exactly the shape of the bug, since the failure is silent on
# both ends. It is that the request reaches wlr_idle_notifier_v1, that it can
# be seen while it is in force, and above all that it GOES AWAY. An inhibition
# nobody can see and nothing releases is a laptop awake in a bag, and the
# entire cost of getting this wrong lands hours later, on a flat battery, with
# nothing on screen ever having said why.
#
# So the release paths get as much attention as the take path: Close on the
# Request (what an application does), and the owner dropping off the bus (what
# a crash does). The second is why every case here uses
# contrib/portal-inhibit-client.py rather than busctl -- busctl exits the
# instant its call returns, so an inhibition it takes is reclaimed immediately
# and correctly, which makes it useless for testing anything except the
# reclaim.

_ip_client="$HL_REPO/contrib/portal-inhibit-client.py"
_ip_bus="org.freedesktop.impl.portal.desktop.asteroidz"
_ip_obj="/org/freedesktop/portal/desktop"
_ip_iface="org.freedesktop.impl.portal.Inhibit"

# No bus, no backend. hl_start brings up a private dbus-daemon when one is
# installed; without it there is nothing here to test and saying so is better
# than a red run on a machine that is fine.
_ip_skip() { # _ip_skip NAME -> 0 if the test should be skipped
	if [ -z "${HL_DBUS_ADDR:-}" ]; then
		hl_skip "$1: no session bus for the test compositor (dbus-daemon not installed?)"
		return 0
	fi
	if ! command -v python3 >/dev/null || \
	   ! python3 -c "import gi" 2>/dev/null; then
		hl_skip "$1: python3 with PyGObject is needed to hold a bus connection open"
		return 0
	fi
	return 1
}

_ip_pids=""
# Start a client and wait for it to say it is connected. Its PID is recorded so
# it can be killed by PID -- never by pattern, which on this machine would
# match the user's real session.
_ip_start() { # _ip_start OUTFILE ARGS...
	local out="$1"; shift
	: > "$out"
	python3 "$_ip_client" --address "$HL_DBUS_ADDR" "$@" >> "$out" 2>&1 &
	local pid=$!
	_ip_pids="$_ip_pids $pid"
	local i
	for i in $(seq 1 40); do
		grep -q '^ready$' "$out" 2>/dev/null && { echo "$pid"; return 0; }
		kill -0 "$pid" 2>/dev/null || break
		sleep 0.1
	done
	echo "" # the caller asserts on the state, and a dead client fails it
	return 1
}

_ip_stop_all() {
	local pid
	for pid in $_ip_pids; do kill "$pid" 2>/dev/null; done
	_ip_pids=""
	sleep 0.4
}

_ip_idle() { hl_get "get idle" | jq -r "$1"; }

test_the_interface_is_exported() {
	_ip_skip "the interface is exported" && return 0
	local out; out="$(hl_busctl introspect "$_ip_bus" "$_ip_obj")"

	hl_assert_true "the Inhibit interface is on the bus at all" \
		"$(echo "$out" | grep -q "$_ip_iface" && echo true || echo false)"
	# Same name, same object as GlobalShortcuts. A .portal file names ONE
	# service, so a second backend that took a connection and a name of its own
	# would lose the race and silently do nothing.
	hl_assert_true "...alongside GlobalShortcuts, on the one shared name" \
		"$(echo "$out" | grep -q "org.freedesktop.impl.portal.GlobalShortcuts" \
			&& echo true || echo false)"
	hl_assert_true "...with all three of its methods" \
		"$(echo "$out" | grep -cE '\.(Inhibit|CreateMonitor|QueryEndResponse) +method' \
			| grep -q '^3$' && echo true || echo false)"
	# Version 3 is what advertises CreateMonitor/QueryEndResponse. A backend
	# claiming less gets the monitor half skipped by xdg-desktop-portal.
	hl_assert "...at spec version 3" \
		"$(hl_busctl get-property "$_ip_bus" "$_ip_obj" "$_ip_iface" version \
			| awk '{print $2}')" "3"
}

test_an_idle_inhibition_reaches_the_notifier() {
	_ip_skip "an idle inhibition reaches the notifier" && return 0
	hl_assert "nothing is holding idling off to begin with" \
		"$(_ip_idle .inhibited)" "false"

	_ip_start "$HL_OUTDIR/ip-idle.log" --inhibit 8 --app-id org.mpv.Player \
		--reason "Playing video" >/dev/null
	sleep 0.4

	# The point of the whole file: not that the call was accepted, but that the
	# idle notifier was actually told. `inhibited` is what was handed to it.
	hl_assert "an idle inhibition really inhibits idling" \
		"$(_ip_idle .inhibited)" "true"
	# ...and NOT the manual flag. They are independent levers, and a bar whose
	# toggle lit up because mpv asked would go dark when clicked having changed
	# nothing the user can see.
	hl_assert "...without touching the manual flag" "$(_ip_idle .manual)" "false"

	hl_assert "the app that asked is named" \
		"$(_ip_idle '.portal[0].app_id')" "org.mpv.Player"
	hl_assert "...with the reason it gave" \
		"$(_ip_idle '.portal[0].reason')" "Playing video"
	hl_assert "...and which of the four flags it asked for" \
		"$(_ip_idle '.portal[0] | "\(.idle),\(.suspend),\(.logout),\(.user_switch)"')" \
		"true,false,false,false"

	_ip_stop_all
	hl_assert "and the whole thing is gone once the client is" \
		"$(_ip_idle .inhibited)" "false"
	hl_assert "...with nothing left in the list" \
		"$(_ip_idle '.portal | length')" "0"
}

test_closing_the_request_releases_it() {
	_ip_skip "closing the request releases it" && return 0
	local handle="/org/freedesktop/portal/desktop/request/tester/close"
	_ip_start "$HL_OUTDIR/ip-close.log" --inhibit 8 --handle "$handle" \
		--app-id org.example.Closer >/dev/null
	sleep 0.4
	hl_assert "the inhibition is in force" "$(_ip_idle .inhibited)" "true"

	# The spec has no "uninhibit": the Request object exported at the handle IS
	# the handle on it, and Close is the only way an application gives one
	# back. The client stays connected throughout, so a pass here cannot be the
	# owner-died path passing by accident.
	hl_busctl call "$_ip_bus" "$handle" \
		"org.freedesktop.impl.portal.Request" Close >/dev/null
	sleep 0.4
	hl_assert "Close on the Request gives it back" "$(_ip_idle .inhibited)" "false"
	hl_assert_true "...while the client is still connected" \
		"$(kill -0 ${_ip_pids# } 2>/dev/null && echo true || echo false)"

	_ip_stop_all
}

test_the_owner_dying_releases_it() {
	_ip_skip "the owner dying releases it" && return 0
	local pid
	pid="$(_ip_start "$HL_OUTDIR/ip-crash.log" --inhibit 8 --app-id org.example.Crasher)"
	sleep 0.4
	hl_assert "the inhibition is in force" "$(_ip_idle .inhibited)" "true"

	# Nothing calls Close for an application that died, and every one of these
	# requests arrives through xdg-desktop-portal -- so if xdp goes down, the
	# inhibition has no owner left to release it and no window to point at.
	# SIGKILL, not SIGTERM: a crash does not get to run cleanup, and a pass
	# here must not depend on the client being polite about it.
	kill -9 "$pid" 2>/dev/null
	_ip_pids=""
	local i
	for i in $(seq 1 30); do
		[ "$(_ip_idle .inhibited)" = "false" ] && break
		sleep 0.1
	done
	hl_assert "an owner that dies without closing does not leave it held" \
		"$(_ip_idle .inhibited)" "false"
}

test_suspend_holds_idling_and_user_switch_does_not() {
	_ip_skip "suspend holds idling and user-switch does not" && return 0

	# Suspend is folded into idle inhibition deliberately -- idle-driven
	# suspend is what an app asking not to be suspended almost always means,
	# and it is the half a compositor is in charge of. See inhibit-portal.h.
	_ip_start "$HL_OUTDIR/ip-suspend.log" --inhibit 4 --app-id org.example.Updater \
		>/dev/null
	sleep 0.4
	hl_assert "asking to block suspend holds idling off" \
		"$(_ip_idle .inhibited)" "true"
	_ip_stop_all

	# User-switch is recorded and NOT enforced: asteroidz has no user
	# switching, so there is nothing to inhibit. It still has to show up in the
	# list, or the entry a client took is invisible -- an entry that appears
	# here without raising `inhibited` is the honest way to say "recorded, not
	# enforced".
	_ip_start "$HL_OUTDIR/ip-switch.log" --inhibit 2 --app-id org.example.Switcher \
		>/dev/null
	sleep 0.4
	hl_assert "asking to block user-switching does not hold idling off" \
		"$(_ip_idle .inhibited)" "false"
	hl_assert "...but is still listed, rather than silently dropped" \
		"$(_ip_idle '.portal[0].app_id')" "org.example.Switcher"
	hl_assert "...as the flag it actually asked for" \
		"$(_ip_idle '.portal[0].user_switch')" "true"
	_ip_stop_all
}

test_one_release_does_not_release_the_other() {
	_ip_skip "one release does not release the other" && return 0
	local keep="/org/freedesktop/portal/desktop/request/tester/keep"
	local drop="/org/freedesktop/portal/desktop/request/tester/drop"
	_ip_start "$HL_OUTDIR/ip-keep.log" --inhibit 8 --handle "$keep" \
		--app-id org.example.Keeper >/dev/null
	_ip_start "$HL_OUTDIR/ip-drop.log" --inhibit 8 --handle "$drop" \
		--app-id org.example.Dropper >/dev/null
	sleep 0.5
	hl_assert "two clients, two entries" "$(_ip_idle '.portal | length')" "2"

	hl_busctl call "$_ip_bus" "$drop" \
		"org.freedesktop.impl.portal.Request" Close >/dev/null
	sleep 0.4
	# The reason inhibitions are a list and not a counter or a flag: releasing
	# one must not release another app's.
	hl_assert "closing one leaves the other's entry" \
		"$(_ip_idle '.portal | length')" "1"
	hl_assert "...the one that did not close" \
		"$(_ip_idle '.portal[0].app_id')" "org.example.Keeper"
	hl_assert "...and idling is still held off" "$(_ip_idle .inhibited)" "true"
	_ip_stop_all
}

test_watch_idle_notifies_when_a_portal_client_asks() {
	_ip_skip "watch idle notifies when a portal client asks" && return 0
	hl_watch_start "watch idle" wportal >/dev/null
	local before; before="$(hl_watch_line_count wportal)"

	_ip_start "$HL_OUTDIR/ip-watch.log" --inhibit 8 --app-id org.example.Watched \
		>/dev/null
	hl_assert_true "a bar drawing this state hears about it" \
		"$(hl_wait_watch_grew wportal "$before" && echo true || echo false)"

	# And on a change that does NOT move the effective boolean. A second app
	# taking an inhibition while one already holds it changes nothing about
	# whether the machine sleeps, and everything about the list of who is
	# holding it -- which is why the push compares the list, not just the flag.
	before="$(hl_watch_line_count wportal)"
	_ip_start "$HL_OUTDIR/ip-watch2.log" --inhibit 8 --app-id org.example.Second \
		--handle /org/freedesktop/portal/desktop/request/tester/2 >/dev/null
	hl_assert_true "...including a second one, which changes only the list" \
		"$(hl_wait_watch_grew wportal "$before" && echo true || echo false)"
	_ip_stop_all
}

test_a_monitor_is_told_the_state_immediately() {
	_ip_skip "a monitor is told the state immediately" && return 0
	local out="$HL_OUTDIR/ip-monitor.log"
	_ip_start "$out" --monitor --app-id org.example.Watcher \
		--session /org/freedesktop/portal/desktop/session/tester/mon >/dev/null
	sleep 0.5

	# CreateMonitor has no companion "what is it now" call -- the spec's only
	# channel is the signal. So a monitor created while the screen is already
	# locked would report the opposite of the truth for as long as the lock
	# lasted, unless the current state is pushed on subscribe.
	local line; line="$(grep -m1 '"session"' "$out")"
	hl_assert_true "a new monitor is sent the state without having to ask" \
		"$([ -n "$line" ] && echo true || echo false)"
	hl_assert "...naming the session it belongs to" \
		"$(echo "$line" | jq -r '.session')" \
		"/org/freedesktop/portal/desktop/session/tester/mon"
	hl_assert "...as Running, with no screensaver up" \
		"$(echo "$line" | jq -r '."state"["session-state"], ."state"["screensaver-active"]' | paste -sd,)" \
		"1,false"
	_ip_stop_all
}

test_nonsense_flags_are_refused() {
	_ip_skip "nonsense flags are refused" && return 0
	# The spec's flags are a closed set of four bits. Quietly honouring the
	# bits we recognise out of a request that also asked for something that
	# does not exist would hide the caller's mistake from them -- and leave
	# them believing they got something they did not.
	local out; out="$(hl_busctl call "$_ip_bus" "$_ip_obj" "$_ip_iface" \
		Inhibit 'ossua{sv}' /org/freedesktop/portal/desktop/request/tester/bad \
		org.example.Confused "" 64 0)"
	hl_assert_true "a flag outside the spec is an error, not a silent no-op" \
		"$(echo "$out" | grep -qi "unsupported inhibit flags" && echo true || echo false)"

	local zero; zero="$(hl_busctl call "$_ip_bus" "$_ip_obj" "$_ip_iface" \
		Inhibit 'ossua{sv}' /org/freedesktop/portal/desktop/request/tester/zero \
		org.example.Confused "" 0 0)"
	hl_assert_true "so is asking to inhibit nothing at all" \
		"$(echo "$zero" | grep -qi "unsupported inhibit flags" && echo true || echo false)"
	hl_assert "and neither one left anything behind" \
		"$(_ip_idle '.portal | length')" "0"
}

test_a_logout_inhibition_is_named_in_the_exit_prompt() {
	_ip_skip "a logout inhibition is named in the exit prompt" && return 0
	if [ "${HL_LIVE_MODE:-0}" = "1" ]; then
		hl_skip "a logout inhibition is named in the exit prompt: live mode -- this raises the real exit prompt"
		return 0
	fi

	_ip_start "$HL_OUTDIR/ip-logout.log" --inhibit 1 --app-id org.example.Installer \
		--reason "Installing updates" >/dev/null
	sleep 0.4
	hl_assert "a logout inhibition does not hold idling off" \
		"$(_ip_idle .inhibited)" "false"
	hl_assert "...but is recorded as a logout inhibition" \
		"$(_ip_idle '.portal[0].logout')" "true"

	# The compositor IS the session, so there is no session manager to veto an
	# exit -- and refusing outright would be the wrong answer anyway, since the
	# person at the keyboard outranks a background request. What honouring the
	# flag means here is that the exit prompt SAYS so, and the log records the
	# same fact where a test can see it. Escape immediately afterwards: the
	# prompt is modal and holds the whole keyboard.
	local log="$HL_STATE/asteroidz/asteroidz.log"
	# grep -c prints 0 AND exits non-zero when it matches nothing, so a `|| echo
	# 0` fallback appends a second line and the arithmetic below chokes on it.
	local before; before="$(grep -c "exit requested" "$log" 2>/dev/null)"
	[ -n "$before" ] || before=0
	hl_dispatch "quit" 0.6
	hl_assert_true "the exit prompt says an application asked not to be interrupted" \
		"$(tail -n +$((before + 1)) "$log" 2>/dev/null \
			| grep -q "exit requested.*asked not to be interrupted" \
			&& echo true || echo false)"

	"$HL_WLVKBD" press ESC >/dev/null 2>&1
	sleep 0.6
	# The prompt really is gone, and the compositor really is still here: both
	# have to be true, or the module after this one starts failing for reasons
	# that point nowhere near itself.
	hl_assert "the compositor is still answering after Escape" \
		"$(hl_get "get idle" | jq -r 'has("inhibited")')" "true"
	_ip_stop_all
}

test_the_other_backend_still_has_its_sessions() {
	_ip_skip "the other backend still has its sessions" && return 0
	# Not an Inhibit test. It is here because adding this backend is what made
	# the two share: one connection, one bus name, and -- the part with teeth
	# -- ONE Session object, since sessions live under a path prefix
	# xdg-desktop-portal chooses rather than we do, so a second fallback vtable
	# on that prefix would have collided with the first. Close is now routed by
	# asking each backend whether the path is its own, and the way that breaks
	# is silent: global shortcuts keep working right up until an app closes its
	# session, and then the compositor holds a session for an application that
	# is gone. Discord's push-to-talk is not reproducible headlessly; this is.
	local gs="org.freedesktop.impl.portal.GlobalShortcuts"
	local session="/org/freedesktop/portal/desktop/session/tester/gs"
	local log="$HL_STATE/asteroidz/asteroidz.log"

	hl_busctl call "$_ip_bus" "$_ip_obj" "$gs" CreateSession 'oosa{sv}' \
		/org/freedesktop/portal/desktop/request/tester/gs "$session" \
		org.example.Shortcutter 0 >/dev/null
	sleep 0.3
	hl_assert_true "a global-shortcuts session can still be created" \
		"$(grep -q "global shortcuts session created for org.example.Shortcutter" \
			"$log" && echo true || echo false)"

	hl_busctl call "$_ip_bus" "$session" \
		"org.freedesktop.impl.portal.Session" Close >/dev/null
	sleep 0.3
	hl_assert_true "...and Close still reaches it through the shared object" \
		"$(grep -q "global shortcuts session closed for org.example.Shortcutter" \
			"$log" && echo true || echo false)"

	# The same Close handler serves both backends, so it must not answer for a
	# path neither of them owns by pretending to have closed something.
	hl_busctl call "$_ip_bus" "/org/freedesktop/portal/desktop/session/tester/nobody" \
		"org.freedesktop.impl.portal.Session" Close >/dev/null
	hl_assert "the compositor is still answering afterwards" \
		"$(hl_get "get idle" | jq -r 'has("inhibited")')" "true"
}
