#ifndef AVK_DMABUF_H
#define AVK_DMABUF_H

#include "../image/avk_image.h"
#include "../command/avk_command.h"
#include "../command/avk_retire.h"
#include "avk_format_table.h"

/*
 * DMA-BUF import.
 *
 * This is the file the Electron/Chromium blank-window class lives or dies in,
 * so the reasoning is written down rather than left in a commit message.
 *
 * A client hands over a buffer as a set of file descriptors plus a format, a
 * modifier and per-plane offsets and strides. The modifier is the tiling: it
 * tells the GPU how bytes are arranged, and without it a buffer is an
 * uninterpretable blob. Vulkan's VK_EXT_image_drm_format_modifier takes the
 * modifier explicitly, which is good -- and means that a buffer arriving with
 * DRM_FORMAT_MOD_INVALID, meaning "the producer did not say", has no direct
 * import path. The driver never enumerates INVALID, so an exact-match lookup
 * can never find it.
 *
 * The wrong answers to that, both of which exist in the wild:
 *
 *   - Treat INVALID as LINEAR. It is not. A tiled buffer read as linear
 *     produces a recognisable diagonal-smear corruption, or on a compressed
 *     modifier, noise. This is worse than a blank window because it looks like
 *     a rendering bug in the client.
 *   - Fail the import and render nothing. This is what happens today, and it
 *     is why Electron windows are blank.
 *
 * The right answer is to find out what the buffer really is. The buffer was
 * allocated by a real driver on a real device and it has a real layout; the
 * question is only who will tell us.
 *
 * The obvious idea -- import the fd through GBM's legacy implicit path and ask
 * the resulting bo for its modifier -- was tried and MEASURED, and it does not
 * work on Mesa. A buffer allocated with an explicitly tiled modifier
 * (0x0200000028A01F04 on this Navi31), exported, and re-imported through
 * GBM_BO_IMPORT_FD comes back reporting DRM_FORMAT_MOD_INVALID. Mesa's
 * implicit path genuinely discards the modifier rather than re-deriving it.
 * So the rung is not assumed either way: avk PROBES it at startup, on this
 * driver, with a real buffer, and only uses it if it works. Capability
 * detection, not a vendor branch and not wishful thinking.
 *
 * What does work everywhere is that GBM can still *import* the buffer, and the
 * driver behind it does know the layout even when it will not name it. So the
 * pixels can be read out linearly with gbm_bo_map(), which detiles, and
 * uploaded. That costs a copy and it is always correct.
 *
 * The ladder, in order, with each rung logged:
 *
 *   0. explicit modifier          -> direct import, zero copy
 *   1. INVALID, and this driver's GBM can recover the modifier
 *                                 -> direct import, zero copy
 *                                    (probed at startup; false on Mesa)
 *   2. INVALID, unrecoverable     -> gbm_bo_map + staged upload. The driver
 *                                    detiles; we never interpret the tiling
 *                                    ourselves. Slow and correct.
 *   3. nothing worked             -> fail LOUDLY with format, modifier, every
 *                                    plane's fd/offset/stride, the device, and
 *                                    which rungs were tried
 *
 * What is NOT on the ladder, at any rung: importing an INVALID buffer as
 * LINEAR. A tiled buffer read as linear is not a blank window, it is a
 * corrupt one, and a corrupt window is harder to diagnose than a missing one
 * because it looks like the client's bug.
 *
 * And separately, the real fix at the protocol level: DMA-BUF feedback built
 * from this device's actual capability table, so a conforming client never
 * allocates a buffer that lands here in the first place.
 */

struct avk_dmabuf_attributes {
	int32_t width, height;
	uint32_t format;          /* DRM fourcc */
	uint64_t modifier;        /* may be DRM_FORMAT_MOD_INVALID */
	int n_planes;
	uint32_t offset[AVK_MAX_PLANES];
	uint32_t stride[AVK_MAX_PLANES];
	int fd[AVK_MAX_PLANES];   /* BORROWED -- the importer dups what it keeps */
};

struct avk_dmabuf_importer {
	struct avk_device *dev;      /* borrowed */
	struct avk_format_table table;

	/* A GBM device on the same DRM node, used only to recover the modifier of
	 * an implicit buffer. NULL if GBM could not be opened, in which case rung
	 * 1 is unavailable and that is said out loud once at startup rather than
	 * once per failed import. */
	struct gbm_device *gbm;

	/* Whether this driver's GBM can recover an implicit buffer's modifier.
	 * Probed once at startup with a real allocation, because the answer is a
	 * driver property and guessing it either way costs correctness. */
	bool gbm_recovers_modifiers;

	/* Uploads for rung 2 get their own command ring, so a slow client's copy
	 * never queues behind -- or in front of -- a frame. */
	struct avk_cmd_ring upload_ring;
	struct avk_retire_queue retire;

	/* Counters, so "how often does the slow path run" is measurable rather
	 * than a matter of opinion. */
	uint64_t imports_explicit;
	uint64_t imports_recovered;
	uint64_t imports_copied;
	uint64_t imports_failed;
};

bool avk_dmabuf_importer_init(struct avk_dmabuf_importer *importer,
	struct avk_device *dev);
void avk_dmabuf_importer_finish(struct avk_dmabuf_importer *importer);

/*
 * Import a client buffer.
 *
 * Returns NULL only when every rung of the ladder failed, and always after
 * logging exactly why. The returned image records which rung it came from in
 * `origin`.
 *
 * `for_render` selects the render capability set instead of the texture one;
 * pass false for client buffers.
 */
struct avk_image *avk_dmabuf_import(struct avk_dmabuf_importer *importer,
	const struct avk_dmabuf_attributes *attribs, bool for_render);

/*
 * What modifier does this dma-buf really have?
 *
 * Rung 1 on its own. Returns DRM_FORMAT_MOD_INVALID when it cannot be
 * recovered, which on Mesa is always -- see the probe in the .c file.
 */
uint64_t avk_dmabuf_recover_modifier(struct avk_dmabuf_importer *importer,
	const struct avk_dmabuf_attributes *attribs);

/* Retire anything the upload path finished with. Cheap; call it per frame. */
void avk_dmabuf_importer_collect(struct avk_dmabuf_importer *importer);

void avk_dmabuf_importer_log_stats(const struct avk_dmabuf_importer *importer);

#endif /* AVK_DMABUF_H */
