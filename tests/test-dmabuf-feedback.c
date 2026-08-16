/*
 * The DMA-BUF capability model, driven with SYNTHETIC capabilities.
 *
 * WHY THIS IS NOT A COMPOSITOR TEST
 *
 * contrib/avk-dmabuf-feedback-test.sh boots a compositor and checks that the
 * advertised set is a subset of what AVK reports. That is worth having, and it
 * cannot test the thing M3.6 is actually about. On this machine the GLES and
 * Vulkan format tables overlap so heavily that advertising the WRONG one
 * produces a set which passes every subset check -- the shipped bug did, for
 * the whole of M3.5. The live test detects the break only because the reported
 * SOURCE string changes, which protects a label rather than a rule.
 *
 * So the rule is tested here instead, on capabilities this file invents:
 *
 *     AVK can import   A B C
 *     GLES can import  A B D
 *     must advertise   A B C          -- C present, D absent
 *
 * A build that derives feedback from the compatibility renderer produces
 * A B D and fails on both halves. Nothing here depends on which GPU is in the
 * machine, which Mesa is installed, or whether the two happen to agree today.
 */

#define _POSIX_C_SOURCE 200809L

#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <wlr/util/log.h>
#include "render/vulkan/dmabuf/avk_format_table.h"
#include "render/az_dmabuf_model.h"

static int failures, checks;
#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); } \
		else { failures++; printf("  FAIL " __VA_ARGS__); \
			printf("   (%s:%d)\n", __FILE__, __LINE__); } \
	} while (0)

/* Modifiers chosen to be recognisable in a failure message rather than
 * realistic. A/B are "both engines agree", C is "only AVK", D is "only GLES". */
#define MOD_A 0x0100000000000001ULL
#define MOD_B 0x0100000000000002ULL
#define MOD_C 0x0100000000000003ULL
#define MOD_D 0x0100000000000004ULL

static struct avk_drm_format fmt_rgb = {
	.drm = DRM_FORMAT_XRGB8888, .plane_count = 1, .is_ycbcr = false,
};
static struct avk_drm_format fmt_ycbcr = {
	.drm = DRM_FORMAT_NV12, .plane_count = 2, .is_ycbcr = true,
};

int main(void) {
	wlr_log_init(WLR_ERROR, NULL);
	printf("dmabuf capability model\n");

	/* ── the AVK set: A, B, C -- plus MOD_INVALID, which no driver reports
	 * but which must be filtered even if one ever did, and a YCbCr format
	 * that is importable but whose colour handling is not implemented. ── */
	/*
	 * BIG is "importable at any size the device allows"; the bar the model is
	 * given below is exactly BIG. Stating it on every modifier is not
	 * ceremony: a zeroed max_extent means "importable up to 0x0", and a test
	 * that left it zero would be asserting against a table in which nothing is
	 * advertisable at all.
	 */
	const VkExtent2D BIG = { 16384, 16384 };
	struct avk_modifier_caps rgb_mods[] = {
		{ .modifier = MOD_A, .max_extent = BIG },
		{ .modifier = MOD_B, .max_extent = BIG },
		{ .modifier = MOD_C, .max_extent = BIG },
		{ .modifier = DRM_FORMAT_MOD_INVALID, .max_extent = BIG },
	};
	struct avk_modifier_caps ycbcr_mods[] = {
		{ .modifier = MOD_A, .max_extent = BIG },
	};
	/* Render-only modifiers must not leak into the composition set: client
	 * content is sampled, and this is the array that would be used if the
	 * wrong field were read. */
	struct avk_modifier_caps render_only[] = {
		{ .modifier = MOD_D, .max_extent = BIG },
	};

	struct avk_format_caps formats[] = {
		{ .format = &fmt_rgb, .texture_mods = rgb_mods, .texture_mod_count = 4,
		  .render_mods = render_only, .render_mod_count = 1 },
		{ .format = &fmt_ycbcr, .texture_mods = ycbcr_mods,
		  .texture_mod_count = 1, .render_mods = NULL, .render_mod_count = 0 },
	};
	struct avk_format_table table = {
		.formats = formats, .count = 2, .texture_pair_count = 5,
	};

	struct wlr_drm_format_set out;
	CHECK(az_dmabuf_composition_formats(&table, BIG, &out),
		"the model produces a set from AVK's table");

	/* THE ASSERTION THIS FILE EXISTS FOR. C is advertised because AVK has it;
	 * D is not, because only the compatibility renderer would. A build that
	 * asked the renderer fails both. */
	CHECK(wlr_drm_format_set_has(&out, DRM_FORMAT_XRGB8888, MOD_C),
		"a modifier only AVK supports IS advertised (C)");
	CHECK(!wlr_drm_format_set_has(&out, DRM_FORMAT_XRGB8888, MOD_D),
		"a modifier only the other renderer supports is NOT advertised (D)");
	CHECK(wlr_drm_format_set_has(&out, DRM_FORMAT_XRGB8888, MOD_A) &&
		wlr_drm_format_set_has(&out, DRM_FORMAT_XRGB8888, MOD_B),
		"modifiers both engines support are advertised (A, B)");

	CHECK(!wlr_drm_format_set_has(&out, DRM_FORMAT_XRGB8888,
			DRM_FORMAT_MOD_INVALID),
		"DRM_FORMAT_MOD_INVALID is filtered out even when present in the table");
	CHECK(wlr_drm_format_set_get(&out, DRM_FORMAT_NV12) == NULL,
		"a YCbCr format is withheld while the colour path cannot interpret it");

	const struct wlr_drm_format *rgb =
		wlr_drm_format_set_get(&out, DRM_FORMAT_XRGB8888);
	CHECK(rgb != NULL && rgb->len == 3,
		"exactly the three texture modifiers are advertised (got %zu)",
		rgb ? rgb->len : (size_t)0);
	wlr_drm_format_set_finish(&out);

	/* ── an empty capability set must be refused, not advertised ────────── */
	struct avk_format_caps none[] = {
		{ .format = &fmt_ycbcr, .texture_mods = ycbcr_mods,
		  .texture_mod_count = 1 },
	};
	struct avk_format_table only_ycbcr = {
		.formats = none, .count = 1, .texture_pair_count = 1,
	};
	struct wlr_drm_format_set empty;
	CHECK(!az_dmabuf_composition_formats(&only_ycbcr, BIG, &empty),
		"a table with nothing advertisable is refused, not sent as an empty set");

	/*
	 * ── SIZE-RESTRICTED PAIRS ───────────────────────────────────────────
	 *
	 * The live defect, reduced to a table. On Navi31 the displayable-DCC
	 * modifiers report maxExtent 2560x2560 while every other modifier for the
	 * same format reports 16384x16384. linux-dmabuf feedback is a list of
	 * format/modifier pairs with no size field, so advertising one of those is
	 * telling a client something that stops being true at a size the client
	 * chooses -- and a nested gamescope chooses the size of the output the
	 * moment Steam starts a game. The import then fails, AVK drops the draw,
	 * and the window is simply not there.
	 *
	 * MOD_C stands in for the DCC pair. It must not be advertised, and A and B
	 * must survive: withholding the restricted pair is the fix, withholding
	 * the format is not.
	 */
	printf("size-restricted modifiers\n");
	struct avk_modifier_caps mixed_mods[] = {
		{ .modifier = MOD_A, .max_extent = BIG },
		{ .modifier = MOD_B, .max_extent = BIG },
		{ .modifier = MOD_C, .max_extent = { 2560, 2560 } },
	};
	struct avk_format_caps mixed_fmt[] = {
		{ .format = &fmt_rgb, .texture_mods = mixed_mods,
		  .texture_mod_count = 3 },
	};
	struct avk_format_table mixed = {
		.formats = mixed_fmt, .count = 1, .texture_pair_count = 3,
	};
	struct wlr_drm_format_set mixed_out;
	CHECK(az_dmabuf_composition_formats(&mixed, BIG, &mixed_out),
		"a table with a size-restricted pair still produces a set");
	CHECK(!wlr_drm_format_set_has(&mixed_out, DRM_FORMAT_XRGB8888, MOD_C),
		"a pair importable only below the required extent is NOT advertised");
	CHECK(wlr_drm_format_set_has(&mixed_out, DRM_FORMAT_XRGB8888, MOD_A) &&
		wlr_drm_format_set_has(&mixed_out, DRM_FORMAT_XRGB8888, MOD_B),
		"the unrestricted pairs of the same format survive");
	CHECK(az_dmabuf_size_restricted_pairs == 1,
		"the restricted pair is counted, not silently dropped (got %" PRIu32 ")",
		az_dmabuf_size_restricted_pairs);
	wlr_drm_format_set_finish(&mixed_out);

	/*
	 * A format with NOTHING but restricted modifiers is the one case where
	 * withholding is worse than advertising: dropping XRGB8888 outright would
	 * take every client on the machine down to fix one. It is advertised, and
	 * the log says so.
	 */
	struct avk_modifier_caps small_only[] = {
		{ .modifier = MOD_A, .max_extent = { 2560, 2560 } },
	};
	struct avk_format_caps small_fmt[] = {
		{ .format = &fmt_rgb, .texture_mods = small_only,
		  .texture_mod_count = 1 },
	};
	struct avk_format_table small = {
		.formats = small_fmt, .count = 1, .texture_pair_count = 1,
	};
	struct wlr_drm_format_set small_out;
	CHECK(az_dmabuf_composition_formats(&small, BIG, &small_out),
		"a format whose every modifier is restricted is still advertised");
	CHECK(wlr_drm_format_set_has(&small_out, DRM_FORMAT_XRGB8888, MOD_A),
		"...with the restricted modifier, because the alternative is no format");
	wlr_drm_format_set_finish(&small_out);

	/*
	 * THE BREAK. With the switch set the model must go back to advertising
	 * the restricted pair -- if this passes, the assertions above are checking
	 * something that was never conditional and the fixture proves nothing.
	 */
	setenv("AZ_DMABUF_ADVERTISE_SIZE_RESTRICTED", "1", 1);
	struct wlr_drm_format_set broken_out;
	CHECK(az_dmabuf_composition_formats(&mixed, BIG, &broken_out),
		"break: the model still produces a set");
	CHECK(wlr_drm_format_set_has(&broken_out, DRM_FORMAT_XRGB8888, MOD_C),
		"break: AZ_DMABUF_ADVERTISE_SIZE_RESTRICTED=1 restores the defect");
	wlr_drm_format_set_finish(&broken_out);
	unsetenv("AZ_DMABUF_ADVERTISE_SIZE_RESTRICTED");

	/* ── the device: AVK's node, whatever the compatibility renderer uses ─ */
	printf("main device selection\n");
	int fd_a = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	int fd_b = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
	if (fd_a < 0 || fd_b < 0) {
		printf("  skip - needs two render nodes to tell the devices apart\n");
		if (fd_a >= 0) close(fd_a);
		if (fd_b >= 0) close(fd_b);
	} else {
		struct stat sa, sb;
		fstat(fd_a, &sa);
		fstat(fd_b, &sb);
		CHECK(sa.st_rdev != sb.st_rdev,
			"the two render nodes really are different devices");

		/* AVK on device B while some other renderer is on A: the feedback's
		 * main device must follow AVK. Constructed rather than hoped for --
		 * on this machine both engines happen to select the same node live,
		 * so the real compositor can never exercise the disagreement. */
		struct avk_device dev = { .drm_fd = fd_b };
		dev_t got = 0;
		CHECK(az_dmabuf_main_device(&dev, &got),
			"the model reports a main device");
		CHECK(got == sb.st_rdev,
			"the main device is AVK's node (%u:%u), not the other renderer's "
			"(%u:%u)", major(sb.st_rdev), minor(sb.st_rdev),
			major(sa.st_rdev), minor(sa.st_rdev));

		struct avk_device none_dev = { .drm_fd = -1 };
		CHECK(!az_dmabuf_main_device(&none_dev, &got),
			"an engine with no DRM node reports no device rather than zero");
		close(fd_a);
		close(fd_b);
	}

	printf("\n%d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
