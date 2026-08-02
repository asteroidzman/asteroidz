#ifndef ASTEROIDZ_IPC_RULES_H
#define ASTEROIDZ_IPC_RULES_H

/* `get window-rule-schema`, `get window-rules`, `get binds`.
 *
 * The rule and bind half of what ipc-config.h does for options: a description of
 * the vocabulary, and the current contents expressed in it. Same division of
 * labour -- the compositor owns the schema and the file, a UI owns the pixels
 * and knows nothing about KDL.
 *
 * The rules are served from the schema by offset, exactly as `get config` is; the
 * binds are served from the SOURCE RECORDS rather than from the parsed
 * KeyBinding array, because that parse is lossy. bind-source.h says why at
 * length. The short version is that `focus_stack next` has become a function
 * pointer and `arg.i = 1` by the time it reaches KeyBinding, and no amount of
 * care reconstructs the name from that.
 */

/* ---------- the vocabulary ---------- */

static cJSON *build_rule_schema_response(void) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddNumberToObject(resp, "schema_version", 1);

	cJSON *groups = cJSON_CreateArray();
	for (size_t g = 0; g < LENGTH(rule_groups); g++) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", rule_groups[g].name);
		cJSON_AddStringToObject(o, "label", rule_groups[g].label);
		cJSON_AddStringToObject(o, "desc", rule_groups[g].desc);
		cJSON_AddItemToArray(groups, o);
	}
	cJSON_AddItemToObject(resp, "groups", groups);

	cJSON *fields = cJSON_CreateArray();
	for (size_t i = 0; i < RULE_SCHEMA_COUNT; i++) {
		const RuleField *f = &rule_schema[i];
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "key", f->key);
		/* Both spellings. `nice` is what a writer should emit; `key` is what the
		 * legacy comma form uses and what everything else here is keyed by. A UI
		 * that only had one of them would either write configs in the old
		 * spelling or fail to look up its own values. */
		cJSON_AddStringToObject(e, "nice", f->nice);
		cJSON_AddStringToObject(e, "group", f->group);
		cJSON_AddStringToObject(e, "label", f->label);
		cJSON_AddStringToObject(e, "desc", f->desc);
		cJSON_AddStringToObject(e, "type", rule_type_name(f->type));
		if (!isnan(f->min))
			cJSON_AddNumberToObject(e, "min", f->min);
		if (!isnan(f->max))
			cJSON_AddNumberToObject(e, "max", f->max);
		if (f->type == RULE_ENUM) {
			cJSON *arr = cJSON_CreateArray();
			for (size_t m = 0; m < f->n_members; m++) {
				cJSON *mo = cJSON_CreateObject();
				cJSON_AddStringToObject(mo, "name", f->members[m].name);
				if (f->members[m].desc)
					cJSON_AddStringToObject(mo, "desc", f->members[m].desc);
				cJSON_AddItemToArray(arr, mo);
			}
			cJSON_AddItemToObject(e, "enum", arr);
		}
		/* The one fact a UI cannot infer and gets wrong by default: these are
		 * regexes. A plain text field here produces rules that never match,
		 * because a `.` in an app id is a wildcard and the user meant a dot. */
		if (f->type == RULE_MATCH)
			cJSON_AddBoolToObject(e, "regex", true);
		/* And the other one: three states, not two. A checkbox drawn against a
		 * tri-state turns every unset field into an explicit 0 on save. */
		if (f->type == RULE_TRISTATE)
			cJSON_AddBoolToObject(e, "tristate", true);
		cJSON_AddItemToArray(fields, e);
	}
	cJSON_AddItemToObject(resp, "fields", fields);
	return resp;
}

/* ---------- the current rules ---------- */

static cJSON *build_rule_source_json(int32_t index) {
	cJSON *src = cJSON_CreateObject();
	const RuleOrigin *o = rule_source_at(index);
	if (!o || o->file < 0 || o->file >= nconfig_files) {
		/* No node behind it: a legacy `windowrule=` line, or a rule added at
		 * runtime. Listed, because a rule editor that hides rules it cannot
		 * write is worse than one that shows them read-only -- you would edit
		 * around a rule you could not see. */
		cJSON_AddStringToObject(src, "kind", o ? "legacy" : "unknown");
		cJSON_AddBoolToObject(src, "editable", false);
		return src;
	}
	const char *file = config_files[o->file];
	char why[64] = "";
	bool foreign = config_file_is_foreign(file, why, sizeof(why));
	cJSON_AddStringToObject(src, "kind", "file");
	cJSON_AddStringToObject(src, "file", file);
	cJSON_AddNumberToObject(src, "line", o->line);
	cJSON_AddBoolToObject(src, "writable", !foreign);
	if (foreign)
		cJSON_AddStringToObject(src, "reason", why);
	/* Editable AND writable are different questions. A rule in a generated file
	 * has a perfectly good span and rewriting it would work -- and be undone the
	 * next time the generator runs. */
	cJSON_AddBoolToObject(src, "editable", o->editable && !foreign);
	return src;
}

static cJSON *build_window_rules_response(void) {
	cJSON *resp = cJSON_CreateObject();
	cJSON *arr = cJSON_CreateArray();
	for (int32_t i = 0; i < config.window_rules_count; i++) {
		const ConfigWinRule *r = &config.window_rules[i];
		cJSON *e = cJSON_CreateObject();
		cJSON_AddNumberToObject(e, "index", i);

		cJSON *fields = cJSON_CreateObject();
		char buf[512];
		for (size_t f = 0; f < RULE_SCHEMA_COUNT; f++) {
			/* Only the fields this rule actually SETS. Emitting all fifty-three
			 * with their unset markers would be a rule editor that cannot tell a
			 * rule saying nothing about blur from one saying "blur on" -- and
			 * would write both back identically. rule_format returns false for
			 * unset, which is the whole reason it returns a bool. */
			if (rule_format(r, &rule_schema[f], buf, sizeof(buf)))
				cJSON_AddStringToObject(fields, rule_schema[f].key, buf);
		}
		cJSON_AddItemToObject(e, "fields", fields);
		cJSON_AddItemToObject(e, "source", build_rule_source_json(i));
		cJSON_AddItemToArray(arr, e);
	}
	cJSON_AddItemToObject(resp, "rules", arr);
	cJSON_AddNumberToObject(resp, "count", config.window_rules_count);
	return resp;
}

/* ---------- the current binds ---------- */

static cJSON *build_binds_response(void) {
	cJSON *resp = cJSON_CreateObject();
	cJSON *arr = cJSON_CreateArray();
	for (int32_t i = 0; i < nbind_sources; i++) {
		const BindSource *b = &bind_sources[i];
		cJSON *e = cJSON_CreateObject();
		cJSON_AddNumberToObject(e, "index", i);
		cJSON_AddStringToObject(e, "kind", b->kind);
		cJSON_AddStringToObject(e, "chord", b->chord);
		cJSON_AddStringToObject(e, "mods", b->mods);
		cJSON_AddStringToObject(e, "key", b->key);
		cJSON_AddStringToObject(e, "action", b->action);
		cJSON *args = cJSON_CreateArray();
		for (int32_t a = 0; a < b->n_args; a++)
			cJSON_AddItemToArray(args, cJSON_CreateString(b->args[a]));
		cJSON_AddItemToObject(e, "args", args);
		cJSON_AddStringToObject(e, "mode", b->mode);

		/* The four flags parse_bind_flags reads. Reported even when false, so a
		 * UI can draw four checkboxes without having to know the set of names --
		 * unlike a rule field, where absent means "says nothing". */
		cJSON *flags = cJSON_CreateObject();
		cJSON_AddBoolToObject(flags, "keysym", b->keysym);
		cJSON_AddBoolToObject(flags, "lock", b->lock);
		cJSON_AddBoolToObject(flags, "release", b->release);
		cJSON_AddBoolToObject(flags, "pass", b->pass);
		cJSON_AddItemToObject(e, "flags", flags);

		cJSON *src = cJSON_CreateObject();
		if (b->file >= 0 && b->file < nconfig_files) {
			const char *file = config_files[b->file];
			char why[64] = "";
			bool foreign = config_file_is_foreign(file, why, sizeof(why));
			cJSON_AddStringToObject(src, "kind", "file");
			cJSON_AddStringToObject(src, "file", file);
			cJSON_AddNumberToObject(src, "line", b->line);
			cJSON_AddBoolToObject(src, "writable", !foreign);
			if (foreign)
				cJSON_AddStringToObject(src, "reason", why);
			cJSON_AddBoolToObject(src, "editable", b->editable && !foreign);
		} else {
			cJSON_AddStringToObject(src, "kind", "legacy");
			cJSON_AddBoolToObject(src, "editable", false);
		}
		cJSON_AddItemToObject(e, "source", src);
		cJSON_AddItemToArray(arr, e);
	}
	cJSON_AddItemToObject(resp, "binds", arr);
	cJSON_AddNumberToObject(resp, "count", nbind_sources);
	/* What is NOT in the list, said out loud.
	 *
	 * axisbind, switchbind and gesturebind have no KDL block handler -- they are
	 * raw comma-string leaves -- so nothing records a source for them and they
	 * cannot appear here. A UI that showed a bind list with no note would be
	 * quietly claiming those do not exist, and a user who then "cleaned up" their
	 * binds through it would lose them. */
	cJSON *unsupported = cJSON_CreateArray();
	cJSON_AddItemToArray(unsupported, cJSON_CreateString("axisbind"));
	cJSON_AddItemToArray(unsupported, cJSON_CreateString("switchbind"));
	cJSON_AddItemToArray(unsupported, cJSON_CreateString("gesturebind"));
	cJSON_AddItemToObject(resp, "not_listed", unsupported);
	return resp;
}

/* ---------- the tag-rule schema, and the current tag rules ---------- */

static cJSON *build_tag_rule_schema_response(void) {
	cJSON *resp = cJSON_CreateObject();

	cJSON *groups = cJSON_CreateArray();
	for (size_t g = 0; g < sizeof(tag_groups) / sizeof(tag_groups[0]); g++) {
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", tag_groups[g].name);
		cJSON_AddStringToObject(e, "label", tag_groups[g].label);
		cJSON_AddStringToObject(e, "desc", tag_groups[g].desc);
		cJSON_AddItemToArray(groups, e);
	}
	cJSON_AddItemToObject(resp, "groups", groups);

	cJSON *fields = cJSON_CreateArray();
	for (size_t i = 0; i < TAG_SCHEMA_COUNT; i++) {
		const TagField *f = &tag_schema[i];
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "key", f->key);
		cJSON_AddStringToObject(e, "nice", f->nice);
		cJSON_AddStringToObject(e, "group", f->group);
		cJSON_AddStringToObject(e, "label", f->label);
		cJSON_AddStringToObject(e, "desc", f->desc);
		cJSON_AddStringToObject(e, "type", rule_type_name(f->type));
		if (!isnan(f->min))
			cJSON_AddNumberToObject(e, "min", f->min);
		if (!isnan(f->max))
			cJSON_AddNumberToObject(e, "max", f->max);
		if (f->type == RULE_ENUM) {
			cJSON *arr = cJSON_CreateArray();
			for (size_t m = 0; m < f->n_members; m++) {
				cJSON *mo = cJSON_CreateObject();
				cJSON_AddStringToObject(mo, "name", f->members[m].name);
				if (f->members[m].desc)
					cJSON_AddStringToObject(mo, "desc", f->members[m].desc);
				cJSON_AddItemToArray(arr, mo);
			}
			cJSON_AddItemToObject(e, "enum", arr);
		}
		if (f->type == RULE_TRISTATE)
			cJSON_AddBoolToObject(e, "tristate", true);
		cJSON_AddItemToArray(fields, e);
	}
	cJSON_AddItemToObject(resp, "fields", fields);
	return resp;
}

static cJSON *build_tag_rules_response(void) {
	cJSON *resp = cJSON_CreateObject();
	cJSON *arr = cJSON_CreateArray();
	for (int32_t i = 0; i < config.tag_rules_count; i++) {
		const ConfigTagRule *r = &config.tag_rules[i];
		cJSON *e = cJSON_CreateObject();
		cJSON_AddNumberToObject(e, "index", i);

		cJSON *fields = cJSON_CreateObject();
		char buf[512];
		for (size_t f = 0; f < TAG_SCHEMA_COUNT; f++) {
			/* Only what this rule SETS -- see tag_format, which returns false
			 * for a field the rule is silent about. Emitting every field with
			 * its unset marker would make a rule that says nothing about
			 * nmaster indistinguishable from one that pins it, and an editor
			 * would write both back the same way. */
			if (tag_format(r, &tag_schema[f], buf, sizeof(buf)))
				cJSON_AddStringToObject(fields, tag_schema[f].key, buf);
		}
		cJSON_AddItemToObject(e, "fields", fields);

		cJSON *src = cJSON_CreateObject();
		const RuleOrigin *o = tag_source_at(i);
		if (o && o->file >= 0 && o->file < nconfig_files) {
			const char *file = config_files[o->file];
			char why[64] = "";
			bool foreign = config_file_is_foreign(file, why, sizeof(why));
			cJSON_AddStringToObject(src, "kind", "file");
			cJSON_AddStringToObject(src, "file", file);
			cJSON_AddNumberToObject(src, "line", o->line);
			cJSON_AddBoolToObject(src, "writable", !foreign);
			if (foreign)
				cJSON_AddStringToObject(src, "reason", why);
			cJSON_AddBoolToObject(src, "editable", o->editable && !foreign);
		} else {
			cJSON_AddStringToObject(src, "kind", "legacy");
			cJSON_AddBoolToObject(src, "editable", false);
		}
		cJSON_AddItemToObject(e, "source", src);
		cJSON_AddItemToArray(arr, e);
	}
	cJSON_AddItemToObject(resp, "rules", arr);
	cJSON_AddNumberToObject(resp, "count", config.tag_rules_count);
	return resp;
}

/* ---------- set-window-rules, set-binds ---------- */

/* Verbs, not dispatches, for the reasons set-config is one: handle_command
 * rewrites every comma to a space in its working copy and the dispatch branch
 * tokenises on commas with a six-token cap, while a rule legitimately contains
 * commas and a batch of them exceeds the buffer. And a dispatch's reply is a
 * hardcoded {"success":true} with the return value discarded, when real errors
 * are the entire point. */

static bool ipc_rw_parse_common(cJSON *jc, RuleWriteChange *c) {
	const cJSON *op = cJSON_GetObjectItem(jc, "op");
	const char *ops = cJSON_IsString(op) ? op->valuestring : "update";
	if (!strcmp(ops, "add"))
		c->op = RW_ADD;
	else if (!strcmp(ops, "remove"))
		c->op = RW_REMOVE;
	else if (!strcmp(ops, "update"))
		c->op = RW_UPDATE;
	else {
		rw_fail(c, "bad-request", "unknown op \"%s\"", ops);
		return false;
	}
	const cJSON *idx = cJSON_GetObjectItem(jc, "index");
	c->index = cJSON_IsNumber(idx) ? (int32_t)idx->valuedouble : -1;
	if (c->op != RW_ADD && c->index < 0) {
		rw_fail(c, "bad-request", "%s needs an index", ops);
		return false;
	}
	return true;
}

/* A value out of JSON, in the one representation that crosses this socket: the
 * string a user would write. Numbers and booleans are accepted because a UI
 * naturally produces them, and normalised here rather than in four places. */
static void ipc_rw_value(const cJSON *v, char *out, size_t cap) {
	if (cJSON_IsString(v))
		snprintf(out, cap, "%s", v->valuestring);
	else if (cJSON_IsBool(v))
		snprintf(out, cap, "%d", cJSON_IsTrue(v) ? 1 : 0);
	else if (cJSON_IsNumber(v)) {
		double d = v->valuedouble;
		if (d == (double)(long long)d)
			snprintf(out, cap, "%lld", (long long)d);
		else
			snprintf(out, cap, "%g", d);
	} else {
		out[0] = '\0';
	}
}

static cJSON *ipc_rw_reply(RuleWriteBatch *b, bool ok) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddBoolToObject(resp, "ok", ok);
	if (b->error[0]) {
		cJSON_AddStringToObject(resp, "error", b->error);
		cJSON_AddStringToObject(resp, "detail", b->detail);
	}
	cJSON *results = cJSON_CreateArray();
	int applied = 0;
	for (size_t i = 0; i < b->n; i++) {
		RuleWriteChange *c = &b->changes[i];
		cJSON *r = cJSON_CreateObject();
		cJSON_AddNumberToObject(r, "index", c->index);
		if (c->error[0]) {
			cJSON_AddBoolToObject(r, "ok", false);
			cJSON_AddStringToObject(r, "error", c->error);
			cJSON_AddStringToObject(r, "detail", c->detail);
		} else if (!ok) {
			/* It did not fail -- it never ran. Saying "ok" here would be a lie
			 * a UI would act on, and the batch is all-or-nothing. */
			cJSON_AddBoolToObject(r, "ok", false);
			cJSON_AddStringToObject(r, "error", "not-applied");
			cJSON_AddStringToObject(r, "detail",
									"another change in the batch was refused");
		} else {
			cJSON_AddBoolToObject(r, "ok", true);
			if (c->file >= 0 && c->file < nconfig_files)
				cJSON_AddStringToObject(r, "file", config_files[c->file]);
			applied++;
		}
		cJSON_AddItemToArray(results, r);
	}
	cJSON_AddNumberToObject(resp, "applied", applied);
	cJSON_AddItemToObject(resp, "results", results);
	return resp;
}

static cJSON *handle_set_window_rules(const char *body) {
	RuleWriteBatch b;
	memset(&b, 0, sizeof(b));

	cJSON *req = cJSON_Parse(body);
	if (!req) {
		cJSON *resp = cJSON_CreateObject();
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "bad-request");
		cJSON_AddStringToObject(resp, "detail", "body is not valid JSON");
		return resp;
	}
	cJSON *changes = cJSON_GetObjectItem(req, "changes");
	if (!cJSON_IsArray(changes) || cJSON_GetArraySize(changes) == 0) {
		cJSON_Delete(req);
		cJSON *resp = cJSON_CreateObject();
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "bad-request");
		cJSON_AddStringToObject(resp, "detail", "no changes");
		return resp;
	}

	bool ok = true;
	cJSON *jc = NULL;
	cJSON_ArrayForEach(jc, changes) {
		if (b.n >= RULE_WRITE_MAX_CHANGES) {
			snprintf(b.error, sizeof(b.error), "too-many");
			snprintf(b.detail, sizeof(b.detail), "at most %d changes per batch",
					 RULE_WRITE_MAX_CHANGES);
			ok = false;
			break;
		}
		RuleWriteChange *c = &b.changes[b.n++];
		if (!ipc_rw_parse_common(jc, c)) {
			ok = false;
			continue;
		}
		cJSON *fields = cJSON_GetObjectItem(jc, "fields");
		if (c->op != RW_REMOVE && !cJSON_IsObject(fields)) {
			rw_fail(c, "bad-request", "a rule needs a fields object");
			ok = false;
			continue;
		}
		cJSON *f = NULL;
		cJSON_ArrayForEach(f, fields) {
			if (c->n_fields >= RULE_WRITE_MAX_FIELDS)
				break;
			const RuleField *rf = rule_field_by_key(f->string);
			if (!rf) {
				const char *k = rule_field_key_for_nice(f->string);
				rf = rule_field_by_key(k);
			}
			if (!rf) {
				rw_fail(c, "unknown-field", "no window-rule field \"%s\"",
						f->string);
				ok = false;
				break;
			}
			snprintf(c->keys[c->n_fields], sizeof(c->keys[0]), "%s", rf->key);
			ipc_rw_value(f, c->vals[c->n_fields], sizeof(c->vals[0]));
			c->n_fields++;
		}
	}
	cJSON_Delete(req);

	if (ok)
		ok = rw_plan_rules(&b);
	if (ok)
		ok = rw_commit(&b, true);
	cJSON *resp = ipc_rw_reply(&b, ok);
	if (ok)
		config_reload_quiet();
	return resp;
}

static cJSON *handle_set_binds(const char *body) {
	RuleWriteBatch b;
	memset(&b, 0, sizeof(b));

	cJSON *req = cJSON_Parse(body);
	if (!req) {
		cJSON *resp = cJSON_CreateObject();
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "bad-request");
		cJSON_AddStringToObject(resp, "detail", "body is not valid JSON");
		return resp;
	}
	cJSON *changes = cJSON_GetObjectItem(req, "changes");
	if (!cJSON_IsArray(changes) || cJSON_GetArraySize(changes) == 0) {
		cJSON_Delete(req);
		cJSON *resp = cJSON_CreateObject();
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "bad-request");
		cJSON_AddStringToObject(resp, "detail", "no changes");
		return resp;
	}

	bool ok = true;
	cJSON *jc = NULL;
	cJSON_ArrayForEach(jc, changes) {
		if (b.n >= RULE_WRITE_MAX_CHANGES) {
			snprintf(b.error, sizeof(b.error), "too-many");
			snprintf(b.detail, sizeof(b.detail), "at most %d changes per batch",
					 RULE_WRITE_MAX_CHANGES);
			ok = false;
			break;
		}
		RuleWriteChange *c = &b.changes[b.n++];
		if (!ipc_rw_parse_common(jc, c)) {
			ok = false;
			continue;
		}
		if (c->op == RW_REMOVE)
			continue;

		const cJSON *v = cJSON_GetObjectItem(jc, "kind");
		if (cJSON_IsString(v))
			snprintf(c->kind, sizeof(c->kind), "%s", v->valuestring);
		else if (c->op == RW_ADD)
			snprintf(c->kind, sizeof(c->kind), "bind");
		v = cJSON_GetObjectItem(jc, "chord");
		if (cJSON_IsString(v))
			snprintf(c->chord, sizeof(c->chord), "%s", v->valuestring);
		v = cJSON_GetObjectItem(jc, "action");
		if (cJSON_IsString(v))
			snprintf(c->action, sizeof(c->action), "%s", v->valuestring);
		if (!c->chord[0] || !c->action[0]) {
			rw_fail(c, "bad-request", "a bind needs a chord and an action");
			ok = false;
			continue;
		}
		/* The action has to exist. A keybind editor offering a name the
		 * compositor does not know writes a config that fails to reload -- and
		 * the failure surfaces at the next login, not at the save. */
		if (!ipc_dispatch_action_known(c->action)) {
			rw_fail(c, "unknown-action", "no dispatch called \"%s\"",
					c->action);
			ok = false;
			continue;
		}
		cJSON *args = cJSON_GetObjectItem(jc, "args");
		if (cJSON_IsArray(args)) {
			cJSON *a = NULL;
			cJSON_ArrayForEach(a, args) {
				if (c->n_args >= BIND_SOURCE_ARGS)
					break;
				ipc_rw_value(a, c->args[c->n_args], sizeof(c->args[0]));
				c->n_args++;
			}
		}
		cJSON *flags = cJSON_GetObjectItem(jc, "flags");
		if (cJSON_IsObject(flags)) {
			c->flag_keysym =
				cJSON_IsTrue(cJSON_GetObjectItem(flags, "keysym"));
			c->flag_lock = cJSON_IsTrue(cJSON_GetObjectItem(flags, "lock"));
			c->flag_release =
				cJSON_IsTrue(cJSON_GetObjectItem(flags, "release"));
			c->flag_pass = cJSON_IsTrue(cJSON_GetObjectItem(flags, "pass"));
		}
	}
	cJSON_Delete(req);

	if (ok)
		ok = rw_plan_binds(&b);
	if (ok)
		ok = rw_commit(&b, false);
	cJSON *resp = ipc_rw_reply(&b, ok);
	if (ok)
		config_reload_quiet();
	return resp;
}

#endif /* ASTEROIDZ_IPC_RULES_H */
