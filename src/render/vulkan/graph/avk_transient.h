#ifndef AVK_TRANSIENT_H
#define AVK_TRANSIENT_H

#include "../command/avk_retire.h"
#include "../image/avk_image.h"

/*
 * Images that exist for a frame, and are not allocated for one.
 *
 * WHAT THIS IS FOR. M4F renders a background into an intermediate, blurs it,
 * and composites the result. Those intermediates have no owner outside the
 * frame -- but creating and destroying a VkImage, its memory and its view every
 * frame is a driver-side allocation on the one code path with a deadline, and
 * the M4E.0 audit's headline finding was that AVK does not currently do that
 * ANYWHERE. This pool is what keeps that true once transients exist.
 *
 * THE LIFETIME RULE, WHICH IS THE WHOLE FILE. A transient does NOT become
 * reusable when CPU recording ends. It becomes reusable when the GPU has
 * finished with it, which is a timeline comparison and nothing else:
 *
 *     reusable  <=>  entry->last_use <= avk_device_timeline_value(dev)
 *
 * And when nothing reusable is available, this pool ALLOCATES. It never waits.
 * A pool that blocked until a slot freed would be a CPU wait on the frame path,
 * which is the one invariant AVK has kept since M3.5 -- and it would turn a
 * memory problem into a latency problem, which is much harder to find later.
 * `cpu_sync_waits` stays 0 by construction here, not by care.
 *
 * WHY IT HANDS OUT struct avk_image. Because everything else already works on
 * one: avk_pipelines_texture_set() caches a sampler descriptor ON the image, so
 * a reused transient keeps its descriptor set and the frame path allocates no
 * descriptors; avk_image_destroy() is already retire-queue shaped; and the
 * graph's resource tracking takes an avk_image and does not care where it came
 * from. A bespoke transient type would have needed all three again.
 *
 * WHY IT IS SHARED BETWEEN OUTPUTS. The pool hangs off the renderer, and a
 * renderer is keyed on VkFormat rather than on output (see the M4E.0 audit), so
 * two monitors composite through one. Two outputs wanting a same-sized
 * transient in the same period must get DIFFERENT images -- and they do, for
 * free, because output A's is still in flight when output B acquires. That is
 * the timeline rule doing its job; a pool keyed on a frame index would have
 * handed out the same one.
 */

/*
 * Everything that makes two images interchangeable. Nothing else.
 *
 * These are the properties Vulkan checks when an image is used, so two images
 * agreeing on all of them are substitutable and two differing in any of them
 * are not. Extent is here because a view and a render area are sized from it;
 * `usage` is here because a VkImage created without SAMPLED_BIT cannot be
 * sampled however much the rest matches, and reusing one that way is a
 * validation error rather than a slow path.
 *
 * What is deliberately NOT in the key: what the image was last used FOR, which
 * output asked for it, and its current layout. The first two are irrelevant to
 * compatibility, and the third is tracked per frame by the graph, which
 * transitions from whatever it finds.
 */
struct avk_transient_key {
	VkFormat format;
	uint32_t width, height;
	VkImageUsageFlags usage;
	VkSampleCountFlagBits samples;
	VkImageType type;
	VkImageTiling tiling;
};

struct avk_transient_entry {
	struct avk_transient_key key;
	struct avk_image *image;
	VkDeviceSize bytes;

	/*
	 * The timeline point of the last submission that used it. Reuse is legal
	 * once the device timeline has passed this, and at no earlier moment.
	 * 0 means it has never been submitted.
	 */
	uint64_t last_use;
	/* The pool's frame counter when it was last handed out, for retirement of
	 * sizes nothing asks for any more. */
	uint64_t last_frame;
	/* Acquired this frame and not yet released. */
	bool in_use;
};

struct avk_transient_stats {
	uint64_t acquires;
	uint64_t reuses;         /* served from an existing image */
	uint64_t creates;        /* a new VkImage was allocated */
	/* WHY a create was needed. Exactly one is incremented per create, so
	 * miss_no_key + miss_in_use + miss_in_flight == creates. */
	uint64_t miss_no_key;    /* nothing of that key existed at all */
	uint64_t miss_in_use;    /* one existed, held by this same frame */
	uint64_t miss_in_flight; /* one existed and was idle, but still on the GPU */
	uint64_t retires;        /* entries handed to the retire queue */
	/*
	 * Acquires served by an image the GPU had NOT finished with.
	 *
	 * Structurally zero: the only path that can produce one is the M4E.4 break.
	 * A counter rather than a comment because "never reused early" is exactly
	 * the kind of claim that is true until it is not, and a corruption bug
	 * caused by one of these would otherwise present as a driver problem.
	 */
	uint64_t unsafe_reuses;

	VkDeviceSize bytes;      /* currently held */
	VkDeviceSize peak_bytes;
	uint32_t live;           /* entries in the pool */
	uint32_t peak_live;
};

struct avk_transient_pool {
	struct avk_device *dev;
	/* Borrowed. Retirement goes through the same queue as everything else, so
	 * an image destroyed because an output changed mode is destroyed at the
	 * same timeline point discipline as one destroyed because a window
	 * closed. */
	struct avk_retire_queue *retire;

	struct avk_transient_entry *entries;
	uint32_t len, cap;
	uint64_t frame;

	/*
	 * EXTENT GRANULARITY, in pixels, and the reason it is not 1.
	 *
	 * A resize animation asks for 800x500, then 810x505, then 820x510. With
	 * exact extents every one of those is a different key and therefore a new
	 * VkImage -- an allocation storm for the length of the drag, which is
	 * precisely the workload transient reuse exists to survive. Rounding the
	 * BACKING extent up to a multiple of this collapses the sweep onto a handful
	 * of images, and the pass renders into the top-left of a possibly-larger
	 * one using its own viewport and scissor.
	 *
	 * The cost is wasted memory, bounded by the granularity: at 64, a
	 * 1920x1080 request backs onto 1920x1088, which is 0.7%. Measured against
	 * exact extents; AZ_TRANSIENT_GRANULARITY
	 * overrides it, and 1 restores exact extents so the two can be compared in
	 * one run rather than argued about.
	 */
	uint32_t granularity;
	/* AZ_TRANSIENT_TRACE=1: one line per CREATE, with the reason no existing
	 * entry could serve it. Off by default; creates are rare in steady state
	 * and a flood of them is exactly the thing being diagnosed. */
	bool trace;

	/* Cache ceiling. Idle entries are retired oldest-first once the pool holds
	 * more than this. Not a per-frame purge: an effect whose intermediate is
	 * dropped and recreated every frame is the allocation storm again, wearing
	 * a different hat. */
	VkDeviceSize budget;

	struct avk_transient_stats stats;

	/*
	 * M4E.4 break: hand out an image whose last_use the GPU has not reached.
	 *
	 * Deterministic, which is why it exists at all. It does not depend on
	 * catching a race -- the pool COUNTS the violation as it commits it, so a
	 * test asserts on `unsafe_reuses` rather than on whether corruption
	 * happened to appear. Synchronization validation reports the resulting
	 * write-after-read hazard as well, which is the second, independent signal.
	 */

	/*
	 * M4F test aid: fill every newly created image with a colour nothing else
	 * in the system produces, so that any consumer which samples outside the
	 * region it wrote produces an unmistakable result rather than a plausible
	 * one.
	 *
	 * The pool rounds allocations up to `granularity`, so a 32x32 blur level
	 * lives in a 64x64 image and three quarters of it is never written. Relying
	 * on that memory being zero is relying on an accident: it is whatever the
	 * driver last left there, which on a reused image is another window's blur.
	 * Nothing here may read outside what it wrote.
	 */
};

void avk_transient_pool_init(struct avk_transient_pool *pool,
	struct avk_device *dev, struct avk_retire_queue *retire);
/* Destroys every image immediately. The GPU must already be idle -- the caller
 * establishes that, as with avk_renderer_finish(). */
void avk_transient_pool_finish(struct avk_transient_pool *pool);

/*
 * An image at least `width` x `height` in `format`, ready to use.
 *
 * The returned image's `extent` is the BACKING extent, which may be larger than
 * what was asked for -- see `granularity`. The caller renders into the region
 * it asked for and must not assume the rest is anything in particular.
 *
 * Returns NULL only if allocation failed. Never blocks, and never returns an
 * image the GPU is still using.
 */
struct avk_image *avk_transient_acquire(struct avk_transient_pool *pool,
	VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage);

/*
 * Every image acquired since the last call becomes reusable once the GPU passes
 * `timeline_value`. Call once per frame, after submitting.
 *
 * A single call for the whole frame rather than a release per image: they were
 * all used by one submission, so they all become free at one timeline point,
 * and asking the caller to track them individually would be asking it to
 * re-derive something the pool already knows.
 */
void avk_transient_release_frame(struct avk_transient_pool *pool,
	uint64_t timeline_value);

/*
 * Retire what is no longer wanted: entries idle for longer than
 * AVK_TRANSIENT_IDLE_FRAMES, and -- oldest first -- whatever is over budget.
 * Never blocks; call beside avk_retire_collect().
 */
void avk_transient_pool_collect(struct avk_transient_pool *pool);

/*
 * Retire every entry whose key no longer describes anything wanted, e.g. after
 * an output resize or mode change. Safe with work in flight: entries go to the
 * retire queue against their own last_use, never destroyed in place.
 */
void avk_transient_pool_drop_all(struct avk_transient_pool *pool);

/* How many frames an unused entry survives before it is retired. Two seconds at
 * 60Hz: long enough that a window that stops animating and starts again does
 * not reallocate, short enough that a size nothing wants any more does not sit
 * in memory for the session. */
#define AVK_TRANSIENT_IDLE_FRAMES 120

/* Rounded-up backing extent for a request. Exposed so a test can state the
 * expected key independently of the pool's own arithmetic. */
uint32_t avk_transient_round(uint32_t v, uint32_t granularity);

#endif /* AVK_TRANSIENT_H */
