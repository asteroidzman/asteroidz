#ifndef AVK_DEBUG_H
#define AVK_DEBUG_H

#include "../device/avk_device.h"

/*
 * Debug labels on command buffers.
 *
 * These are what turn a RenderDoc capture from a flat list of 400 draws into
 * a tree with "output DP-1" > "blur downsample 2" > "draw". They cost a
 * function pointer call when nothing is capturing, so they are compiled in
 * always and simply do nothing when VK_EXT_debug_utils is absent.
 *
 * Object *names* live on avk_device (avk_device_name_object) because naming is
 * a device operation; these are the command-stream half.
 */

void avk_debug_label_begin(struct avk_device *dev, VkCommandBuffer cb,
	const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void avk_debug_label_end(struct avk_device *dev, VkCommandBuffer cb);
void avk_debug_label_insert(struct avk_device *dev, VkCommandBuffer cb,
	const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif /* AVK_DEBUG_H */
