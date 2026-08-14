#ifndef ASTEROIDZ_CONFIG_SCHEMA_H
#define ASTEROIDZ_CONFIG_SCHEMA_H

/* A machine-readable description of every settable config option.
 *
 * There wasn't one. Types, ranges, enum members and defaults live inline in
 * parse_option()'s 2000-line if/else chain and in set_value_default()'s 440
 * lines of assignments, and the nested KDL spellings live in a third table. A
 * settings UI needs all three at once, so without this it would have to carry a
 * hardcoded mirror of the compositor's own parser -- which drifts the first time
 * either side gains a default, silently, and then writes wrong values into a
 * hand-maintained config.
 *
 * HAND-WRITTEN, and checked mechanically rather than generated. A wrong
 * generator produces a wrong schema silently; a wrong checker produces a red
 * test. Generation also cannot produce the two fields that make the table worth
 * having -- the human description and the grouping -- and it mishandles every
 * irregular branch (srgb_blending accepts both "srgb" and "encoded";
 * animation_curve_type writes a field called animation_curve_spring).
 *
 * What keeps it honest, in both directions:
 *   - `asteroidz -S` (config-schema-check.h) drives the REAL parse_option,
 *     set_value_default and override_config and asserts every default, clamp,
 *     offset and type against them. No C is parsed to do it.
 *   - tests/check-config-schema.py covers the one direction a dynamic check
 *     cannot: keys that exist in parse_option and are MISSING here.
 *
 * `offset` is what makes the whole thing affordable. With offset plus type, the
 * IPC layer reads every current value generically instead of needing a second
 * hand-written mirror of set_value_default.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
	OPT_BOOL,   /* int32_t, 0/1                                              */
	OPT_INT,    /* int32_t                                                   */
	OPT_FLOAT,  /* float                                                     */
	OPT_DOUBLE, /* double                                                    */
	OPT_COLOR,  /* float[4] in the struct, 0xRRGGBBAA on the wire            */
	OPT_STRING, /* char[N] in-struct; `size` is the cap parse_option
				 * truncates to. The UI needs that number or it offers a text
				 * box that silently loses the tail -- animation_type_open is
				 * written with "%.9s"                                       */
	OPT_STRPTR, /* char *, strdup'd                                          */
	OPT_ENUM,   /* int32_t plus members[]                                     */
	OPT_BEZIER, /* double[4]                                                 */
} OptType;

typedef struct {
	const char *name; /* what the user writes: "spring", "srgb"              */
	int32_t value;    /* what the field holds                                */
	bool alias;       /* accepted on input, never emitted on output           */
	const char *desc;
} OptEnumMember;

/* Flags. Presentation and policy hints for a UI; none of them change how the
 * option is parsed. */
enum {
	/* Takes effect only on the next reload, or not until a restart. */
	SCHEMA_NO_LIVE = 1u << 0,
	SCHEMA_NEEDS_RESTART = 1u << 1,
	/* Normally written by matugen's generated palette. A UI hint only -- the
	 * hard guard against clobbering a generated file is per-FILE, because it
	 * has to work for files that do not exist yet. */
	SCHEMA_MATUGEN = 1u << 2,
	/* Rarely wanted; a UI should keep it behind "advanced". */
	SCHEMA_ADVANCED = 1u << 3,
};

typedef struct {
	const char *key;  /* the internal name parse_option matches              */
	const char *path; /* canonical nested KDL path, or NULL                  */
	const char *group;
	const char *subgroup;
	const char *label;
	const char *desc;
	OptType type;
	size_t offset;
	size_t size;      /* sizeof the field for OPT_STRING; else 0             */
	double min, max;  /* the clamp that is actually applied; NAN when none.
					   * Note clamps live in BOTH parse_option and
					   * override_config -- blur_transparency_threshold is
					   * clamped in the former, borderpx in the latter -- so
					   * the self-check derives them from BEHAVIOUR rather
					   * than from where the code happens to put them.       */
	const OptEnumMember *members;
	size_t n_members;
	/* THE DEFAULT, as the value a user would write -- unquoted, so
	 * `monospace Bold 16` not `"monospace Bold 16"`. One string serves three
	 * consumers: the docs table cell, the UI's Reset, and the self-check's
	 * probe value. */
	const char *def;
	uint32_t flags;
} ConfigOption;

#define SCHEMA_NOCLAMP NAN

/* ---------- enum member tables ---------- */

static const OptEnumMember schema_blend_space[] = {
	{"linear", 0, false, "Physically correct. Required for HDR."},
	{"srgb", 1, false, "Blend encoded values, as most compositors do."},
	{"encoded", 1, true, NULL},
};

/* Stored as a STRING, not an index, unlike the two tables below it.
 *
 * OPT_ENUM writes an int32_t into the struct, and these fields are char[10] --
 * so the type stays OPT_STRING and the closed set is carried in `members`
 * alone. Everything downstream keys off "does this option have members", not
 * off the type, which is what lets a fixed set of names be a dropdown without
 * inventing a second enum type for the one case where the name IS the value.
 *
 * `asteroid` and `fall` are close-only: they break the window into pieces, and
 * there is nothing to break on the way in. They are NOT the same effect and
 * neither is an alias of the other -- `fall` was the old name for `asteroid`
 * and now selects the tile scatter it always actually was, so both are real
 * members and both belong in the list. */
static const OptEnumMember schema_anim_type_open[] = {
	{"none", 0, false, "Appear instantly."},
	{"zoom", 0, false, "Scale up from the centre."},
	{"slide", 0, false, "Slide in from the nearest edge."},
	{"fade", 0, false, "Fade in."},
};

static const OptEnumMember schema_anim_type_close[] = {
	{"none", 0, false, "Disappear instantly."},
	{"zoom", 0, false, "Scale down to the centre."},
	{"slide", 0, false, "Slide out to the nearest edge."},
	{"fade", 0, false, "Fade out."},
	{"asteroid", 0, false, "Break apart and fly off."},
	{"fall", 0, false, "Break into a grid of tiles and scatter."},
};

static const OptEnumMember schema_curve_type[] = {
	{"bezier", 0, false, "A cubic bezier, from the curve control points."},
	{"spring", 1, false, "A damped spring, from damping and frequency."},
};

/* ---------- groups, for presentation ---------- */

typedef struct {
	const char *name;
	const char *label;
	const char *desc;
} ConfigGroup;

static const ConfigGroup config_groups[] = {
	{"appearance", "Appearance",
	 "Borders, titlebars, and the shared style every native overlay uses."},
	{"effects", "Effects", "Blur, shadows, and the blending space."},
	{"layout", "Layout", "Gaps and how windows are placed."},
	{"animations", "Animations", "How windows move, open and close."},
	{"overview", "Overview", "The zoomed-out view of every tag."},
	{"input", "Input", "Cursor, keyboard and pointer behaviour."},
	{"misc", "Miscellaneous", "System integration and everything else."},
};

/* ---------- the table ---------- */

/* Deliberately grouped the way a settings page is, not the way parse_option is.
 * The KDL path is where the value LIVES; group/subgroup is where a person would
 * look for it, and those are not the same question -- `borderpx` lives at
 * layout/border/width and belongs on an Appearance page beside the colours it
 * is drawn with. */
static const ConfigOption config_schema[] = {

	/* ===== appearance / border ===== */
	{"borderpx", "layout/border/width", "appearance", "border", "Width",
	 "Border thickness in pixels. 0 removes the border entirely.", OPT_INT,
	 offsetof(Config, borderpx), 0, 0, 32, NULL, 0, "4", 0},
	{"border_radius", "border_radius", "appearance", "border", "Corner radius",
	 "Rounds window corners. 0 is square.", OPT_INT,
	 offsetof(Config, border_radius), 0, 0, 64, NULL, 0, "0", 0},
	{"bordercolor", "layout/border/color", "appearance", "border",
	 "Resting colour",
	 "Border colour of an unfocused window. Normally set by the generated "
	 "palette.",
	 OPT_COLOR, offsetof(Config, bordercolor), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0x444444ff", SCHEMA_MATUGEN},
	{"focuscolor", "layout/border/focus-color", "appearance", "border",
	 "Focus colour",
	 "Border colour of the focused window. Normally set by the generated "
	 "palette.",
	 OPT_COLOR, offsetof(Config, focuscolor), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0xc66b25ff", SCHEMA_MATUGEN},
	{"urgentcolor", "layout/border/urgent-color", "appearance", "border",
	 "Urgent colour",
	 "Border colour of a window demanding attention. Normally set by the "
	 "generated palette.",
	 OPT_COLOR, offsetof(Config, urgentcolor), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0xad401fff", SCHEMA_MATUGEN},
	{"maximizescreencolor", "layout/border/maximize-color", "appearance",
	 "border", "Maximized colour",
	 "Border colour while the focused window is maximized. Defaults to the "
	 "focus colour, so a themed border does not change hue on maximize.",
	 OPT_COLOR, offsetof(Config, maximizescreencolor), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0x00000000", 0},
	{"scratchpadcolor", "layout/border/scratchpad-color", "appearance",
	 "border", "Scratchpad colour",
	 "Border colour while the focused window is a scratchpad. Defaults to the "
	 "focus colour.",
	 OPT_COLOR, offsetof(Config, scratchpadcolor), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0x00000000", 0},
	{"globalcolor", "layout/border/global-color", "appearance", "border",
	 "Global colour",
	 "Border colour while the focused window is global (shown on every tag). "
	 "Defaults to the focus colour.",
	 OPT_COLOR, offsetof(Config, globalcolor), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0x00000000", 0},
	{"overlaycolor", "layout/border/overlay-color", "appearance", "border",
	 "Overlay colour",
	 "Border colour while the focused window is an overlay. Defaults to the "
	 "focus colour.",
	 OPT_COLOR, offsetof(Config, overlaycolor), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0x00000000", 0},
	{"no_border_when_single", "no_border_when_single", "appearance", "border",
	 "Hide when alone",
	 "Drop the border when a tag holds only one window -- there is nothing to "
	 "distinguish it from.",
	 OPT_BOOL, offsetof(Config, no_border_when_single), 0, 0, 1, NULL, 0, "0",
	 0},
	{"no_radius_when_single", "no_radius_when_single", "appearance", "border",
	 "Square when alone",
	 "Drop the corner rounding when a tag holds only one window.", OPT_BOOL,
	 offsetof(Config, no_radius_when_single), 0, 0, 1, NULL, 0, "0", 0},
	{"border_gradient", "layout/border/gradient/enable", "appearance",
	 "border", "Gradient",
	 "Draw the focused border as a two-tone ramp. Only the focused border; an "
	 "unfocused one stays flat, which is what keeps the focus obvious.",
	 OPT_BOOL, offsetof(Config, border_gradient), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0", 0},
	{"border_gradient_angle", "layout/border/gradient/angle", "appearance",
	 "border", "Gradient angle", "Direction of the ramp, in degrees.",
	 OPT_FLOAT, offsetof(Config, border_gradient_angle), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "45", 0},
	{"border_gradient_color2", "layout/border/gradient/color2", "appearance",
	 "border", "Gradient far colour",
	 "The far end of the ramp; the near end is the focus colour, so a gradient "
	 "follows the theme without a second colour to keep in step.",
	 OPT_COLOR, offsetof(Config, border_gradient_color2), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0xffffffff", SCHEMA_MATUGEN},

	/* ===== appearance / titlebar ===== */
	{"enable_titlebar", "layout/titlebar/enable", "appearance", "titlebar",
	 "Titlebars",
	 "A server-side titlebar on tiled windows. It reserves real space -- the "
	 "window does not grow to compensate, so enabling this shrinks the usable "
	 "content area.",
	 OPT_BOOL, offsetof(Config, enable_titlebar), 0, 0, 1, NULL, 0, "0", 0},
	/* There is deliberately no titlebar height here. It is DERIVED in
	 * override_config() from the theme font's line height plus theme/padding/y,
	 * so a settings UI has nothing to offer -- see the comment there. */

	/* ===== appearance / theme =====
	 *
	 * One style shared by every native overlay -- the monocle tab bar, the
	 * titlebars, the overview jump labels, the screenshot size badge. There is
	 * no separate theming for any of them; changing one of these restyles all
	 * of them at once, which is the point. */
	{"theme_font_desc", "theme/font", "appearance", "theme", "Font",
	 "Pango font description for every native overlay, e.g. \"Ubuntu 17\".",
	 OPT_STRPTR, offsetof(Config, theme.font_desc), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "monospace Bold 16", 0},
	{"theme_fg_color", "theme/fg-color", "appearance", "theme", "Text",
	 "Text colour. Normally set by the generated palette.", OPT_COLOR,
	 offsetof(Config, theme.fg_color), 0, SCHEMA_NOCLAMP, SCHEMA_NOCLAMP, NULL,
	 0, "0xc4939dff", SCHEMA_MATUGEN},
	{"theme_bg_color", "theme/bg-color", "appearance", "theme", "Background",
	 "Background colour. Normally set by the generated palette.", OPT_COLOR,
	 offsetof(Config, theme.bg_color), 0, SCHEMA_NOCLAMP, SCHEMA_NOCLAMP, NULL,
	 0, "0x323232ff", SCHEMA_MATUGEN},
	{"theme_focus_fg_color", "theme/focus-fg-color", "appearance", "theme",
	 "Text (focused)",
	 "Text colour on a focused element. Normally set by the generated palette.",
	 OPT_COLOR, offsetof(Config, theme.focus_fg_color), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0xeda6b4ff", SCHEMA_MATUGEN},
	{"theme_focus_bg_color", "theme/focus-bg-color", "appearance", "theme",
	 "Background (focused)",
	 "Background colour of a focused element -- the accent. Normally set by "
	 "the generated palette.",
	 OPT_COLOR, offsetof(Config, theme.focus_bg_color), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0x4e453cff", SCHEMA_MATUGEN},
	{"theme_urgent_color", "theme/urgent-color", "appearance", "theme",
	 "Attention",
	 "Accent for something wanting attention. Normally set by the generated "
	 "palette.",
	 OPT_COLOR, offsetof(Config, theme.urgent_color), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0xffb4abff", SCHEMA_MATUGEN},
	{"theme_border_color", "theme/border-color", "appearance", "theme",
	 "Outline",
	 "Outline colour of a native overlay. Only drawn when the outline width is "
	 "above 0.",
	 OPT_COLOR, offsetof(Config, theme.border_color), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0x8baa9bff", 0},
	{"theme_border_width", "theme/border-width", "appearance", "theme",
	 "Outline width", "Outline thickness of a native overlay. 0 disables it.",
	 OPT_INT, offsetof(Config, theme.border_width), 0, 0, 32, NULL, 0, "4", 0},
	{"theme_corner_radius", "theme/corner-radius", "appearance", "theme",
	 "Corner radius",
	 "Corner rounding of a native overlay. -1 gives a full pill shape.",
	 OPT_INT, offsetof(Config, theme.corner_radius), 0, -1, 64, NULL, 0, "5",
	 0},
	{"theme_padding_x", "theme/padding/x", "appearance", "theme",
	 "Padding (horizontal)", "Horizontal padding inside a native overlay.",
	 OPT_INT, offsetof(Config, theme.padding_x), 0, 0, 64, NULL, 0, "0", 0},
	{"theme_padding_y", "theme/padding/y", "appearance", "theme",
	 "Padding (vertical)", "Vertical padding inside a native overlay.",
	 OPT_INT, offsetof(Config, theme.padding_y), 0, 0, 64, NULL, 0, "0", 0},

	/* ===== effects / blur ===== */
	{"blur", "effects/blur/enable", "effects", "blur", "Blur",
	 "Frost the area behind a transparent window.", OPT_BOOL,
	 offsetof(Config, blur), 0, 0, 1, NULL, 0, "0", 0},
	{"blur_layer", "effects/blur/layer", "effects", "blur", "Blur layers",
	 "Also blur behind layer-shell surfaces -- bars, notifications, launchers.",
	 OPT_BOOL, offsetof(Config, blur_layer), 0, 0, 1, NULL, 0, "0", 0},
	{"blur_optimized", "effects/blur/optimized", "effects", "blur",
	 "Cache the blur",
	 "Blur the wallpaper once and reuse it, rather than per frame. Much "
	 "cheaper; the cache is rebuilt when what is behind it changes.",
	 OPT_BOOL, offsetof(Config, blur_optimized), 0, 0, 1, NULL, 0, "1", 0},
	{"blur_params_num_passes", "effects/blur/passes", "effects", "blur",
	 "Passes",
	 "Dual-Kawase passes. Higher is smoother and more expensive; 1 leaves "
	 "visible bleed-through.",
	 OPT_INT, offsetof(Config, blur_params.num_passes), 0, 0, 10, NULL, 0, "2",
	 0},
	{"blur_params_radius", "effects/blur/radius", "effects", "blur", "Radius",
	 "Blur radius in pixels.", OPT_FLOAT,
	 offsetof(Config, blur_params.radius), 0, 0, 100, NULL, 0, "5", 0},
	{"blur_params_noise", "effects/blur/params/noise", "effects", "blur",
	 "Noise",
	 "Grain mixed into the blur, which hides the banding a large radius "
	 "produces on a gradient.",
	 OPT_FLOAT, offsetof(Config, blur_params.noise), 0, 0, 1, NULL, 0, "0.02",
	 0},
	{"blur_params_brightness", "effects/blur/params/brightness", "effects",
	 "blur", "Brightness", "Brightness of the blurred result.", OPT_FLOAT,
	 offsetof(Config, blur_params.brightness), 0, 0, 1, NULL, 0, "0.9", 0},
	{"blur_params_contrast", "effects/blur/params/contrast", "effects", "blur",
	 "Contrast", "Contrast of the blurred result.", OPT_FLOAT,
	 offsetof(Config, blur_params.contrast), 0, 0, 1, NULL, 0, "0.9", 0},
	{"blur_params_saturation", "effects/blur/params/saturation", "effects",
	 "blur", "Saturation", "Saturation of the blurred result.", OPT_FLOAT,
	 offsetof(Config, blur_params.saturation), 0, 0, 2, NULL, 0, "1.2", 0},
	{"blur_transparency_threshold", "effects/blur/transparency-threshold",
	 "effects", "blur", "Transparency threshold",
	 "How transparent a surface has to be before it is blurred behind. A fully "
	 "opaque window has nothing to show through it, so blurring behind one is "
	 "wasted work.",
	 OPT_FLOAT, offsetof(Config, blur_params.transparency_threshold), 0, 0, 1,
	 NULL, 0, "0", SCHEMA_ADVANCED},
	{"blur_unfocused_strength", "effects/blur/unfocused-strength", "effects",
	 "blur", "Unfocused strength",
	 "Scales the blur on unfocused windows, so focus reads as depth.",
	 OPT_FLOAT, offsetof(Config, blur_unfocused_strength), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "1", 0},

	/* ===== effects / shadow ===== */
	{"shadows", "effects/shadow/enable", "effects", "shadow", "Shadows",
	 "Drop a shadow behind windows.", OPT_BOOL, offsetof(Config, shadows), 0, 0,
	 1, NULL, 0, "0", 0},
	{"shadow_only_floating", "effects/shadow/only-floating", "effects",
	 "shadow", "Floating only",
	 "Shadow floating windows only. Tiled windows share every edge with a "
	 "neighbour, so a shadow there is drawn onto another window.",
	 OPT_BOOL, offsetof(Config, shadow_only_floating), 0, 0, 1, NULL, 0, "1",
	 0},
	{"layer_shadows", "effects/shadow/layer", "effects", "shadow",
	 "Shadow layers", "Also shadow layer-shell surfaces.", OPT_BOOL,
	 offsetof(Config, layer_shadows), 0, 0, 1, NULL, 0, "0", 0},
	{"shadows_size", "effects/shadow/size", "effects", "shadow", "Size",
	 "How far the shadow reaches, in pixels.", OPT_INT,
	 offsetof(Config, shadows_size), 0, 0, 100, NULL, 0, "24", 0},
	{"shadows_blur", "effects/shadow/blur", "effects", "shadow", "Blur",
	 "Softness of the shadow's edge.", OPT_FLOAT,
	 offsetof(Config, shadows_blur), 0, 0, 100, NULL, 0, "24", 0},
	{"shadowscolor", "effects/shadow/color", "effects", "shadow", "Colour",
	 "Shadow colour. The alpha channel is the opacity.", OPT_COLOR,
	 offsetof(Config, shadowscolor), 0, SCHEMA_NOCLAMP, SCHEMA_NOCLAMP, NULL, 0,
	 "0x00000066", 0},
	{"shadows_position_x", "effects/shadow/position/x", "effects", "shadow",
	 "Offset X", "Horizontal shadow offset.", OPT_INT,
	 offsetof(Config, shadows_position_x), 0, -1000, 200, NULL, 0, "0", 0},
	{"shadows_position_y", "effects/shadow/position/y", "effects", "shadow",
	 "Offset Y",
	 "Vertical shadow offset. A positive value casts downwards, which is what "
	 "reads as light from above.",
	 OPT_INT, offsetof(Config, shadows_position_y), 0, -1000, 200, NULL, 0,
	 "10", 0},
	{"shadows_blur_background", "effects/shadow/blur-background", "effects",
	 "shadow", "Blur behind the shadow",
	 "Blur what is under the shadow as well as darkening it.", OPT_BOOL,
	 offsetof(Config, shadows_blur_background), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0", 0},
	{"primary_selection", "misc/primary-selection", "misc", "general",
	 "Primary selection",
	 "Advertise the middle-click \"copy on select\" clipboard. Off leaves one "
	 "clipboard: the global is never bound, so toolkits stop publishing on "
	 "select, and XWayland's X PRIMARY is refused as well.",
	 OPT_BOOL, offsetof(Config, primary_selection), 0, 0, 1, NULL, 0, "1",
	 SCHEMA_NEEDS_RESTART},
	{"shadows_blur_background_darken", "effects/shadow/blur-background-darken",
	 "effects", "shadow", "Blur-behind may only darken",
	 "Clamp the blurred backdrop against the unblurred one, so a shadow can "
	 "never brighten what it covers. A blur is an average, and averaging "
	 "bright detail over a dark ground raises the mean -- without this a "
	 "shadow over a terminal reads as a glow.",
	 OPT_BOOL, offsetof(Config, shadows_blur_background_darken), 0, 0, 1,
	 NULL, 0, "1", 0},
	{"shadows_blur_background_strength",
	 "effects/shadow/blur-background-strength", "effects", "shadow",
	 "Blur-behind strength", "How strongly the area under the shadow blurs.",
	 OPT_FLOAT, offsetof(Config, shadows_blur_background_strength), 0,
	 SCHEMA_NOCLAMP, SCHEMA_NOCLAMP, NULL, 0, "0.5", 0},
	{"shadows_unfocused_scale", "effects/shadow/unfocused-scale", "effects",
	 "shadow", "Unfocused scale",
	 "Scales the shadow on unfocused windows, so the focused one sits above "
	 "them.",
	 OPT_FLOAT, offsetof(Config, shadows_unfocused_scale), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0.45", 0},
	{"shadows_tiled_scale", "effects/shadow/tiled-scale", "effects", "shadow",
	 "Tiled scale",
	 "Scales the shadow on tiled windows, which have less room for one.",
	 OPT_FLOAT, offsetof(Config, shadows_tiled_scale), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0.3", 0},
	{"shadows_contact", "effects/shadow/contact/enable", "effects", "shadow",
	 "Contact shadow",
	 "A second, tighter shadow right at the window's edge -- the dark line "
	 "where an object meets what it rests on.",
	 OPT_BOOL, offsetof(Config, shadows_contact), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "1", 0},
	{"shadows_contact_size", "effects/shadow/contact/size", "effects",
	 "shadow", "Contact size", "Reach of the contact shadow.", OPT_INT,
	 offsetof(Config, shadows_contact_size), 0, SCHEMA_NOCLAMP, SCHEMA_NOCLAMP,
	 NULL, 0, "8", 0},
	{"shadows_contact_blur", "effects/shadow/contact/blur", "effects",
	 "shadow", "Contact blur", "Softness of the contact shadow.", OPT_FLOAT,
	 offsetof(Config, shadows_contact_blur), 0, SCHEMA_NOCLAMP, SCHEMA_NOCLAMP,
	 NULL, 0, "9", 0},
	{"shadowscolor_contact", "effects/shadow/contact/color", "effects",
	 "shadow", "Contact colour", "Colour of the contact shadow.", OPT_COLOR,
	 offsetof(Config, shadowscolor_contact), 0, SCHEMA_NOCLAMP, SCHEMA_NOCLAMP,
	 NULL, 0, "0x0000004d", 0},
	{"shadows_contact_position_x", "effects/shadow/contact/position/x",
	 "effects", "shadow", "Contact offset X",
	 "Horizontal offset of the contact shadow.", OPT_INT,
	 offsetof(Config, shadows_contact_position_x), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "0", 0},
	{"shadows_contact_position_y", "effects/shadow/contact/position/y",
	 "effects", "shadow", "Contact offset Y",
	 "Vertical offset of the contact shadow.", OPT_INT,
	 offsetof(Config, shadows_contact_position_y), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, NULL, 0, "2", 0},

	/* ===== effects / general ===== */
	{"srgb_blending", "effects/blend-space", "effects", "general",
	 "Blend space",
	 "Which space transparency is blended in. Spelled as a space rather than a "
	 "boolean because \"linear\" and \"srgb\" say what it does.",
	 OPT_ENUM, offsetof(Config, srgb_blending), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, schema_blend_space,
	 sizeof(schema_blend_space) / sizeof(schema_blend_space[0]), "linear", 0},
	{"focused_opacity", "focused_opacity", "effects", "general",
	 "Focused opacity", "Opacity of the focused window.", OPT_FLOAT,
	 offsetof(Config, focused_opacity), 0, 0, 1, NULL, 0, "1", 0},
	{"unfocused_opacity", "unfocused_opacity", "effects", "general",
	 "Unfocused opacity", "Opacity of every other window.", OPT_FLOAT,
	 offsetof(Config, unfocused_opacity), 0, 0, 1, NULL, 0, "1", 0},

	/* ===== layout / gaps ===== */
	{"gappih", "gappih", "layout", "gaps", "Inner horizontal",
	 "Horizontal gap between windows.", OPT_INT, offsetof(Config, gappih), 0, 0,
	 200, NULL, 0, "5", 0},
	{"gappiv", "gappiv", "layout", "gaps", "Inner vertical",
	 "Vertical gap between windows.", OPT_INT, offsetof(Config, gappiv), 0, 0,
	 200, NULL, 0, "5", 0},
	{"gappoh", "gappoh", "layout", "gaps", "Outer horizontal",
	 "Horizontal gap between the windows and the screen edge.", OPT_INT,
	 offsetof(Config, gappoh), 0, 0, 200, NULL, 0, "10", 0},
	{"gappov", "gappov", "layout", "gaps", "Outer vertical",
	 "Vertical gap between the windows and the screen edge.", OPT_INT,
	 offsetof(Config, gappov), 0, 0, 200, NULL, 0, "10", 0},
	{"smartgaps", "smartgaps", "layout", "gaps", "Smart gaps",
	 "Drop every gap, inner and outer, when a tag holds only one window.",
	 OPT_BOOL,
	 offsetof(Config, smartgaps), 0, 0, 1, NULL, 0, "0", 0},

	/* ===== animations ===== */
	{"animations", "animations/enable", "animations", "general", "Animations",
	 "Animate window movement, opening and closing.", OPT_BOOL,
	 offsetof(Config, animations), 0, 0, 1, NULL, 0, "1", 0},
	{"layer_animations", "layer_animations", "animations", "general",
	 "Animate layers", "Also animate layer-shell surfaces.", OPT_BOOL,
	 offsetof(Config, layer_animations), 0, 0, 1, NULL, 0, "0", 0},
	{"animation_curve_type", "animations/curve", "animations", "general",
	 "Curve type",
	 "Whether motion follows a bezier or a damped spring. A spring overshoots "
	 "and settles; a bezier cannot.",
	 OPT_ENUM, offsetof(Config, animation_curve_spring), 0, SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, schema_curve_type,
	 sizeof(schema_curve_type) / sizeof(schema_curve_type[0]), "bezier", 0},
	{"spring_damping", "animations/spring/damping", "animations", "spring",
	 "Damping",
	 "How quickly the spring settles. Below 1 it overshoots before coming to "
	 "rest.",
	 OPT_DOUBLE, offsetof(Config, spring_damping), 0, 0.1, 2, NULL, 0, "0.75",
	 0},
	{"spring_frequency", "animations/spring/frequency", "animations", "spring",
	 "Frequency", "How fast the spring moves.", OPT_DOUBLE,
	 offsetof(Config, spring_frequency), 0, 4, 60, NULL, 0, "18", 0},
	{"animation_duration_open", "animations/window-open/duration",
	 "animations", "window-open", "Duration",
	 "How long a window takes to open, in milliseconds.", OPT_INT,
	 offsetof(Config, animation_duration_open), 0, 1, 3000, NULL, 0, "400", 0},
	{"animation_type_open", "animations/window-open/type", "animations",
	 "window-open", "Type",
	 "Which opening animation to use.", OPT_STRING,
	 offsetof(Config, animation_type_open),
	 sizeof(((Config *)0)->animation_type_open), SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, schema_anim_type_open,
	 sizeof(schema_anim_type_open) / sizeof(schema_anim_type_open[0]), "", 0},
	{"fadein_begin_opacity", "animations/window-open/fade-begin-opacity",
	 "animations", "window-open", "Fade from",
	 "Opacity a window fades in from.", OPT_FLOAT,
	 offsetof(Config, fadein_begin_opacity), 0, 0, 1, NULL, 0, "0.5", 0},
	{"animation_duration_close", "animations/window-close/duration",
	 "animations", "window-close", "Duration",
	 "How long a window takes to close, in milliseconds.", OPT_INT,
	 offsetof(Config, animation_duration_close), 0, 1, 3000, NULL, 0, "300",
	 0},
	{"animation_type_close", "animations/window-close/type", "animations",
	 "window-close", "Type", "Which closing animation to use.", OPT_STRING,
	 offsetof(Config, animation_type_close),
	 sizeof(((Config *)0)->animation_type_close), SCHEMA_NOCLAMP,
	 SCHEMA_NOCLAMP, schema_anim_type_close,
	 sizeof(schema_anim_type_close) / sizeof(schema_anim_type_close[0]),
	 "asteroid", 0},
	{"fadeout_begin_opacity", "animations/window-close/fade-begin-opacity",
	 "animations", "window-close", "Fade to",
	 "Opacity a window fades out to.", OPT_FLOAT,
	 offsetof(Config, fadeout_begin_opacity), 0, 0, 1, NULL, 0, "0.5", 0},
	{"fall_cols", "animations/window-close/fall-columns", "animations",
	 "window-close", "Fall columns",
	 "Columns the window breaks into for the fall animation.", OPT_INT,
	 offsetof(Config, fall_cols), 0, 1, 12, NULL, 0, "4", 0},
	{"fall_rows", "animations/window-close/fall-rows", "animations",
	 "window-close", "Fall rows",
	 "Rows the window breaks into for the fall animation.", OPT_INT,
	 offsetof(Config, fall_rows), 0, 1, 12, NULL, 0, "3", 0},
	{"animation_duration_move", "animation_duration_move", "animations",
	 "general", "Move duration",
	 "How long a window takes to move or resize, in milliseconds.", OPT_INT,
	 offsetof(Config, animation_duration_move), 0, 1, 3000, NULL, 0, "500", 0},
	{"animation_duration_tag", "animation_duration_tag", "animations",
	 "general", "Tag-switch duration",
	 "How long a tag switch takes, in milliseconds.", OPT_INT,
	 offsetof(Config, animation_duration_tag), 0, 1, 3000, NULL, 0, "300", 0},
	{"animation_duration_focus", "animation_duration_focus", "animations",
	 "general", "Focus duration",
	 "How long a focus change takes, in milliseconds.", OPT_INT,
	 offsetof(Config, animation_duration_focus), 0, 1, 3000, NULL, 0, "1",
	 0},
	{"animation_fade_in", "animation_fade_in", "animations", "general",
	 "Fade in", "Fade a window's opacity as it opens.", OPT_BOOL,
	 offsetof(Config, animation_fade_in), 0, 0, 1, NULL, 0, "1", 0},
	{"animation_fade_out", "animation_fade_out", "animations", "general",
	 "Fade out", "Fade a window's opacity as it closes.", OPT_BOOL,
	 offsetof(Config, animation_fade_out), 0, 0, 1, NULL, 0, "1", 0},

	/* ===== overview ===== */
	{"overviewgappi", "overview/gaps/inner", "overview", "gaps",
	 "Inner gap", "Gap between the overview's window previews.", OPT_INT,
	 offsetof(Config, overviewgappi), 0, 0, 200, NULL, 0, "5", 0},
	{"overviewgappo", "overview/gaps/outer", "overview", "gaps",
	 "Outer gap", "Gap between the overview and the screen edge.", OPT_INT,
	 offsetof(Config, overviewgappo), 0, 0, 200, NULL, 0, "30", 0},
	{"enable_hotarea", "overview/hotarea/enable", "overview", "hotarea",
	 "Corner trigger", "Open the overview by putting the cursor in a corner.",
	 OPT_BOOL, offsetof(Config, enable_hotarea), 0, 0, 1, NULL, 0, "0", 0},
	{"hotarea_size", "overview/hotarea/size", "overview", "hotarea",
	 "Trigger size", "Size of the corner trigger, in pixels.", OPT_INT,
	 offsetof(Config, hotarea_size), 0, 1, 200, NULL, 0, "10", 0},
	{"hotarea_corner", "hotarea_corner", "overview", "hotarea",
	 "Trigger corner",
	 "Which corner triggers it: 0 top-left, 1 top-right, 2 bottom-left, "
	 "3 bottom-right.",
	 OPT_INT, offsetof(Config, hotarea_corner), 0, 0, 3, NULL, 0, "2", 0},
	{"ov_tab_mode", "overview/tab-mode", "overview", "general", "Tab mode",
	 "Show the overview's tags as tabs.", OPT_BOOL,
	 offsetof(Config, ov_tab_mode), 0, 0, 1, NULL, 0, "1", 0},
	{"ov_no_resize", "overview/no-resize", "overview", "general", "No resize",
	 "Keep window previews at their real proportions rather than filling the "
	 "cell.",
	 OPT_BOOL, offsetof(Config, ov_no_resize), 0, 0, 1, NULL, 0, "1", 0},

	/* ===== input / cursor ===== */
	{"cursor_size", "input/cursor/size", "input", "cursor", "Size",
	 "Cursor size in pixels.", OPT_INT, offsetof(Config, cursor_size), 0, 4,
	 128, NULL, 0, "24", 0},
	{"cursor_theme", "input/cursor/theme", "input", "cursor", "Theme",
	 "Cursor theme name, e.g. Adwaita.", OPT_STRPTR,
	 offsetof(Config, cursor_theme), 0, SCHEMA_NOCLAMP, SCHEMA_NOCLAMP, NULL, 0,
	 "", 0},
	{"cursor_hide_timeout", "cursor_hide_timeout", "input", "cursor",
	 "Hide after",
	 /* 60s, not an hour. This is a slider, and a ceiling nobody would choose
	  * puts every usable value in the first pixels of the track: at 3600 the
	  * whole 1-30s range anyone actually wants was under 1% of it. Reported as
	  * "impossible to set to a reasonable level". */
	 "Seconds of inactivity before the cursor hides. 0 never hides it.",
	 OPT_INT, offsetof(Config, cursor_hide_timeout), 0, 0, 60, NULL, 0, "0",
	 0},
	{"cursor_hide_on_keypress", "cursor_hide_on_keypress", "input", "cursor",
	 "Hide on keypress", "Hide the cursor as soon as you start typing.",
	 OPT_BOOL, offsetof(Config, cursor_hide_on_keypress), 0, 0, 1, NULL, 0, "0",
	 0},
	{"cursor_zoom_step", "cursor_zoom_step", "input", "cursor", "Zoom step",
	 "How much each zoom step magnifies.", OPT_FLOAT,
	 offsetof(Config, cursor_zoom_step), 0, 0.01, 7, NULL, 0, "0.1", 0},
	{"cursor_zoom_rigid", "cursor_zoom_rigid", "input", "cursor",
	 "Rigid zoom", "Keep the magnifier's view locked to the cursor.",
	 OPT_BOOL, offsetof(Config, cursor_zoom_rigid), 0, 0, 1, NULL, 0, "0", 0},

};

#define CONFIG_SCHEMA_COUNT                                                    \
	(sizeof(config_schema) / sizeof(config_schema[0]))

/* ---------- lookup ---------- */

static const ConfigOption *schema_by_key(const char *key) {
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++)
		if (!strcmp(config_schema[i].key, key))
			return &config_schema[i];
	return NULL;
}

static const ConfigOption *schema_by_path(const char *path) {
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++)
		if (config_schema[i].path && !strcmp(config_schema[i].path, path))
			return &config_schema[i];
	return NULL;
}

/* ---------- reading a value out of a Config ---------- */

static void *schema_field(const Config *c, const ConfigOption *o) {
	return (void *)((const char *)c + o->offset);
}

/* Format a live value as the string a user would write.
 *
 * Colours are rounded, not truncated: (uint8_t)(0.17254f * 255) is 43 where
 * lround gives 44, so a truncating round trip darkens every colour by one step
 * every time the settings app saves. */
static void schema_format(const Config *c, const ConfigOption *o, char *out,
						  size_t cap) {
	const void *f = schema_field(c, o);
	switch (o->type) {
	case OPT_BOOL:
	case OPT_INT:
		snprintf(out, cap, "%d", *(const int32_t *)f);
		break;
	case OPT_FLOAT:
		snprintf(out, cap, "%g", (double)*(const float *)f);
		break;
	case OPT_DOUBLE:
		snprintf(out, cap, "%g", *(const double *)f);
		break;
	case OPT_COLOR: {
		const float *rgba = f;
		snprintf(out, cap, "0x%02lx%02lx%02lx%02lx",
				 lround(rgba[0] * 255.0f) & 0xff,
				 lround(rgba[1] * 255.0f) & 0xff,
				 lround(rgba[2] * 255.0f) & 0xff,
				 lround(rgba[3] * 255.0f) & 0xff);
		break;
	}
	case OPT_STRING:
		snprintf(out, cap, "%s", (const char *)f);
		break;
	case OPT_STRPTR: {
		const char *s = *(const char *const *)f;
		snprintf(out, cap, "%s", s ? s : "");
		break;
	}
	case OPT_ENUM: {
		int32_t v = *(const int32_t *)f;
		snprintf(out, cap, "%d", v);
		for (size_t i = 0; i < o->n_members; i++)
			if (!o->members[i].alias && o->members[i].value == v) {
				snprintf(out, cap, "%s", o->members[i].name);
				break;
			}
		break;
	}
	case OPT_BEZIER: {
		const double *d = f;
		snprintf(out, cap, "%g %g %g %g", d[0], d[1], d[2], d[3]);
		break;
	}
	}
}

/* Is `value` acceptable for this option? Type, enum membership, and range.
 *
 * Range is REJECTED rather than clamped. A UI has the schema and can bound its
 * own controls; clamping silently is how a panel ends up showing 200 while the
 * compositor runs 100. */
static bool schema_validate(const ConfigOption *o, const char *value,
							char *err, size_t errlen) {
	if (!value) {
		snprintf(err, errlen, "no value");
		return false;
	}
	switch (o->type) {
	case OPT_ENUM: {
		for (size_t i = 0; i < o->n_members; i++)
			if (!strcmp(o->members[i].name, value))
				return true;
		size_t n = (size_t)snprintf(err, errlen, "expected one of:");
		for (size_t i = 0; i < o->n_members && n < errlen; i++)
			n += (size_t)snprintf(err + n, errlen - n, " %s",
								  o->members[i].name);
		return false;
	}
	case OPT_STRING:
		if (o->size && strlen(value) >= o->size) {
			snprintf(err, errlen, "at most %zu characters", o->size - 1);
			return false;
		}
		return true;
	case OPT_STRPTR:
		return true;
	case OPT_COLOR: {
		if (strncmp(value, "0x", 2) && strncmp(value, "0X", 2)) {
			snprintf(err, errlen, "expected 0xRRGGBBAA");
			return false;
		}
		size_t n = strlen(value + 2);
		if (n != 8 && n != 6) {
			snprintf(err, errlen, "expected 0xRRGGBBAA");
			return false;
		}
		for (const char *p = value + 2; *p; p++)
			if (!isxdigit((unsigned char)*p)) {
				snprintf(err, errlen, "expected 0xRRGGBBAA");
				return false;
			}
		return true;
	}
	case OPT_BEZIER: {
		int n = 0;
		double d;
		const char *p = value;
		while (*p) {
			while (*p == ' ' || *p == ',')
				p++;
			if (!*p)
				break;
			char *end;
			d = strtod(p, &end);
			(void)d;
			if (end == p) {
				snprintf(err, errlen, "expected four numbers");
				return false;
			}
			p = end;
			n++;
		}
		if (n != 4) {
			snprintf(err, errlen, "expected four numbers");
			return false;
		}
		return true;
	}
	default: {
		char *end;
		double d = strtod(value, &end);
		if (end == value || *end) {
			snprintf(err, errlen, "expected a number");
			return false;
		}
		if (!isnan(o->min) && d < o->min) {
			snprintf(err, errlen, "below the minimum of %g", o->min);
			return false;
		}
		if (!isnan(o->max) && d > o->max) {
			snprintf(err, errlen, "above the maximum of %g", o->max);
			return false;
		}
		return true;
	}
	}
}

static const char *schema_type_name(OptType t) {
	switch (t) {
	case OPT_BOOL: return "bool";
	case OPT_INT: return "int";
	case OPT_FLOAT: return "float";
	case OPT_DOUBLE: return "double";
	case OPT_COLOR: return "color";
	case OPT_STRING: return "string";
	case OPT_STRPTR: return "string";
	case OPT_ENUM: return "enum";
	case OPT_BEZIER: return "bezier";
	}
	return "unknown";
}

/*
 * ── THE TABLE'S min/max, ACTUALLY ENFORCED ────────────────────────────────
 *
 * `min`/`max` are documented as "the clamp that is actually applied", and the
 * settings window builds its slider track from the same two numbers. Those are
 * one promise, not two, and keeping it by hand did not survive contact:
 *
 *   f53619d  schema borderpx max 200, override_config clamps 0..200. Agreed.
 *   033b133  twenty-two ceilings lowered so the sliders were aimable --
 *            borderpx to 32, the gaps to 200, the animation durations to 3000.
 *            The commit said in as many words that "these bounds are enforced,
 *            not advisory", and the enforcing half was never written.
 *
 * The hand-written CLAMP_INT calls in override_config still held the OLD, wider
 * numbers, so a slider that stopped at 32 was backed by a clamp that allowed
 * 200. `asteroidz -S` reported it as 22 failures from that day and was right
 * every time.
 *
 * Rather than copy twenty-two numbers into a second place again -- which is the
 * step that just failed -- this walks the table and clamps each numeric field
 * at its own offset. The schema becomes the one source, the duplicate can no
 * longer disagree with it, and lowering a ceiling in the table is now a single
 * edit that takes effect everywhere.
 *
 * Runs LAST in override_config, after the hand-written clamps. Those are kept:
 * they are all equal or wider, so they are no-ops for anything described here,
 * and they still guard the fields the table does not yet cover.
 *
 * Types the check itself skips are skipped here for the same reasons: an enum's
 * bounds are its member list, a colour is four channels behind one offset, and
 * a bezier is four doubles.
 */
static inline void schema_clamp_all(void) {
	extern Config config;
	for (size_t i = 0; i < CONFIG_SCHEMA_COUNT; i++) {
		const ConfigOption *o = &config_schema[i];
		if (isnan(o->min) && isnan(o->max)) {
			continue;
		}
		switch (o->type) {
		case OPT_BOOL:
		case OPT_INT: {
			int32_t *p = (int32_t *)((char *)&config + o->offset);
			if (!isnan(o->min) && (double)*p < o->min) {
				*p = (int32_t)o->min;
			}
			if (!isnan(o->max) && (double)*p > o->max) {
				*p = (int32_t)o->max;
			}
			break;
		}
		case OPT_FLOAT: {
			float *p = (float *)((char *)&config + o->offset);
			if (!isnan(o->min) && (double)*p < o->min) {
				*p = (float)o->min;
			}
			if (!isnan(o->max) && (double)*p > o->max) {
				*p = (float)o->max;
			}
			break;
		}
		case OPT_DOUBLE: {
			double *p = (double *)((char *)&config + o->offset);
			if (!isnan(o->min) && *p < o->min) {
				*p = o->min;
			}
			if (!isnan(o->max) && *p > o->max) {
				*p = o->max;
			}
			break;
		}
		/* An enum's valid set is `members`; a numeric range over its indices
		 * would be a different claim and is not one this makes. A colour is
		 * four channels behind one offset and a bezier is four doubles --
		 * neither is a scalar to clamp. The self-check skips exactly these. */
		case OPT_ENUM:
		case OPT_COLOR:
		case OPT_STRING:
		case OPT_STRPTR:
		case OPT_BEZIER:
			break;
		}
	}
}

#endif /* ASTEROIDZ_CONFIG_SCHEMA_H */
