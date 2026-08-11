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
#include "vulkan/sync/avk_sync.h"

#include <cjson/cJSON.h>
#include <drm.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <wlr/backend.h>
#include <wlr/render/drm_syncobj.h>
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
	uint64_t commit_imports;      /* buffers taken ownership of at commit */
	uint64_t late_imports;        /* taken at DRAW time -- must stay 0 for
	                               * surfaces; see az_avk_walk_node() */
	uint64_t cache_hits;
	uint64_t cache_misses;
	uint64_t shm_upload_bytes;
	uint64_t client_images_cached;   /* live entries, not a total */
	uint64_t output_targets;         /* live imported scan-out images */
	uint64_t software_cursor_frames;

	/* Presentation synchronisation. Every frame AVK composites must leave
	 * through exactly one of the first two, and `present_sync_none` must stay
	 * at zero: a frame handed to a display engine with no fence attached is a
	 * frame the display may scan out while the GPU is still writing it. */
	uint64_t present_sync_timeline;  /* fence passed as a drm_syncobj point */
	uint64_t present_sync_dmabuf;    /* fence attached to the target dma-buf */
	uint64_t present_sync_none;      /* MUST stay 0 outside the break test */
	uint64_t present_sync_fails;
	/* GPU-side waits inserted before rendering into a target somebody else may
	 * still be reading. A wait, not a stall: it is the GPU that waits, and the
	 * CPU returns from the frame immediately.
	 *
	 * It means slightly different things on the two routes, which is worth
	 * knowing before reading anything into the number. On the timeline route
	 * it counts only the frames where the display engine had genuinely not
	 * released the target yet, and should be rare. On the dma-buf route the
	 * buffer's fence list also contains our own previous write, so the count
	 * is close to one per frame and says nothing about contention. */
	uint64_t presentation_waits;
	/* A target re-acquired in a state it should not have been in. Anything but
	 * zero means the swapchain handed back a buffer that was still in use. */
	uint64_t target_state_violations;
	/* Damage, in pixels actually redrawn versus pixels the outputs have. Both
	 * accumulate, so the ratio is over the whole run rather than one frame. */
	uint64_t damage_pixels;
	uint64_t output_pixels;
	uint64_t full_redraw_frames;
	uint64_t partial_redraw_frames;
	/* The most rectangles one frame's damage arrived in. Worth watching: the
	 * ring collapses to a bounding box past WLR_DAMAGE_RING_MAX_RECTS, so a
	 * number stuck at that limit means damage is being rounded up rather than
	 * tracked. */
	uint64_t damage_rects_max;
	/* Wall clock spent building a frame on the CPU, and the rolling maximum,
	 * because a mean hides exactly the frames that miss a vblank. */
	uint64_t cpu_frame_us;
	uint64_t cpu_frame_us_max;

	/* Each of these is a real limitation, and each is said exactly once so a
	 * log stays readable while still telling the truth. */
	bool warned_effect_node;
	bool warned_color_transform;
	bool warned_software_cursor;
	bool warned_zoom;
	bool warned_shm;
	bool warned_shm_source_gone;
	bool warned_gradient;
	bool warned_late_import;
	bool warned_no_present_sync;
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

	/* True when this entry was created from a wl_surface commit rather than
	 * discovered mid-frame. The distinction is the whole ownership rule: at
	 * commit the client's content is guaranteed valid, and later it is only
	 * valid by luck. */
	bool taken_at_commit;
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
	if (avk.client_images_cached > 0) {
		avk.client_images_cached--;
	}
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

/*
 * What a buffer actually IS, decided without asking a renderer.
 *
 * The old path answered "can this be drawn?" with "does it have a
 * wlr_texture?", which is a question about a renderer rather than about the
 * buffer. This asks the buffer: it is a dma-buf, or it is CPU-readable, or it
 * is neither and nothing can draw it. A single-pixel buffer is CPU-readable
 * like any other and needs no special case -- it is called out here only
 * because it is the one buffer type that is neither client-allocated nor
 * shared memory, and a reader looking for it should find the answer.
 */
enum az_buffer_kind {
	AZ_BUFFER_NONE,
	AZ_BUFFER_DMABUF,
	AZ_BUFFER_DATA_PTR,
};

struct az_buffer_source {
	enum az_buffer_kind kind;
	struct wlr_dmabuf_attributes dmabuf;   /* AZ_BUFFER_DMABUF only */
};

static bool az_buffer_get_source(struct wlr_buffer *buffer,
		struct az_buffer_source *out) {
	*out = (struct az_buffer_source){ .kind = AZ_BUFFER_NONE };
	if (buffer == NULL) {
		return false;
	}
	if (wlr_buffer_get_dmabuf(buffer, &out->dmabuf)) {
		out->kind = AZ_BUFFER_DMABUF;
		return true;
	}
	/* Probed by opening and immediately closing the access window. The
	 * alternative -- inspecting buffer->impl -- is reaching into wlroots
	 * internals to learn something wlroots will answer if asked. */
	void *data = NULL;
	uint32_t format = 0;
	size_t stride = 0;
	if (wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		wlr_buffer_end_data_ptr_access(buffer);
		out->kind = AZ_BUFFER_DATA_PTR;
		return true;
	}
	return false;
}

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
		avk.shm_upload_bytes += (uint64_t)stride * entry->buffer->height;
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
		avk.cache_hits++;
		if (entry->is_shm) {
			return az_avk_upload_shm(entry) ? entry->image : NULL;
		}
		return entry->image;
	}
	avk.cache_misses++;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return NULL;
	}
	entry->buffer = buffer;
	wlr_addon_init(&entry->addon, &buffer->addons, &avk,
		&az_avk_buffer_addon_impl);
	wl_list_insert(&avk.buffers, &entry->link);
	avk.client_images_cached++;

	struct az_buffer_source source;
	az_buffer_get_source(buffer, &source);
	switch (source.kind) {
	case AZ_BUFFER_DMABUF: {
		struct avk_dmabuf_attributes attribs;
		az_avk_attribs_from_wlr(&attribs, &source.dmabuf);
		/* The importer dup()s every fd it keeps, so from here on AVK's copy of
		 * the content survives the client destroying its wl_buffer. */
		entry->image = avk_dmabuf_import(&avk.importer, &attribs, false);
		break;
	}
	case AZ_BUFFER_DATA_PTR:
		if (!az_avk_upload_shm(entry)) {
			entry->image = NULL;
		}
		break;
	case AZ_BUFFER_NONE:
		wlr_log(WLR_ERROR, "AVK: a %dx%d buffer is neither a dma-buf nor "
			"CPU-readable; nothing can draw it", buffer->width,
			buffer->height);
		entry->image = NULL;
		break;
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
/*
 * Where a scan-out target is in its cycle.
 *
 * The states exist so that "may I render into this?" is a question with an
 * answer rather than an assumption. wlr_swapchain will only hand back a buffer
 * nobody holds a lock on, and until now that was the entire argument for
 * reusing it -- an argument that is true of the CPU's view and says nothing
 * about whether the display engine has finished reading the pixels. On an
 * implicitly synchronised path the kernel covered the difference. On an
 * explicitly synchronised one nothing does, and rendering into a buffer the
 * scanout engine is still reading produces tearing that looks like a damage
 * bug.
 */
enum az_avk_target_state {
	/* Never used, or the display engine has signalled it is done. */
	AZ_AVK_TARGET_FREE,
	/* A render was submitted into it but it never reached a commit -- a
	 * rejected output state, or a frame we abandoned. Safe to render into
	 * again: both submissions are on the same queue and therefore ordered. */
	AZ_AVK_TARGET_RENDERED,
	/* Handed to the display engine. Must not be rendered into until its
	 * release point is signalled. */
	AZ_AVK_TARGET_IN_FLIGHT,
};

struct az_avk_target {
	struct wlr_addon addon;
	struct avk_image *image;

	enum az_avk_target_state state;
	/* The point on the output's release timeline that frees this target, or 0
	 * when the release travels through the buffer's own dma-buf fences. */
	uint64_t release_point;
};

static void az_avk_target_addon_destroy(struct wlr_addon *addon) {
	struct az_avk_target *target = wl_container_of(addon, target, addon);
	if (target->image != NULL) {
		avk_retire_push(&avk.importer.retire, avk.device,
			target->image->last_use, avk_image_destroy, target->image);
		if (avk.output_targets > 0) {
			avk.output_targets--;
		}
	}
	wlr_addon_finish(&target->addon);
	free(target);
}

static const struct wlr_addon_interface az_avk_target_addon_impl = {
	.name = "az_avk_target",
	.destroy = az_avk_target_addon_destroy,
};

/*
 * How this output's finished frame reaches the display engine with a fence
 * attached to it.
 *
 * Two mechanisms, chosen from what the backend can actually do rather than
 * from what is nicer:
 *
 *   TIMELINE  The backend supports drm_syncobj timelines in an output commit.
 *             AVK exports its render completion as a sync_file, imports it at
 *             a point on a timeline, and hands the point to the output state.
 *             KMS waits on it in the kernel, and signals a second timeline
 *             when it has finished with the buffer. This is the good path: two
 *             fences, both directions covered, nothing polled.
 *
 *   DMABUF    The backend has no timeline support -- the headless backend has
 *             no DRM device at all, and a DRM backend on a kernel without
 *             DRM_CAP_SYNCOBJ_TIMELINE has no timelines either. The fence is
 *             instead attached to the target's own dma-buf reservation object,
 *             where any implicitly synchronised consumer will find it. The
 *             release direction comes back out of the same object.
 *
 * There is deliberately no third mechanism. If neither works the output is
 * declined and SceneFX renders it, because the alternative -- present anyway
 * and hope the driver is inserting fences on our behalf -- is exactly the
 * assumption this milestone exists to remove.
 */
enum az_avk_present_sync {
	AZ_AVK_PRESENT_SYNC_UNSET,
	AZ_AVK_PRESENT_SYNC_TIMELINE,
	AZ_AVK_PRESENT_SYNC_DMABUF,
	AZ_AVK_PRESENT_SYNC_BROKEN,
};

struct az_avk_output {
	struct wlr_swapchain *swapchain;
	int width, height;
	uint32_t drm_format;
	VkFormat vk_format;
	struct az_avk_renderer_slot *slot;

	/* The Vulkan end of the fence bridge. Per output rather than per device:
	 * two monitors run two independent frame cadences, and one shared export
	 * semaphore would have each one exporting the other's frame. */
	struct avk_sync sync;
	bool sync_ready;

	enum az_avk_present_sync present_sync;
	/* AVK -> display. A point is signalled by importing the frame's sync_file
	 * and handed to the output state as its wait point. */
	struct wlr_drm_syncobj_timeline *in_timeline;
	uint64_t in_point;
	/* display -> AVK. KMS signals a point when it has released the buffer. */
	struct wlr_drm_syncobj_timeline *out_timeline;
	uint64_t out_point;
};

static struct az_avk_target *az_avk_target_for_buffer(
		struct wlr_buffer *buffer) {
	struct wlr_addon *addon = wlr_addon_find(&buffer->addons, &avk,
		&az_avk_target_addon_impl);
	if (addon != NULL) {
		struct az_avk_target *target = wl_container_of(addon, target, addon);
		return target;
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
	target->state = AZ_AVK_TARGET_FREE;
	wlr_addon_init(&target->addon, &buffer->addons, &avk,
		&az_avk_target_addon_impl);
	avk.output_targets++;
	return target;
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

/* ── presentation synchronisation ───────────────────────────────────────── */

/*
 * Two switches, both of them for tests, both of them named for what they
 * break rather than what they enable.
 *
 * AZ_AVK_NO_PRESENT_SYNC=1 hands the frame over with no fence at all. It is
 * the break test for this whole file: with it set, `present_sync_none` climbs
 * and a test that claims to prove synchronisation had better start failing.
 *
 * AZ_AVK_NO_TIMELINE=1 pretends the backend has no drm_syncobj timeline
 * support, which forces the dma-buf reservation path. Without it that path is
 * only reachable on hardware nobody here has, and an untested fallback is a
 * fallback that does not work.
 */
static bool az_avk_env_flag(const char *name) {
	const char *v = getenv(name);
	return v != NULL && v[0] == '1';
}

/*
 * Decide, once per output, how a finished frame will carry its fence.
 *
 * Returns false when neither mechanism is available, in which case the output
 * is marked BROKEN and stays on the SceneFX path for the rest of the session.
 */
static bool az_avk_present_sync_prepare(struct az_avk_output *out,
		struct wlr_output *output) {
	if (out->present_sync == AZ_AVK_PRESENT_SYNC_BROKEN) {
		return false;
	}
	if (out->present_sync != AZ_AVK_PRESENT_SYNC_UNSET) {
		return true;
	}

	if (!out->sync_ready) {
		if (!avk_sync_init(&out->sync, avk.device, output->name)) {
			wlr_log(WLR_ERROR, "AVK: %s cannot be presented safely -- this "
				"device cannot turn a finished frame into a fence the display "
				"engine can wait on; staying on the SceneFX path",
				output->name);
			out->present_sync = AZ_AVK_PRESENT_SYNC_BROKEN;
			return false;
		}
		out->sync_ready = true;
	}

	int drm_fd = wlr_backend_get_drm_fd(output->backend);
	bool want_timeline = drm_fd >= 0 && output->backend->features.timeline
		&& !az_avk_env_flag("AZ_AVK_NO_TIMELINE");
	if (want_timeline) {
		out->in_timeline = wlr_drm_syncobj_timeline_create(drm_fd);
		out->out_timeline = wlr_drm_syncobj_timeline_create(drm_fd);
		if (out->in_timeline != NULL && out->out_timeline != NULL) {
			out->present_sync = AZ_AVK_PRESENT_SYNC_TIMELINE;
			wlr_log(WLR_INFO, "AVK: %s presents with drm_syncobj timelines; "
				"the display engine waits for the GPU in the kernel",
				output->name);
			return true;
		}
		wlr_drm_syncobj_timeline_unref(out->in_timeline);
		wlr_drm_syncobj_timeline_unref(out->out_timeline);
		out->in_timeline = NULL;
		out->out_timeline = NULL;
		wlr_log(WLR_ERROR, "AVK: %s supports timelines but they could not be "
			"created; falling back to dma-buf fences", output->name);
	}

	/* The dma-buf path cannot be probed without a real fence to attach, so it
	 * is assumed here and verified on the first frame -- see the handover.
	 * A kernel too old for the ioctl marks the output BROKEN there. */
	out->present_sync = AZ_AVK_PRESENT_SYNC_DMABUF;
	wlr_log(WLR_INFO, "AVK: %s has no drm_syncobj timeline support%s; the "
		"frame's fence travels on the target's dma-buf instead", output->name,
		az_avk_env_flag("AZ_AVK_NO_TIMELINE") ? " (forced by "
			"AZ_AVK_NO_TIMELINE)" : "");
	return true;
}

/*
 * Has the display engine finished with this target, and if not, what do we
 * wait on?
 *
 * Appends at most one wait to `waits`. Returns false only when the target is
 * genuinely unusable, which is the assertion the state machine exists for.
 */
static bool az_avk_target_acquire(struct az_avk_output *out,
		struct az_avk_target *target, struct wlr_buffer *buffer,
		VkSemaphoreSubmitInfo *waits, uint32_t *wait_count) {
	if (target->state == AZ_AVK_TARGET_FREE ||
			target->state == AZ_AVK_TARGET_RENDERED) {
		/* RENDERED is safe without a wait: the earlier submission and this one
		 * are on the same queue, and a queue orders its own work. */
		return true;
	}

	int fence = -1;
	if (out->present_sync == AZ_AVK_PRESENT_SYNC_TIMELINE) {
		bool signalled = false;
		if (wlr_drm_syncobj_timeline_check(out->out_timeline,
				target->release_point, 0, &signalled) && signalled) {
			target->state = AZ_AVK_TARGET_FREE;
			return true;
		}
		/* Not signalled. Either the display engine is still using it, or the
		 * commit that would have signalled it was rejected and the point will
		 * never materialise. Those need opposite responses, and asking is one
		 * ioctl. */
		bool available = false;
		if (!wlr_drm_syncobj_timeline_check(out->out_timeline,
				target->release_point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
				&available) || !available) {
			target->state = AZ_AVK_TARGET_FREE;
			return true;
		}
		fence = wlr_drm_syncobj_timeline_export_sync_file(out->out_timeline,
			target->release_point);
		if (fence < 0) {
			wlr_log(WLR_ERROR, "AVK: cannot export the release fence for a "
				"target still held by the display engine");
			avk.target_state_violations++;
			return false;
		}
	} else {
		/* The reservation object carries whatever anyone put on it -- the
		 * display engine's read, and also our own last write. Asking as a
		 * WRITER returns both, which is what we are about to become. Waiting
		 * on our own write is redundant, since one queue orders its own work,
		 * but it is one semaphore on a wait list rather than a stall and
		 * telling the two apart would cost more than it saves. */
		struct wlr_dmabuf_attributes dmabuf;
		if (!wlr_buffer_get_dmabuf(buffer, &dmabuf) || dmabuf.n_planes < 1) {
			avk.target_state_violations++;
			return false;
		}
		if (!avk_sync_dmabuf_fences(dmabuf.fd[0], DMA_BUF_SYNC_WRITE, &fence)) {
			avk.target_state_violations++;
			return false;
		}
		if (fence < 0) {
			/* No fences on the buffer: nobody is using it. */
			target->state = AZ_AVK_TARGET_FREE;
			return true;
		}
	}

	VkSemaphore sem = avk_sync_import_sync_file(&out->sync, fence);
	if (sem == VK_NULL_HANDLE) {
		avk.target_state_violations++;
		return false;
	}
	waits[*wait_count] = (VkSemaphoreSubmitInfo){
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = sem,
		/* The wait guards the colour attachment write, so that is the stage
		 * that has to block. Everything earlier in the pipeline may run. */
		.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	};
	(*wait_count)++;
	avk.presentation_waits++;
	target->state = AZ_AVK_TARGET_FREE;
	return true;
}

/*
 * Attach the frame's completion fence to the thing being presented.
 *
 * Called after the render has been submitted and before the output state is
 * committed. Returns false if the fence could not be attached, in which case
 * the caller must NOT present the frame: an unfenced buffer handed to a display
 * engine is a buffer that may be scanned out while the GPU is still writing it,
 * and the resulting tearing is indistinguishable from a damage-tracking bug.
 */
static bool az_avk_present_handover(struct az_avk_output *out,
		struct az_avk_target *target, struct wlr_buffer *buffer,
		struct wlr_output_state *state) {
	/* The export happens first and unconditionally, even in the break test.
	 * A binary semaphore signalled by one submission and then signalled again
	 * by the next, with nothing in between consuming the payload, is undefined
	 * behaviour -- so a break test that simply skipped the export would not be
	 * testing "no fence is handed on", it would be testing "the semaphore is
	 * misused", and it would fail for the wrong reason. Exporting and dropping
	 * the fence on the floor is exactly and only the missing handover. */
	int fence = -1;
	if (!avk_sync_export_sync_file(&out->sync, &fence)) {
		/* Not transient: the device either can export its completion or it
		 * cannot, and retrying every frame would produce one error line per
		 * vblank forever. */
		wlr_log(WLR_ERROR, "AVK: the frame's completion could not be exported "
			"as a fence; this output goes back to the SceneFX path");
		out->present_sync = AZ_AVK_PRESENT_SYNC_BROKEN;
		avk.present_sync_fails++;
		return false;
	}

	if (az_avk_env_flag("AZ_AVK_NO_PRESENT_SYNC")) {
		if (!avk.warned_no_present_sync) {
			avk.warned_no_present_sync = true;
			wlr_log(WLR_ERROR, "AVK: AZ_AVK_NO_PRESENT_SYNC=1 -- frames are "
				"being handed to the display with no fence attached. This is "
				"the break test for presentation synchronisation and it must "
				"never be set on a desktop you care about.");
		}
		if (fence >= 0) {
			close(fence);
		}
		avk.present_sync_none++;
		target->state = AZ_AVK_TARGET_RENDERED;
		return true;
	}

	if (out->present_sync == AZ_AVK_PRESENT_SYNC_TIMELINE) {
		if (fence >= 0) {
			out->in_point++;
			if (!wlr_drm_syncobj_timeline_import_sync_file(out->in_timeline,
					out->in_point, fence)) {
				wlr_log(WLR_ERROR, "AVK: the frame's fence could not be placed "
					"on the output's wait timeline");
				close(fence);
				avk.present_sync_fails++;
				return false;
			}
			close(fence);
			wlr_output_state_set_wait_timeline(state, out->in_timeline,
				out->in_point);
		}
		/* The release direction is asked for whether or not there was anything
		 * to wait on: it is how the target gets back to FREE. */
		out->out_point++;
		wlr_output_state_set_signal_timeline(state, out->out_timeline,
			out->out_point);
		target->release_point = out->out_point;
		target->state = AZ_AVK_TARGET_IN_FLIGHT;
		avk.present_sync_timeline++;
		return true;
	}

	if (fence < 0) {
		/* Already complete. Nothing to attach, and nothing to wait for. */
		target->release_point = 0;
		target->state = AZ_AVK_TARGET_IN_FLIGHT;
		avk.present_sync_dmabuf++;
		return true;
	}

	struct wlr_dmabuf_attributes dmabuf;
	if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		close(fence);
		avk.present_sync_fails++;
		return false;
	}
	/* One fence, every plane: a multi-planar target is one image to the GPU
	 * and several reservation objects to the kernel, and a consumer that reads
	 * plane 1 without waiting is just as wrong as one that reads plane 0.
	 * avk_sync_dmabuf_attach consumes the fd, so each plane gets its own dup. */
	bool ok = true;
	for (int i = 0; i < dmabuf.n_planes; i++) {
		int dup = fcntl(fence, F_DUPFD_CLOEXEC, 0);
		if (dup < 0 || !avk_sync_dmabuf_attach(dmabuf.fd[i],
				DMA_BUF_SYNC_WRITE, dup)) {
			ok = false;
			break;
		}
	}
	close(fence);
	if (!ok) {
		wlr_log(WLR_ERROR, "AVK: the frame's fence could not be attached to "
			"the target dma-buf; this output cannot be presented safely and "
			"is going back to the SceneFX path");
		out->present_sync = AZ_AVK_PRESENT_SYNC_BROKEN;
		avk.present_sync_fails++;
		return false;
	}

	target->release_point = 0;
	target->state = AZ_AVK_TARGET_IN_FLIGHT;
	avk.present_sync_dmabuf++;
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
	/* Signal the timelines to their maximum before dropping them: anything
	 * still waiting on a point that will now never be reached would wait for
	 * the lifetime of the process. */
	if (out->in_timeline != NULL) {
		wlr_drm_syncobj_timeline_signal(out->in_timeline, UINT64_MAX);
		wlr_drm_syncobj_timeline_unref(out->in_timeline);
		out->in_timeline = NULL;
	}
	if (out->out_timeline != NULL) {
		wlr_drm_syncobj_timeline_signal(out->out_timeline, UINT64_MAX);
		wlr_drm_syncobj_timeline_unref(out->out_timeline);
		out->out_timeline = NULL;
	}
	if (out->sync_ready) {
		avk_sync_log_stats(&out->sync, "output");
		avk_sync_finish(&out->sync);
		out->sync_ready = false;
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

/*
 * One frame's scene, as AVK sees it, at DEBUG.
 *
 * Enabled with AVK_SCENE_DUMP=<n>: dump frame n and stop. This exists because
 * twice now a rendering fault has been diagnosed by guessing at the scene when
 * the answer was one print away -- once a border rect covering its own window,
 * once a surface node that was simply not there. The scene walker is the only
 * place that knows what AVK was actually asked to draw.
 */
static bool az_avk_dumping(void) {
	static int frame = -2;
	if (frame == -2) {
		const char *env = getenv("AVK_SCENE_DUMP");
		frame = env != NULL ? atoi(env) : -1;
	}
	return frame >= 0 && (uint64_t)frame == avk.frames;
}

static void az_avk_walk_node(struct az_avk_walk *walk,
		struct wlr_scene_node *node, int lx, int ly) {
	if (!node->enabled) {
		if (az_avk_dumping()) {
			wlr_log(WLR_ERROR, "scene: node=%p type=%d DISABLED at %d,%d",
				(void *)node, node->type, lx, ly);
		}
		return;
	}
	if (az_avk_dumping()) {
		int w = 0, h = 0;
		if (node->type == WLR_SCENE_NODE_RECT) {
			w = wlr_scene_rect_from_node(node)->width;
			h = wlr_scene_rect_from_node(node)->height;
		} else if (node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *b = wlr_scene_buffer_from_node(node);
			w = b->dst_width;
			h = b->dst_height;
			wlr_log(WLR_ERROR, "scene: node=%p BUFFER at %d,%d %dx%d buffer=%p",
				(void *)node, lx, ly, w, h, (void *)b->buffer);
		}
		if (node->type != WLR_SCENE_NODE_BUFFER) {
			wlr_log(WLR_ERROR, "scene: node=%p type=%d at %d,%d %dx%d",
				(void *)node, node->type, lx, ly, w, h);
		}
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
		/*
		 * A surface's content must already be ours by now.
		 *
		 * If this is the first time AVK has seen a surface's buffer at DRAW
		 * time, the commit hook did not run, and the only reason it renders
		 * correctly is that the client has not yet reused or destroyed the
		 * buffer. That is the bug this milestone removed, so it is worth a
		 * loud complaint rather than a silent success.
		 */
		if (wlr_scene_surface_try_from_buffer(buf) != NULL &&
				wlr_addon_find(&buf->buffer->addons, &avk,
					&az_avk_buffer_addon_impl) == NULL) {
			avk.late_imports++;
			if (!avk.warned_late_import) {
				avk.warned_late_import = true;
				wlr_log(WLR_ERROR, "AVK: a surface's buffer reached the frame "
					"without having been taken at commit; AVK's ownership of "
					"client content is not what it should be");
			}
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

	/* Decided once, on the first frame, and permanent: an output whose frames
	 * cannot be handed over with a fence attached is not one AVK may present,
	 * however well it composites. */
	if (m->avk != NULL && m->avk->present_sync == AZ_AVK_PRESENT_SYNC_BROKEN) {
		return false;
	}

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

	struct timespec frame_t0;
	clock_gettime(CLOCK_MONOTONIC, &frame_t0);

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

	/* Before anything is rendered: can this frame be handed over with a fence
	 * at the end of it? Finding out afterwards would mean either presenting it
	 * unsynchronised or throwing away work already submitted. */
	if (!az_avk_present_sync_prepare(out, output)) {
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
	struct az_avk_target *target_rec = az_avk_target_for_buffer(buffer);
	if (target_rec == NULL) {
		wlr_buffer_unlock(buffer);
		avk.fallback_frames++;
		return false;
	}
	struct avk_image *target = target_rec->image;

	/* Is the display engine still reading it? */
	VkSemaphoreSubmitInfo waits[2];
	uint32_t wait_count = 0;
	if (!az_avk_target_acquire(out, target_rec, buffer, waits, &wait_count)) {
		wlr_log(WLR_ERROR, "AVK: %s's swapchain handed back a target the "
			"display engine has not released", output->name);
		wlr_buffer_unlock(buffer);
		avk.fallback_frames++;
		return false;
	}

	/* ── the snapshot ───────────────────────────────────────────────────── */
	struct avk_scene scene;
	avk_scene_init(&scene);

	/*
	 * What has to be redrawn into THIS buffer.
	 *
	 * Not "what changed since the last frame" -- that would be right only for a
	 * swapchain one buffer deep. A target that last held frame N-3 needs
	 * everything that has changed since frame N-3, and the ring is what knows
	 * that, per buffer, because it is keyed on the wlr_buffer itself. A buffer
	 * it has never seen comes back fully damaged, which is exactly right for a
	 * target whose contents are undefined.
	 *
	 * It is the *scene's* ring, deliberately, and shared with the SceneFX path:
	 * a frame that falls back renders into a different buffer, and the ring
	 * accounts for both because it tracks buffers rather than frames. Keeping a
	 * second ring here would mean each path silently forgetting the other's
	 * frames.
	 *
	 * rotate_buffer MUTATES the ring -- it moves `current` into this buffer's
	 * entry -- so it happens after every check that can still decline the
	 * frame, and every failure below it has to trash the ring. Rotating and
	 * then not rendering is how a region gets acknowledged without ever being
	 * drawn, which shows up as a stale rectangle that survives until something
	 * else happens to damage it.
	 */
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	wlr_damage_ring_rotate_buffer(&m->scene_output->damage_ring, buffer,
		&damage);
	if (az_avk_env_flag("AZ_AVK_FULL_DAMAGE")) {
		/* What M3b did on every frame. Kept as a switch because it is the
		 * reference a damage test compares against: the same scene, redrawn
		 * whole, is the only thing that can say whether the partially redrawn
		 * one is right. */
		pixman_region32_clear(&damage);
		pixman_region32_union_rect(&damage, &damage, 0, 0, width, height);
	}
	pixman_region32_copy(&scene.damage, &damage);

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

	/* The extra signal is the frame's completion as something exportable: a
	 * timeline semaphore cannot become a sync_file, so a binary one rides
	 * alongside it purely to be turned into a file descriptor a moment later. */
	VkSemaphoreSubmitInfo signals[1] = {{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = avk_sync_signal_semaphore(&out->sync),
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	}};
	uint64_t timeline = avk_render_frame(&out->slot->renderer, target, &scene,
		waits, wait_count, signals, 1);
	avk_scene_finish(&scene);
	if (timeline == 0) {
		/* The ring has already been rotated, so the damage this frame was
		 * going to draw is now recorded as drawn. Trashing it is the only
		 * honest recovery: the next frame redraws everything rather than
		 * inheriting a region nobody ever painted. */
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
		pixman_region32_fini(&damage);
		wlr_buffer_unlock(buffer);
		avk.fallback_frames++;
		return false;
	}
	target_rec->state = AZ_AVK_TARGET_RENDERED;

	/* Submitted. Now give the display engine something to wait on. */
	if (!az_avk_present_handover(out, target_rec, buffer, state)) {
		/* The work is already in flight and will complete; the buffer just
		 * never becomes a frame. SceneFX renders this one into a different
		 * buffer, so nothing is torn and nothing is lost but the effort. */
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
		pixman_region32_fini(&damage);
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

	/*
	 * What the BACKEND is told changed, which is a slightly different question
	 * from what we just redrew.
	 *
	 * The minimal answer is "the region that differs from what is currently on
	 * screen", and wlr_scene keeps exactly that -- but in a WLR_PRIVATE field,
	 * and reaching into one of those to save a few pixels of blit is not a
	 * trade worth making.
	 *
	 * The redraw region is the honest superset. It is damage accumulated since
	 * THIS buffer was last presented, and the on-screen buffer was presented
	 * more recently, so it contains the minimal answer and over-reports by at
	 * most the last few frames' damage. Over-reporting output damage costs the
	 * backend a little extra work and is never wrong; under-reporting leaves
	 * stale pixels on screen.
	 *
	 * Containing it also matters for a second reason. wlr_scene subtracts the
	 * committed damage from its own pending region, and
	 * wlr_scene_output_needs_frame() is true while that region is non-empty --
	 * so a report that did not cover it would leave the compositor rendering
	 * frames forever on an idle desktop.
	 */
	wlr_output_state_set_damage(state, &damage);

	/* Damage accounting. Both totals accumulate over the run, so
	 * damage_pixels/output_pixels is the fraction of the desktop AVK actually
	 * redrew -- the single number that says whether damage tracking is doing
	 * anything at all. */
	uint64_t damage_px = 0;
	int rect_count = 0;
	const pixman_box32_t *rects = pixman_region32_rectangles(&damage,
		&rect_count);
	for (int i = 0; i < rect_count; i++) {
		damage_px += (uint64_t)(rects[i].x2 - rects[i].x1)
			* (uint64_t)(rects[i].y2 - rects[i].y1);
	}
	uint64_t output_px = (uint64_t)width * (uint64_t)height;
	avk.damage_pixels += damage_px;
	avk.output_pixels += output_px;
	if (damage_px >= output_px) {
		avk.full_redraw_frames++;
	} else {
		avk.partial_redraw_frames++;
	}
	if (rect_count > (int)avk.damage_rects_max) {
		avk.damage_rects_max = (uint64_t)rect_count;
	}
	pixman_region32_fini(&damage);

	struct timespec frame_t1;
	clock_gettime(CLOCK_MONOTONIC, &frame_t1);
	uint64_t us = (uint64_t)((frame_t1.tv_sec - frame_t0.tv_sec) * 1000000
		+ (frame_t1.tv_nsec - frame_t0.tv_nsec) / 1000);
	avk.cpu_frame_us += us;
	if (us > avk.cpu_frame_us_max) {
		avk.cpu_frame_us_max = us;
	}

	avk.frames++;
	return true;
}

/* ── taking ownership at commit ─────────────────────────────────────────── */

/*
 * When AVK acquires the content, and why it cannot be later.
 *
 * A client may destroy its wl_buffer the moment it has committed it. The old
 * architecture survived that because wlroots had already copied the pixels
 * into a renderer-owned wlr_texture; AVK has no such copy, so it has to take
 * its own ownership at the one moment the content is guaranteed valid -- the
 * commit itself.
 *
 * "Ownership" here does not mean holding the wlr_buffer. It means holding
 * something that outlives it: a dma-buf import dup()s every file descriptor,
 * and a CPU buffer is copied into an AVK image. After that the client can do
 * whatever it likes with the original.
 *
 * The pattern this replaces -- wait for a render frame, then hope the buffer
 * is still readable -- is not merely fragile. It produced exactly one visible
 * symptom, a missing wallpaper, and it would have produced others at random
 * for any client that draws once and lets go.
 */
struct az_avk_surface {
	struct wlr_surface *surface;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void az_avk_surface_commit(struct wl_listener *listener, void *data) {
	struct az_avk_surface *as = wl_container_of(listener, as, commit);
	struct wlr_buffer *buffer = as->surface->current.buffer;
	if (buffer == NULL) {
		return;
	}
	struct wlr_addon *addon = wlr_addon_find(&buffer->addons, &avk,
		&az_avk_buffer_addon_impl);
	if (addon != NULL) {
		/* Already ours. A client re-committing the same buffer is ordinary. */
		return;
	}
	if (az_avk_image_for_buffer(buffer) != NULL) {
		addon = wlr_addon_find(&buffer->addons, &avk,
			&az_avk_buffer_addon_impl);
		if (addon != NULL) {
			struct az_avk_buffer *entry = wl_container_of(addon, entry, addon);
			entry->taken_at_commit = true;
		}
		avk.commit_imports++;
	}
}

static void az_avk_surface_destroy(struct wl_listener *listener, void *data) {
	struct az_avk_surface *as = wl_container_of(listener, as, destroy);
	wl_list_remove(&as->commit.link);
	wl_list_remove(&as->destroy.link);
	free(as);
}

static void az_avk_new_surface(struct wl_listener *listener, void *data) {
	if (!avk.active) {
		return;
	}
	struct wlr_surface *surface = data;
	struct az_avk_surface *as = calloc(1, sizeof(*as));
	if (as == NULL) {
		return;
	}
	as->surface = surface;
	as->commit.notify = az_avk_surface_commit;
	wl_signal_add(&surface->events.commit, &as->commit);
	as->destroy.notify = az_avk_surface_destroy;
	wl_signal_add(&surface->events.destroy, &as->destroy);
}

static struct wl_listener az_avk_new_surface_listener = {
	.notify = az_avk_new_surface,
};
static bool az_avk_new_surface_attached = false;

/*
 * Detach from wlroots before the display goes away.
 *
 * wlr_compositor asserts that nothing is still listening to new_surface when
 * the display is destroyed, and it is right to: a listener outliving the
 * signal is a use-after-free waiting for the next client. This has to run
 * before wl_display_destroy(), which is earlier than az_avk_finish().
 */
static void az_avk_detach(void) {
	if (az_avk_new_surface_attached) {
		wl_list_remove(&az_avk_new_surface_listener.link);
		az_avk_new_surface_attached = false;
	}
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

/*
 * `amsg get avk-stats`.
 *
 * Counters only mattered at shutdown until now, which made every question
 * about a running desktop ("is the copy path hot?", "did damage actually
 * shrink?") unanswerable without killing the thing you were measuring.
 *
 * Anything not yet measured is reported as JSON null rather than 0. A zero is
 * a measurement; a null is an admission. Reporting "gpu_frame_us: 0" for a
 * timing this build does not collect would be the kind of number that gets
 * quoted back later as evidence.
 */
static cJSON *az_avk_stats_json(void) {
	cJSON *o = cJSON_CreateObject();
	if (o == NULL) {
		return NULL;
	}
	if (!avk.active) {
		cJSON_AddStringToObject(o, "backend", "scenefx");
		cJSON_AddBoolToObject(o, "active", false);
		return o;
	}

	const struct avk_caps *caps = &avk.device->caps;
	cJSON_AddStringToObject(o, "backend", "avk");
	cJSON_AddBoolToObject(o, "active", true);
	cJSON_AddStringToObject(o, "physical_device", caps->device_name);
	cJSON_AddStringToObject(o, "driver", caps->driver_name);
	char drm[64];
	snprintf(drm, sizeof(drm), "%" PRId64 ":%" PRId64,
		caps->drm_render_major, caps->drm_render_minor);
	cJSON_AddStringToObject(o, "drm_device", drm);
	cJSON_AddBoolToObject(o, "wl_compositor_has_renderer", false);

	/* composition */
	uint64_t surfaces = 0, rects = 0, submit_ns = 0, sync_waits = 0;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (!avk.renderers[i].used) {
			continue;
		}
		const struct avk_renderer_stats *st = &avk.renderers[i].renderer.stats;
		surfaces += st->surfaces;
		rects += st->rects;
		submit_ns += st->gpu_submit_ns;
		sync_waits += st->cpu_sync_waits;
	}
	cJSON_AddNumberToObject(o, "frames", (double)avk.frames);
	cJSON_AddNumberToObject(o, "fallback_frames", (double)avk.fallback_frames);
	cJSON_AddNumberToObject(o, "surfaces", (double)surfaces);
	cJSON_AddNumberToObject(o, "rects", (double)rects);

	/* import */
	const struct avk_dmabuf_importer *imp = &avk.importer;
	cJSON_AddNumberToObject(o, "dmabuf_zero_copy",
		(double)(imp->imports_explicit + imp->imports_recovered));
	cJSON_AddNumberToObject(o, "dmabuf_explicit_modifier",
		(double)imp->imports_explicit);
	cJSON_AddNumberToObject(o, "dmabuf_implicit_fallback",
		(double)imp->imports_copied);
	cJSON_AddNumberToObject(o, "dmabuf_import_failures",
		(double)imp->imports_failed);
	cJSON_AddNumberToObject(o, "shm_upload_bytes",
		(double)avk.shm_upload_bytes);
	cJSON_AddNumberToObject(o, "implicit_copy_bytes",
		(double)imp->copied_bytes);
	cJSON_AddNumberToObject(o, "implicit_copy_us", (double)imp->copied_us);

	/* ownership */
	cJSON_AddNumberToObject(o, "commit_imports", (double)avk.commit_imports);
	cJSON_AddNumberToObject(o, "late_imports", (double)avk.late_imports);
	cJSON_AddNumberToObject(o, "client_images_cached",
		(double)avk.client_images_cached);
	cJSON_AddNumberToObject(o, "client_image_cache_hits",
		(double)avk.cache_hits);
	cJSON_AddNumberToObject(o, "client_image_cache_misses",
		(double)avk.cache_misses);

	/* timing */
	cJSON_AddNumberToObject(o, "gpu_submit_us", (double)(submit_ns / 1000));
	cJSON_AddNumberToObject(o, "cpu_frame_us", (double)avk.cpu_frame_us);
	cJSON_AddNumberToObject(o, "cpu_frame_us_max",
		(double)avk.cpu_frame_us_max);
	/* Not measured: needs GPU timestamp queries around the frame, which this
	 * build does not record. Null, not zero. */
	cJSON_AddNullToObject(o, "gpu_frame_us");

	/* damage */
	cJSON_AddNumberToObject(o, "damage_pixels", (double)avk.damage_pixels);
	cJSON_AddNumberToObject(o, "output_pixels", (double)avk.output_pixels);
	if (avk.output_pixels > 0) {
		cJSON_AddNumberToObject(o, "damage_ratio",
			(double)avk.damage_pixels / (double)avk.output_pixels);
	} else {
		cJSON_AddNullToObject(o, "damage_ratio");
	}
	cJSON_AddNumberToObject(o, "full_redraw_frames",
		(double)avk.full_redraw_frames);
	cJSON_AddNumberToObject(o, "partial_redraw_frames",
		(double)avk.partial_redraw_frames);
	cJSON_AddNumberToObject(o, "damage_rects_max",
		(double)avk.damage_rects_max);

	/* synchronisation and presentation */
	cJSON_AddNumberToObject(o, "cpu_sync_waits", (double)sync_waits);
	cJSON_AddNumberToObject(o, "presentation_waits",
		(double)avk.presentation_waits);
	cJSON_AddNumberToObject(o, "present_sync_timeline",
		(double)avk.present_sync_timeline);
	cJSON_AddNumberToObject(o, "present_sync_dmabuf",
		(double)avk.present_sync_dmabuf);
	cJSON_AddNumberToObject(o, "present_sync_none",
		(double)avk.present_sync_none);
	cJSON_AddNumberToObject(o, "present_sync_failures",
		(double)avk.present_sync_fails);
	cJSON_AddNumberToObject(o, "target_state_violations",
		(double)avk.target_state_violations);
	cJSON_AddNumberToObject(o, "output_targets_in_flight",
		(double)avk.output_targets);

	cJSON_AddNumberToObject(o, "software_cursor_frames",
		(double)avk.software_cursor_frames);
	cJSON_AddNumberToObject(o, "validation_errors",
		(double)avk_validation_errors());
	return o;
}

/*
 * Zero the counters without restarting.
 *
 * Live values -- how many images are cached, how many targets exist -- are
 * deliberately NOT reset: they describe the present, not an interval, and
 * zeroing them would make the next reading a lie until the caches turned over.
 */
static void az_avk_stats_reset(void) {
	avk.frames = 0;
	avk.fallback_frames = 0;
	avk.buffer_imports = 0;
	avk.buffer_import_fails = 0;
	avk.shm_uploads = 0;
	avk.shm_upload_bytes = 0;
	avk.commit_imports = 0;
	avk.late_imports = 0;
	avk.cache_hits = 0;
	avk.cache_misses = 0;
	avk.damage_pixels = 0;
	avk.output_pixels = 0;
	avk.full_redraw_frames = 0;
	avk.partial_redraw_frames = 0;
	avk.damage_rects_max = 0;
	avk.cpu_frame_us = 0;
	avk.cpu_frame_us_max = 0;
	avk.software_cursor_frames = 0;
	avk.presentation_waits = 0;
	avk.present_sync_timeline = 0;
	avk.present_sync_dmabuf = 0;
	avk.present_sync_none = 0;
	avk.present_sync_fails = 0;
	avk.target_state_violations = 0;
	if (avk.active) {
		avk.importer.imports_explicit = 0;
		avk.importer.imports_recovered = 0;
		avk.importer.imports_copied = 0;
		avk.importer.imports_failed = 0;
		avk.importer.copied_bytes = 0;
		avk.importer.copied_us = 0;
		for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
			if (avk.renderers[i].used) {
				avk.renderers[i].renderer.stats =
					(struct avk_renderer_stats){0};
			}
		}
	}
}

static void az_avk_log_stats(void) {
	if (!avk.active) {
		return;
	}
	wlr_log(WLR_INFO, "avk.frames=%" PRIu64 " avk.fallback_frames=%" PRIu64
		" avk.buffer_imports=%" PRIu64 " avk.buffer_import_fails=%" PRIu64
		" avk.shm_uploads=%" PRIu64, avk.frames, avk.fallback_frames,
		avk.buffer_imports, avk.buffer_import_fails, avk.shm_uploads);
	wlr_log(WLR_INFO, "avk.commit_imports=%" PRIu64 " avk.late_imports=%" PRIu64
		" avk.cache_hits=%" PRIu64 " avk.cache_misses=%" PRIu64,
		avk.commit_imports, avk.late_imports, avk.cache_hits,
		avk.cache_misses);
	wlr_log(WLR_INFO, "avk.damage_pixels=%" PRIu64 " avk.output_pixels=%" PRIu64
		" avk.full_redraw_frames=%" PRIu64 " avk.partial_redraw_frames=%" PRIu64
		" avk.damage_rects_max=%" PRIu64, avk.damage_pixels, avk.output_pixels,
		avk.full_redraw_frames, avk.partial_redraw_frames,
		avk.damage_rects_max);
	wlr_log(WLR_INFO, "avk.present_sync_timeline=%" PRIu64
		" avk.present_sync_dmabuf=%" PRIu64 " avk.present_sync_none=%" PRIu64
		" avk.present_sync_fails=%" PRIu64 " avk.presentation_waits=%" PRIu64
		" avk.target_state_violations=%" PRIu64, avk.present_sync_timeline,
		avk.present_sync_dmabuf, avk.present_sync_none, avk.present_sync_fails,
		avk.presentation_waits, avk.target_state_violations);
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
