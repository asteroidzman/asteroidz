#define _POSIX_C_SOURCE 200809L

#include "avk_image.h"
#include "../avk.h"

#include <inttypes.h>
#include <stdlib.h>

/*
 * M5/C7 (Path A). The image's _SRGB view, created on demand.
 *
 * Used two ways and the guard is the same for both: as a SAMPLED view the
 * hardware applies the sRGB EOTF on every fetch, and as a COLOUR ATTACHMENT it
 * applies the inverse on every write. Path A is exactly those two together --
 * decode and encode in fixed function, no shader and no extra pass.
 *
 * NULL when the image was not created MUTABLE with that format in its view
 * list. That is not a soft failure to paper over: a view in a format an image
 * was not created for is undefined behaviour, and validation will not reliably
 * catch it. The caller falls back and renders the pre-M5 picture.
 */
VkImageView avk_image_srgb_view(struct avk_device *dev,
		struct avk_image *image) {
	if (image == NULL || !image->srgb_mutable
			|| image->format_srgb == VK_FORMAT_UNDEFINED) {
		return VK_NULL_HANDLE;
	}
	if (image->view_srgb != VK_NULL_HANDLE) {
		return image->view_srgb;
	}
	VkImageViewCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = image->format_srgb,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	if (!avk_check(vkCreateImageView(dev->dev, &vi, NULL, &image->view_srgb),
			"vkCreateImageView (_SRGB)")) {
		image->view_srgb = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}
	AVK_LIVE_INC(dev, image_views);
	return image->view_srgb;
}

void avk_image_destroy(struct avk_device *dev, void *data) {
	struct avk_image *image = data;
	if (image == NULL) {
		return;
	}

	/*
	 * Destroying an image twice destroys the driver's host allocation for its
	 * VkImage twice, and that is a plain double free wearing a Vulkan hat: it
	 * aborts inside glibc, at a free() with no AVK symbol anywhere near it, at
	 * whatever unrelated moment the allocator next notices its arena is
	 * inconsistent. This catches it at the second destroy instead, names the
	 * image, and declines to do it.
	 *
	 * The read is of memory this function may already have freed, which is
	 * itself undefined -- deliberately. Under ASan it is a use-after-free
	 * report with both stacks, which is the best possible outcome; without
	 * ASan the freed chunk almost never still reads as AVK_IMAGE_LIVE, so the
	 * check holds. What it must NOT do is be relied on as protection, which is
	 * why it counts a violation rather than quietly carrying on.
	 */
	if (image->life != AVK_IMAGE_LIVE) {
		if (dev != NULL) {
			dev->lifecycle_violations++;
		}
		avk_log(AVK_ERROR, "avk_image #%" PRIu64 " destroyed twice (state %u) "
			"-- something owns it that should not", image->id, image->life);
		return;
	}
	image->life = AVK_IMAGE_DESTROYED;

	if (image->view != VK_NULL_HANDLE) {
		vkDestroyImageView(dev->dev, image->view, NULL);
		AVK_LIVE_DEC(dev, image_views);
	}
	if (image->view_srgb != VK_NULL_HANDLE) {
		vkDestroyImageView(dev->dev, image->view_srgb, NULL);
		AVK_LIVE_DEC(dev, image_views);
	}
	if (image->image != VK_NULL_HANDLE) {
		vkDestroyImage(dev->dev, image->image, NULL);
		AVK_LIVE_DEC(dev, images);
	}
	/* Memory after the image, always: freeing memory an image is still bound
	 * to is undefined behaviour that happens to work on desktop drivers right
	 * up until it does not. */
	for (uint32_t i = 0; i < image->memory_count; i++) {
		if (image->memory[i] != VK_NULL_HANDLE) {
			vkFreeMemory(dev->dev, image->memory[i], NULL);
			AVK_LIVE_DEC(dev, device_memory);
		}
	}

	AVK_LIVE_DEC(dev, avk_images);
	free(image);
}

struct avk_image *avk_image_alloc(struct avk_device *dev) {
	struct avk_image *image = calloc(1, sizeof(*image));
	if (image == NULL) {
		return NULL;
	}
	image->dev = dev;
	/* Every image is 2D unless it says otherwise. Set HERE rather than left at
	 * calloc's zero so that "depth" is never ambiguous between "one slice" and
	 * "nobody filled this in" -- a 0 reaching a VkImageCreateInfo is an invalid
	 * extent, and a 0 reaching the view-type choice below would be a 2D view of
	 * a 3D image. */
	image->depth = 1;
	image->life = AVK_IMAGE_LIVE;
	image->id = ++dev->next_object_id;
	AVK_LIVE_INC(dev, avk_images);
	return image;
}
