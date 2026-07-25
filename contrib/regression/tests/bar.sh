# bar.sh — the compositor-native status bar (src/draw/bar.h).
#
# The bar is deliberately NOT enabled in the shared test config: turning it on
# globally would shrink the usable area for every other module and silently
# break the geometry/layout tests that assert against it (the same trap the
# multi-monitor topology hits -- see multimonitor.sh). Each test here enables
# the bar itself via a config rewrite + reload_config, and puts it back
# afterwards.
#
# Covers: space reservation at the top and bottom, the runtime enable toggle,
# survival across repeated reloads (a double free in bar_pill_release aborted
# in glibc on the first reload -- the tab-bar node already frees the hit-test
# tag passed to _create, so the bar must not free it again), and click routing
# from a tag pill through to a view switch.
#
# Skipped automatically when the binary was built with -Dnative-bar=false:
# the config keys do not exist in that build, so there is nothing to test.

BAR_PRISTINE=""

# Rewrite the shared config with $1 appended as the bar block, then reload.
bar_set() { # bar_set "<kdl fragment>"
	[ -n "$BAR_PRISTINE" ] || {
		BAR_PRISTINE="$HL_OUTDIR/config.pristine.kdl"
		cp "$HL_CONFIG" "$BAR_PRISTINE"
	}
	cp "$BAR_PRISTINE" "$HL_CONFIG"
	printf '%s\n' "$1" >> "$HL_CONFIG"
	hl_dispatch "reload_config" 1
}

bar_off() { bar_set ""; }

# Does this build have the bar compiled in? An unknown config key is warned
# about and ignored, so probe the binary instead of guessing.
bar_supported() {
	if strings "$HL_ASTEROIDZ" | grep -q '^bar_modules_left$'; then
		echo true
	else
		echo false
	fi
}

bar_client_y() { hl_get "get all-clients" | jq -r ".clients[] | select(.title==\"$1\") | .y"; }
bar_client_h() { hl_get "get all-clients" | jq -r ".clients[] | select(.title==\"$1\") | .height"; }
bar_active_tags() { hl_get "get all-monitors" | jq -c ".monitors[0].active_tags"; }

test_bar_reserves_space_at_the_top() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local base_y base_h
	base_y="$(bar_client_y W1)"
	base_h="$(bar_client_h W1)"

	# height 30 + 2*margin-y 4 = 38 reserved
	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; modules-left "tags"; modules-center "clock" }'
	sleep 0.5
	local y h
	y="$(bar_client_y W1)"
	h="$(bar_client_h W1)"
	hl_assert_eq "a top bar pushes the tiled area down by its full footprint" \
		"$((y - base_y))" "38"
	hl_assert_eq "and takes the same amount out of the tiled height" \
		"$((base_h - h))" "38"

	bar_off
	sleep 0.5
	hl_assert_eq "disabling the bar gives the space back" \
		"$(bar_client_y W1)" "$base_y"
}

test_bar_at_the_bottom_reserves_from_the_bottom() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local base_y base_h
	base_y="$(bar_client_y W1)"
	base_h="$(bar_client_h W1)"

	bar_set 'bar { enable true; height 30; position "bottom"; margin { x 8; y 4 }; modules-left "tags" }'
	sleep 0.5
	hl_assert_eq "a bottom bar leaves the top of the tiled area alone" \
		"$(bar_client_y W1)" "$base_y"
	hl_assert_eq "but still takes its footprint out of the height" \
		"$((base_h - $(bar_client_h W1)))" "38"
	bar_off
}

test_bar_survives_repeated_reloads() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# Regression pin: bar_pill_release used to free the AsteroidzNodeData that
	# asteroidz_tab_bar_node_destroy already frees, so the FIRST reload with a
	# bar configured aborted in glibc. Reload several times with pills alive.
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	bar_set 'bar { enable true; height 30; modules-left "tags,layout"; modules-center "clock"; modules-right "title" }'
	local i
	for i in 1 2 3; do
		hl_dispatch "reload_config" 1
	done
	hl_assert_true "the compositor is still alive after 4 reloads with a bar" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
	bar_off
}

test_bar_tag_pill_click_switches_view() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# One window on tag 1, then move to tag 2 with another, so the tags module
	# renders a pill for tag 1 (occupied) before the selected tag 2 pill --
	# the leftmost pill is therefore tag 1 and clicking it must switch back.
	hl_dispatch "view,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "view,2"
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2

	# panels off: they inset the pill row by panel-padding, and this test is
	# about click routing, not appearance. With them off the first pill starts
	# exactly at margin-x, so the click coordinate below follows from config
	# alone rather than from the panel geometry of the day.
	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; pill-min-width 28; panel { enable false }; modules-left "tags" }'
	sleep 0.5
	hl_assert_eq "precondition: tag 2 is the active tag" "$(bar_active_tags)" "[2]"

	# The first pill starts at margin-x (8) and is >= pill-min-width (28)
	# wide; its vertical centre is margin-y + height/2 = 19. Clicking a few
	# pixels in is comfortably inside it regardless of the label's width.
	hl_click 12 19
	sleep 0.5
	hl_assert_eq "clicking the leftmost tag pill views that tag" \
		"$(bar_active_tags)" "[1]"

	bar_off
	hl_dispatch "view,1"
}

test_bar_show_all_tags() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# Default: an empty, unselected tag gets no pill, so a session with a
	# single occupied tag renders a single pill. show-all-tags overrides that.
	# Asserted through the reserved width rather than pixels: more pills means
	# a wider left slot, which is the only externally visible consequence.
	hl_dispatch "view,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1

	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; pill-min-width 28; panel { enable false }; modules-left "tags" }'
	sleep 0.5
	# with one occupied+selected tag there is exactly one pill: a click just
	# left of where a SECOND pill would start must therefore hit nothing.
	hl_dispatch "view,1"
	local before; before="$(bar_active_tags)"
	hl_click 50 19
	sleep 0.4
	hl_assert_eq "with tags hidden, there is no second pill to click" \
		"$(bar_active_tags)" "$before"

	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; pill-min-width 28; show-all-tags true; panel { enable false }; modules-left "tags" }'
	sleep 0.5
	hl_click 50 19
	sleep 0.4
	hl_assert_eq "with show-all-tags, the second pill exists and views tag 2" \
		"$(bar_active_tags)" "[2]"

	bar_off
	hl_dispatch "view,1"
}

test_bar_panel_respects_the_margin() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# Regression pin: the panel used to be padded on ALL four sides, so with
	# margin.y == panel.padding its top landed at y=0 and it sat flush against
	# the screen edge with no gap at all. Padding is horizontal only now; the
	# gap above comes from margin.y alone. Asserted through the reserved area,
	# which is the only part of this the IPC can see: the footprint is
	# height + 2*margin.y regardless of panel padding.
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local base_y; base_y="$(bar_client_y W1)"

	bar_set 'bar { enable true; height 30; margin { x 8; y 9 }; panel { enable true; padding 6 }; modules-left "tags" }'
	sleep 0.5
	hl_assert_eq "panel padding does not change the reserved footprint" \
		"$(( $(bar_client_y W1) - base_y ))" "48"
	bar_off
}

test_bar_metric_modules() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# cpu/memory/network read /proc and /sys directly -- no subprocess, which
	# is what makes them safe to run on the compositor's own event loop. There
	# is no IPC surface exposing pill text, so this asserts what is observable:
	# the modules parse, the sampling timer arms, repeated reloads (which
	# re-arm it) are survivable, and they do not disturb the reserved area.
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local base_y; base_y="$(bar_client_y W1)"

	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; interval 1; modules-left "tags"; modules-right "cpu,memory,network" }'
	sleep 1.5
	hl_assert_eq "metric modules do not change the reserved footprint" \
		"$(( $(bar_client_y W1) - base_y ))" "38"

	local i
	for i in 1 2; do hl_dispatch "reload_config" 1; done
	hl_assert_true "the compositor survives reloads with the sampling timer armed" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"

	# an unknown module name must warn and be skipped, not refuse to start
	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; modules-left "tags,not_a_real_module" }'
	sleep 0.5
	hl_assert_true "an unknown module name is skipped rather than fatal" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
	bar_off
}

test_bar_idle_inhibitor() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# The pill mirrors a compositor-level flag rather than the protocol state,
	# so the dispatch is the observable surface. Force both states explicitly
	# rather than toggling, so the test cannot depend on what ran before it.
	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; modules-center "idle" }'
	sleep 0.3
	hl_dispatch "toggle_idle_inhibit,1"
	hl_assert_true "forcing idle inhibit on is accepted" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
	hl_dispatch "toggle_idle_inhibit,0"
	hl_dispatch "toggle_idle_inhibit"
	hl_assert_true "toggling it back and forth leaves the compositor healthy" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
	hl_dispatch "toggle_idle_inhibit,0"
	bar_off
}
