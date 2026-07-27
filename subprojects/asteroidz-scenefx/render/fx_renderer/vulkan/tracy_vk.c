#ifdef TRACY_ENABLE
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tracy/TracyC.h>
#include <wlr/util/log.h>

#include "render/tracy.h"
#include "render/vulkan.h"

// Tracy GPU context for the Vulkan (fx_vk) renderer.
//
// The GLES side of this lives in render/fx_renderer/tracy.c and is built on
// EXT_disjoint_timer_query. None of it transfers: that path writes a timestamp
// the moment the macro runs, because GLES is immediate-mode. Vulkan records
// into a command buffer and submits later, so a zone here is a pair of
// vkCmdWriteTimestamp calls recorded INTO a command buffer, and the results
// only exist once the GPU has actually run it.
//
// That deferral is also why the zones are pass-level rather than per-draw.
// fx_vk batches its draws and submits once per frame; bracketing every
// recorded draw would mean two timestamps per draw, a query pool sized for the
// worst frame, and a pipeline barrier's worth of ordering guarantees on a
// tiler -- for numbers whose sum is already the pass total.
//
// Docs used:
// - https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf
// - https://github.com/wolfpld/tracy/blob/master/public/tracy/TracyVulkan.hpp
// - VK_EXT_calibrated_timestamps (optional; see calibrate() below)

// Pass-level zones only, so this is generous: a handful per frame, and the
// collect drains it every frame.
#define VK_QUERY_QUEUE_LEN 4096

#define RED4 0x8b0000

static atomic_int vk_id_counter = 0;

struct tracy_vk_data {
	struct fx_vk_renderer *renderer;

	uint8_t context_id;
	VkQueryPool pool;
	float period; // nanoseconds per timestamp tick

	struct {
		uint32_t head;
		uint32_t tail;
	} queue;
};

static inline uint32_t vk_next_query(struct tracy_vk_data *d) {
	const uint32_t id = d->queue.head;
	d->queue.head = (d->queue.head + 1) % VK_QUERY_QUEUE_LEN;
	assert(d->queue.head != d->queue.tail);
	return id;
}

/*
 * GPU Zone
 */

void tracy_vk_zone_begin(struct tracy_vk_data *d, VkCommandBuffer cb,
		struct tracy_vk_zone_context *out_ctx, const int line,
		const char *source, const char *func, const char *name) {
	if (out_ctx == NULL || d == NULL || cb == VK_NULL_HANDLE) {
		if (out_ctx != NULL) {
			out_ctx->data = NULL;
			out_ctx->is_active = false;
		}
		return;
	}

	out_ctx->data = d;
	out_ctx->is_active = TracyCIsConnected;
	if (!out_ctx->is_active) {
		return;
	}

	const uint32_t query = vk_next_query(d);
	// A query must be reset before it is written. Doing it here, one at a
	// time, keeps this independent of VK_EXT_host_query_reset (which not every
	// driver in range advertises) at the cost of one extra command.
	vkCmdResetQueryPool(cb, d->pool, query, 1);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, d->pool, query);
	out_ctx->begin_query = query;

	const uint64_t srcloc =
		___tracy_alloc_srcloc_name(line, source, strlen(source), func,
			strlen(func), name, strlen(name), 0);
	const struct ___tracy_gpu_zone_begin_data data = {
		.context = d->context_id,
		.queryId = (uint16_t)query,
		.srcloc = srcloc,
	};
	___tracy_emit_gpu_zone_begin_alloc(data);
}

void tracy_vk_zone_end(struct tracy_vk_zone_context *ctx, VkCommandBuffer cb) {
	if (ctx == NULL || ctx->data == NULL || !ctx->is_active ||
			cb == VK_NULL_HANDLE) {
		return;
	}
	struct tracy_vk_data *d = ctx->data;

	const uint32_t query = vk_next_query(d);
	vkCmdResetQueryPool(cb, d->pool, query, 1);
	// BOTTOM_OF_PIPE for the end so the zone spans the work rather than the
	// gap before it -- TOP at both ends would measure command submission, not
	// execution.
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d->pool,
		query);

	const struct ___tracy_gpu_zone_end_data data = {
		.context = d->context_id,
		.queryId = (uint16_t)query,
	};
	___tracy_emit_gpu_zone_end(data);
}

/*
 * Collect
 */

void tracy_vk_context_collect(struct tracy_vk_data *d) {
	if (d == NULL) {
		return;
	}

	TracyCZoneC(ctx, RED4, true);

	if (d->queue.tail == d->queue.head) {
		goto done;
	}

#ifdef TRACY_ON_DEMAND
	if (!TracyCIsConnected) {
		// Nothing is listening: drop the backlog rather than let it wrap and
		// trip the assert in vk_next_query once a viewer finally attaches.
		d->queue.head = 0;
		d->queue.tail = 0;
		goto done;
	}
#endif

	while (d->queue.tail != d->queue.head) {
		uint64_t result = 0;
		// WITHOUT _WAIT_BIT: an unfinished query returns NOT_READY and we stop
		// draining. Blocking here would stall the render thread on the GPU,
		// which is exactly the cost this is supposed to be measuring.
		VkResult res = vkGetQueryPoolResults(d->renderer->dev->dev, d->pool,
			d->queue.tail, 1, sizeof(result), &result, sizeof(result),
			VK_QUERY_RESULT_64_BIT);
		if (res != VK_SUCCESS) {
			goto done;
		}

		const struct ___tracy_gpu_time_data data = {
			.context = d->context_id,
			.gpuTime = (int64_t)result,
			.queryId = (uint16_t)d->queue.tail,
		};
		___tracy_emit_gpu_time(data);

		d->queue.tail = (d->queue.tail + 1) % VK_QUERY_QUEUE_LEN;
	}

done:
	TracyCZoneEnd(ctx);
}

/*
 * Context lifecycle
 */

// One reference point where the GPU clock and Tracy's CPU clock are read as
// close together as possible. VK_EXT_calibrated_timestamps would give a
// genuinely simultaneous pair; without it, write one timestamp, wait for it,
// and accept the submit latency as a constant offset -- zone DURATIONS are
// unaffected either way, only their absolute placement on the timeline.
static bool calibrate(struct fx_vk_renderer *renderer, VkQueryPool pool,
		int64_t *out_gpu_time) {
	VkCommandBuffer cb = fx_vulkan_record_stage_cb(renderer);
	if (cb == VK_NULL_HANDLE) {
		return false;
	}
	vkCmdResetQueryPool(cb, pool, 0, 1);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
	if (!fx_vulkan_submit_stage_wait(renderer)) {
		return false;
	}

	uint64_t gpu = 0;
	VkResult res = vkGetQueryPoolResults(renderer->dev->dev, pool, 0, 1,
		sizeof(gpu), &gpu, sizeof(gpu),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
	if (res != VK_SUCCESS) {
		return false;
	}
	*out_gpu_time = (int64_t)gpu;
	return true;
}

struct tracy_vk_data *tracy_vk_context_new(struct fx_vk_renderer *renderer) {
	assert(renderer);

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(renderer->dev->phdev, &props);
	if (props.limits.timestampComputeAndGraphics == VK_FALSE) {
		// Not an error worth failing the renderer over -- it just means this
		// device cannot answer the question.
		wlr_log(WLR_INFO,
			"Tracy: device does not support timestamps on the graphics queue, "
			"no Vulkan GPU zones");
		return NULL;
	}

	struct tracy_vk_data *d = calloc(1, sizeof(*d));
	if (d == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	d->renderer = renderer;
	d->period = props.limits.timestampPeriod;

	const VkQueryPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = VK_QUERY_QUEUE_LEN,
	};
	if (vkCreateQueryPool(renderer->dev->dev, &pool_info, NULL, &d->pool) !=
			VK_SUCCESS) {
		wlr_log(WLR_ERROR, "Tracy: vkCreateQueryPool failed");
		free(d);
		return NULL;
	}

	int64_t gpu_time = 0;
	if (!calibrate(renderer, d->pool, &gpu_time)) {
		wlr_log(WLR_ERROR, "Tracy: timestamp calibration failed");
		vkDestroyQueryPool(renderer->dev->dev, d->pool, NULL);
		free(d);
		return NULL;
	}

	// Calibration used query 0; start the ring after it so the first zone does
	// not read a slot whose result has already been consumed.
	d->context_id = ++vk_id_counter;
	d->queue.head = 1;
	d->queue.tail = 1;

	const struct ___tracy_gpu_new_context_data data = {
		.context = d->context_id,
		.gpuTime = gpu_time,
		// Unlike the GLES path, this is NOT 1.0: Vulkan reports ticks, and
		// timestampPeriod is how many nanoseconds one tick is. AMD reports 1.0
		// here and Intel does not, so hardcoding it would silently scale every
		// duration on half the machines that run this.
		.period = d->period,
		.flags = 0,
		.type = 2, // GpuContextType::Vulkan, TracyQueue.hpp
	};
	___tracy_emit_gpu_new_context(data);

	char name[128];
	int len = snprintf(name, sizeof(name), "FX Renderer (Vulkan): %s",
		props.deviceName);
	if (len > 0) {
		if (len >= (int)sizeof(name)) {
			len = (int)sizeof(name) - 1;
		}
		const struct ___tracy_gpu_context_name_data name_data = {
			.context = d->context_id,
			.name = name,
			.len = (uint16_t)len,
		};
		___tracy_emit_gpu_context_name(name_data);
	}

	return d;
}

void tracy_vk_context_destroy(struct tracy_vk_data *d) {
	if (d == NULL) {
		return;
	}
	vk_id_counter--;
	vkDestroyQueryPool(d->renderer->dev->dev, d->pool, NULL);
	free(d);
}

#endif // TRACY_ENABLE
