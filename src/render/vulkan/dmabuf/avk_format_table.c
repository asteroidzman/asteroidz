#define _POSIX_C_SOURCE 200809L

#include "avk_format_table.h"

#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Usage flags the probe asks about.
 *
 * These have to match what the importer will actually request, or the table
 * says yes to something that then fails at vkCreateImage. SAMPLED and
 * TRANSFER_SRC for a client texture (TRANSFER_SRC so it can be copied into a
 * readback or a fallback intermediate); COLOR_ATTACHMENT and TRANSFER_DST for
 * something we render into.
 */
static const VkImageUsageFlags texture_usage =
	VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
static const VkImageUsageFlags render_usage =
	VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

/*
 * Can an importable image be created with exactly these parameters?
 *
 * The chain matters and is easy to get subtly wrong:
 *   VkPhysicalDeviceImageFormatInfo2
 *     -> VkPhysicalDeviceExternalImageFormatInfo   (handleType = DMA_BUF)
 *        -> VkPhysicalDeviceImageDrmFormatModifierInfoEXT (the modifier)
 *           -> VkImageFormatListCreateInfo         (only when mutable)
 *
 * Leaving the external-image-format info out asks "can I create such an
 * image", which is a different and more permissive question than "can I
 * create one backed by an imported dma-buf" -- and the difference is exactly
 * the set of format/modifier pairs that would pass a naive probe and then
 * fail at import time.
 */
static bool probe_modifier(struct avk_device *dev,
		const struct avk_drm_format *fmt, uint64_t modifier,
		VkImageUsageFlags usage, bool mutable_srgb,
		VkExtent2D *max_extent) {
	VkFormat view_formats[2] = { fmt->vk, fmt->vk_srgb };
	VkImageFormatListCreateInfo format_list = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = 2,
		.pViewFormats = view_formats,
	};

	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		.pNext = mutable_srgb ? &format_list : NULL,
		.drmFormatModifier = modifier,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkPhysicalDeviceExternalImageFormatInfo external_info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.pNext = &modifier_info,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkPhysicalDeviceImageFormatInfo2 info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.pNext = &external_info,
		.format = fmt->vk,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = usage,
		.flags = mutable_srgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
	};

	VkExternalImageFormatProperties external_props = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
	};
	VkImageFormatProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		.pNext = &external_props,
	};

	VkResult res = vkGetPhysicalDeviceImageFormatProperties2(dev->phys, &info,
		&props);
	if (res != VK_SUCCESS) {
		/* FORMAT_NOT_SUPPORTED is the expected "no" and is not worth a log
		 * line -- most format/modifier pairs on a GPU are not supported and
		 * saying so 400 times at startup is noise. */
		return false;
	}

	/* Supported to create is not the same as supported to import. */
	if (!(external_props.externalMemoryProperties.externalMemoryFeatures
			& VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
		return false;
	}

	if (max_extent != NULL) {
		max_extent->width = props.imageFormatProperties.maxExtent.width;
		max_extent->height = props.imageFormatProperties.maxExtent.height;
	}
	return true;
}

static bool query_format(struct avk_device *dev,
		const struct avk_drm_format *fmt, struct avk_format_caps *out) {
	memset(out, 0, sizeof(*out));
	out->format = fmt;

	/* A YUV format is unusable without a Ycbcr conversion, so it is left out
	 * of the table entirely rather than advertised and then refused. */
	if (fmt->is_ycbcr && !dev->caps.sampler_ycbcr_conversion) {
		return false;
	}

	VkDrmFormatModifierPropertiesListEXT modifier_list = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};
	VkFormatProperties2 format_props = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &modifier_list,
	};
	vkGetPhysicalDeviceFormatProperties2(dev->phys, fmt->vk, &format_props);

	if (modifier_list.drmFormatModifierCount == 0) {
		return false;
	}

	VkDrmFormatModifierPropertiesEXT *mods =
		calloc(modifier_list.drmFormatModifierCount, sizeof(*mods));
	if (mods == NULL) {
		return false;
	}
	modifier_list.pDrmFormatModifierProperties = mods;
	vkGetPhysicalDeviceFormatProperties2(dev->phys, fmt->vk, &format_props);

	uint32_t count = modifier_list.drmFormatModifierCount;
	out->texture_mods = calloc(count, sizeof(*out->texture_mods));
	out->render_mods = calloc(count, sizeof(*out->render_mods));
	if (out->texture_mods == NULL || out->render_mods == NULL) {
		free(out->texture_mods);
		free(out->render_mods);
		free(mods);
		memset(out, 0, sizeof(*out));
		return false;
	}

	for (uint32_t i = 0; i < count; i++) {
		const VkDrmFormatModifierPropertiesEXT *m = &mods[i];

		/* The plane count the *driver* associates with this modifier. An
		 * import whose dma-buf disagrees with this is malformed, and that
		 * check lives here so the importer can just compare. */
		uint32_t plane_count = m->drmFormatModifierPlaneCount;
		bool disjoint = (m->drmFormatModifierTilingFeatures
			& VK_FORMAT_FEATURE_DISJOINT_BIT) != 0;

		VkExtent2D extent = { 0, 0 };
		if (probe_modifier(dev, fmt, m->drmFormatModifier, texture_usage,
				false, &extent)) {
			bool srgb_mutable = fmt->vk_srgb != VK_FORMAT_UNDEFINED
				&& probe_modifier(dev, fmt, m->drmFormatModifier,
					texture_usage, true, NULL);
			out->texture_mods[out->texture_mod_count++] =
				(struct avk_modifier_caps){
					.modifier = m->drmFormatModifier,
					.plane_count = plane_count,
					.max_extent = extent,
					.supports_disjoint = disjoint,
					.srgb_mutable = srgb_mutable,
				};
		}

		extent = (VkExtent2D){ 0, 0 };
		if (probe_modifier(dev, fmt, m->drmFormatModifier, render_usage,
				false, &extent)) {
			bool srgb_mutable = fmt->vk_srgb != VK_FORMAT_UNDEFINED
				&& probe_modifier(dev, fmt, m->drmFormatModifier,
					render_usage, true, NULL);
			out->render_mods[out->render_mod_count++] =
				(struct avk_modifier_caps){
					.modifier = m->drmFormatModifier,
					.plane_count = plane_count,
					.max_extent = extent,
					.supports_disjoint = disjoint,
					.srgb_mutable = srgb_mutable,
				};
		}
	}

	free(mods);

	if (out->texture_mod_count == 0 && out->render_mod_count == 0) {
		free(out->texture_mods);
		free(out->render_mods);
		memset(out, 0, sizeof(*out));
		return false;
	}
	return true;
}

bool avk_format_table_init(struct avk_format_table *table,
		struct avk_device *dev) {
	memset(table, 0, sizeof(*table));

	if (!dev->caps.image_drm_format_modifier) {
		avk_log(AVK_ERROR, "no VK_EXT_image_drm_format_modifier: this device "
			"cannot import client buffers with a known tiling");
		return false;
	}

	size_t known = 0;
	const struct avk_drm_format *formats = avk_drm_format_table(&known);

	table->formats = calloc(known, sizeof(*table->formats));
	if (table->formats == NULL) {
		avk_log(AVK_ERROR, "allocation failed");
		return false;
	}

	for (size_t i = 0; i < known; i++) {
		struct avk_format_caps caps;
		if (!query_format(dev, &formats[i], &caps)) {
			continue;
		}
		table->formats[table->count++] = caps;
		table->texture_pair_count += caps.texture_mod_count;
		table->render_pair_count += caps.render_mod_count;
	}

	if (table->count == 0) {
		avk_log(AVK_ERROR, "no importable DMA-BUF formats at all -- something "
			"is badly wrong with this driver or this device");
		free(table->formats);
		table->formats = NULL;
		return false;
	}

	return true;
}

void avk_format_table_finish(struct avk_format_table *table) {
	for (uint32_t i = 0; i < table->count; i++) {
		free(table->formats[i].texture_mods);
		free(table->formats[i].render_mods);
	}
	free(table->formats);
	memset(table, 0, sizeof(*table));
}

const struct avk_format_caps *avk_format_table_find(
		const struct avk_format_table *table, uint32_t fourcc) {
	for (uint32_t i = 0; i < table->count; i++) {
		if (table->formats[i].format->drm == fourcc) {
			return &table->formats[i];
		}
	}
	return NULL;
}

const struct avk_modifier_caps *avk_format_caps_find_modifier(
		const struct avk_format_caps *caps, uint64_t modifier,
		bool for_render) {
	const struct avk_modifier_caps *mods =
		for_render ? caps->render_mods : caps->texture_mods;
	uint32_t count = for_render ? caps->render_mod_count
		: caps->texture_mod_count;

	for (uint32_t i = 0; i < count; i++) {
		if (mods[i].modifier == modifier) {
			return &mods[i];
		}
	}
	return NULL;
}

void avk_format_table_log(const struct avk_format_table *table) {
	avk_log(AVK_INFO, "avk formats: %u importable format(s), %u texture and "
		"%u render format/modifier pairs",
		table->count, table->texture_pair_count, table->render_pair_count);

	if (avk_log_get_level() < AVK_DEBUG) {
		return;
	}

	for (uint32_t i = 0; i < table->count; i++) {
		const struct avk_format_caps *caps = &table->formats[i];
		char fmt_name[64];
		avk_drm_format_name(caps->format->drm, fmt_name, sizeof(fmt_name));
		avk_log(AVK_DEBUG, "  %s: %u texture, %u render", fmt_name,
			caps->texture_mod_count, caps->render_mod_count);

		for (uint32_t j = 0; j < caps->texture_mod_count; j++) {
			const struct avk_modifier_caps *m = &caps->texture_mods[j];
			char mod_name[80];
			avk_drm_modifier_name(m->modifier, mod_name, sizeof(mod_name));
			avk_log(AVK_DEBUG,
				"    tex %s planes=%u max=%ux%u disjoint=%d srgb=%d",
				mod_name, m->plane_count, m->max_extent.width,
				m->max_extent.height, m->supports_disjoint, m->srgb_mutable);
		}
	}
}
