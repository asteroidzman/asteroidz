#ifndef ASTEROIDZ_IPC_CONFIG_H
#define ASTEROIDZ_IPC_CONFIG_H

/* The config, over IPC: the schema, the current values, and where they came
 * from.
 *
 * `get bar-config` already establishes the principle these follow, and its
 * comment says it plainly: the compositor is the only process that knows what
 * the config currently IS. Defaults, clamping, cross-key coherence and a palette
 * rewritten at runtime by matugen all happen here, so a client that parsed
 * config.kdl for itself would be a second reader that agrees with the first
 * until one of them gains a default.
 *
 * Three verbs, because they change at three different rates:
 *
 *   get config-schema         compile-time constant. Cache it.
 *   get config-schema-digest  ~60 bytes, so a cache can be revalidated in one
 *                             round trip across a compositor restart.
 *   get config                changes whenever anything is set.
 *
 * plus `watch config`, which pushes a DIFF rather than the whole set -- a
 * matugen reload changes nine keys and should not cost 30KB.
 */

#include "../config/config-schema.h"
#include "../config/config-source.h"
#include "../config/config-write.h"
#include "../config/rule-write.h"

/* ---------- schema ---------- */

static cJSON *ipc_schema_enum_array(const ConfigOption *o) {
	cJSON *arr = cJSON_CreateArray();
	for (size_t i = 0; i < o->n_members; i++) {
		cJSON *m = cJSON_CreateObject();
		cJSON_AddStringToObject(m, "name", o->members[i].name);
		if (o->members[i].alias)
			cJSON_AddBoolToObject(m, "alias", true);
		if (o->members[i].desc)
			cJSON_AddStringToObject(m, "desc", o->members[i].desc);
		cJSON_AddItemToArray(arr, m);
	}
	return arr;
}

static void ipc_schema_add_flags(cJSON *obj, uint32_t flags) {
	cJSON *arr = cJSON_CreateArray();
	struct {
		uint32_t bit;
		const char *name;
	} names[] = {
		{SCHEMA_NO_LIVE, "no-live"},
		{SCHEMA_NEEDS_RESTART, "needs-restart"},
		{SCHEMA_MATUGEN, "matugen"},
		{SCHEMA_ADVANCED, "advanced"},
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (flags & names[i].bit)
			cJSON_AddItemToArray(arr, cJSON_CreateString(names[i].name));
	cJSON_AddItemToObject(obj, "flags", arr);
}

/* FNV-1a over the rendered schema.
 *
 * So a client can keep the schema across compositor restarts and revalidate in
 * one small request. Cheaper than paging the schema, and a great deal less
 * protocol to maintain: paging is forever, a digest is eight characters. */
static uint32_t ipc_fnv1a(const char *s) {
	uint32_t h = 2166136261u;
	for (; *s; s++) {
		h ^= (uint8_t)*s;
		h *= 16777619u;
	}
	return h;
}

static cJSON *build_config_schema_response(const char *only_group) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddNumberToObject(resp, "schema_version", 1);

	cJSON *groups = cJSON_CreateArray();
	for (size_t g = 0; g < sizeof(config_groups) / sizeof(config_groups[0]);
		 g++) {
		if (only_group && strcmp(config_groups[g].name, only_group))
			continue;
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", config_groups[g].name);
		cJSON_AddStringToObject(o, "label", config_groups[g].label);
		cJSON_AddStringToObject(o, "desc", config_groups[g].desc);
		cJSON_AddItemToArray(groups, o);
	}
	cJSON_AddItemToObject(resp, "groups", groups);

	cJSON *opts = cJSON_CreateArray();
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		if (only_group && strcmp(o->group, only_group))
			continue;
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "key", o->key);
		if (o->path)
			cJSON_AddStringToObject(e, "path", o->path);
		else
			cJSON_AddNullToObject(e, "path");
		cJSON_AddStringToObject(e, "group", o->group);
		if (o->subgroup)
			cJSON_AddStringToObject(e, "subgroup", o->subgroup);
		cJSON_AddStringToObject(e, "label", o->label);
		cJSON_AddStringToObject(e, "desc", o->desc);
		cJSON_AddStringToObject(e, "type", schema_type_name(o->type));
		cJSON_AddStringToObject(e, "default", o->def);
		/* The cap parse_option truncates a string to. Without it a UI offers a
		 * text box that silently loses the tail -- animation_type_open is
		 * written with "%.9s". */
		if (o->size)
			cJSON_AddNumberToObject(e, "max_length", (double)(o->size - 1));
		if (!isnan(o->min))
			cJSON_AddNumberToObject(e, "min", o->min);
		if (!isnan(o->max))
			cJSON_AddNumberToObject(e, "max", o->max);
		/* Members, not type. An OPT_STRING can have a closed set too --
		 * animation types are stored as names rather than indices because the
		 * name IS the value -- and a client that keyed off the type alone would
		 * put a text box in front of a fixed list of five answers. */
		if (o->n_members)
			cJSON_AddItemToObject(e, "enum", ipc_schema_enum_array(o));
		ipc_schema_add_flags(e, o->flags);
		cJSON_AddItemToArray(opts, e);
	}
	cJSON_AddItemToObject(resp, "options", opts);

	/* Digested from the rendered options, not from the whole response: the
	 * group list is derived from the same table, and including it would only
	 * make the digest depend on the `only_group` argument. */
	char *rendered = cJSON_PrintUnformatted(opts);
	if (rendered) {
		char dig[16];
		snprintf(dig, sizeof(dig), "%08x", ipc_fnv1a(rendered));
		cJSON_AddStringToObject(resp, "digest", dig);
		free(rendered);
	}
	return resp;
}

static cJSON *build_config_schema_digest_response(void) {
	cJSON *full = build_config_schema_response(NULL);
	cJSON *resp = cJSON_CreateObject();
	cJSON *d = cJSON_GetObjectItem(full, "digest");
	cJSON_AddStringToObject(resp, "digest",
							(d && d->valuestring) ? d->valuestring : "");
	cJSON_AddNumberToObject(resp, "options", (double)CONFIG_SCHEMA_COUNT);
	cJSON_Delete(full);
	return resp;
}

/* ---------- current values ---------- */

/* Where a value came from, as the settings UI needs to reason about it. */
static cJSON *build_config_source_json(const ConfigOption *o) {
	cJSON *src = cJSON_CreateObject();
	const ConfigOrigin *g = config_source_of(o->key);
	if (!g) {
		/* Never set by anything: this is the compiled-in default. */
		cJSON_AddStringToObject(src, "kind", "default");
		cJSON_AddBoolToObject(src, "writable", true);
		return src;
	}
	if (g->file < 0 || g->file >= nconfig_files) {
		/* Nothing on disk sets it. Whether that is `runtime` or `default` depends
		 * on the VALUE, not on how it got here.
		 *
		 * `runtime` means "a reload will change this", which is the only reason a
		 * UI cares. A key set in memory to something a reload would undo is
		 * runtime; a key set in memory to the value it would have anyway -- an
		 * un-set, or a preview that happened to land on the default -- is in
		 * exactly the state it would be in if nobody had touched it, and calling
		 * that runtime promises a change that will not happen. The settings window
		 * showed "changed in memory, not saved" against a reset control for
		 * precisely this reason. */
		char now[512];
		schema_format(&config, o, now, sizeof(now));
		cJSON_AddStringToObject(src, "kind",
								strcmp(now, o->def) ? "runtime" : "default");
		cJSON_AddBoolToObject(src, "writable", true);
		return src;
	}
	const char *file = config_files[g->file];
	char why[64] = "";
	bool foreign = config_file_is_foreign(file, why, sizeof(why));
	/* `runtime`, but the file fields come too.
	 *
	 * Both halves are true after a `persist:false` write: the running value was
	 * set in memory AND there is a declaration in a file that still says something
	 * else. Reporting only the first would hide the file a UI needs to explain
	 * itself; reporting only the second would claim a value is saved when it is
	 * not. `kind` answers "is it saved", `file`/`line`/`path` answer "where does
	 * it live", and they are different questions. */
	cJSON_AddStringToObject(src, "kind", g->runtime ? "runtime" : "file");
	cJSON_AddStringToObject(src, "file", file);
	cJSON_AddNumberToObject(src, "line", g->line);
	/* The path it was ACTUALLY written at, which for ~190 keys is not the
	 * canonical one -- a writer that assumed otherwise would append a second
	 * declaration that silently wins by position. */
	cJSON_AddStringToObject(src, "path", g->path);
	cJSON_AddBoolToObject(src, "writable", !foreign);
	if (foreign)
		cJSON_AddStringToObject(src, "reason", why);
	return src;
}

static void ipc_add_config_value(cJSON *dst, const ConfigOption *o) {
	char buf[512];
	schema_format(&config, o, buf, sizeof(buf));

	cJSON *e = cJSON_CreateObject();
	cJSON_AddStringToObject(e, "value", buf);
	/* Colours carry BOTH forms. `value` is the 0xRRGGBBAA a user types and the
	 * writer round-trips; `rgba` is what a UI paints with. Neither has to guess
	 * the other's byte order, which is the same reason bar_cfg_color exists. */
	if (o->type == OPT_COLOR) {
		const float *c = schema_field(&config, o);
		cJSON *arr = cJSON_CreateArray();
		for (int i = 0; i < 4; i++)
			cJSON_AddItemToArray(arr, cJSON_CreateNumber(c[i]));
		cJSON_AddItemToObject(e, "rgba", arr);
	}
	cJSON_AddBoolToObject(e, "is_default", strcmp(buf, o->def) == 0);
	cJSON_AddItemToObject(e, "source", build_config_source_json(o));
	cJSON_AddItemToObject(dst, o->key, e);
}

static cJSON *build_config_response(const char *only_group) {
	cJSON *resp = cJSON_CreateObject();
	cJSON *vals = cJSON_CreateObject();
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		if (only_group && strcmp(o->group, only_group))
			continue;
		ipc_add_config_value(vals, o);
	}
	cJSON_AddItemToObject(resp, "values", vals);

	/* Which files the config was actually read from, and which of them the
	 * compositor is willing to write. A UI showing "managed by matugen" needs
	 * the list, not just the per-key flag. */
	cJSON *files = cJSON_CreateArray();
	for (int32_t i = 0; i < nconfig_files; i++) {
		char why[64] = "";
		bool foreign = config_file_is_foreign(config_files[i], why, sizeof(why));
		cJSON *f = cJSON_CreateObject();
		cJSON_AddStringToObject(f, "path", config_files[i]);
		cJSON_AddBoolToObject(f, "writable", !foreign);
		if (foreign)
			cJSON_AddStringToObject(f, "reason", why);
		cJSON_AddItemToArray(files, f);
	}
	cJSON_AddItemToObject(resp, "files", files);
	return resp;
}

/* ---------- the dispatch table, for a keybind editor ---------- */

/* What kind of thing an argument is, so a UI can offer the right control
 * instead of a text box. Hand-written beside parse_func_name, and checked by
 * tests/check-dispatch-actions.py against the names that function accepts --
 * the same arrangement as the option schema, for the same reason. */
typedef struct {
	const char *name;
	const char *args; /* space-separated kinds; "" for none */
	const char *desc;
} DispatchAction;

static const DispatchAction dispatch_actions[] = {
	/* windows */
	{"kill_client", "", "Close the focused window."},
	{"toggle_floating", "", "Float or tile the focused window."},
	{"toggle_all_floating", "", "Float or tile every window on the tag."},
	{"toggle_fullscreen", "", "Fullscreen the focused window."},
	{"toggle_fake_fullscreen", "",
	 "Tell the window it is fullscreen without resizing it."},
	{"toggle_maximize", "int", "Maximize the focused window."},
	{"toggle_overlay", "", "Keep the focused window above everything."},
	{"toggle_global", "", "Show the focused window on every tag."},
	{"minimize", "", "Minimize the focused window."},
	{"restore_minimized", "", "Restore the last minimized window."},
	{"pin", "", "Pin the focused window to its position."},
	{"center_window", "", "Centre a floating window."},
	{"move_window", "int int", "Move a floating window to x,y."},
	{"resize_window", "int int", "Resize a window to width,height."},
	{"smart_move_window", "direction", "Move a floating window by the snap distance."},
	{"smart_resize_window", "direction", "Resize a floating window by the snap distance."},
	{"move_resize", "string", "Start an interactive move or resize."},
	{"toggle_render_border", "", "Show or hide the focused window's border."},
	/* focus */
	{"focus_stack", "circle-direction", "Focus the next or previous window."},
	{"focus_direction", "direction", "Focus the window in a direction."},
	{"focus_last", "", "Focus the previously focused window."},
	{"focus_id", "int", "Focus a window by id."},
	{"focus_monitor", "string", "Focus a monitor by name or direction."},
	{"exchange_client", "direction", "Swap the focused window with its neighbour."},
	{"exchange_stack_client", "circle-direction", "Swap along the stack."},
	{"zoom", "", "Move the focused window to the master area."},
	/* tags */
	{"view", "tag-index", "Switch to a tag."},
	{"toggle_view", "tag-index", "Add or remove a tag from the view."},
	{"tag", "tag-index", "Move the focused window to a tag."},
	{"toggle_tag", "tag-index", "Add or remove a tag from the focused window."},
	{"tag_silent", "tag-index", "Move a window to a tag without following it."},
	{"view_to_left", "", "Switch to the tag on the left."},
	{"view_to_right", "", "Switch to the tag on the right."},
	{"view_to_left_occupied", "", "Switch left, skipping empty tags."},
	{"view_to_right_occupied", "", "Switch right, skipping empty tags."},
	{"tag_to_left", "", "Move the focused window one tag left."},
	{"tag_to_right", "", "Move the focused window one tag right."},
	{"view_cross_monitor", "tag-index", "Switch to a tag on another monitor."},
	{"tag_cross_monitor", "tag-index", "Move a window to a tag on another monitor."},
	{"tag_monitor", "string", "Move the focused window to a monitor."},
	{"combo_view", "tag-index", "Add a tag to the current view."},
	{"set_tag_name", "string", "Rename the current tag (memory only)."},
	/* layout */
	{"set_layout", "layout-name", "Switch the layout."},
	{"switch_layout", "circle-direction", "Cycle through layouts."},
	{"set_master_factor", "float", "Resize the master area."},
	{"adjust_master_count", "int", "Change how many windows are masters."},
	{"toggle_gaps", "", "Show or hide the gaps."},
	{"adjust_gaps", "int", "Change the gap size."},
	{"set_proportion", "float", "Set the focused window's proportion."},
	{"switch_proportion_preset", "circle-direction", "Cycle proportion presets."},
	{"dwindle_split_horizontal", "", "Split horizontally in the dwindle layout."},
	{"dwindle_split_vertical", "", "Split vertically in the dwindle layout."},
	{"dwindle_toggle_split_direction", "", "Flip the next dwindle split."},
	{"scroller_consume", "", "Absorb the next window into this column."},
	{"scroller_expel", "", "Push a window out of this column."},
	{"scroller_stack", "", "Stack the column."},
	/* scratchpad and overview */
	{"toggle_scratchpad", "", "Show or hide the scratchpad."},
	{"toggle_named_scratchpad", "string", "Show or hide a named scratchpad."},
	{"move_to_special_workspace", "string", "Move a window to a special workspace."},
	{"toggle_special_workspace", "string", "Show or hide a special workspace."},
	{"toggle_overview", "string", "Open the overview; `jump` for jump mode."},
	/* monitors */
	{"set_output_mode", "string string", "Set an output's resolution, WxH@Hz."},
	{"set_output_scale", "string float", "Set an output's scale."},
	{"set_output_transform", "string int",
	 "Rotate or flip an output: 0-7, the wl_output_transform values."},
	{"set_output_mode_transform", "string string int",
	 "Set an output's resolution and transform in ONE commit."},
	{"set_output_position", "string int int", "Move an output in the layout."},
	{"set_output_vrr", "string bool", "Enable adaptive sync on an output."},
	{"set_output_hdr", "string bool", "Set an output's HDR baseline."},
	{"set_output_icc", "string string", "Set an output's ICC profile path."},
	{"toggle_hdr", "", "Invert the focused output's HDR baseline."},
	{"set_sdr_luminance", "int", "Set the SDR reference luminance."},
	{"retrain_monitor", "string", "Retrain an output's link."},
	{"enable_monitor", "string", "Power an output on."},
	{"disable_monitor", "string", "Shut an output down."},
	{"toggle_monitor", "string", "Toggle an output's power."},
	{"dpms_on_monitor", "string", "Wake an output without removing it."},
	{"dpms_off_monitor", "string", "Blank an output without removing it."},
	{"dpms_toggle_monitor", "string", "Toggle an output's blanking."},
	{"create_virtual_output", "", "Create a headless output."},
	{"destroy_all_virtual_output", "", "Destroy every headless output."},
	/* zoom */
	{"zoom_in", "", "Magnify around the cursor."},
	{"zoom_out", "", "Reduce the magnification."},
	{"zoom_reset", "", "Reset the magnification."},
	/* session */
	{"spawn", "string", "Run a command."},
	{"spawn_shell", "string", "Run a command through a shell."},
	{"spawn_on_empty", "string", "Run a command only if the tag is empty."},
	{"reload_config", "", "Re-read the config from disk."},
	{"reset_avk_stats", "",
	 "Zero the Vulkan renderer's counters (see amsg get avk-stats)."},
	{"set_t_pipe", "int",
	 "M6A: the VRR pipeline constant in microseconds (see amsg get "
	 "presentation)."},
	{"reset_presentation", "",
	 "Zero the per-output presentation counters (see amsg get presentation). "
	 "The proven clock domain is kept."},
	{"set_frame_trace", "int",
	 "Diagnostic: log per-frame GPU timings and tag-transition progress at "
	 "ERROR (1 on, 0 off). Several lines per frame."},
	{"set_blur_rect_cap", "int",
	 "Diagnostic: the rectangle count past which a blur's rebuild region "
	 "collapses to its bounding box (0 restores the default). The collapse "
	 "is conservative, so this changes cost and never pixels."},
	{"set_blur_cache", "int",
	 "Enable (1) or disable (0) the monitor background blur cache. Off makes "
	 "every backdrop blur reconstruct the background for itself."},
	{"set_blur_cache_starve", "int",
	 "Diagnostic: treat one cached blur kind as having no damaged consumer "
	 "(0 none, 1 plain, 2 dark). Reproduces the ordinary frame that rebuilds "
	 "one cached image and leaves the other alone."},
	{"set_blur_chain_trace", "int",
	 "Diagnostic: log every blur chain's role, geometry and rebuilt area at "
	 "ERROR (1 on, 0 off). One line per chain, several per frame."},
	{"dump_scene", "",
	 "Log the next frame's scene nodes and AVK command stream at ERROR."},
	{"damage_all", "",
	 "Mark every output fully damaged and repaint (the damage-tracking "
	 "oracle: a screenshot before and after must be identical)."},
	{"capture_output", "",
	 "Write every output's next frame to AZ_AVK_CAPTURE_DIR as a PPM, read "
	 "back from the actual Vulkan attachment (works at every output "
	 "transform, unlike a screen-capture client)."},
	{"restart", "", "Restart the compositor in place."},
	{"quit", "", "Exit."},
	{"chvt", "int", "Switch virtual terminal."},
	{"screenshot_ui", "string", "Open the screenshot UI: region, window or screen."},
	{"set_key_mode", "string", "Switch key mode."},
	{"switch_keyboard_layout", "int", "Switch keyboard layout."},
	{"toggle_idle_inhibit", "", "Stop or allow idling."},
	{"toggle_trackpad_enable", "", "Enable or disable the trackpad."},
	/* the odd one out */
	{"set_option", "option-key option-value",
	 "Set a config option in memory only; discarded at the next reload."},
	/* and the diagnostic: not a way to drive the session, a way to look at it */
	{"dump_blur_source", "string",
	 "Write the next few backdrop-blur SOURCE images to <prefix>[,<frames>]; "
	 "no argument disarms. Vulkan only."},
};

#define DISPATCH_ACTION_COUNT                                                  \
	(sizeof(dispatch_actions) / sizeof(dispatch_actions[0]))

/* One action per line, tab separated: name, then its argument kinds. */
/* Does a dispatch by this name exist?
 *
 * For the keybind writer. A bind naming a dispatch the compositor does not know
 * writes a config that fails to reload, and the failure surfaces at the next
 * login rather than at the save -- which is the worst possible place for it. */
static bool ipc_dispatch_action_known(const char *name) {
	if (!name || !*name)
		return false;
	for (size_t i = 0; i < LENGTH(dispatch_actions); i++)
		if (!strcmp(dispatch_actions[i].name, name))
			return true;
	return false;
}

static void dispatch_actions_list(void) {
	printf("# name\targs\n");
	for (size_t i = 0; i < DISPATCH_ACTION_COUNT; i++)
		printf("%s\t%s\n", dispatch_actions[i].name, dispatch_actions[i].args);
}

static cJSON *build_dispatch_actions_response(void) {
	cJSON *resp = cJSON_CreateObject();
	cJSON *arr = cJSON_CreateArray();
	for (size_t i = 0; i < DISPATCH_ACTION_COUNT; i++) {
		cJSON *a = cJSON_CreateObject();
		cJSON_AddStringToObject(a, "name", dispatch_actions[i].name);
		cJSON_AddStringToObject(a, "desc", dispatch_actions[i].desc);
		cJSON *args = cJSON_CreateArray();
		const char *p = dispatch_actions[i].args;
		while (*p) {
			while (*p == ' ')
				p++;
			if (!*p)
				break;
			const char *start = p;
			while (*p && *p != ' ')
				p++;
			char kind[32];
			size_t n = (size_t)(p - start);
			if (n >= sizeof(kind))
				n = sizeof(kind) - 1;
			memcpy(kind, start, n);
			kind[n] = '\0';
			cJSON_AddItemToArray(args, cJSON_CreateString(kind));
		}
		cJSON_AddItemToObject(a, "args", args);
		cJSON_AddItemToArray(arr, a);
	}
	cJSON_AddItemToObject(resp, "actions", arr);
	return resp;
}

/* ---------- watch config ---------- */

/* The previous values, so a push can carry a diff.
 *
 * Compared THROUGH THE SCHEMA, field by field, never as a memcmp of the struct:
 * Config holds heap pointers (cursor_theme, theme.font_desc) and padding, and a
 * memcmp would report a change on every reload for neither reason. */
static char config_prev_values[CONFIG_SCHEMA_COUNT][512];
static bool config_prev_valid;

static void config_snapshot(void) {
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++)
		schema_format(&config, &config_schema[i], config_prev_values[i],
					  sizeof(config_prev_values[i]));
	config_prev_valid = true;
}

/* `reason` is what happened: "initial", "reload", "set". */
static cJSON *build_config_diff_response(const char *reason, bool everything) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddStringToObject(resp, "reason", reason);
	cJSON *changed = cJSON_CreateObject();
	int n = 0;
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		char now[512];
		schema_format(&config, o, now, sizeof(now));
		if (!everything && config_prev_valid &&
			!strcmp(now, config_prev_values[i]))
			continue;
		ipc_add_config_value(changed, o);
		n++;
	}
	cJSON_AddItemToObject(resp, "changed", changed);
	cJSON_AddNumberToObject(resp, "count", n);
	return resp;
}

#endif /* ASTEROIDZ_IPC_CONFIG_H */

/* ---------- set-config ---------- */

/* A VERB, not a dispatch, for three reasons visible in handle_command:
 *
 *   1. It rewrites every `,` to ` ` in its cmd[1024] copy, and the dispatch
 *      branch then tokenises on commas with a six-token cap. Values legitimately
 *      contain commas -- an animation curve is four numbers.
 *   2. cmd[1024] truncates. A batch of twenty changes is one to two kilobytes.
 *      Read from cmd_raw the way the dispatch branch does; the read buffer
 *      already grows to a megabyte.
 *   3. Dispatch's reply is a hardcoded {"success":true} with the return value
 *      discarded. Real errors are the entire point of this.
 *
 *   set-config {"changes":[{"path":"effects/blur/radius","value":"8"},
 *                          {"key":"cursor_theme","value":null}],
 *               "persist":true,"override":false}
 *
 * `value: null` removes the declaration and reverts to the compiled-in default.
 * `persist: false` is the live-preview path: memory only, nothing written. */
static cJSON *handle_set_config(const char *body) {
	cJSON *resp = cJSON_CreateObject();
	cJSON *req = cJSON_Parse(body ? body : "");
	if (!req) {
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "bad-request");
		cJSON_AddStringToObject(resp, "detail", "body is not valid JSON");
		return resp;
	}

	static ConfigWriteBatch b;
	memset(&b, 0, sizeof(b));
	cJSON *persist = cJSON_GetObjectItem(req, "persist");
	/* Persisting is the DEFAULT. A caller that means "preview only" says so;
	 * a caller that forgot means the ordinary thing, which is a setting that
	 * survives a reload. */
	b.persist = !persist || cJSON_IsTrue(persist);
	b.override_foreign = cJSON_IsTrue(cJSON_GetObjectItem(req, "override"));

	cJSON *changes = cJSON_GetObjectItem(req, "changes");
	if (!cJSON_IsArray(changes) || cJSON_GetArraySize(changes) == 0) {
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "bad-request");
		cJSON_AddStringToObject(resp, "detail", "no changes");
		cJSON_Delete(req);
		return resp;
	}

	cJSON *item;
	cJSON_ArrayForEach(item, changes) {
		if (b.n >= CONFIG_WRITE_MAX_CHANGES)
			break;
		ConfigChange *c = &b.changes[b.n++];
		cJSON *jk = cJSON_GetObjectItem(item, "key");
		cJSON *jp = cJSON_GetObjectItem(item, "path");
		const char *ident = NULL;
		if (cJSON_IsString(jk) && jk->valuestring[0]) {
			ident = jk->valuestring;
			c->opt = schema_by_key(ident);
		} else if (cJSON_IsString(jp) && jp->valuestring[0]) {
			ident = jp->valuestring;
			c->opt = schema_by_path(ident);
		}
		if (!c->opt) {
			snprintf(c->error, sizeof(c->error), "unknown-key");
			snprintf(c->detail, sizeof(c->detail), "%s",
					 ident ? ident : "no key or path given");
			/* Remembered so the reply can name it, since c->opt is NULL. */
			snprintf(c->value, sizeof(c->value), "%s", ident ? ident : "");
			continue;
		}
		cJSON *jv = cJSON_GetObjectItem(item, "value");
		if (!jv || cJSON_IsNull(jv)) {
			c->remove = true;
		} else if (cJSON_IsString(jv)) {
			snprintf(c->value, sizeof(c->value), "%s", jv->valuestring);
		} else if (cJSON_IsNumber(jv)) {
			/* Accepted for convenience, but rendered through the same path as a
			 * string so there is one formatting rule, not two. */
			double d = jv->valuedouble;
			if (d == (double)(long long)d)
				snprintf(c->value, sizeof(c->value), "%lld", (long long)d);
			else
				snprintf(c->value, sizeof(c->value), "%g", d);
		} else if (cJSON_IsBool(jv)) {
			snprintf(c->value, sizeof(c->value), "%d", cJSON_IsTrue(jv) ? 1 : 0);
		} else {
			snprintf(c->error, sizeof(c->error), "bad-value");
			snprintf(c->detail, sizeof(c->detail),
					 "value must be a string, number, bool or null");
		}
	}
	cJSON_Delete(req);

	bool ok = config_write_plan(&b);
	if (ok && b.persist) {
		ok = config_write_stage(&b) && config_write_commit(&b);
	}
	if (ok)
		config_write_apply_memory(&b);

	cJSON_AddBoolToObject(resp, "ok", ok);
	cJSON_AddBoolToObject(resp, "persisted", ok && b.persist);
	if (b.error[0]) {
		cJSON_AddStringToObject(resp, "error", b.error);
		cJSON_AddStringToObject(resp, "detail", b.detail);
	}

	int applied = 0;
	cJSON *results = cJSON_CreateArray();
	for (size_t i = 0; i < b.n; i++) {
		ConfigChange *c = &b.changes[i];
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "key",
								c->opt ? c->opt->key : c->value);
		if (c->error[0]) {
			cJSON_AddBoolToObject(r, "ok", false);
			cJSON_AddStringToObject(r, "error", c->error);
			cJSON_AddStringToObject(r, "detail", c->detail);
			/* The bounds, so a UI can fix its own control rather than guess. */
			if (c->opt && !strcmp(c->error, "out-of-range")) {
				if (!isnan(c->opt->min))
					cJSON_AddNumberToObject(r, "min", c->opt->min);
				if (!isnan(c->opt->max))
					cJSON_AddNumberToObject(r, "max", c->opt->max);
			}
		} else if (!ok) {
			/* Nothing was applied, so this one did not fail -- it never ran.
			 * Saying "ok" here would be a lie a UI would act on. */
			cJSON_AddBoolToObject(r, "ok", false);
			cJSON_AddStringToObject(r, "error", "not-applied");
			cJSON_AddStringToObject(r, "detail",
									"another change in the batch was refused");
		} else {
			cJSON_AddBoolToObject(r, "ok", true);
			char now[512];
			schema_format(&config, c->opt, now, sizeof(now));
			cJSON_AddStringToObject(r, "value", now);
			if (b.persist && c->file >= -1) {
				cJSON_AddStringToObject(
					r, "file", config_files[c->file < 0 ? 0 : c->file]);
				if (c->created)
					cJSON_AddBoolToObject(r, "created", true);
			}
			applied++;
		}
		cJSON_AddItemToArray(results, r);
	}
	cJSON_AddNumberToObject(resp, "applied", applied);
	cJSON_AddItemToObject(resp, "results", results);

	cJSON *written = cJSON_CreateArray();
	if (ok && b.persist)
		for (size_t i = 0; i < b.n_staged; i++)
			cJSON_AddItemToArray(
				written, cJSON_CreateString(config_files[b.staged[i].file]));
	cJSON_AddItemToObject(resp, "written", written);

	config_write_free(&b);
	if (ok)
		ipc_notify_config("set");
	return resp;
}
