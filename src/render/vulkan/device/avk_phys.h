#ifndef AVK_PHYS_H
#define AVK_PHYS_H

#include "avk_device.h"

/*
 * Physical-device enumeration and selection.
 *
 * Separate from avk_device.c because "which GPU, and why" is a question that
 * gets asked on its own -- by the multi-GPU code, by DMA-BUF feedback, and by
 * anyone reading a bug report. It must be answerable without creating a
 * logical device.
 */

struct avk_phys {
	VkPhysicalDevice handle;
	struct avk_caps caps;
	/* Set when the device is missing something avk requires. The device is
	 * still listed -- being able to log "GPU1 was rejected because it has no
	 * VK_EXT_image_drm_format_modifier" is worth more than a shorter list. */
	const char *reject_reason;
};

/* Enumerate every physical device and fill in its capability table.
 * `*out` is allocated with calloc(); free() it. */
bool avk_phys_enumerate(struct avk_instance *inst, struct avk_phys **out,
	uint32_t *count);

/*
 * Pick the device that owns the DRM node `drm_fd` refers to.
 *
 * Matching is by dev_t: the node is fstat()ed and its major/minor compared
 * against VK_EXT_physical_device_drm's primary and render pairs. There is no
 * name matching and no index fallback, because both are how a multi-GPU box
 * ends up compositing on one GPU while its clients render on another and
 * nobody can tell from the log.
 *
 * `drm_fd` may be -1, in which case the first usable device is chosen with
 * discrete GPUs preferred -- appropriate for tests, never for a session.
 *
 * Returns an index into `list`, or -1. On failure the reason is logged for
 * every candidate.
 */
int avk_phys_select(const struct avk_phys *list, uint32_t count, int drm_fd);

/* One INFO line per device: name, driver, type, DRM node, verdict. */
void avk_phys_log_all(const struct avk_phys *list, uint32_t count,
	int selected);

#endif /* AVK_PHYS_H */
