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

test_combo_view_via_two_held_keys_ors_the_tags_together() {
	hl_dispatch "view,1"
	"$HL_WLVKBD" hold F11 F12 -- sleep 0.2
	sleep 0.3
	hl_assert_eq "holding F11 then F12 (without releasing) combines tags 2+3" \
		"$(hl_get "get all-monitors" | jq -c '.monitors[] | select(.name=="HEADLESS-1") | .active_tags')" \
		"[2,3]"
}

test_combo_view_after_a_key_release_replaces_instead_of_combining() {
	hl_dispatch "view,1"
	"$HL_WLVKBD" hold F11 F12 -- sleep 0.2  # combine 2+3 first, same as above
	sleep 0.3
	"$HL_WLVKBD" hold F11 -- sleep 0.1      # a fresh, separate press+release
	sleep 0.3
	hl_assert_eq "a later fresh press+release replaces rather than combining (tag_combo reset on release)" \
		"$(hl_get "get all-monitors" | jq -c '.monitors[] | select(.name=="HEADLESS-1") | .active_tags')" \
		"[2]"
}
