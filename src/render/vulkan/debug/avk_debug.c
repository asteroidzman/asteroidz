#define _POSIX_C_SOURCE 200809L

#include "avk_debug.h"

#include <stdio.h>

void avk_debug_label_begin(struct avk_device *dev, VkCommandBuffer cb,
		const char *fmt, ...) {
	if (dev->instance->cmd_begin_label == NULL) {
		return;
	}
	char name[128];
	va_list args;
	va_start(args, fmt);
	vsnprintf(name, sizeof(name), fmt, args);
	va_end(args);

	VkDebugUtilsLabelEXT label = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
		.pLabelName = name,
	};
	dev->instance->cmd_begin_label(cb, &label);
}

void avk_debug_label_end(struct avk_device *dev, VkCommandBuffer cb) {
	if (dev->instance->cmd_end_label == NULL) {
		return;
	}
	dev->instance->cmd_end_label(cb);
}

void avk_debug_label_insert(struct avk_device *dev, VkCommandBuffer cb,
		const char *fmt, ...) {
	if (dev->instance->cmd_insert_label == NULL) {
		return;
	}
	char name[128];
	va_list args;
	va_start(args, fmt);
	vsnprintf(name, sizeof(name), fmt, args);
	va_end(args);

	VkDebugUtilsLabelEXT label = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
		.pLabelName = name,
	};
	dev->instance->cmd_insert_label(cb, &label);
}
