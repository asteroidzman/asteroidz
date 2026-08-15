/*
 * M4F.2A.3: every command kind states what it samples, and none of them can
 * state it by omission.
 *
 * WHY THIS FILE EXISTS. avk_render_declare_segment() used to derive its graph
 * uses inline, and the derivation was `if (cmd->type == AVK_CMD_TEXTURE)`. When
 * AVK_CMD_BLUR arrived -- a command that samples its own finished result -- it
 * was not added to that condition. The consequence was a missing barrier, and a
 * missing barrier does not fail: it renders. Validation caught it
 * (VUID-vkCmdDraw-imageLayout-00344) and the driver would not have.
 *
 * So the knowledge moved into avk_cmd_graph_uses(), once, and this file is the
 * test that every kind has been through it. It is deliberately NOT a test of
 * synchronisation -- Vulkan's own validation layers remain the oracle for that,
 * because they see the actual barriers. This is a structural test whose only
 * job is to notice an omission earlier than a validation run would.
 *
 * NO GPU IS NEEDED and none is opened. The resolver is pure: a scene, an index,
 * a context, an answer.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "render/vulkan/scene/avk_render.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { \
			printf("  ok   " __VA_ARGS__); \
			printf("\n"); \
		} else { \
			failures++; \
			printf("  FAIL " __VA_ARGS__); \
			printf("   (%s:%d)\n", __FILE__, __LINE__); \
		} \
	} while (0)

/* Stand-ins. Nothing dereferences them: the resolver reports WHICH images a
 * command consumes, and identity is all that is being checked. */
static struct avk_image fake_surface;
static struct avk_image fake_blur_result;

/*
 * EVERY COMMAND KIND, AND WHAT IT MUST SAY.
 *
 * A table rather than a sequence of calls, because the point of the exercise is
 * COVERAGE: the count below is asserted against AVK_CMD_TYPE_COUNT, so a new
 * command type fails this test at the same moment it fails the _Static_assert
 * in avk_render.c -- once for the implementation, once for the test that was
 * supposed to notice.
 */
struct expectation {
	enum avk_cmd_type type;
	const char *name;
	/* NULL means the command genuinely samples nothing, which is a statement,
	 * not an absence -- see the "no resource is not unknown resource" note in
	 * avk_render.h. */
	struct avk_image *expect;
	const char *why;
};

static const struct expectation table[] = {
	{ AVK_CMD_RECT, "rect", NULL,
		"its input is a colour or a gradient; gradient colours live in a "
		"per-frame storage buffer bound for the whole segment" },
	{ AVK_CMD_TEXTURE, "texture", &fake_surface,
		"the client surface it draws" },
	{ AVK_CMD_TEXTURE_QUAD, "texture-quad", &fake_surface,
		"the same client surface a texture samples -- only its destination "
		"corners differ, and the graph cares what a draw READS, not where it "
		"lands" },
	{ AVK_CMD_SHADOW, "shadow", NULL,
		"M4D's shadow is analytic -- an SDF in the fragment shader, with no "
		"blurred texture to sample" },
	{ AVK_CMD_BLUR, "blur", &fake_blur_result,
		"its own finished result, produced earlier this frame" },
};

static void test_every_kind(void) {
	printf("\n-- every command kind declares its sampled resources --\n");

	CHECK((int)(sizeof(table) / sizeof(table[0])) == AVK_CMD_TYPE_COUNT,
		"the table covers all %d command types -- a new one fails HERE as well "
		"as in avk_cmd_graph_uses()", AVK_CMD_TYPE_COUNT);

	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		const struct expectation *e = &table[i];

		struct avk_scene scene;
		avk_scene_init(&scene);
		struct avk_cmd *cmd = avk_scene_add(&scene, e->type);
		cmd->dst = (struct avk_box){ 0, 0, 32, 32 };
		/* Both sampling kinds read cmd->image. Spelled as a list rather than
		 * as `!= AVK_CMD_RECT` so a new kind that samples something has to be
		 * named here too, instead of being handed an image by a condition that
		 * happens to include it. */
		if (e->type == AVK_CMD_TEXTURE || e->type == AVK_CMD_TEXTURE_QUAD) {
			cmd->image = &fake_surface;
		}

		struct avk_blur_result results[1] = {
			{ .image = &fake_blur_result, .capture = { 0, 0, 32, 32 } },
		};
		struct avk_cmd_use_ctx ctx = {
			.blur_results = results,
			.blur_results_len = 1,
		};

		struct avk_cmd_uses uses;
		memset(&uses, 0xAA, sizeof(uses));
		bool ok = avk_cmd_graph_uses(&scene, 0, &ctx, &uses);

		CHECK(ok, "%s: the resolver has a declared answer for it", e->name);
		if (e->expect == NULL) {
			CHECK(uses.sampled_len == 0,
				"%s: samples nothing, stated -- %s", e->name, e->why);
		} else {
			CHECK(uses.sampled_len == 1 && uses.sampled[0] == e->expect,
				"%s: samples %s", e->name, e->why);
		}
		avk_scene_finish(&scene);
	}
}

static void test_blur_without_a_result(void) {
	printf("\n-- a blur whose chain did not run samples nothing --\n");

	/*
	 * Zero levels, a region too small, a transient that could not be had: in all
	 * three the renderer produces no result and the draw loop skips the command.
	 * The resolver must agree, and must still RETURN TRUE -- "there is nothing
	 * to sample" and "nobody has thought about this command type" are different
	 * answers and are reported differently.
	 */
	struct avk_scene scene;
	avk_scene_init(&scene);
	avk_scene_add(&scene, AVK_CMD_BLUR);

	struct avk_blur_result results[1] = { { .image = NULL } };
	struct avk_cmd_use_ctx ctx = {
		.blur_results = results, .blur_results_len = 1,
	};
	struct avk_cmd_uses uses;
	CHECK(avk_cmd_graph_uses(&scene, 0, &ctx, &uses),
		"the answer is still a declared one");
	CHECK(uses.sampled_len == 0, "and it is 'nothing'");

	/* No context at all -- what a caller replaying a range outside a frame
	 * would pass. Same answer, no crash, no invented resource. */
	CHECK(avk_cmd_graph_uses(&scene, 0, NULL, &uses)
		&& uses.sampled_len == 0, "and with no context either");

	avk_scene_finish(&scene);
}

static void test_texture_without_an_image(void) {
	printf("\n-- a texture command with no image samples nothing --\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	avk_scene_add(&scene, AVK_CMD_TEXTURE);   /* image left NULL */

	struct avk_cmd_uses uses;
	CHECK(avk_cmd_graph_uses(&scene, 0, NULL, &uses), "declared");
	CHECK(uses.sampled_len == 0, "and empty rather than a NULL entry");
	avk_scene_finish(&scene);
}

static void test_out_of_range(void) {
	printf("\n-- an index past the end is refused, not answered --\n");

	struct avk_scene scene;
	avk_scene_init(&scene);
	struct avk_cmd_uses uses;
	CHECK(!avk_cmd_graph_uses(&scene, 0, NULL, &uses),
		"an empty scene has no command 0");
	avk_scene_add(&scene, AVK_CMD_RECT);
	CHECK(avk_cmd_graph_uses(&scene, 0, NULL, &uses), "but it has now");
	CHECK(!avk_cmd_graph_uses(&scene, 1, NULL, &uses),
		"and still not a command 1");
	CHECK(!avk_cmd_graph_uses(NULL, 0, NULL, &uses), "nor a NULL scene");
	CHECK(!avk_cmd_graph_uses(&scene, 0, NULL, NULL), "nor a NULL output");
	avk_scene_finish(&scene);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("== avk command graph-uses contract (M4F.2A.3) ==\n");

	test_every_kind();
	test_blur_without_a_result();
	test_texture_without_an_image();
	test_out_of_range();

	printf("\n---- %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
