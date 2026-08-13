/*
 * The M5 capability predicates, driven with SYNTHETIC format feature sets.
 * Contract C5.
 *
 * WHY SYNTHETIC AND NOT LIVE. Three reasons, and the third is the real one.
 *
 * 1. This machine's RADV answers yes to everything these predicates ask, so a
 *    live test would pass for a predicate that returned `true` unconditionally.
 *    That is coverage by coincidence -- the exact failure this project has
 *    written down twice -- and the fix is a case the hardware cannot produce.
 * 2. The interesting answers are the PARTIAL ones. A device that supports
 *    R16G16B16A16_SFLOAT for sampling but not for blending exists; a modifier
 *    whose _SRGB twin can be sampled but not attached exists. Those are the
 *    cases that decide whether Path A is real, and no single GPU can be made
 *    to produce them on demand.
 * 3. A predicate driven by flags is a predicate that can be reasoned about.
 *    The live probe is three vkGetPhysicalDeviceFormatProperties2 calls whose
 *    only interesting content is which bits it then tests; testing the bit
 *    tests is testing the part with a decision in it.
 *
 * WHAT IS NOT TESTED HERE. That the live probe passes the RIGHT flags to the
 * predicates -- that it queries the _SRGB twin's modifier list rather than the
 * UNORM one, in particular. That needs a device and is DEFERRED; see
 * docs/m5-hdr/opus-findings.md, F9.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>

#include "render/vulkan/device/avk_color_caps.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); } \
		else { failures++; printf("  FAIL " __VA_ARGS__); printf("\n"); } \
	} while (0)

#define ATTACH VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
#define BLEND VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
#define SAMPLED VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
#define LINEAR VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
#define STORAGE VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT
#define XFER (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
	VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

int main(void) {
	printf("== C5 colour capability predicates ==\n");

	/* ── the working format (ADR-001 criterion a) ────────────────────────── */
	printf("fp16 working format\n");
	CHECK(avk_fp16_working_format_ok(ATTACH | BLEND | SAMPLED | LINEAR),
		"all four features -> yes");
	CHECK(avk_fp16_working_format_ok(
			  ATTACH | BLEND | SAMPLED | LINEAR | STORAGE | XFER),
		"extra features do not change the answer");

	/* Each bit removed in turn. Every one of these is a device that exists
	 * and a failure mode that is invisible until the wrong frame. */
	CHECK(!avk_fp16_working_format_ok(BLEND | SAMPLED | LINEAR),
		"no COLOR_ATTACHMENT -> no (cannot render into it)");
	CHECK(!avk_fp16_working_format_ok(ATTACH | SAMPLED | LINEAR),
		"no COLOR_ATTACHMENT_BLEND -> no (can only overwrite, not composite)");
	CHECK(!avk_fp16_working_format_ok(ATTACH | BLEND | LINEAR),
		"no SAMPLED_IMAGE -> no (the encode pass cannot read it back)");
	CHECK(!avk_fp16_working_format_ok(ATTACH | BLEND | SAMPLED),
		"no FILTER_LINEAR -> no (the blur chain would be nearest-neighbour)");
	CHECK(!avk_fp16_working_format_ok(0), "no features at all -> no");
	/* The one a naive probe passes: attachment alone. */
	CHECK(!avk_fp16_working_format_ok(ATTACH),
		"COLOR_ATTACHMENT alone -> no (this is what a naive probe asks)");

	/* ── the 10-bit scanout target ───────────────────────────────────────── */
	printf("rgb10a2 attachment\n");
	CHECK(avk_rgb10a2_attach_ok(ATTACH | BLEND), "attachment + blend -> yes");
	CHECK(!avk_rgb10a2_attach_ok(ATTACH), "attachment without blend -> no");
	CHECK(!avk_rgb10a2_attach_ok(SAMPLED | LINEAR | XFER),
		"sampling only -> no");
	/* Sampling is NOT required of a scanout target: it is written, never read
	 * back by the renderer. A predicate that demanded it would reject a
	 * perfectly good 10-bit output. */
	CHECK(avk_rgb10a2_attach_ok(ATTACH | BLEND | XFER),
		"and sampling is not required of a write-only target");

	/* ── Path A's existence test ─────────────────────────────────────────── */
	printf("scanout _SRGB attachment (Path A)\n");
	CHECK(avk_scanout_srgb_attach_ok(true, ATTACH | BLEND),
		"mutable image AND an attachable+blendable _SRGB view -> Path A");

	/* THE CASE THE WHOLE PREDICATE EXISTS FOR. The mutable probe says yes --
	 * the image can be created with both view formats -- and the _SRGB twin
	 * can be sampled but not attached. Asking only the first question yields
	 * a Path A that passes startup and fails at the first frame on the live
	 * desktop. */
	CHECK(!avk_scanout_srgb_attach_ok(true, SAMPLED | LINEAR),
		"mutable yes, but the _SRGB view is sample-only -> NOT Path A");
	CHECK(!avk_scanout_srgb_attach_ok(true, ATTACH),
		"mutable yes, _SRGB attachable but not blendable -> NOT Path A "
		"(composition is a blend)");
	CHECK(!avk_scanout_srgb_attach_ok(true, 0),
		"mutable yes, _SRGB has no features on this modifier -> NOT Path A");

	/* And the converse: the driver will happily report features for a format
	 * whose mutable image cannot be created on this modifier at all. */
	CHECK(!avk_scanout_srgb_attach_ok(false, ATTACH | BLEND),
		"_SRGB attachable but the mutable image cannot be created -> "
		"NOT Path A");
	CHECK(!avk_scanout_srgb_attach_ok(false, 0), "neither -> NOT Path A");

	/* The predicate must not be satisfiable by ANY single input: two
	 * independent facts, both required. Asserted as a truth table so a
	 * refactor that drops one of them is a failure here. */
	{
		int yes = 0;
		for (int m = 0; m < 2; m++) {
			for (int f = 0; f < 4; f++) {
				VkFormatFeatureFlags flags =
					(VkFormatFeatureFlags)((f & 1 ? ATTACH : 0) |
						(f & 2 ? BLEND : 0));
				if (avk_scanout_srgb_attach_ok(m != 0, flags)) {
					yes++;
				}
			}
		}
		CHECK(yes == 1,
			"exactly one of the eight (mutable x attach x blend) combinations "
			"is Path A (got %d)",
			yes);
	}

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
