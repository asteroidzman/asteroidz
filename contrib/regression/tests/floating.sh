# floating.sh — i3-style floating policy added after 0.17.0:
#   layout/floating/center-new     (float_center_new, default 1)
#   layout/floating/min-{w,h}      (float_min_width/height, default 75x50)
#   layout/floating/max-{w,h}      (float_max_width/height, 0 = output size)
#   layout/floating/keep-onscreen  (float_keep_onscreen px, default 30)
#
# The harness config leaves all of these at their defaults, so these tests
# assert the DEFAULT i3-like behavior. move_window/resize_window follow the
# sign-prefix convention (see geometry.sh header): "+N"/"-N" are relative
# deltas, a bare "N" is absolute.
#
# COORDINATES ARE LAYOUT-ABSOLUTE and must be interpreted relative to the
# actual monitor/layout, NOT assumed to start at 0,0: in live mode the
# virtual test output sits at a nonzero offset inside the real multi-monitor
# layout (headless.sh:218). So centering is checked against the window's OWN
# monitor, and the keep-on-screen strip against the whole-layout bounding box
# (float clamping uses sgeom, the union of all outputs -- a floating window
# is only stopped at the outer edge of the entire layout, i3-style, so it can
# still be dragged across outputs). In single-output headless mode the
# monitor IS the layout at (0,0), so the same assertions hold there too.

# layout bounding box across every output (matches sgeom, the bbox float
# clamping uses); LEFT/TOP are usually 0 but never assumed.
hl_layout_right()  { hl_get "get all-monitors" | jq '[.monitors[] | .x + .width]  | max'; }
hl_layout_bottom() { hl_get "get all-monitors" | jq '[.monitors[] | .y + .height] | max'; }
hl_layout_left()   { hl_get "get all-monitors" | jq '[.monitors[] | .x] | min'; }
hl_layout_top()    { hl_get "get all-monitors" | jq '[.monitors[] | .y] | min'; }
# geometry of the monitor a given window currently lives on
hl_win_mon_field() { # hl_win_mon_field TITLE FIELD
	local mon; mon="$(hl_client_field "$1" monitor)"
	hl_get "get all-monitors" | jq -r ".monitors[] | select(.name==\"$mon\") | .$2"
}

# A new window on a float-layout tag is centered on its own monitor's work
# area (i3's default placement), not dropped at the old top-left cascade slot.
test_float_layout_centers_new_window() {
	hl_dispatch "set_layout,float"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.4
	local x y w h mx my mw mh
	x="$(hl_client_field W1 x)"; y="$(hl_client_field W1 y)"
	w="$(hl_client_field W1 width)"; h="$(hl_client_field W1 height)"
	mx="$(hl_win_mon_field W1 x)"; my="$(hl_win_mon_field W1 y)"
	mw="$(hl_win_mon_field W1 width)"; mh="$(hl_win_mon_field W1 height)"
	local mid_x=$((x + w / 2)) mid_y=$((y + h / 2))
	local ctr_x=$((mx + mw / 2)) ctr_y=$((my + mh / 2))
	local dx=$((mid_x - ctr_x)); dx=${dx#-}
	local dy=$((mid_y - ctr_y)); dy=${dy#-}
	# generous tolerance: work area may be inset by gaps/layers, the midpoint
	# just needs to be near its monitor's center rather than at a top-left
	# cascade slot (which would sit a full quarter-width off).
	hl_assert_true "float layout centers a new window on its monitor (float_center_new; d=($dx,$dy))" \
		"$([ "$dx" -lt 120 ] && [ "$dy" -lt 120 ] && echo true || echo false)"
}

# A floating window resized far larger than its output is capped at the
# output size (float_max_* defaulting to the work area). Without the clamp
# the window would keep the full 5000px it was told to take.
test_floating_size_capped_to_output() {
	hl_dispatch "set_layout,float"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "resize_window,5000,5000" 0.5
	local w h mw mh
	w="$(hl_client_field W1 width)"; h="$(hl_client_field W1 height)"
	mw="$(hl_win_mon_field W1 width)"; mh="$(hl_win_mon_field W1 height)"
	hl_assert_true "floating width capped at its output width ($w <= $mw)" \
		"$([ "$w" -le "$mw" ] && echo true || echo false)"
	hl_assert_true "floating height capped at its output height ($h <= $mh)" \
		"$([ "$h" -le "$mh" ] && echo true || echo false)"
}

# A floating window resized below the floor is clamped up. The floor is
# max(float_min_*, the client's own min-size hint), so this is an invariant
# guard ("never smaller than 75x50") rather than a proof the config floor,
# and not kitty's hint, is the binding one.
test_floating_size_has_a_floor() {
	hl_dispatch "set_layout,float"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "resize_window,10,10" 0.5
	local w h
	w="$(hl_client_field W1 width)"; h="$(hl_client_field W1 height)"
	hl_assert_true "floating width kept at/above the 75px floor ($w >= 75)" \
		"$([ "$w" -ge 75 ] && echo true || echo false)"
	hl_assert_true "floating height kept at/above the 50px floor ($h >= 50)" \
		"$([ "$h" -ge 50 ] && echo true || echo false)"
}

# Moving a floating window far past the right/bottom leaves a grabbable strip
# on-screen (float_keep_onscreen, default 30px) instead of letting it vanish.
# The clamp is against the whole-layout bounding box, so a +5000 move lands
# the window's left/top edge exactly 30px inside the layout's far edge.
test_floating_kept_onscreen_bottom_right() {
	hl_dispatch "set_layout,float"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "move_window,+5000,+5000" 0.5
	local x y right bottom
	x="$(hl_client_field W1 x)"; y="$(hl_client_field W1 y)"
	right="$(hl_layout_right)"; bottom="$(hl_layout_bottom)"
	hl_assert_true "left edge kept >=30px inside the layout's right edge ($x <= $((right - 30)))" \
		"$([ "$x" -le $((right - 30)) ] && echo true || echo false)"
	hl_assert_true "top edge kept >=30px inside the layout's bottom edge ($y <= $((bottom - 30)))" \
		"$([ "$y" -le $((bottom - 30)) ] && echo true || echo false)"
}

# ...and past the top/left: at least ~30px of the window's far edge stays
# within the layout's near edge.
test_floating_kept_onscreen_top_left() {
	hl_dispatch "set_layout,float"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	hl_dispatch "move_window,-5000,-5000" 0.5
	local x y w h right bottom left top
	x="$(hl_client_field W1 x)"; y="$(hl_client_field W1 y)"
	w="$(hl_client_field W1 width)"; h="$(hl_client_field W1 height)"
	left="$(hl_layout_left)"; top="$(hl_layout_top)"
	right=$((x + w)); bottom=$((y + h))
	hl_assert_true "right edge kept >=30px past the layout's left edge ($right >= $((left + 30)))" \
		"$([ "$right" -ge $((left + 30)) ] && echo true || echo false)"
	hl_assert_true "bottom edge kept >=30px past the layout's top edge ($bottom >= $((top + 30)))" \
		"$([ "$bottom" -ge $((top + 30)) ] && echo true || echo false)"
}
