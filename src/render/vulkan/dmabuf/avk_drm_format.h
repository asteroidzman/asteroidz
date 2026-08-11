#ifndef AVK_DRM_FORMAT_H
#define AVK_DRM_FORMAT_H

#include "../avk.h"

/*
 * DRM fourcc <-> VkFormat.
 *
 * The two vocabularies do not line up, and the places they disagree are where
 * the bugs live:
 *
 *  - DRM fourccs name channels in *little-endian memory order*, Vulkan names
 *    them in component order. DRM_FORMAT_ARGB8888 is B,G,R,A in memory, which
 *    is VK_FORMAT_B8G8R8A8_UNORM -- the names look transposed and are not.
 *  - DRM has X variants (XRGB8888) that mean "there are 8 bits here and they
 *    are not alpha". Vulkan has no such format; the same VkFormat is used and
 *    the alpha channel has to be ignored by the *shader*, not by the import.
 *    Getting this wrong makes opaque windows translucent in exactly the way
 *    that looks like a blending bug.
 *  - Packed 10-bit formats reverse again: DRM_FORMAT_ARGB2101010 is
 *    VK_FORMAT_A2R10G10B10_UNORM_PACK32, where PACK32 means the whole thing is
 *    one 32-bit word and endianness is the word's, not the bytes'.
 *
 * So this is a table, written once, with the reasoning attached -- not a
 * conversion function anyone should be tempted to derive on the spot.
 */

struct avk_drm_format {
	uint32_t drm;              /* DRM_FORMAT_* fourcc */
	VkFormat vk;               /* the UNORM/SFLOAT view */
	VkFormat vk_srgb;          /* the sRGB view, or VK_FORMAT_UNDEFINED */
	/* False for the X variants. The import is identical; what differs is that
	 * the sampled alpha is meaningless and must be replaced with 1.0. */
	bool has_alpha;
	uint8_t plane_count;       /* 1 for RGB, 2 for NV12, 3 for YUV420 */
	bool is_ycbcr;
};

/* NULL when avk has no mapping for this fourcc. That is a real answer -- it
 * means the format cannot be imported and the caller must say so with the
 * format name, rather than substituting something that is nearly right. */
const struct avk_drm_format *avk_drm_format_from_fourcc(uint32_t fourcc);

/* Every format avk knows about, for building the capability table. */
const struct avk_drm_format *avk_drm_format_table(size_t *count);

/* "AR24 (DRM_FORMAT_ARGB8888)" into a caller-supplied buffer. Used in every
 * import failure message, because a bare 0x34325241 in a log helps nobody. */
void avk_drm_format_name(uint32_t fourcc, char *buf, size_t size);

/* "I915_FORMAT_MOD_Y_TILED", "LINEAR", "INVALID", or the raw value. */
void avk_drm_modifier_name(uint64_t modifier, char *buf, size_t size);

#endif /* AVK_DRM_FORMAT_H */
