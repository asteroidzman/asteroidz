#ifndef AVK_PIPELINE_H
#define AVK_PIPELINE_H

#include "../image/avk_image.h"

/*
 * The pipelines AVK draws with, and the descriptor machinery behind them.
 *
 * Two pipelines, one layout, one push-constant block. That is deliberate: a
 * single 80-byte push block covers destination, UV mapping, colour and
 * opacity for both, so switching between a rectangle and a surface is a
 * vkCmdBindPipeline and nothing else -- no descriptor rebind, no uniform
 * buffer, no per-command allocation.
 *
 * Dynamic rendering (Vulkan 1.3 core) rather than VkRenderPass objects.
 * Compositing is one colour attachment with no subpasses; a render-pass object
 * would buy tiler optimisations this renderer cannot use and cost a
 * framebuffer object per output size, cached and invalidated on every mode
 * change. The fx_vk path spent an entire restructure on the fact that its
 * two-subpass render pass could not be split mid-frame to sample what it had
 * drawn. Dynamic rendering has no such shape to be trapped by.
 */

/* Must match the layout in shader/src/quad.vert exactly. Checked at compile
 * time below, because a silent mismatch here reads as a geometry bug. */
struct avk_push_constants {
	float uv_org_dx[4];   /* uv origin xy, du/dx zw */
	float uv_dy[4];       /* du/dy xy, unused zw */
	float color[4];       /* premultiplied */
	/*
	 * opacity, alpha_mask, viewport width, viewport height (output pixels).
	 *
	 * params[1] is read as the alpha mask by the texture pipeline and as the
	 * GRADIENT RECORD INDEX by the gradient pipeline. The two never draw the
	 * same command, and push.glsl names both readings.
	 */
	float params[4];
	/*
	 * OUTER geometry: the rounded rectangle this command fills, in OUTPUT
	 * PIXELS as x0, y0, x1, y1.
	 *
	 * Pixels rather than NDC because the shader evaluates a signed distance
	 * field against gl_FragCoord, and a distance is only meaningful in a space
	 * with uniform units. It is also why a window hanging off the edge of an
	 * output still rounds correctly: both the fragment position and the box are
	 * absolute, so a clipped draw is not a different shape.
	 *
	 * The vertex shader DERIVES its NDC position from this box and the
	 * viewport in params.zw, so the destination rectangle exists exactly once.
	 * It used to be stored twice, once here and once pre-converted to NDC, and
	 * two copies of one rectangle is how M4A's radii nearly ended up scaled in
	 * one place and not the other.
	 */
	float round_box[4];
	/* top-left, top-right, bottom-right, bottom-left, in output pixels.
	 * All zero means no rounding. */
	float corners[4];
	/*
	 * INNER geometry: the rounded rectangle cut OUT of the outer one, same
	 * space and same clockwise corner order. A window border is exactly this
	 * -- a rounded rect minus a rounded rect -- and carrying both as first
	 * class geometry is what makes the border a single analytic primitive
	 * instead of an outline approximated by pieces.
	 *
	 * All-zero inner corners mean "no arcs to subtract"; the square part of
	 * the cut is done by the scissor region, which is exact. Both are needed:
	 * the region cannot express an arc and the shader should not be asked to
	 * shade pixels the scissor could have dropped.
	 */
	float inner_box[4];
	float inner_corners[4];
};
/*
 * Exactly 128, the minimum maxPushConstantsSize every Vulkan implementation
 * must support, so this needs no capability check -- and NO ROOM TO GROW.
 *
 * That ceiling is why the block is dual-purpose from M4C onward: a gradient
 * rect puts its record index in params[1], which only a texture command
 * otherwise uses, and its colours in a storage buffer. shader/src/push.glsl
 * carries the field-by-field table and is the one declaration all four shaders
 * include, so the two readings cannot drift apart.
 */
_Static_assert(sizeof(struct avk_push_constants) == 128,
	"push constants must match the shader block");

struct avk_pipelines {
	struct avk_device *dev;

	VkPipelineLayout layout;
	VkDescriptorSetLayout texture_set_layout;
	/*
	 * Set 1: the frame's packed gradient data (avk_gradient.h). A SECOND SET
	 * rather than a second binding in set 0, because set 0's sets are cached
	 * per image for the image's whole lifetime -- putting the gradient buffer
	 * in there would mean rewriting every cached surface descriptor whenever
	 * the buffer grew, under frames still in flight.
	 */
	VkDescriptorSetLayout gradient_set_layout;

	VkPipeline rect;
	VkPipeline texture;
	/*
	 * M4D. ONE shadow pipeline for every shadow on the desktop: the caster's
	 * geometry and its blur radius are push constants, so a window shadow, a
	 * titlebar shadow and an overview thumbnail's shadow are the same pipeline
	 * with different numbers. Nothing here compiles on the frame path.
	 */
	VkPipeline shadow;
	/*
	 * ONE gradient pipeline. Not one per stop count, not one per gradient type:
	 * the colours come from a storage buffer whose length the shader never
	 * names, and linear-versus-conic is a uniform branch on a value in the
	 * record. The GLES reference relinks its program when a scene wants more
	 * stops than the current build allows; that behaviour has no Vulkan
	 * translation that is not a pipeline compile on the frame path.
	 *
	 * Separate from `rect` rather than a branch inside quad.frag so that a
	 * plain rectangle -- every window background, every panel, the clear --
	 * neither reads a storage buffer nor needs set 1 bound.
	 */
	VkPipeline gradient;
	/*
	 * M4F. Dual-Kawase: one 5-tap downsample and one 8-tap upsample, used for
	 * every level of every blur on the desktop. The level count and the kernel
	 * step are push constants, so a two-pass blur and a five-pass one are the
	 * same two pipelines with different numbers -- nothing here compiles on the
	 * frame path, and there is no pipeline per radius or per image size.
	 *
	 * REPLACING rather than blending: these resample into a transient whose
	 * previous contents belong to another window's blur from three frames ago.
	 */
	VkPipeline blur_down;
	VkPipeline blur_up;

	/* One sampler each, because filtering is a per-command choice and a
	 * sampler is immutable. Nearest for 1:1 blits -- linear at 1:1 is not a
	 * no-op once a destination lands on a half pixel, and it shows up as a
	 * soft desktop nobody can point at. */
	VkSampler nearest;
	VkSampler linear;

	/* Descriptor sets are cached per image (see avk_pipelines_texture_set),
	 * so the frame path allocates none. The pool grows in blocks. */
	VkDescriptorPool *pools;
	uint32_t pool_count;
	uint32_t sets_left_in_current_pool;
};

/*
 * `format` is the colour format the pipelines will render into. Dynamic
 * rendering bakes it into the pipeline, so a second output format needs a
 * second set -- which is a real constraint and the reason this takes it
 * explicitly instead of guessing.
 */
bool avk_pipelines_init(struct avk_pipelines *pipes, struct avk_device *dev,
	VkFormat format);
void avk_pipelines_finish(struct avk_pipelines *pipes);

/*
 * The descriptor set for sampling `image`.
 *
 * Allocated on first use and then cached ON THE IMAGE, so a client surface
 * costs one descriptor write for its whole lifetime rather than one per frame.
 * Returns VK_NULL_HANDLE if a set could not be allocated.
 */
VkDescriptorSet avk_pipelines_texture_set(struct avk_pipelines *pipes,
	struct avk_image *image, bool linear);

#endif /* AVK_PIPELINE_H */
