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
	/* opacity, alpha_mask, viewport width, viewport height (output pixels) */
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
/* Exactly 128, the minimum maxPushConstantsSize every Vulkan implementation
 * must support, so this needs no capability check -- and no room to grow.
 * M4C's gradients carry a variable number of colour stops and want a buffer
 * rather than another push constant, so this is the right ceiling to sit at. */
_Static_assert(sizeof(struct avk_push_constants) == 128,
	"push constants must match the shader block");

struct avk_pipelines {
	struct avk_device *dev;

	VkPipelineLayout layout;
	VkDescriptorSetLayout texture_set_layout;

	VkPipeline rect;
	VkPipeline texture;

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
