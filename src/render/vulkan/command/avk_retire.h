#ifndef AVK_RETIRE_H
#define AVK_RETIRE_H

#include "../device/avk_device.h"

/*
 * Deferred destruction, keyed to GPU timeline progress.
 *
 * The rule this enforces: nothing the GPU might still be reading is ever
 * destroyed. The usual ways of getting that wrong are (a) destroying
 * immediately and hoping, which corrupts under load and looks like a driver
 * bug, and (b) calling vkDeviceWaitIdle before destroying, which is correct
 * and costs the entire pipeline every time a window closes.
 *
 * Here, a resource is handed to the queue together with the timeline value of
 * the last submission that touched it. Once per frame the queue is collected:
 * one vkGetSemaphoreCounterValue, then every entry at or below that value runs
 * its destructor. No waiting, ever.
 */

typedef void (*avk_retire_fn)(struct avk_device *dev, void *data);

struct avk_retire_entry {
	uint64_t timeline_value;
	avk_retire_fn fn;
	void *data;
};

struct avk_retire_queue {
	struct avk_retire_entry *entries;
	size_t len;
	size_t cap;

	/* High-water mark. A queue that keeps growing means something is being
	 * pushed and never retired -- usually a submission that failed, so its
	 * timeline point is never reached. */
	size_t peak;
};

void avk_retire_init(struct avk_retire_queue *q);

/*
 * Run every remaining destructor unconditionally.
 *
 * Only safe once the GPU is idle, which is why it takes the device and is
 * called from teardown after avk_device_destroy's wait. Anywhere else it would
 * be freeing memory the GPU is reading.
 */
void avk_retire_finish(struct avk_retire_queue *q, struct avk_device *dev);

/*
 * Destroy `data` with `fn` once the GPU passes `timeline_value`.
 *
 * A value of 0 means "nothing ever submitted touched this", so it runs at the
 * next collect. Returns false only if the queue could not grow, in which case
 * the destructor is run immediately -- leaking would be worse, and by that
 * point the process is out of memory anyway.
 */
bool avk_retire_push(struct avk_retire_queue *q, struct avk_device *dev,
	uint64_t timeline_value, avk_retire_fn fn, void *data);

/* Run everything the GPU has finished with. Returns how many ran. Never
 * blocks; call it once per frame. */
size_t avk_retire_collect(struct avk_retire_queue *q, struct avk_device *dev);

#endif /* AVK_RETIRE_H */
