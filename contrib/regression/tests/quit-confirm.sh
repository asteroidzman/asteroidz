# quit-confirm.sh — exiting asks first.
#
# The one dispatch whose failure mode is the whole session. Every other test in
# this suite can be wrong and cost a rerun; this one being wrong costs whatever
# was open.
#
# Which is also why the assertions here are shaped the way they are. "The prompt
# appeared" is not the claim worth checking -- a prompt that appears and exits
# anyway is the bug. So each case asserts the compositor is STILL ANSWERING
# afterwards, over the socket, which is the only statement that cannot be true
# of a compositor that has quit.
#
# Keys go in through wtype (zwp_virtual_keyboard_v1), not through a dispatch:
# the prompt reads real key events ahead of the bind table, so a test that
# dispatched its way past it would be testing a path that does not exist.

_qc_alive() { # is the compositor still there?
	hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false
}

_qc_key() { # _qc_key <keysym>
	WAYLAND_DISPLAY="$HL_SOCK" XDG_RUNTIME_DIR="$HL_XDG" \
		wtype -k "$1" 2>/dev/null
	sleep 0.5
}

test_quit_asks_before_exiting() {
	if ! command -v wtype >/dev/null 2>&1; then
		hl_skip "quit confirmation: wtype is not installed"
		return
	fi

	hl_dispatch "quit" 0.6
	hl_assert_true "dispatching quit does not exit on its own" "$(_qc_alive)"

	# Dispatched again while the prompt is up. A repeating keybind can send this
	# many times a second, and reading the second one as consent would make the
	# prompt worse than no prompt at all.
	hl_dispatch "quit" 0.4
	hl_assert_true "...and a second quit is not taken as the answer" "$(_qc_alive)"

	# Any key that is neither answer leaves it up. The prompt has two exits and
	# no third, so that pressing through it is not a habit anyone can form.
	_qc_key "space"
	hl_assert_true "...and an unrelated key neither exits nor dismisses" \
		"$(_qc_alive)"

	_qc_key "Escape"
	hl_assert_true "Escape dismisses the prompt" "$(_qc_alive)"

	# The real proof that Escape DISMISSED rather than merely not-exited: a key
	# that would have been swallowed by the prompt reaches the bind table again.
	# Without this, a prompt that stayed up forever would pass every line above.
	local before after
	before="$(hl_current_tag_index)"
	hl_dispatch "view,2" 0.4
	after="$(hl_current_tag_index)"
	hl_assert "...and the session is interactive again (tag $before -> $after)" \
		"$after" "2"
	hl_dispatch "view,1" 0.3
}

# Deliberately the last case in this file, and it takes the compositor with it.
# The harness starts a fresh instance per module, so this costs nothing --
# whereas asserting that Enter exits without actually letting it exit would be
# asserting something else.
test_enter_exits() {
	if ! command -v wtype >/dev/null 2>&1; then
		hl_skip "quit confirmation: wtype is not installed"
		return
	fi

	hl_dispatch "quit" 0.6
	hl_assert_true "quit is still pending before Enter" "$(_qc_alive)"

	_qc_key "Return"
	sleep 1
	hl_assert_false "Enter exits" "$(_qc_alive)"

	# Nothing after this: the instance is gone, and hl_stop copes with a
	# compositor that has already exited.
}
