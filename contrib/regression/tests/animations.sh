# animations.sh — config.animations / config.layer_animations toggles.
#
# What this CAN'T test: whether an animation actually plays. IPC's client
# geometry (x/y/width/height) is always c->geom -- the LOGICAL/target box,
# updated immediately regardless of config.animations -- never the
# interpolated c->animation.current the renderer actually draws from (that
# field isn't exposed over IPC at all). So there's no dispatch-and-poll
# sequence that can distinguish "snapped instantly" from "mid-animation" for
# any client. Real animation verification needs pixel/frame-level capture
# (contrib/anim-test.sh's recording+montage), a fundamentally different kind
# of tool than this assertion-based harness -- not something to fake here
# with a timing-based guess.
#
# What IS worth pinning: that the option round-trips through set_option
# without erroring or wedging the compositor (a bad key/value silently
# doing nothing would be indistinguishable from "worked" otherwise), by
# checking the compositor keeps responding to IPC afterwards.

test_toggling_animations_off_and_on_leaves_the_compositor_responsive() {
	hl_dispatch "set_option,animations,0"
	hl_assert_true "compositor still responds to IPC after animations off" \
		"$([ -n "$(hl_get "get all-monitors")" ] && echo true || echo false)"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_assert_eq "a window still opens fine with animations off" \
		"$(hl_get "get all-clients" | jq '.clients | length')" "1"
	hl_dispatch "set_option,animations,1"  # restore default
	hl_assert_true "compositor still responds to IPC after animations back on" \
		"$([ -n "$(hl_get "get all-monitors")" ] && echo true || echo false)"
}

test_toggling_layer_animations_off_and_on_leaves_the_compositor_responsive() {
	# layer_animations defaults to 0/off (unlike animations, which defaults
	# to 1/on) -- restore to 0, not 1, or this leaks into later modules.
	hl_dispatch "set_option,layer_animations,1"
	hl_assert_true "compositor still responds to IPC after layer_animations on" \
		"$([ -n "$(hl_get "get all-monitors")" ] && echo true || echo false)"
	hl_dispatch "set_option,layer_animations,0"  # restore default (off)
	hl_assert_true "compositor still responds to IPC after layer_animations back off" \
		"$([ -n "$(hl_get "get all-monitors")" ] && echo true || echo false)"
}
