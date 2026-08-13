/*
 * avk_stat.h -- a fixed-bucket histogram, for distributions of GPU durations.
 *
 * WHY A DISTRIBUTION AND NOT A MEAN. A mean is the wrong statistic for a
 * deadline. A frame budget is missed or met per frame, so what a reader needs
 * is p95 and p99 against the refresh interval, and those need the shape.
 *
 * FIXED BUCKETS rather than a reservoir sample: no allocation, no per-frame
 * sort, bounded memory, and a percentile that is exact to within one bucket
 * rather than statistically approximate. The bucket width is stated with the
 * number so that "exact to within one bucket" is checkable rather than a
 * claim.
 *
 * WHY IT IS HERE AND NOT IN az_avk.h. The compositor has its own copy of this
 * idea for the quantities IT measures -- CPU frame time, damage ratio -- and
 * that one may not be included from inside src/render/vulkan: AVK does not
 * include compositor headers, which is what tests/check-vulkan-isolation.py
 * enforces. The duplication is 30 lines of arithmetic and buys the isolation
 * rule; sharing it would mean either breaking the rule or moving the
 * compositor's stats into the renderer, and neither is worth it.
 */
#ifndef AVK_STAT_H
#define AVK_STAT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 20 us per bucket over 2048 buckets: 40.96 ms.
 *
 * IT WAS 512 BUCKETS -- 10.24 ms -- AND THAT WAS NOT ENOUGH. An eight-blur
 * scene put every sample in the top bucket, so p50 and p95 came back as
 * 10 240 us: not measurements but the ceiling, reported with the same
 * confidence as a real number. A censored percentile that looks like a
 * percentile is worse than no percentile.
 *
 * The resolution is unchanged at 20 us, deliberately: the low end is where
 * graph_build (3-16 us) and record (50-200 us) live, and widening the bucket
 * to buy ceiling would have thrown those away. 2048 buckets is 16 KiB per
 * histogram, which is nothing beside a blur transient.
 *
 * 40.96 ms is ~6 frames at 144 Hz and ~2.5 at 60 Hz. A frame that overflows it
 * is a frame that has already failed at something other than blur cost -- so
 * the answer is not a bigger ceiling but the overflow counter below, which
 * makes saturation VISIBLE instead of silent.
 */
#define AVK_HIST_BUCKETS 2048
#define AVK_HIST_SCALE_NS 20000ULL

struct avk_hist {
	uint64_t count;
	uint64_t total;
	uint64_t max;
	/*
	 * Samples that fell OFF each end, counted rather than clamped in silence.
	 *
	 * `overflow` is the one that matters: any percentile computed from a
	 * histogram with overflow > 0 is a lower bound, and a qualification
	 * measurement that reports one is reporting a floor. Callers print
	 * CENSORED instead of a number.
	 *
	 * `underflow` is not an error -- it is the count of samples shorter than
	 * one bucket, which the histogram cannot resolve. It says "the resolution
	 * is too coarse for this quantity", which is worth knowing about
	 * graph_build at 3 us against a 20 us bucket.
	 */
	uint64_t overflow;
	uint64_t underflow;
	/* Anything past the last bucket lands in it, so the top bucket means "at
	 * least this" and a percentile that falls there is reported as saturated
	 * rather than as a number the data cannot support. */
	uint64_t bucket[AVK_HIST_BUCKETS];
};

static inline void avk_hist_add(struct avk_hist *h, uint64_t ns)
{
	h->count++;
	h->total += ns;
	if (ns > h->max) {
		h->max = ns;
	}
	uint64_t idx = ns / AVK_HIST_SCALE_NS;
	if (idx == 0) {
		h->underflow++;
	}
	if (idx >= AVK_HIST_BUCKETS) {
		idx = AVK_HIST_BUCKETS - 1;
		h->overflow++;
	}
	h->bucket[idx]++;
}

/*
 * The value at `pct` (0..100), in nanoseconds: the UPPER EDGE of the bucket the
 * percentile falls in. That is an over-estimate by at most one bucket width,
 * which is stated rather than hidden -- a percentile quoted to three decimals
 * from bucketed data would be a false precision.
 */
static inline uint64_t avk_hist_pct(const struct avk_hist *h, double pct)
{
	if (h->count == 0) {
		return 0;
	}
	uint64_t want = (uint64_t)((double)h->count * pct / 100.0);
	if (want == 0) {
		want = 1;
	}
	uint64_t seen = 0;
	for (uint32_t i = 0; i < AVK_HIST_BUCKETS; i++) {
		seen += h->bucket[i];
		if (seen >= want) {
			return (uint64_t)(i + 1) * AVK_HIST_SCALE_NS;
		}
	}
	return (uint64_t)AVK_HIST_BUCKETS * AVK_HIST_SCALE_NS;
}

/*
 * Whether any sample saturated the top bucket. A percentile off a saturated
 * histogram is a FLOOR, and the caller must say so rather than print it.
 */
static inline bool avk_hist_censored(const struct avk_hist *h)
{
	return h->overflow > 0;
}

static inline uint64_t avk_hist_mean(const struct avk_hist *h)
{
	return h->count > 0 ? h->total / h->count : 0;
}

#endif /* AVK_STAT_H */
