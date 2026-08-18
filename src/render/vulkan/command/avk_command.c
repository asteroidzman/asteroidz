#define _POSIX_C_SOURCE 200809L

#include "avk_command.h"

#include <inttypes.h>
#include <time.h>
#include <string.h>

/* One second. Not a real timeout in the sense of "this might legitimately take
 * a second" -- if a frame's worth of commands has not completed in a second
 * the GPU is hung, and returning to the caller with an error beats hanging the
 * compositor's event loop with UINT64_MAX. */
#define AVK_SLOT_WAIT_NS (1000ULL * 1000ULL * 1000ULL)

bool avk_cmd_ring_init(struct avk_cmd_ring *ring, struct avk_device *dev,
		const char *name) {
	memset(ring, 0, sizeof(*ring));
	ring->dev = dev;
	ring->recording = -1;

	for (uint32_t i = 0; i < AVK_FRAMES_IN_FLIGHT; i++) {
		VkCommandPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			/* TRANSIENT, and deliberately NOT RESET_COMMAND_BUFFER: buffers
			 * are only ever reset by resetting the whole pool, which is the
			 * point of having one pool per slot. */
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
			.queueFamilyIndex = dev->caps.graphics_family,
		};
		VkResult res = vkCreateCommandPool(dev->dev, &pool_info, NULL,
			&ring->slots[i].pool);
		if (res != VK_SUCCESS) {
			avk_check(res, "vkCreateCommandPool");
			goto error;
		}
		AVK_LIVE_INC(dev, command_pools);

		VkCommandBufferAllocateInfo alloc_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = ring->slots[i].pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		res = vkAllocateCommandBuffers(dev->dev, &alloc_info,
			&ring->slots[i].cb);
		if (res != VK_SUCCESS) {
			avk_check(res, "vkAllocateCommandBuffers");
			goto error;
		}

		avk_device_name_object(dev, VK_OBJECT_TYPE_COMMAND_POOL,
			(uint64_t)ring->slots[i].pool, "%s pool %u", name, i);
		avk_device_name_object(dev, VK_OBJECT_TYPE_COMMAND_BUFFER,
			(uint64_t)ring->slots[i].cb, "%s cb %u", name, i);
	}

	return true;

error:
	avk_cmd_ring_finish(ring);
	return false;
}

void avk_cmd_ring_finish(struct avk_cmd_ring *ring) {
	if (ring->dev == NULL) {
		return;
	}
	for (uint32_t i = 0; i < AVK_FRAMES_IN_FLIGHT; i++) {
		/* Destroying the pool frees its buffers; freeing them first would be
		 * legal but pointless. */
		if (ring->slots[i].pool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(ring->dev->dev, ring->slots[i].pool, NULL);
			AVK_LIVE_DEC(ring->dev, command_pools);
			ring->slots[i].pool = VK_NULL_HANDLE;
			ring->slots[i].cb = VK_NULL_HANDLE;
		}
	}
	ring->dev = NULL;
}

static uint64_t avk_now_ns_dbg(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

VkCommandBuffer avk_cmd_ring_begin(struct avk_cmd_ring *ring) {
	if (ring->recording >= 0) {
		avk_log(AVK_ERROR, "avk_cmd_ring_begin while slot %d is still being "
			"recorded", ring->recording);
		return VK_NULL_HANDLE;
	}

	uint64_t begin_t0 = avk_now_ns_dbg();
	uint32_t index = ring->next;
	struct avk_cmd_slot *slot = &ring->slots[index];

	if (slot->timeline_value != 0) {
		/* The comparison, not the wait, is the normal path: three frames of
		 * slack means the GPU has almost always passed this point already. */
		if (avk_device_timeline_value(ring->dev) < slot->timeline_value) {
			ring->stalls++;
			avk_log(AVK_DEBUG, "command ring backpressure: waiting for "
				"timeline %" PRIu64, slot->timeline_value);
			if (!avk_device_timeline_wait(ring->dev, slot->timeline_value,
					AVK_SLOT_WAIT_NS)) {
				avk_log(AVK_ERROR, "command ring: GPU did not reach timeline "
					"%" PRIu64 " within 1s", slot->timeline_value);
				return VK_NULL_HANDLE;
			}
		}
	}

	uint64_t t_backpressure = avk_now_ns_dbg() - begin_t0;
	uint64_t reset_t0 = avk_now_ns_dbg();
	VkResult res = vkResetCommandPool(ring->dev->dev, slot->pool, 0);
	uint64_t t_reset = avk_now_ns_dbg() - reset_t0;
	if (res != VK_SUCCESS) {
		avk_check(res, "vkResetCommandPool");
		return VK_NULL_HANDLE;
	}

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	uint64_t begin_cb_t0 = avk_now_ns_dbg();
	res = vkBeginCommandBuffer(slot->cb, &begin_info);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkBeginCommandBuffer");
		return VK_NULL_HANDLE;
	}
	{
		/* The submit step of a 42x41 upload measures 30ms with vkQueueSubmit2
		 * accounting for none of it and the backpressure wait never firing.
		 * Everything before the submit is this function. */
		uint64_t t_begin_cb = avk_now_ns_dbg() - begin_cb_t0;
		uint64_t total_us = (avk_now_ns_dbg() - begin_t0) / 1000;
		if (total_us > 5000) {
			avk_log(AVK_ERROR, "cmd_ring_begin blocked for %" PRIu64
				"us: backpressure %" PRIu64 "us + resetPool %" PRIu64
				"us + beginCB %" PRIu64 "us", total_us,
				t_backpressure / 1000, t_reset / 1000, t_begin_cb / 1000);
		}
	}

	ring->recording = (int)index;
	ring->acquires++;
	return slot->cb;
}

uint64_t avk_cmd_ring_submit(struct avk_cmd_ring *ring,
		const VkSemaphoreSubmitInfo *waits, uint32_t wait_count,
		const VkSemaphoreSubmitInfo *signals, uint32_t signal_count) {
	if (ring->recording < 0) {
		avk_log(AVK_ERROR, "avk_cmd_ring_submit with nothing recorded");
		return 0;
	}

	struct avk_cmd_slot *slot = &ring->slots[ring->recording];

	VkResult res = vkEndCommandBuffer(slot->cb);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkEndCommandBuffer");
		ring->recording = -1;
		return 0;
	}

	uint64_t value = avk_device_timeline_reserve(ring->dev);

	/* The device timeline signal is appended to whatever the caller asked for,
	 * so every submission advances it whether or not the caller cares. That
	 * invariant is what makes resource retirement a comparison against one
	 * number. */
	enum { MAX_SIGNALS = 8 };
	VkSemaphoreSubmitInfo signal_infos[MAX_SIGNALS];
	if (signal_count + 1 > MAX_SIGNALS) {
		avk_log(AVK_ERROR, "too many signal semaphores (%u)", signal_count);
		ring->recording = -1;
		return 0;
	}
	for (uint32_t i = 0; i < signal_count; i++) {
		signal_infos[i] = signals[i];
	}
	signal_infos[signal_count] = (VkSemaphoreSubmitInfo){
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = ring->dev->timeline,
		.value = value,
		/* ALL_COMMANDS: the timeline means "the GPU is done with everything
		 * this submission touched", which is exactly what a resource-lifetime
		 * signal has to mean. A narrower stage would retire buffers that a
		 * later stage is still reading. */
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	};

	VkCommandBufferSubmitInfo cb_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = slot->cb,
	};
	VkSubmitInfo2 submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.waitSemaphoreInfoCount = wait_count,
		.pWaitSemaphoreInfos = waits,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &cb_info,
		.signalSemaphoreInfoCount = signal_count + 1,
		.pSignalSemaphoreInfos = signal_infos,
	};

	/* No fence. The timeline is the fence, and passing one here would be a
	 * second thing to poll and a second thing to reset. */
	/*
	 * TIMED. A 42x41 upload measured 30ms inside "submit", with the ring's own
	 * backpressure wait never once firing, which leaves the driver call
	 * itself. vkQueueSubmit2 is not supposed to block; when it does, it is the
	 * kernel's submission path, and that is worth naming rather than guessing
	 * at from the outside.
	 */
	uint64_t submit_t0 = avk_now_ns_dbg();
	res = vkQueueSubmit2(ring->dev->graphics_queue, 1, &submit,
		VK_NULL_HANDLE);
	{
		uint64_t submit_us = (avk_now_ns_dbg() - submit_t0) / 1000;
		if (submit_us > 5000) {
			avk_log(AVK_ERROR, "vkQueueSubmit2 blocked for %" PRIu64
				"us (%u wait semaphore(s))", submit_us, wait_count);
		}
	}
	ring->recording = -1;
	if (res != VK_SUCCESS) {
		avk_check(res, "vkQueueSubmit2");
		/* The reserved point will never be signalled, so nothing may be
		 * retired against it. Leaving the slot's old value alone means the
		 * slot is still guarded by whatever it was guarded by before. */
		return 0;
	}

	slot->timeline_value = value;
	ring->next = (ring->next + 1) % AVK_FRAMES_IN_FLIGHT;
	ring->submits++;
	return value;
}

void avk_cmd_ring_abandon(struct avk_cmd_ring *ring) {
	if (ring->recording < 0) {
		return;
	}
	struct avk_cmd_slot *slot = &ring->slots[ring->recording];
	/* End it so the buffer is in a resettable state; the pool reset on the
	 * next acquire throws the contents away. */
	vkEndCommandBuffer(slot->cb);
	ring->recording = -1;
}
