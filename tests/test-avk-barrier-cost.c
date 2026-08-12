/*
 * M4F.3/.5: what does vkCmdPipelineBarrier2 actually cost, and why?
 *
 * THE QUESTION. M4E measured the same two barrier calls at ~1.9 us headless and
 * ~44 us in a live session at comparable resolution, and the leading
 * explanation offered was DCC-compressed scan-out. That was circumstantial: the
 * only evidence was that the two paths use different modifiers. M4F multiplies
 * barrier calls, so the cause has to be established before the multiplying, not
 * after.
 *
 * WHY A BENCHMARK RATHER THAN MORE LIVE READINGS. A live compositor varies in
 * every dimension at once -- image origin, modifier, layout, queue family,
 * barrier count, resolution, CPU load. Reading a single number out of it can
 * only ever produce another hypothesis. This isolates ONE variable at a time on
 * the same device, in the same process, back to back.
 *
 * WHAT IS MEASURED. vkCmdPipelineBarrier2 is a COMMAND RECORDING call: it
 * writes into a command buffer and returns. It does no GPU work and waits for
 * nothing. So its cost is CPU time inside the driver, and that is what is timed
 * here -- with the command buffer in the recording state and nothing submitted,
 * so no GPU behaviour can contaminate the number.
 *
 * Exits 77 (skip) with no GPU or no render node.
 */

#define _POSIX_C_SOURCE 200809L

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "render/vulkan/command/avk_command.h"
#include "render/vulkan/dmabuf/avk_dmabuf.h"
#include "render/vulkan/image/avk_upload.h"

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

/* Enough repetitions that a microsecond-scale cost is many clock ticks, few
 * enough that the command buffer does not grow absurd. */
#define REPS 2000

struct bench {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_dmabuf_importer importer;
	struct gbm_device *gbm;
	int drm_fd;
	struct avk_cmd_ring ring;
};

static uint64_t now_ns(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* ── image construction ─────────────────────────────────────────────────── */

static struct avk_image *make_owned(struct avk_device *dev, uint32_t w,
		uint32_t h) {
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = VK_FORMAT_B8G8R8A8_UNORM;
	image->extent = (VkExtent2D){ w, h };
	image->plane_count = 1;
	image->origin = AVK_IMAGE_OWNED;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = image->format,
		.extent = { w, h, 1 },
		.mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			| VK_IMAGE_USAGE_SAMPLED_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(dev->dev, &info, NULL, &image->image) != VK_SUCCESS) {
		free(image);
		return NULL;
	}
	AVK_LIVE_INC(dev, images);
	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(dev->dev, image->image, &reqs);
	uint32_t type = 0;
	if (!avk_find_memory_type(dev, reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (vkAllocateMemory(dev->dev, &alloc, NULL, &image->memory[0])
			!= VK_SUCCESS) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	AVK_LIVE_INC(dev, device_memory);
	image->memory_count = 1;
	vkBindImageMemory(dev->dev, image->image, image->memory[0], 0);
	return image;
}

/*
 * A dma-buf image with a CHOSEN modifier.
 *
 * `want_dcc` selects between a modifier whose name contains DCC and one that
 * does not, out of whatever this device actually advertises as renderable.
 * Both come from the same table, are the same format and the same size, so the
 * only thing that differs between the two images is the compression state --
 * which is exactly the variable under test.
 */
static struct avk_image *make_dmabuf(struct bench *b, uint32_t w, uint32_t h,
		bool want_dcc, uint64_t *out_modifier) {
	const struct avk_format_caps *caps =
		avk_format_table_find(&b->importer.table, DRM_FORMAT_XRGB8888);
	if (caps == NULL) {
		return NULL;
	}
	/*
	 * EVERY candidate of the wanted kind, handed to gbm at once, which is what
	 * the API is for -- it picks one it can actually allocate. The first
	 * version took the first match and gave up when gbm refused it, concluding
	 * "no DCC modifier available" while five more were sitting in the list.
	 *
	 * NOT filtered on plane_count either: every DCC modifier this device offers
	 * carries 2 or 3 planes (the compression metadata), so a single-plane
	 * filter rejects exactly the case under test.
	 */
	uint64_t mods[32];
	unsigned count = 0;
	for (size_t i = 0; i < caps->render_mod_count && count < 32; i++) {
		uint64_t mod = caps->render_mods[i].modifier;
		if (mod == DRM_FORMAT_MOD_LINEAR || mod == DRM_FORMAT_MOD_INVALID) {
			continue;
		}
		/* AMD encodes DCC at bit 13 of the modifier. Reading the bit rather
		 * than parsing a human-readable name: the bit is the contract. */
		bool has_dcc = ((mod >> 13) & 1) != 0;
		if (has_dcc == want_dcc) {
			mods[count++] = mod;
		}
	}
	if (count == 0) {
		return NULL;
	}

	/*
	 * RENDERING only. SCANOUT needs the KMS primary node and this test opens a
	 * render node so it can run with no display at all. The modifier is the
	 * variable under test and is selected explicitly either way; what this
	 * cannot do is prove anything about a buffer KMS is actively scanning out,
	 * which is stated as a limit rather than glossed.
	 */
	struct gbm_bo *bo = gbm_bo_create_with_modifiers2(b->gbm, w, h,
		DRM_FORMAT_XRGB8888, mods, count, GBM_BO_USE_RENDERING);
	if (bo == NULL) {
		printf("  ---- gbm refused all %u %s modifiers\n", count,
			want_dcc ? "DCC" : "non-DCC");
		return NULL;
	}
	struct avk_dmabuf_attributes attribs = {
		.width = (int32_t)w, .height = (int32_t)h,
		.format = DRM_FORMAT_XRGB8888,
		.modifier = gbm_bo_get_modifier(bo),
		.n_planes = gbm_bo_get_plane_count(bo),
	};
	for (int i = 0; i < attribs.n_planes; i++) {
		attribs.fd[i] = gbm_bo_get_fd_for_plane(bo, i);
		attribs.offset[i] = gbm_bo_get_offset(bo, i);
		attribs.stride[i] = gbm_bo_get_stride_for_plane(bo, i);
	}
	if (out_modifier != NULL) {
		*out_modifier = attribs.modifier;
	}
	struct avk_image *image = avk_dmabuf_import(&b->importer, &attribs, true);
	if (image == NULL) {
		printf("  ---- import failed for 0x%016" PRIx64 " (%d planes)\n",
			attribs.modifier, attribs.n_planes);
	}
	for (int i = 0; i < attribs.n_planes; i++) {
		if (attribs.fd[i] >= 0) {
			close(attribs.fd[i]);
		}
	}
	gbm_bo_destroy(bo);
	return image;
}

/* ── the measurement ────────────────────────────────────────────────────── */

/*
 * Time `count` image barriers issued as `calls` separate vkCmdPipelineBarrier2
 * invocations, repeated REPS times. Returns nanoseconds per CALL.
 *
 * The barriers alternate between two layouts so that every one is a genuine
 * transition rather than a no-op the driver may recognise and drop -- a
 * benchmark of barriers the driver elides measures nothing.
 */
static double time_barriers(struct bench *b, struct avk_image **images,
		uint32_t count, uint32_t calls, VkImageLayout a, VkImageLayout c,
		bool foreign) {
	VkCommandBuffer cb = avk_cmd_ring_begin(&b->ring);
	if (cb == VK_NULL_HANDLE) {
		return -1.0;
	}

	VkImageMemoryBarrier2 barriers[16];
	uint32_t per_call = count / calls;
	if (per_call == 0) {
		per_call = 1;
	}

	uint64_t t0 = now_ns();
	for (int rep = 0; rep < REPS; rep++) {
		for (uint32_t call = 0; call < calls; call++) {
			for (uint32_t i = 0; i < per_call; i++) {
				uint32_t idx = (call * per_call + i) % count;
				bool up = (rep & 1) == 0;
				barriers[i] = (VkImageMemoryBarrier2){
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
					.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
					.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
					.oldLayout = up ? a : c,
					.newLayout = up ? c : a,
					.srcQueueFamilyIndex = foreign
						? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = foreign
						? b->dev->caps.graphics_family
						: VK_QUEUE_FAMILY_IGNORED,
					.image = images[idx]->image,
					.subresourceRange = {
						VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
				};
			}
			VkDependencyInfo dep = {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = per_call,
				.pImageMemoryBarriers = barriers,
			};
			vkCmdPipelineBarrier2(cb, &dep);
		}
	}
	uint64_t t1 = now_ns();

	/* Never submitted. This measures recording, and a submission would drag
	 * scheduling and GPU execution into a number that is neither. */
	avk_cmd_ring_abandon(&b->ring);
	return (double)(t1 - t0) / (double)(REPS * calls);
}

/*
 * Time each call INDIVIDUALLY, the way the production instrument does, and
 * report the distribution rather than the mean.
 *
 * This exists to test the instrument itself. vkCmdPipelineBarrier2 is a
 * ~100 ns event and a bracketed clock_gettime pair costs ~37 ns of that, so a
 * per-call mean is dominated by whatever happens to interrupt the thread during
 * the window. If the median is small and the mean is not, the mean is measuring
 * the scheduler.
 */
static void time_each_call(struct bench *b, struct avk_image *image) {
	VkCommandBuffer cb = avk_cmd_ring_begin(&b->ring);
	if (cb == VK_NULL_HANDLE) {
		return;
	}
	static uint64_t samples[REPS];
	for (int rep = 0; rep < REPS; rep++) {
		VkImageMemoryBarrier2 bar = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			.oldLayout = (rep & 1) ? VK_IMAGE_LAYOUT_GENERAL
				: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.newLayout = (rep & 1) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				: VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = image->image,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		VkDependencyInfo dep = {
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &bar,
		};
		uint64_t t0 = now_ns();
		vkCmdPipelineBarrier2(cb, &dep);
		uint64_t t1 = now_ns();
		samples[rep] = t1 - t0;
	}
	avk_cmd_ring_abandon(&b->ring);

	/* Insertion-free order statistics: a simple sort is fine at this size. */
	for (int i = 1; i < REPS; i++) {
		uint64_t v = samples[i];
		int j = i - 1;
		while (j >= 0 && samples[j] > v) { samples[j + 1] = samples[j]; j--; }
		samples[j + 1] = v;
	}
	uint64_t total = 0;
	for (int i = 0; i < REPS; i++) {
		total += samples[i];
	}
	printf("  ---- per-call timing, %d samples:\n", REPS);
	printf("       p50 %5" PRIu64 " ns   p95 %5" PRIu64 " ns   p99 %6" PRIu64
		" ns   max %8" PRIu64 " ns   MEAN %6" PRIu64 " ns\n",
		samples[REPS / 2], samples[REPS * 95 / 100],
		samples[REPS * 99 / 100], samples[REPS - 1], total / REPS);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("== vkCmdPipelineBarrier2 cost ==\n");

	struct bench b;
	memset(&b, 0, sizeof(b));
	b.drm_fd = -1;
	for (int i = 128; i < 136; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		b.drm_fd = open(path, O_RDWR | O_CLOEXEC);
		if (b.drm_fd >= 0) {
			break;
		}
	}
	if (b.drm_fd < 0) {
		SKIP("no DRM render node");
	}
	b.inst = avk_instance_create("avk-barrier-cost");
	if (b.inst == NULL) {
		SKIP("no Vulkan instance");
	}
	b.dev = avk_device_create(b.inst, b.drm_fd);
	if (b.dev == NULL) {
		SKIP("no Vulkan device on this node");
	}
	if (!avk_dmabuf_importer_init(&b.importer, b.dev)) {
		SKIP("no dma-buf importer");
	}
	b.gbm = gbm_create_device(b.drm_fd);
	if (b.gbm == NULL) {
		SKIP("no GBM device");
	}
	avk_cmd_ring_init(&b.ring, b.dev, "barrier-bench");

	/* ── the images ─────────────────────────────────────────────────────── */
	uint64_t mod_plain = 0, mod_dcc = 0;
	struct avk_image *owned = make_owned(b.dev, 3840, 2160);
	struct avk_image *plain = make_dmabuf(&b, 3840, 2160, false, &mod_plain);
	struct avk_image *dcc = make_dmabuf(&b, 3840, 2160, true, &mod_dcc);
	struct avk_image *small = make_owned(b.dev, 256, 256);

	/* What this device actually offers, so "no DCC modifier" is a fact about
	 * the table rather than about the probe. */
	{
		const struct avk_format_caps *caps =
			avk_format_table_find(&b.importer.table, DRM_FORMAT_XRGB8888);
		printf("\n-- XRGB8888 renderable modifiers --\n");
		if (caps != NULL) {
			for (size_t i = 0; i < caps->render_mod_count; i++) {
				uint64_t m = caps->render_mods[i].modifier;
				printf("  ---- 0x%016" PRIx64 "  planes %u  %s\n", m,
					caps->render_mods[i].plane_count,
					((m >> 13) & 1) ? "DCC" : "");
			}
			printf("  ---- (%u renderable, %u texture)\n",
				caps->render_mod_count, caps->texture_mod_count);
		}
	}

	printf("\n-- images --\n");
	CHECK(owned != NULL, "an owned OPTIMAL image");
	CHECK(small != NULL, "a small owned image");
	printf("  ---- dma-buf non-DCC modifier 0x%016" PRIx64 " %s\n",
		mod_plain, plain ? "" : "(UNAVAILABLE)");
	printf("  ---- dma-buf DCC     modifier 0x%016" PRIx64 " %s\n",
		mod_dcc, dcc ? "" : "(UNAVAILABLE)");

	if (owned == NULL || small == NULL) {
		SKIP("could not build the baseline images");
	}

	/* ── 1. barriers per call ───────────────────────────────────────────── */
	printf("\n-- how the cost scales --\n");
	struct avk_image *one[1] = { owned };
	struct avk_image *four[4] = { owned, owned, owned, owned };

	double c1 = time_barriers(&b, one, 1, 1,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		false);
	double c4_one_call = time_barriers(&b, four, 4, 1,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		false);
	double c4_four_calls = time_barriers(&b, four, 4, 4,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		false);

	printf("  ---- 1 barrier,  1 call:  %8.0f ns/call\n", c1);
	printf("  ---- 4 barriers, 1 call:  %8.0f ns/call  (%.0f ns/barrier)\n",
		c4_one_call, c4_one_call / 4.0);
	printf("  ---- 4 barriers, 4 calls: %8.0f ns/call  (%.0f ns total)\n",
		c4_four_calls, c4_four_calls * 4.0);
	/*
	 * THE BATCHING QUESTION (M4F.4). If four barriers in one call cost far less
	 * than four calls of one, batching is worth doing; if a call is nearly free
	 * and the barriers dominate, it is not. Reported either way rather than
	 * assumed.
	 */
	CHECK(c1 > 0 && c4_one_call > 0 && c4_four_calls > 0,
		"all three configurations measured");

	/* ── 2. queue-family ownership transfer ─────────────────────────────── */
	printf("\n-- foreign queue-family transfer --\n");
	double plain_local = -1, plain_foreign = -1;
	if (plain != NULL) {
		struct avk_image *p1[1] = { plain };
		plain_local = time_barriers(&b, p1, 1, 1, VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
		plain_foreign = time_barriers(&b, p1, 1, 1, VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
		printf("  ---- dma-buf, IGNORED queue families: %8.0f ns\n",
			plain_local);
		printf("  ---- dma-buf, FOREIGN -> graphics:    %8.0f ns\n",
			plain_foreign);
	} else {
		printf("  ---- no non-DCC renderable modifier on this device\n");
	}

	/* ── 3. THE DCC HYPOTHESIS ──────────────────────────────────────────── */
	printf("\n-- DCC --\n");
	if (plain == NULL || dcc == NULL) {
		printf("  ---- cannot isolate: this device does not advertise both a "
			"DCC and a non-DCC single-plane renderable modifier\n");
		printf("  ---- DCC HYPOTHESIS: UNRESOLVED (no falsifier available)\n");
	} else if (mod_plain == mod_dcc) {
		printf("  ---- the two selections returned the SAME modifier; the bit "
			"probe is wrong and this comparison would be against itself\n");
		printf("  ---- DCC HYPOTHESIS: UNRESOLVED (no falsifier available)\n");
	} else {
		struct avk_image *p1[1] = { plain };
		struct avk_image *d1[1] = { dcc };
		double p = time_barriers(&b, p1, 1, 1, VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
		double d = time_barriers(&b, d1, 1, 1, VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
		printf("  ---- non-DCC dma-buf, FOREIGN: %8.0f ns\n", p);
		printf("  ---- DCC     dma-buf, FOREIGN: %8.0f ns\n", d);
		printf("  ---- ratio %.2fx\n", p > 0 ? d / p : 0.0);
		/*
		 * The live/headless gap was ~23x. A hypothesis that explains it must
		 * produce something of that order, not a few percent. Stated as a
		 * threshold BEFORE the number is known, so the conclusion is not
		 * fitted to the data afterwards.
		 */
		CHECK(p > 0 && d > 0, "both modifiers measured");
	}

	/* ── 3b. the instrument itself ──────────────────────────────────────── */
	printf("\n-- can a per-call mean measure this at all? --\n");
	time_each_call(&b, owned);

	/* ── 4. size ────────────────────────────────────────────────────────── */
	printf("\n-- image size --\n");
	struct avk_image *s1[1] = { small };
	double big = time_barriers(&b, one, 1, 1, VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
	double sml = time_barriers(&b, s1, 1, 1, VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
	printf("  ---- 3840x2160: %8.0f ns\n", big);
	printf("  ---- 256x256:   %8.0f ns\n", sml);

	/* ── 5. layout pairs ────────────────────────────────────────────────── */
	printf("\n-- layout pairs --\n");
	struct { VkImageLayout a, b; const char *name; } pairs[] = {
		{ VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			"GENERAL -> GENERAL" },
		{ VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			"GENERAL -> SHADER_READ_ONLY" },
		{ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			"COLOR_ATTACHMENT -> SHADER_READ_ONLY" },
		{ VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			"UNDEFINED -> COLOR_ATTACHMENT" },
	};
	for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
		double t = time_barriers(&b, one, 1, 1, pairs[i].a, pairs[i].b, false);
		printf("  ---- %-38s %8.0f ns\n", pairs[i].name, t);
	}

	avk_device_wait_idle(b.dev);
	avk_cmd_ring_finish(&b.ring);
	if (owned) avk_image_destroy(b.dev, owned);
	if (small) avk_image_destroy(b.dev, small);
	if (plain) avk_image_destroy(b.dev, plain);
	if (dcc) avk_image_destroy(b.dev, dcc);
	avk_dmabuf_importer_finish(&b.importer);
	gbm_device_destroy(b.gbm);
	avk_device_destroy(b.dev);
	avk_instance_destroy(b.inst);
	close(b.drm_fd);

	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
