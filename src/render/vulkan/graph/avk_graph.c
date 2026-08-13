#define _POSIX_C_SOURCE 200809L

#include "avk_graph.h"

#include "../debug/avk_debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * THE ONE TABLE.
 *
 * A usage class is an intent -- "this pass samples that image" -- and this is
 * the only place an intent becomes Vulkan synchronisation. Every barrier the
 * graph emits is derived from two rows of it, so a wrong row is wrong
 * everywhere at once and a right row is right everywhere at once. That is the
 * whole argument for having a graph rather than barriers at call sites.
 *
 * COLOR_WRITE carries the READ bit as well as the write. loadOp LOAD is a
 * COLOR_ATTACHMENT_READ of the target, and a barrier whose dstAccessMask covers
 * only the write leaves that load unsynchronised against whatever produced the
 * previous frame's contents -- which synchronization validation reports at
 * vkCmdBeginRendering, some distance from the draw that looks guilty.
 */
static const struct {
	VkPipelineStageFlags2 stage;
	VkAccessFlags2 access;
	VkImageLayout layout;
	const char *name;
	bool is_write;
} az_usage[AVK_USE_COUNT] = {
	[AVK_USE_COLOR_WRITE] = {
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
			| VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, "color-write", true,
	},
	[AVK_USE_SAMPLED_READ] = {
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "sampled-read", false,
	},
	[AVK_USE_TRANSFER_READ] = {
		VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "transfer-read", false,
	},
	[AVK_USE_TRANSFER_WRITE] = {
		VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "transfer-write", true,
	},
};

void avk_graph_usage_state(enum avk_graph_usage usage,
		VkPipelineStageFlags2 *stage, VkAccessFlags2 *access,
		VkImageLayout *layout) {
	if (usage >= AVK_USE_COUNT) {
		usage = AVK_USE_COLOR_WRITE;
	}
	if (stage != NULL) {
		*stage = az_usage[usage].stage;
	}
	if (access != NULL) {
		*access = az_usage[usage].access;
	}
	if (layout != NULL) {
		*layout = az_usage[usage].layout;
	}
}

const char *avk_graph_usage_name(enum avk_graph_usage usage) {
	return usage < AVK_USE_COUNT ? az_usage[usage].name : "?";
}

/*
 * The layout a use wants, which is the table's -- except for a FOREIGN COLOUR
 * ATTACHMENT, which stays in GENERAL.
 *
 * The exception is narrow and both halves of it are deliberate.
 *
 * A foreign colour attachment is a scan-out buffer: KMS is its other owner, it
 * has to be handed back in a layout the display engine can read, and GENERAL is
 * that layout. Entering COLOR_ATTACHMENT_OPTIMAL would mean transitioning in and
 * straight back out again for no gain, and dynamic rendering accepts GENERAL as
 * an attachment perfectly happily.
 *
 * A foreign SAMPLED image is a client's surface, and it does go to
 * SHADER_READ_ONLY_OPTIMAL. The two transitions are worth it there because the
 * read covers the whole surface and happens every frame the window is visible,
 * whereas an attachment is written through the render pass machinery either way.
 * This is also exactly what the pre-graph renderer did, so the direct path's
 * barriers are unchanged in kind and in count.
 */
static VkImageLayout az_layout_for(const struct avk_graph_resource *res,
		enum avk_graph_usage usage) {
	if (res->foreign && usage == AVK_USE_COLOR_WRITE) {
		return VK_IMAGE_LAYOUT_GENERAL;
	}
	return az_usage[usage].layout;
}

/* ── storage ────────────────────────────────────────────────────────────────
 *
 * Three flat arrays, grown geometrically and never shrunk. avk_graph_reset()
 * sets the lengths to zero and keeps the capacity, so a stable scene stops
 * allocating entirely after the first few frames -- which is the requirement,
 * and `stats.allocs` is what makes it checkable rather than asserted.
 */
#define AZ_GROW(graph, arr, cap, want, type) \
	az_grow(graph, (void **)&(graph)->arr, &(graph)->cap, want, sizeof(type))

static bool az_grow(struct avk_graph *graph, void **arr, uint32_t *cap,
		uint32_t want, size_t elem) {
	if (*cap >= want) {
		return true;
	}
	uint32_t next = *cap == 0 ? 8 : *cap;
	while (next < want) {
		next *= 2;
	}
	void *grown = realloc(*arr, (size_t)next * elem);
	if (grown == NULL) {
		return false;
	}
	*arr = grown;
	*cap = next;
	graph->stats.allocs++;
	return true;
}

void avk_graph_init(struct avk_graph *graph, struct avk_device *dev) {
	memset(graph, 0, sizeof(*graph));
	graph->dev = dev;
	/*
	 * M4E.4 break, read once. Removes the write->read dependency between two
	 * passes -- the second samples an image the first may still be writing.
	 * Deliberately not a switch that removes barriers wholesale: the interesting
	 * failure is the one specific missing edge, because that is the edge a
	 * hand-written multipass effect forgets.
	 */
	graph->break_missing_write_read =
		getenv("AZ_GRAPH_NO_WRITE_READ") != NULL;
	if (graph->break_missing_write_read) {
		avk_log(AVK_ERROR, "M4E break switch active: the graph is NOT emitting "
			"write->read barriers between passes");
	}
}

void avk_graph_finish(struct avk_graph *graph) {
	free(graph->resources);
	free(graph->passes);
	free(graph->uses);
	free(graph->barriers);
	free(graph->log);
	memset(graph, 0, sizeof(*graph));
}

void avk_graph_reset(struct avk_graph *graph) {
	graph->res_len = 0;
	graph->pass_len = 0;
	graph->use_len = 0;
	graph->log_len = 0;
	uint64_t allocs = graph->stats.allocs;
	uint64_t build_ns = graph->stats.build_ns;
	uint64_t frames = graph->stats.frames;
	uint32_t hist[64];
	memcpy(hist, graph->stats.build_hist, sizeof(hist));
	memset(&graph->stats, 0, sizeof(graph->stats));
	graph->stats.allocs = allocs;
	graph->stats.build_ns = build_ns;
	graph->stats.frames = frames;
	memcpy(graph->stats.build_hist, hist, sizeof(hist));
}

/* ── declaration ──────────────────────────────────────────────────────────── */

uint32_t avk_graph_add_image(struct avk_graph *graph, struct avk_image *image,
		bool foreign, enum avk_graph_exit exit) {
	if (image == NULL) {
		return AVK_GRAPH_INVALID;
	}
	/*
	 * Linear scan, and it stays linear on purpose. A frame declares one target
	 * plus one resource per distinct sampled surface -- tens, not thousands --
	 * and a hash table would cost more to maintain across a reset than the scan
	 * costs to run. If a profile ever says otherwise it will say so with a
	 * number.
	 */
	for (uint32_t i = 0; i < graph->res_len; i++) {
		if (graph->resources[i].image == image) {
			return i;
		}
	}
	if (!AZ_GROW(graph, resources, res_cap, graph->res_len + 1,
			struct avk_graph_resource)) {
		return AVK_GRAPH_INVALID;
	}
	struct avk_graph_resource *res = &graph->resources[graph->res_len];
	memset(res, 0, sizeof(*res));
	res->image = image;
	res->foreign = foreign;
	res->exit = exit;
	/*
	 * The entry state is READ FROM THE IMAGE and not assumed. avk_image.layout
	 * is the cross-frame source of truth -- it is what the previous frame's
	 * exit barrier wrote -- and the graph tracks only within-frame state on top
	 * of it. A graph that guessed COLOR_ATTACHMENT_OPTIMAL here would be right
	 * about an image it had just rendered into and wrong about every one it had
	 * not.
	 */
	res->entry_layout = image->layout;
	res->layout = image->layout;
	/*
	 * ALL_COMMANDS / MEMORY_WRITE as the incoming source scope. The previous
	 * writer might have been an upload on the other command ring, a previous
	 * frame, or a client's own submission reaching us through an imported
	 * fence, and this layer genuinely does not know which. Narrowing it would be
	 * a guess about a producer outside the graph.
	 */
	res->last_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	res->last_access = VK_ACCESS_2_MEMORY_WRITE_BIT;
	res->read_only_since_write = false;
	res->read_stages = VK_PIPELINE_STAGE_2_NONE;
	res->read_access = 0;
	res->last_usage = -1;
	return graph->res_len++;
}

bool avk_graph_pass_begin(struct avk_graph *graph, const char *label,
		avk_graph_record_fn record, void *user) {
	if (!AZ_GROW(graph, passes, pass_cap, graph->pass_len + 1,
			struct avk_graph_pass)) {
		return false;
	}
	struct avk_graph_pass *pass = &graph->passes[graph->pass_len];
	memset(pass, 0, sizeof(*pass));
	pass->label = label;
	pass->first_use = graph->use_len;
	pass->record = record;
	pass->user = user;
	graph->pass_len++;
	return true;
}

bool avk_graph_use(struct avk_graph *graph, uint32_t resource,
		enum avk_graph_usage usage, const struct avk_box *region) {
	if (graph->pass_len == 0 || resource >= graph->res_len ||
			usage >= AVK_USE_COUNT) {
		return false;
	}
	if (!AZ_GROW(graph, uses, use_cap, graph->use_len + 1,
			struct avk_graph_use)) {
		return false;
	}
	struct avk_graph_use *use = &graph->uses[graph->use_len++];
	use->resource = resource;
	use->usage = usage;
	use->region = region != NULL ? *region : (struct avk_box){0, 0, 0, 0};
	graph->passes[graph->pass_len - 1].use_count++;
	return true;
}

void avk_graph_pass_time_move_end(struct avk_graph *graph,
		enum avk_ts_mark end) {
	if (graph->pass_len == 0 || end >= AVK_TS_MARKS) {
		return;
	}
	for (uint32_t p = 0; p < graph->pass_len; p++) {
		if (graph->passes[p].timed && graph->passes[p].mark_end == end) {
			graph->passes[p].mark_end = AVK_TS_NONE;
			if (graph->passes[p].mark_begin == AVK_TS_NONE) {
				graph->passes[p].timed = false;
			}
		}
	}
	struct avk_graph_pass *pass = &graph->passes[graph->pass_len - 1];
	/* A pass that was not timed has no begin mark of its own; give it the
	 * sentinel rather than leaving the zero-initialised FRAME_BEGIN, which
	 * would write that query a second time. */
	if (!pass->timed) {
		pass->mark_begin = AVK_TS_NONE;
	}
	pass->timed = true;
	pass->mark_end = end;
}

void avk_graph_pass_time(struct avk_graph *graph, enum avk_ts_mark begin,
		enum avk_ts_mark end) {
	if (graph->pass_len == 0) {
		return;
	}
	struct avk_graph_pass *pass = &graph->passes[graph->pass_len - 1];
	pass->timed = true;
	pass->mark_begin = begin;
	pass->mark_end = end;
}

void avk_graph_pass_end(struct avk_graph *graph) {
	/* Nothing to close: a pass owns the uses between its first_use and the next
	 * pass's. The call exists so the declaration reads as a block and so a
	 * future invariant (a pass that declares nothing, say) has somewhere to
	 * live. */
	(void)graph;
}

/* ── compilation ──────────────────────────────────────────────────────────── */

static void az_log_barrier(struct avk_graph *graph, uint32_t pass,
		uint32_t resource, const VkImageMemoryBarrier2 *b, const char *from,
		const char *to) {
	if (!AZ_GROW(graph, log, log_cap, graph->log_len + 1,
			struct avk_graph_barrier_log)) {
		return;
	}
	graph->log[graph->log_len++] = (struct avk_graph_barrier_log){
		.pass = pass,
		.resource = resource,
		.old_layout = b->oldLayout,
		.new_layout = b->newLayout,
		.src_stage = b->srcStageMask,
		.dst_stage = b->dstStageMask,
		.src_access = b->srcAccessMask,
		.dst_access = b->dstAccessMask,
		.from = from,
		.to = to,
	};
}

/*
 * The barrier one use requires, or false if it requires none.
 *
 * THREE CASES, and the middle one is the requirement that keeps the direct path
 * cheap:
 *
 *   WRITE after anything    always needs a barrier -- the hazard is real
 *   READ after READ         needs NOTHING when the layout already matches.
 *                           Two passes sampling one image are not ordered with
 *                           respect to each other and do not need to be; the
 *                           barrier that made them visible was emitted before
 *                           the first of them.
 *   READ after WRITE        needs a barrier, and this is the edge M4F cannot
 *                           do without: sampling an image a previous pass wrote.
 */
static bool az_barrier_for(struct avk_graph *graph,
		struct avk_graph_resource *res, enum avk_graph_usage usage,
		VkImageMemoryBarrier2 *out, const char **from_name) {
	VkImageLayout want = az_layout_for(res, usage);
	bool is_write = az_usage[usage].is_write;

	if (!is_write && res->read_only_since_write && res->layout == want) {
		/* Read after read, same layout: nothing to order. Counted nowhere
		 * because a barrier that is not emitted is not an event -- what proves
		 * this branch is taken is the barrier count NOT rising, which is what
		 * the test asserts. */
		res->read_stages |= az_usage[usage].stage;
		res->read_access |= az_usage[usage].access;
		return false;
	}

	/*
	 * M4E.4 break: remove the DEPENDENCY, keep the TRANSITION.
	 *
	 * The bug being modelled is a forgotten edge between two passes -- pass B
	 * samples an image pass A is still writing -- and that is a synchronisation
	 * mistake, not a layout mistake. So the barrier is still emitted and the
	 * image still reaches SHADER_READ_ONLY_OPTIMAL, which keeps the read legal;
	 * what goes is the source scope naming the write it must wait for.
	 *
	 * The first version of this break SKIPPED the barrier when the layouts
	 * already matched, which for a write->read pair they never do -- so it was
	 * inert against the one assertion it exists for, and scored a full pass on
	 * the case it was supposed to falsify. A break that cannot fail its own
	 * test is decoration.
	 *
	 * Narrow on purpose: `last_usage >= 0` means a pass in THIS graph wrote it,
	 * so a per-frame acquire of something that arrived from outside is
	 * untouched.
	 */
	bool drop_dependency = graph->break_missing_write_read && !is_write
		&& !res->read_only_since_write && res->last_usage >= 0;

	VkPipelineStageFlags2 src_stage;
	VkAccessFlags2 src_access;
	if (is_write && res->read_only_since_write) {
		/*
		 * Write after read. The source scope is every read since the last
		 * write, accumulated -- not the last write, which has already been made
		 * visible. A write-after-read hazard is an EXECUTION dependency: the
		 * reads must finish before the write starts, and no cache flush is
		 * required of a reader, so srcAccessMask stays 0.
		 */
		src_stage = res->read_stages;
		src_access = 0;
	} else {
		src_stage = res->last_stage;
		src_access = res->last_access;
	}
	if (drop_dependency) {
		src_stage = VK_PIPELINE_STAGE_2_NONE;
		src_access = 0;
	}

	*out = (VkImageMemoryBarrier2){
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = src_stage,
		.srcAccessMask = src_access,
		.dstStageMask = az_usage[usage].stage,
		.dstAccessMask = az_usage[usage].access,
		.oldLayout = res->layout,
		.newLayout = want,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = res->image->image,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	/*
	 * The FIRST use of a foreign resource in a frame is an ACQUIRE from its real
	 * owner. Without it a client's dma-buf is read in a state Vulkan was never
	 * told about; the matching release at frame end is what makes the scan-out
	 * buffer legible to KMS, and shipping without it once turned the whole
	 * display flat white with every window rendered correctly inside it.
	 */
	if (res->foreign && res->last_usage < 0) {
		out->srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		out->dstQueueFamilyIndex = graph->dev->caps.graphics_family;
	}
	*from_name = res->last_usage < 0 ? "external"
		: avk_graph_usage_name((enum avk_graph_usage)res->last_usage);

	res->last_usage = (int)usage;
	res->layout = want;
	res->last_stage = az_usage[usage].stage;
	res->last_access = az_usage[usage].access;
	res->read_only_since_write = !is_write;
	res->read_stages = is_write ? VK_PIPELINE_STAGE_2_NONE
		: az_usage[usage].stage;
	res->read_access = is_write ? 0 : az_usage[usage].access;
	return true;
}

static void az_flush(struct avk_graph *graph, VkCommandBuffer cb,
		uint32_t count) {
	if (count == 0) {
		return;
	}
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = count,
		.pImageMemoryBarriers = graph->barriers,
	};
	/*
	 * NOT individually timed. A bracketing clock_gettime pair costs ~37 ns
	 * around a call that costs ~60, so the instrument perturbs the measurement
	 * by more than half and, on a preemptible thread, its mean measures the
	 * scheduler. tests/test-avk-barrier-cost.c measures this properly, in a
	 * tight loop, one variable at a time.
	 */
	vkCmdPipelineBarrier2(cb, &dep);
	graph->stats.barriers++;
	for (uint32_t i = 0; i < count; i++) {
		if (graph->barriers[i].oldLayout != graph->barriers[i].newLayout) {
			graph->stats.image_transitions++;
		} else {
			graph->stats.memory_barriers++;
		}
	}
}

void avk_graph_execute(struct avk_graph *graph, VkCommandBuffer cb,
		struct avk_timestamps *ts, uint32_t ts_slot) {
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	uint64_t record_ns = 0;

	/* Worst case for one flush: every resource transitioned at once. Sized
	 * once, here, rather than per pass. */
	if (!AZ_GROW(graph, barriers, barrier_cap,
			graph->res_len > 0 ? graph->res_len : 1, VkImageMemoryBarrier2)) {
		avk_log(AVK_ERROR, "avk graph: no room for %u barriers", graph->res_len);
		return;
	}

	for (uint32_t p = 0; p < graph->pass_len; p++) {
		struct avk_graph_pass *pass = &graph->passes[p];
		uint32_t count = 0;
		for (uint32_t u = pass->first_use; u < pass->first_use + pass->use_count;
				u++) {
			struct avk_graph_use *use = &graph->uses[u];
			struct avk_graph_resource *res = &graph->resources[use->resource];
			const char *from = "?";
			if (az_barrier_for(graph, res, use->usage,
					&graph->barriers[count], &from)) {
				az_log_barrier(graph, p, use->resource,
					&graph->barriers[count], from,
					avk_graph_usage_name(use->usage));
				count++;
			}
		}
		/*
		 * ALL of a pass's transitions in ONE call, and BEFORE it -- not on
		 * demand as each resource comes up. Inside a dynamic-rendering instance
		 * vkCmdPipelineBarrier2 may carry only memory barriers and a layout
		 * transition is forbidden outright
		 * (VUID-vkCmdPipelineBarrier2-oldLayout-01181), so anything a pass
		 * samples has to have reached its layout before the pass begins. That
		 * constraint is what makes "barriers belong to the pass boundary" a
		 * requirement rather than a tidiness preference.
		 */
		az_flush(graph, cb, count);

		if (pass->label != NULL) {
			avk_debug_label_begin(graph->dev, cb, "%s", pass->label);
		}
		if (pass->timed && ts != NULL) {
			avk_timestamps_mark(ts, cb, ts_slot, pass->mark_begin,
				VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
		}
		if (pass->record != NULL) {
			/* Bracketed and subtracted: the callback is the frame's own draw
			 * work, not the graph's cost of ordering it. */
			struct timespec r0, r1;
			clock_gettime(CLOCK_MONOTONIC, &r0);
			pass->record(cb, pass->user);
			clock_gettime(CLOCK_MONOTONIC, &r1);
			record_ns += (uint64_t)(r1.tv_sec - r0.tv_sec) * 1000000000ULL
				+ (uint64_t)(r1.tv_nsec - r0.tv_nsec);
		}
		if (pass->timed && ts != NULL) {
			avk_timestamps_mark(ts, cb, ts_slot, pass->mark_end,
				VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
		}
		if (pass->label != NULL) {
			avk_debug_label_end(graph->dev, cb);
		}
	}

	/*
	 * Exit. Every foreign resource goes back to VK_QUEUE_FAMILY_FOREIGN_EXT in
	 * GENERAL, which is what makes the frame visible; anything AVK owns is left
	 * where the last pass put it. The image's own layout field is then updated
	 * so the NEXT frame's entry state is the truth rather than an assumption.
	 */
	uint32_t count = 0;
	for (uint32_t i = 0; i < graph->res_len; i++) {
		struct avk_graph_resource *res = &graph->resources[i];
		if (res->exit != AVK_EXIT_FOREIGN) {
			res->image->layout = res->layout;
			continue;
		}
		graph->barriers[count] = (VkImageMemoryBarrier2){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = res->last_stage,
			.srcAccessMask = res->last_access,
			/* NONE, not BOTTOM_OF_PIPE: the second scope of a queue-family
			 * release is ignored, and synchronization2 wants that said. */
			.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
			.dstAccessMask = 0,
			.oldLayout = res->layout,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = graph->dev->caps.graphics_family,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
			.image = res->image->image,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
		};
		az_log_barrier(graph, graph->pass_len, i, &graph->barriers[count],
			res->last_usage < 0 ? "external"
				: avk_graph_usage_name((enum avk_graph_usage)res->last_usage),
			"exit");
		count++;
		/* The image ends the frame in the foreign owner's hands, so that is
		 * what the next frame must acquire from. */
		res->image->layout = VK_IMAGE_LAYOUT_GENERAL;
	}
	az_flush(graph, cb, count);

	graph->stats.passes = graph->pass_len;
	graph->stats.resources = graph->res_len;
	graph->stats.uses = graph->use_len;
	graph->stats.frames++;

	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	uint64_t total = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL
		+ (uint64_t)(t1.tv_nsec - t0.tv_nsec);
	uint64_t ours = total > record_ns ? total - record_ns : 0;
	graph->stats.build_ns += ours;
	/* 250 ns buckets, saturating. 64 buckets covers 16 us exactly; anything
	 * beyond that is a preempted frame and belongs in the top bucket rather
	 * than in a mean. */
	uint32_t bucket = (uint32_t)(ours / 250);
	graph->stats.build_hist[bucket < 64 ? bucket : 63]++;
}

/* ── inspection ───────────────────────────────────────────────────────────── */

static size_t az_append(char *out, size_t cap, size_t at, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(at < cap ? out + at : NULL, at < cap ? cap - at : 0,
		fmt, ap);
	va_end(ap);
	return n < 0 ? at : at + (size_t)n;
}

size_t avk_graph_dump(const struct avk_graph *graph, char *out, size_t cap) {
	size_t at = 0;
	uint32_t log_at = 0;
	for (uint32_t p = 0; p <= graph->pass_len; p++) {
		while (log_at < graph->log_len && graph->log[log_at].pass == p) {
			const struct avk_graph_barrier_log *b = &graph->log[log_at++];
			at = az_append(out, cap, at, "BARRIER\n    R%u %s -> %s\n",
				b->resource, b->from, b->to);
		}
		if (p == graph->pass_len) {
			break;
		}
		const struct avk_graph_pass *pass = &graph->passes[p];
		at = az_append(out, cap, at, "PASS %s\n",
			pass->label != NULL ? pass->label : "(unnamed)");
		for (uint32_t u = pass->first_use;
				u < pass->first_use + pass->use_count; u++) {
			const struct avk_graph_use *use = &graph->uses[u];
			at = az_append(out, cap, at, "    %s R%u %s\n",
				az_usage[use->usage].is_write ? "WRITE" : "READ",
				use->resource, avk_graph_usage_name(use->usage));
		}
	}
	if (cap > 0) {
		out[at < cap ? at : cap - 1] = '\0';
	}
	return at;
}
