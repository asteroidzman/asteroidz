#ifndef AVK_SCENE_H
#define AVK_SCENE_H

#include "../image/avk_image.h"

#include <pixman.h>

/*
 * What AVK renders: a flat list of commands, built by the compositor on its
 * own thread and then immutable.
 *
 * This is the replacement for `wlr_render_pass`, and the differences are the
 * point of the exercise:
 *
 *  - It is a SNAPSHOT. Once handed over, nothing in it can change. The old
 *    path walks a live wlr_scene node tree while recording, so a client commit
 *    or a scene update landing mid-frame mutates geometry the renderer is
 *    halfway through using. Here the compositor freezes what it wants drawn,
 *    and AVK never looks at a scene node.
 *  - It carries no renderer objects. A command references an avk_image, which
 *    the compositor-side buffer cache has already resolved -- so there is no
 *    wlr_texture, no downcast to reach effects, and no way for the renderer
 *    abstraction to creep back in.
 *  - It has room for M4. Corner radius, shadow and blur fields exist in the
 *    command struct and are simply not honoured yet, so effects arrive as
 *    renderer work rather than as a redesign of this file.
 *
 * Geometry is in OUTPUT pixels with the origin at the top-left, after any
 * output transform has been applied by the compositor. AVK converts to
 * normalised device coordinates itself.
 */

/* AVK's own transform enum, mirroring wl_output_transform but not including
 * it: a Wayland protocol header in the renderer would be exactly the kind of
 * incidental coupling this subsystem is supposed to be free of. The compositor
 * converts at the boundary. */
enum avk_transform {
	AVK_TRANSFORM_NORMAL = 0,
	AVK_TRANSFORM_90,
	AVK_TRANSFORM_180,
	AVK_TRANSFORM_270,
	AVK_TRANSFORM_FLIPPED,
	AVK_TRANSFORM_FLIPPED_90,
	AVK_TRANSFORM_FLIPPED_180,
	AVK_TRANSFORM_FLIPPED_270,
};

struct avk_box {
	int32_t x, y, width, height;
};

/* Sub-pixel source rectangle, in image pixels. Wayland's viewporter gives
 * clients fractional source rectangles, and rounding them here would show up
 * as a half-pixel shimmer when a scaled window moves. */
struct avk_fbox {
	double x, y, width, height;
};

enum avk_cmd_type {
	AVK_CMD_RECT,
	AVK_CMD_TEXTURE,
};

struct avk_cmd {
	enum avk_cmd_type type;

	/* Where it lands, in output pixels. */
	struct avk_box dst;

	/* Everything outside this is not drawn. The compositor puts the node's
	 * visible region here -- what is left after occlusion -- so AVK does not
	 * shade pixels that something opaque covers. Empty means "no clip". */
	pixman_region32_t clip;
	bool has_clip;

	float opacity;

	/* AVK_CMD_RECT: straight (non-premultiplied) RGBA, premultiplied by AVK
	 * before it reaches the shader so callers can write the colour they mean.
	 */
	float color[4];

	/* AVK_CMD_TEXTURE */
	struct avk_image *image;   /* borrowed; the caller keeps it alive */
	struct avk_fbox src;       /* in image pixels */
	enum avk_transform transform;
	/* Nearest for a 1:1 blit, linear when scaled. Chosen by the compositor
	 * because it knows the scale factor; a renderer guessing from the box
	 * ratio gets fractional-scale cases wrong. */
	bool filter_linear;

	/*
	 * Rounded corners, in OUTPUT PIXELS, clockwise from the top left:
	 * top-left, top-right, bottom-right, bottom-left.
	 *
	 * Four values and not one because SceneFX stores four
	 * (struct fx_corner_radii), and asteroidz uses them: a window joined to
	 * its titlebar is rounded on two corners and square on the other two.
	 * Anything carrying a single radius renders that wrong.
	 *
	 * All zero means no rounding, which is the common case and costs one
	 * uniform branch in the shader.
	 */
	float corners[4];

	/*
	 * The INTERIOR CUT-OUT, in the same space and the same clockwise order as
	 * `dst`/`corners` above. `dst` minus `inner` is the shape actually drawn.
	 *
	 * This is how a window border exists: SceneFX draws one filled rect with
	 * the window's interior removed, and BOTH edges of that ring are rounded
	 * -- the outer one at the window's radius, the inner one at the radius the
	 * compositor computed for the client underneath. Carrying the inner box
	 * without its radii was the M4A artifact: the ring's outside was an arc,
	 * its inside was a square, and on each corner's diagonal the square cut
	 * away border the arc never covered, leaving the wallpaper showing through
	 * a wedge 104 pixels across.
	 *
	 * Both halves are needed and neither is redundant. `inner` is an exact
	 * scissor subtraction, which is free and is the whole answer when the
	 * radii are zero; `inner_corners` are the arcs, which a region cannot
	 * express at all.
	 *
	 * has_inner false means the command fills `dst` solid.
	 */
	struct avk_box inner;
	float inner_corners[4];
	bool has_inner;

	/* reserved for later M4 stages */
	bool has_shadow;
	bool has_blur;
};

/*
 * One frame's worth of work.
 *
 * `damage` is the region AVK is permitted to touch. Commands are still all
 * present -- clipping to damage is the renderer's job, because an effect in M4
 * may need to read outside the damaged area to write inside it.
 */
struct avk_scene {
	struct avk_cmd *cmds;
	size_t len;
	size_t cap;

	pixman_region32_t damage;

	/* Cleared before anything is drawn, in the damaged region only. */
	float clear_color[4];
	bool has_clear;
};

void avk_scene_init(struct avk_scene *scene);
void avk_scene_finish(struct avk_scene *scene);

/* Returns a zeroed command with sane defaults (opacity 1, no clip) ready to
 * fill in, or NULL if the list could not grow. */
struct avk_cmd *avk_scene_add(struct avk_scene *scene, enum avk_cmd_type type);

/* Set a command's clip region. Copies, so the caller's region can go away. */
bool avk_cmd_set_clip(struct avk_cmd *cmd, const pixman_region32_t *region);

#endif /* AVK_SCENE_H */
