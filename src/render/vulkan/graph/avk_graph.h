#ifndef AVK_GRAPH_H
#define AVK_GRAPH_H

#include "../avk.h"
#include "../command/avk_timestamp.h"
#include "../image/avk_image.h"

#include <pixman.h>

/*
 * The render graph: passes, the resources they touch, and the barriers that
 * follow from those two facts.
 *
 * WHAT THIS IS FOR, AND WHAT IT IS NOT FOR. M4F needs to render a background
 * into an intermediate image, blur it, and composite the result -- three
 * operations with real dependencies between them, on images that do not exist
 * outside the frame. Doing that with barriers written by hand at each call site
 * is how a renderer acquires the class of bug that works on the machine it was
 * written on: the pixels are right, the synchronisation is not, and nothing
 * says so until a driver reorders something.
 *
 * So this file owns exactly four things: pass ordering, resource usage
 * declarations, the barriers derived from them, and image layout state. It does
 * NOT own window focus, layout decisions, Wayland state, animation, monitor
 * ownership or anything else a compositor knows. Nothing under
 * src/render/vulkan/ may include a wlroots header and
 * tests/check-vulkan-isolation.py fails the build if that changes.
 *
 * IT IS NOT A GENERIC RENDER GRAPH. There is no dependency solver, no automatic
 * pass reordering, no culling of unreachable passes and no aliasing. Passes
 * execute in declaration order, which is the order the compositor already
 * wants, and the graph's job is to make the transitions between them correct
 * and inspectable rather than to be clever about the order.
 *
 * THE COST CONSTRAINT IS THE HARD ONE. A frame with no multipass effects --
 * which is every frame on a desktop today -- must come out the far side as one
 * pass, one barrier call, one rendering instance and zero transient images.
 * That is why the storage below is three flat arrays reset by setting their
 * lengths to zero: after warmup a frame's graph costs no allocation at all.
 * `avk_graph_stats.allocs` is the number that proves it.
 *
 * THE GRAPH IS PER OUTPUT. One graph describes one output's frame. Two outputs
 * sharing a renderer (which they do -- see the M4E.0 audit, the renderer is
 * keyed on VkFormat and not on output) each build their own, and the only thing
 * they share is the device timeline that orders their submissions. There is no
 * cross-output DAG and no cross-output wait.
 */

/*
 * How a pass touches a resource.
 *
 * Deliberately a SHORT list of things the compositor actually does, not a
 * re-export of Vulkan's access-mask taxonomy. Each maps internally to exactly
 * one (stage, access, layout) triple -- see avk_graph_usage_state() -- so a
 * caller states intent and the graph derives synchronisation. Adding a class
 * means adding a row to that one table.
 */
enum avk_graph_usage {
	/* Written as a colour attachment. loadOp LOAD makes this a READ as well as
	 * a write, which is why the access mask below carries both: a barrier that
	 * covered only the write would leave the load unsynchronised against
	 * whatever produced the previous contents, and synchronization validation
	 * reports that at vkCmdBeginRendering rather than at the draw. */
	AVK_USE_COLOR_WRITE = 0,
	/* Sampled in a fragment shader. */
	AVK_USE_SAMPLED_READ,
	/* vkCmdCopy* source and destination. */
	AVK_USE_TRANSFER_READ,
	AVK_USE_TRANSFER_WRITE,
	AVK_USE_COUNT,
};

/*
 * What must be true of a resource when the frame ENDS.
 *
 * External resources enter the graph from outside and leave it to outside, and
 * neither state may be guessed. A scan-out target has to be handed back to KMS
 * in the layout the display engine can read; a client's dma-buf has to go back
 * to its owner. Getting the exit wrong is not a subtle bug -- a scan-out buffer
 * never released to VK_QUEUE_FAMILY_FOREIGN_EXT shows a flat white screen on a
 * compressed AMD modifier, with every window rendered correctly inside an image
 * nothing can interpret.
 */
enum avk_graph_exit {
	/* Leave it wherever the last pass put it. Correct for anything AVK owns. */
	AVK_EXIT_KEEP = 0,
	/* Release to VK_QUEUE_FAMILY_FOREIGN_EXT in VK_IMAGE_LAYOUT_GENERAL. For
	 * every imported dma-buf: client surfaces and the scan-out target alike. */
	AVK_EXIT_FOREIGN,
};

/*
 * A resource, as the graph sees it.
 *
 * `image` is BORROWED and outlives the graph; the graph never allocates, frees
 * or retires anything here. `entry_layout` is read from the image at
 * declaration and `image->layout` is written back at execute, so the image
 * object stays the single cross-frame source of truth it has always been and
 * the graph tracks only within-frame state.
 */
struct avk_graph_resource {
	struct avk_image *image;

	/* Entry state, stated rather than assumed. */
	VkImageLayout entry_layout;
	bool foreign;              /* acquired from / released to FOREIGN */
	enum avk_graph_exit exit;

	/* Within-frame tracking, updated as passes are compiled. */
	VkImageLayout layout;
	VkPipelineStageFlags2 last_stage;
	VkAccessFlags2 last_access;
	/* True while the last thing that happened was a read and nothing has
	 * written since. Read->read then needs execution ordering only where the
	 * layouts differ, which for two reads of the same class they do not -- so
	 * it needs no barrier at all. This is the flag that keeps requirement 10
	 * ("read->read paths do not receive unnecessary write barriers") from being
	 * a claim. */
	bool read_only_since_write;
	/* Reads accumulated since the last write, so one write-after-read barrier
	 * can name every reader instead of one barrier per reader. */
	VkPipelineStageFlags2 read_stages;
	VkAccessFlags2 read_access;
	/* The usage that last touched it, or -1 for "it arrived from outside the
	 * graph". Kept purely so a barrier can be PRINTED as the transition it is
	 * -- "color-write -> sampled-read" rather than "something -> sampled-read"
	 * -- which is what makes the dump readable by inspection. */
	int last_usage;
};

struct avk_graph_use {
	uint32_t resource;
	enum avk_graph_usage usage;
	/*
	 * The region of the resource this use touches, in resource pixels.
	 *
	 * THREE REGIONS, NOT ONE, and they are not collapsed because M4F is the
	 * consumer. A blur's WRITE region is where its output lands; its READ
	 * region is that dilated by the kernel radius; and the RESOURCE EXTENT is
	 * the backing image, which is at least the read region and may be larger
	 * because the pool handed out a bigger one. Every primitive AVK draws today
	 * has all three equal, which is exactly why a design that assumed so would
	 * survive M4E and fail M4F.
	 *
	 * An empty box means "the whole resource" -- the common case, and it costs
	 * nothing to say.
	 */
	struct avk_box region;
};

/* Records this pass's commands. The graph has already emitted the barriers the
 * declared uses require, and will emit whatever the next pass needs. */
typedef void (*avk_graph_record_fn)(VkCommandBuffer cb, void *user);

/*
 * One emitted barrier, kept so the topology can be printed and asserted on.
 *
 * Recorded rather than reconstructed, because the question a test wants to ask
 * is "what did the graph actually emit", and anything that re-derives the
 * answer from the same rules the implementation used cannot fail. Stored in a
 * grown-once array like everything else here, so logging costs no allocation
 * after warmup.
 */
struct avk_graph_barrier_log {
	uint32_t pass;             /* index of the pass it precedes, or pass_len
	                            * for the frame's exit barriers */
	uint32_t resource;
	VkImageLayout old_layout;
	VkImageLayout new_layout;
	VkPipelineStageFlags2 src_stage, dst_stage;
	VkAccessFlags2 src_access, dst_access;
	const char *from;          /* usage name, or "external" / "exit" */
	const char *to;
};

struct avk_graph_pass {
	/* Borrowed, must outlive the frame -- a string literal in practice. Used
	 * for the debug label and for the dump. */
	const char *label;

	uint32_t first_use;
	uint32_t use_count;

	avk_graph_record_fn record;
	void *user;

	/*
	 * Optional GPU timing, off by default.
	 *
	 * NOT every pass, and not automatically. A timestamp pair measures a
	 * contiguous span and the query pool has AVK_TS_MARKS marks per frame slot;
	 * timestamping every small pass would both exhaust that and quietly change
	 * what is being measured. A pass asks for timing when someone is asking a
	 * question about it -- which for M4F means the blur passes and nothing
	 * else.
	 */
	bool timed;
	enum avk_ts_mark mark_begin;
	enum avk_ts_mark mark_end;
};

struct avk_graph_stats {
	/* Per frame, reset by avk_graph_reset(). */
	uint32_t passes;
	uint32_t resources;
	uint32_t uses;
	uint32_t barriers;           /* vkCmdPipelineBarrier2 calls */
	uint32_t image_transitions;  /* VkImageMemoryBarrier2 with a layout change */
	uint32_t memory_barriers;    /* VkImageMemoryBarrier2 without one */
	/*
	 * VkBufferMemoryBarrier2 emitted. STRUCTURALLY ZERO, and reported anyway.
	 *
	 * The graph declares image resources only, because the two buffers AVK owns
	 * are already ordered without one: the gradient storage buffer and the
	 * command buffer both live in a per-frame slot the command ring has already
	 * waited on before the frame begins (avk_cmd_ring_begin), so there is no
	 * hazard for a barrier to resolve. SHM staging is ordered by the submission
	 * that reads it, on a different ring.
	 *
	 * A counter reading zero says that on purpose. The alternative -- no
	 * counter -- makes the absence something a reader has to notice, and makes
	 * a future buffer resource silently unaccounted for.
	 */
	uint32_t buffer_barriers;

	/*
	 * Cumulative, NOT reset: heap allocations the graph has ever performed.
	 *
	 * The requirement this exists for is "graph construction has no pathological
	 * heap churn", and the only honest way to state that is a counter that stops
	 * moving. On a stable scene this reaches a small number during warmup and
	 * then never changes again; a frame that keeps incrementing it is the defect.
	 */
	uint64_t allocs;
	/*
	 * Cumulative CPU time the graph's OWN BOOKKEEPING costs: walking the uses,
	 * comparing state, deriving barrier structs, logging them.
	 *
	 * TWO THINGS ARE SUBTRACTED, and both had to be, because each in turn made
	 * this counter say something false:
	 *
	 *   the pass record callbacks -- that is the draw loop, work the frame was
	 *   always going to do. Including them read 21.5us a frame.
	 *
	 *   vkCmdPipelineBarrier2 itself. Separating it read 35-42us a frame live
	 *   against 2-7us headless, which looked like a graph that fell apart on
	 *   real hardware -- and was in fact the instrument, not the call. The call
	 *   costs ~60 ns (tests/test-avk-barrier-cost.c); bracketing it with a
	 *   clock_gettime pair costs ~37, and a scheduler slice landing in that
	 *   window is charged entirely to it. The emission is now folded back in
	 *   here and reported as a DISTRIBUTION rather than a mean.
	 *
	 * What is left is the part M4E actually added, and it is the number that
	 * belongs in a sentence beginning "the graph costs".
	 */
	uint64_t build_ns;
	/*
	 * PER-FRAME DISTRIBUTION of build_ns, in 250 ns buckets.
	 *
	 * A MEAN WAS THE WRONG STATISTIC AND IT PRODUCED A WRONG CONCLUSION. M4E
	 * timed each vkCmdPipelineBarrier2 call individually and reported the mean:
	 * 1.9 us headless, 44 us live, which was written up as evidence that
	 * DCC-compressed scan-out makes barriers expensive.
	 *
	 * tests/test-avk-barrier-cost.c then measured the call directly, one
	 * variable at a time on the same GPU. vkCmdPipelineBarrier2 costs 46-70 ns.
	 * DCC: 46 ns against 46 ns for the same-size non-DCC modifier. A foreign
	 * queue-family transfer: 44 ns against 51 ns without. Neither is a factor
	 * of anything.
	 *
	 * The 44 us was the INSTRUMENT. A bracketing clock_gettime pair costs ~37 ns
	 * around a ~60 ns event, and any scheduler slice landing inside that window
	 * is attributed entirely to it -- so on a loaded desktop the mean is a
	 * measure of preemption. In a quiet loop the same per-call timing reads
	 * p50 60 / p95 70 / mean 61 ns, which is what tells you the technique is
	 * sound and the environment was not.
	 *
	 * So: the per-call barrier timing is gone, and what remains is reported as a
	 * distribution. A percentile is robust to the outliers a mean is captured
	 * by.
	 */
	uint32_t build_hist[64];
	uint64_t frames;
};

struct avk_graph {
	struct avk_device *dev;

	struct avk_graph_resource *resources;
	uint32_t res_len, res_cap;
	struct avk_graph_pass *passes;
	uint32_t pass_len, pass_cap;
	struct avk_graph_use *uses;
	uint32_t use_len, use_cap;

	/* Scratch for one pass's barriers, sized once and reused. Never grows past
	 * the number of resources a single pass declares. */
	VkImageMemoryBarrier2 *barriers;
	uint32_t barrier_cap;

	struct avk_graph_barrier_log *log;
	uint32_t log_len, log_cap;

	struct avk_graph_stats stats;

	/*
	 * M4E.4 break. Drops the barrier that a write followed by a read requires,
	 * which is the one dependency a multipass effect cannot do without: pass B
	 * samples an image pass A is still writing. Read once at init.
	 */
};

void avk_graph_init(struct avk_graph *graph, struct avk_device *dev);
void avk_graph_finish(struct avk_graph *graph);

/* Begin a frame. Keeps every allocation; sets the lengths to zero. */
void avk_graph_reset(struct avk_graph *graph);

/*
 * Declare an image the frame will touch, returning its resource index, or
 * AVK_GRAPH_INVALID if it could not be recorded.
 *
 * Declaring the same image twice returns the FIRST index, so a caller may
 * declare defensively without producing two independent state trackers for one
 * VkImage -- which would emit a second barrier whose oldLayout the first had
 * already consumed. `foreign` and `exit` are taken from the first declaration.
 */
#define AVK_GRAPH_INVALID UINT32_MAX

uint32_t avk_graph_add_image(struct avk_graph *graph, struct avk_image *image,
	bool foreign, enum avk_graph_exit exit);

/*
 * Begin a pass. Every avk_graph_use() until the next avk_graph_pass_end()
 * belongs to it.
 *
 * `label` is borrowed and must outlive the frame.
 */
bool avk_graph_pass_begin(struct avk_graph *graph, const char *label,
	avk_graph_record_fn record, void *user);

/* Declare that the pass in progress touches `resource` in this way. `region`
 * may be NULL for "the whole resource". */
bool avk_graph_use(struct avk_graph *graph, uint32_t resource,
	enum avk_graph_usage usage, const struct avk_box *region);

/* Ask for the pass in progress to be bracketed by a timestamp pair. */
/*
 * Move an END mark onto the pass just declared, taking it off whichever pass
 * currently holds it.
 *
 * For "the last one of N", where N is not known until the loop ends and some
 * candidates may drop out. BLUR_END was bound to the last blur SLOT, but a
 * slot whose chain is declined never becomes a pass -- so on a two-slot frame
 * where the second was skipped the mark was never written, BLUR_BEGIN had no
 * partner, and gpu_blur_total collected ZERO samples while ten chains ran.
 *
 * Writing the same query twice is invalid usage, so this clears before it
 * sets. Linear over the frame's passes, which number in the tens.
 */
void avk_graph_pass_time_move_end(struct avk_graph *graph,
	enum avk_ts_mark end);

void avk_graph_pass_time(struct avk_graph *graph, enum avk_ts_mark begin,
	enum avk_ts_mark end);

void avk_graph_pass_end(struct avk_graph *graph);

/*
 * Compile and record the whole frame into `cb`.
 *
 * For each pass in declaration order: derive the barriers its uses require from
 * the state the graph has tracked, emit them as at most ONE
 * vkCmdPipelineBarrier2, then call the pass's record callback. Afterwards emit
 * the exit barriers every external resource needs and write the final layouts
 * back onto the images.
 *
 * `ts` may be NULL, in which case timed passes are simply not timed. Does not
 * submit, does not wait, and allocates nothing.
 */
void avk_graph_execute(struct avk_graph *graph, VkCommandBuffer cb,
	struct avk_timestamps *ts, uint32_t ts_slot);

/*
 * Write a human-readable topology to `out`, one line per pass and per barrier,
 * in the form the M4E brief asks to be understandable by inspection:
 *
 *     PASS compose_scene
 *         WRITE R0 color
 *     BARRIER
 *         R0 color-write -> sampled-read
 *     PASS blur_h
 *         READ R0 sampled
 *         WRITE R1 color
 *
 * Call it after avk_graph_execute(), which is what fills the barrier list in.
 * Returns the number of bytes it would have written, snprintf-style.
 */
size_t avk_graph_dump(const struct avk_graph *graph, char *out, size_t cap);

/* The (stage, access, layout) a usage class implies. Exposed because the
 * barrier tests assert on it directly: a table read by both the implementation
 * and its test is a tautology, so the test states the triples independently and
 * this is what it compares against. */
void avk_graph_usage_state(enum avk_graph_usage usage,
	VkPipelineStageFlags2 *stage, VkAccessFlags2 *access, VkImageLayout *layout);

/* "color-write", "sampled-read", ... for the dump and for test failure
 * messages. */
const char *avk_graph_usage_name(enum avk_graph_usage usage);

#endif /* AVK_GRAPH_H */
