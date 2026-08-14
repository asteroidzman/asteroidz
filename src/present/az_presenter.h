#ifndef AZ_PRESENTER_H
#define AZ_PRESENTER_H

/*
 * ── WHAT TIME IS IT ON THIS OUTPUT ────────────────────────────────────────
 *
 * M6A/ADR-602. One owner for a question nothing owned: when will the frame
 * being built right now actually turn into light on THIS display?
 *
 * Before this, timing state was scattered -- a trace-only last-present stamp,
 * the render-late controller's own history, five independent clock reads
 * inside animation code, and the nominal refresh re-derived ad hoc wherever it
 * was wanted. Nothing held the answer, so nothing could be wrong about it in a
 * way a test could catch.
 *
 * THE PRESENTER DOES NOT RENDER, SCHEDULE OR DECIDE ANYTHING. It observes
 * presentation, states a prediction, and records how wrong that prediction
 * turned out to be. The error is a first-class output, not a diagnostic: a
 * predictor whose error is not measured is a guess with better manners.
 *
 * OWNERSHIP. Embedded by value in Monitor, never pointed to -- a pointer
 * member invites exactly the stale-across-hotplug class this milestone exists
 * to make impossible. Mutated by the four az_presenter_* entry points below
 * and by nothing else; every other reader, including all of AVK, the animation
 * code and the render-late controller, is read-only.
 *
 * WHY NOT INSIDE AVK. The milestone's phrasing is "AVK should own the
 * relationship", and the relationship spans compositor and GPU. Frame events,
 * KMS commits and output lifecycle are main-loop compositor facts; what AVK
 * uniquely owns is GPU execution timing, and it contributes exactly that.
 * Putting the presenter in the renderer would also leave the scenefx-fallback
 * outputs with no timing owner at all.
 */

#include <stdbool.h>
#include <stdint.h>

struct wlr_output_event_present;

/*
 * TWO REGIMES, DIFFERENT IN KIND -- not one model with a parameter.
 *
 * On a fixed-refresh output presentation lands on a vblank LATTICE and the
 * compositor predicts which slot it will hit; its influence is limited to
 * making a deadline or missing it.
 *
 * Under adaptive sync there is no lattice. Presentation largely FOLLOWS the
 * commit, bounded below by the panel's max-refresh period, so the compositor
 * substantially chooses the presentation time by choosing when to commit --
 * and what it must predict is its own commit-to-photons latency.
 *
 * This is not a hypothetical. DP-1 on this desk runs with adaptive sync on,
 * and its vblank interval measured 11916.8us idle against 7475.4us under
 * continuous damage, with cadence overwhelmingly 1x: genuinely stretched
 * vblanks, not skipped ones. A lattice model applied there is systematically
 * early by most of a frame.
 */
enum az_present_regime {
	AZ_PRESENT_FIXED = 0,
	AZ_PRESENT_VRR,
};

/* Phase is knowable only after a present has been accepted this epoch. Before
 * that the predictor must not pretend to know where the vblank lattice sits;
 * it falls back to a coarse honest answer instead (ADR-605). */
enum az_present_sync {
	AZ_PRESENT_UNSYNCED = 0,
	AZ_PRESENT_SYNCED,
};

/*
 * WHICH CLOCK THE BACKEND'S PRESENTATION STAMP IS IN -- established by
 * measurement on the first presented frame of an epoch, never assumed.
 *
 * Both real outputs on this machine report CLOCK_MONOTONIC, proven by
 * recording the stamp's distance to both clocks: -56.089ms from monotonic
 * against -1.786e15ms from realtime. That is nine orders of magnitude and not
 * a close call. The gate stays anyway: a backend that reports otherwise must
 * degrade to the coarse predictor rather than subtract two clocks and produce
 * a number wrong by the machine's uptime.
 */
enum az_present_clock {
	AZ_PRESENT_CLOCK_UNKNOWN = 0,
	AZ_PRESENT_CLOCK_MONOTONIC,
	AZ_PRESENT_CLOCK_FOREIGN,
};

/* Why an epoch ended. Logged and counted, so "the timing state reset" is a
 * readable event rather than an inference from a discontinuity. */
enum az_present_reset_reason {
	AZ_PRESENT_RESET_CREATE = 0,
	AZ_PRESENT_RESET_MODE,
	AZ_PRESENT_RESET_ENABLE,
	AZ_PRESENT_RESET_SCALE_TRANSFORM,
	AZ_PRESENT_RESET_ADAPTIVE_SYNC,
	AZ_PRESENT_RESET_REQUEST_STATE,
	AZ_PRESENT_RESET_SESSION,
	AZ_PRESENT_RESET_DPMS,
	AZ_PRESENT_RESET_COUNT,
};

#define AZ_PRESENT_INFLIGHT 4

struct az_present_inflight {
	uint32_t commit_seq;
	uint64_t target_ns;      /* what was predicted when this frame was armed */
	uint64_t arm_ns;
	uint64_t commit_ret_ns;
	bool used;
};

struct az_presenter {
	uint32_t epoch;
	enum az_present_regime regime;
	enum az_present_sync sync;
	enum az_present_clock clock;

	/* From the committed mode. Under VRR this is the MINIMUM period -- the
	 * max-refresh floor -- and emphatically not "the interval". */
	uint64_t nominal_period_ns;
	/*
	 * VRR only: arm-to-photons with the display READY, seeded from measurement
	 * M-8. Deliberately a constant within the milestone -- it never
	 * self-adjusts, because an adaptive estimator whose error nobody has
	 * measured yet is how a timing bug becomes untraceable.
	 *
	 * It must NOT be seeded from the mean latency. The mean is dominated by
	 * waiting for the display to become ready, and that wait is already
	 * carried by the `last_present + P_min` floor in the predictor; counting
	 * it twice puts every target a frame late.
	 */
	uint64_t t_pipe_ns;

	uint64_t last_present_ns;  /* meaningful only when SYNCED */
	uint32_t last_seq;         /* epoch-local; deltas only, so wrap is a
	                            * non-event */
	uint32_t reset_commit_seq; /* discards in-flight presents from the epoch
	                            * before this one, by construction */

	uint64_t armed_target_ns;  /* 0 == nothing armed */
	uint64_t armed_at_ns;
	/*
	 * The last target this output aimed at, kept for OBSERVATION only.
	 *
	 * armed_target_ns is cleared when the frame commits -- 0 genuinely means
	 * "nothing armed", and accessors depend on that. But it also makes the
	 * target invisible to anything asking between frames, which is every
	 * external observer. This copy is never cleared and is never read by the
	 * predictor.
	 */
	uint64_t last_target_ns;

	struct az_present_inflight inflight[AZ_PRESENT_INFLIGHT];

	/* Observed period, accumulated as sum(dwhen)/sum(dseq) rather than as an
	 * average of ratios: the two differ whenever the deltas differ, and only
	 * the former is the period. */
	uint64_t obs_when_sum_ns;
	uint64_t obs_seq_sum;

	/*
	 * THE ERROR SERIES -- the reason any of this is trustworthy.
	 *
	 * Signed, in nanoseconds: actual - predicted. Positive means the frame
	 * lit up later than predicted. Kept as sum/count/min/max plus a signed
	 * absolute sum, so a bias and a spread can be told apart; a predictor
	 * that is early half the time and late half the time has a mean near zero
	 * and is not therefore good.
	 */
	uint64_t err_count;
	int64_t err_sum_ns;
	int64_t err_min_ns, err_max_ns;
	uint64_t err_abs_sum_ns;

	uint64_t presents_accepted, presents_discarded_epoch;
	uint64_t resets[AZ_PRESENT_RESET_COUNT];
};

/*
 * TYPES ONLY. Monitor embeds this by value, so this header has to be included
 * before Monitor is defined -- which means nothing here may mention Monitor.
 * The four mutators and the accessors live in az_presenter_impl.h, included
 * after it, the same split the rest of this compositor uses (see the note
 * above `struct az_avk_output` in asteroidz.c).
 */

static inline const char *az_present_regime_name(enum az_present_regime r) {
	return r == AZ_PRESENT_VRR ? "vrr" : "fixed";
}

static inline const char *az_present_reset_reason_name(
		enum az_present_reset_reason r) {
	switch (r) {
	case AZ_PRESENT_RESET_CREATE:           return "create";
	case AZ_PRESENT_RESET_MODE:             return "mode";
	case AZ_PRESENT_RESET_ENABLE:           return "enable";
	case AZ_PRESENT_RESET_SCALE_TRANSFORM:  return "scale/transform";
	case AZ_PRESENT_RESET_ADAPTIVE_SYNC:    return "adaptive-sync";
	case AZ_PRESENT_RESET_REQUEST_STATE:    return "request-state";
	case AZ_PRESENT_RESET_SESSION:          return "session";
	case AZ_PRESENT_RESET_DPMS:             return "dpms";
	case AZ_PRESENT_RESET_COUNT:            break;
	}
	return "?";
}

#endif /* AZ_PRESENTER_H */
