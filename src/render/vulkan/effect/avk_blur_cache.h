#ifndef AVK_BLUR_CACHE_H
#define AVK_BLUR_CACHE_H

/*
 * The monitor background blur result cache -- resource ownership and validity.
 *
 * struct avk_blur_cache and the reason enum live in ../scene/avk_render.h, next
 * to the renderer that uses them and to the contract that describes what makes
 * an entry valid. Only the operations are here.
 */
#include "../scene/avk_render.h"

/*
 * Do two kernels produce the same picture from the same source?
 *
 * Exact float equality, deliberately. These are config values multiplied by a
 * constant output scale, so two frames with the same configuration produce
 * bit-identical floats and a difference IS a change. An epsilon here could only
 * ever keep a cache that no longer matches its parameters.
 */
bool avk_blur_params_equal(const struct avk_blur_params *a,
	const struct avk_blur_params *b);

/*
 * Is the cache usable for a frame with this identity?
 *
 * Pure: it reads, decides and returns a reason. It never mutates the cache and
 * never allocates, so the answer can be taken twice (once to decide, once to
 * log) without a side effect between them.
 *
 * `force_rebuild` is the falsifier's entry point -- see
 * AZ_BLUR_CACHE_ALWAYS_DIRTY. It is checked FIRST so that a forced run always
 * reports FORCED rather than whichever ordinary reason happened to also apply.
 */
enum avk_blur_cache_reason avk_blur_cache_check(
	const struct avk_blur_cache *cache, uint64_t generation,
	int32_t origin_x, int32_t origin_y, uint32_t width, uint32_t height,
	VkFormat format, const struct avk_blur_params *params,
	bool force_rebuild);

/* Add one to the counter for `r` and to the invalidation total. OK counts
 * nothing -- it is not an invalidation. */
void avk_blur_cache_count_reason(struct avk_blur_cache *cache,
	enum avk_blur_cache_reason r);

/*
 * The image the next rebuild must render into: the slot the previous frame is
 * NOT sampling, created or resized as needed, with the old one handed to the
 * retire queue at `last_submit`. Advances cache->slot. NULL on allocation
 * failure, in which case the cache is left exactly as it was and the caller
 * must take the live path.
 */
struct avk_image *avk_blur_cache_next_slot(struct avk_blur_cache *cache,
	struct avk_device *dev, struct avk_retire_queue *retire,
	VkFormat format, uint32_t width, uint32_t height, bool unsafe_reuse);

/*
 * Release both slots. With `retire` non-NULL each image is destroyed once the
 * GPU passes its own last use; with it NULL they are destroyed immediately, which
 * is only correct from teardown after the device is idle.
 */
void avk_blur_cache_finish(struct avk_blur_cache *cache,
	struct avk_device *dev, struct avk_retire_queue *retire);

#endif /* AVK_BLUR_CACHE_H */
