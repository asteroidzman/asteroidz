# rules-ipc.sh — `get window-rule-schema`, `get window-rules`, `get binds`.
#
# The read half of a rule and keybind editor. What makes these worth testing
# separately from the option schema is that both structures are LOSSY once
# parsed: a KeyBinding is a function pointer and a union by the time it exists,
# and a ConfigWinRule cannot say whether the file wrote `0` or wrote nothing. So
# these verbs are served from records captured while reading, and the thing to
# check is that those records say what the file said.
#
# THIS MODULE WRITES TO $HL_CONFIG and restores a pristine copy after every test,
# the way config-write.sh and bar.sh do. Modules run in name order, which puts
# this one between output and scratchpad; a rule left behind would apply to every
# window mapped for the rest of the run.

_ri_pristine=""

_ri_save() {
	if [ -z "$_ri_pristine" ]; then
		_ri_pristine="$HL_OUTDIR/config.ri-pristine.kdl"
		cp "$HL_CONFIG" "$_ri_pristine"
	fi
}

_ri_restore() {
	[ -n "$_ri_pristine" ] || return 0
	cp "$_ri_pristine" "$HL_CONFIG"
	hl_dispatch "reload_config" 2
}

_ri_rule() { # _ri_rule <index> <jq-expression-relative-to-the-rule>
	hl_get "get window-rules" | jq -r ".rules[$1] | $2"
}

# The rule that a given app-id matches, by index. Rules accumulate in file order
# and this module appends, so hard-coding an index would break the moment
# anything else in the suite adds one.
_ri_index_of() { # _ri_index_of <appid>
	hl_get "get window-rules" \
		| jq -r --arg a "$1" \
			'[.rules[] | select(.fields.appid == $a)][0].index // -1'
}

test_window_rule_schema_describes_the_vocabulary() {
	local s; s="$(hl_get "get window-rule-schema")"
	hl_assert_true "the schema is valid JSON" \
		"$(printf '%s' "$s" | jq -e . >/dev/null 2>&1 && echo true || echo false)"
	hl_assert_true "...with every field described" \
		"$([ "$(printf '%s' "$s" | jq '.fields | length')" -ge 50 ] \
			&& echo true || echo false)"
	hl_assert_true "...and groups to put them in" \
		"$([ "$(printf '%s' "$s" | jq '.groups | length')" -ge 5 ] \
			&& echo true || echo false)"

	# The two facts a UI gets wrong by default, and the reason this schema
	# exists at all rather than a list of names.
	hl_assert_true "app-id is marked as a regex" \
		"$(printf '%s' "$s" \
			| jq -r '[.fields[] | select(.key=="appid")][0].regex')"
	hl_assert_true "a tri-state says so" \
		"$(printf '%s' "$s" \
			| jq -r '[.fields[] | select(.key=="isfloating")][0].tristate')"
	# A checkbox against a tri-state writes an explicit 0 for every field the
	# rule never mentioned, so "how many are tri-state" is worth asserting.
	hl_assert_true "most fields are tri-state" \
		"$([ "$(printf '%s' "$s" | jq '[.fields[] | select(.tristate)] | length')" \
			-ge 30 ] && echo true || echo false)"

	hl_assert_eq "every field carries both spellings" \
		"$(printf '%s' "$s" \
			| jq '[.fields[] | select((.key|length)>0 and (.nice|length)>0)] | length')" \
		"$(printf '%s' "$s" | jq '.fields | length')"
	hl_assert_eq "...and an explanation" \
		"$(printf '%s' "$s" | jq '[.fields[] | select((.desc|length)>10)] | length')" \
		"$(printf '%s' "$s" | jq '.fields | length')"
}

test_window_rules_report_only_what_the_rule_sets() {
	_ri_save
	cat >> "$HL_CONFIG" <<'EOF'
window-rule {
	match app-id="ri-probe"
	open-floating
}
EOF
	hl_dispatch "reload_config" 2

	local i; i="$(_ri_index_of ri-probe)"
	hl_assert_true "the rule is listed" \
		"$([ "$i" -ge 0 ] && echo true || echo false)"
	hl_assert_eq "...with the field it set" "$(_ri_rule "$i" '.fields.isfloating')" "1"
	# THE assertion of this module. A rule that says nothing about blur must
	# report nothing about blur: the field is -1 in the struct, and a serialiser
	# that emitted every field would have a rule editor unable to distinguish
	# "leave it alone" from "turn it off" -- and would write the latter back for
	# all fifty-three on the first save.
	hl_assert_eq "...and nothing about the fields it did not" \
		"$(_ri_rule "$i" '.fields | keys | length')" "2"
	hl_assert_eq "...specifically not the unset tri-states" \
		"$(_ri_rule "$i" '.fields.noblur // "absent"')" "absent"
	_ri_restore
}

test_window_rules_use_the_written_spelling_of_a_value() {
	_ri_save
	cat >> "$HL_CONFIG" <<'EOF'
window-rule {
	match app-id="ri-tags"
	tags 4
}
EOF
	hl_dispatch "reload_config" 2
	local i; i="$(_ri_index_of ri-tags)"
	# `tags 4` is stored as 1 << 3. Reporting the mask would be a rule editor
	# offering tag 8 for a rule that says 4 -- and writing 8 back, which is tag 4
	# again only by coincidence of the same bug on both sides.
	hl_assert_eq "a tag is reported as the number that was written" \
		"$(_ri_rule "$i" '.fields.tags')" "4"
	_ri_restore
}

test_window_rules_accept_both_spellings_of_a_field() {
	_ri_save
	# Two axes, both of which have a wrong answer that is silent.
	#
	# The dashed FIELD name is new; the underscore one is what every config in
	# existence uses, and breaking it to add the other is the change nobody
	# notices until a reload rejects their file.
	#
	# And `#true` is KDL v2's spelling of a boolean while bare `true` is v1's.
	# `#` is a legal bare-word character, so a spec-correct `#true` used to parse
	# as the STRING "#true" and every consumer ran it through atoi and got 0 --
	# a boolean reading as false with nothing said. Nothing in the tree writes v2
	# spelling, which is the only reason it never bit.
	cat >> "$HL_CONFIG" <<'EOF'
window-rule {
	match app-id="ri-nice"
	vrr-only-fullscreen #true
	force_hdr true
	no-blur #false
}
EOF
	hl_dispatch "reload_config" 2
	local i; i="$(_ri_index_of ri-nice)"
	hl_assert_eq "the canonical KDL spelling resolves" \
		"$(_ri_rule "$i" '.fields.vrr_only_fullscreen')" "1"
	hl_assert_eq "...and so does the bare key" \
		"$(_ri_rule "$i" '.fields.force_hdr')" "1"
	# The contrast: #false has to reach the field as 0, not as "not set" and not
	# as 1. An unset tri-state reports nothing at all, so `0` here is proof the
	# value crossed rather than the key being ignored.
	hl_assert_eq "...and #false is false, not absent" \
		"$(_ri_rule "$i" '.fields.noblur')" "0"
	hl_assert_true "and the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"
	_ri_restore
}

test_window_rules_carry_where_they_came_from() {
	_ri_save
	cat >> "$HL_CONFIG" <<'EOF'
window-rule {
	match app-id="ri-source"
	no-blur
}
EOF
	hl_dispatch "reload_config" 2
	local i; i="$(_ri_index_of ri-source)"
	hl_assert_eq "a KDL rule names its file" \
		"$(_ri_rule "$i" '.source.file')" "$HL_CONFIG"
	hl_assert_true "...and its line" \
		"$([ "$(_ri_rule "$i" '.source.line')" -gt 0 ] && echo true || echo false)"
	hl_assert_true "...and says it can be rewritten" \
		"$(_ri_rule "$i" '.source.editable')"
	_ri_restore
}

test_binds_are_listed_with_what_was_written() {
	local b; b="$(hl_get "get binds")"
	hl_assert_true "the bind list is valid JSON" \
		"$(printf '%s' "$b" | jq -e . >/dev/null 2>&1 && echo true || echo false)"
	hl_assert_true "...and is not empty" \
		"$([ "$(printf '%s' "$b" | jq '.count')" -gt 0 ] && echo true || echo false)"

	# The harness config binds F11 -> combo_view 2 in a KDL binds block.
	local f11; f11="$(printf '%s' "$b" \
		| jq -c '[.binds[] | select(.chord=="F11")][0]')"
	hl_assert_eq "a KDL bind reports its chord verbatim" \
		"$(printf '%s' "$f11" | jq -r '.chord')" "F11"
	# The ACTION NAME, not a function pointer. This is the whole reason the
	# source records exist: by the time it reaches KeyBinding it is `comboview`
	# and `arg.ui`, and no dispatch name survives.
	hl_assert_eq "...and the dispatch it was written with" \
		"$(printf '%s' "$f11" | jq -r '.action')" "combo_view"
	hl_assert_eq "...and its argument, unresolved" \
		"$(printf '%s' "$f11" | jq -r '.args[0]')" "2"
	hl_assert_eq "...with no padding arguments" \
		"$(printf '%s' "$f11" | jq '.args | length')" "1"
	hl_assert_true "...and says it can be rewritten" \
		"$(printf '%s' "$f11" | jq -r '.source.editable')"

	# A legacy comma-string mousebind. Listed, because hiding binds a UI cannot
	# write is how a user edits around one they cannot see -- but not editable,
	# because there is no node and no span to replace.
	local mb; mb="$(printf '%s' "$b" \
		| jq -c '[.binds[] | select(.kind=="mousebind")][0]')"
	hl_assert_eq "a legacy mousebind is listed too" \
		"$(printf '%s' "$mb" | jq -r '.action')" "move_resize"
	hl_assert_false "...and is marked not editable" \
		"$(printf '%s' "$mb" | jq -r '.source.editable')"

	# The kinds with no KDL handler at all, named rather than silently absent.
	hl_assert_true "the kinds that cannot be listed are named" \
		"$(printf '%s' "$b" \
			| jq -e '.not_listed | index("axisbind") != null' >/dev/null 2>&1 \
			&& echo true || echo false)"
}

test_bind_flags_can_be_written_in_kdl() {
	_ri_save
	# parse_bind_flags has always read s/l/r/p from the KEY -- "bindr", not a
	# field -- and kdl_binds always passed the bare "bind". So these four were
	# reachable only by writing the legacy string form by hand, and a KDL config
	# could not express a release binding at all. Nothing said so; the flag was
	# simply dropped.
	cat >> "$HL_CONFIG" <<'EOF'
binds {
	Alt+F9 release=#true { combo_view 2; }
	Alt+F8 release=true { combo_view 2; }
	Alt+F10 { combo_view 3; }
}
EOF
	hl_dispatch "reload_config" 2
	local b; b="$(hl_get "get binds")"
	hl_assert_true "release=#true reaches the binding" \
		"$(printf '%s' "$b" \
			| jq -r '[.binds[] | select(.chord=="Alt+F9")][0].flags.release')"
	hl_assert_true "...in either KDL boolean spelling" \
		"$(printf '%s' "$b" \
			| jq -r '[.binds[] | select(.chord=="Alt+F8")][0].flags.release')"
	# The contrast is the assertion. "release is true" alone would pass against a
	# build that reported true for everything.
	hl_assert_false "...and a chord without it is not a release bind" \
		"$(printf '%s' "$b" \
			| jq -r '[.binds[] | select(.chord=="Alt+F10")][0].flags.release')"
	hl_assert_false "...and it did not turn on the other three" \
		"$(printf '%s' "$b" \
			| jq -r '[.binds[] | select(.chord=="Alt+F9")][0].flags.lock')"
	hl_assert_true "and the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"
	_ri_restore
}
