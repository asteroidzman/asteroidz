# titlebar.sh — the titlebar is as tall as its own text.
#
# There is no `layout { titlebar { height } }` any more. The height is derived
# in override_config() from the line height of `theme { font }` plus
# `theme { padding { y } }` above and below, so raising the theme font grows
# the bar around the text instead of putting bigger text into a box that stayed
# the same size.
#
# Measured through the LAYOUT rather than through a screenshot: a titlebar
# reserves real space above a tiled window (client_tile_resize subtracts it
# from the geometry), so the top of the tiled window moves down by exactly the
# titlebar's height. That gives an integer to compare instead of a band of
# pixels to find, and it also proves the thing that matters -- that the rest of
# the compositor agrees about the new height, not just the renderer.
#
# Only DIFFERENCES are asserted. The absolute number is whatever Pango says
# Ubuntu's ascent plus descent comes to at a given size, which is a property of
# the installed font file and not something a test should pin.
#
# THIS MODULE CHANGES theme/font AND theme/padding/y, so every test restores
# them; leaving the theme font changed would move every later module's windows.

_tb_font=""    # the values found on entry, restored after each test
_tb_pad=""

_tb_save() {
	[ -n "$_tb_font" ] && return 0
	_tb_font="$(hl_get "get config" | jq -r '.values.theme_font_desc.value')"
	_tb_pad="$(hl_get "get config" | jq -r '.values.theme_padding_y.value')"
}

# One line, deliberately. The request travels as a single IPC command and a
# newline inside it ends the command early -- a pretty-printed body silently
# applied nothing here and the test read back the height it started with.
_tb_set() { # _tb_set <font> <padding-y>
	local body
	body="$(printf '{"changes":[{"path":"theme/font","value":"%s"},{"path":"theme/padding/y","value":"%s"}],"persist":false}' "$1" "$2")"
	ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg "set-config $body" >/dev/null 2>&1
	sleep 0.4
}

_tb_restore() {
	[ -n "$_tb_font" ] || return 0
	_tb_set "$_tb_font" "$_tb_pad"
}

# The top edge of our own tiled window. Everything above it on this tag is
# fixed (gaps, borders) except the titlebar, so a change here IS the change in
# titlebar height.
_tb_top() { hl_get "get all-clients" | jq -r '.clients[] | select(.title=="W1") | .y'; }

test_titlebar_height_follows_the_theme_font() {
	_tb_save
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3

	_tb_set "Ubuntu 10" 0
	local small; small="$(_tb_top)"
	_tb_set "Ubuntu 24" 0
	local big; big="$(_tb_top)"

	hl_assert_true "a bigger theme font reserves a taller titlebar ($small -> $big)" \
		"$([ -n "$small" ] && [ -n "$big" ] && [ "$big" -gt "$small" ] && echo true || echo false)"

	# Not merely different: proportionate. Ubuntu at 24pt is 2.4x the 10pt
	# line height, so the reserved space has to grow by a comparable share --
	# a height that moved by one or two pixels would pass a bare inequality
	# and would still not be "the size of the text".
	local grew=$((big - small))
	hl_assert_true "...by a whole line, not a rounding ($grew px)" \
		"$([ "$grew" -ge 10 ] && echo true || echo false)"

	_tb_restore
}

test_titlebar_height_adds_theme_padding_above_and_below() {
	_tb_save
	hl_dispatch "set_layout,tile"
	hl_spawn_kitty W1 >/dev/null
	hl_wait_client_count 1
	sleep 0.3

	_tb_set "Ubuntu 12" 0
	local bare; bare="$(_tb_top)"
	_tb_set "Ubuntu 12" 8
	local padded; padded="$(_tb_top)"

	# Twice, because the padding is above the text AND below it. Getting this
	# wrong gives a bar that looks right at one font size and top-heavy at
	# every other.
	hl_assert_eq "8px of theme padding adds 16px of titlebar" \
		"$((padded - bare))" "16"

	_tb_restore
}
