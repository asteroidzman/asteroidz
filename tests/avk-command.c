#include <assert.h>
#include <stdio.h>
#include "render/vulkan/command/avk_command.h"

/* No device required: exercise the real ring against a queue that stays
 * pending until the ring explicitly waits. Handles identify allocated slots. */
static uint32_t pools, resets, waits;
static uint64_t reached;
void avk_log(enum avk_log_level level, const char *fmt, ...) {}
bool avk_check(VkResult result, const char *msg) { return result == VK_SUCCESS; }
void avk_device_name_object(struct avk_device *dev, VkObjectType type,
		uint64_t handle, const char *fmt, ...) {}
uint64_t avk_device_timeline_value(struct avk_device *dev) { return reached; }
uint64_t avk_device_timeline_reserve(struct avk_device *dev) { return dev->timeline_next++; }
bool avk_device_timeline_wait(struct avk_device *dev, uint64_t value, uint64_t timeout) {
	waits++;
	reached = value;
	return true;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice dev,
		const VkCommandPoolCreateInfo *info, const VkAllocationCallbacks *alloc,
		VkCommandPool *pool) {
	*pool = (VkCommandPool)(uintptr_t)++pools;
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice dev, VkCommandPool pool,
		const VkAllocationCallbacks *alloc) {}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice dev,
		const VkCommandBufferAllocateInfo *info, VkCommandBuffer *cb) {
	*cb = (VkCommandBuffer)(uintptr_t)info->commandPool;
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice dev, VkCommandPool pool,
		VkCommandPoolResetFlags flags) {
	assert((uintptr_t)pool > 0 && (uintptr_t)pool <= pools);
	resets++;
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cb,
		const VkCommandBufferBeginInfo *info) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cb) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t count,
		const VkSubmitInfo2 *info, VkFence fence) { return VK_SUCCESS; }

int main(void) {
	const uint32_t depths[] = {1, 2, AVK_FRAMES_IN_FLIGHT, AVK_CMD_RING_MAX_SLOTS};
	for (unsigned d = 0; d < sizeof(depths) / sizeof(depths[0]); d++) {
		pools = resets = waits = 0;
		reached = 0;
		struct avk_device dev = {.timeline_next = 1};
		struct avk_cmd_ring ring;
		assert(avk_cmd_ring_init(&ring, &dev, "test", depths[d]));
		for (uint32_t i = 0; i < depths[d]; i++) {
			assert(avk_cmd_ring_begin(&ring) == (VkCommandBuffer)(uintptr_t)(i + 1));
			assert(avk_cmd_ring_submit(&ring, NULL, 0, NULL, 0) == i + 1);
			assert(waits == 0);
		}
		assert(ring.next == 0);
		assert(avk_cmd_ring_begin(&ring) != VK_NULL_HANDLE);
		assert(waits == 1 && reached == 1);
		avk_cmd_ring_abandon(&ring);
		assert(ring.next == 0 && ring.recording == -1);
		assert(avk_cmd_ring_begin(&ring) != VK_NULL_HANDLE);
		assert(waits == 1);
		avk_cmd_ring_abandon(&ring);
		avk_cmd_ring_finish(&ring);
	}
	puts("Command rings: all configured slots used before backpressure, abandon safe");
	return 0;
}
