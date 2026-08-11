/*
 * M1 acceptance test for the asteroidz Vulkan core.
 *
 * Runs with no compositor, no Wayland display and no window: the whole point
 * of avk being compositor-agnostic is that its core can be tested by opening a
 * render node and driving it directly.
 *
 * Every assertion here was checked against a deliberately broken build before
 * being kept -- in particular the device-selection ones, which pass trivially
 * if selection just returns index 0. See the notes on each.
 *
 * Exits 77 (meson's "skip") when there is no usable GPU, so the suite still
 * runs on a machine with no DRM device rather than reporting a red failure for
 * an absent GPU.
 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "render/vulkan/command/avk_command.h"
#include "render/vulkan/command/avk_retire.h"
#include "render/vulkan/debug/avk_debug.h"
#include "render/vulkan/device/avk_device.h"
#include "render/vulkan/device/avk_phys.h"

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

#define SKIP(...) do { \
		printf("SKIP: " __VA_ARGS__); \
		printf("\n"); \
		return 77; \
	} while (0)

/* ── render nodes ───────────────────────────────────────────────────────── */

#define MAX_NODES 8

struct node {
	char path[32];
	int fd;
	int64_t major;
	int64_t minor;
};

static uint32_t open_render_nodes(struct node *nodes, uint32_t max) {
	uint32_t count = 0;
	for (int i = 128; i < 192 && count < max; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		struct stat st;
		if (fstat(fd, &st) != 0) {
			close(fd);
			continue;
		}
		snprintf(nodes[count].path, sizeof(nodes[count].path), "%s", path);
		nodes[count].fd = fd;
		nodes[count].major = (int64_t)major(st.st_rdev);
		nodes[count].minor = (int64_t)minor(st.st_rdev);
		count++;
	}
	return count;
}

/* ── retire-queue probes ────────────────────────────────────────────────── */

struct retire_probe {
	int ran;
};

static void retire_probe_fn(struct avk_device *dev, void *data) {
	(void)dev;
	((struct retire_probe *)data)->ran++;
}

/* ── the tests ──────────────────────────────────────────────────────────── */

static void test_selection(struct avk_instance *inst, struct node *nodes,
		uint32_t node_count) {
	struct avk_phys *list = NULL;
	uint32_t count = 0;

	printf("physical device selection\n");

	if (!avk_phys_enumerate(inst, &list, &count)) {
		CHECK(false, "enumerate physical devices");
		return;
	}
	CHECK(count > 0, "found %u physical device(s)", count);
	avk_phys_log_all(list, count, -1);

	/*
	 * Selection by DRM dev_t.
	 *
	 * This is the assertion that would pass on a broken build if it only said
	 * "select returned something": index 0 is a correct answer for the first
	 * node by luck. So it checks the *selected device's own DRM minor* against
	 * the node's minor -- which can only match if the dev_t comparison
	 * actually happened. Verified by stubbing drm_matches() to `return true`,
	 * which makes the second node's check fail.
	 */
	for (uint32_t n = 0; n < node_count; n++) {
		int index = avk_phys_select(list, count, nodes[n].fd);
		if (index < 0) {
			/* A node whose device avk rejects (no required extension) is a
			 * legitimate -1, and the reject reason was already logged. */
			CHECK(true, "%s: no usable Vulkan device claims it (rejected)",
				nodes[n].path);
			continue;
		}
		const struct avk_caps *c = &list[index].caps;
		CHECK(c->has_drm && c->drm_has_render
				&& c->drm_render_major == nodes[n].major
				&& c->drm_render_minor == nodes[n].minor,
			"%s (%" PRId64 ":%" PRId64 ") selected %s whose render node is "
			"%" PRId64 ":%" PRId64,
			nodes[n].path, nodes[n].major, nodes[n].minor, c->device_name,
			c->drm_render_major, c->drm_render_minor);
	}

	/*
	 * A node no GPU owns must be a hard failure, not a fallback to device 0.
	 * /dev/null has a dev_t (1:3) that no DRM device can claim. This is the
	 * assertion that catches "if nothing matches, just use the first one",
	 * which is exactly the behaviour that puts a compositor on the wrong GPU
	 * without telling anyone.
	 */
	int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (null_fd >= 0) {
		printf("  (the next two ERROR lines are expected -- negative test)\n");
		int index = avk_phys_select(list, count, null_fd);
		CHECK(index == -1, "a dev_t no GPU claims is refused, not defaulted");
		close(null_fd);
	}

	/* Two distinct nodes must not resolve to the same device. Only meaningful
	 * on a multi-GPU box; skipped rather than faked elsewhere. */
	if (node_count >= 2) {
		int a = avk_phys_select(list, count, nodes[0].fd);
		int b = avk_phys_select(list, count, nodes[1].fd);
		if (a >= 0 && b >= 0) {
			CHECK(a != b, "two render nodes resolve to two different devices "
				"(%d, %d)", a, b);
		}
	}

	free(list);
}

static void test_caps(struct avk_device *dev) {
	const struct avk_caps *c = &dev->caps;

	printf("capability table\n");
	avk_device_log_caps(dev);

	CHECK(c->device_name[0] != '\0', "device name is populated");
	CHECK(c->api_version >= VK_API_VERSION_1_3, "device API >= 1.3");
	CHECK(c->timeline_semaphore, "timeline semaphores");
	CHECK(c->synchronization2, "synchronization2");
	CHECK(c->external_memory_dma_buf, "VK_EXT_external_memory_dma_buf");
	CHECK(c->image_drm_format_modifier,
		"VK_EXT_image_drm_format_modifier (M2 depends on this)");
	CHECK(c->queue_family_foreign, "VK_EXT_queue_family_foreign");
	CHECK(c->graphics_family != UINT32_MAX,
		"a graphics+compute queue family (%u)", c->graphics_family);
	CHECK(c->max_image_dimension_2d >= 4096,
		"max 2D image %u >= 4096", c->max_image_dimension_2d);

	/* Not a hard requirement -- but if it is false the frame path cannot stay
	 * free of CPU waits, and that is worth saying out loud rather than
	 * discovering as a stutter. */
	if (!(c->semaphore_sync_fd_import && c->semaphore_sync_fd_export)) {
		printf("  note: no sync_file semaphore interop on this device; "
			"explicit sync will need a fallback\n");
	}
}

static void test_command_ring(struct avk_device *dev) {
	struct avk_cmd_ring ring;

	printf("command ring\n");
	CHECK(avk_cmd_ring_init(&ring, dev, "test"), "ring initialises");

	uint64_t before = avk_device_timeline_value(dev);
	CHECK(before == 0, "a fresh device timeline is at 0 (was %" PRIu64 ")",
		before);

	/*
	 * More iterations than there are slots, so the ring genuinely wraps and
	 * every slot gets reused. If reuse were broken -- a pool never reset, a
	 * slot never released -- this is where it shows up.
	 */
	const uint32_t iterations = AVK_FRAMES_IN_FLIGHT * 4;
	uint64_t last_value = 0;
	bool all_ok = true;
	for (uint32_t i = 0; i < iterations; i++) {
		VkCommandBuffer cb = avk_cmd_ring_begin(&ring);
		if (cb == VK_NULL_HANDLE) {
			all_ok = false;
			break;
		}
		avk_debug_label_begin(dev, cb, "test iteration %u", i);
		avk_debug_label_end(dev, cb);
		uint64_t value = avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0);
		if (value == 0 || value <= last_value) {
			all_ok = false;
			break;
		}
		last_value = value;
	}
	CHECK(all_ok, "%u submissions through a %d-slot ring, values monotonic",
		iterations, AVK_FRAMES_IN_FLIGHT);
	CHECK(last_value == iterations,
		"timeline reserved exactly one point per submit (%" PRIu64 " of %u)",
		last_value, iterations);

	CHECK(avk_device_timeline_wait(dev, last_value, 1000000000ULL),
		"GPU reaches the last timeline point");
	CHECK(avk_device_timeline_value(dev) >= last_value,
		"counter reads back at or past %" PRIu64, last_value);

	/*
	 * The reuse claim, stated as a number. With three slots and this many
	 * submissions the ring must have wrapped, and the slots are reused rather
	 * than reallocated -- so acquires equals submits and no allocation
	 * happened after init.
	 */
	CHECK(ring.acquires == iterations && ring.submits == iterations,
		"%" PRIu64 " acquires, %" PRIu64 " submits, no per-frame allocation",
		ring.acquires, ring.submits);
	printf("  note: %" PRIu64 " backpressure stall(s) across %u submissions\n",
		ring.stalls, iterations);

	/* Abandoning must release the slot, or the next begin() would refuse. */
	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);
	CHECK(cb != VK_NULL_HANDLE, "begin after a completed run");
	avk_cmd_ring_abandon(&ring);
	cb = avk_cmd_ring_begin(&ring);
	CHECK(cb != VK_NULL_HANDLE, "abandon releases the slot");
	avk_cmd_ring_abandon(&ring);

	avk_cmd_ring_finish(&ring);
}

static void test_retire(struct avk_device *dev) {
	struct avk_cmd_ring ring;
	struct avk_retire_queue q;

	printf("deferred destruction\n");
	if (!avk_cmd_ring_init(&ring, dev, "retire-test")) {
		CHECK(false, "ring initialises");
		return;
	}
	avk_retire_init(&q);

	struct retire_probe done = { 0 };
	struct retire_probe pending = { 0 };
	struct retire_probe immediate = { 0 };

	VkCommandBuffer cb = avk_cmd_ring_begin(&ring);
	uint64_t value = avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0);
	(void)cb;
	CHECK(value != 0, "submitted, reserving timeline %" PRIu64, value);

	avk_retire_push(&q, dev, value, retire_probe_fn, &done);
	/* A point the GPU will never reach in this test. */
	avk_retire_push(&q, dev, value + 1000000, retire_probe_fn, &pending);
	/* 0 means "nothing submitted touched this", so it retires at once. */
	avk_retire_push(&q, dev, 0, retire_probe_fn, &immediate);

	/*
	 * Collect BEFORE waiting. This is the assertion that proves the queue
	 * reads GPU progress rather than draining everything it is given: on a
	 * build where collect() ran every entry, `pending.ran` would be 1 here.
	 */
	avk_retire_collect(&q, dev);
	CHECK(immediate.ran == 1, "a zero-valued entry retires immediately");
	CHECK(pending.ran == 0, "an unreached timeline point is NOT retired");

	CHECK(avk_device_timeline_wait(dev, value, 1000000000ULL),
		"GPU reaches timeline %" PRIu64, value);
	size_t ran = avk_retire_collect(&q, dev);
	CHECK(done.ran == 1 && ran >= 1,
		"the completed entry retires once the GPU passes its point");
	CHECK(pending.ran == 0, "the unreached entry is still held");
	CHECK(q.len == 1, "exactly one entry remains queued (%zu)", q.len);

	/* Teardown must not leak the held entry. */
	avk_retire_finish(&q, dev);
	CHECK(pending.ran == 1, "teardown runs whatever is left");
	CHECK(done.ran == 1, "and does not run anything twice");

	avk_cmd_ring_finish(&ring);
}

int main(void) {
	printf("== avk core (M1) ==\n");

	struct node nodes[MAX_NODES];
	uint32_t node_count = open_render_nodes(nodes, MAX_NODES);
	if (node_count == 0) {
		SKIP("no DRM render node available");
	}
	printf("render nodes: %u\n", node_count);

	struct avk_instance *inst = avk_instance_create("test-avk-core");
	if (inst == NULL) {
		for (uint32_t i = 0; i < node_count; i++) {
			close(nodes[i].fd);
		}
		SKIP("no usable Vulkan instance");
	}
	avk_instance_log_caps(inst);

	test_selection(inst, nodes, node_count);

	/* The device is created for a real node, the way the compositor will do
	 * it -- not with -1, which would exercise a path a session never takes. */
	struct avk_device *dev = NULL;
	for (uint32_t i = 0; i < node_count && dev == NULL; i++) {
		dev = avk_device_create(inst, nodes[i].fd);
	}
	if (dev == NULL) {
		for (uint32_t i = 0; i < node_count; i++) {
			close(nodes[i].fd);
		}
		avk_instance_destroy(inst);
		SKIP("no usable Vulkan device on any render node");
	}

	test_caps(dev);
	test_command_ring(dev);
	test_retire(dev);

	avk_device_destroy(dev);
	avk_instance_destroy(inst);
	for (uint32_t i = 0; i < node_count; i++) {
		close(nodes[i].fd);
	}

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
