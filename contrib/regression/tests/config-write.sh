# config-write.sh — `set-config`: applying changes, and making them stick.
#
# `dispatch set_option` could always change an option in memory. It writes
# nothing, so the change is gone at the next reload -- a setting that vanishes
# when you reload is a preview, not a setting. This is the other half.
#
# THIS MODULE WRITES TO $HL_CONFIG, so it restores a pristine copy after every
# test, the way bar.sh does. Modules run in name order, which puts this one ahead
# of `geometry` -- and geometry's assertions depend on gaps and border widths
# that a test here could have left changed. Cross-module state leakage in this
# suite is not hypothetical: an assertion in config-ipc.sh dispatched
# `toggle_gaps` to prove an action was real, gaps stayed off for the rest of the
# run, and geometry failed three modules later for a reason nothing pointed at.

_cw_pristine="" # set on first use

_cw_save() {
	if [ -z "$_cw_pristine" ]; then
		_cw_pristine="$HL_OUTDIR/config.cw-pristine.kdl"
		cp "$HL_CONFIG" "$_cw_pristine"
	fi
}

_cw_restore() {
	[ -n "$_cw_pristine" ] || return 0
	cp "$_cw_pristine" "$HL_CONFIG"
	rm -f "$HL_CONFIG.bak"
	hl_dispatch "reload_config" 2
}

_cw() { # _cw '<json>'  -> the reply on stdout
	ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" amsg "set-config $1" 2>/dev/null
}

_cw_stdin() { # _cw_stdin < file
	ASTEROIDZ_INSTANCE_SIGNATURE="$HL_SIG" "$HL_REPO/build/amsg" set-config @- 2>/dev/null
}

_cw_val() { hl_get "get config" | jq -r ".values.\"$1\".value"; }
_cw_kind() { hl_get "get config" | jq -r ".values.\"$1\".source.kind"; }

test_set_config_persists_and_survives_a_reload() {
	_cw_save
	local r; r="$(_cw '{"changes":[{"path":"layout/border/width","value":"11"}]}')"
	hl_assert_true "a single change is accepted" \
		"$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "...and applied" "$(_cw_val borderpx)" "11"
	hl_assert_true "...naming the file it wrote" \
		"$(printf '%s' "$r" | jq -e --arg f "$HL_CONFIG" '.written | index($f) != null' >/dev/null 2>&1 && echo true || echo false)"
	# The whole point. set_option could already do everything above.
	hl_dispatch "reload_config" 2
	hl_assert_eq "the value survives a reload" "$(_cw_val borderpx)" "11"
	hl_assert_eq "...and reads as coming from the file" "$(_cw_kind borderpx)" "file"
	_cw_restore
}

test_set_config_refuses_out_of_range_rather_than_clamping() {
	_cw_save
	local before; before="$(_cw_val borderpx)"
	local r; r="$(_cw '{"changes":[{"key":"borderpx","value":"9999"}]}')"
	hl_assert_false "an out-of-range value is refused" \
		"$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "...with a code a UI can act on" \
		"$(printf '%s' "$r" | jq -r '.results[0].error')" "out-of-range"
	# The bounds come back, so a UI can correct its own control instead of
	# guessing. Clamping silently is how a panel ends up showing 9999 while the
	# compositor runs 200.
	hl_assert_eq "...and the bound it broke" \
		"$(printf '%s' "$r" | jq -r '.results[0].max')" "200"
	hl_assert_eq "the value is unchanged, not clamped" "$(_cw_val borderpx)" "$before"
	_cw_restore
}

test_set_config_applies_a_batch_all_or_nothing() {
	_cw_save
	local g; g="$(_cw_val gappih)"
	local r; r="$(_cw '{"changes":[{"key":"gappih","value":"33"},{"key":"not_a_real_key","value":"1"}]}')"
	hl_assert_false "a batch with one bad change is refused" \
		"$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "...naming the bad one" \
		"$(printf '%s' "$r" | jq -r '.results[] | select(.error=="unknown-key") | .key')" \
		"not_a_real_key"
	# The good one must report that it did not run, not that it succeeded. A UI
	# told "ok" for a change that never happened will show the wrong value.
	hl_assert_eq "...and saying the good one never ran" \
		"$(printf '%s' "$r" | jq -r '.results[] | select(.key=="gappih") | .error')" \
		"not-applied"
	hl_assert_eq "the good change was NOT applied" "$(_cw_val gappih)" "$g"
	hl_assert_eq "...and nothing was written" \
		"$(printf '%s' "$r" | jq -r '.written | length')" "0"
	_cw_restore
}

test_set_config_persist_false_is_memory_only() {
	_cw_save
	local mt; mt="$(stat -c %Y "$HL_CONFIG")"
	local r; r="$(_cw '{"changes":[{"key":"gappiv","value":"44"}],"persist":false}')"
	hl_assert_true "a preview change is accepted" \
		"$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_false "...and says it did not persist" \
		"$(printf '%s' "$r" | jq -r '.persisted')"
	hl_assert_eq "...but the value did change" "$(_cw_val gappiv)" "44"
	hl_assert_eq "the file was not touched" "$(stat -c %Y "$HL_CONFIG")" "$mt"
	# This is the state a panel has to be able to show: it will not survive.
	hl_assert_eq "...and provenance says runtime" "$(_cw_kind gappiv)" "runtime"
	hl_dispatch "reload_config" 2
	hl_assert_true "a reload discards it" \
		"$([ "$(_cw_val gappiv)" != "44" ] && echo true || echo false)"
	_cw_restore
}

test_set_config_leaves_the_comments_alone() {
	_cw_save
	# The entire reason the writer is textual rather than a serialiser. A config
	# regenerated from the parsed tree would be correct and would still lose the
	# line explaining why a setting is what it is.
	cat >> "$HL_CONFIG" <<'EOF'
// This comment explains the gap and must survive an edit to it.
gappoh 12
EOF
	hl_dispatch "reload_config" 2
	_cw '{"changes":[{"key":"gappoh","value":"21"}]}' >/dev/null

	hl_assert_eq "the value was edited" "$(_cw_val gappoh)" "21"
	hl_assert_true "the comment above it survived" \
		"$(grep -q 'This comment explains the gap' "$HL_CONFIG" && echo true || echo false)"
	hl_assert_true "...still directly above it" \
		"$(grep -A1 'This comment explains the gap' "$HL_CONFIG" | tail -1 | grep -q '^gappoh 21$' && echo true || echo false)"
	hl_assert_true "and the config still parses" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' && echo true || echo false)"
	_cw_restore
}

test_set_config_refuses_a_generated_file_and_can_shadow_it() {
	_cw_save
	local gen="$HL_OUTDIR/cw-gen.kdl"
	printf '// ! Auto-generated file. Do not edit directly.\nlayout { border { color 0xaabbccff } }\n' > "$gen"
	printf 'source "%s"\n' "$gen" >> "$HL_CONFIG"
	hl_dispatch "reload_config" 2
	local sum; sum="$(md5sum "$gen" | cut -d' ' -f1)"

	local r; r="$(_cw '{"changes":[{"key":"bordercolor","value":"0x11223344"}]}')"
	hl_assert_false "a key owned by a generated file is refused" \
		"$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "...with a reason" \
		"$(printf '%s' "$r" | jq -r '.results[0].error')" "read-only-source"
	hl_assert_eq "the generated file is untouched" \
		"$(md5sum "$gen" | cut -d' ' -f1)" "$sum"
	hl_assert_eq "...and the value is unchanged" "$(_cw_val bordercolor)" "0xaabbccff"

	# override:true appends to the MAIN config, not to the generated file --
	# `source` is applied in place and later declarations win, so the shadow
	# survives the generator rewriting its own file.
	r="$(_cw '{"changes":[{"key":"bordercolor","value":"0x11223344"}],"override":true}')"
	hl_assert_true "override:true is accepted" \
		"$(printf '%s' "$r" | jq -r '.ok')"
	hl_assert_eq "...writing to the main config" \
		"$(printf '%s' "$r" | jq -r '.results[0].file')" "$HL_CONFIG"
	hl_assert_eq "the generated file is STILL untouched" \
		"$(md5sum "$gen" | cut -d' ' -f1)" "$sum"
	hl_assert_eq "the override took" "$(_cw_val bordercolor)" "0x11223344"
	hl_dispatch "reload_config" 2
	hl_assert_eq "...and beats the sourced file on reload" \
		"$(_cw_val bordercolor)" "0x11223344"
	hl_assert_true "...saying so in a comment" \
		"$(grep -q 'shadows the' "$HL_CONFIG" && echo true || echo false)"
	_cw_restore
}

# A live preview must not cost the declaration.
#
# This is the whole of what makes `persist:false` usable, and it was broken in
# three ways at once by one line: config_source_note cleared the file, line and
# path whenever a value was set in memory, so after a preview the compositor no
# longer knew where the key was declared.
#
# Found by the settings window, which previews every edit as you make it -- so
# every Apply in that window went through the broken path. The symptom that
# surfaced was the least serious of the three.
test_set_config_a_preview_does_not_lose_the_declaration() {
	_cw_save

	# 1. A key at a NON-CANONICAL path. `misc { border_radius 9 }` is a legal
	#    spelling of a top-level `border_radius` and this config uses it, so a
	#    writer that fell back to the canonical path would append a second
	#    declaration -- leaving the misc block dead and a duplicate winning by
	#    position. Both would be in the file and both would parse.
	printf 'misc { border_radius 9 }\n' >> "$HL_CONFIG"
	hl_dispatch "reload_config" 2
	hl_assert_eq "the key is read from the misc block" \
		"$(hl_get 'get config' | jq -r '.values.border_radius.source.path')" \
		"misc/border_radius"
	# Counted before, not assumed to be one. The harness config already declares a
	# top-level `border_radius 8`, so the misc block is the SECOND declaration and
	# wins by position -- which is the case worth testing, and an assertion of
	# "exactly one afterwards" would have failed against correct code.
	local n_before; n_before="$(grep -c 'border_radius' "$HL_CONFIG")"

	_cw '{"changes":[{"key":"border_radius","value":"15"}],"persist":false}' >/dev/null
	hl_assert_eq "a preview says runtime" "$(_cw_kind border_radius)" "runtime"
	# The part that was lost: WHERE it is declared is still true after a preview.
	hl_assert_eq "...and still knows where it is declared" \
		"$(hl_get 'get config' | jq -r '.values.border_radius.source.path')" \
		"misc/border_radius"

	_cw '{"changes":[{"key":"border_radius","value":"21"}]}' >/dev/null
	hl_assert_eq "persisting after a preview adds no new declaration" \
		"$(grep -c 'border_radius' "$HL_CONFIG")" "$n_before"
	hl_assert_true "...inside the block it was written in" \
		"$(grep -q 'misc { border_radius 21 }' "$HL_CONFIG" && echo true || echo false)"
	hl_assert_eq "...and the value took" "$(_cw_val border_radius)" "21"
	_cw_restore

	# 2. A previewed REMOVAL still has a line to remove. Before the fix this
	#    reported success with the declaration untouched, because the preview had
	#    already forgotten which file it was in.
	_cw_save
	printf 'gappoh 17\n' >> "$HL_CONFIG"
	hl_dispatch "reload_config" 2
	hl_assert_eq "a key is set in the file" "$(_cw_val gappoh)" "17"
	_cw '{"changes":[{"key":"gappoh","value":null}],"persist":false}' >/dev/null
	hl_assert_true "a previewed removal shows the default" \
		"$([ "$(_cw_val gappoh)" != "17" ] && echo true || echo false)"
	_cw '{"changes":[{"key":"gappoh","value":null}]}' >/dev/null
	hl_assert_eq "...and persisting it deletes the line" \
		"$(grep -c '^gappoh' "$HL_CONFIG")" "0"
	_cw_restore

	# 3. A previewed key owned by a GENERATED file is still refused. Before the
	#    fix the preview lost the colors.kdl origin, so the next persisting write
	#    went to the main config without override:true ever being asked for --
	#    exactly the thing the read-only guard exists to prevent.
	_cw_save
	local gen="$HL_OUTDIR/cw-preview-gen.kdl"
	printf '// ! Auto-generated file. Do not edit directly.\nlayout { border { color 0xaabbccff } }\n' > "$gen"
	printf 'source "%s"\n' "$gen" >> "$HL_CONFIG"
	hl_dispatch "reload_config" 2
	local sum; sum="$(md5sum "$gen" | cut -d' ' -f1)"

	_cw '{"changes":[{"key":"bordercolor","value":"0x11223344"}],"persist":false}' >/dev/null
	hl_assert_eq "a previewed generated key still reads as read-only" \
		"$(hl_get 'get config' | jq -r '.values.bordercolor.source.writable')" "false"
	local r; r="$(_cw '{"changes":[{"key":"bordercolor","value":"0x11223344"}]}')"
	hl_assert_eq "...and persisting it is still refused" \
		"$(printf '%s' "$r" | jq -r '.results[0].error')" "read-only-source"
	hl_assert_eq "the generated file is untouched" \
		"$(md5sum "$gen" | cut -d' ' -f1)" "$sum"
	hl_assert_eq "and nothing was appended to the main config" \
		"$(grep -c 'border { color' "$HL_CONFIG")" "0"
	_cw_restore
}

test_set_config_null_resets_to_the_default() {
	_cw_save
	_cw '{"changes":[{"key":"borderpx","value":"15"}]}' >/dev/null
	hl_assert_eq "a value is set first" "$(_cw_val borderpx)" "15"

	local r; r="$(_cw '{"changes":[{"key":"borderpx","value":null}]}')"
	hl_assert_true "null is accepted" "$(printf '%s' "$r" | jq -r '.ok')"
	# Back to the compiled-in default, and the declaration gone from the file --
	# not "set to the default value", which would still be a declaration and
	# would still win over anything sourced later.
	hl_assert_eq "...and the value is the compiled-in default" \
		"$(_cw_val borderpx)" \
		"$(hl_get 'get config-schema' | jq -r '.options[] | select(.key=="borderpx") | .default')"
	hl_assert_eq "the declaration is gone from the file" \
		"$(grep -c '^borderpx' "$HL_CONFIG")" "0"
	hl_dispatch "reload_config" 2
	hl_assert_eq "...and stays gone across a reload" "$(_cw_kind borderpx)" "default"
	_cw_restore
}

test_set_config_can_write_every_described_option() {
	_cw_save
	# The corpus case. Writing one option proves the mechanism; writing all of
	# them proves the SCHEMA -- every claimed path has to be one the parser can
	# read back, and every rendered value has to be legal KDL. This is what
	# caught theme/border-color and animations/enable claiming nested paths that
	# kdl_key_map had no entry for: the write succeeded and the next reload said
	# "Unknown keyword".
	local total=0 applied=0
	local grp
	for grp in appearance effects layout animations overview input; do
		hl_get "get config-schema $grp" | jq -c '{changes: [.options[] | {key: .key, value: .default}]}' \
			> "$HL_OUTDIR/cw-batch.json"
		local n; n="$(jq '.changes | length' "$HL_OUTDIR/cw-batch.json")"
		local got; got="$(_cw_stdin < "$HL_OUTDIR/cw-batch.json" | jq -r '.applied')"
		total=$((total + n))
		applied=$((applied + ${got:-0}))
	done
	hl_assert_true "every group has options to write" \
		"$([ "$total" -ge 90 ] && echo true || echo false)"
	hl_assert_eq "every described option can be written" "$applied" "$total"
	hl_assert_true "the config still parses afterwards" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>/dev/null | grep -q 'config OK' && echo true || echo false)"
	# And no reload warning, which is where the unreachable-path bug showed up.
	hl_assert_eq "...with no unknown keywords" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG" 2>&1 | grep -c 'Unknown keyword')" "0"
	hl_dispatch "reload_config" 2
	hl_assert_true "and the compositor is still healthy" \
		"$(hl_get 'get version' | grep -q version && echo true || echo false)"
	_cw_restore
}

test_set_config_keeps_a_backup() {
	_cw_save
	rm -f "$HL_CONFIG.bak"
	_cw '{"changes":[{"key":"gappov","value":"19"}]}' >/dev/null
	# A dozen options across two files can change in one Apply. "Undo the last
	# apply" should not require having thought about it beforehand -- the user
	# was already doing this by hand, judging by the config.kdl.bak.* files
	# beside the real one.
	hl_assert_true "a backup is written beside the config" \
		"$([ -f "$HL_CONFIG.bak" ] && echo true || echo false)"
	hl_assert_true "...holding the PREVIOUS contents" \
		"$(grep -q 'gappov 19' "$HL_CONFIG.bak" && echo false || echo true)"
	hl_assert_true "...and it parses too" \
		"$("$HL_ASTEROIDZ" -p -c "$HL_CONFIG.bak" 2>/dev/null | grep -q 'config OK' && echo true || echo false)"
	_cw_restore
}

test_set_config_rejects_a_malformed_request() {
	_cw_save
	hl_assert_eq "a body that is not JSON is refused" \
		"$(_cw 'not json at all' | jq -r '.error')" "bad-request"
	hl_assert_eq "an empty change list is refused" \
		"$(_cw '{"changes":[]}' | jq -r '.error')" "bad-request"
	hl_assert_true "...and the compositor is unharmed" \
		"$(hl_get 'get version' | grep -q version && echo true || echo false)"
	_cw_restore
}
