/*
 * The monitor background blur result cache.
 *
 * See struct avk_blur_cache in ../scene/avk_render.h for the contract. This
 * file owns only the RESOURCE: allocating the cached image, deciding whether
 * the one already held still describes the frame asking for it, and retiring
 * the one it replaces without a CPU wait.
 *
 * Nothing here decides WHEN to rebuild. That is a scene question and it lives
 * in avk_render.c beside the chain declaration, so that the producer and the
 * consumers of the cache are visible in one place.
 */
#include "avk_blur_cache.h"

#include <stdlib.h>
#include <string.h>

#include "../avk.h"
#include "../command/avk_retire.h"
#include "../device/avk_device.h"
#include "../image/avk_image.h"
#include "../image/avk_upload.h"

/*
 * The cached image's usage, and every bit of it is load-bearing.
 *
 * COLOR_ATTACHMENT   the chain's final upsample renders straight into it, so
 *                    there is no full-output copy on a rebuild -- SceneFX's
 *                    fx_vk pays one (copy_effect_image) and AVK does not.
 * SAMPLED            every consumer reads it as an ordinary texture.
 * TRANSFER_SRC       only for the oracle's readback. Requested unconditionally
 *                    because unlike a pooled transient this image is not keyed
 *                    on its usage, so there is no second allocation to diverge
 *                    from the one the milestone measured.
 */
#define AVK_BLUR_CACHE_USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT \
	| VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)

bool avk_blur_params_equal(const struct avk_blur_params *a,
		const struct avk_blur_params *b) {
	/*
	 * Exact equality on the floats, deliberately, with no epsilon.
	 *
	 * These are not measurements: they are values copied unchanged from the
	 * config through blur_data and multiplied by an output scale that is itself
	 * constant between changes. Two frames with the same configuration produce
	 * bit-identical floats, so a difference here IS a change. An epsilon would
	 * only ever serve to keep a cache that no longer matches its parameters,
	 * which is the one outcome this whole comparison exists to prevent.
	 */
	return a->levels == b->levels
		&& a->radius == b->radius
		&& a->brightness == b->brightness
		&& a->contrast == b->contrast
		&& a->saturation == b->saturation
		&& a->noise == b->noise
		&& a->apply_effects == b->apply_effects
		&& a->darken == b->darken;
}

enum avk_blur_cache_reason avk_blur_cache_check(
		const struct avk_blur_cache *cache,
		uint64_t generation, int32_t origin_x, int32_t origin_y,
		uint32_t width, uint32_t height, VkFormat format,
		const struct avk_blur_params *params, bool force_rebuild) {
	if (cache == NULL) {
		return AVK_BLUR_CACHE_NEVER_BUILT;
	}
	/*
	 * FORCED FIRST, so the falsifier cannot be masked by a cache that happens
	 * to be invalid for another reason. A break that reports GENERATION on a
	 * run where it was meant to report FORCED is a break nobody can check.
	 */
	if (force_rebuild) {
		return AVK_BLUR_CACHE_FORCED;
	}
	if (!cache->valid || cache->slots[cache->slot] == NULL) {
		return AVK_BLUR_CACHE_NEVER_BUILT;
	}
	if (cache->format != format) {
		return AVK_BLUR_CACHE_FORMAT;
	}
	if (cache->width != width || cache->height != height
			|| cache->origin_x != origin_x || cache->origin_y != origin_y) {
		return AVK_BLUR_CACHE_GEOMETRY;
	}
	if (!avk_blur_params_equal(&cache->params, params)) {
		return AVK_BLUR_CACHE_PARAMS;
	}
	/*
	 * LAST, and the only test that is about time rather than identity. Ordered
	 * last so that a geometry or parameter change is reported as what it is:
	 * both of those also bump the generation in practice, and reporting them
	 * all as GENERATION would make the reason table useless exactly when it is
	 * needed.
	 */
	if (cache->generation != generation) {
		return AVK_BLUR_CACHE_GENERATION;
	}
	return AVK_BLUR_CACHE_OK;
}

void avk_blur_cache_count_reason(struct avk_blur_cache *cache,
		enum avk_blur_cache_reason r) {
	if (cache == NULL) {
		return;
	}
	switch (r) {
	case AVK_BLUR_CACHE_OK:                                        return;
	case AVK_BLUR_CACHE_NEVER_BUILT: cache->inv_never_built++;     break;
	case AVK_BLUR_CACHE_GENERATION:  cache->inv_generation++;      break;
	case AVK_BLUR_CACHE_GEOMETRY:    cache->inv_geometry++;        break;
	case AVK_BLUR_CACHE_PARAMS:      cache->inv_params++;          break;
	case AVK_BLUR_CACHE_FORMAT:      cache->inv_format++;          break;
	case AVK_BLUR_CACHE_FORCED:      cache->inv_forced++;          break;
	}
	cache->invalidations++;
}

static struct avk_image *cache_image_create(struct avk_device *dev,
		VkFormat format, uint32_t width, uint32_t height,
		VkDeviceSize *out_bytes) {
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = format;
	image->extent = (VkExtent2D){ width, height };
	image->plane_count = 1;
	image->has_alpha = true;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	/* Owned: nothing outside this device can see it, so the graph must not try
	 * to acquire it from or release it to VK_QUEUE_FAMILY_FOREIGN_EXT. */
	image->origin = AVK_IMAGE_OWNED;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = AVK_BLUR_CACHE_USAGE,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (!avk_check(vkCreateImage(dev->dev, &info, NULL, &image->image),
			"vkCreateImage (blur cache)")) {
		free(image);
		return NULL;
	}
	AVK_LIVE_INC(dev, images);

	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(dev->dev, image->image, &reqs);
	uint32_t type = 0;
	if (!avk_find_memory_type(dev, reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		avk_log(AVK_ERROR, "avk blur cache: no device-local memory type");
		avk_image_destroy(dev, image);
		return NULL;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (!avk_check(vkAllocateMemory(dev->dev, &alloc, NULL, &image->memory[0]),
			"vkAllocateMemory (blur cache)")) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	AVK_LIVE_INC(dev, device_memory);
	image->memory_count = 1;
	if (!avk_check(vkBindImageMemory(dev->dev, image->image,
			image->memory[0], 0), "vkBindImageMemory (blur cache)")) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	VkImageViewCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	if (!avk_check(vkCreateImageView(dev->dev, &vi, NULL, &image->view),
			"vkCreateImageView (blur cache)")) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	AVK_LIVE_INC(dev, image_views);
	*out_bytes = reqs.size;
	return image;
}

struct avk_image *avk_blur_cache_next_slot(struct avk_blur_cache *cache,
		struct avk_device *dev, struct avk_retire_queue *retire,
		VkFormat format, uint32_t width, uint32_t height, bool unsafe_reuse) {
	if (cache == NULL || dev == NULL) {
		return NULL;
	}
	/*
	 * ALTERNATE, DO NOT OVERWRITE.
	 *
	 * A rebuild renders into the slot the previous frame was NOT sampling. The
	 * previous frame is the only one that can still be reading, because this
	 * rebuild is recorded into a command buffer submitted after it on the same
	 * queue -- so one spare slot is exactly enough, and a third would be an
	 * output-resolution image that is never read.
	 *
	 * Writing back into the same image instead would be a read-after-write
	 * hazard across frames: legal to record, undetectable in a single-frame
	 * fixture, and visible only as one torn frame of wallpaper whenever a
	 * rebuild happens to land while the previous frame is still in flight.
	 */
	uint32_t next = unsafe_reuse ? cache->slot : (cache->slot ^ 1u);
	struct avk_image *img = cache->slots[next];
	if (img != NULL && (img->format != format
			|| img->extent.width != width || img->extent.height != height)) {
		/* Wrong shape: retire it rather than reuse it. Ownership transfers to
		 * the queue, so the slot must be cleared before anything can fail. */
		cache->slots[next] = NULL;
		cache->slot_bytes[next] = 0;
		/*
		 * Deferred against THE IMAGE'S OWN last use, not against some current
		 * timeline value. Every frame that sampled this image stamped last_use
		 * on it, so this is the exact point past which no submitted command
		 * buffer can still be reading -- and it is the only number that is
		 * right whether the image was read last frame or fifty frames ago.
		 */
		avk_retire_push(retire, dev, img->last_use, avk_image_destroy, img);
		img = NULL;
	}
	if (img == NULL) {
		VkDeviceSize bytes = 0;
		img = cache_image_create(dev, format, width, height, &bytes);
		if (img == NULL) {
			return NULL;
		}
		cache->slots[next] = img;
		cache->slot_bytes[next] = (uint64_t)bytes;
	}
	cache->bytes = cache->slot_bytes[0] + cache->slot_bytes[1];
	cache->slot = next;
	return img;
}

void avk_blur_cache_finish(struct avk_blur_cache *cache,
		struct avk_device *dev, struct avk_retire_queue *retire) {
	if (cache == NULL) {
		return;
	}
	for (uint32_t i = 0; i < 2; i++) {
		struct avk_image *img = cache->slots[i];
		cache->slots[i] = NULL;
		cache->slot_bytes[i] = 0;
		if (img == NULL) {
			continue;
		}
		if (retire != NULL) {
			avk_retire_push(retire, dev, img->last_use, avk_image_destroy, img);
		} else {
			avk_image_destroy(dev, img);
		}
	}
	cache->valid = false;
	cache->bytes = 0;
}
