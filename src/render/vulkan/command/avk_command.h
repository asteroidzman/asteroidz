#ifndef AVK_COMMAND_H
#define AVK_COMMAND_H

#include "../device/avk_device.h"

/*
 * A ring of reusable command pools.
 *
 * The thing this exists to prevent: allocating a command buffer per frame.
 * vkAllocateCommandBuffers on the frame path is a driver-side allocation in
 * the middle of the one code path with a deadline, and freeing it afterwards
 * hands the memory back only to ask for it again 8ms later.
 *
 * Instead there are AVK_FRAMES_IN_FLIGHT slots, each with its own pool and one
 * primary buffer. A slot is reusable once the GPU has passed the timeline
 * point its last submission signalled -- so reuse is a single integer
 * comparison, and the only CPU wait in the whole file happens when every slot
 * is still in flight. That is genuine backpressure (the CPU is more than three
 * frames ahead of the GPU) and it is the one case where waiting is correct
 * rather than sloppy; `stalls` counts it so it can be measured instead of
 * argued about.
 *
 * A pool per slot rather than one pool with RESET_COMMAND_BUFFER_BIT:
 * vkResetCommandPool hands a whole frame's command memory back to the driver
 * in one call and lets it reuse the block, which individual buffer resets
 * cannot do.
 */

struct avk_cmd_slot {
	VkCommandPool pool;
	VkCommandBuffer cb;
	/* The timeline point this slot's last submission will signal. 0 means the
	 * slot has never been submitted, so it is free. */
	uint64_t timeline_value;
};

struct avk_cmd_ring {
	struct avk_device *dev;
	struct avk_cmd_slot slots[AVK_FRAMES_IN_FLIGHT];
	uint32_t next;
	int recording;   /* index of the slot being recorded, or -1 */

	/* Counters, not guesses. `stalls` is the number that matters: nonzero in
	 * steady state means the ring is too short or the GPU is behind. */
	uint64_t acquires;
	uint64_t submits;
	uint64_t stalls;
};

bool avk_cmd_ring_init(struct avk_cmd_ring *ring, struct avk_device *dev,
	const char *name);
void avk_cmd_ring_finish(struct avk_cmd_ring *ring);

/*
 * Take the next slot and begin recording into it.
 *
 * Returns VK_NULL_HANDLE on failure. Blocks only if the slot's previous
 * submission has not completed.
 */
VkCommandBuffer avk_cmd_ring_begin(struct avk_cmd_ring *ring);

/*
 * End recording and submit, signalling the device timeline.
 *
 * Extra waits and signals are passed straight through to vkQueueSubmit2, so a
 * caller can add a client's acquire fence as a wait and an exportable
 * semaphore as a signal without this layer knowing what either is for.
 *
 * Returns the timeline value that this submission will signal -- the value to
 * hand to avk_retire_push() for anything the commands touched -- or 0 on
 * failure.
 */
uint64_t avk_cmd_ring_submit(struct avk_cmd_ring *ring,
	const VkSemaphoreSubmitInfo *waits, uint32_t wait_count,
	const VkSemaphoreSubmitInfo *signals, uint32_t signal_count);

/* Abandon the in-flight recording without submitting. For error paths: a
 * begun command buffer that is never ended or submitted would leak the slot
 * forever. */
void avk_cmd_ring_abandon(struct avk_cmd_ring *ring);

#endif /* AVK_COMMAND_H */
