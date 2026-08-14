/*
 * M5/C2: every command carries a VALID luminance domain, not a zeroed one.
 *
 * ── THE ONE STATEMENT THIS FILE ESTABLISHES ──────────────────────────────
 *
 *   No matter which command type is created, and whether or not its site
 *   knows anything about colour, `cmd->lum.scale > 0` and its enums are in
 *   range.
 *
 * ── WHY THIS IS WORTH A TEST OF ITS OWN ──────────────────────────────────
 *
 * `scale` is a multiplier a decode shader applies to a source's pixels. The
 * resolver's contract is that it is always positive, and every consumer is
 * entitled to rely on that. avk_scene_add() memsets the command, so the
 * DEFAULT value of a field nobody fills is zero -- and a scale of zero is not
 * a neutral default, it is a black surface.
 *
 * That is not hypothetical. Three of the four command types have no source
 * description at all (a rect, a shadow and a blur result are already scene
 * values), and so does the output cursor, which is a texture command built
 * from a wlr_output_cursor rather than a scene buffer. Four sites that would
 * each have had to remember. The default lives in avk_scene_add() instead, and
 * this asserts it for every type -- including types added after this was
 * written, because the loop runs to AVK_CMD_TYPE_COUNT.
 *
 * No GPU: avk_scene_add() is allocation and assignment.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>

#include "render/vulkan/scene/avk_scene.h"

static int failures;

static void check(bool ok, const char *what) {
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

int main(void) {
	printf("== M5/C2: every command's luminance domain ==\n\n");

	struct avk_scene scene = {0};

	/* EVERY type, by count rather than by name: a command type added later is
	 * covered without anyone remembering to come back here. */
	for (int t = 0; t < AVK_CMD_TYPE_COUNT; t++) {
		struct avk_cmd *cmd = avk_scene_add(&scene, (enum avk_cmd_type)t);
		char what[96];
		if (cmd == NULL) {
			snprintf(what, sizeof(what), "type %d: allocated", t);
			check(false, what);
			continue;
		}
		snprintf(what, sizeof(what), "type %d: scale > 0 (got %.4f)",
			t, (double)cmd->lum.scale);
		check(cmd->lum.scale > 0.0f, what);

		snprintf(what, sizeof(what), "type %d: tf in range (got %d)",
			t, (int)cmd->lum.tf);
		check((int)cmd->lum.tf >= 0 && (int)cmd->lum.tf < (int)AZ_TF_COUNT,
			what);

		snprintf(what, sizeof(what), "type %d: primaries in range (got %d)",
			t, (int)cmd->lum.primaries);
		check((int)cmd->lum.primaries >= 0
			&& (int)cmd->lum.primaries < (int)AZ_PRIM_COUNT, what);

		/* content_peak 0 means "no per-source ceiling" and is the correct
		 * default -- asserted so that a future change to a sentinel like -1
		 * has to come through here. */
		snprintf(what, sizeof(what), "type %d: content_peak is 0 (unknown)", t);
		check(cmd->lum.content_peak == 0.0f, what);
	}

	/*
	 * AND THE DEFAULT IS THE UNTAGGED DOMAIN, not merely a positive number.
	 * ADR-004 says an untagged surface is piecewise-sRGB BT.709 at scale 1;
	 * a command that defaulted to, say, PQ would satisfy every check above and
	 * be wrong in a way only a picture would show.
	 */
	struct avk_cmd *cmd = avk_scene_add(&scene, AVK_CMD_RECT);
	struct az_lum_domain want = az_lum_domain_untagged();
	check(cmd != NULL && cmd->lum.tf == want.tf
		&& cmd->lum.primaries == want.primaries
		&& cmd->lum.scale == want.scale
		&& cmd->lum.content_peak == want.content_peak,
		"the default IS the ADR-004 untagged domain");

	/*
	 * THE PREMISE: a zeroed domain would have failed the first check.
	 *
	 * Without this the loop above passes on a build where `lum` happens to be
	 * fine for another reason, and nobody learns that the assertion has teeth.
	 */
	struct az_lum_domain zeroed = {0};
	check(!(zeroed.scale > 0.0f),
		"PREMISE: a zeroed domain does NOT satisfy scale > 0");

	avk_scene_finish(&scene);

	printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
