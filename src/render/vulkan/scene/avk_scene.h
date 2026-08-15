#ifndef AVK_SCENE_H
#define AVK_SCENE_H

/* M5/C2: the source luminance domain carried on a texture command. A plain
 * POD header with no wlroots and no Vulkan in it, which is why a renderer
 * header may include it. */
#include "render/az_lum.h"

#include "../avk.h"
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

/* Sub-pixel source rectangle, in image pixels. Wayland's viewporter gives
 * clients fractional source rectangles, and rounding them here would show up
 * as a half-pixel shimmer when a scaled window moves. */
struct avk_fbox {
	double x, y, width, height;
};

enum avk_cmd_type {
	AVK_CMD_RECT,
	AVK_CMD_TEXTURE,
	/*
	 * P2. A textured quad whose four corners are placed independently, rather
	 * than derived from an axis-aligned rectangle.
	 *
	 * Everything about SAMPLING is the same as AVK_CMD_TEXTURE: the same
	 * image, the same `src` crop, the same `transform`, the same decode
	 * variant chosen from the same `lum` domain. What differs is only where
	 * the four destination corners land -- `quad`, below.
	 *
	 * WHAT IT DELIBERATELY DOES NOT HAVE: rounding, an inner cut-out, or a
	 * shadow. Not an omission to be filled in later -- the corner positions
	 * are CARRIED IN the push-constant slots those fields would occupy (the
	 * block is at the 128-byte guaranteed minimum with no room to grow), so a
	 * rounded quad would ask the fragment shader to read a corner position as
	 * a rectangle. `corners` and the inner fields are asserted zero when one
	 * of these is recorded.
	 *
	 * It is also NEVER AN OCCLUDER. The occlusion pass reasons about
	 * axis-aligned opaque rectangles; a rotated quad's `dst` is its bounding
	 * box, which is larger than the quad and contains pixels it does not
	 * cover, so treating it as an occluder would erase things behind it that
	 * are actually visible.
	 */
	AVK_CMD_TEXTURE_QUAD,
	/*
	 * M4D. A rounded-rectangle drop shadow, evaluated analytically -- no
	 * intermediate image, no blur pass. `dst` is the shadow's ENVELOPE and the
	 * caster is that box inset by `blur_sigma`; see shadow.frag.
	 */
	AVK_CMD_SHADOW,
	/*
	 * M4F. A live background blur.
	 *
	 * `dst` is the WRITE region -- where the blurred result is composited. What
	 * it blurs is the current-frame scene prefix behind it: the renderer
	 * replays commands [0, index-of-this-command) into a regional transient and
	 * runs the dual-Kawase chain on that.
	 *
	 * Its source is therefore decided by SCENE ORDER and nothing else. A
	 * command later in the list does not contribute however large or bright it
	 * is; a command earlier in the list does, whatever kind it is -- including
	 * an earlier blur's own composited result.
	 */
	AVK_CMD_BLUR,
};

/*
 * How many command types there are, for exhaustiveness checks.
 *
 * Derived from the last enumerator rather than written as a literal, and NOT an
 * enumerator itself: a sentinel inside the enum would have to be given a `case`
 * in every switch, which is precisely the silent-fallthrough slot this exists to
 * close. Adding a type here changes this number, which fails the _Static_assert
 * in avk_render.c and points at the switch that has to learn about it.
 */
#define AVK_CMD_TYPE_COUNT ((int)AVK_CMD_BLUR + 1)

/* 1 and 2 are the values SceneFX's `gradient_linear` field carries, kept rather
 * than renumbered so a mismatch between the two is a compile error rather than
 * a gradient that silently renders as the other kind. */
enum avk_gradient_type {
	AVK_GRADIENT_NONE = 0,
	AVK_GRADIENT_LINEAR = 1,
	AVK_GRADIENT_CONIC = 2,
};

/*
 * A gradient, as the renderer receives it: immutable, self-contained, and
 * holding no pointer into anything the compositor might free or mutate.
 *
 * WHAT IS NOT IN HERE, and must not be added: stop positions. SceneFX's
 * gradient is a colour COUNT and nothing else -- spacing is derived from the
 * count, and `blend` decides whether that means `count` hard bands or
 * `count - 1` interpolated segments. There is no position array in the source
 * to carry, and adding one would be a different feature.
 *
 * `degree` is in DEGREES because that is the source semantic (the reference
 * shader calls radians() on it). The conversion happens once, on the way to the
 * GPU, rather than being baked in here where it would quietly diverge from the
 * field it was copied from.
 *
 * The colours live in the scene's packed array, not here: several gradients in
 * one frame share one array and one upload, and a per-gradient allocation on
 * the frame path is exactly what this renderer does not do. `color_offset` is
 * an index in COLOURS into avk_scene.gradient_colors.
 */
struct avk_gradient {
	enum avk_gradient_type type;
	float degree;
	bool blend;
	float origin[2];   /* normalised 0..1 within the command's dst box */
	uint32_t color_offset;
	uint32_t color_count;
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
	 * AVK_CMD_TEXTURE_QUAD only: the four destination corners in OUTPUT
	 * PIXELS, as x0,y0, x1,y1, x2,y2, x3,y3.
	 *
	 * The order is the one gl_VertexIndex walks, so that a strip winds the
	 * same way it does for every other primitive:
	 *
	 *     0 ── 1        0 top-left      1 top-right
	 *     │    │        2 bottom-left   3 bottom-right
	 *     2 ── 3        (of the UNROTATED quad; after a rotation they are
	 *                   simply four points, and "top-left" names which
	 *                   source corner each one carries)
	 *
	 * `dst` must still be set, to the quad's bounding box: it is what the
	 * scissor, the damage arithmetic and the transient-target sizing all read.
	 * A dst that does not contain all four corners clips the quad.
	 */
	float quad[8];

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

	/*
	 * The fill. `type == AVK_GRADIENT_NONE` means `color` above is the whole
	 * answer; anything else means the gradient replaces the colour and only
	 * `color[3]` still applies, which is what the reference does
	 * (quad_grad_round.frag gates the gradient by v_color.a).
	 *
	 * Geometry above, material here, and the two do not know about each other:
	 * that is what lets a gradient BORDER be M4B's annulus with a different
	 * fill rather than a second border path.
	 */
	struct avk_gradient gradient;

	/*
	 * AVK_CMD_SHADOW only, in OUTPUT PIXELS.
	 *
	 * Output pixels and not logical ones, and that is a divergence from the
	 * reference worth stating here as well as in the shader. SceneFX scales a
	 * shadow's BOX and its RADII to output pixels and leaves blur_sigma in
	 * logical units, so on a fractional-scale output the falloff is measured
	 * against a box it no longer matches -- the same window shows a different
	 * softness on a 1.0 and a 1.5 monitor, and drags across the seam changing
	 * shape. AVK scales all three together.
	 *
	 * Two things are derived from it and they are NOT the same number: the
	 * caster is the envelope inset by `blur_sigma`, and the Gaussian's sigma
	 * is `blur_sigma * 0.5`.
	 */
	float blur_sigma;

	/*
	 * AVK_CMD_BLUR only.
	 *
	 * `blur_levels` 0 means the command draws nothing, which is how a scene
	 * disables one blur without removing it. The remaining fields are the
	 * kernel and the post-effects; see avk_blur.h.
	 */
	uint32_t blur_levels;
	float blur_radius;
	float blur_brightness, blur_contrast, blur_saturation, blur_noise;
	bool blur_apply_effects;
	/*
	 * The result may never come out lighter than the source it replaced. See
	 * avk_blur.h -- it exists because a blur of dark-with-bright-detail (a
	 * terminal) raises the mean and reads as a glow.
	 */
	bool blur_darken;
	/*
	 * 0 -- the node's edge is `corners`: a hard rounded-rect SDF with a ~1px
	 * antialiasing band, like any other rounded texture.
	 *
	 * > 0 -- the edge instead fades over the same wide analytic Gaussian a
	 * shadow's tint fades over, with this as the sigma, and `corners` becomes
	 * that falloff's own corner radii rather than a hard radius. The producer
	 * hands a shadow-backdrop blur the SHADOW'S OWN sigma so the two fade in
	 * lockstep and read as one halo.
	 *
	 * IN OUTPUT PIXELS, scaled at the walker beside the box and the radii. The
	 * reference scales this field and does NOT scale a shadow's blur_sigma, so
	 * on a fractional-scale output its two edges disagree; AVK scales both. See
	 * docs/avk-effects.md.
	 */
	float blur_edge_softness;
	/*
	 * Compatibility metadata, in this command's own coordinates.
	 *
	 * SceneFX needs it because its live blur samples the previous frame's FINAL
	 * COMPOSITE, which contains the owning window; it repairs that box by edge
	 * extension. AVK samples the current-frame scene PREFIX, in which the owner
	 * has not been drawn yet -- so the invariant the field exists to guarantee
	 * is already satisfied and no masking is performed.
	 *
	 * Carried rather than dropped so the renderer does not silently lose
	 * awareness of it. If a future producer gives it a different meaning -- a
	 * box of legitimate background to suppress, say -- that is a renderer
	 * contract change and must be made explicitly, not inherited from the name.
	 * See docs/avk-effects.md, "sample_exclude -- the producer table".
	 */
	struct avk_box sample_exclude;
	bool has_sample_exclude;

	/*
	 * M4I. The producer asked for the CACHED BOTTOM LAYER rather than a live
	 * sample of the scene prefix -- SceneFX's should_only_blur_bottom_layer.
	 *
	 * It is a request about this blur's SOURCE, not about its appearance: the
	 * node wants the monitor-wide background blur that was computed once, and
	 * is willing to be wrong about anything drawn above the background. AVK has
	 * always recorded the request and always ignored it, which is not free --
	 * every such node instead replays the scene prefix and runs a private
	 * dual-Kawase chain over it, so a desktop with n of them recomputes the
	 * same background blur n times a frame.
	 *
	 * Carried on the command so the renderer can CLASSIFY a chain by role
	 * (avk_blur_role) with no scene-graph lookup at record time, which is what
	 * makes same-frame attribution possible; whether the request is honoured is
	 * a separate decision made in avk_render.c.
	 */
	bool blur_bottom_only;

	/*
	 * M5/C2. HOW THIS SOURCE'S ELECTRICAL PIXELS BECOME SCENE VALUES.
	 *
	 * Only meaningful on AVK_CMD_TEXTURE -- a rect, a shadow and a blur result
	 * are already scene values by construction, so they have no source domain
	 * to declare and this stays at its zero value for them.
	 *
	 * Carried BY VALUE, 16 bytes, for the same reason blur_bottom_only is: the
	 * renderer must be able to pick a decode variant at record time with no
	 * scene-graph lookup, and a pointer here would be a lifetime question about
	 * a client that can be destroyed between snapshot and submit.
	 *
	 * NOTHING READS IT YET. C7 selects the decode variant from it; until then a
	 * command carries its domain and every draw takes the same path it always
	 * did, which is what makes the field checkable before it is load-bearing.
	 */
	struct az_lum_domain lum;

	/* reserved for M4F */
	bool has_blur;
};

/*
 * WHAT A BLUR CHAIN IS FOR -- the axis that chain COUNT could not express.
 *
 * Frame cost correlated with rebuilt pixels at r=+0.664 and with chain count at
 * only +0.431, because two chains are not two of the same thing: one may be a
 * 200x40 tooltip backdrop and the next a full-screen window's, and averaging
 * them produced a "chains=3 is slower than chains=4" inversion that read as
 * nonsense. Role is the missing key.
 */
enum avk_blur_role {
	/* A window/layer backdrop that asked for the cached bottom layer. Its
	 * source is the monitor background and nothing else, by the producer's own
	 * declaration. */
	AVK_BLUR_ROLE_WINDOW_BACKDROP,
	/* A live blur: its source is genuinely the current scene prefix, including
	 * whatever windows lie beneath it. Never cacheable. */
	AVK_BLUR_ROLE_LIVE,
	/* The monitor-wide background blur node itself. */
	AVK_BLUR_ROLE_MONITOR_BACKGROUND,
};
#define AVK_BLUR_ROLE_COUNT ((int)AVK_BLUR_ROLE_MONITOR_BACKGROUND + 1)

static inline const char *avk_blur_role_name(enum avk_blur_role r) {
	switch (r) {
	case AVK_BLUR_ROLE_WINDOW_BACKDROP:    return "WINDOW_BACKDROP";
	case AVK_BLUR_ROLE_LIVE:               return "LIVE";
	case AVK_BLUR_ROLE_MONITOR_BACKGROUND: return "MONITOR_BACKGROUND";
	}
	return "?";
}

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

	/*
	 * ── PRESENTATION IS NOT SOURCE ────────────────────────────────────────
	 *
	 * `source_bounds` is the region of the scene this frame is able to
	 * RECONSTRUCT, in the same coordinates as everything else here: the
	 * output's own pixels, so it normally starts at a NEGATIVE x or y.
	 *
	 * The target's extent is where pixels can be PRESENTED. A blur is the one
	 * effect for which those are different questions. A blur pixel ten pixels
	 * inside the left edge of an output samples source up to one support
	 * FURTHER left -- which on a multi-output desktop is scene content that
	 * belongs to the monitor next door. Clamping the dependency to the output
	 * would replace it with the edge-clamped colour and put a seam down the
	 * join of every window that spans two displays.
	 *
	 * So the compositor widens this by the blur halo and retains the commands
	 * that fall in it; the renderer reconstructs the halo into THIS output's
	 * device grid, at THIS output's pixel density, from the global scene. It
	 * never samples the neighbouring output's framebuffer -- that would couple
	 * format, scale, presentation history and (in M5) the colour domain, and
	 * the whole current-frame-prefix architecture exists to avoid exactly that.
	 *
	 * A zero box means "the target's extent", which is what a single-output
	 * frame and every renderer test want.
	 */
	struct avk_box source_bounds;

	/*
	 * Every gradient stop in the frame, packed end to end, 4 floats per colour
	 * and PREMULTIPLIED -- the values wlr_scene_rect stores, copied without
	 * conversion. A command's gradient names its own run by offset and count.
	 *
	 * One array for the whole frame rather than one per gradient: the renderer
	 * turns this into a single buffer write, so three gradients cost one upload
	 * and no allocation of any kind on the GPU side.
	 */
	float *gradient_colors;
	uint32_t gradient_color_len;   /* in COLOURS, not floats */
	uint32_t gradient_color_cap;

	/* Cleared before anything is drawn, in the damaged region only. */
	float clear_color[4];
	bool has_clear;

	/*
	 * ── THE MONITOR BACKGROUND BLUR, AS A REQUEST RATHER THAN A COMMAND ───
	 *
	 * SceneFX's WLR_SCENE_NODE_OPTIMIZED_BLUR: blur everything drawn so far,
	 * once, and keep the result for the nodes above to sample. asteroidz puts
	 * it in LyrBlur, one layer above LyrBg, so "everything drawn so far" is the
	 * wallpaper and nothing else -- which is why the result survives frames in
	 * which the entire desktop moved.
	 *
	 * NOT a command, deliberately. A command would have to be given a draw, a
	 * region, an opaque region, an occlusion answer and a class in five
	 * exhaustive switches, and it draws nothing in any of them: it is a
	 * PRODUCER of an off-screen resource, not a thing on the screen. Carrying it
	 * beside the command list keeps every one of those switches honest and makes
	 * the one fact that matters -- where its source range ends -- a field rather
	 * than a search.
	 *
	 * `prefix_end` is the number of commands emitted BEFORE the node was
	 * reached, so the source is scene->cmds[0, prefix_end). Exactly the same
	 * rule a live blur's source obeys; the only difference is that this one's
	 * answer is kept.
	 */
	struct {
		bool present;
		size_t prefix_end;
		/* Output pixels, like every other box here. The cache is allocated at
		 * this extent and invalidated when it changes. */
		struct avk_box bounds;
		uint32_t levels;
		float radius;
		float brightness, contrast, saturation, noise;
		bool apply_effects;
		/*
		 * A MONOTONIC GENERATION, NOT A DIRTY BIT.
		 *
		 * SceneFX exposes `dirty`, a boolean the producer clears on success.
		 * A boolean cannot survive two changes that race a single frame: mark,
		 * render, mark again, clear -- and the second change is lost with the
		 * flag reading clean. The compositor therefore converts every dirty
		 * edge into an increment and never clears anything, so the renderer's
		 * validity test is an equality between two numbers that only ever go
		 * forward.
		 */
		uint64_t generation;
	} blur_cache;
};

void avk_scene_init(struct avk_scene *scene);
void avk_scene_finish(struct avk_scene *scene);

/* Returns a zeroed command with sane defaults (opacity 1, no clip) ready to
 * fill in, or NULL if the list could not grow. */
struct avk_cmd *avk_scene_add(struct avk_scene *scene, enum avk_cmd_type type);

/* Set a command's clip region. Copies, so the caller's region can go away. */
bool avk_cmd_set_clip(struct avk_cmd *cmd, const pixman_region32_t *region);

/*
 * Give `cmd` a gradient fill, copying `count` colours (4 floats each,
 * PREMULTIPLIED) into the scene's packed array.
 *
 * Copies rather than borrows, and that is the point: `colors` belongs to a
 * wlr_scene_rect that a client commit or a config reload may free between the
 * walk and the submission. A snapshot that holds a pointer into the scene graph
 * is not a snapshot.
 *
 * `count == 0` is refused -- a gradient with no colours has no defined colour
 * anywhere, and the reference reads out of range rather than saying so.
 * Returns false and leaves the command's fill alone.
 */
bool avk_cmd_set_gradient(struct avk_scene *scene, struct avk_cmd *cmd,
	enum avk_gradient_type type, float degree, bool blend,
	const float origin[2], const float *colors, uint32_t count);

#endif /* AVK_SCENE_H */
