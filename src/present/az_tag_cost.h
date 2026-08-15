/*
 * ── WHAT A TAG SWITCH ACTUALLY COSTS ──────────────────────────────────────
 *
 * P4, first half. Before anything is made cheaper, the cost has to be a number
 * -- per transition, not averaged over a session, because a tag slide is a
 * burst and an average over idle frames hides it completely.
 *
 * A TRANSITION is the interval during which at least one client is running a
 * TAG animation. It opens on the first frame that sees one and closes on the
 * first frame that sees none, and everything below is accumulated between
 * those two instants:
 *
 *   frames          committed frames, per output
 *   blur prefix px  the prefix area the blur chain PRICED. Not work done --
 *                   see the field comment; this was mistaken for work once and
 *                   the mistake is what P4b turned on
 *   blur rebuilds   the background blur actually re-rendered. THIS is the work
 *   blur hits       the times it was served from the M4I cache instead
 *   damage px       what the frame committed
 *   frame ms        the per-frame render cost, kept as a SAMPLE ARRAY so p50
 *                   and p95 are real order statistics
 *
 * WHY p95 AND NOT A MAX. One pathological frame is a different fact from a
 * slide that is uniformly slow, and the maximum cannot tell them apart -- the
 * pacing work made exactly that mistake with a decaying-max estimator that
 * threw the distribution away. p50 says what the slide normally costs; p95
 * says what its bad frames cost; the gap between them says which of the two
 * problems you have.
 *
 * SAMPLES ARE BOUNDED AND THE BOUND IS HONEST. A transition is a few hundred
 * milliseconds, so AZ_TAG_COST_SAMPLES is far more than one can produce; if it
 * ever does overflow, the count keeps rising while the samples stop, and
 * `truncated` says so rather than quietly reporting percentiles of a prefix.
 */

#define AZ_TAG_COST_SAMPLES 512

struct az_tag_cost {
	bool active;
	uint64_t started_ns;
	uint64_t ended_ns;

	uint64_t frames;
	uint64_t committed;
	/*
	 * WHAT IT WOULD HAVE COST versus WHAT IT COST.
	 *
	 * blur_prefix_px is the prefix area the blur chain PRICED -- accumulated
	 * for every slot whether or not its chain ran, because a skipped blur's
	 * saving is only meaningful against what it would otherwise have cost. It
	 * is a price list.
	 *
	 * blur_rebuilds is the invoice: how many times the background blur was
	 * genuinely re-rendered during the transition. The two are wildly
	 * different on this compositor, and reading the first as the second is the
	 * mistake this field pair exists to make impossible -- see status.md.
	 */
	uint64_t blur_prefix_px;
	uint64_t blur_rebuilds;
	uint64_t blur_hits;
	uint64_t damage_px;

	uint32_t nsamples;
	bool truncated;
	double sample_ms[AZ_TAG_COST_SAMPLES];

	/* The completed transition, kept for the stats IPC to read after the
	 * slide is over -- which is when anyone asks. */
	struct {
		bool valid;
		uint64_t duration_ns;
		uint64_t frames, committed, blur_prefix_px, blur_rebuilds, blur_hits;
		uint64_t damage_px;
		double p50_ms, p95_ms;
		uint32_t nsamples;
		bool truncated;
	} last;

	/* Monotonic across the session, so a fixture can difference them. */
	uint64_t transitions;
};

static struct az_tag_cost az_tag_cost;

static int az_tag_cost_cmp(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

/* An order statistic, by the nearest-rank method: the smallest sample at or
 * above the requested fraction. No interpolation, because a frame time that
 * was never measured is not a frame time. */
static double az_tag_cost_pct(double *sorted, uint32_t n, double frac) {
	if (n == 0) {
		return 0.0;
	}
	uint32_t rank = (uint32_t)ceil(frac * (double)n);
	if (rank == 0) {
		rank = 1;
	}
	if (rank > n) {
		rank = n;
	}
	return sorted[rank - 1];
}

static void az_tag_cost_close(uint64_t now_ns) {
	struct az_tag_cost *t = &az_tag_cost;
	if (!t->active) {
		return;
	}
	t->active = false;
	t->ended_ns = now_ns;

	double sorted[AZ_TAG_COST_SAMPLES];
	uint32_t n = t->nsamples;
	if (n > AZ_TAG_COST_SAMPLES) {
		n = AZ_TAG_COST_SAMPLES;
	}
	memcpy(sorted, t->sample_ms, (size_t)n * sizeof(*sorted));
	qsort(sorted, n, sizeof(*sorted), az_tag_cost_cmp);

	t->last.valid = true;
	t->last.duration_ns = t->ended_ns - t->started_ns;
	t->last.frames = t->frames;
	t->last.committed = t->committed;
	t->last.blur_prefix_px = t->blur_prefix_px;
	t->last.blur_rebuilds = t->blur_rebuilds;
	t->last.blur_hits = t->blur_hits;
	t->last.damage_px = t->damage_px;
	t->last.p50_ms = az_tag_cost_pct(sorted, n, 0.50);
	t->last.p95_ms = az_tag_cost_pct(sorted, n, 0.95);
	t->last.nsamples = n;
	t->last.truncated = t->truncated;
	t->transitions++;

	AZ_PACE("tag cost dur_ms=%.3f frames=%llu committed=%llu "
		"blur_prefix_px=%llu blur_rebuilds=%llu blur_hits=%llu "
		"damage_px=%llu p50_ms=%.3f p95_ms=%.3f "
		"samples=%u truncated=%d n=%llu",
		(double)t->last.duration_ns / 1.0e6,
		(unsigned long long)t->last.frames,
		(unsigned long long)t->last.committed,
		(unsigned long long)t->last.blur_prefix_px,
		(unsigned long long)t->last.blur_rebuilds,
		(unsigned long long)t->last.blur_hits,
		(unsigned long long)t->last.damage_px,
		t->last.p50_ms, t->last.p95_ms, t->last.nsamples,
		t->last.truncated ? 1 : 0,
		(unsigned long long)t->transitions);
}

/*
 * One output's frame, folded in. `in_tag` is whether this frame saw a client
 * running a TAG animation -- the compositor's own answer, not a guess from the
 * geometry.
 */
static void az_tag_cost_frame(bool in_tag, uint64_t now_ns, double dur_ms,
		bool committed, uint64_t damage_px, uint64_t blur_prefix_delta,
		uint64_t blur_rebuild_delta, uint64_t blur_hit_delta) {
	struct az_tag_cost *t = &az_tag_cost;

	if (!in_tag) {
		az_tag_cost_close(now_ns);
		return;
	}
	if (!t->active) {
		t->active = true;
		t->started_ns = now_ns;
		t->frames = t->committed = 0;
		t->blur_prefix_px = t->blur_rebuilds = t->blur_hits = 0;
		t->damage_px = 0;
		t->nsamples = 0;
		t->truncated = false;
	}
	t->frames++;
	if (committed) {
		t->committed++;
	}
	t->damage_px += damage_px;
	t->blur_prefix_px += blur_prefix_delta;
	t->blur_rebuilds += blur_rebuild_delta;
	t->blur_hits += blur_hit_delta;
	if (t->nsamples < AZ_TAG_COST_SAMPLES) {
		t->sample_ms[t->nsamples++] = dur_ms;
	} else {
		t->truncated = true;
	}
}
