#ifndef AVK_ORACLE_H
#define AVK_ORACLE_H

/*
 * ── THE FIRST-DIVERGENCE ORACLE ───────────────────────────────────────────
 *
 * TEST ONLY. Off unless AZ_FRAME_ORACLE=1, and it costs one branch when off.
 *
 * M4F.2C.4 left a defect that could only be seen after the fact: a 180-degree
 * output with a blur whose source crosses an output seam ended an interaction
 * with 13-22 stale pixels, about two runs in three. Everything known about it
 * was a POSTMORTEM -- the interaction finished, a forced repaint was asked for,
 * and the difference was counted. That says a frame somewhere in the sequence
 * was wrong. It does not say WHICH, and it cannot say WHY, because by the time
 * the measurement is taken the frame that stranded the pixels is gone.
 *
 * This turns that into a boundary. For every frame, against the SAME immutable
 * scene snapshot:
 *
 *     production partial render  ->  P     (the real scan-out target)
 *     independent full render    ->  F     (a renderer-owned image)
 *
 * and it reports the FIRST frame where P != F. Everything downstream of that --
 * which blur owns the pixels, whether the prefix was already wrong, whether the
 * blur chain was -- is then a question about one identified frame rather than
 * about a run.
 *
 * ── WHAT IT MAY NOT DO ────────────────────────────────────────────────────
 *
 * The reference render must not advance the compositor. It does not consume
 * damage, rotate a damage ring, touch buffer-age state, alter the scene, change
 * transient lifetime rules or schedule a frame: it is a second, independent
 * render of a snapshot the compositor has already finished with, into an image
 * nothing presents.
 *
 * It DOES move the renderer's own counters, because it goes through
 * avk_render_frame() exactly like a real frame and that is the whole point --
 * a reference produced by a different code path would be a different renderer.
 * So the oracle says so, loudly, once: a run with AZ_FRAME_ORACLE=1 must not
 * have its avk-stats asserted on. That is a stated limitation of a diagnostic
 * mode, not a silent one.
 *
 * ── TAPS ──────────────────────────────────────────────────────────────────
 *
 * Three boundaries, one mechanism. A tap is a graph pass that copies a region
 * of an image into a host-visible buffer, declared with AVK_USE_TRANSFER_READ
 * so the graph emits the barriers -- including the acquire and release of the
 * foreign scan-out target, which is why the production output can be read at
 * all without hand-rolling a queue-family transfer.
 *
 *     AVK_TAP_PREFIX   the regional scene-prefix transient, AFTER the segment
 *                      has reconstructed it and BEFORE the blur chain samples
 *                      it. Boundary 1.
 *     AVK_TAP_BLUR     the same image after the chain has written its result
 *                      back into it. Boundary 2.
 *     AVK_TAP_OUTPUT   the frame's target, after the output segment and before
 *                      presentation. Boundary 3.
 *
 * Comparing P's tap against F's tap at the same boundary answers, for the first
 * divergent frame, exactly one of:
 *
 *     A  PREFIX WRONG                     segmented source reconstruction
 *     B  PREFIX RIGHT / BLUR WRONG        the chain's own coordinate mapping
 *     C  PREFIX+BLUR RIGHT / OUTPUT WRONG damage, scissor, or buffer history
 */

#include "../avk.h"
#include "../graph/avk_graph.h"
#include "../image/avk_image.h"
#include "avk_scene.h"

#include <pixman.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum avk_oracle_tap_kind {
	AVK_TAP_PREFIX = 0,
	AVK_TAP_BLUR,
	AVK_TAP_OUTPUT,
	AVK_TAP_KIND_COUNT,
};

/* Which of the two renders a tap belongs to. Not a "double buffer": the two
 * are semantically different images and confusing them is the one mistake that
 * would make every comparison trivially pass. */
enum avk_oracle_pass {
	AVK_ORACLE_PRODUCTION = 0,
	AVK_ORACLE_REFERENCE,
	AVK_ORACLE_PASS_COUNT,
};

struct avk_oracle_slot {
	enum avk_oracle_pass pass;
	enum avk_oracle_tap_kind kind;
	/* The COMMAND INDEX the tap belongs to, so a prefix tap can be matched to
	 * the blur that owns it across the two renders. AVK_TAP_OUTPUT uses 0. */
	size_t cmd_index;
	struct avk_box box;      /* region copied, in the image's own pixels */
	VkDeviceSize offset;     /* into the readback buffer */
	uint32_t stride;         /* bytes per row */
	bool valid;
	/*
	 * WHERE THE TWO ARE ALLOWED TO BE COMPARED, in the image's own pixels.
	 *
	 * Not a convenience. A production prefix transient is written only inside
	 * prefix_rebuild and everything else in it is deliberately undefined -- it
	 * is loadOp DONT_CARE, and under AZ_TRANSIENT_POISON it is a colour nothing
	 * produces. A reference rendered whole writes all of it. Comparing the two
	 * over the full capture therefore reports a difference for every pixel the
	 * design says nothing about, which is not a defect and would bury the one
	 * that is.
	 *
	 * Empty means "the whole tap", which is what the output target wants: every
	 * presented pixel is defined and must match.
	 */
	bool has_mask;
	pixman_region32_t mask;
};

#define AVK_ORACLE_MAX_SLOTS 64

struct avk_oracle {
	bool enabled;
	struct avk_device *dev;

	/* One host-visible buffer, bump-allocated per frame. Grown by waiting for
	 * idle first, which is legal here and nowhere else. */
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *mapped;
	VkDeviceSize capacity;
	VkDeviceSize used;

	struct avk_oracle_slot slots[AVK_ORACLE_MAX_SLOTS];
	size_t slot_len;
	/* Bumped when a tap could not be recorded, so "no divergence" can never be
	 * reported by an oracle that quietly stopped looking. */
	uint64_t dropped;
	/*
	 * Growths of the readback buffer, which INVALIDATE every slot already
	 * recorded into the old allocation.
	 *
	 * This is counted rather than hidden because it was a real defect in the
	 * first version of this file: the buffer started at 4 MB, a frame with four
	 * blurs needed 24, and the growth happened during the reference render --
	 * so the production taps recorded into freed memory and every pixel of two
	 * frames "differed". A whole-frame mismatch with a worst error of 255 is
	 * exactly what a broken oracle looks like, and it is indistinguishable from
	 * a catastrophic renderer bug unless the growth is visible.
	 */
	uint64_t invalidated;

	enum avk_oracle_pass pass;

	/* The reference target, one per extent+format actually seen. */
	struct avk_image *ref_image;

	/* Set once the run has reported a first divergence, so the forensic dump
	 * covers the frame before, the frame itself and the frame after and then
	 * goes quiet. */
	bool armed;
	int64_t divergent_frame;
	int tail;
};

bool avk_oracle_enabled(void);
void avk_oracle_init(struct avk_oracle *o, struct avk_device *dev);
void avk_oracle_finish(struct avk_oracle *o);

/* Start recording taps for one render. Clears the previous render's slots for
 * that pass only. */
void avk_oracle_begin(struct avk_oracle *o, enum avk_oracle_pass pass);

/*
 * Declare a copy of `box` of `image` into the readback buffer, as a graph pass.
 * `resource` is the image's handle in `graph`. Returns false if the tap could
 * not be recorded, having counted it.
 */
/*
 * `mask` is in the IMAGE's own pixels and may be NULL for "all of `box`". It is
 * copied, so the caller's region can go away.
 */
bool avk_oracle_tap(struct avk_oracle *o, struct avk_graph *graph,
	uint32_t resource, struct avk_image *image, struct avk_box box,
	enum avk_oracle_tap_kind kind, size_t cmd_index,
	const pixman_region32_t *mask);

/* The pixels of a tap, or NULL. Only valid once the submission that recorded it
 * has completed. */
const uint8_t *avk_oracle_pixels(const struct avk_oracle *o,
	enum avk_oracle_pass pass, enum avk_oracle_tap_kind kind, size_t cmd_index,
	struct avk_box *box_out, uint32_t *stride_out);

/*
 * A reference target of exactly `width` x `height` in `format`.
 *
 * EXACTLY, which is why this does not come from the transient pool: that pool
 * rounds up to a granularity and avk_render_frame() takes the frame's size from
 * the target's extent, so a pooled image would render a different-sized frame
 * and every comparison would be against the wrong picture.
 */
struct avk_image *avk_oracle_ref_target(struct avk_oracle *o, VkFormat format,
	uint32_t width, uint32_t height);

/*
 * Compare two taps of the same kind and command index, over the PRODUCTION
 * tap's mask -- the region production claims to have reconstructed.
 *
 * Returns the number of differing pixels; fills the mismatch bounding box and
 * the worst absolute per-channel error. A missing tap on either side counts as
 * -1, and mismatched geometry -2; neither is a pass.
 */
/*
 * The first differing pixel, in the image's own coordinates, with both values.
 *
 * A bounding box and a worst error say a difference exists and how big it is.
 * They do not let one pixel be traced, which is the thing an end-to-end trace
 * of a stale pixel actually needs -- "production had 118,118,118,255 where a
 * full render of the same snapshot had 119,119,119,255" is a fact about one
 * address, and the bbox is a summary of many.
 */
struct avk_oracle_sample {
	bool found;
	int x, y;
	uint8_t production[4];
	uint8_t reference[4];
};

int64_t avk_oracle_compare(const struct avk_oracle *o,
	enum avk_oracle_tap_kind kind, size_t cmd_index, struct avk_box *bbox_out,
	int *worst_out, struct avk_oracle_sample *sample_out);

/* Whether a tap of this kind and index exists for one render. Distinguishes
 * "the two agree" from "one of them never ran the blur at all", which are
 * opposite findings and were previously the same silence. */
bool avk_oracle_has_tap(const struct avk_oracle *o, enum avk_oracle_pass pass,
	enum avk_oracle_tap_kind kind, size_t cmd_index);

#endif
