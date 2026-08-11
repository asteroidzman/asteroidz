#define _POSIX_C_SOURCE 200809L

#include "avk_image.h"

#include <stdlib.h>

void avk_image_destroy(struct avk_device *dev, void *data) {
	struct avk_image *image = data;
	if (image == NULL) {
		return;
	}

	if (image->view != VK_NULL_HANDLE) {
		vkDestroyImageView(dev->dev, image->view, NULL);
	}
	if (image->image != VK_NULL_HANDLE) {
		vkDestroyImage(dev->dev, image->image, NULL);
	}
	/* Memory after the image, always: freeing memory an image is still bound
	 * to is undefined behaviour that happens to work on desktop drivers right
	 * up until it does not. */
	for (uint32_t i = 0; i < image->memory_count; i++) {
		if (image->memory[i] != VK_NULL_HANDLE) {
			vkFreeMemory(dev->dev, image->memory[i], NULL);
		}
	}

	free(image);
}
