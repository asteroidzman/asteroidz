#define _POSIX_C_SOURCE 200809L

#include "avk_timestamp.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "../avk.h"

static bool read_slot(struct avk_timestamps *ts, uint32_t slot,
	bool known_complete);

bool avk_timestamps_init(struct avk_timestamps *ts, struct avk_device *dev) {
	memset(ts, 0, sizeof(*ts));
	ts->dev = dev;

	/*
	 * Two independent capability questions, and answering only the first is
	 * the classic way to ship a timestamp path that returns garbage on some
	 * hardware. timestampPeriod says the DEVICE has a counter;
	 * timestampValidBits says the QUEUE FAMILY this renderer submits to can
	 * write it. A transfer-only family commonly reports 0 valid bits on a
	 * device whose period is perfectly good.
	 */
	uint32_t valid_bits = dev->caps.timestamp_valid_bits;
	if (dev->caps.timestamp_period <= 0.0f || valid_bits == 0) {
		avk_log(AVK_INFO, "avk: GPU TIMESTAMPS: UNSUPPORTED "
			"(period %.2f ns, %u valid bits on queue family %u)",
			(double)dev->caps.timestamp_period, valid_bits,
			dev->caps.graphics_family);
		return false;
	}

	ts->period_ns = dev->caps.timestamp_period;
	/* 64 is not a shift, it is undefined behaviour. */
	ts->valid_mask = valid_bits >= 64
		? UINT64_MAX : (((uint64_t)1 << valid_bits) - 1);

	VkQueryPoolCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = AVK_FRAMES_IN_FLIGHT * AVK_TS_MARKS,
	};
	VkResult res = vkCreateQueryPool(dev->dev, &info, NULL, &ts->pool);
	if (res != VK_SUCCESS) {
		avk_log(AVK_ERROR, "avk: vkCreateQueryPool failed (%d); "
			"GPU TIMESTAMPS: UNSUPPORTED", res);
		ts->pool = VK_NULL_HANDLE;
		return false;
	}
	AVK_LIVE_INC(dev, query_pools);

	ts->supported = true;
	{
		const char *e = getenv("AZ_TS_TRACE");
		ts->trace = e != NULL && e[0] == '1';
	}
	avk_log(AVK_INFO, "avk: GPU timestamps on, %.2f ns/tick, %u valid bits, "
		"%u queries", (double)ts->period_ns, valid_bits,
		info.queryCount);
	return true;
}

void avk_timestamps_finish(struct avk_timestamps *ts) {
	if (ts->pool != VK_NULL_HANDLE) {
		vkDestroyQueryPool(ts->dev->dev, ts->pool, NULL);
		AVK_LIVE_DEC(ts->dev, query_pools);
		ts->pool = VK_NULL_HANDLE;
	}
	ts->supported = false;
}

void avk_timestamps_begin(struct avk_timestamps *ts, VkCommandBuffer cb,
		uint32_t slot) {
	if (!ts->supported || slot >= AVK_FRAMES_IN_FLIGHT) {
		return;
	}
	/*
	 * READ THE OUTGOING RESULT BEFORE RESETTING OVER IT.
	 *
	 * This is not an optimisation, and leaving it out cost most of the
	 * samples in a run. The first version of this file relied on the
	 * per-frame collect() to drain slots, and a compositor drawing faster
	 * than the GPU retires loses the race every time: collect() runs, the
	 * slot is still in flight so it is skipped; the CPU comes round the ring,
	 * avk_cmd_ring_begin() waits for that exact slot, and the result becomes
	 * available a microsecond before this function threw it away. Measured:
	 * 23 samples and 37 drops out of 60 frames.
	 *
	 * The moment the ring hands a slot back is the moment its results are
	 * guaranteed complete -- that wait is the ring's whole contract -- so
	 * this is the one place a read can never be premature and never has to
	 * wait. `true` says so: the timeline has already been consulted, by
	 * someone else, on this caller's behalf.
	 */
	if (ts->slots[slot].timeline_value != 0) {
		read_slot(ts, slot, true);
	}
	if (ts->slots[slot].timeline_value != 0) {
		/* read_slot only leaves it set if the read genuinely failed. */
		ts->dropped++;
		ts->slots[slot].timeline_value = 0;
	}
	ts->slots[slot].written = 0;
	ts->slots[slot].single_chain = false;
	ts->slots[slot].blur_active = false;

	vkCmdResetQueryPool(cb, ts->pool, slot * AVK_TS_MARKS, AVK_TS_MARKS);
}

void avk_timestamps_mark(struct avk_timestamps *ts, VkCommandBuffer cb,
		uint32_t slot, enum avk_ts_mark mark, VkPipelineStageFlags2 stage) {
	if (!ts->supported || slot >= AVK_FRAMES_IN_FLIGHT
			|| mark >= AVK_TS_MARKS) {
		return;
	}
	vkCmdWriteTimestamp2(cb, stage, ts->pool, slot * AVK_TS_MARKS + mark);
	ts->slots[slot].written |= 1u << mark;
}

void avk_timestamps_single_chain(struct avk_timestamps *ts, uint32_t slot,
		bool single) {
	if (!ts->supported || slot >= AVK_FRAMES_IN_FLIGHT) {
		return;
	}
	ts->slots[slot].single_chain = single;
}

void avk_timestamps_blur_active(struct avk_timestamps *ts, uint32_t slot,
		bool active, uint32_t chains) {
	if (!ts->supported || slot >= AVK_FRAMES_IN_FLIGHT) {
		return;
	}
	ts->slots[slot].blur_active = active;
	/* Travels with the frame, not with the CPU: see avk_ts_slot.output. */
	size_t n = 0;
	while (ts->pending_output[n] != '\0'
			&& n + 1 < sizeof(ts->slots[slot].output)) {
		ts->slots[slot].output[n] = ts->pending_output[n];
		n++;
	}
	ts->slots[slot].output[n] = '\0';
	ts->slots[slot].chains = chains;
	ts->slots[slot].frame_id = ++ts->frames_built;
	ts->slots[slot].generation = ts->generation;
	/* What the CPU is doing NOW. Read by nothing except the break. */
	ts->cur_blur_active = active;
	if (active) {
		ts->cohort_blur_frames++;
	} else {
		ts->cohort_idle_frames++;
	}
	if (ts->trace) {
		avk_log(AVK_INFO, "avk cohort: BUILD frame=%" PRIu64 " slot=%u "
			"blur_active=%d chains=%u", ts->slots[slot].frame_id, slot,
			active ? 1 : 0, chains);
	}
}

void avk_timestamps_new_generation(struct avk_timestamps *ts) {
	ts->generation++;
}

void avk_timestamps_submitted(struct avk_timestamps *ts, uint32_t slot,
		uint64_t timeline_value) {
	if (!ts->supported || slot >= AVK_FRAMES_IN_FLIGHT) {
		return;
	}
	if (ts->slots[slot].written == 0) {
		/* Nothing was marked, so there is nothing to come back for. */
		ts->slots[slot].timeline_value = 0;
		return;
	}
	ts->slots[slot].timeline_value = timeline_value;
}

uint64_t avk_ts_ticks_between(uint64_t mask, uint64_t a, uint64_t b) {
	a &= mask;
	b &= mask;
	/* The counter wraps within its valid bits; masked unsigned subtraction is
	 * correct across the wrap and costs nothing when there is none. */
	return (b - a) & mask;
}

/*
 * Read one slot's marks, if they are there. Returns true if a result was
 * consumed; leaves `timeline_value` set (so the slot is retried) when the
 * results are not available yet, and clears it on an outright failure.
 *
 * `known_complete` skips the timeline comparison for the one caller that has
 * already had the wait done for it -- see avk_timestamps_begin().
 */
static bool read_slot(struct avk_timestamps *ts, uint32_t slot,
		bool known_complete) {
	struct avk_ts_slot *s = &ts->slots[slot];
	if (s->timeline_value == 0) {
		return false;
	}
	if (!known_complete
			&& s->timeline_value > avk_device_timeline_value(ts->dev)) {
		return false;
	}

	/*
	 * Two results per query: the value and its availability. NO WAIT BIT --
	 * that is the whole point of this file. The timeline already says the work
	 * is done, and the availability bit says the driver has written the answer
	 * down; if it somehow has not, this returns VK_NOT_READY and the slot is
	 * tried again next frame.
	 */
	uint64_t raw[AVK_TS_MARKS * 2] = {0};
	VkResult res = vkGetQueryPoolResults(ts->dev->dev, ts->pool,
		slot * AVK_TS_MARKS, AVK_TS_MARKS, sizeof(raw), raw,
		2 * sizeof(uint64_t),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	/*
	 * VK_NOT_READY DOES NOT MEAN "COME BACK LATER" HERE.
	 *
	 * vkGetQueryPoolResults returns VK_NOT_READY when ANY query in the range
	 * is unavailable -- and once marks became optional (M4F.2D), a frame that
	 * writes only some of them leaves the rest reset-but-never-written, which
	 * is unavailable FOREVER. The range read then returned VK_NOT_READY on
	 * every frame, every slot was retried until it came round again, and the
	 * collector lost 100% of its samples: measured, 0 collected and 35
	 * dropped over 38 frames, with the frame marks written every time.
	 *
	 * The availability words are still written for the queries that ARE
	 * available, so the per-mark check below is the one that decides. It was
	 * always the real gate; this return code was a second, wrong one that
	 * happened to agree while every frame wrote every mark.
	 */
	if (res != VK_SUCCESS && res != VK_NOT_READY) {
		/* Give the slot back rather than retrying a failing read every frame
		 * for the life of the compositor. */
		s->timeline_value = 0;
		ts->dropped++;
		return false;
	}

	for (uint32_t m = 0; m < AVK_TS_MARKS; m++) {
		bool wanted = (s->written & (1u << m)) != 0;
		bool available = raw[m * 2 + 1] != 0;
		if (wanted && !available) {
			return false;
		}
	}

	/* A span is only computed when BOTH of its marks were written. A mark
	 * that was not written has no result, and reading its query returns a
	 * stale value from three frames ago -- indistinguishable from a plausible
	 * one, which is how a "measurement" becomes a number nobody can defend. */
	/*
	 * A RESULT FROM THE PREVIOUS MEASUREMENT WINDOW IS NOT A RESULT.
	 *
	 * The slot is still consumed -- it has to be, or it would be retried
	 * forever -- but nothing it carries is accumulated. Without this, up to
	 * AVK_FRAMES_IN_FLIGHT frames built before a reset land in the window
	 * after it, which on a 25-sample run is 12% of the population and is
	 * exactly what made the control's cohort count disagree with its frame
	 * count by one.
	 */
	if (s->generation != ts->generation) {
		s->timeline_value = 0;
		s->written = 0;
		ts->straddled++;
		return true;
	}

	uint32_t need = (1u << AVK_TS_FRAME_BEGIN) | (1u << AVK_TS_FRAME_END);
	if ((s->written & need) == need) {
		uint64_t frame = avk_ts_ticks_between(ts->valid_mask,
			raw[AVK_TS_FRAME_BEGIN * 2], raw[AVK_TS_FRAME_END * 2]);
		ts->gpu_frame_ns = (uint64_t)((double)frame * ts->period_ns);
		ts->gpu_frame_ns_total += ts->gpu_frame_ns;
		ts->samples++;
		if (ts->gpu_frame_ns >= (uint64_t)AVK_HIST_BUCKETS * AVK_HIST_SCALE_NS) {
			avk_log(AVK_ERROR, "avk timing: gpu_frame span %.3f ms exceeds the "
				"histogram ceiling (slot %u, written=0x%x)",
				(double)ts->gpu_frame_ns / 1e6, slot, s->written);
		}
		avk_hist_add(&ts->gpu_frame_hist, ts->gpu_frame_ns);
		/* The same sample, in the cohort that can be compared against
		 * gpu_blur_total: `s->blur_active` was recorded for THIS frame when it
		 * was built, so a result arriving three frames later is still
		 * classified by its own frame. */
		if (s->blur_active) {
			avk_hist_add(&ts->gpu_frame_blur_hist, ts->gpu_frame_ns);
		}
		/*
		 * ── OVER BUDGET, WHICH IS THE ONLY NUMBER THE USER FEELS ──────────
		 *
		 * A percentile cannot answer "did a frame miss". p95 swung from 2880
		 * to 4860us across two identical live cohorts while p50 and p99 stayed
		 * put, so quoting it either way was going to be wrong. A COUNT of
		 * frames past the deadline does not move with cohort size, and its
		 * multiples say whether a miss cost one refresh or three.
		 *
		 * The budget is set per output from its real refresh, so a 60Hz
		 * display is judged against 16.7ms and a 144Hz one against 6.944ms.
		 * Zero means the budget was never published and the frame is not
		 * counted either way -- an unset budget must not silently score every
		 * frame as on time.
		 */
		if (ts->budget_ns > 0) {
			ts->budget_frames++;
			if (ts->gpu_frame_ns > ts->budget_ns) {
				ts->over_budget++;
				if (ts->gpu_frame_ns > 3 * ts->budget_ns) {
					ts->over_budget_3x++;
				} else if (ts->gpu_frame_ns > 2 * ts->budget_ns) {
					ts->over_budget_2x++;
				}
			}
		}
		ts->trace_gpu_frame_ns = ts->gpu_frame_ns;
		ts->trace_cohort = s->blur_active;
		ts->trace_slot_active = s->blur_active;
		ts->trace_cur_active = ts->cur_blur_active;
		size_t on = 0;
		while (s->output[on] != '\0' && on + 1 < sizeof(ts->trace_output)) {
			ts->trace_output[on] = s->output[on];
			on++;
		}
		ts->trace_output[on] = '\0';
		ts->trace_damage_px = s->damage_px;
		ts->trace_rebuild_px = s->rebuild_px;
		ts->trace_frame_id = s->frame_id;
		ts->trace_slot = slot;
		ts->trace_pending = true;
	}

	/*
	 * ── OVERFLOW FORENSICS ────────────────────────────────────────────────
	 *
	 * A span past the histogram ceiling is either a real outlier worth
	 * explaining or a mark being paired with the wrong one, and a bucket
	 * counter cannot tell those apart. So an overflowing span says so once,
	 * with its exact value and the slot's topology -- and NOTHING is logged
	 * for the ordinary frames, which are the overwhelming majority.
	 */
	uint64_t span;
#define AVK_TS_REPORT(name, v) do { \
		if ((v) >= (uint64_t)AVK_HIST_BUCKETS * AVK_HIST_SCALE_NS) { \
			avk_log(AVK_ERROR, "avk timing: %s span %.3f ms exceeds the " \
				"histogram ceiling (slot %u, single_chain=%d, written=0x%x)", \
				name, (double)(v) / 1e6, slot, s->single_chain ? 1 : 0, \
				s->written); \
		} \
	} while (0)
#define AVK_TS_SPAN(a, b) \
	((((s->written >> (a)) & 1u) && (((s->written >> (b)) & 1u))) \
		? (span = (uint64_t)((double)avk_ts_ticks_between(ts->valid_mask, \
			raw[(a) * 2], raw[(b) * 2]) * ts->period_ns), true) : false)

	uint64_t tr_total = 0, tr_prefix = 0, tr_down = 0;
	if (AVK_TS_SPAN(AVK_TS_BLUR_BEGIN, AVK_TS_BLUR_END)) {
		AVK_TS_REPORT("blur_total", span);
		avk_hist_add(&ts->blur_total_hist, span);
		tr_total = span;
	}
	if (AVK_TS_SPAN(AVK_TS_BLUR_BEGIN, AVK_TS_BLUR_PREFIX_END)) {
		avk_hist_add(&ts->blur_prefix_hist, span);
		tr_prefix = span;
	}
	if (AVK_TS_SPAN(AVK_TS_BLUR_PREFIX_END, AVK_TS_BLUR_DOWN_END)) {
		avk_hist_add(&ts->blur_down_hist, span);
		tr_down = span;
	}
	/* The two ends of the frame that are not blur. */
	uint64_t tr_pre = 0, tr_post = 0;
	if (AVK_TS_SPAN(AVK_TS_FRAME_BEGIN, AVK_TS_BLUR_BEGIN)) {
		tr_pre = span;
	}
	if (AVK_TS_SPAN(AVK_TS_BLUR_END, AVK_TS_FRAME_END)) {
		tr_post = span;
	}
	/* ONLY on a single-chain frame: with two chains this range is
	 * "up0 + prefix1 + chain1", which is not an upsample cost. */
	if (s->single_chain && AVK_TS_SPAN(AVK_TS_BLUR_DOWN_END, AVK_TS_BLUR_END)) {
		avk_hist_add(&ts->blur_up_hist, span);
	}
	if (s->single_chain
			&& AVK_TS_SPAN(AVK_TS_BLUR_UP_PENULT_END, AVK_TS_BLUR_END)) {
		avk_hist_add(&ts->blur_up0_hist, span);
	}
#undef AVK_TS_SPAN
#undef AVK_TS_REPORT

	/*
	 * ONE LINE PER FRAME, WITH ITS PHASES ALONGSIDE ITS TOTAL.
	 *
	 * A bimodal histogram says a population has two modes; it cannot say what
	 * separates them, because a percentile has thrown the frame identity away.
	 * This keeps the identity: frame id, whether it was blur-bearing, its
	 * whole-frame span, and the phases inside it.
	 *
	 * `remainder` is DOWN_END -> BLUR_END, and on a MULTI-CHAIN frame it is
	 * not "the upsample". The PREFIX_END and DOWN_END marks are written by the
	 * first chain only, so on N chains the remainder is chain 1's upsample
	 * plus every one of chains 2..N entire. Named for what it is rather than
	 * what it would be at N=1, because that is precisely the reading that
	 * would turn a prefix-replay cost into an "upsample" cost.
	 */
	if (ts->trace && ts->trace_pending) {
		/* The first five fields are the cohort provenance the cohort test
		 * asserts on -- which slot the result came from, what THAT slot was
		 * classified as, what the CPU happens to be doing now, and which of
		 * the two decided. They are matched by a regex, so their order and
		 * spelling are load-bearing; the phase fields are appended after. */
		avk_log(AVK_INFO, "avk cohort: READ  out=%s frame=%" PRIu64 " slot=%u "
			"slot.blur_active=%d cur.blur_active=%d -> cohort=%d "
			"(built %" PRIu64 " frames ago) gpu_frame=%.1f us "
			"chains=%u single=%d damage_px=%" PRIu64 " rebuild_px=%" PRIu64 " "
			"blur_total_us=%.1f prefix_us=%.1f "
			"down_us=%.1f remainder_us=%.1f pre_us=%.1f post_us=%.1f",
			ts->trace_output, ts->trace_frame_id, ts->trace_slot,
			ts->trace_slot_active ? 1 : 0,
			ts->trace_cur_active ? 1 : 0, ts->trace_cohort ? 1 : 0,
			ts->frames_built - ts->trace_frame_id,
			(double)ts->trace_gpu_frame_ns / 1e3,
			s->chains, s->single_chain ? 1 : 0,
			ts->trace_damage_px, ts->trace_rebuild_px, (double)tr_total / 1e3,
			(double)tr_prefix / 1e3, (double)tr_down / 1e3,
			tr_total > tr_prefix + tr_down
				? (double)(tr_total - tr_prefix - tr_down) / 1e3 : 0.0,
			(double)tr_pre / 1e3, (double)tr_post / 1e3);
	}
	ts->trace_pending = false;

	s->timeline_value = 0;
	return true;
}

void avk_timestamps_set_output(struct avk_timestamps *ts, const char *name) {
	if (name == NULL) {
		ts->pending_output[0] = '\0';
		return;
	}
	size_t n = 0;
	while (name[n] != '\0' && n + 1 < sizeof(ts->pending_output)) {
		ts->pending_output[n] = name[n];
		n++;
	}
	ts->pending_output[n] = '\0';
}

size_t avk_timestamps_collect(struct avk_timestamps *ts) {
	if (!ts->supported) {
		return 0;
	}
	size_t consumed = 0;
	for (uint32_t slot = 0; slot < AVK_FRAMES_IN_FLIGHT; slot++) {
		if (read_slot(ts, slot, false)) {
			consumed++;
		}
	}
	return consumed;
}
