#define _POSIX_C_SOURCE 200809L

#include "avk_transient.h"

#include "../avk.h"
#include "../image/avk_upload.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

uint32_t avk_transient_round(uint32_t v, uint32_t granularity) {
	if (granularity <= 1) {
		return v;
	}
	uint32_t r = v % granularity;
	return r == 0 ? v : v + (granularity - r);
}

void avk_transient_pool_init(struct avk_transient_pool *pool,
		struct avk_device *dev, struct avk_retire_queue *retire) {
	memset(pool, 0, sizeof(*pool));
	pool->dev = dev;
	pool->retire = retire;

	pool->granularity = 64;
	const char *g = getenv("AZ_TRANSIENT_GRANULARITY");
	if (g != NULL) {
		long v = strtol(g, NULL, 10);
		if (v >= 1 && v <= 4096) {
			pool->granularity = (uint32_t)v;
		}
	}

	/*
	 * 256 MB. Generous against what M4F will actually hold -- a couple of
	 * full-screen intermediates per output is under 70 MB at 3840x2160 -- and
	 * finite, which is the point. An unbounded cache is a leak that only
	 * presents on the machine with the most monitors.
	 */
	pool->budget = 256ull * 1024 * 1024;
	const char *b = getenv("AZ_TRANSIENT_BUDGET_MB");
	if (b != NULL) {
		long v = strtol(b, NULL, 10);
		if (v >= 1) {
			pool->budget = (VkDeviceSize)v * 1024 * 1024;
		}
	}

	pool->poison = getenv("AZ_TRANSIENT_POISON") != NULL;
	if (pool->poison) {
		avk_log(AVK_WARN, "AZ_TRANSIENT_POISON=1 -- new transients are filled "
			"with magenta so that sampling outside a written region shows");
	}
	pool->trace = getenv("AZ_TRANSIENT_TRACE") != NULL;
	pool->break_early_reuse = getenv("AZ_TRANSIENT_EARLY_REUSE") != NULL;
	if (pool->break_early_reuse) {
		avk_log(AVK_ERROR, "M4E break switch active: the transient pool will "
			"hand out images the GPU has not finished with");
	}
}

static void destroy_entry(struct avk_transient_pool *pool,
		struct avk_transient_entry *entry) {
	if (entry->image == NULL) {
		return;
	}
	pool->stats.bytes -= entry->bytes;
	/*
	 * THROUGH THE RETIRE QUEUE, against the entry's own last_use, and never
	 * destroyed in place. The image being dropped is very often the one two
	 * frames ahead of the GPU are still reading -- an output that changed mode
	 * drops every transient it had while the previous frame is still in flight.
	 * This is the M3.5 staging lesson applied to a third resource.
	 */
	if (pool->retire != NULL) {
		avk_retire_push(pool->retire, pool->dev, entry->last_use,
			avk_image_destroy, entry->image);
	} else {
		avk_image_destroy(pool->dev, entry->image);
	}
	entry->image = NULL;
	pool->stats.retires++;
}

void avk_transient_pool_finish(struct avk_transient_pool *pool) {
	if (pool->dev == NULL) {
		return;
	}
	for (uint32_t i = 0; i < pool->len; i++) {
		if (pool->entries[i].image != NULL) {
			avk_image_destroy(pool->dev, pool->entries[i].image);
		}
	}
	free(pool->entries);
	memset(pool, 0, sizeof(*pool));
}

static bool key_eq(const struct avk_transient_key *a,
		const struct avk_transient_key *b) {
	return a->format == b->format && a->width == b->width
		&& a->height == b->height && a->usage == b->usage
		&& a->samples == b->samples && a->type == b->type
		&& a->tiling == b->tiling;
}

static struct avk_image *create_image(struct avk_transient_pool *pool,
		const struct avk_transient_key *key, VkDeviceSize *out_bytes) {
	struct avk_device *dev = pool->dev;
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = key->format;
	image->extent = (VkExtent2D){ key->width, key->height };
	image->plane_count = 1;
	image->has_alpha = true;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	/*
	 * OWNED, and that matters beyond bookkeeping: avk_image_is_foreign() is
	 * false for it, so the graph will not try to acquire it from
	 * VK_QUEUE_FAMILY_FOREIGN_EXT or release it back. A transient has no owner
	 * outside this device and transferring it to one would be a lie about
	 * memory nothing else can see.
	 */
	image->origin = AVK_IMAGE_OWNED;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = key->type,
		.format = key->format,
		.extent = { key->width, key->height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = key->samples,
		.tiling = key->tiling,
		/*
		 * TRANSFER_DST is added ONLY under the poison switch, and only to the
		 * VkImage -- not to the key. vkCmdClearColorImage requires it, and an
		 * image with MORE usage is still substitutable for one with less, so
		 * key equality is unaffected and a poisoned run pools exactly as a
		 * normal one does.
		 */
		.usage = pool->poison
			? (key->usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT) : key->usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (!avk_check(vkCreateImage(dev->dev, &info, NULL, &image->image),
			"vkCreateImage (transient)")) {
		free(image);
		return NULL;
	}
	AVK_LIVE_INC(dev, images);

	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(dev->dev, image->image, &reqs);
	uint32_t type = 0;
	if (!avk_find_memory_type(dev, reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		avk_log(AVK_ERROR, "avk transient: no device-local memory type");
		avk_image_destroy(dev, image);
		return NULL;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (!avk_check(vkAllocateMemory(dev->dev, &alloc, NULL, &image->memory[0]),
			"vkAllocateMemory (transient)")) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	AVK_LIVE_INC(dev, device_memory);
	image->memory_count = 1;
	if (!avk_check(vkBindImageMemory(dev->dev, image->image,
			image->memory[0], 0), "vkBindImageMemory (transient)")) {
		avk_image_destroy(dev, image);
		return NULL;
	}

	/*
	 * The view, created ONCE with the image and cached on it. A transient is
	 * both rendered into and sampled, so it needs one either way, and creating
	 * it per frame would be a per-frame driver allocation of exactly the kind
	 * the pool exists to remove -- smaller than a VkImage and just as much on
	 * the deadline path.
	 */
	VkImageViewCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = key->format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	if (!avk_check(vkCreateImageView(dev->dev, &vi, NULL, &image->view),
			"vkCreateImageView (transient)")) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	AVK_LIVE_INC(dev, image_views);

	*out_bytes = reqs.size;

	if (pool->poison) {
		/*
		 * Magenta, opaque, over the WHOLE allocation -- including the padding a
		 * caller's logical extent does not cover. Submitted and waited on here
		 * rather than folded into the frame: this runs once per image, only
		 * under the test switch, and doing it inline keeps the poison out of
		 * the frame path entirely.
		 */
		struct avk_cmd_ring ring;
		if (avk_cmd_ring_init(&ring, dev, "transient-poison")) {
			VkCommandBuffer cb = avk_cmd_ring_begin(&ring);
			if (cb != VK_NULL_HANDLE) {
				VkImageMemoryBarrier2 b = {
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
					.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
					.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
					.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
					.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = image->image,
					.subresourceRange = {
						VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
				};
				VkDependencyInfo dep = {
					.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
					.imageMemoryBarrierCount = 1,
					.pImageMemoryBarriers = &b,
				};
				vkCmdPipelineBarrier2(cb, &dep);
				VkClearColorValue magenta = {
					.float32 = { 1.0f, 0.0f, 1.0f, 1.0f } };
				VkImageSubresourceRange range = {
					VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkCmdClearColorImage(cb, image->image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &range);
				image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				uint64_t v = avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0);
				if (v != 0) {
					avk_device_timeline_wait(dev, v, 2000000000ULL);
				}
			}
			avk_cmd_ring_finish(&ring);
		}
	}
	return image;
}

struct avk_image *avk_transient_acquire(struct avk_transient_pool *pool,
		VkFormat format, uint32_t width, uint32_t height,
		VkImageUsageFlags usage) {
	if (pool->dev == NULL || width == 0 || height == 0) {
		return NULL;
	}
	struct avk_transient_key key = {
		.format = format,
		.width = avk_transient_round(width, pool->granularity),
		.height = avk_transient_round(height, pool->granularity),
		.usage = usage,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
	};
	pool->stats.acquires++;

	/* One timeline read for the whole acquire, not one per candidate. */
	uint64_t completed = avk_device_timeline_value(pool->dev);

	/*
	 * WHY a miss happened, not just that one did.
	 *
	 * "80-130 images created during one tag transition" is not actionable
	 * while every miss is the same category. An entry can be unavailable for
	 * three quite different reasons and they have three different fixes:
	 * nothing of that shape exists (extent churn -> quantise or grow), one
	 * exists but another chain in THIS frame holds it (concurrency -> more
	 * instances are genuinely needed), or one exists and is idle but still in
	 * flight on the GPU (pipeline depth -> more instances, or wait, which
	 * this pool deliberately never does).
	 */
	bool saw_key = false, saw_in_use = false, saw_in_flight = false;

	for (uint32_t i = 0; i < pool->len; i++) {
		struct avk_transient_entry *e = &pool->entries[i];
		if (e->image == NULL || !key_eq(&e->key, &key)) {
			continue;
		}
		saw_key = true;
		if (e->in_use) {
			saw_in_use = true;
			continue;
		}
		bool ready = e->last_use <= completed;
		if (!ready) {
			saw_in_flight = true;
		}
		if (!ready && !pool->break_early_reuse) {
			/* Still in flight. NOT a reason to wait -- fall through and
			 * allocate. A pool that blocked here would put a CPU wait on the
			 * frame path to save memory, which is the wrong trade in a
			 * compositor and the one AVK has never made. */
			continue;
		}
		if (!ready) {
			pool->stats.unsafe_reuses++;
		}
		e->in_use = true;
		e->last_frame = pool->frame;
		pool->stats.reuses++;
		return e->image;
	}

	/* Nothing compatible and free. Grow. */
	if (pool->len == pool->cap) {
		uint32_t next = pool->cap == 0 ? 4 : pool->cap * 2;
		struct avk_transient_entry *grown =
			realloc(pool->entries, (size_t)next * sizeof(*grown));
		if (grown == NULL) {
			return NULL;
		}
		pool->entries = grown;
		pool->cap = next;
	}
	const char *why = !saw_key ? "NO_KEY_MATCH"
		: saw_in_use ? "ALL_IN_USE_THIS_FRAME"
		: saw_in_flight ? "IN_FLIGHT_ON_GPU" : "OTHER";
	pool->stats.miss_no_key += !saw_key ? 1 : 0;
	pool->stats.miss_in_use += (saw_key && saw_in_use) ? 1 : 0;
	pool->stats.miss_in_flight += (saw_key && !saw_in_use && saw_in_flight) ? 1 : 0;
	if (pool->trace) {
		avk_log(AVK_INFO, "aztrans create why=%s key=%ux%u fmt=%d usage=0x%x "
			"pool_len=%u req=%ux%u", why, key.width, key.height,
			(int)key.format, (unsigned)key.usage, pool->len, width, height);
	}
	VkDeviceSize bytes = 0;
	struct avk_image *image = create_image(pool, &key, &bytes);
	if (image == NULL) {
		return NULL;
	}
	pool->entries[pool->len] = (struct avk_transient_entry){
		.key = key,
		.image = image,
		.bytes = bytes,
		.last_use = 0,
		.last_frame = pool->frame,
		.in_use = true,
	};
	pool->len++;
	pool->stats.creates++;
	pool->stats.bytes += bytes;
	if (pool->stats.bytes > pool->stats.peak_bytes) {
		pool->stats.peak_bytes = pool->stats.bytes;
	}
	pool->stats.live = pool->len;
	if (pool->stats.live > pool->stats.peak_live) {
		pool->stats.peak_live = pool->stats.live;
	}
	return image;
}

void avk_transient_release_frame(struct avk_transient_pool *pool,
		uint64_t timeline_value) {
	for (uint32_t i = 0; i < pool->len; i++) {
		struct avk_transient_entry *e = &pool->entries[i];
		if (!e->in_use) {
			continue;
		}
		e->in_use = false;
		e->last_use = timeline_value;
		/* The image records it too, so anything that retires the image without
		 * going through the pool still has the right timeline point. */
		if (e->image != NULL) {
			e->image->last_use = timeline_value;
		}
	}
	pool->frame++;
}

/* Compact away the holes destroy_entry() leaves. */
static void compact(struct avk_transient_pool *pool) {
	uint32_t out = 0;
	for (uint32_t i = 0; i < pool->len; i++) {
		if (pool->entries[i].image != NULL) {
			if (out != i) {
				pool->entries[out] = pool->entries[i];
			}
			out++;
		}
	}
	pool->len = out;
	pool->stats.live = out;
}

void avk_transient_pool_collect(struct avk_transient_pool *pool) {
	if (pool->dev == NULL || pool->len == 0) {
		return;
	}
	bool dropped = false;
	for (uint32_t i = 0; i < pool->len; i++) {
		struct avk_transient_entry *e = &pool->entries[i];
		if (e->image == NULL || e->in_use) {
			continue;
		}
		if (pool->frame - e->last_frame > AVK_TRANSIENT_IDLE_FRAMES) {
			destroy_entry(pool, e);
			dropped = true;
		}
	}

	/*
	 * Over budget: retire idle entries oldest-first until under it.
	 *
	 * Oldest-first and not largest-first, deliberately. The entry least
	 * recently asked for is the one least likely to be asked for again; the
	 * largest is very often the full-screen intermediate that every frame
	 * wants. Evicting by size would reliably throw away the one image whose
	 * reallocation costs most.
	 */
	while (pool->stats.bytes > pool->budget) {
		struct avk_transient_entry *oldest = NULL;
		for (uint32_t i = 0; i < pool->len; i++) {
			struct avk_transient_entry *e = &pool->entries[i];
			if (e->image == NULL || e->in_use) {
				continue;
			}
			if (oldest == NULL || e->last_frame < oldest->last_frame) {
				oldest = e;
			}
		}
		if (oldest == NULL) {
			/* Everything left is in flight. Over budget for now rather than
			 * destroying something the GPU is reading; the next collect gets
			 * it. */
			break;
		}
		destroy_entry(pool, oldest);
		dropped = true;
	}

	if (dropped) {
		compact(pool);
	}
}

void avk_transient_pool_drop_all(struct avk_transient_pool *pool) {
	if (pool->dev == NULL) {
		return;
	}
	for (uint32_t i = 0; i < pool->len; i++) {
		if (pool->entries[i].image != NULL) {
			destroy_entry(pool, &pool->entries[i]);
		}
	}
	compact(pool);
}
