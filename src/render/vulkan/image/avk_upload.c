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

struct avk_image *avk_upload_image_create(struct avk_device *dev,
		uint32_t drm_format, uint32_t width, uint32_t height,
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
	if (width == 0 || height == 0) {
		avk_log(AVK_ERROR, "cannot upload a %ux%u image", width, height);
		return NULL;
	}

	struct avk_image *image = calloc(1, sizeof(*image));
	if (image == NULL) {
		return NULL;
	}
	image->dev = dev;
	image->format = fmt->vk;
	image->drm_format = drm_format;
	image->extent = (VkExtent2D){ width, height };
	/* Not a lie about a dma-buf: this image genuinely has no DRM modifier,
	 * because it was never allocated through DRM. */
	image->modifier = DRM_FORMAT_MOD_INVALID;
	image->plane_count = 1;
	image->has_alpha = fmt->has_alpha;
	image->origin = origin;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = fmt->vk,
		.extent = { width, height, 1 },
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

static bool staging_ensure(struct avk_device *dev, struct avk_upload *up,
		VkDeviceSize size) {
	if (up->buffer != VK_NULL_HANDLE && up->size >= size) {
		return true;
	}
	/* Grow, never shrink: a window that resizes back and forth should not
	 * reallocate on every wobble. */
	avk_upload_finish(dev, up);

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

uint64_t avk_upload_image_write(struct avk_device *dev,
		struct avk_cmd_ring *ring, struct avk_upload *up,
		struct avk_image *image, const void *pixels, uint32_t stride,
		uint32_t height, const VkSemaphoreSubmitInfo *waits,
		uint32_t wait_count) {
	uint32_t bpp = avk_format_bytes_per_pixel(image->format);
	if (bpp == 0) {
		return 0;
	}
	if (stride % bpp != 0) {
		/* bufferRowLength is in PIXELS. A stride that is not a whole number of
		 * pixels cannot be expressed, and rounding it shears the image. */
		avk_log(AVK_ERROR, "upload: stride %u is not a multiple of %u bytes "
			"per pixel", stride, bpp);
		return 0;
	}

	VkDeviceSize size = (VkDeviceSize)stride * height;
	if (!staging_ensure(dev, up, size)) {
		return 0;
	}
	memcpy(up->mapped, pixels, (size_t)size);

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
	if (image->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
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

	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferOffset = 0,
		/* In PIXELS, not bytes -- the classic way to get a sheared image. */
		.bufferRowLength = stride / bpp,
		.bufferImageHeight = height,
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.imageExtent = { image->extent.width, image->extent.height, 1 },
	};
	VkCopyBufferToImageInfo2 copy = {
		.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
		.srcBuffer = up->buffer,
		.dstImage = image->image,
		.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.regionCount = 1,
		.pRegions = &region,
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
	return timeline;
}

void avk_upload_finish(struct avk_device *dev, struct avk_upload *up) {
	if (up->mapped != NULL) {
		vkUnmapMemory(dev->dev, up->memory);
		up->mapped = NULL;
	}
	if (up->buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(dev->dev, up->buffer, NULL);
		up->buffer = VK_NULL_HANDLE;
	}
	if (up->memory != VK_NULL_HANDLE) {
		vkFreeMemory(dev->dev, up->memory, NULL);
		up->memory = VK_NULL_HANDLE;
	}
	up->size = 0;
}

void avk_upload_retire(struct avk_device *dev, void *upload) {
	struct avk_upload *up = upload;
	avk_upload_finish(dev, up);
	free(up);
}
