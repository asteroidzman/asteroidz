#ifndef AVK_TIMESTAMP_H
#define AVK_TIMESTAMP_H

#include "../device/avk_device.h"

/*
 * GPU timestamps, measured without ever making the CPU wait for them.
 *
 * M4C reported "GPU time = NOT MEASURED" and it was the honest answer: AVK
 * read timestampPeriod at device creation and nothing used it, so every
 * duration the renderer could quote was CPU command-recording time. That is
 * the wrong number in both directions -- it understates a shader-bound frame
 * and overstates a submission-bound one -- and M4D adds a full-screen
 * fragment workload, which is exactly the case it understates.
 *
 * THE RULE THIS FILE EXISTS TO KEEP. Measuring must not perturb what it
 * measures. The obvious implementation -- submit, wait, read -- would turn
 * every frame into a GPU round trip and make `cpu_sync_waits` nonzero, which
 * is the one counter AVK has promised since M3.5 stays at zero. So nothing
 * here waits:
 *
 *   frame N records its marks into slot N%3 and submits, remembering the
 *   timeline value that submission will signal;
 *
 *   some later frame calls avk_timestamps_collect(), which reads the device
 *   timeline ONCE and only reads back slots whose value the GPU has already
 *   passed. Work known to be complete has results already written.
 *
 * The readback additionally passes WITH_AVAILABILITY_BIT and no WAIT_BIT, so
 * even a driver that has not yet made the results visible returns VK_NOT_READY
 * instead of blocking. Belt and braces, deliberately: the timeline argument is
 * the correct one, and the availability bit is what makes "correct" not depend
 * on my reading of the spec being right.
 *
 * WRAP. Queries live in one pool, AVK_TS_MARKS per frame slot, indexed by the
 * COMMAND RING's slot -- the same index the gradient store uses, and for the
 * same reason: avk_cmd_ring_begin() has already waited for that slot's previous
 * submission before this frame can reset its queries. So the ring wraps every
 * three frames and there is no hazard in it. A slot whose result was never
 * collected before it came round again simply loses that sample, which is a
 * gap in statistics and not a correctness problem; `dropped` counts them.
 *
 * NOT A PROFILER. Four marks, one pool, no per-pass hierarchy, no string
 * labels. Enough to answer "what does a frame cost on the GPU, and how much of
 * it is effects", which is what M4D owes and what M4F will need.
 */

/*
 * The marks. Two of them, and the reason there are not four is worth stating
 * because the obvious design has four.
 *
 * WHY THERE IS NO "EFFECTS PHASE" MARK. A timestamp pair measures a CONTIGUOUS
 * span of the command stream. AVK draws in scene order, and a window's shadow
 * is drawn immediately beneath that window -- so shadow draws are interleaved
 * with content draws throughout the frame, not gathered into a phase.
 * Bracketing the first and last shadow draw would measure "the span of the
 * frame that happens to contain shadow work", which is very nearly the whole
 * frame and is not shadow cost. Quoting it as shadow cost would be the same
 * class of error as calling CPU recording time GPU time, which is the thing
 * this file exists to stop.
 *
 * Isolating an interleaved primitive's cost honestly means differencing two
 * frames that differ only in whether it is drawn -- which is how M4C measured
 * a gradient and how M4D measures a shadow. When M4F adds blur, that IS a
 * separable pass with its own barriers, and a phase pair can be added then,
 * driven by the code that needs it rather than waiting speculatively for it.
 */
enum avk_ts_mark {
	AVK_TS_FRAME_BEGIN = 0,
	AVK_TS_FRAME_END,
	AVK_TS_MARKS,
};

struct avk_ts_slot {
	/* The timeline point the submission carrying these marks will signal.
	 * 0 means nothing is outstanding in this slot. */
	uint64_t timeline_value;
	/* Which marks were actually written this frame. A mark that was not
	 * written has no result, and reading its query would return a stale value
	 * from three frames ago -- indistinguishable from a plausible one. */
	uint32_t written;
};

struct avk_timestamps {
	struct avk_device *dev;
	VkQueryPool pool;

	/*
	 * Off, and harmless, when the device cannot do this. Both conditions are
	 * real: timestampPeriod is 0 on a device with no timestamp support, and
	 * timestampValidBits is 0 on a QUEUE FAMILY that cannot write them even
	 * where the device can. Rendering never consults this flag; only
	 * measurement does.
	 */
	bool supported;
	/* Mask of the bits the queue family actually writes. The upper bits of a
	 * raw query result are UNDEFINED, not zero, so subtracting two unmasked
	 * values on a 36-bit-valid device produces a duration in the exabytes. */
	uint64_t valid_mask;
	float period_ns;

	struct avk_ts_slot slots[AVK_FRAMES_IN_FLIGHT];

	/* Latest complete sample, in nanoseconds. Zero until one is collected. */
	uint64_t gpu_frame_ns;
	/* A sum and a count, so a mean over a run is available without keeping
	 * every sample. */
	uint64_t gpu_frame_ns_total;
	uint64_t samples;
	uint64_t dropped;
};

/*
 * Returns true if timestamps are available and false if they are not. A false
 * return is NOT a failure: the struct is left in a valid, disabled state where
 * every other call is a no-op, because a device that cannot measure itself
 * must still be able to draw.
 */
bool avk_timestamps_init(struct avk_timestamps *ts, struct avk_device *dev);
void avk_timestamps_finish(struct avk_timestamps *ts);

/*
 * Begin a frame in `slot`. Resets that slot's queries on the command buffer.
 *
 * Must be called after avk_cmd_ring_begin() has handed out the slot, which is
 * what guarantees the previous submission through it has completed and its
 * queries are therefore safe to reset.
 */
void avk_timestamps_begin(struct avk_timestamps *ts, VkCommandBuffer cb,
	uint32_t slot);

/*
 * Write one mark. `stage` is the pipeline stage the timestamp is taken AFTER;
 * BOTTOM_OF_PIPE for an end-of-frame mark, TOP_OF_PIPE for a beginning.
 */
void avk_timestamps_mark(struct avk_timestamps *ts, VkCommandBuffer cb,
	uint32_t slot, enum avk_ts_mark mark, VkPipelineStageFlags2 stage);

/* Record the timeline value the frame's submission will signal. */
void avk_timestamps_submitted(struct avk_timestamps *ts, uint32_t slot,
	uint64_t timeline_value);

/*
 * Read back whatever the GPU has finished. Never blocks; call once per frame
 * beside avk_retire_collect(). Returns the number of samples consumed.
 */
size_t avk_timestamps_collect(struct avk_timestamps *ts);

/*
 * Ticks from `a` to `b`, both masked to `mask`, correct across the counter's
 * wrap within its valid bits.
 *
 * Exposed for tests rather than kept static, because the case it exists for
 * cannot be produced on this hardware: the GPU here reports 64 valid bits, so
 * masking is the identity and a wrap needs 585 years at 1 ns/tick. A test that
 * could only run against real queries would assert nothing about either. This
 * is the synthetic-case remedy for coverage by coincidence, and the reason the
 * function has a name.
 */
uint64_t avk_ts_ticks_between(uint64_t mask, uint64_t a, uint64_t b);

#endif /* AVK_TIMESTAMP_H */
