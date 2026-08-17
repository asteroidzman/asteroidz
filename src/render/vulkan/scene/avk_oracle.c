#include "avk_oracle.h"

#include "avk_blur_dump.h"
#include "../device/avk_device.h"
#include "../image/avk_upload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool avk_oracle_enabled(void) {
	static int cached = -1;
	if (cached < 0) {
		const char *env = getenv("AZ_FRAME_ORACLE");
		cached = env != NULL && env[0] == '1';
		if (cached) {
			/*
			 * SAID ONCE, AND SAID LOUDLY. Every frame is rendered twice, the
			 * second time through the same avk_render_frame() -- so every
			 * counter on the renderer includes the reference render's work. A
			 * counter assertion in this mode measures the oracle.
			 */
			avk_log(AVK_ERROR, "AZ_FRAME_ORACLE=1 -- every frame is rendered "
				"TWICE and read back. Renderer counters include the "
				"reference render; do not assert on avk-stats in this run.");
		}
	}
	return cached != 0;
}

bool avk_oracle_capture_armed(const struct avk_oracle *o) {
	return o->capture_armed;
}

void avk_oracle_arm_capture(struct avk_oracle *o) {
	o->capture_armed = true;
}

void avk_oracle_disarm_capture(struct avk_oracle *o) {
	o->capture_armed = false;
}

void avk_oracle_init(struct avk_oracle *o, struct avk_device *dev) {
	memset(o, 0, sizeof(*o));
	o->dev = dev;
	o->enabled = avk_oracle_enabled();
	o->divergent_frame = -1;
}

static void az_oracle_drop_buffer(struct avk_oracle *o) {
	if (o->buffer == VK_NULL_HANDLE) {
		return;
	}
	if (o->mapped != NULL) {
		vkUnmapMemory(o->dev->dev, o->memory);
		o->mapped = NULL;
	}
	vkDestroyBuffer(o->dev->dev, o->buffer, NULL);
	vkFreeMemory(o->dev->dev, o->memory, NULL);
	o->buffer = VK_NULL_HANDLE;
	o->memory = VK_NULL_HANDLE;
	o->capacity = 0;
}

static void az_oracle_clear_slots(struct avk_oracle *o) {
	for (size_t i = 0; i < o->slot_len; i++) {
		if (o->slots[i].has_mask) {
			pixman_region32_fini(&o->slots[i].mask);
		}
	}
	o->slot_len = 0;
	o->used = 0;
	memset(o->slots, 0, sizeof(o->slots));
}

void avk_oracle_finish(struct avk_oracle *o) {
	if (o->dev == NULL) {
		return;
	}
	az_oracle_clear_slots(o);
	az_oracle_drop_buffer(o);
	if (o->ref_image != NULL) {
		avk_image_destroy(o->dev, o->ref_image);
		o->ref_image = NULL;
	}
	o->enabled = false;
}

void avk_oracle_begin(struct avk_oracle *o, enum avk_oracle_pass pass) {
	o->pass = pass;
	if (pass == AVK_ORACLE_PRODUCTION) {
		/* A frame starts here: both passes' slots go, and the bump allocator
		 * rewinds. The reference render that follows appends to the same
		 * buffer, so the two are readable side by side. */
		az_oracle_clear_slots(o);
	}
}

/* Grow the readback buffer to at least `need`. Waits for idle first: the buffer
 * being replaced may still be the destination of a submitted copy, and this is
 * a diagnostic path where a stall costs nothing. */
static bool az_oracle_reserve(struct avk_oracle *o, VkDeviceSize need) {
	if (need <= o->capacity) {
		return true;
	}
	/*
	 * 128 MB UP FRONT, and that is not generosity.
	 *
	 * Growing DESTROYS the allocation the slots already recorded into, so any
	 * growth in the middle of a frame leaves the production taps pointing at
	 * freed memory -- which reads back as every pixel differing at full
	 * amplitude and is indistinguishable from a catastrophic renderer bug. A
	 * frame's whole demand is one output plus two taps per blur, so a size that
	 * covers the largest plausible frame outright means the failure mode simply
	 * does not arise. If it arises anyway, the slots are invalidated and
	 * counted rather than silently compared.
	 */
	VkDeviceSize want = o->capacity == 0 ? (VkDeviceSize)128 << 20 : o->capacity;
	while (want < need) {
		want *= 2;
	}
	avk_device_wait_idle(o->dev);
	if (o->slot_len > 0) {
		o->invalidated++;
		avk_log(AVK_ERROR, "avk oracle: readback buffer grew to %llu MB with "
			"%zu slots live; this frame's comparison is discarded",
			(unsigned long long)(want >> 20), o->slot_len);
		az_oracle_clear_slots(o);
	}
	az_oracle_drop_buffer(o);

	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = want,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (!avk_check(vkCreateBuffer(o->dev->dev, &bi, NULL, &o->buffer),
			"vkCreateBuffer (oracle)")) {
		o->buffer = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements reqs;
	vkGetBufferMemoryRequirements(o->dev->dev, o->buffer, &reqs);
	uint32_t type = 0;
	if (!avk_find_memory_type(o->dev, reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
			| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
		avk_log(AVK_ERROR, "avk oracle: no host-visible memory type");
		az_oracle_drop_buffer(o);
		return false;
	}
	VkMemoryAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (!avk_check(vkAllocateMemory(o->dev->dev, &ai, NULL, &o->memory),
			"vkAllocateMemory (oracle)")) {
		o->memory = VK_NULL_HANDLE;
		az_oracle_drop_buffer(o);
		return false;
	}
	if (!avk_check(vkBindBufferMemory(o->dev->dev, o->buffer, o->memory, 0),
			"vkBindBufferMemory (oracle)")
			|| !avk_check(vkMapMemory(o->dev->dev, o->memory, 0, want, 0,
				&o->mapped), "vkMapMemory (oracle)")) {
		az_oracle_drop_buffer(o);
		return false;
	}
	o->capacity = want;
	return true;
}

struct az_oracle_copy {
	struct avk_oracle *o;
	VkImage image;
	VkImageLayout layout;
	struct avk_box box;
	VkDeviceSize offset;
};

/*
 * One per tap, alive until the graph has recorded. Kept in a fixed array so a
 * tap costs no allocation.
 *
 * ONE ARRAY FOR EVERY RENDERER, and that is safe rather than lucky: a graph's
 * passes are recorded and finished inside avk_render_frame(), so two frames are
 * never being built at once -- not two outputs, and not the production and
 * reference renders, which run one after the other. The same argument is what
 * lets the renderer hold a single struct avk_graph.
 */
static struct az_oracle_copy az_copies[AVK_ORACLE_MAX_SLOTS];

static void az_oracle_record(VkCommandBuffer cb, void *user) {
	const struct az_oracle_copy *c = user;
	VkBufferImageCopy region = {
		.bufferOffset = c->offset,
		.bufferRowLength = (uint32_t)c->box.width,
		.bufferImageHeight = (uint32_t)c->box.height,
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.imageOffset = { c->box.x, c->box.y, 0 },
		.imageExtent = {
			(uint32_t)c->box.width, (uint32_t)c->box.height, 1,
		},
	};
	vkCmdCopyImageToBuffer(cb, c->image, c->layout, c->o->buffer, 1, &region);

	/*
	 * AND THE HOST BARRIER, in the same pass.
	 *
	 * A timeline wait alone does not make a transfer write visible to the host
	 * in the way validation requires; the copy has to be ordered against
	 * HOST_READ explicitly. Cheap, and it keeps synchronization validation
	 * silent, which matters because a diagnostic that generates its own
	 * SYNC-HAZARDs makes the run it is diagnosing unreadable.
	 */
	VkBufferMemoryBarrier2 bb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
		.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = c->o->buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.bufferMemoryBarrierCount = 1,
		.pBufferMemoryBarriers = &bb,
	};
	vkCmdPipelineBarrier2(cb, &dep);
}

bool avk_oracle_tap(struct avk_oracle *o, struct avk_graph *graph,
		uint32_t resource, struct avk_image *image, struct avk_box box,
		enum avk_oracle_tap_kind kind, size_t cmd_index,
		const pixman_region32_t *mask) {
	/*
	 * WHO WANTS THIS TAP. Three diagnostics share one readback path and each
	 * one must be able to run without dragging the others in:
	 *
	 *   OUTPUT   the oracle's boundary 3, and also what a bare capture_output
	 *            needs -- arming that must not start a reference render.
	 *   PREFIX   the oracle's boundary 1, and also the blur SOURCE the dump
	 *            exists to write.
	 *   BLUR     boundary 2 only. The dump is about the source; capturing the
	 *            result as well would double a stall for a picture the screen
	 *            already shows.
	 *   CACHE    the dump only. See the enum.
	 */
	bool want;
	switch (kind) {
	case AVK_TAP_OUTPUT:
		want = o->enabled || o->capture_armed;
		break;
	case AVK_TAP_PREFIX:
		want = o->enabled || avk_blur_dump_armed();
		break;
	case AVK_TAP_CACHE:
		want = avk_blur_dump_armed();
		break;
	default:
		want = o->enabled;
		break;
	}
	if (!want || box.width <= 0 || box.height <= 0) {
		return false;
	}
	if (o->slot_len >= AVK_ORACLE_MAX_SLOTS) {
		o->dropped++;
		return false;
	}
	/*
	 * FOUR BYTES A PIXEL, OR NOTHING.
	 *
	 * Every stride here, the compare, and the PPM writer below all assume it,
	 * and on Path B they would be wrong: the prefix and blur taps land on
	 * scene-linear FP16 images at EIGHT bytes a pixel, so a tap would read half
	 * of each row and report the difference between two misaligned pictures as
	 * a divergence. The OUTPUT tap is unaffected -- that is still the scan-out
	 * buffer, in the scan-out format.
	 *
	 * DECLINED rather than adapted, deliberately. This oracle's entire
	 * vocabulary is "N pixels differ by M CODES", and a code is a property of a
	 * quantised format -- there is no honest reading of "worst channel 3" over
	 * half-floats. Giving it a second notion of difference is real work and it
	 * is not this one, so the tap says so once and stands down.
	 */
	if (avk_format_bytes_per_pixel(image->format) != 4) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			avk_log(AVK_INFO, "avk oracle: prefix/blur taps are declined on a "
				"%u-byte format (Path B's FP16 intermediate); the OUTPUT tap "
				"still names a divergent frame",
				avk_format_bytes_per_pixel(image->format));
		}
		o->dropped++;
		return false;
	}
	/* Clamp to the image: a capture region may be declared larger than the
	 * backing extent the pool handed out, and a copy past the edge is a
	 * validation error rather than an interesting result. */
	if (box.x < 0) {
		box.width += box.x;
		box.x = 0;
	}
	if (box.y < 0) {
		box.height += box.y;
		box.y = 0;
	}
	if (box.x + box.width > (int32_t)image->extent.width) {
		box.width = (int32_t)image->extent.width - box.x;
	}
	if (box.y + box.height > (int32_t)image->extent.height) {
		box.height = (int32_t)image->extent.height - box.y;
	}
	if (box.width <= 0 || box.height <= 0) {
		o->dropped++;
		return false;
	}

	const VkDeviceSize stride = (VkDeviceSize)box.width * 4;
	const VkDeviceSize bytes = stride * (VkDeviceSize)box.height;
	/* 16-byte aligned, which every bufferOffset requirement here is a divisor
	 * of, and which keeps a row start on a word boundary for the compare. */
	VkDeviceSize offset = (o->used + 15) & ~(VkDeviceSize)15;
	if (!az_oracle_reserve(o, offset + bytes)) {
		o->dropped++;
		return false;
	}

	struct az_oracle_copy *c = &az_copies[o->slot_len];
	*c = (struct az_oracle_copy){
		.o = o,
		.image = image->image,
		.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.box = box,
		.offset = offset,
	};
	if (!avk_graph_pass_begin(graph, "oracle tap", az_oracle_record, c)) {
		o->dropped++;
		return false;
	}
	if (!avk_graph_use(graph, resource, AVK_USE_TRANSFER_READ, &box)) {
		avk_graph_pass_end(graph);
		o->dropped++;
		return false;
	}
	avk_graph_pass_end(graph);

	struct avk_oracle_slot *slot = &o->slots[o->slot_len++];
	*slot = (struct avk_oracle_slot){
		.pass = o->pass,
		.kind = kind,
		.cmd_index = cmd_index,
		.box = box,
		.offset = offset,
		.stride = (uint32_t)stride,
		.valid = true,
	};
	if (mask != NULL) {
		slot->has_mask = true;
		pixman_region32_init(&slot->mask);
		pixman_region32_copy(&slot->mask, (pixman_region32_t *)mask);
	}
	o->used = offset + bytes;
	return true;
}

static const struct avk_oracle_slot *az_find(const struct avk_oracle *o,
		enum avk_oracle_pass pass, enum avk_oracle_tap_kind kind,
		size_t cmd_index) {
	for (size_t i = 0; i < o->slot_len; i++) {
		const struct avk_oracle_slot *s = &o->slots[i];
		if (s->valid && s->pass == pass && s->kind == kind
				&& s->cmd_index == cmd_index) {
			return s;
		}
	}
	return NULL;
}

const uint8_t *avk_oracle_pixels(const struct avk_oracle *o,
		enum avk_oracle_pass pass, enum avk_oracle_tap_kind kind,
		size_t cmd_index, struct avk_box *box_out, uint32_t *stride_out) {
	const struct avk_oracle_slot *s = az_find(o, pass, kind, cmd_index);
	if (s == NULL || o->mapped == NULL) {
		return NULL;
	}
	if (box_out != NULL) {
		*box_out = s->box;
	}
	if (stride_out != NULL) {
		*stride_out = s->stride;
	}
	return (const uint8_t *)o->mapped + s->offset;
}

bool avk_oracle_has_tap(const struct avk_oracle *o, enum avk_oracle_pass pass,
		enum avk_oracle_tap_kind kind, size_t cmd_index) {
	return az_find(o, pass, kind, cmd_index) != NULL;
}

int64_t avk_oracle_compare(const struct avk_oracle *o,
		enum avk_oracle_tap_kind kind, size_t cmd_index,
		struct avk_box *bbox_out, int *worst_out,
		struct avk_oracle_sample *sample_out) {
	if (sample_out != NULL) {
		*sample_out = (struct avk_oracle_sample){ 0 };
	}
	struct avk_box pb, rb;
	uint32_t ps, rs;
	const uint8_t *p = avk_oracle_pixels(o, AVK_ORACLE_PRODUCTION, kind,
		cmd_index, &pb, &ps);
	const uint8_t *r = avk_oracle_pixels(o, AVK_ORACLE_REFERENCE, kind,
		cmd_index, &rb, &rs);
	if (p == NULL || r == NULL) {
		return -1;
	}
	/*
	 * THE SAME REGION, OR NOTHING.
	 *
	 * Two taps of different geometry are not comparable, and quietly comparing
	 * the overlap would turn a capture-geometry defect -- which is one of the
	 * candidate root causes -- into a clean result. A mismatch here is itself
	 * the finding.
	 */
	if (pb.x != rb.x || pb.y != rb.y || pb.width != rb.width
			|| pb.height != rb.height) {
		avk_log(AVK_ERROR, "avk oracle: tap kind=%d cmd=%zu geometry differs: "
			"production %d,%d %dx%d vs reference %d,%d %dx%d", (int)kind,
			cmd_index, pb.x, pb.y, pb.width, pb.height,
			rb.x, rb.y, rb.width, rb.height);
		return -2;
	}

	/*
	 * THE PRODUCTION TAP'S MASK, not the reference's.
	 *
	 * The question is whether everything production CLAIMS to have produced is
	 * right, and its claim is exactly its mask: prefix_rebuild for a prefix tap,
	 * result_region for a blur tap, the whole target for the output. The
	 * reference writes more than that by construction and the surplus is not a
	 * defect.
	 */
	const struct avk_oracle_slot *pslot = az_find(o, AVK_ORACLE_PRODUCTION,
		kind, cmd_index);

	int64_t wrong = 0;
	int worst = 0;
	int x1 = pb.width, y1 = pb.height, x2 = 0, y2 = 0;
	for (int y = 0; y < pb.height; y++) {
		const uint8_t *pr = p + (size_t)y * ps;
		const uint8_t *rr = r + (size_t)y * rs;
		for (int x = 0; x < pb.width; x++) {
			if (pslot->has_mask && !pixman_region32_contains_point(
					(pixman_region32_t *)&pslot->mask, pb.x + x, pb.y + y,
					NULL)) {
				continue;
			}
			int d = 0;
			for (int ch = 0; ch < 4; ch++) {
				int e = (int)pr[x * 4 + ch] - (int)rr[x * 4 + ch];
				if (e < 0) {
					e = -e;
				}
				if (e > d) {
					d = e;
				}
			}
			if (d > 0) {
				wrong++;
				if (sample_out != NULL && !sample_out->found) {
					sample_out->found = true;
					sample_out->x = pb.x + x;
					sample_out->y = pb.y + y;
					memcpy(sample_out->production, pr + x * 4, 4);
					memcpy(sample_out->reference, rr + x * 4, 4);
				}
				if (d > worst) {
					worst = d;
				}
				if (x < x1) { x1 = x; }
				if (y < y1) { y1 = y; }
				if (x + 1 > x2) { x2 = x + 1; }
				if (y + 1 > y2) { y2 = y + 1; }
			}
		}
	}
	if (bbox_out != NULL) {
		/* In the IMAGE's coordinates, not the tap's, so a bbox can be compared
		 * against a write region without remembering where the tap started. */
		*bbox_out = wrong > 0
			? (struct avk_box){ pb.x + x1, pb.y + y1, x2 - x1, y2 - y1 }
			: (struct avk_box){ 0, 0, 0, 0 };
	}
	if (worst_out != NULL) {
		*worst_out = worst;
	}
	return wrong;
}

bool avk_oracle_write_ppm(const struct avk_oracle *o, const char *path,
		VkFormat format) {
	struct avk_box box;
	uint32_t stride;
	const uint8_t *px = avk_oracle_pixels(o, AVK_ORACLE_PRODUCTION,
		AVK_TAP_OUTPUT, 0, &box, &stride);
	if (px == NULL) {
		avk_log(AVK_ERROR, "avk oracle: no output tap to write to %s", path);
		return false;
	}
	/*
	 * CHANNEL ORDER FROM THE FORMAT, not from a guess. XR24 imports as
	 * B8G8R8A8, and a capture written as if it were R8G8B8A8 swaps red and
	 * blue -- which on a grey desktop is invisible and on the asymmetric
	 * colour markers a transform fixture needs is a wrong answer that looks
	 * like a mirror.
	 */
	int r = 2, g = 1, b = 0;
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
		r = 0; g = 1; b = 2;
		break;
	default:
		break;
	}

	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		avk_log(AVK_ERROR, "avk oracle: cannot write %s", path);
		return false;
	}
	fprintf(f, "P6\n%d %d\n255\n", box.width, box.height);
	uint8_t *row = malloc((size_t)box.width * 3);
	if (row == NULL) {
		fclose(f);
		return false;
	}
	/*
	 * BREAK SWITCH: read each row one pixel further along than the last.
	 *
	 * A readback whose row pitch is wrong by a constant produces a picture
	 * that is SHEARED rather than corrupt: every pixel is a real pixel, the
	 * colours are right, and at a glance it looks like a rendering artefact
	 * somewhere else entirely. That is why the capture-layout test measures a
	 * straight edge rather than trusting the extents, and this is what proves
	 * that measurement can fail. Test only; see contrib/avk-capture-layout-test.sh.
	 */
	static int skew = -1;
	if (skew < 0) {
		const char *env = getenv("AZ_AVK_CAPTURE_ROW_SKEW");
		skew = env != NULL ? atoi(env) : 0;
		if (skew != 0) {
			avk_log(AVK_ERROR, "AZ_AVK_CAPTURE_ROW_SKEW=%d -- every capture is "
				"read back with a deliberately wrong row pitch", skew);
		}
	}
	for (int y = 0; y < box.height; y++) {
		const uint8_t *src = px + (size_t)y * stride
			+ (size_t)((VkDeviceSize)y * (VkDeviceSize)skew * 4);
		if (skew != 0 && (size_t)((char *)src - (char *)px)
				+ (size_t)box.width * 4 > (size_t)(stride * box.height)) {
			src = px + (size_t)y * stride;   /* stay inside the mapping */
		}
		for (int x = 0; x < box.width; x++) {
			row[x * 3 + 0] = src[x * 4 + r];
			row[x * 3 + 1] = src[x * 4 + g];
			row[x * 3 + 2] = src[x * 4 + b];
		}
		fwrite(row, 1, (size_t)box.width * 3, f);
	}
	free(row);
	fclose(f);
	avk_log(AVK_ERROR, "avk capture: wrote %dx%d to %s", box.width,
		box.height, path);
	return true;
}

struct avk_image *avk_oracle_ref_target(struct avk_oracle *o, VkFormat format,
		uint32_t width, uint32_t height) {
	if (!o->enabled) {
		return NULL;
	}
	if (o->ref_image != NULL) {
		if (o->ref_image->format == format
				&& o->ref_image->extent.width == width
				&& o->ref_image->extent.height == height) {
			return o->ref_image;
		}
		avk_device_wait_idle(o->dev);
		avk_image_destroy(o->dev, o->ref_image);
		o->ref_image = NULL;
	}

	struct avk_image *image = avk_image_alloc(o->dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = format;
	image->extent = (VkExtent2D){ width, height };
	image->plane_count = 1;
	image->has_alpha = true;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	image->origin = AVK_IMAGE_OWNED;

	/*
	 * ── THE REFERENCE MUST BE ABLE TO ENCODE THE WAY PRODUCTION DOES ──────
	 *
	 * On Path A a frame is composited through the scan-out buffer's _SRGB
	 * attachment view, so the hardware encodes on write. avk_image_srgb_view()
	 * returns NULL for an image that was not created MUTABLE with that format
	 * in its view list, and avk_render_frame() then falls back to the plain
	 * view -- silently, because that fallback is correct for a target which
	 * genuinely has no twin.
	 *
	 * The reference target used to be such an image. The production frame
	 * therefore wrote ENCODED values and the reference wrote LINEAR ones, and
	 * the oracle reported every pixel of every frame as divergent: 18 of 20
	 * frames, WRONG=480000, worst 42 codes on a flat background. That is the
	 * instrument disagreeing with itself, not the compositor -- and it appeared
	 * the moment M6B/D5 made Path A the default, having been invisible for as
	 * long as Path A was opt-in.
	 *
	 * Only 8-bit formats have sRGB twins (F11: Vulkan defines none at 10 bits
	 * or half float), which is exactly the set Path A applies to.
	 */
	VkFormat twin = VK_FORMAT_UNDEFINED;
	switch (format) {
	case VK_FORMAT_B8G8R8A8_UNORM: twin = VK_FORMAT_B8G8R8A8_SRGB; break;
	case VK_FORMAT_R8G8B8A8_UNORM: twin = VK_FORMAT_R8G8B8A8_SRGB; break;
	default: break;
	}
	VkFormat view_formats[2] = { format, twin };
	VkImageFormatListCreateInfo fmt_list = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = 2,
		.pViewFormats = view_formats,
	};

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = twin != VK_FORMAT_UNDEFINED ? &fmt_list : NULL,
		.flags = twin != VK_FORMAT_UNDEFINED
			? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			| VK_IMAGE_USAGE_SAMPLED_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	image->format_srgb = twin;
	image->srgb_mutable = twin != VK_FORMAT_UNDEFINED;
	if (!avk_check(vkCreateImage(o->dev->dev, &info, NULL, &image->image),
			"vkCreateImage (oracle reference)")) {
		free(image);
		return NULL;
	}
	AVK_LIVE_INC(o->dev, images);

	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(o->dev->dev, image->image, &reqs);
	uint32_t type = 0;
	if (!avk_find_memory_type(o->dev, reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		avk_image_destroy(o->dev, image);
		return NULL;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (!avk_check(vkAllocateMemory(o->dev->dev, &alloc, NULL,
			&image->memory[0]), "vkAllocateMemory (oracle reference)")) {
		avk_image_destroy(o->dev, image);
		return NULL;
	}
	AVK_LIVE_INC(o->dev, device_memory);
	image->memory_count = 1;
	if (!avk_check(vkBindImageMemory(o->dev->dev, image->image,
			image->memory[0], 0), "vkBindImageMemory (oracle reference)")) {
		avk_image_destroy(o->dev, image);
		return NULL;
	}
	o->ref_image = image;
	return image;
}
