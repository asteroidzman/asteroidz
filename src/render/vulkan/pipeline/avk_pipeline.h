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
	float dst[4];         /* x0, y0, x1, y1 in NDC */
	float uv_org_dx[4];   /* uv origin xy, du/dx zw */
	float uv_dy[4];       /* du/dy xy, unused zw */
	float color[4];       /* premultiplied */
	float params[4];      /* opacity, alpha_mask, unused, unused */
	/*
	 * The rounded-corner rectangle, in OUTPUT PIXELS: x0, y0, x1, y1.
	 *
	 * Pixels rather than NDC because the shader evaluates a signed distance
	 * field against gl_FragCoord, and a distance is only meaningful in a space
	 * with uniform units. It is also why a window hanging off the edge of an
	 * output still rounds correctly: both the fragment position and the box are
	 * absolute, so a clipped draw is not a different shape.
	 */
	float round_box[4];
	/* top-left, top-right, bottom-right, bottom-left, in output pixels.
	 * All zero means no rounding. */
	float corners[4];
};
/* 112 <= 128, the minimum maxPushConstantsSize every Vulkan implementation
 * must support, so this needs no capability check. */
_Static_assert(sizeof(struct avk_push_constants) == 112,
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
