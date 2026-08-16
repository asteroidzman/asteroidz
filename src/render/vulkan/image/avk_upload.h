#ifndef AVK_UPLOAD_H
#define AVK_UPLOAD_H

#include "../command/avk_command.h"
#include "avk_image.h"

/*
 * Getting CPU pixels onto the GPU.
 *
 * Two callers want this and they want it for opposite reasons, which is why it
 * is a file rather than a helper inside one of them:
 *
 *  - The dma-buf copy rung (avk_dmabuf.c) uploads ONCE. The buffer it reads
 *    from is a mapped gbm bo it is about to throw away, and the staging memory
 *    is dead the moment the GPU has read it.
 *  - The SHM client path uploads EVERY TIME THE CLIENT COMMITS. Its staging
 *    memory should be allocated once and reused, because a per-frame
 *    vkAllocateMemory on a surface that is redrawing at 60Hz is exactly the
 *    per-frame allocation the architecture rules out.
 *
 * So `struct avk_upload` is a *reusable* staging buffer, kept persistently
 * mapped, that grows when it has to and otherwise costs nothing. The one-shot
 * caller simply retires it after a single use.
 *
 * The image side is shared too. An uploaded image is ours: OPTIMAL tiling, no
 * external memory, no modifier. That is the one case where "no modifier" is
 * honest rather than a lie about a dma-buf.
 */

struct avk_upload {
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size;
	void *mapped;   /* persistently mapped for the buffer's whole life */

	/* The timeline point of the last submission that READ this staging
	 * buffer, and therefore the point at which it becomes safe to destroy.
	 *
	 * Recorded because it was previously not recorded, and both places that
	 * needed it made something up instead: the retire at destroy used
	 * "current + 1" -- a point that no submission owns and that, at teardown,
	 * nothing will ever signal -- and staging_ensure() used nothing at all,
	 * destroying a buffer that a copy might still be reading. Neither can be
	 * right, because neither is derived from the submission that used it. */
	uint64_t last_use;
};

/*
 * Find a memory type satisfying `want` among `type_bits`.
 *
 * Shared rather than duplicated: picking the wrong memory type is a class of
 * bug that only shows up on hardware with an unusual heap layout, and one
 * implementation is one thing to get right.
 */
bool avk_find_memory_type(struct avk_device *dev, uint32_t type_bits,
	VkMemoryPropertyFlags want, uint32_t *out);

/* Bytes per pixel, or 0 for formats that do not have a single answer (planar
 * YCbCr). The copy path refuses rather than computing a wrong size. */
uint32_t avk_format_bytes_per_pixel(VkFormat format);

/*
 * An image AVK owns, sized for `drm_format` at `width` x `height`, ready to be
 * written by avk_upload_image_write() and sampled afterwards.
 *
 * Returns NULL, having logged why, if the format is unknown to AVK or the
 * allocation failed. `origin` is recorded on the image so a later "why is this
 * window slow" has an answer.
 */
struct avk_image *avk_upload_image_create(struct avk_device *dev,
	uint32_t drm_format, uint32_t width, uint32_t height,
	enum avk_image_origin origin);

/*
 * The same, as a dim x dim x dim VOLUME. M6C's cLUT and nothing else.
 *
 * Separate from the 2D creator rather than a depth argument on it, because the
 * two have different callers and different failure modes: every 2D caller is a
 * client buffer whose extent is not ours to choose, and this one is a cube whose
 * edge is a compile-time constant. A shared signature would put a `1` at forty
 * call sites to serve one.
 *
 * avk_upload_image_write() then works unchanged -- pass `stride` = one row of
 * the cube and `height` = dim*dim rows, and the copy takes its image extent from
 * the image itself.
 */
struct avk_image *avk_upload_image_create_3d(struct avk_device *dev,
	uint32_t drm_format, uint32_t dim, enum avk_image_origin origin);

/*
 * Copy `height` rows of `stride` bytes from `pixels` into `image`, on `ring`.
 *
 * `waits` is passed through to the submission, which is how the caller keeps
 * the upload behind whatever last read the image without a CPU wait: pass a
 * timeline wait on the frame that sampled it.
 *
 * Leaves the image in SHADER_READ_ONLY_OPTIMAL and updates its `layout` and
 * `last_use`. Returns the timeline value the upload will signal, or 0.
 */
uint64_t avk_upload_image_write(struct avk_device *dev,
	struct avk_cmd_ring *ring, struct avk_upload *up, struct avk_image *image,
	const void *pixels, uint32_t stride, uint32_t height,
	const VkSemaphoreSubmitInfo *waits, uint32_t wait_count);

/*
 * The same, for a list of changed rectangles instead of the whole image.
 *
 * Each rectangle is copied ROW BY ROW out of the source, because a source row
 * is `stride` bytes apart and a rectangle's rows are not contiguous unless it
 * happens to span the full width. Copying `width * height * bpp` as one block
 * is the obvious shortcut and it produces a diagonally sheared rectangle the
 * moment the rectangle is narrower than the buffer -- which is every
 * interesting case.
 *
 * Rows are packed tightly into staging, so each region's bufferRowLength is
 * its own width rather than the source stride. Offsets are aligned to what
 * Vulkan requires of bufferOffset: a multiple of 4 and of the texel size.
 *
 * `bytes_copied` receives what was actually moved, which is the number the
 * whole exercise exists to reduce.
 */
struct avk_upload_rect {
	uint32_t x, y, width, height;
};

uint64_t avk_upload_image_write_regions(struct avk_device *dev,
	struct avk_cmd_ring *ring, struct avk_upload *up, struct avk_image *image,
	const void *pixels, uint32_t stride, uint32_t height,
	const struct avk_upload_rect *rects, uint32_t rect_count,
	uint64_t *bytes_copied,
	const VkSemaphoreSubmitInfo *waits, uint32_t wait_count);

/*
 * ── THE COPY, SEPARATED FROM EVERYTHING THAT MUST HAPPEN ON ONE THREAD ────
 *
 * The two functions above are plan, memcpy and submit fused together, which is
 * fine for a caller that is allowed to spend the memcpy where it stands. The
 * SHM client path is not: it is reached from a wl_surface commit on the
 * compositor's event loop, and while it runs the loop is not in poll(), so
 * libinput's fds are not read. A 6.5 MB memcpy there is measured on the mouse.
 *
 * So the same work is also available as three pieces, split exactly along the
 * line of what may leave the main thread:
 *
 *   plan    pure arithmetic. Decides which bytes move and where they land in
 *           staging. No Vulkan, no memory touched.
 *   pack    the memcpy, and NOTHING else. Reads `src`, writes `dst`, reads the
 *           plan. Touches no shared state, calls into no library, so it is
 *           safe on a worker thread.
 *   submit  the barriers, the copy command and the queue submission. Vulkan
 *           objects are externally synchronised, so this stays on the thread
 *           that owns the ring -- the main one.
 *
 * The fused functions are implemented on top of these, so the synchronous path
 * and the asynchronous one are the same code and cannot drift.
 */
enum { AVK_UPLOAD_MAX_REGIONS = 64 };

struct avk_upload_plan {
	/* One region per damage rectangle, or a single whole-image region. */
	struct avk_upload_rect rects[AVK_UPLOAD_MAX_REGIONS];
	VkDeviceSize offsets[AVK_UPLOAD_MAX_REGIONS];
	uint32_t rect_count;

	/* Bytes staging must hold, and bytes the pack will actually move. Equal
	 * for the whole-image plan; the region plan's total includes the padding
	 * between regions that alignment forces and pack does not write. */
	VkDeviceSize total;
	uint64_t bytes;

	uint32_t bpp;
	uint32_t stride;     /* SOURCE stride in bytes */
	uint32_t height;     /* SOURCE height in rows */

	/*
	 * Whole image, copied verbatim including the source's row padding, so the
	 * copy is one memcpy and bufferRowLength is stride/bpp. The region path
	 * repacks rows tightly instead, which is why it cannot use the same
	 * bufferRowLength -- see the note on avk_upload_image_write_regions().
	 */
	bool full;
};

/* Plan a whole-image copy. False when the format or the stride makes the copy
 * inexpressible, having said which. */
bool avk_upload_plan_full(struct avk_upload_plan *plan,
	const struct avk_image *image, uint32_t stride, uint32_t height);

/* Plan a copy of `rect_count` rectangles. False when a rectangle is outside
 * the image, there are more than this path packs, or nothing would move. */
bool avk_upload_plan_regions(struct avk_upload_plan *plan,
	const struct avk_image *image, uint32_t stride, uint32_t height,
	const struct avk_upload_rect *rects, uint32_t rect_count);

/*
 * Move the bytes. THREAD-SAFE and deliberately dependency-free: this is the
 * function a worker thread runs, and anything it touched beyond src, dst and
 * the plan would be a race waiting to be found by a user rather than by this
 * comment.
 */
void avk_upload_pack(const struct avk_upload_plan *plan, const void *src,
	void *dst);

/* Make staging at least `size` bytes, retiring the old buffer against the
 * submission that read it rather than destroying it under a copy in flight. */
bool avk_upload_staging_ensure(struct avk_device *dev, struct avk_upload *up,
	struct avk_retire_queue *retire, VkDeviceSize size);

/*
 * Record and submit a copy for staging that is ALREADY PACKED.
 *
 * `waits` is the caller's ordering against whatever last read the image --
 * for the client path, a timeline wait on image->last_use. Same contract as
 * avk_upload_image_write(): returns the timeline value, or 0.
 */
uint64_t avk_upload_submit_packed(struct avk_device *dev,
	struct avk_cmd_ring *ring, struct avk_upload *up, struct avk_image *image,
	const struct avk_upload_plan *plan,
	const VkSemaphoreSubmitInfo *waits, uint32_t wait_count);

/*
 * Copy into `image` straight out of a client's own wl_shm pool, imported as
 * device memory -- no memcpy on any thread.
 *
 * `src` is a VkBuffer bound to the imported pool and `src_offset` is where this
 * buffer's first row starts inside it. The plan's rectangles and stride are
 * read; its packed offsets are not, because nothing was packed.
 *
 * The caller MUST keep the client's wl_buffer locked until the returned
 * timeline point is reached. The staging path did not need that -- it had
 * already taken a copy of the bytes -- but here the GPU reads memory the client
 * draws into again as soon as it is released.
 */
uint64_t avk_upload_submit_host(struct avk_cmd_ring *ring, VkBuffer src,
	VkDeviceSize src_offset, struct avk_image *image,
	const struct avk_upload_plan *plan,
	const VkSemaphoreSubmitInfo *waits, uint32_t wait_count);

/* Release the staging buffer. Does NOT wait for the GPU -- retire it. */
void avk_upload_finish(struct avk_device *dev, struct avk_upload *up);

/* avk_retire_fn-shaped: finishes the upload and frees the struct itself. For
 * the one-shot caller. */
void avk_upload_retire(struct avk_device *dev, void *upload);

#endif /* AVK_UPLOAD_H */
