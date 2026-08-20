/*
 * test-avk-encode -- the H.265 Main 10 encode session, on a real device.
 *
 * M14A. This asserts the part of the encoder that the driver can refuse: the
 * profile, the session, its memory bindings and its parameter sets. Whether
 * the resulting bitstream decodes is a later question and a different test;
 * what this one exists to catch is the class of failure where a session is
 * created against a profile that is nearly right and then rejects every
 * picture -- which is a runtime error in a compositor and a compile-time-shaped
 * bug in nobody's build.
 *
 * The premise is asserted first. A test that "passes" on a machine with no
 * encode queue has asserted nothing, so the encode capability is checked and
 * the run SKIPS rather than reporting success.
 *
 * Exits 77 (meson's skip) with no GPU or no encode hardware.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "render/vulkan/device/avk_device.h"
#include "render/vulkan/device/avk_instance.h"
#include "render/vulkan/encode/avk_encode.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
		checks++; \
		if (cond) { \
			printf("  ok   "); \
		} else { \
			printf("  FAIL "); \
			failures++; \
		} \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} while (0)

#define SKIP(...) do { \
		printf("SKIP: " __VA_ARGS__); \
		printf("\n"); \
		return 77; \
	} while (0)

static int open_render_node(void) {
	for (int i = 128; i < 136; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			return fd;
		}
	}
	return -1;
}

int main(void) {
	printf("== avk encode (M14A) ==\n");

	int fd = open_render_node();
	if (fd < 0) {
		SKIP("no DRM render node available");
	}
	struct avk_instance *inst = avk_instance_create("test-avk-encode");
	if (inst == NULL) {
		close(fd);
		SKIP("no usable Vulkan instance");
	}
	struct avk_device *dev = avk_device_create(inst, fd);
	if (dev == NULL) {
		avk_instance_destroy(inst);
		close(fd);
		SKIP("no usable Vulkan device");
	}

	/* The premise. Without this the checks below would pass by not running. */
	if (!dev->has_encode_queue) {
		printf("  device caps: encode_family=%d video_queue=%d "
			"encode_queue=%d h265=%d rgb=%d\n",
			dev->caps.has_video_encode_family,
			dev->caps.video_queue, dev->caps.video_encode_queue,
			dev->caps.video_encode_h265, dev->caps.video_encode_rgb);
		avk_device_destroy(dev);
		avk_instance_destroy(inst);
		close(fd);
		SKIP("this device cannot encode video");
	}
	printf("  encode queue on family %u\n", dev->caps.video_encode_family);

	/* The real display's size, because the alignment question only has a
	 * right answer relative to a real one: 3840x2160 lands exactly on this
	 * encoder's 64x16 granularity and 1366x768 does not. */
	struct avk_encoder *enc = avk_encoder_create(dev, 3840, 2160);
	CHECK(enc != NULL, "a 3840x2160 H.265 Main 10 session is created");
	if (enc != NULL) {
		CHECK(enc->session != VK_NULL_HANDLE, "the session handle is real");
		CHECK(enc->params != VK_NULL_HANDLE,
			"its VPS/SPS/PPS parameter sets were accepted");
		CHECK(enc->coded_width == 3840 && enc->coded_height == 2160,
			"3840x2160 needs no padding (coded %ux%u)",
			enc->coded_width, enc->coded_height);
		/* The whole architectural claim of M14A: the encoder takes the format
		 * AVK already renders an HDR output in, so the composited image is
		 * handed over rather than converted. */
		CHECK(enc->src_format == VK_FORMAT_A2R10G10B10_UNORM_PACK32,
			"the encode source format is A2R10G10B10 -- AVK's own HDR "
			"render format (got %d)", (int)enc->src_format);
		avk_encoder_destroy(enc);
	}

	/* A size that does NOT land on the granularity has to be rounded up
	 * rather than silently encoded at the wrong extent. 1366 is the width
	 * that made this worth asserting: 1366/64 is not an integer. */
	struct avk_encoder *odd = avk_encoder_create(dev, 1366, 768);
	CHECK(odd != NULL, "an unaligned 1366x768 session is created too");
	if (odd != NULL) {
		CHECK(odd->coded_width == 1408,
			"1366 is rounded up to the 64-wide granularity (coded %u)",
			odd->coded_width);
		CHECK(odd->coded_height == 768,
			"768 already lands on the 16-high granularity (coded %u)",
			odd->coded_height);
		avk_encoder_destroy(odd);
	}

	avk_device_destroy(dev);
	avk_instance_destroy(inst);
	close(fd);

	printf("\n%d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
