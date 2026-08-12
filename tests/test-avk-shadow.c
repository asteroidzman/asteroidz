/*
 * M4D: shadows, asserted on the framebuffer against an independent oracle.
 *
 * WHAT IS BEING COMPARED AGAINST. SceneFX's box_shadow.frag, traced in
 * docs/avk-effects.md. The oracle below is transcribed from THAT file, in
 * double precision, not from AVK's shader -- a test written from the same
 * source as the implementation only proves the transcription was consistent.
 *
 * The reference is NOT a 2D Gaussian and the oracle must not be one either.
 * It is a closed-form erf() across x, integrated over y with FOUR samples
 * inside +-3 sigma, with the corner radius for a scanline chosen by whether
 * that scanline is above or below the caster's centre. A principled Gaussian
 * blur would be more correct and would disagree with every shadow on the
 * desktop.
 *
 * HOW A COVERAGE IS READ BACK. The clear is opaque WHITE and the shadow is
 * pure black at full alpha, so the composited channel is exactly
 * 255 * (1 - coverage) and a pixel is a coverage measurement rather than a
 * colour to be judged. Over a black background a black shadow is invisible,
 * which is how a shadow test can pass while drawing nothing.
 *
 * Exits 77 (skip) with no GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "render/vulkan/scene/avk_render.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { \
			printf("  ok   " __VA_ARGS__); \
			printf("\n"); \
		} else { \
			failures++; \
			printf("  FAIL " __VA_ARGS__); \
			printf("   (%s:%d)\n", __FILE__, __LINE__); \
		} \
	} while (0)

#define SKIP(...) do { \
		printf("SKIP: " __VA_ARGS__); printf("\n"); return 77; \
	} while (0)

#define W 256
#define H 256
#define TARGET_FORMAT VK_FORMAT_B8G8R8A8_UNORM

/* M_PI is not exposed under _POSIX_C_SOURCE 200809L. */
#define AZ_PI 3.14159265358979323846

/* ── the oracle, transcribed from box_shadow.frag ───────────────────────── */

/* Abramowitz & Stegun 7.1.26, exactly as the reference spells it. */
static double ref_erf(double x) {
	double s = (x > 0.0) - (x < 0.0);
	double a = fabs(x);
	double t = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
	t *= t;
	return s - s / (t * t);
}

static double ref_gaussian(double x, double sigma) {
	return exp(-(x * x) / (2.0 * sigma * sigma))
		/ (sqrt(2.0 * AZ_PI) * sigma);
}

static double ref_clamp(double v, double lo, double hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Blurred coverage of one scanline between two rounded edges. */
static double ref_shadow_x(double x, double y, double sigma, double corner_l,
		double corner_r, double half_x, double half_y) {
	double delta_l = fmin(half_y - corner_l - fabs(y), 0.0);
	double delta_r = fmin(half_y - corner_r - fabs(y), 0.0);
	double curved_l = half_x - corner_l
		+ sqrt(fmax(0.0, corner_l * corner_l - delta_l * delta_l));
	double curved_r = half_x - corner_r
		+ sqrt(fmax(0.0, corner_r * corner_r - delta_r * delta_r));
	double k = sqrt(0.5) / sigma;
	double lo = 0.5 + 0.5 * ref_erf((x - curved_l) * k);
	double hi = 0.5 + 0.5 * ref_erf((x + curved_r) * k);
	return hi - lo;
}

/*
 * `radii` is CLOCKWISE -- tl, tr, br, bl -- which is struct avk_cmd::corners's
 * order. The reference's own helper takes (tl, tr, bl, br), and that mismatch
 * is exactly the bug the M4A audit found waiting to happen, so the unpacking
 * is spelled out here rather than passed through.
 */
static double ref_box_shadow(const double lo[2], const double hi[2],
		double px, double py, double sigma, const double radii[4]) {
	double r_tl = radii[0], r_tr = radii[1], r_br = radii[2], r_bl = radii[3];
	double cx = (lo[0] + hi[0]) * 0.5, cy = (lo[1] + hi[1]) * 0.5;
	double half_x = (hi[0] - lo[0]) * 0.5, half_y = (hi[1] - lo[1]) * 0.5;
	double x = px - cx, y = py - cy;

	double low = y - half_y, high = y + half_y;
	double start = ref_clamp(-3.0 * sigma, low, high);
	double end = ref_clamp(3.0 * sigma, low, high);
	double step = (end - start) / 4.0;
	double sample_y = start + step * 0.5;

	double value = 0.0;
	for (int i = 0; i < 4; i++) {
		double sy = y - sample_y;
		/* Negative y is the TOP: gl_FragCoord is top-down and so is the box. */
		double corner_l = sy < 0.0 ? r_tl : r_bl;
		double corner_r = sy < 0.0 ? r_tr : r_br;
		value += ref_shadow_x(x, sy, sigma, corner_l, corner_r, half_x, half_y)
			* ref_gaussian(sample_y, sigma) * step;
		sample_y += step;
	}
	return value;
}

/*
 * The whole material: envelope in, coverage at a pixel out. `blur_sigma` does
 * TWO different jobs and they are not the same number -- it is the inset that
 * recovers the caster from the envelope, and HALF of it is the Gaussian's
 * sigma. Transcribing the field name straight into the Gaussian is the way to
 * get a shadow that is exactly twice as soft as the reference's.
 */
static double oracle_coverage(const struct avk_box *env, double blur_sigma,
		const double radii[4], int x, int y) {
	double lo[2] = { (double)env->x + blur_sigma,
		(double)env->y + blur_sigma };
	double hi[2] = { (double)(env->x + env->width) - blur_sigma,
		(double)(env->y + env->height) - blur_sigma };
	if (hi[0] < lo[0]) { hi[0] = lo[0]; }
	if (hi[1] < lo[1]) { hi[1] = lo[1]; }
	/*
	 * THE ENVELOPE TRUNCATES THE SHADOW, and the oracle has to say so.
	 *
	 * The primitive is a quad the size of the node's box; nothing outside it
	 * is shaded, whatever the Gaussian would still have contributed there.
	 * Leaving this out of the oracle put its worst disagreement with AVK at
	 * x = 31 -- one pixel outside an envelope starting at 32 -- and called a
	 * correctly clipped shadow a 5.2/255 rendering error.
	 *
	 * It is not a rounding detail. With the shipped defaults (size 24,
	 * blur 24) the envelope's edge is 24 px from the caster and the effective
	 * Gaussian sigma is 12, so the cut lands at 2 sigma: about 2.3% of
	 * coverage is discarded all the way round. That is the reference's own
	 * geometry, it is what makes the node's box a legitimate damage bound,
	 * and test_falloff() measures it rather than assuming it away.
	 */
	if (x < env->x || y < env->y || x >= env->x + env->width
			|| y >= env->y + env->height) {
		return 0.0;
	}
	/* gl_FragCoord is the pixel CENTRE. Sampling at the integer corner is a
	 * half-pixel error, which on a falloff this steep is several 8-bit
	 * steps -- and it looks like a renderer that smears its edges. */
	double cov = ref_box_shadow(lo, hi, (double)x + 0.5, (double)y + 0.5,
		blur_sigma * 0.5, radii);
	return ref_clamp(cov, 0.0, 1.0);
}

/* ── plumbing ───────────────────────────────────────────────────────────── */

struct harness {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_renderer renderer;
	struct avk_image *target;
	uint32_t pixels[W * H];
};

static uint32_t find_memory(struct avk_device *dev, uint32_t bits,
		VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(dev->phys, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((bits & (1u << i))
				&& (props.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

static struct avk_image *make_target(struct avk_device *dev) {
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = TARGET_FORMAT;
	image->extent = (VkExtent2D){ W, H };
	image->has_alpha = true;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	image->plane_count = 1;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = TARGET_FORMAT,
		.extent = { W, H, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(dev->dev, &info, NULL, &image->image) != VK_SUCCESS) {
		free(image);
		return NULL;
	}
	AVK_LIVE_INC(dev, images);
	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(dev->dev, image->image, &reqs);
	uint32_t type = find_memory(dev, reqs.memoryTypeBits,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (type == UINT32_MAX
			|| vkAllocateMemory(dev->dev, &alloc, NULL, &image->memory[0])
				!= VK_SUCCESS
			|| vkBindImageMemory(dev->dev, image->image, image->memory[0], 0)
				!= VK_SUCCESS) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	AVK_LIVE_INC(dev, device_memory);
	image->memory_count = 1;
	return image;
}

static bool readback(struct harness *h) {
	struct avk_device *dev = h->dev;
	VkDeviceSize size = (VkDeviceSize)W * H * 4;
	VkBuffer buffer;
	VkDeviceMemory memory;

	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	if (vkCreateBuffer(dev->dev, &bi, NULL, &buffer) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements reqs;
	vkGetBufferMemoryRequirements(dev->dev, buffer, &reqs);
	uint32_t type = find_memory(dev, reqs.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
		| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkMemoryAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (type == UINT32_MAX
			|| vkAllocateMemory(dev->dev, &ai, NULL, &memory) != VK_SUCCESS) {
		vkDestroyBuffer(dev->dev, buffer, NULL);
		return false;
	}
	vkBindBufferMemory(dev->dev, buffer, memory, 0);

	struct avk_cmd_ring ring;
	avk_cmd_ring_init(&ring, dev, "readback");
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);

	VkImageMemoryBarrier2 b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
		.oldLayout = h->target->layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = h->target->image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &b,
	};
	vkCmdPipelineBarrier2(cb, &dep);
	h->target->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferRowLength = W,
		.bufferImageHeight = H,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { W, H, 1 },
	};
	VkCopyImageToBufferInfo2 copy = {
		.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
		.srcImage = h->target->image,
		.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.dstBuffer = buffer,
		.regionCount = 1,
		.pRegions = &region,
	};
	vkCmdCopyImageToBuffer2(cb, &copy);

	uint64_t value = avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0);
	bool ok = value != 0
		&& avk_device_timeline_wait(dev, value, 2000000000ULL);
	if (ok) {
		void *mapped = NULL;
		if (vkMapMemory(dev->dev, memory, 0, size, 0, &mapped) == VK_SUCCESS) {
			memcpy(h->pixels, mapped, (size_t)size);
			vkUnmapMemory(dev->dev, memory);
		} else {
			ok = false;
		}
	}
	avk_cmd_ring_finish(&ring);
	vkDestroyBuffer(dev->dev, buffer, NULL);
	vkFreeMemory(dev->dev, memory, NULL);
	return ok;
}

/* B8G8R8A8 read as a little-endian uint32 is 0xAARRGGBB. */
static int chan(struct harness *h, int x, int y) {
	return (int)((h->pixels[(size_t)y * W + (size_t)x] >> 16) & 0xFF);
}

/*
 * The measurement this whole file is built on: white background, black
 * shadow, so the red channel is 255 * (1 - coverage).
 */
static double coverage_at(struct harness *h, int x, int y) {
	return 1.0 - (double)chan(h, x, y) / 255.0;
}

static bool render(struct harness *h, struct avk_scene *scene) {
	uint64_t value = avk_render_frame(&h->renderer, h->target, scene, NULL, 0,
		NULL, 0);
	if (value == 0) {
		return false;
	}
	return avk_device_timeline_wait(h->dev, value, 2000000000ULL)
		&& readback(h);
}

/* Opaque WHITE, so a black shadow is a readable darkening. */
static void scene_begin(struct avk_scene *scene) {
	avk_scene_init(scene);
	pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	scene->has_clear = true;
	scene->clear_color[0] = 1.0f;
	scene->clear_color[1] = 1.0f;
	scene->clear_color[2] = 1.0f;
	scene->clear_color[3] = 1.0f;
}

static struct avk_cmd *add_shadow(struct avk_scene *scene,
		const struct avk_box *env, float sigma, const float radii[4]) {
	struct avk_cmd *cmd = avk_scene_add(scene, AVK_CMD_SHADOW);
	if (cmd == NULL) {
		return NULL;
	}
	cmd->dst = *env;
	cmd->opacity = 1.0f;
	cmd->blur_sigma = sigma;
	/* Pure black at full alpha: the composited channel is then exactly the
	 * complement of coverage, with no colour arithmetic in between. */
	cmd->color[0] = cmd->color[1] = cmd->color[2] = 0.0f;
	cmd->color[3] = 1.0f;
	for (int i = 0; i < 4; i++) {
		cmd->corners[i] = radii != NULL ? radii[i] : 0.0f;
	}
	return cmd;
}

/* ── the reference geometry every fixture starts from ───────────────────── */

static const struct avk_box kEnv = { 32, 32, 192, 192 };
static const float kSigma = 24.0f;
/* Caster: the envelope inset by sigma, i.e. (56,56) 144x144. */
static const int kCasterX0 = 56, kCasterY0 = 56;
static const int kCasterX1 = 200, kCasterY1 = 200;

/* ── tests ──────────────────────────────────────────────────────────────── */

static void test_oracle_agreement(struct harness *h) {
	printf("the shadow AVK draws is the shadow the reference formula "
		"describes\n");
	const float radii[4] = { 20.0f, 20.0f, 20.0f, 20.0f };
	const double dradii[4] = { 20.0, 20.0, 20.0, 20.0 };

	struct avk_scene scene;
	scene_begin(&scene);
	CHECK(add_shadow(&scene, &kEnv, kSigma, radii) != NULL,
		"the shadow command was built");
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) {
		CHECK(false, "frame rendered");
		return;
	}

	/*
	 * Every pixel of the envelope and a margin around it -- not a handful of
	 * chosen points, because a formula can agree at four samples and diverge
	 * everywhere between them.
	 */
	double max_err = 0.0, sum_err = 0.0;
	int samples = 0, outside_tol = 0;
	int worst_x = 0, worst_y = 0;
	for (int y = kEnv.y - 8; y < kEnv.y + kEnv.height + 8; y++) {
		for (int x = kEnv.x - 8; x < kEnv.x + kEnv.width + 8; x++) {
			if (x < 0 || y < 0 || x >= W || y >= H) {
				continue;
			}
			double want = oracle_coverage(&kEnv, kSigma, dradii, x, y);
			double got = coverage_at(h, x, y);
			double err = fabs(want - got);
			if (err > max_err) {
				max_err = err;
				worst_x = x;
				worst_y = y;
			}
			sum_err += err;
			samples++;
			/* 2/255 is a rounding step either way plus the fp32-vs-fp64
			 * difference between the shader and the oracle. */
			if (err > 2.5 / 255.0) {
				outside_tol++;
			}
		}
	}
	printf("    %d samples, max err %.4f (%.1f/255) at %d,%d, mean %.5f, "
		"%d outside tolerance\n", samples, max_err, max_err * 255.0,
		worst_x, worst_y, sum_err / samples, outside_tol);

	CHECK(samples > 40000, "premise: the whole envelope was sampled (%d)",
		samples);
	CHECK(max_err < 4.0 / 255.0,
		"worst pixel is within 4/255 of the oracle (%.1f/255)",
		max_err * 255.0);
	CHECK(sum_err / samples < 0.5 / 255.0,
		"mean error is under half an 8-bit step (%.2f/255)",
		sum_err / samples * 255.0);
	CHECK(outside_tol * 200 < samples,
		"fewer than 0.5%% of pixels exceed 2.5/255 (%d of %d)",
		outside_tol, samples);

	/*
	 * AND THE PREMISE THE AGREEMENT CANNOT SUPPLY. An oracle and a renderer
	 * that both produce a blank frame agree perfectly. There has to be a real
	 * shadow in there.
	 */
	double deep = coverage_at(h, (kCasterX0 + kCasterX1) / 2,
		(kCasterY0 + kCasterY1) / 2);
	double outside = coverage_at(h, 4, 4);
	CHECK(deep > 0.9, "premise: the caster's middle is nearly opaque (%.3f)",
		deep);
	CHECK(outside < 0.02, "premise: the far corner is nearly clear (%.3f)",
		outside);
}

static void test_falloff(struct harness *h) {
	printf("the falloff: strong at the edge, smooth, and gone by the "
		"envelope\n");
	const float radii[4] = { 20.0f, 20.0f, 20.0f, 20.0f };

	struct avk_scene scene;
	scene_begin(&scene);
	add_shadow(&scene, &kEnv, kSigma, radii);
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) {
		CHECK(false, "frame rendered");
		return;
	}

	int cx = (kCasterX0 + kCasterX1) / 2;
	/* Distances below the caster's bottom edge. */
	const int dist[] = { 2, 6, 10, 16, 22 };
	double cov[5];
	for (int i = 0; i < 5; i++) {
		cov[i] = coverage_at(h, cx, kCasterY1 + dist[i]);
		printf("    %2dpx below the caster: coverage %.4f\n", dist[i], cov[i]);
	}

	bool monotonic = true;
	for (int i = 1; i < 5; i++) {
		if (cov[i] > cov[i - 1] + 1e-3) {
			monotonic = false;
		}
	}
	CHECK(monotonic, "coverage decreases with distance, without a bump");
	CHECK(cov[0] > 0.35, "there is real shadow just outside the caster "
		"(%.3f at 2px)", cov[0]);
	CHECK(cov[4] < cov[0] * 0.6,
		"and it has fallen well off by 22px (%.3f vs %.3f)", cov[4], cov[0]);

	/*
	 * CONVERGENCE, AND WHAT IT ACTUALLY IS.
	 *
	 * The envelope is not where the Gaussian ends -- it is where the
	 * compositor stopped drawing. With size and blur both 24 the envelope's
	 * edge sits 24 px from the caster against an effective sigma of 12, so it
	 * cuts at 2 sigma and discards the ~2.3% of coverage beyond. The first
	 * version of this assertion demanded under 2% and failed at 0.0235, which
	 * is the erf tail at 2 sigma to two decimal places: the number was right
	 * and the threshold was wrong.
	 *
	 * So what is asserted is what matters -- that the residual at the cut is
	 * small enough for the node's box to be an honest damage bound, and that
	 * it really is a tail rather than a shadow still going strong.
	 */
	double at_edge = coverage_at(h, cx, kEnv.y + kEnv.height - 1);
	printf("    at the envelope's own edge (2 sigma out): %.4f\n", at_edge);
	CHECK(at_edge < 0.035,
		"the shadow is down to a tail by the envelope's edge (%.4f) -- so "
		"the node's box really does contain it", at_edge);
	CHECK(at_edge < cov[0] * 0.1,
		"and that tail is a small fraction of the contact value (%.4f vs "
		"%.4f)", at_edge, cov[0]);
}

static void test_per_corner_radii(struct harness *h) {
	printf("per-corner radii: 0 / 7 / 19 / 37, and each corner is its own\n");
	/* Clockwise tl, tr, br, bl. Deliberately all different, and one of them
	 * square: a single-radius implementation renders this plausibly. */
	const float radii[4] = { 0.0f, 7.0f, 19.0f, 37.0f };
	const double dradii[4] = { 0.0, 7.0, 19.0, 37.0 };

	struct avk_scene scene;
	scene_begin(&scene);
	add_shadow(&scene, &kEnv, kSigma, radii);
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) {
		CHECK(false, "frame rendered");
		return;
	}

	CHECK(h->renderer.stats.asymmetric_shadow_draws > 0,
		"premise: the renderer saw four different radii (%" PRIu64
		" asymmetric draws)", h->renderer.stats.asymmetric_shadow_draws);

	/*
	 * HOW THIS IS ASSERTED, AND WHY NOT THE OBVIOUS WAY.
	 *
	 * The obvious test samples each corner and expects coverage to fall as
	 * the radius grows. It was written, it failed, and it was wrong: the
	 * bottom-left corner at r=37 read HIGHER than the bottom-right at r=19,
	 * while the frame matched the oracle to 0.6/255 everywhere.
	 *
	 * The reason is in the approximation. The x direction is a closed form
	 * across a whole SCANLINE, between a left edge and a right edge, so the
	 * coverage near the bottom-left corner is a function of the bottom-LEFT
	 * and bottom-RIGHT radii together. The corners are not separable in this
	 * formula, and expecting them to be ordered independently is expecting a
	 * property of a true 2D distance field from something that is not one.
	 *
	 * So the claim is made by DISCRIMINATION instead: the rendered frame is
	 * compared against three oracles, and it must match the right one while
	 * clearly failing the two that stand for the real bugs -- a single-radius
	 * implementation, and the clockwise-versus-(tl,tr,bl,br) swap of the two
	 * bottom corners that the M4A audit found waiting to happen.
	 */
	struct { const char *name; double r[4]; } candidates[] = {
		{ "the radii it was given (0,7,19,37)", { 0.0, 7.0, 19.0, 37.0 } },
		{ "bottom corners swapped (0,7,37,19)", { 0.0, 7.0, 37.0, 19.0 } },
		{ "one radius for all four (0,0,0,0)", { 0.0, 0.0, 0.0, 0.0 } },
		{ "one radius for all four (37 x4)", { 37.0, 37.0, 37.0, 37.0 } },
	};
	double worst[4] = {0};
	for (int c = 0; c < 4; c++) {
		for (int y = kEnv.y; y < kEnv.y + kEnv.height; y++) {
			for (int x = kEnv.x; x < kEnv.x + kEnv.width; x++) {
				double err = fabs(oracle_coverage(&kEnv, kSigma,
					candidates[c].r, x, y) - coverage_at(h, x, y));
				if (err > worst[c]) {
					worst[c] = err;
				}
			}
		}
		printf("    vs %-38s worst %.1f/255\n", candidates[c].name,
			worst[c] * 255.0);
	}
	(void)dradii;

	CHECK(worst[0] < 4.0 / 255.0,
		"the frame matches the oracle built from its own four radii "
		"(%.1f/255)", worst[0] * 255.0);
	CHECK(worst[1] > 20.0 / 255.0,
		"and does NOT match one with the bottom corners swapped (%.1f/255)",
		worst[1] * 255.0);
	CHECK(worst[2] > 20.0 / 255.0,
		"nor one that squared every corner (%.1f/255)", worst[2] * 255.0);
	CHECK(worst[3] > 20.0 / 255.0,
		"nor one that rounded every corner to 37 (%.1f/255)",
		worst[3] * 255.0);
}

static void test_oversized_radii(struct harness *h) {
	printf("oversized radii: a radius larger than the caster\n");
	/* The caster is 144x144, so 400 is far past any sane clamp. */
	const float radii[4] = { 400.0f, 400.0f, 400.0f, 400.0f };

	struct avk_scene scene;
	scene_begin(&scene);
	add_shadow(&scene, &kEnv, kSigma, radii);
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) {
		CHECK(false, "frame rendered");
		return;
	}

	bool sane = true;
	double peak = 0.0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			double c = coverage_at(h, x, y);
			if (!(c >= -0.001 && c <= 1.001)) {
				sane = false;
			}
			if (c > peak) {
				peak = c;
			}
		}
	}
	CHECK(sane, "every pixel is a coverage in 0..1 -- no NaN, no inversion");
	CHECK(peak > 0.5, "and something was still drawn (peak %.3f)", peak);

	/* An unclamped radius folds the distance field back on itself and the
	 * shape stops being convex. The centre must remain the darkest place. */
	double centre = coverage_at(h, (kCasterX0 + kCasterX1) / 2,
		(kCasterY0 + kCasterY1) / 2);
	double corner = coverage_at(h, kCasterX0 + 2, kCasterY0 + 2);
	CHECK(centre > corner,
		"the centre is still darker than the corner (%.3f > %.3f) -- the "
		"corner did not invert", centre, corner);
}

static void test_interior_cutout(struct harness *h) {
	printf("the window's own footprint, kept out of its own shadow\n");
	const float radii[4] = { 20.0f, 20.0f, 20.0f, 20.0f };

	struct avk_scene scene;
	scene_begin(&scene);
	struct avk_cmd *cmd = add_shadow(&scene, &kEnv, kSigma, radii);
	CHECK(cmd != NULL, "the shadow command was built");
	if (cmd == NULL) {
		avk_scene_finish(&scene);
		return;
	}
	/*
	 * The cut-out is the caster itself, exactly as the compositor sets it:
	 * the region subtracts the box it can express, and the radii travel to
	 * the shader for the arcs it cannot.
	 */
	cmd->inner = (struct avk_box){ kCasterX0, kCasterY0,
		kCasterX1 - kCasterX0, kCasterY1 - kCasterY0 };
	cmd->has_inner = true;
	for (int i = 0; i < 4; i++) {
		cmd->inner_corners[i] = 19.0f;
	}
	pixman_region32_t clip;
	pixman_region32_init_rect(&clip, kEnv.x, kEnv.y, (unsigned)kEnv.width,
		(unsigned)kEnv.height);
	pixman_region32_t cut;
	/* Shrunk by the same 0.3-of-a-radius rule the border path uses, so the
	 * region can never delete a pixel the arcs still want. */
	int in = (int)ceilf(19.0f * 0.3f) + 1;
	pixman_region32_init_rect(&cut, cmd->inner.x + in, cmd->inner.y + in,
		(unsigned)(cmd->inner.width - 2 * in),
		(unsigned)(cmd->inner.height - 2 * in));
	pixman_region32_subtract(&clip, &clip, &cut);
	pixman_region32_fini(&cut);
	avk_cmd_set_clip(cmd, &clip);
	pixman_region32_fini(&clip);

	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) {
		CHECK(false, "frame rendered");
		return;
	}

	double centre = coverage_at(h, (kCasterX0 + kCasterX1) / 2,
		(kCasterY0 + kCasterY1) / 2);
	CHECK(centre < 0.01,
		"nothing is drawn under the window's footprint (%.4f)", centre);

	/* And the shadow still exists outside it, or the cut-out has simply
	 * erased the whole effect. */
	int cx = (kCasterX0 + kCasterX1) / 2;
	double below = coverage_at(h, cx, kCasterY1 + 5);
	CHECK(below > 0.3, "premise: the shadow outside the cut-out survives "
		"(%.3f)", below);

	/*
	 * The corner is the M4A wedge's home ground: on the diagonal, an arc the
	 * region cut square would leave a bright notch between the cut-out's
	 * corner and the shadow's.
	 */
	double notch = coverage_at(h, kCasterX0 + 2, kCasterY0 + 2);
	CHECK(notch > 0.05,
		"the cut-out's own corner arc is shaded, not squared off (%.3f)",
		notch);
}

static void test_alpha_and_opacity(struct harness *h) {
	printf("shadow colour alpha and node opacity both attenuate, once each\n");
	const float radii[4] = { 20.0f, 20.0f, 20.0f, 20.0f };
	int cx = (kCasterX0 + kCasterX1) / 2, cy = (kCasterY0 + kCasterY1) / 2;

	struct avk_scene scene;
	scene_begin(&scene);
	add_shadow(&scene, &kEnv, kSigma, radii);
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }
	double full = coverage_at(h, cx, cy);

	scene_begin(&scene);
	struct avk_cmd *cmd = add_shadow(&scene, &kEnv, kSigma, radii);
	cmd->color[3] = 0.4f;
	ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }
	double alpha40 = coverage_at(h, cx, cy);

	scene_begin(&scene);
	cmd = add_shadow(&scene, &kEnv, kSigma, radii);
	cmd->opacity = 0.4f;
	ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }
	double opacity40 = coverage_at(h, cx, cy);

	printf("    full %.4f   colour a=0.4 %.4f   node opacity 0.4 %.4f\n",
		full, alpha40, opacity40);
	CHECK(fabs(alpha40 - full * 0.4) < 3.0 / 255.0,
		"colour alpha scales coverage exactly once (%.4f vs %.4f)",
		alpha40, full * 0.4);
	CHECK(fabs(opacity40 - full * 0.4) < 3.0 / 255.0,
		"node opacity scales it exactly once (%.4f vs %.4f)",
		opacity40, full * 0.4);
	/*
	 * The two are separate multipliers, and the way to get this wrong is to
	 * apply one of them twice -- which reads as 0.16 rather than 0.4 and is
	 * still a plausible-looking shadow.
	 */
	CHECK(alpha40 > full * 0.3,
		"neither is applied twice (0.4 and not 0.16 of full)");
}

static void test_coloured_shadow_does_not_glow(struct harness *h) {
	printf("a coloured shadow darkens -- the reference's premultiply bug\n");
	const float radii[4] = { 20.0f, 20.0f, 20.0f, 20.0f };
	int cx = (kCasterX0 + kCasterX1) / 2, cy = (kCasterY0 + kCasterY1) / 2;

	struct avk_scene scene;
	scene_begin(&scene);
	struct avk_cmd *cmd = add_shadow(&scene, &kEnv, kSigma, radii);
	/* A mid-blue shadow at half alpha, over white. */
	cmd->color[0] = 0.0f;
	cmd->color[1] = 0.0f;
	cmd->color[2] = 0.6f;
	cmd->color[3] = 0.5f;
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }

	uint32_t p = h->pixels[(size_t)cy * W + (size_t)cx];
	int r = (int)((p >> 16) & 0xFF);
	int g = (int)((p >> 8) & 0xFF);
	int b = (int)(p & 0xFF);
	printf("    centre over white: r=%d g=%d b=%d\n", r, g, b);

	/*
	 * Coverage is ~1 at the centre, so alpha ~= 0.5 and the result over white
	 * is 0.5*colour + 0.5*white: r,g ~= 128, b ~= 128 + 0.5*0.6*255 ~= 204.
	 *
	 * The reference emits vec4(rgb, alpha) UNPREMULTIPLIED into a
	 * premultiplied blend, so its blue would be 0.6*255 + 0.5*255 = 280,
	 * clamped to 255 -- a shadow brighter in blue than the white paper it is
	 * cast on. Every channel here must be no brighter than the background.
	 */
	CHECK(r <= 255 && g <= 255 && b <= 255, "premise: readable pixel");
	CHECK(b < 245, "the blue channel does not blow out to white (%d)", b);
	CHECK(r < 200 && g < 200,
		"and the shadow still darkens r and g (%d, %d)", r, g);
	CHECK(b > r && b > g, "premise: the shadow really is blue (%d > %d)",
		b, r);
}

static void test_no_shadow_is_no_draw(struct harness *h) {
	printf("premise checks: an empty scene really is empty\n");
	struct avk_scene scene;
	scene_begin(&scene);
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }
	CHECK(coverage_at(h, (kCasterX0 + kCasterX1) / 2,
		(kCasterY0 + kCasterY1) / 2) < 0.01,
		"with no shadow command the frame is white everywhere");
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
	int drm_fd = -1;
	for (int i = 128; i < 132 && drm_fd < 0; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
	}
	if (drm_fd < 0) {
		SKIP("no DRM render node");
	}

	struct harness h = {0};
	h.inst = avk_instance_create("test-avk-shadow");
	if (h.inst == NULL) {
		close(drm_fd);
		SKIP("no Vulkan instance");
	}
	h.dev = avk_device_create(h.inst, drm_fd);
	if (h.dev == NULL) {
		avk_instance_destroy(h.inst);
		close(drm_fd);
		SKIP("no Vulkan device");
	}

	if (!avk_renderer_init(&h.renderer, h.dev, TARGET_FORMAT)) {
		CHECK(false, "renderer initialises");
		goto done;
	}
	h.target = make_target(h.dev);
	if (h.target == NULL) {
		CHECK(false, "output target allocates");
		goto done;
	}

	test_no_shadow_is_no_draw(&h);
	test_oracle_agreement(&h);
	test_falloff(&h);
	test_per_corner_radii(&h);
	test_oversized_radii(&h);
	test_interior_cutout(&h);
	test_alpha_and_opacity(&h);
	test_coloured_shadow_does_not_glow(&h);

done:
	if (h.target != NULL) {
		avk_image_destroy(h.dev, h.target);
	}
	if (h.renderer.dev != NULL) {
		avk_renderer_finish(&h.renderer);
	}
	if (h.dev != NULL) {
		avk_device_destroy(h.dev);
	}
	if (h.inst != NULL) {
		avk_instance_destroy(h.inst);
	}
	close(drm_fd);

	printf("----\n%d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
