# keybind-combo.sh — combo_view's tag_combo chord state (dispatch/bind_
# define.h's comboview()), tested via real key events (wlvkbd), not IPC
# dispatch.
#
# tag_combo is a single process-global bool, reset to false ONLY by a real
# WL_KEYBOARD_KEY_STATE_RELEASED event in the compositor's own key handler
# -- an `amsg dispatch combo_view,N` call goes straight to comboview()
# without ever touching that handler, so it can SET tag_combo=true but can
# never reset it. That means combo_view's actual "hold key A, also press key
# B without releasing A, tags OR together; release, next press replaces
# instead" chord behavior is only observable by driving real key events
# (wlvkbd), never via bare dispatch -- hl_start's shared config binds F11 ->
# combo_view,2 and F12 -> combo_view,3 specifically for this.

# F11/F12 -> combo_view,2/combo_view,3 only exist in hl_start's own synthetic
# test config -- in live mode the compositor runs the user's REAL config,
# which (confirmed live 2026-07-20) has no F11/F12 bindings and no
# combo_view bindings at all. There's no IPC introspection of the live
# keymap to find an equivalent real binding, and injecting one would mean
# altering the user's actual config -- skip rather than assert on keys that
# structurally can't do anything in this session.
hl_skip_if_live_no_test_keybinds() {
	if [ "${HL_LIVE_MODE:-0}" = "1" ]; then
		hl_skip "$1: needs F11/F12 bound to combo_view, which only exists in the synthetic test config -- skipping rather than assert on keys your real config doesn't bind"
		return 1
	fi
	return 0
}

test_combo_view_via_two_held_keys_ors_the_tags_together() {
	hl_skip_if_live_no_test_keybinds "test_combo_view_via_two_held_keys_ors_the_tags_together" || return
	hl_dispatch "focus_monitor,$HL_MON" 0.2
	hl_dispatch "view,1"
	"$HL_WLVKBD" hold F11 F12 -- sleep 0.2
	sleep 0.3
	hl_assert_eq "holding F11 then F12 (without releasing) combines tags 2+3" \
		"$(hl_get "get all-monitors" | jq -c ".monitors[] | select(.name==\"$HL_MON\") | .active_tags")" \
		"[2,3]"
}

test_combo_view_after_a_key_release_replaces_instead_of_combining() {
	hl_skip_if_live_no_test_keybinds "test_combo_view_after_a_key_release_replaces_instead_of_combining" || return
	hl_dispatch "focus_monitor,$HL_MON" 0.2
	hl_dispatch "view,1"
	"$HL_WLVKBD" hold F11 F12 -- sleep 0.2  # combine 2+3 first, same as above
	sleep 0.3
	"$HL_WLVKBD" hold F11 -- sleep 0.1      # a fresh, separate press+release
	sleep 0.3
	hl_assert_eq "a later fresh press+release replaces rather than combining (tag_combo reset on release)" \
		"$(hl_get "get all-monitors" | jq -c ".monitors[] | select(.name==\"$HL_MON\") | .active_tags")" \
		"[2]"
}

test_consumed_key_release_is_swallowed() {
	# A press-only bind consumes its PRESS (handled -> early return) but the
	# RELEASE used to match nothing, fall through, and reach the focused client
	# as a release with no press. Proton/Windows games under gamescope track
	# raw key state and act on that; ordinary toolkits discard it, which is why
	# it went unnoticed for so long.
	#
	# The client side is asserted separately, in the two tests below, now that
	# contrib/wlkeys can report what it was actually sent. This one pins the
	# regression risk of the fix -- that tracking consumed keycodes does not
	# break normal binding, and that hammering a bound key neither exhausts the
	# fixed-size set nor wedges the compositor.
	hl_dispatch "view,1"
	local i
	for i in 1 2 3 4 5 6 7 8 9 10; do
		"$HL_WLVKBD" press F11    # bound to combo_view 2 in hl_start's config
	done
	sleep 0.5
	hl_assert_true "the compositor survives repeated bound press/release" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"

	# and the binding still fires after all that bookkeeping
	hl_dispatch "view,1"
	"$HL_WLVKBD" press F11
	sleep 0.5
	hl_assert_eq "a bound key still switches view after the release fix" \
		"$(hl_get "get all-monitors" | jq -c '.monitors[0].active_tags')" "[2]"
	hl_dispatch "view,1"
}

# F11 is bound to combo_view,2 in hl_start's config, so pressing it while a
# client sits on tag 2 both consumes the key AND moves focus to that client --
# the exact sequence that made a Proton game receive a keypress forever.
test_a_consumed_key_is_not_reported_as_held_on_focus_enter() {
	hl_skip_if_live_no_test_keybinds "test_a_consumed_key_is_not_reported_as_held_on_focus_enter" || return
	hl_dispatch "view,2"
	hl_spawn_wlkeys "wlkeys-enter" 8 "wlkeys-enter" >/dev/null
	hl_wait_client_count 1 || true
	sleep 0.5
	hl_dispatch "view,1" 0.3   # the probe loses focus

	# hold it across the focus change, which is when enter's key array is built
	"$HL_WLVKBD" hold F11 -- sleep 0.4
	sleep 0.4

	# wl_keyboard.enter says "these keys are held"; a client told F11 (evdev 87)
	# is held repeats it until a release it will never get, because the release
	# of a consumed press is swallowed. It must not appear.
	local held
	held="$(hl_wlkeys_last_enter wlkeys-enter)"
	hl_assert_true "the bound key is absent from the focus-enter held-key array (got '$held')" \
		"$(case ",$held," in *,87,*) echo false;; *) echo true;; esac)"

	# and the client saw neither edge of it: no press (the binding ate it) and
	# no release (swallowed to match)
	hl_assert_eq "the client is sent no key event at all for the consumed key" \
		"$(hl_count_lines '^key 87 ' "$HL_OUTDIR/wlkeys-enter.log")" "0"
	hl_dispatch "view,1"
}

# The filter must not swallow ordinary typing: it keys off the consumed set,
# so an unbound key has to survive it untouched.
test_an_unbound_key_still_reaches_the_focused_client() {
	hl_dispatch "view,1"
	hl_spawn_wlkeys "wlkeys-pass" 8 "wlkeys-pass" >/dev/null
	hl_wait_client_count 1 || true
	sleep 0.5

	"$HL_WLVKBD" press F9   # evdev 67, bound to nothing
	sleep 0.4

	hl_assert_eq "an unbound key's press reaches the client" \
		"$(hl_count_lines '^key 67 pressed' "$HL_OUTDIR/wlkeys-pass.log")" "1"
	hl_assert_eq "an unbound key's release reaches the client" \
		"$(hl_count_lines '^key 67 released' "$HL_OUTDIR/wlkeys-pass.log")" "1"
}
