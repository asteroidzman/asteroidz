#ifndef AVK_TIMESTAMP_H
#define AVK_TIMESTAMP_H

#include "../device/avk_device.h"
#include "avk_stat.h"

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
/*
 * ── AND M4F DID ADD ONE, SO HERE IT IS ────────────────────────────────────
 *
 * Blur IS separable, which is exactly the condition the paragraph above set.
 * Every blur pass -- each blur's prefix replay and its whole down/up chain --
 * is declared BEFORE the output segment, so the span from the first prefix
 * replay to the last upsample is ONE CONTIGUOUS RANGE of the command stream
 * containing nothing else. That is what makes a pair honest here and dishonest
 * for shadows.
 *
 * WHAT IS NOT MEASURABLE THIS WAY, and is therefore not offered: the blur
 * COMPOSITE. A blur's result is drawn into the output by one draw call sitting
 * at that blur's index in the output segment, interleaved with ordinary
 * content draws. Bracketing it would measure "the span of the output segment
 * that happens to contain the composite", which is very nearly the whole
 * segment. gpu_blur_composite_ns does not exist; the cost is obtained by
 * differencing two frames that differ only in whether the composite runs.
 *
 * THE PHASE MARKS DESCRIBE THE FIRST CHAIN ONLY. With N blurs the stream is
 * prefix0, chain0, prefix1, chain1, ... so "all the prefix replays" is not a
 * contiguous range and cannot be one pair. PREFIX_END and DOWN_END are written
 * for the first chain; the up phase is only separable when there is exactly
 * one chain, and the slot records whether that was so.
 */
enum avk_ts_mark {
	AVK_TS_FRAME_BEGIN = 0,
	AVK_TS_FRAME_END,
	/* Before the first blur pass of the frame (TOP_OF_PIPE) and after the
	 * last one (BOTTOM_OF_PIPE). */
	AVK_TS_BLUR_BEGIN,
	AVK_TS_BLUR_END,
	/* After the FIRST chain's prefix replay, and after its last downsample. */
	AVK_TS_BLUR_PREFIX_END,
	AVK_TS_BLUR_DOWN_END,
	/*
	 * After the PENULTIMATE upsample -- so BLUR_UP_PENULT_END -> BLUR_END is
	 * the FINAL, full-resolution upsample alone.
	 *
	 * That pass is the one the M4F.2D decision is about: it rasterises at the
	 * full capture extent, it applies the effects, and its required region is
	 * the one the falsifier proved tight. It is a real graph pass boundary --
	 * `blur_up` and `blur_up_final` are separate passes already -- so this
	 * costs one more query and no restructuring. Only meaningful on a
	 * single-chain frame with at least two levels.
	 */
	AVK_TS_BLUR_UP_PENULT_END,
	AVK_TS_MARKS,
	/*
	 * "No mark." Deliberately equal to AVK_TS_MARKS, so the bounds check
	 * avk_timestamps_mark() already performs skips it and no call site needs a
	 * second kind of guard. A pass that wants only a begin or only an end
	 * passes this for the other.
	 */
	AVK_TS_NONE = AVK_TS_MARKS,
};

struct avk_ts_slot {
	/* The timeline point the submission carrying these marks will signal.
	 * 0 means nothing is outstanding in this slot. */
	uint64_t timeline_value;
	/* Whether this frame had EXACTLY ONE blur chain, which is the condition
	 * under which DOWN_END -> BLUR_END is the first chain's upsample phase
	 * rather than "everything after the first chain's downsamples". Recorded
	 * per slot because it is a property of the frame, not of the renderer. */
	bool single_chain;
	/*
	 * Whether this frame ran any blur work at all.
	 *
	 * A POPULATION FILTER, not a new measurement: the same FRAME_BEGIN ->
	 * FRAME_END pair is recorded, and this only decides which histogram it
	 * joins. gpu_frame over ALL frames answers a different question from
	 * gpu_frame over blur-bearing frames, and on a sparse-pulse fixture the
	 * two medians differ by more than the optimisation being measured -- 3840
	 * vs 6580 us, where the "frame" median was mostly idle frames.
	 *
	 * It lives in the SLOT because timestamp results are consumed later: the
	 * classification has to travel with the frame it describes, not be read
	 * off whatever the CPU is doing when the result finally comes back.
	 */
	bool blur_active;
	/*
	 * WHICH OUTPUT THIS FRAME WAS FOR, carried in the SLOT for the same
	 * reason blur_active is: a timestamp result arrives several frames later,
	 * and reading the name off whatever the CPU is doing when it lands
	 * attributes a 4K frame to whichever display happened to be rendering
	 * next. Every percentile in this milestone has been mixed across a
	 * 4K/144Hz output and a 1080p/60Hz one, and a 1080p frame is a quarter of
	 * the fill --- so the aggregate is bimodal and was being read as one
	 * population. A short fixed buffer, because this must not own memory.
	 */
	char output[16];
	/*
	 * The REGION SIZES this frame worked on, in pixels.
	 *
	 * prefix and post were observed growing ~5x together at a FIXED chain
	 * count, and two phases moving in lockstep by the same factor share an
	 * input. The only thing a prefix replay and the output composite have in
	 * common is how much of the screen is being rebuilt --- so that quantity
	 * has to be on the frame, not in a running total that cannot be joined
	 * back to the frame that produced it.
	 */
	uint64_t damage_px;
	uint64_t rebuild_px;
	/* How many blur chains this frame declared. The count, not the bool: at
	 * N > 1 the PREFIX_END and DOWN_END marks belong to the FIRST chain only,
	 * so a phase decomposition read without it silently attributes chains
	 * 2..N to chain 1's upsample. */
	uint32_t chains;
	/* Monotonic id of the frame that wrote this slot, so a trace can follow
	 * one frame from build to a readback several frames later. Diagnostic
	 * only -- nothing about the measurement depends on it. */
	uint64_t frame_id;
	/* The stats generation this frame was BUILT in. Results outlive a stats
	 * reset -- up to AVK_FRAMES_IN_FLIGHT of them are in the queue when it
	 * happens -- and accumulating those into the fresh window mixes frames
	 * from before the measurement started into it. Caught by the cohort test:
	 * the single-chain control classified 30 frames blur-active and collected
	 * 30 frame samples, but only 29 of the samples carried the flag, because
	 * one straddling frame was built idle before the reset and read after it. */
	uint64_t generation;
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
	/*
	 * M4H.7. The deadline this output actually has, in nanoseconds, and how
	 * often the GPU missed it. Set by avk_timestamps_set_budget() from the
	 * output's refresh; 0 disables the accounting rather than defaulting to a
	 * number, because a wrong budget scores silently.
	 */
	uint64_t budget_ns;
	uint64_t budget_frames;
	uint64_t over_budget;
	uint64_t over_budget_2x;
	uint64_t over_budget_3x;
	uint64_t dropped;

	/*
	 * ── THE BLUR SPANS ────────────────────────────────────────────────────
	 *
	 * total    BLUR_BEGIN -> BLUR_END. Every prefix replay and every down/up
	 *          chain in the frame. EXCLUDES the composite draws, which are
	 *          interleaved with content in the output segment.
	 * prefix   BLUR_BEGIN -> BLUR_PREFIX_END, the FIRST chain's prefix replay.
	 * down     BLUR_PREFIX_END -> BLUR_DOWN_END, the FIRST chain's downsamples.
	 * up       BLUR_DOWN_END -> BLUR_END, and only sampled on frames with
	 *          exactly one chain, where that range is the upsamples and
	 *          nothing else.
	 */
	struct avk_hist gpu_frame_hist;
	struct avk_hist blur_total_hist;
	struct avk_hist blur_prefix_hist;
	struct avk_hist blur_down_hist;
	struct avk_hist blur_up_hist;
	/* The final upsample alone: PENULT_END -> BLUR_END, single-chain frames
	 * with levels >= 2. */
	struct avk_hist blur_up0_hist;
	/* gpu_frame, restricted to frames that actually ran blur. Same samples,
	 * same boundaries, different cohort. */
	struct avk_hist gpu_frame_blur_hist;
	/*
	 * THE REST OF THE FRAME, which nothing measured.
	 *
	 * FRAME_BEGIN -> BLUR_BEGIN is everything drawn before the first blur
	 * pass; BLUR_END -> FRAME_END is the final composite and everything after
	 * it. Together with blur_total they partition a blur-bearing frame with no
	 * overlap and no gap, and until they existed the answer to "where does the
	 * frame go" was 44% blur and 56% unattributed -- which is not an answer.
	 * No new queries: both spans come from marks already written.
	 */

	/*
	 * ── COHORT ACCOUNTING ─────────────────────────────────────────────────
	 *
	 * How many frames were CLASSIFIED each way when they were built. These
	 * are what the cohort falsifier checks the histogram against: if
	 * gpu_frame_blur_hist.count does not track cohort_blur_frames (minus the
	 * ones whose results were dropped), the classification is not surviving
	 * to readback.
	 */
	uint64_t cohort_blur_frames;
	uint64_t cohort_idle_frames;
	uint64_t frames_built;
	/* Bumped by a stats reset. A result whose slot generation does not match
	 * is discarded rather than counted: it belongs to the previous window. */
	uint64_t generation;
	uint64_t straddled;

	/*
	 * ── THE BREAK ─────────────────────────────────────────────────────────
	 *
	 * AZ_TS_COHORT_WRONG=1 classifies a returned result by what the CPU is
	 * doing NOW instead of by the slot it came from -- the exact defect this
	 * design exists to avoid. It is here because a delayed-readback bug is
	 * invisible in any fixture where every frame is blur-active, which is
	 * most of them; the sparse-pulse cohort test asserts that turning this on
	 * CHANGES the answer, so the test is proven able to fail.
	 */
	bool cohort_wrong;
	bool cur_blur_active;
	bool trace;
	/* Set per frame before recording; copied into the slot at begin. */
	char pending_output[16];
	/* Carried from the gpu_frame block down to the trace line at the end of
	 * read_slot(), so one frame prints as ONE line with its phases beside its
	 * total instead of two lines that have to be joined by frame id. */
	bool trace_pending, trace_cohort, trace_slot_active, trace_cur_active;
	char trace_output[16];
	uint64_t trace_damage_px, trace_rebuild_px;
	uint64_t trace_gpu_frame_ns, trace_frame_id;
	uint32_t trace_slot;
};

/* Whether this frame's blur work was a single chain, so the up phase can be
 * attributed. Called by the renderer before submission. */
void avk_timestamps_single_chain(struct avk_timestamps *ts, uint32_t slot,
	bool single);

/* Whether this frame ran blur work, so its gpu_frame sample joins the
 * blur-active cohort as well as the all-frames one. */
/* The frame deadline for the output this renderer is serving, in nanoseconds.
 * Called once per frame from the render path, because an output's refresh can
 * change under a mode set and a budget captured at init would then be judging
 * against a rate the display no longer runs at. */
static inline void avk_timestamps_set_budget(struct avk_timestamps *ts,
		uint64_t budget_ns) {
	ts->budget_ns = budget_ns;
}

/* Turn the per-frame READ trace on or off while running. AZ_TS_TRACE is read
 * once at init and `restart` re-execs with the same environ, so on a live
 * session there is otherwise no way to get per-frame data at all --- and every
 * aggregate percentile this milestone has produced has been mixed across two
 * outputs of different size and refresh, which is exactly what per-frame data
 * exists to replace. */
static inline void avk_timestamps_set_trace(struct avk_timestamps *ts,
		bool on) {
	ts->trace = on;
}

/* The output name this frame is being recorded for. Copied, not borrowed, and
 * truncated rather than allocated. */
void avk_timestamps_set_output(struct avk_timestamps *ts, const char *name);

/* This frame's damaged output area and total blur rebuild area, recorded into
 * the slot alongside the chain count. */
static inline void avk_timestamps_set_regions(struct avk_timestamps *ts,
		uint32_t slot, uint64_t damage_px, uint64_t rebuild_px) {
	if (slot < AVK_FRAMES_IN_FLIGHT) {
		ts->slots[slot].damage_px = damage_px;
		ts->slots[slot].rebuild_px = rebuild_px;
	}
}

void avk_timestamps_blur_active(struct avk_timestamps *ts, uint32_t slot,
	bool active, uint32_t chains);

/* Start a new measurement window. Results still in flight from the previous
 * one are dropped when they arrive. */
void avk_timestamps_new_generation(struct avk_timestamps *ts);

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
