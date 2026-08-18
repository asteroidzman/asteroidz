#define _POSIX_C_SOURCE 200809L

#include "avk_retire.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

void avk_retire_init(struct avk_retire_queue *q, const char *name) {
	memset(q, 0, sizeof(*q));
	q->name = name;
}

/*
 * Run every remaining destructor, whatever its timeline value.
 *
 * Ignoring the timeline is correct HERE and nowhere else: the caller has
 * established that the GPU is idle, so every value has been passed by
 * definition and filtering on it would only leak whatever the counter failed
 * to report. It is correct because of the precondition, not because timeline
 * values stop mattering at shutdown -- so the precondition is checked rather
 * than assumed.
 */
void avk_retire_finish(struct avk_retire_queue *q, struct avk_device *dev) {
	if (dev != NULL && q->len > 0) {
		uint64_t reached = avk_device_timeline_value(dev);
		for (size_t i = 0; i < q->len; i++) {
			if (q->entries[i].timeline_value > reached) {
				dev->lifecycle_violations++;
				avk_log(AVK_ERROR, "retire queue '%s': finishing an entry at "
					"timeline %" PRIu64 " with the GPU only at %" PRIu64
					" -- the device was not idle before teardown",
					q->name != NULL ? q->name : "?",
					q->entries[i].timeline_value, reached);
				break;
			}
		}
	}
	for (size_t i = 0; i < q->len; i++) {
		q->entries[i].fn(dev, q->entries[i].data);
		AVK_LIVE_DEC(dev, retire_entries);
	}
	free(q->entries);
	const char *name = q->name;
	memset(q, 0, sizeof(*q));
	q->name = name;
}

bool avk_retire_push(struct avk_retire_queue *q, struct avk_device *dev,
		uint64_t timeline_value, avk_retire_fn fn, void *data) {
	/*
	 * The same resource queued twice is destroyed twice. Linear, because the
	 * queue is short by construction -- it holds only what the GPU has not yet
	 * finished with, which is a few frames' worth -- and because a hash table
	 * for a list of tens of entries would cost more than it saved.
	 *
	 * Refusing is the right recovery, not merely the safe one: the resource
	 * already has a destruction owner, and the second caller believing it also
	 * owns it is the bug being reported.
	 */
	for (size_t i = 0; i < q->len; i++) {
		if (q->entries[i].data == data && q->entries[i].fn == fn) {
			q->duplicate_pushes++;
			if (dev != NULL) {
				dev->lifecycle_violations++;
			}
			avk_log(AVK_ERROR, "retire queue '%s': %p is already queued for "
				"destruction and was handed over a second time -- it has two "
				"owners and one of them is wrong",
				q->name != NULL ? q->name : "?", data);
			return false;
		}
	}

	if (q->len == q->cap) {
		size_t cap = q->cap == 0 ? 32 : q->cap * 2;
		struct avk_retire_entry *entries =
			realloc(q->entries, cap * sizeof(*entries));
		if (entries == NULL) {
			avk_log(AVK_ERROR, "retire queue: out of memory, destroying "
				"immediately (this may race the GPU)");
			fn(dev, data);
			return false;
		}
		q->entries = entries;
		q->cap = cap;
	}

	q->entries[q->len++] = (struct avk_retire_entry){
		.timeline_value = timeline_value,
		.fn = fn,
		.data = data,
	};
	AVK_LIVE_INC(dev, retire_entries);
	if (q->len > q->peak) {
		q->peak = q->len;
		/* Loud once per new high-water mark rather than every push: a queue
		 * that grows without bound is a real bug and this is how it announces
		 * itself, but only after it is well past anything ordinary. */
		if (q->peak >= 4096 && (q->peak & (q->peak - 1)) == 0) {
			avk_log(AVK_WARN, "retire queue has reached %zu entries -- "
				"something is being deferred but never retired", q->peak);
		}
	}
	return true;
}

size_t avk_retire_collect_fn(struct avk_retire_queue *q,
		struct avk_device *dev, avk_retire_fn only) {
	if (q->len == 0) {
		return 0;
	}
	uint64_t reached = avk_device_timeline_value(dev);
	size_t kept = 0;
	size_t ran = 0;
	for (size_t i = 0; i < q->len; i++) {
		if (q->entries[i].fn == only
				&& q->entries[i].timeline_value <= reached) {
			q->entries[i].fn(dev, q->entries[i].data);
			AVK_LIVE_DEC(dev, retire_entries);
			ran++;
		} else {
			q->entries[kept++] = q->entries[i];
		}
	}
	q->len = kept;
	return ran;
}

size_t avk_retire_collect(struct avk_retire_queue *q, struct avk_device *dev) {
	if (q->len == 0) {
		return 0;
	}

	/* One counter read for the whole queue. */
	uint64_t reached = avk_device_timeline_value(dev);

	size_t kept = 0;
	size_t ran = 0;
	for (size_t i = 0; i < q->len; i++) {
		if (q->entries[i].timeline_value <= reached) {
			q->entries[i].fn(dev, q->entries[i].data);
			AVK_LIVE_DEC(dev, retire_entries);
			ran++;
		} else {
			/* Compact in place. Entries are not sorted -- an upload and a
			 * frame reserve points independently -- so this is a filter, not
			 * a prefix trim. */
			q->entries[kept++] = q->entries[i];
		}
	}
	q->len = kept;
	return ran;
}
