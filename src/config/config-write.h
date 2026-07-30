#ifndef ASTEROIDZ_CONFIG_WRITE_H
#define ASTEROIDZ_CONFIG_WRITE_H

/* Applying a batch of config changes, and making them stick.
 *
 * `dispatch set_option` has always been able to change any option in memory. It
 * writes nothing, so the change is gone at the next reload -- a setting that
 * vanishes when you reload is not a setting, it is a preview. This is the other
 * half.
 *
 * A BATCH, not one call per option, because that is how a settings panel works:
 * you touch six controls and press Apply, and either all six take effect or none
 * do. Applying them one at a time means a failure halfway leaves the config in a
 * state nobody asked for and the panel showing something else again.
 *
 * The order is: resolve everything, validate everything, and only then touch
 * anything. Validation before the first write is the whole contract.
 */

/* Included from parse_config.h's side of the tree, so `Config`, `config_files`
 * and `nconfig_files` are already in scope -- this file cannot be compiled on
 * its own, the same as config-schema.h and config-source.h. stdarg and errno are
 * named explicitly rather than inherited: config_write_fail is variadic and the
 * commit path reports strerror(errno), and both worked only because something
 * upstream happened to include them. */
#include <errno.h>
#include <stdarg.h>

#include "../common/kdl-file.h"
#include "../common/kdl-write.h"
#include "config-schema.h"
#include "config-source.h"

#define CONFIG_WRITE_MAX_CHANGES 64
#define CONFIG_WRITE_MAX_FILES 8

/* Render a value as the KDL text for its type.
 *
 * The editor takes pre-formatted arguments and does not know what an option is,
 * so deciding this is the caller's job. Getting it wrong is not a cosmetic
 * problem: an unquoted font description is several arguments rather than one,
 * and `theme { font Ubuntu 17 }` is a two-argument node whose first argument is
 * the string "Ubuntu". */
static bool config_write_render(const ConfigOption *o, const char *value,
								char args[4][256], size_t *nargs) {
	*nargs = 0;
	switch (o->type) {
	case OPT_BEZIER: {
		/* Four separate arguments, not one string with spaces in it. */
		const char *p = value;
		while (*p && *nargs < 4) {
			while (*p == ' ' || *p == ',')
				p++;
			if (!*p)
				break;
			const char *s = p;
			while (*p && *p != ' ' && *p != ',')
				p++;
			size_t n = (size_t)(p - s);
			if (n >= sizeof(args[0]))
				return false;
			memcpy(args[*nargs], s, n);
			args[*nargs][n] = '\0';
			(*nargs)++;
		}
		return *nargs == 4;
	}
	case OPT_STRING:
	case OPT_STRPTR: {
		/* Always quoted, even when the value would survive bare. A font that
		 * gains a space later must not silently change meaning, and a path
		 * beginning with `/` is a comment to the KDL lexer -- the regression
		 * suite lost an afternoon to `spawn /tmp/x.sh` for exactly that. */
		size_t o_i = 0;
		args[0][o_i++] = '"';
		for (const char *p = value; *p; p++) {
			if (o_i + 3 >= sizeof(args[0]))
				return false;
			if (*p == '"' || *p == '\\')
				args[0][o_i++] = '\\';
			args[0][o_i++] = *p;
		}
		args[0][o_i++] = '"';
		args[0][o_i] = '\0';
		*nargs = 1;
		return true;
	}
	default:
		/* Numbers, colours and enum member names are all bare identifiers as
		 * far as KDL is concerned. schema_validate has already refused anything
		 * that is not. */
		if (strlen(value) >= sizeof(args[0]))
			return false;
		snprintf(args[0], sizeof(args[0]), "%s", value);
		*nargs = 1;
		return true;
	}
}

/* ---------- a change, as it moves through the pipeline ---------- */

typedef struct {
	const ConfigOption *opt;
	char value[512]; /* empty when `remove` */
	bool remove;     /* reset to the compiled-in default */

	/* filled in while planning */
	int32_t file;      /* index into config_files[], or -1 for "append" */
	char path[160];    /* where to write it */
	bool from_source;  /* the path came from provenance, not the schema */

	/* filled in on failure */
	char error[48];
	char detail[224];
	int32_t line;
	bool created;
} ConfigChange;

typedef struct {
	ConfigChange changes[CONFIG_WRITE_MAX_CHANGES];
	size_t n;
	bool persist;
	bool override_foreign;

	/* staged documents, one per file touched */
	struct {
		int32_t file;
		char *text;
	} staged[CONFIG_WRITE_MAX_FILES];
	size_t n_staged;

	char error[48];   /* a whole-batch failure */
	char detail[224];
} ConfigWriteBatch;

static void config_write_fail(ConfigChange *c, const char *err,
							  const char *fmt, ...) {
	snprintf(c->error, sizeof(c->error), "%s", err);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c->detail, sizeof(c->detail), fmt, ap);
	va_end(ap);
}

/* Step 1+2: resolve every change and validate every value.
 *
 * Returns false if ANY change is bad, having marked which. Nothing has been
 * touched at this point and nothing will be -- that is the Apply button's
 * contract, and the reason validation is a separate pass rather than something
 * each write discovers for itself. */
static bool config_write_plan(ConfigWriteBatch *b) {
	bool ok = true;
	for (size_t i = 0; i < b->n; i++) {
		ConfigChange *c = &b->changes[i];
		if (!c->opt) {
			ok = false; /* the caller already recorded unknown-key */
			continue;
		}
		if (!c->remove) {
			char why[192];
			if (!schema_validate(c->opt, c->value, why, sizeof(why))) {
				/* REFUSED, not clamped. A UI has the schema and can bound its
				 * own controls; clamping silently is how a panel ends up showing
				 * 200 while the compositor runs 100. */
				const char *code = strstr(why, "minimum") || strstr(why, "maximum")
									   ? "out-of-range"
									   : "bad-value";
				config_write_fail(c, code, "%s", why);
				ok = false;
				continue;
			}
		}
		if (!b->persist)
			continue;

		/* Which file, and which path inside it.
		 *
		 * Provenance first, because the declaration in force is the only one
		 * worth editing -- and it is often not at the canonical path. Falling
		 * back to the canonical path in the main config is right only when
		 * nothing sets the key yet. */
		const ConfigOrigin *g = config_source_of(c->opt->key);
		if (g && g->file >= 0 && g->file < nconfig_files) {
			const char *file = config_files[g->file];
			char why[64] = "";
			if (config_file_is_foreign(file, why, sizeof(why)) &&
				!b->override_foreign) {
				config_write_fail(
					c, "read-only-source",
					"%s is generated (%s); pass override:true to shadow it from "
					"the main config",
					file, why);
				ok = false;
				continue;
			}
			if (config_file_is_foreign(file, why, sizeof(why))) {
				/* Overriding: append to the MAIN config, not to the generated
				 * file. `source` is applied in place and later declarations win,
				 * so a block after the `source` line shadows it -- and survives
				 * the generator rewriting its own file. */
				c->file = -1;
				snprintf(c->path, sizeof(c->path), "%s",
						 c->opt->path ? c->opt->path : c->opt->key);
			} else {
				c->file = g->file;
				snprintf(c->path, sizeof(c->path), "%s", g->path);
				c->from_source = true;
			}
		} else {
			if (nconfig_files < 1) {
				config_write_fail(c, "no-writable-file",
								  "the config was not read from any file");
				ok = false;
				continue;
			}
			c->file = 0; /* the main config: parse_config_file reads it first */
			snprintf(c->path, sizeof(c->path), "%s",
					 c->opt->path ? c->opt->path : c->opt->key);
		}

		if (c->remove && !c->from_source) {
			/* Nothing on disk to remove. Not an error -- "reset this to its
			 * default" is satisfied by a value that was never written down --
			 * but there is no file edit to make. */
			c->file = -2;
		}
	}
	return ok;
}

static char **config_write_slot(ConfigWriteBatch *b, int32_t file) {
	for (size_t i = 0; i < b->n_staged; i++)
		if (b->staged[i].file == file)
			return &b->staged[i].text;
	if (b->n_staged >= CONFIG_WRITE_MAX_FILES)
		return NULL;
	b->staged[b->n_staged].file = file;
	b->staged[b->n_staged].text = NULL;
	return &b->staged[b->n_staged++].text;
}

/* Step 3: build the new text for every file, in memory.
 *
 * Re-parsed between edits to the same file, because a span is an offset into the
 * text it came from and every edit moves the bytes after it. Parsing three times
 * to change three options in one file is not a cost worth optimising: this runs
 * when someone presses Apply. */
static bool config_write_stage(ConfigWriteBatch *b) {
	for (size_t i = 0; i < b->n; i++) {
		ConfigChange *c = &b->changes[i];
		if (c->file == -2)
			continue;
		int32_t target = c->file < 0 ? 0 : c->file;
		char **slot = config_write_slot(b, target);
		if (!slot) {
			config_write_fail(c, "no-writable-file",
							  "too many files in one batch");
			return false;
		}
		if (!*slot) {
			*slot = kdl_file_slurp(config_files[target]);
			if (!*slot) {
				config_write_fail(c, "write-failed", "cannot read %s",
								  config_files[target]);
				return false;
			}
		}

		KdlDocument doc = {0};
		char kdlerr[256] = "";
		if (!kdl_parse(*slot, &doc, kdlerr, sizeof(kdlerr))) {
			config_write_fail(c, "would-not-parse", "%s: %s",
							  config_files[target], kdlerr);
			kdl_free(&doc);
			return false;
		}

		char *next = NULL;
		if (c->remove) {
			const KdlNode *n = kdl_locate_path(&doc, c->path);
			if (!n)
				n = kdl_locate_key(&doc, c->opt->key);
			next = n ? kdl_edit_remove(*slot, n, false) : strdup(*slot);
			if (n)
				c->line = n->span.line;
		} else {
			char args[4][256];
			size_t nargs = 0;
			if (!config_write_render(c->opt, c->value, args, &nargs)) {
				config_write_fail(c, "bad-value",
								  "value cannot be written as KDL");
				kdl_free(&doc);
				return false;
			}
			const char *argv[4] = {args[0], args[1], args[2], args[3]};

			if (c->file < 0) {
				/* The override: a new block at the end of the main config, with
				 * a comment saying what it shadows and why it wins. Silently
				 * shadowing a generated file would be worse than refusing. */
				char why[256];
				snprintf(why, sizeof(why),
						 "// Set here by the settings app. This shadows the\n"
						 "// generated file that also sets it -- `source` is\n"
						 "// applied in place, so a later declaration wins.");
				next = kdl_edit_append_path(*slot, c->path, argv, nargs, why);
			} else {
				const KdlNode *before = kdl_locate_path(&doc, c->path);
				if (!before)
					before = kdl_locate_key(&doc, c->opt->key);
				c->created = (before == NULL);
				next = kdl_edit_set_path(*slot, &doc, c->path, argv, nargs);
			}
		}
		kdl_free(&doc);

		if (!next) {
			config_write_fail(c, "write-failed", "could not edit %s at %s",
							  config_files[target], c->path);
			return false;
		}
		free(*slot);
		*slot = next;
	}
	return true;
}

/* Step 4: every staged document must still parse, THEN they are written.
 *
 * "Apply must never leave me with a config that does not load" is what a
 * settings app owes a file someone maintains by hand, and it costs one parse per
 * file. True atomicity across files is not achievable with rename(2) and a
 * journal is not worth it here: a crash between two renames leaves one file
 * updated, which is survivable, where an unparseable config is not. */
static bool config_write_commit(ConfigWriteBatch *b) {
	for (size_t i = 0; i < b->n_staged; i++) {
		if (!b->staged[i].text)
			continue;
		KdlDocument doc = {0};
		char kdlerr[256] = "";
		bool parses = kdl_parse(b->staged[i].text, &doc, kdlerr, sizeof(kdlerr));
		kdl_free(&doc);
		if (!parses) {
			snprintf(b->error, sizeof(b->error), "would-not-parse");
			snprintf(b->detail, sizeof(b->detail), "%s: %s",
					 config_files[b->staged[i].file], kdlerr);
			return false;
		}
	}
	for (size_t i = 0; i < b->n_staged; i++) {
		if (!b->staged[i].text)
			continue;
		/* A backup, unlike output_persist: that writes one number the user just
		 * picked from a panel showing them the old one, where this can rewrite
		 * a dozen options across two files at once. */
		if (!kdl_file_replace(config_files[b->staged[i].file],
							  b->staged[i].text, true)) {
			snprintf(b->error, sizeof(b->error), "write-failed");
			snprintf(b->detail, sizeof(b->detail), "%s: %s",
					 config_files[b->staged[i].file], strerror(errno));
			return false;
		}
	}
	return true;
}

static void config_write_free(ConfigWriteBatch *b) {
	for (size_t i = 0; i < b->n_staged; i++)
		free(b->staged[i].text);
	b->n_staged = 0;
}

/* Apply the batch in memory. Called AFTER the write succeeded, so the file and
 * the running state cannot disagree about what happened.
 *
 * Each written file is re-parsed once first, purely to give the values their
 * provenance back. Without that, going through parse_option's hook with no
 * context records every persisted change as `runtime` -- "changed in memory,
 * will not survive a reload" -- about a value that had just been written to
 * disk. It also broke removal: the next `value: null` for the same key saw no
 * file provenance, decided there was nothing on disk to remove, and reported
 * success with the declaration still in the file. */
static void config_write_apply_memory(ConfigWriteBatch *b) {
	KdlDocument docs[CONFIG_WRITE_MAX_FILES] = {0};
	bool parsed[CONFIG_WRITE_MAX_FILES] = {false};
	if (b->persist) {
		for (size_t i = 0; i < b->n_staged; i++) {
			char kdlerr[256];
			parsed[i] = kdl_parse(b->staged[i].text, &docs[i], kdlerr,
								  sizeof(kdlerr));
		}
	}

	for (size_t i = 0; i < b->n; i++) {
		ConfigChange *c = &b->changes[i];
		if (!c->opt || c->error[0])
			continue;
		char key[128], val[512];
		snprintf(key, sizeof(key), "%s", c->opt->key);
		snprintf(val, sizeof(val), "%s", c->remove ? c->opt->def : c->value);

		int32_t target = c->file < 0 ? 0 : c->file;
		const KdlNode *node = NULL;
		int32_t slot = -1;
		if (b->persist && c->file != -2) {
			for (size_t s = 0; s < b->n_staged; s++)
				if (b->staged[s].file == target && parsed[s])
					slot = (int32_t)s;
		}
		if (slot >= 0 && !c->remove) {
			node = kdl_locate_path(&docs[slot], c->path);
			if (!node)
				node = kdl_locate_key(&docs[slot], c->opt->key);
		}

		if (c->remove) {
			parse_option(&config, key, val);
			/* Gone from the file, so it reads as the compiled-in default again.
			 * An origin left behind would name a line that no longer holds it. */
			config_source_forget(c->opt->key);
		} else if (node) {
			parse_option(&config, key, val);
			config_source_note_at(c->opt->key, target, node, c->path);
		} else {
			/* persist:false, or the node could not be found again. `runtime` is
			 * then the honest answer. */
			parse_option(&config, key, val);
		}
	}

	for (size_t i = 0; i < b->n_staged; i++)
		if (parsed[i])
			kdl_free(&docs[i]);

	override_config();
	config_apply_live();
}

#endif /* ASTEROIDZ_CONFIG_WRITE_H */
