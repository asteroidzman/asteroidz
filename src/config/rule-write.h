#ifndef ASTEROIDZ_RULE_WRITE_H
#define ASTEROIDZ_RULE_WRITE_H

/* Writing window rules and keybinds back to the config file.
 *
 * Nested KDL is rendered and the existing reader flattens it. The writer never
 * learns the legacy `windowrule=` comma form or the `bind=` line form: a
 * `window-rule { … }` block goes into the file, and on the next read the
 * existing kdl_window_rule() -> windowrule= -> parse_option chain consumes it.
 * One parsing path, and the thing that was written is the thing that is read.
 *
 * Edits are SPAN REPLACEMENTS, the same as the option writer:
 *
 *   update   replace the node's bytes with a freshly rendered block
 *   remove   delete the span, taking the comment lines directly above it
 *   add      insert after the last rule of its kind, or append
 *
 * The comments come with a removal because they are its explanation, and
 * orphaning "// keep mpv on top" above an unrelated rule is worse than losing
 * it. That is the same judgement kdl_edit_remove already makes.
 *
 * Multiple edits to one file are applied in DESCENDING span order. Every splice
 * shifts every offset after it, so editing back to front means the offsets still
 * to be used are all before the ones already touched and none of them move. The
 * alternative -- re-parsing between edits and re-locating each node -- needs a
 * way to recognise the same node in a document that has changed, which is
 * exactly the problem the offsets were recorded to avoid.
 *
 * There is no preview mode, unlike options. A window rule takes effect when a
 * window maps and a keybind is a lookup, so there is nothing to see between
 * writing and applying; and the arrays behind them are rebuilt wholesale on
 * read, so "apply in memory" would mean reimplementing the reader. Writing then
 * re-reading is both simpler and exactly right.
 */

#include "../common/kdl-file.h"
#include "../common/kdl-write.h"

#define RULE_WRITE_MAX_CHANGES 64
#define RULE_WRITE_MAX_FIELDS 64

typedef enum { RW_ADD, RW_UPDATE, RW_REMOVE } RuleWriteOp;

typedef struct {
	RuleWriteOp op;
	int32_t index; /* into window_rules / bind_sources, for update and remove */

	/* For a rule: field key -> written value. For a bind: the pieces. */
	char keys[RULE_WRITE_MAX_FIELDS][64];
	char vals[RULE_WRITE_MAX_FIELDS][256];
	size_t n_fields;

	char kind[16]; /* binds: "bind" or "mousebind" */
	char chord[96];
	char action[64];
	char args[BIND_SOURCE_ARGS][96];
	int32_t n_args;
	bool flag_keysym, flag_lock, flag_release, flag_pass;

	/* Resolved during planning. */
	int32_t file;
	size_t span_start, span_end;

	char error[64];
	char detail[192];
} RuleWriteChange;

typedef struct {
	RuleWriteChange changes[RULE_WRITE_MAX_CHANGES];
	size_t n;
	bool persist;
	char error[64];
	char detail[192];
} RuleWriteBatch;

static void rw_fail(RuleWriteChange *c, const char *code, const char *fmt, ...) {
	snprintf(c->error, sizeof(c->error), "%s", code);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c->detail, sizeof(c->detail), fmt, ap);
	va_end(ap);
}

/* ---------- rendering ---------- */

/* A KDL string literal. Always quoted, never bare.
 *
 * A regex is exactly the kind of value that must not be written bare: `^/tmp`
 * starts with a slash, which is a comment to the KDL lexer, and `a b` would
 * become a node with an argument. The option writer learned this the hard way
 * with `spawn /tmp/x.sh`. */
static bool rw_quote(const char *v, char *out, size_t cap) {
	size_t o = 0;
	if (o + 1 >= cap)
		return false;
	out[o++] = '"';
	for (const char *p = v; *p; p++) {
		if (*p == '"' || *p == '\\') {
			if (o + 2 >= cap)
				return false;
			out[o++] = '\\';
			out[o++] = *p;
		} else {
			if (o + 1 >= cap)
				return false;
			out[o++] = *p;
		}
	}
	if (o + 2 >= cap)
		return false;
	out[o++] = '"';
	out[o] = '\0';
	return true;
}

/* One `window-rule { … }` block.
 *
 * Matchers go as properties on a `match` child and everything else is an action
 * node beside it, which is the shape kdl_window_rule reads. Fields are emitted
 * in SCHEMA order rather than in the order the request listed them, so a rule
 * edited twice does not shuffle its own lines and produce a diff that looks like
 * a rewrite.
 */
static bool rw_render_rule(const RuleWriteChange *c, const char *indent,
						   char *out, size_t cap) {
	char body[4096];
	size_t bo = 0;
	char match[1024];
	size_t mo = 0;

	for (size_t s = 0; s < RULE_SCHEMA_COUNT; s++) {
		const RuleField *f = &rule_schema[s];
		const char *val = NULL;
		for (size_t i = 0; i < c->n_fields; i++)
			if (!strcmp(c->keys[i], f->key) || !strcmp(c->keys[i], f->nice)) {
				val = c->vals[i];
				break;
			}
		if (!val || !*val)
			continue;

		char quoted[512];
		if (f->type == RULE_MATCH) {
			if (!rw_quote(val, quoted, sizeof(quoted)))
				return false;
			int32_t w = snprintf(match + mo, sizeof(match) - mo, " %s=%s",
								 f->nice, quoted);
			if (w < 0 || (size_t)w >= sizeof(match) - mo)
				return false;
			mo += (size_t)w;
			continue;
		}

		int32_t w;
		switch (f->type) {
		case RULE_TRISTATE:
			/* A bare node means 1 -- kdl_window_rule reads an argument-less
			 * child as "1" -- and that is the idiomatic spelling. For 0 the
			 * value has to be written, and `false` rather than `#false`:
			 * both parse today, only the first parses on an older build, and
			 * generated config should not require the newest compositor. */
			w = strcmp(val, "0")
					? snprintf(body + bo, sizeof(body) - bo, "%s\t%s\n", indent,
							   f->nice)
					: snprintf(body + bo, sizeof(body) - bo, "%s\t%s false\n",
							   indent, f->nice);
			break;
		case RULE_STRING:
		case RULE_ENUM:
		case RULE_BIND:
			if (!rw_quote(val, quoted, sizeof(quoted)))
				return false;
			w = snprintf(body + bo, sizeof(body) - bo, "%s\t%s %s\n", indent,
						 f->nice, quoted);
			break;
		default: /* numbers, and the tag number */
			w = snprintf(body + bo, sizeof(body) - bo, "%s\t%s %s\n", indent,
						 f->nice, val);
			break;
		}
		if (w < 0 || (size_t)w >= sizeof(body) - bo)
			return false;
		bo += (size_t)w;
	}

	if (mo == 0 && bo == 0)
		return false; /* a rule that says nothing */

	int32_t w = snprintf(out, cap, "window-rule {\n");
	if (w < 0 || (size_t)w >= cap)
		return false;
	size_t o = (size_t)w;
	if (mo > 0) {
		w = snprintf(out + o, cap - o, "%s\tmatch%s\n", indent, match);
		if (w < 0 || (size_t)w >= cap - o)
			return false;
		o += (size_t)w;
	}
	w = snprintf(out + o, cap - o, "%s%s}", body, indent);
	return w >= 0 && (size_t)w < cap - o;
}

/* One chord node inside a `binds` block. */
static bool rw_render_bind(const RuleWriteChange *c, const char *indent,
						   char *out, size_t cap) {
	if (!c->chord[0] || !c->action[0])
		return false;
	char props[128] = "";
	size_t po = 0;
	const struct {
		bool on;
		const char *name;
	} flags[] = {
		{c->flag_keysym, "keysym"},
		{c->flag_lock, "lock"},
		{c->flag_release, "release"},
		{c->flag_pass, "pass"},
	};
	for (size_t i = 0; i < LENGTH(flags); i++) {
		if (!flags[i].on)
			continue;
		/* `true`, not `#true`, for the same reason the tri-states are written
		 * bare: both parse now and only one parses on an older build. */
		int32_t w = snprintf(props + po, sizeof(props) - po, " %s=true",
							 flags[i].name);
		if (w < 0 || (size_t)w >= sizeof(props) - po)
			return false;
		po += (size_t)w;
	}

	char argstr[768] = "";
	size_t ao = 0;
	for (int32_t i = 0; i < c->n_args; i++) {
		char quoted[256];
		/* Quoted unless it is plainly a number. A spawn command is the common
		 * case and it has spaces in it; a bare `1` reads better than `"1"` and
		 * is what a hand-written config looks like. */
		bool numeric = c->args[i][0] &&
					   strspn(c->args[i], "-+.0123456789") ==
						   strlen(c->args[i]);
		if (numeric)
			snprintf(quoted, sizeof(quoted), "%s", c->args[i]);
		else if (!rw_quote(c->args[i], quoted, sizeof(quoted)))
			return false;
		int32_t w = snprintf(argstr + ao, sizeof(argstr) - ao, " %s", quoted);
		if (w < 0 || (size_t)w >= sizeof(argstr) - ao)
			return false;
		ao += (size_t)w;
	}

	int32_t w = snprintf(out, cap, "%s%s { %s%s; }", c->chord, props,
						 c->action, argstr);
	(void)indent;
	return w >= 0 && (size_t)w < cap;
}

/* ---------- planning ---------- */

/* Resolve every change to a file and a span, and refuse the whole batch if any
 * of them cannot be. All-or-nothing is what a Save button owes the file: half a
 * form applied is worse than none of it, because there is no way to tell which
 * half from looking at the result. */
static bool rw_plan_rules(RuleWriteBatch *b) {
	bool ok = true;
	for (size_t i = 0; i < b->n; i++) {
		RuleWriteChange *c = &b->changes[i];
		if (c->op == RW_ADD) {
			if (nconfig_files < 1) {
				rw_fail(c, "no-writable-file",
						"the config was not read from any file");
				ok = false;
				continue;
			}
			c->file = 0;
			continue;
		}
		const RuleOrigin *o = rule_source_at(c->index);
		if (!o) {
			rw_fail(c, "unknown-rule", "no rule at index %d", c->index);
			ok = false;
			continue;
		}
		if (!o->editable || o->file < 0 || o->file >= nconfig_files) {
			/* A legacy `windowrule=` line has no node and no span. Rewriting it
			 * would mean writing the comma form, which is the one thing this
			 * writer deliberately does not know how to do. */
			rw_fail(c, "not-editable",
					"this rule was not written as a `window-rule` block");
			ok = false;
			continue;
		}
		const char *file = config_files[o->file];
		char why[64] = "";
		if (config_file_is_foreign(file, why, sizeof(why))) {
			rw_fail(c, "read-only-source", "%s is generated (%s)", file, why);
			ok = false;
			continue;
		}
		c->file = o->file;
		c->span_start = o->span_start;
		c->span_end = o->span_end;
	}
	return ok;
}

static bool rw_plan_binds(RuleWriteBatch *b) {
	bool ok = true;
	for (size_t i = 0; i < b->n; i++) {
		RuleWriteChange *c = &b->changes[i];
		if (c->op == RW_ADD) {
			if (nconfig_files < 1) {
				rw_fail(c, "no-writable-file",
						"the config was not read from any file");
				ok = false;
				continue;
			}
			c->file = 0;
			continue;
		}
		if (c->index < 0 || c->index >= nbind_sources) {
			rw_fail(c, "unknown-bind", "no bind at index %d", c->index);
			ok = false;
			continue;
		}
		const BindSource *s = &bind_sources[c->index];
		if (!s->editable || s->file < 0 || s->file >= nconfig_files) {
			rw_fail(c, "not-editable",
					"this bind was not written in a `binds` block");
			ok = false;
			continue;
		}
		const char *file = config_files[s->file];
		char why[64] = "";
		if (config_file_is_foreign(file, why, sizeof(why))) {
			rw_fail(c, "read-only-source", "%s is generated (%s)", file, why);
			ok = false;
			continue;
		}
		c->file = s->file;
		c->span_start = s->span_start;
		c->span_end = s->span_end;
		/* An update keeps the kind it already had: turning a keyboard bind into
		 * a mousebind by editing it is not something a UI should be able to do
		 * by omitting a field. */
		if (!c->kind[0])
			snprintf(c->kind, sizeof(c->kind), "%s", s->kind);
	}
	return ok;
}

/* ---------- committing ---------- */

/* Descending by span, so that applying one edit does not move the next.
 * Insertion sort over at most RULE_WRITE_MAX_CHANGES entries. */
static void rw_sort_desc(RuleWriteChange **v, size_t n) {
	for (size_t i = 1; i < n; i++) {
		RuleWriteChange *k = v[i];
		size_t j = i;
		while (j > 0 && v[j - 1]->span_start < k->span_start) {
			v[j] = v[j - 1];
			j--;
		}
		v[j] = k;
	}
}

/* Where a new node goes: just after the last top-level node named `name`, so
 * rules stay with rules and binds go inside the binds block that already exists.
 *
 * Position is not cosmetic for rules. Every matching rule applies and later ones
 * override earlier ones, so a rule appended to the end of the file beats the
 * ones above it -- which is what a person adding a rule means. Appending after
 * the last EXISTING rule gets both: it wins, and it is where you would look for
 * it. */
static size_t rw_insert_point(const KdlDocument *doc, const char *name,
							  const char *text, bool *inside_block) {
	*inside_block = false;
	const KdlNode *last = NULL;
	for (size_t i = 0; i < doc->n_nodes; i++)
		if (!strcmp(doc->nodes[i].name, name))
			last = &doc->nodes[i];
	if (!last)
		return strlen(text);
	if (last->span.body_close != KDL_NO_OFFSET) {
		/* A block with children: go inside it, just before the closing brace. */
		*inside_block = true;
		return last->span.body_close;
	}
	return last->span.end;
}

static void rw_free_texts(char **texts, size_t n) {
	for (size_t i = 0; i < n; i++)
		free(texts[i]);
}

/* Apply every change to the staged file texts and write them out.
 *
 * `rules` selects which vocabulary is being written; the two differ only in what
 * is rendered and where a new node goes, so sharing the traversal keeps the
 * transactional part -- read, edit, re-parse, replace -- in one place instead of
 * two that drift. */
static bool rw_commit(RuleWriteBatch *b, bool rules) {
	char *texts[CONFIG_WRITE_MAX_FILES] = {0};
	bool touched[CONFIG_WRITE_MAX_FILES] = {false};

	for (size_t i = 0; i < b->n; i++) {
		int32_t f = b->changes[i].file;
		if (f < 0 || f >= (int32_t)CONFIG_WRITE_MAX_FILES)
			continue;
		if (!texts[f]) {
			texts[f] = kdl_file_slurp(config_files[f]);
			if (!texts[f]) {
				snprintf(b->error, sizeof(b->error), "write-failed");
				snprintf(b->detail, sizeof(b->detail), "cannot read %s: %s",
						 config_files[f], strerror(errno));
				rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
				return false;
			}
		}
	}

	/* Updates and removals first, back to front. */
	RuleWriteChange *ordered[RULE_WRITE_MAX_CHANGES];
	size_t n_ordered = 0;
	for (size_t i = 0; i < b->n; i++)
		if (b->changes[i].op != RW_ADD)
			ordered[n_ordered++] = &b->changes[i];
	rw_sort_desc(ordered, n_ordered);

	for (size_t i = 0; i < n_ordered; i++) {
		RuleWriteChange *c = ordered[i];
		char *text = texts[c->file];
		char indent[32];
		kdl_line_indent(text, c->span_start, indent, sizeof(indent));

		char *next = NULL;
		if (c->op == RW_REMOVE) {
			/* Re-parsed to find the node, because kdl_edit_remove wants a node
			 * to take the comments above it. Parsing the CURRENT text rather
			 * than the original is what makes the descending order safe: this
			 * span has not moved, and the ones already edited are all after it.
			 */
			KdlDocument doc;
			char err[256];
			if (!kdl_parse(text, &doc, err, sizeof(err))) {
				snprintf(b->error, sizeof(b->error), "would-not-parse");
				snprintf(b->detail, sizeof(b->detail), "%s: %s",
						 config_files[c->file], err);
				rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
				return false;
			}
			const KdlNode *node = NULL;
			for (size_t d = 0; d < doc.n_nodes && !node; d++) {
				if (doc.nodes[d].span.start == c->span_start)
					node = &doc.nodes[d];
				for (size_t k = 0; k < doc.nodes[d].n_children && !node; k++)
					if (doc.nodes[d].children[k].span.start == c->span_start)
						node = &doc.nodes[d].children[k];
			}
			if (node)
				next = kdl_edit_remove(text, node, true);
			kdl_free(&doc);
			if (!next) {
				rw_fail(c, "write-failed", "could not locate the node to remove");
				rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
				return false;
			}
		} else {
			char rendered[4096];
			bool made = rules ? rw_render_rule(c, indent, rendered,
											   sizeof(rendered))
							  : rw_render_bind(c, indent, rendered,
											   sizeof(rendered));
			if (!made) {
				rw_fail(c, "bad-value", "cannot be written as KDL");
				rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
				return false;
			}
			next = kdl_splice(text, c->span_start, c->span_end, rendered);
			if (!next) {
				rw_fail(c, "write-failed", "splice failed");
				rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
				return false;
			}
		}
		free(texts[c->file]);
		texts[c->file] = next;
		touched[c->file] = true;
	}

	/* Then the additions, appended -- which cannot disturb any offset above. */
	for (size_t i = 0; i < b->n; i++) {
		RuleWriteChange *c = &b->changes[i];
		if (c->op != RW_ADD)
			continue;
		char *text = texts[c->file];
		if (!text) {
			text = kdl_file_slurp(config_files[c->file]);
			if (!text) {
				snprintf(b->error, sizeof(b->error), "write-failed");
				snprintf(b->detail, sizeof(b->detail), "cannot read %s",
						 config_files[c->file]);
				rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
				return false;
			}
			texts[c->file] = text;
		}
		KdlDocument doc;
		char err[256];
		if (!kdl_parse(text, &doc, err, sizeof(err))) {
			snprintf(b->error, sizeof(b->error), "would-not-parse");
			snprintf(b->detail, sizeof(b->detail), "%s: %s",
					 config_files[c->file], err);
			rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
			return false;
		}
		bool inside = false;
		size_t at = rw_insert_point(&doc, rules ? "window-rule" : "binds", text,
									&inside);
		kdl_free(&doc);

		char indent[32] = "";
		char rendered[4096];
		if (rules) {
			if (!rw_render_rule(c, indent, rendered, sizeof(rendered))) {
				rw_fail(c, "bad-value", "cannot be written as KDL");
				rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
				return false;
			}
		} else {
			kdl_detect_indent(text, indent, sizeof(indent));
			if (!rw_render_bind(c, indent, rendered, sizeof(rendered))) {
				rw_fail(c, "bad-value", "cannot be written as KDL");
				rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
				return false;
			}
		}

		char ins[4352];
		if (inside)
			/* Inside an existing `binds { }`: one indented line before the
			 * closing brace. */
			snprintf(ins, sizeof(ins), "%s%s\n", indent[0] ? indent : "\t",
					 rendered);
		else if (rules)
			snprintf(ins, sizeof(ins), "\n%s\n", rendered);
		else
			/* No binds block at all: make one. */
			snprintf(ins, sizeof(ins), "\nbinds {\n%s%s\n}\n",
					 indent[0] ? indent : "\t", rendered);

		char *next = kdl_splice(text, at, at, ins);
		if (!next) {
			rw_fail(c, "write-failed", "splice failed");
			rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
			return false;
		}
		free(texts[c->file]);
		texts[c->file] = next;
		touched[c->file] = true;
	}

	/* Re-parse every touched file BEFORE writing any of them. "Apply must never
	 * leave me with a config that does not load" is what a settings app owes
	 * this file, and it costs one parse. */
	for (size_t f = 0; f < CONFIG_WRITE_MAX_FILES; f++) {
		if (!touched[f])
			continue;
		KdlDocument doc;
		char err[256];
		if (!kdl_parse(texts[f], &doc, err, sizeof(err))) {
			snprintf(b->error, sizeof(b->error), "would-not-parse");
			snprintf(b->detail, sizeof(b->detail), "%s: %s", config_files[f],
					 err);
			rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
			return false;
		}
		kdl_free(&doc);
	}

	for (size_t f = 0; f < CONFIG_WRITE_MAX_FILES; f++) {
		if (!touched[f])
			continue;
		if (!kdl_file_replace(config_files[f], texts[f], true)) {
			snprintf(b->error, sizeof(b->error), "write-failed");
			snprintf(b->detail, sizeof(b->detail), "%s: %s", config_files[f],
					 strerror(errno));
			rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
			return false;
		}
	}

	rw_free_texts(texts, CONFIG_WRITE_MAX_FILES);
	return true;
}

#endif /* ASTEROIDZ_RULE_WRITE_H */
