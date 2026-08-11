#ifndef AVK_IMAGE_H
#define AVK_IMAGE_H

#include "../device/avk_device.h"

/*
 * A VkImage plus everything needed to reason about it without asking Vulkan
 * again: where its memory came from, what tiling it really has, and what
 * layout it is currently in.
 *
 * The layout field is the one that matters. Vulkan tracks image layout
 * nowhere -- it is entirely the application's job -- and the usual failure is
 * a barrier that transitions from the layout the code *believes* the image is
 * in rather than the one it is actually in, which produces garbage on one
 * driver and nothing at all on another. Keeping it on the object means the
 * render graph can emit barriers from observed state instead of assumed state.
 */

#define AVK_MAX_PLANES 4

/* How the image's memory was obtained. Recorded because "why is this window
 * slow" and "why is this window blank" have the same first question. */
enum avk_image_origin {
	/* Allocated by us. */
	AVK_IMAGE_OWNED,
	/* Imported zero-copy from a dma-buf whose modifier the client stated. */
	AVK_IMAGE_DMABUF_EXPLICIT,
	/* Imported zero-copy from a dma-buf that arrived with
	 * DRM_FORMAT_MOD_INVALID, whose real modifier was recovered. */
	AVK_IMAGE_DMABUF_RECOVERED,
	/* Filled by copying, because no zero-copy path existed. Correct but
	 * costs a copy per frame. */
	AVK_IMAGE_DMABUF_COPIED,
};

struct avk_image {
	struct avk_device *dev;

	VkImage image;
	VkImageView view;

	/* One entry unless the image is disjoint, in which case one per plane. */
	VkDeviceMemory memory[AVK_MAX_PLANES];
	uint32_t memory_count;

	VkFormat format;
	VkExtent2D extent;
	uint64_t modifier;
	uint32_t drm_format;
	uint32_t plane_count;
	bool disjoint;
	/* False for the DRM X variants: the alpha channel exists in memory and
	 * means nothing, so whatever samples this must substitute 1.0. */
	bool has_alpha;

	enum avk_image_origin origin;

	/* Current layout and the last timeline point that touched the image.
	 * Both are maintained by whoever records commands against it. */
	VkImageLayout layout;
	uint64_t last_use;

	/* Descriptor sets for sampling this image, [0] nearest and [1] linear,
	 * allocated lazily and cached HERE rather than per frame. A client
	 * surface is sampled every frame for as long as its window is open, so
	 * writing its descriptor once instead of 144 times a second is the
	 * difference between descriptor work being invisible and being a
	 * profile entry. Owned by the image; freed with the pool. */
	VkDescriptorSet sampler_set[2];

	/* Ownership state and identity. See avk_image_destroy(). The enum values
	 * are deliberately not 0 and 1: a freed chunk that happens to be zeroed
	 * must not read as LIVE. */
	enum avk_image_life {
		AVK_IMAGE_LIVE = 0x41564b4c,      /* 'AVKL' */
		AVK_IMAGE_DESTROYED = 0x41564b44, /* 'AVKD' */
	} life;
	uint64_t id;
};

/*
 * Is this image's memory owned by something outside this Vulkan device?
 *
 * A dma-buf imported from a client -- or handed to KMS for scan-out -- is
 * written and read by an owner Vulkan knows nothing about. Such an image has
 * to be ACQUIRED from VK_QUEUE_FAMILY_FOREIGN_EXT before it is used and
 * RELEASED back afterwards, and it lives in VK_IMAGE_LAYOUT_GENERAL in
 * between.
 *
 * The RELEASE half is what makes a frame visible on a real display. A scan-out
 * buffer that is never released back to VK_QUEUE_FAMILY_FOREIGN_EXT is handed
 * to KMS in a state the display engine cannot interpret, and on a compressed
 * AMD modifier the monitor comes up flat white -- with every window rendered
 * correctly inside an image nothing can read. That was shipped once, on a
 * headless test suite that passed throughout, because nothing scans a headless
 * buffer out. Headless proves composition; only a display proves presentation.
 *
 * The ACQUIRE half has no measurable effect on this machine (RADV, Navi31),
 * which is why AVK_NO_FOREIGN_ACQUIRE=1 exists -- but note that the switch
 * disables both halves, so it is not a subtle knob: it turns the desktop
 * white.
 *
 * An image we allocated ourselves (AVK_IMAGE_OWNED, or a dma-buf we copied
 * out of) has no foreign owner and must NOT be transferred, because there is
 * nothing to transfer it from.
 */
static inline bool avk_image_is_foreign(const struct avk_image *image) {
	return image->origin == AVK_IMAGE_DMABUF_EXPLICIT ||
		image->origin == AVK_IMAGE_DMABUF_RECOVERED;
}

/*
 * Destroy the image and its memory.
 *
 * This is the raw destructor and it does NOT wait for the GPU. Use it as the
 * callback to avk_retire_push() -- its signature matches avk_retire_fn on
 * purpose -- or call it directly only when the GPU is known to be idle.
 */
void avk_image_destroy(struct avk_device *dev, void *image);

/* Allocate a zeroed avk_image with its identity and ownership state set. Every
 * avk_image must come from here -- a bare calloc() produces one whose `life`
 * is 0, which avk_image_destroy() correctly refuses to destroy. */
struct avk_image *avk_image_alloc(struct avk_device *dev);

#endif /* AVK_IMAGE_H */
