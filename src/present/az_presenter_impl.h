#ifndef AZ_PRESENTER_IMPL_H
#define AZ_PRESENTER_IMPL_H

/*
 * M6A/ADR-601..605. The per-output presenter -- behaviour.
 *
 * Types live in az_presenter.h, which Monitor needs before it can embed the
 * struct by value; everything here needs Monitor and so comes after it.
 *
 * The presenter observes presentation, states a prediction, and records how
 * wrong that prediction was. It does not schedule, render or decide. The error
 * series is a first-class output rather than a diagnostic: a predictor whose
 * error nobody measures is a guess with better manners.
 */

#include "az_presenter.h"

/* refresh is in mHz, so the period in ns is 1e12/refresh. Spelled once,
 * here, because getting it wrong is not a compile error -- an earlier 1e15 in
 * the instrumentation folded a 468ms idle gap into a 60Hz output's period and
 * only a visibly absurd number gave it away. */
static uint64_t az_presenter_period_from_mode(const struct wlr_output *o) {
	if (o == NULL || o->refresh <= 0) {
		return 0;
	}
	return (uint64_t)(1.0e12 / (double)o->refresh);
}

static void az_presenter_reset(Monitor *m, enum az_present_reset_reason why) {
	if (m == NULL) {
		return;
	}
	struct az_presenter *p = &m->presenter;

	/* Carried across, because they describe the RUN and not the epoch: an
	 * operator asking "how often does this output reset its timing" must not
	 * have the answer erased by a reset. */
	uint32_t epoch = p->epoch + 1;
	uint64_t carried[AZ_PRESENT_RESET_COUNT];
	memcpy(carried, p->resets, sizeof(carried));

	/*
	 * TOTAL REASSIGNMENT, not field-wise invalidation. "A stale field survived
	 * the reset" is then not an expressible state -- and this file's own
	 * history shows what one surviving field does to a controller: render-late
	 * wedged at frac 0.040 for 11900 consecutive samples, no log, no recovery
	 * short of a restart.
	 */
	*p = (struct az_presenter){0};
	p->epoch = epoch;
	memcpy(p->resets, carried, sizeof(carried));
	if (why < AZ_PRESENT_RESET_COUNT) {
		p->resets[why]++;
	}

	p->sync = AZ_PRESENT_UNSYNCED;
	p->clock = AZ_PRESENT_CLOCK_UNKNOWN;
	p->nominal_period_ns = az_presenter_period_from_mode(m->wlr_output);
	/* Decided ONCE per epoch, from the state just committed. An adaptive-sync
	 * toggle arrives as a commit and a commit is itself a reset trigger, so
	 * the regime cannot go stale within an epoch. */
	p->regime = (m->wlr_output != NULL
			&& m->wlr_output->adaptive_sync_status
				== WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED)
		? AZ_PRESENT_VRR : AZ_PRESENT_FIXED;
	p->reset_commit_seq =
		m->wlr_output != NULL ? m->wlr_output->commit_seq : 0;
	/*
	 * ── t_pipe, CHOSEN BY SWEEP RATHER THAN BY DERIVATION ─────────────────
	 *
	 * Measured on DP-1, 25s per cell, error in us (abs is the honest column --
	 * a mean can cancel a predictor that is early half the time):
	 *
	 *     t_pipe      idle mean / abs      continuous mean / abs
	 *          0        5745 / 5745                 31 / 31
	 *       3000        1612 / 2509                 19 / 25
	 *       6000        1763 / 3812                 14 / 17
	 *       9050        -910 / 3939              -2025 / 2072
	 *
	 * 9050 is the idle arm-to-photons mean and is exactly the wrong answer:
	 * under load the frame event fires close enough to the previous present
	 * that `now + t_pipe` overtakes `last_present + P_min`, the max() picks the
	 * term that does not apply, and the loaded case -- the only one animation
	 * lives in -- degrades by two orders of magnitude.
	 *
	 * 3000 costs nothing under load and takes the idle bias from 5.7ms to
	 * 1.6ms. It does not narrow the idle SPREAD, and no constant can: an idle
	 * VRR panel's readiness is genuinely variable (commit-to-photons p10 2400us
	 * against p95 12900us). Making this adaptive to chase that spread is
	 * explicitly out of scope for M6A -- the error series is the evidence a
	 * later milestone would have to argue from.
	 *
	 * Zero on FIXED outputs: there is a lattice to project onto there, and it
	 * predicts to single-digit microseconds without help.
	 */
	p->t_pipe_ns = p->regime == AZ_PRESENT_VRR ? 3000000ull : 0;

	wlr_log(WLR_DEBUG,
		"M6A presenter reset: %s epoch=%u reason=%s regime=%s period=%.3fms",
		m->wlr_output != NULL ? m->wlr_output->name : "?", p->epoch,
		az_present_reset_reason_name(why), az_present_regime_name(p->regime),
		(double)p->nominal_period_ns / 1.0e6);
}

static uint64_t az_presenter_period_ns(const Monitor *m) {
	if (m == NULL) {
		return 0;
	}
	const struct az_presenter *p = &m->presenter;
	/*
	 * Observed once there is enough of it to be a measurement rather than a
	 * sample; nominal otherwise. Both are kept because disagreeing is
	 * informative -- HDMI-A-1's observed period matched the hardware's own
	 * figure to 18ns, and that is what makes the same derivation trustworthy
	 * on DP-1, where the hardware reports a constant nominal regardless of the
	 * actual stretched interval.
	 */
	if (p->obs_seq_sum >= 8 && p->obs_when_sum_ns > 0) {
		return p->obs_when_sum_ns / p->obs_seq_sum;
	}
	return p->nominal_period_ns;
}

static uint64_t az_presenter_arm(Monitor *m, uint64_t now_ns) {
	if (m == NULL) {
		return now_ns;
	}
	struct az_presenter *p = &m->presenter;
	uint64_t period = az_presenter_period_ns(m);
	uint64_t target;

	if (period == 0) {
		/* No mode to reason from. Say `now` and mean it, rather than inventing
		 * a lattice out of a zero. */
		target = now_ns;
	} else if (p->sync != AZ_PRESENT_SYNCED
			|| p->clock != AZ_PRESENT_CLOCK_MONOTONIC) {
		/*
		 * No usable phase: nothing has presented this epoch, or the backend's
		 * stamp is in a clock we cannot subtract from ours. Coarse and honest,
		 * one period out -- error bounded by about a period, still strictly
		 * better than sampling at `now`, and it never writes phase state.
		 */
		target = now_ns + period;
	} else if (p->regime == AZ_PRESENT_VRR) {
		/*
		 * Under adaptive sync there is no lattice: presentation follows the
		 * commit, floored by the panel's max-refresh period.
		 *
		 * Measured behaviour is that this output converges to exactly that
		 * floor under sustained load -- 6944.413us against a 6944.492us mode,
		 * 2870 consecutive frames at cadence 1x, not one skipped vblank. So
		 * during animation, the only time prediction matters, this reduces to
		 * the lattice below with n == 1. The stretching happens when nothing
		 * is moving.
		 *
		 * t_pipe_ns stays zero until measurement seeds it; while it is zero
		 * the floor alone decides, which is the right answer under load and a
		 * documented under-estimate at idle. It is deliberately NOT seeded
		 * from the mean latency: the mean is dominated by waiting for the
		 * display to be ready, and that wait is already this floor.
		 */
		uint64_t floor_ns = p->last_present_ns + p->nominal_period_ns;
		uint64_t pipe_ns = now_ns + p->t_pipe_ns;
		target = pipe_ns > floor_ns ? pipe_ns : floor_ns;
	} else {
		/*
		 * FIXED: the next lattice point strictly after `now`. Re-anchored on
		 * every accepted present, so nominal-vs-true drift cannot accumulate
		 * beyond the projection window -- which is why the nominal period
		 * suffices here and the phase-accumulation worry does not apply.
		 */
		uint64_t since = now_ns > p->last_present_ns
			? now_ns - p->last_present_ns : 0;
		uint64_t n = since / period + 1;
		target = p->last_present_ns + n * period;
	}

	p->armed_target_ns = target;
	p->armed_at_ns = now_ns;
	return target;
}

static void az_presenter_committed(Monitor *m, uint32_t commit_seq,
		uint64_t commit_ret_ns) {
	if (m == NULL) {
		return;
	}
	struct az_presenter *p = &m->presenter;
	if (p->armed_target_ns == 0) {
		/* A commit with no armed frame is not this milestone's frame -- a
		 * modeset, a cursor-only update. It has no target and must not consume
		 * an in-flight slot. */
		return;
	}
	/* Oldest slot wins, so a slot leaked by a commit that never presented
	 * cannot wedge the ring. */
	struct az_present_inflight *slot = &p->inflight[0];
	for (int i = 1; i < AZ_PRESENT_INFLIGHT; i++) {
		if (!p->inflight[i].used
				|| p->inflight[i].commit_ret_ns < slot->commit_ret_ns) {
			slot = &p->inflight[i];
		}
	}
	*slot = (struct az_present_inflight){
		.commit_seq = commit_seq,
		.target_ns = p->armed_target_ns,
		.arm_ns = p->armed_at_ns,
		.commit_ret_ns = commit_ret_ns,
		.used = true,
	};
	p->armed_target_ns = 0;
}

static void az_presenter_present(Monitor *m,
		const struct wlr_output_event_present *ev, uint64_t now_ns) {
	if (m == NULL || ev == NULL) {
		return;
	}
	struct az_presenter *p = &m->presenter;

	/*
	 * GATE 1: a flip from BEFORE this epoch. Closed by construction rather
	 * than by reasoning about listener ordering -- the commit sequence is
	 * monotonic and the reset recorded where the epoch began.
	 */
	if (ev->commit_seq < p->reset_commit_seq) {
		p->presents_discarded_epoch++;
		return;
	}
	/* GATE 2: a dropped update still fires this signal, and must never anchor
	 * phase -- folding one in invents a refresh that did not happen. */
	if (!ev->presented) {
		return;
	}
	if (!ev->when.tv_sec && !ev->when.tv_nsec) {
		return;
	}
	uint64_t when_ns =
		(uint64_t)ev->when.tv_sec * 1000000000ull + (uint64_t)ev->when.tv_nsec;

	/*
	 * GATE 3: which clock is that in? Established on the first presented frame
	 * of the epoch, never assumed. Both real outputs here report
	 * CLOCK_MONOTONIC -- proven, nine orders of magnitude clear of realtime --
	 * but a backend reporting otherwise must degrade to the coarse predictor
	 * rather than subtract two unrelated clocks and be wrong by an uptime.
	 */
	if (p->clock == AZ_PRESENT_CLOCK_UNKNOWN) {
		int64_t d = (int64_t)when_ns - (int64_t)now_ns;
		if (d < 0) {
			d = -d;
		}
		p->clock = d < 1000000000ll ? AZ_PRESENT_CLOCK_MONOTONIC
		                            : AZ_PRESENT_CLOCK_FOREIGN;
		if (p->clock == AZ_PRESENT_CLOCK_FOREIGN) {
			wlr_log(WLR_ERROR,
				"M6A: %s presentation stamp is NOT in our clock (%.3fms "
				"away); phase prediction disabled for this epoch",
				m->wlr_output->name, (double)d / 1.0e6);
		}
	}

	/* The observed period, as sum(dwhen)/sum(dseq): an average of ratios is
	 * not the period whenever the deltas differ. */
	if (p->sync == AZ_PRESENT_SYNCED && ev->seq > p->last_seq
			&& when_ns > p->last_present_ns) {
		uint64_t dw = when_ns - p->last_present_ns;
		uint64_t ds = ev->seq - p->last_seq;
		uint64_t per = dw / ds;
		uint64_t nom = p->nominal_period_ns;
		if (nom == 0 || (per > nom / 2 && per < nom * 2)) {
			p->obs_when_sum_ns += dw;
			p->obs_seq_sum += ds;
		}
	}

	/*
	 * THE ERROR SERIES, matched strictly on commit_seq. A latency attributed
	 * to whatever happened to be in a slot looks plausible and is fiction.
	 */
	for (int i = 0; i < AZ_PRESENT_INFLIGHT; i++) {
		struct az_present_inflight *s = &p->inflight[i];
		if (!s->used || s->commit_seq != ev->commit_seq) {
			continue;
		}
		if (p->clock == AZ_PRESENT_CLOCK_MONOTONIC && s->target_ns) {
			int64_t err = (int64_t)when_ns - (int64_t)s->target_ns;
			if (p->err_count == 0 || err < p->err_min_ns) {
				p->err_min_ns = err;
			}
			if (p->err_count == 0 || err > p->err_max_ns) {
				p->err_max_ns = err;
			}
			p->err_count++;
			p->err_sum_ns += err;
			p->err_abs_sum_ns += (uint64_t)(err < 0 ? -err : err);
		}
		s->used = false;
		break;
	}

	p->last_present_ns = when_ns;
	p->last_seq = ev->seq;
	p->sync = AZ_PRESENT_SYNCED;
	p->presents_accepted++;
}

/*
 * THE SAMPLE INSTANT for this pass -- the single answer every animated object
 * must use instead of each reading its own clock (audit G3).
 *
 * Zero when nothing is armed, and the caller must treat that as "not in a
 * frame". Substituting `now` here would silently reintroduce the very bug this
 * milestone removes, in the one place nobody would think to look.
 */
static uint64_t az_presenter_sample_ns(const Monitor *m) {
	return m != NULL ? m->presenter.armed_target_ns : 0;
}

#endif /* AZ_PRESENTER_IMPL_H */
