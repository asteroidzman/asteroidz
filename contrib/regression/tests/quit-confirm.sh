# quit-confirm.sh — exiting asks first, and Escape actually means Escape.
#
# The one dispatch whose failure mode is the whole session. Every other test in
# this suite can be wrong and cost a rerun; this one being wrong costs whatever
# was open.
#
# Everything here goes through a KEYBIND, not `amsg dispatch quit`, and that is
# the whole point of the file. The first version dispatched over IPC, which
# never arms the keyboard's repeat timer -- so it passed against a build where
# Escape dismissed the prompt and the still-armed repeat put it straight back,
# leaving confirming the exit as the only way out. Testing the path a person
# cannot take is worse than not testing it, because it reports a pass.
#
# WHAT THIS FILE CANNOT TEST, so nobody reads a pass here as more than it is:
# the key-repeat loop. wlvkbd exits as soon as it has sent its keys, that
# destroys its virtual keyboard, and destroykeyboardgroup() removes the group's
# key_repeat_source with it -- so a repeat can never outlive a press here, and
# the "Escape dismisses it and the still-armed repeat puts it straight back"
# failure is not reproducible headlessly at all. That one was found live and is
# fixed by disarming the repeat on every consumed key, the same way the
# screenshot overlay does; the cases below cover everything else.
#
# Assertions are shaped so they cannot pass vacuously either. "Still answering
# over the socket" is the only statement that cannot be true of a compositor
# that has quit, and "a bound key still does its job" is the only one that
# cannot be true while a modal prompt is swallowing the keyboard -- a dispatch
# proves nothing about the second, because it bypasses the keyboard entirely.

# F9 quits, F8 switches to tag 2. F8 is the probe: while the prompt is up it is
# swallowed, and the moment the prompt is really gone it works again.
#
# Appended and reloaded rather than set in HL_EXTRA_KDL, which does not work
# from here and fails SILENTLY: run.sh calls hl_start before it sources this
# file, so the variable is set long after the compositor read its config. The
# first version did that, and with F9 and F8 bound to nothing every "the prompt
# swallowed it" assertion passed by measuring a key that was never bound.
_qc_setup_done=""
_qc_setup() {
	[ -z "$_qc_setup_done" ] || return 0
	cat >> "$HL_CONFIG" <<'EOF'
binds {
	NONE+F9 { quit; }
	NONE+F8 { view 2; }
}
EOF
	hl_dispatch "reload_config" 1
	# Proof the binds took, before anything relies on them. Without this the
	# whole file can go back to passing vacuously after any config change.
	hl_dispatch "view,1" 0.3
	"$HL_WLVKBD" press F8 >/dev/null 2>&1
	sleep 0.6
	hl_assert "the probe bind is live before anything else is claimed" \
		"$(hl_current_tag_index)" "2"
	hl_dispatch "view,1" 0.3
	_qc_setup_done=1
}

_qc_alive() { hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false; }

_qc_press() { "$HL_WLVKBD" press "$1" >/dev/null 2>&1; sleep 0.6; }

# Held long enough to pass the repeat delay, so the timer is armed exactly as it
# is when somebody actually presses their quit bind.
_qc_hold_quit() { "$HL_WLVKBD" hold F9 -- sleep 1.2 >/dev/null 2>&1; sleep 0.8; }

_qc_tag() { hl_current_tag_index; }

test_quit_asks_before_exiting() {
	_qc_setup
	hl_dispatch "view,1" 0.3
	_qc_hold_quit
	hl_assert_true "the quit bind does not exit on its own" "$(_qc_alive)"

	# While the prompt is up the keyboard belongs to it.
	_qc_press F8
	hl_assert "...and it swallows other keys while it waits" "$(_qc_tag)" "1"

	# Any key that is neither answer leaves it up: two exits and no third, so
	# that pressing through it never becomes a habit.
	_qc_press SPACE
	hl_assert_true "...and an unrelated key neither exits nor dismisses" \
		"$(_qc_alive)"
	hl_assert "...and the prompt is still holding the keyboard" \
		"$(_qc_tag)" "1"

	_qc_press ESC
	hl_assert_true "Escape does not exit" "$(_qc_alive)"

	# THE regression. Escape cleared the prompt, but the repeat timer armed by
	# the F9 press was still firing the bind, so quit() ran again and the prompt
	# came straight back -- Escape looked broken and confirming the exit was the
	# only way out.
	_qc_press F8
	hl_assert "Escape really dismisses it: a bound key works again" \
		"$(_qc_tag)" "2"

	# And it stays gone. A repeat that outlived the press would put it back a
	# fraction of a second later, which the check above could still race.
	sleep 1.5
	hl_dispatch "view,1" 0.3
	_qc_press F8
	hl_assert "...and it does not come back a moment later" "$(_qc_tag)" "2"

	hl_dispatch "view,1" 0.3
}

test_the_prompt_can_be_reopened() {
	_qc_setup
	# Dismissing must not leave it unable to ask again -- that would be the
	# other way to break this, and just as quiet.
	_qc_hold_quit
	hl_assert_true "the prompt opens a second time" "$(_qc_alive)"
	_qc_press F8
	hl_assert "...and is modal again" "$(_qc_tag)" "1"
	_qc_press ESC
	_qc_press F8
	hl_assert "...and dismisses again" "$(_qc_tag)" "2"
	hl_dispatch "view,1" 0.3
}

# Deliberately last in this file, and it takes the compositor with it. The
# harness starts a fresh instance per module, so that costs nothing -- whereas
# asserting Enter exits without letting it exit would be asserting something
# else.
test_enter_exits() {
	_qc_setup
	_qc_hold_quit
	hl_assert_true "quit is still pending before Enter" "$(_qc_alive)"

	_qc_press ENTER
	sleep 1
	hl_assert_false "Enter exits" "$(_qc_alive)"
}
