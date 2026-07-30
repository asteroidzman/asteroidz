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
	printf("# key\tfile\tline\tpath\twritable\treason\n");
	for (int32_t i = 0; i < nconfig_origins; i++) {
		const ConfigOrigin *o = &config_origins[i];
		const char *file = "<runtime>";
		const char *writable = "n/a";
		char why[64] = "";
		if (o->file >= 0 && o->file < nconfig_files) {
			file = config_files[o->file];
			writable = config_file_is_foreign(file, why, sizeof(why)) ? "no"
																	  : "yes";
		}
		printf("%s\t%s\t%d\t%s\t%s\t%s\n", o->key, file, o->line,
			   o->path, writable, why);
	}
}

/* Dump the dispatch-action table, one action per line.
 *
 * Same reason config_schema_list exists: tests/check-dispatch-actions.py asks
 * the binary rather than regexing a multi-line C initialiser out of the source,
 * because a pattern that quietly over- or under-matches turns a checker into
 * decoration. Declared here and defined in ipc-config.h, which owns the table. */
static void dispatch_actions_list(void);

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

	printf("%zu options, %d checks, %d failures\n", CONFIG_SCHEMA_COUNT,
		   schema_check_count, schema_check_failures);
	return schema_check_failures;
}

#endif /* ASTEROIDZ_CONFIG_SCHEMA_CHECK_H */
