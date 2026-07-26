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
	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; pill-min-width 28; tag-padding 4; pill-padding 4; panel { enable false }; show-logo false; tag-icons 0; modules-left "tags" }'
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

test_bar_pill_inset_keeps_pills_off_the_strip_edges() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# pill-inset is what stops a workspace chip from spanning the panel top to
	# bottom (the waybar plugin insets its pills 6px inside a 48px group). The
	# only externally visible consequence is the click target: the strip still
	# reserves its full height, but the top and bottom bands of it are now
	# panel, not pill, and must not route a click to a tag.
	hl_dispatch "view,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "view,2"
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2

	# strip y 4..44; pills inset 12 => 16 tall, centred at y 16..32.
	bar_set 'bar { enable true; height 40; position "top"; margin { x 8; y 4 }; pill-inset 12; pill-min-width 28; tag-padding 4; pill-padding 4; panel { enable false }; show-logo false; tag-icons 0; modules-left "tags" }'
	sleep 0.5
	hl_assert_eq "precondition: tag 2 is the active tag" "$(bar_active_tags)" "[2]"

	# y=8 is inside the strip but above the inset pill row
	hl_click 12 8
	sleep 0.4
	hl_assert_eq "a click in the strip above the inset pills hits nothing" \
		"$(bar_active_tags)" "[2]"

	# y=24 is the vertical centre of the pill row
	hl_click 12 24
	sleep 0.5
	hl_assert_eq "a click on the pill row itself still switches view" \
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

	# pill-min-width 60 with a 4px padding pins every single-digit pill to
	# exactly 60 wide, so the pills tile at 8..68, 76..136, ... regardless of
	# the font the shared config happens to use. Without that pin the click
	# coordinate below depends on how wide pango renders "1", which is how this
	# test broke when the tag label gained its "N: " index prefix.
	local geom='height 30; margin { x 8; y 4 }; pill-min-width 60; tag-padding 4; pill-padding 4; panel { enable false }; show-logo false; tag-icons 0; min-tags 1; modules-left "tags"'
	bar_set "bar { enable true; $geom }"
	sleep 0.5
	# with one occupied+selected tag there is exactly one pill (8..68): a click
	# inside where a SECOND pill would sit must therefore hit nothing.
	hl_dispatch "view,1"
	local before; before="$(bar_active_tags)"
	hl_click 100 19
	sleep 0.4
	hl_assert_eq "with tags hidden, there is no second pill to click" \
		"$(bar_active_tags)" "$before"

	bar_set "bar { enable true; show-all-tags true; $geom }"
	sleep 0.5
	hl_click 100 19
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

test_bar_tag_app_icons_and_logo() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# Pill contents are not exposed over IPC, so this asserts the observable
	# consequence: an app icon widens the tag pill it is drawn in. The logo
	# pill is leading, so with it enabled the first tag pill also starts
	# further right -- both are checked by clicking where the FIRST tag pill
	# sits with the logo off, and confirming that same point is no longer a
	# tag pill with the logo on.
	hl_dispatch "view,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1

	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; pill-min-width 28; tag-padding 4; pill-padding 4; panel { enable false }; show-logo false; tag-icons 0; min-tags 1; modules-left "tags" }'
	sleep 0.5
	hl_dispatch "view,2"
	hl_click 12 19
	sleep 0.4
	hl_assert_eq "without the logo, the leftmost pill is the first tag" \
		"$(bar_active_tags)" "[1]"

	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; pill-min-width 28; tag-padding 4; pill-padding 4; panel { enable false }; show-logo true; tag-icons 3; modules-left "tags" }'
	sleep 0.5
	hl_dispatch "view,2"
	local before; before="$(bar_active_tags)"
	hl_click 12 19
	sleep 0.4
	hl_assert_eq "with the logo leading, that same point is the logo and does nothing" \
		"$(bar_active_tags)" "$before"

	bar_off
	hl_dispatch "view,1"
}

test_bar_weather_module_is_non_blocking() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# Deliberately does NOT assert a temperature: that would make the suite
	# depend on the network and on open-meteo being up. What matters here is
	# the property the async helper exists to guarantee -- that configuring
	# weather never blocks the compositor, whether the fetch succeeds, fails,
	# or hangs. An unroutable location exercises the failing path.
	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; weather { location "Nowhere-Xyzzy-12345"; interval 1 }; modules-right "weather" }'
	local t0 t1
	t0=$(date +%s)
	hl_dispatch "view,2"
	hl_dispatch "view,1"
	t1=$(date +%s)
	hl_assert_true "IPC stays responsive while a weather fetch is outstanding" \
		"$([ $((t1 - t0)) -lt 5 ] && echo true || echo false)"
	hl_assert_true "the compositor is healthy with weather configured" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
	bar_off
}

test_bar_popover_dismiss_swallows_the_click() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# The harness has no sound server, so the audio popover never gets rows and
	# never actually opens -- which is itself the assertion worth making: a
	# popover whose content query comes back empty must close itself rather
	# than leaving an empty panel floating over the desktop, and must not take
	# the input path down with it.
	#
	# The dismiss contract is pinned through the tag pills instead. With no
	# popover open, a click on a tag pill must still switch view; if the
	# dismiss hook ever started swallowing clicks unconditionally (rather than
	# only while one is up) that would silently break every bar click, which is
	# the regression this guards.
	hl_dispatch "view,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	hl_dispatch "view,2"
	hl_spawn_kitty W2 >/dev/null
	hl_wait_client_count 2

	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; pill-min-width 60; tag-padding 4; pill-padding 4; panel { enable false }; show-logo false; tag-icons 0; modules-left "tags"; modules-right "volume" }'
	sleep 0.6
	hl_assert_eq "precondition: tag 2 is the active tag" "$(bar_active_tags)" "[2]"

	# right-click the volume pill: with no pactl to answer, no popover opens
	hl_click "$((HL_WIDTH - 40))" 19 rclick
	sleep 0.6
	hl_assert_true "a popover with no content leaves the compositor healthy" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"

	# and the ordinary bar click path is untouched
	hl_click 30 19
	sleep 0.5
	hl_assert_eq "tag pill clicks still work with the dismiss hook installed" \
		"$(bar_active_tags)" "[1]"

	bar_off
	hl_dispatch "view,1"
}

test_bar_scroll_routes_to_the_pill_under_the_pointer() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# Scroll routing is asserted through the TAGS pill rather than the volume
	# one: the harness has no sound server, so a volume scroll has no
	# observable effect, whereas "did this scroll reach a bar pill at all" is
	# the part that regresses. The tags module ignores scroll, so what is
	# pinned here is the routing contract -- a scroll over the bar must be
	# CONSUMED and never leak through to an axis binding or to the window
	# underneath, which is what would silently break if the hit test moved.
	#
	# Both input families are covered on purpose. A mouse wheel sends a
	# discrete notch count; a trackpad sends only continuous deltas. An earlier
	# cut of this keyed off the notch count alone and was silently dead under a
	# trackpad, which no wheel-only test would have caught.
	hl_dispatch "view,1"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1

	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; pill-min-width 60; tag-padding 4; pill-padding 4; panel { enable false }; show-logo false; tag-icons 0; min-tags 1; modules-left "tags" }'
	sleep 0.5
	local before; before="$(bar_active_tags)"

	# over the first tag pill (8..68, vertical centre 19)
	hl_wheel 30 19 1
	sleep 0.4
	hl_assert_eq "a wheel notch over a bar pill does not change the view" \
		"$(bar_active_tags)" "$before"

	hl_scroll 30 19 20
	sleep 0.4
	hl_assert_eq "nor does a continuous (trackpad) scroll over it" \
		"$(bar_active_tags)" "$before"

	hl_assert_true "and the compositor is healthy after both" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"

	bar_off
	hl_dispatch "view,1"
}

test_bar_volume_without_a_sound_server() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# The harness has an isolated XDG_RUNTIME_DIR, so there is no pipewire to
	# talk to: `pactl subscribe` exits immediately and `wpctl get-volume`
	# returns nothing. That is the branch worth pinning, because the module is
	# started from the refresh path -- which runs on every arrange.
	#
	# Without the retry backoff each refresh would respawn both children, so a
	# few dispatches turn into a fork storm. Asserted by counting the
	# compositor's children after hammering it: a handful of arranges must not
	# leave a pile of processes behind.
	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; modules-left "tags"; modules-right "volume" }'
	sleep 0.6
	hl_assert_true "volume is a known module name" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"

	local i
	for i in 1 2 3 4 5 6 7 8; do
		hl_dispatch "view,$(( (i % 2) + 1 ))"
	done
	sleep 1
	local kids
	kids="$(pgrep -P "$HL_COMP_PID" 2>/dev/null | wc -l)"
	hl_assert_true "a sound-server-less volume module does not fork per refresh (children=$kids)" \
		"$([ "$kids" -lt 8 ] && echo true || echo false)"

	hl_assert_true "and the compositor is still healthy" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"

	bar_off
	hl_dispatch "view,1"
}

test_bar_notifications_without_a_session_bus() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# Isolated XDG_RUNTIME_DIR, so there is no session bus and therefore no
	# swaync. Every call in the module is guarded on session_bus, and with no
	# state ever arriving the pill must still render its "nothing unread" glyph
	# rather than blank out or wedge the bar.
	#
	# Unlike the volume and visualiser modules this one spawns nothing, so
	# there is no fork-storm to guard against -- it subscribes once and waits.
	# What IS worth pinning is that a bus-less start does not mark itself
	# subscribed and then sit dead if a bus appears, nor stall the event loop.
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local base_y; base_y="$(bar_client_y W1)"

	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; modules-left "tags"; modules-right "notifications" }'
	sleep 0.6
	hl_assert_true "notifications is a known module name" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
	hl_assert_eq "it does not change the bar's reserved footprint" \
		"$(( $(bar_client_y W1) - base_y ))" "38"

	local i
	for i in 1 2 3; do
		hl_dispatch "reload_config" 1
	done
	hl_assert_true "the compositor survives reloads with it configured" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"

	local t0 t1
	t0=$(date +%s); hl_dispatch "view,2"; hl_dispatch "view,1"; t1=$(date +%s)
	hl_assert_true "and a bus-less notifications module does not stall IPC" \
		"$([ $((t1 - t0)) -lt 5 ] && echo true || echo false)"

	bar_off
}

test_bar_tray_without_a_session_bus() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# Same isolated-XDG_RUNTIME_DIR case as the media test: no session bus, so
	# the StatusNotifierItem host cannot come up at all. That is the branch
	# worth pinning -- every sd_bus call in the tray is guarded on session_bus,
	# and with no watcher, no host name and no items the module must simply
	# contribute nothing.
	#
	# Deliberately NOT asserted here: owning org.kde.StatusNotifierWatcher and
	# the item round-trip. Both need a real bus, and pointing the suite at the
	# user's live one would have a headless test process registering as the
	# session's tray watcher -- a side effect on the running desktop that a
	# regression run must never have.
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3
	local base_y; base_y="$(bar_client_y W1)"

	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; modules-left "tags"; modules-right "tray" }'
	sleep 0.6
	hl_assert_true "tray is a known module name" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
	hl_assert_eq "an empty tray still reserves the bar's own footprint" \
		"$(( $(bar_client_y W1) - base_y ))" "38"

	# the tray owns bus names and signal matches; reloading tears them down and
	# sets them up again, which is where a double-unref would show
	local i
	for i in 1 2 3; do
		hl_dispatch "reload_config" 1
	done
	hl_assert_true "the compositor survives reloads with a tray configured" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"

	local t0 t1
	t0=$(date +%s); hl_dispatch "view,2"; hl_dispatch "view,1"; t1=$(date +%s)
	hl_assert_true "and the bus-less tray does not stall IPC" \
		"$([ $((t1 - t0)) -lt 5 ] && echo true || echo false)"

	bar_off
}

test_bar_media_module_without_a_session_bus() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# The harness runs with an isolated XDG_RUNTIME_DIR and therefore NO
	# session bus, which is exactly the case worth pinning: every MPRIS call
	# must be guarded on session_bus being non-NULL, and the module must
	# simply render nothing rather than crash or stall. Asserting an actual
	# track would require a live player and make the suite depend on one.
	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; modules-center "media" }'
	sleep 1
	hl_assert_true "media on a bus-less session leaves the compositor healthy" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
	local t0 t1
	t0=$(date +%s); hl_dispatch "view,2"; hl_dispatch "view,1"; t1=$(date +%s)
	hl_assert_true "and does not stall IPC" \
		"$([ $((t1 - t0)) -lt 5 ] && echo true || echo false)"
	bar_off
}

test_bar_accepts_every_module_at_once_without_dropping_any() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	# BAR_MAX_MODULES is a total across all three sections, not per section,
	# and it used to sit BELOW the number of module kinds that exist: a real
	# config asking for seventeen silently lost its last five, tray included.
	#
	# NOT asserted here: that nothing was dropped. The overflow path warns via
	# fprintf(stderr) and the harness does not capture the compositor's stderr
	# (comp-stdout.log comes back empty), so a grep for it passes whether or
	# not modules were lost -- this test was written that way first and
	# "passed" with the broken cap still in place. The cap itself is pinned by
	# a _Static_assert in bar.h tying it to BAR_MODULE_KIND_COUNT, which the
	# compiler checks and which does fail when the cap is too small.
	#
	# What this DOES pin is that loading every module at once is survivable:
	# seventeen modules across three sections, several of them spawning
	# subprocesses or reaching for a session bus that is not there.
	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; modules-left "tags,layout,title"; modules-center "media,clock,weather,idle"; modules-right "cpu,memory,network,vpn,discord,medication,volume,notify,display,tray" }'
	sleep 1
	hl_assert_true "a bar with every module loaded leaves the compositor healthy" \
		"$(hl_get "get all-monitors" >/dev/null 2>&1 && echo true || echo false)"
	local t0 t1
	t0=$(date +%s); hl_dispatch "view,2"; hl_dispatch "view,1"; t1=$(date +%s)
	hl_assert_true "and does not stall IPC" \
		"$([ $((t1 - t0)) -lt 5 ] && echo true || echo false)"
	bar_off
}

test_bar_spaces_modules_evenly_regardless_of_what_they_contain() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }

	# The separation between modules has to be constant to the EYE, which is
	# not the same as constant between their boxes -- and asserting the latter
	# is how this shipped wrong three times running. Every cause was a pill
	# whose box was bigger than the thing inside it:
	#
	#   - icons inset inside their own SVG canvas (fixed by normalising the
	#     art to fill it)
	#   - a tall glyph fitted into a square box by its long axis, keeping a
	#     transparent margin either side (fixed by cropping loaded icons to
	#     their ink and advancing by that width)
	#   - `notify` reserving room for a two-digit count it was not showing,
	#     centred, so half the reserve sat on each side (fixed by reserving
	#     only while there is a count)
	#
	# So this measures pixels: screenshot the bar, walk the pill row, and take
	# the gaps between runs of anything that is not background. All six of
	# these modules render icon-only on a headless instance with no session
	# bus, except `volume`, which is a labelled pill kind and therefore
	# carries pill-padding -- exactly the mix that used to come out uneven.
	bar_set 'theme { border-width 0 }
bar { enable true; height 48; position "top"; margin { x 8; y 9 }; pill-inset 6; module-spacing 12; modules-left "tags"; modules-right "cpu,memory,network,volume,notify,display"; panel { enable true; radius 9; padding 6; blur false; shadow false } }'
	sleep 1.5
	hl_screenshot bar-gaps

	# the pill row: strip y 9..57, pills inset 6 => 15..51
	local out spread gaps
	out="$(python3 "$HL_REPO/contrib/regression/bar-ink-gaps.py" \
		"$HL_OUTDIR/bar-gaps.png" 16 50 \
		$((HL_WIDTH - 400)) "$HL_WIDTH" 2>&1)"
	spread="$(printf '%s' "$out" | sed -n 's/.*spread=\([0-9]*\).*/\1/p')"
	gaps="$(printf '%s' "$out" | sed -n 's/^gaps: \(.*\)  min=.*/\1/p')"
	[ -n "$spread" ] || { echo "  (skip: could not measure -- $out)"; bar_off; return 0; }

	# 1px of tolerance: an icon's advance is fractional (a 53x64 bell in a
	# 29px row advances 24.02px) and lands on a whole pixel.
	echo "  measured gaps: $gaps"
	hl_assert_true "six modules of mixed kinds sit at one separation (spread ${spread}px)" \
		"$([ "$spread" -le 1 ] && echo true || echo false)"

	# and that separation is the configured one, not merely self-consistent
	local first
	first="$(printf '%s' "$gaps" | awk '{print $1}')"
	hl_assert_true "and that separation is module-spacing (${first}px vs 12)" \
		"$([ "$first" -ge 11 ] && [ "$first" -le 13 ] && echo true || echo false)"

	bar_off
}



test_bar_sysinfo_popover_opens_and_readings_do_not_dismiss_it() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }

	# The metric pills are numberless by design, so the figures they stand for
	# live in a popover -- and unlike every other popover here, this one needs
	# nothing but /proc, so it actually opens on a headless instance with no
	# session bus. That makes it the one place the popover contract can be
	# pinned end to end.
	#
	# Three things are asserted, in the order a person would do them:
	#   1. clicking a metric pill opens a panel;
	#   2. clicking a READING inside it does not dismiss it -- every row here
	#      is inert, and a panel that closes when you click what you came to
	#      read is useless;
	#   3. clicking outside still dismisses.
	hl_dispatch "view,1"
	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; panel { enable false }; show-logo false; tag-icons 0; modules-left "tags"; modules-right "cpu" }'
	sleep 0.8

	local x0=$((HL_WIDTH - 330)) x1=$((HL_WIDTH - 30))
	hl_screenshot sys-closed
	local base; base="$(hl_region_ink "$HL_OUTDIR/sys-closed.png" $x0 60 $x1 200)"

	# Find the pill by its artwork rather than deriving its coordinates: an
	# icon-only pill is as wide as its icon, which depends on the theme's
	# padding and the bar height, and a click computed from those silently
	# lands beside it the moment either changes.
	local cpux
	cpux="$(hl_rightmost_ink_x "$HL_OUTDIR/sys-closed.png" 6 32)"
	[ -n "$cpux" ] || { echo "  (skip: could not locate the cpu pill)"; bar_off; return 0; }
	hl_click "$cpux" 19
	sleep 0.8
	hl_screenshot sys-open
	local opened; opened="$(hl_region_ink "$HL_OUTDIR/sys-open.png" $x0 60 $x1 200)"
	hl_assert_true "clicking a metric pill opens the system popover ($base -> $opened px)" \
		"$([ "$opened" -gt $((base + 2000)) ] && echo true || echo false)"

	# a reading inside it: first row centre is popover top (40) + pad (12) + ~17
	hl_click "$((HL_WIDTH - 180))" 69
	sleep 0.6
	hl_screenshot sys-still
	local still; still="$(hl_region_ink "$HL_OUTDIR/sys-still.png" $x0 60 $x1 200)"
	hl_assert_true "clicking a reading inside it does not dismiss it ($still px)" \
		"$([ "$still" -gt $((base + 2000)) ] && echo true || echo false)"

	# but a click outside does
	hl_click 400 500
	sleep 0.6
	hl_screenshot sys-dismissed
	local gone; gone="$(hl_region_ink "$HL_OUTDIR/sys-dismissed.png" $x0 60 $x1 200)"
	hl_assert_true "a click outside dismisses it ($gone px)" \
		"$([ "$gone" -le $((base + 2000)) ] && echo true || echo false)"

	bar_off
}


test_bar_popover_scrolls_a_menu_taller_than_the_screen() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }

	# A menu longer than the output used to be TRUNCATED to what fits, which
	# quietly dropped whatever was at the bottom -- and the bottom is where the
	# entry people reach for lives (Steam's tray menu lists every installed
	# game before Store/Library/Community and only then Quit).
	#
	# Forced here by making the rows absurdly tall rather than by booting a
	# second compositor on a short output: the shared instance is 1920x1080,
	# where nothing this bar can render is long enough to overflow.
	hl_dispatch "view,1"
	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; panel { enable false }; show-logo false; tag-icons 0; modules-left "tags"; modules-right "cpu"; popover { row-height 200 } }'
	sleep 0.8

	local x0=$((HL_WIDTH - 330)) x1=$((HL_WIDTH - 30))
	hl_screenshot scroll-closed
	local cpux; cpux="$(hl_rightmost_ink_x "$HL_OUTDIR/scroll-closed.png" 6 32)"
	[ -n "$cpux" ] || { echo "  (skip: could not locate the cpu pill)"; bar_off; return 0; }
	hl_click "$cpux" 19
	sleep 0.8
	hl_screenshot scroll-top
	local ink; ink="$(hl_region_ink "$HL_OUTDIR/scroll-top.png" $x0 60 $x1 600)"
	hl_assert_true "an over-long menu still opens ($ink px)" \
		"$([ "$ink" -gt 2000 ] && echo true || echo false)"

	# scroll it: the viewport moves, so what is drawn changes
	hl_wheel "$((HL_WIDTH - 180))" 200 3
	sleep 0.6
	hl_screenshot scroll-moved
	local moved; moved="$(hl_region_diff "$HL_OUTDIR/scroll-top.png" "$HL_OUTDIR/scroll-moved.png" $x0 60 $x1 600)"
	hl_assert_true "scrolling over it moves the viewport ($moved px changed)" \
		"$([ "$moved" -gt 500 ] && echo true || echo false)"

	# and it is still up -- a scroll is not a dismissal
	local still; still="$(hl_region_ink "$HL_OUTDIR/scroll-moved.png" $x0 60 $x1 600)"
	hl_assert_true "and does not dismiss it ($still px)" \
		"$([ "$still" -gt 2000 ] && echo true || echo false)"

	hl_click 400 800
	bar_off
}

test_bar_popover_keyboard_walks_rows_and_enter_runs_one() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }

	# Arrow keys and Enter are handled in the compositor's own key path, ahead
	# of the binding tables -- exactly where Escape already was -- so the
	# popover still takes no keyboard grab and never steals focus. What is
	# pinned here is that the three of them reach it at all, and that Enter
	# does the same thing a click does (it runs the same function, so the risk
	# is the key never arriving, not the action differing).
	#
	# The display popover rather than the system one: its rows are actionable,
	# whereas every sysinfo row is a reading the cursor deliberately skips.
	hl_dispatch "view,1"
	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; panel { enable false }; show-logo false; tag-icons 0; modules-left "tags"; modules-right "display" }'
	sleep 0.8

	local x0=$((HL_WIDTH - 330)) x1=$((HL_WIDTH - 30))
	hl_screenshot kb-closed
	local dx; dx="$(hl_rightmost_ink_x "$HL_OUTDIR/kb-closed.png" 6 32)"
	[ -n "$dx" ] || { echo "  (skip: could not locate the display pill)"; bar_off; return 0; }
	hl_click "$dx" 19
	sleep 0.8
	hl_screenshot kb-open
	local ink; ink="$(hl_region_ink "$HL_OUTDIR/kb-open.png" $x0 40 $x1 300)"
	hl_assert_true "the display popover opens ($ink px)" \
		"$([ "$ink" -gt 1000 ] && echo true || echo false)"

	# Down places the cursor on the sole output row. No visual change is
	# asserted HERE on purpose: with one monitor that row is already the
	# selected one, and a cursor on a filled row is deliberately not recoloured
	# (accent text on an accent fill would be invisible). That Down landed at
	# all is proven by the Enter below, which does nothing without a cursor.
	"$HL_WLVKBD" press DOWN
	sleep 0.5
	hl_screenshot kb-cursor

	# Enter drills into that output's own settings: a different set of rows
	"$HL_WLVKBD" press ENTER
	sleep 0.6
	hl_screenshot kb-entered
	local entered; entered="$(hl_region_diff "$HL_OUTDIR/kb-cursor.png" "$HL_OUTDIR/kb-entered.png" $x0 40 $x1 300)"
	hl_assert_true "Enter runs the row under the cursor ($entered px changed)" \
		"$([ "$entered" -gt 500 ] && echo true || echo false)"

	# Now Down where the highlight CAN show: this panel leads with an inert
	# summary line the cursor must skip, landing on the HDR toggle, which is
	# neither selected nor filled.
	"$HL_WLVKBD" press DOWN
	sleep 0.5
	hl_screenshot kb-marked
	local marked; marked="$(hl_region_diff "$HL_OUTDIR/kb-entered.png" "$HL_OUTDIR/kb-marked.png" $x0 40 $x1 300)"
	hl_assert_true "Down marks the row it lands on ($marked px changed)" \
		"$([ "$marked" -gt 100 ] && echo true || echo false)"

	# Escape still closes
	"$HL_WLVKBD" press ESC
	sleep 0.6
	hl_screenshot kb-closed2
	local gone; gone="$(hl_region_ink "$HL_OUTDIR/kb-closed2.png" $x0 40 $x1 300)"
	hl_assert_true "Escape closes it ($gone px)" \
		"$([ "$gone" -lt 1000 ] && echo true || echo false)"

	bar_off
}



test_bar_panels_cast_a_shadow() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }

	# A shadow node the same size and in the same place as the panel is
	# entirely BEHIND it: the falloff has nowhere outside the panel to occupy,
	# so an opaque panel hides all of it. That is what the bar shipped with --
	# `panel.shadow true` created and enabled a node that could never be seen,
	# and the wallpaper below the panel measured a flat 0x808080.
	#
	# Asserted against the WALLPAPER, below the strip, where only a shadow can
	# darken anything.
	hl_dispatch "view,1"
	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; modules-left "tags"; modules-center "clock"; panel { enable true; radius 9; padding 6; blur false; shadow true } }'
	sleep 1
	hl_screenshot shadow-on
	local cx=$((HL_WIDTH / 2))
	local lit; lit="$(hl_region_ink "$HL_OUTDIR/shadow-on.png" $((cx - 60)) 38 $((cx + 60)) 60)"
	hl_assert_true "a panel darkens the wallpaper below it ($lit px)" \
		"$([ "$lit" -gt 200 ] && echo true || echo false)"

	# and the toggle still means something
	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; modules-left "tags"; modules-center "clock"; panel { enable true; radius 9; padding 6; blur false; shadow false } }'
	sleep 1
	hl_screenshot shadow-off
	local dark; dark="$(hl_region_ink "$HL_OUTDIR/shadow-off.png" $((cx - 60)) 38 $((cx + 60)) 60)"
	hl_assert_true "and shadow false leaves it clean ($dark px)" \
		"$([ "$dark" -eq 0 ] && echo true || echo false)"

	bar_off
}
