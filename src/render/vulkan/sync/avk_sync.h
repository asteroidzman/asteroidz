#ifndef AVK_SYNC_H
#define AVK_SYNC_H

#include "../device/avk_device.h"

/*
 * The fence bridge: Vulkan on one side, the rest of the kernel on the other.
 *
 * AVK finishes a frame on the GPU and then has to hand the buffer to a display
 * engine that knows nothing about VkSemaphore. There are exactly three honest
 * ways to close that gap, and this file is the two that do not stall:
 *
 *   1. Export the render completion as a sync_file and give it to KMS, either
 *      through a drm_syncobj timeline or by attaching it to the target
 *      dma-buf's reservation object. The display engine then waits on the GPU,
 *      asynchronously, in the kernel.
 *   2. Import a sync_file back as a semaphore, so the GPU waits for someone
 *      else -- KMS releasing a scan-out buffer, a client finishing a draw --
 *      without the CPU being involved.
 *
 * The third way is vkQueueWaitIdle() before the commit, which is what a
 * renderer does when it has neither of the above, and it is what AVK must
 * never do. It costs a full pipeline drain every frame and it converts a
 * synchronisation bug into a latency bug, which is much harder to find later.
 *
 * A sync_file is a kernel fence with a file descriptor. It is the lingua
 * franca here precisely because it is not a Vulkan object, not a DRM object,
 * and not a GBM object: every subsystem in the chain can either produce one or
 * consume one, and none of them has to understand the others.
 *
 * Nothing in this file knows what an output, a buffer or a compositor is. Its
 * inputs and outputs are VkSemaphores and file descriptors, which is what lets
 * it be tested without a display -- and what keeps the wlroots vocabulary on
 * the other side of the boundary in src/render/az_avk.h.
 */

/*
 * How many wait-semaphore slots the bridge cycles through.
 *
 * A sync_file import is TEMPORARY: the payload is consumed by the wait that
 * uses it, so a slot may not be re-imported until the submission that waited on
 * it has completed. Cycling through AVK_FRAMES_IN_FLIGHT * 2 slots means a slot
 * is reused only after the command ring has already come all the way round,
 * which the ring itself enforces. `reuse_hazards` counts the times that
 * reasoning turned out to be wrong, because an invariant nothing checks is a
 * comment.
 */
/*
 * Raised from FRAMES_IN_FLIGHT*2 when client buffers started contributing
 * waits. Two per frame covered the display engine's fence and nothing else;
 * a frame now waits on an acquire fence for every client surface it samples,
 * so the per-frame draw on this pool is bounded by AZ_AVK_MAX_ACQUIRE rather
 * than by 1. The reuse argument is unchanged -- a slot is safe once the
 * command ring has come all the way round -- but it only holds while
 * SLOTS/FRAMES_IN_FLIGHT stays above the most any single frame takes, which
 * is what reuse_hazards exists to catch if this is ever sized wrong.
 */
#define AVK_SYNC_WAIT_SLOTS (AVK_FRAMES_IN_FLIGHT * 40)

struct avk_sync_slot {
	VkSemaphore semaphore;
	/* The device timeline value of the submission expected to consume this
	 * slot's payload. 0 when the slot has never been used. */
	uint64_t consumed_at;
};

struct avk_sync {
	struct avk_device *dev;

	/*
	 * The semaphore the frame submission signals, and which is then exported.
	 *
	 * One, not a ring: exporting a sync_file has copy transference, so
	 * vkGetSemaphoreFdKHR hands back the payload and resets the semaphore to
	 * unsignalled in the same call. The export happens immediately after the
	 * submit that signals it, so there is never a second pending signal
	 * outstanding on it.
	 */
	VkSemaphore export_sem;

	struct avk_sync_slot waits[AVK_SYNC_WAIT_SLOTS];
	uint32_t next_wait;

	uint64_t exports;
	uint64_t imports;
	uint64_t export_fails;
	uint64_t import_fails;
	uint64_t reuse_hazards;
};

/*
 * Prepare the bridge.
 *
 * Fails when the device cannot both export and import a binary semaphore as a
 * sync_file. That is not a soft limitation: without it there is no way to hand
 * a fence to KMS, and the caller must not present through AVK at all.
 */
bool avk_sync_init(struct avk_sync *sync, struct avk_device *dev,
	const char *name);
void avk_sync_finish(struct avk_sync *sync);

/*
 * The semaphore to add as an extra signal on the frame's submission.
 *
 * Add it, submit, then call avk_sync_export_sync_file(). Exporting without a
 * signal pending is an error the driver will report.
 */
static inline VkSemaphore avk_sync_signal_semaphore(struct avk_sync *sync) {
	return sync->export_sem;
}

/*
 * Take the pending render completion out as a sync_file.
 *
 * Returns false if the export failed. On success `*out_fd` is an owned fd, or
 * -1 meaning the work is already complete and there is nothing to wait for.
 * Those two are separate answers on purpose: one is a bug and the other is a
 * frame that finished early, and a function that returned -1 for both would
 * make the caller treat them the same.
 *
 * Resets the export semaphore, so the next frame may signal it immediately.
 */
bool avk_sync_export_sync_file(struct avk_sync *sync, int *out_fd);

/*
 * Turn a sync_file into something the GPU can wait on.
 *
 * Takes ownership of `sync_file_fd` unconditionally -- on success the driver
 * owns it, on failure this closes it. One owner and one place that closes it:
 * the alternative is a caller that leaks an fd on the error path it never
 * tests, and an fd leak in a per-frame function reaches the process limit in
 * about an hour.
 *
 * Returns VK_NULL_HANDLE on failure. The returned semaphore is valid until the
 * submission that waits on it completes; it must be waited on exactly once.
 */
VkSemaphore avk_sync_import_sync_file(struct avk_sync *sync, int sync_file_fd);

/* ── dma-buf reservation objects ─────────────────────────────────────────────
 *
 * The other half of the bridge, for the case where there is no drm_syncobj
 * timeline to hand the fence to: attach it to the buffer itself.
 *
 * Every dma-buf carries a reservation object -- the kernel's own list of
 * fences for that memory -- and DMA_BUF_IOCTL_IMPORT_SYNC_FILE adds one to it.
 * A consumer that does implicit synchronisation (which KMS does) then waits on
 * it without anybody having to pass it a fence explicitly. This is what makes
 * a renderer that only speaks explicit sync interoperable with a display path
 * that only speaks implicit sync, and it is why AVK is correct on a backend
 * with no timeline support instead of merely lucky.
 *
 * Both calls return false / -1 with errno set when the running kernel is too
 * old for the ioctl (ENOTTY, before 5.20). That is a real answer and the caller
 * must treat it as "AVK cannot present here", not as "probably fine".
 */

/* Flags are DMA_BUF_SYNC_READ / DMA_BUF_SYNC_WRITE. A frame we rendered into
 * the buffer is a WRITE. Takes ownership of `sync_file_fd` either way. */
bool avk_sync_dmabuf_attach(int dmabuf_fd, uint32_t flags, int sync_file_fd);

/* The fences already on the buffer, as a sync_file to wait on before touching
 * it. Pass DMA_BUF_SYNC_WRITE to get everything (reads and writes) -- that is
 * what a writer has to wait for. Same two-answer convention as the export
 * above: false is a failure, `*out_fd == -1` is an idle buffer. */
bool avk_sync_dmabuf_fences(int dmabuf_fd, uint32_t flags, int *out_fd);

void avk_sync_log_stats(const struct avk_sync *sync, const char *name);

#endif /* AVK_SYNC_H */
