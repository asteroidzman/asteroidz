#ifndef ASTEROIDZ_TAG_SCHEMA_H
#define ASTEROIDZ_TAG_SCHEMA_H

/* A machine-readable description of every tag-rule field.
 *
 * What rule-schema.h does for `window-rule`, for `tag`. A tag rule is where the
 * per-tag layout lives -- which layout tag 3 opens in, on which monitor, with
 * which master factor and which scroller proportions -- and until this it was
 * reachable only by editing the config file. `set-config` writes OPTIONS; a tag
 * rule is not an option, so nothing served it and nothing could write it.
 *
 * The types are reused from rule-schema.h rather than restated. They are the
 * same distinctions and they matter here for the same reasons: a tri-state is
 * not a checkbox, an enum is not a text field, and a float with a real range is
 * a slider rather than a box you can type 40 into.
 *
 * ── the spelling ────────────────────────────────────────────────────────────
 *
 * `layout` is the KDL name for `layout_name`, and that special case was already
 * in kdl_tag(). Everything else took its child name VERBATIM as the tagrule key,
 * so `tag 6 { no-render-border 1 }` -- spelled the way every other block in this
 * config language spells things -- produced the key `no-render-border`, which
 * parse_option does not know, and set nothing at all while looking correct.
 * kdl_tag now folds hyphens to underscores the way kdl_bar_custom already did,
 * so both spellings work and `nice` below is the hyphenated one.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	const char *key;  /* what the tagrule= parser matches */
	const char *nice; /* the canonical KDL spelling */
	const char *group;
	const char *label;
	const char *desc;
	RuleType type;
	size_t offset; /* offsetof(ConfigTagRule, ...) */
	double min, max;
	const RuleEnumMember *members;
	size_t n_members;
} TagField;

static const RuleGroup tag_groups[] = {
	{"which", "Which tag",
	 "The tag this applies to, and the monitor it applies on. A rule with no "
	 "monitor applies on every one of them."},
	{"layout", "Layout", "How windows are arranged on this tag."},
	{"tile", "Tile", "The master area, for the tiled layout."},
	{"scroller", "Scroller", "Column widths, for the scroller layout."},
	{"behaviour", "Behaviour", "How windows on this tag open and are drawn."},
};

/* The names in layouts[], which is the list `set_layout` and `switch_layout`
 * cycle through. `overview` is deliberately absent: it is a mode you toggle
 * into, not a layout a tag rests in, and offering it here would let a config
 * declare a tag that opens in overview -- a state the compositor leaves as soon
 * as you pick a window. */
static const RuleEnumMember tag_layouts[] = {
	{"tile", "Manual tiling, i3-like. The default for a fresh tag."},
	{"scroller", "A horizontal row of columns you scroll through."},
	{"monocle", "One window at a time, filling the area."},
	{"float", "Nothing is tiled; windows cascade where they open."},
};

/* clang-format off */
static const TagField tag_schema[] = {

/* ===== which tag ===== */
{"id", "id", "which", "Tag",
 "Which tag, 1 to 9. Written as the block's own argument -- `tag 3 { ... }` --"
 " rather than as a field inside it.",
 RULE_INT, offsetof(ConfigTagRule, id), 1, 9, NULL, 0},

{"name", "name", "which", "Name",
 "What the bar calls this tag instead of its number.",
 RULE_STRING, offsetof(ConfigTagRule, name), RULE_NOCLAMP, RULE_NOCLAMP, NULL, 0},

{"monitor_name", "monitor-name", "which", "Monitor",
 "Apply only on this output, by connector name (DP-1, HDMI-A-1). Empty means"
 " every monitor.",
 RULE_STRING, offsetof(ConfigTagRule, monitor_name), RULE_NOCLAMP, RULE_NOCLAMP, NULL, 0},

{"monitor_make", "monitor-make", "which", "Monitor make",
 "Apply only on outputs with this manufacturer in their EDID.",
 RULE_STRING, offsetof(ConfigTagRule, monitor_make), RULE_NOCLAMP, RULE_NOCLAMP, NULL, 0},

{"monitor_model", "monitor-model", "which", "Monitor model",
 "Apply only on outputs with this model in their EDID.",
 RULE_STRING, offsetof(ConfigTagRule, monitor_model), RULE_NOCLAMP, RULE_NOCLAMP, NULL, 0},

{"monitor_serial", "monitor-serial", "which", "Monitor serial",
 "Apply only on the one output with this serial -- the way to tell two"
 " identical panels apart.",
 RULE_STRING, offsetof(ConfigTagRule, monitor_serial), RULE_NOCLAMP, RULE_NOCLAMP, NULL, 0},

/* ===== layout ===== */
{"layout_name", "layout", "layout", "Layout",
 "The layout this tag opens in.",
 RULE_ENUM, offsetof(ConfigTagRule, layout_name), RULE_NOCLAMP, RULE_NOCLAMP,
 tag_layouts, sizeof(tag_layouts) / sizeof(tag_layouts[0])},

/* ===== tile ===== */
{"mfact", "mfact", "tile", "Master factor",
 "How much of the width the master area takes. Only the tiled layout reads it.",
 RULE_FLOAT, offsetof(ConfigTagRule, mfact), 0.05, 0.95, NULL, 0},

{"nmaster", "nmaster", "tile", "Master count",
 "How many windows go in the master area.",
 RULE_INT, offsetof(ConfigTagRule, nmaster), 1, 10, NULL, 0},

/* ===== scroller ===== */
{"scroller_default_proportion", "scroller-default-proportion", "scroller",
 "Column width",
 "The share of the screen a new column takes, 0 to 1.",
 RULE_FLOAT, offsetof(ConfigTagRule, scroller_default_proportion), 0.05, 1.0, NULL, 0},

{"scroller_default_proportion_single", "scroller-default-proportion-single",
 "scroller", "Column width, alone",
 "The share a column takes when it is the only one. A full-width single window"
 " and narrow columns once there are several is the usual reason to set both.",
 RULE_FLOAT, offsetof(ConfigTagRule, scroller_default_proportion_single), 0.05, 1.0, NULL, 0},

{"scroller_ignore_proportion_single", "scroller-ignore-proportion-single",
 "scroller", "Ignore the single width",
 "Use the ordinary column width even for a lone window.",
 RULE_TRISTATE, offsetof(ConfigTagRule, scroller_ignore_proportion_single),
 RULE_NOCLAMP, RULE_NOCLAMP, NULL, 0},

/* ===== behaviour ===== */
{"open_as_floating", "open-as-floating", "behaviour", "Open floating",
 "Windows opening on this tag start floating, whatever the layout.",
 RULE_INT, offsetof(ConfigTagRule, open_as_floating), 0, 1, NULL, 0},

{"no_render_border", "no-render-border", "behaviour", "No borders",
 "Draw no window borders on this tag.",
 RULE_INT, offsetof(ConfigTagRule, no_render_border), 0, 1, NULL, 0},

{"no_hide", "no-hide", "behaviour", "Never hide",
 "Windows on this tag stay mapped when the tag is not selected.",
 RULE_INT, offsetof(ConfigTagRule, no_hide), 0, 1, NULL, 0},
};
/* clang-format on */

#define TAG_SCHEMA_COUNT (sizeof(tag_schema) / sizeof(tag_schema[0]))

static const void *tag_field_ptr(const ConfigTagRule *r, const TagField *f) {
	return (const char *)r + f->offset;
}

/* The value as a user would write it, or false when the rule says nothing about
 * this field.
 *
 * "Says nothing" and "says zero" have to be told apart or an editor cannot
 * round-trip: a rule that never mentioned `nmaster` and one that set it to its
 * default would be written back identically, and the second would start
 * overriding a global the first inherited. Every field here has a sentinel that
 * cannot be a real value -- NULL for the strings, 0 for counts and factors that
 * are meaningless at zero, -1 for the tri-state.
 */
static bool tag_format(const ConfigTagRule *r, const TagField *f, char *out,
					   size_t cap) {
	const void *p = tag_field_ptr(r, f);
	switch (f->type) {
	case RULE_STRING:
	case RULE_ENUM: {
		const char *s = *(const char *const *)p;
		if (!s || !*s)
			return false;
		snprintf(out, cap, "%s", s);
		return true;
	}
	case RULE_TRISTATE: {
		int32_t v = *(const int32_t *)p;
		if (v < 0)
			return false;
		snprintf(out, cap, "%d", v ? 1 : 0);
		return true;
	}
	case RULE_INT: {
		int32_t v = *(const int32_t *)p;
		/* `id` is the rule's identity rather than one of its settings, so it is
		 * always reported -- a tag rule for tag 0 is not a thing. */
		if (v == 0 && strcmp(f->key, "id") != 0)
			return false;
		snprintf(out, cap, "%d", v);
		return true;
	}
	case RULE_FLOAT: {
		float v = *(const float *)p;
		if (v <= 0.0f)
			return false;
		snprintf(out, cap, "%g", (double)v);
		return true;
	}
	default:
		return false;
	}
}

#endif /* ASTEROIDZ_TAG_SCHEMA_H */
