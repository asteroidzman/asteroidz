/*
 * ── AZ_PACE: THE TIMELINE INSTRUMENT ──────────────────────────────────────
 *
 * "Animations feel laggy" is not one claim, it is at least four: the motion
 * takes too long, it starts too late, it advances unevenly, or it advances
 * evenly but is not PRESENTED evenly. Each one has a different fix and a
 * different owner, and none of them can be told apart from a frame-rate
 * counter or from watching the screen.
 *
 * So this records the timeline itself, as raw events, and derives nothing --
 * every conclusion is drawn by whoever reads the trace, from events they can
 * argue with. Four event kinds, all on one monotonic clock:
 *
 *   anim start  a semantic change gave a client a new target
 *   anim tick   one interpolation step: the clock it read, the eased factor
 *               it derived, and the INTEGER geometry it actually stored
 *   present     the compositor was told a frame reached the screen
 *   render      one render_monitor pass: what it cost, and whether it
 *               committed anything at all
 *
 * The tick line carries stored geometry rather than the ideal real-valued
 * position on purpose: the gap between "the curve says 240.6" and "the scene
 * node says 240" is exactly the quantisation question, and a trace that
 * logged the ideal value could not answer it.
 *
 * OFF unless AZ_PACE=1 is in the environment, including the present listener,
 * which is not even wired otherwise. A trace this dense is a measurement
 * instrument, not a feature.
 */
#ifndef AZ_PACE_H
#define AZ_PACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/util/log.h>

static inline bool az_pace_on(void) {
	static int cached = -1;
	if (cached < 0) {
		const char *env = getenv("AZ_PACE");
		cached = (env != NULL && env[0] == '1' && env[1] == '\0');
	}
	return cached != 0;
}

/*
 * Which output's render pass is currently running. Animation ticks happen
 * inside render_monitor's client loop and the interpolator has no idea which
 * monitor drove it -- but "did the 60Hz panel tick this window as often as the
 * 144Hz one" is the whole mixed-refresh question, so the attribution has to
 * come from somewhere. It comes from here.
 */
static const char *az_pace_mon = "-";

static inline uint64_t az_pace_now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
 * Logged at ERROR so the trace survives the default log level. It is not an
 * error; nothing reaches this function unless the operator asked for it by
 * name. The alternative -- INFO -- would silently produce an empty trace on
 * any build or session that had raised the threshold, and an empty trace is
 * indistinguishable from a compositor that never animated.
 */
#define AZ_PACE(fmt, ...)                                                      \
	do {                                                                       \
		if (az_pace_on())                                                      \
			wlr_log(WLR_ERROR, "azpace " fmt, ##__VA_ARGS__);                  \
	} while (0)

/* Per-output present interval, from the backend's own presentation feedback.
 * The delta is against this output's previous present and nothing else --
 * mixing two outputs into one series is how a 60Hz panel gets blamed for a
 * 144Hz panel's misses. */
static inline void az_pace_present(const char *mon, uint64_t *last_ns,
		uint32_t seq, uint64_t when_ns) {
	if (!az_pace_on())
		return;
	int64_t delta = *last_ns ? (int64_t)(when_ns - *last_ns) : -1;
	*last_ns = when_ns;
	AZ_PACE("present mon=%s seq=%u t_ns=%llu delta_us=%lld", mon, seq,
		(unsigned long long)when_ns, (long long)(delta / 1000));
}

#endif
