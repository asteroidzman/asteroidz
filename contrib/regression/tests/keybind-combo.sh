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
