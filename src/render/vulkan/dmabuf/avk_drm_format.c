#define _POSIX_C_SOURCE 200809L

#include "avk_drm_format.h"

#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>

/*
 * The order here is the order the capability table probes in, which is the
 * order they get advertised to clients: 8-bit first because that is what
 * almost everything actually uses, then 10-bit, then float, then YUV.
 */
static const struct avk_drm_format formats[] = {
	/* ── 8-bit RGB ──────────────────────────────────────────────────────
	 * The four spellings of "32 bits per pixel" every toolkit uses. ARGB/
	 * XRGB are what GTK, Qt and XWayland hand over; ABGR/XBGR are what GL
	 * clients tend to produce. */
	{ DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_B8G8R8A8_SRGB, true,  1, false },
	{ DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_B8G8R8A8_SRGB, false, 1, false },
	{ DRM_FORMAT_ABGR8888, VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_R8G8B8A8_SRGB, true,  1, false },
	{ DRM_FORMAT_XBGR8888, VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_R8G8B8A8_SRGB, false, 1, false },

	/* ── 10-bit RGB ─────────────────────────────────────────────────────
	 * The HDR10 scanout formats. No sRGB view: a 2101010 buffer carrying PQ
	 * must not be handed to the hardware sRGB decoder, which would apply a
	 * transfer function the content does not have. The colour pipeline
	 * decodes these explicitly. */
	{ DRM_FORMAT_ARGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32,
		VK_FORMAT_UNDEFINED, true,  1, false },
	{ DRM_FORMAT_XRGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32,
		VK_FORMAT_UNDEFINED, false, 1, false },
	{ DRM_FORMAT_ABGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
		VK_FORMAT_UNDEFINED, true,  1, false },
	{ DRM_FORMAT_XBGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
		VK_FORMAT_UNDEFINED, false, 1, false },

	/* ── 16-bit ─────────────────────────────────────────────────────────
	 * Half-float is what a scene-referred HDR client (and our own
	 * compositing intermediate) uses. Already linear, so again no sRGB
	 * view. */
	{ DRM_FORMAT_ABGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_FORMAT_UNDEFINED, true,  1, false },
	{ DRM_FORMAT_XBGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_FORMAT_UNDEFINED, false, 1, false },
	{ DRM_FORMAT_ABGR16161616, VK_FORMAT_R16G16B16A16_UNORM,
		VK_FORMAT_UNDEFINED, true,  1, false },
	{ DRM_FORMAT_XBGR16161616, VK_FORMAT_R16G16B16A16_UNORM,
		VK_FORMAT_UNDEFINED, false, 1, false },

	/* ── 16 bpp ─────────────────────────────────────────────────────────
	 * Rare, but some cursors and some very old clients still use it. */
	{ DRM_FORMAT_RGB565, VK_FORMAT_R5G6B5_UNORM_PACK16,
		VK_FORMAT_UNDEFINED, false, 1, false },

	/* ── YUV ────────────────────────────────────────────────────────────
	 * Video. These need a sampler Ycbcr conversion object to sample at all,
	 * which is why the capability table refuses them outright when
	 * samplerYcbcrConversion is unsupported rather than importing an image
	 * nothing can read. P010 is the 10-bit one, i.e. HDR video. */
	{ DRM_FORMAT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
		VK_FORMAT_UNDEFINED, false, 2, true },
	{ DRM_FORMAT_YUV420, VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
		VK_FORMAT_UNDEFINED, false, 3, true },
	{ DRM_FORMAT_P010, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
		VK_FORMAT_UNDEFINED, false, 2, true },
};

const struct avk_drm_format *avk_drm_format_from_fourcc(uint32_t fourcc) {
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (formats[i].drm == fourcc) {
			return &formats[i];
		}
	}
	return NULL;
}

const struct avk_drm_format *avk_drm_format_table(size_t *count) {
	*count = sizeof(formats) / sizeof(formats[0]);
	return formats;
}

void avk_drm_format_name(uint32_t fourcc, char *buf, size_t size) {
	char *name = drmGetFormatName(fourcc);
	if (name != NULL) {
		snprintf(buf, size, "%s (0x%08" PRIX32 ")", name, fourcc);
		free(name);
	} else {
		snprintf(buf, size, "0x%08" PRIX32, fourcc);
	}
}

void avk_drm_modifier_name(uint64_t modifier, char *buf, size_t size) {
	/* Named explicitly rather than left to drmGetFormatModifierName, which
	 * returns NULL for it: INVALID is the single most important modifier to
	 * see spelled out in a log, because it is the one whose handling is a
	 * decision rather than a lookup. */
	if (modifier == DRM_FORMAT_MOD_INVALID) {
		snprintf(buf, size, "INVALID (implicit)");
		return;
	}
	char *name = drmGetFormatModifierName(modifier);
	if (name != NULL) {
		snprintf(buf, size, "%s (0x%016" PRIX64 ")", name, modifier);
		free(name);
	} else {
		snprintf(buf, size, "0x%016" PRIX64, modifier);
	}
}
