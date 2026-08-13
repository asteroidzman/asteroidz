/*
 * The luminance-domain resolver. Contract C2, ADR-006/003/004.
 *
 * The resolver is one switch and three multiplies, which is exactly why it is
 * worth a test file: every one of those multiplies is a conversion between two
 * definitions of white, and getting one of them backwards produces a picture
 * that is plausible everywhere and correct nowhere -- HDR video at a fifth of
 * its intended brightness, or an SDR desktop that changes brightness when the
 * user adjusts a reference the SDR path is not supposed to touch.
 *
 * THE ASYMMETRY IS THE POINT. ADR-003's claim, in its unit form, is that a
 * sweep of `scene_reference_luminance`
 *
 *   - changes the PQ and scRGB scales, exactly as 1/ref, and
 *   - leaves the SDR scale untouched.
 *
 * If both moved, the anchor is applied on the wrong side of the split and HDR
 * media's absolute luminance would drift every time the user adjusts UI
 * brightness. That is test_reference_sweep(), and it is the falsifier the ADR
 * names.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "render/az_lum.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); } \
		else { failures++; printf("  FAIL " __VA_ARGS__); printf("\n"); } \
	} while (0)

#define NEAR(a, b, tol, ...) do { \
		double _d = fabs((double)(a) - (double)(b)); \
		checks++; \
		if (_d <= (tol)) { \
			printf("  ok   " __VA_ARGS__); \
			printf("  (%.6f, |d| = %.3g)\n", (double)(a), _d); \
		} else { \
			failures++; \
			printf("  FAIL " __VA_ARGS__); \
			printf("  got %.9g want %.9g, |d| = %.3g > %.3g\n", \
				(double)(a), (double)(b), _d, (double)(tol)); \
		} \
	} while (0)

static struct az_lum_source_desc untagged(void) {
	struct az_lum_source_desc s = {0};
	s.tagged = false;
	return s;
}

static struct az_lum_source_desc tagged(enum az_tf tf, enum az_primaries p,
		float max_cll) {
	struct az_lum_source_desc s = {
		.tagged = true, .tf = tf, .primaries = p, .max_cll = max_cll};
	return s;
}

/* ── the table ──────────────────────────────────────────────────────────── */

/*
 * Every (source, rules) combination the contract names, with the expected
 * `scale` written out as a NUMBER rather than as the formula the resolver
 * uses. A table that restates the implementation's expression passes for any
 * consistent pair of bugs; these were computed by hand from ADR-006 and are
 * the values a person can check against the ADR without reading the code.
 */
struct row {
	const char *what;
	struct az_lum_source_desc src;
	struct az_lum_rules rules;
	float ref;
	enum az_tf want_tf;
	enum az_primaries want_prim;
	double want_scale;
	double want_peak;
};

#define NO_RULES {0.0f, 0.0f}
#define WHITE2 {2.0f, 0.0f}
#define GAIN_HALF {0.0f, 0.5f}

int main(void) {
	printf("== C2 luminance domain ==\n");

	const struct row ROWS[] = {
		/* ADR-004: untagged is piecewise sRGB, BT.709, no luminance term. */
		{"untagged, no rules", {0}, NO_RULES, 203.0f,
			AZ_TF_SRGB, AZ_PRIM_BT709, 1.0, 0.0},
		{"untagged, sdr-white-scale 2.0", {0}, WHITE2, 203.0f,
			AZ_TF_SRGB, AZ_PRIM_BT709, 2.0, 0.0},
		/* an hdr-gain on an SDR source is not a brightness slider by the
		 * back door: it applies to HDR sources only. */
		{"untagged, hdr-gain 0.5 (must not apply)", {0}, GAIN_HALF, 203.0f,
			AZ_TF_SRGB, AZ_PRIM_BT709, 1.0, 0.0},

		{"sRGB-tagged, no rules", {true, AZ_TF_SRGB, AZ_PRIM_BT709, 0.0f},
			NO_RULES, 203.0f, AZ_TF_SRGB, AZ_PRIM_BT709, 1.0, 0.0},
		{"GAMMA22-tagged", {true, AZ_TF_GAMMA22, AZ_PRIM_BT709, 0.0f},
			NO_RULES, 203.0f, AZ_TF_GAMMA22, AZ_PRIM_BT709, 1.0, 0.0},
		{"BT1886-tagged, sdr-white-scale 2.0",
			{true, AZ_TF_BT1886, AZ_PRIM_BT709, 0.0f}, WHITE2, 203.0f,
			AZ_TF_BT1886, AZ_PRIM_BT709, 2.0, 0.0},

		/* PQ: 10000/203 = 49.26108. The contract's anchor. */
		{"PQ, no max_cll, no rules",
			{true, AZ_TF_PQ, AZ_PRIM_BT2020, 0.0f}, NO_RULES, 203.0f,
			AZ_TF_PQ, AZ_PRIM_BT2020, 49.2610837, 0.0},
		/* 1000 cd/m2 of declared content peak is 1000/203 = 4.926108 scene
		 * units -- the tone-map ceiling a source can ask for. */
		{"PQ, max_cll 1000", {true, AZ_TF_PQ, AZ_PRIM_BT2020, 1000.0f},
			NO_RULES, 203.0f, AZ_TF_PQ, AZ_PRIM_BT2020, 49.2610837,
			4.92610837},
		{"PQ, hdr-gain 0.5", {true, AZ_TF_PQ, AZ_PRIM_BT2020, 0.0f},
			GAIN_HALF, 203.0f, AZ_TF_PQ, AZ_PRIM_BT2020, 24.6305419, 0.0},
		/* an sdr-white-scale on a PQ source must NOT apply. */
		{"PQ, sdr-white-scale 2.0 (must not apply)",
			{true, AZ_TF_PQ, AZ_PRIM_BT2020, 0.0f}, WHITE2, 203.0f,
			AZ_TF_PQ, AZ_PRIM_BT2020, 49.2610837, 0.0},

		/* scRGB: 80/203 = 0.394089. */
		{"EXT_LINEAR, no rules",
			{true, AZ_TF_LINEAR_EXT, AZ_PRIM_BT709, 0.0f}, NO_RULES, 203.0f,
			AZ_TF_LINEAR_EXT, AZ_PRIM_BT709, 0.39408867, 0.0},
		{"EXT_LINEAR, hdr-gain 0.5",
			{true, AZ_TF_LINEAR_EXT, AZ_PRIM_BT709, 0.0f}, GAIN_HALF, 203.0f,
			AZ_TF_LINEAR_EXT, AZ_PRIM_BT709, 0.19704434, 0.0},

		/* ref = 0 means "unset" and must resolve to the BT.2408 default, not
		 * to a division by zero. */
		{"PQ, reference unset -> 203", {true, AZ_TF_PQ, AZ_PRIM_BT2020, 0.0f},
			NO_RULES, 0.0f, AZ_TF_PQ, AZ_PRIM_BT2020, 49.2610837, 0.0},
		/* and a user reference of 80 makes scRGB's scale exactly 1. */
		{"EXT_LINEAR at ref 80 -> scale 1",
			{true, AZ_TF_LINEAR_EXT, AZ_PRIM_BT709, 0.0f}, NO_RULES, 80.0f,
			AZ_TF_LINEAR_EXT, AZ_PRIM_BT709, 1.0, 0.0},
	};

	printf("the table: source x rules -> domain\n");
	for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
		const struct row *r = &ROWS[i];
		struct az_lum_domain d = az_lum_resolve(&r->src, &r->rules, r->ref);
		CHECK(d.tf == r->want_tf, "%s: tf", r->what);
		CHECK(d.primaries == r->want_prim, "%s: primaries", r->what);
		NEAR(d.scale, r->want_scale, 1e-4, "%s: scale", r->what);
		NEAR(d.content_peak, r->want_peak, 1e-4, "%s: content_peak", r->what);
	}

	/* ── ADR-003's falsifier, in unit form ──────────────────────────────── */
	printf("reference sweep: PQ scale ~ 1/ref, SDR scale invariant\n");
	{
		struct az_lum_source_desc pq = tagged(AZ_TF_PQ, AZ_PRIM_BT2020, 0.0f);
		struct az_lum_source_desc sdr = untagged();
		struct az_lum_rules none = {0.0f, 0.0f};
		const float refs[] = {80.0f, 100.0f, 203.0f, 300.0f, 400.0f};
		int sdr_moved = 0, pq_wrong = 0;
		float sdr0 = az_lum_resolve(&sdr, &none, refs[0]).scale;
		for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
			struct az_lum_domain dp = az_lum_resolve(&pq, &none, refs[i]);
			struct az_lum_domain ds = az_lum_resolve(&sdr, &none, refs[i]);
			if (ds.scale != sdr0) {
				sdr_moved++;
			}
			double want = 10000.0 / refs[i];
			if (fabs(dp.scale - want) > 1e-3) {
				pq_wrong++;
			}
			printf("    ref %6.1f  pq %10.5f  sdr %6.3f\n", refs[i],
				(double)dp.scale, (double)ds.scale);
		}
		CHECK(sdr_moved == 0,
			"SDR scale is invariant under the reference sweep (%d moved)",
			sdr_moved);
		CHECK(pq_wrong == 0, "PQ scale is exactly 10000/ref at every point");
		/* The product is the invariant that matters physically: scale x ref
		 * is the absolute luminance scene 1.0 stands for, and for PQ it is
		 * 10000 cd/m2 at every reference -- which is why HDR media does not
		 * move when the UI brightness does. */
		for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
			struct az_lum_domain dp = az_lum_resolve(&pq, &none, refs[i]);
			NEAR((double)dp.scale * refs[i], 10000.0, 1e-2,
				"PQ scale x ref = 10000 cd/m2 at ref %.0f", refs[i]);
		}
	}

	/* ── the per-window claim (ADR-006's falsifier, unit form) ──────────── */
	printf("two windows, one scene: the rule is per window\n");
	{
		struct az_lum_source_desc s = untagged();
		struct az_lum_rules plain = {0.0f, 0.0f};
		struct az_lum_rules bright = {2.0f, 0.0f};
		struct az_lum_domain a = az_lum_resolve(&s, &plain, 203.0f);
		struct az_lum_domain b = az_lum_resolve(&s, &bright, 203.0f);
		NEAR(b.scale / a.scale, 2.0, 1e-6,
			"sdr-white-scale 2.0 brightens exactly one of two identical "
			"windows, by exactly 2x");
		/* and it is the SAME factor whatever the reference is, because the
		 * scene is one scene: the SDR output and the HDR output see the same
		 * domain. */
		struct az_lum_domain b2 = az_lum_resolve(&s, &bright, 400.0f);
		CHECK(b2.scale == b.scale,
			"the domain does not depend on any output-side quantity");
	}

	/* ── the premise tests ──────────────────────────────────────────────── */
	printf("malformed input falls back rather than failing\n");
	{
		/* A transfer function from a protocol version this build has never
		 * heard of. It must not index a table, must not produce a scale of
		 * zero, and must not stop the surface being drawn. */
		struct az_lum_source_desc junk = tagged((enum az_tf)999,
			(enum az_primaries)77, 0.0f);
		CHECK(!az_lum_source_valid(&junk),
			"an out-of-range tf is reported invalid (so the caller can log "
			"it once)");
		struct az_lum_rules none = {0.0f, 0.0f};
		struct az_lum_domain d = az_lum_resolve(&junk, &none, 203.0f);
		CHECK(d.tf == AZ_TF_SRGB && d.primaries == AZ_PRIM_BT709,
			"and it resolves to the untagged default (ADR-004)");
		NEAR(d.scale, 1.0, 1e-9, "with scale 1");

		/* untagged is a valid state, not a malformed one -- the distinction
		 * matters because one of them should produce a log line and the
		 * other is every surface on the desktop. */
		struct az_lum_source_desc plain = untagged();
		CHECK(az_lum_source_valid(&plain), "untagged is VALID, not malformed");

		/* A rule written as 0 or as a negative is "unset", the same
		 * convention APPLY_FLOAT_PROP uses everywhere else in the config. */
		struct az_lum_rules bad = {-3.0f, -1.0f};
		struct az_lum_domain n = az_lum_resolve(&plain, &bad, 203.0f);
		CHECK(n.scale > 0.0f, "scale > 0 even from a negative rule");
		NEAR(n.scale, 1.0, 1e-9, "a negative rule reads as unset");

		/* NULLs: the scene walk has surfaces with no description and clients
		 * with no rules, and neither is an error. */
		struct az_lum_domain z = az_lum_resolve(NULL, NULL, 0.0f);
		CHECK(z.tf == AZ_TF_SRGB && z.scale == 1.0f && z.content_peak == 0.0f,
			"NULL source and NULL rules resolve to the untagged default");
	}

	/* ── the struct's own contract ──────────────────────────────────────── */
	printf("the struct\n");
	CHECK(sizeof(struct az_lum_domain) == 16,
		"az_lum_domain is 16 bytes (got %zu)",
		sizeof(struct az_lum_domain));
	/* content_peak 0 means UNKNOWN. Spelled out as a test because the one
	 * way this field gets misread is as a ceiling of zero, which would
	 * tone-map every untagged window to black. */
	{
		struct az_lum_source_desc pq = tagged(AZ_TF_PQ, AZ_PRIM_BT2020, 0.0f);
		struct az_lum_rules none = {0.0f, 0.0f};
		struct az_lum_domain d = az_lum_resolve(&pq, &none, 203.0f);
		CHECK(d.content_peak == 0.0f,
			"PQ without max_cll leaves content_peak 0 = unknown");
		struct az_lum_source_desc pq2 = tagged(AZ_TF_PQ, AZ_PRIM_BT2020,
			4000.0f);
		struct az_lum_domain d2 = az_lum_resolve(&pq2, &none, 203.0f);
		CHECK(d2.content_peak > d.content_peak,
			"and a declared max_cll produces a real one");
		/* content_peak must NOT absorb the gain: it describes the master,
		 * not the presentation. */
		struct az_lum_rules g = {0.0f, 0.5f};
		struct az_lum_domain d3 = az_lum_resolve(&pq2, &g, 203.0f);
		CHECK(d3.content_peak == d2.content_peak,
			"hdr-gain moves scale but not content_peak");
		CHECK(d3.scale < d2.scale, "  (and it did move scale -- premise)");
	}

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
