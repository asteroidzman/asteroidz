#ifndef AVK_UPLOAD_WORKER_H
#define AVK_UPLOAD_WORKER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "avk_upload.h"

/*
 * ── GETTING THE CLIENT MEMCPY OFF THE EVENT LOOP ──────────────────────────
 *
 * A wl_shm client's pixels have to be copied from its mapping into staging
 * before the GPU can read them, and the only moment that mapping is guaranteed
 * to hold the committed content is inside the surface-commit handler:
 * wlr_compositor unlocks `surface->current.buffer` immediately after emitting
 * the commit signal, precisely so a shm buffer can be released to the client
 * as soon as the compositor has taken a copy.
 *
 * Doing the copy there costs exactly what it costs. Measured on this tree: a
 * 1637x1160 Qt window committing ~6.5 MB per repaint, 26.5 GB in one session,
 * memcpy at ~27% of all samples -- and, because the event loop is not in
 * poll() while it runs, libinput reporting "client bug: event processing
 * lagging behind by 47-53ms" on the mouse AND the keyboard.
 *
 * The copy cannot be made smaller (that client damages 96.6% of its surface
 * per repaint, so the damage-rectangle path is already doing its job) and it
 * cannot be skipped. It can be moved. This is a worker thread that does
 * nothing but avk_upload_pack(): read a client mapping, write staging. No
 * Vulkan, no wlroots, no allocation. The main thread keeps everything that
 * must stay on one thread -- deciding what to copy, sizing staging, recording
 * and submitting -- and none of that is measurable next to the memcpy.
 *
 * WHAT KEEPS THE CLIENT'S PIXELS ALIVE while the worker reads them is the
 * caller's job and not this file's: the buffer must be locked and its data-ptr
 * access held for the whole life of the job. See az_avk_shm_job_start().
 *
 * SIGBUS is already handled correctly across threads, and that was checked
 * rather than assumed: wlroots installs a process-wide SIGBUS handler in
 * wlr_buffer_begin_data_ptr_access() and threads its registrations onto an
 * `_Atomic` global (types/wlr_shm.c). A client that shrinks its pool while the
 * worker is reading it therefore lands in the same recovery path it would have
 * landed in on the main thread.
 *
 * COMPLETION reaches the event loop through an eventfd rather than a poll or a
 * timer: the loop is already in poll(), so a completed copy costs one wakeup
 * and the submission happens in the same dispatch round. Nothing spins and
 * nothing waits.
 */

struct avk_upload_job {
	/* Filled by the poster. */
	struct avk_upload_plan plan;
	const void *src;      /* the client's mapping, kept valid by the poster */
	void *dst;            /* persistently mapped staging */
	void *user;           /* the poster's own object; never touched here */

	/*
	 * 0 queued, 1 claimed (being packed), 2 packed.
	 *
	 * A CAS rather than a flag, because the main thread is allowed to STEAL a
	 * job it needs right now: if the worker has not started it, whoever wins
	 * the claim does the copy. Waiting for a busy worker to reach a job that
	 * the caller could have done itself is the one way this design could be
	 * slower than the code it replaces.
	 */
	atomic_int state;

	struct avk_upload_job *next;   /* worker queue; owned by the worker */
};

enum { AVK_UPLOAD_JOB_QUEUED = 0, AVK_UPLOAD_JOB_CLAIMED = 1,
	AVK_UPLOAD_JOB_DONE = 2 };

struct avk_upload_worker;

/* Start the worker. NULL if the thread or the eventfd could not be created,
 * which the caller must treat as "do it synchronously" and not as fatal. */
struct avk_upload_worker *avk_upload_worker_create(void);

/* Stop the worker and join it. Any job still queued is packed first, so no
 * caller is left holding a job that will never complete. */
void avk_upload_worker_destroy(struct avk_upload_worker *w);

/* The eventfd the worker writes when a job completes. Add it to the event
 * loop and read 8 bytes when it fires. Never negative for a live worker. */
int avk_upload_worker_fd(const struct avk_upload_worker *w);

/* Hand a job over. The job must stay alive until it reports done. */
void avk_upload_worker_post(struct avk_upload_worker *w,
	struct avk_upload_job *job);

/* Has the copy finished? Non-blocking. */
static inline bool avk_upload_job_done(const struct avk_upload_job *job) {
	return atomic_load_explicit((atomic_int *)&job->state,
		memory_order_acquire) == AVK_UPLOAD_JOB_DONE;
}

/*
 * Make sure this job is packed, doing the work here if the worker has not
 * started it. Returns the nanoseconds THIS THREAD spent, which is the number
 * the whole exercise exists to drive to zero -- 0 when the job was already
 * done, the remaining wait when the worker was mid-copy, and the whole copy
 * when the main thread stole it.
 */
uint64_t avk_upload_worker_finish(struct avk_upload_worker *w,
	struct avk_upload_job *job);

/* Jobs packed by the worker, and jobs the posting thread had to do itself. */
void avk_upload_worker_counts(const struct avk_upload_worker *w,
	uint64_t *packed, uint64_t *stolen);

#endif /* AVK_UPLOAD_WORKER_H */
