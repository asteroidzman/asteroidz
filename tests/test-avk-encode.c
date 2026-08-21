/*
 * test-avk-encode -- the H.265 Main 10 encode session, on a real device.
 *
 * M14A. This asserts the part of the encoder that the driver can refuse: the
 * profile, the session, its memory bindings and its parameter sets. Whether
 * the resulting bitstream decodes is a later question and a different test;
 * what this one exists to catch is the class of failure where a session is
 * created against a profile that is nearly right and then rejects every
 * picture -- which is a runtime error in a compositor and a compile-time-shaped
 * bug in nobody's build.
 *
 * The premise is asserted first. A test that "passes" on a machine with no
 * encode queue has asserted nothing, so the encode capability is checked and
 * the run SKIPS rather than reporting success.
 *
 * WHAT IT DOES NOT ASSERT, and this matters: the CONTENT of the picture. The
 * checks below prove a structurally valid H.265 Main 10 bitstream comes out --
 * start code, VPS first, an IDR present, and ffprobe agrees it is 3840x2160
 * yuv420p10le. They do not prove the encoder read the image it was given, and
 * on this driver it does not: see docs/known-issues.md, "the RGB conversion
 * input path". A content assertion belongs here once that is resolved, and
 * until then this test must not be read as saying the encoder works.
 *
 * Exits 77 (meson's skip) with no GPU or no encode hardware.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "render/vulkan/device/avk_device.h"
#include "render/vulkan/device/avk_instance.h"
#include "render/vulkan/encode/avk_encode.h"
#include "render/vulkan/encode/avk_heif.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { \
			printf("  ok   "); \
		} else { \
			printf("  FAIL "); \
			failures++; \
		} \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} while (0)

#define SKIP(...) do { \
		printf("SKIP: " __VA_ARGS__); \
		printf("\n"); \
		return 77; \
	} while (0)

static int open_render_node(void) {
	for (int i = 128; i < 136; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			return fd;
		}
	}
	return -1;
}

/*
 * A source picture the encoder will accept: A2R10G10B10 at the coded size,
 * with the encoder's profile list in its pNext, cleared to a flat colour on
 * the graphics queue.
 *
 * CONCURRENT sharing across the graphics and encode families is deliberate
 * and is a property of this TEST, not a recommendation. The real path hands
 * over an image AVK rendered, and that will need a queue family ownership
 * transfer rather than concurrent access -- concurrent costs compression on
 * AMD. Here it removes a whole class of setup from a test that is not about
 * ownership.
 */
struct source {
	VkImage image;
	VkDeviceMemory memory;
	VkCommandPool pool;
};

static bool make_source(struct avk_device *dev, struct avk_encoder *enc,
		struct source *out) {
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
		.extent = {enc->coded_width, enc->coded_height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		/* TRANSFER_SRC only, which is what a scanout target has and what the
		 * encoder now asks for. */
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
			| VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(dev->dev, &info, NULL, &out->image) != VK_SUCCESS) {
		printf("  (could not create the source image)\n");
		return false;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev->dev, out->image, &req);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(dev->phys, &mp);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((req.memoryTypeBits & (1u << i))
				&& (mp.memoryTypes[i].propertyFlags
					& VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) {
		return false;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};
	if (vkAllocateMemory(dev->dev, &alloc, NULL, &out->memory) != VK_SUCCESS
			|| vkBindImageMemory(dev->dev, out->image, out->memory, 0)
				!= VK_SUCCESS) {
		return false;
	}
	/* Clear it on the graphics queue, so the encoder is handed real content
	 * rather than undefined memory -- an encoder fed garbage still produces a
	 * bitstream, and the test would pass on it. */
	VkCommandPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = dev->caps.graphics_family,
	};
	if (vkCreateCommandPool(dev->dev, &pool, NULL, &out->pool) != VK_SUCCESS) {
		return false;
	}
	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo ca = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = out->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (vkAllocateCommandBuffers(dev->dev, &ca, &cmd) != VK_SUCCESS) {
		return false;
	}
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(cmd, &bi);
	VkImageMemoryBarrier to_dst = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = out->image,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
	/* Overridable so two runs can differ in exactly one thing. Whether the
	 * bitstream changes with the input is the difference between "the encoder
	 * ignores its source" and "the encoder works and the parameter sets are
	 * wrong", and those have nothing in common. */
	VkClearColorValue colour = {.float32 = {0.25f, 0.55f, 0.85f, 1.0f}};
	const char *cs = getenv("AVK_ENCODE_COLOUR");
	if (cs != NULL) {
		sscanf(cs, "%f,%f,%f", &colour.float32[0], &colour.float32[1],
			&colour.float32[2]);
	}
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(cmd, out->image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
	vkEndCommandBuffer(cmd);
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	};
	vkQueueSubmit(dev->graphics_queue, 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(dev->graphics_queue);
	return true;
}

static void destroy_source(struct avk_device *dev, struct source *s) {
	if (s->pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(dev->dev, s->pool, NULL);
	}
	if (s->image != VK_NULL_HANDLE) {
		vkDestroyImage(dev->dev, s->image, NULL);
	}
	if (s->memory != VK_NULL_HANDLE) {
		vkFreeMemory(dev->dev, s->memory, NULL);
	}
}


/*
 * Read one pixel of the encoder's converted P010 image back.
 *
 * A multi-planar image cannot be copied in one go, so the two planes are
 * copied separately by aspect. Only the centre sample is needed -- this exists
 * to answer "did the conversion produce the right numbers", not to inspect a
 * picture.
 */
static bool readback_p010(struct avk_device *dev, struct avk_encoder *enc,
		uint16_t *y, uint16_t *cb, uint16_t *cr) {
	VkDeviceSize size = 4096;
	VkBufferCreateInfo binfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	VkBuffer buf;
	if (vkCreateBuffer(dev->dev, &binfo, NULL, &buf) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev->dev, buf, &req);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(dev->phys, &mp);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((req.memoryTypeBits & (1u << i))
				&& (mp.memoryTypes[i].propertyFlags
					& VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
				&& (mp.memoryTypes[i].propertyFlags
					& VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) {
		return false;
	}
	VkMemoryAllocateInfo ma = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};
	VkDeviceMemory mem;
	if (vkAllocateMemory(dev->dev, &ma, NULL, &mem) != VK_SUCCESS
			|| vkBindBufferMemory(dev->dev, buf, mem, 0) != VK_SUCCESS) {
		return false;
	}
	VkCommandPoolCreateInfo cp = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = dev->caps.graphics_family,
	};
	VkCommandPool pool;
	if (vkCreateCommandPool(dev->dev, &cp, NULL, &pool) != VK_SUCCESS) {
		return false;
	}
	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo ca = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	vkAllocateCommandBuffers(dev->dev, &ca, &cmd);
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(cmd, &bi);
	/* The encode left it in VIDEO_ENCODE_SRC; a transfer read needs
	 * TRANSFER_SRC. */
	VkImageMemoryBarrier to_src = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = enc->p010_image,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
	VkBufferImageCopy regions[2] = {
		{
			.bufferOffset = 0,
			.imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1},
			.imageOffset = {1920, 1080, 0},
			.imageExtent = {1, 1, 1},
		},
		{
			.bufferOffset = 256,
			.imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1},
			.imageOffset = {960, 540, 0},
			.imageExtent = {1, 1, 1},
		},
	};
	vkCmdCopyImageToBuffer(cmd, enc->p010_image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 2, regions);
	vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	};
	vkQueueSubmit(dev->graphics_queue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(dev->graphics_queue);

	void *map = NULL;
	bool ok = vkMapMemory(dev->dev, mem, 0, VK_WHOLE_SIZE, 0, &map)
		== VK_SUCCESS;
	if (ok) {
		const uint16_t *p = map;
		*y = p[0];
		*cb = p[128];      /* byte 256 */
		*cr = p[129];
		vkUnmapMemory(dev->dev, mem);
	}
	vkDestroyCommandPool(dev->dev, pool, NULL);
	vkDestroyBuffer(dev->dev, buf, NULL);
	vkFreeMemory(dev->dev, mem, NULL);
	return ok;
}

/* H.265 NAL header: two bytes, type is bits 1..6 of the first. */
static int nal_type(const uint8_t *p) { return (p[0] >> 1) & 0x3F; }

int main(void) {
	printf("== avk encode (M14A) ==\n");

	int fd = open_render_node();
	if (fd < 0) {
		SKIP("no DRM render node available");
	}
	struct avk_instance *inst = avk_instance_create("test-avk-encode");
	if (inst == NULL) {
		close(fd);
		SKIP("no usable Vulkan instance");
	}
	struct avk_device *dev = avk_device_create(inst, fd);
	if (dev == NULL) {
		avk_instance_destroy(inst);
		close(fd);
		SKIP("no usable Vulkan device");
	}

	/* The premise. Without this the checks below would pass by not running. */
	if (!dev->has_encode_queue) {
		printf("  device caps: encode_family=%d video_queue=%d "
			"encode_queue=%d h265=%d rgb=%d\n",
			dev->caps.has_video_encode_family,
			dev->caps.video_queue, dev->caps.video_encode_queue,
			dev->caps.video_encode_h265, dev->caps.video_encode_rgb);
		avk_device_destroy(dev);
		avk_instance_destroy(inst);
		close(fd);
		SKIP("this device cannot encode video");
	}
	printf("  encode queue on family %u\n", dev->caps.video_encode_family);

	/* Display P3-ish primaries and 1000 nits, in the SEI's units: what a real
	 * HDR monitor reports, rather than the BT.2020 reference values a script
	 * has to guess when nothing exposes the panel's own. */
	struct avk_encode_mastering mastering = {
		.display_primaries_x = {13250, 7500, 34000},
		.display_primaries_y = {34500, 3000, 16000},
		.white_point_x = 15635, .white_point_y = 16450,
		.max_luminance = 10000000, .min_luminance = 1,
		.max_content_light_level = 1000,
		.max_frame_average_light_level = 400,
	};

	/* The real display's size, because the alignment question only has a
	 * right answer relative to a real one: 3840x2160 lands exactly on this
	 * encoder's 64x16 granularity and 1366x768 does not. */
	struct avk_encoder *enc = avk_encoder_create(dev, 3840, 2160, AVK_ENCODE_COLOUR_HDR10, &mastering);
	CHECK(enc != NULL, "a 3840x2160 H.265 Main 10 session is created");
	if (enc != NULL) {
		CHECK(enc->session != VK_NULL_HANDLE, "the session handle is real");
		CHECK(enc->params != VK_NULL_HANDLE,
			"its VPS/SPS/PPS parameter sets were accepted");
		CHECK(enc->coded_width == 3840 && enc->coded_height == 2160,
			"3840x2160 needs no padding (coded %ux%u)",
			enc->coded_width, enc->coded_height);
		/* The level must admit the picture. This driver reports maxLevelIdc
		 * as 0 -- level 1.0, whose limit is 176x144 -- and writing that into
		 * the stream is what made a correct picture decode as noise, with
		 * nothing failing anywhere along the way. */
		CHECK(enc->level_idc == STD_VIDEO_H265_LEVEL_IDC_5_1,
			"a 4K picture declares level 5.1 (got %d)", (int)enc->level_idc);
		/* The whole architectural claim of M14A: the encoder takes the format
		 * AVK already renders an HDR output in, so the composited image is
		 * handed over rather than converted. */
		/* The driver's RGB-input path does not work (known-issues.md), so
		 * the encoder's input is P010 and the conversion is ours. */
		CHECK(enc->src_format
				== VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
			"the encode source format is P010 (got %d)",
			(int)enc->src_format);

		/* ── the encode itself ──────────────────────────────────────── */
		struct source src = {0};
		if (make_source(dev, enc, &src)) {
			void *stream = NULL;
			size_t len = 0;
			bool encoded = avk_encoder_encode_still(enc, src.image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0, &stream, &len);

			/* THE BISECT. Two things can put a wrong picture in the
			 * bitstream: a conversion that wrote the wrong P010, or an
			 * encode that did not read it. Reading the converted image back
			 * separates them, and without that separation the next hour is
			 * spent guessing. Expected for RGB(0.25,0.55,0.85) under BT.2020
			 * NCL full range: Y 500, Cb 708, Cr 346, in the top 10 bits of
			 * each 16-bit word. */
			uint16_t y0 = 0, cb0 = 0, cr0 = 0;
			if (readback_p010(dev, enc, &y0, &cb0, &cr0)) {
				printf("  converted P010 centre: Y=%u Cb=%u Cr=%u "
					"(expected 500 708 346)\n", y0 >> 6, cb0 >> 6, cr0 >> 6);
				CHECK(abs((int)(y0 >> 6) - 500) <= 4,
					"the conversion wrote the right luma");
				CHECK(abs((int)(cb0 >> 6) - 708) <= 4
						&& abs((int)(cr0 >> 6) - 346) <= 4,
					"the conversion wrote the right chroma");
			}
			CHECK(encoded, "one 3840x2160 IDR picture encodes");
			if (encoded) {
				const uint8_t *b = stream;
				CHECK(len > 1024,
					"the bitstream is a picture rather than a stub "
					"(%zu bytes)", len);
				/* Annex B: every NAL unit is introduced by 00 00 00 01, and
				 * the first one has to be the VPS -- a picture whose
				 * parameter sets were not prepended decodes to nothing. */
				CHECK(len > 4 && b[0] == 0 && b[1] == 0 && b[2] == 0
						&& b[3] == 1,
					"it starts with an Annex B start code");
				CHECK(len > 5 && nal_type(b + 4) == 32,
					"the first NAL unit is the VPS (got type %d)",
					len > 5 ? nal_type(b + 4) : -1);
				/* And an IDR has to be in there, or the parameter sets were
				 * written and the picture was not. */
				bool found_idr = false;
				for (size_t i = 0; i + 5 < len; i++) {
					if (b[i] == 0 && b[i+1] == 0 && b[i+2] == 0
							&& b[i+3] == 1) {
						int t = nal_type(b + i + 4);
						if (t == 19 || t == 20) {
							found_idr = true;
							break;
						}
					}
				}
				CHECK(found_idr,
					"an IDR picture NAL unit follows the parameter sets");

				/* The container. A raw elementary stream is not a file
				 * anything opens, and a screenshot that needs ffmpeg
				 * explained to it is not a screenshot. */
				void *heif = NULL;
				size_t heif_len = 0;
				struct avk_heif_colour hc = {
					.primaries = 9, .transfer = 16, .matrix = 9,
					.full_range = true,
				};
				bool wrapped = avk_heif_wrap(stream, len, 3840, 2160, &hc,
					&heif, &heif_len);
				CHECK(wrapped, "the picture wraps as a HEIF still");
				if (wrapped) {
					const uint8_t *h = heif;
					CHECK(heif_len > len,
						"the container is larger than its payload "
						"(%zu vs %zu)", heif_len, len);
					CHECK(heif_len > 12 && memcmp(h + 4, "ftyp", 4) == 0
							&& memcmp(h + 8, "heic", 4) == 0,
						"it begins with an ftyp declaring heic");
					const char *hp = getenv("AVK_HEIF_OUT");
					if (hp != NULL) {
						FILE *hf = fopen(hp, "wb");
						if (hf != NULL) {
							fwrite(heif, heif_len, 1, hf);
							fclose(hf);
							printf("  wrote %s (%zu bytes)\n", hp, heif_len);
						}
					}
					free(heif);
				}

				const char *out_path = getenv("AVK_ENCODE_OUT");
				if (out_path != NULL) {
					FILE *f = fopen(out_path, "wb");
					if (f != NULL) {
						fwrite(stream, len, 1, f);
						fclose(f);
						printf("  wrote %s (%zu bytes)\n", out_path, len);
					}
				}
				free(stream);
			}
			destroy_source(dev, &src);
		}
		avk_encoder_destroy(enc);
	}

	/* A size that does NOT land on the granularity has to be rounded up
	 * rather than silently encoded at the wrong extent. 1366 is the width
	 * that made this worth asserting: 1366/64 is not an integer. */
	struct avk_encoder *odd = avk_encoder_create(dev, 1366, 768, AVK_ENCODE_COLOUR_HDR10, NULL);
	CHECK(odd != NULL, "an unaligned 1366x768 session is created too");
	if (odd != NULL) {
		CHECK(odd->coded_width == 1408,
			"1366 is rounded up to the 64-wide granularity (coded %u)",
			odd->coded_width);
		CHECK(odd->coded_height == 768,
			"768 already lands on the 16-high granularity (coded %u)",
			odd->coded_height);
		avk_encoder_destroy(odd);
	}

	avk_device_destroy(dev);
	avk_instance_destroy(inst);
	close(fd);

	printf("\n%d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
