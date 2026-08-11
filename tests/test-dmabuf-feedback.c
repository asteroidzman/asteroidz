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
	struct avk_modifier_caps rgb_mods[] = {
		{ .modifier = MOD_A }, { .modifier = MOD_B }, { .modifier = MOD_C },
		{ .modifier = DRM_FORMAT_MOD_INVALID },
	};
	struct avk_modifier_caps ycbcr_mods[] = { { .modifier = MOD_A } };
	/* Render-only modifiers must not leak into the composition set: client
	 * content is sampled, and this is the array that would be used if the
	 * wrong field were read. */
	struct avk_modifier_caps render_only[] = { { .modifier = MOD_D } };

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
	CHECK(az_dmabuf_composition_formats(&table, &out),
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
	CHECK(!az_dmabuf_composition_formats(&only_ycbcr, &empty),
		"a table with nothing advertisable is refused, not sent as an empty set");

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
