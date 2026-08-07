// Dump a live blur's SOURCE image to disk, so what the blur is about to spread
// can be looked at instead of reasoned about.
//
// A backdrop blur's source is the scene image so far, copied into a scratch
// effect image and then patched by blur_exclude_from_source() to keep the
// window's own pixels out of its shadow. Every question about the halo around a
// floating window -- is the hole filled, with what, does the fill reach far
// enough -- is a question about the contents of that scratch image at that
// moment, and nothing downstream of it can answer them: by the time anything is
// on screen the blur has already averaged the evidence away.
//
// Off unless FX_BLUR_DUMP is set, and it stops after FX_BLUR_DUMP_FRAMES
// flushes. Both matter: the readback is a full device stall per frame, so a
// forgotten variable would otherwise turn a live session into a slideshow.
//
//   FX_BLUR_DUMP=/tmp/blur FX_BLUR_DUMP_FRAMES=3 asteroidz
//
// writes /tmp/blur-<n>-<tag>.pam plus a sidecar .txt naming the boxes, in
// screen coordinates, that the .pam is a crop of.

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#include <scenefx/render/fx_renderer/fx_vk_renderer.h>

#include "render/vulkan.h"

// One per capture site per frame. Two are in use (before and after the hole is
// patched); the spares cost nothing until a tag asks for one.
#define BLUR_DUMP_SLOTS 4

struct blur_dump_slot {
	char tag[32];
	VkImage image;
	VkDeviceMemory memory;
	int width, height;
	// Where the crop sits on screen, so the sidecar can say so.
	struct wlr_box region, exclude;
	bool pending;
};

static struct {
	bool checked;
	const char *prefix;
	// Owns the prefix when it came from fx_vk_blur_debug_arm(); the env var's
	// string belongs to the environment and is left alone.
	char *owned_prefix;
	int budget;
	unsigned seq;
	struct blur_dump_slot slots[BLUR_DUMP_SLOTS];
} dump;

void fx_vk_blur_debug_arm(const char *prefix, int frames) {
	dump.checked = true;
	free(dump.owned_prefix);
	dump.owned_prefix = NULL;
	dump.prefix = NULL;
	if (prefix == NULL || prefix[0] == '\0') {
		wlr_log(WLR_INFO, "fx_vk blur dump: disarmed");
		return;
	}
	dump.owned_prefix = strdup(prefix);
	dump.prefix = dump.owned_prefix;
	dump.budget = frames > 0 ? frames : 1;
	// Fresh numbering per arming, so a second look does not overwrite the
	// first one's files and leave two runs interleaved in one directory.
	dump.seq = 0;
	wlr_log(WLR_INFO, "fx_vk blur dump: armed at %s for %d frames",
		dump.prefix, dump.budget);
}

bool fx_vk_blur_debug_enabled(void) {
	if (!dump.checked) {
		dump.checked = true;
		const char *prefix = getenv("FX_BLUR_DUMP");
		if (prefix != NULL && prefix[0] != '\0') {
			dump.prefix = prefix;
		}
		const char *frames = getenv("FX_BLUR_DUMP_FRAMES");
		dump.budget = frames != NULL ? atoi(frames) : 4;
		if (dump.budget < 1) {
			dump.budget = 1;
		}
		if (dump.prefix != NULL) {
			wlr_log(WLR_INFO, "fx_vk blur dump: %s, %d frames",
				dump.prefix, dump.budget);
		}
	}
	return dump.prefix != NULL && dump.budget > 0;
}

static struct blur_dump_slot *slot_for(const char *tag) {
	struct blur_dump_slot *free_slot = NULL;
	for (size_t i = 0; i < BLUR_DUMP_SLOTS; i++) {
		struct blur_dump_slot *s = &dump.slots[i];
		if (s->tag[0] != '\0' && strcmp(s->tag, tag) == 0) {
			return s;
		}
		if (free_slot == NULL && s->tag[0] == '\0') {
			free_slot = s;
		}
	}
	if (free_slot != NULL) {
		snprintf(free_slot->tag, sizeof(free_slot->tag), "%s", tag);
	}
	return free_slot;
}

// A linear, host-visible image the size of the crop. Recreated when the crop
// changes size, which in practice happens once: the blur region follows the
// window, and a window that is moving is not what anybody is inspecting.
static bool slot_realloc(struct fx_vk_renderer *renderer,
		struct blur_dump_slot *slot, int width, int height) {
	if (slot->image != VK_NULL_HANDLE &&
			slot->width == width && slot->height == height) {
		return true;
	}
	VkDevice dev = renderer->dev->dev;
	if (slot->image != VK_NULL_HANDLE) {
		vkFreeMemory(dev, slot->memory, NULL);
		vkDestroyImage(dev, slot->image, NULL);
		slot->image = VK_NULL_HANDLE;
		slot->memory = VK_NULL_HANDLE;
	}

	// 8-bit, because the blit converts for free and this is for looking at.
	// The effect images are 16F, so a value above 1.0 clamps to white here --
	// fine on an SDR capture, and worth remembering before reading an HDR one.
	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { .width = width, .height = height, .depth = 1 },
		.arrayLayers = 1,
		.mipLevels = 1,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_LINEAR,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	};
	VkResult res = vkCreateImage(dev, &image_info, NULL, &slot->image);
	if (res != VK_SUCCESS) {
		fx_vk_error("vkCreateImage", res);
		slot->image = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(dev, slot->image, &reqs);
	int mem_type = fx_vulkan_find_mem_type(renderer->dev,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
		reqs.memoryTypeBits);
	if (mem_type < 0) {
		wlr_log(WLR_ERROR, "fx_vk blur dump: no host-visible memory type");
		goto destroy;
	}
	VkMemoryAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = mem_type,
	};
	res = vkAllocateMemory(dev, &alloc_info, NULL, &slot->memory);
	if (res != VK_SUCCESS) {
		fx_vk_error("vkAllocateMemory", res);
		goto destroy;
	}
	res = vkBindImageMemory(dev, slot->image, slot->memory, 0);
	if (res != VK_SUCCESS) {
		fx_vk_error("vkBindImageMemory", res);
		vkFreeMemory(dev, slot->memory, NULL);
		slot->memory = VK_NULL_HANDLE;
		goto destroy;
	}
	slot->width = width;
	slot->height = height;
	return true;

destroy:
	vkDestroyImage(dev, slot->image, NULL);
	slot->image = VK_NULL_HANDLE;
	return false;
}

void fx_vk_blur_debug_capture(struct fx_vk_renderer *renderer,
		VkCommandBuffer cb, VkImage image, const struct wlr_box *region,
		const struct wlr_box *exclude, const char *tag) {
	if (!fx_vk_blur_debug_enabled() ||
			region->width <= 0 || region->height <= 0) {
		return;
	}
	struct blur_dump_slot *slot = slot_for(tag);
	if (slot == NULL || !slot_realloc(renderer, slot, region->width, region->height)) {
		return;
	}
	slot->region = *region;
	slot->exclude = exclude != NULL ? *exclude : (struct wlr_box){0};

	fx_vulkan_change_layout(cb, slot->image,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

	// The staging copy and the hole's four blits are transfer writes into this
	// same image, and they are the whole point of the capture -- read before
	// they land and the dump shows the previous frame's scratch instead.
	VkMemoryBarrier wait_for_writes = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &wait_for_writes, 0, NULL, 0, NULL);

	VkImageBlit blit = {
		.srcSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.srcOffsets = {
			{ region->x, region->y, 0 },
			{ region->x + region->width, region->y + region->height, 1 },
		},
		.dstSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.dstOffsets = {
			{ 0, 0, 0 },
			{ region->width, region->height, 1 },
		},
	};
	vkCmdBlitImage(cb, image, VK_IMAGE_LAYOUT_GENERAL,
		slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_NEAREST);

	fx_vulkan_change_layout(cb, slot->image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT);

	slot->pending = true;
}

// PAM rather than PPM: the source is premultiplied RGBA and the alpha is part
// of the evidence -- a hole filled with something fully transparent looks
// identical to one filled correctly once alpha has been thrown away.
static void write_pam(struct fx_vk_renderer *renderer,
		struct blur_dump_slot *slot, unsigned seq) {
	VkDevice dev = renderer->dev->dev;
	VkImageSubresource sub = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
	VkSubresourceLayout layout;
	vkGetImageSubresourceLayout(dev, slot->image, &sub, &layout);

	void *mapped;
	VkResult res = vkMapMemory(dev, slot->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (res != VK_SUCCESS) {
		fx_vk_error("vkMapMemory", res);
		return;
	}
	VkMappedMemoryRange range = {
		.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
		.memory = slot->memory,
		.size = VK_WHOLE_SIZE,
	};
	vkInvalidateMappedMemoryRanges(dev, 1, &range);

	char path[512];
	snprintf(path, sizeof(path), "%s-%u-%s.pam", dump.prefix, seq, slot->tag);
	FILE *f = fopen(path, "wb");
	if (f != NULL) {
		fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\n"
			"TUPLTYPE RGB_ALPHA\nENDHDR\n", slot->width, slot->height);
		const unsigned char *base = (const unsigned char *)mapped + layout.offset;
		for (int y = 0; y < slot->height; y++) {
			fwrite(base + (size_t)y * layout.rowPitch, 4, slot->width, f);
		}
		fclose(f);
	} else {
		wlr_log_errno(WLR_ERROR, "fx_vk blur dump: %s", path);
	}
	vkUnmapMemory(dev, slot->memory);

	// The crop's own origin, so a measurement taken in the .pam can be stated
	// in screen coordinates -- and, more to the point, so the hole can be
	// located inside it without guessing.
	snprintf(path, sizeof(path), "%s-%u-%s.txt", dump.prefix, seq, slot->tag);
	f = fopen(path, "w");
	if (f != NULL) {
		fprintf(f, "tag %s\nregion %d %d %d %d\nexclude %d %d %d %d\n",
			slot->tag,
			slot->region.x, slot->region.y, slot->region.width, slot->region.height,
			slot->exclude.x, slot->exclude.y,
			slot->exclude.width, slot->exclude.height);
		fclose(f);
	}
	wlr_log(WLR_INFO, "fx_vk blur dump: %s-%u-%s.pam %dx%d region %d,%d "
		"exclude %d,%d %dx%d", dump.prefix, seq, slot->tag,
		slot->width, slot->height, slot->region.x, slot->region.y,
		slot->exclude.x, slot->exclude.y,
		slot->exclude.width, slot->exclude.height);
}

void fx_vk_blur_debug_flush(struct fx_vk_renderer *renderer) {
	if (dump.prefix == NULL) {
		return;
	}
	bool any = false;
	for (size_t i = 0; i < BLUR_DUMP_SLOTS; i++) {
		any = any || dump.slots[i].pending;
	}
	if (!any) {
		return;
	}
	// Blunt, and deliberately so: this runs after the frame has been submitted
	// and the only requirement is that the copy has landed before the map.
	vkDeviceWaitIdle(renderer->dev->dev);
	for (size_t i = 0; i < BLUR_DUMP_SLOTS; i++) {
		struct blur_dump_slot *slot = &dump.slots[i];
		if (slot->pending) {
			write_pam(renderer, slot, dump.seq);
			slot->pending = false;
		}
	}
	dump.seq++;
	if (--dump.budget <= 0) {
		wlr_log(WLR_INFO, "fx_vk blur dump: budget spent, no more captures");
	}
}
