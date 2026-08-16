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

/* Release the staging buffer. Does NOT wait for the GPU -- retire it. */
void avk_upload_finish(struct avk_device *dev, struct avk_upload *up);

/* avk_retire_fn-shaped: finishes the upload and frees the struct itself. For
 * the one-shot caller. */
void avk_upload_retire(struct avk_device *dev, void *upload);

#endif /* AVK_UPLOAD_H */
