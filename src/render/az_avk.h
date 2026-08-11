#ifndef AZ_AVK_H
#define AZ_AVK_H

/*
 * The compositor half of the native Vulkan path.
 *
 * src/render/vulkan/ is the engine: it knows Vulkan and nothing else, and
 * tests/check-vulkan-isolation.py fails the build if a wlroots or GLES header
 * ever appears under it. This file is the other side of that boundary -- it
 * knows wlroots and asteroidz, and it is the only place the two vocabularies
 * meet.
 *
 * What it does, in the order a frame goes through it:
 *
 *   1. Freezes the wlr_scene tree into an avk_scene: a flat, immutable command
 *      list. Nothing below this point can see a scene node, so a client commit
 *      landing mid-frame cannot move geometry the GPU is already using.
 *   2. Resolves every buffer the frame references to an avk_image, cached on
 *      the wlr_buffer itself via wlr_addon so the cache cannot outlive what it
 *      caches. Client dma-bufs go through avk_dmabuf_import(), which is where
 *      the DRM_FORMAT_MOD_INVALID ladder lives; SHM goes through the staged
 *      upload. wlr_texture_from_buffer() is never called.
 *   3. Renders into a buffer from a wlr_swapchain we own, and hands that
 *      buffer to the output state. wlr_scene_output_build_state(),
 *      wlr_renderer_begin_buffer_pass() and wlr_render_pass are not involved.
 *
 * And, just as importantly, what it refuses to do. AVK in M3 has no colour
 * management, no effects and no software cursor of its own. Rather than render
 * those frames wrongly, az_avk_build_frame() returns false and the caller
 * falls back to the SceneFX path for that frame. A wrong frame is worse than a
 * slow one, and "AVK handles the outputs it can handle correctly" is a
 * position that can be shipped while the rest is built.
 */

#include "vulkan/avk.h"
#include "vulkan/device/avk_device.h"
#include "vulkan/device/avk_instance.h"
#include "vulkan/dmabuf/avk_dmabuf.h"
#include "vulkan/image/avk_upload.h"
#include "vulkan/scene/avk_render.h"
#include "vulkan/scene/avk_scene.h"

#include <wlr/render/swapchain.h>
#include <wlr/util/addon.h>
#include <wlr/util/transform.h>

/* ── state ──────────────────────────────────────────────────────────────── */

/*
 * One renderer per output colour format.
 *
 * Dynamic rendering bakes the attachment format into the pipeline, so an
 * XRGB8888 output and an XRGB2101010 output genuinely cannot share one. Four
 * slots is not a guess about hardware -- it is the number of distinct formats
 * a mixed SDR/HDR/10-bit desktop can present at once, and running out is
 * logged rather than silently mishandled.
 */
#define AZ_AVK_MAX_FORMATS 4

struct az_avk_renderer_slot {
	bool used;
	VkFormat format;
	struct avk_renderer renderer;
};

struct az_avk {
	/* Whether ASTEROIDZ_RENDERER asked for AVK at all. */
	bool requested;
	/* Whether the engine actually came up. Requested-but-failed is a hard
	 * error at startup, not a silent downgrade -- see az_avk_init(). */
	bool active;

	struct avk_instance *instance;
	struct avk_device *device;
	struct avk_dmabuf_importer importer;

	struct az_avk_renderer_slot renderers[AZ_AVK_MAX_FORMATS];

	/* Every live cache entry, so shutdown can free the images that are still
	 * attached to buffers nobody destroyed. Without it, vkDestroyDevice
	 * reports leaked memory objects -- which is not merely untidy, it is the
	 * difference between "AVK cleans up after itself" being a claim and being
	 * a checked fact. */
	struct wl_list buffers;   /* az_avk_buffer.link */

	/* Counters. The instrumentation the M3 brief asks for; dumped by
	 * az_avk_log_stats() at shutdown and on demand over IPC. */
	uint64_t frames;              /* frames AVK composited */
	uint64_t fallback_frames;     /* frames handed back to the SceneFX path */
	uint64_t buffer_imports;      /* client buffers resolved to an avk_image */
	uint64_t buffer_import_fails;
	uint64_t shm_uploads;         /* re-uploads of CPU buffers */

	/* Each of these is a real limitation, and each is said exactly once so a
	 * log stays readable while still telling the truth. */
	bool warned_effect_node;
	bool warned_color_transform;
	bool warned_software_cursor;
	bool warned_zoom;
	bool warned_full_damage;
	bool warned_shm;
	bool warned_shm_source_gone;
	bool warned_gradient;
};

static struct az_avk avk = {0};

/* ── client buffer cache ────────────────────────────────────────────────── */

/*
 * A wlr_buffer's avk_image, hung off the buffer with wlr_addon.
 *
 * An addon rather than a lookup table on purpose: the entry is destroyed by
 * the buffer's own destruction, so there is no window in which the cache holds
 * a pointer to a buffer that is gone. A table keyed on the buffer pointer has
 * exactly that window, and it is the shape of bug that reproduces once a week.
 */
struct az_avk_buffer {
	struct wlr_addon addon;
	struct wl_list link;      /* az_avk.buffers */
	struct wlr_buffer *buffer;
	struct avk_image *image;

	/* CPU-backed buffers have to be re-uploaded when their contents change,
	 * so they keep their staging buffer rather than reallocating it. */
	bool is_shm;
	struct avk_upload upload;

	/* Set once nothing worked, so the failure is diagnosed once instead of
	 * once per frame for as long as the window is open. */
	bool failed;
};

static void az_avk_buffer_destroy(struct az_avk_buffer *entry) {
	if (entry->image != NULL) {
		/* Retired against the timeline, not freed: a frame that sampled this
		 * image may still be in flight, and vkDestroyImage on an image the GPU
		 * is reading is undefined behaviour that usually survives long enough
		 * to be blamed on something else. */
		avk_retire_push(&avk.importer.retire, avk.device, entry->image->last_use,
			avk_image_destroy, entry->image);
		entry->image = NULL;
	}
	if (entry->is_shm) {
		struct avk_upload *up = calloc(1, sizeof(*up));
		if (up != NULL) {
			*up = entry->upload;
			avk_retire_push(&avk.importer.retire, avk.device,
				avk_device_timeline_value(avk.device) + 1, avk_upload_retire,
				up);
		}
	}
	wlr_addon_finish(&entry->addon);
	wl_list_remove(&entry->link);
	free(entry);
}

static void az_avk_buffer_addon_destroy(struct wlr_addon *addon) {
	struct az_avk_buffer *entry = wl_container_of(addon, entry, addon);
	az_avk_buffer_destroy(entry);
}

static const struct wlr_addon_interface az_avk_buffer_addon_impl = {
	.name = "az_avk_buffer",
	.destroy = az_avk_buffer_addon_destroy,
};

/* wlroots' dma-buf attributes into AVK's. Two structs saying the same thing,
 * because the engine may not include a wlroots header -- the conversion is the
 * price of that isolation and it is three lines. */
static void az_avk_attribs_from_wlr(struct avk_dmabuf_attributes *dst,
		const struct wlr_dmabuf_attributes *src) {
	*dst = (struct avk_dmabuf_attributes){
		.width = src->width,
		.height = src->height,
		.format = src->format,
		.modifier = src->modifier,
		.n_planes = src->n_planes,
	};
	for (int i = 0; i < src->n_planes && i < AVK_MAX_PLANES; i++) {
		dst->offset[i] = src->offset[i];
		dst->stride[i] = src->stride[i];
		dst->fd[i] = src->fd[i];
	}
}

/*
 * Upload a CPU-backed buffer into its cached image, allocating the image on
 * first sight.
 *
 * Re-uploaded on every frame that draws it. wlroots gives no content-change
 * signal for a wlr_buffer that a client keeps re-committing, and inventing one
 * by comparing pixels would cost more than the copy. This is a real AVK-mode
 * cost on SHM surfaces and it is logged as one rather than hidden.
 *
 * The awkward case, and the reason this function can succeed without reading
 * anything: wlroots wraps a client's buffer in a wlr_client_buffer, uploads it
 * into a wlr_texture, and then lets the client have the original back. Once
 * that happens the wlr_client_buffer answers "not a dma-buf" AND "not
 * CPU-readable" -- the only surviving copy of the pixels is inside a
 * wlr_texture, which is precisely the object this renderer may not touch.
 *
 * A statically-drawn surface -- a wallpaper, a menu, an icon -- hits that on
 * its second frame and every frame after. Failing there would blank exactly
 * the content that never changes. So: upload while the source is readable,
 * and keep the image we already have when it is not. That is correct for
 * static content and stale-by-at-most-nothing for it, because content that
 * does not change cannot go stale.
 *
 * The real fix is to stop wlroots uploading client buffers at all -- see
 * "SHM surfaces and the wlr_client_buffer problem" in
 * docs/vulkan-native-architecture.md.
 */
static bool az_avk_upload_shm(struct az_avk_buffer *entry) {
	void *data = NULL;
	uint32_t format = 0;
	size_t stride = 0;
	if (!wlr_buffer_begin_data_ptr_access(entry->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		if (entry->image != NULL) {
			if (!avk.warned_shm_source_gone) {
				avk.warned_shm_source_gone = true;
				wlr_log(WLR_INFO, "AVK: wlroots has taken a CPU buffer back "
					"from us after uploading it to a texture; AVK keeps the "
					"copy it made, so static content stays correct and "
					"changing content would freeze");
			}
			return true;
		}
		/* Said out loud. A buffer that is neither a dma-buf nor CPU-readable
		 * and that we have never managed to copy has no route onto the GPU at
		 * all, and the whole point of this subsystem is that such a buffer
		 * produces a diagnosis rather than a blank window. */
		wlr_log(WLR_ERROR, "AVK: a %dx%d buffer is neither a dma-buf nor "
			"CPU-readable and was never copied; it cannot be drawn",
			entry->buffer->width, entry->buffer->height);
		return false;
	}

	bool ok = false;
	if (entry->image == NULL) {
		entry->image = avk_upload_image_create(avk.device, format,
			(uint32_t)entry->buffer->width, (uint32_t)entry->buffer->height,
			AVK_IMAGE_OWNED);
		if (entry->image == NULL) {
			goto out;
		}
		entry->is_shm = true;
		if (!avk.warned_shm) {
			avk.warned_shm = true;
			wlr_log(WLR_INFO, "AVK: a client is using CPU (SHM) buffers; "
				"these are re-uploaded on every frame that draws them");
		}
	}

	/* The upload must not overwrite pixels a frame in flight is still
	 * sampling. One timeline wait on that frame's value costs the GPU an
	 * ordering edge and the CPU nothing. */
	VkSemaphoreSubmitInfo wait = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = avk.device->timeline,
		.value = entry->image->last_use,
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	};
	uint32_t wait_count = entry->image->last_use > 0 ? 1 : 0;

	ok = avk_upload_image_write(avk.device, &avk.importer.upload_ring,
		&entry->upload, entry->image, data, (uint32_t)stride,
		(uint32_t)entry->buffer->height, &wait, wait_count) != 0;
	if (ok) {
		avk.shm_uploads++;
	}

out:
	wlr_buffer_end_data_ptr_access(entry->buffer);
	return ok;
}

/*
 * The avk_image for a wlr_buffer, importing it on first sight.
 *
 * Returns NULL when the buffer cannot be represented on the GPU at all, having
 * said why. The caller drops the command rather than drawing black -- a black
 * rectangle where a window should be is the failure mode this whole subsystem
 * exists to remove, and reproducing it in the new path would be absurd.
 */
static struct avk_image *az_avk_image_for_buffer(struct wlr_buffer *buffer) {
	struct wlr_addon *addon = wlr_addon_find(&buffer->addons, &avk,
		&az_avk_buffer_addon_impl);
	struct az_avk_buffer *entry = NULL;
	if (addon != NULL) {
		entry = wl_container_of(addon, entry, addon);
		if (entry->failed) {
			/* Remembered, so a buffer nothing can import costs one log line
			 * and one pointer comparison rather than a fresh diagnosis at
			 * every refresh. */
			return NULL;
		}
		if (entry->is_shm) {
			return az_avk_upload_shm(entry) ? entry->image : NULL;
		}
		return entry->image;
	}

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return NULL;
	}
	entry->buffer = buffer;
	wlr_addon_init(&entry->addon, &buffer->addons, &avk,
		&az_avk_buffer_addon_impl);
	wl_list_insert(&avk.buffers, &entry->link);

	struct wlr_dmabuf_attributes dmabuf;
	if (wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		struct avk_dmabuf_attributes attribs;
		az_avk_attribs_from_wlr(&attribs, &dmabuf);
		entry->image = avk_dmabuf_import(&avk.importer, &attribs, false);
	} else {
		/* Not a dma-buf: the only other thing a wlr_buffer can be is
		 * CPU-accessible. */
		if (!az_avk_upload_shm(entry)) {
			entry->image = NULL;
		}
	}

	if (entry->image == NULL) {
		avk.buffer_import_fails++;
		/* The entry stays, holding the failure. Destroying it would put the
		 * buffer back to "never seen", and the next frame would run the whole
		 * import and log the whole diagnosis again. */
		entry->failed = true;
		return NULL;
	}
	avk.buffer_imports++;
	return entry->image;
}

/* ── output targets ─────────────────────────────────────────────────────── */

/*
 * The swapchain AVK renders into, and its buffers' imported images.
 *
 * A swapchain of our own rather than the output's: wlr_output's swapchain is
 * created for, and reallocated by, wlr_scene's own build path, and sharing it
 * would mean the two paths silently fight over buffer lifetime the first time
 * a frame falls back.
 */
struct az_avk_target {
	struct wlr_addon addon;
	struct avk_image *image;
};

static void az_avk_target_addon_destroy(struct wlr_addon *addon) {
	struct az_avk_target *target = wl_container_of(addon, target, addon);
	if (target->image != NULL) {
		avk_retire_push(&avk.importer.retire, avk.device,
			target->image->last_use, avk_image_destroy, target->image);
	}
	wlr_addon_finish(&target->addon);
	free(target);
}

static const struct wlr_addon_interface az_avk_target_addon_impl = {
	.name = "az_avk_target",
	.destroy = az_avk_target_addon_destroy,
};

struct az_avk_output {
	struct wlr_swapchain *swapchain;
	int width, height;
	uint32_t drm_format;
	VkFormat vk_format;
	struct az_avk_renderer_slot *slot;
};

static struct avk_image *az_avk_target_for_buffer(struct wlr_buffer *buffer) {
	struct wlr_addon *addon = wlr_addon_find(&buffer->addons, &avk,
		&az_avk_target_addon_impl);
	if (addon != NULL) {
		struct az_avk_target *target = wl_container_of(addon, target, addon);
		return target->image;
	}

	struct wlr_dmabuf_attributes dmabuf;
	if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		wlr_log(WLR_ERROR, "AVK: the output swapchain handed back a buffer "
			"that is not a dma-buf; AVK cannot render into it");
		return NULL;
	}

	struct avk_dmabuf_attributes attribs;
	az_avk_attribs_from_wlr(&attribs, &dmabuf);
	/* for_render: a target needs COLOR_ATTACHMENT capability, which is a
	 * different question from whether it can be sampled and has a different
	 * answer on some modifiers. */
	struct avk_image *image = avk_dmabuf_import(&avk.importer, &attribs, true);
	if (image == NULL) {
		return NULL;
	}

	struct az_avk_target *target = calloc(1, sizeof(*target));
	if (target == NULL) {
		avk_image_destroy(avk.device, image);
		return NULL;
	}
	target->image = image;
	wlr_addon_init(&target->addon, &buffer->addons, &avk,
		&az_avk_target_addon_impl);
	return image;
}

static struct az_avk_renderer_slot *az_avk_renderer_for(VkFormat format) {
	struct az_avk_renderer_slot *free_slot = NULL;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		struct az_avk_renderer_slot *slot = &avk.renderers[i];
		if (slot->used && slot->format == format) {
			return slot;
		}
		if (!slot->used && free_slot == NULL) {
			free_slot = slot;
		}
	}
	if (free_slot == NULL) {
		wlr_log(WLR_ERROR, "AVK: more than %d output formats in use at once; "
			"no renderer available for VkFormat %d", AZ_AVK_MAX_FORMATS,
			format);
		return NULL;
	}
	if (!avk_renderer_init(&free_slot->renderer, avk.device, format)) {
		return NULL;
	}
	free_slot->used = true;
	free_slot->format = format;
	wlr_log(WLR_INFO, "AVK: renderer ready for VkFormat %d", format);
	return free_slot;
}

/*
 * The format the swapchain should allocate, as the intersection of three
 * opinions that all have to be satisfied at once:
 *
 *   - the output's, because the buffer has to be scanned out;
 *   - AVK's, because it has to be a colour attachment;
 *   - and the allocator's, which finds out by trying.
 *
 * DRM_FORMAT_MOD_INVALID is deliberately excluded. As a *target*, an implicit
 * modifier has no recovery path -- the copy rung reads pixels out of a buffer,
 * which is no use for one we are about to render into -- so allocating one
 * would produce a buffer AVK cannot attach.
 */
static bool az_avk_pick_format(struct wlr_output *output, uint32_t fourcc,
		int width, int height, struct wlr_drm_format *out) {
	const struct avk_format_caps *caps =
		avk_format_table_find(&avk.importer.table, fourcc);
	if (caps == NULL) {
		wlr_log(WLR_ERROR, "AVK: no importable modifiers for format 0x%08x "
			"on this device", fourcc);
		return false;
	}

	/* NULL from wlr_output_get_primary_formats() means the backend has no
	 * format CONSTRAINT -- everything is allowed -- not that it supports
	 * nothing. Reading it the other way is how the headless backend, which
	 * constrains nothing at all, ends up looking like a backend that can
	 * scan out no formats whatsoever. */
	const struct wlr_drm_format_set *primary =
		wlr_output_get_primary_formats(output, WLR_BUFFER_CAP_DMABUF);
	if (primary != NULL && wlr_drm_format_set_get(primary, fourcc) == NULL) {
		wlr_log(WLR_ERROR, "AVK: output %s cannot scan out format 0x%08x",
			output->name, fourcc);
		return false;
	}

	*out = (struct wlr_drm_format){ .format = fourcc };
	for (uint32_t i = 0; i < caps->render_mod_count; i++) {
		const struct avk_modifier_caps *mc = &caps->render_mods[i];
		if (mc->modifier == DRM_FORMAT_MOD_INVALID) {
			continue;
		}
		if (primary != NULL &&
				!wlr_drm_format_set_has(primary, fourcc, mc->modifier)) {
			continue;
		}
		if ((int)mc->max_extent.width < width ||
				(int)mc->max_extent.height < height) {
			/* Not pedantry: compressed modifiers really do cap out below the
			 * resolutions people run, and a rejected commit at 4K is a much
			 * worse way to discover it. */
			continue;
		}
		uint64_t *grown = realloc(out->modifiers,
			(out->len + 1) * sizeof(*grown));
		if (grown == NULL) {
			wlr_drm_format_finish(out);
			return false;
		}
		out->modifiers = grown;
		out->modifiers[out->len++] = mc->modifier;
		out->capacity = out->len;
	}

	if (out->len == 0) {
		wlr_log(WLR_ERROR, "AVK: output %s and this GPU share no renderable "
			"modifier for format 0x%08x at %dx%d", output->name, fourcc,
			width, height);
		wlr_drm_format_finish(out);
		return false;
	}
	return true;
}

/* Called when a monitor goes away. The swapchain's buffers carry the target
 * addons, so destroying it destroys those too -- which is the reason the
 * images are hung off the buffers rather than kept in a list here. */
static void az_avk_output_finish(struct az_avk_output *out) {
	if (out == NULL) {
		return;
	}
	if (out->swapchain != NULL) {
		wlr_swapchain_destroy(out->swapchain);
		out->swapchain = NULL;
	}
	free(out);
}

/* ── the scene walker ───────────────────────────────────────────────────── */

struct az_avk_walk {
	struct avk_scene *scene;
	/* Output-buffer geometry: scale, transform and size, applied once here so
	 * every command is emitted in the pixels AVK will actually write. */
	float scale;
	enum wl_output_transform transform;
	int width, height;
	/* Layout coordinates of the output's top-left corner. */
	int ox, oy;
};

static enum avk_transform az_avk_transform(enum wl_output_transform t) {
	/* The two enumerations agree value for value -- both are the dihedral
	 * group of the square in the same order -- but AVK deliberately does not
	 * include a Wayland header, so the mapping is written out rather than
	 * cast. If wl_output_transform ever gains a member this becomes a compile
	 * error instead of a silently rotated desktop. */
	switch (t) {
	case WL_OUTPUT_TRANSFORM_NORMAL:      return AVK_TRANSFORM_NORMAL;
	case WL_OUTPUT_TRANSFORM_90:          return AVK_TRANSFORM_90;
	case WL_OUTPUT_TRANSFORM_180:         return AVK_TRANSFORM_180;
	case WL_OUTPUT_TRANSFORM_270:         return AVK_TRANSFORM_270;
	case WL_OUTPUT_TRANSFORM_FLIPPED:     return AVK_TRANSFORM_FLIPPED;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:  return AVK_TRANSFORM_FLIPPED_90;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180: return AVK_TRANSFORM_FLIPPED_180;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270: return AVK_TRANSFORM_FLIPPED_270;
	}
	return AVK_TRANSFORM_NORMAL;
}

/*
 * A node's logical box into the output buffer's pixels.
 *
 * Rounding matches wlr_scene's scale_box() exactly -- round the far edge and
 * subtract, rather than rounding the width. Rounding the width independently
 * makes adjacent windows on a fractional scale overlap by a pixel or leave a
 * seam, depending on where they land, which is the classic 1.25x-scaling
 * hairline.
 */
static void az_avk_box_to_output(const struct az_avk_walk *walk,
		int lx, int ly, int lw, int lh, struct wlr_box *out) {
	double x = (double)(lx - walk->ox);
	double y = (double)(ly - walk->oy);
	out->width = (int)(round((x + lw) * walk->scale) - round(x * walk->scale));
	out->height = (int)(round((y + lh) * walk->scale) - round(y * walk->scale));
	out->x = (int)round(x * walk->scale);
	out->y = (int)round(y * walk->scale);
	wlr_box_transform(out, out, wlr_output_transform_invert(walk->transform),
		walk->width, walk->height);
}

static void az_avk_walk_node(struct az_avk_walk *walk,
		struct wlr_scene_node *node, int lx, int ly);

static void az_avk_walk_children(struct az_avk_walk *walk,
		struct wlr_scene_tree *tree, int lx, int ly) {
	struct wlr_scene_node *child;
	/* Bottom to top: wlr_scene keeps children in paint order, and a painter's
	 * algorithm needs no occlusion information to be correct. Occlusion
	 * culling is an optimisation and is deliberately not in the first cut. */
	wl_list_for_each(child, &tree->children, link) {
		az_avk_walk_node(walk, child, lx + child->x, ly + child->y);
	}
}

static void az_avk_walk_node(struct az_avk_walk *walk,
		struct wlr_scene_node *node, int lx, int ly) {
	if (!node->enabled) {
		return;
	}

	switch (node->type) {
	case WLR_SCENE_NODE_TREE:
		az_avk_walk_children(walk, wlr_scene_tree_from_node(node), lx, ly);
		return;

	case WLR_SCENE_NODE_RECT: {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		struct wlr_box dst;
		az_avk_box_to_output(walk, lx, ly, rect->width, rect->height, &dst);
		if (dst.width <= 0 || dst.height <= 0) {
			return;
		}
		struct avk_cmd *cmd = avk_scene_add(walk->scene, AVK_CMD_RECT);
		if (cmd == NULL) {
			return;
		}
		cmd->dst = (struct avk_box){ dst.x, dst.y, dst.width, dst.height };
		/*
		 * A window border is not four rectangles -- SceneFX draws it as ONE
		 * filled rect with the window's interior clipped OUT of it, which is
		 * how a rounded border gets a rounded inside edge.
		 *
		 * Ignoring that clip does not produce a subtly wrong border. It
		 * produces a filled rectangle over the whole window, and since the
		 * border sits above the surface in the scene, every window renders as
		 * a flat block of its border colour. That was the first thing AVK got
		 * visibly wrong on a real desktop, and it looked like a texture bug
		 * for far longer than it should have.
		 *
		 * Expressed as a clip region rather than in the shader: pixman can
		 * subtract a box from a box, and AVK already scissors every command to
		 * its clip. The corner radii are not honoured -- the cut-out is square
		 * where it should be rounded, which is cosmetic and belongs to M4.
		 */
		/* AVK_NO_BORDER_CLIP=1 skips the cut-out, for the same reason: the
		 * test that asserts a window is not covered by its own border needs a
		 * build where it fails. */
		static int no_border_clip = -1;
		if (no_border_clip < 0) {
			const char *env = getenv("AVK_NO_BORDER_CLIP");
			no_border_clip = env != NULL && env[0] == '1';
		}
		if (!no_border_clip && !wlr_box_empty(&rect->clipped_region.area)) {
			struct wlr_box hole = rect->clipped_region.area;
			struct wlr_box hole_out;
			az_avk_box_to_output(walk, lx + hole.x, ly + hole.y, hole.width,
				hole.height, &hole_out);
			pixman_region32_t clip;
			pixman_region32_init_rect(&clip, dst.x, dst.y,
				(unsigned)dst.width, (unsigned)dst.height);
			pixman_region32_t cut;
			pixman_region32_init_rect(&cut, hole_out.x, hole_out.y,
				(unsigned)hole_out.width, (unsigned)hole_out.height);
			pixman_region32_subtract(&clip, &clip, &cut);
			avk_cmd_set_clip(cmd, &clip);
			pixman_region32_fini(&cut);
			pixman_region32_fini(&clip);
		}
		if (rect->has_gradient && rect->gradient_count > 0 &&
				!avk.warned_gradient) {
			avk.warned_gradient = true;
			wlr_log(WLR_INFO, "AVK: gradient-filled rects are drawn as their "
				"first colour; gradients are M4");
		}
		/* wlr_scene rect colours are PREMULTIPLIED -- scenefx writes them
		 * straight to the framebuffer under source-over blending. AVK's
		 * command takes a straight colour and premultiplies it itself, so the
		 * value has to be undone here. Getting this backwards darkens every
		 * translucent panel in the overview by exactly its own alpha, which
		 * looks like a theming choice rather than a bug. */
		float a = rect->color[3];
		for (int i = 0; i < 3; i++) {
			cmd->color[i] = a > 0.0f ? rect->color[i] / a : 0.0f;
		}
		cmd->color[3] = a;
		return;
	}

	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
		if (buf->buffer == NULL) {
			return;
		}
		struct wlr_box dst;
		az_avk_box_to_output(walk, lx, ly, buf->dst_width, buf->dst_height,
			&dst);
		if (dst.width <= 0 || dst.height <= 0) {
			return;
		}
		struct avk_image *image = az_avk_image_for_buffer(buf->buffer);
		if (image == NULL) {
			/* Already logged, in full, by the importer. Dropping the command
			 * leaves whatever is behind the window visible, which is at least
			 * honest about something having gone wrong. */
			return;
		}
		struct avk_cmd *cmd = avk_scene_add(walk->scene, AVK_CMD_TEXTURE);
		if (cmd == NULL) {
			return;
		}
		cmd->dst = (struct avk_box){ dst.x, dst.y, dst.width, dst.height };
		cmd->image = image;
		cmd->opacity = buf->opacity;
		if (wlr_fbox_empty(&buf->src_box)) {
			cmd->src = (struct avk_fbox){ 0, 0, image->extent.width,
				image->extent.height };
		} else {
			cmd->src = (struct avk_fbox){ buf->src_box.x, buf->src_box.y,
				buf->src_box.width, buf->src_box.height };
		}
		/* Same composition wlr_scene uses: undo the buffer's own transform,
		 * then apply the output's. */
		cmd->transform = az_avk_transform(wlr_output_transform_compose(
			wlr_output_transform_invert(buf->transform), walk->transform));
		cmd->filter_linear = buf->filter_mode == WLR_SCALE_FILTER_BILINEAR;
		return;
	}

	case WLR_SCENE_NODE_SHADOW:
	case WLR_SCENE_NODE_OPTIMIZED_BLUR:
	case WLR_SCENE_NODE_BLUR:
		/* M4. Recognised and skipped, with one warning -- not silently
		 * dropped, because "my shadows disappeared" should have an answer in
		 * the log rather than a bisect. */
		if (!avk.warned_effect_node) {
			avk.warned_effect_node = true;
			wlr_log(WLR_INFO, "AVK: shadow and blur nodes are not implemented "
				"yet (M4); they are skipped in AVK mode");
		}
		return;
	}
}

/* ── the frame ──────────────────────────────────────────────────────────── */

/*
 * Is this output one AVK can render correctly right now?
 *
 * Every check here is a thing M3 does not implement. Answering "no" costs a
 * frame on the SceneFX path; answering "yes" when it is not true costs a
 * visibly wrong desktop, which is the failure mode that makes a new renderer
 * untrustworthy for months.
 */
static bool az_avk_output_supported(Monitor *m,
		struct wlr_color_transform *color_transform) {
	struct wlr_output *output = m->wlr_output;

	if (color_transform != NULL || output->image_description != NULL) {
		if (!avk.warned_color_transform) {
			avk.warned_color_transform = true;
			wlr_log(WLR_INFO, "AVK: %s needs a colour transform (ICC or HDR); "
				"colour management is M6, so this output stays on the SceneFX "
				"path", output->name);
		}
		return false;
	}

	if (m->scene_output != NULL && m->scene_output->zoom > 1.0f) {
		if (!avk.warned_zoom) {
			avk.warned_zoom = true;
			wlr_log(WLR_INFO, "AVK: output magnification is not implemented "
				"yet; zoomed frames stay on the SceneFX path");
		}
		return false;
	}

	/* A software cursor has to be composited into the frame, and the only
	 * handle wlroots offers for that is wlr_output_add_software_cursors_to_
	 * render_pass() -- a wlr_render_pass, which is exactly what must not be on
	 * this path. Until AVK draws the cursor from asteroidz's own cursor state,
	 * a frame that needs a software cursor goes back to SceneFX. That is the
	 * difference between a slower cursor and no cursor. */
	if (output->hardware_cursor == NULL) {
		struct wlr_output_cursor *cursor;
		wl_list_for_each(cursor, &output->cursors, link) {
			if (cursor->enabled && cursor->visible) {
				if (!avk.warned_software_cursor) {
					avk.warned_software_cursor = true;
					wlr_log(WLR_INFO, "AVK: %s needs a software cursor; those "
						"frames stay on the SceneFX path until AVK draws the "
						"cursor itself", output->name);
				}
				return false;
			}
		}
	}

	return true;
}

/*
 * Build a frame with AVK, or return false and let the caller fall back.
 *
 * This is the function that replaces wlr_scene_output_build_state() in AVK
 * mode, and it is deliberately the only one: everything it does not do --
 * direct scanout, gamma LUTs, dma-buf feedback -- is absent rather than half
 * present, and listed in docs/vulkan-native-architecture.md.
 */
static bool az_avk_build_frame(Monitor *m, struct wlr_output_state *state,
		struct wlr_color_transform *color_transform) {
	if (!avk.active || m->scene_output == NULL) {
		return false;
	}
	if (!az_avk_output_supported(m, color_transform)) {
		avk.fallback_frames++;
		return false;
	}

	struct wlr_output *output = m->wlr_output;
	struct az_avk_output *out = m->avk;
	if (out == NULL) {
		out = calloc(1, sizeof(*out));
		if (out == NULL) {
			return false;
		}
		m->avk = out;
	}

	/* Size and format of the buffer we are about to allocate. Both are read
	 * from the state being built when it changes them, so a modeset frame
	 * renders at the new size rather than the old one. */
	int width, height;
	wlr_output_transformed_resolution(output, &width, &height);
	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		if (state->mode_type == WLR_OUTPUT_STATE_MODE_FIXED) {
			width = state->mode->width;
			height = state->mode->height;
		} else {
			width = state->custom_mode.width;
			height = state->custom_mode.height;
		}
	}
	uint32_t fourcc = (state->committed & WLR_OUTPUT_STATE_RENDER_FORMAT)
		? state->render_format : output->render_format;
	if (width <= 0 || height <= 0) {
		avk.fallback_frames++;
		return false;
	}

	if (out->swapchain == NULL || out->width != width || out->height != height
			|| out->drm_format != fourcc) {
		struct wlr_drm_format format;
		if (!az_avk_pick_format(output, fourcc, width, height, &format)) {
			avk.fallback_frames++;
			return false;
		}
		if (out->swapchain != NULL) {
			wlr_swapchain_destroy(out->swapchain);
		}
		out->swapchain = wlr_swapchain_create(alloc, width, height, &format);
		wlr_drm_format_finish(&format);
		if (out->swapchain == NULL) {
			wlr_log(WLR_ERROR, "AVK: could not create a %dx%d swapchain for %s",
				width, height, output->name);
			avk.fallback_frames++;
			return false;
		}
		out->width = width;
		out->height = height;
		out->drm_format = fourcc;

		const struct avk_drm_format *vk_fmt =
			avk_drm_format_from_fourcc(fourcc);
		out->vk_format = vk_fmt != NULL ? vk_fmt->vk : VK_FORMAT_UNDEFINED;
		out->slot = az_avk_renderer_for(out->vk_format);
		if (out->slot == NULL) {
			avk.fallback_frames++;
			return false;
		}
		wlr_log(WLR_INFO, "AVK: %s now composited at %dx%d, format 0x%08x",
			output->name, width, height, fourcc);
	}
	if (out->slot == NULL) {
		avk.fallback_frames++;
		return false;
	}

	struct wlr_buffer *buffer = wlr_swapchain_acquire(out->swapchain);
	if (buffer == NULL) {
		wlr_log(WLR_ERROR, "AVK: no free buffer in %s's swapchain",
			output->name);
		avk.fallback_frames++;
		return false;
	}
	struct avk_image *target = az_avk_target_for_buffer(buffer);
	if (target == NULL) {
		wlr_buffer_unlock(buffer);
		avk.fallback_frames++;
		return false;
	}

	/* ── the snapshot ───────────────────────────────────────────────────── */
	struct avk_scene scene;
	avk_scene_init(&scene);

	/* Full damage, every frame. This is a known AVK-mode regression, taken
	 * deliberately so the switch could land: the scene's damage ring is
	 * maintained by wlr_scene for its own build path, and consuming it
	 * correctly from here is the next commit rather than this one. */
	if (!avk.warned_full_damage) {
		avk.warned_full_damage = true;
		wlr_log(WLR_INFO, "AVK: rendering full damage every frame; partial "
			"damage is the next step and this is a real cost until then");
	}
	pixman_region32_union_rect(&scene.damage, &scene.damage, 0, 0, width,
		height);

	/* Cleared black underneath everything. asteroidz's own root_bg rect
	 * normally covers the output, but a clear costs one scissored draw and
	 * means an uncovered region is black rather than the previous frame. */
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;

	struct az_avk_walk walk = {
		.scene = &scene,
		.scale = output->scale,
		.transform = output->transform,
		.width = width,
		.height = height,
		.ox = m->scene_output->x,
		.oy = m->scene_output->y,
	};
	az_avk_walk_children(&walk, &m->scene_output->scene->tree, 0, 0);

	uint64_t timeline = avk_render_frame(&out->slot->renderer, target, &scene,
		NULL, 0, NULL, 0);
	avk_scene_finish(&scene);
	if (timeline == 0) {
		wlr_buffer_unlock(buffer);
		avk.fallback_frames++;
		return false;
	}

	/* Retire whatever the GPU has finished with. Once per frame, never
	 * blocking -- one vkGetSemaphoreCounterValue and a walk of a short list. */
	avk_renderer_collect(&out->slot->renderer);
	avk_dmabuf_importer_collect(&avk.importer);

	wlr_output_state_set_buffer(state, buffer);
	/* set_buffer locks it; the reference wlr_swapchain_acquire() gave us is
	 * ours to drop. */
	wlr_buffer_unlock(buffer);

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, width, height);
	wlr_output_state_set_damage(state, &damage);
	pixman_region32_fini(&damage);

	avk.frames++;
	return true;
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

static void az_avk_log_forward(enum avk_log_level level, const char *fmt,
		va_list args) {
	enum wlr_log_importance importance;
	switch (level) {
	case AVK_ERROR:
	case AVK_WARN:
		importance = WLR_ERROR;
		break;
	case AVK_INFO:
		importance = WLR_INFO;
		break;
	default:
		importance = WLR_DEBUG;
		break;
	}
	char msg[1024];
	vsnprintf(msg, sizeof(msg), fmt, args);
	/* "avk: " so a line in the compositor log says which subsystem produced
	 * it without the reader having to recognise the wording. */
	wlr_log(importance, "avk: %s", msg);
}

/*
 * Bring the engine up, or fail loudly.
 *
 * `drm_fd` is the node the compositor presents on. AVK is told which device to
 * use rather than choosing one: a compositor that renders on a different GPU
 * from the one it scans out on is a real configuration, and one that does so
 * by accident is undiagnosable after the fact.
 */
static bool az_avk_init(int drm_fd) {
	wl_list_init(&avk.buffers);
	avk_log_set_handler(az_avk_log_forward);

	avk.instance = avk_instance_create("asteroidz");
	if (avk.instance == NULL) {
		return false;
	}
	avk_instance_log_caps(avk.instance);

	avk.device = avk_device_create(avk.instance, drm_fd);
	if (avk.device == NULL) {
		avk_instance_destroy(avk.instance);
		avk.instance = NULL;
		return false;
	}
	avk_device_log_caps(avk.device);

	if (!avk_dmabuf_importer_init(&avk.importer, avk.device)) {
		avk_device_destroy(avk.device);
		avk_instance_destroy(avk.instance);
		avk.device = NULL;
		avk.instance = NULL;
		return false;
	}
	avk_format_table_log(&avk.importer.table);

	avk.active = true;
	return true;
}

static void az_avk_log_stats(void) {
	if (!avk.active) {
		return;
	}
	wlr_log(WLR_INFO, "avk.frames=%" PRIu64 " avk.fallback_frames=%" PRIu64
		" avk.buffer_imports=%" PRIu64 " avk.buffer_import_fails=%" PRIu64
		" avk.shm_uploads=%" PRIu64, avk.frames, avk.fallback_frames,
		avk.buffer_imports, avk.buffer_import_fails, avk.shm_uploads);
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			avk_renderer_log_stats(&avk.renderers[i].renderer);
		}
	}
	avk_dmabuf_importer_log_stats(&avk.importer);
}

static void az_avk_finish(void) {
	if (!avk.active) {
		return;
	}
	az_avk_log_stats();

	/* The images first, then the queues that free them, then the device. Any
	 * other order either leaks or frees twice. */
	struct az_avk_buffer *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &avk.buffers, link) {
		az_avk_buffer_destroy(entry);
	}

	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			avk_renderer_finish(&avk.renderers[i].renderer);
			avk.renderers[i].used = false;
		}
	}
	avk_dmabuf_importer_finish(&avk.importer);
	avk_device_save_pipeline_cache(avk.device);
	avk_device_destroy(avk.device);
	avk_instance_destroy(avk.instance);
	avk.device = NULL;
	avk.instance = NULL;
	avk.active = false;
}

#endif /* AZ_AVK_H */
