#define _POSIX_C_SOURCE 200809L

#include "avk_gradient.h"

#include <stdlib.h>
#include <string.h>

#include "../image/avk_upload.h"

/* vec4s in a freshly created slot. An ordinary desktop frame draws a focused
 * window's two-stop border and, in the overview, two five-stop vignettes: three
 * records and twelve colours, eighteen vec4s. 128 is 2 KiB and never grows in
 * normal use, which is what makes gradient_buffer_grows a meaningful counter
 * rather than a number that ticks up every time a window is focused. */
#define AVK_GRADIENT_INITIAL_VEC4S 128

/* Two vec4s of record, then the colours. */
#define AVK_GRADIENT_RECORD_VEC4S 2

struct avk_gradient_retire {
	VkBuffer buffer;
	VkDeviceMemory memory;
};

static void gradient_buffer_retire(struct avk_device *dev, void *data) {
	struct avk_gradient_retire *old = data;
	if (old->buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(dev->dev, old->buffer, NULL);
		AVK_LIVE_DEC(dev, buffers);
	}
	if (old->memory != VK_NULL_HANDLE) {
		/* Unmapping is implicit in freeing the memory it was mapped from. */
		vkFreeMemory(dev->dev, old->memory, NULL);
		AVK_LIVE_DEC(dev, device_memory);
	}
	free(old);
}

static void slot_destroy_in_place(struct avk_device *dev,
		struct avk_gradient_slot *slot) {
	if (slot->buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(dev->dev, slot->buffer, NULL);
		AVK_LIVE_DEC(dev, buffers);
	}
	if (slot->memory != VK_NULL_HANDLE) {
		vkFreeMemory(dev->dev, slot->memory, NULL);
		AVK_LIVE_DEC(dev, device_memory);
	}
	slot->buffer = VK_NULL_HANDLE;
	slot->memory = VK_NULL_HANDLE;
	slot->mapped = NULL;
	slot->size = 0;
	slot->capacity = 0;
}

/*
 * Make `slot` hold at least `vec4s`, replacing its buffer if it does not.
 *
 * The old buffer goes to the retire queue against slot->last_use, NOT destroyed
 * here. Two frames may still be in flight reading it, and "the slot I am about
 * to write has retired" says nothing about the buffer this slot held three
 * frames ago if it was never replaced since. Destroying in place is the mistake
 * staging_ensure() had to be taught not to make, and this is the same shape.
 */
static bool slot_ensure(struct avk_gradient_store *store,
		struct avk_gradient_slot *slot, uint32_t vec4s) {
	if (slot->capacity >= vec4s && slot->mapped != NULL) {
		store->stats.buffer_reuses++;
		return true;
	}

	struct avk_device *dev = store->dev;
	uint32_t want = slot->capacity == 0 ? AVK_GRADIENT_INITIAL_VEC4S
		: slot->capacity;
	while (want < vec4s) {
		want *= 2;
	}

	if (slot->buffer != VK_NULL_HANDLE) {
		struct avk_gradient_retire *old = calloc(1, sizeof(*old));
		if (old != NULL) {
			old->buffer = slot->buffer;
			old->memory = slot->memory;
			slot->buffer = VK_NULL_HANDLE;
			slot->memory = VK_NULL_HANDLE;
			slot->mapped = NULL;
			avk_retire_push(store->retire, dev, slot->last_use,
				gradient_buffer_retire, old);
		} else {
			avk_log(AVK_ERROR, "gradient: cannot defer the old buffer's "
				"destruction; destroying it in place, which may race a frame "
				"the GPU has not run yet");
			slot_destroy_in_place(dev, slot);
		}
	}

	VkDeviceSize size = (VkDeviceSize)want * 4 * sizeof(float);
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (!avk_check(vkCreateBuffer(dev->dev, &info, NULL, &slot->buffer),
			"vkCreateBuffer (gradient)")) {
		slot->buffer = VK_NULL_HANDLE;
		return false;
	}
	AVK_LIVE_INC(dev, buffers);

	VkMemoryRequirements reqs;
	vkGetBufferMemoryRequirements(dev->dev, slot->buffer, &reqs);
	uint32_t type = 0;
	/* HOST_VISIBLE and COHERENT: the frame path writes this with a memcpy while
	 * recording and must not have to flush a range afterwards. It is read once
	 * per fragment and is a few kilobytes, so device-local staging would cost a
	 * copy and a barrier to save nothing measurable. */
	if (!avk_find_memory_type(dev, reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
			| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
		avk_log(AVK_ERROR, "gradient: no host-visible coherent memory type");
		goto error;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (!avk_check(vkAllocateMemory(dev->dev, &alloc, NULL, &slot->memory),
			"vkAllocateMemory (gradient)")) {
		slot->memory = VK_NULL_HANDLE;
		goto error;
	}
	AVK_LIVE_INC(dev, device_memory);
	if (!avk_check(vkBindBufferMemory(dev->dev, slot->buffer, slot->memory, 0),
			"vkBindBufferMemory (gradient)")) {
		goto error;
	}
	if (!avk_check(vkMapMemory(dev->dev, slot->memory, 0, reqs.size, 0,
			&slot->mapped), "vkMapMemory (gradient)")) {
		slot->mapped = NULL;
		goto error;
	}
	slot->size = size;
	slot->capacity = want;

	/*
	 * Point the slot's descriptor at the new buffer.
	 *
	 * Legal here and nowhere else: a descriptor set may not be updated while a
	 * command buffer that uses it is pending, and this slot's previous
	 * submission has completed -- the command ring waited for it before this
	 * frame began recording. That is the whole reason there is a set PER SLOT
	 * rather than one shared set that would have to be updated under frames
	 * still in flight.
	 */
	VkDescriptorBufferInfo buffer_info = {
		.buffer = slot->buffer,
		.offset = 0,
		.range = size,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = slot->set,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &buffer_info,
	};
	vkUpdateDescriptorSets(dev->dev, 1, &write, 0, NULL);

	store->stats.buffer_grows++;
	avk_device_name_object(dev, VK_OBJECT_TYPE_BUFFER, (uint64_t)slot->buffer,
		"avk gradient data");
	return true;

error:
	slot_destroy_in_place(dev, slot);
	return false;
}

bool avk_gradient_store_init(struct avk_gradient_store *store,
		struct avk_device *dev, VkDescriptorSetLayout set_layout,
		struct avk_retire_queue *retire) {
	memset(store, 0, sizeof(*store));
	store->dev = dev;
	store->set_layout = set_layout;
	store->retire = retire;
	store->writing = -1;

	VkDescriptorPoolSize size = {
		.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = AVK_FRAMES_IN_FLIGHT,
	};
	VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = AVK_FRAMES_IN_FLIGHT,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (!avk_check(vkCreateDescriptorPool(dev->dev, &pool_info, NULL,
			&store->pool), "vkCreateDescriptorPool (gradient)")) {
		store->pool = VK_NULL_HANDLE;
		return false;
	}
	AVK_LIVE_INC(dev, descriptor_pools);

	/* Every set the store will ever need, allocated once. The pool is sized
	 * exactly, so a descriptor pool never appears on the frame path -- which is
	 * an invariant a test asserts rather than a hope. */
	for (uint32_t i = 0; i < AVK_FRAMES_IN_FLIGHT; i++) {
		VkDescriptorSetAllocateInfo alloc = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = store->pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &store->set_layout,
		};
		if (!avk_check(vkAllocateDescriptorSets(dev->dev, &alloc,
				&store->slots[i].set),
				"vkAllocateDescriptorSets (gradient)")) {
			avk_gradient_store_finish(store);
			return false;
		}
		/*
		 * Give every slot a buffer up front.
		 *
		 * The gradient pipeline's fragment shader STATICALLY USES this storage
		 * buffer, so the set has to be bound with a real buffer behind it
		 * whenever that pipeline draws -- including the first gradient of the
		 * session. Allocating here rather than lazily also means the common
		 * case never allocates on the frame path at all.
		 */
		if (!slot_ensure(store, &store->slots[i],
				AVK_GRADIENT_INITIAL_VEC4S)) {
			avk_gradient_store_finish(store);
			return false;
		}
	}
	/* Those were the initial allocations, not growth. */
	store->stats.buffer_grows = 0;
	store->stats.buffer_reuses = 0;
	return true;
}

void avk_gradient_store_finish(struct avk_gradient_store *store) {
	if (store->dev == NULL) {
		return;
	}
	for (uint32_t i = 0; i < AVK_FRAMES_IN_FLIGHT; i++) {
		slot_destroy_in_place(store->dev, &store->slots[i]);
	}
	if (store->pool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(store->dev->dev, store->pool, NULL);
		AVK_LIVE_DEC(store->dev, descriptor_pools);
	}
	memset(store, 0, sizeof(*store));
	store->writing = -1;
}

VkDescriptorSet avk_gradient_store_begin(struct avk_gradient_store *store,
		uint32_t slot_index, uint32_t vec4s) {
	if (slot_index >= AVK_FRAMES_IN_FLIGHT) {
		return VK_NULL_HANDLE;
	}
	struct avk_gradient_slot *slot = &store->slots[slot_index];
	if (!slot_ensure(store, slot, vec4s)) {
		store->writing = -1;
		return VK_NULL_HANDLE;
	}
	store->writing = (int32_t)slot_index;
	store->cursor = 0;
	return slot->set;
}

uint32_t avk_gradient_store_push(struct avk_gradient_store *store,
		const struct avk_gradient *gradient, const float *colors) {
	if (store->writing < 0 || gradient->color_count == 0 || colors == NULL) {
		return UINT32_MAX;
	}
	struct avk_gradient_slot *slot = &store->slots[store->writing];
	uint32_t need = AVK_GRADIENT_RECORD_VEC4S + gradient->color_count;
	if (store->cursor + need > slot->capacity) {
		/* Cannot happen after a begin() sized from the same scene; if it does,
		 * the frame's demand and the frame's contents disagree, and drawing
		 * with a wrong index would be a silent out-of-bounds read in the
		 * shader. */
		avk_log(AVK_ERROR, "gradient: %u vec4s wanted at cursor %u but the "
			"slot holds %u -- this gradient is not drawn", need, store->cursor,
			slot->capacity);
		return UINT32_MAX;
	}

	float *data = (float *)slot->mapped;
	uint32_t rec = store->cursor;
	uint32_t color_at = rec + AVK_GRADIENT_RECORD_VEC4S;

	float *r0 = data + (size_t)rec * 4;
	r0[0] = gradient->origin[0];
	r0[1] = gradient->origin[1];
	/* DEGREES on the snapshot, radians on the GPU. The conversion happens here,
	 * once per command, rather than once per fragment. */
	r0[2] = gradient->degree * (3.14159265358979323846f / 180.0f);
	bool blend = gradient->blend;
	r0[3] = blend ? 1.0f : 0.0f;

	float *r1 = data + ((size_t)rec + 1) * 4;
	r1[0] = (float)gradient->type;
	r1[1] = (float)color_at;
	r1[2] = (float)gradient->color_count;
	r1[3] = 0.0f;

	memcpy(data + (size_t)color_at * 4, colors,
		(size_t)gradient->color_count * 4 * sizeof(float));

	store->cursor += need;
	store->stats.draws++;
	store->stats.colors_processed += gradient->color_count;
	if (gradient->type == AVK_GRADIENT_CONIC) {
		store->stats.conic_draws++;
	} else {
		store->stats.linear_draws++;
	}
	return rec;
}

void avk_gradient_store_submitted(struct avk_gradient_store *store,
		uint64_t timeline_value) {
	if (store->writing < 0) {
		return;
	}
	struct avk_gradient_slot *slot = &store->slots[store->writing];
	slot->last_use = timeline_value;
	if (store->cursor > 0) {
		store->stats.buffer_uploads++;
		store->stats.buffer_upload_bytes +=
			(uint64_t)store->cursor * 4 * sizeof(float);
	}
	store->writing = -1;
}
