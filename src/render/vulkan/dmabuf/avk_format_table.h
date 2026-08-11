#ifndef AVK_FORMAT_TABLE_H
#define AVK_FORMAT_TABLE_H

#include "../device/avk_device.h"
#include "avk_drm_format.h"

/*
 * What this device can actually import, asked of the driver rather than
 * assumed.
 *
 * Two things make this different from a list of supported formats:
 *
 * 1. Every entry is *probed*, not inferred. A modifier appearing in
 *    VkDrmFormatModifierPropertiesListEXT means the driver knows the tiling;
 *    it does not mean an importable image of that format, modifier, usage and
 *    size can be created. Only vkGetPhysicalDeviceImageFormatProperties2 with
 *    the external-memory handle type chained in answers that, and its answer
 *    includes the maximum extent -- which differs per modifier, and which a
 *    4K client on a compressed modifier can genuinely exceed.
 *
 * 2. Texture and render capability are tracked separately, because they are
 *    separate questions with different usage flags. A modifier can be
 *    perfectly samplable and not renderable, and treating the two as one set
 *    means either rejecting importable client buffers or promising output
 *    formats that cannot be attached.
 *
 * This table is also the source of DMA-BUF feedback: what we advertise to
 * clients is derived from what we just proved we can import, so a conforming
 * client cannot allocate a buffer we will then have to reject.
 */

struct avk_modifier_caps {
	uint64_t modifier;
	uint32_t plane_count;
	VkExtent2D max_extent;
	bool supports_disjoint;
	/* Whether an image can be created MUTABLE_FORMAT so both the UNORM and
	 * sRGB views exist. That is how content gets sRGB-decoded by the sampler
	 * instead of in a shader, and it is not available on every modifier. */
	bool srgb_mutable;
};

struct avk_format_caps {
	const struct avk_drm_format *format;

	struct avk_modifier_caps *texture_mods;
	uint32_t texture_mod_count;

	struct avk_modifier_caps *render_mods;
	uint32_t render_mod_count;
};

struct avk_format_table {
	struct avk_format_caps *formats;
	uint32_t count;

	/* Totals, so the log can say "17 formats, 214 importable
	 * format/modifier pairs" rather than making anyone count. */
	uint32_t texture_pair_count;
	uint32_t render_pair_count;
};

bool avk_format_table_init(struct avk_format_table *table,
	struct avk_device *dev);
void avk_format_table_finish(struct avk_format_table *table);

const struct avk_format_caps *avk_format_table_find(
	const struct avk_format_table *table, uint32_t fourcc);

/*
 * Look up one modifier.
 *
 * Exact equality, deliberately -- and therefore DRM_FORMAT_MOD_INVALID never
 * matches, because the driver never enumerates it. That is correct and it is
 * the whole reason avk_dmabuf.c has a compatibility ladder: the answer to an
 * implicit-modifier buffer is to *find out what the modifier really is*, not
 * to make this lookup lie.
 */
const struct avk_modifier_caps *avk_format_caps_find_modifier(
	const struct avk_format_caps *caps, uint64_t modifier, bool for_render);

/* The full table at DEBUG, a summary at INFO. */
void avk_format_table_log(const struct avk_format_table *table);

#endif /* AVK_FORMAT_TABLE_H */
