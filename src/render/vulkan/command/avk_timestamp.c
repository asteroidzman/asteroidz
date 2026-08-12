#define _POSIX_C_SOURCE 200809L

#include "avk_timestamp.h"

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
	if (res == VK_NOT_READY) {
		return false;
	}
	if (res != VK_SUCCESS) {
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

	uint32_t need = (1u << AVK_TS_FRAME_BEGIN) | (1u << AVK_TS_FRAME_END);
	if ((s->written & need) == need) {
		uint64_t frame = avk_ts_ticks_between(ts->valid_mask,
			raw[AVK_TS_FRAME_BEGIN * 2], raw[AVK_TS_FRAME_END * 2]);
		ts->gpu_frame_ns = (uint64_t)((double)frame * ts->period_ns);
		ts->gpu_frame_ns_total += ts->gpu_frame_ns;
		ts->samples++;
	}

	s->timeline_value = 0;
	return true;
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
