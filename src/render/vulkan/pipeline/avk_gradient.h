#ifndef AVK_GRADIENT_H
#define AVK_GRADIENT_H

#include "../command/avk_command.h"
#include "../command/avk_retire.h"
#include "../scene/avk_scene.h"

/*
 * Where a frame's gradients live on the GPU.
 *
 * THE PROBLEM THIS SOLVES. A gradient has an arbitrary number of colour stops,
 * and the GLES reference deals with that by holding them in a uniform array
 * whose length is a #define -- and RELINKING the shader program whenever a
 * scene asks for more (link_quad_grad_program(..., count + 1), fx_pass.c:997).
 * Translated to Vulkan that is a pipeline per stop count, compiled on the frame
 * path, the first time a five-stop gradient appears. A storage buffer has no
 * length in the shader at all, so AVK has ONE gradient pipeline for every
 * gradient that will ever be drawn.
 *
 * THE LAYOUT. One flat array of vec4, holding records and colours together:
 *
 *     data[r + 0] = origin.x, origin.y, degree in RADIANS, blend
 *     data[r + 1] = type, colour offset, colour count, --
 *     data[offset .. offset + count - 1] = colours, premultiplied
 *
 * A draw carries only `r`, in push constants. One array rather than a record
 * buffer plus a colour buffer because there is then one capacity, one growth
 * path and one upload -- and because a gradient's colours end up written next
 * to the record describing them.
 *
 * LIFETIME, AND WHY THERE IS NO WAIT. There is one buffer per frame in flight,
 * indexed by the command ring's slot. avk_cmd_ring_begin() has already waited
 * for that slot's previous submission to complete before this is written, so
 * overwriting it is safe by construction, with no fence, no vkDeviceWaitIdle
 * and no CPU wait of any kind added to the frame path. That is also what makes
 * updating the slot's DESCRIPTOR legal on growth: the command buffer that used
 * the old one has retired.
 *
 * GROWTH. Geometric, and the old buffer is handed to the retire queue against
 * the timeline point of the submission that last read it -- never destroyed in
 * place. That is the M3.5 staging lesson applied to a second resource: the
 * buffer being replaced is very often the one two frames ahead of the GPU are
 * still reading.
 */

struct avk_gradient_stats {
	uint64_t draws;
	uint64_t linear_draws;
	uint64_t conic_draws;
	uint64_t colors_processed;

	uint64_t buffer_uploads;       /* frames that wrote anything */
	uint64_t buffer_upload_bytes;
	uint64_t buffer_reuses;        /* uploads that fitted the existing buffer */
	uint64_t buffer_grows;         /* new VkBuffer/VkDeviceMemory allocations */
};

struct avk_gradient_slot {
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *mapped;                  /* persistently mapped, like staging */
	VkDeviceSize size;             /* bytes actually allocated */
	uint32_t capacity;             /* vec4 slots the shader may index */
	VkDescriptorSet set;
	/* The timeline point of the last submission that READ this slot, and
	 * therefore what its buffer must be retired against if it is replaced. */
	uint64_t last_use;
};

struct avk_gradient_store {
	struct avk_device *dev;
	/* Borrowed from avk_pipelines, which owns every layout in the renderer so
	 * that the pipeline layout and the sets bound into it are described in one
	 * place. */
	VkDescriptorSetLayout set_layout;
	struct avk_retire_queue *retire;

	VkDescriptorPool pool;
	struct avk_gradient_slot slots[AVK_FRAMES_IN_FLIGHT];

	/* Where the current frame is writing: the slot, and how far into it. */
	int32_t writing;
	uint32_t cursor;

	struct avk_gradient_stats stats;

	/* AZ_GRADIENT_COLOR_OFFSET=1 -- see avk_gradient.c. */
	bool break_color_offset;
};

bool avk_gradient_store_init(struct avk_gradient_store *store,
	struct avk_device *dev, VkDescriptorSetLayout set_layout,
	struct avk_retire_queue *retire);
void avk_gradient_store_finish(struct avk_gradient_store *store);

/*
 * Open `slot` for writing and make sure it can hold `vec4s`.
 *
 * Returns the descriptor set to bind, or VK_NULL_HANDLE if the buffer could not
 * be made big enough -- in which case the caller must draw the frame without
 * gradients rather than with a wrong one.
 *
 * `vec4s` is the whole frame's demand, computed before anything is recorded, so
 * the buffer can never grow with draws already referring to it.
 */
VkDescriptorSet avk_gradient_store_begin(struct avk_gradient_store *store,
	uint32_t slot, uint32_t vec4s);

/*
 * Append one gradient's record and colours, and return the record's index in
 * vec4 units -- the number the draw puts in its push constants.
 *
 * Returns UINT32_MAX if it does not fit, which cannot happen after a
 * correctly-sized begin() and is checked anyway: a wrong index is an
 * out-of-bounds read in a shader, and those are silent.
 */
uint32_t avk_gradient_store_push(struct avk_gradient_store *store,
	const struct avk_gradient *gradient, const float *colors);

/* Record the timeline point of the submission that read this frame's slot. */
void avk_gradient_store_submitted(struct avk_gradient_store *store,
	uint64_t timeline_value);

#endif /* AVK_GRADIENT_H */
