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

# ── writing ─────────────────────────────────────────────────────────────────

# Through `@-` (the body on stdin), not as an argv element.
#
# Two reasons, both of which a real client hits: the IPC protocol is
# newline-delimited, so a pretty-printed body sent as an argument would be read
# as several commands and every line after the first would be an unknown one;
# and the argv path caps the whole command at 4KB, which a batch of a dozen rules
# reaches. `@-` folds newlines to spaces and grows its buffer.
_ri_set_rules() { # _ri_set_rules '<json>'
	printf '%s' "$1" | ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" \
		"$HL_REPO/build/amsg" set-window-rules @- 2>/dev/null
}

_ri_set_binds() { # _ri_set_binds '<json>'
	printf '%s' "$1" | ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" \
		"$HL_REPO/build/amsg" set-binds @- 2>/dev/null
}

test_a_second_rule_is_added_beside_the_first_not_inside_it() {
	# The case a test that adds the FIRST rule cannot reach.
	#
	# rw_insert_point took "the last node of this name has a body" to mean "go
	# inside it". True of `binds { }`, which holds many chords; false of
	# `window-rule { }`, which is a sibling with a body of its own. So the second
	# rule was spliced INSIDE the first:
	#
	#     window-rule { match app-id=^mpv$; isfloating 	window-rule {
	#         match app-id="probe-nest"
	#     }
	#     }
	#
	# The file still parsed, kdl_window_rule ignored the nested node, the count
	# did not move, and the reply said {"ok":true,"applied":1}. A settings window
	# offering "New rule" damaged an existing rule and reported success.
	_ri_save
	_ri_set_rules '{"changes":[{"op":"add","fields":{"appid":"ri-first"}}]}' >/dev/null
	local before; before="$(hl_get "get window-rules" | jq '.count')"

	_ri_set_rules '{"changes":[{"op":"add","fields":{"appid":"ri-second"}}]}' >/dev/null
	local after; after="$(hl_get "get window-rules" | jq '.count')"

	hl_assert_eq "adding a second rule increases the count" \
		"$after" "$((before + 1))"
	hl_assert_true "and the first rule is still there" \
		"$([ "$(_ri_index_of ri-first)" -ge 0 ] && echo true || echo false)"
	hl_assert_true "and so is the second" \
		"$([ "$(_ri_index_of ri-second)" -ge 0 ] && echo true || echo false)"
	# The shape on disk, not just the count.
	#
	# INDENTATION, not "both on one line" -- which is what this asserted first
	# and it passed against the bug: the nested block only shares a line with its
	# host when the host was written on one line. A top-level rule starts at
	# column 0, so anything indented is inside something.
	hl_assert_eq "no window-rule block is nested inside another" \
		"$(grep -cE '^[[:space:]]+window-rule' "$HL_CONFIG")" "0"
	hl_assert_true "and the file still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"

	_ri_restore
}

test_set_window_rules_adds_a_rule_and_it_survives_a_reload() {
	_ri_save
	local r; r="$(_ri_set_rules '{"changes":[{"op":"add","fields":{"appid":"ri-add","isfloating":"1","tags":"3"}}]}')"
	hl_assert_true "an added rule is accepted" \
		"$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "...and names the file it wrote" \
		"$(printf '%s' "$r" | jq -r '.results[0].file')" "$HL_CONFIG"

	local i; i="$(_ri_index_of ri-add)"
	hl_assert_true "the rule is live immediately" \
		"$([ "$i" -ge 0 ] && echo true || echo false)"
	hl_assert_eq "...with the fields it was given" \
		"$(_ri_rule "$i" '.fields.isfloating')" "1"
	hl_assert_eq "...including the tag, as a number" \
		"$(_ri_rule "$i" '.fields.tags')" "3"

	# Nested KDL was written and the existing reader flattened it. If the writer
	# had emitted the legacy comma form this would still pass -- so the file is
	# checked too.
	hl_assert_true "a window-rule BLOCK was written, not a legacy line" \
		"$(grep -q 'window-rule {' "$HL_CONFIG" && echo true || echo false)"
	hl_assert_true "...spelled with the canonical field names" \
		"$(grep -q 'open-floating' "$HL_CONFIG" && echo true || echo false)"
	hl_assert_true "the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"
	hl_dispatch "reload_config" 2
	hl_assert_true "and it survives a reload" \
		"$([ "$(_ri_index_of ri-add)" -ge 0 ] && echo true || echo false)"
	_ri_restore
}

test_set_window_rules_updates_in_place_and_keeps_the_comment() {
	_ri_save
	# The whole reason the writer edits bytes instead of serialising: a config
	# regenerated from the parsed tree would be correct and would lose the line
	# saying why the rule is there.
	cat >> "$HL_CONFIG" <<'EOF'
// mpv has to stay on top or the subtitles vanish behind the browser
window-rule {
	match app-id="ri-upd"
	pinned
}
EOF
	hl_dispatch "reload_config" 2
	local i; i="$(_ri_index_of ri-upd)"
	local before; before="$(grep -c 'window-rule {' "$HL_CONFIG")"

	local r; r="$(_ri_set_rules "{\"changes\":[{\"op\":\"update\",\"index\":$i,\"fields\":{\"appid\":\"ri-upd\",\"ispinned\":\"1\",\"noblur\":\"1\"}}]}")"
	hl_assert_true "an update is accepted" "$(printf '%s' "$r" | jq -r '.ok')"

	i="$(_ri_index_of ri-upd)"
	hl_assert_eq "the new field took" "$(_ri_rule "$i" '.fields.noblur')" "1"
	hl_assert_eq "...and the old one is still there" \
		"$(_ri_rule "$i" '.fields.ispinned')" "1"
	hl_assert_eq "no second rule was appended" \
		"$(grep -c 'window-rule {' "$HL_CONFIG")" "$before"
	hl_assert_true "the comment above it survived" \
		"$(grep -q 'subtitles vanish' "$HL_CONFIG" && echo true || echo false)"
	hl_assert_true "...still directly above it" \
		"$(grep -A1 'subtitles vanish' "$HL_CONFIG" | tail -1 \
			| grep -q 'window-rule {' && echo true || echo false)"
	_ri_restore
}

test_set_window_rules_removes_a_rule_with_its_comment() {
	_ri_save
	cat >> "$HL_CONFIG" <<'EOF'
// this rule explains itself and should go with it
window-rule {
	match app-id="ri-del"
	no-shadow
}
EOF
	hl_dispatch "reload_config" 2
	local i; i="$(_ri_index_of ri-del)"
	hl_assert_true "the rule exists first" \
		"$([ "$i" -ge 0 ] && echo true || echo false)"

	local r; r="$(_ri_set_rules "{\"changes\":[{\"op\":\"remove\",\"index\":$i}]}")"
	hl_assert_true "a removal is accepted" "$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "the rule is gone" "$(_ri_index_of ri-del)" "-1"
	hl_assert_eq "...and so is its text" \
		"$(grep -c 'ri-del' "$HL_CONFIG")" "0"
	# The comment goes with the rule it explains. Leaving it orphaned above an
	# unrelated setting is worse than losing it.
	hl_assert_eq "...and the comment that explained it" \
		"$(grep -c 'explains itself' "$HL_CONFIG")" "0"
	hl_assert_true "the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"
	_ri_restore
}

test_set_window_rules_applies_a_batch_all_or_nothing() {
	_ri_save
	local before; before="$(md5sum "$HL_CONFIG" | cut -d' ' -f1)"
	local r; r="$(_ri_set_rules '{"changes":[{"op":"add","fields":{"appid":"ri-good","noblur":"1"}},{"op":"add","fields":{"appid":"ri-bad","no_such_field":"1"}}]}')"
	hl_assert_false "a batch with an unknown field is refused" \
		"$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "...naming the field" \
		"$(printf '%s' "$r" | jq -r '[.results[] | select(.error=="unknown-field")] | length')" "1"
	# The one that was fine did not run, and says so rather than claiming
	# success -- a UI would otherwise show one row applied and one failed for a
	# batch where neither happened.
	hl_assert_eq "...and the valid one reports not-applied" \
		"$(printf '%s' "$r" | jq -r '.results[0].error')" "not-applied"
	hl_assert_eq "nothing was written" \
		"$(md5sum "$HL_CONFIG" | cut -d' ' -f1)" "$before"
	hl_assert_eq "and neither rule exists" "$(_ri_index_of ri-good)" "-1"
	_ri_restore
}

test_the_legacy_comma_form_is_read_rather_than_swallowed() {
	_ri_save
	# `windowrule "appid:x,isfloating:1"` is what someone migrating an old
	# `windowrule=` line writes. kdl_window_rule only ever looked at CHILDREN, so
	# a node with an argument and no block built an empty value -- and an empty
	# window rule has no matchers, which means it matches EVERY window. Silent,
	# and reported as a successful parse.
	printf 'windowrule appid:ri-legacy,isfloating:1\n' >> "$HL_CONFIG"
	hl_dispatch "reload_config" 2
	local i; i="$(_ri_index_of ri-legacy)"
	hl_assert_true "the legacy form produces a rule that matches something" \
		"$([ "$i" -ge 0 ] && echo true || echo false)"
	hl_assert_eq "...with the field it asked for" \
		"$(_ri_rule "$i" '.fields.isfloating')" "1"
	# It has a node and therefore a span, so it can be rewritten -- and doing so
	# upgrades it to a block, which is the spelling everything else uses.
	hl_assert_true "...and it is editable, because it has a node" \
		"$(_ri_rule "$i" '.source.editable')"

	local r; r="$(_ri_set_rules "{\"changes\":[{\"op\":\"update\",\"index\":$i,\"fields\":{\"appid\":\"ri-legacy\",\"isfloating\":\"1\",\"noblur\":\"1\"}}]}")"
	hl_assert_true "editing it is accepted" "$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_true "...and rewrites it as a block" \
		"$(grep -q 'window-rule {' "$HL_CONFIG" && echo true || echo false)"
	hl_assert_eq "...with no leftover legacy line" \
		"$(grep -c '^windowrule ' "$HL_CONFIG")" "0"
	i="$(_ri_index_of ri-legacy)"
	hl_assert_eq "...and both fields set" "$(_ri_rule "$i" '.fields.noblur')" "1"
	_ri_restore
}

test_an_empty_window_rule_is_ignored_rather_than_matching_everything() {
	_ri_save
	local before; before="$(hl_get "get window-rules" | jq '.count')"
	printf 'window-rule { }\n' >> "$HL_CONFIG"
	hl_dispatch "reload_config" 2
	# A rule with no matchers matches every window. One that also sets nothing is
	# harmless today and a trap the moment someone adds a field to it, so it is
	# refused with a warning rather than quietly installed.
	hl_assert_eq "an empty rule creates nothing" \
		"$(hl_get "get window-rules" | jq '.count')" "$before"
	_ri_restore
}

test_set_window_rules_refuses_a_generated_file() {
	_ri_save
	local gen="$HL_OUTDIR/ri-gen.kdl"
	printf '// ! Auto-generated file. Do not edit directly.\nwindow-rule {\n\tmatch app-id="ri-gen"\n\tno-blur\n}\n' > "$gen"
	printf 'source "%s"\n' "$gen" >> "$HL_CONFIG"
	hl_dispatch "reload_config" 2
	local sum; sum="$(md5sum "$gen" | cut -d' ' -f1)"
	local i; i="$(_ri_index_of ri-gen)"
	hl_assert_true "a rule in a generated file is listed" \
		"$([ "$i" -ge 0 ] && echo true || echo false)"
	# Editable and writable are different questions. The span is perfectly good
	# and rewriting it would work -- and be undone the next time the generator
	# runs, which looks like the compositor forgetting things at random.
	hl_assert_false "...but not editable" "$(_ri_rule "$i" '.source.editable')"

	local r; r="$(_ri_set_rules "{\"changes\":[{\"op\":\"update\",\"index\":$i,\"fields\":{\"appid\":\"ri-gen\",\"noblur\":\"0\"}}]}")"
	hl_assert_eq "editing it is refused" \
		"$(printf '%s' "$r" | jq -r '.results[0].error')" "read-only-source"
	hl_assert_eq "and the generated file is untouched" \
		"$(md5sum "$gen" | cut -d' ' -f1)" "$sum"
	_ri_restore
}

test_set_binds_round_trips_a_keybind() {
	_ri_save
	local r; r="$(_ri_set_binds '{"changes":[{"op":"add","chord":"Alt+F7","action":"combo_view","args":["2"],"flags":{"release":true}}]}')"
	hl_assert_true "an added bind is accepted" \
		"$(printf '%s' "$r" | jq -r '.ok')"

	local b; b="$(hl_get "get binds")"
	local got; got="$(printf '%s' "$b" | jq -c '[.binds[] | select(.chord=="Alt+F7")][0]')"
	hl_assert_eq "it reads back with its chord" \
		"$(printf '%s' "$got" | jq -r '.chord')" "Alt+F7"
	hl_assert_eq "...its dispatch" \
		"$(printf '%s' "$got" | jq -r '.action')" "combo_view"
	hl_assert_eq "...its argument" \
		"$(printf '%s' "$got" | jq -r '.args[0]')" "2"
	# The round trip that matters: a flag written by the writer has to come back
	# through the reader. These two halves were built separately and each could
	# be wrong on its own.
	hl_assert_true "...and its flag" \
		"$(printf '%s' "$got" | jq -r '.flags.release')"
	hl_assert_true "it went into a binds block" \
		"$(grep -q 'Alt+F7 release=true' "$HL_CONFIG" && echo true || echo false)"
	hl_assert_true "the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"
	_ri_restore
}

test_set_binds_refuses_an_action_that_does_not_exist() {
	_ri_save
	local before; before="$(md5sum "$HL_CONFIG" | cut -d' ' -f1)"
	# A config naming an unknown dispatch fails to reload, and the failure
	# surfaces at the next login rather than at the save -- the worst place for
	# it. Caught here instead.
	local r; r="$(_ri_set_binds '{"changes":[{"op":"add","chord":"Alt+F6","action":"not_a_dispatch"}]}')"
	hl_assert_false "an unknown dispatch is refused" \
		"$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "...by name" \
		"$(printf '%s' "$r" | jq -r '.results[0].error')" "unknown-action"
	hl_assert_eq "and nothing was written" \
		"$(md5sum "$HL_CONFIG" | cut -d' ' -f1)" "$before"
	_ri_restore
}

test_set_binds_removes_a_bind() {
	_ri_save
	_ri_set_binds '{"changes":[{"op":"add","chord":"Alt+F5","action":"combo_view","args":["3"]}]}' >/dev/null
	local i; i="$(hl_get "get binds" \
		| jq -r '[.binds[] | select(.chord=="Alt+F5")][0].index // -1')"
	hl_assert_true "the bind exists first" \
		"$([ "$i" -ge 0 ] && echo true || echo false)"

	local r; r="$(_ri_set_binds "{\"changes\":[{\"op\":\"remove\",\"index\":$i}]}")"
	hl_assert_true "a removal is accepted" "$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "the bind is gone" \
		"$(hl_get "get binds" | jq '[.binds[] | select(.chord=="Alt+F5")] | length')" "0"
	hl_assert_eq "...and so is its line" \
		"$(grep -c 'Alt+F5' "$HL_CONFIG")" "0"
	hl_assert_true "the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"
	_ri_restore
}

test_writing_rules_does_not_respawn_the_exec_list() {
	_ri_save
	# The bug the option writer already had to fix once, one level up.
	# reload_config runs run_exec() because that is what a reload IS -- but
	# saving a window rule is not a reload, and on a real machine the exec list
	# is an `xrdb -load`. Five saves would be five of them.
	local marker="$HL_OUTDIR/ri-exec-marker"
	rm -f "$marker"
	printf 'spawn sh -c "echo x >> %s"\n' "$marker" >> "$HL_CONFIG"
	hl_dispatch "reload_config" 3
	local after_reload; after_reload="$(wc -l < "$marker" 2>/dev/null || echo 0)"
	hl_assert_true "a real reload does run the exec list" \
		"$([ "${after_reload:-0}" -ge 1 ] && echo true || echo false)"

	_ri_set_rules '{"changes":[{"op":"add","fields":{"appid":"ri-exec","noblur":"1"}}]}' >/dev/null
	sleep 2
	local after_write; after_write="$(wc -l < "$marker" 2>/dev/null || echo 0)"
	hl_assert_eq "but writing a rule does not" \
		"${after_write:-0}" "${after_reload:-0}"
	hl_assert_true "...and the rule still took" \
		"$([ "$(_ri_index_of ri-exec)" -ge 0 ] && echo true || echo false)"
	rm -f "$marker"
	_ri_restore
}

test_a_batch_of_edits_does_not_shift_itself_apart() {
	_ri_save
	# The property that is silently wrong if the edits are applied in the order
	# they arrive. Every splice moves every offset after it, so editing front to
	# back leaves the second span pointing into the middle of whatever the first
	# edit produced -- and the result still parses, which is what makes it a bug
	# that ships. Edits are applied back to front instead.
	cat >> "$HL_CONFIG" <<'EOF'
window-rule {
	match app-id="ri-b1"
	no-blur
}
window-rule {
	match app-id="ri-b2"
	no-shadow
}
window-rule {
	match app-id="ri-b3"
	pinned
}
EOF
	hl_dispatch "reload_config" 2
	local i1 i2 i3
	i1="$(_ri_index_of ri-b1)"; i2="$(_ri_index_of ri-b2)"; i3="$(_ri_index_of ri-b3)"
	hl_assert_true "three rules to work with" \
		"$([ "$i1" -ge 0 ] && [ "$i2" -ge 0 ] && [ "$i3" -ge 0 ] \
			&& echo true || echo false)"

	# The first one grows a lot, which is what moves everything below it, and the
	# LAST one is removed. Listed first-to-last on purpose: if the writer honours
	# the request order rather than sorting, this is where it breaks.
	local r; r="$(_ri_set_rules "{\"changes\":[
		{\"op\":\"update\",\"index\":$i1,\"fields\":{\"appid\":\"ri-b1\",\"noblur\":\"1\",\"isnoshadow\":\"1\",\"ispinned\":\"1\",\"isnoradius\":\"1\",\"isnotitlebar\":\"1\"}},
		{\"op\":\"update\",\"index\":$i2,\"fields\":{\"appid\":\"ri-b2\",\"isnoshadow\":\"1\",\"noblur\":\"1\"}},
		{\"op\":\"remove\",\"index\":$i3}]}")"
	hl_assert_true "the batch is accepted" "$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_true "the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"

	i1="$(_ri_index_of ri-b1)"; i2="$(_ri_index_of ri-b2)"
	hl_assert_eq "the first rule got its new fields" \
		"$(_ri_rule "$i1" '.fields.isnotitlebar')" "1"
	hl_assert_eq "...and kept its matcher" \
		"$(_ri_rule "$i1" '.fields.appid')" "ri-b1"
	hl_assert_eq "the second rule got its new field" \
		"$(_ri_rule "$i2" '.fields.noblur')" "1"
	hl_assert_eq "...and kept its own matcher, not the first one's" \
		"$(_ri_rule "$i2" '.fields.appid')" "ri-b2"
	hl_assert_eq "the third is gone" "$(_ri_index_of ri-b3)" "-1"
	hl_assert_eq "...and nothing is left of it" \
		"$(grep -c 'ri-b3' "$HL_CONFIG")" "0"
	_ri_restore
}

# ── capture-chord ───────────────────────────────────────────────────────────

_ri_active_tag() {
	hl_get "get all-tags" \
		| jq -r '[.all_tags[0].tags[] | select(.is_active)][0].index // 0'
}

# Start a capture in the background and return the file its reply will land in.
# The reply is DEFERRED -- the compositor answers when a key is pressed, not when
# the command is sent -- so amsg sits there until then.
_ri_capture_start() {
	: > "$HL_OUTDIR/chord.txt"
	( ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" \
		"$HL_REPO/build/amsg" capture-chord > "$HL_OUTDIR/chord.txt" 2>&1 & )
	sleep 1
}

_ri_captured() { cat "$HL_OUTDIR/chord.txt" 2>/dev/null; }

test_capture_chord_reports_what_was_pressed() {
	_ri_capture_start
	"$HL_WLVKBD" press LEFTMETA LEFTSHIFT Q >/dev/null 2>&1
	sleep 1.5
	hl_assert_eq "a modified chord comes back in the config's own spelling" \
		"$(_ri_captured | jq -r '.chord')" "SUPER+SHIFT+Q"

	_ri_capture_start
	"$HL_WLVKBD" press LEFTCTRL LEFTALT DELETE >/dev/null 2>&1
	sleep 1.5
	# The KEY NAME is whatever xkb calls it, which is what parse_key reads back
	# -- `Delete`, not `DEL` or `KEY_DELETE`. Naming it any other way would need a
	# table kept in step with xkb by hand.
	hl_assert_eq "...using xkb's name for the key" \
		"$(_ri_captured | jq -r '.chord')" "CTRL+ALT+Delete"

	_ri_capture_start
	"$HL_WLVKBD" press F5 >/dev/null 2>&1
	sleep 1.5
	hl_assert_eq "an unmodified key is a chord too" \
		"$(_ri_captured | jq -r '.chord')" "F5"
}

test_capture_chord_waits_for_a_real_key() {
	_ri_capture_start
	"$HL_WLVKBD" press LEFTMETA >/dev/null 2>&1
	sleep 1.5
	# Holding Super is the beginning of a chord, not one. Without this the first
	# modifier down would be captured instantly and every chord would come out as
	# "SUPER".
	hl_assert_eq "a modifier on its own does not end the capture" \
		"$(_ri_captured)" ""
	"$HL_WLVKBD" press F2 >/dev/null 2>&1
	sleep 1.5
	hl_assert_eq "...and the next real key does" \
		"$(_ri_captured | jq -r '.chord')" "F2"
}

test_capture_chord_sees_chords_that_are_already_bound() {
	_ri_save
	# THE reason this lives in the compositor rather than in the settings window.
	# Bindings are taken before the focused surface sees the key, so a client
	# reading its own key events would receive everything EXCEPT the combinations
	# that are already bound -- exactly the ones you reach for when rebinding.
	#
	# The bound action is `set_option`, and the observable is a config VALUE,
	# because both cheaper choices turned out to be order-dependent. combo_view
	# keeps a process-global chord flag that only a real key release clears; and
	# `view` then ORs rather than replaces while that flag is set, so an earlier
	# module leaving it on made "which tag is active" mean something different
	# here. This assertion passed alone and failed twice in the full suite before
	# landing on an observable with no state behind it at all.
	cat >> "$HL_CONFIG" <<'EOF'
binds { Alt+F12 { set_option "gappoh" "23"; } }
EOF
	hl_dispatch "reload_config" 2
	local before; before="$(hl_get 'get config' | jq -r '.values.gappoh.value')"
	hl_assert_true "the probe value does not start at the target" \
		"$([ "$before" != "23" ] && echo true || echo false)"

	_ri_capture_start
	"$HL_WLVKBD" press LEFTALT F12 >/dev/null 2>&1
	sleep 1.5
	hl_assert_eq "a bound chord is still captured" \
		"$(_ri_captured | jq -r '.chord')" "ALT+F12"
	# And it must not ALSO run. A captured Super+Q that closed a window would be
	# a keybind editor that shuts the window you are editing from.
	hl_assert_eq "...and does not fire while it is being captured" \
		"$(hl_get 'get config' | jq -r '.values.gappoh.value')" "$before"

	# The keyboard is not left captured: the same chord works immediately after.
	"$HL_WLVKBD" press LEFTALT F12 >/dev/null 2>&1
	sleep 1
	hl_assert_eq "...and fires normally once capture is over" \
		"$(hl_get 'get config' | jq -r '.values.gappoh.value')" "23"
	_ri_restore
}

test_capture_chord_can_be_cancelled_and_is_exclusive() {
	_ri_capture_start
	"$HL_WLVKBD" press ESC >/dev/null 2>&1
	sleep 1.5
	hl_assert_eq "Escape cancels" "$(_ri_captured | jq -r '.error')" "cancelled"

	# Unmodified Escape only, so Shift+Escape is still a chord you can bind.
	_ri_capture_start
	"$HL_WLVKBD" press LEFTSHIFT ESC >/dev/null 2>&1
	sleep 1.5
	hl_assert_eq "...but a modified Escape is a chord" \
		"$(_ri_captured | jq -r '.chord')" "SHIFT+Escape"

	_ri_capture_start
	local second; second="$(ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" \
		"$HL_REPO/build/amsg" capture-chord)"
	hl_assert_eq "a second capture is refused rather than queued" \
		"$(printf '%s' "$second" | jq -r '.error')" "busy"
	"$HL_WLVKBD" press F3 >/dev/null 2>&1
	sleep 1
	hl_assert_eq "...and the first one still gets its chord" \
		"$(_ri_captured | jq -r '.chord')" "F3"
	hl_assert_true "and the compositor is unharmed" \
		"$(hl_get 'get version' | grep -q version && echo true || echo false)"
}

test_a_captured_chord_can_be_written_as_a_bind() {
	_ri_save
	# The loop closed: capture a chord, write it, read it back. Each half was
	# built separately and either could be wrong on its own -- a chord that
	# formats prettily and does not parse would pass every test above.
	_ri_capture_start
	"$HL_WLVKBD" press LEFTMETA LEFTALT F4 >/dev/null 2>&1
	sleep 1.5
	local chord; chord="$(_ri_captured | jq -r '.chord')"
	hl_assert_eq "the chord was captured" "$chord" "SUPER+ALT+F4"

	local r; r="$(_ri_set_binds "{\"changes\":[{\"op\":\"add\",\"chord\":\"$chord\",\"action\":\"kill_client\"}]}")"
	hl_assert_true "it is accepted as a bind" "$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "...and reads back unchanged" \
		"$(hl_get "get binds" | jq -r --arg c "$chord" \
			'[.binds[] | select(.chord==$c)][0].action')" "kill_client"
	hl_assert_true "...and the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' \
			&& echo true || echo false)"
	_ri_restore
}
