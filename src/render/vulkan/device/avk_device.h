#ifndef AVK_DEVICE_H
#define AVK_DEVICE_H

#include "avk_instance.h"

/*
 * The logical device, its queues, and the capability table.
 *
 * The capability table is built once, logged once, and then *read* -- nothing
 * below this layer is allowed to re-probe the driver mid-frame or guess from a
 * vendor ID. If a decision depends on what the GPU can do, the answer is a
 * field in `struct avk_caps` and it is visible in the log.
 */

/* How many frames of GPU work may be in flight before the CPU has to wait.
 * Three is the usual sweet spot: two lets the CPU stall behind a single slow
 * frame, and more than three just adds latency between input and photon
 * without adding throughput. */
#define AVK_FRAMES_IN_FLIGHT 3

struct avk_caps {
	/* identity */
	char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
	char driver_name[VK_MAX_DRIVER_NAME_SIZE];
	char driver_info[VK_MAX_DRIVER_INFO_SIZE];
	VkDriverId driver_id;
	uint32_t vendor_id;
	uint32_t device_id;
	VkPhysicalDeviceType device_type;
	uint32_t api_version;

	/* DRM identity. This is how a physical device is matched to a render
	 * node -- by dev_t, never by name, never by index, never by
	 * WLR_DRM_DEVICES. `has_drm` is false on drivers without
	 * VK_EXT_physical_device_drm, in which case there is no honest way to
	 * match and selection has to say so out loud. */
	bool has_drm;
	bool drm_has_primary;
	bool drm_has_render;
	int64_t drm_primary_major, drm_primary_minor;
	int64_t drm_render_major, drm_render_minor;

	/* core features we require (all Vulkan 1.2/1.3 core) */
	bool timeline_semaphore;
	bool synchronization2;
	bool dynamic_rendering;

	/* extensions we require to talk to the rest of the graphics stack */
	bool external_memory_fd;
	bool external_memory_dma_buf;
	bool external_semaphore_fd;
	bool image_drm_format_modifier;
	bool queue_family_foreign;

	/* optional, each one a real behavioural difference rather than a box to
	 * tick */
	bool external_fence_fd;
	bool memory_budget;
	bool calibrated_timestamps;
	bool sampler_ycbcr_conversion;
	bool pipeline_executable_properties;

	/* Can a binary semaphore be imported from, and exported to, a sync_file?
	 * This is what makes implicit-sync interop with clients possible without
	 * a CPU wait -- if it is false, some other path has to carry the fence and
	 * the frame path is at risk of a stall. Probed, not assumed. */
	bool semaphore_sync_fd_import;
	bool semaphore_sync_fd_export;
	/* Can a *timeline* semaphore be exported as an opaque FD? This is the
	 * handoff to DRM syncobj timelines. */
	bool timeline_opaque_fd_export;

	/*
	 * M5 colour capabilities (contract C5). Probed at init, logged in one
	 * line, and consumed by nothing yet -- C5 is deliberately pure
	 * information, so that the day the working intermediate is wired up,
	 * "does this device support the working format" is a fact recorded at
	 * startup rather than a question asked for the first time inside a frame.
	 *
	 * fp16_attach_blend_sample  R16G16B16A16_SFLOAT can be a blendable colour
	 *                           attachment AND a linearly-filtered sampled
	 *                           image on optimal tiling: ADR-001's criterion
	 *                           (a) for the Path-B working intermediate.
	 * rgb10a2_attach            A2B10G10R10_UNORM_PACK32 can be a blendable
	 *                           colour attachment: the 10-bit scanout target.
	 *
	 * The raw feature masks are kept beside the verdicts so a log line can say
	 * WHICH bit is missing rather than just "no".
	 */
	bool fp16_attach_blend_sample;
	bool rgb10a2_attach;
	VkFormatFeatureFlags fp16_optimal_features;
	VkFormatFeatureFlags rgb10a2_optimal_features;

	/* queues */
	uint32_t graphics_family;
	/* A transfer-only family, if the device has one. Recorded here because
	 * the staging ring (M2) will want it; deliberately NOT created yet --
	 * an unused VkQueue would be infrastructure claiming to exist before
	 * anything drives it. */
	bool has_dedicated_transfer_family;
	uint32_t transfer_family;
	bool has_dedicated_compute_family;
	uint32_t compute_family;

	/* limits worth having to hand */
	uint32_t max_image_dimension_2d;
	uint64_t max_memory_allocation_size;
	float timestamp_period;
	/* How many bits of a timestamp the GRAPHICS QUEUE FAMILY actually writes.
	 * A device-wide timestampPeriod says nothing about this, and the upper
	 * bits of a query result are undefined rather than zero -- so a duration
	 * computed without masking to these bits is meaningless. 0 means this
	 * family cannot write timestamps at all. */
	uint32_t timestamp_valid_bits;
	VkDeviceSize non_coherent_atom_size;
	VkDeviceSize optimal_buffer_copy_offset_alignment;
	VkDeviceSize optimal_buffer_copy_row_pitch_alignment;
};

/*
 * Device entry points that are not in core Vulkan and so have no linkable
 * symbol: they have to be fetched with vkGetDeviceProcAddr after the device
 * exists. Kept in one struct so that "is this call available?" is answered by a
 * NULL check on the pointer rather than by re-reading which extension was
 * enabled where.
 */
struct avk_device_api {
	PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR;
	PFN_vkImportSemaphoreFdKHR vkImportSemaphoreFdKHR;
};

/*
 * What is still alive on the device, by class.
 *
 * The point of this is the one number that matters at shutdown: how many
 * device children AVK still owns when it is about to call vkDestroyDevice.
 * Every one of them should be zero. A non-zero count is a leak; a NEGATIVE
 * count -- which is why these are signed -- is a double destruction, and a
 * double destruction of a driver object is a double free of the driver's host
 * allocation, which is how a clean-looking shutdown ends in "double free or
 * corruption (out)" with no AVK frame anywhere near the abort.
 *
 * Counted at the two or three places each class is actually created and
 * destroyed, so the numbers cannot drift from the code the way a hand-kept
 * tally does.
 */
struct avk_live_objects {
	int64_t images;
	int64_t image_views;
	int64_t device_memory;
	int64_t buffers;
	int64_t samplers;
	int64_t pipelines;
	int64_t pipeline_layouts;
	int64_t descriptor_set_layouts;
	int64_t descriptor_pools;
	int64_t command_pools;
	int64_t semaphores;
	int64_t query_pools;

	/* AVK-level wrappers, which are host allocations rather than device
	 * children but have exactly the same ownership question. */
	int64_t avk_images;
	int64_t avk_uploads;
	int64_t retire_entries;
};

struct avk_device {
	struct avk_instance *instance;   /* borrowed */
	VkPhysicalDevice phys;
	VkDevice dev;
	struct avk_caps caps;
	struct avk_device_api api;

	VkQueue graphics_queue;

	/* The device's own DRM fd. OWNED: dup()ed from whatever was passed in, so
	 * the caller's lifetime and ours are independent. Handing a renderer a
	 * borrowed fd and then closing it elsewhere is the kind of bug that shows
	 * up as a random EBADF three subsystems away. */
	int drm_fd;

	/* Everything the device does is ordered on this one timeline. A frame,
	 * an upload and a readback all take points on it, so "has the GPU
	 * finished with this resource" is a single integer comparison rather
	 * than a fence per object. */
	VkSemaphore timeline;
	uint64_t timeline_next;   /* next value to be signalled */

	VkPipelineCache pipeline_cache;
	char *pipeline_cache_path;   /* owned, may be NULL */

	struct avk_live_objects live;
	/* Stable identity for AVK-level objects, so a lifecycle complaint names
	 * something a log can be searched for rather than an address that has
	 * already been recycled. */
	uint64_t next_object_id;
	/* Ownership violations detected rather than crashed on. Every one of
	 * these is a bug; zero is the only correct value. */
	uint64_t lifecycle_violations;

	/*
	 * ── STAGING BUFFERS ARE EXPENSIVE THE FIRST TIME THEY ARE TOUCHED ────
	 *
	 * Not to allocate -- to *fault*. A freshly mapped 56MB staging buffer is
	 * 13691 pages the process has never touched, and the first memcpy into it
	 * takes every one of those faults inline: measured at 52ms for a copy
	 * that runs in 4ms once the pages exist, which is 1.1GB/s against
	 * 14GB/s. A client that presents a fresh wl_buffer every frame allocated
	 * a fresh staging buffer every frame and paid it every frame.
	 *
	 * So a retired staging buffer is kept rather than freed, and the next
	 * allocation of a size that fits takes it warm. The retire queue already
	 * guarantees the GPU is finished with it, which is exactly the condition
	 * that makes reuse safe -- this cache adds no new ordering rule, it only
	 * declines to throw away what that queue hands back.
	 */
	struct avk_upload *staging_cache[4];
	uint32_t staging_cache_count;
	VkDeviceSize staging_cache_bytes;
	uint64_t staging_reused;    /* allocations served warm */
	uint64_t staging_created;   /* allocations that had to fault */
};

/* Free every staging buffer the cache is holding. Shutdown only: the live
 * object census runs immediately afterwards and must see zero. */
void avk_staging_cache_drain(struct avk_device *dev);

/* Both no-ops when dev is NULL, because a failed create unwinds through paths
 * that may not have one yet. */
#define AVK_LIVE_INC(dev, field) \
	do { if ((dev) != NULL) { (dev)->live.field++; } } while (0)
#define AVK_LIVE_DEC(dev, field) \
	do { if ((dev) != NULL) { (dev)->live.field--; } } while (0)

/* Log what AVK still owns. Called immediately before vkDestroyDevice, where
 * every count must be zero, and available over IPC while running. Returns the
 * number of classes with a non-zero count. */
int avk_device_log_live_objects(const struct avk_device *dev, const char *when);

/*
 * Create a device for the DRM node `drm_fd` refers to.
 *
 * `drm_fd` is duplicated, not taken: the caller keeps its own. Pass -1 to let
 * avk pick a device on its own, which is only appropriate in tests -- the
 * compositor always knows which node it is presenting on and must say so, or
 * the client and the compositor can end up on different GPUs with no
 * diagnosis available afterwards.
 */
struct avk_device *avk_device_create(struct avk_instance *inst, int drm_fd);
void avk_device_destroy(struct avk_device *dev);

/* Wait for the GPU to finish everything in flight. Shutdown and device-loss
 * only -- a frame path that calls this is a bug. Separate from
 * avk_device_destroy() so a caller can establish quiescence BEFORE destroying
 * the resources submitted work still refers to. */
void avk_device_wait_idle(struct avk_device *dev);

void avk_device_log_caps(const struct avk_device *dev);

/* The value the GPU has actually reached. Never blocks. */
uint64_t avk_device_timeline_value(struct avk_device *dev);

/* Reserve the next timeline point for a submission. The caller signals it. */
uint64_t avk_device_timeline_reserve(struct avk_device *dev);

/* Block until the timeline reaches `value`, or `timeout_ns` elapses.
 * NOT for the frame path -- see docs/vulkan-native-architecture.md §5.7. This
 * exists for teardown, for readback, and for genuine ring backpressure. */
bool avk_device_timeline_wait(struct avk_device *dev, uint64_t value,
	uint64_t timeout_ns);

/* Attach a name to a Vulkan object so validation messages, RenderDoc captures
 * and driver crash dumps say "avk output DP-1 scene image" instead of
 * "VkImage 0x5591...". No-op when debug utils is unavailable. */
void avk_device_name_object(struct avk_device *dev, VkObjectType type,
	uint64_t handle, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

/* Write the pipeline cache back to disk. Called at shutdown; safe to call
 * when there is no cache path. */
void avk_device_save_pipeline_cache(struct avk_device *dev);

#endif /* AVK_DEVICE_H */
