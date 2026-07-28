#ifndef ASTEROIDZ_BAR_CUSTOM_H
#define ASTEROIDZ_BAR_CUSTOM_H

/* Out-of-process bar plugins.
 *
 * A plugin is a command, never a shared object. Loading one into this process
 * would put third-party code on the compositor's event loop with the
 * compositor's lifetime: one blocking read is a frozen screen, one bad free is
 * a lost session, and `bar.h` has no ABI to offer it in any case. The same
 * reasoning bar-discord.h gives for keeping songbird in a daemon applies with
 * more force here, because that was a bar process and this is the compositor.
 *
 * So the plugin describes state and the compositor draws it. It writes one
 * JSON object per update on stdout:
 *
 *   {"text":"12","icon":"mail.svg","tint":"urgent","class":"flat",
 *    "tooltip":"12 unread","hidden":false}
 *
 * Anything that is not JSON is taken as the text itself, so a two-line shell
 * script is a valid plugin. Every field is optional; a plugin that prints
 * nothing renders nothing rather than an error pill, because a broken plugin
 * should be quiet, not permanently in the way.
 *
 * Two ways to run, both already built for something else:
 *
 *   interval N    async_spawn on the shared metrics tick, exactly as
 *                 bar_vpn_poll drives the nordvpn CLI
 *   continuous    async_spawn_lines, a long-lived child emitting a line per
 *                 update, exactly as the media visualiser reads cava
 *
 * What a plugin deliberately does NOT get: a drawing surface, a frame
 * callback, or a widget tree. A pill is one scene node and hit testing is per
 * node -- the same argument bar-popover.h makes against painting -/+ zones on
 * a row -- so sub-widgets a plugin drew itself would be decoration that lies
 * about where you may click. Artwork that has to be generated is generated
 * here (bar_net_icon, bar_viz_icon) where it can be cached on quantised state.
 */

/* Everything the compositor knows about one plugin. Indexed in lockstep with
 * config.bar_custom, so a module holds an index and never a pointer into a
 * table the next reload rebuilds. */
typedef struct {
	char text[BAR_TEXT_MAX];
	/* "icon" takes a string OR an array of them, because one pill genuinely
	 * can show more than one: the built-in discord module draws the logo with
	 * the mic-mute glyph beside it while muted, and a plugin reproducing that
	 * module had no way to say so. Capped at what the pill widget draws. */
	char icons[ASTEROIDZ_TAB_MAX_ICONS][192];
	int32_t nicons;
	char tooltip[BAR_TEXT_MAX];
	float tint[4];
	bool have_tint;
	enum bar_pill_look look;
	bool hidden;
	/* Has it ever answered? Until it has, the pill renders nothing at all --
	 * a plugin polled every 60s should not show a placeholder for the first
	 * minute of the session, and one that is broken should show it forever. */
	bool have;
	bool in_flight;			/* an interval run is out; do not start another */
	int32_t due;			/* seconds until the next interval run */
	/* The running child, in EITHER mode: a `continuous` stream for its whole
	 * life, or a single interval run for the moment it takes. Held for both so
	 * bar_custom_finish can stop whatever is out. */
	AsyncSpawn *proc;
} BarCustomState;

static BarCustomState bar_custom_state[MAX_BAR_CUSTOM];

/* Which plugin "custom/<name>" refers to, or -1. */
static int32_t bar_custom_index(const char *name) {
	if (!name || !*name)
		return -1;
	for (int32_t i = 0; i < config.bar_custom_count; i++)
		if (strcmp(config.bar_custom[i].name, name) == 0)
			return i;
	return -1;
}

/* Map a `class` to the pill looks the rest of the bar already uses. Anything
 * unrecognised is flat: a plugin naming a look this build does not have should
 * render plainly, not vanish. */
static enum bar_pill_look bar_custom_look(const char *cls) {
	if (!cls || !*cls)
		return BAR_LOOK_FLAT;
	if (!strcmp(cls, "urgent"))
		return BAR_LOOK_URGENT;
	if (!strcmp(cls, "active") || !strcmp(cls, "focused"))
		return BAR_LOOK_ACTIVE;
	if (!strcmp(cls, "occupied"))
		return BAR_LOOK_OCCUPIED;
	if (!strcmp(cls, "empty"))
		return BAR_LOOK_EMPTY;
	if (!strcmp(cls, "sunken"))
		return BAR_LOOK_SUNKEN;
	return BAR_LOOK_FLAT;
}

/* Icon tint, as a THEME TOKEN by preference.
 *
 * A plugin that hardcodes #89b4fa is wrong the moment the theme changes, and
 * the artwork most plugins will reach for is the monochrome SVG set the
 * sysinfo pills use -- solid #000 silhouettes that paint as an invisible blob
 * on a dark panel unless they are tinted (bar_module_refresh_metric says the
 * same thing about the same files). Hex is accepted for the cases a token
 * cannot express, but tokens are what the documentation shows. */
static bool bar_custom_tint(const char *name, float out[4]) {
	if (!name || !*name)
		return false;
	if (!strcmp(name, "accent") || !strcmp(name, "focus")) {
		memcpy(out, config.theme.focus_bg_color, sizeof(float) * 4);
		return true;
	}
	if (!strcmp(name, "urgent")) {
		memcpy(out, config.theme.urgent_color, sizeof(float) * 4);
		return true;
	}
	if (!strcmp(name, "fg") || !strcmp(name, "foreground")) {
		memcpy(out, config.theme.fg_color, sizeof(float) * 4);
		return true;
	}
	if (!strcmp(name, "dim")) {
		memcpy(out, config.theme.fg_color, sizeof(float) * 4);
		out[3] *= 0.45f; /* an unlit indicator, as the vpn pill draws one */
		return true;
	}
	int64_t hex = parse_color((char *)name);
	if (hex == -1)
		return false;
	convert_hex_to_rgba(out, hex);
	return true;
}

/* Take one update. `out` is a whole JSON object, or plain text.
 *
 * Never fatal: a plugin that emits garbage keeps whatever it last showed,
 * because blanking a pill on one bad line would make a plugin with an
 * occasional hiccup flicker rather than simply be wrong for a moment. */
static void bar_custom_apply(int32_t idx, const char *out) {
	if (idx < 0 || idx >= config.bar_custom_count || !out)
		return;
	BarCustomState *st = &bar_custom_state[idx];

	/* trim: a shell one-liner's output ends in a newline */
	char buf[BAR_TEXT_MAX * 2];
	snprintf(buf, sizeof(buf), "%s", out);
	size_t n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	const char *p = buf;
	while (*p == ' ' || *p == '\t')
		p++;

	st->have = true;

	if (*p != '{') {
		/* plain text: the whole output is the label, everything else resets to
		 * its default so a plugin can switch between the two forms. The
		 * precision is the cap being stated rather than implied -- a plugin
		 * that floods gets its first BAR_TEXT_MAX bytes, not a realloc. */
		snprintf(st->text, sizeof(st->text), "%.*s",
				 (int32_t)sizeof(st->text) - 1, p);
		st->nicons = 0;
		st->tooltip[0] = '\0';
		st->have_tint = false;
		st->look = BAR_LOOK_FLAT;
		st->hidden = false;
		return;
	}

	cJSON *root = cJSON_Parse(p);
	if (!root) {
		wlr_log(WLR_DEBUG, "bar: custom '%s' emitted unparseable JSON",
				config.bar_custom[idx].name);
		return;
	}
	cJSON *v;
	if ((v = cJSON_GetObjectItem(root, "text")) && cJSON_IsString(v))
		snprintf(st->text, sizeof(st->text), "%s", v->valuestring);
	else
		st->text[0] = '\0';
	st->nicons = 0;
	v = cJSON_GetObjectItem(root, "icon");
	if (cJSON_IsString(v)) {
		snprintf(st->icons[0], sizeof(st->icons[0]), "%s", v->valuestring);
		st->nicons = 1;
	} else if (cJSON_IsArray(v)) {
		cJSON *e = NULL;
		cJSON_ArrayForEach(e, v) {
			if (st->nicons >= ASTEROIDZ_TAB_MAX_ICONS)
				break;
			if (!cJSON_IsString(e) || !e->valuestring[0])
				continue; /* skip a gap rather than drawing one */
			snprintf(st->icons[st->nicons], sizeof(st->icons[0]), "%s",
					 e->valuestring);
			st->nicons++;
		}
	}
	if ((v = cJSON_GetObjectItem(root, "tooltip")) && cJSON_IsString(v))
		snprintf(st->tooltip, sizeof(st->tooltip), "%s", v->valuestring);
	else
		st->tooltip[0] = '\0';
	v = cJSON_GetObjectItem(root, "class");
	st->look = bar_custom_look(cJSON_IsString(v) ? v->valuestring : NULL);
	v = cJSON_GetObjectItem(root, "tint");
	st->have_tint =
		cJSON_IsString(v) ? bar_custom_tint(v->valuestring, st->tint) : false;
	v = cJSON_GetObjectItem(root, "hidden");
	st->hidden = cJSON_IsTrue(v);
	cJSON_Delete(root);
}

/* One interval run finished. */
static void bar_custom_on_output(const char *out, size_t len, void *user) {
	(void)len;
	int32_t idx = (int32_t)(intptr_t)user;
	if (idx < 0 || idx >= MAX_BAR_CUSTOM)
		return;
	bar_custom_state[idx].in_flight = false;
	bar_custom_apply(idx, out);
	bar_update_all();
}

/* One line from a `continuous` plugin. */
static void bar_custom_on_line(const char *line, void *user) {
	int32_t idx = (int32_t)(intptr_t)user;
	if (idx < 0 || idx >= MAX_BAR_CUSTOM)
		return;
	bar_custom_apply(idx, line);
	bar_update_all();
}

/* Run a plugin's command through the shell.
 *
 * `sh -c` rather than a hand-rolled argv split because these fields are shell:
 * someone writing `exec "curl -s x | jq -r .n"` means the pipe, and splitting
 * on spaces would hand jq's arguments to curl. It also makes `~` and $VARS
 * behave the way they do everywhere else in the config. */
static bool bar_custom_spawn(const char *cmd, int32_t idx, bool lines) {
	if (!cmd || !*cmd || idx < 0 || idx >= MAX_BAR_CUSTOM)
		return false;
	BarCustomState *st = &bar_custom_state[idx];
	char *const argv[] = {"/bin/sh", "-c", (char *)cmd, NULL};
	/* Both modes keep the handle, and both hand it `owner` so it is nulled
	 * whichever way the child ends. An interval run is short, but it is not
	 * instantaneous: a reload landing while one is out would otherwise leave a
	 * child whose completion callback still carries the OLD index, and it
	 * would report into whatever plugin now sits there. */
	AsyncSpawn *as = async_spawn_run(event_loop, argv,
									 lines ? NULL : bar_custom_on_output,
									 lines ? bar_custom_on_line : NULL,
									 (void *)(intptr_t)idx);
	if (!as)
		return false;
	as->owner = &st->proc;
	st->proc = as;
	return true;
}

/* Is this plugin on any bar right now? A `custom` block that no module list
 * mentions is configuration, not a running process: it must not be spawned,
 * exactly as the weather timer is only armed when a weather module exists. */
static bool bar_custom_in_use(int32_t idx) {
	Monitor *m = NULL;
	if (!config.bar_enable)
		return false;
	wl_list_for_each(m, &mons, link) {
		if (!m->bar)
			continue;
		for (int32_t i = 0; i < m->bar->nmodules; i++)
			if (m->bar->modules[i].kind == BAR_MODULE_CUSTOM &&
				m->bar->modules[i].custom == idx)
				return true;
	}
	return false;
}

/* Start every `continuous` plugin that is on a bar, and stop the ones that are
 * not. Called from bar_clock_sync, which is where every other conditional
 * timer and child is armed. */
static void bar_custom_sync(void) {
	for (int32_t i = 0; i < config.bar_custom_count; i++) {
		ConfigBarCustom *cm = &config.bar_custom[i];
		BarCustomState *st = &bar_custom_state[i];
		/* Only streaming plugins are managed here. An interval plugin also
		 * parks its child in `proc`, and it is out for a fraction of a second
		 * -- treating it as unwanted because it is not `continuous` would kill
		 * every run that happened to overlap an arrange. */
		if (!cm->continuous)
			continue;
		bool want = bar_custom_in_use(i) && cm->exec[0];
		if (want && !st->proc) {
			bar_custom_spawn(cm->exec, i, true);
		} else if (!want && st->proc) {
			async_spawn_stop(st->proc);
			st->proc = NULL;
		}
	}
}

/* Stop every plugin child and forget everything they said.
 *
 * A reload rebuilds config.bar_custom underneath these indices, so the
 * children have to go before it does -- and so does the STATE, which is the
 * less obvious half. Index 0 after a reload is very likely a different plugin
 * than index 0 before it, and carrying `have` across meant a run-once plugin
 * was judged to have already run when it had never been started, leaving the
 * previous config's text on screen forever. Whatever a plugin last said is a
 * fact about the config that named it, and does not survive that config. */
static void bar_custom_finish(void) {
	for (int32_t i = 0; i < MAX_BAR_CUSTOM; i++) {
		if (bar_custom_state[i].proc) {
			async_spawn_stop(bar_custom_state[i].proc);
			bar_custom_state[i].proc = NULL;
		}
	}
	memset(bar_custom_state, 0, sizeof(bar_custom_state));
}

/* Due-time poll, driven by the shared metrics tick (one second resolution).
 *
 * Counting down per plugin rather than giving each its own wl_event_source:
 * these all want waking at second granularity to run a short command, and N
 * timers to do what one already does would be N wakeups on an otherwise idle
 * compositor. `interval 0` means run once and never again -- a plugin whose
 * answer cannot change. */
static void bar_custom_tick(int32_t elapsed_sec) {
	for (int32_t i = 0; i < config.bar_custom_count; i++) {
		ConfigBarCustom *cm = &config.bar_custom[i];
		BarCustomState *st = &bar_custom_state[i];
		if (cm->continuous || !cm->exec[0] || !bar_custom_in_use(i))
			continue;
		if (st->in_flight)
			continue;
		if (cm->interval <= 0) {
			if (st->have)
				continue; /* run-once, and it has run */
		} else {
			st->due -= elapsed_sec;
			if (st->have && st->due > 0)
				continue;
			st->due = cm->interval;
		}
		st->in_flight = bar_custom_spawn(cm->exec, i, false);
	}
}

/* A click on a plugin's pill. Fire-and-forget through the shell, like every
 * other command in the config: the plugin's next update reports the outcome,
 * and waiting for the child here would block the pointer. */
static bool bar_custom_click(int32_t idx, uint32_t button) {
	if (idx < 0 || idx >= config.bar_custom_count)
		return false;
	ConfigBarCustom *cm = &config.bar_custom[idx];
	const char *cmd = button == BTN_RIGHT ? cm->on_click_right : cm->on_click;
	if (button != BTN_LEFT && button != BTN_RIGHT)
		return false;
	if (!cmd || !*cmd)
		return false;
	char *const argv[] = {"/bin/sh", "-c", (char *)cmd, NULL};
	async_spawn(event_loop, argv, NULL, NULL);
	return true;
}

#endif /* ASTEROIDZ_BAR_CUSTOM_H */
