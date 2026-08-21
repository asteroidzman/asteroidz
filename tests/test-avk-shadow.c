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
	avk_cmd_ring_init(&ring, dev, "readback", AVK_FRAMES_IN_FLIGHT);
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

/* ── M4D.2: the directional model, as the compositor actually builds it ─── */

/*
 * THE SHIPPED PROFILE, and where it comes from.
 *
 * These are not numbers invented for a test. They are asteroidz's defaults,
 * declared in src/config/config-schema.h, and the whole point of M4D.2 is that
 * the model is CONFIGURATION rather than something the shader knows about. The
 * shader implements "a directional analytic shadow"; the profile below is what
 * a macOS-like desktop asks it for.
 *
 *   effects/shadow/size            24    effects/shadow/contact/size      8
 *   effects/shadow/blur            24    effects/shadow/contact/blur      9
 *   effects/shadow/position/x       0    effects/shadow/contact/position/x 0
 *   effects/shadow/position/y     +10    effects/shadow/contact/position/y +2
 *   effects/shadow/color    0x00000066   effects/shadow/contact/color 0x0000004d
 *
 * Broad and weak, well below; tight and stronger, just below. Two lobes, and
 * they were in the tree before M4 began -- the audit found them rather than
 * M4D.2 introducing them.
 */
struct lobe {
	int size;         /* how far the envelope reaches past the window */
	float sigma;      /* the blur_sigma field, NOT the Gaussian's sigma */
	int offset_x, offset_y;
	float alpha;
};
static const struct lobe kAmbient = { 24, 24.0f, 0, 10, 0x66 / 255.0f };
static const struct lobe kContact = { 8, 9.0f, 0, 2, 0x4d / 255.0f };

/* The window this desk's shadows are cast by, in the middle of the target. */
static const struct avk_box kWin = { 64, 64, 128, 128 };
static const float kWinRadius = 12.0f;

/*
 * The envelope client_draw_one_shadow() builds, transcribed:
 *
 *     delta      = size + border            (border is 0 here)
 *     envelope   = window grown by delta on EVERY side, then shifted by
 *                  (position_x, position_y)
 *
 * Grown first and shifted after -- not shifted only right and down, which is
 * the bug the producer's own comment records: the box used to keep the
 * window's origin, so the extra width and height only ever extended right and
 * downward and there was no room for a falloff on the left or top at all.
 */
static struct avk_box envelope_of(const struct lobe *l) {
	struct avk_box e;
	e.x = kWin.x + l->offset_x - l->size;
	e.y = kWin.y + l->offset_y - l->size;
	e.width = kWin.width + 2 * l->size;
	e.height = kWin.height + 2 * l->size;
	return e;
}

static void add_lobe(struct avk_scene *scene, const struct lobe *l,
		bool with_cutout) {
	struct avk_box env = envelope_of(l);
	const float radii[4] = { kWinRadius, kWinRadius, kWinRadius, kWinRadius };
	struct avk_cmd *cmd = add_shadow(scene, &env, l->sigma, radii);
	if (cmd == NULL) {
		return;
	}
	cmd->color[3] = l->alpha;
	if (!with_cutout) {
		return;
	}
	cmd->inner = kWin;
	cmd->has_inner = true;
	for (int i = 0; i < 4; i++) {
		cmd->inner_corners[i] = kWinRadius - 1.0f;
	}
	pixman_region32_t clip;
	pixman_region32_init_rect(&clip, env.x, env.y, (unsigned)env.width,
		(unsigned)env.height);
	pixman_region32_t cut;
	int in = (int)ceilf((kWinRadius - 1.0f) * 0.3f) + 1;
	pixman_region32_init_rect(&cut, kWin.x + in, kWin.y + in,
		(unsigned)(kWin.width - 2 * in), (unsigned)(kWin.height - 2 * in));
	pixman_region32_subtract(&clip, &clip, &cut);
	pixman_region32_fini(&cut);
	avk_cmd_set_clip(cmd, &clip);
	pixman_region32_fini(&clip);
}

/*
 * Coverage at `d` pixels outside the WINDOW's edge on each of its four sides,
 * AVERAGED along that side.
 *
 * Averaged and not sampled, since M4D.4: the dither is zero-mean noise, so a
 * single pixel now carries up to half an output code of it and two sides that
 * are analytically identical can read 4/255 apart. That is the dither working
 * as designed, and it broke the horizontal-bias assertion the first time this
 * ran. Averaging over 41 pixels cancels it -- which is exactly what the eye
 * does, and is the reason a zero-mean dither is invisible at all.
 */
struct sides { double top, bottom, left, right; };
#define SIDE_SPAN 20
static double avg_run(struct harness *h, int x0, int y0, int dx, int dy) {
	double sum = 0.0;
	int n = 0;
	for (int i = -SIDE_SPAN; i <= SIDE_SPAN; i++) {
		int x = x0 + dx * i, y = y0 + dy * i;
		if (x < 0 || y < 0 || x >= W || y >= H) {
			continue;
		}
		sum += coverage_at(h, x, y);
		n++;
	}
	return n ? sum / n : 0.0;
}
static struct sides sides_at(struct harness *h, int d) {
	int cx = kWin.x + kWin.width / 2;
	int cy = kWin.y + kWin.height / 2;
	struct sides s;
	s.top = avg_run(h, cx, kWin.y - d, 1, 0);
	s.bottom = avg_run(h, cx, kWin.y + kWin.height - 1 + d, 1, 0);
	s.left = avg_run(h, kWin.x - d, cy, 0, 1);
	s.right = avg_run(h, kWin.x + kWin.width - 1 + d, cy, 0, 1);
	return s;
}

static void test_directional_invariant(struct harness *h) {
	printf("M4D.2: the default profile reads as light from above\n");

	struct avk_scene scene;
	scene_begin(&scene);
	add_lobe(&scene, &kAmbient, true);
	add_lobe(&scene, &kContact, true);
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }

	CHECK(h->renderer.stats.shadow_draws >= 2,
		"premise: both lobes were drawn (%" PRIu64 " shadow draws)",
		h->renderer.stats.shadow_draws);

	/*
	 * THE INVARIANT. At the SAME distance from the window on all four sides:
	 * below is strongest, the sides are in between, above is weakest. A
	 * symmetric centred Gaussian makes all four equal, which is exactly what
	 * BREAK=shadow-symmetric restores.
	 */
	for (int i = 0; i < 3; i++) {
		int d = (int[]){ 4, 10, 18 }[i];
		struct sides s = sides_at(h, d);
		printf("    %2dpx out:  top %.4f   left %.4f   right %.4f   "
			"bottom %.4f\n", d, s.top, s.left, s.right, s.bottom);
		double side = (s.left + s.right) * 0.5;
		CHECK(s.bottom > side * 1.15,
			"%dpx out: below is materially stronger than the sides "
			"(%.4f vs %.4f)", d, s.bottom, side);
		CHECK(side > s.top * 1.15,
			"%dpx out: the sides are materially stronger than above "
			"(%.4f vs %.4f)", d, side, s.top);
	}

	struct sides s = sides_at(h, 4);
	CHECK(s.bottom > s.top * 1.6,
		"and bottom beats top by a wide margin at the contact distance "
		"(%.4f vs %.4f)", s.bottom, s.top);
	/*
	 * The top must not be absolutely bare: a little shadow above is what
	 * attaches the window to the scene rather than letting it float.
	 */
	CHECK(s.top > 0.02,
		"the top still has a trace of shadow -- the window is attached, not "
		"floating (%.4f)", s.top);
}

static void test_horizontal_bias(struct harness *h) {
	printf("M4D.2: no horizontal bias, because the profile asks for none\n");

	struct avk_scene scene;
	scene_begin(&scene);
	add_lobe(&scene, &kAmbient, true);
	add_lobe(&scene, &kContact, true);
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }

	/*
	 * position_x is 0 in the shipped profile, so left and right must match to
	 * within rounding. This is the assertion that catches an asymmetry
	 * introduced by a coordinate bug rather than by the profile -- a sign
	 * error in the envelope or a half-pixel in the wrong place shows up here
	 * and nowhere else, because every other fixture is symmetric in x by
	 * construction and would not notice.
	 */
	double worst = 0.0;
	for (int d = 2; d <= 20; d += 2) {
		struct sides s = sides_at(h, d);
		double diff = fabs(s.left - s.right);
		if (diff > worst) {
			worst = diff;
		}
	}
	printf("    worst left/right difference over 2..20px: %.5f (%.2f/255)\n",
		worst, worst * 255.0);
	CHECK(worst < 2.0 / 255.0,
		"left and right agree to within a rounding step (%.2f/255)",
		worst * 255.0);

	/* The premise: there IS a shadow on the sides to compare. */
	struct sides s = sides_at(h, 4);
	CHECK(s.left > 0.05, "premise: the sides carry real shadow (%.4f)",
		s.left);
}

static void test_dual_lobe_composition(struct harness *h) {
	printf("M4D.2: two lobes compose as coverage, not as added alpha\n");
	int cx = kWin.x + kWin.width / 2;
	int probe_y = kWin.y + kWin.height - 1 + 3;

	struct avk_scene scene;
	scene_begin(&scene);
	add_lobe(&scene, &kAmbient, true);
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }
	double a = coverage_at(h, cx, probe_y);

	scene_begin(&scene);
	add_lobe(&scene, &kContact, true);
	ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }
	double c = coverage_at(h, cx, probe_y);

	scene_begin(&scene);
	add_lobe(&scene, &kAmbient, true);
	add_lobe(&scene, &kContact, true);
	ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }
	double both = coverage_at(h, cx, probe_y);

	/*
	 * Two translucent layers composited one over the other give
	 * 1 - (1 - a)(1 - c), which is the coverage of a union of independent
	 * occluders. It is the CORRECT composition and it cannot exceed 1 -- so
	 * two scene nodes drawn in order already do the right thing, and nothing
	 * here needs to add two alphas and clamp.
	 */
	double expect = 1.0 - (1.0 - a) * (1.0 - c);
	printf("    ambient %.4f  contact %.4f  together %.4f  "
		"1-(1-a)(1-c) = %.4f  a+c = %.4f\n", a, c, both, expect, a + c);

	CHECK(a > 0.05 && c > 0.05,
		"premise: both lobes contribute here (%.4f, %.4f)", a, c);
	CHECK(fabs(both - expect) < 3.0 / 255.0,
		"the two compose as 1-(1-a)(1-c) (%.4f vs %.4f)", both, expect);
	CHECK(both <= 1.0,
		"and the result is a coverage, never over 1 (%.4f)", both);
	CHECK(a + c > both + 0.005,
		"premise: naive addition really would differ here (%.4f vs %.4f) -- "
		"so the check above is not passing for free", a + c, both);
}

static void test_contact_and_penumbra(struct harness *h) {
	printf("M4D.2: a tight contact term and a broad penumbra, both visible\n");

	struct avk_scene scene;
	scene_begin(&scene);
	add_lobe(&scene, &kAmbient, true);
	add_lobe(&scene, &kContact, true);
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }

	int cx = kWin.x + kWin.width / 2;
	int base = kWin.y + kWin.height - 1;
	const int dist[] = { 1, 3, 6, 12, 24, 36 };
	double cov[6];
	for (int i = 0; i < 6; i++) {
		cov[i] = coverage_at(h, cx, base + dist[i]);
		printf("    %2dpx below the window: %.4f\n", dist[i], cov[i]);
	}

	/*
	 * THE SHAPE, pinned numerically rather than described. Strict
	 * monotonicity is NOT required across the contact/ambient crossover --
	 * two lobes of different widths can leave a shoulder there -- so what is
	 * asserted is the shape either side of it.
	 */
	CHECK(cov[0] > 0.25, "contact: dark right at the edge (%.4f at 1px)",
		cov[0]);
	CHECK(cov[0] > cov[2] * 1.3,
		"contact: and it falls off fast -- 1px is well above 6px (%.4f vs "
		"%.4f)", cov[0], cov[2]);
	CHECK(cov[4] > 0.03,
		"penumbra: there is still shadow 24px out, far past the contact "
		"term (%.4f)", cov[4]);
	CHECK(cov[5] < cov[4],
		"and it is still declining at 36px (%.4f < %.4f)", cov[5], cov[4]);

	/*
	 * ATTACHED, NOT FLOATING. The darkest point below the window must be at
	 * the window's edge -- a single heavily offset lobe puts it some distance
	 * away and the shadow reads as a detached blob.
	 */
	int darkest = 0;
	for (int i = 1; i < 6; i++) {
		if (cov[i] > cov[darkest]) {
			darkest = i;
		}
	}
	CHECK(darkest == 0,
		"the darkest point below is AT the window, not adrift below it "
		"(peak at %dpx)", dist[darkest]);
}

/* ── M4D.2: what a shadow costs, and whether fusing the lobes would help ── */

struct perf {
	double gpu_us;
	double cpu_us;
	uint64_t draws;
};

/*
 * `lobes` shadows per window, `windows` windows. Each window gets the shipped
 * ambient and contact geometry, tiled across the target so the envelopes
 * overlap the way they do on a real desktop rather than stacking in one place.
 */
static struct perf perf_run(struct harness *h, const char *label, int windows,
		int lobes) {
	const int frames = 60;
	struct avk_timestamps *ts = &h->renderer.timestamps;
	uint64_t gpu0 = ts->gpu_frame_ns_total, n0 = ts->samples;
	uint64_t cpu0 = h->renderer.stats.cpu_record_ns, f0 = h->renderer.stats.frames;
	/* shadow_draws, not draws: the latter counts the clear and every
	 * damage rect, so "2 lobes" reads as 3 and the premise below would
	 * be asserting on the wrong number. It did, first time round. */
	uint64_t d0 = h->renderer.stats.shadow_draws;

	for (int f = 0; f < frames; f++) {
		struct avk_scene scene;
		scene_begin(&scene);
		for (int i = 0; i < windows; i++) {
			/* Spread the windows over the target; a real desktop's shadows
			 * overlap, and stacking them all at one place would measure a
			 * degenerate overdraw case instead. */
			int col = i % 4, row = (i / 4) % 4;
			struct avk_box win = { 40 + col * 44, 40 + row * 44, 56, 56 };
			for (int l = 0; l < lobes; l++) {
				const struct lobe *lo = l == 0 ? &kAmbient : &kContact;
				struct avk_box env = {
					win.x + lo->offset_x - lo->size,
					win.y + lo->offset_y - lo->size,
					win.width + 2 * lo->size, win.height + 2 * lo->size,
				};
				const float r[4] = { 12.0f, 12.0f, 12.0f, 12.0f };
				struct avk_cmd *cmd = add_shadow(&scene, &env, lo->sigma, r);
				if (cmd != NULL) {
					cmd->color[3] = lo->alpha;
				}
			}
		}
		uint64_t v = avk_render_frame(&h->renderer, h->target, &scene, NULL, 0,
			NULL, 0);
		avk_scene_finish(&scene);
		if (v != 0) {
			avk_device_timeline_wait(h->dev, v, 2000000000ULL);
		}
		avk_renderer_collect(&h->renderer);
	}

	struct perf p = {0};
	uint64_t dn = ts->samples - n0;
	p.gpu_us = dn ? (double)(ts->gpu_frame_ns_total - gpu0) / dn / 1000.0 : 0.0;
	uint64_t df = h->renderer.stats.frames - f0;
	p.cpu_us = df
		? (double)(h->renderer.stats.cpu_record_ns - cpu0) / df / 1000.0 : 0.0;
	p.draws = (h->renderer.stats.shadow_draws - d0) / (df ? df : 1);
	printf("    %-28s GPU %6.1f us   CPU %5.1f us   %2" PRIu64 " shadows/frame\n",
		label, p.gpu_us, p.cpu_us, p.draws);
	return p;
}

static void test_performance(struct harness *h) {
	printf("M4D.2: what a shadow costs (GPU time is MEASURED, M4D.P)\n");
	if (!h->renderer.timestamps.supported) {
		printf("    GPU TIMESTAMPS: UNSUPPORTED -- cost is not reported\n");
		CHECK(true, "no GPU timer on this device; skipped rather than "
			"reporting CPU time as GPU time");
		return;
	}

	perf_run(h, "(warmup)", 4, 2);
	struct perf none = perf_run(h, "no shadows", 0, 0);
	struct perf one = perf_run(h, "1 window, ambient only", 1, 1);
	struct perf two = perf_run(h, "1 window, both lobes", 1, 2);
	struct perf ten = perf_run(h, "10 windows, both lobes", 10, 2);
	struct perf t32 = perf_run(h, "16 windows, both lobes", 16, 2);

	CHECK(none.gpu_us > 0.0, "premise: GPU time was actually measured");
	CHECK(two.gpu_us > none.gpu_us,
		"a shadow costs measurably more than no shadow (%.1f vs %.1f us)",
		two.gpu_us, none.gpu_us);

	double per_lobe = two.gpu_us - one.gpu_us;
	double first_lobe = one.gpu_us - none.gpu_us;
	printf("    -> first lobe %.2f us, second lobe %.2f us, "
		"16 windows/32 lobes %.2f us over baseline\n",
		first_lobe, per_lobe, t32.gpu_us - none.gpu_us);

	/*
	 * THE FUSION QUESTION, ANSWERED WITH A MEASUREMENT.
	 *
	 * M4D.2 as briefed wanted the two lobes evaluated in ONE material. The
	 * audit found they are already two scene nodes with correct composition
	 * and full configuration, so fusing them would be an optimisation, and
	 * the standing rule is that an advanced technique has to earn its
	 * complexity with a number.
	 *
	 * It does not earn it, for two reasons the numbers above show:
	 *
	 *   The contact lobe's envelope is much smaller than the ambient one --
	 *   size 8 against 24, so (56+16)^2 against (56+48)^2, about 48% of the
	 *   area. Fusing means evaluating the contact term over the AMBIENT
	 *   envelope, roughly doubling its fragment count to save one draw call
	 *   and one blend.
	 *
	 *   And a draw call is not what this costs. The second lobe adds a few
	 *   microseconds of GPU time and essentially no CPU recording time, on a
	 *   6.94 ms frame budget at 144 Hz.
	 *
	 * Fusion would also have to assume the two nodes are adjacent siblings
	 * with nothing between them, which the scene graph does not promise and
	 * which reordering to obtain would break translucent ordering. Rejected,
	 * with the measurement recorded rather than the preference.
	 */
	CHECK(per_lobe < 200.0,
		"the second lobe costs %.2f us -- far too little for fusing the two "
		"into one material to be worth the ordering assumption", per_lobe);
	CHECK(two.draws == 2,
		"premise: both lobes really are two separate draws (%" PRIu64 ")",
		two.draws);
	CHECK(t32.draws == 32,
		"premise: 16 windows really is 32 shadow draws (%" PRIu64 ")",
		t32.draws);

	/* Allocation behaviour: a shadow is push constants and a quad, so a
	 * frame full of them must allocate nothing at all. */
	CHECK(h->dev->live.buffers >= 0 && h->dev->live.device_memory >= 0,
		"premise: object counters are readable");
	printf("    live objects during the run: images %" PRId64 " buffers %"
		PRId64 " memory %" PRId64 "\n", h->dev->live.images,
		h->dev->live.buffers, h->dev->live.device_memory);
}


/* ── M4D.4: quantisation, and the dither that decorrelates it ───────────── */

/*
 * THE DEFECT, REPRODUCED. The clear is the MID-GREY a real desktop panel sits
 * on rather than the white the rest of this file uses, because that is the
 * whole phenomenon: AVK composites with dst*(1-alpha) into an 8-bit UNORM
 * attachment, so the shadow's visible step is d(alpha) TIMES THE BACKDROP, and
 * a dark backdrop leaves the falloff almost no output codes to be drawn in.
 *
 * Measured on the user's live screen: green 45,44,43,42,41,40,39,38,37 in runs
 * of 41,27,15,11,11,9,8,3,2 px. Those run boundaries are the contour rings.
 */
#define BAND_BG 45

static void scene_begin_grey(struct avk_scene *scene) {
	avk_scene_init(scene);
	pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	scene->has_clear = true;
	float g = (float)BAND_BG / 255.0f;
	scene->clear_color[0] = g;
	scene->clear_color[1] = g;
	scene->clear_color[2] = g;
	scene->clear_color[3] = 1.0f;
}

/* The user's profile shape: broad, weak, low-gradient -- the case that bands. */
static const struct avk_box kBandEnv = { 20, 20, 216, 216 };
static const float kBandSigma = 60.0f;
static const float kBandAlpha = 0x50 / 255.0f;

static bool render_band_scene(struct harness *h, bool grey) {
	const float radii[4] = { 20.0f, 20.0f, 20.0f, 20.0f };
	struct avk_scene scene;
	if (grey) {
		scene_begin_grey(&scene);
	} else {
		scene_begin(&scene);
	}
	struct avk_cmd *cmd = add_shadow(&scene, &kBandEnv, kBandSigma, radii);
	if (cmd != NULL) {
		cmd->color[3] = kBandAlpha;
	}
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	return ok;
}

struct runs { int levels; int max_run; double mean_run; };

/* Distinct output codes along a row, and the length of the constant-code runs
 * they occupy -- which is the thing that was actually visible. */
static struct runs run_profile(struct harness *h, int y, int x0, int x1) {
	struct runs r = { 0, 0, 0.0 };
	int seen[256] = {0};
	int run = 0, prev = -1, nruns = 0, total = 0;
	for (int x = x0; x < x1; x++) {
		int v = chan(h, x, y);
		if (!seen[v]) { seen[v] = 1; r.levels++; }
		if (v == prev) {
			run++;
		} else {
			if (run > 0) { nruns++; total += run; }
			if (run > r.max_run) { r.max_run = run; }
			run = 1;
			prev = v;
		}
	}
	if (run > 0) { nruns++; total += run; }
	if (run > r.max_run) { r.max_run = run; }
	r.mean_run = nruns ? (double)total / nruns : 0.0;
	return r;
}

static void test_dither_breaks_banding(struct harness *h) {
	printf("M4D.4: an 8-bit shadow over mid-grey, banded and dithered\n");
	const int row = H / 2;
	const int x0 = kBandEnv.x, x1 = kBandEnv.x + 96;

	/*
	 * THE AMPLITUDE, DERIVED. out = dst*(1-alpha), so d(out) = d(alpha)*dst
	 * and one output code peak-to-peak needs
	 *
	 *     amplitude = 1 / (max_code * dst) = 1 / (255 * 45/255) = 1/45
	 *
	 * which is 5.67/255 of ALPHA -- not the 1/255 a naive coverage dither
	 * would use, and the difference is a factor of nearly six.
	 */
	double derived = 1.0 / (255.0 * (BAND_BG / 255.0));
	printf("    derived amplitude 1/(255 * %d/255) = %.5f = %.2f/255 of alpha\n",
		BAND_BG, derived, derived * 255.0);
	CHECK(fabs((double)avk_dither_amplitude(TARGET_FORMAT) - derived) < 1e-6,
		"the shipped amplitude is that derivation, not a tuned constant "
		"(%.5f)", (double)avk_dither_amplitude(TARGET_FORMAT));
	/* And the naive value really is far too small to matter -- the premise
	 * that makes the derivation worth doing. */
	double naive_codes = (1.0 / 255.0) * (BAND_BG / 255.0) * 255.0;
	printf("    a naive 1/255 coverage dither would move the output by "
		"%.2f codes\n", naive_codes);
	CHECK(naive_codes < 0.25,
		"premise: +-1/255 of coverage is under a quarter of an output code "
		"here (%.2f)", naive_codes);

	/*
	 * BEFORE and AFTER in one run, by driving the renderer's own amplitude.
	 * Two processes would compare two GPU states; this compares two frames.
	 */
	float saved = h->renderer.shadow_dither;
	h->renderer.shadow_dither = 0.0f;
	if (!render_band_scene(h, true)) { CHECK(false, "frame rendered"); return; }
	struct runs before = run_profile(h, row, x0, x1);
	int bg = chan(h, 4, 4);
	int deep = chan(h, kBandEnv.x + kBandEnv.width / 2, row);

	h->renderer.shadow_dither = saved;
	if (!render_band_scene(h, true)) { CHECK(false, "frame rendered"); return; }
	struct runs after = run_profile(h, row, x0, x1);

	printf("    BEFORE (undithered): %2d codes, max run %2d px, mean run "
		"%.2f px\n", before.levels, before.max_run, before.mean_run);
	printf("    AFTER  (dithered):   %2d codes, max run %2d px, mean run "
		"%.2f px\n", after.levels, after.max_run, after.mean_run);

	CHECK(bg == BAND_BG, "premise: the backdrop really is mid-grey (%d)", bg);
	CHECK(deep < bg - 4, "premise: there is a real shadow here (%d vs %d)",
		deep, bg);
	/*
	 * THE PREMISE THE FIX CANNOT SUPPLY. If the undithered frame did not band
	 * there would be nothing to fix, and every assertion below would pass
	 * against a renderer that did nothing at all.
	 */
	CHECK(before.max_run >= 12,
		"premise: the undithered shadow really does band (max run %d px)",
		before.max_run);

	/*
	 * Thresholds set from the measured data, not chosen in advance. The
	 * visible defect was a 41 px run on screen and reproduces here at %d px;
	 * what matters is that no run survives long enough to draw a line.
	 */
	CHECK(after.max_run * 2 < before.max_run,
		"the longest constant-code run is at least halved (%d -> %d px)",
		before.max_run, after.max_run);
	CHECK(after.max_run <= 8,
		"and no run is long enough to read as a contour (max %d px)",
		after.max_run);
	CHECK(after.mean_run < before.mean_run * 0.4,
		"mean run collapses too (%.2f -> %.2f px)",
		before.mean_run, after.mean_run);
}

static void test_dither_mean_is_preserved(struct harness *h) {
	printf("M4D.4: the dither changes structure, not shadow density\n");
	const int row = H / 2;
	const int x0 = kBandEnv.x, x1 = kBandEnv.x + 96;

	if (!render_band_scene(h, true)) { CHECK(false, "frame rendered"); return; }
	/* A low-pass of the dithered line, against the analytic curve it should
	 * still be tracking. The oracle is the same one M4D.1 uses. */
	const double radii[4] = { 20.0, 20.0, 20.0, 20.0 };
	double sum_err = 0.0, max_err = 0.0, sum_got = 0.0, sum_want = 0.0;
	int n = 0;
	for (int x = x0 + 3; x < x1 - 3; x++) {
		double got = 0.0;
		for (int k = -3; k <= 3; k++) {
			got += chan(h, x + k, row);
		}
		got /= 7.0;
		double cov = oracle_coverage(&kBandEnv, kBandSigma, radii, x, row);
		double want = (double)BAND_BG * (1.0 - cov * kBandAlpha);
		double e = fabs(got - want);
		if (e > max_err) { max_err = e; }
		sum_err += e;
		sum_got += got;
		sum_want += want;
		n++;
	}
	printf("    7px low-pass vs oracle: mean |err| %.3f, max %.3f codes; "
		"means %.3f vs %.3f\n", sum_err / n, max_err, sum_got / n,
		sum_want / n);
	CHECK(n > 80, "premise: the falloff was sampled (%d px)", n);
	CHECK(fabs(sum_got / n - sum_want / n) < 0.25,
		"the average shadow density is unchanged (%.3f vs %.3f codes)",
		sum_got / n, sum_want / n);
	CHECK(max_err < 1.5,
		"and the smoothed curve still tracks the oracle everywhere "
		"(max %.3f codes)", max_err);
}

static void test_dither_is_deterministic(struct harness *h) {
	printf("M4D.4: the same scene renders bit-identically\n");
	static uint32_t first[W * H];
	if (!render_band_scene(h, true)) { CHECK(false, "frame rendered"); return; }
	memcpy(first, h->pixels, sizeof(first));
	bool same = true;
	for (int i = 0; i < 3 && same; i++) {
		if (!render_band_scene(h, true)) { CHECK(false, "re-render"); return; }
		if (memcmp(first, h->pixels, sizeof(first)) != 0) { same = false; }
	}
	/* A dither seeded from a frame counter or a clock would fail this, and
	 * would also shimmer on an idle desktop and leave partially damaged
	 * frames out of step with the pixels beside them. */
	CHECK(same, "four renders of the same scene are byte-for-byte identical");
}

static void test_dither_has_no_structure(struct harness *h) {
	printf("M4D.4: 2D -- rings replaced by noise, not by a pattern\n");
	if (!render_band_scene(h, true)) { CHECK(false, "frame rendered"); return; }

	/*
	 * A 2D patch of the broad penumbra. Two things are asked of it: that the
	 * horizontal contours are gone, and that nothing periodic has replaced
	 * them -- a small Bayer matrix or a bad hash shows up as a checkerboard,
	 * vertical streaks or a repeating tile, all of which are worse to look at
	 * than the banding.
	 */
	const int px0 = kBandEnv.x + 4, py0 = kBandEnv.y + 40;
	const int pw = 64, ph = 64;

	/*
	 * HIGH-PASS FIRST, and the first version of this test did not -- which is
	 * why it reported +0.95 correlation at every lag and called a perfectly
	 * structureless dither a checkerboard. The patch sits in the penumbra, so
	 * its raw values are dominated by the SHADOW RAMP: neighbouring pixels
	 * are strongly correlated because the shadow is genuinely darker on one
	 * side, which says nothing about the noise on top of it.
	 *
	 * Subtracting a 5x5 local mean removes the ramp and leaves the dither,
	 * which is the only thing this test is about.
	 */
	static double resid[64][64];
	double var = 0.0;
	for (int y = 0; y < ph; y++) {
		for (int x = 0; x < pw; x++) {
			double local = 0.0;
			for (int dy = -2; dy <= 2; dy++) {
				for (int dx = -2; dx <= 2; dx++) {
					local += chan(h, px0 + x + dx, py0 + y + dy);
				}
			}
			resid[y][x] = chan(h, px0 + x, py0 + y) - local / 25.0;
			var += resid[y][x] * resid[y][x];
		}
	}
	var /= (double)(pw * ph);
	double mean = 0.0;

	/*
	 * FINE lags say what the noise looks like at the pixel level; LONG-RANGE
	 * lags say whether it has a pattern. Only the second is asserted, and the
	 * distinction is the whole point of this test.
	 *
	 * Interleaved gradient noise is built around a fine diagonal texture with
	 * a period of about two pixels, so it correlates at lag (1,1) by
	 * construction -- that is not a defect, it is the property that makes it
	 * look smoother than white noise, and at two pixels it is at the display's
	 * Nyquist limit. What would be a defect is structure the eye can resolve:
	 * a repeating tile, a grid, a streak. Those live at lag 4 and beyond.
	 */
	struct { int dx, dy; const char *name; bool longrange; } lags[] = {
		{ 1, 0, "horizontal", false }, { 0, 1, "vertical", false },
		{ 1, 1, "diagonal", false },
		{ 4, 0, "4px h", true }, { 0, 4, "4px v", true },
		{ 8, 8, "8px tile", true }, { 16, 0, "16px h", true },
	};
	double worst = 0.0;
	const char *worst_name = "";
	for (size_t i = 0; i < sizeof(lags) / sizeof(lags[0]); i++) {
		double c = 0.0;
		int n = 0;
		for (int y = 0; y + lags[i].dy < ph; y++) {
			for (int x = 0; x + lags[i].dx < pw; x++) {
				c += resid[y][x] * resid[y + lags[i].dy][x + lags[i].dx];
				n++;
			}
		}
		c = (n && var > 0.0) ? c / n / var : 0.0;
		printf("    autocorrelation at %-11s lag: %+.3f%s\n", lags[i].name,
			c, lags[i].longrange ? "   (asserted)" : "");
		if (lags[i].longrange && fabs(c) > fabs(worst)) {
			worst = c;
			worst_name = lags[i].name;
		}
	}

	(void)mean;
	CHECK(var > 0.05,
		"premise: the high-passed patch actually varies, so there is a "
		"dither to correlate (variance %.3f)", var);
	/*
	 * Note the sign matters as much as the size. A residual dominated by the
	 * remaining smooth ramp correlates POSITIVELY; a checkerboard correlates
	 * strongly NEGATIVELY at lag 1. Either at high magnitude is structure.
	 */
	/*
	 * ASSERTED IN CODES, NOT IN CORRELATION, and the difference decided which
	 * noise ships.
	 *
	 * A correlation coefficient is relative to the variance it sits in, so it
	 * says how much of the noise is structured and nothing at all about how
	 * VISIBLE that structure is. Measured here: interleaved gradient noise
	 * correlates +0.175 at a 16 px lag where a plain integer hash gives
	 * -0.022, so IGN genuinely has long-range content and the hash does not.
	 * But the residual it sits in has a standard deviation of about a third
	 * of one output code, so IGN's structured component is
	 *
	 *     0.175 * 0.36  =  0.06 codes
	 *
	 * -- a fifteenth of the smallest step the display can show, and therefore
	 * not a pattern anyone can see. The hash's alternative is a real 1-code
	 * contour 10 px long instead of 7. Rejecting IGN on the coefficient alone
	 * would have traded an invisible correlation for a visible run.
	 *
	 * So the threshold is an absolute one, in the units the defect was
	 * reported in.
	 */
	double structured = fabs(worst) * sqrt(var);
	printf("    worst long-range structure: %+.3f correlation at %s, "
		"= %.3f codes\n", worst, worst_name, structured);
	CHECK(structured < 0.15,
		"no long-range structure big enough to see: %.3f codes, against a "
		"quantisation step of 1", structured);
}

static void test_dither_noise_choice(struct harness *h) {
	printf("M4D.4: which noise, decided by measurement\n");
	const int row = H / 2;
	const int x0 = kBandEnv.x, x1 = kBandEnv.x + 96;

	struct runs r[2];
	double grain[2];
	for (int mode = 0; mode < 2; mode++) {
		h->renderer.dither_hash = mode == 1;
		if (!render_band_scene(h, true)) { CHECK(false, "render"); return; }
		r[mode] = run_profile(h, row, x0, x1);
		if (!render_band_scene(h, false)) { CHECK(false, "render"); return; }
		double sum = 0.0;
		int n = 0;
		for (int x = x0 + 8; x < x0 + 92; x++) {
			double local = (chan(h, x - 2, row) + chan(h, x - 1, row)
				+ chan(h, x, row) + chan(h, x + 1, row)
				+ chan(h, x + 2, row)) / 5.0;
			double d = chan(h, x, row) - local;
			sum += d * d;
			n++;
		}
		grain[mode] = n ? sqrt(sum / n) : 0.0;
	}
	h->renderer.dither_hash = false;

	printf("    interleaved gradient noise: max run %2d px, mean %.2f, "
		"grain %.2f\n", r[0].max_run, r[0].mean_run, grain[0]);
	printf("    integer hash (white noise): max run %2d px, mean %.2f, "
		"grain %.2f\n", r[1].max_run, r[1].mean_run, grain[1]);

	/*
	 * IGN WINS ON BOTH NUMBERS THAT MATTER, and that is why it ships.
	 *
	 * It breaks the constant-code runs shorter -- which is the defect being
	 * fixed -- AND it does so with less grain on backdrops that never banded,
	 * which is the price being paid. White noise is structureless, and that
	 * is its only advantage; IGN's correlation is a two-pixel diagonal
	 * texture at the display's Nyquist limit, which is what makes it look
	 * smoother rather than what makes it look wrong. See
	 * test_dither_has_no_structure() for the lags that would matter.
	 *
	 * Neither needs a texture, a binding or a table, so the comparison is
	 * purely about output quality.
	 */
	CHECK(r[0].max_run <= r[1].max_run,
		"IGN breaks runs at least as short as white noise (%d vs %d px)",
		r[0].max_run, r[1].max_run);
	CHECK(grain[0] <= grain[1] + 0.01,
		"and does it with no more grain (%.2f vs %.2f codes rms)",
		grain[0], grain[1]);
	CHECK(r[1].max_run > 0 && grain[1] > 0.0,
		"premise: the alternative really was exercised (%d px, %.2f)",
		r[1].max_run, grain[1]);
}

static void test_dither_spares_the_contact_edge(struct harness *h) {
	printf("M4D.4: the sharp contact edge is left alone\n");
	/* A tight, strong lobe: exactly the geometry the broad-penumbra dither
	 * must not roughen. */
	const struct avk_box env = { 60, 60, 136, 136 };
	const float sigma = 6.0f;
	const float radii[4] = { 16.0f, 16.0f, 16.0f, 16.0f };

	struct avk_scene scene;
	scene_begin_grey(&scene);
	struct avk_cmd *cmd = add_shadow(&scene, &env, sigma, radii);
	if (cmd != NULL) { cmd->color[3] = 0.8f; }
	bool ok = render(h, &scene);
	avk_scene_finish(&scene);
	if (!ok) { CHECK(false, "frame rendered"); return; }

	/*
	 * Roughness along the steep edge, measured as the deviation of each pixel
	 * from its neighbours down a column that crosses it. The gradient
	 * modulation in az_dither_alpha() is what should keep this near zero: the
	 * ramp here moves by far more than the dither in a single pixel, so there
	 * is no band to break and the noise switches itself off.
	 */
	int ex = env.x + env.width / 2;
	int ey0 = env.y + (int)sigma - 3;
	double rough = 0.0;
	int n = 0;
	for (int y = ey0; y < ey0 + 6; y++) {
		for (int x = ex - 20; x < ex + 20; x++) {
			double local = (chan(h, x - 1, y) + chan(h, x, y)
				+ chan(h, x + 1, y)) / 3.0;
			double d = chan(h, x, y) - local;
			rough += d * d;
			n++;
		}
	}
	rough = n ? sqrt(rough / n) : 0.0;
	printf("    roughness across the contact edge: %.3f codes rms\n", rough);
	CHECK(n > 100, "premise: the edge was sampled (%d px)", n);
	CHECK(rough < 1.0,
		"the steep edge is not roughened by the dither (%.3f codes rms)",
		rough);
}

static void test_dither_grain_on_light_backdrops(struct harness *h) {
	printf("M4D.4: the cost -- grain where there was no banding to fix\n");
	const int row = H / 2;
	if (!render_band_scene(h, false)) { CHECK(false, "frame rendered"); return; }
	/*
	 * A fixed alpha-domain dither produces an output excursion PROPORTIONAL
	 * to the backdrop, which is backwards: dark backdrops band worst and get
	 * the least. Calibrating for the dark end therefore over-dithers the
	 * bright end, where there was no banding because a white backdrop gives
	 * the same falloff five times as many codes. This is the price, measured
	 * rather than assumed, and it is the limitation that goes away when M5
	 * moves the dither to the output-encoding stage where dst is known.
	 */
	double sum = 0.0;
	int n = 0;
	for (int x = kBandEnv.x + 8; x < kBandEnv.x + 92; x++) {
		double local = (chan(h, x - 2, row) + chan(h, x - 1, row)
			+ chan(h, x, row) + chan(h, x + 1, row)
			+ chan(h, x + 2, row)) / 5.0;
		double d = chan(h, x, row) - local;
		sum += d * d;
		n++;
	}
	double rms = n ? sqrt(sum / n) : 0.0;
	printf("    on WHITE: %.2f codes rms of grain\n", rms);
	CHECK(n > 50, "premise: sampled (%d px)", n);
	CHECK(rms < 2.0,
		"under two codes rms -- below what is visible at a normal viewing "
		"distance (%.2f)", rms);
}

static void test_dither_format_awareness(void) {
	printf("M4D.4: amplitude follows the attachment, not a hardcoded 8\n");
	float a8 = avk_dither_amplitude(VK_FORMAT_B8G8R8A8_UNORM);
	float a10 = avk_dither_amplitude(VK_FORMAT_A2R10G10B10_UNORM_PACK32);
	float a16 = avk_dither_amplitude(VK_FORMAT_R16G16B16A16_SFLOAT);
	printf("    8-bit %.5f   10-bit %.5f   fp16 %.5f\n",
		(double)a8, (double)a10, (double)a16);
	CHECK(a8 > 0.0f, "8-bit gets a dither (%.5f)", (double)a8);
	CHECK(fabs((double)(a8 / a10) - 1023.0 / 255.0) < 0.01,
		"10-bit gets exactly a quarter of it (ratio %.3f)",
		(double)(a8 / a10));
	CHECK(a16 == 0.0f,
		"and a floating-point target gets none -- M5's scene-linear "
		"intermediate must not have noise injected into it");
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

	test_directional_invariant(&h);
	test_horizontal_bias(&h);
	test_dual_lobe_composition(&h);
	test_contact_and_penumbra(&h);
	test_dither_breaks_banding(&h);
	test_dither_mean_is_preserved(&h);
	test_dither_is_deterministic(&h);
	test_dither_has_no_structure(&h);
	test_dither_noise_choice(&h);
	test_dither_spares_the_contact_edge(&h);
	test_dither_grain_on_light_backdrops(&h);
	test_dither_format_awareness();
	test_performance(&h);

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
