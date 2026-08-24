/* _GNU_SOURCE for pthread_setname_np(), so this thread is identifiable in a
 * profile rather than showing up as an anonymous second thread in the
 * compositor -- which is exactly the thing a reader of a perf report should
 * not have to guess about. */
#define _GNU_SOURCE

#include "avk_upload_worker.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

struct avk_upload_worker {
	pthread_t thread;
	bool thread_started;

	pthread_mutex_t lock;
	pthread_cond_t queued;     /* the worker waits here for work */
	pthread_cond_t finished;   /* a caller waits here for one job */

	/* FIFO of jobs the worker has not claimed yet. A job may leave this queue
	 * without the worker ever seeing it -- see the steal in
	 * avk_upload_worker_finish() -- so the worker skips anything already
	 * claimed rather than assuming it owns what it dequeues. */
	struct avk_upload_job *head, *tail;

	bool stopping;
	int notify_fd;

	uint64_t packed;   /* jobs the worker did */
	uint64_t stolen;   /* jobs the calling thread had to do itself */

	/*
	 * How long the memcpy ITSELF took, on this thread, and over how many
	 * bytes.
	 *
	 * Without this the only available number was the join wait, and
	 * az_avk_shm_job_settle() then recorded that wait as a copy time -- so
	 * "the copy took 59ms" and "the frame waited 59ms" were the same
	 * measurement printed twice, and could not disagree. They answer
	 * different questions: a slow copy is a bandwidth or page-fault problem,
	 * a long wait behind a fast copy is a scheduling or wake-up problem, and
	 * the fixes have nothing in common.
	 *
	 * Written under `lock`, which the worker retakes after the pack anyway.
	 */
	uint64_t pack_ns_max, pack_ns_sum, pack_count, pack_bytes;
	/*
	 * A standalone benchmark of the same bytes into the same Vulkan memory
	 * type once ran 22x faster than the compositor's own copy, which is a
	 * difference no property of the
	 * allocation can explain.
	 */
};

/*
 * ONE worker, not a pool.
 *
 * The copies are per-surface and independent, so a pool would parallelise
 * them -- but the load this exists for is a handful of megabytes per frame,
 * which one core moves in a couple of milliseconds inside a 16.7 ms budget.
 * A second thread would buy nothing measurable and would cost a second thread
 * competing with the compositor for the CPU it is trying to free up. If a
 * workload ever arrives that queues faster than one thread drains, the steal
 * path already covers correctness and the counters (`stolen`) will say so
 * before anyone has to guess.
 */

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Pop the next job nobody has claimed. Called with the lock held. */
static struct avk_upload_job *dequeue(struct avk_upload_worker *w) {
	struct avk_upload_job *job = w->head;
	if (job == NULL) {
		return NULL;
	}
	w->head = job->next;
	if (w->head == NULL) {
		w->tail = NULL;
	}
	job->next = NULL;
	return job;
}

/* Take this job out of the queue wherever it is. Called with the lock held.
 * Returns true if it was still there, which is what makes the steal safe: the
 * job is gone from the queue before it is claimed, so the worker cannot pick
 * it up in between. */
static bool unlink_job(struct avk_upload_worker *w,
		struct avk_upload_job *job) {
	struct avk_upload_job **link = &w->head;
	struct avk_upload_job *prev = NULL;
	while (*link != NULL) {
		if (*link == job) {
			*link = job->next;
			if (w->tail == job) {
				w->tail = prev;
			}
			job->next = NULL;
			return true;
		}
		prev = *link;
		link = &(*link)->next;
	}
	return false;
}

static void notify(struct avk_upload_worker *w) {
	uint64_t one = 1;
	ssize_t n;
	do {
		n = write(w->notify_fd, &one, sizeof(one));
	} while (n < 0 && errno == EINTR);
}

static void *worker_main(void *data) {
	struct avk_upload_worker *w = data;
	pthread_mutex_lock(&w->lock);
	for (;;) {
		struct avk_upload_job *job = dequeue(w);
		if (job == NULL) {
			if (w->stopping) {
				break;
			}
			pthread_cond_wait(&w->queued, &w->lock);
			continue;
		}
		int expected = AVK_UPLOAD_JOB_QUEUED;
		if (!atomic_compare_exchange_strong(&job->state, &expected,
				AVK_UPLOAD_JOB_CLAIMED)) {
			/* Stolen between the post and here. Nothing to do and nothing to
			 * signal: whoever claimed it signals. */
			continue;
		}
		w->packed++;
		pthread_mutex_unlock(&w->lock);

		/* THE ONLY WORK THIS THREAD DOES, and the only reason it exists. */
		/* Read BEFORE the pack: storing JOB_DONE below lets a waiter free the
		 * job, so reading plan through it afterwards is a use-after-free that
		 * only appears under a race. */
		uint64_t pack_t0 = now_ns();
		uint64_t pack_bytes = job->plan.bytes;
		uint32_t pack_rects = job->plan.rect_count;
		uint32_t pack_stride = job->plan.stride;
		uint32_t pack_rows = job->plan.height;
		bool pack_full = job->plan.full;
		avk_upload_pack(&job->plan, job->src, job->dst);
		uint64_t pack_ns = now_ns() - pack_t0;

		atomic_store_explicit(&job->state, AVK_UPLOAD_JOB_DONE,
			memory_order_release);
		/* The broadcast is for a caller already blocked in finish(); the
		 * eventfd is for the event loop, which is in poll() and would
		 * otherwise not learn about this until its next frame. */
		pthread_mutex_lock(&w->lock);
		w->pack_count++;
		w->pack_ns_sum += pack_ns;
		w->pack_bytes += pack_bytes;
		if (pack_ns > w->pack_ns_max) {
			w->pack_ns_max = pack_ns;
		}
		/*
		 * THE OUTLIER, described rather than averaged.
		 *
		 * The mean pack is ~5.7ms and the worst is ~57ms, and a mean cannot
		 * tell which of several possible shapes the slow one had: a huge
		 * buffer, a plan of many narrow rectangles whose row-by-row copy is
		 * nearly all per-call overhead, or ordinary bytes moving at a
		 * fraction of memcpy speed because something else owns the memory
		 * bus. Those have different fixes, and the frame that stalls is
		 * always one of these -- never the average.
		 *
		 * 20ms is chosen against the 6.9ms frame budget: past three frames,
		 * this copy is why something visibly stuttered.
		 */
		if (pack_ns > 20000000ull) {
			avk_log(AVK_ERROR,
				"slow upload pack: %.1fms for %.2fMB in %u rect(s) %s "
				"= %.0fMB/s (stride %u, %u rows)",
				(double)pack_ns / 1e6, (double)pack_bytes / 1e6,
				pack_rects, pack_full ? "whole" : "partial",
				(double)pack_bytes * 1000.0 / (double)pack_ns,
				pack_stride, pack_rows);
		}
		pthread_cond_broadcast(&w->finished);
		notify(w);
	}
	pthread_mutex_unlock(&w->lock);
	return NULL;
}

struct avk_upload_worker *avk_upload_worker_create(void) {
	struct avk_upload_worker *w = calloc(1, sizeof(*w));
	if (w == NULL) {
		return NULL;
	}
	w->notify_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (w->notify_fd < 0) {
		avk_log(AVK_ERROR, "upload worker: eventfd failed: %s",
			strerror(errno));
		free(w);
		return NULL;
	}
	pthread_mutex_init(&w->lock, NULL);
	pthread_cond_init(&w->queued, NULL);
	pthread_cond_init(&w->finished, NULL);
	if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
		avk_log(AVK_ERROR, "upload worker: pthread_create failed: %s",
			strerror(errno));
		pthread_cond_destroy(&w->finished);
		pthread_cond_destroy(&w->queued);
		pthread_mutex_destroy(&w->lock);
		close(w->notify_fd);
		free(w);
		return NULL;
	}
	w->thread_started = true;
	pthread_setname_np(w->thread, "avk-upload");
	return w;
}

void avk_upload_worker_destroy(struct avk_upload_worker *w) {
	if (w == NULL) {
		return;
	}
	if (w->thread_started) {
		pthread_mutex_lock(&w->lock);
		w->stopping = true;
		pthread_cond_broadcast(&w->queued);
		pthread_mutex_unlock(&w->lock);
		pthread_join(w->thread, NULL);
	}
	/*
	 * Anything still queued is packed here rather than dropped. A caller
	 * holding a job that never completes would wait for it forever at
	 * teardown, and a job silently discarded would leave an image holding a
	 * generation nobody wrote. Neither is worth saving a memcpy at shutdown.
	 */
	struct avk_upload_job *job;
	while ((job = dequeue(w)) != NULL) {
		int expected = AVK_UPLOAD_JOB_QUEUED;
		if (atomic_compare_exchange_strong(&job->state, &expected,
				AVK_UPLOAD_JOB_CLAIMED)) {
			avk_upload_pack(&job->plan, job->src, job->dst);
			atomic_store_explicit(&job->state, AVK_UPLOAD_JOB_DONE,
				memory_order_release);
		}
	}
	pthread_cond_destroy(&w->finished);
	pthread_cond_destroy(&w->queued);
	pthread_mutex_destroy(&w->lock);
	close(w->notify_fd);
	free(w);
}

int avk_upload_worker_fd(const struct avk_upload_worker *w) {
	return w == NULL ? -1 : w->notify_fd;
}

void avk_upload_worker_post(struct avk_upload_worker *w,
		struct avk_upload_job *job) {
	atomic_store_explicit(&job->state, AVK_UPLOAD_JOB_QUEUED,
		memory_order_release);
	job->next = NULL;
	pthread_mutex_lock(&w->lock);
	if (w->tail != NULL) {
		w->tail->next = job;
	} else {
		w->head = job;
	}
	w->tail = job;
	pthread_cond_signal(&w->queued);
	pthread_mutex_unlock(&w->lock);
}

uint64_t avk_upload_worker_finish(struct avk_upload_worker *w,
		struct avk_upload_job *job) {
	if (avk_upload_job_done(job)) {
		return 0;
	}
	uint64_t t0 = now_ns();
	if (w == NULL) {
		/* The worker is gone and this job never reached it. Nothing else can
		 * claim it, so pack it here rather than leaving a caller waiting on a
		 * completion that can no longer arrive. */
		int expected = AVK_UPLOAD_JOB_QUEUED;
		if (atomic_compare_exchange_strong(&job->state, &expected,
				AVK_UPLOAD_JOB_CLAIMED)) {
			avk_upload_pack(&job->plan, job->src, job->dst);
			atomic_store_explicit(&job->state, AVK_UPLOAD_JOB_DONE,
				memory_order_release);
		}
		return now_ns() - t0;
	}

	pthread_mutex_lock(&w->lock);
	bool mine = false;
	if (unlink_job(w, job)) {
		/* Still queued, so the worker has not seen it. Claiming it here can
		 * only succeed -- nothing else can reach it now -- but the CAS is
		 * kept because "can only succeed" is an argument, not a guarantee. */
		int expected = AVK_UPLOAD_JOB_QUEUED;
		mine = atomic_compare_exchange_strong(&job->state, &expected,
			AVK_UPLOAD_JOB_CLAIMED);
		if (mine) {
			w->stolen++;
		}
	}
	pthread_mutex_unlock(&w->lock);

	if (mine) {
		avk_upload_pack(&job->plan, job->src, job->dst);
		atomic_store_explicit(&job->state, AVK_UPLOAD_JOB_DONE,
			memory_order_release);
		return now_ns() - t0;
	}

	/* The worker has it and is partway through. Wait for the rest. */
	pthread_mutex_lock(&w->lock);
	while (!avk_upload_job_done(job)) {
		pthread_cond_wait(&w->finished, &w->lock);
	}
	pthread_mutex_unlock(&w->lock);
	return now_ns() - t0;
}

void avk_upload_worker_counts(const struct avk_upload_worker *w,
		uint64_t *packed, uint64_t *stolen) {
	if (w == NULL) {
		*packed = 0;
		*stolen = 0;
		return;
	}
	*packed = w->packed;
	*stolen = w->stolen;
}

void avk_upload_worker_timing(struct avk_upload_worker *w,
		uint64_t *ns_max, uint64_t *ns_sum, uint64_t *count, uint64_t *bytes) {
	uint64_t mx = 0, sum = 0, n = 0, b = 0;
	if (w != NULL) {
		/* Under the lock: the worker writes these from its own thread, and a
		 * torn 64-bit read would be a measurement that never happened. */
		pthread_mutex_lock(&w->lock);
		mx = w->pack_ns_max;
		sum = w->pack_ns_sum;
		n = w->pack_count;
		b = w->pack_bytes;
		pthread_mutex_unlock(&w->lock);
	}
	if (ns_max != NULL) { *ns_max = mx; }
	if (ns_sum != NULL) { *ns_sum = sum; }
	if (count != NULL)  { *count = n; }
	if (bytes != NULL)  { *bytes = b; }
}
