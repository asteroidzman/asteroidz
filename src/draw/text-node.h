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
 * strips, group bars, jump-mode labels, and the screenshot UI. Colours are
 * normally generated from the matugen palette (dms/colors.kdl). Formerly
 * "pilldata"/"pill/*" after the DMS pill widgets it started with; those
 * spellings remain accepted as deprecated aliases. */
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

struct asteroidz_tab_bar_node {
	struct wlr_scene_buffer *scene_buffer;
	struct wlr_scene_tree *parent_tree;
	/* soft shadow rendered below the pill (scenefx shadow node) */
	struct wlr_scene_shadow *shadow;
	float shadow_sigma;
	int32_t shadow_offset_y;
	float shadow_color[4];
	int32_t last_x, last_y;
	/* app icon drawn before the title; owned by the shared icon cache */
	cairo_surface_t *icon_surface;
	cairo_surface_t *cached_icon;
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

/* Group bar node: one segment of the window-group title bar.
 * `type` must stay the first member so generic scene node.data walks
 * (xytonode) can identify it via the shared client-type enum (GroupBar). */
typedef struct {
	uint32_t type; // must be first in struct
	struct wlr_scene_buffer *scene_buffer;
	struct asteroidz_text_buffer *buffer;
	cairo_surface_t *surface;
	int surface_pixel_w, surface_pixel_h;
	void *node_data; // owning Client pointer

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

	// size
	int32_t target_width;
	int32_t target_height;

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
} AsteroidzGroupBar;

void asteroidz_text_global_finish(void);

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

void asteroidz_jump_label_node_set_focus(struct asteroidz_jump_label_node *node,
									 bool focused);
void asteroidz_tab_bar_node_set_focus(struct asteroidz_tab_bar_node *node,
								  bool focused);

void asteroidz_tab_bar_node_set_colors(struct asteroidz_tab_bar_node *node,
								   const float fg[4], const float bg[4]);

AsteroidzGroupBar *asteroidz_group_bar_create(void *cdata, uint32_t type,
									  struct wlr_scene_tree *parent,
									  AsteroidzTheme data, int32_t width,
									  int32_t height);
void asteroidz_group_bar_destroy(AsteroidzGroupBar *node);
void asteroidz_group_bar_set_size(AsteroidzGroupBar *node, int32_t width,
							  int32_t height);
void asteroidz_group_bar_update(AsteroidzGroupBar *node, const char *text, float scale);
void asteroidz_group_bar_set_focus(AsteroidzGroupBar *node, bool focused);
void asteroidz_group_bar_set_colors(AsteroidzGroupBar *node, const float fg[4],
								const float bg[4]);
void asteroidz_group_bar_apply_config(AsteroidzGroupBar *node,
								  const AsteroidzTheme *data);
void asteroidz_jump_label_node_apply_config(struct asteroidz_jump_label_node *node,
										const AsteroidzTheme *data);
void asteroidz_tab_bar_node_apply_config(struct asteroidz_tab_bar_node *node,
									 const AsteroidzTheme *data);
#endif // jump_label_node_H