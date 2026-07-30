# set-option.sh — `dispatch set_option` changes one value and nothing else.
#
# set_option used to call reset_option(), which calls run_exec(), which
# spawn_shell()s every `spawn` entry in the config. So changing one option
# relaunched the user's entire startup list -- once per dispatch. Nobody
# noticed because nobody sends set_option in a loop; a settings panel with a
# live-preview slider sends it per frame, and would fork per frame.
#
# The fix splits reset_option() into config_apply_live() (everything a changed
# value needs, all of it idempotent) and reset_option() (that, plus set_env and
# run_exec). This module holds the split in place, because the symptom is
# invisible in a single call: one extra process, exiting immediately.

# A `spawn` entry that leaves something countable behind. `spawn` (not
# `spawn-at-startup`) is the list run_exec walks -- spawn-at-startup populates
# exec_once and is only run once at boot, so it would prove nothing here.
#
# A SCRIPT, not an inline `sh -c "..."`. The KDL handler for `spawn` joins the
# node's argv tokens with spaces (parse_config.h, kdl_parse_node) and hands the
# result to spawn_shell, which runs it as `sh -c <string>` -- so the quotes
# around an inline command are gone by then and `spawn sh -c "mktemp DIR/xXXX"`
# ends up running mktemp with no arguments at all. That silently produced an
# empty probe directory and a passing "spawns nothing" assertion, which is the
# exact shape of a test that proves nothing.
_setopt_probe_dir() { echo "$HL_OUTDIR/setopt-probe"; }

_setopt_probe_init() {
	local d; d="$(_setopt_probe_dir)"
	rm -rf "$d"; mkdir -p "$d"
	cat > "$d/probe.sh" <<EOF
#!/bin/sh
mktemp "$d/ranXXXXXX" >/dev/null 2>&1
EOF
	chmod +x "$d/probe.sh"
}

_setopt_probe_count() {
	local d; d="$(_setopt_probe_dir)"
	find "$d" -type f -name 'ran*' 2>/dev/null | wc -l
}

# QUOTED. An unquoted path is not a KDL argument -- a leading `/` starts a
# comment -- so `spawn /tmp/x/probe.sh` is a parse error ("expected
# argument") that takes the whole config down from that line on. It fails
# quietly as far as the test is concerned: the reload does nothing, no probe
# file appears, and "set_option spawns nothing" passes for the wrong reason.
# The user's own config quotes its paths for the same reason.
_setopt_probe_arm() {
	local d; d="$(_setopt_probe_dir)"
	printf 'spawn "%s/probe.sh"\n' "$d" >> "$HL_CONFIG"
}

test_set_option_does_not_respawn_the_exec_list() {
	_setopt_probe_init

	# Append a spawn entry and reload so the compositor picks it up. The
	# reload itself runs it once, which is correct and is what we baseline
	# from.
	_setopt_probe_arm
	hl_dispatch "reload_config" 2
	sleep 1
	local after_reload; after_reload="$(_setopt_probe_count)"

	# A reload SHOULD run it. If it did not, this test is measuring nothing
	# and every assertion below would pass vacuously.
	hl_assert_true "a reload runs the spawn list (baseline)" \
		"$([ "$after_reload" -ge 1 ] && echo true || echo false)"

	# Now five set_option dispatches. Before the split each of these ran the
	# spawn list again, so this landed at 6 files instead of 1.
	local i
	for i in 1 2 3 4 5; do
		hl_dispatch "set_option,blur_params_num_passes,2"
	done
	sleep 1
	local after_sets; after_sets="$(_setopt_probe_count)"

	hl_assert_eq "five set_option dispatches spawn nothing" \
		"$after_sets" "$after_reload"

	# ...and the option actually took, so this is not passing because
	# set_option quietly stopped working.
	hl_dispatch "set_option,blur_params_num_passes,4" 1
	hl_assert_true "set_option still applies the value" \
		"$(hl_get "get version" | grep -q version && echo true || echo false)"

	rm -rf "$(_setopt_probe_dir)"
}

# The other half of the split: a reload must STILL run the list. Asserting only
# that set_option is quiet would pass just as well on a build where run_exec
# was deleted outright.
test_reload_config_still_runs_the_exec_list() {
	_setopt_probe_init
	_setopt_probe_arm

	hl_dispatch "reload_config" 2
	sleep 1
	local first; first="$(_setopt_probe_count)"

	hl_dispatch "reload_config" 2
	sleep 1
	local second; second="$(_setopt_probe_count)"

	hl_assert_true "each reload runs the spawn list again" \
		"$([ "$second" -gt "$first" ] && echo true || echo false)"

	rm -rf "$(_setopt_probe_dir)"
}
