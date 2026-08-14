/*
 * M3 composition tests: AVK renders a scene snapshot, the pixels are read back
 * and compared.
 *
 * These are the tests that decide whether AVK is a compositor renderer. Every
 * one of them asserts on the framebuffer, not on a function returning true --
 * a renderer that binds the wrong sampler, mixes up premultiplied alpha, or
 * folds a transform the wrong way round returns success from every call it
 * makes and puts the wrong picture on screen.
 *
 * Composition happens in the surfaces' own encoding (8-bit sRGB-encoded,
 * premultiplied), which is what every SDR compositor does today and what makes
 * these results exact integers rather than tolerances. The linear FP16
 * pipeline is M5's job and will change these numbers deliberately.
 *
 * Exits 77 (skip) with no GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <drm_fourcc.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "render/vulkan/scene/avk_render.h"
/* C1's primitives and C4's CPU reference: the PQ test compares against them
 * rather than against a second copy of the same arithmetic. */
#include "render/az_output_color.h"
#include "render/color/az_color.h"
#include "render/color/az_color_ref.h"
/* M6B/G2: the profile reduction the encode pass is checked against. */
#include "render/color/az_icc.h"

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

#define W 64
#define H 64
#define TARGET_FORMAT VK_FORMAT_B8G8R8A8_UNORM

struct harness {
	struct avk_instance *inst;
	struct avk_device *dev;
	struct avk_renderer renderer;
	/* M5/C6. The scene-linear working renderer, built lazily: composition on
	 * Path B targets an FP16 attachment, and dynamic rendering bakes that
	 * format into every pipeline, so it is a second renderer by construction. */
	struct avk_renderer fp16;
	bool fp16_ok;
	struct avk_image *target;
	uint32_t pixels[W * H];
};

/* ── plumbing ───────────────────────────────────────────────────────────── */

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

static struct avk_image *make_image_fmt(struct avk_device *dev, uint32_t width,
		uint32_t height, VkImageUsageFlags usage, bool has_alpha,
		bool srgb_mutable, VkFormat format) {
	/*
	 * avk_image_alloc(), not calloc(). A bare calloc leaves `life` at 0, and
	 * avk_image_destroy() correctly REFUSES to destroy an image that does not
	 * read as AVK_IMAGE_LIVE -- so every image this fixture made used to leak,
	 * and vkDestroyDevice reported 21 objects still alive. The suite passed
	 * throughout, because nothing it asserted was about teardown.
	 */
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = format;
	image->extent = (VkExtent2D){ width, height };
	image->has_alpha = has_alpha;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	image->plane_count = 1;
	image->format_srgb = srgb_mutable
		? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_UNDEFINED;
	image->srgb_mutable = srgb_mutable;

	/* M5/C7: the caller may ask for an image that can carry an _SRGB view.
	 * Opt-in, because the flag is not free and every pre-M5 image here wants
	 * exactly what it always had. */
	VkFormat view_formats[2] = { format, VK_FORMAT_B8G8R8A8_SRGB };
	VkImageFormatListCreateInfo format_list = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = 2,
		.pViewFormats = view_formats,
	};
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = srgb_mutable ? (const void *)&format_list : NULL,
		.flags = srgb_mutable ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
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

static struct avk_image *make_image_ex(struct avk_device *dev, uint32_t width,
		uint32_t height, VkImageUsageFlags usage, bool has_alpha,
		bool srgb_mutable) {
	return make_image_fmt(dev, width, height, usage, has_alpha, srgb_mutable,
		TARGET_FORMAT);
}

static struct avk_image *make_image(struct avk_device *dev, uint32_t width,
		uint32_t height, VkImageUsageFlags usage, bool has_alpha) {
	return make_image_ex(dev, width, height, usage, has_alpha, false);
}

/* Fill an image from host pixels via a staging buffer -- the same route a SHM
 * client's contents take, so test 5 exercises the real upload shape. */
static bool upload(struct harness *h, struct avk_image *image,
		const uint32_t *src, uint32_t width, uint32_t height) {
	struct avk_device *dev = h->dev;
	VkDeviceSize size = (VkDeviceSize)width * height * 4;

	VkBuffer buffer;
	VkDeviceMemory memory;
	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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
	void *dst = NULL;
	vkMapMemory(dev->dev, memory, 0, size, 0, &dst);
	memcpy(dst, src, (size_t)size);
	vkUnmapMemory(dev->dev, memory);

	struct avk_cmd_ring ring;
	avk_cmd_ring_init(&ring, dev, "upload");
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);

	VkImageMemoryBarrier2 b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.oldLayout = image->layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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

	VkBufferImageCopy2 region = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		.bufferRowLength = width,
		.bufferImageHeight = height,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { width, height, 1 },
	};
	VkCopyBufferToImageInfo2 copy = {
		.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
		.srcBuffer = buffer,
		.dstImage = image->image,
		.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.regionCount = 1,
		.pRegions = &region,
	};
	vkCmdCopyBufferToImage2(cb, &copy);
	image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

	uint64_t value = avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0);
	bool ok = value != 0
		&& avk_device_timeline_wait(dev, value, 2000000000ULL);
	avk_cmd_ring_finish(&ring);
	vkDestroyBuffer(dev->dev, buffer, NULL);
	vkFreeMemory(dev->dev, memory, NULL);
	return ok;
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
	avk_cmd_ring_finish(&ring);

	if (ok) {
		void *mapped = NULL;
		if (vkMapMemory(dev->dev, memory, 0, size, 0, &mapped) == VK_SUCCESS) {
			memcpy(h->pixels, mapped, (size_t)size);
			vkUnmapMemory(dev->dev, memory);
		} else {
			ok = false;
		}
	}
	vkDestroyBuffer(dev->dev, buffer, NULL);
	vkFreeMemory(dev->dev, memory, NULL);
	return ok;
}

/* Target is B8G8R8A8: byte 0 is blue. Read as a little-endian uint32 that is
 * 0xAARRGGBB, so the accessors below match how the pattern is written. */
static uint32_t px(struct harness *h, uint32_t x, uint32_t y) {
	return h->pixels[y * W + x];
}
static int r_of(uint32_t p) { return (int)((p >> 16) & 0xFF); }
static int g_of(uint32_t p) { return (int)((p >> 8) & 0xFF); }
static int b_of(uint32_t p) { return (int)(p & 0xFF); }

static bool near(int got, int want, int tol) {
	return got >= want - tol && got <= want + tol;
}

static bool render_with(struct harness *h, struct avk_renderer *r,
		struct avk_scene *scene) {
	uint64_t value = avk_render_frame(r, h->target, scene, NULL, 0, NULL, 0);
	if (value == 0) {
		return false;
	}
	return avk_device_timeline_wait(h->dev, value, 2000000000ULL)
		&& readback(h);
}

static bool render(struct harness *h, struct avk_scene *scene) {
	return render_with(h, &h->renderer, scene);
}

/* ── THE M5 SDR GATE, ON THE GPU ────────────────────────────────────────── */

/*
 * C4 gate 1: an opaque sRGB source drawn 1:1 at full opacity comes back
 * UNCHANGED.
 *
 * ── WHY THIS EXISTS BEFORE THE THING IT JUDGES ───────────────────────────
 *
 * C4 says "HDR10 output work may not be enabled in the compositor until this
 * gate is green on-GPU". The CPU half of it already passes
 * (tests/test-color-pipeline.c). This is the on-GPU half, and it is written
 * BEFORE C7's decode variants exist on purpose: run against today's renderer it
 * establishes the baseline, so that a divergence after C7 lands is C7's and not
 * an open question about which of two changes moved the pixels.
 *
 * ── BIT-EXACT, NOT A TOLERANCE ───────────────────────────────────────────
 *
 * Today AVK composites in the surfaces' own encoding and performs no decode at
 * all, so an opaque texture blitted 1:1 must be the identity -- not "within a
 * code", exactly equal. C4 allows <= 1 8-bit code once a decode and re-encode
 * round trip exists; until then a tolerance would hide the very first frame in
 * which a variant starts firing when it should not.
 *
 * ── THE VALUES ARE CHOSEN TO BE HARD ─────────────────────────────────────
 *
 * 0 and 255 are the endpoints an encode/decode pair must be exact at -- F2
 * records that two encode endpoints are not algebraically zero, so they are the
 * first thing to move. 1 and 254 are one step in from those, where a rounding
 * rule that is off by half a code shows. 127/128 straddle the midpoint. A flat
 * grey would pass with almost any wrong curve.
 */
static void test_sdr_roundtrip_gate(struct harness *h) {
	printf("M5 GATE: opaque sRGB round-trip on the GPU\n");

	static const uint8_t hard[] = { 0, 1, 16, 64, 127, 128, 192, 254, 255 };
	const size_t n = sizeof(hard) / sizeof(hard[0]);

	uint32_t src[16 * 16];
	for (size_t y = 0; y < 16; y++) {
		for (size_t x = 0; x < 16; x++) {
			/* Opaque, and every channel independent: a shader that mixed up
			 * two channels would survive a grey ramp. */
			uint8_t r = hard[(x + 0) % n];
			uint8_t g = hard[(y + 3) % n];
			uint8_t b = hard[(x + y + 6) % n];
			src[y * 16 + x] = 0xFF000000u | ((uint32_t)r << 16)
				| ((uint32_t)g << 8) | b;
		}
	}

	struct avk_image *surface = make_image(h->dev, 16, 16,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
	if (surface == NULL || !upload(h, surface, src, 16, 16)) {
		CHECK(false, "gate: source uploads");
		if (surface != NULL) {
			avk_image_destroy(h->dev, surface);
		}
		return;
	}

	struct avk_scene scene;
	avk_scene_init(&scene);
	/* DAMAGE, or nothing is drawn at all. A `= {0}` scene leaves the pixman
	 * region uninitialised and the frame renders an empty command list into an
	 * undefined attachment -- which reads back as garbage and looks exactly
	 * like a catastrophic colour bug. */
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	/* And a clear, so the 16x16 result sits on a known background rather than
	 * on whatever the attachment happened to contain. */
	scene.has_clear = true;
	scene.clear_color[0] = 0.0f;
	scene.clear_color[1] = 0.0f;
	scene.clear_color[2] = 0.0f;
	scene.clear_color[3] = 1.0f;

	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, 16, 16 };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 16, 16 };
	tex->opacity = 1.0f;
	/* NEAREST. A linear filter at 1:1 is the identity in theory and a source
	 * of half-code drift in practice; this gate is about the colour pipeline,
	 * not about sampling. */
	tex->filter_linear = false;

	/* THE DOMAIN THE COMMAND CARRIES, asserted before the pixels. An untagged
	 * source is what almost every client is (ADR-004) and it is what this gate
	 * is about; if the walk or the default ever resolved it to something else,
	 * the pixel result below would be right for the wrong reason. */
	CHECK(tex->lum.tf == AZ_TF_SRGB && tex->lum.primaries == AZ_PRIM_BT709
		&& tex->lum.scale == 1.0f,
		"gate: the source resolves to the untagged sRGB domain (tf=%d prim=%d scale=%.3f)",
		(int)tex->lum.tf, (int)tex->lum.primaries, (double)tex->lum.scale);

	if (!render(h, &scene)) {
		CHECK(false, "gate: frame renders");
		avk_scene_finish(&scene);
		avk_image_destroy(h->dev, surface);
		return;
	}
	avk_scene_finish(&scene);

	int worst = 0, worst_x = -1, worst_y = -1;
	for (uint32_t y = 0; y < 16; y++) {
		for (uint32_t x = 0; x < 16; x++) {
			uint32_t want = src[y * 16 + x];
			uint32_t got = px(h, x, y);
			int d = 0;
			int dr = r_of(got) - r_of(want); if (dr < 0) dr = -dr;
			int dg = g_of(got) - g_of(want); if (dg < 0) dg = -dg;
			int db = b_of(got) - b_of(want); if (db < 0) db = -db;
			d = dr > dg ? dr : dg;
			d = d > db ? d : db;
			if (d > worst) { worst = d; worst_x = (int)x; worst_y = (int)y; }
		}
	}
	CHECK(worst == 0,
		"THE M5 SDR GATE: opaque sRGB round-trips bit-exactly "
		"(worst channel %d at %d,%d)", worst, worst_x, worst_y);

	/*
	 * THE PREMISE: the gate can see a difference of one code.
	 *
	 * Without this, "worst == 0" is also what a comparison against the wrong
	 * buffer, or against itself, would report. One deliberate corruption of a
	 * single channel must move `worst` to exactly 1.
	 */
	uint32_t saved = h->pixels[0];
	uint32_t bumped = (saved & 0xFFFF00FFu)
		| ((uint32_t)((g_of(saved) + 1) & 0xFF) << 8);
	/* Against the READBACK, not against the source. Comparing the corrupted
	 * pixel to src[0] measures whatever mismatch already existed plus one --
	 * which on a broken run reported 63 and said nothing about sensitivity. */
	int probe = g_of(bumped) - g_of(saved);
	if (probe < 0) { probe = -probe; }
	CHECK(probe == 1,
		"PREMISE: the comparison detects a one-code change (saw %d)", probe);

	avk_image_destroy(h->dev, surface);
}

/*
 * M5/C7: the _SRGB sampling path REFUSES an image that cannot legally have one.
 *
 * A view in a format the image was not created for is undefined behaviour, not
 * a failed call, and validation does not reliably catch it. Every image this
 * fixture makes is a plain non-mutable one -- exactly like the transients the
 * renderer allocates for itself -- so the accessor must return NULL for all of
 * them and the fast path must simply not be taken.
 *
 * The interesting case, an imported dmabuf whose modifier IS srgb_mutable,
 * needs a real client buffer and belongs to the headless suites; what is
 * asserted here is the half that must never produce a VUID.
 */
static void test_srgb_view_refusal(struct harness *h) {
	printf("M5/C7: the _SRGB sampling path refuses a non-mutable image\n");

	struct avk_image *plain = make_image(h->dev, 16, 16,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
	if (plain == NULL) {
		CHECK(false, "srgb: image allocates");
		return;
	}

	CHECK(!plain->srgb_mutable,
		"an image allocated by the renderer is not srgb_mutable");
	CHECK(avk_pipelines_texture_set_srgb(&h->renderer.pipes, plain, false)
			== VK_NULL_HANDLE
		&& avk_pipelines_texture_set_srgb(&h->renderer.pipes, plain, true)
			== VK_NULL_HANDLE,
		"and the _SRGB descriptor is refused for both filters");
	CHECK(plain->view_srgb == VK_NULL_HANDLE,
		"no illegal view was created on the way to refusing");

	/* THE PREMISE: the PLAIN accessor still works on the same image, so the
	 * refusal above is about the sRGB path and not about a broken fixture. */
	CHECK(avk_pipelines_texture_set(&h->renderer.pipes, plain, false)
			!= VK_NULL_HANDLE,
		"PREMISE: the ordinary descriptor is still produced");

	avk_image_destroy(h->dev, plain);
}

/*
 * M5/C7: a decode variant actually decodes, and agrees with the CPU twin.
 *
 * ── WHAT WOULD OTHERWISE GO UNNOTICED ────────────────────────────────────
 *
 * A specialisation constant that never reaches the pipeline, a variant that
 * failed to compile, or a selector that matched nothing all produce EXACTLY the
 * pre-M5 picture -- which is the picture every other test in this file asserts
 * is correct. So the decode is checked against an independent computation of
 * what it should be, not against "it still looks fine".
 *
 * The reference is the sRGB EOTF evaluated here in double precision from the
 * published constants. It is deliberately not az_color_ref's version: this
 * fixture links the renderer, not the reference library, and a second reading
 * of the same standard is a better oracle than the same reading twice.
 *
 * ── WHY THE TOLERANCE IS ONE CODE AND NOT ZERO ───────────────────────────
 *
 * The result lands in an 8-bit UNORM attachment, so the linear value is
 * quantised on the way out. One code is quantisation; two is a different curve.
 */
static double srgb_eotf_ref(double e) {
	if (e <= 0.04045) {
		return e / 12.92;
	}
	return pow((e + 0.055) / 1.055, 2.4);
}

static void test_decode_variant(struct harness *h) {
	printf("M5/C7: the sRGB decode variant, against an independent EOTF\n");

	/* Mid-tones, where the curve is steepest and a wrong exponent shows most.
	 * 0 and 255 are excluded on purpose: both are fixed points of every
	 * candidate curve and would pass against a shader that did nothing. */
	static const uint8_t vals[] = { 32, 64, 96, 128, 160, 192, 224 };
	const size_t n = sizeof(vals) / sizeof(vals[0]);

	uint32_t src[16 * 16];
	for (size_t i = 0; i < 16 * 16; i++) {
		uint8_t v = vals[i % n];
		src[i] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
	}
	struct avk_image *surface = make_image(h->dev, 16, 16,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
	if (surface == NULL || !upload(h, surface, src, 16, 16)) {
		CHECK(false, "decode: source uploads");
		if (surface != NULL) { avk_image_destroy(h->dev, surface); }
		return;
	}

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;

	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, 16, 16 };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 16, 16 };
	tex->opacity = 1.0f;
	tex->filter_linear = false;

	uint64_t before = h->renderer.stats.decode_draws;
	h->renderer.decode_enabled = true;
	bool ok = render(h, &scene);
	h->renderer.decode_enabled = false;
	uint64_t took = h->renderer.stats.decode_draws - before;
	avk_scene_finish(&scene);
	if (!ok) {
		CHECK(false, "decode: frame renders");
		avk_image_destroy(h->dev, surface);
		return;
	}

	/* THE PREMISE, FIRST. A pixel comparison that passes because no variant
	 * ran is the failure this whole test exists to prevent. */
	CHECK(took == 1, "PREMISE: exactly one draw took a decode variant (%llu)",
		(unsigned long long)took);

	int worst = 0; int at = -1;
	for (uint32_t i = 0; i < 16 * 16; i++) {
		int e = (int)(src[i] & 0xFF);
		int want = (int)(srgb_eotf_ref((double)e / 255.0) * 255.0 + 0.5);
		int got = b_of(px(h, i % 16, i / 16));
		int d = got - want; if (d < 0) { d = -d; }
		if (d > worst) { worst = d; at = (int)i; }
	}
	CHECK(worst <= 1,
		"the sRGB variant matches an independent EOTF (worst %d codes at %d)",
		worst, at);

	/*
	 * AND IT IS NOT THE IDENTITY. Every value above is decoded to something
	 * much darker; if the shader passed the texel through, `worst` would be
	 * enormous. Asserting that explicitly means the tolerance above cannot be
	 * satisfied by a curve that does nothing.
	 */
	int e_mid = 128;
	int decoded_mid = (int)(srgb_eotf_ref(128.0 / 255.0) * 255.0 + 0.5);
	CHECK(decoded_mid < e_mid - 30,
		"PREMISE: the reference curve is far from the identity (%d -> %d)",
		e_mid, decoded_mid);

	avk_image_destroy(h->dev, surface);
}

/*
 * M5 PATH A, END TO END: decode on sample, encode on write, and the picture
 * comes back.
 *
 * ── WHY THIS IS THE ONE THAT MATTERS ─────────────────────────────────────
 *
 * The two halves are only correct together. Decode alone composites in linear
 * light and writes it as though it were still electrical -- everything washes
 * out. Encode alone applies an inverse curve to values that were never decoded
 * -- everything darkens. Either half on its own is a wrong picture that a test
 * of that half in isolation would happily call correct.
 *
 * So this asserts all three states from one fixture: neither (bit-exact, the
 * pre-M5 path), decode only (must be WRONG, and by a lot), and both (within a
 * code). The middle one is the falsifier -- without it, "both halves round-trip"
 * is also what two no-ops would report.
 *
 * ── AND IT USES ITS OWN TARGET ───────────────────────────────────────────
 *
 * A mutable one, because the shared target is not. That is deliberate: it keeps
 * every other test in this file rendering into exactly the image it always did,
 * so a regression here cannot be a regression there.
 */
static void test_path_a_roundtrip(struct harness *h) {
	printf("M5 PATH A: decode on sample + encode on write\n");

	struct avk_image *target = make_image_ex(h->dev, W, H,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
		| VK_IMAGE_USAGE_SAMPLED_BIT, true, true);
	if (target == NULL) {
		CHECK(false, "path A: mutable target allocates");
		return;
	}
	CHECK(avk_image_srgb_view(h->dev, target) != VK_NULL_HANDLE,
		"PREMISE: the mutable target really can carry an _SRGB view");

	/*
	 * ALL 256 CODES, NOT A HANDFUL.
	 *
	 * The first version of this sampled seven values and reported a perfect
	 * round trip. On a real output the same pair was one code out on 94% of
	 * the frame -- because the shader's pow()-based EOTF and the hardware's
	 * sRGB table are not exact inverses, and seven values is a small enough
	 * sample to miss that entirely. A 16x16 tile holds every 8-bit code
	 * exactly once, so there is no reason to sample at all.
	 */
	uint32_t src[16 * 16];
	for (size_t i = 0; i < 16 * 16; i++) {
		uint8_t v = (uint8_t)i;
		src[i] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
	}
	struct avk_image *surface = make_image_ex(h->dev, 16, 16,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		false, true);
	if (surface == NULL || !upload(h, surface, src, 16, 16)) {
		CHECK(false, "path A: source uploads");
		avk_image_destroy(h->dev, target);
		if (surface != NULL) { avk_image_destroy(h->dev, surface); }
		return;
	}

	struct avk_image *saved_target = h->target;
	h->target = target;

	int worst[3] = { -1, -1, -1 };
	uint64_t decoded[3] = { 0, 0, 0 };
	for (int arm = 0; arm < 3; arm++) {
		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
		/*
		 * A FULL-COVER RECT UNDER THE TEXTURE, because that is what a real
		 * frame draws: the compositor lays down a background rect and the
		 * wallpaper covers it. AZ_AVK_CMD_DUMP on a live frame showed exactly
		 * this pair, and it is the only structural difference between what
		 * this fixture rendered and what the display did.
		 */
		struct avk_cmd *bg = avk_scene_add(&scene, AVK_CMD_RECT);
		bg->dst = (struct avk_box){ 0, 0, 16, 16 };
		bg->color[0] = 0.15f; bg->color[1] = 0.15f; bg->color[2] = 0.15f;
		bg->color[3] = 1.0f;

		struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
		tex->dst = (struct avk_box){ 0, 0, 16, 16 };
		tex->image = surface;
		tex->src = (struct avk_fbox){ 0, 0, 16, 16 };
		tex->opacity = 1.0f;
		tex->filter_linear = false;

		uint64_t before = h->renderer.stats.decode_draws;
		h->renderer.decode_enabled = (arm >= 1);
		h->renderer.encode_srgb = (arm == 2);
		bool ok = render(h, &scene);
		h->renderer.decode_enabled = false;
		h->renderer.encode_srgb = false;
		decoded[arm] = h->renderer.stats.decode_draws - before;
		avk_scene_finish(&scene);
		if (!ok) {
			CHECK(false, "path A: arm %d renders", arm);
			continue;
		}
		int w = 0;
		for (uint32_t i = 0; i < 16 * 16; i++) {
			int want = (int)(src[i] & 0xFF);
			int got = b_of(px(h, i % 16, i / 16));
			int d = got - want; if (d < 0) { d = -d; }
			if (d > w) { w = d; }
		}
		worst[arm] = w;
	}

	h->target = saved_target;

	printf("  ---- worst channel: neither %d, decode-only %d, both %d\n",
		worst[0], worst[1], worst[2]);
	printf("  ---- decode draws:  neither %llu, decode-only %llu, both %llu\n",
		(unsigned long long)decoded[0], (unsigned long long)decoded[1],
		(unsigned long long)decoded[2]);
	/* THE PREMISE THAT WAS MISSING. "Both halves round-trip at 0 codes" and
	 * "neither half ran" produce the same number, and only this tells them
	 * apart. It is the assertion whose absence let a fixture disagree with a
	 * real output for an hour. */
	CHECK(decoded[0] == 0 && decoded[1] == 1 && decoded[2] == 1,
		"PREMISE: decode ran in exactly the arms it should (%llu/%llu/%llu)",
		(unsigned long long)decoded[0], (unsigned long long)decoded[1],
		(unsigned long long)decoded[2]);
	CHECK(worst[0] == 0,
		"neither half: bit-exact, the pre-M5 path (worst %d)", worst[0]);
	/* THE FALSIFIER. Decode without encode must be visibly wrong, or "both
	 * halves round-trip" is also what two no-ops would report. */
	CHECK(worst[1] > 20,
		"FALSIFIER: decode without encode is badly wrong (worst %d)",
		worst[1]);
	CHECK(worst[2] >= 0 && worst[2] <= 1,
		"BOTH HALVES: the round trip closes within a code (worst %d)",
		worst[2]);

	avk_image_destroy(h->dev, surface);
	avk_image_destroy(h->dev, target);
}

/*
 * ── C7: THE REST OF THE DECODE PATH ──────────────────────────────────────
 *
 * PQ decode, source primaries, and the Path-A ceiling -- the three pieces of
 * C7 that were missing while the transfer-function half shipped.
 *
 * WHY THE FRAMEBUFFER IS A DIRECT READING OF THE SCENE VALUE HERE. With decode
 * on and NO encode pass, composited scene values are written to the 8-bit
 * attachment as they are. So `code / 255` IS the scene value the decode
 * produced, clamped to the attachment's range -- which makes this a probe of
 * the decode itself rather than of a round trip, and means a wrong answer shows
 * as a wrong number rather than as a cancelled pair of errors.
 */
static void test_c7_decode_path(struct harness *h) {
	printf("M5/C7: PQ decode, source primaries, and the Path-A ceiling\n");

	uint32_t src[16 * 16];
	struct avk_image *surface = make_image(h->dev, 16, 16,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
	if (surface == NULL) {
		CHECK(false, "c7: source allocates");
		return;
	}

	/* ── 1. PQ ──────────────────────────────────────────────────────────
	 *
	 * Codes chosen where PQ has resolution to spare: it is so steep near black
	 * that most of the 8-bit range decodes to under one output code, and a test
	 * built from evenly spaced inputs would be comparing zeros. */
	static const uint8_t pq_codes[] = { 255, 235, 204, 180, 153 };
	const int npq = (int)(sizeof(pq_codes) / sizeof(pq_codes[0]));
	for (int i = 0; i < 16 * 16; i++) {
		uint8_t v = pq_codes[i % npq];
		src[i] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
	}
	if (!upload(h, surface, src, 16, 16)) {
		CHECK(false, "c7: PQ source uploads");
		avk_image_destroy(h->dev, surface);
		return;
	}
	{
		struct avk_scene sc;
		avk_scene_init(&sc);
		pixman_region32_union_rect(&sc.damage, &sc.damage, 0, 0, W, H);
		sc.has_clear = true;
		sc.clear_color[3] = 1.0f;
		struct avk_cmd *t = avk_scene_add(&sc, AVK_CMD_TEXTURE);
		t->dst = (struct avk_box){ 0, 0, 16, 16 };
		t->image = surface;
		t->src = (struct avk_fbox){ 0, 0, 16, 16 };
		t->opacity = 1.0f;
		t->filter_linear = false;
		t->lum.tf = AZ_TF_PQ;
		t->lum.primaries = AZ_PRIM_BT709;
		t->lum.scale = 1.0f;   /* isolate the curve from the domain's scale */
		uint64_t before = h->renderer.stats.decode_by_variant[AVK_DECODE_PQ];
		h->renderer.decode_enabled = true;
		bool ok = render_with(h, &h->renderer, &sc);
		h->renderer.decode_enabled = false;
		uint64_t pq_draws =
			h->renderer.stats.decode_by_variant[AVK_DECODE_PQ] - before;
		avk_scene_finish(&sc);

		CHECK(pq_draws == 1, "PREMISE: the PQ variant was selected (%llu draws)",
			(unsigned long long)pq_draws);
		int worst = 0;
		for (int i = 0; i < npq && ok; i++) {
			double want_lin = az_pq_eotf((float)pq_codes[i] / 255.0f);
			int want = (int)(want_lin * 255.0 + 0.5);
			int got = b_of(px(h, (uint32_t)i, 0));
			int d = got - want; if (d < 0) { d = -d; }
			if (d > worst) { worst = d; }
			printf("  ---- PQ code %3d -> scene %.5f  want %3d  got %3d\n",
				pq_codes[i], want_lin, want, got);
		}
		CHECK(ok && worst <= 1,
			"PQ decode matches C1's az_pq_eotf (worst %d codes)", worst);
	}

	/* ── 2. SOURCE PRIMARIES ────────────────────────────────────────────
	 *
	 * A colour whose BT.2020 -> BT.709 conversion stays INSIDE [0,1]. A
	 * saturated one would leave the container, the attachment would clamp it,
	 * and a clamped result cannot tell a correct conversion from an absent
	 * one -- the same reason the earlier falsifiers needed colour rather than
	 * grey, one step further on. */
	{
		const uint8_t cr = 200, cg = 150, cb = 100;
		for (int i = 0; i < 16 * 16; i++) {
			src[i] = 0xFF000000u | ((uint32_t)cr << 16)
				| ((uint32_t)cg << 8) | cb;
		}
		if (!upload(h, surface, src, 16, 16)) {
			CHECK(false, "c7: primaries source uploads");
			avk_image_destroy(h->dev, surface);
			return;
		}
		int got709[3] = {0,0,0}, got2020[3] = {0,0,0};
		for (int arm = 0; arm < 2; arm++) {
			struct avk_scene sc;
			avk_scene_init(&sc);
			pixman_region32_union_rect(&sc.damage, &sc.damage, 0, 0, W, H);
			sc.has_clear = true;
			sc.clear_color[3] = 1.0f;
			struct avk_cmd *t = avk_scene_add(&sc, AVK_CMD_TEXTURE);
			t->dst = (struct avk_box){ 0, 0, 16, 16 };
			t->image = surface;
			t->src = (struct avk_fbox){ 0, 0, 16, 16 };
			t->opacity = 1.0f;
			t->filter_linear = false;
			t->lum.tf = AZ_TF_SRGB;
			t->lum.primaries = arm == 0 ? AZ_PRIM_BT709 : AZ_PRIM_BT2020;
			t->lum.scale = 1.0f;
			h->renderer.decode_enabled = true;
			bool ok = render_with(h, &h->renderer, &sc);
			h->renderer.decode_enabled = false;
			avk_scene_finish(&sc);
			if (!ok) { CHECK(false, "c7: primaries arm %d renders", arm); }
			uint32_t p = px(h, 4, 4);
			int *dst = arm == 0 ? got709 : got2020;
			dst[0] = r_of(p); dst[1] = g_of(p); dst[2] = b_of(p);
		}
		double lin[3] = {
			az_srgb_eotf((float)cr / 255.0f),
			az_srgb_eotf((float)cg / 255.0f),
			az_srgb_eotf((float)cb / 255.0f),
		};
		float linf[3] = { (float)lin[0], (float)lin[1], (float)lin[2] };
		float conv[3];
		az_mat_mul_vec3(AZ_MAT_2020_TO_709, linf, conv);
		int want[3];
		for (int c = 0; c < 3; c++) {
			double v = conv[c] < 0.0f ? 0.0 : (conv[c] > 1.0f ? 1.0 : conv[c]);
			want[c] = (int)(v * 255.0 + 0.5);
		}
		printf("  ---- src(%d,%d,%d)  as BT.709 -> %d,%d,%d   "
			"as BT.2020 -> %d,%d,%d (want %d,%d,%d)\n", cr, cg, cb,
			got709[0], got709[1], got709[2], got2020[0], got2020[1], got2020[2],
			want[0], want[1], want[2]);
		int worst = 0;
		for (int c = 0; c < 3; c++) {
			int d = got2020[c] - want[c]; if (d < 0) { d = -d; }
			if (d > worst) { worst = d; }
		}
		CHECK(worst <= 1,
			"a BT.2020 source is converted to the scene's primaries "
			"(worst %d codes)", worst);
		/* Without this, "the conversion matched" is also what NO conversion
		 * would report if the reference happened to be the identity. */
		int moved = 0;
		for (int c = 0; c < 3; c++) {
			int d = got2020[c] - got709[c]; if (d < 0) { d = -d; }
			if (d > moved) { moved = d; }
		}
		CHECK(moved > 8,
			"PREMISE: declaring BT.2020 CHANGES the picture (%d codes)", moved);
	}

	/* ── 3. THE PATH-A CEILING (F5) ─────────────────────────────────────── */
	{
		static const uint8_t codes[] = { 128, 180, 220, 255 };
		const int nc = (int)(sizeof(codes) / sizeof(codes[0]));
		for (int i = 0; i < 16 * 16; i++) {
			uint8_t v = codes[i % nc];
			src[i] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
		}
		if (!upload(h, surface, src, 16, 16)) {
			CHECK(false, "c7: ceiling source uploads");
			avk_image_destroy(h->dev, surface);
			return;
		}
		int got[2][4] = {{0}};
		for (int arm = 0; arm < 2; arm++) {
			struct avk_scene sc;
			avk_scene_init(&sc);
			pixman_region32_union_rect(&sc.damage, &sc.damage, 0, 0, W, H);
			sc.has_clear = true;
			sc.clear_color[3] = 1.0f;
			struct avk_cmd *t = avk_scene_add(&sc, AVK_CMD_TEXTURE);
			t->dst = (struct avk_box){ 0, 0, 16, 16 };
			t->image = surface;
			t->src = (struct avk_fbox){ 0, 0, 16, 16 };
			t->opacity = 1.0f;
			t->filter_linear = false;
			t->lum.tf = AZ_TF_SRGB;
			t->lum.primaries = AZ_PRIM_BT709;
			/* arm 0: an ordinary SDR domain, which must never see the curve.
			 * arm 1: a domain that can reach scene 3.0, which on Path A has
			 * nowhere to be bounded but the decode. */
			t->lum.scale = arm == 0 ? 1.0f : 3.0f;
			h->renderer.decode_enabled = true;
			bool ok = render_with(h, &h->renderer, &sc);
			h->renderer.decode_enabled = false;
			avk_scene_finish(&sc);
			if (!ok) { CHECK(false, "c7: ceiling arm %d renders", arm); }
			for (int i = 0; i < nc; i++) {
				got[arm][i] = b_of(px(h, (uint32_t)i, 0));
			}
		}
		printf("  ---- scale 1.0: %3d %3d %3d %3d\n",
			got[0][0], got[0][1], got[0][2], got[0][3]);
		printf("  ---- scale 3.0: %3d %3d %3d %3d\n",
			got[1][0], got[1][1], got[1][2], got[1][3]);

		/* An ordinary SDR domain is untouched: every value below the knee, and
		 * the knee is never even set for scale <= 1. Compared against C1
		 * directly rather than against the other arm. */
		int worst_sdr = 0;
		for (int i = 0; i < nc; i++) {
			int want = (int)(az_srgb_eotf((float)codes[i] / 255.0f) * 255.0
				+ 0.5);
			int d = got[0][i] - want; if (d < 0) { d = -d; }
			if (d > worst_sdr) { worst_sdr = d; }
		}
		CHECK(worst_sdr <= 1,
			"an SDR domain never meets the ceiling (worst %d codes)",
			worst_sdr);
		/*
		 * THE FALSIFIER THE KNEE OWES (F5): a >1 domain must not hard-clip at
		 * white. Every one of these scene values exceeds 1.0 -- 128 alone is
		 * 0.216*3 = 0.65 and the rest are far above -- so with the identity
		 * curve ADR-009 mandates they would ALL read 255 and be
		 * indistinguishable. They must instead be distinct and below 255.
		 */
		CHECK(got[1][3] < 255,
			"a >1 domain does NOT clip at white (top value %d)", got[1][3]);
		CHECK(got[1][1] < got[1][2] && got[1][2] < got[1][3],
			"and values above the knee stay DISTINCT (%d < %d < %d)",
			got[1][1], got[1][2], got[1][3]);
	}

	avk_image_destroy(h->dev, surface);
}

/* ── M5 PATH B: THE OUTPUT-ENCODE PASS (C6, ADR-008) ─────────────────────── */

/*
 * The scene-linear working renderer, built on first use.
 *
 * A SECOND RENDERER and not a mode on the first, because dynamic rendering
 * bakes the colour-attachment format into every pipeline: composition on
 * Path B targets an FP16 intermediate, so it is a different pipeline set by
 * construction. az_avk.h picks between them exactly this way.
 */
static struct avk_renderer *fp16_renderer(struct harness *h) {
	if (!h->fp16_ok) {
		if (!avk_renderer_init(&h->fp16, h->dev,
				VK_FORMAT_R16G16B16A16_SFLOAT)) {
			return NULL;
		}
		h->fp16_ok = true;
	}
	return &h->fp16;
}

/* SDR, identity everything: the state C3 derives for an 8-bit or 10-bit output
 * with no image description. Dither off, because this is a round-trip test and
 * a deliberate +-half-code perturbation would be measuring the dither. */
static struct avk_encode_params sdr_encode_params(void) {
	struct avk_encode_params p = {0};
	for (int i = 0; i < 9; i++) {
		p.matrix[i] = (i % 4) == 0 ? 1.0f : 0.0f;
	}
	p.knee = 1.0f;
	p.peak = 1.0f;
	p.anchor = 0.0f;
	p.dither_q = 0.0f;
	p.tf = AVK_ENCODE_TF_SRGB;
	return p;
}

/*
 * THE PATH-B SDR GATE.
 *
 * C4's gate says an opaque sRGB source drawn 1:1 must come back within one
 * 8-bit code once a decode/encode round trip exists. Path A closes it at ZERO
 * because both halves are the hardware's own sRGB conversion and exact
 * inverses. Path B cannot: the decode is a shader pow(), the encode is a
 * different shader pow(), and the value between them has been through an FP16
 * store. One code is the contract and this measures how much of it is used.
 *
 * IT RUNS ON THE SAME 8-BIT TARGET AS PATH A ON PURPOSE. A gate that ran only
 * on a 10-bit target would compare Path B against nothing -- there IS no
 * pre-M5 10-bit picture to be within a code of. Forcing Path B onto an 8-bit
 * output is what makes "within a code of the picture we already had" a
 * statement about this pass rather than about a format change.
 */
static void test_path_b_sdr_gate(struct harness *h) {
	printf("M5 PATH B: the encode pass, against the pre-M5 8-bit picture\n");

	struct avk_renderer *fp = fp16_renderer(h);
	if (fp == NULL) {
		CHECK(false, "path B: FP16 renderer initialises");
		return;
	}

	/*
	 * All 256 codes in every channel -- the same reason as Path A's: seven
	 * values missed a one-code error covering 94% of a real display.
	 *
	 * NOT A GREY RAMP, and that is not a refinement. Every row of the
	 * BT.709->BT.2020 matrix sums to 1, so a NEUTRAL colour is invariant under
	 * it: this test's falsifier ran the wrong matrix over a grey ramp and
	 * reported ZERO difference, which is a true statement about grey and no
	 * statement at all about the matrix. Offsetting the channels by a third of
	 * the range each keeps the per-channel coverage complete and makes no
	 * pixel neutral.
	 */
	uint32_t src[16 * 16];
	for (size_t i = 0; i < 16 * 16; i++) {
		uint8_t r = (uint8_t)i;
		uint8_t g = (uint8_t)((i + 85) % 256);
		uint8_t b = (uint8_t)((i + 170) % 256);
		src[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
	}
	struct avk_image *surface = make_image(h->dev, 16, 16,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
	if (surface == NULL || !upload(h, surface, src, 16, 16)) {
		CHECK(false, "path B: source uploads");
		if (surface != NULL) { avk_image_destroy(h->dev, surface); }
		return;
	}

	struct avk_encode_intermediate work = {0};
	struct avk_image *inter = avk_encode_intermediate_get(&work, h->dev,
		&fp->retire, VK_FORMAT_R16G16B16A16_SFLOAT, W, H);
	CHECK(inter != NULL, "PREMISE: the FP16 intermediate allocates at %dx%d",
		W, H);
	if (inter == NULL) {
		avk_image_destroy(h->dev, surface);
		return;
	}

	int worst[3] = { -1, -1, -1 };
	uint64_t enc_draws[3] = { 0, 0, 0 };
	for (int arm = 0; arm < 3; arm++) {
		struct avk_renderer *r = arm == 0 ? &h->renderer : fp;

		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
		struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
		tex->dst = (struct avk_box){ 0, 0, 16, 16 };
		tex->image = surface;
		tex->src = (struct avk_fbox){ 0, 0, 16, 16 };
		tex->opacity = 1.0f;
		tex->filter_linear = false;

		uint64_t before = r->stats.encode_draws;
		if (arm > 0) {
			r->decode_enabled = true;
			r->encode_intermediate = inter;
			r->encode_params = sdr_encode_params();
			/* The first frame into a freshly created intermediate; it holds
			 * nothing, so the whole thing has to be composited. */
			r->encode_full_frame = (arm == 1);
			if (arm == 2) {
				/*
				 * THE FALSIFIER: the BT.709 -> BT.2020 matrix, on an SDR
				 * output that must not have one. C3 derives the identity here
				 * (the scene's primaries ARE the output's), and a wrong matrix
				 * is the failure ADR-008's own falsifier names -- it renders a
				 * complete, plausible, differently-coloured desktop.
				 */
				for (int i = 0; i < 9; i++) {
					r->encode_params.matrix[i] = AZ_MAT_709_TO_2020[i];
				}
			}
		}
		bool ok = render_with(h, r, &scene);
		enc_draws[arm] = r->stats.encode_draws - before;
		r->decode_enabled = false;
		r->encode_intermediate = NULL;
		r->encode_full_frame = false;
		avk_scene_finish(&scene);
		if (!ok) {
			CHECK(false, "path B: arm %d renders", arm);
			continue;
		}
		int w = 0;
		for (uint32_t i = 0; i < 16 * 16; i++) {
			uint32_t want = src[i];
			uint32_t got = px(h, i % 16, i / 16);
			int dr = r_of(got) - r_of(want); if (dr < 0) { dr = -dr; }
			int dg = g_of(got) - g_of(want); if (dg < 0) { dg = -dg; }
			int db = b_of(got) - b_of(want); if (db < 0) { db = -db; }
			int d = dr > dg ? dr : dg;
			if (db > d) { d = db; }
			if (d > w) { w = d; }
		}
		worst[arm] = w;
	}

	printf("  ---- worst channel: direct %d, path B %d, wrong-matrix %d\n",
		worst[0], worst[1], worst[2]);
	printf("  ---- encode draws:  direct %llu, path B %llu, wrong-matrix %llu\n",
		(unsigned long long)enc_draws[0], (unsigned long long)enc_draws[1],
		(unsigned long long)enc_draws[2]);

	/* THE PREMISE. "Path B round-trips" and "the encode pass never ran" produce
	 * the same number when the arm silently fell back to the direct path. */
	CHECK(enc_draws[0] == 0 && enc_draws[1] > 0 && enc_draws[2] > 0,
		"PREMISE: the encode pass ran in exactly the arms it should "
		"(%llu/%llu/%llu)",
		(unsigned long long)enc_draws[0], (unsigned long long)enc_draws[1],
		(unsigned long long)enc_draws[2]);
	CHECK(worst[0] == 0,
		"direct: bit-exact, the pre-M5 path (worst %d)", worst[0]);
	CHECK(worst[1] >= 0 && worst[1] <= 1,
		"THE PATH B SDR GATE: within one code of the pre-M5 picture (worst %d)",
		worst[1]);
	/* Without this, "within a code" is also what an encode pass that did
	 * nothing at all would report. */
	CHECK(worst[2] > 20,
		"FALSIFIER: a wrong gamut matrix is visibly wrong (worst %d)",
		worst[2]);

	avk_encode_intermediate_finish(&work, h->dev, &fp->retire);
	avk_image_destroy(h->dev, surface);
}

/*
 * SOLID COLOURS ARE SCENE VALUES ON A LINEAR PATH, AND THE COST OF FORGETTING.
 *
 * Every texture a client hands over is decoded (C7). A RECT's colour is not a
 * client buffer -- it is an sRGB hex triple out of a config file -- and nothing
 * used to decode it, because composition happened in the same encoding the
 * config was written in. On a linear path it must be decoded like any other
 * source, and az_avk.h's az_avk_scene_rgb() is where that happens.
 *
 * MEASURED BEFORE THE FIX, on this fixture: an electrical 64 came back 137, 128
 * came back 188, 192 came back 225. That is every border, every background rect
 * and every shadow tint on the desktop, not an edge case.
 *
 * What THIS asserts is the renderer's half of the contract, which is the half
 * the renderer owns: given a SCENE value, the encode puts the right electrical
 * code on screen. The other half -- that the walk supplies scene values -- is a
 * compositor question and is asserted by contrib/avk-m5-path-b-test.sh, which
 * has a walk in it. Splitting them is deliberate: with one test spanning both,
 * a fix in either place makes it pass and neither is pinned.
 */

/*
 * ── M6B GATE G2: THE MEASURED CURVE, ON THE GPU ───────────────────────────
 *
 * G1 established that az_icc_apply() reproduces lcms2's own transform to 1.92
 * codes. THIS asserts that the AZ_TF_LUT1D encode variant reproduces
 * az_icc_apply() -- so the chain from the .icm file to a pixel is closed by two
 * links, each checked against an implementation that is not the other one.
 *
 * az_icc_apply IS the reference here rather than a fresh C4 extension, and
 * deliberately: a second CPU implementation of the same table lookup would
 * agree with the first for the same reasons and disagree with the GPU for the
 * same reasons, which is a second copy of the arithmetic rather than a second
 * opinion. What differs between the two sides is what is worth measuring --
 * a CPU lerp against a hardware LINEAR filter with quantised subtexel weights.
 *
 * NON-NEUTRAL, and that is not decoration. A profile's matrix is near-identity
 * on the neutral axis for the same reason every plausible wrong matrix is: the
 * rows very nearly sum to 1. G1 measured 100 codes over a 16^3 grid and 9 codes
 * on greys alone -- an elevenfold understatement -- so a grey ramp here would be
 * a gate that agrees with almost anything.
 *
 * Display-specific: skips loudly without the profile, because a machine without
 * it cannot run this gate and passing silently would be worse than not having
 * it.
 */
#define G2_PROFILE "/home/ralf/FI32U.icm"

static bool g2_slurp(const char *path, void **data, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) {
		fclose(f);
		return false;
	}
	*data = malloc((size_t)n);
	*size = fread(*data, 1, (size_t)n, f);
	fclose(f);
	return *size == (size_t)n;
}

static void test_path_b_lut_encode(struct harness *h) {
	printf("M6B/G2: the measured-curve encode against az_icc_apply\n");

	void *data = NULL;
	size_t size = 0;
	if (!g2_slurp(G2_PROFILE, &data, &size)) {
		printf("  SKIP %s not present -- this gate is display-specific\n",
			G2_PROFILE);
		return;
	}
	struct az_icc_shaper shaper;
	enum az_icc_reject rc = az_icc_load_shaper(data, size, true, &shaper);
	free(data);
	CHECK(rc == AZ_ICC_OK, "G2: the profile reduces to a matrix-shaper (%s)",
		az_icc_reject_name(rc));
	if (rc != AZ_ICC_OK) {
		return;
	}

	/*
	 * THE DERIVE TABLE IS PART OF THE GATE. Building the params by hand would
	 * pass on a C3 that never chooses LUT1D at all -- which is precisely the
	 * state this milestone is changing.
	 */
	struct az_output_desc desc = {
		.bits_per_channel = 8,
		.hdr = false,
		.has_icc = true,
		.scene_ref_nits = 203.0f,
		.scanout_srgb_view_ok = true,
		.icc_shaper = &shaper,
	};
	struct az_output_color_state state = az_output_color_derive(&desc);
	CHECK(state.path == AZ_OUTPUT_PATH_B_ENCODE
			&& state.encode_tf == AZ_TF_LUT1D,
		"PREMISE: C3 puts a profiled SDR output on Path B with LUT1D "
		"(path=%d tf=%d)", (int)state.path, (int)state.encode_tf);
	if (state.encode_tf != AZ_TF_LUT1D) {
		return;
	}

	struct avk_renderer *fp = fp16_renderer(h);
	if (fp == NULL) {
		CHECK(false, "G2: FP16 renderer initialises");
		return;
	}

	struct avk_encode_intermediate work = {0};
	struct avk_image *inter = avk_encode_intermediate_get(&work, h->dev,
		&fp->retire, VK_FORMAT_R16G16B16A16_SFLOAT, W, H);
	if (inter == NULL) {
		CHECK(false, "G2: intermediate allocates");
		return;
	}

	/*
	 * The LUT, through the same call the compositor makes -- not a bespoke
	 * upload written for the test. A gate on an upload path the product does
	 * not use is a gate on nothing.
	 */
	struct avk_encode_lut lut = {0};
	struct avk_image *lut_img = avk_encode_lut_get(&lut, h->dev, &fp->ring,
		&fp->retire, shaper.curve, 1);
	CHECK(lut_img != NULL, "G2: the curve uploads as a 256-tap texture");
	if (lut_img == NULL) {
		avk_encode_intermediate_finish(&work, h->dev, &fp->retire);
		return;
	}
	/* Idempotent on an unchanged serial: this is what keeps a display
	 * characterisation off the frame path, and it is one line to assert. */
	CHECK(avk_encode_lut_get(&lut, h->dev, &fp->ring, &fp->retire,
			shaper.curve, 1) == lut_img,
		"G2: an unchanged serial re-uses the image rather than re-uploading");

	struct avk_encode_params params = {0};
	for (int i = 0; i < 9; i++) {
		params.matrix[i] = state.matrix[i];
	}
	params.knee = 1.0f;
	params.peak = state.peak_scene;
	params.anchor = state.ref_nits / 10000.0f;
	/* Dither OFF. ADR-011's noise is +-half a code by design and this gate's
	 * whole tolerance is one code. */
	params.dither_q = 0.0f;
	params.tf = AVK_ENCODE_TF_LUT1D;
	params.lut = lut_img;

	/*
	 * SCENE VALUES WITH THE CHANNELS APART. Saturated primaries put the
	 * matrix's off-diagonal terms in charge; the mixed triples cover the range
	 * where the curve is steepest. A neutral is included LAST and only so the
	 * printout shows how little it would have told us on its own.
	 */
	static const float vals[][3] = {
		{ 0.90f, 0.10f, 0.05f },
		{ 0.10f, 0.80f, 0.20f },
		{ 0.05f, 0.15f, 0.85f },
		{ 0.60f, 0.35f, 0.10f },
		{ 0.20f, 0.05f, 0.40f },
		{ 0.02f, 0.03f, 0.01f },
		{ 0.50f, 0.50f, 0.50f },
	};
	const int nv = (int)(sizeof(vals) / sizeof(vals[0]));

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;
	for (int i = 0; i < nv; i++) {
		struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_RECT);
		c->dst = (struct avk_box){ i * 8, 0, 8, 8 };
		for (int ch = 0; ch < 3; ch++) {
			c->color[ch] = vals[i][ch];
		}
		c->color[3] = 1.0f;
	}

	fp->encode_intermediate = inter;
	fp->encode_params = params;
	fp->encode_full_frame = true;
	bool ok = render_with(h, fp, &scene);
	fp->encode_intermediate = NULL;
	fp->encode_full_frame = false;
	avk_scene_finish(&scene);

	if (!ok) {
		CHECK(false, "G2: frame renders");
	} else {
		int worst = 0, worst_i = -1;
		/* THE PREMISE, measured on THESE patches rather than quoted from G1:
		 * how far the profile moves them away from a plain sRGB encode. A gate
		 * whose subject barely moves cannot fail for the right reason. */
		int premise = 0;
		for (int i = 0; i < nv; i++) {
			float lin[3] = { vals[i][0], vals[i][1], vals[i][2] };
			float want_f[3];
			az_icc_apply(&shaper, lin, want_f);
			uint32_t p = px(h, (uint32_t)(i * 8 + 4), 4);
			int got[3] = { r_of(p), g_of(p), b_of(p) };
			for (int ch = 0; ch < 3; ch++) {
				int want = (int)(want_f[ch] * 255.0f + 0.5f);
				int d = got[ch] - want;
				if (d < 0) { d = -d; }
				if (d > worst) { worst = d; worst_i = i; }
				/* sRGB encode of the same scene value: what this pixel would
				 * have been on an unprofiled output. */
				double v = (double)lin[ch];
				double srgb = v <= 0.0031308 ? v * 12.92
					: 1.055 * pow(v, 1.0 / 2.4) - 0.055;
				int plain = (int)(srgb * 255.0 + 0.5);
				int e = want - plain;
				if (e < 0) { e = -e; }
				if (e > premise) { premise = e; }
			}
			printf("  ---- scene %.2f/%.2f/%.2f -> want %3d/%3d/%3d  "
				"got %3d/%3d/%3d\n", (double)vals[i][0], (double)vals[i][1],
				(double)vals[i][2],
				(int)(want_f[0] * 255.0f + 0.5f),
				(int)(want_f[1] * 255.0f + 0.5f),
				(int)(want_f[2] * 255.0f + 0.5f),
				got[0], got[1], got[2]);
		}
		printf("  the profile moves these patches by up to %d codes away from "
			"a plain sRGB encode\n", premise);
		CHECK(premise > 10,
			"PREMISE: the profile is measurably non-identity ON THIS FIXTURE "
			"(%d codes)", premise);
		CHECK(worst <= 1,
			"G2: the LUT1D encode matches az_icc_apply (worst %d codes at "
			"patch %d)", worst, worst_i);
	}

	/*
	 * ── THE FALSIFIER: AZ_BREAK_ICC_LUT_IDENTITY ──────────────────────────
	 *
	 * Run in-process by re-uploading under the break, because that is the only
	 * way this file can observe its own falsifier rather than merely document
	 * one. Every stage still runs -- variant, descriptor, sample -- and only
	 * the table's contents are the identity, which is exactly the failure this
	 * gate exists to catch: the wiring intact and the characterisation gone.
	 *
	 * The delta must be LARGE. An identity table encodes a linear value to
	 * itself, so mid-grey leaves at 0.5 where the display's curve would have
	 * sent it past 0.7 -- tens of codes, not a tolerance.
	 */
	if (ok) {
		setenv("AZ_BREAK_ICC_LUT_IDENTITY", "1", 1);
		struct avk_encode_lut broke = {0};
		struct avk_image *broke_img = avk_encode_lut_get(&broke, h->dev,
			&fp->ring, &fp->retire, shaper.curve, 1);
		unsetenv("AZ_BREAK_ICC_LUT_IDENTITY");
		CHECK(broke_img != NULL, "BREAK: the identity table uploads");
		if (broke_img != NULL) {
			struct avk_scene sc;
			avk_scene_init(&sc);
			pixman_region32_union_rect(&sc.damage, &sc.damage, 0, 0, W, H);
			sc.has_clear = true;
			sc.clear_color[3] = 1.0f;
			for (int i = 0; i < nv; i++) {
				struct avk_cmd *c = avk_scene_add(&sc, AVK_CMD_RECT);
				c->dst = (struct avk_box){ i * 8, 0, 8, 8 };
				for (int ch = 0; ch < 3; ch++) {
					c->color[ch] = vals[i][ch];
				}
				c->color[3] = 1.0f;
			}
			params.lut = broke_img;
			fp->encode_intermediate = inter;
			fp->encode_params = params;
			fp->encode_full_frame = true;
			bool ok2 = render_with(h, fp, &sc);
			fp->encode_intermediate = NULL;
			fp->encode_full_frame = false;
			avk_scene_finish(&sc);

			int worst = 0;
			if (ok2) {
				for (int i = 0; i < nv; i++) {
					float lin[3] = { vals[i][0], vals[i][1], vals[i][2] };
					float want_f[3];
					az_icc_apply(&shaper, lin, want_f);
					uint32_t p = px(h, (uint32_t)(i * 8 + 4), 4);
					int got[3] = { r_of(p), g_of(p), b_of(p) };
					for (int ch = 0; ch < 3; ch++) {
						int d = got[ch] - (int)(want_f[ch] * 255.0f + 0.5f);
						if (d < 0) { d = -d; }
						if (d > worst) { worst = d; }
					}
				}
			}
			printf("  ---- identity table: worst %d codes from the measured "
				"curve\n", worst);
			CHECK(ok2 && worst > 20,
				"BREAK: an identity curve is DETECTED (%d codes)", worst);
			avk_encode_lut_finish(&broke, h->dev, &fp->retire);
		}
	}

	avk_encode_lut_finish(&lut, h->dev, &fp->retire);
	avk_encode_intermediate_finish(&work, h->dev, &fp->retire);
}

static void test_solid_colour_domain(struct harness *h) {
	printf("M5: what a solid rect's colour means on a linear path\n");

	struct avk_renderer *fp = fp16_renderer(h);
	if (fp == NULL) {
		CHECK(false, "solid: FP16 renderer initialises");
		return;
	}
	struct avk_encode_intermediate work = {0};
	struct avk_image *inter = avk_encode_intermediate_get(&work, h->dev,
		&fp->retire, VK_FORMAT_R16G16B16A16_SFLOAT, W, H);
	if (inter == NULL) {
		CHECK(false, "solid: intermediate allocates");
		return;
	}

	/* Three values across the midtones, where the two curves are furthest
	 * apart in absolute codes. */
	static const uint8_t want[] = { 64, 128, 192 };
	int got[3][2] = {{0, 0}, {0, 0}, {0, 0}};
	/* THE LINEAR ARM IS FED SCENE VALUES, exactly what the walk now produces.
	 * Feeding it the electrical code instead is the defect this exists for, and
	 * it is what the numbers in the comment above were measured with. */

	for (int arm = 0; arm < 2; arm++) {
		struct avk_renderer *r = arm == 0 ? &h->renderer : fp;
		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
		for (int i = 0; i < 3; i++) {
			struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_RECT);
			c->dst = (struct avk_box){ i * 16, 0, 16, 16 };
			float e = (float)want[i] / 255.0f;
			float v = arm == 1 ? az_srgb_eotf(e) : e;
			for (int ch = 0; ch < 3; ch++) {
				c->color[ch] = v;
			}
			c->color[3] = 1.0f;
		}
		if (arm == 1) {
			r->decode_enabled = true;
			r->encode_intermediate = inter;
			r->encode_params = sdr_encode_params();
			r->encode_full_frame = true;
		}
		bool ok = render_with(h, r, &scene);
		r->decode_enabled = false;
		r->encode_intermediate = NULL;
		r->encode_full_frame = false;
		avk_scene_finish(&scene);
		if (!ok) {
			CHECK(false, "solid: arm %d renders", arm);
			avk_encode_intermediate_finish(&work, h->dev, &fp->retire);
			return;
		}
		for (int i = 0; i < 3; i++) {
			got[i][arm] = b_of(px(h, (uint32_t)(i * 16 + 8), 8));
		}
	}

	int worst = 0;
	for (int i = 0; i < 3; i++) {
		int d = got[i][1] - got[i][0];
		if (d < 0) { d = -d; }
		if (d > worst) { worst = d; }
		printf("  ---- asked %3d  direct %3d  linear path %3d  (%+d)\n",
			want[i], got[i][0], got[i][1], got[i][1] - got[i][0]);
	}
	CHECK(got[0][0] == want[0] && got[1][0] == want[1]
			&& got[2][0] == want[2],
		"PREMISE: the direct path puts a rect's colour on screen unchanged");
	CHECK(worst <= 1,
		"a solid rect given as a SCENE value comes back at its own code "
		"(worst %d)", worst);
	/*
	 * THE FALSIFIER, and it is the whole reason this test is not vacuous:
	 * feeding the linear arm the ELECTRICAL value -- the pre-fix behaviour --
	 * must be badly wrong. Without it, "worst 0" is also what an encode pass
	 * that did nothing at all would report.
	 */
	{
		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;
		struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_RECT);
		c->dst = (struct avk_box){ 0, 0, 16, 16 };
		for (int ch = 0; ch < 3; ch++) {
			c->color[ch] = 128.0f / 255.0f;
		}
		c->color[3] = 1.0f;
		fp->decode_enabled = true;
		fp->encode_intermediate = inter;
		fp->encode_params = sdr_encode_params();
		bool ok = render_with(h, fp, &scene);
		fp->decode_enabled = false;
		fp->encode_intermediate = NULL;
		avk_scene_finish(&scene);
		int raw = ok ? b_of(px(h, 8, 8)) : -1;
		printf("  ---- undecoded 128 through the encode: %d\n", raw);
		CHECK(raw > 170,
			"FALSIFIER: an ELECTRICAL colour fed in as a scene value is "
			"visibly wrong (%d, asked 128)", raw);
	}

	avk_encode_intermediate_finish(&work, h->dev, &fp->retire);
}

/* ── 10-bit readback ─────────────────────────────────────────────────────── */

/* A2R10G10B10_UNORM_PACK32: one uint32 per pixel, alpha in the top two bits.
 * Separate accessors from the 8-bit ones rather than a scale factor, because a
 * 10-bit value silently divided by four is exactly the class of error a PQ
 * comparison would absorb into its tolerance. */
static int r10_of(uint32_t p) { return (int)((p >> 20) & 0x3FF); }
static int g10_of(uint32_t p) { return (int)((p >> 10) & 0x3FF); }
static int b10_of(uint32_t p) { return (int)(p & 0x3FF); }

/*
 * ADR-008's FALSIFIER, ON THE GPU.
 *
 * "PQ signal for the 203-nit patch must equal PQ-1(203/10000) +- 1/1023. A
 * miss means a luminance constant is on the wrong side of the matrix."
 *
 * The scene values are stated by RECT COMMANDS rather than decoded from a
 * texture, and that is the point: a rect's colour reaches the FP16 attachment
 * unmodified, so the input to the encode is known exactly and every code of
 * disagreement belongs to the pass. Decoding a source first would fold C7's
 * error into C6's measurement, and the two are separately falsifiable.
 *
 * The reference is az_ref_encode_scene() -- C4's CPU implementation of the same
 * six steps, written from the ADR rather than from this shader.
 */
static void test_path_b_pq_encode(struct harness *h) {
	printf("M5 PATH B: PQ encode against the C4 reference\n");

	struct avk_renderer *fp = fp16_renderer(h);
	if (fp == NULL) {
		CHECK(false, "pq: FP16 renderer initialises");
		return;
	}

	/*
	 * A 1000-nit panel at the 203-nit scene reference: peak_scene = 4.926, so
	 * SDR white sits at a fifth of the panel's ceiling. That is ADR-003's
	 * headroom and it is what makes the tone map's knee reachable by the
	 * values below rather than a branch nothing takes.
	 */
	struct az_output_desc desc = {
		.bits_per_channel = 10,
		.hdr = true,
		.hdr_max_nits = 1000.0f,
		.scene_ref_nits = 203.0f,
	};
	struct az_output_color_state state = az_output_color_derive(&desc);
	CHECK(state.path == AZ_OUTPUT_PATH_B_ENCODE && state.encode_tf == AZ_TF_PQ,
		"PREMISE: C3 puts a 1000-nit HDR output on Path B with PQ (path=%d tf=%d)",
		(int)state.path, (int)state.encode_tf);

	struct avk_encode_params params = {0};
	for (int i = 0; i < 9; i++) {
		params.matrix[i] = state.matrix[i];
	}
	params.knee = 1.0f;
	params.peak = state.peak_scene;
	params.anchor = state.ref_nits / 10000.0f;
	/* Dither OFF for the comparison. ADR-011's noise is +-half a code by
	 * design, which is the whole tolerance this test has. */
	params.dither_q = 0.0f;
	params.tf = AVK_ENCODE_TF_PQ;

	/* Scene values across the interesting range: below SDR white, AT it (the
	 * 203-nit patch the ADR names), through the knee, and at the panel's
	 * ceiling where the tone map's asymptote is. */
	static const float vals[] = { 0.05f, 0.25f, 1.0f, 2.0f, 4.0f, 4.926f };
	const int nv = (int)(sizeof(vals) / sizeof(vals[0]));

	struct avk_image *target = make_image_fmt(h->dev, W, H,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
		| VK_IMAGE_USAGE_SAMPLED_BIT, true, false,
		VK_FORMAT_A2R10G10B10_UNORM_PACK32);
	if (target == NULL) {
		CHECK(false, "pq: 10-bit target allocates");
		return;
	}
	struct avk_encode_intermediate work = {0};
	struct avk_image *inter = avk_encode_intermediate_get(&work, h->dev,
		&fp->retire, VK_FORMAT_R16G16B16A16_SFLOAT, W, H);
	if (inter == NULL) {
		CHECK(false, "pq: intermediate allocates");
		avk_image_destroy(h->dev, target);
		return;
	}

	struct avk_image *saved = h->target;
	h->target = target;

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;
	for (int i = 0; i < nv; i++) {
		struct avk_cmd *c = avk_scene_add(&scene, AVK_CMD_RECT);
		/* One 8-pixel column each, so a patch is sampled well away from any
		 * antialiased edge. */
		c->dst = (struct avk_box){ i * 8, 0, 8, 8 };
		c->color[0] = vals[i];
		c->color[1] = vals[i];
		c->color[2] = vals[i];
		c->color[3] = 1.0f;
	}

	fp->encode_intermediate = inter;
	fp->encode_params = params;
	fp->encode_full_frame = true;
	bool ok = render_with(h, fp, &scene);
	fp->encode_intermediate = NULL;
	fp->encode_full_frame = false;
	avk_scene_finish(&scene);

	h->target = saved;

	if (!ok) {
		CHECK(false, "pq: frame renders");
	} else {
		struct az_ref_output ref = { .state = state, .knee = 1.0, .dither = false };
		int worst = 0;
		int worst_i = -1;
		for (int i = 0; i < nv; i++) {
			double rgb[3] = { vals[i], vals[i], vals[i] };
			double e[3];
			az_ref_encode_scene(&ref, rgb, e);
			int want = (int)(e[0] * 1023.0 + 0.5);
			uint32_t p = px(h, (uint32_t)(i * 8 + 4), 4);
			int got = r10_of(p);
			int d = got - want; if (d < 0) { d = -d; }
			printf("  ---- scene %.3f -> want %4d  got %4d  (%+d)\n",
				(double)vals[i], want, got, got - want);
			if (d > worst) { worst = d; worst_i = i; }
			/* Every channel took the same route: a matrix applied to the wrong
			 * side, or a channel swap, shows here and nowhere else on a grey. */
			CHECK(g10_of(p) == got && b10_of(p) == got,
				"pq: scene %.3f is neutral out (%d/%d/%d)", (double)vals[i],
				got, g10_of(p), b10_of(p));
		}
		/*
		 * TWO CODES, not one, and the second is FP16.
		 *
		 * ADR-008 asks for +-1/1023 and the arithmetic here is exact to well
		 * within that; what is not exact is the STORE. The composited value
		 * makes a round trip through a half-float, whose relative precision is
		 * about 5e-4, and PQ's slope near black turns that into rather more
		 * than one 10-bit code. The extra code is the intermediate's format
		 * rather than the encode's arithmetic, which is why it is stated here
		 * instead of being folded into a vaguer bound.
		 */
		CHECK(worst <= 2,
			"ADR-008: PQ encode matches the CPU reference (worst %d codes at "
			"scene %.3f)", worst,
			worst_i >= 0 ? (double)vals[worst_i] : 0.0);
	}

	/*
	 * ── AND THE SAME THING IN COLOUR, WHICH IS A DIFFERENT TEST ────────────
	 *
	 * Every value above is NEUTRAL, and neutral CANNOT SEE THE GAMUT MATRIX:
	 * every row of BT.709->BT.2020 sums to 1, so grey is invariant under it.
	 * An absent matrix, a transposed one and the correct one all produce
	 * identical output for r=g=b -- the same blind spot that made the Path-B
	 * SDR falsifier read 0 codes until its ramp was given colour. Worth
	 * catching once rather than twice.
	 *
	 * Saturated values make the matrix the dominant term: BT.709 primary red
	 * lands at BT.2020 (0.6274, 0.0691, 0.0164), and the minor channels are
	 * where a wrong matrix shows first.
	 */
	if (ok) {
		static const float sats[][3] = {
			{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
			{1.0f, 1.0f, 0.0f}, {0.5f, 0.25f, 0.125f},
		};
		const int ns = (int)(sizeof(sats) / sizeof(sats[0]));
		struct avk_scene sc;
		avk_scene_init(&sc);
		pixman_region32_union_rect(&sc.damage, &sc.damage, 0, 0, W, H);
		sc.has_clear = true;
		sc.clear_color[3] = 1.0f;
		for (int i = 0; i < ns; i++) {
			struct avk_cmd *c = avk_scene_add(&sc, AVK_CMD_RECT);
			c->dst = (struct avk_box){ i * 12, 0, 12, 12 };
			for (int ch = 0; ch < 3; ch++) {
				c->color[ch] = sats[i][ch];
			}
			c->color[3] = 1.0f;
		}
		struct avk_image *keep = h->target;
		h->target = target;
		fp->encode_intermediate = inter;
		fp->encode_params = params;
		fp->encode_full_frame = true;
		bool ok2 = render_with(h, fp, &sc);
		fp->encode_intermediate = NULL;
		fp->encode_full_frame = false;
		avk_scene_finish(&sc);
		h->target = keep;

		struct az_ref_output ref2 = { .state = state, .knee = 1.0,
			.dither = false };
		int worst2 = 0;
		for (int i = 0; i < ns && ok2; i++) {
			double rgb[3] = { sats[i][0], sats[i][1], sats[i][2] };
			double e[3];
			az_ref_encode_scene(&ref2, rgb, e);
			uint32_t p = px(h, (uint32_t)(i * 12 + 6), 6);
			int got[3] = { r10_of(p), g10_of(p), b10_of(p) };
			int wnt[3];
			for (int c = 0; c < 3; c++) {
				wnt[c] = (int)(e[c] * 1023.0 + 0.5);
				int d = got[c] - wnt[c];
				if (d < 0) { d = -d; }
				if (d > worst2) { worst2 = d; }
			}
			printf("  ---- scene %.2f,%.2f,%.2f -> want %4d %4d %4d  "
				"got %4d %4d %4d\n", (double)sats[i][0], (double)sats[i][1],
				(double)sats[i][2], wnt[0], wnt[1], wnt[2],
				got[0], got[1], got[2]);
		}
		CHECK(ok2 && worst2 <= 2,
			"ADR-008: the GAMUT MATRIX matches the reference on SATURATED "
			"colour (worst %d codes)", worst2);
	}
	avk_encode_intermediate_finish(&work, h->dev, &fp->retire);
	avk_image_destroy(h->dev, target);
}

/* ── test 1: overlapping surfaces and alpha ─────────────────────────────── */

static void test_overlap_alpha(struct harness *h) {
	printf("test 1: background + opaque rect + 50%% alpha surface\n");

	/* A 50%-alpha white surface, PREMULTIPLIED as Wayland requires: the
	 * colour channels are already scaled by alpha, so 50% white is 0x80 in
	 * every channel, not 0xFF. Writing 0xFF here is the classic
	 * straight-vs-premultiplied mistake and it makes the result too bright --
	 * which the assertions below would catch. */
	uint32_t src[16 * 16];
	for (int i = 0; i < 16 * 16; i++) {
		src[i] = 0x80808080u;
	}
	struct avk_image *surface = make_image(h->dev, 16, 16,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
	if (surface == NULL || !upload(h, surface, src, 16, 16)) {
		CHECK(false, "prepare the translucent surface");
		return;
	}

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);

	/* blue background */
	scene.has_clear = true;
	scene.clear_color[0] = 0.0f;
	scene.clear_color[1] = 0.0f;
	scene.clear_color[2] = 1.0f;
	scene.clear_color[3] = 1.0f;

	/* opaque red rectangle */
	struct avk_cmd *rect = avk_scene_add(&scene, AVK_CMD_RECT);
	rect->dst = (struct avk_box){ 8, 8, 32, 32 };
	rect->color[0] = 1.0f;
	rect->color[1] = 0.0f;
	rect->color[2] = 0.0f;
	rect->color[3] = 1.0f;

	/* The translucent surface, straddling the rectangle's bottom-right
	 * corner: the rect covers 8..40, so a 16x16 surface at 32 is half over
	 * red and half over the blue background. Placing it entirely inside the
	 * rect would test one blend twice and call it two cases. */
	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 32, 32, 16, 16 };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 16, 16 };

	CHECK(render(h, &scene), "rendered");

	CHECK(r_of(px(h, 2, 2)) == 0 && b_of(px(h, 2, 2)) == 255,
		"background is blue (%d,%d,%d)", r_of(px(h, 2, 2)),
		g_of(px(h, 2, 2)), b_of(px(h, 2, 2)));
	CHECK(r_of(px(h, 12, 12)) == 255 && b_of(px(h, 12, 12)) == 0,
		"the rectangle is red (%d,%d,%d)", r_of(px(h, 12, 12)),
		g_of(px(h, 12, 12)), b_of(px(h, 12, 12)));

	/* 50% white over red: 0x80 + 255*(1-0.5) = 128 + 127 = 255 in red,
	 * 128 + 0 = 128 in green and blue. */
	uint32_t over_red = px(h, 34, 34);
	CHECK(near(r_of(over_red), 255, 1) && near(g_of(over_red), 128, 1)
			&& near(b_of(over_red), 128, 1),
		"50%% white over red is (255,128,128), got (%d,%d,%d)",
		r_of(over_red), g_of(over_red), b_of(over_red));

	/* 50% white over blue: (128, 128, 255). */
	uint32_t over_blue = px(h, 44, 44);
	CHECK(near(r_of(over_blue), 128, 1) && near(g_of(over_blue), 128, 1)
			&& near(b_of(over_blue), 255, 1),
		"50%% white over blue is (128,128,255), got (%d,%d,%d)",
		r_of(over_blue), g_of(over_blue), b_of(over_blue));

	avk_scene_finish(&scene);
	avk_image_destroy(h->dev, surface);
}

/* ── test 2: source crop and destination scale ──────────────────────────── */

static void test_crop_scale(struct harness *h) {
	printf("test 2: source crop + destination scale\n");

	/* Four quadrants, each a flat colour, so a crop can be checked by which
	 * colours survive and a scale by where the boundary lands. */
	uint32_t src[32 * 32];
	for (uint32_t y = 0; y < 32; y++) {
		for (uint32_t x = 0; x < 32; x++) {
			uint32_t c;
			if (x < 16 && y < 16)       c = 0xFFFF0000u;  /* TL red   */
			else if (x >= 16 && y < 16) c = 0xFF00FF00u;  /* TR green */
			else if (x < 16)            c = 0xFF0000FFu;  /* BL blue  */
			else                        c = 0xFFFFFF00u;  /* BR yellow*/
			src[y * 32 + x] = c;
		}
	}
	struct avk_image *surface = make_image(h->dev, 32, 32,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
	if (surface == NULL || !upload(h, surface, src, 32, 32)) {
		CHECK(false, "prepare the quadrant surface");
		return;
	}

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;   /* black */

	/* Crop the bottom-right quadrant (yellow) and blow it up to fill the
	 * whole target: every pixel must be yellow. A crop that ignored the
	 * source offset would show red; one that ignored the size would show all
	 * four colours. */
	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, W, H };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 16, 16, 16, 16 };
	tex->filter_linear = false;

	CHECK(render(h, &scene), "rendered a 16x16 crop scaled to 64x64");

	int wrong = 0;
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			uint32_t p = px(h, x, y);
			if (r_of(p) != 255 || g_of(p) != 255 || b_of(p) != 0) {
				wrong++;
			}
		}
	}
	CHECK(wrong == 0, "all %d pixels are the cropped yellow quadrant "
		"(%d wrong)", W * H, wrong);

	/* Now crop the top half and scale it into the top half only, so the
	 * boundary position is checked as well as the colours. */
	avk_scene_finish(&scene);
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;

	tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, W, 32 };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 32, 16 };

	CHECK(render(h, &scene), "rendered the top half");
	CHECK(r_of(px(h, 8, 8)) == 255 && g_of(px(h, 8, 8)) == 0,
		"top-left is red");
	CHECK(r_of(px(h, 56, 8)) == 0 && g_of(px(h, 56, 8)) == 255,
		"top-right is green");
	CHECK(r_of(px(h, 8, 40)) == 0 && g_of(px(h, 8, 40)) == 0
			&& b_of(px(h, 8, 40)) == 0,
		"below the destination box is untouched black");

	avk_scene_finish(&scene);
	avk_image_destroy(h->dev, surface);
}

/* ── rounded clipping: per corner, and owned by the destination ─────────── */

/*
 * M4A. Four DIFFERENT radii and a corner-coded source, so every mistake the
 * audit identified is separately visible:
 *
 *   one radius for all four   -> every corner cut the same, TL stops being square
 *   bottom corners swapped    -> BR and BL cut by each other's radius (19 vs 37)
 *   rounding in source space  -> a crop or a destination scale moves the arcs
 *   radii not permuted with a transform -> the right corner keeps the wrong arc
 *
 * The clear colour is opaque black, so "clipped" is measurable: a pixel inside
 * the arc carries the source quadrant's colour and one outside it is black.
 */
#define R_TL 0
#define R_TR 7
#define R_BR 19
#define R_BL 37

static bool clipped(struct harness *h, uint32_t x, uint32_t y) {
	uint32_t p = px(h, x, y);
	return r_of(p) == 0 && g_of(p) == 0 && b_of(p) == 0;
}

/* The deepest inset along a corner's diagonal that is still cut away. For a
 * quarter-disc of radius r the diagonal is cut for about r*(1 - 1/sqrt2)
 * pixels, so this rises and falls with r without having to model the AA. */
static int corner_cut_depth(struct harness *h, int cx, int cy, int dx, int dy) {
	int depth = 0;
	for (int i = 0; i < 48; i++) {
		int x = cx + (dx > 0 ? i : -i - 1);
		int y = cy + (dy > 0 ? i : -i - 1);
		if (x < 0 || y < 0 || x >= (int)W || y >= (int)H) {
			break;
		}
		if (!clipped(h, (uint32_t)x, (uint32_t)y)) {
			break;
		}
		depth = i + 1;
	}
	return depth;
}

static struct avk_image *quadrant_surface(struct harness *h) {
	uint32_t src[32 * 32];
	for (uint32_t y = 0; y < 32; y++) {
		for (uint32_t x = 0; x < 32; x++) {
			uint32_t c;
			if (x < 16 && y < 16)       c = 0xFFFF0000u;  /* TL red    */
			else if (x >= 16 && y < 16) c = 0xFF00FF00u;  /* TR green  */
			else if (x < 16)            c = 0xFF0000FFu;  /* BL blue   */
			else                        c = 0xFFFFFF00u;  /* BR yellow */
			src[y * 32 + x] = c;
		}
	}
	struct avk_image *s = make_image(h->dev, 32, 32,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
	if (s == NULL || !upload(h, s, src, 32, 32)) {
		return NULL;
	}
	return s;
}

static void scene_begin(struct avk_scene *scene) {
	avk_scene_init(scene);
	pixman_region32_union_rect(&scene->damage, &scene->damage, 0, 0, W, H);
	scene->has_clear = true;
	scene->clear_color[3] = 1.0f;   /* opaque black */
}

static void test_rounded_asymmetric(struct harness *h) {
	printf("test 6: per-corner rounded clipping\n");
	struct avk_image *surface = quadrant_surface(h);
	if (surface == NULL) {
		CHECK(false, "prepare the quadrant surface");
		return;
	}

	struct avk_scene scene;
	scene_begin(&scene);
	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, W, H };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 32, 32 };
	tex->corners[0] = R_TL; tex->corners[1] = R_TR;
	tex->corners[2] = R_BR; tex->corners[3] = R_BL;
	CHECK(render(h, &scene), "rendered four different radii");

	int d_tl = corner_cut_depth(h, 0, 0, 1, 1);
	int d_tr = corner_cut_depth(h, W, 0, -1, 1);
	int d_br = corner_cut_depth(h, W, H, -1, -1);
	int d_bl = corner_cut_depth(h, 0, H, 1, -1);
	printf("  cut depth  TL=%d TR=%d BR=%d BL=%d  (radii %d %d %d %d)\n",
		d_tl, d_tr, d_br, d_bl, R_TL, R_TR, R_BR, R_BL);

	CHECK(d_tl == 0, "TL radius 0 leaves the corner square (cut %d)", d_tl);
	CHECK(d_tr > 0, "TR radius 7 cuts the corner (%d)", d_tr);
	/* THE ordering assertion. Swapping the two bottom corners exchanges 19
	 * and 37, and nothing else in the suite would notice. */
	CHECK(d_br > d_tr, "BR radius 19 cuts deeper than TR's 7 (%d > %d)",
		d_br, d_tr);
	CHECK(d_bl > d_br, "BL radius 37 cuts deeper than BR's 19 (%d > %d)",
		d_bl, d_br);

	/* The surviving content at each corner must be that corner's own quadrant:
	 * a build that clipped correctly but sampled wrongly would pass the depth
	 * checks alone. */
	CHECK(r_of(px(h, 3, 3)) > 200 && g_of(px(h, 3, 3)) < 60,
		"the square TL corner still shows the red quadrant");
	CHECK(b_of(px(h, W - 4, H - 4 - d_br)) < 60
			&& r_of(px(h, W - 4, H - 4 - d_br)) > 150,
		"content under the BR arc is the yellow quadrant");

	avk_scene_finish(&scene);
	avk_image_destroy(h->dev, surface);
}

static void test_rounded_destination_space(struct harness *h) {
	printf("test 7: rounding follows the destination, not the source\n");
	struct avk_image *surface = quadrant_surface(h);
	if (surface == NULL) {
		CHECK(false, "prepare the quadrant surface");
		return;
	}

	/*
	 * Crop ONE quadrant and blow it up to the whole target. Every pixel that
	 * survives is yellow, so the arcs cannot be coming from the source's own
	 * corners -- the source crop has no corners of its own in the destination.
	 * If coverage were computed in source space the arcs would shrink by the
	 * 4x scale factor, or land in the wrong place entirely.
	 */
	struct avk_scene scene;
	scene_begin(&scene);
	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, W, H };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 16, 16, 16, 16 };   /* BR quadrant only */
	tex->corners[0] = R_TL; tex->corners[1] = R_TR;
	tex->corners[2] = R_BR; tex->corners[3] = R_BL;
	CHECK(render(h, &scene), "rendered a 16x16 crop upscaled 4x, rounded");

	int u_tr = corner_cut_depth(h, W, 0, -1, 1);
	int u_br = corner_cut_depth(h, W, H, -1, -1);
	int u_bl = corner_cut_depth(h, 0, H, 1, -1);
	printf("  upscaled crop cut depth  TR=%d BR=%d BL=%d\n", u_tr, u_br, u_bl);
	CHECK(u_bl > u_br && u_br > u_tr,
		"the destination's own radii still order the corners (%d > %d > %d)",
		u_bl, u_br, u_tr);
	CHECK(r_of(px(h, W / 2, H / 2)) > 200 && g_of(px(h, W / 2, H / 2)) > 200
			&& b_of(px(h, W / 2, H / 2)) < 60,
		"the interior is the cropped yellow quadrant");

	/* Now DOWNSCALE the whole source into a small destination. The radii are
	 * in destination pixels, so the arcs must not shrink with the content. */
	avk_scene_finish(&scene);
	scene_begin(&scene);
	tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, 48, 48 };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 32, 32 };
	tex->corners[0] = 0; tex->corners[1] = 4;
	tex->corners[2] = 16; tex->corners[3] = 0;
	CHECK(render(h, &scene), "rendered the source downscaled into 48x48");
	int s_tr = corner_cut_depth(h, 48, 0, -1, 1);
	int s_br = corner_cut_depth(h, 48, 48, -1, -1);
	printf("  downscaled cut depth  TR=%d BR=%d\n", s_tr, s_br);
	CHECK(s_br > s_tr, "the destination radii survive a downscale (%d > %d)",
		s_br, s_tr);
	CHECK(clipped(h, 60, 60), "nothing is drawn outside the 48x48 destination");

	avk_scene_finish(&scene);
	avk_image_destroy(h->dev, surface);
}

static void test_rounded_with_transform(struct harness *h) {
	printf("test 8: rounded clipping composed with a transform\n");
	struct avk_image *surface = quadrant_surface(h);
	if (surface == NULL) {
		CHECK(false, "prepare the quadrant surface");
		return;
	}

	/*
	 * The defect this catches: the texture transform is applied and the radii
	 * are not, so the right corner shows the right CONTENT with the wrong ARC.
	 * A symmetric-radius test cannot see it at all.
	 *
	 * The radii handed to the renderer are already in output space -- the
	 * compositor permutes them (az_avk_corners_from_scenefx) before they get
	 * here -- so what is asserted here is that the two are applied to the SAME
	 * corner, independently of which corner the compositor chose.
	 */
	struct avk_scene scene;
	scene_begin(&scene);
	struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
	tex->dst = (struct avk_box){ 0, 0, W, H };
	tex->image = surface;
	tex->src = (struct avk_fbox){ 0, 0, 32, 32 };
	tex->transform = AVK_TRANSFORM_90;
	tex->corners[0] = R_TL; tex->corners[1] = R_TR;
	tex->corners[2] = R_BR; tex->corners[3] = R_BL;
	CHECK(render(h, &scene), "rendered 90-degree transform with four radii");

	int t_tl = corner_cut_depth(h, 0, 0, 1, 1);
	int t_tr = corner_cut_depth(h, W, 0, -1, 1);
	int t_br = corner_cut_depth(h, W, H, -1, -1);
	int t_bl = corner_cut_depth(h, 0, H, 1, -1);
	printf("  transformed cut depth  TL=%d TR=%d BR=%d BL=%d\n",
		t_tl, t_tr, t_br, t_bl);
	CHECK(t_tl == 0 && t_bl > t_br && t_br > t_tr,
		"the arcs stay on the physical corners they were given");
	/* 90 degrees puts the source's GREEN (top-right) quadrant top-left -- the
	 * mapping wl_output_transform defines, and the one test 3 asserts. It was
	 * blue until M4F.2C.4e; see the table there. */
	CHECK(g_of(px(h, 4, 4)) > 200 && r_of(px(h, 4, 4)) < 60,
		"the transformed content is in place too (green at top-left)");

	avk_scene_finish(&scene);
	avk_image_destroy(h->dev, surface);
}

/* ── test 3: transforms ─────────────────────────────────────────────────── */

static void test_transforms(struct harness *h) {
	printf("test 3: all eight transforms\n");

	uint32_t src[32 * 32];
	for (uint32_t y = 0; y < 32; y++) {
		for (uint32_t x = 0; x < 32; x++) {
			uint32_t c;
			if (x < 16 && y < 16)       c = 0xFFFF0000u;  /* TL red    */
			else if (x >= 16 && y < 16) c = 0xFF00FF00u;  /* TR green  */
			else if (x < 16)            c = 0xFF0000FFu;  /* BL blue   */
			else                        c = 0xFFFFFF00u;  /* BR yellow */
			src[y * 32 + x] = c;
		}
	}
	struct avk_image *surface = make_image(h->dev, 32, 32,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
	if (surface == NULL || !upload(h, surface, src, 32, 32)) {
		CHECK(false, "prepare the quadrant surface");
		return;
	}

	/* Which source quadrant ends up in the destination's top-left, for each
	 * transform. Written out as the answer rather than derived, so the test
	 * disagrees with the implementation rather than restating it.
	 *   0 = red(TL) 1 = green(TR) 2 = blue(BL) 3 = yellow(BR) */
	static const struct {
		enum avk_transform transform;
		const char *name;
		int expect_tl;
	} cases[] = {
		/*
		 * 90 AND 270 CHANGED IN M4F.2C.4e, and the old values are worth
		 * recording: this table said 90 -> 2 (blue, the source's BOTTOM-LEFT)
		 * and 270 -> 1, which is the mapping the implementation had rather than
		 * the one wl_output_transform defines. wlroots resolves
		 * WL_OUTPUT_TRANSFORM_90 by putting the source's TOP-RIGHT quadrant at
		 * the destination's top-left. Every texture on a 90 or 270 degree
		 * output was therefore rotated 180 degrees inside its own box --
		 * 167400 of 480000 pixels of a real desktop -- and this test agreed
		 * with the defect instead of catching it, because its expectations had
		 * been read off the implementation.
		 *
		 * The six other transforms are involutions and were right.
		 */
		{ AVK_TRANSFORM_NORMAL,      "normal",      0 },
		{ AVK_TRANSFORM_90,          "90",          1 },
		{ AVK_TRANSFORM_180,         "180",         3 },
		{ AVK_TRANSFORM_270,         "270",         2 },
		{ AVK_TRANSFORM_FLIPPED,     "flipped",     1 },
		{ AVK_TRANSFORM_FLIPPED_90,  "flipped-90",  0 },
		{ AVK_TRANSFORM_FLIPPED_180, "flipped-180", 2 },
		{ AVK_TRANSFORM_FLIPPED_270, "flipped-270", 3 },
	};
	static const int quad_rgb[4][3] = {
		{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct avk_scene scene;
		avk_scene_init(&scene);
		pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
		scene.has_clear = true;
		scene.clear_color[3] = 1.0f;

		struct avk_cmd *tex = avk_scene_add(&scene, AVK_CMD_TEXTURE);
		tex->dst = (struct avk_box){ 0, 0, W, H };
		tex->image = surface;
		tex->src = (struct avk_fbox){ 0, 0, 32, 32 };
		tex->transform = cases[i].transform;

		if (!render(h, &scene)) {
			CHECK(false, "%s: rendered", cases[i].name);
			avk_scene_finish(&scene);
			continue;
		}

		uint32_t p = px(h, 8, 8);
		const int *want = quad_rgb[cases[i].expect_tl];
		CHECK(r_of(p) == want[0] && g_of(p) == want[1] && b_of(p) == want[2],
			"%s puts (%d,%d,%d) top-left, got (%d,%d,%d)", cases[i].name,
			want[0], want[1], want[2], r_of(p), g_of(p), b_of(p));

		avk_scene_finish(&scene);
	}

	avk_image_destroy(h->dev, surface);
}

/* ── test 4: damage ─────────────────────────────────────────────────────── */

static void test_damage(struct harness *h) {
	printf("test 4: damage restricts what is redrawn\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[2] = 1.0f;   /* blue */
	scene.clear_color[3] = 1.0f;
	CHECK(render(h, &scene), "frame A: full-damage blue");
	CHECK(b_of(px(h, 32, 32)) == 255, "frame A is blue");
	avk_scene_finish(&scene);

	/*
	 * Frame B asks to paint the WHOLE target red, but damages only a 16x16
	 * box. Everything outside that box must still be frame A's blue.
	 *
	 * This is the assertion that fails if damage is ignored -- and it fails
	 * loudly, because ignoring damage turns the whole target red rather than
	 * producing a subtle artefact.
	 */
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 20, 20, 16, 16);
	scene.has_clear = true;
	scene.clear_color[0] = 1.0f;
	scene.clear_color[3] = 1.0f;
	CHECK(render(h, &scene), "frame B: red, damaged to a 16x16 box");

	CHECK(r_of(px(h, 28, 28)) == 255 && b_of(px(h, 28, 28)) == 0,
		"inside the damage is the new red");
	CHECK(b_of(px(h, 4, 4)) == 255 && r_of(px(h, 4, 4)) == 0,
		"outside the damage the old blue survives");
	CHECK(b_of(px(h, 60, 60)) == 255, "the far corner survives too");
	CHECK(r_of(px(h, 19, 28)) == 0,
		"the pixel just left of the damage box is untouched");
	CHECK(r_of(px(h, 20, 28)) == 255,
		"the first pixel inside the damage box IS touched");

	avk_scene_finish(&scene);
}

/* ── test 5: clipping to a visible region ───────────────────────────────── */

static void test_clip(struct harness *h) {
	printf("test 5: a command's visible region clips it\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;   /* black */

	/* A full-target green rectangle that is only visible in its left half --
	 * exactly what occlusion produces when another window covers the right
	 * half of this one. */
	struct avk_cmd *rect = avk_scene_add(&scene, AVK_CMD_RECT);
	rect->dst = (struct avk_box){ 0, 0, W, H };
	rect->color[1] = 1.0f;
	rect->color[3] = 1.0f;

	pixman_region32_t visible;
	pixman_region32_init_rect(&visible, 0, 0, 32, H);
	avk_cmd_set_clip(rect, &visible);
	pixman_region32_fini(&visible);

	CHECK(render(h, &scene), "rendered a clipped rectangle");
	CHECK(g_of(px(h, 8, 32)) == 255, "the visible half is green");
	CHECK(g_of(px(h, 48, 32)) == 0, "the clipped half is untouched black");
	CHECK(g_of(px(h, 31, 32)) == 255, "the last visible column is drawn");
	CHECK(g_of(px(h, 32, 32)) == 0, "the first clipped column is not");

	avk_scene_finish(&scene);
}

/* ── test 6: opacity ────────────────────────────────────────────────────── */

static void test_opacity(struct harness *h) {
	printf("test 6: per-command opacity\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;   /* black */

	struct avk_cmd *rect = avk_scene_add(&scene, AVK_CMD_RECT);
	rect->dst = (struct avk_box){ 0, 0, W, H };
	rect->color[0] = 1.0f;
	rect->color[3] = 1.0f;
	rect->opacity = 0.5f;

	CHECK(render(h, &scene), "rendered a 50%% opaque red over black");
	uint32_t p = px(h, 32, 32);
	CHECK(near(r_of(p), 128, 1) && g_of(p) == 0 && b_of(p) == 0,
		"50%% red over black is (128,0,0), got (%d,%d,%d)", r_of(p), g_of(p),
		b_of(p));

	avk_scene_finish(&scene);

	/*
	 * A rect whose COLOUR carries the alpha, rather than opacity carrying it.
	 *
	 * This case exists because of a break test: removing AVK's premultiply
	 * changed nothing in any other assertion here, since every other rect has
	 * alpha 1.0 and `c * 1.0` is `c`. A renderer that forgot to premultiply
	 * would have passed the whole file. Here it cannot: unpremultiplied, the
	 * shader emits (1,0,0,0.5) and the blend leaves 255; premultiplied it
	 * emits (0.5,0,0,0.5) and leaves 128.
	 */
	avk_scene_init(&scene);
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, W, H);
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;   /* black */

	struct avk_cmd *translucent = avk_scene_add(&scene, AVK_CMD_RECT);
	translucent->dst = (struct avk_box){ 0, 0, W, H };
	translucent->color[0] = 1.0f;
	translucent->color[3] = 0.5f;   /* straight alpha, AVK premultiplies */

	CHECK(render(h, &scene), "rendered a rect with 50%% colour alpha");
	p = px(h, 32, 32);
	CHECK(near(r_of(p), 128, 1),
		"a half-alpha red rect over black is 128, not 255 (got %d) -- this is "
		"the premultiply", r_of(p));

	avk_scene_finish(&scene);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
	printf("== avk composition (M3) ==\n");

	int drm_fd = -1;
	for (int i = 128; i < 192 && drm_fd < 0; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
	}
	if (drm_fd < 0) {
		SKIP("no DRM render node");
	}

	struct harness h = {0};
	h.inst = avk_instance_create("test-avk-render");
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
	h.target = make_image(h.dev, W, H,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
		| VK_IMAGE_USAGE_SAMPLED_BIT, true);
	if (h.target == NULL) {
		CHECK(false, "output target allocates");
		goto done;
	}

	test_sdr_roundtrip_gate(&h);
	test_srgb_view_refusal(&h);
	test_decode_variant(&h);
	test_path_a_roundtrip(&h);
	test_path_b_sdr_gate(&h);
	test_path_b_pq_encode(&h);
	test_path_b_lut_encode(&h);
	test_solid_colour_domain(&h);
	test_c7_decode_path(&h);
	test_overlap_alpha(&h);
	test_crop_scale(&h);
	test_transforms(&h);
	test_damage(&h);
	test_rounded_asymmetric(&h);
	test_rounded_destination_space(&h);
	test_rounded_with_transform(&h);
	test_clip(&h);
	test_opacity(&h);

	/* The claim the whole architecture rests on, stated as a number. */
	printf("instrumentation\n");
	avk_renderer_log_stats(&h.renderer);
	CHECK(h.renderer.stats.frames > 0, "%" PRIu64 " frames composited by AVK",
		h.renderer.stats.frames);

done:
	if (h.target != NULL) {
		avk_image_destroy(h.dev, h.target);
	}
	avk_device_wait_idle(h.dev);
	if (h.fp16_ok) {
		avk_renderer_finish(&h.fp16);
	}
	avk_renderer_finish(&h.renderer);
	avk_device_destroy(h.dev);
	avk_instance_destroy(h.inst);
	close(drm_fd);

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
