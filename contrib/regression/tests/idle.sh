# idle.sh — the idle-inhibit state is READABLE, not just settable.
#
# `toggle_idle_inhibit` was write-only for its whole life: a dispatch went in,
# nothing came back, and the only client of it -- the bar's cup pill -- had to
# keep its own copy of what it had last done. That copy is a guess, and it is
# wrong in every case where something else touches the same flag: a keybind
# bound to the same dispatch, a second bar, and above all a bar restart, which
# starts the guess at "not inhibited" over a compositor that is still holding
# sleep off. A machine that never sleeps behind an icon saying it will is the
# failure mode, and nothing about it is visible until the battery is flat.
#
# So `get idle` and `watch idle` exist, and this module is about the property
# that makes them worth having: what you read back is what is actually in
# force, whoever set it.
#
# Two fields, and the difference matters. `inhibited` is what the idle notifier
# was told and answers "will this machine sleep"; `manual` is the flag this
# dispatch owns. A video player's own protocol inhibitor raises the first and
# not the second, and a toggle cannot clear it -- so a pill that showed
# `inhibited` would go dark when clicked having changed nothing.

_idle_get() { hl_get "get idle" | jq -r ".$1"; }

# Whatever the session had, put it back. In live mode this is the user's real
# "keep awake" state, and leaving it flipped would either flatten a battery or
# let a screen lock that was deliberately being held open.
_idle_restore=""
_idle_remember() {
	[ -n "$_idle_restore" ] && return 0
	_idle_restore="$(_idle_get manual)"
}
_idle_put_back() {
	[ -n "$_idle_restore" ] || return 0
	hl_dispatch "toggle_idle_inhibit,$([ "$_idle_restore" = "true" ] && echo 1 || echo 0)" 0.3
}

test_get_idle_reports_both_halves() {
	_idle_remember
	local out; out="$(hl_get "get idle")"
	hl_assert "get idle reports whether idling is inhibited" \
		"$(echo "$out" | jq -r 'has("inhibited")')" "true"
	hl_assert "...and whether it is the manual flag doing it" \
		"$(echo "$out" | jq -r 'has("manual")')" "true"
	# Booleans, not 0/1 or "true". A client reading this in JavaScript gets
	# `false !== 0` -> true out of the wrong one, which is exactly how a bar
	# flag inverted itself once already.
	hl_assert "...as JSON booleans" \
		"$(echo "$out" | jq -r '(.inhibited|type) + "," + (.manual|type)')" \
		"boolean,boolean"
}

test_toggle_idle_inhibit_is_readable_afterwards() {
	_idle_remember
	hl_dispatch "toggle_idle_inhibit,0" 0.3
	hl_assert "forcing it off reads back as off" "$(_idle_get manual)" "false"

	hl_dispatch "toggle_idle_inhibit,1" 0.3
	hl_assert "forcing it on reads back as on" "$(_idle_get manual)" "true"
	# The whole point: the manual flag reaches the notifier, so this is not a
	# variable that only the dispatch can see.
	hl_assert "...and idling really is inhibited by it" \
		"$(_idle_get inhibited)" "true"

	hl_dispatch "toggle_idle_inhibit,-1" 0.3
	hl_assert "-1 toggles from whatever it was" "$(_idle_get manual)" "false"
	hl_dispatch "toggle_idle_inhibit,-1" 0.3
	hl_assert "...and back" "$(_idle_get manual)" "true"

	hl_dispatch "toggle_idle_inhibit,0" 0.3
	hl_assert "clearing it clears the notifier too" \
		"$(_idle_get inhibited)" "false"
	_idle_put_back
}

test_watch_idle_notifies_on_toggle() {
	_idle_remember
	hl_dispatch "toggle_idle_inhibit,0" 0.3
	hl_watch_start "watch idle" widle >/dev/null
	local before; before="$(hl_watch_line_count widle)"
	# A watch pushes the current state on subscribe, so there is already a
	# line here; the assertion is that a CHANGE adds another.
	hl_assert_true "watch idle pushes the state on subscribe" \
		"$([ "$before" -ge 1 ] && echo true || echo false)"

	hl_dispatch "toggle_idle_inhibit,1"
	hl_assert_true "watch idle grows when the inhibit is taken" \
		"$(hl_wait_watch_grew widle "$before" && echo true || echo false)"

	before="$(hl_watch_line_count widle)"
	hl_dispatch "toggle_idle_inhibit,0"
	hl_assert_true "...and when it is given back" \
		"$(hl_wait_watch_grew widle "$before" && echo true || echo false)"
	_idle_put_back
}
