# tag-rules-ipc.sh — `get tag-rule-schema` and `get tag-rules`.
#
# The per-tag layout settings: which layout a tag opens in, on which monitor,
# with which master factor and which scroller proportions. They live in `tag`
# blocks and were reachable only by editing the config file -- `set-config`
# writes OPTIONS, and a tag rule is not one, so nothing served them.
#
# Worth testing separately from window rules for the reason those are worth
# testing separately from options: ConfigTagRule cannot say whether the file
# wrote `0` or wrote nothing, so the response is built from a formatter that
# returns "unset" for every field's own sentinel, and from records captured while
# the file was read. The thing to check is that both say what the file said.
#
# THIS MODULE WRITES TO $HL_CONFIG and restores a pristine copy after every test.
# Modules run in name order; a tag rule left behind would set a layout for the
# rest of the run.

_tr_pristine=""

_tr_save() {
	if [ -z "$_tr_pristine" ]; then
		_tr_pristine="$HL_OUTDIR/config.tr-pristine.kdl"
		cp "$HL_CONFIG" "$_tr_pristine"
	fi
}

_tr_restore() {
	[ -n "$_tr_pristine" ] || return 0
	cp "$_tr_pristine" "$HL_CONFIG"
	hl_dispatch "reload_config" 2
}

# The LAST rule for a tag id, which is the one in force: tag rules are applied in
# order and a later one overrides an earlier, so the harness config's own tags do
# not have to be removed for a test to add its own.
_tr_last() { # _tr_last <tag-id> <jq-expression-relative-to-.fields>
	hl_get "get tag-rules" \
		| jq -r --arg id "$1" \
			"[.rules[] | select(.fields.id == \$id)] | last | .fields | $2"
}

test_the_tag_rule_schema_describes_every_field() {
	local n
	n="$(hl_get "get tag-rule-schema" | jq '.fields | length')"
	hl_assert_true "the tag-rule schema has fields (got ${n:-0})" \
		"$([ "${n:-0}" -ge 12 ] && echo true || echo false)"

	# The layout is an ENUM, not free text. A text field here produces configs
	# naming layouts that do not exist, which parse and then do nothing.
	hl_assert_eq "layout is an enum" \
		"$(hl_get "get tag-rule-schema" \
			| jq -r '.fields[] | select(.key=="layout_name") | .type')" \
		"enum"
	hl_assert_eq "and it offers the four real layouts" \
		"$(hl_get "get tag-rule-schema" \
			| jq -r '[.fields[] | select(.key=="layout_name") | .enum[].name] | join(",")')" \
		"tile,scroller,monocle,float"

	# Three states, not two. A checkbox drawn against it writes an explicit 0 for
	# every rule that was silent.
	hl_assert_eq "the scroller single-width override is a tri-state" \
		"$(hl_get "get tag-rule-schema" \
			| jq -r '.fields[] | select(.key=="scroller_ignore_proportion_single") | .tristate')" \
		"true"

	# Both spellings are published, because the file may hold either.
	hl_assert_eq "fields carry their hyphenated KDL name" \
		"$(hl_get "get tag-rule-schema" \
			| jq -r '.fields[] | select(.key=="no_render_border") | .nice')" \
		"no-render-border"
}

test_a_tag_rule_reports_only_what_it_sets() {
	_tr_save
	cat >> "$HL_CONFIG" <<'EOF'
tag 3 { layout tile; nmaster 2; mfact 0.6 }
EOF
	hl_dispatch "reload_config" 2

	hl_assert_eq "the layout is reported" "$(_tr_last 3 '.layout_name')" "tile"
	hl_assert_eq "so is nmaster" "$(_tr_last 3 '.nmaster')" "2"
	hl_assert_eq "and mfact" "$(_tr_last 3 '.mfact')" "0.6"

	# The half that a struct cannot answer: this rule says nothing about the
	# scroller, and a response that emitted every field with its default would be
	# indistinguishable from one that pinned them.
	hl_assert_eq "a field the rule is silent about is absent" \
		"$(_tr_last 3 'has("scroller_default_proportion")')" "false"
	hl_assert_eq "and so is one whose sentinel is zero" \
		"$(_tr_last 3 'has("no_hide")')" "false"

	_tr_restore
}

test_a_tag_rule_says_which_file_and_line_it_came_from() {
	_tr_save
	cat >> "$HL_CONFIG" <<'EOF'
tag 7 { layout monocle }
EOF
	hl_dispatch "reload_config" 2

	local src
	src="$(hl_get "get tag-rules" \
		| jq -r '[.rules[] | select(.fields.id == "7")] | last | .source')"
	hl_assert_eq "it came from a file" "$(echo "$src" | jq -r '.kind')" "file"
	hl_assert_eq "that file is the one it was written to" \
		"$(echo "$src" | jq -r '.file')" "$HL_CONFIG"
	hl_assert_true "with a line number" \
		"$([ "$(echo "$src" | jq -r '.line')" -gt 0 ] && echo true || echo false)"
	# Editable is what decides whether an editor offers to save over it, and it
	# needs a byte span to do that -- a legacy `tagrule=` leaf has none.
	hl_assert_eq "and it is editable" "$(echo "$src" | jq -r '.editable')" "true"

	_tr_restore
}

test_hyphenated_tag_fields_are_accepted() {
	# The spelling every other block in this config language uses.
	#
	# kdl_tag passed child names through VERBATIM, so `no-render-border` became a
	# tagrule key of that name, which parse_option does not know -- it set
	# nothing and warned about nothing. Measured before the fix: the field was
	# simply absent from `get tag-rules`, which is what this asserts.
	_tr_save
	cat >> "$HL_CONFIG" <<'EOF'
tag 8 { layout tile; no-render-border 1; open-as-floating 1 }
EOF
	hl_dispatch "reload_config" 2

	hl_assert_eq "a hyphenated field is set" \
		"$(_tr_last 8 '.no_render_border')" "1"
	hl_assert_eq "and so is the next one" \
		"$(_tr_last 8 '.open_as_floating')" "1"

	_tr_restore
}

test_the_underscore_spelling_still_works() {
	# Years of configs are written this way, including the one in the docs, so
	# folding hyphens must not have cost the old spelling.
	_tr_save
	cat >> "$HL_CONFIG" <<'EOF'
tag 9 { layout scroller; no_render_border 1; monitor_name HEADLESS-1 }
EOF
	hl_dispatch "reload_config" 2

	hl_assert_eq "the underscore spelling still sets the field" \
		"$(_tr_last 9 '.no_render_border')" "1"
	hl_assert_eq "and the monitor it applies on" \
		"$(_tr_last 9 '.monitor_name')" "HEADLESS-1"

	_tr_restore
}

# ── writing ─────────────────────────────────────────────────────────────────

# Through `@-`, for the reasons rules-ipc gives: the protocol is
# newline-delimited so a pretty-printed body sent as argv would be read as
# several commands, and argv caps the whole thing at 4KB.
_tr_set() { # _tr_set '<json>'
	printf '%s' "$1" | ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" \
		"$HL_REPO/build/amsg" set-tag-rules @- 2>/dev/null
}

_tr_index_of() { # _tr_index_of <tag-id> -> the LAST rule for that tag, or -1
	hl_get "get tag-rules" \
		| jq -r --arg id "$1" \
			'[.rules[] | select(.fields.id == $id)] | last | .index // -1'
}

test_set_tag_rules_adds_a_rule_and_it_is_live_immediately() {
	_tr_save
	local r; r="$(_tr_set '{"changes":[{"op":"add","fields":{"id":"7","layout_name":"monocle","nmaster":"2"}}]}')"
	hl_assert_true "an added tag rule is accepted" \
		"$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "...and names the file it wrote" \
		"$(printf '%s' "$r" | jq -r '.results[0].file')" "$HL_CONFIG"

	hl_assert_eq "the layout is live without a reload" "$(_tr_last 7 '.layout_name')" "monocle"
	hl_assert_eq "and so is nmaster" "$(_tr_last 7 '.nmaster')" "2"

	# The id is the block's ARGUMENT, not a child. Written the other way the
	# block would apply to tag 0 -- the ~0 tag -- while looking right.
	hl_assert_true "it is written as a \`tag N\` block" \
		"$(grep -qE '^tag 7 \{' "$HL_CONFIG" && echo true || echo false)"
	hl_assert_true "the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"
	_tr_restore
}

test_a_second_tag_rule_is_added_beside_the_first() {
	# The nesting bug rw_insert_point had for window rules, which a tag block
	# would have inherited: a `tag N { }` has a body, but it is a sibling and not
	# a container.
	_tr_save
	_tr_set '{"changes":[{"op":"add","fields":{"id":"7","layout_name":"monocle"}}]}' >/dev/null
	local before; before="$(hl_get "get tag-rules" | jq '.count')"
	_tr_set '{"changes":[{"op":"add","fields":{"id":"8","layout_name":"float"}}]}' >/dev/null

	hl_assert_eq "the count goes up" \
		"$(hl_get "get tag-rules" | jq '.count')" "$((before + 1))"
	hl_assert_eq "no tag block is nested inside another" \
		"$(grep -cE '^[[:space:]]+tag [0-9]' "$HL_CONFIG")" "0"
	hl_assert_eq "and both are in force" "$(_tr_last 8 '.layout_name')" "float"
	_tr_restore
}

test_set_tag_rules_updates_in_place() {
	_tr_save
	_tr_set '{"changes":[{"op":"add","fields":{"id":"7","layout_name":"monocle"}}]}' >/dev/null
	local i; i="$(_tr_index_of 7)"
	_tr_set "{\"changes\":[{\"op\":\"update\",\"index\":$i,\"fields\":{\"id\":\"7\",\"layout_name\":\"scroller\",\"scroller_default_proportion\":\"0.5\"}}]}" >/dev/null

	hl_assert_eq "the layout changed" "$(_tr_last 7 '.layout_name')" "scroller"
	hl_assert_eq "and the new field is set" \
		"$(_tr_last 7 '.scroller_default_proportion')" "0.5"
	# Written in the canonical hyphenated spelling, which is what the schema
	# publishes and what kdl_tag now folds.
	hl_assert_true "...spelled the hyphenated way" \
		"$(grep -q 'scroller-default-proportion' "$HL_CONFIG" && echo true || echo false)"
	hl_assert_true "the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"
	_tr_restore
}

test_set_tag_rules_removes_a_rule() {
	_tr_save
	_tr_set '{"changes":[{"op":"add","fields":{"id":"7","layout_name":"monocle"}}]}' >/dev/null
	local before; before="$(hl_get "get tag-rules" | jq '.count')"
	local i; i="$(_tr_index_of 7)"
	_tr_set "{\"changes\":[{\"op\":\"remove\",\"index\":$i}]}" >/dev/null

	hl_assert_eq "the count goes down" \
		"$(hl_get "get tag-rules" | jq '.count')" "$((before - 1))"
	hl_assert_eq "and the block is gone from the file" \
		"$(grep -cE '^tag 7 \{' "$HL_CONFIG")" "0"
	_tr_restore
}

test_a_tag_rule_without_an_id_is_refused() {
	# A block with no id applies to tag 0 -- the ~0 tag -- which is never what an
	# editor meant. Refused rather than written somewhere surprising.
	_tr_save
	local r; r="$(_tr_set '{"changes":[{"op":"add","fields":{"layout_name":"monocle"}}]}')"
	hl_assert_eq "it is refused" "$(printf '%s' "$r" | jq -r '.ok')" "false"
	hl_assert_eq "and nothing was written" \
		"$(grep -cE '^tag 0 \{' "$HL_CONFIG")" "0"
	_tr_restore
}
