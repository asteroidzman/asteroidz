/*
 * asteroidz-avk-probe -- run the Vulkan engine on this machine and say what
 * happened.
 *
 * AVK composites the desktop unconditionally, but starting a session is a
 * much bigger thing to do than asking whether the engine works here at all.
 * This tool answers the smaller question without touching
 * the running compositor: it opens a render node, builds a device, imports
 * buffers the way a client's would be imported, composites a frame, and writes
 * it out as a PNG you can look at.
 *
 * It is deliberately read-only with respect to the desktop. No Wayland
 * connection, no DRM master, no modeset -- so running it while your session is
 * up is as safe as running any other GPU application.
 *
 * Exit status is 0 only if every stage succeeded, so it is also usable as a
 * post-install smoke test.
 */

#define _POSIX_C_SOURCE 200809L

#include <cairo.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "render/vulkan/device/avk_phys.h"
#include "render/vulkan/dmabuf/avk_dmabuf.h"
#include "render/vulkan/scene/avk_render.h"

#define OUT_W 512
#define OUT_H 512
#define TARGET_FORMAT VK_FORMAT_B8G8R8A8_UNORM

static int failures = 0;

static void ok(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	printf("  \033[32mok\033[0m   ");
	vprintf(fmt, args);
	printf("\n");
	va_end(args);
}

static void bad(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	printf("  \033[31mFAIL\033[0m ");
	vprintf(fmt, args);
	printf("\n");
	va_end(args);
	failures++;
}

/* ── small Vulkan helpers ───────────────────────────────────────────────── */

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

static struct avk_image *make_image(struct avk_device *dev, uint32_t w,
		uint32_t h, VkImageUsageFlags usage, bool has_alpha) {
	struct avk_image *image = calloc(1, sizeof(*image));
	if (image == NULL) {
		return NULL;
	}
	image->dev = dev;
	image->format = TARGET_FORMAT;
	image->extent = (VkExtent2D){ w, h };
	image->has_alpha = has_alpha;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	image->plane_count = 1;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = TARGET_FORMAT,
		.extent = { w, h, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
	};
	if (vkCreateImage(dev->dev, &info, NULL, &image->image) != VK_SUCCESS) {
		free(image);
		return NULL;
	}
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
	image->memory_count = 1;
	return image;
}

static bool staged_copy(struct avk_device *dev, VkBuffer buffer,
		struct avk_image *image, uint32_t w, uint32_t h, bool to_image) {
	struct avk_cmd_ring ring;
	if (!avk_cmd_ring_init(&ring, dev, "probe", AVK_FRAMES_IN_FLIGHT)) {
		return false;
	}
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);
	if (cb == VK_NULL_HANDLE) {
		avk_cmd_ring_finish(&ring);
		return false;
	}

	VkImageLayout want = to_image ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
		: VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	VkImageMemoryBarrier2 b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = to_image ? VK_ACCESS_2_TRANSFER_WRITE_BIT
			: VK_ACCESS_2_TRANSFER_READ_BIT,
		.oldLayout = image->layout,
		.newLayout = want,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image->image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &b,
	};
	vkCmdPipelineBarrier2(cb, &dep);
	image->layout = want;

	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferRowLength = w,
		.bufferImageHeight = h,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { w, h, 1 },
	};
	if (to_image) {
		VkCopyBufferToImageInfo2 copy = {
			.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
			.srcBuffer = buffer,
			.dstImage = image->image,
			.dstImageLayout = want,
			.regionCount = 1,
			.pRegions = &region,
		};
		vkCmdCopyBufferToImage2(cb, &copy);
	} else {
		VkCopyImageToBufferInfo2 copy = {
			.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
			.srcImage = image->image,
			.srcImageLayout = want,
			.dstBuffer = buffer,
			.regionCount = 1,
			.pRegions = &region,
		};
		vkCmdCopyImageToBuffer2(cb, &copy);
	}

	uint64_t value = avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0);
	bool done = value != 0
		&& avk_device_timeline_wait(dev, value, 5000000000ULL);
	avk_cmd_ring_finish(&ring);
	return done;
}

/* Host pixels <-> image, through a temporary staging buffer. Slow and simple:
 * this is a diagnostic tool, not the frame path. */
static bool transfer(struct avk_device *dev, struct avk_image *image,
		uint32_t *pixels, uint32_t w, uint32_t h, bool to_image) {
	VkDeviceSize size = (VkDeviceSize)w * h * 4;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	bool done = false;

	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = to_image ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
			: VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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
		goto out;
	}
	vkBindBufferMemory(dev->dev, buffer, memory, 0);

	void *mapped = NULL;
	if (to_image) {
		if (vkMapMemory(dev->dev, memory, 0, size, 0, &mapped) != VK_SUCCESS) {
			goto out;
		}
		memcpy(mapped, pixels, (size_t)size);
		vkUnmapMemory(dev->dev, memory);
	}

	done = staged_copy(dev, buffer, image, w, h, to_image);

	if (done && !to_image) {
		if (vkMapMemory(dev->dev, memory, 0, size, 0, &mapped) == VK_SUCCESS) {
			memcpy(pixels, mapped, (size_t)size);
			vkUnmapMemory(dev->dev, memory);
		} else {
			done = false;
		}
	}

out:
	if (buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(dev->dev, buffer, NULL);
	}
	if (memory != VK_NULL_HANDLE) {
		vkFreeMemory(dev->dev, memory, NULL);
	}
	return done;
}

/* ── stages ─────────────────────────────────────────────────────────────── */

static void report_devices(struct avk_instance *inst, int drm_fd) {
	struct avk_phys *list = NULL;
	uint32_t count = 0;
	printf("\n\033[1mGPUs\033[0m\n");
	if (!avk_phys_enumerate(inst, &list, &count)) {
		bad("could not enumerate physical devices");
		return;
	}
	int selected = avk_phys_select(list, count, drm_fd);
	avk_phys_log_all(list, count, selected);
	if (selected >= 0) {
		ok("selected by DRM node, not by name or index");
	} else {
		bad("no device claims this DRM node");
	}
	free(list);
}

static void report_import(struct avk_dmabuf_importer *importer,
		struct gbm_device *gbm) {
	printf("\n\033[1mDMA-BUF import\033[0m\n");
	printf("  %u importable format(s), %u texture and %u render "
		"format/modifier pairs\n", importer->table.count,
		importer->table.texture_pair_count,
		importer->table.render_pair_count);
	printf("  implicit-modifier recovery through GBM: %s\n",
		importer->gbm_recovers_modifiers ? "available"
			: "NOT available on this driver (the copy path covers it)");

	/* A tiled buffer, imported the ordinary way. */
	const struct avk_format_caps *caps =
		avk_format_table_find(&importer->table, DRM_FORMAT_XRGB8888);
	if (caps == NULL) {
		bad("this device cannot import XRGB8888 at all");
		return;
	}
	uint64_t mods[32];
	uint32_t nmods = 0;
	for (uint32_t i = 0; i < caps->texture_mod_count && nmods < 32; i++) {
		if (caps->texture_mods[i].modifier != DRM_FORMAT_MOD_LINEAR
				&& caps->texture_mods[i].plane_count == 1) {
			mods[nmods++] = caps->texture_mods[i].modifier;
		}
	}
	if (nmods == 0) {
		printf("  (no tiled single-plane modifier -- import check skipped)\n");
		return;
	}

	struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gbm, 256, 128,
		DRM_FORMAT_XRGB8888, mods, nmods, GBM_BO_USE_RENDERING);
	if (bo == NULL) {
		bad("could not allocate a tiled test buffer");
		return;
	}

	/* Fill it with a coordinate-derived pattern so the import can be checked
	 * on pixels rather than on a success code. */
	uint32_t map_stride = 0;
	void *map_data = NULL;
	void *px = gbm_bo_map(bo, 0, 0, 256, 128, GBM_BO_TRANSFER_WRITE,
		&map_stride, &map_data);
	if (px == NULL) {
		bad("could not map the test buffer");
		gbm_bo_destroy(bo);
		return;
	}
	for (uint32_t y = 0; y < 128; y++) {
		uint32_t *row = (uint32_t *)((char *)px + (size_t)y * map_stride);
		for (uint32_t x = 0; x < 256; x++) {
			row[x] = 0xFF000000u | ((x * 7u + y * 13u) & 0xFFu) << 16
				| ((x * 3u) & 0xFFu) << 8 | ((y * 5u) & 0xFFu);
		}
	}
	gbm_bo_unmap(bo, map_data);

	struct avk_dmabuf_attributes attribs = {
		.width = 256, .height = 128,
		.format = DRM_FORMAT_XRGB8888,
		.modifier = gbm_bo_get_modifier(bo),
		.n_planes = gbm_bo_get_plane_count(bo),
	};
	for (int i = 0; i < attribs.n_planes; i++) {
		attribs.fd[i] = gbm_bo_get_fd_for_plane(bo, i);
		attribs.offset[i] = gbm_bo_get_offset(bo, i);
		attribs.stride[i] = gbm_bo_get_stride_for_plane(bo, i);
	}

	char mod_name[80];
	avk_drm_modifier_name(attribs.modifier, mod_name, sizeof(mod_name));
	printf("  test buffer: 256x128 XR24, %s\n", mod_name);

	struct avk_image *image = avk_dmabuf_import(importer, &attribs, false);
	if (image != NULL && image->origin == AVK_IMAGE_DMABUF_EXPLICIT) {
		ok("explicit modifier: imported zero-copy");
	} else {
		bad("explicit modifier: import failed");
	}
	if (image != NULL) {
		avk_image_destroy(importer->dev, image);
	}

	/*
	 * The same buffer with its modifier thrown away -- what a wl_drm client,
	 * or an Electron build that negotiated without modifiers, actually hands
	 * over. This is the case that renders blank on the fx_vk path.
	 */
	uint64_t real = attribs.modifier;
	attribs.modifier = DRM_FORMAT_MOD_INVALID;
	image = avk_dmabuf_import(importer, &attribs, false);
	if (image == NULL) {
		bad("implicit modifier: import FAILED -- this is the blank-window "
			"case and it is not covered on this machine");
	} else {
		const char *how = image->origin == AVK_IMAGE_DMABUF_RECOVERED
			? "modifier recovered, zero-copy"
			: "copied through a driver-detiled mapping";
		ok("implicit modifier: imported (%s)", how);
		if (image->modifier == DRM_FORMAT_MOD_LINEAR
				&& real != DRM_FORMAT_MOD_LINEAR) {
			bad("...but it was treated as LINEAR, which would be corrupt");
		}

		/* Verify the pixels, because "imported" is not "imported correctly". */
		uint32_t *got = calloc(256 * 128, 4);
		if (got != NULL
				&& transfer(importer->dev, image, got, 256, 128, false)) {
			long wrong = 0;
			for (uint32_t y = 0; y < 128; y++) {
				for (uint32_t x = 0; x < 256; x++) {
					uint32_t want = 0xFF000000u
						| ((x * 7u + y * 13u) & 0xFFu) << 16
						| ((x * 3u) & 0xFFu) << 8 | ((y * 5u) & 0xFFu);
					if ((got[y * 256 + x] & 0x00FFFFFFu)
							!= (want & 0x00FFFFFFu)) {
						wrong++;
					}
				}
			}
			if (wrong == 0) {
				ok("all 32768 pixels survived the import exactly");
			} else {
				bad("%ld of 32768 pixels are wrong -- the import is corrupt",
					wrong);
			}
		}
		free(got);
		avk_image_destroy(importer->dev, image);
	}

	for (int i = 0; i < attribs.n_planes; i++) {
		if (attribs.fd[i] >= 0) {
			close(attribs.fd[i]);
		}
	}
	gbm_bo_destroy(bo);
}

/* A picture with something of everything in it, so the PNG is worth looking
 * at rather than just being non-black. */
static void report_composition(struct avk_device *dev, const char *png_path) {
	printf("\n\033[1mComposition\033[0m\n");

	struct avk_renderer renderer;
	if (!avk_renderer_init(&renderer, dev, TARGET_FORMAT)) {
		bad("renderer would not initialise");
		return;
	}
	struct avk_image *target = make_image(dev, OUT_W, OUT_H,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
		| VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true);
	if (target == NULL) {
		bad("output target would not allocate");
		avk_device_wait_idle(dev);
		avk_renderer_finish(&renderer);
		return;
	}

	/* A four-quadrant surface, so the transform row below is readable. */
	uint32_t src[64 * 64];
	for (uint32_t y = 0; y < 64; y++) {
		for (uint32_t x = 0; x < 64; x++) {
			uint32_t c;
			if (x < 32 && y < 32)       c = 0xFFE05050u;
			else if (x >= 32 && y < 32) c = 0xFF50E050u;
			else if (x < 32)            c = 0xFF5050E0u;
			else                        c = 0xFFE0E050u;
			src[y * 64 + x] = c;
		}
	}
	struct avk_image *quad = make_image(dev, 64, 64,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
	if (quad == NULL || !transfer(dev, quad, src, 64, 64, true)) {
		bad("could not prepare the sample surface");
		goto out;
	}

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, OUT_W,
		OUT_H);
	scene.has_clear = true;
	scene.clear_color[0] = 0.09f;
	scene.clear_color[1] = 0.10f;
	scene.clear_color[2] = 0.13f;
	scene.clear_color[3] = 1.0f;

	/* Three overlapping opaque panels. */
	static const float panel[3][3] = {
		{ 0.85f, 0.30f, 0.35f },
		{ 0.30f, 0.70f, 0.55f },
		{ 0.35f, 0.45f, 0.90f },
	};
	for (int i = 0; i < 3; i++) {
		struct avk_cmd *r = avk_scene_add(&scene, AVK_CMD_RECT);
		r->dst = (struct avk_box){ 40 + i * 60, 40 + i * 40, 200, 140 };
		r->color[0] = panel[i][0];
		r->color[1] = panel[i][1];
		r->color[2] = panel[i][2];
		r->color[3] = 1.0f;
	}

	/* A translucent sheet across them, to show blending. */
	struct avk_cmd *sheet = avk_scene_add(&scene, AVK_CMD_RECT);
	sheet->dst = (struct avk_box){ 20, 150, OUT_W - 40, 90 };
	sheet->color[0] = 1.0f;
	sheet->color[1] = 1.0f;
	sheet->color[2] = 1.0f;
	sheet->color[3] = 0.35f;

	/* The eight transforms, in a row, each scaled up so it is visible. */
	for (int i = 0; i < 8; i++) {
		struct avk_cmd *t = avk_scene_add(&scene, AVK_CMD_TEXTURE);
		t->dst = (struct avk_box){ 16 + i * 61, 280, 56, 56 };
		t->image = quad;
		t->src = (struct avk_fbox){ 0, 0, 64, 64 };
		t->transform = (enum avk_transform)i;
	}

	/* A cropped and scaled surface, and a clipped one, side by side. */
	struct avk_cmd *crop = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	crop->dst = (struct avk_box){ 40, 370, 180, 110 };
	crop->image = quad;
	crop->src = (struct avk_fbox){ 32, 32, 32, 32 };   /* bottom-right only */
	crop->filter_linear = true;

	struct avk_cmd *clipped = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	clipped->dst = (struct avk_box){ 280, 370, 180, 110 };
	clipped->image = quad;
	clipped->src = (struct avk_fbox){ 0, 0, 64, 64 };
	clipped->opacity = 0.8f;
	pixman_region32_t visible;
	pixman_region32_init_rect(&visible, 280, 370, 90, 110);
	avk_cmd_set_clip(clipped, &visible);
	pixman_region32_fini(&visible);

	uint64_t value = avk_render_frame(&renderer, target, &scene, NULL, 0,
		NULL, 0);
	if (value == 0 || !avk_device_timeline_wait(dev, value, 5000000000ULL)) {
		bad("composition failed");
		avk_scene_finish(&scene);
		goto out;
	}
	ok("composited %zu commands", scene.len + 1);
	avk_scene_finish(&scene);

	uint32_t *pixels = calloc(OUT_W * OUT_H, 4);
	if (pixels == NULL || !transfer(dev, target, pixels, OUT_W, OUT_H,
			false)) {
		bad("could not read the frame back");
		free(pixels);
		goto out;
	}

	/* B8G8R8A8 in memory is exactly cairo's ARGB32 on a little-endian host,
	 * so this is a wrap rather than a conversion. */
	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		(unsigned char *)pixels, CAIRO_FORMAT_ARGB32, OUT_W, OUT_H,
		OUT_W * 4);
	cairo_status_t status = cairo_surface_write_to_png(surface, png_path);
	cairo_surface_destroy(surface);
	if (status == CAIRO_STATUS_SUCCESS) {
		ok("wrote %s (%dx%d) -- open it: every pixel came out of AVK",
			png_path, OUT_W, OUT_H);
	} else {
		bad("could not write %s: %s", png_path,
			cairo_status_to_string(status));
	}
	free(pixels);

	printf("\n\033[1mCounters\033[0m\n");
	avk_renderer_log_stats(&renderer);
	if (renderer.stats.cpu_sync_waits == 0) {
		ok("no CPU synchronisation waits on the frame path");
	} else {
		bad("%" PRIu64 " CPU sync waits on the frame path",
			renderer.stats.cpu_sync_waits);
	}

out:
	if (quad != NULL) {
		avk_image_destroy(dev, quad);
	}
	avk_image_destroy(dev, target);
	avk_device_wait_idle(dev);
	avk_renderer_finish(&renderer);
}

/* ── main ───────────────────────────────────────────────────────────────── */

static void usage(const char *argv0) {
	printf("usage: %s [-o output.png] [-n /dev/dri/renderDNNN]\n\n"
		"Runs asteroidz's Vulkan engine (AVK) on this machine and reports\n"
		"what it can do. Does not touch the running compositor.\n\n"
		"  -o PATH   where to write the composited frame "
		"(default /tmp/avk-probe.png)\n"
		"  -n PATH   which DRM render node to use (default: the first that "
		"opens)\n"
		"  -h        this\n\n"
		"Set ASTEROIDZ_VK_DEBUG=1 to enable the Vulkan validation layers.\n",
		argv0);
}

int main(int argc, char **argv) {
	const char *png_path = "/tmp/avk-probe.png";
	const char *node_path = NULL;

	int opt;
	while ((opt = getopt(argc, argv, "o:n:h")) != -1) {
		switch (opt) {
		case 'o': png_path = optarg; break;
		case 'n': node_path = optarg; break;
		default:  usage(argv[0]); return opt == 'h' ? 0 : 1;
		}
	}

	printf("\033[1masteroidz-avk-probe\033[0m -- the Vulkan engine, on this "
		"machine\n");

	int drm_fd = -1;
	char opened[64] = {0};
	if (node_path != NULL) {
		drm_fd = open(node_path, O_RDWR | O_CLOEXEC);
		snprintf(opened, sizeof(opened), "%s", node_path);
	} else {
		for (int i = 128; i < 192 && drm_fd < 0; i++) {
			snprintf(opened, sizeof(opened), "/dev/dri/renderD%d", i);
			drm_fd = open(opened, O_RDWR | O_CLOEXEC);
		}
	}
	if (drm_fd < 0) {
		printf("\n  no DRM render node could be opened%s%s\n",
			node_path ? ": " : "", node_path ? node_path : "");
		printf("  (are you in the 'render' group?)\n");
		return 1;
	}
	printf("render node: %s\n", opened);

	struct avk_instance *inst = avk_instance_create("asteroidz-avk-probe");
	if (inst == NULL) {
		printf("\n  no usable Vulkan instance -- see the errors above\n");
		close(drm_fd);
		return 1;
	}
	avk_instance_log_caps(inst);

	report_devices(inst, drm_fd);

	struct avk_device *dev = avk_device_create(inst, drm_fd);
	if (dev == NULL) {
		printf("\n  no usable Vulkan device on this node\n");
		avk_instance_destroy(inst);
		close(drm_fd);
		return 1;
	}
	printf("\n\033[1mCapabilities\033[0m\n");
	avk_device_log_caps(dev);

	struct gbm_device *gbm = gbm_create_device(drm_fd);
	struct avk_dmabuf_importer importer;
	if (gbm != NULL && avk_dmabuf_importer_init(&importer, dev)) {
		report_import(&importer, gbm);
		avk_device_wait_idle(dev);
		avk_dmabuf_importer_finish(&importer);
	} else {
		bad("could not start the DMA-BUF importer");
	}
	if (gbm != NULL) {
		gbm_device_destroy(gbm);
	}

	report_composition(dev, png_path);

	avk_device_destroy(dev);
	avk_instance_destroy(inst);
	close(drm_fd);

	printf("\n");
	if (failures == 0) {
		printf("\033[32mAVK works on this machine.\033[0m\n");
		printf("It is what renders your desktop whenever asteroidz runs; "
			"there is nothing to enable.\n");
		return 0;
	}
	printf("\033[31m%d check(s) failed.\033[0m\n", failures);
	return 1;
}
