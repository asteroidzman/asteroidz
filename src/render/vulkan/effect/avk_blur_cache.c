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
		&& a->linear_src == b->linear_src
		&& a->darken == b->darken;
}

/*
 * FNV-1a over the fields that decide the prefix's pixels.
 *
 * Not a cryptographic digest and it does not need to be: the alternative it is
 * being compared against is a counter that could be wrong in one direction
 * only, and a 64-bit collision between two consecutive wallpapers is not a
 * failure mode anybody will meet. What it must not do is MISS a change, so it
 * takes the image POINTER (a re-imported buffer is a different avk_image, which
 * is exactly the wallpaper-swap case) alongside the geometry, rather than
 * trying to be clever about content.
 */
static inline uint64_t hash_bytes(uint64_t h, const void *p, size_t n) {
	const uint8_t *b = p;
	for (size_t i = 0; i < n; i++) {
		h = (h ^ b[i]) * 0x100000001b3ULL;
	}
	return h;
}

uint64_t avk_blur_cache_source_hash(const struct avk_cmd *cmds, size_t len) {
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < len; i++) {
		const struct avk_cmd *c = &cmds[i];
		uint32_t type = (uint32_t)c->type;
		h = hash_bytes(h, &type, sizeof(type));
		h = hash_bytes(h, &c->dst, sizeof(c->dst));
		h = hash_bytes(h, &c->opacity, sizeof(c->opacity));
		h = hash_bytes(h, c->color, sizeof(c->color));
		/*
		 * The identity of the sampled image, and the window into it. A
		 * wallpaper swap changes the first; a crop or transform change changes
		 * the second.
		 *
		 * avk_image.id, NOT the pointer. Images are freed and reallocated
		 * constantly, so a heap address recycled for the new wallpaper would
		 * hash the same as the old one -- an ABA that would make this whole
		 * check silently useless in exactly the case it exists for. `id` comes
		 * from a monotonic per-device counter and is never reused.
		 */
		uint64_t img = c->image != NULL ? c->image->id : 0;
		h = hash_bytes(h, &img, sizeof(img));
		/*
		 * AND WHAT IS IN IT, which is not the same question.
		 *
		 * A dmabuf wallpaper swap gives a new buffer and therefore a new `id`.
		 * A CPU-path client that repaints into the SAME shm buffer does not:
		 * the importer's cache returns the same avk_image, the id is unchanged,
		 * and only the pixels moved. Hashing identity alone would miss exactly
		 * that case and leave this check useless for a whole class of client.
		 */
		uint64_t seq = c->image != NULL ? c->image->content_seq : 0;
		h = hash_bytes(h, &seq, sizeof(seq));
		h = hash_bytes(h, &c->src, sizeof(c->src));
		uint32_t tf = (uint32_t)c->transform;
		h = hash_bytes(h, &tf, sizeof(tf));
	}
	/* Reserve 0 for "no prefix", so an empty background and an unhashed one
	 * are distinguishable in the trace. */
	return h == 0 ? 1 : h;
}

enum avk_blur_cache_reason avk_blur_cache_check(
		const struct avk_blur_cache *cache,
		uint64_t generation, uint64_t source_hash,
		int32_t origin_x, int32_t origin_y,
		uint32_t width, uint32_t height, VkFormat format,
		const struct avk_blur_params *params, enum avk_blur_cache_kind kind,
		bool force_rebuild) {
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
	if (!cache->img[kind].valid || cache->img[kind].image == NULL) {
		return AVK_BLUR_CACHE_NEVER_BUILT;
	}
	if (cache->format != format) {
		return AVK_BLUR_CACHE_FORMAT;
	}
	if (cache->width != width || cache->height != height
			|| cache->origin_x != origin_x || cache->origin_y != origin_y) {
		return AVK_BLUR_CACHE_GEOMETRY;
	}
	if (!avk_blur_params_equal(&cache->img[kind].params, params)) {
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
	/*
	 * AFTER the generation, so that an ordinary notified change is still
	 * reported as GENERATION and the reason table keeps its meaning. SOURCE
	 * therefore names exactly one thing: the background changed and nothing
	 * told us. That is a defect somewhere upstream, and a non-zero
	 * blur_cache_inv_source is how it becomes visible instead of becoming a
	 * stale picture.
	 *
	 * WHICH MEANS AZ_BLUR_CACHE_IGNORE_DIRTY CANNOT TEST THIS. This function
	 * returns at the first disagreement, so when the generation has moved it
	 * returns GENERATION and never evaluates the line below; suppressing that
	 * reason afterwards leaves the source comparison unreached, and a fixture
	 * built on it reads inv_source == 0 and concludes the digest is inert. The
	 * break for this rule is AZ_BLUR_CACHE_NO_DIRTY_EDGE, which drops the
	 * notification so the generations still agree -- which is also the shipped
	 * defect rather than an invented one.
	 */
	if (cache->source_hash != source_hash) {
		return AVK_BLUR_CACHE_SOURCE;
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
	case AVK_BLUR_CACHE_SOURCE:      cache->inv_source++;          break;
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

struct avk_image *avk_blur_cache_target(struct avk_blur_cache *cache,
		enum avk_blur_cache_kind kind, struct avk_device *dev,
		struct avk_retire_queue *retire, VkFormat format, uint32_t width,
		uint32_t height) {
	if (cache == NULL || dev == NULL) {
		return NULL;
	}
	struct avk_blur_cache_image *ci = &cache->img[kind];
	/*
	 * ONE IMAGE. The rebuild renders into the same image the previous frame
	 * sampled, and that is ordered rather than raced -- see
	 * struct avk_blur_cache_image for why, and for the break that established
	 * it. What orders it is the graph's own layout transition out of
	 * SHADER_READ_ONLY_OPTIMAL, whose first synchronisation scope covers every
	 * command already submitted to this device's single graphics queue.
	 */
	struct avk_image *img = ci->image;
	if (img != NULL && (img->format != format
			|| img->extent.width != width || img->extent.height != height)) {
		/* Wrong shape: retire it rather than reuse it. Ownership transfers to
		 * the queue, so the slot must be cleared before anything can fail. */
		ci->image = NULL;
		ci->image_bytes = 0;
		ci->valid = false;
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
		ci->image = img;
		ci->image_bytes = (uint64_t)bytes;
	}
	cache->bytes = 0;
	for (int k = 0; k < AVK_BLUR_CACHE_KINDS; k++) {
		cache->bytes += cache->img[k].image_bytes;
	}
	return img;
}

void avk_blur_cache_inventory(const struct avk_blur_cache *cache,
		struct avk_blur_cache_inventory *out) {
	if (cache == NULL || out == NULL) {
		return;
	}
	for (int k = 0; k < AVK_BLUR_CACHE_KINDS; k++) {
		const struct avk_blur_cache_image *ci = &cache->img[k];
		if (ci->image == NULL) {
			continue;
		}
		out->images++;
		out->req_bytes += ci->image_bytes;
		/*
		 * LOGICAL TEXEL BYTES: width x height x the format's block size, with
		 * no tiling, no alignment and no metadata. Reported beside req_bytes
		 * rather than instead of it, because the difference between the two IS
		 * the answer to "why is the cache bigger than the arithmetic" -- and a
		 * single number that silently folds driver padding into the extent is
		 * what made the 39,387,136 figure unreconcilable in the first place.
		 */
		out->texel_bytes += (uint64_t)ci->image->extent.width
			* ci->image->extent.height
			* avk_format_bytes_per_pixel(ci->image->format);
	}
}

void avk_blur_cache_finish(struct avk_blur_cache *cache,
		struct avk_device *dev, struct avk_retire_queue *retire) {
	if (cache == NULL) {
		return;
	}
	for (int k = 0; k < AVK_BLUR_CACHE_KINDS; k++) {
		struct avk_blur_cache_image *ci = &cache->img[k];
		struct avk_image *img = ci->image;
		ci->image = NULL;
		ci->image_bytes = 0;
		ci->valid = false;
		ci->wanted = false;
		if (img == NULL) {
			continue;
		}
		if (retire != NULL) {
			avk_retire_push(retire, dev, img->last_use, avk_image_destroy, img);
		} else {
			avk_image_destroy(dev, img);
		}
	}
	cache->bytes = 0;
}
