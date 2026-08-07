#ifndef jump_label_node_H
#define jump_label_node_H

#include <cairo.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <scenefx/types/wlr_scene.h>
#include "../common/corner_location.h"

/* The compositor-native UI theme (config.theme, KDL block `theme {}`):
 * the single look shared by every native overlay -- titlebars/monocle tab
 * strips, jump-mode labels, and the screenshot UI. Colours are
 * normally generated from the matugen palette (dms/colors.kdl). */
typedef struct {
	float fg_color[4];
	float bg_color[4];
	float focus_fg_color[4];
	float focus_bg_color[4];
	float urgent_color[4]; /* attention accent (matugen error) */
	float border_color[4];
	int32_t border_width;
	int32_t corner_radius;
	int32_t padding_x;
	int32_t padding_y;
	const char *font_desc;
} AsteroidzTheme;

struct asteroidz_text_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

struct asteroidz_jump_label_node {
	struct wlr_scene_buffer *scene_buffer;
	struct asteroidz_text_buffer *buffer;
	cairo_surface_t *surface;
	int surface_pixel_w, surface_pixel_h;

	float fg_color[4];
	float bg_color[4];
	float focus_fg_color[4];
	float focus_bg_color[4];
	float border_color[4];
	int32_t border_width;
	int32_t corner_radius;
	int32_t padding_x;
	int32_t padding_y;
	char *font_desc;

	/* The text is Pango MARKUP rather than a plain string. Off by default:
	 * every other label draws text that came from a client (a window title, an
	 * app id) and would have to be escaped first. The exit prompt sets it,
	 * because "ENTER" has to carry weight and colour the rest of the sentence
	 * does not. */
	bool markup;

	// cache
	char *cached_text;
	char *cached_font_desc;
	float cached_scale;
	float cached_fg_color[4];
	float cached_bg_color[4];
	float cached_focus_fg_color[4];
	float cached_focus_bg_color[4];
	float cached_border_color[4];
	int32_t cached_border_width;
	int32_t cached_corner_radius;
	int32_t cached_padding_x;
	int32_t cached_padding_y;
	bool cached_focused;

	bool focused;

	// measurement
	cairo_surface_t *measure_surface;
	cairo_t *measure_cr;
	PangoContext *measure_context;
	PangoLayout *measure_layout;
	float measure_scale;

	int32_t logical_width;
	int32_t logical_height;
};

#define ASTEROIDZ_TAB_MAX_ICONS 4

struct asteroidz_tab_bar_node {
	struct wlr_scene_buffer *scene_buffer;
	struct wlr_scene_tree *parent_tree;
	/* soft shadow rendered below the pill (scenefx shadow node) */
	struct wlr_scene_shadow *shadow;
	float shadow_sigma;
	int32_t shadow_offset_y;
	float shadow_color[4];
	int32_t last_x, last_y;
	/* Icons drawn before the text, left to right. One for a titlebar's app
	 * icon; several for a workspace pill showing what is running on that tag.
	 * Surfaces are owned references taken from the shared icon cache. */
	cairo_surface_t *icons[ASTEROIDZ_TAB_MAX_ICONS];
	int32_t nicons;
	cairo_surface_t *cached_icons[ASTEROIDZ_TAB_MAX_ICONS];
	int32_t cached_nicons;
	/* Draw the icon row AFTER the text instead of before it. Titlebars want the
	 * app icon leading the title; the bar's workspace pills want "1: web" then
	 * the icons of what is running there, which is the order the waybar
	 * workspace module uses. */
	bool icons_after_text;
	bool cached_icons_after_text;
	/* Recolour the icons to `icon_tint` instead of painting them as they are.
	 * The waybar status plugins ship MONOCHROME svgs (a solid #000 silhouette)
	 * and tint them to the widget's resolved CSS colour -- painted as-is they
	 * come out flat black, which is what a bar over a dark panel shows as
	 * nothing at all. App icons and the logo are real artwork and must never
	 * be tinted, hence the per-node opt-in. */
	bool icon_tinted;
	float icon_tint[4];
	bool cached_icon_tinted;
	float cached_icon_tint[4];
	/* Fraction of the text area's height an icon is fitted into. 1.0 -- the
	 * whole of it -- is right for a glyph drawn with its own margins, which is
	 * what an icon theme ships. A silhouette that runs edge to edge in its
	 * viewBox comes out visually much larger than a themed icon of the same
	 * nominal size, so the pills using those (the media transport buttons) ask
	 * for less. Scaling here rather than in the SVG keeps the advance and the
	 * drawn size in step: both read this. */
	double icon_scale;
	double cached_icon_scale;
	struct asteroidz_text_buffer *buffer;
	cairo_surface_t *surface;
	int surface_pixel_w, surface_pixel_h;

	// initial config
	float fg_color[4];
	float bg_color[4];
	float focus_fg_color[4];
	float focus_bg_color[4];
	float border_color[4];
	int32_t border_width;
	int32_t corner_radius;
	int32_t padding_x;
	int32_t padding_y;
	char *font_desc;
	enum corner_location corner_mask;
	bool text_align_left;
	/* when > 0, draw a titlebar-style border inset so it aligns with the
	 * window's own border. The top edge is always drawn and the bottom
	 * never (it's flush against the window); the left/right edges are drawn
	 * only where the flags say so, letting adjacent segments/pills (close
	 * button + title tab, monocle segments in a row) omit the borders on
	 * their touching internal seams. The color is swapped by focus state
	 * (the focused pill borders in the unfocused bg color and vice versa)
	 * rather than using border_color. Kept separate from border_width so
	 * the pill background still fills to the bottom edge. */
	int32_t titlebar_border_width;
	bool titlebar_border_left;
	bool titlebar_border_right;
	/* draw a separator (in the fg/contrast color) on the right edge, to
	 * divide adjacent same-colored titlebar segments in a monocle strip */
	bool titlebar_separator_right;

	/* uniform shrink of the CONTENT (font size + padding + icon gap), used by
	 * the overview's scaled-down titlebars. Distinct from the `scale` param of
	 * _update(), which is a HiDPI pixel-density scale (the surface is rendered
	 * at target*scale pixels and displayed at the logical target size, so it
	 * does NOT change the visual size of anything). 1.0 = desktop look. */
	float content_scale;

	// size
	int32_t target_width;
	int32_t target_height;

	// cache
	char *cached_text;
	char *cached_font_desc;
	float cached_scale;
	float cached_content_scale;
	float cached_fg_color[4];
	float cached_bg_color[4];
	float cached_focus_fg_color[4];
	float cached_focus_bg_color[4];
	float cached_border_color[4];
	int32_t cached_border_width;
	int32_t cached_corner_radius;
	int32_t cached_padding_x;
	int32_t cached_padding_y;
	int32_t cached_target_width;
	int32_t cached_target_height;
	bool cached_focused;
	enum corner_location cached_corner_mask;
	int32_t cached_titlebar_border_width;
	bool cached_titlebar_border_left;
	bool cached_titlebar_border_right;
	bool cached_titlebar_separator_right;

	bool focused;

	// last draw params (used to redraw on size change)
	char *last_text;
	float last_scale;

	// measurement
	cairo_surface_t *measure_surface;
	cairo_t *measure_cr;
	PangoContext *measure_context;
	PangoLayout *measure_layout;
	float measure_scale;

	int32_t logical_width;
	int32_t logical_height;
};

void asteroidz_text_global_finish(void);

/* The natural line height of a Pango font description, in LOGICAL pixels at
 * 96 dpi -- the same resolution every native overlay measures at, so the
 * answer is in the units a config value is written in and the output scale is
 * applied to it later like any other size.
 *
 * The font's own ascent plus descent, NOT the extent of some particular
 * string. A box sized from a string changes height with its contents: a
 * titlebar would be one pixel taller for a title with a "g" in it than for one
 * without, and every tiled window below it would move. */
int32_t asteroidz_font_line_height(const char *font_desc);

/* Install a raw ARGB32 (network byte order, straight alpha) image into the
 * shared icon cache under `key`, so it can then be drawn with
 * _set_icon/_set_icons exactly like a themed icon name. This is how a
 * StatusNotifierItem's IconPixmap gets on screen: the item hands over pixels
 * rather than a name, and there is no file anywhere to resolve. Replacing an
 * existing key frees the surface it displaces. */
bool asteroidz_icon_cache_put_argb32(const char *key, const uint8_t *argb_be,
									 int32_t w, int32_t h);

/* Same, for artwork the compositor DRAWS rather than loads: hands a finished
 * cairo surface straight to the cache, which takes ownership. Used by the
 * network indicator, whose two halves are lit independently and so cannot be
 * a tinted stencil. */
bool asteroidz_icon_cache_put_surface(const char *key, cairo_surface_t *surf);

/* a standalone app-icon buffer (used for overview thumbnails) */
struct asteroidz_icon_node {
	struct wlr_scene_buffer *scene_buffer;
	struct asteroidz_text_buffer *buffer;
	cairo_surface_t *surface;
	int32_t size;
	char *cached_name;
};
struct asteroidz_icon_node *
asteroidz_icon_node_create(struct wlr_scene_tree *parent);
bool asteroidz_icon_node_set(struct asteroidz_icon_node *node,
							 const char *icon_name, int32_t size);
void asteroidz_icon_node_destroy(struct asteroidz_icon_node *node);

struct asteroidz_jump_label_node *
asteroidz_jump_label_node_create(struct wlr_scene_tree *parent,
							 AsteroidzTheme data);
void asteroidz_jump_label_node_destroy(struct asteroidz_jump_label_node *node);
void asteroidz_jump_label_node_set_background(struct asteroidz_jump_label_node *node,
										  float r, float g, float b, float a);
void asteroidz_jump_label_node_set_border(struct asteroidz_jump_label_node *node,
									  float r, float g, float b, float a,
									  int32_t width, int32_t radius);
void asteroidz_jump_label_node_set_padding(struct asteroidz_jump_label_node *node,
									   int32_t pad_x, int32_t pad_y);
/* Draw this label's text as Pango markup. The caller owns escaping anything
 * that did not come from asteroidz itself. */
void asteroidz_jump_label_node_set_markup(
	struct asteroidz_jump_label_node *node, bool markup);

void asteroidz_jump_label_node_update(struct asteroidz_jump_label_node *node,
								  const char *text, float scale);

struct asteroidz_tab_bar_node *
asteroidz_tab_bar_node_create(void *asteroidz_node_data, struct wlr_scene_tree *parent,
						  AsteroidzTheme data, int32_t width, int32_t height);
void asteroidz_tab_bar_node_destroy(struct asteroidz_tab_bar_node *node);
void asteroidz_tab_bar_node_set_size(struct asteroidz_tab_bar_node *node, int32_t width,
								 int32_t height);
void asteroidz_tab_bar_node_set_enabled(struct asteroidz_tab_bar_node *node,
									bool enabled);
void asteroidz_tab_bar_node_set_position(struct asteroidz_tab_bar_node *node,
									 int32_t x, int32_t y);
void asteroidz_tab_bar_node_set_shadow(struct asteroidz_tab_bar_node *node,
								   bool enabled, float sigma, int32_t offset_y,
								   const float color[4]);
void asteroidz_tab_bar_node_set_icon(struct asteroidz_tab_bar_node *node,
								 const char *icon_name);
/* Replace the icon row wholesale. `count` is clamped to
 * ASTEROIDZ_TAB_MAX_ICONS; a NULL or empty name is skipped rather than
 * leaving a gap. */
void asteroidz_tab_bar_node_set_icons(struct asteroidz_tab_bar_node *node,
								  const char *const *icon_names,
								  int32_t count);
/* false (default) draws the icons before the text, true after it. */
void asteroidz_tab_bar_node_set_icons_after_text(
	struct asteroidz_tab_bar_node *node, bool after);
/* Fraction of the text-area height the icons are fitted into; 1.0 is the
 * default and the full height. Clamped to a sane range. */
void asteroidz_tab_bar_node_set_icon_scale(struct asteroidz_tab_bar_node *node,
									   double scale);
/* Paint the icons in `rgba` (through their own alpha) rather than in their own
 * colours. NULL restores untinted painting. */
void asteroidz_tab_bar_node_set_icon_tint(struct asteroidz_tab_bar_node *node,
									  const float rgba[4]);
void asteroidz_tab_bar_node_set_corner_mask(struct asteroidz_tab_bar_node *node,
										enum corner_location mask);
void asteroidz_tab_bar_node_set_text_align_left(struct asteroidz_tab_bar_node *node,
											bool align_left);
void asteroidz_tab_bar_node_set_padding(struct asteroidz_tab_bar_node *node,
									int32_t padding_x, int32_t padding_y);
void asteroidz_tab_bar_node_set_content_scale(struct asteroidz_tab_bar_node *node,
										  float content_scale);
void asteroidz_tab_bar_node_reparent(struct asteroidz_tab_bar_node *node,
								 struct wlr_scene_tree *parent);
void asteroidz_tab_bar_node_set_titlebar_border(struct asteroidz_tab_bar_node *node,
											int32_t width, bool border_left,
											bool border_right);
void asteroidz_tab_bar_node_set_titlebar_separator(
	struct asteroidz_tab_bar_node *node, bool separator_right);
void asteroidz_text_node_set_icon_theme(const char *theme);
void asteroidz_tab_bar_node_update(struct asteroidz_tab_bar_node *node,
							   const char *text, float scale);
/* Natural ("fit the content") logical width this pill would need to render
 * `text` at `height` without ellipsizing, including border, padding, and the
 * icon plus its gap when one is set. Titlebars don't need this -- their width
 * is dictated by the window -- but a bar lays pills out end to end, so it has
 * to ask each one how wide it wants to be BEFORE assigning positions. Returns
 * a logical (unscaled) width to pair with _set_size, which is also logical. */
int32_t asteroidz_tab_bar_node_measure_width(struct asteroidz_tab_bar_node *node,
										 const char *text, int32_t height);

void asteroidz_jump_label_node_set_focus(struct asteroidz_jump_label_node *node,
									 bool focused);
void asteroidz_tab_bar_node_set_focus(struct asteroidz_tab_bar_node *node,
								  bool focused);

void asteroidz_tab_bar_node_set_colors(struct asteroidz_tab_bar_node *node,
								   const float fg[4], const float bg[4]);

void asteroidz_jump_label_node_apply_config(struct asteroidz_jump_label_node *node,
										const AsteroidzTheme *data);
void asteroidz_tab_bar_node_apply_config(struct asteroidz_tab_bar_node *node,
									 const AsteroidzTheme *data);
#endif // jump_label_node_H