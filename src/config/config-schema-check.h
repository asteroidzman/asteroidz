#ifndef ASTEROIDZ_CONFIG_SCHEMA_CHECK_H
#define ASTEROIDZ_CONFIG_SCHEMA_CHECK_H

/* `asteroidz -S`: does the schema agree with the code it describes?
 *
 * config-schema.h is a hand-written table of ~90 entries and growing, which is
 * not reviewable line by line. This makes that acceptable: it drives the REAL
 * parse_option, set_value_default and override_config and asserts every
 * default, every clamp, every offset and every type against them, so review can
 * concentrate on the two things a test cannot check -- the descriptions and the
 * grouping.
 *
 * No C is parsed to do it. An earlier idea was to generate the table from the
 * source, which fails asymmetrically: a wrong generator produces a wrong schema
 * SILENTLY and the settings app then writes wrong values into someone's config,
 * where a wrong checker just produces a red test.
 *
 * Reachable without a compositor for the same reason `-p` is: parse_config,
 * set_value_default and override_config touch nothing but the Config struct.
 * config_apply_live must NOT be called from here -- it talks to the scene.
 *
 * The clamp check in particular is DERIVED FROM BEHAVIOUR, feeding a value past
 * each bound and seeing where it lands, rather than from where the clamp is
 * written. Clamps live in both parse_option (blur_transparency_threshold) and
 * override_config (borderpx), and a check that looked in one place would report
 * half the table as unclamped.
 */

#include "config-schema.h"

static int schema_check_failures;
static int schema_check_count;

static void schema_check_fail(const ConfigOption *o, const char *what,
							  const char *got, const char *want) {
	fprintf(stderr, "FAIL  %-34s %s\n", o->key, what);
	if (got || want)
		fprintf(stderr, "        got %-24s want %s\n", got ? got : "(null)",
				want ? want : "(null)");
	schema_check_failures++;
}

static void schema_check_ok(void) { schema_check_count++; }

/* A value guaranteed not to be the one we are about to write, so "the field
 * changed" is a real signal.
 *
 * For anything with a range the poison must be INSIDE it. An out-of-range
 * sentinel gets clamped by override_config, which changes the field all on its
 * own -- so a schema entry naming a key parse_option does not handle would go on
 * passing the reachability check, which is the one check that exists to catch
 * exactly that. Renaming `gappih` in the table and leaving the poison at
 * 0x5A5A5A5A did precisely this: the clamp pulled it to 1000, the field had
 * "changed", and only the round-trip assertions noticed. */
static void schema_poison(Config *c, const ConfigOption *o,
						  const char *incoming) {
	void *f = schema_field(c, o);
	switch (o->type) {
	case OPT_BOOL:
	case OPT_INT:
	case OPT_ENUM: {
		int32_t p = 0x5A5A5A5A;
		if (!isnan(o->min) && !isnan(o->max)) {
			p = (int32_t)o->min;
			if (incoming && atof(incoming) == o->min)
				p = (int32_t)o->max;
		}
		*(int32_t *)f = p;
		break;
	}
	case OPT_FLOAT: {
		float p = -12345.5f;
		if (!isnan(o->min) && !isnan(o->max)) {
			p = (float)o->min;
			if (incoming && (float)atof(incoming) == p)
				p = (float)o->max;
		}
		*(float *)f = p;
		break;
	}
	case OPT_DOUBLE: {
		double p = -12345.5;
		if (!isnan(o->min) && !isnan(o->max)) {
			p = o->min;
			if (incoming && atof(incoming) == p)
				p = o->max;
		}
		*(double *)f = p;
		break;
	}
	case OPT_COLOR: {
		float *rgba = f;
		rgba[0] = 0.123f;
		rgba[1] = 0.456f;
		rgba[2] = 0.789f;
		rgba[3] = 0.321f;
		break;
	}
	case OPT_STRING:
		if (o->size)
			snprintf((char *)f, o->size, "%s", "\x01poison");
		break;
	case OPT_STRPTR:
		*(char **)f = NULL;
		break;
	case OPT_BEZIER: {
		double *d = f;
		for (int i = 0; i < 4; i++)
			d[i] = -12345.5;
		break;
	}
	}
}

/* Numbers compared as numbers: "24" and "24.0" and "0.5" vs "0.50" are the same
 * value and a strcmp says otherwise. Colours and strings compare literally. */
static bool schema_same(const ConfigOption *o, const char *a, const char *b) {
	switch (o->type) {
	case OPT_BOOL:
	case OPT_INT:
	case OPT_FLOAT:
	case OPT_DOUBLE:
		return fabs(atof(a) - atof(b)) < 1e-6;
	default:
		return !strcmp(a, b);
	}
}

static bool schema_set(const ConfigOption *o, const char *value) {
	char key[128], val[512];
	snprintf(key, sizeof(key), "%s", o->key);
	snprintf(val, sizeof(val), "%s", value);
	parse_option(&config, key, val);
	override_config();
	return true;
}

/* Dump the table, one option per line, tab separated.
 *
 * So tests/check-config-schema.py can ask the BINARY which keys are described
 * instead of parsing this table out of C. Extracting the key list from a
 * multi-line C initialiser by regex is exactly the kind of fragile step that
 * makes a checker quietly stop checking -- an over-matching pattern reported
 * 100 keys where there were 95, and would have hidden five missing entries. The
 * one thing the checker genuinely must parse from source is parse_option's own
 * key list, because there is nowhere else it exists. */
static void config_schema_list(void) {
	printf("# key\tpath\tgroup\tsubgroup\ttype\tmin\tmax\tdefault\n");
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		char lo[32] = "", hi[32] = "";
		if (!isnan(o->min))
			snprintf(lo, sizeof(lo), "%g", o->min);
		if (!isnan(o->max))
			snprintf(hi, sizeof(hi), "%g", o->max);
		printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", o->key,
			   o->path ? o->path : "", o->group,
			   o->subgroup ? o->subgroup : "", schema_type_name(o->type), lo, hi,
			   o->def);
	}
}

/* Dump provenance for every recorded key: which file, which line, which path
 * it was written at, and whether that file may be written back to.
 *
 * `asteroidz -P -c FILE`. Reads a real config, unlike -S and -L, because
 * provenance is a property of the files -- there is nothing to report about the
 * compiled-in defaults. */
static void config_source_dump(void) {
	printf("# key\tfile\tline\tpath\twritable\tin-memory\treason\n");
	for (int32_t i = 0; i < nconfig_origins; i++) {
		const ConfigOrigin *o = &config_origins[i];
		const char *file = "<none>";
		const char *writable = "n/a";
		char why[64] = "";
		if (o->file >= 0 && o->file < nconfig_files) {
			file = config_files[o->file];
			writable = config_file_is_foreign(file, why, sizeof(why)) ? "no"
																	  : "yes";
		}
		/* Two columns, not one. A key can have been set in memory AND still have
		 * a declaration in a file -- that is what a live preview is -- so
		 * collapsing them into "<runtime>" in the file column hid the file the
		 * write path needs. */
		printf("%s\t%s\t%d\t%s\t%s\t%s\t%s\n", o->key, file, o->line,
			   o->path, writable, o->runtime ? "yes" : "no", why);
	}
}

/* Dump the dispatch-action table, one action per line.
 *
 * Same reason config_schema_list exists: tests/check-dispatch-actions.py asks
 * the binary rather than regexing a multi-line C initialiser out of the source,
 * because a pattern that quietly over- or under-matches turns a checker into
 * decoration. Declared here and defined in ipc-config.h, which owns the table. */
static void dispatch_actions_list(void);

static void rule_check_fail(const RuleField *f, const char *what,
							const char *got, const char *want) {
	fprintf(stderr, "  rule %-28s %s", f ? f->key : "?", what);
	if (got)
		fprintf(stderr, " (got '%s'", got);
	if (want)
		fprintf(stderr, ", want '%s'", want);
	fprintf(stderr, "%s\n", got ? ")" : "");
	schema_check_failures++;
}

/* Check the rule table against the parser it describes.
 *
 * Every field is written through the REAL windowrule branch -- one rule per
 * field, built as the legacy comma string parse_option takes -- and read back
 * through its own offset. That is what makes a wrong offset a red test rather
 * than a rule editor that silently writes `isnoborder` into `isnoshadow`: the
 * struct is thirty-odd consecutive int32_t and nothing about a wrong offsetof is
 * visible by eye or to the compiler.
 *
 * On a scratch Config, never `config`: this runs before a session exists on the
 * -S path, and appending fifty rules to the live one would be a compositor that
 * starts with fifty rules matching nothing.
 */
static void rule_schema_self_check(void) {
	char buf[512];

	/* ---- table sanity ---- */
	for (size_t i = 0; i < RULE_SCHEMA_COUNT; i++) {
		const RuleField *f = &rule_schema[i];
		if (!f->key || !*f->key || !f->nice || !*f->nice || !f->label ||
			!f->desc || !f->group) {
			rule_check_fail(f, "missing a required field", NULL, NULL);
			continue;
		}
		for (size_t j = i + 1; j < RULE_SCHEMA_COUNT; j++) {
			if (!strcmp(f->key, rule_schema[j].key))
				rule_check_fail(f, "duplicate key", NULL, NULL);
			if (!strcmp(f->nice, rule_schema[j].nice))
				rule_check_fail(f, "duplicate KDL name", f->nice, NULL);
		}
		/* A nice name that collides with a DIFFERENT field's key would make the
		 * fall-through resolve to the wrong branch: rule_field_key_for_nice
		 * returns its argument unchanged when nothing matches, so a nice name
		 * equal to another key silently becomes that key. */
		for (size_t j = 0; j < RULE_SCHEMA_COUNT; j++) {
			if (i == j)
				continue;
			if (!strcmp(f->nice, rule_schema[j].key))
				rule_check_fail(f, "KDL name collides with another field's key",
								f->nice, rule_schema[j].key);
		}
		bool group_known = false;
		for (size_t g = 0; g < LENGTH(rule_groups); g++)
			if (!strcmp(f->group, rule_groups[g].name))
				group_known = true;
		if (!group_known)
			rule_check_fail(f, "group is not in rule_groups", f->group, NULL);
		if (f->type == RULE_ENUM && f->n_members == 0)
			rule_check_fail(f, "enum with no members", NULL, NULL);
		if (f->type != RULE_ENUM && f->n_members != 0)
			rule_check_fail(f, "members on a non-enum", NULL, NULL);
		if (f->offset >= sizeof(ConfigWinRule))
			rule_check_fail(f, "offset is outside ConfigWinRule", NULL, NULL);
		schema_check_count++;

		/* Both directions of the name mapping. The second is the one that keeps
		 * old configs working, and nothing else tests it. */
		if (strcmp(rule_field_key_for_nice(f->nice), f->key))
			rule_check_fail(f, "the KDL name does not map back to the key",
							rule_field_key_for_nice(f->nice), f->key);
		else
			schema_check_count++;
		if (strcmp(rule_field_key_for_nice(f->key), f->key))
			rule_check_fail(f, "the bare key no longer falls through",
							rule_field_key_for_nice(f->key), f->key);
		else
			schema_check_count++;
	}

	/* ---- reachability and offsets, through the real parser ---- */
	Config scratch;
	memset(&scratch, 0, sizeof(scratch));

	for (size_t i = 0; i < RULE_SCHEMA_COUNT; i++) {
		const RuleField *f = &rule_schema[i];
		/* RULE_BIND resolves to a mod mask and a keysym and cannot be formatted
		 * back, so there is nothing to compare against. Its reachability is
		 * covered by the coverage checker instead. */
		if (f->type == RULE_BIND)
			continue;

		/* A probe value that is legal for the type AND distinguishable from the
		 * unset state -- which for the numeric kinds means not zero, since zero
		 * IS unset for every int and float rule here. */
		const char *probe;
		switch (f->type) {
		case RULE_MATCH:
		case RULE_STRING:
			probe = "asteroidz-probe";
			break;
		case RULE_ENUM:
			probe = f->members[0].name;
			break;
		case RULE_TAG:
		case RULE_INT:
			/* The SAME probe for both, and not 1.
			 *
			 * A tag is written 1..9 and stored as 1 << (n-1), so the only way to
			 * tell RULE_TAG from RULE_INT is a value where the two disagree --
			 * and 1 is not one, because 1 << 0 is 1. Relabelling `tags` as an
			 * int passed this check cleanly until the probe stopped being a
			 * fixed point of the mask. */
			probe = "4";
			break;
		case RULE_FLOAT:
			probe = "0.5";
			break;
		default:
			probe = "1";
			break;
		}

		snprintf(buf, sizeof(buf), "%s:%s", f->key, probe);
		scratch.window_rules_count = 0;
		if (!parse_option(&scratch, "windowrule", buf)) {
			rule_check_fail(f, "the parser refused its own key", buf, NULL);
			continue;
		}
		if (scratch.window_rules_count != 1) {
			rule_check_fail(f, "no rule was produced", buf, NULL);
			continue;
		}
		char got[256];
		const ConfigWinRule *r = &scratch.window_rules[0];
		if (!rule_format(r, f, got, sizeof(got)))
			rule_check_fail(f, "wrote nothing the schema can read back", probe,
							NULL);
		else if (strcmp(got, probe))
			rule_check_fail(f, "read back as something else", got, probe);
		else
			schema_check_count++;

		/* And that it wrote ONLY its own field. The check that catches a
		 * copy-pasted offsetof: every other field must still be unset, which is
		 * exactly what rule_format reports by returning false. */
		for (size_t j = 0; j < RULE_SCHEMA_COUNT; j++) {
			if (j == i || rule_schema[j].type == RULE_BIND)
				continue;
			char other[256];
			if (rule_format(r, &rule_schema[j], other, sizeof(other))) {
				rule_check_fail(f, "also set another field",
								rule_schema[j].key, other);
				break;
			}
		}
		schema_check_count++;
	}

	/* Both ends of the tag arithmetic, explicitly.
	 *
	 * The round trip above uses one value, and one value cannot distinguish an
	 * off-by-one in the shift from a correct one: `1 << (n-1)` and `1 << n` both
	 * round-trip if the formatter makes the matching mistake. Tag 1 is the low
	 * bit and tag 9 is the top of the range, so a shift that is off by one lands
	 * outside the first or produces the wrong number for the second. */
	for (int32_t tag = 1; tag <= 9; tag += 8) {
		const RuleField *f = rule_field_by_key("tags");
		snprintf(buf, sizeof(buf), "tags:%d", tag);
		scratch.window_rules_count = 0;
		char want[32];
		snprintf(want, sizeof(want), "%d", tag);
		if (parse_option(&scratch, "windowrule", buf) &&
			scratch.window_rules_count == 1) {
			char got[32];
			if (!rule_format(&scratch.window_rules[0], f, got, sizeof(got)))
				rule_check_fail(f, "a tag read back as unset", want, NULL);
			else if (strcmp(got, want))
				rule_check_fail(f, "a tag did not round-trip", got, want);
			else
				schema_check_count++;
		} else {
			rule_check_fail(f, "a tag rule did not parse", buf, NULL);
		}
	}

	/* The unset state is what an untouched rule reports, and every RULE_TRISTATE
	 * has to start at -1 rather than 0 for that to be true. Checked against a
	 * rule the parser produced from a match alone, not against a memset one:
	 * the initialisation lives in the parser branch, which is the thing that
	 * could stop doing it. */
	scratch.window_rules_count = 0;
	/* A writable copy, not a literal: the windowrule branch tokenises its value
	 * with strtok, which writes NULs into the string it is given. A literal
	 * there is a segfault, and the compiler has nothing to say about it. */
	snprintf(buf, sizeof(buf), "appid:probe");
	if (parse_option(&scratch, "windowrule", buf) &&
		scratch.window_rules_count == 1) {
		const ConfigWinRule *r = &scratch.window_rules[0];
		for (size_t i = 0; i < RULE_SCHEMA_COUNT; i++) {
			const RuleField *f = &rule_schema[i];
			if (f->type == RULE_BIND || !strcmp(f->key, "appid"))
				continue;
			if (rule_format(r, f, buf, sizeof(buf)))
				rule_check_fail(f, "is set in a rule that only matched", buf,
								NULL);
			else
				schema_check_count++;
		}
	} else {
		rule_check_fail(NULL, "a match-only rule did not parse", NULL, NULL);
	}
	free(scratch.window_rules);
}

/* One rule field per line, for tests/check-rule-schema.py. Same reason
 * config_schema_list exists: the checker asks the binary rather than regexing a
 * multi-line C initialiser, because a pattern that quietly under-matches turns a
 * coverage test into decoration. */
static void rule_schema_list(void) {
	printf("# key\tnice\tgroup\ttype\tmin\tmax\tlabel\n");
	for (size_t i = 0; i < RULE_SCHEMA_COUNT; i++) {
		const RuleField *f = &rule_schema[i];
		char lo[32] = "-", hi[32] = "-";
		if (!isnan(f->min))
			snprintf(lo, sizeof(lo), "%g", f->min);
		if (!isnan(f->max))
			snprintf(hi, sizeof(hi), "%g", f->max);
		printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n", f->key, f->nice, f->group,
			   rule_type_name(f->type), lo, hi, f->label);
	}
}

/* Run every check. Returns the number of failures. */
static int config_schema_self_check(void) {
	char got[512], want[512], buf[512];
	schema_check_failures = 0;
	schema_check_count = 0;

	/* ---- table sanity: things no other check can see ---- */
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		if (!o->key || !*o->key || !o->label || !o->desc || !o->group ||
			!o->def) {
			schema_check_fail(o, "an entry is missing a required field", NULL,
							  NULL);
			continue;
		}
		for (size_t j = i + 1; j < CONFIG_SCHEMA_COUNT; j++) {
			if (!strcmp(o->key, config_schema[j].key))
				schema_check_fail(o, "duplicate key", NULL, NULL);
			if (o->path && config_schema[j].path &&
				!strcmp(o->path, config_schema[j].path))
				schema_check_fail(o, "duplicate KDL path", o->path, NULL);
		}
		bool group_known = false;
		for (size_t g = 0; g < sizeof(config_groups) / sizeof(config_groups[0]);
			 g++)
			if (!strcmp(o->group, config_groups[g].name))
				group_known = true;
		if (!group_known)
			schema_check_fail(o, "group is not in config_groups", o->group,
							  NULL);
		if (o->type == OPT_ENUM && o->n_members == 0)
			schema_check_fail(o, "enum with no members", NULL, NULL);
		if (o->type == OPT_STRING && o->size == 0)
			schema_check_fail(o, "string without its buffer size", NULL, NULL);
		if (o->type != OPT_STRING && o->size != 0)
			schema_check_fail(o, "size set on a non-string", NULL, NULL);
		/* A default that is not a legal value is a Reset button that fails. */
		if (!schema_validate(o, o->def, buf, sizeof(buf)))
			schema_check_fail(o, "default is not a valid value", o->def, buf);
		if (o->type == OPT_ENUM) {
			bool def_is_real = false;
			for (size_t m = 0; m < o->n_members; m++)
				if (!o->members[m].alias && !strcmp(o->members[m].name, o->def))
					def_is_real = true;
			if (!def_is_real)
				schema_check_fail(
					o, "enum default is an alias or not a member", o->def, NULL);
		}
		schema_check_ok();
	}

	/* ---- every claimed KDL path is one the parser can read back ----
	 *
	 * The hole this closes: every other check here goes through parse_option with
	 * the internal KEY, so a schema entry could name a nested `path` that
	 * kdl_lookup_key has no entry for and nothing would notice. The write path
	 * then produces exactly that path in the config file, and the next reload
	 * answers "Unknown keyword". Found the hard way -- theme/border-color and
	 * animations/enable were both claimed here and unreachable, and the failure
	 * surfaced as a config that would not load after a settings batch. */
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		if (!o->path)
			continue;
		if (!strcmp(o->path, o->key)) {
			/* A bare top-level key: kdl_leaf's fallback to the node's own name
			 * handles it, and no map entry is needed or wanted. */
			schema_check_ok();
			continue;
		}
		const char *back = kdl_lookup_key(o->path);
		if (!back)
			schema_check_fail(o, "claims a KDL path the parser cannot resolve",
							  o->path, "an entry in kdl_key_map");
		else if (strcmp(back, o->key))
			schema_check_fail(o, "its KDL path resolves to another key", back,
							  o->key);
		else
			schema_check_ok();
	}

	/* ---- defaults: locked to set_value_default, with no C parsed ---- */
	set_value_default();
	override_config();
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		schema_format(&config, o, got, sizeof(got));
		snprintf(want, sizeof(want), "%s", o->def);
		if (!schema_same(o, got, want))
			schema_check_fail(o, "default does not match the code", got, want);
		else
			schema_check_ok();
	}

	/* ---- reachability: does parse_option actually handle this key? ----
	 *
	 * The if/else chain silently ignores a key it does not know, so a schema
	 * entry naming a key that was renamed would look fine everywhere else. */
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		/* Several options default to empty or zero, which leaves nothing for
		 * "the field changed" to detect. Probe with a value of the right type
		 * that is definitely NOT the default rather than skipping -- skipping
		 * would exempt exactly the entries whose key is hardest to verify by
		 * eye, and reachability is the check that catches a renamed key. */
		const char *probe = o->def;
		char synth[64];
		if (!*o->def || !strcmp(o->def, "0") || !strcmp(o->def, "0x00000000")) {
			switch (o->type) {
			case OPT_COLOR: probe = "0x1234567f"; break;
			case OPT_BOOL: probe = "1"; break;
			case OPT_ENUM:
				probe = o->members[o->n_members - 1].name;
				break;
			case OPT_STRING:
			case OPT_STRPTR: probe = "probe"; break;
			case OPT_BEZIER: probe = "0.11 0.22 0.33 0.44"; break;
			default:
				/* Inside the clamp, or override_config puts it back and the
				 * field looks unreachable. */
				snprintf(synth, sizeof(synth), "%g",
						 isnan(o->max) ? 7.0 : (isnan(o->min) ? o->max
														: (o->min + o->max) / 2));
				probe = synth;
				break;
			}
		}
		schema_poison(&config, o, probe);
		schema_format(&config, o, buf, sizeof(buf));
		schema_set(o, probe);
		schema_format(&config, o, got, sizeof(got));
		if (!strcmp(got, buf))
			schema_check_fail(o, "parse_option does not reach this key", got,
							  "anything else");
		else
			schema_check_ok();
	}

	/* ---- round trip: format(parse(x)) == x ---- */
	set_value_default();
	override_config();
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		if (!*o->def) {
			/* Nothing to round-trip, but the min/max probes below still are. */
			schema_check_ok();
			goto bounds;
		}
		schema_set(o, o->def);
		schema_format(&config, o, got, sizeof(got));
		if (!schema_same(o, got, o->def))
			schema_check_fail(o, "value does not survive a round trip", got,
							  o->def);
		else
			schema_check_ok();
	bounds:
		if (isnan(o->min) || isnan(o->max))
			continue;
		if (o->type == OPT_ENUM || o->type == OPT_COLOR ||
			o->type == OPT_STRING || o->type == OPT_STRPTR ||
			o->type == OPT_BEZIER)
			continue;
		char probe[64];
		snprintf(probe, sizeof(probe), "%g", o->min);
		schema_set(o, probe);
		schema_format(&config, o, got, sizeof(got));
		if (!schema_same(o, got, probe))
			schema_check_fail(o, "the minimum does not survive a round trip",
							  got, probe);
		else
			schema_check_ok();
		snprintf(probe, sizeof(probe), "%g", o->max);
		schema_set(o, probe);
		schema_format(&config, o, got, sizeof(got));
		if (!schema_same(o, got, probe))
			schema_check_fail(o, "the maximum does not survive a round trip",
							  got, probe);
		else
			schema_check_ok();
	}

	/* ---- clamps, derived from behaviour ---- */
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		if (isnan(o->min) && isnan(o->max))
			continue;
		if (o->type == OPT_ENUM || o->type == OPT_COLOR ||
			o->type == OPT_STRING || o->type == OPT_STRPTR ||
			o->type == OPT_BEZIER)
			continue;
		char probe[64];
		if (!isnan(o->min)) {
			double under = o->min - (o->type == OPT_INT || o->type == OPT_BOOL
										 ? 1.0
										 : (fabs(o->min) + 1.0));
			snprintf(probe, sizeof(probe), "%g", under);
			schema_set(o, probe);
			schema_format(&config, o, got, sizeof(got));
			snprintf(want, sizeof(want), "%g", o->min);
			if (!schema_same(o, got, want))
				schema_check_fail(o, "a value below the minimum is not clamped",
								  got, want);
			else
				schema_check_ok();
		}
		if (!isnan(o->max)) {
			double over = o->max + (o->type == OPT_INT || o->type == OPT_BOOL
										? 1.0
										: (fabs(o->max) + 1.0));
			snprintf(probe, sizeof(probe), "%g", over);
			schema_set(o, probe);
			schema_format(&config, o, got, sizeof(got));
			snprintf(want, sizeof(want), "%g", o->max);
			if (!schema_same(o, got, want))
				schema_check_fail(o, "a value above the maximum is not clamped",
								  got, want);
			else
				schema_check_ok();
		}
	}

	rule_schema_self_check();

	printf("%zu options, %zu rule fields, %d checks, %d failures\n",
		   CONFIG_SCHEMA_COUNT, RULE_SCHEMA_COUNT, schema_check_count,
		   schema_check_failures);
	return schema_check_failures;
}

#endif /* ASTEROIDZ_CONFIG_SCHEMA_CHECK_H */
