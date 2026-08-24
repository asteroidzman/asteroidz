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
 * `force_rebuild` is checked FIRST, so a forced run always reports FORCED
 * rather than whichever ordinary reason happened to also apply.
 */
enum avk_blur_cache_reason avk_blur_cache_check(
	const struct avk_blur_cache *cache, uint64_t generation,
	uint64_t source_hash,
	int32_t origin_x, int32_t origin_y, uint32_t width, uint32_t height,
	VkFormat format, const struct avk_blur_params *params,
	enum avk_blur_cache_kind kind, bool force_rebuild, bool shared_identity);

/*
 * A digest of the commands that will be blurred into the cache: everything
 * about the prefix that decides its pixels, and nothing that does not.
 *
 * Exists because `generation` describes a NOTIFICATION and this describes the
 * SOURCE. See avk_blur_cache.source_hash for what went wrong with only the
 * former. Never zero for a non-empty prefix, so "not computed" and "computed
 * and empty" cannot be confused.
 */
uint64_t avk_blur_cache_source_hash(const struct avk_cmd *cmds, size_t len);

/* Add one to the counter for `r` and to the invalidation total. OK counts
 * nothing -- it is not an invalidation. */
void avk_blur_cache_count_reason(struct avk_blur_cache *cache,
	enum avk_blur_cache_reason r);

/*
 * The image the next rebuild must render into: the single image for this kind,
 * created or reshaped as needed, with a wrongly-shaped predecessor handed to
 * the retire queue at its OWN last use. NULL on allocation failure, in which
 * case the cache is left exactly as it was and the caller must take the live
 * path.
 */
struct avk_image *avk_blur_cache_target(struct avk_blur_cache *cache,
	enum avk_blur_cache_kind kind, struct avk_device *dev,
	struct avk_retire_queue *retire, VkFormat format, uint32_t width,
	uint32_t height);

/*
 * WHAT THIS CACHE ACTUALLY COSTS, in three separable numbers.
 *
 * They are separate because they answer different questions and only one of
 * them is arithmetic: texel_bytes is extent x format and can be checked by
 * hand; req_bytes is what VkMemoryRequirements asked for, which includes the
 * driver's tiling, alignment and any compression metadata; images says how many
 * of them exist at all. A single figure conflating these is how a byte total
 * came to disagree with its own extent for a whole milestone.
 */
struct avk_blur_cache_inventory {
	uint32_t images;
	uint64_t texel_bytes;
	uint64_t req_bytes;
};

/* Accumulates INTO `out` -- so it can be summed over outputs without a caller
 * having to add three fields itself. Zero it first. */
void avk_blur_cache_inventory(const struct avk_blur_cache *cache,
	struct avk_blur_cache_inventory *out);

/*
 * Release every cached image. With `retire` non-NULL each is destroyed once the
 * GPU passes its own last use; with it NULL they are destroyed immediately, which
 * is only correct from teardown after the device is idle.
 */
void avk_blur_cache_finish(struct avk_blur_cache *cache,
	struct avk_device *dev, struct avk_retire_queue *retire);

#endif /* AVK_BLUR_CACHE_H */
