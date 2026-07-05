#ifndef jump_label_node_H
#define jump_label_node_H

#include <cairo.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <scenefx/types/wlr_scene.h>

// Pre-existing struct, assumed to already exist
typedef struct {
	float fg_color[4];
	float bg_color[4];
	float focus_fg_color[4];
	float focus_bg_color[4];
	float border_color[4];
	int32_t border_width;
	int32_t corner_radius;
	int32_t padding_x;
	int32_t padding_y;
	const char *font_desc;
} DecorateDrawData;

struct mango_text_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

struct mango_jump_label_node {
	struct wlr_scene_buffer *scene_buffer;
	struct mango_text_buffer *buffer;
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

struct mango_tab_bar_node {
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
	struct mango_text_buffer *buffer;
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
	struct mango_text_buffer *buffer;
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
} MangoGroupBar;

void mango_text_global_finish(void);
struct mango_jump_label_node *
mango_jump_label_node_create(struct wlr_scene_tree *parent,
							 DecorateDrawData data);
void mango_jump_label_node_destroy(struct mango_jump_label_node *node);
void mango_jump_label_node_set_background(struct mango_jump_label_node *node,
										  float r, float g, float b, float a);
void mango_jump_label_node_set_border(struct mango_jump_label_node *node,
									  float r, float g, float b, float a,
									  int32_t width, int32_t radius);
void mango_jump_label_node_set_padding(struct mango_jump_label_node *node,
									   int32_t pad_x, int32_t pad_y);
void mango_jump_label_node_update(struct mango_jump_label_node *node,
								  const char *text, float scale);

struct mango_tab_bar_node *
mango_tab_bar_node_create(void *mango_node_data, struct wlr_scene_tree *parent,
						  DecorateDrawData data, int32_t width, int32_t height);
void mango_tab_bar_node_destroy(struct mango_tab_bar_node *node);
void mango_tab_bar_node_set_size(struct mango_tab_bar_node *node, int32_t width,
								 int32_t height);
void mango_tab_bar_node_set_enabled(struct mango_tab_bar_node *node,
									bool enabled);
void mango_tab_bar_node_set_position(struct mango_tab_bar_node *node,
									 int32_t x, int32_t y);
void mango_tab_bar_node_set_shadow(struct mango_tab_bar_node *node,
								   bool enabled, float sigma, int32_t offset_y,
								   const float color[4]);
void mango_tab_bar_node_set_icon(struct mango_tab_bar_node *node,
								 const char *icon_name);
void mango_text_node_set_icon_theme(const char *theme);
void mango_tab_bar_node_update(struct mango_tab_bar_node *node,
							   const char *text, float scale);

void mango_jump_label_node_set_focus(struct mango_jump_label_node *node,
									 bool focused);
void mango_tab_bar_node_set_focus(struct mango_tab_bar_node *node,
								  bool focused);

void mango_tab_bar_node_set_colors(struct mango_tab_bar_node *node,
								   const float fg[4], const float bg[4]);

MangoGroupBar *mango_group_bar_create(void *cdata, uint32_t type,
									  struct wlr_scene_tree *parent,
									  DecorateDrawData data, int32_t width,
									  int32_t height);
void mango_group_bar_destroy(MangoGroupBar *node);
void mango_group_bar_set_size(MangoGroupBar *node, int32_t width,
							  int32_t height);
void mango_group_bar_update(MangoGroupBar *node, const char *text, float scale);
void mango_group_bar_set_focus(MangoGroupBar *node, bool focused);
void mango_group_bar_set_colors(MangoGroupBar *node, const float fg[4],
								const float bg[4]);
void mango_group_bar_apply_config(MangoGroupBar *node,
								  const DecorateDrawData *data);
void mango_jump_label_node_apply_config(struct mango_jump_label_node *node,
										const DecorateDrawData *data);
void mango_tab_bar_node_apply_config(struct mango_tab_bar_node *node,
									 const DecorateDrawData *data);
#endif // jump_label_node_H