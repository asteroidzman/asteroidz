/*
 * M2 acceptance test: format/modifier capability and DMA-BUF import.
 *
 * The important test in this file is `test_implicit_modifier`. It allocates a
 * real GBM buffer, then deliberately throws away the modifier before handing
 * it to the importer -- which is exactly what a client using wl_drm, or an
 * Electron/Chromium build negotiating without modifiers, does. On the fx_vk
 * path that buffer produces `Failed to create texture` and a blank window.
 *
 * It asserts on PIXELS, not on the import returning non-NULL: the buffer is
 * filled with a coordinate-derived pattern first and the imported image is
 * read back and compared. An import that guessed LINEAR succeeds and returns
 * noise; only the readback tells them apart.
 *
 * And it asserts the negative: with GBM removed, the same import must fail.
 * Otherwise the test would pass on a build that guessed at the tiling anyway.
 *
 * Exits 77 (skip) with no GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <poll.h>
#include <time.h>

#include "render/vulkan/dmabuf/avk_dmabuf.h"
#include "render/vulkan/image/avk_upload.h"
#include "render/vulkan/sync/avk_sync.h"

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
		printf("SKIP: " __VA_ARGS__); \
		printf("\n"); \
		return 77; \
	} while (0)

#define WIDTH  256
#define HEIGHT 128

/* Turn a gbm_bo into the attributes a Wayland client would have sent. */
static bool bo_to_attribs(struct gbm_bo *bo, uint32_t format,
		struct avk_dmabuf_attributes *out) {
	memset(out, 0, sizeof(*out));
	out->width = WIDTH;
	out->height = HEIGHT;
	out->format = format;
	out->modifier = gbm_bo_get_modifier(bo);
	out->n_planes = gbm_bo_get_plane_count(bo);

	for (int i = 0; i < out->n_planes; i++) {
		out->fd[i] = gbm_bo_get_fd_for_plane(bo, i);
		if (out->fd[i] < 0) {
			for (int j = 0; j < i; j++) {
				close(out->fd[j]);
			}
			return false;
		}
		out->offset[i] = gbm_bo_get_offset(bo, i);
		out->stride[i] = gbm_bo_get_stride_for_plane(bo, i);
	}
	return true;
}

static void close_attribs(struct avk_dmabuf_attributes *a) {
	for (int i = 0; i < a->n_planes; i++) {
		if (a->fd[i] >= 0) {
			close(a->fd[i]);
		}
	}
}

/* ── a known pattern, written in and read back ──────────────────────────────
 *
 * "It imported" is not the assertion that matters. A copy path that computes
 * its row pitch in bytes where Vulkan wants pixels imports perfectly and
 * produces a sheared image; an import that guessed LINEAR for a tiled buffer
 * imports perfectly and produces noise. Both are worse than a blank window.
 *
 * So the buffer gets a pattern whose every pixel is a function of its
 * coordinates, and the imported image is read back and compared. Only that
 * catches a wrong stride, a wrong tiling and a wrong channel order.
 */
static uint32_t pattern_pixel(uint32_t x, uint32_t y) {
	/* Deliberately varies fast in x and slow in y: a stride error shifts rows
	 * against each other, which a flat or low-frequency pattern hides. */
	return 0xFF000000u | ((x * 7u + y * 13u) & 0xFFu) << 16
		| ((x * 3u) & 0xFFu) << 8 | ((y * 5u) & 0xFFu);
}

static bool write_pattern(struct gbm_bo *bo) {
	uint32_t stride = 0;
	void *map_data = NULL;
	void *pixels = gbm_bo_map(bo, 0, 0, WIDTH, HEIGHT, GBM_BO_TRANSFER_WRITE,
		&stride, &map_data);
	if (pixels == NULL) {
		return false;
	}
	for (uint32_t y = 0; y < HEIGHT; y++) {
		uint32_t *row = (uint32_t *)((char *)pixels + (size_t)y * stride);
		for (uint32_t x = 0; x < WIDTH; x++) {
			row[x] = pattern_pixel(x, y);
		}
	}
	gbm_bo_unmap(bo, map_data);
	return true;
}

/* Copy the imported VkImage back to host memory and compare it to the pattern.
 * Returns the number of mismatching pixels, or -1 if the readback itself
 * failed. */
static long readback_mismatches(struct avk_device *dev,
		struct avk_image *image) {
	VkDeviceSize size = (VkDeviceSize)WIDTH * HEIGHT * 4;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	long mismatches = -1;

	VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	if (vkCreateBuffer(dev->dev, &buffer_info, NULL, &buffer) != VK_SUCCESS) {
		return -1;
	}

	VkMemoryRequirements reqs;
	vkGetBufferMemoryRequirements(dev->dev, buffer, &reqs);
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(dev->phys, &props);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((reqs.memoryTypeBits & (1u << i))
				&& (props.memoryTypes[i].propertyFlags
					& (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
						| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
					== (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
						| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) {
		goto out;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (vkAllocateMemory(dev->dev, &alloc, NULL, &memory) != VK_SUCCESS
			|| vkBindBufferMemory(dev->dev, buffer, memory, 0) != VK_SUCCESS) {
		goto out;
	}

	struct avk_cmd_ring ring;
	if (!avk_cmd_ring_init(&ring, dev, "readback", AVK_FRAMES_IN_FLIGHT)) {
		goto out;
	}
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);
	if (cb == VK_NULL_HANDLE) {
		avk_cmd_ring_finish(&ring);
		goto out;
	}

	/* From whatever layout the image is actually in -- which the image itself
	 * records, so the barrier does not have to assume. */
	VkImageMemoryBarrier2 barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
		.oldLayout = image->layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image->image,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &barrier,
	};
	vkCmdPipelineBarrier2(cb, &dep);

	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferRowLength = WIDTH,
		.bufferImageHeight = HEIGHT,
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.imageExtent = { WIDTH, HEIGHT, 1 },
	};
	VkCopyImageToBufferInfo2 copy = {
		.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
		.srcImage = image->image,
		.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.dstBuffer = buffer,
		.regionCount = 1,
		.pRegions = &region,
	};
	vkCmdCopyImageToBuffer2(cb, &copy);

	uint64_t value = avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0);
	if (value == 0 || !avk_device_timeline_wait(dev, value, 2000000000ULL)) {
		avk_cmd_ring_finish(&ring);
		goto out;
	}
	avk_cmd_ring_finish(&ring);

	void *mapped = NULL;
	if (vkMapMemory(dev->dev, memory, 0, size, 0, &mapped) != VK_SUCCESS) {
		goto out;
	}
	mismatches = 0;
	const uint32_t *got = mapped;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			uint32_t want = pattern_pixel(x, y);
			/* Ignore alpha: XRGB has none and the import is entitled to
			 * whatever is in those bits. */
			if ((got[y * WIDTH + x] & 0x00FFFFFFu) != (want & 0x00FFFFFFu)) {
				mismatches++;
			}
		}
	}
	vkUnmapMemory(dev->dev, memory);

out:
	if (buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(dev->dev, buffer, NULL);
	}
	if (memory != VK_NULL_HANDLE) {
		vkFreeMemory(dev->dev, memory, NULL);
	}
	return mismatches;
}

/* Allocate with a real, tiled, single-plane modifier taken from the device's
 * own capability table -- gbm_bo_create() without modifiers produces a buffer
 * that reports MOD_INVALID on Mesa, which would make the "explicit" test a
 * test of the implicit path by accident. */
static struct gbm_bo *alloc_tiled(struct avk_dmabuf_importer *importer,
		struct gbm_device *gbm, uint32_t format) {
	const struct avk_format_caps *caps =
		avk_format_table_find(&importer->table, format);
	if (caps == NULL) {
		return NULL;
	}
	uint64_t mods[32];
	uint32_t count = 0;
	for (uint32_t i = 0; i < caps->texture_mod_count && count < 32; i++) {
		const struct avk_modifier_caps *m = &caps->texture_mods[i];
		if (m->modifier != DRM_FORMAT_MOD_LINEAR && m->plane_count == 1) {
			mods[count++] = m->modifier;
		}
	}
	if (count == 0) {
		return NULL;
	}
	return gbm_bo_create_with_modifiers2(gbm, WIDTH, HEIGHT, format, mods,
		count, GBM_BO_USE_RENDERING);
}

static void test_format_table(struct avk_dmabuf_importer *importer) {
	printf("format/modifier capability table\n");

	const struct avk_format_caps *argb =
		avk_format_table_find(&importer->table, DRM_FORMAT_ARGB8888);
	const struct avk_format_caps *xrgb =
		avk_format_table_find(&importer->table, DRM_FORMAT_XRGB8888);

	CHECK(argb != NULL && argb->texture_mod_count > 0,
		"ARGB8888 has %u importable texture modifier(s)",
		argb ? argb->texture_mod_count : 0);
	CHECK(xrgb != NULL && xrgb->render_mod_count > 0,
		"XRGB8888 has %u renderable modifier(s)",
		xrgb ? xrgb->render_mod_count : 0);

	/*
	 * The premise of the entire implicit-modifier ladder: the driver never
	 * enumerates DRM_FORMAT_MOD_INVALID, so an exact-match lookup for it
	 * cannot succeed. If this ever failed, the ladder would be unnecessary --
	 * and, more importantly, a lookup that DID match INVALID would be a lookup
	 * that had been made to lie, which is the corruption bug this design
	 * exists to avoid.
	 */
	bool invalid_absent = true;
	for (uint32_t i = 0; i < importer->table.count; i++) {
		const struct avk_format_caps *caps = &importer->table.formats[i];
		if (avk_format_caps_find_modifier(caps, DRM_FORMAT_MOD_INVALID, false)
				|| avk_format_caps_find_modifier(caps,
					DRM_FORMAT_MOD_INVALID, true)) {
			invalid_absent = false;
			break;
		}
	}
	CHECK(invalid_absent,
		"MOD_INVALID appears nowhere in the table (this is why rung 1 exists)");

	/* An extent of 0 would mean the probe never ran and the struct is just
	 * zeroed -- a table full of plausible-looking nothing. */
	bool extents_probed = true;
	for (uint32_t i = 0; i < importer->table.count && extents_probed; i++) {
		const struct avk_format_caps *caps = &importer->table.formats[i];
		for (uint32_t j = 0; j < caps->texture_mod_count; j++) {
			if (caps->texture_mods[j].max_extent.width == 0) {
				extents_probed = false;
				break;
			}
		}
	}
	CHECK(extents_probed, "every modifier carries a probed max extent");

	CHECK(avk_format_table_find(&importer->table, 0xDEADBEEF) == NULL,
		"an unknown fourcc is not in the table");
}

/*
 * THE ADVERTISEMENT PROBE (M3.6).
 *
 * M3.6 made avk_format_table the source of the format/modifier pairs the
 * compositor advertises to clients. That is a PROMISE: a conforming client
 * allocating one of those pairs must get a buffer AVK can import. The probe
 * keeps the promise honest by taking pairs straight out of the advertised set
 * and allocating real buffers with them.
 *
 * "the table says it is importable" is not the claim being tested -- the table
 * already says that, and it is what the advertisement is derived from, so
 * asserting it here would be a tautology. What is tested is that GBM can
 * ALLOCATE the pair and AVK can then import the result: the whole round trip a
 * client performs, for pairs the compositor asked clients to use.
 *
 * Representative rather than exhaustive: the common compositor formats, every
 * modifier each of them advertises. A driver whose table lists a modifier GBM
 * refuses to allocate is a real finding and is reported, not skipped silently.
 */
static void test_advertised_pairs_import(struct avk_dmabuf_importer *importer,
		struct gbm_device *gbm) {
	printf("every advertised pair can be allocated and imported\n");

	const uint32_t interesting[] = {
		DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB2101010,
	};
	uint32_t tried = 0, ok = 0, unallocatable = 0, failed_import = 0;

	for (size_t f = 0; f < sizeof(interesting) / sizeof(interesting[0]); f++) {
		const struct avk_format_caps *caps =
			avk_format_table_find(&importer->table, interesting[f]);
		if (caps == NULL) {
			continue;
		}
		char fname[64];
		avk_drm_format_name(interesting[f], fname, sizeof(fname));
		for (uint32_t m = 0; m < caps->texture_mod_count; m++) {
			uint64_t mod = caps->texture_mods[m].modifier;
			tried++;

			struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gbm, WIDTH,
				HEIGHT, interesting[f], &mod, 1, GBM_BO_USE_RENDERING);
			if (bo == NULL) {
				/* The pair is advertised and the allocator will not produce
				 * it. Not fatal -- a client would simply pick another -- but
				 * it means the advertisement is wider than reality, so it is
				 * counted and named rather than passed over. */
				char mname[80];
				avk_drm_modifier_name(mod, mname, sizeof(mname));
				printf("  note: %s %s is advertised but GBM would not "
					"allocate it\n", fname, mname);
				unallocatable++;
				continue;
			}

			struct avk_dmabuf_attributes attribs;
			if (!bo_to_attribs(bo, interesting[f], &attribs)) {
				gbm_bo_destroy(bo);
				failed_import++;
				continue;
			}
			struct avk_image *image =
				avk_dmabuf_import(importer, &attribs, false);
			if (image != NULL) {
				/* Zero-copy, not the compatibility ladder: an advertised
				 * explicit modifier that lands on the copy rung would be a
				 * promise kept by accident. */
				if (image->origin == AVK_IMAGE_DMABUF_EXPLICIT) {
					ok++;
				} else {
					failed_import++;
				}
				avk_image_destroy(importer->dev, image);
			} else {
				failed_import++;
			}
			close_attribs(&attribs);
			gbm_bo_destroy(bo);
		}
	}

	printf("  %" PRIu32 " advertised pairs tried: %" PRIu32 " imported "
		"zero-copy, %" PRIu32 " not allocatable by GBM\n",
		tried, ok, unallocatable);
	CHECK(tried > 0, "there were advertised pairs to probe");
	CHECK(failed_import == 0,
		"every pair GBM allocated imported zero-copy (%" PRIu32 " did not)",
		failed_import);
}

static void test_explicit_modifier(struct avk_dmabuf_importer *importer,
		struct gbm_device *gbm) {
	printf("import with an explicit modifier (the ordinary path)\n");

	struct gbm_bo *bo = alloc_tiled(importer, gbm, DRM_FORMAT_ARGB8888);
	if (bo == NULL) {
		printf("  (no tiled single-plane modifier available -- skipped)\n");
		return;
	}

	struct avk_dmabuf_attributes attribs;
	if (!bo_to_attribs(bo, DRM_FORMAT_ARGB8888, &attribs)) {
		CHECK(false, "export the buffer as dma-buf fds");
		gbm_bo_destroy(bo);
		return;
	}

	char mod_name[80];
	avk_drm_modifier_name(attribs.modifier, mod_name, sizeof(mod_name));
	printf("  buffer is %s, %d plane(s), stride %" PRIu32 "\n", mod_name,
		attribs.n_planes, attribs.stride[0]);
	CHECK(attribs.modifier != DRM_FORMAT_MOD_INVALID
			&& attribs.modifier != DRM_FORMAT_MOD_LINEAR,
		"the test buffer really is tiled (otherwise this proves nothing)");

	struct avk_image *image = avk_dmabuf_import(importer, &attribs, false);
	CHECK(image != NULL, "imported");
	if (image != NULL) {
		CHECK(image->origin == AVK_IMAGE_DMABUF_EXPLICIT,
			"zero-copy via the stated modifier");
		CHECK(image->modifier == attribs.modifier,
			"the image kept the buffer's modifier");
		CHECK(image->extent.width == WIDTH && image->extent.height == HEIGHT,
			"extent is %ux%u", image->extent.width, image->extent.height);
		CHECK(image->has_alpha, "ARGB8888 is imported as having alpha");
		avk_image_destroy(importer->dev, image);
	}

	close_attribs(&attribs);
	gbm_bo_destroy(bo);
}

static void test_implicit_modifier(struct avk_dmabuf_importer *importer,
		struct gbm_device *gbm) {
	printf("import with an IMPLICIT modifier (the blank-window case)\n");
	printf("  this driver %s recover implicit modifiers\n",
		importer->gbm_recovers_modifiers ? "DOES" : "does NOT");

	struct gbm_bo *bo = alloc_tiled(importer, gbm, DRM_FORMAT_XRGB8888);
	if (bo == NULL) {
		printf("  (no tiled single-plane modifier available -- skipped)\n");
		return;
	}

	CHECK(write_pattern(bo), "wrote a coordinate-derived pattern into the "
		"buffer");

	struct avk_dmabuf_attributes attribs;
	if (!bo_to_attribs(bo, DRM_FORMAT_XRGB8888, &attribs)) {
		CHECK(false, "export the buffer as dma-buf fds");
		gbm_bo_destroy(bo);
		return;
	}

	uint64_t real_modifier = attribs.modifier;
	CHECK(real_modifier != DRM_FORMAT_MOD_INVALID
			&& real_modifier != DRM_FORMAT_MOD_LINEAR,
		"the buffer is genuinely tiled (0x%016" PRIX64 ")", real_modifier);

	/* Here is the client that did not tell us its tiling. */
	attribs.modifier = DRM_FORMAT_MOD_INVALID;

	struct avk_image *image = avk_dmabuf_import(importer, &attribs, false);
	CHECK(image != NULL,
		"a buffer with no stated modifier still imports -- no blank window");
	if (image != NULL) {
		CHECK(image->origin == AVK_IMAGE_DMABUF_RECOVERED
				|| image->origin == AVK_IMAGE_DMABUF_COPIED,
			"took the recovery or copy path (origin %d), not a guess",
			(int)image->origin);
		CHECK(image->modifier != DRM_FORMAT_MOD_LINEAR,
			"it was NOT silently treated as LINEAR");
		CHECK(!image->has_alpha, "XRGB8888 is imported as opaque");

		/*
		 * The assertion the whole rung exists for. A tiled buffer read as
		 * linear, or copied with a byte-vs-pixel row pitch, imports perfectly
		 * and contains garbage. Only the pixels can tell the difference.
		 */
		long mismatches = readback_mismatches(importer->dev, image);
		CHECK(mismatches == 0,
			"every one of %d pixels survived the import (%ld wrong)",
			WIDTH * HEIGHT, mismatches);

		avk_image_destroy(importer->dev, image);
	}

	/*
	 * The negative half, and the reason the positive half means anything.
	 *
	 * Take GBM away and re-run the identical import. It must fail: every rung
	 * below the explicit one goes through GBM, so without it there is no
	 * honest way to read this buffer. If this passed, some path would be
	 * importing an unknown tiling anyway -- which is the corruption bug.
	 */
	printf("  (the next ERROR block is expected -- negative test)\n");
	struct gbm_device *saved = importer->gbm;
	importer->gbm = NULL;
	struct avk_image *without_gbm =
		avk_dmabuf_import(importer, &attribs, false);
	importer->gbm = saved;
	CHECK(without_gbm == NULL,
		"with GBM removed the same buffer FAILS rather than being guessed at");
	if (without_gbm != NULL) {
		avk_image_destroy(importer->dev, without_gbm);
	}

	close_attribs(&attribs);
	gbm_bo_destroy(bo);
}

static void test_rejections(struct avk_dmabuf_importer *importer,
		struct gbm_device *gbm) {
	printf("malformed buffers are refused, not misread\n");

	struct gbm_bo *bo = alloc_tiled(importer, gbm, DRM_FORMAT_ARGB8888);
	if (bo == NULL) {
		printf("  (no tiled modifier available -- skipped)\n");
		return;
	}
	struct avk_dmabuf_attributes base;
	if (!bo_to_attribs(bo, DRM_FORMAT_ARGB8888, &base)) {
		CHECK(false, "export");
		gbm_bo_destroy(bo);
		return;
	}

	printf("  (ERROR lines below are expected -- negative tests)\n");

	/* A format avk has no mapping for. */
	struct avk_dmabuf_attributes bad = base;
	bad.format = 0xDEADBEEF;
	CHECK(avk_dmabuf_import(importer, &bad, false) == NULL,
		"an unknown fourcc is refused");

	/* A plane count that disagrees with the modifier. */
	bad = base;
	bad.n_planes = 3;
	CHECK(avk_dmabuf_import(importer, &bad, false) == NULL,
		"a plane count the modifier does not have is refused");

	/* Larger than the device can import. */
	bad = base;
	bad.width = 1 << 20;
	bad.height = 1 << 20;
	CHECK(avk_dmabuf_import(importer, &bad, false) == NULL,
		"an impossibly large buffer is refused");

	/* Zero planes. */
	bad = base;
	bad.n_planes = 0;
	CHECK(avk_dmabuf_import(importer, &bad, false) == NULL,
		"a zero-plane buffer is refused");

	close_attribs(&base);
	gbm_bo_destroy(bo);
}

/*
 * The other way a finished frame carries its fence.
 *
 * When the backend has no drm_syncobj timeline -- the headless backend has no
 * DRM device at all, and an older kernel has no timeline capability -- AVK puts
 * the fence on the target's own dma-buf instead, where any implicitly
 * synchronised consumer will find it. This is the path every headless test run
 * actually takes, so it is worth its own assertions rather than being the
 * branch nobody looks at.
 *
 * Getting a real premise out of this took a correction. The obvious one --
 * "a fresh buffer carries no fences" -- was written first and MEASURED FALSE:
 * a buffer straight out of gbm_bo_create_with_modifiers2() already has one,
 * because the driver initialises the modifier's compression metadata with a
 * GPU write. So "the buffer has a fence afterwards" proves nothing at all; it
 * was already true beforehand, and a version of avk_sync_dmabuf_attach() that
 * returned true and did nothing would have passed.
 *
 * What is used instead is a fence that is still PENDING when it is looked at:
 * the buffer's own fence is waited to completion first (asserted), so a
 * pending fence found on it afterwards can only have arrived through the
 * attach. Then the work is waited out and the same fence is observed to
 * signal.
 *
 * Making the fence reliably pending took a second correction. The obvious way
 * -- submit behind a timeline wait the test holds shut, then open it -- DEADLOCKS
 * on RADV: amdgpu has no wait-before-signal, Mesa defers such a submission in
 * userspace, and vkGetSemaphoreFdKHR then blocks waiting for a submission that
 * cannot be dispatched until the test opens the gate it is blocked before
 * opening. So the work is made genuinely long instead: repeated large fills,
 * serialised with barriers so the driver cannot collapse them. The margin is
 * measured and printed rather than assumed, because "long enough" is a
 * property of the machine and not of the source.
 */
#define FILL_BYTES (256u << 20)
#define FILL_REPEATS 48
static bool fence_signalled(int sync_file_fd) {
	struct pollfd pfd = { .fd = sync_file_fd, .events = POLLIN };
	return poll(&pfd, 1, 0) > 0;
}

static bool fence_wait(int sync_file_fd, int timeout_ms) {
	struct pollfd pfd = { .fd = sync_file_fd, .events = POLLIN };
	return poll(&pfd, 1, timeout_ms) > 0;
}

static void test_dmabuf_fences(struct avk_dmabuf_importer *importer,
		struct gbm_device *gbm) {
	printf("fences on the buffer itself (the no-timeline path)\n");

	struct gbm_bo *bo = alloc_tiled(importer, gbm, DRM_FORMAT_ARGB8888);
	if (bo == NULL) {
		printf("  (no tiled modifier available -- skipped)\n");
		return;
	}
	struct avk_dmabuf_attributes attribs;
	if (!bo_to_attribs(bo, DRM_FORMAT_ARGB8888, &attribs)) {
		CHECK(false, "export the buffer as dma-buf fds");
		gbm_bo_destroy(bo);
		return;
	}

	/* Premise: whatever the allocator left on the buffer has finished, so any
	 * pending fence seen later is one this test put there. */
	int idle = -1;
	if (!avk_sync_dmabuf_fences(attribs.fd[0], DMA_BUF_SYNC_WRITE, &idle)) {
		printf("  (kernel does not support dma-buf sync_file ioctls -- "
			"skipped)\n");
		close_attribs(&attribs);
		gbm_bo_destroy(bo);
		return;
	}
	if (idle >= 0) {
		CHECK(fence_wait(idle, 2000),
			"the buffer starts idle (allocation fence has signalled)");
		close(idle);
	} else {
		CHECK(true, "the buffer starts idle (no fences at all)");
	}

	struct avk_sync sync;
	struct avk_cmd_ring ring;
	if (!avk_sync_init(&sync, importer->dev, "dmabuf-test")) {
		CHECK(false, "the fence bridge initialises");
		close_attribs(&attribs);
		gbm_bo_destroy(bo);
		return;
	}
	if (!avk_cmd_ring_init(&ring, importer->dev, "dmabuf-sync-test", AVK_FRAMES_IN_FLIGHT)) {
		CHECK(false, "ring initialises");
		avk_sync_finish(&sync);
		close_attribs(&attribs);
		gbm_bo_destroy(bo);
		return;
	}

	/* Something for the GPU to be busy with for long enough that its completion
	 * fence is unambiguously pending while the CPU looks at it. */
	VkBuffer scratch = VK_NULL_HANDLE;
	VkDeviceMemory scratch_mem = VK_NULL_HANDLE;
	VkBufferCreateInfo buf_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = FILL_BYTES,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	bool have_scratch = vkCreateBuffer(importer->dev->dev, &buf_info, NULL,
		&scratch) == VK_SUCCESS;
	if (have_scratch) {
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(importer->dev->dev, scratch, &req);
		uint32_t type = 0;
		have_scratch = avk_find_memory_type(importer->dev, req.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type);
		if (have_scratch) {
			VkMemoryAllocateInfo alloc = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = req.size,
				.memoryTypeIndex = type,
			};
			have_scratch = vkAllocateMemory(importer->dev->dev, &alloc, NULL,
					&scratch_mem) == VK_SUCCESS
				&& vkBindBufferMemory(importer->dev->dev, scratch, scratch_mem,
					0) == VK_SUCCESS;
		}
	}
	if (!have_scratch) {
		CHECK(false, "a scratch buffer to keep the GPU busy");
		goto cleanup;
	}

	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);
	if (cb == VK_NULL_HANDLE) {
		CHECK(false, "a command buffer to record into");
		goto cleanup;
	}
	for (unsigned i = 0; i < FILL_REPEATS; i++) {
		vkCmdFillBuffer(cb, scratch, 0, FILL_BYTES, i);
		/* Serialised on purpose: without the barrier the driver is free to
		 * overlap the fills and the work stops being long. */
		VkMemoryBarrier2 mb = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
			.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		};
		VkDependencyInfo dep = {
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &mb,
		};
		vkCmdPipelineBarrier2(cb, &dep);
	}

	VkSemaphoreSubmitInfo signal = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = avk_sync_signal_semaphore(&sync),
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	};
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	uint64_t point = avk_cmd_ring_submit(&ring, NULL, 0, &signal, 1);
	CHECK(point != 0, "a long frame submits");

	int fence = -1;
	CHECK(avk_sync_export_sync_file(&sync, &fence) && fence >= 0,
		"its completion exports as a sync_file");
	if (fence >= 0) {
		/* The premise, asserted rather than hoped for. If this fails the GPU
		 * finished sooner than the CPU could look, and nothing below it means
		 * anything -- so it fails here, where the message says so, instead of
		 * quietly weakening the assertion that follows. */
		CHECK(!fence_signalled(fence),
			"the work is still running when the fence is looked at");
		CHECK(avk_sync_dmabuf_attach(attribs.fd[0], DMA_BUF_SYNC_WRITE, fence),
			"it attaches to the target's dma-buf");

		int attached = -1;
		CHECK(avk_sync_dmabuf_fences(attribs.fd[0], DMA_BUF_SYNC_WRITE,
				&attached) && attached >= 0,
			"the buffer's fences read back");
		if (attached >= 0) {
			/* The load-bearing one. The buffer was idle a moment ago and is
			 * now pending, which can only be the fence just attached. An
			 * attach that returned true and did nothing fails here. */
			CHECK(!fence_signalled(attached),
				"the buffer now makes a consumer wait for work not yet done");
			CHECK(fence_wait(attached, 5000),
				"and stops making it wait once the work completes");
			clock_gettime(CLOCK_MONOTONIC, &t1);
			printf("  note: the GPU stayed busy for %.1f ms (the margin this "
				"test's premise rests on)\n",
				(t1.tv_sec - t0.tv_sec) * 1e3
					+ (t1.tv_nsec - t0.tv_nsec) / 1e6);
			close(attached);
		}
	}

	CHECK(avk_device_timeline_wait(importer->dev, point, 5000000000ULL),
		"the work completes");

cleanup:
	avk_cmd_ring_finish(&ring);
	avk_sync_finish(&sync);
	if (scratch != VK_NULL_HANDLE) {
		vkDestroyBuffer(importer->dev->dev, scratch, NULL);
	}
	if (scratch_mem != VK_NULL_HANDLE) {
		vkFreeMemory(importer->dev->dev, scratch_mem, NULL);
	}
	close_attribs(&attribs);
	gbm_bo_destroy(bo);
}

int main(void) {
	printf("== avk dmabuf (M2) ==\n");

	int drm_fd = -1;
	for (int i = 128; i < 192 && drm_fd < 0; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
	}
	if (drm_fd < 0) {
		SKIP("no DRM render node available");
	}

	struct avk_instance *inst = avk_instance_create("test-avk-dmabuf");
	if (inst == NULL) {
		close(drm_fd);
		SKIP("no usable Vulkan instance");
	}

	struct avk_device *dev = avk_device_create(inst, drm_fd);
	if (dev == NULL) {
		avk_instance_destroy(inst);
		close(drm_fd);
		SKIP("no usable Vulkan device on this render node");
	}

	/* A separate GBM device for allocating the test buffers, so the test is
	 * not reaching into the importer's own. */
	struct gbm_device *gbm = gbm_create_device(drm_fd);
	if (gbm == NULL) {
		avk_device_destroy(dev);
		avk_instance_destroy(inst);
		close(drm_fd);
		SKIP("no GBM device");
	}

	struct avk_dmabuf_importer importer;
	if (!avk_dmabuf_importer_init(&importer, dev)) {
		gbm_device_destroy(gbm);
		avk_device_destroy(dev);
		avk_instance_destroy(inst);
		SKIP("importer would not initialise");
	}

	test_format_table(&importer);
	test_explicit_modifier(&importer, gbm);
	test_advertised_pairs_import(&importer, gbm);
	test_implicit_modifier(&importer, gbm);
	test_rejections(&importer, gbm);
	test_dmabuf_fences(&importer, gbm);

	avk_dmabuf_importer_log_stats(&importer);
	avk_device_wait_idle(dev);
	avk_dmabuf_importer_finish(&importer);
	gbm_device_destroy(gbm);
	avk_device_destroy(dev);
	avk_instance_destroy(inst);
	close(drm_fd);

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
