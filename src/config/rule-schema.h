#ifndef ASTEROIDZ_RULE_SCHEMA_H
#define ASTEROIDZ_RULE_SCHEMA_H

/* A machine-readable description of every window-rule field.
 *
 * Same problem config-schema.h solves, one level down. A rule editor has to know
 * that `app-id` is a REGEX and not a literal, that `isfloating` has three states
 * and not two, that `tags` is written 1..9 but stored as a bitmask, and that
 * `width` under 1.0 means a fraction of the screen. None of that is derivable
 * from the parsed struct: `ConfigWinRule` is int32_t after int32_t, and the two
 * facts that distinguish them -- tri-state versus boolean, regex versus literal
 * -- are conventions held in the reader's head.
 *
 * It also replaces kdl_rule_map[], which carried pretty KDL names for eleven of
 * the fifty-three and fell through to the bare key for the rest. Two tables that
 * had to agree, one of them 80% empty. Now the nice name lives beside the key it
 * maps to and "the schema and the KDL spelling disagree" is not a representable
 * state -- the same move that folded kdl_key_map[] into config_schema[].
 *
 * Both spellings keep working. `nice` is canonical for writing and for the docs;
 * the underscore key is still accepted, because kdl_rule_key falls through
 * unchanged when nothing matches and years of configs are written that way.
 *
 * Checked from both ends, like the option schema:
 *   - `asteroidz -R` drives the REAL windowrule parser: every field is written
 *     through parse_option and read back through its own offset, so a wrong
 *     offset or type is a red test rather than a rule that silently sets its
 *     neighbour.
 *   - tests/check-rule-schema.py covers what a dynamic check cannot: a key
 *     `parse_option` handles that is MISSING here.
 *
 * Writing it turned up drift that had been there a while: six fields the parser
 * accepts were in no documentation at all (force_hdr, isnotitlebar, nofocus,
 * noscanout, privacy_shield, vrr_only_fullscreen), and `single_scratchpad`
 * was documented as a window rule the parser has never accepted.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
	/* A POSIX regex tested against a client property. The distinction from
	 * RULE_STRING is the whole reason a UI can be correct here: a plain text
	 * field for these produces rules that never match anything, because a `.`
	 * and a `+` in an app id are wildcards and the user meant them literally. */
	RULE_MATCH,
	/* int32_t with THREE states: -1 unset (inherit the global setting), 0 off,
	 * 1 on. A checkbox cannot express it, and one drawn against these turns
	 * every unset field into an explicit "off" the moment the rule is saved. */
	RULE_TRISTATE,
	RULE_INT,
	RULE_FLOAT,
	RULE_STRING, /* free text: a monitor spec, a workspace name           */
	RULE_ENUM,
	/* uint32_t bitmask, written as the tag NUMBER. `tags 4` parses to
	 * 1 << 3, so formatting it back means finding the set bit. */
	RULE_TAG,
	/* A chord, "mod-key". Held as a resolved KeyBinding, which cannot be
	 * formatted back -- see the note on BindSource. Served from the source
	 * record instead, and read-only until it is. */
	RULE_BIND,
} RuleType;

#define RULE_NOCLAMP NAN

typedef struct {
	const char *name;
	const char *desc;
} RuleEnumMember;

typedef struct {
	/* What the windowrule= parser matches. Also the legacy KDL spelling. */
	const char *key;
	/* The canonical KDL spelling. Never NULL: where they are the same string
	 * it repeats it, so a writer never has to decide which field to read. */
	const char *nice;
	const char *group;
	const char *label;
	const char *desc;
	RuleType type;
	size_t offset; /* offsetof(ConfigWinRule, ...) */
	double min, max;
	const RuleEnumMember *members;
	size_t n_members;
} RuleField;

typedef struct {
	const char *name;
	const char *label;
	const char *desc;
} RuleGroup;

static const RuleGroup rule_groups[] = {
	{"match", "Match",
	 "Which windows the rule applies to. Every field here is a regex, and a "
	 "rule with none of them set matches everything."},
	{"state", "State",
	 "How the window opens and how it answers the requests it makes."},
	{"geometry", "Geometry", "Where it goes and how big it is."},
	{"visuals", "Appearance",
	 "Per-window overrides of the border, shadow, blur, opacity and "
	 "luminance."},
	{"animation", "Animation", "How this window opens and closes."},
	{"layout", "Layout", "Scroller proportions for this window."},
	{"swallow", "Swallowing",
	 "A terminal that is replaced by the GUI application it launches."},
	{"special", "Special windows",
	 "Scratchpads, global windows, and windows that are not managed at all."},
	{"performance", "Performance",
	 "Tearing, scan-out and variable refresh for this window."},
};

/* M13. Spellings are az_present_class_name()'s, and must stay in step with it
 * for the same reason rule_luminance_domain does. */
static const RuleEnumMember rule_presentation_class[] = {
	{"desktop-ui", "Smooth, predictable pacing. The default."},
	{"game", "Lowest latency: tearing if asked, VRR when fullscreen."},
	/* Not "cadence fidelity": that is what the class is for and not what it
	 * does yet. See az_present_intent.h. */
	{"video", "Never tears."},
};

/* M12. Spellings are az_lum_class_name()'s, and must stay in step with it:
 * that function is the parser the compositor actually uses, and this table is
 * only what a config UI offers. A member here that it cannot parse would be a
 * pickable option that silently does nothing. */
static const RuleEnumMember rule_luminance_domain[] = {
	{"sdr-ui", "Desktop furniture: hold white at 203 cd/m2."},
	{"sdr-normal", "Ordinary SDR content at the desktop reference."},
	{"sdr-extended", "Wide-gamut SDR, e.g. photography."},
	{"hdr-content", "HDR video, games and stills: absolute luminance."},
};

static const RuleEnumMember rule_anim_open[] = {
	{"zoom", "Grow from the centre."},
	{"slide", "Slide in from an edge."},
	{"fade", "Fade in on the spot."},
	{"none", "Appear immediately."},
};

static const RuleEnumMember rule_anim_close[] = {
	{"asteroid", "Break apart and fly off."},
	{"fall", "Break into a grid and drop."},
	{"zoom", "Shrink to the centre."},
	{"slide", "Slide out to an edge."},
	{"fade", "Fade out on the spot."},
	{"none", "Disappear immediately."},
};

/* clang-format off */
static const RuleField rule_schema[] = {

/* ===== match =====
 *
 * All three are regexes, tested with the POSIX matcher. `.` and `+` are
 * wildcards, so an app id containing either matches more than it looks like it
 * does -- `org.gnome.Nautilus` matches `orgXgnomeXNautilus` too. Harmless in
 * practice and worth knowing before writing `^…$`. */
{"appid", "app-id", "match", "Application ID",
 "Regex against the window's app id -- `kitty`, `org.mozilla.firefox`. The most "
 "reliable matcher: a title changes as the application runs, an app id does not.",
 RULE_MATCH, offsetof(ConfigWinRule, id), RULE_NOCLAMP, RULE_NOCLAMP, NULL, 0},
{"title", "title", "match", "Title",
 "Regex against the window title. Titles change while an application runs, so a "
 "rule keyed on one can start or stop applying at any moment.",
 RULE_MATCH, offsetof(ConfigWinRule, title), RULE_NOCLAMP, RULE_NOCLAMP, NULL, 0},
{"toplevel_tag", "toplevel-tag", "match", "Toplevel tag",
 "Regex against the tag a client set for itself through xdg-toplevel-tag-v1. "
 "Few applications set one; those that do use it to distinguish their own "
 "windows from each other.",
 RULE_MATCH, offsetof(ConfigWinRule, toplevel_tag), RULE_NOCLAMP, RULE_NOCLAMP,
 NULL, 0},

/* ===== state ===== */
{"isfloating", "open-floating", "state", "Open floating",
 "Open the window floating rather than tiled.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isfloating), 0, 1, NULL, 0},
{"isfullscreen", "open-fullscreen", "state", "Open fullscreen",
 "Open the window fullscreen.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isfullscreen), 0, 1, NULL, 0},
{"isfakefullscreen", "open-fake-fullscreen", "state", "Open fake fullscreen",
 "Tell the window it is fullscreen while keeping it inside its tile. Games and "
 "video players change their rendering path when told; this gets that without "
 "giving up the layout.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isfakefullscreen), 0, 1, NULL, 0},
{"ispinned", "pinned", "state", "Pinned",
 "Forced floating, kept above everything, and visible on every tag of its "
 "monitor.",
 RULE_TRISTATE, offsetof(ConfigWinRule, ispinned), 0, 1, NULL, 0},
{"isoverlay", "overlay", "state", "Overlay",
 "Keep the window in the top layer, above ordinary windows.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isoverlay), 0, 1, NULL, 0},
{"isopensilent", "open-silent", "state", "Open without focus",
 "Map the window without giving it focus.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isopensilent), 0, 1, NULL, 0},
{"istagsilent", "tag-silent", "state", "Do not steal the view",
 "Do not switch to the window's tag when it opens on one you are not looking at.",
 RULE_TRISTATE, offsetof(ConfigWinRule, istagsilent), 0, 1, NULL, 0},
{"nofocus", "no-focus", "state", "Never focus",
 "Refuse focus entirely. For overlays and pets that should never take the "
 "keyboard -- distinct from `open-silent`, which only declines focus at map.",
 RULE_TRISTATE, offsetof(ConfigWinRule, nofocus), 0, 1, NULL, 0},
{"force_tiled_state", "force-tiled-state", "state", "Claim to be tiled",
 "Tell the window it is tiled even when it is not, so it accepts the size it is "
 "given instead of insisting on its own.",
 RULE_TRISTATE, offsetof(ConfigWinRule, force_tiled_state), 0, 1, NULL, 0},
{"force_fakemaximize", "force-fake-maximize", "state", "Fake maximize",
 "Report the window as maximized without actually maximizing it.",
 RULE_TRISTATE, offsetof(ConfigWinRule, force_fakemaximize), 0, 1, NULL, 0},
{"ignore_maximize", "ignore-maximize", "state", "Ignore maximize requests",
 "Drop the window's own requests to maximize itself.",
 RULE_TRISTATE, offsetof(ConfigWinRule, ignore_maximize), 0, 1, NULL, 0},
{"ignore_minimize", "ignore-minimize", "state", "Ignore minimize requests",
 "Drop the window's own requests to minimize itself. Useful for tray-minimizing "
 "applications that vanish where you cannot get them back.",
 RULE_TRISTATE, offsetof(ConfigWinRule, ignore_minimize), 0, 1, NULL, 0},
{"allow_shortcuts_inhibit", "allow-shortcuts-inhibit", "state",
 "Allow shortcut inhibiting",
 "Let this window take the compositor's keybindings for itself -- what a nested "
 "compositor or a remote-desktop client asks for.",
 RULE_TRISTATE, offsetof(ConfigWinRule, allow_shortcuts_inhibit), 0, 1, NULL, 0},
{"idleinhibit_when_focus", "idle-inhibit-when-focused", "state",
 "Inhibit idle while focused",
 "Keep the session awake whenever this window has focus, whether or not it asks.",
 RULE_TRISTATE, offsetof(ConfigWinRule, idleinhibit_when_focus), 0, 1, NULL, 0},
{"isnosizehint", "no-size-hints", "state", "Ignore size hints",
 "Ignore the window's minimum and maximum size. Some toolkits report hints that "
 "make a window untileable.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isnosizehint), 0, 1, NULL, 0},
{"isglobal", "global", "state", "Global",
 "Show the window on every tag.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isglobal), 0, 1, NULL, 0},

/* ===== geometry ===== */
{"tags", "tags", "geometry", "Tag",
 "Open the window on this tag, 1 to 9.",
 RULE_TAG, offsetof(ConfigWinRule, tags), 1, 9, NULL, 0},
{"monitor", "monitor", "geometry", "Monitor",
 "Open the window on this output. Takes a monitor spec: a name, make, model or "
 "serial.",
 RULE_STRING, offsetof(ConfigWinRule, monitor), RULE_NOCLAMP, RULE_NOCLAMP,
 NULL, 0},
{"width", "width", "geometry", "Width",
 "Floating width. Below 1 it is a fraction of the screen; 1 and above it is "
 "pixels.",
 RULE_FLOAT, offsetof(ConfigWinRule, width), 0, 9999, NULL, 0},
{"height", "height", "geometry", "Height",
 "Floating height. Below 1 it is a fraction of the screen; 1 and above it is "
 "pixels.",
 RULE_FLOAT, offsetof(ConfigWinRule, height), 0, 9999, NULL, 0},
{"offsetx", "offset-x", "geometry", "X offset",
 "Horizontal offset from centre, as a percentage. 100 is the screen edge inside "
 "the outer gap.",
 RULE_INT, offsetof(ConfigWinRule, offsetx), -999, 999, NULL, 0},
{"offsety", "offset-y", "geometry", "Y offset",
 "Vertical offset from centre, as a percentage. 100 is the screen edge inside "
 "the outer gap.",
 RULE_INT, offsetof(ConfigWinRule, offsety), -999, 999, NULL, 0},
{"no_force_center", "no-force-center", "geometry", "Do not centre",
 "Leave the window where it asked to be instead of centring it when it floats.",
 RULE_TRISTATE, offsetof(ConfigWinRule, no_force_center), 0, 1, NULL, 0},

/* ===== visuals ===== */
{"isnoborder", "no-border", "visuals", "No border",
 "Draw no border around this window.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isnoborder), 0, 1, NULL, 0},
{"isnoradius", "no-rounding", "visuals", "Square corners",
 "Do not round this window's corners.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isnoradius), 0, 1, NULL, 0},
{"isnoshadow", "no-shadow", "visuals", "No shadow",
 "Draw no shadow for this window.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isnoshadow), 0, 1, NULL, 0},
{"isnotitlebar", "no-titlebar", "visuals", "No titlebar",
 "Draw no titlebar, whatever the global titlebar setting is.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isnotitlebar), 0, 1, NULL, 0},
{"noblur", "no-blur", "visuals", "No blur",
 "Do not blur what is behind this window, even where it is transparent.",
 RULE_TRISTATE, offsetof(ConfigWinRule, noblur), 0, 1, NULL, 0},
{"isnoanimation", "no-animation", "visuals", "No animation",
 "Open and close this window without animating either.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isnoanimation), 0, 1, NULL, 0},
{"focused_opacity", "focused-opacity", "visuals", "Focused opacity",
 "Opacity while focused, 0 to 1. 0 leaves the global setting alone.",
 RULE_FLOAT, offsetof(ConfigWinRule, focused_opacity), 0, 1, NULL, 0},
{"unfocused_opacity", "unfocused-opacity", "visuals", "Unfocused opacity",
 "Opacity while unfocused, 0 to 1. 0 leaves the global setting alone.",
 RULE_FLOAT, offsetof(ConfigWinRule, unfocused_opacity), 0, 1, NULL, 0},
/* The two per-window LUMINANCE levers (M5, ADR-006). They are not opacity and
 * not a global brightness slider -- there deliberately is no global one. Each
 * multiplies one class of source on its way into the scene, so raising one
 * window's SDR white touches that window and nothing else on the screen,
 * including the same window's own shadow and the wallpaper behind it. */
{"presentation_class", "presentation-class", "performance",
 "Presentation class",
 "What this window's frames are FOR, which decides WHEN they appear -- never "
 "what they look like. 'game' takes lowest latency: tearing if the client "
 "asks, VRR when fullscreen. 'video' never tears. "
 "'desktop-ui' is the default: smooth, predictable pacing. Unset derives it "
 "from wp-content-type, which is the client saying what it is. Colour "
 "management, HDR and synchronisation are never traded away by any of them.",
 RULE_ENUM, offsetof(ConfigWinRule, presentation_class), RULE_NOCLAMP,
 RULE_NOCLAMP, rule_presentation_class, LENGTH(rule_presentation_class)},
{"luminance_domain", "luminance-domain", "visuals", "Luminance domain",
 "What this window's content is FOR, which is a different question from how "
 "it is encoded: a terminal and a film are both sRGB and should not share a "
 "white on an HDR display. Unset derives it from what the client declared -- "
 "HDR content is recognised automatically, and so is wide-gamut SDR. "
 "'sdr-ui' is the one that cannot be derived and must be asked for: it holds "
 "the window's white at 203 cd/m2 however bright the desktop reference is.",
 RULE_ENUM, offsetof(ConfigWinRule, luminance_domain), RULE_NOCLAMP,
 RULE_NOCLAMP, rule_luminance_domain, LENGTH(rule_luminance_domain)},
{"sdr_white_scale", "sdr-white-scale", "visuals", "SDR white scale",
 "Multiply this window's SDR white. 1.0 is unchanged; 1.5 makes an SDR "
 "application 50% brighter on an HDR output without touching anything else. "
 "0 leaves it alone. Has no effect on HDR (PQ or scRGB) content.",
 RULE_FLOAT, offsetof(ConfigWinRule, sdr_white_scale), 0, 10, NULL, 0},
{"hdr_gain", "hdr-gain", "visuals", "HDR gain",
 "Multiply this window's HDR content. 1.0 is unchanged; 0.5 halves the "
 "absolute luminance of a PQ video. 0 leaves it alone. Has no effect on SDR "
 "content.",
 RULE_FLOAT, offsetof(ConfigWinRule, hdr_gain), 0, 10, NULL, 0},
{"allow_csd", "allow-csd", "visuals", "Allow client decorations",
 "Let the window draw its own titlebar instead of being given one.",
 RULE_TRISTATE, offsetof(ConfigWinRule, allow_csd), 0, 1, NULL, 0},
{"force_ssd", "force-ssd", "visuals", "Force server decorations",
 "Draw a titlebar and border for a window that supports neither xdg-decoration "
 "nor its own -- SDL and GLFW games, mostly.",
 RULE_TRISTATE, offsetof(ConfigWinRule, force_ssd), 0, 1, NULL, 0},

/* ===== animation ===== */
{"animation_type_open", "animation/open", "animation", "Open animation",
 "How this window appears, overriding the global open animation.",
 RULE_ENUM, offsetof(ConfigWinRule, animation_type_open), RULE_NOCLAMP,
 RULE_NOCLAMP, rule_anim_open, LENGTH(rule_anim_open)},
{"animation_type_close", "animation/close", "animation", "Close animation",
 "How this window disappears, overriding the global close animation.",
 RULE_ENUM, offsetof(ConfigWinRule, animation_type_close), RULE_NOCLAMP,
 RULE_NOCLAMP, rule_anim_close, LENGTH(rule_anim_close)},
{"nofadein", "no-fade-in", "animation", "No fade in",
 "Skip the fade on open while keeping the movement.",
 RULE_TRISTATE, offsetof(ConfigWinRule, nofadein), 0, 1, NULL, 0},
{"nofadeout", "no-fade-out", "animation", "No fade out",
 "Skip the fade on close while keeping the movement.",
 RULE_TRISTATE, offsetof(ConfigWinRule, nofadeout), 0, 1, NULL, 0},

/* ===== layout ===== */
{"scroller_proportion", "scroller/proportion", "layout", "Scroller proportion",
 "Fraction of the screen this window takes in the scroller layout.",
 RULE_FLOAT, offsetof(ConfigWinRule, scroller_proportion), 0, 1, NULL, 0},
{"scroller_proportion_single", "scroller/proportion-single", "layout",
 "Scroller proportion when alone",
 "Fraction of the screen this window takes when it is the only one on the tag.",
 RULE_FLOAT, offsetof(ConfigWinRule, scroller_proportion_single), 0, 1, NULL, 0},

/* ===== swallow ===== */
{"isterm", "is-terminal", "swallow", "Is a terminal",
 "Mark this window as a terminal, so a GUI application launched from it "
 "replaces it until that application exits.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isterm), 0, 1, NULL, 0},
{"noswallow", "no-swallow", "swallow", "Never swallow a terminal",
 "This window does not replace the terminal that launched it.",
 RULE_TRISTATE, offsetof(ConfigWinRule, noswallow), 0, 1, NULL, 0},

/* ===== special ===== */
{"isnamedscratchpad", "named-scratchpad", "special", "Named scratchpad",
 "Send the window to a named scratchpad rather than leaving it on a tag.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isnamedscratchpad), 0, 1, NULL, 0},
{"special_workspace", "special-workspace", "special", "Special workspace",
 "Put the window on this named special workspace when it opens.",
 RULE_STRING, offsetof(ConfigWinRule, special_workspace), RULE_NOCLAMP,
 RULE_NOCLAMP, NULL, 0},
{"isunglobal", "unmanaged", "special", "Unmanaged",
 "Do not manage the window at all: no tiling, no focus, no tag. For desktop "
 "pets and camera overlays.",
 RULE_TRISTATE, offsetof(ConfigWinRule, isunglobal), 0, 1, NULL, 0},
{"globalkeybinding", "global-keybinding", "special", "Global keybinding",
 "A chord that raises and focuses this window from anywhere, written "
 "`Super-n`. Wayland clients only.",
 RULE_BIND, offsetof(ConfigWinRule, globalkeybinding), RULE_NOCLAMP,
 RULE_NOCLAMP, NULL, 0},

/* ===== performance ===== */
{"force_tearing", "force-tearing", "performance", "Allow tearing",
 "Let this window's frames reach the screen without waiting for vblank. Lower "
 "latency, visible tearing.",
 RULE_TRISTATE, offsetof(ConfigWinRule, force_tearing), 0, 1, NULL, 0},
{"xwayland_scale_one", "xwayland-scale-one", "visuals",
 "Native-resolution XWayland",
 "Override misc/xwayland-force-scale-one for this window. On a fractionally "
 "scaled output it renders an X11 window at device resolution instead of "
 "magnifying a smaller buffer. THE TRADE: Xwayland sizes its X screen from the "
 "outputs' logical geometry, so a pixel-sized window overflows it and X11 "
 "clamps the pointer to the screen edge -- on a 1.5x output every click below "
 "roughly the bottom third of the window lands short. Worth it for a "
 "fullscreen game, which grabs the pointer and uses relative motion; not worth "
 "it for a window whose controls are along the bottom. Set 0 to opt a window "
 "out.",
 RULE_TRISTATE, offsetof(ConfigWinRule, xwayland_scale_one), 0, 1, NULL, 0},
{"noscanout", "no-scanout", "performance", "No direct scan-out",
 "Keep this window out of direct scan-out and push it through the render pass. "
 "For clients whose buffers are not safe to hand straight to a KMS plane -- "
 "gamescope without explicit sync tears RGB noise across the screen otherwise.",
 RULE_TRISTATE, offsetof(ConfigWinRule, noscanout), 0, 1, NULL, 0},
{"vrr_only_fullscreen", "vrr-only-fullscreen", "performance",
 "Variable refresh only when fullscreen",
 "Turn variable refresh on while this window is fullscreen and off again "
 "afterwards, rather than leaving it on for the whole output.",
 RULE_TRISTATE, offsetof(ConfigWinRule, vrr_only_fullscreen), 0, 1, NULL, 0},
{"force_hdr", "force-hdr", "performance", "Force HDR",
 "Switch the output to HDR while this window is on it. The way to run HDR for "
 "one player without an HDR desktop.",
 RULE_TRISTATE, offsetof(ConfigWinRule, force_hdr), 0, 1, NULL, 0},
{"privacy_shield", "privacy-shield", "performance",
 "Hide from captures",
 "Cover this window with an opaque shield while a screen capture is running, so "
 "it does not appear in recordings or shares.",
 RULE_TRISTATE, offsetof(ConfigWinRule, privacy_shield), 0, 1, NULL, 0},
};
/* clang-format on */

#define RULE_SCHEMA_COUNT (sizeof(rule_schema) / sizeof(rule_schema[0]))

static const RuleField *rule_field_by_key(const char *key) {
	if (!key)
		return NULL;
	for (size_t i = 0; i < RULE_SCHEMA_COUNT; i++)
		if (!strcmp(rule_schema[i].key, key))
			return &rule_schema[i];
	return NULL;
}

/* The KDL spelling -> the key the windowrule= parser matches.
 *
 * Returns `nice` unchanged when nothing matches, which is what keeps every
 * underscore spelling working: `force_hdr` is not in the nice column, falls
 * through, and lands on the parser branch of the same name. */
static const char *rule_field_key_for_nice(const char *nice) {
	if (!nice)
		return NULL;
	for (size_t i = 0; i < RULE_SCHEMA_COUNT; i++)
		if (!strcmp(rule_schema[i].nice, nice))
			return rule_schema[i].key;
	return nice;
}

static const char *rule_type_name(RuleType t) {
	switch (t) {
	case RULE_MATCH:
		return "match";
	case RULE_TRISTATE:
		return "tristate";
	case RULE_INT:
		return "int";
	case RULE_FLOAT:
		return "float";
	case RULE_STRING:
		return "string";
	case RULE_ENUM:
		return "enum";
	case RULE_TAG:
		return "tag";
	case RULE_BIND:
		return "bind";
	}
	return "?";
}

static void *rule_field_ptr(const ConfigWinRule *r, const RuleField *f) {
	return (void *)((const char *)r + f->offset);
}

/* One field of one rule, as the string a user would write.
 *
 * Returns false when the field is UNSET, which is not the same as empty: an
 * unset tri-state is -1 and an unset string is NULL, and both mean "this rule
 * says nothing about that". A UI that cannot tell them apart writes every field
 * of every rule back out.
 */
static bool rule_format(const ConfigWinRule *r, const RuleField *f, char *out,
						size_t cap) {
	const void *p = rule_field_ptr(r, f);
	switch (f->type) {
	case RULE_MATCH:
	case RULE_STRING:
	case RULE_ENUM: {
		const char *s = *(const char *const *)p;
		if (!s)
			return false;
		snprintf(out, cap, "%s", s);
		return true;
	}
	case RULE_TRISTATE: {
		int32_t v = *(const int32_t *)p;
		if (v < 0)
			return false;
		snprintf(out, cap, "%d", v);
		return true;
	}
	case RULE_INT: {
		int32_t v = *(const int32_t *)p;
		if (v == 0)
			return false; /* the unset value for every int rule here */
		snprintf(out, cap, "%d", v);
		return true;
	}
	case RULE_FLOAT: {
		float v = *(const float *)p;
		if (v == 0.0f)
			return false;
		snprintf(out, cap, "%g", (double)v);
		return true;
	}
	case RULE_TAG: {
		uint32_t mask = *(const uint32_t *)p;
		if (!mask)
			return false;
		/* Written as the tag NUMBER, which is what `tags 4` means -- the field
		 * holds 1 << 3. Reporting the mask would be a rule editor that offers
		 * tag 8 for a rule that says 4. */
		for (int32_t i = 0; i < 32; i++)
			if (mask & (1u << i)) {
				snprintf(out, cap, "%d", i + 1);
				return true;
			}
		return false;
	}
	case RULE_BIND:
		/* Resolved to a mod mask and a keysym, and formatting that back is the
		 * lossy direction -- see BindSource. Served from the source record. */
		return false;
	}
	return false;
}

#endif /* ASTEROIDZ_RULE_SCHEMA_H */
