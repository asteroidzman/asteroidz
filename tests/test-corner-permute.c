/*
 * Corner permutation under output transforms — the production helper, driven
 * directly.
 *
 * WHY THIS IS NOT A COMPOSITOR TEST
 *
 * A rotated output needs a monitorrule, whose colon-separated key:value format
 * the headless harness cannot emit, so there is no route to a transformed
 * output in the fixture. That is a harness limitation and not a reason to
 * leave the invariant untested: the permutation is a pure function of the
 * transform, so it is called here exactly as production calls it.
 *
 * WHAT IS ASSERTED
 *
 * Not the specific mapping -- that would restate the implementation and agree
 * with it however wrong it was. What is asserted are the PROPERTIES any
 * correct permutation must have, plus one anchor case:
 *
 *   - NORMAL is the identity;
 *   - every transform is a bijection: four corners in, four distinct corners
 *     out, none dropped or duplicated. A permutation that collapses two
 *     corners is the shape of the bottom-swap family of bugs;
 *   - a rotation preserves cyclic (clockwise) order, because rotating a
 *     rectangle cannot reorder its corners around the perimeter;
 *   - a flip REVERSES cyclic order, because a mirror does;
 *   - 180 degrees maps each corner to its diagonal opposite.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "render/az_corner_permute.h"

static int failures, checks;
#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); } \
		else { failures++; printf("  FAIL " __VA_ARGS__); \
			printf("   (%s:%d)\n", __FILE__, __LINE__); } \
	} while (0)

/* Distinct, so a swap or a collapse is visible. Clockwise: tl, tr, br, bl. */
static const float IN[4] = { 1.0f, 2.0f, 3.0f, 4.0f };

static int index_of(const float out[4], float v) {
	for (int i = 0; i < 4; i++) {
		if (out[i] == v) {
			return i;
		}
	}
	return -1;
}

static bool is_rotation_of(const float out[4]) {
	/* out is a cyclic rotation of IN if some shift k maps every element. */
	for (int k = 0; k < 4; k++) {
		bool ok = true;
		for (int i = 0; i < 4; i++) {
			if (out[(i + k) % 4] != IN[i]) {
				ok = false;
				break;
			}
		}
		if (ok) {
			return true;
		}
	}
	return false;
}

static bool is_reflection_of(const float out[4]) {
	/* A mirror reverses the clockwise walk; any starting point is allowed. */
	for (int k = 0; k < 4; k++) {
		bool ok = true;
		for (int i = 0; i < 4; i++) {
			if (out[(k - i + 8) % 4] != IN[i]) {
				ok = false;
				break;
			}
		}
		if (ok) {
			return true;
		}
	}
	return false;
}

int main(void) {
	printf("corner permutation under output transforms\n");

	static const struct {
		enum wl_output_transform t;
		const char *name;
		bool flipped;
	} cases[] = {
		{ WL_OUTPUT_TRANSFORM_NORMAL,      "normal",      false },
		{ WL_OUTPUT_TRANSFORM_90,          "90",          false },
		{ WL_OUTPUT_TRANSFORM_180,         "180",         false },
		{ WL_OUTPUT_TRANSFORM_270,         "270",         false },
		{ WL_OUTPUT_TRANSFORM_FLIPPED,     "flipped",     true  },
		{ WL_OUTPUT_TRANSFORM_FLIPPED_90,  "flipped-90",  true  },
		{ WL_OUTPUT_TRANSFORM_FLIPPED_180, "flipped-180", true  },
		{ WL_OUTPUT_TRANSFORM_FLIPPED_270, "flipped-270", true  },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		float out[4] = {0};
		az_corner_permute(IN, cases[i].t, out);
		printf("  %-12s tl<-%g tr<-%g br<-%g bl<-%g\n", cases[i].name,
			out[0], out[1], out[2], out[3]);

		bool bijection = true;
		for (int v = 1; v <= 4; v++) {
			if (index_of(out, (float)v) < 0) {
				bijection = false;
			}
		}
		CHECK(bijection, "%s: every corner survives exactly once",
			cases[i].name);

		if (cases[i].flipped) {
			CHECK(is_reflection_of(out),
				"%s: a mirror reverses the clockwise order", cases[i].name);
		} else {
			CHECK(is_rotation_of(out),
				"%s: a rotation preserves the clockwise order", cases[i].name);
		}
	}

	float out[4] = {0};
	az_corner_permute(IN, WL_OUTPUT_TRANSFORM_NORMAL, out);
	CHECK(memcmp(out, IN, sizeof(out)) == 0, "normal is the identity");

	az_corner_permute(IN, WL_OUTPUT_TRANSFORM_180, out);
	CHECK(out[0] == IN[2] && out[1] == IN[3] && out[2] == IN[0]
			&& out[3] == IN[1],
		"180 maps every corner to its diagonal opposite");

	az_corner_permute(IN, WL_OUTPUT_TRANSFORM_90, out);
	float back[4] = {0};
	az_corner_permute(out, WL_OUTPUT_TRANSFORM_270, back);
	CHECK(memcmp(back, IN, sizeof(back)) == 0,
		"90 then 270 returns to where it started");

	printf("\n%d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
