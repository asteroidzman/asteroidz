# config-ipc.sh — the config over IPC: schema, values, provenance, watch.
#
# `asteroidz -S` checks the schema against the parser and `-L`/`-D` feed the
# static coverage checkers, but none of those go through the socket. These do,
# because the settings UI is a client and everything it sees arrives as JSON.
#
# The point of most of these is not "does the field exist" but "does it say
# something TRUE". A schema whose defaults are wrong still parses; provenance
# naming the wrong file still parses; a watch that pushes the whole config every
# time still parses.

_cfg_json() { hl_get "$1" 2>/dev/null; }

test_config_schema_is_served_whole_and_cacheable() {
	local s; s="$(_cfg_json "get config-schema")"
	local n; n="$(printf '%s' "$s" | jq '.options | length' 2>/dev/null)"
	hl_assert_true "get config-schema returns options" \
		"$([ "${n:-0}" -ge 90 ] && echo true || echo false)"
	hl_assert_true "...with a schema_version" \
		"$(printf '%s' "$s" | jq -e 'has("schema_version")' >/dev/null 2>&1 && echo true || echo false)"
	hl_assert_true "...and groups every option belongs to" \
		"$(printf '%s' "$s" | jq -e '
			[.groups[].name] as $g
			| [.options[].group] | unique | all(. as $x | $g | index($x) != null)
		' >/dev/null 2>&1 && echo true || echo false)"

	# The digest is the whole reason a client can cache this. It has to match the
	# one the full response carries, or a cache revalidates against nothing.
	local d1 d2
	d1="$(printf '%s' "$s" | jq -r '.digest')"
	d2="$(_cfg_json "get config-schema-digest" | jq -r '.digest')"
	hl_assert_eq "the digest matches the full schema's" "$d2" "$d1"
	hl_assert_true "the digest is stable across calls" \
		"$([ "$(_cfg_json 'get config-schema-digest' | jq -r .digest)" = "$d1" ] \
			&& echo true || echo false)"
}

test_config_schema_describes_types_ranges_and_enums() {
	local s; s="$(_cfg_json "get config-schema")"
	# A range where one exists, so a UI can bound its own control instead of
	# discovering the clamp by being refused.
	hl_assert_true "a clamped option carries min and max" \
		"$(printf '%s' "$s" | jq -e '
			.options[] | select(.key=="borderpx") | (.min==0 and .max==200)
		' >/dev/null 2>&1 && echo true || echo false)"
	# Enum members by NAME: the field holds an int, and a UI offering "0" and "1"
	# for a blend space is useless.
	hl_assert_true "an enum lists its members by name" \
		"$(printf '%s' "$s" | jq -e '
			.options[] | select(.key=="srgb_blending")
			| ([.enum[].name] | index("linear")) != null
		' >/dev/null 2>&1 && echo true || echo false)"
	# The string cap. Without it a text box silently loses the tail --
	# animation_type_open is written with "%.9s".
	hl_assert_true "a capped string reports its max_length" \
		"$(printf '%s' "$s" | jq -e '
			.options[] | select(.key=="animation_type_open") | .max_length==9
		' >/dev/null 2>&1 && echo true || echo false)"
	hl_assert_true "a colour is typed as a colour, not a string" \
		"$(printf '%s' "$s" | jq -e '
			.options[] | select(.key=="bordercolor") | .type=="color"
		' >/dev/null 2>&1 && echo true || echo false)"
}

test_config_values_cover_the_schema() {
	local schema_keys values_keys
	schema_keys="$(_cfg_json "get config-schema" | jq -r '.options[].key' | sort)"
	values_keys="$(_cfg_json "get config" | jq -r '.values | keys[]' | sort)"
	# A set difference, not a count: a broken `offset` in one entry would leave
	# that key out while the totals still matched.
	hl_assert_eq "every schema key appears in get config" \
		"$(comm -23 <(echo "$schema_keys") <(echo "$values_keys") | wc -l)" "0"
	hl_assert_eq "and get config invents none" \
		"$(comm -13 <(echo "$schema_keys") <(echo "$values_keys") | wc -l)" "0"
}

test_config_values_carry_colours_in_both_forms() {
	local c; c="$(_cfg_json "get config" | jq '.values.bordercolor')"
	hl_assert_true "a colour has a 0xRRGGBBAA value" \
		"$(printf '%s' "$c" | jq -e '.value | test("^0x[0-9a-f]{8}$")' >/dev/null 2>&1 && echo true || echo false)"
	hl_assert_true "...and four floats to paint with" \
		"$(printf '%s' "$c" | jq -e '.rgba | length == 4' >/dev/null 2>&1 && echo true || echo false)"
	# The two must AGREE, on every channel. Truncating instead of rounding makes
	# 0x2c come back as 0x2b, so a settings app that reads the floats and writes
	# the hex appears to darken the theme a little every time it saves.
	#
	# Compared in python, not jq: jq's `tonumber` handles decimal only, so
	# "0x44" | tonumber is an error, and the first version of this assertion was
	# comparing nothing at all -- it failed against a build whose colours were
	# exactly right.
	hl_assert_true "the hex and the floats agree on every channel" \
		"$(printf '%s' "$c" | python3 -c '
import json, sys
d = json.load(sys.stdin)
v = d["value"]
hexes = [int(v[2 + i * 2 : 4 + i * 2], 16) for i in range(4)]
floats = [round(c * 255) for c in d["rgba"]]
print("true" if hexes == floats else "false")
')"
}

test_config_provenance_names_the_file_and_line() {
	# borderpx is set by the shared harness config, so it must not read as a
	# default -- and it must point at the file that actually sets it.
	local p; p="$(_cfg_json "get config" | jq '.values.borderpx.source')"
	hl_assert_eq "a value set in the config reports kind=file" \
		"$(printf '%s' "$p" | jq -r '.kind')" "file"
	hl_assert_eq "...naming the file it was read from" \
		"$(printf '%s' "$p" | jq -r '.file')" "$HL_CONFIG"
	hl_assert_true "...with a line number in it" \
		"$(printf '%s' "$p" | jq -e '.line > 0' >/dev/null 2>&1 && echo true || echo false)"
	hl_assert_true "...and the file is writable" \
		"$(printf '%s' "$p" | jq -e '.writable' >/dev/null 2>&1 && echo true || echo false)"
	# The line has to be RIGHT, not merely present.
	local line; line="$(printf '%s' "$p" | jq -r '.line')"
	hl_assert_true "the line number really holds that setting" \
		"$(sed -n "${line}p" "$HL_CONFIG" | grep -q 'borderpx' && echo true || echo false)"
}

test_config_provenance_refuses_a_generated_file() {
	# A sourced file carrying a generator's marker must come back unwritable, or
	# a settings app will happily overwrite something matugen regenerates on the
	# next wallpaper change.
	local gen="$HL_OUTDIR/generated-colors.kdl"
	cat > "$gen" <<'EOF'
// ! Auto-generated file. Do not edit directly.
layout { border { color 0x123456ff } }
EOF
	printf 'source "%s"\n' "$gen" >> "$HL_CONFIG"
	hl_dispatch "reload_config" 2

	local p; p="$(_cfg_json "get config" | jq '.values.bordercolor.source')"
	hl_assert_eq "a key from a generated file names that file" \
		"$(printf '%s' "$p" | jq -r '.file')" "$gen"
	hl_assert_false "...and is reported unwritable" \
		"$(printf '%s' "$p" | jq -r '.writable')"
	hl_assert_eq "...with the reason" \
		"$(printf '%s' "$p" | jq -r '.reason')" "matugen"
	hl_assert_eq "...and the value from it actually took" \
		"$(_cfg_json "get config" | jq -r '.values.bordercolor.value')" "0x123456ff"
	# The file list says the same thing, so a UI can explain itself without
	# walking every key.
	hl_assert_true "the file list marks it unwritable too" \
		"$(_cfg_json "get config" | jq -e --arg f "$gen" '
			.files[] | select(.path == $f) | .writable == false
		' >/dev/null 2>&1 && echo true || echo false)"

	# Put the config back: every later test in this module, and every later
	# module, reads it. Removing just the line this test appended, because the
	# harness has no pristine copy to restore from and rewriting the whole file
	# would drop whatever an earlier module added.
	grep -v "^source \"$gen\"$" "$HL_CONFIG" > "$HL_CONFIG.tmp" \
		&& mv "$HL_CONFIG.tmp" "$HL_CONFIG"
	hl_dispatch "reload_config" 2
	hl_assert_eq "the generated source is gone again" \
		"$(_cfg_json "get config" | jq -r '.values.bordercolor.source.kind')" "default"
}

test_config_provenance_distinguishes_a_runtime_change() {
	# set_option is memory-only, and a reload silently undoes it. That is exactly
	# the state a settings panel has to be able to show, and it was invisible
	# before provenance existed.
	hl_dispatch "set_option,borderpx,7" 1
	local p; p="$(_cfg_json "get config" | jq '.values.borderpx.source')"
	hl_assert_eq "a set_option change reports kind=runtime" \
		"$(printf '%s' "$p" | jq -r '.kind')" "runtime"
	hl_assert_eq "...and the new value is served" \
		"$(_cfg_json "get config" | jq -r '.values.borderpx.value')" "7"

	hl_dispatch "reload_config" 2
	hl_assert_eq "a reload puts provenance back to the file" \
		"$(_cfg_json "get config" | jq -r '.values.borderpx.source.kind')" "file"
}

test_watch_config_pushes_a_diff_not_the_whole_set() {
	hl_watch_start "watch config" wcfg >/dev/null
	sleep 0.5

	# The first push is everything, because a subscriber joining mid-stream has
	# nothing for a diff to apply to.
	hl_assert_eq "the initial push carries every option" \
		"$(head -1 "$HL_OUTDIR/wcfg.log" | jq -r '.reason')" "initial"
	hl_assert_true "...all of them" \
		"$(head -1 "$HL_OUTDIR/wcfg.log" | jq -e '.count >= 90' >/dev/null 2>&1 && echo true || echo false)"

	local before; before="$(hl_watch_line_count wcfg)"
	hl_dispatch "set_option,borderpx,9" 1
	sleep 0.5
	hl_assert_true "a change pushes" \
		"$(hl_wait_watch_grew wcfg "$before" && echo true || echo false)"
	local last; last="$(tail -1 "$HL_OUTDIR/wcfg.log")"
	hl_assert_eq "...as a diff of one key" \
		"$(printf '%s' "$last" | jq -r '.count')" "1"
	hl_assert_eq "...naming the key that changed" \
		"$(printf '%s' "$last" | jq -r '.changed | keys[0]')" "borderpx"
	hl_assert_eq "...and carrying its provenance" \
		"$(printf '%s' "$last" | jq -r '.changed.borderpx.source.kind')" "runtime"

	# A set that changes nothing must be silent. matugen fires on every wallpaper
	# change and often lands on the same colours; waking a settings panel to tell
	# it nothing happened is the difference between a push and a poll.
	before="$(hl_watch_line_count wcfg)"
	hl_dispatch "set_option,borderpx,9" 1
	sleep 1
	hl_assert_eq "a no-op change pushes nothing" \
		"$(hl_watch_line_count wcfg)" "$before"

	hl_dispatch "reload_config" 2
}

test_dispatch_actions_are_described_for_a_bind_editor() {
	local a; a="$(_cfg_json "get dispatch-actions")"
	hl_assert_true "get dispatch-actions lists them" \
		"$(printf '%s' "$a" | jq -e '.actions | length >= 90' >/dev/null 2>&1 && echo true || echo false)"
	# Argument KINDS, not just arity: a UI that knows `view` takes a tag index can
	# offer the tags, and one that only knows "an int" cannot.
	hl_assert_eq "an action's argument kinds are named" \
		"$(printf '%s' "$a" | jq -r '.actions[] | select(.name=="focus_direction") | .args[0]')" \
		"direction"
	hl_assert_eq "...including the ones that compose with the schema" \
		"$(printf '%s' "$a" | jq -r '.actions[] | select(.name=="set_option") | .args[0]')" \
		"option-key"
	hl_assert_true "every action has a description" \
		"$(printf '%s' "$a" | jq -e '[.actions[] | select(.desc == "" or .desc == null)] | length == 0' >/dev/null 2>&1 && echo true || echo false)"
	# And the names are real, checked by contrast.
	#
	# `zoom_reset` rather than something more interesting, because this assertion
	# must not change anything: the first version dispatched `toggle_gaps`, which
	# turns gaps off GLOBALLY and stays off -- hl_reset does not restore it. The
	# whole suite then ran with no gaps, and geometry's adjust_gaps test failed
	# three modules later, having changed a gap size that was no longer being
	# drawn. Nothing in the failure pointed back here. zoom_reset sets the cursor
	# zoom to 1.0, which is already the default, so calling it is a no-op.
	#
	# Contrast, because a bare "the compositor still answers" proves nothing: a
	# good name and an invented one have to come back DIFFERENT, or the table
	# could be full of typos and this would pass.
	hl_assert_eq "a described action is accepted" \
		"$(hl_get "dispatch zoom_reset")" '{"success":true}'
	hl_assert_eq "...and an undescribed one is refused" \
		"$(hl_get "dispatch make_coffee")" '{"error":"unknown function"}'
}
