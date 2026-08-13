/*
 * avk_color_caps -- the M5 colour capability questions, as pure predicates.
 * Contract C5.
 *
 * WHY THE PREDICATES ARE SEPARATE FROM THE PROBES. Each of these is one line
 * of bit tests over a VkFormatFeatureFlags, and each answers a question that
 * decides an architectural path:
 *
 *   fp16_attach_blend_sample  can the Path-B working intermediate exist?
 *   rgb10a2_attach            can a 10-bit scanout target be attached?
 *   scanout_srgb_attach       does PATH A EXIST AT ALL?
 *
 * The last one is the audit's biggest UNKNOWN. If a scanout dma-buf on this
 * GPU's modifiers cannot carry an _SRGB view usable as a COLOUR ATTACHMENT,
 * then every output takes Path B, the SDR floor claim in ADR-001 has to be
 * re-measured, and 66 MB of intermediate appears per SDR output.
 *
 * A question that important should not be answerable only by running on the
 * one machine that has the answer. The bit tests live here, take their inputs
 * as plain flags, and are driven by tests/test-avk-color-caps.c with SYNTHETIC
 * feature sets -- the same technique test-dmabuf-feedback uses, and for the
 * same reason: on a device where the right and wrong answers coincide, a live
 * test certifies nothing.
 */
#ifndef AVK_COLOR_CAPS_H
#define AVK_COLOR_CAPS_H

#include <stdbool.h>
#include <vulkan/vulkan.h>

/*
 * ADR-001's working-format criteria (a), for R16G16B16A16_SFLOAT on optimal
 * tiling. All four bits, and none of them is redundant:
 *
 *   COLOR_ATTACHMENT        composition renders into it
 *   COLOR_ATTACHMENT_BLEND  and BLENDS into it -- source-over is fixed
 *                           function, so an attachment without this is an
 *                           attachment we can only overwrite
 *   SAMPLED_IMAGE           the encode pass reads it back
 *   SAMPLED_IMAGE_FILTER_LINEAR  and the blur chain reads it BILINEARLY;
 *                           without this every down/upsample becomes a
 *                           nearest-neighbour fetch and the blur is aliased
 *
 * Checking only COLOR_ATTACHMENT would pass on a device that cannot blend into
 * the format, which is the failure that produces a correct-looking first draw
 * and a black frame after the second.
 */
#define AVK_FP16_REQUIRED_FEATURES \
	(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | \
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | \
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)

static inline bool avk_fp16_working_format_ok(VkFormatFeatureFlags optimal) {
	return (optimal & AVK_FP16_REQUIRED_FEATURES) ==
		AVK_FP16_REQUIRED_FEATURES;
}

/*
 * A2B10G10R10_UNORM_PACK32 as a 10-bit scanout target. Attachment and blend,
 * because on Path B the encode pass writes it and on a hypothetical Path-A
 * 10-bit output composition would blend into it. Sampling is NOT required: the
 * scanout buffer is written, never read back by the renderer.
 */
#define AVK_RGB10A2_REQUIRED_FEATURES \
	(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | \
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)

static inline bool avk_rgb10a2_attach_ok(VkFormatFeatureFlags optimal) {
	return (optimal & AVK_RGB10A2_REQUIRED_FEATURES) ==
		AVK_RGB10A2_REQUIRED_FEATURES;
}

/*
 * PATH A'S EXISTENCE TEST, and the one place this file earns its keep.
 *
 * Two facts have to hold together and they are answered by two DIFFERENT
 * queries, which is exactly how this gets got wrong:
 *
 *   mutable_ok   an importable image of the UNORM format on this modifier can
 *                be created with MUTABLE_FORMAT and a view-format list
 *                containing the _SRGB format, under ATTACHMENT usage. That is
 *                what avk_format_table.c's existing render-usage probe already
 *                asks, and on its own it is NOT enough --
 *
 *   srgb_modifier_features
 *                ...because it says nothing about what the _SRGB VIEW can be
 *                USED FOR. The features are per (format, modifier), and the
 *                _SRGB format has its OWN modifier feature set. A driver may
 *                well let you create the mutable image and sample the _SRGB
 *                view while refusing to attach it -- sampling and attachment
 *                are separate bits precisely because they are separate
 *                capabilities.
 *
 * Asking only the first question yields a Path A that passes every probe and
 * then fails at vkCreateImageView or at render-pass creation, on the live
 * desktop, at the first frame. Asking only the second yields a modifier the
 * driver can attach but whose image cannot be created mutable in the first
 * place.
 *
 * BLEND is required as well as ATTACHMENT: Path A's entire premise is that the
 * fixed-function blend operates on decoded linear values through the _SRGB
 * view (Vulkan's sRGB conversion rules). An attachment that cannot blend
 * cannot composite.
 */
#define AVK_SCANOUT_SRGB_REQUIRED_FEATURES \
	(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | \
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)

static inline bool avk_scanout_srgb_attach_ok(bool mutable_ok,
		VkFormatFeatureFlags srgb_modifier_features) {
	if (!mutable_ok) {
		return false;
	}
	return (srgb_modifier_features & AVK_SCANOUT_SRGB_REQUIRED_FEATURES) ==
		AVK_SCANOUT_SRGB_REQUIRED_FEATURES;
}

#endif /* AVK_COLOR_CAPS_H */
