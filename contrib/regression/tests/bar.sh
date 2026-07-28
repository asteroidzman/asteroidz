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

test_bar_config_keys_are_all_reachable() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }

	# Every bar setting needs THREE things that can drift apart: a field, a
	# parse_option branch, and a row in the KDL name table. Miss the last and
	# the key is accepted by the file but never reaches the config -- the
	# option silently does nothing, which is the failure that looks like a bug
	# in the feature rather than in its wiring.
	#
	# `-p` is the compositor's own config check and reports an unknown keyword
	# on stderr, so this asks the binary rather than reading the table.
	local cfg="$HL_OUTDIR/keycheck.kdl"
	cat > "$cfg" <<'KDL'
bar {
    enable true
    modules-left "tags"
    modules-right "network,weather,media"
    modules-right-monitor "focused"
    network { max-down 266.4; max-up 222.8 }
    weather { interval 15 }
    media { bars 6; visualiser true }
    popover { width 340; row-height 34 }
    tooltip { enable true; delay 500 }
    discord { daemon-cmd "" }
}
KDL
	local out
	out="$("$HL_ASTEROIDZ" -p -c "$cfg" 2>&1)"
	hl_assert_true "every bar config key in the docs is reachable ($(echo "$out" | tr '\n' ' ' | cut -c1-80))" \
		"$(echo "$out" | grep -qi "unknown keyword" && echo false || echo true)"
}

test_bar_idle_inhibitor_stays_visible_when_it_is_on() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }

	# Turning the inhibitor ON used to ERASE it. The pill took the "active"
	# look, which fills it with the accent colour, while the module tinted the
	# mug with that same accent -- so the state you can act on was the one
	# state you could not see, a blank chip. The bell had the identical bug
	# for the identical reason; both are flat now, with the artwork carrying
	# the state.
	#
	# Asserted relatively rather than against a colour constant: the mug is
	# the mug in both states, so its ink area must stay in the same ballpark.
	# A filled chip is a ~7x jump in non-background pixels and fails loudly.
	bar_set 'theme { border-width 0 }
bar { enable true; height 48; position "top"; margin { x 8; y 9 }; pill-inset 6; modules-left "tags"; modules-right "idle"; panel { enable true; radius 9; padding 6; blur false; shadow false } }'
	sleep 1.2
	hl_dispatch "toggle_idle_inhibit,0" 1
	hl_screenshot idle-off
	local x
	x=$(hl_rightmost_ink_x "$HL_OUTDIR/idle-off.png" 16 50)
	hl_dispatch "toggle_idle_inhibit,1" 1
	hl_screenshot idle-on

	local verdict
	verdict=$(python3 - "$HL_OUTDIR/idle-off.png" "$HL_OUTDIR/idle-on.png" "$x" <<'PY'
import sys
from PIL import Image
off, on, x = (Image.open(sys.argv[1]).convert("RGB"),
              Image.open(sys.argv[2]).convert("RGB"), int(sys.argv[3]))
x0, x1, y0, y1 = max(x - 40, 0), x + 4, 12, 52
def hist(im):
    px = im.load()
    h = {}
    for xx in range(x0, x1):
        for yy in range(y0, y1):
            h[px[xx, yy]] = h.get(px[xx, yy], 0) + 1
    return h
h_off = hist(off)
bg = max(h_off.items(), key=lambda kv: kv[1])[0]      # the panel behind the pill
ink_off = sum(v for k, v in h_off.items() if k != bg)
ink_on = sum(v for k, v in hist(on).items() if k != bg)
print("true" if 0 < ink_on < 2 * ink_off else f"false ({ink_off} -> {ink_on})")
PY
)
	hl_assert_true "the idle mug is still artwork once the inhibitor is on ($verdict)" \
		"${verdict%% *}"
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
	# daemon-cmd "" on purpose: the discord module SPAWNS the voice daemon when
	# nothing is serving its socket, and this instance's runtime dir is empty
	# by construction -- so without this the suite logged a second Discord
	# session in, with the developer's real token, on every run.
	bar_set 'bar { enable true; height 30; margin { x 8; y 4 }; modules-left "tags,layout,title"; modules-center "media,clock,weather,idle"; modules-right "cpu,memory,network,vpn,discord,medication,volume,notify,display,tray"; discord { daemon-cmd "" } }'
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

# Write a synthetic `channels` snapshot: 13 servers x 10 voice channels, with
# $2 participants in each. Shaped exactly like the daemon's own output, down to
# the global sort by channel position, so the bar cannot tell it apart.
voice_snapshot() { # voice_snapshot FILE NPARTICIPANTS
	python3 - "$1" "$2" <<'PY'
import json, sys
n = int(sys.argv[2])
guilds = [{"id": str(100 + g), "name": f"Server {g:02d}", "position": 0}
          for g in range(13)]
def people(g, c):
    return [{"id": str(900000 + g * 100 + c * 10 + i), "name": f"Someone {i}"}
            for i in range(n)]
channels = [{"id": str(10000 + g * 100 + c), "guild": str(100 + g),
             "name": f"Channel {c:02d}", "position": c,
             "parent": "0", "participants": people(g, c)}
            for g in range(13) for c in range(10)]
# the daemon's own ordering: position across every server, not grouped
channels.sort(key=lambda c: (c["position"], c["name"].lower()))
with open(sys.argv[1], "w") as f:
    f.write(json.dumps({"event": "ready", "username": "tester"}) + "\n")
    f.write(json.dumps({"event": "status", "state": "idle"}) + "\n")
    f.write(json.dumps({"event": "channels", "guilds": guilds,
                        "channels": channels}) + "\n")
PY
}

# Stand in for the voice daemon: hand every client the snapshot and hold the
# connection open. Sets VOICE_SOCAT_PID; stop it with voice_stop.
voice_serve() { # voice_serve SNAPSHOT_FILE
	rm -f "$HL_XDG/discord-voiced.sock"
	socat UNIX-LISTEN:"$HL_XDG/discord-voiced.sock",fork \
		SYSTEM:"cat $1; sleep 120" >/dev/null 2>&1 &
	VOICE_SOCAT_PID=$!
}

# Children FIRST, and by parent pid, never by name. socat with `fork` hands
# each connection to a child process, and killing only the listener leaves the
# bar's own connection alive and served -- which is how the headcount test
# first "passed" its swap without the bar ever seeing the second snapshot.
# Count near-white pixels in a region: the LABELS, not the chips they sit on.
# hl_region_ink cannot see a headcount -- a menu row is a filled rect either
# way, so "(4)" appears inside pixels that already counted as ink and the two
# menus measure identical to the pixel. Text is the only thing that changes.
voice_label_ink() { # voice_label_ink PNG X0 Y0 X1 Y1
	python3 - "$@" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
x1, y1 = min(x1, im.size[0]), min(y1, im.size[1])
print(sum(1 for x in range(x0, x1) for y in range(y0, y1)
          if min(px[x, y]) > 200))
PY
}

voice_stop() {
	[ -n "${VOICE_SOCAT_PID:-}" ] || return 0
	local kid
	for kid in $(pgrep -P "$VOICE_SOCAT_PID" 2>/dev/null); do
		pkill -P "$kid" 2>/dev/null   # the SYSTEM: shell under the child
		kill "$kid" 2>/dev/null
	done
	kill "$VOICE_SOCAT_PID" 2>/dev/null
	VOICE_SOCAT_PID=""
}

test_bar_weather_popover_shows_the_week() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	# open-meteo is the module's only data source and there is no way to point
	# it somewhere else, so this test needs the network. Skipped, not failed,
	# when there is none: a suite that goes red on a train is a suite people
	# stop running.
	curl -sS --max-time 6 -o /dev/null \
		"https://api.open-meteo.com/v1/forecast?latitude=0&longitude=0&current=temperature_2m" \
		2>/dev/null || { echo "  (skip: no network / open-meteo unreachable)"; return 0; }

	# The pill is a glyph and a temperature; everything the request already
	# returns -- conditions, metrics, seven days -- lives in the popover, as it
	# did in the waybar plugin. A popover with only the two current-condition
	# rows means the daily block was dropped on the way in.
	bar_set 'theme { border-width 0 }
bar { enable true; height 48; position "top"; margin { x 8; y 9 }; pill-inset 6; modules-left "tags"; modules-right "weather"; panel { enable true; radius 9; padding 6; blur false; shadow false } }'
	local i temp=""
	for i in $(seq 1 30); do
		sleep 1
		hl_screenshot wx-wait
		# the pill renders "--°" until the first fetch lands; once it has a
		# reading the ink to the left of the panel edge grows
		temp=$(hl_region_ink "$HL_OUTDIR/wx-wait.png" $((HL_WIDTH - 200)) 16 $((HL_WIDTH - 8)) 50)
		[ "${temp:-0}" -gt 200 ] && break
	done
	[ "${temp:-0}" -gt 200 ] || { echo "  (skip: no forecast arrived in 30s)"; bar_off; return 0; }

	hl_screenshot wx-closed
	local px; px="$(hl_rightmost_ink_x "$HL_OUTDIR/wx-closed.png" 16 50)"
	hl_click $((px - 20)) 32
	sleep 1.5
	hl_screenshot wx-open
	local x0=$((px - 460)) x1=$((px + 10))
	[ $x0 -lt 0 ] && x0=0
	local ink; ink="$(hl_region_ink "$HL_OUTDIR/wx-open.png" $x0 60 $x1 700)"
	hl_assert_true "the weather pill opens a forecast panel ($ink px)" \
		"$([ "$ink" -gt 3000 ] && echo true || echo false)"

	# Seven days plus the current block is a TALL panel. Measuring its height
	# is what separates "the forecast is in there" from "two rows opened".
	local rows
	rows=$(python3 - "$HL_OUTDIR/wx-open.png" "$x0" "$x1" <<'PYEOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
x0, x1 = int(sys.argv[2]), int(sys.argv[3])
w, h = im.size
bg = px[10, h - 10]                       # wallpaper, well away from the bar
last = 0
for y in range(60, h):
    if any(px[x, y] != bg for x in range(x0, min(x1, w))):
        last = y
print(last)
PYEOF
)
	hl_assert_true "and it is tall enough to hold the week (panel reaches y=$rows)" \
		"$([ "${rows:-0}" -gt 400 ] && echo true || echo false)"

	hl_click 400 800
	bar_off
}

test_bar_voice_menu_holds_a_real_accounts_worth_of_channels() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v socat >/dev/null || { echo "  (skip: socat not available)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }

	# Two caps used to cut this list: 64 parsed channels and 64 popover rows.
	# A dozen servers is 119 channels plus a header each, and the daemon emits
	# them sorted by channel position across ALL servers rather than grouped,
	# so truncating took a bite out of every server at once and left the last
	# few with no header at all -- they looked like servers the account was not
	# in. The symptom was reported as "why is <server> not in the list".
	#
	# Served by socat from a synthetic snapshot: no daemon, no token, no
	# network. The bar cannot tell the difference -- it reads newline JSON off
	# a unix socket, which is exactly what this is.
	local snap="$HL_OUTDIR/dv-snapshot.jsonl"
	voice_snapshot "$snap" 0
	voice_serve "$snap"
	local socatpid=$VOICE_SOCAT_PID

	bar_set 'theme { border-width 0 }
bar { enable true; height 48; position "top"; margin { x 8; y 9 }; pill-inset 6; modules-left "tags"; modules-right "discord"; discord { daemon-cmd "" }; panel { enable true; radius 9; padding 6; blur false; shadow false } }'
	sleep 4

	hl_screenshot voice-closed
	local px; px="$(hl_rightmost_ink_x "$HL_OUTDIR/voice-closed.png" 16 50)"
	[ -n "$px" ] || { echo "  (skip: could not locate the discord pill)"
		voice_stop; bar_off; return 0; }
	hl_click $((px - 10)) 32
	sleep 1.2
	hl_screenshot voice-open

	local x0=$((px - 320)) x1=$((px + 10))
	[ $x0 -lt 0 ] && x0=0
	local ink; ink="$(hl_region_ink "$HL_OUTDIR/voice-open.png" $x0 60 $x1 1000)"
	hl_assert_true "the join menu opens with channels in it ($ink px)" \
		"$([ "$ink" -gt 2000 ] && echo true || echo false)"

	# Scroll well past where the old 64-row menu ended, twice. The first hop
	# moves under either cap; the second only moves if the list really is
	# longer than the cut used to allow.
	hl_wheel $((px - 100)) 200 40
	sleep 0.8
	hl_screenshot voice-s40
	hl_wheel $((px - 100)) 200 40
	sleep 0.8
	hl_screenshot voice-s80
	local deep
	deep="$(hl_region_diff "$HL_OUTDIR/voice-s40.png" "$HL_OUTDIR/voice-s80.png" $x0 60 $x1 1000)"
	hl_assert_true "and keeps scrolling past the old 64-row cut ($deep px changed)" \
		"$([ "$deep" -gt 1000 ] && echo true || echo false)"

	hl_click 400 800
	voice_stop
	rm -f "$HL_XDG/discord-voiced.sock"
	bar_off
}

test_bar_voice_menu_offers_to_connect_when_the_daemon_is_logged_out() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v socat >/dev/null || { echo "  (skip: socat not available)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }

	# The daemon starts logged OUT now, so a running daemon reporting "offline"
	# is the NORMAL state at session start rather than a fault. The menu has to
	# tell that apart from no daemon at all: one offers Connect, the other
	# offers to start a daemon. Both are one row, so this measures that the
	# menu opens with something in it at all -- an empty popover closes itself,
	# which is what "clicking the pill does nothing" looked like before.
	local snap="$HL_OUTDIR/dv-offline.jsonl"
	printf '%s\n' '{"event":"status","state":"offline"}' > "$snap"
	voice_serve "$snap"

	bar_set 'theme { border-width 0 }
bar { enable true; height 48; position "top"; margin { x 8; y 9 }; pill-inset 6; modules-left "tags"; modules-right "discord"; discord { daemon-cmd "" }; panel { enable true; radius 9; padding 6; blur false; shadow false } }'
	sleep 4

	hl_screenshot dv-off-closed
	local px; px="$(hl_rightmost_ink_x "$HL_OUTDIR/dv-off-closed.png" 16 50)"
	[ -n "$px" ] || { echo "  (skip: could not locate the discord pill)"
		voice_stop; bar_off; return 0; }
	hl_click $((px - 10)) 32
	sleep 1.2
	hl_screenshot dv-off-open
	local x0=$((px - 400)) x1=$((px + 10))
	[ $x0 -lt 0 ] && x0=0
	local ink; ink="$(hl_region_ink "$HL_OUTDIR/dv-off-open.png" $x0 60 $x1 400)"
	hl_assert_true "a logged-out daemon still opens a menu with actions in it ($ink px)" \
		"$([ "$ink" -gt 1500 ] && echo true || echo false)"

	hl_click 400 800
	voice_stop
	rm -f "$HL_XDG/discord-voiced.sock"
	bar_off
}

test_bar_voice_menu_shows_who_is_in_a_channel() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v socat >/dev/null || { echo "  (skip: socat not available)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }

	# The headcount beside a channel is the whole reason to look at this menu
	# before joining, and it read 0 for every channel on a busy account: the
	# daemon calls the field `participants` and the bar was counting `people`,
	# a key nothing sends. Reading a missing key is silent in cJSON -- NULL,
	# count zero, an occupied channel drawn as an empty one.
	#
	# Asserted by INK, not by reading the digits: the same menu is served
	# twice, once with empty channels and once with four people in each, and
	# " (4)" on every row is a large, one-directional change in how much of
	# the panel is painted. A regression puts the two back at parity.
	local empty="$HL_OUTDIR/dv-empty.jsonl" busy="$HL_OUTDIR/dv-busy.jsonl"
	voice_snapshot "$empty" 0
	voice_snapshot "$busy" 4

	local cfg='theme { border-width 0 }
bar { enable true; height 48; position "top"; margin { x 8; y 9 }; pill-inset 6; modules-left "tags"; modules-right "discord"; discord { daemon-cmd "" }; panel { enable true; radius 9; padding 6; blur false; shadow false } }'

	voice_serve "$empty"
	bar_set "$cfg"
	sleep 4
	hl_screenshot heads-closed
	local px; px="$(hl_rightmost_ink_x "$HL_OUTDIR/heads-closed.png" 16 50)"
	[ -n "$px" ] || { echo "  (skip: could not locate the discord pill)"
		voice_stop; bar_off; return 0; }
	local x0=$((px - 400)) x1=$((px + 10))
	[ $x0 -lt 0 ] && x0=0

	hl_click $((px - 10)) 32
	sleep 1.2
	hl_screenshot heads-empty
	local ink_empty
	ink_empty="$(voice_label_ink "$HL_OUTDIR/heads-empty.png" $x0 60 $x1 1000)"
	hl_click 400 800
	sleep 0.5

	# Swap the snapshot under it. The reload is what forces the module to
	# reconnect NOW: left to itself the client backs off up to 8s and the
	# window between "socket is back" and "channels have arrived" is not
	# something a sleep should be asked to straddle.
	voice_stop
	voice_serve "$busy"
	bar_set "$cfg"
	sleep 5
	hl_click $((px - 10)) 32
	sleep 1.2
	hl_screenshot heads-busy
	local ink_busy
	ink_busy="$(voice_label_ink "$HL_OUTDIR/heads-busy.png" $x0 60 $x1 1000)"

	# " (4)" on every row is ~25% more label ink; a tenth is well clear of
	# antialiasing noise and nowhere near reachable by an unchanged menu.
	hl_assert_true "channels with people in them say so ($ink_empty -> $ink_busy px of label)" \
		"$([ "$ink_busy" -gt $((ink_empty + ink_empty / 10)) ] && echo true || echo false)"

	hl_click 400 800
	voice_stop
	rm -f "$HL_XDG/discord-voiced.sock"
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

test_bar_tooltip_appears_on_hover_and_leaves_on_exit() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }

	# The bar is deliberately terse -- no number on a metric, no count on the
	# bell, a capped title -- and the tooltip is where the answers live. What
	# is pinned here is the hover contract itself: it appears only after the
	# pointer SETTLES, and it goes when the pointer does. A tooltip that
	# outlives the hover would sit over the desktop forever.
	hl_dispatch "view,1"
	bar_set 'bar { enable true; height 30; position "top"; margin { x 8; y 4 }; panel { enable true; radius 9; padding 12; blur false; shadow false }; show-logo false; tag-icons 0; modules-left "tags"; modules-center "clock"; tooltip { enable true; delay 200 } }'
	sleep 1

	local cx=$((HL_WIDTH / 2))
	# below the strip (4+30) and its popover gap: only a tooltip lives here
	local y0=42 y1=80
	hl_screenshot tip-none
	local base; base="$(hl_region_ink "$HL_OUTDIR/tip-none.png" $((cx - 200)) $y0 $((cx + 200)) $y1)"

	hl_move "$cx" 19
	sleep 1
	hl_screenshot tip-shown
	local shown; shown="$(hl_region_ink "$HL_OUTDIR/tip-shown.png" $((cx - 200)) $y0 $((cx + 200)) $y1)"
	hl_assert_true "hovering a pill shows a tooltip below the bar ($base -> $shown px)" \
		"$([ "$shown" -gt $((base + 500)) ] && echo true || echo false)"

	# leave the bar entirely
	hl_move "$cx" 600
	sleep 0.6
	hl_screenshot tip-gone
	local gone; gone="$(hl_region_ink "$HL_OUTDIR/tip-gone.png" $((cx - 200)) $y0 $((cx + 200)) $y1)"
	hl_assert_true "and moving off it takes the tooltip away ($gone px)" \
		"$([ "$gone" -le $((base + 500)) ] && echo true || echo false)"

	bar_off
}

# ─── plugins (custom modules, src/draw/bar-custom.h) ────────────────────────

# A plugin is a command, so these tests write real scripts and let the
# compositor run them. That is the point of the feature and also its risk: an
# `exec` that blocks would block the event loop, so every assertion below is
# also implicitly a check that the compositor is still answering IPC.
bar_plugin_dir() {
	local d="$HL_OUTDIR/plugins"
	mkdir -p "$d"
	echo "$d"
}

# Ink across the left end of the bar, where these tests put their pill.
bar_plugin_ink() { # bar_plugin_ink NAME
	hl_screenshot "$1"
	hl_region_ink "$HL_OUTDIR/$1.png" 8 4 400 44
}

test_bar_plugin_renders_plain_text() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	local d; d="$(bar_plugin_dir)"
	# Not JSON: the whole of stdout is the label. A two-line shell script has
	# to be a valid plugin or the feature is only for people who can emit JSON.
	printf '#!/bin/sh\necho PLUGINTEXT\n' > "$d/plain"
	chmod +x "$d/plain"

	bar_set "bar { enable true; height 36; position \"top\"; margin { x 8; y 4 }; panel { enable false }; modules-left \"custom/p1\"; custom \"p1\" { exec \"$d/plain\" } }"
	sleep 1.5
	local ink; ink="$(bar_plugin_ink plug-plain)"
	hl_assert_true "a plugin printing plain text renders it as the pill label ($ink px)" \
		"$([ "${ink:-0}" -gt 100 ] && echo true || echo false)"

	bar_off
}

test_bar_plugin_json_fields_are_honoured() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	local d; d="$(bar_plugin_dir)"
	printf '#!/bin/sh\necho %s\n' "'{\"text\":\"JSONTEXT\"}'" > "$d/json"
	printf '#!/bin/sh\necho %s\n' "'{\"text\":\"JSONTEXT\",\"hidden\":true}'" > "$d/hidden"
	chmod +x "$d/json" "$d/hidden"

	local cfg='bar { enable true; height 36; position "top"; margin { x 8; y 4 }; panel { enable false }; modules-left "custom/p1"'
	bar_set "$cfg; custom \"p1\" { exec \"$d/json\" } }"
	sleep 1.5
	local shown; shown="$(bar_plugin_ink plug-json)"
	hl_assert_true "a JSON \"text\" field becomes the label ($shown px)" \
		"$([ "${shown:-0}" -gt 100 ] && echo true || echo false)"

	# "hidden" is the plugin saying it has nothing to show. It must render
	# NOTHING -- not an empty pill holding a slot, which is what a module that
	# merely blanked its text would leave behind.
	bar_set "$cfg; custom \"p1\" { exec \"$d/hidden\" } }"
	sleep 1.5
	local gone; gone="$(bar_plugin_ink plug-hidden)"
	hl_assert_true "\"hidden\":true takes the pill off the bar entirely ($gone px)" \
		"$([ "${gone:-0}" -lt 20 ] && echo true || echo false)"

	bar_off
}

test_bar_plugin_updates_on_its_interval() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	local d; d="$(bar_plugin_dir)"
	# The bar's redraw is gated on a digest of everything it displays, so a
	# plugin whose state changed but whose hash did not would update silently
	# never. This is that gate: the script's output changes on every run, and
	# the pill has to follow it.
	printf '#!/bin/sh\nn=$(cat %s/counter 2>/dev/null || echo 0)\nn=$((n+1))\necho $n > %s/counter\n[ $n -ge 2 ] && echo WWWWWWWWWWWWWWWW || echo i\n' "$d" "$d" > "$d/tick"
	chmod +x "$d/tick"
	rm -f "$d/counter"

	# Timings have margin on purpose. The first run happens on the first
	# metrics tick whatever `interval` says (nothing is known yet), so with
	# interval 3 the narrow output is on screen from ~1s to ~4s and the wide
	# one from ~4s on. Sampling at 2s and 6s sits well inside both windows --
	# an earlier version used interval 1 and a 1.5s first sample, which the
	# cost of taking a screenshot was enough to push past the second run, so
	# it measured the wide string twice and read as "never updates".
	bar_set "bar { enable true; height 36; position \"top\"; margin { x 8; y 4 }; interval 1; panel { enable false }; modules-left \"custom/p1\"; custom \"p1\" { exec \"$d/tick\"; interval 3 } }"
	sleep 2
	local first; first="$(bar_plugin_ink plug-tick1)"
	sleep 4
	local second; second="$(bar_plugin_ink plug-tick2)"
	hl_assert_true "an interval plugin redraws when its output changes ($first -> $second px)" \
		"$([ "${second:-0}" -gt $(( ${first:-0} + 100 )) ] && echo true || echo false)"

	bar_off
	rm -f "$d/counter"
}

test_bar_plugin_click_runs_its_command() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	local d; d="$(bar_plugin_dir)"
	printf '#!/bin/sh\necho CLICKME\n' > "$d/clicky"
	chmod +x "$d/clicky"
	rm -f "$d/clicked"

	# Panels off and the plugin alone on the left, so the pill starts at
	# margin-x and the click coordinate follows from the config -- the same
	# reasoning the tag-pill click test spells out.
	bar_set "bar { enable true; height 30; position \"top\"; margin { x 8; y 4 }; pill-min-width 28; pill-padding 4; panel { enable false }; modules-left \"custom/p1\"; custom \"p1\" { exec \"$d/clicky\"; on-click \"touch $d/clicked\" } }"
	sleep 1.5
	hl_click 12 19
	sleep 1
	hl_assert_true "left-clicking a plugin pill runs its on-click command" \
		"$([ -f "$d/clicked" ] && echo true || echo false)"

	bar_off
	rm -f "$d/clicked"
}

test_bar_plugin_that_never_answers_shows_nothing() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	# A plugin that is not installed is the common case -- someone copies a
	# config and does not have the script. It must be invisible, not a
	# permanent error pill, and above all it must not stop the bar working:
	# the clock beside it is what proves the rest of the strip still drew.
	bar_set 'bar { enable true; height 36; position "top"; margin { x 8; y 4 }; panel { enable false }; modules-left "custom/nope"; modules-center "clock"; custom "nope" { exec "/nonexistent/bar/plugin"; interval 1 } }'
	sleep 2
	hl_screenshot plug-missing
	local left centre
	left="$(hl_region_ink "$HL_OUTDIR/plug-missing.png" 8 4 400 44)"
	centre="$(hl_region_ink "$HL_OUTDIR/plug-missing.png" $((HL_WIDTH / 2 - 150)) 4 $((HL_WIDTH / 2 + 150)) 44)"
	hl_assert_true "a plugin whose command does not exist draws nothing ($left px)" \
		"$([ "${left:-0}" -lt 20 ] && echo true || echo false)"
	hl_assert_true "and the rest of the bar is unaffected ($centre px)" \
		"$([ "${centre:-0}" -gt 100 ] && echo true || echo false)"

	# and the compositor is still answering, i.e. nothing blocked the loop
	hl_assert_eq "the event loop is still live" "$(hl_get "get all-monitors" | jq -r '.monitors | length')" "1"

	bar_off
}

test_bar_plugin_survives_reload_with_a_changed_list() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	local d; d="$(bar_plugin_dir)"
	# A continuous plugin is a long-lived child holding an INDEX into the
	# config's plugin table, and a reload rebuilds that table underneath it.
	# Reloading between differently-shaped lists is what catches a child that
	# outlived the array it was indexed against.
	printf '#!/bin/sh\nwhile :; do echo A; sleep 1; done\n' > "$d/streamA"
	printf '#!/bin/sh\nwhile :; do echo B; sleep 1; done\n' > "$d/streamB"
	chmod +x "$d/streamA" "$d/streamB"

	bar_set "bar { enable true; height 36; modules-left \"custom/a\"; custom \"a\" { exec \"$d/streamA\"; continuous true } }"
	sleep 1
	bar_set "bar { enable true; height 36; modules-left \"custom/b,custom/a\"; custom \"b\" { exec \"$d/streamB\"; continuous true }; custom \"a\" { exec \"$d/streamA\"; continuous true } }"
	sleep 1
	bar_set "bar { enable true; height 36; modules-left \"custom/a\"; custom \"a\" { exec \"$d/streamA\"; continuous true } }"
	sleep 1
	hl_assert_eq "reloading across changed plugin lists leaves the compositor alive" \
		"$(hl_get "get all-monitors" | jq -r '.monitors | length')" "1"

	bar_off
	sleep 0.5
	# With no plugin on any bar, no plugin child may still be running.
	hl_assert_true "taking a continuous plugin off the bar stops its child" \
		"$(pgrep -f "$d/streamA" >/dev/null && echo false || echo true)"
}

# The reference plugin: contrib/bar-plugins/discord-voice reimplements the
# built-in `discord` module as a `continuous` plugin against the same daemon
# socket. Worth testing precisely because a claim was made about it -- that a
# module whose real work already lives in a daemon can leave the compositor
# without losing anything the bar actually draws.
test_bar_discord_plugin_matches_the_builtin_module() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v socat >/dev/null || { echo "  (skip: socat not available)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	local plugin="$HL_REPO/contrib/bar-plugins/discord-voice"
	[ -x "$plugin" ] || { echo "  (skip: $plugin not executable)"; return 0; }

	# Connected, in a named channel, so both renderings have a label to draw
	# rather than the "Offline" both would agree on trivially.
	local snap="$HL_OUTDIR/dv-plugin.jsonl"
	python3 - "$snap" <<'PY'
import json, sys
events = [
    {"event": "ready", "username": "tester"},
    {"event": "channels", "guilds": [{"id": "g1", "name": "Server"}],
     "channels": [{"id": "c1", "guild": "g1", "name": "General",
                   "participants": []}]},
    {"event": "status", "state": "connected", "channel": "c1", "muted": False},
]
open(sys.argv[1], "w").write("".join(json.dumps(e) + "\n" for e in events))
PY
	voice_serve "$snap"
	sleep 0.5

	# Both renderings alone on the left, so the same region measures each.
	local common='bar { enable true; height 36; position "top"; margin { x 8; y 4 }; panel { enable false }'

	bar_set "$common; modules-left \"discord\" }"
	sleep 1.5
	local builtin_ink; builtin_ink="$(bar_plugin_ink dv-builtin)"

	# ASTEROIDZ_DISCORD_DAEMON is pinned empty so the test never tries to
	# start a real daemon on a machine that happens to have one installed.
	bar_set "$common; modules-left \"custom/discord\"; custom \"discord\" { exec \"ASTEROIDZ_DISCORD_DAEMON= $plugin\"; continuous true } }"
	sleep 2.5
	local plugin_ink; plugin_ink="$(bar_plugin_ink dv-plugin)"

	hl_assert_true "the built-in discord module draws a connected pill ($builtin_ink px)" \
		"$([ "${builtin_ink:-0}" -gt 500 ] && echo true || echo false)"
	hl_assert_true "the plugin draws one too ($plugin_ink px)" \
		"$([ "${plugin_ink:-0}" -gt 500 ] && echo true || echo false)"
	# Not pixel equality: the two resolve the same artwork and the same label,
	# so they land within a few percent, but the built-in pins no width and the
	# plugin's pill is measured from its own text. Close is the claim.
	local lo=$(( builtin_ink * 85 / 100 )) hi=$(( builtin_ink * 115 / 100 ))
	hl_assert_true "and the two are within 15% of each other ($plugin_ink vs $builtin_ink)" \
		"$([ "${plugin_ink:-0}" -ge "$lo" ] && [ "${plugin_ink:-0}" -le "$hi" ] && echo true || echo false)"

	voice_stop
	bar_off
}

test_bar_plugin_icon_array_draws_them_side_by_side() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	local d; d="$(bar_plugin_dir)"

	# "icon" taking an ARRAY exists because of a real case: the built-in discord
	# module draws the logo with the mic-mute glyph beside it while muted, and a
	# single-icon schema could not express it.
	#
	# Tested here with a trivial plugin rather than through discord, because the
	# muted state ALSO dims the tint -- an earlier version of this test measured
	# the two states of the real plugin and found them equal, since two icons at
	# 45% alpha fall below the ink threshold about as far as one at full alpha
	# rises above it. Two variables, one number, no conclusion. Here the tint is
	# pinned and only the icon count moves.
	local icons='waybar-discord-voice/discord.svg'
	printf '#!/bin/sh\necho %s\n' "'{\"icon\":[\"$icons\"],\"tint\":\"fg\"}'" > "$d/icon1"
	printf '#!/bin/sh\necho %s\n' "'{\"icon\":[\"$icons\",\"$icons\"],\"tint\":\"fg\"}'" > "$d/icon2"
	chmod +x "$d/icon1" "$d/icon2"

	local cfg='bar { enable true; height 36; position "top"; margin { x 8; y 4 }; panel { enable false }; modules-left "custom/p1"'
	bar_set "$cfg; custom \"p1\" { exec \"$d/icon1\" } }"
	sleep 1.5
	local one; one="$(bar_plugin_ink icons-one)"
	bar_set "$cfg; custom \"p1\" { exec \"$d/icon2\" } }"
	sleep 1.5
	local two; two="$(bar_plugin_ink icons-two)"

	hl_assert_true "a plugin naming one icon draws it ($one px)" \
		"$([ "${one:-0}" -gt 300 ] && echo true || echo false)"
	# The same glyph twice, so the second is exactly as much ink as the first:
	# anything short of a near-doubling means the array was not honoured.
	hl_assert_true "and naming two draws both, near double the ink ($two vs $one px)" \
		"$([ "${two:-0}" -gt $(( ${one:-0} * 17 / 10 )) ] && echo true || echo false)"

	bar_off
}

test_bar_plugin_items_array_draws_a_row_of_pills() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	local d; d="$(bar_plugin_dir)"

	# One plugin, N pills. This is what lets a tray host live outside the
	# compositor at all: a tray is icons appearing and vanishing as
	# applications come and go, and a plugin limited to a single pill could
	# never stand in for one.
	printf '#!/bin/sh\necho %s\n' "'{\"items\":[{\"id\":\"a\",\"text\":\"AAA\"},{\"id\":\"b\",\"text\":\"BBB\"},{\"id\":\"c\",\"text\":\"CCC\"}]}'" > "$d/three"
	printf '#!/bin/sh\necho %s\n' "'{\"items\":[{\"id\":\"a\",\"text\":\"AAA\"}]}'" > "$d/one"
	chmod +x "$d/three" "$d/one"

	local cfg='bar { enable true; height 36; position "top"; margin { x 8; y 4 }; panel { enable false }; modules-left "custom/p1"'
	bar_set "$cfg; custom \"p1\" { exec \"$d/one\" } }"
	sleep 1.5
	local one; one="$(bar_plugin_ink items-one)"
	bar_set "$cfg; custom \"p1\" { exec \"$d/three\" } }"
	sleep 1.5
	local three; three="$(bar_plugin_ink items-three)"

	hl_assert_true "one item draws one pill ($one px)" \
		"$([ "${one:-0}" -gt 200 ] && echo true || echo false)"
	# Three labels of equal length, so the ink scales with the count. Anything
	# near parity means only the first item was drawn.
	hl_assert_true "three items draw three, near triple the ink ($three vs $one px)" \
		"$([ "${three:-0}" -gt $(( ${one:-0} * 25 / 10 )) ] && echo true || echo false)"

	bar_off
}

test_bar_plugin_click_reaches_a_streaming_plugin_with_the_item_id() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	local d; d="$(bar_plugin_dir)"
	rm -f "$d/clicklog"

	# A continuous plugin gets a writable stdin, and a click on one of its
	# pills arrives there as JSON naming the item. Without this a tray host
	# outside the compositor could draw icons but never react to them -- and
	# no on-click shell command can carry the screen coordinates an item's
	# Activate needs.
	cat > "$d/clicksink" <<'EOF'
#!/bin/sh
echo '{"items":[{"id":"first","text":"AAAA"},{"id":"second","text":"BBBB"}]}'
while IFS= read -r line; do
	printf '%s\n' "$line" >> CLICKLOG
done
EOF
	sed -i "s|CLICKLOG|$d/clicklog|" "$d/clicksink"
	chmod +x "$d/clicksink"

	# Panels off and the plugin alone on the left, so the first pill starts at
	# margin-x and the click coordinate follows from the config.
	bar_set "bar { enable true; height 30; position \"top\"; margin { x 8; y 4 }; pill-min-width 28; pill-padding 4; spacing 8; panel { enable false }; modules-left \"custom/p1\"; custom \"p1\" { exec \"$d/clicksink\"; continuous true } }"
	sleep 2
	hl_click 12 19
	sleep 1

	hl_assert_true "a click on a streaming plugin's pill reaches its stdin" \
		"$([ -s "$d/clicklog" ] && echo true || echo false)"
	hl_assert_true "and the event names the item that was hit ($(head -1 "$d/clicklog" 2>/dev/null))" \
		"$(grep -q '"item":"first"' "$d/clicklog" 2>/dev/null && echo true || echo false)"
	hl_assert_true "and carries the button and screen position" \
		"$(grep -q '"button":"left"' "$d/clicklog" 2>/dev/null && grep -q '"x":' "$d/clicklog" 2>/dev/null && echo true || echo false)"

	bar_off
	rm -f "$d/clicklog"
}

test_bar_plugin_can_open_a_popover_menu() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	local d; d="$(bar_plugin_dir)"
	rm -f "$d/menulog"

	# A plugin cannot draw a popover -- it has no scene tree, no hit testing
	# and no keyboard. So it sends ROWS and the compositor renders them into
	# the same panel every built-in menu uses. This is what makes a tray host
	# outside the compositor viable at all: without menus, a tray icon you can
	# only left-click is not a tray.
	cat > "$d/menuplug" <<'EOF'
#!/bin/sh
echo '{"items":[{"id":"one","text":"MENU"}]}'
while IFS= read -r line; do
	printf '%s\n' "$line" >> MENULOG
	case "$line" in
	*'"button":"right"'*)
		echo '{"menu":{"item":"one","rows":[{"text":"Alpha","value":"a"},{"text":"Bravo","value":"b"},{"separator":true},{"text":"Charlie","value":"c"}]}}'
		;;
	esac
done
EOF
	sed -i "s|MENULOG|$d/menulog|" "$d/menuplug"
	chmod +x "$d/menuplug"

	bar_set "bar { enable true; height 30; position \"top\"; margin { x 8; y 4 }; pill-min-width 28; pill-padding 4; panel { enable false }; modules-left \"custom/p1\"; custom \"p1\" { exec \"$d/menuplug\"; continuous true } }"
	sleep 2

	hl_screenshot menu-before
	local before; before="$(hl_region_ink "$HL_OUTDIR/menu-before.png" 0 40 500 500)"

	# right-click the pill; the menu arrives a round trip later
	hl_click 12 19 rclick
	sleep 2
	hl_screenshot menu-open
	local opened; opened="$(hl_region_ink "$HL_OUTDIR/menu-open.png" 0 40 500 500)"

	hl_assert_true "the plugin was told about the right-click" \
		"$(grep -q '"button":"right"' "$d/menulog" 2>/dev/null && echo true || echo false)"
	hl_assert_true "and the rows it sent back were drawn as a popover ($before -> $opened px)" \
		"$([ "${opened:-0}" -gt $(( ${before:-0} + 2000 )) ] && echo true || echo false)"

	bar_off
	rm -f "$d/menulog"
}

test_bar_plugin_menu_survives_a_long_row_list() {
	[ "$(bar_supported)" = "true" ] || { echo "  (skip: built without -Dnative-bar)"; return 0; }
	command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null || {
		echo "  (skip: python3 PIL not available)"; return 0; }
	local d; d="$(bar_plugin_dir)"

	# A plugin's line is a whole DOCUMENT, not a pill's worth of text, and a
	# menu is as long as the menu is. bar_custom_apply used to copy every line
	# into a 512-byte scratch before parsing, so anything bigger was truncated
	# and then failed to parse -- silently. Short menus worked, which made it
	# look like the one application with a long menu was at fault. Steam's tray
	# menu is sixteen rows and several kilobytes; this is that, synthetically.
	python3 - "$d/bigmenu" <<'PYEOF'
import json, sys
rows = [{"text": f"Some Reasonably Long Game Title {i:02d}", "value": str(i)}
        for i in range(24)]
menu = json.dumps({"menu": {"item": "", "rows": rows}})
open(sys.argv[1], "w").write(
    "#!/bin/sh\n"
    "echo '{\"items\":[{\"id\":\"one\",\"text\":\"BIG\"}]}'\n"
    "while IFS= read -r line; do\n"
    "  case \"$line\" in\n"
    "  *'\"button\":\"right\"'*) echo '" + menu + "' ;;\n"
    "  esac\n"
    "done\n")
PYEOF
	chmod +x "$d/bigmenu"
	echo "  (menu payload: $(python3 -c "
import json
rows=[{'text':f'Some Reasonably Long Game Title {i:02d}','value':str(i)} for i in range(24)]
print(len(json.dumps({'menu':{'item':'','rows':rows}})))") bytes)"

	bar_set "bar { enable true; height 30; position \"top\"; margin { x 8; y 4 }; pill-min-width 28; pill-padding 4; panel { enable false }; modules-left \"custom/p1\"; custom \"p1\" { exec \"$d/bigmenu\"; continuous true } }"
	sleep 2
	hl_screenshot big-before
	local before; before="$(hl_region_ink "$HL_OUTDIR/big-before.png" 0 40 500 700)"
	hl_click 12 19 rclick
	sleep 2
	hl_screenshot big-open
	local opened; opened="$(hl_region_ink "$HL_OUTDIR/big-open.png" 0 40 500 700)"

	hl_assert_true "a menu far larger than one pill's text still opens ($before -> $opened px)" \
		"$([ "${opened:-0}" -gt $(( ${before:-0} + 5000 )) ] && echo true || echo false)"

	bar_off
}
