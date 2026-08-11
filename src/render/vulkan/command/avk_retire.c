#define _POSIX_C_SOURCE 200809L

#include "avk_retire.h"

#include <stdlib.h>
#include <string.h>

void avk_retire_init(struct avk_retire_queue *q) {
	memset(q, 0, sizeof(*q));
}

void avk_retire_finish(struct avk_retire_queue *q, struct avk_device *dev) {
	for (size_t i = 0; i < q->len; i++) {
		q->entries[i].fn(dev, q->entries[i].data);
	}
	free(q->entries);
	memset(q, 0, sizeof(*q));
}

bool avk_retire_push(struct avk_retire_queue *q, struct avk_device *dev,
		uint64_t timeline_value, avk_retire_fn fn, void *data) {
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
