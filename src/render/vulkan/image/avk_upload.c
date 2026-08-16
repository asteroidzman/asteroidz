#include "avk_upload.h"

#include "../dmabuf/avk_drm_format.h"

#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

bool avk_find_memory_type(struct avk_device *dev, uint32_t type_bits,
		VkMemoryPropertyFlags want, uint32_t *out) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(dev->phys, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if (!(type_bits & (1u << i))) {
			continue;
		}
		if ((props.memoryTypes[i].propertyFlags & want) == want) {
			*out = i;
			return true;
		}
	}
	return false;
}

uint32_t avk_format_bytes_per_pixel(VkFormat format) {
	switch (format) {
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		return 4;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	case VK_FORMAT_R16G16B16A16_UNORM:
		return 8;
	case VK_FORMAT_R5G6B5_UNORM_PACK16:
		return 2;
	default:
		/* Planar and anything else: no single bytes-per-pixel exists, so the
		 * caller refuses rather than computing a wrong size. */
		return 0;
	}
}

static struct avk_image *upload_image_create(struct avk_device *dev,
		uint32_t drm_format, uint32_t width, uint32_t height, uint32_t depth,
		enum avk_image_origin origin) {
	const struct avk_drm_format *fmt = avk_drm_format_from_fourcc(drm_format);
	if (fmt == NULL) {
		char name[64];
		avk_drm_format_name(drm_format, name, sizeof(name));
		avk_log(AVK_ERROR, "cannot upload %s: avk has no mapping for it", name);
		return NULL;
	}
	if (avk_format_bytes_per_pixel(fmt->vk) == 0) {
		char name[64];
		avk_drm_format_name(drm_format, name, sizeof(name));
		avk_log(AVK_ERROR, "cannot upload %s: no single bytes-per-pixel",
			name);
		return NULL;
	}
	if (width == 0 || height == 0 || depth == 0) {
		avk_log(AVK_ERROR, "cannot upload a %ux%ux%u image", width, height,
			depth);
		return NULL;
	}

	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = fmt->vk;
	image->drm_format = drm_format;
	image->extent = (VkExtent2D){ width, height };
	image->depth = depth;
	/* Not a lie about a dma-buf: this image genuinely has no DRM modifier,
	 * because it was never allocated through DRM. */
	image->modifier = DRM_FORMAT_MOD_INVALID;
	image->plane_count = 1;
	image->has_alpha = fmt->has_alpha;
	image->origin = origin;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
		.format = fmt->vk,
		.extent = { width, height, depth },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		/* OPTIMAL, because nothing external touches this image -- it is ours,
		 * so it may as well be laid out the way the GPU samples fastest. */
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT
			| VK_IMAGE_USAGE_TRANSFER_DST_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkResult res = vkCreateImage(dev->dev, &info, NULL, &image->image);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateImage (upload)");
		goto error;
	}
	AVK_LIVE_INC(dev, images);

	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(dev->dev, image->image, &reqs);
	uint32_t type = 0;
	if (!avk_find_memory_type(dev, reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		avk_log(AVK_ERROR, "upload: no device-local memory type");
		goto error;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	res = vkAllocateMemory(dev->dev, &alloc, NULL, &image->memory[0]);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkAllocateMemory (upload image)");
		goto error;
	}
	AVK_LIVE_INC(dev, device_memory);
	image->memory_count = 1;
	res = vkBindImageMemory(dev->dev, image->image, image->memory[0], 0);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkBindImageMemory (upload image)");
		goto error;
	}

	/* No image view here on purpose: avk_pipelines_texture_set() creates one
	 * lazily and caches it on the image, so there is exactly one place that
	 * decides what a sampled view looks like. */
	return image;

error:
	avk_image_destroy(dev, image);
	return NULL;
}

struct avk_image *avk_upload_image_create(struct avk_device *dev,
		uint32_t drm_format, uint32_t width, uint32_t height,
		enum avk_image_origin origin) {
	return upload_image_create(dev, drm_format, width, height, 1, origin);
}

struct avk_image *avk_upload_image_create_3d(struct avk_device *dev,
		uint32_t drm_format, uint32_t dim, enum avk_image_origin origin) {
	return upload_image_create(dev, drm_format, dim, dim, dim, origin);
}

bool avk_upload_staging_ensure(struct avk_device *dev, struct avk_upload *up,
		struct avk_retire_queue *retire, VkDeviceSize size) {
	if (up->buffer != VK_NULL_HANDLE && up->size >= size) {
		return true;
	}
	/*
	 * Grow, never shrink: a window that resizes back and forth should not
	 * reallocate on every wobble.
	 *
	 * The old buffer may still be the source of a copy the GPU has not run
	 * yet, and destroying it here would be destroying it in use -- the exact
	 * thing the retire queue exists to prevent, done in the one place that
	 * did not use it. Refusing to grow is not an option (the upload would be
	 * wrong), and waiting is not an option (this is reached from a client
	 * commit), so the old buffer is handed to the retire queue against the
	 * submission that read it and a fresh one is built alongside.
	 */
	if (up->buffer != VK_NULL_HANDLE && retire != NULL) {
		struct avk_upload *old = calloc(1, sizeof(*old));
		if (old != NULL) {
			*old = *up;
			AVK_LIVE_INC(dev, avk_uploads);
			memset(up, 0, sizeof(*up));
			avk_retire_push(retire, dev, old->last_use, avk_upload_retire,
				old);
		} else {
			/* Out of memory. Destroying in place is the lesser evil of the
			 * two remaining, and it is loud rather than silent. */
			avk_log(AVK_ERROR, "staging: cannot defer the old buffer's "
				"destruction; destroying it in place, which may race a copy "
				"the GPU has not run yet");
			avk_upload_finish(dev, up);
		}
	} else {
		avk_upload_finish(dev, up);
	}

	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkResult res = vkCreateBuffer(dev->dev, &info, NULL, &up->buffer);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateBuffer (staging)");
		return false;
	}
	AVK_LIVE_INC(dev, buffers);
	VkMemoryRequirements reqs;
	vkGetBufferMemoryRequirements(dev->dev, up->buffer, &reqs);
	uint32_t type = 0;
	if (!avk_find_memory_type(dev, reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
			| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
		avk_log(AVK_ERROR, "staging: no host-visible coherent memory type");
		goto error;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	res = vkAllocateMemory(dev->dev, &alloc, NULL, &up->memory);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkAllocateMemory (staging)");
		goto error;
	}
	AVK_LIVE_INC(dev, device_memory);
	res = vkBindBufferMemory(dev->dev, up->buffer, up->memory, 0);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkBindBufferMemory (staging)");
		goto error;
	}
	/* Mapped once and left mapped. vkMapMemory is not free, and a surface
	 * that redraws continuously would otherwise pay for it every commit. */
	res = vkMapMemory(dev->dev, up->memory, 0, reqs.size, 0, &up->mapped);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkMapMemory (staging)");
		goto error;
	}
	up->size = reqs.size;
	return true;

error:
	avk_upload_finish(dev, up);
	return false;
}


/*
 * AVK_UNSAFE_REUSE=1 -- the break switch for image-update ordering.
 *
 * It strips the read-before-write dependency from the barrier that guards
 * overwriting an image a frame in flight may still be sampling.
 *
 * It removes the BARRIER and not a semaphore wait, and that distinction was
 * measured rather than assumed. The caller also passes a timeline wait on the
 * image's last use; removing only that changed nothing and validation stayed
 * clean, because uploads and frames are submitted to the same queue and a
 * pipeline barrier orders against earlier submissions on that queue in
 * submission order. The barrier is what protects this. The timeline wait is
 * belt to its braces, and worth keeping for the day an upload moves to a
 * transfer queue -- but it is not the thing under test.
 */
static bool unsafe_reuse(void) {
	static int cached = -1;
	if (cached < 0) {
		const char *env = getenv("AVK_UNSAFE_REUSE");
		cached = env != NULL && env[0] == '1';
		if (cached) {
			avk_log(AVK_ERROR, "AVK_UNSAFE_REUSE=1 -- image updates are being "
				"recorded with no dependency on the frames still reading "
				"them");
		}
	}
	return cached != 0;
}

/* lcm(bpp, 4): what Vulkan requires of bufferOffset -- a multiple of 4 and of
 * the texel size. Computed rather than assumed 4, because a format with an odd
 * texel size would silently produce a misaligned copy. */
static VkDeviceSize copy_offset_align(uint32_t bpp) {
	VkDeviceSize align = bpp;
	while (align % 4 != 0) {
		align += bpp;
	}
	return align;
}

bool avk_upload_plan_full(struct avk_upload_plan *plan,
		const struct avk_image *image, uint32_t stride, uint32_t height) {
	*plan = (struct avk_upload_plan){ 0 };
	uint32_t bpp = avk_format_bytes_per_pixel(image->format);
	if (bpp == 0) {
		return false;
	}
	if (stride % bpp != 0) {
		/* bufferRowLength is in PIXELS. A stride that is not a whole number of
		 * pixels cannot be expressed, and rounding it shears the image. */
		avk_log(AVK_ERROR, "upload: stride %u is not a multiple of %u bytes "
			"per pixel", stride, bpp);
		return false;
	}
	if (height == 0) {
		return false;
	}
	plan->full = true;
	plan->bpp = bpp;
	plan->stride = stride;
	plan->height = height;
	plan->rect_count = 1;
	plan->rects[0] = (struct avk_upload_rect){
		.x = 0, .y = 0,
		.width = image->extent.width, .height = image->extent.height,
	};
	plan->offsets[0] = 0;
	plan->total = (VkDeviceSize)stride * height;
	plan->bytes = (uint64_t)stride * height;
	return true;
}

bool avk_upload_plan_regions(struct avk_upload_plan *plan,
		const struct avk_image *image, uint32_t stride, uint32_t height,
		const struct avk_upload_rect *rects, uint32_t rect_count) {
	*plan = (struct avk_upload_plan){ 0 };
	if (rect_count == 0) {
		return false;
	}
	uint32_t bpp = avk_format_bytes_per_pixel(image->format);
	if (bpp == 0) {
		return false;
	}
	if (rect_count > AVK_UPLOAD_MAX_REGIONS) {
		/* The caller is expected to coalesce before it gets here; refusing is
		 * better than truncating, because a truncated copy leaves the image
		 * holding a mixture of two generations with nothing to say so. */
		avk_log(AVK_ERROR, "upload: %u regions is more than the %d this path "
			"packs at once", rect_count, AVK_UPLOAD_MAX_REGIONS);
		return false;
	}

	const VkDeviceSize align = copy_offset_align(bpp);
	VkDeviceSize total = 0;
	uint64_t bytes = 0;
	uint32_t packed = 0;
	for (uint32_t i = 0; i < rect_count; i++) {
		const struct avk_upload_rect *r = &rects[i];
		if (r->width == 0 || r->height == 0) {
			continue;
		}
		/* Bounds are the caller's job, but a rectangle that escaped it would
		 * read past the client's mapping -- so it is checked here as well. */
		if (r->x + r->width > image->extent.width ||
				r->y + r->height > image->extent.height) {
			avk_log(AVK_ERROR, "upload: region %u,%u %ux%u is outside a %ux%u "
				"image", r->x, r->y, r->width, r->height,
				image->extent.width, image->extent.height);
			return false;
		}
		total = (total + align - 1) / align * align;
		plan->offsets[packed] = total;
		plan->rects[packed] = *r;
		packed++;
		VkDeviceSize span = (VkDeviceSize)r->width * r->height * bpp;
		total += span;
		bytes += (uint64_t)span;
	}
	if (packed == 0 || total == 0) {
		return false;
	}
	plan->rect_count = packed;
	plan->total = total;
	/*
	 * `bytes` is what pack() writes; `total` is what staging must hold. They
	 * differ by the alignment padding between regions, and reporting `total`
	 * as "bytes copied" would credit the damage path with moving padding it
	 * never touches.
	 */
	plan->bytes = bytes;
	plan->bpp = bpp;
	plan->stride = stride;
	plan->height = height;
	plan->full = false;
	return true;
}

void avk_upload_pack(const struct avk_upload_plan *plan, const void *src,
		void *dst) {
	if (plan->full) {
		memcpy(dst, src, (size_t)plan->total);
		return;
	}
	uint8_t *out = dst;
	const uint8_t *in = src;
	for (uint32_t i = 0; i < plan->rect_count; i++) {
		const struct avk_upload_rect *r = &plan->rects[i];
		size_t row_bytes = (size_t)r->width * plan->bpp;
		for (uint32_t row = 0; row < r->height; row++) {
			/*
			 * ROW BY ROW, because a source row is `stride` bytes apart and a
			 * rectangle's rows are not contiguous unless it spans the full
			 * width. Copying width * height * bpp as one block is the obvious
			 * shortcut and it shears the rectangle diagonally.
			 */
			memcpy(out + plan->offsets[i] + (size_t)row * row_bytes,
				in + (size_t)(r->y + row) * plan->stride
					+ (size_t)r->x * plan->bpp,
				row_bytes);
		}
	}
}

uint64_t avk_upload_submit_packed(struct avk_device *dev,
		struct avk_cmd_ring *ring, struct avk_upload *up,
		struct avk_image *image, const struct avk_upload_plan *plan,
		const VkSemaphoreSubmitInfo *waits, uint32_t wait_count) {
	(void)dev;
	if (plan->rect_count == 0) {
		return 0;
	}
	if (!plan->full && image->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
		/* UNDEFINED discards the contents, which for a partial update would
		 * throw away every pixel outside the damaged rectangles. A partial
		 * update into an image nothing has written yet is a caller bug. */
		avk_log(AVK_ERROR, "upload: a partial update of an image with no "
			"previous contents would discard everything outside the damage");
		return 0;
	}

	VkCommandBuffer cb = avk_cmd_ring_begin(ring);
	if (cb == VK_NULL_HANDLE) {
		return 0;
	}

	VkImageMemoryBarrier2 to_dst = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		/* The image may still be being sampled by a frame in flight. The
		 * caller keeps that honest with a timeline wait; this barrier makes
		 * the read-before-write ordering explicit to the driver as well. */
		.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.oldLayout = image->layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image->image,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	if (image->layout == VK_IMAGE_LAYOUT_UNDEFINED || unsafe_reuse()) {
		/* Nothing has read it yet, and claiming a shader read happened would
		 * be a barrier describing a dependency that does not exist. */
		to_dst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
		to_dst.srcAccessMask = 0;
	}
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &to_dst,
	};
	vkCmdPipelineBarrier2(cb, &dep);

	VkBufferImageCopy2 regions[AVK_UPLOAD_MAX_REGIONS];
	for (uint32_t i = 0; i < plan->rect_count; i++) {
		const struct avk_upload_rect *r = &plan->rects[i];
		regions[i] = (VkBufferImageCopy2){
			.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
			.bufferOffset = plan->offsets[i],
			/* In PIXELS, not bytes -- the classic way to get a sheared image.
			 * The whole-image plan keeps the SOURCE stride because it copied
			 * the padding too; the region plan packed rows tightly, so its row
			 * length is each rectangle's own width. */
			.bufferRowLength = plan->full ? plan->stride / plan->bpp : r->width,
			/*
			 * ROWS PER SLICE, which is the IMAGE's height and not the caller's
			 * row count. The two are the same for every 2D caller -- it hands
			 * over the whole image -- and differ by a factor of `depth` for
			 * M6C's cube, whose dim*dim rows are dim slices of dim rows.
			 * Taking it from the image is the reading that is right in both
			 * cases; taking it from the plan shears the cube.
			 */
			.bufferImageHeight = plan->full ? image->extent.height : r->height,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.layerCount = 1,
			},
			.imageOffset = { (int32_t)r->x, (int32_t)r->y, 0 },
			/* depth from the IMAGE for the same reason: 1 for every surface,
			 * `dim` for the cube. A partial region is always 2D. */
			.imageExtent = { r->width, r->height,
				plan->full ? image->depth : 1 },
		};
	}
	VkCopyBufferToImageInfo2 copy = {
		.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
		.srcBuffer = up->buffer,
		.dstImage = image->image,
		.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.regionCount = plan->rect_count,
		.pRegions = regions,
	};
	vkCmdCopyBufferToImage2(cb, &copy);

	VkImageMemoryBarrier2 to_read = to_dst;
	to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
	to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dep.pImageMemoryBarriers = &to_read;
	vkCmdPipelineBarrier2(cb, &dep);

	uint64_t timeline = avk_cmd_ring_submit(ring, waits, wait_count, NULL, 0);
	if (timeline == 0) {
		return 0;
	}

	image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	image->last_use = timeline;
	up->last_use = timeline;
	return timeline;
}

uint64_t avk_upload_image_write(struct avk_device *dev,
		struct avk_cmd_ring *ring, struct avk_upload *up,
		struct avk_image *image, const void *pixels, uint32_t stride,
		uint32_t height, const VkSemaphoreSubmitInfo *waits,
		uint32_t wait_count) {
	struct avk_upload_plan plan;
	if (!avk_upload_plan_full(&plan, image, stride, height)) {
		return 0;
	}
	if (!avk_upload_staging_ensure(dev, up, ring->retire, plan.total)) {
		return 0;
	}
	avk_upload_pack(&plan, pixels, up->mapped);
	return avk_upload_submit_packed(dev, ring, up, image, &plan, waits,
		wait_count);
}

uint64_t avk_upload_image_write_regions(struct avk_device *dev,
		struct avk_cmd_ring *ring, struct avk_upload *up,
		struct avk_image *image, const void *pixels, uint32_t stride,
		uint32_t height, const struct avk_upload_rect *rects,
		uint32_t rect_count, uint64_t *bytes_copied,
		const VkSemaphoreSubmitInfo *waits, uint32_t wait_count) {
	if (bytes_copied != NULL) {
		*bytes_copied = 0;
	}
	struct avk_upload_plan plan;
	if (!avk_upload_plan_regions(&plan, image, stride, height, rects,
			rect_count)) {
		return 0;
	}
	if (!avk_upload_staging_ensure(dev, up, ring->retire, plan.total)) {
		return 0;
	}
	avk_upload_pack(&plan, pixels, up->mapped);
	uint64_t timeline = avk_upload_submit_packed(dev, ring, up, image, &plan,
		waits, wait_count);
	if (timeline != 0 && bytes_copied != NULL) {
		*bytes_copied = plan.bytes;
	}
	return timeline;
}

void avk_upload_finish(struct avk_device *dev, struct avk_upload *up) {
	if (up->mapped != NULL) {
		vkUnmapMemory(dev->dev, up->memory);
		up->mapped = NULL;
	}
	if (up->buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(dev->dev, up->buffer, NULL);
		AVK_LIVE_DEC(dev, buffers);
		up->buffer = VK_NULL_HANDLE;
	}
	if (up->memory != VK_NULL_HANDLE) {
		vkFreeMemory(dev->dev, up->memory, NULL);
		AVK_LIVE_DEC(dev, device_memory);
		up->memory = VK_NULL_HANDLE;
	}
	up->size = 0;
}

void avk_upload_retire(struct avk_device *dev, void *upload) {
	struct avk_upload *up = upload;
	avk_upload_finish(dev, up);
	AVK_LIVE_DEC(dev, avk_uploads);
	free(up);
}
