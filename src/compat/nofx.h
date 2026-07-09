/*
 * no-scenefx compatibility shim (vulkan branch, iteration 1).
 *
 * asteroidz normally renders through scenefx (blur, shadows, rounded corners).
 * The wlroots + Vulkan path drops scenefx entirely, so this header provides the
 * scenefx-only types and API as no-ops: effect scene nodes are drawn as plain
 * (transparent) rects and every effect setter does nothing. Base surfaces and
 * rects still render via wlroots' scene + Vulkan renderer; there are just no
 * effects. A real Vulkan scene engine replaces this later.
 */
#ifndef ASTEROIDZ_COMPAT_NOFX_H
#define ASTEROIDZ_COMPAT_NOFX_H

#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

/* --- scenefx value types --- */
struct fx_corner_radii {
	uint16_t top_left;
	uint16_t top_right;
	uint16_t bottom_right;
	uint16_t bottom_left;
};
struct clipped_region {
	struct wlr_box area;
	struct fx_corner_radii corners;
};
struct blur_data {
	int num_passes;
	float radius;
	float noise;
	float brightness;
	float contrast;
	float saturation;
	float transparency_threshold;
};
static inline struct clipped_region clipped_region_get_default(void) {
	return (struct clipped_region){0};
}
static inline bool fx_corner_radii_eq(const struct fx_corner_radii *a,
		const struct fx_corner_radii *b) {
	return a->top_left == b->top_left && a->top_right == b->top_right &&
		a->bottom_right == b->bottom_right && a->bottom_left == b->bottom_left;
}

/* --- effect scene nodes are just rects (invisible) --- */
#define wlr_scene_shadow wlr_scene_rect
#define wlr_scene_blur wlr_scene_rect
#define wlr_scene_optimized_blur wlr_scene_rect

/* effect scene-node types never occur (nodes are real rects); keep the enum
 * names valid but out of range so any `case` on them is simply dead. */
#define WLR_SCENE_NODE_SHADOW 0x1000
#define WLR_SCENE_NODE_BLUR 0x1001
#define WLR_SCENE_NODE_OPTIMIZED_BLUR 0x1002

static inline const float *nofx_clear(void) {
	static const float c[4] = {0, 0, 0, 0};
	return c;
}

static inline struct wlr_scene_rect *wlr_scene_shadow_create(
		struct wlr_scene_tree *parent, int width, int height,
		int corner_radius, float blur_sigma, const float color[4]) {
	(void)corner_radius; (void)blur_sigma; (void)color;
	return wlr_scene_rect_create(parent, width, height, nofx_clear());
}
static inline struct wlr_scene_rect *wlr_scene_blur_create(
		struct wlr_scene_tree *parent, int width, int height) {
	return wlr_scene_rect_create(parent, width, height, nofx_clear());
}
static inline struct wlr_scene_rect *wlr_scene_optimized_blur_create(
		struct wlr_scene_tree *parent, int width, int height) {
	return wlr_scene_rect_create(parent, width, height, nofx_clear());
}

#define wlr_scene_shadow_from_node(n) ((struct wlr_scene_rect *)NULL)
#define wlr_scene_blur_from_node(n) ((struct wlr_scene_rect *)NULL)
#define wlr_scene_blur_get_transparency_mask_source(b) \
	((struct wlr_scene_buffer *)NULL)

/* every effect setter is a no-op */
#define wlr_scene_shadow_set_size(...) ((void)0)
#define wlr_scene_shadow_set_color(...) ((void)0)
#define wlr_scene_shadow_set_blur_sigma(...) ((void)0)
#define wlr_scene_shadow_set_corner_radius(...) ((void)0)
#define wlr_scene_shadow_set_corner_radii(...) ((void)0)
#define wlr_scene_shadow_set_clipped_region(...) ((void)0)
#define wlr_scene_blur_set_size(...) ((void)0)
#define wlr_scene_blur_set_alpha(...) ((void)0)
#define wlr_scene_blur_set_strength(...) ((void)0)
#define wlr_scene_blur_set_region(...) ((void)0)
#define wlr_scene_blur_set_clipped_region(...) ((void)0)
#define wlr_scene_blur_set_corner_radii(...) ((void)0)
#define wlr_scene_blur_set_should_only_blur_bottom_layer(...) ((void)0)
#define wlr_scene_blur_set_transparency_mask_source(...) ((void)0)
#define wlr_scene_optimized_blur_set_size(...) ((void)0)
#define wlr_scene_optimized_blur_mark_dirty(...) ((void)0)
#define wlr_scene_rect_set_corner_radius(...) ((void)0)
#define wlr_scene_rect_set_corner_radii(...) ((void)0)
#define wlr_scene_rect_set_clipped_region(...) ((void)0)
#define wlr_scene_rect_set_backdrop_blur(...) ((void)0)
#define wlr_scene_rect_set_backdrop_blur_optimized(...) ((void)0)
#define wlr_scene_buffer_set_corner_radii(...) ((void)0)
#define wlr_scene_buffer_set_backdrop_blur(...) ((void)0)
#define wlr_scene_set_blur_data(...) ((void)0)
#define wlr_scene_set_sdr_reference_luminance(...) ((void)0)
#define wlr_scene_set_sdr_saturation(...) ((void)0)
#define wlr_scene_output_set_zoom(...) ((void)0)
#define wlr_scene_rect_set_gradient(...) ((void)0)

static inline uint16_t corner_radius_clamp(int r) {
	return r < 0 ? 0 : (r > 65535 ? 65535 : (uint16_t)r);
}

#endif
