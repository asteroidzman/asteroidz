#ifndef AZ_AVK_H
#define AZ_AVK_H

/*
 * The compositor half of the native Vulkan path.
 *
 * src/render/vulkan/ is the engine: it knows Vulkan and nothing else, and
 * tests/check-vulkan-isolation.py fails the build if a wlroots or GLES header
 * ever appears under it. This file is the other side of that boundary -- it
 * knows wlroots and asteroidz, and it is the only place the two vocabularies
 * meet.
 *
 * What it does, in the order a frame goes through it:
 *
 *   1. Freezes the wlr_scene tree into an avk_scene: a flat, immutable command
 *      list. Nothing below this point can see a scene node, so a client commit
 *      landing mid-frame cannot move geometry the GPU is already using.
 *   2. Resolves every buffer the frame references to an avk_image, cached on
 *      the wlr_buffer itself via wlr_addon so the cache cannot outlive what it
 *      caches. Client dma-bufs go through avk_dmabuf_import(), which is where
 *      the DRM_FORMAT_MOD_INVALID ladder lives; SHM goes through the staged
 *      upload. wlr_texture_from_buffer() is never called.
 *   3. Renders into a buffer from a wlr_swapchain we own, and hands that
 *      buffer to the output state. wlr_scene_output_build_state(),
 *      wlr_renderer_begin_buffer_pass() and wlr_render_pass are not involved.
 *
 * And, just as importantly, what it refuses to do. AVK in M3 has no colour
 * management, no effects and no software cursor of its own. Rather than render
 * those frames wrongly, az_avk_build_frame() returns false and the caller
 * falls back to the SceneFX path for that frame. A wrong frame is worse than a
 * slow one, and "AVK handles the outputs it can handle correctly" is a
 * position that can be shipped while the rest is built.
 */

/* The renderer's two raster extents, as two types that cannot be assigned to
 * one another. Everything below that says `att` or `pres` means one of these,
 * and nothing below is allowed to mean both. */
#include "az_extent.h"
/* AVK's own mapping of a client's wl_shm pool, and the SIGBUS guard over it.
 * The worker thread reads through this and never through
 * wlr_buffer_begin_data_ptr_access() -- see the header for why that matters. */
#include "az_shm_source.h"
#include "vulkan/avk.h"
#include "vulkan/device/avk_device.h"
#include "vulkan/device/avk_instance.h"
#include "vulkan/dmabuf/avk_dmabuf.h"
#include "vulkan/effect/avk_blur_cache.h"
#include "vulkan/image/avk_upload.h"
#include "vulkan/image/avk_upload_worker.h"
#include "vulkan/scene/avk_render.h"
#include "vulkan/scene/avk_scene.h"
#include "vulkan/sync/avk_sync.h"

#include <cjson/cJSON.h>
#include <drm.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <wlr/backend.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>

/* Most client acquire fences one frame will wait on. Beyond this the frame
 * would be submitted under-synchronised, so the overflow is counted rather
 * than silently ignored. */
#define AZ_AVK_MAX_ACQUIRE 32

#include <wlr/render/swapchain.h>
#include <wlr/util/addon.h>
#include <wlr/util/transform.h>

/* ── state ──────────────────────────────────────────────────────────────── */

/*
 * One renderer per output colour format.
 *
 * Dynamic rendering bakes the attachment format into the pipeline, so an
 * XRGB8888 output and an XRGB2101010 output genuinely cannot share one. Four
 * slots is not a guess about hardware -- it is the number of distinct formats
 * a mixed SDR/HDR/10-bit desktop can present at once, and running out is
 * logged rather than silently mishandled.
 */
/*
 * A fixed-bucket histogram, so a distribution can be reported instead of a
 * mean.
 *
 * An average hides exactly the frames that matter. A 6.6 ms mean is compatible
 * with every frame taking 6.6 ms and with nine frames taking 2 ms and one
 * taking 48 -- and only the second of those drops frames at 144 Hz. What is
 * wanted is p95 and p99 against a 6.944 ms budget, which needs the shape.
 *
 * Fixed buckets rather than a reservoir sample: no allocation, no per-frame
 * branchy bookkeeping, and the resolution is chosen per metric so the answer
 * is exact to within one bucket rather than statistically approximate.
 */
#define AZ_AVK_HIST_BUCKETS 512
/* 20us per bucket: 10.24ms of exact resolution, which brackets the 6.944ms a
 * 144Hz frame has and leaves room to see the tail past it. */
#define AZ_AVK_CPU_BUCKET_US 20

struct az_avk_hist {
	uint64_t count;
	uint64_t sum;
	uint64_t max;
	/* Values are divided by `scale` to pick a bucket; anything past the end
	 * lands in the last one, which is why `max` is kept separately. */
	uint64_t bucket[AZ_AVK_HIST_BUCKETS];
};

static void az_avk_hist_add(struct az_avk_hist *h, uint64_t value,
		uint64_t scale) {
	h->count++;
	h->sum += value;
	if (value > h->max) {
		h->max = value;
	}
	uint64_t idx = scale > 0 ? value / scale : value;
	if (idx >= AZ_AVK_HIST_BUCKETS) {
		idx = AZ_AVK_HIST_BUCKETS - 1;
	}
	h->bucket[idx]++;
}

/* The value at `pct` (0..100), in the histogram's own units. Returns the upper
 * edge of the bucket the percentile falls in, which is an over-estimate by at
 * most one bucket -- stated rather than hidden, because a percentile quoted to
 * three decimals from bucketed data would be a false precision. */
static double az_avk_hist_pct(const struct az_avk_hist *h, double pct,
		uint64_t scale) {
	if (h->count == 0) {
		return 0.0;
	}
	uint64_t want = (uint64_t)((double)h->count * pct / 100.0);
	if (want == 0) {
		want = 1;
	}
	uint64_t seen = 0;
	for (size_t i = 0; i < AZ_AVK_HIST_BUCKETS; i++) {
		seen += h->bucket[i];
		if (seen >= want) {
			double edge = (double)((i + 1) * scale);
			return edge > (double)h->max ? (double)h->max : edge;
		}
	}
	return (double)h->max;
}

#define AZ_AVK_MAX_FORMATS 4

/* How many source-damage rectangles a partial upload packs before giving up
 * and copying the whole buffer. wlroots' own damage ring collapses past 20, so
 * a client rarely presents more than a handful; the fallback is correct rather
 * than merely cheaper, so the limit costs nothing but a full copy. */
#define AZ_AVK_MAX_DAMAGE_RECTS 32

/* How many SHM sources the debug view reports. A desktop has a handful of
 * CPU-backed surfaces; the cap is there so the reply cannot grow with a
 * misbehaving client rather than because eight is a meaningful number. */
#define AZ_AVK_STAT_SOURCES 8

struct az_avk_renderer_slot {
	bool used;
	VkFormat format;
	struct avk_renderer renderer;
};

struct az_avk {
	/* Always true since AVK became the only renderer; kept because init still
	 * reads it to tell "asked for and failed" from "never asked", and the
	 * failure is a hard error either way. There is no env var behind it any
	 * more -- ASTEROIDZ_RENDERER is gone. */
	bool requested;
	/* Whether the engine actually came up. Requested-but-failed is a hard
	 * error at startup, not a silent downgrade -- see az_avk_init(). */
	bool active;

	struct avk_instance *instance;
	struct avk_device *device;
	struct avk_dmabuf_importer importer;

	struct az_avk_renderer_slot renderers[AZ_AVK_MAX_FORMATS];

	/* Every live cache entry, so shutdown can free the images that are still
	 * attached to buffers nobody destroyed. Without it, vkDestroyDevice
	 * reports leaked memory objects -- which is not merely untidy, it is the
	 * difference between "AVK cleans up after itself" being a claim and being
	 * a checked fact. */
	struct wl_list buffers;   /* az_avk_buffer.link */

	/* Counters. The instrumentation the M3 brief asks for; dumped by
	 * az_avk_log_stats() at shutdown and on demand over IPC. */
	uint64_t frames;              /* frames AVK composited */
	/*
	 * ── HOW LONG THE EVENT LOOP WAS NOT READING INPUT ────────────────────
	 *
	 * cpu_frame_us measures AVK's RECORD phase and nothing else, so a frame
	 * handler that spends 50ms inside wlr_output_commit_state() reads as a
	 * 240us frame. libinput was reporting "event processing lagging behind by
	 * 47-52ms" with every other explanation eliminated -- no modesets, no GL,
	 * no VRR dependency, no software cursor, no CPU sync waits -- and nothing
	 * in the compositor could see the gap because nothing measured the commit.
	 *
	 * These are the commit itself (call -> return) and the whole handler
	 * (frame event -> commit returned). The second is the one that matters:
	 * it is the interval the compositor is not in poll(), which is exactly
	 * what libinput is complaining about.
	 */
	uint64_t commit_ns_max, commit_ns_sum, commit_samples;
	uint64_t handler_ns_max, handler_ns_sum;
	uint64_t handler_over_10ms, handler_over_30ms;
	uint64_t fallback_frames;     /* frames handed back to the SceneFX path */
	uint64_t buffer_imports;      /* client buffers resolved to an avk_image */
	uint64_t buffer_import_fails;
	uint64_t shm_uploads;         /* re-uploads of CPU buffers */
	/* Content generations committed, versus uploads actually performed,
	 * versus lookups that found the GPU copy already current. The third one
	 * is the whole point: it should dwarf the second on a static desktop. */
	uint64_t shm_commits;
	uint64_t shm_full_uploads;
	uint64_t shm_partial_uploads;
	uint64_t shm_upload_skips;
	/* Source pixels the client said it changed, versus the whole area of the
	 * buffers those generations belonged to. The ratio is how much partial
	 * uploads are worth on this workload. */
	uint64_t shm_damage_pixels;
	uint64_t shm_committed_pixels;

	/*
	 * ── HOW LONG A CLIENT'S COMMIT HELD THE EVENT LOOP ───────────────────
	 *
	 * The frame path has counters for the interval it is not in poll(). The
	 * CLIENT REQUEST path had none, and that is where the block actually was:
	 * a wl_shm commit ran a synchronous memcpy of the client's buffer --
	 * 6.5 MB on the window that produced this, 26.5 GB in one session, ~27%
	 * of all perf samples -- inside az_avk_surface_commit(), while libinput's
	 * fds went unread and libinput reported "event processing lagging behind
	 * by 47-53ms" on the pointer AND the keyboard.
	 *
	 * `shm_commit_ns_*` is the commit handler itself, call to return: THE
	 * number this work exists to collapse.
	 *
	 * `shm_copy_ns_*` is main-thread time inside the pixel copy WHEREVER it
	 * happens -- the commit handler, the frame path, or a join that had to
	 * wait for the worker. Kept separately because moving the copy from the
	 * commit handler to the frame handler would improve the first number
	 * while blocking the loop for exactly as long, and that would be a lie
	 * this file has no business telling.
	 */
	uint64_t shm_commit_ns_max, shm_commit_ns_sum, shm_commit_samples;
	uint64_t shm_commit_over_5ms;
	uint64_t shm_copy_ns_max, shm_copy_ns_sum, shm_copy_samples;

	/* Copies handed to the worker, copies the main thread still did itself,
	 * and joins that actually had to wait (with what the waiting cost). A
	 * join that never waits is the steady state; one that waits is the worker
	 * not having finished before the frame needed the pixels. */
	uint64_t shm_async_jobs;
	uint64_t shm_async_bytes;
	uint64_t shm_sync_copies;
	uint64_t shm_async_joins, shm_async_join_waits;
	uint64_t shm_async_join_ns_max, shm_async_join_ns_sum;
	/* Times the alternating staging slot was STILL being read by the GPU when
	 * the host wanted to write it. Double buffering exists so this is zero;
	 * a nonzero count means uploads are outrunning the transfer queue, and
	 * the wait is what stops that from becoming corruption. */
	uint64_t shm_staging_waits;
	/* Staging buffers handed back by az_avk_shm_staging_reap(). */
	uint64_t shm_staging_reaped;
	/* ...and by az_avk_shm_staging_release(), as each copy is submitted. */
	uint64_t shm_staging_returned;
	uint64_t shm_staging_wait_ns_max, shm_staging_wait_ns_sum;
	/* Frames that showed the previous generation instead of blocking, and the
	 * microseconds of event loop that decision gave back. */
	uint64_t shm_stale_frames;
	/* Why a frame blocked anyway: the buffer was never pooled (so it has no
	 * siblings to ask about), or the pool held nothing usable. Split because
	 * 336 frames found a previous generation and 307 did not, and those two
	 * have different causes and different fixes. */
	uint64_t shm_stale_from_surface;
	/* Debts paid on TWO OR MORE outputs at once. Zero is the shape of the
	 * single-slot `stale_output` defect: a surface spanning a monitor edge
	 * owes both outputs a repaint and only ever got one. Non-zero only if a
	 * spanning surface actually drew stale, so a fixture must assert that
	 * premise too. AZ_BREAK_STALE_ONE_OUTPUT forces it back to zero. */
	uint64_t shm_stale_multi_output_repaints;
	uint64_t shm_stale_no_pool;
	uint64_t shm_stale_no_sibling;
	uint64_t shm_stale_pool_size_sum, shm_stale_pool_samples;
	/* Buffers the walker asked to resolve, and nodes it discarded before
	 * asking because they cannot touch this output at all. */
	uint64_t buffer_resolve_attempts;
	uint64_t nodes_output_culled_before_resolve;
	/*
	 * M4F.2A.3. Blur nodes the walk met, emitted as commands, and discarded --
	 * and how many asked for the cached bottom-layer path and got the live one
	 * anyway.
	 *
	 * Three numbers rather than one because they answer different questions and
	 * a single "blurs" counter conflates them: seen == 0 says the producer is
	 * not running, emitted == 0 with seen > 0 says the culling is wrong, and
	 * forced_live is the size of the M4F.2E decision.
	 */
	uint64_t blur_nodes_seen;
	uint64_t blur_nodes_emitted;
	/* M4F.2C. Commands kept ONLY because a blur on this output might sample
	 * them, and how wide the halo that kept them was. The first says whether a
	 * narrow halo has quietly become "replay the monitor next door"; the second
	 * says what the kernel asked for. */
	uint64_t nodes_retained_for_halo;
	uint64_t blur_halo_px;
	/* Frames whose damage arrived from OUTSIDE this output -- a change on the
	 * monitor next door that this output's blur can see. Zero on a
	 * single-output desktop, and zero on a multi-output one until something
	 * actually changes near a seam. */
	uint64_t blur_halo_damage_frames;
	/* Frames whose halo damage was recorded and then returned empty. Zero is
	 * the invariant; see where it is incremented. */
	uint64_t blur_halo_damage_lost;
	/* Summed over every output: rectangles inserted into a damage ring that lay
	 * outside the attachment. The ring accepts them and discards them at
	 * rotate, so this is the generic form of the M4F.2C.4c defect and must stay
	 * zero for every caller, not only for blur. */
	uint64_t damage_ring_out_of_bounds;
	uint64_t blur_nodes_culled;
	uint64_t blur_nodes_forced_live;
	/* Nodes whose composite was restricted by a clip_region or a
	 * clipped_region. Its own counter because the walker's node-local clip
	 * conversion is the only path that touches it, and "is that path exercised
	 * at all" is a question a fixture must be able to answer -- a clip test that
	 * ran against nodes that carry no clip proves nothing. */
	uint64_t blur_nodes_clipped;
	uint64_t commit_imports;      /* buffers taken ownership of at commit */
	uint64_t late_imports;        /* taken at DRAW time -- must stay 0 for
	                               * surfaces; see az_avk_walk_node() */
	uint64_t cache_hits;
	uint64_t cache_misses;
	uint64_t shm_upload_bytes;
	uint64_t client_images_cached;   /* live entries, not a total */
	uint64_t output_targets;         /* live imported scan-out images */

	/*
	 * The thread that does the SHM memcpy, and the entries with a copy
	 * currently in flight on it.
	 *
	 * NULL when the thread could not be started or AZ_AVK_SYNC_UPLOAD=1 asked
	 * for the old behaviour. Both cases must work, so nothing below may assume
	 * a worker exists -- the synchronous path stays the fallback rather than
	 * becoming dead code, which is also what makes the A/B measurable in one
	 * binary.
	 */
	struct avk_upload_worker *upload_worker;
	struct wl_list shm_jobs;              /* az_avk_buffer.job_link */
	struct wl_event_source *upload_source;

	/*
	 * The cursor, which is its own composition problem.
	 *
	 * A hardware cursor costs AVK nothing and must stay the fast path, so
	 * these separate "AVK drew it" from "the plane carried it" rather than
	 * reporting one number for both. `cursor_no_image` is the M3.5E
	 * regression's fingerprint: wlroots says a cursor is enabled and visible
	 * and asteroidz has no picture to draw for it.
	 */
	uint64_t software_cursor_frames;  /* frames AVK composited a cursor into */
	/* Frames where wlr_output_cursor's box did not describe the image AVK was
	 * about to draw into it. Every one is a second owner of the cursor; zero
	 * is the only correct value. */
	uint64_t cursor_geometry_mismatch;
	uint64_t hardware_cursor_frames;  /* frames the plane carried it instead */
	uint64_t cursor_commands;         /* cursor draws emitted */
	uint64_t cursor_no_image;         /* visible cursor, no buffer to draw */
	uint64_t cursor_import_failures;  /* the image would not go to the GPU */
	uint64_t cursor_culled;           /* entirely outside this output */
	/* Position, kept rigorously apart from content -- the D.1 principle
	 * applied to the cursor. `cursor_moves` counts frames whose cursor box
	 * differs from the previous frame's, which is what actually drives damage;
	 * it is not a count of pointer motion EVENTS, several of which can land
	 * inside one frame. */
	uint64_t cursor_moves;
	uint64_t cursor_damage_pixels;    /* area AVK drew for the cursor */
	uint64_t cursor_hw_to_sw;         /* plane gave it up */
	uint64_t cursor_sw_to_hw;         /* plane took it back */

	/* Presentation synchronisation. Every frame AVK composites must leave
	 * through exactly one of the first two, and `present_sync_none` must stay
	 * at zero: a frame handed to a display engine with no fence attached is a
	 * frame the display may scan out while the GPU is still writing it. */
	uint64_t present_sync_timeline;  /* fence passed as a drm_syncobj point */
	uint64_t present_sync_dmabuf;    /* fence attached to the target dma-buf */
	uint64_t present_sync_none;      /* MUST stay 0 outside the break test */
	uint64_t present_sync_fails;
	/* GPU-side waits inserted before rendering into a target somebody else may
	 * still be reading. A wait, not a stall: it is the GPU that waits, and the
	 * CPU returns from the frame immediately.
	 *
	 * It means slightly different things on the two routes, which is worth
	 * knowing before reading anything into the number. On the timeline route
	 * it counts only the frames where the display engine had genuinely not
	 * released the target yet, and should be rare. On the dma-buf route the
	 * buffer's fence list also contains our own previous write, so the count
	 * is close to one per frame and says nothing about contention. */
	uint64_t presentation_waits;
	/* A target re-acquired in a state it should not have been in. Anything but
	 * zero means the swapchain handed back a buffer that was still in use. */
	uint64_t target_state_violations;
	/* Damage, in pixels actually redrawn versus pixels the outputs have. Both
	 * accumulate, so the ratio is over the whole run rather than one frame. */
	uint64_t damage_pixels;
	uint64_t output_pixels;
	uint64_t full_redraw_frames;
	uint64_t partial_redraw_frames;
	/* The most rectangles one frame's damage arrived in. Worth watching: the
	 * ring collapses to a bounding box past WLR_DAMAGE_RING_MAX_RECTS, so a
	 * number stuck at that limit means damage is being rounded up rather than
	 * tracked. */
	uint64_t damage_rects_max;
	/* Wall clock spent building a frame on the CPU, and the rolling maximum,
	 * because a mean hides exactly the frames that miss a vblank. */
	uint64_t cpu_frame_us;
	uint64_t cpu_frame_us_max;
	/*
	 * THIS frame's record time and THIS frame's shm join time, kept so a stall
	 * can be attributed instead of guessed at.
	 *
	 * handler_over_30ms and cpu_frame_us_p99 disagreed -- 3.3% of frames over
	 * 30ms in the handler against 1% over 10ms in the record phase -- and a
	 * handler cannot exceed the record phase unless the time is somewhere
	 * else. Two aggregates cannot say where; one line naming the parts of a
	 * single slow frame can. Scratch, not statistics: both are overwritten
	 * every frame.
	 */
	uint64_t frame_record_us;
	/* Per frame: time inside az_avk_image_for_buffer() and how many of those
	 * resolves had to build a new image. Zeroed at the top of each frame. */
	uint64_t frame_resolve_ns;
	uint64_t frame_resolve_creates;
	uint64_t frame_join_ns;
	/* The output being recorded right now, so a decision taken deep in the
	 * walk can say WHICH output it was taken for. NULL outside a build. */
	struct wlr_output *frame_output;
	/* Set by the stale decision, consumed by the walker one step later --
	 * the only place that knows both the entry and where it is being drawn. */
	struct az_avk_buffer *frame_stale_entry;
	/*
	 * ── CLIENT ACQUIRE FENCES FOR THIS FRAME ─────────────────────────────
	 *
	 * AVK sampled every client's dma-buf with no producer->consumer ordering
	 * at all: no explicit acquire point, and no implicit dma-buf fence
	 * either. It waits for the display engine before overwriting its OWN
	 * targets (az_avk_target_acquire) -- that machinery was simply never
	 * pointed at client buffers.
	 *
	 * The consequence is a compositor that may read a client's buffer while
	 * the client's GPU work is still writing it. That is the defect the
	 * per-app `no-scanout` window rule has been standing in front of, and it
	 * is not gamescope-specific: it applies to every dma-buf client.
	 *
	 * Fences are collected during the walk and imported just before submit,
	 * because importing is per-submission: a sync_file import is consumed by
	 * the wait that uses it.
	 */
	int frame_acquire_fds[AZ_AVK_MAX_ACQUIRE];
	uint32_t frame_acquire_count;
	/*
	 * Surfaces this frame sampled that use explicit sync, so each can be told
	 * WHEN the read finished. Collected even when the surface attached no
	 * acquire fence: releasing is owed either way, and wlroots' default -- to
	 * signal the release point at the client's next commit, ungated by any
	 * GPU work -- tells a client its buffer is free while the compositor is
	 * still reading it.
	 */
	struct wlr_surface *frame_surfaces[AZ_AVK_MAX_ACQUIRE];
	uint32_t frame_surface_count;
	uint64_t release_points_set, release_points_dropped;
	/* Acquire fences waited on, dropped because the pool was full, and
	 * surfaces that had an explicit acquire point rather than an implicit
	 * fence. A rising `dropped` means AZ_AVK_MAX_ACQUIRE is too small and
	 * frames are being submitted under-synchronised. */
	uint64_t acquire_waits, acquire_explicit, acquire_implicit;
	uint64_t acquire_dropped, acquire_import_fails;
	/* Distributions, because the mean is the wrong statistic for a deadline.
	 * 20us buckets over 512 gives 10.2ms of exact resolution, which brackets
	 * the 6.944ms budget at 144Hz with room to see the tail. */
	struct az_avk_hist cpu_frame_hist;
	struct az_avk_hist gpu_submit_hist;
	/* Damage as parts per thousand of the output, so the ratio has a
	 * distribution too rather than only a run-long total. */
	struct az_avk_hist damage_permille_hist;

	/* Each of these is a real limitation, and each is said exactly once so a
	 * log stays readable while still telling the truth. */
	/* Transition and motion bookkeeping for the counters above. */
	bool cursor_was_software;
	int cursor_last_x, cursor_last_y;

	bool warned_effect_node;
	bool warned_optimized_blur;
	bool warned_color_transform;
	bool warned_zoom;
	bool warned_shm;
	bool warned_shm_source_gone;
	bool warned_late_import;
	bool warned_no_present_sync;
	bool warned_damage_hole;
};

static struct az_avk avk = {0};

/*
 * P4. Blur prefix rebuild pixels, summed across every renderer slot.
 *
 * Slots are keyed by VkFormat and shared between outputs, so this is not a
 * per-output number on its own -- but render_monitor runs one output's pass at
 * a time on the main thread, so a DELTA taken around one pass is that pass's
 * contribution. That is how a tag transition's blur cost is attributed to the
 * output paying it without threading a counter through the renderer.
 */
static uint64_t az_avk_blur_prefix_px(void) {
	uint64_t total = 0;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			total += avk.renderers[i].renderer.blur_prefix_rebuild_pixels;
		}
	}
	return total;
}



/* A switch is on. Used by the break tests and by the two presentation
 * fallbacks; read every time rather than cached, because these are set once
 * before the process starts and never change. */
/*
 * Is the C6 encode pass enabled in this session, and is it FORCED?
 *
 * One reader for AZ_M5_PATH_B, because two call sites now depend on the answer
 * -- the frame path, which decides where the scene composites, and
 * az_avk_output_supported(), which must not accept an HDR output the pass is
 * not going to run for. Those disagreeing is exactly the failure that puts
 * scene-linear values into a PQ buffer.
 */
static bool az_avk_encode_pass_enabled(bool *forced_out) {
	/*
	 * ── ON BY DEFAULT, WHEREVER C3 CHOSE IT ───────────────────────────────
	 *
	 * Path B is not an experiment to opt into: an HDR or 10-bit output CANNOT
	 * be driven without it. Vulkan has no sRGB variant of a 10-bit or
	 * half-float format, so Path A's attachment trick cannot exist there
	 * (F11), and the M5.6 interlock therefore REFUSES such an output when the
	 * pass is off -- handing it to SceneFX rather than writing scene-linear
	 * values into a PQ buffer. Leaving this opt-in meant HDR silently fell
	 * back on a compositor that can now drive it correctly.
	 *
	 * It changes nothing for an ordinary 8-bit SDR output: C3 puts those on
	 * Path A, which is separately gated and still off, so this only reaches
	 * the outputs that have no alternative.
	 *
	 *   unset     enabled where C3 chose Path B (HDR, 10-bit)
	 *   =0        disabled -- the bisect handle, and what restores the
	 *             pre-M5 fallback behaviour exactly
	 *   =force    additionally put a Path-A output on it. THE TEST INSTRUMENT
	 *             and not a setting: Path A is strictly cheaper where it is
	 *             available.
	 */
	const char *v = getenv("AZ_M5_PATH_B");
	const bool forced = v != NULL && strcmp(v, "force") == 0;
	if (forced_out != NULL) {
		*forced_out = forced;
	}
	if (forced) {
		return true;
	}
	return v == NULL || v[0] != '0';
}

static bool az_avk_env_flag(const char *name) {
	const char *v = getenv(name);
	return v != NULL && v[0] == '1';
}

/* ── client buffer cache ────────────────────────────────────────────────── */

/*
 * A wlr_buffer's avk_image, hung off the buffer with wlr_addon.
 *
 * An addon rather than a lookup table on purpose: the entry is destroyed by
 * the buffer's own destruction, so there is no window in which the cache holds
 * a pointer to a buffer that is gone. A table keyed on the buffer pointer has
 * exactly that window, and it is the shape of bug that reproduces once a week.
 */
struct az_avk_buffer {
	struct wlr_addon addon;
	struct wl_list link;      /* az_avk.buffers */
	/*
	 * The pool this buffer belongs to, if a surface has committed it.
	 *
	 * A client is entitled to rotate several wl_buffers behind one surface,
	 * and the damage it posts is defined against the PREVIOUS SURFACE
	 * CONTENT -- not against the previous content of the buffer being
	 * committed. Those are the same thing only while a client keeps reusing
	 * one buffer. Rotate two, and the damage at commit N understates what
	 * differs from the copy AVK holds of the buffer last seen at commit N-2.
	 *
	 * So a commit's damage is owed to every buffer in the surface's pool, not
	 * just the one carrying it. Membership is what makes "every buffer" a
	 * question this file can answer.
	 */
	struct wl_list pool_link;  /* az_avk_surface.pool */
	/* The head of the pool this entry is in, so its SIBLINGS can be found.
	 * A pointer to the list rather than to the surface because az_avk_surface
	 * is declared far below this and the frame path needs the walk here. */
	struct wl_list *pool_head;
	/* The surface this buffer belongs to. Only ever used through the two
	 * accessors below, because az_avk_surface is declared much further down
	 * and the frame path needs the answer up here. */
	struct az_avk_surface *as;
	/* The frame number this buffer's acquire fence was already taken for.
	 * A buffer resolved twice in one frame -- two outputs, or a cache hit
	 * after the walk revisits it -- must not consume its fence twice. */
	uint64_t acquire_frame;
	struct wlr_buffer *buffer;
	struct avk_image *image;

	/* CPU-backed buffers have to be re-uploaded when their contents change,
	 * so they keep their staging buffer rather than reallocating it. */
	bool is_shm;
	/*
	 * TWO staging buffers, alternating, and the reason is a corruption bug.
	 *
	 * A copy out of staging is a GPU read that outlives the submission call:
	 * vkCmdCopyBufferToImage2 returns immediately and the transfer happens
	 * later. With one staging buffer the next upload's memcpy writes the very
	 * bytes that copy is still reading -- a host write-after-GPU-read hazard
	 * with nothing between them. The result on screen is horizontal bands,
	 * because the copy walks staging in row order and picks up whichever
	 * generation had reached each row.
	 *
	 * avk_upload_staging_ensure() cannot fix it: it returns early whenever the
	 * existing buffer is big enough, and it is asked about SIZE, not about who
	 * is still reading. The wait that protects the destination image
	 * (az_avk_shm_wait, on image->last_use) is the other direction and does
	 * not help either.
	 *
	 * The hazard predates the upload worker -- the synchronous path packs and
	 * submits back to back as well -- but it was hard to reach, because the
	 * next commit was a frame away and the transfer had usually finished. The
	 * worker made it reachable: settle() submits and the very next commit
	 * posts a memcpy into the same buffer, so a client committing every vblank
	 * hits it every vblank.
	 *
	 * Alternating means the slot being written is two submissions old, so
	 * az_avk_shm_staging_take() finds it complete and never waits in the
	 * steady state. shm_staging_waits counts the times it did, because "it
	 * never waits" is a claim and not an assumption.
	 */
	struct avk_upload upload[2];
	uint32_t upload_slot;   /* the slot the LAST copy used */
	/*
	 * The frame this entry last copied anything, so idle staging can be given
	 * back. See az_avk_shm_staging_reap(): staging used to be held until the
	 * wl_buffer was destroyed, and a client that keeps its old buffers alive
	 * -- Firefox does -- therefore held one staging buffer per buffer it had
	 * ever drawn, forever, while every NEW buffer had to allocate and fault a
	 * fresh one. Measured live: 79 cached entries, 68 allocations, and a
	 * retire queue with nothing in it to recycle.
	 *
	 * CLOCK_MONOTONIC, not a frame count. The first version counted frames and
	 * could not see idleness at all: a desktop with nothing moving produces no
	 * frames, so an entry that stopped drawing ten seconds ago still read as
	 * "used one frame ago" -- and the reap then ran only once the NEXT client
	 * was already allocating.
	 */
	uint64_t upload_ns;

	/* A frame drew this buffer's PREVIOUS generation rather than wait for the
	 * copy in flight. Whoever settles that copy owes a repaint, because the
	 * damage that would have caused one was consumed by the frame that
	 * skipped. */
	bool drew_stale;
	/* Which output actually drew it. The repaint is owed to that output and
	 * to no other: damaging every monitor made a two-output window redraw
	 * fully instead of partially, which avk-crossoutput-border-test.sh
	 * measures directly (it wants partial > full, and got 115 against 174). */
	struct wlr_output *stale_output;
	/* Where it was drawn, in LAYOUT coordinates, so the repaint owed for a
	 * late copy can be that rectangle instead of the whole output. A box
	 * rather than the scene node it came from: a node pointer would dangle,
	 * four ints cannot. */
	struct wlr_box stale_box;
	bool stale_box_valid;


	/* Set once nothing worked, so the failure is diagnosed once instead of
	 * once per frame for as long as the window is open. */
	bool failed;

	/* True when this entry was created from a wl_surface commit rather than
	 * discovered mid-frame. The distinction is the whole ownership rule: at
	 * commit the client's content is guaranteed valid, and later it is only
	 * valid by luck. */
	bool taken_at_commit;

	/*
	 * Which committed version of the pixels this is.
	 *
	 * The bug this exists to kill: `az_avk_image_for_buffer()` treated "this
	 * buffer was encountered while walking the scene" as "this buffer has new
	 * pixels", and re-uploaded a CPU buffer on every frame that drew it. On a
	 * two-monitor desktop with a full-screen wallpaper per output that was
	 * 41.5 MB of CPU->GPU copy per output frame, 924 GB over one session, and
	 * essentially all of a 6.6 ms CPU frame time -- for pixels that had not
	 * changed since the wallpaper was drawn once.
	 *
	 * The two are simply different questions. A window moving across the
	 * desktop damages a great deal of the output and changes nothing in the
	 * client's buffer.
	 *
	 * `content` is bumped only by an event that means new pixels became
	 * current; `uploaded` records which of those the GPU copy holds. Equal
	 * means there is nothing to do, however many frames look the buffer up.
	 *
	 * A counter and not a pointer comparison, deliberately. A client may
	 * reuse the same wl_buffer for a later commit with different pixels, so
	 * `buffer == cached_buffer` is not "the contents are unchanged" -- it is
	 * the naive cache that renders a terminal permanently frozen on its first
	 * frame.
	 */
	uint64_t content_generation;
	uint64_t uploaded_generation;

	/*
	 * WHICH pixels changed in the generations not yet uploaded, in
	 * buffer-local coordinates.
	 *
	 * A different region from anything the output knows about, and the
	 * distinction is the point of Phase 2. Source damage says what the client
	 * redrew; scene damage says what the compositor must recomposite. Moving a
	 * static terminal across the desktop produces a great deal of the second
	 * and none of the first.
	 *
	 * wlroots has already done the hard part: wlr_surface.buffer_damage is
	 * buffer-local, clipped to the buffer bounds, and has surface-coordinate
	 * damage folded in through the surface's scale, transform and viewport
	 * (wlr_compositor.c surface_update_damage). Redoing that arithmetic here
	 * would be a second implementation of it to keep in step.
	 *
	 * `pending_full` is the honest fallback: a generation whose damage is
	 * unknown or unrepresentable uploads the whole buffer ONCE, which is the
	 * correct answer and still nothing like uploading it every frame.
	 */
	pixman_region32_t pending_damage;
	bool pending_full;

	/*
	 * ── THE COPY THAT IS HAPPENING SOMEWHERE ELSE ────────────────────────
	 *
	 * Non-NULL while a worker thread is copying this buffer's pixels into
	 * `upload`'s staging. The job carries the plan built at commit time, so
	 * `job_generation` -- not content_generation -- is what the image will
	 * hold once it settles; a commit landing while the job runs bumps the
	 * generation past it and is owed a second job.
	 *
	 * While a job is in flight this entry holds ONE thing it must give back: a
	 * wlr_buffer_lock(). wlr_compositor unlocks surface->current.buffer
	 * immediately after emitting the commit signal -- deliberately, so a wl_shm
	 * buffer can be released to the client the moment the compositor has copied
	 * it. Without the lock the client is free to draw into the memory the worker
	 * is reading, and the tearing would look like a client bug.
	 *
	 * IT DOES NOT HOLD wlr_buffer_begin_data_ptr_access(), and that is the
	 * whole correction to this design. That access is a plain bool with no
	 * nesting, and wlroots and SceneFX both take one on a client's committed
	 * buffer at moments we do not control -- see az_shm_source.h, which is where
	 * the pixels come from instead.
	 */
	struct avk_upload_job *job;
	uint64_t job_generation;
	/* Which of the two staging slots the worker is filling. The submit has to
	 * copy out of the slot the memcpy went into, and by the time it runs
	 * upload_slot may have moved on. */
	uint32_t job_slot;
	struct wl_list job_link;   /* az_avk.shm_jobs */

	/* AVK's own mapping of this buffer's wl_shm pool: what a deferred copy
	 * reads. Made on first deferred use and kept for the buffer's life, because
	 * a client's fd, offset and stride never change under it. */
	struct az_shm_source shm_source;

	/*
	 * The buffer's dimensions, COPIED at import rather than read back through
	 * entry->buffer when the stats are serialised.
	 *
	 * The stats path used to dereference the client's wlr_buffer at an
	 * arbitrary later moment, and a live reading of it once produced
	 * `height: 21861` -- 0x5565, the high half of a heap pointer, i.e. a read
	 * through freed-and-reused memory. That was never reproduced and
	 * inspection found no path by which an entry outlives its buffer (the
	 * addon that destroys it is attached to the buffer itself), so this does
	 * not claim to fix a lifetime bug.
	 *
	 * It removes the question. A diagnostic counter has no business holding a
	 * client pointer across time; the two numbers it wanted are immutable for
	 * a buffer's whole life and cost eight bytes to keep.
	 */
	uint32_t src_width, src_height;

	/* Per-source accounting, so "which surface is uploading the most" is a
	 * question with an answer instead of a guess. Cheap: five counters on an
	 * object that already exists, and read only over IPC. */
	uint64_t stat_generations;
	uint64_t stat_full_uploads;
	uint64_t stat_partial_uploads;
	uint64_t stat_upload_bytes;
	uint64_t stat_lookups;
};

struct az_avk_surface;
/*
 * The last image this surface successfully got onto the GPU, whatever buffer
 * carried it -- kept alive by the surface itself, so it survives that buffer
 * being destroyed. Defined below az_avk_surface; declared here because the
 * frame path is above it.
 */
static struct avk_image *az_avk_surface_last_good(struct az_avk_buffer *entry,
	uint64_t *generation);
/* The wlr_surface this buffer was committed by, or NULL. Needed to reach the
 * explicit-sync state, which hangs off the surface and not the buffer. */
static struct wlr_surface *az_avk_surface_wlr(struct az_avk_buffer *entry);
static void az_avk_surface_note_good(struct az_avk_buffer *entry);
static void az_avk_surface_adopt_image(struct az_avk_buffer *entry);

static void az_avk_buffer_destroy(struct az_avk_buffer *entry) {
	/*
	 * ── A COPY IN FLIGHT OUTRANKS EVERYTHING ELSE HERE ───────────────────
	 *
	 * The worker may be reading the client's mapping and writing this entry's
	 * staging buffer; freeing either underneath it is a use-after-free with a
	 * second thread as the accused.
	 *
	 * This should be unreachable, and by construction rather than by luck: a
	 * job holds a wlr_buffer_lock(), so the addon path (which runs from
	 * wlr_buffer_destroy, at n_locks == 0) cannot see one, and the teardown
	 * path drains every job before it walks this list. It is handled anyway,
	 * and handled WITHOUT giving the lock back here -- the unlock is deferred
	 * to the very end, past the free, because releasing the last reference to
	 * a buffer from inside its own entry's destructor re-enters this function.
	 */
	struct wlr_buffer *unlock_after = NULL;
	if (entry->job != NULL) {
		wlr_log(WLR_ERROR, "AVK: a buffer with a SHM copy in flight is being "
			"destroyed; waiting for the copy rather than freeing under it");
		avk_upload_worker_finish(avk.upload_worker, entry->job);
		wl_list_remove(&entry->job_link);
		wl_list_init(&entry->job_link);
		free(entry->job);
		entry->job = NULL;
		unlock_after = entry->buffer;
	}
	/* After the job has been finished above, and never before: unmapping the
	 * pages a worker is copying out of is the use-after-free this whole file is
	 * arranged to avoid. */
	az_shm_source_finish(&entry->shm_source);
	/* If this is the image its surface is keeping as the last known good one,
	 * the surface takes ownership instead and the retire below is skipped --
	 * the whole point is that it outlives this buffer. */
	az_avk_surface_adopt_image(entry);
	if (entry->image != NULL) {
		/* Retired against the timeline, not freed: a frame that sampled this
		 * image may still be in flight, and vkDestroyImage on an image the GPU
		 * is reading is undefined behaviour that usually survives long enough
		 * to be blamed on something else. */
		avk_retire_push(&avk.importer.retire, avk.device, entry->image->last_use,
			avk_image_destroy, entry->image);
		entry->image = NULL;
	}
	/* BOTH slots. Retiring only the one the last copy used would leak the
	 * other and, worse, destroy nothing while the GPU may still be reading
	 * it. */
	for (size_t s = 0; entry->is_shm && s < 2; s++) {
		if (entry->upload[s].buffer == VK_NULL_HANDLE) {
			continue;
		}
		struct avk_upload *up = calloc(1, sizeof(*up));
		if (up != NULL) {
			AVK_LIVE_INC(avk.device, avk_uploads);
			*up = entry->upload[s];
			/* Against the submission that actually read it. This used to be
			 * `current value + 1` -- a point no submission owns, chosen to
			 * mean "one more frame from now". During a session that is
			 * accidentally safe, because uploads and frames share one queue
			 * and so complete in submission order. At teardown it is a point
			 * nothing will ever signal, so the entry is destroyed by
			 * avk_retire_finish() with its stated precondition unmet -- which
			 * is indistinguishable, from inside the queue, from a genuine
			 * destroy-before-idle. */
			avk_retire_push(&avk.importer.retire, avk.device, up->last_use,
				avk_upload_retire, up);
		}
	}
	/* Out of its surface's pool before the memory goes: the list outlives the
	 * buffer, and a stale link is a use-after-free the next commit would walk
	 * straight into. */
	if (!wl_list_empty(&entry->pool_link)) {
		wl_list_remove(&entry->pool_link);
		wl_list_init(&entry->pool_link);
	}
	pixman_region32_fini(&entry->pending_damage);
	wlr_addon_finish(&entry->addon);
	wl_list_remove(&entry->link);
	if (avk.client_images_cached > 0) {
		avk.client_images_cached--;
	}
	free(entry);
	/* Last, and after the free on purpose: this may be the reference that
	 * destroys the buffer, which would come straight back here. */
	if (unlock_after != NULL) {
		wlr_buffer_unlock(unlock_after);
	}
}

static void az_avk_buffer_addon_destroy(struct wlr_addon *addon) {
	struct az_avk_buffer *entry = wl_container_of(addon, entry, addon);
	az_avk_buffer_destroy(entry);
}

static const struct wlr_addon_interface az_avk_buffer_addon_impl = {
	.name = "az_avk_buffer",
	.destroy = az_avk_buffer_addon_destroy,
};

/*
 * What a buffer actually IS, decided without asking a renderer.
 *
 * The old path answered "can this be drawn?" with "does it have a
 * wlr_texture?", which is a question about a renderer rather than about the
 * buffer. This asks the buffer: it is a dma-buf, or it is CPU-readable, or it
 * is neither and nothing can draw it. A single-pixel buffer is CPU-readable
 * like any other and needs no special case -- it is called out here only
 * because it is the one buffer type that is neither client-allocated nor
 * shared memory, and a reader looking for it should find the answer.
 */
enum az_buffer_kind {
	AZ_BUFFER_NONE,
	AZ_BUFFER_DMABUF,
	AZ_BUFFER_DATA_PTR,
};

struct az_buffer_source {
	enum az_buffer_kind kind;
	struct wlr_dmabuf_attributes dmabuf;   /* AZ_BUFFER_DMABUF only */
};

static bool az_buffer_get_source(struct wlr_buffer *buffer,
		struct az_buffer_source *out) {
	*out = (struct az_buffer_source){ .kind = AZ_BUFFER_NONE };
	if (buffer == NULL) {
		return false;
	}
	if (wlr_buffer_get_dmabuf(buffer, &out->dmabuf)) {
		out->kind = AZ_BUFFER_DMABUF;
		return true;
	}
	/* Probed by opening and immediately closing the access window. The
	 * alternative -- inspecting buffer->impl -- is reaching into wlroots
	 * internals to learn something wlroots will answer if asked. */
	void *data = NULL;
	uint32_t format = 0;
	size_t stride = 0;
	if (wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		wlr_buffer_end_data_ptr_access(buffer);
		out->kind = AZ_BUFFER_DATA_PTR;
		return true;
	}
	return false;
}

/* wlroots' dma-buf attributes into AVK's. Two structs saying the same thing,
 * because the engine may not include a wlroots header -- the conversion is the
 * price of that isolation and it is three lines. */
static void az_avk_attribs_from_wlr(struct avk_dmabuf_attributes *dst,
		const struct wlr_dmabuf_attributes *src) {
	*dst = (struct avk_dmabuf_attributes){
		.width = src->width,
		.height = src->height,
		.format = src->format,
		.modifier = src->modifier,
		.n_planes = src->n_planes,
	};
	for (int i = 0; i < src->n_planes && i < AVK_MAX_PLANES; i++) {
		dst->offset[i] = src->offset[i];
		dst->stride[i] = src->stride[i];
		dst->fd[i] = src->fd[i];
	}
}

/*
 * ── WHERE THE COPY RUNS, AND WHY IT IS NOT HERE ──────────────────────────
 *
 * A wl_shm client's pixels reach the GPU by being copied out of the client's
 * mapping into staging, and the copy is real work: 6.5 MB per repaint from a
 * 1637x1160 Qt window, 26.5 GB in one session, memcpy at ~27% of all perf
 * samples with the compositor at 30% of a core while the mouse moved.
 *
 * Doing it on the compositor's thread is what made that a LATENCY bug rather
 * than a throughput one. The whole time it runs the event loop is not in
 * poll(), so libinput's fds are not read, and libinput says so:
 *
 *     USB Gaming Mouse: client bug: event processing lagging behind by 47-53ms
 *     Cherry Corded Device: ... lagging behind by 30ms
 *
 * Pointer AND keyboard, so not a device. The frame path was not the cause and
 * is not blamed here: its own handler counters read 4.8 ms worst case with
 * zero CPU sync waits while input was 53 ms late. The block was in the client
 * request path.
 *
 * There is no smaller copy available. That client reports 96.6% of its
 * surface as damaged per repaint (shm_damage_pixels vs shm_committed_pixels),
 * so the damage-rectangle path below is already doing everything it can; the
 * bytes have to move. And "tell the client to use dma-buf" is not a fix: wl_shm
 * is core protocol, any client may use it, and a compositor that stalls input
 * on it is broken regardless of what the client chose.
 *
 * So the copy moves to a worker thread (vulkan/image/avk_upload_worker.h) and
 * the main thread keeps only what must stay on one thread:
 *
 *   at COMMIT   decide what to copy, size staging, lock the buffer, open its
 *               data-ptr access, post the job. Microseconds.
 *   on the WORKER   the memcpy, and nothing else.
 *   at SETTLE   record the barriers and the copy, submit, close the access,
 *               unlock the buffer. Microseconds, and it happens either from
 *               the worker's eventfd -- the loop is in poll() anyway -- or
 *               from the frame that needs the pixels, whichever comes first.
 *
 * WHAT ORDERS THE GPU IS UNCHANGED. The submission still carries a timeline
 * wait on image->last_use and the same read-before-write barrier, still on the
 * main thread and therefore still in submission order against the frames. That
 * matters for more than tidiness: AZ_AVK_UNSAFE_REUSE exists to strip that
 * ordering, and a change that quietly removed the ordering along with the wait
 * would leave the break switch testing nothing.
 */

/* Whether a copy may leave this thread at all. */
static bool az_avk_shm_async_enabled(void) {
	/*
	 * AZ_AVK_SYNC_UPLOAD=1 puts the copy back on the event loop.
	 *
	 * Not a fallback and not an escape hatch: it is the BEFORE arm of the
	 * measurement. The same binary, the same fixture and the same client, with
	 * the only difference being which thread runs memcpy -- which is the only
	 * way a before/after number on this is worth anything.
	 */
	return avk.upload_worker != NULL && !az_avk_env_flag("AZ_AVK_SYNC_UPLOAD");
}

static uint64_t az_avk_now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Main-thread nanoseconds spent moving client pixels, wherever they were
 * spent. The number that must collapse, and the one a "fix" that merely moved
 * the block from one handler to another could not fake. */
static void az_avk_note_copy_ns(uint64_t ns) {
	avk.shm_copy_samples++;
	avk.shm_copy_ns_sum += ns;
	if (ns > avk.shm_copy_ns_max) {
		avk.shm_copy_ns_max = ns;
	}
}

/*
 * Record what changed in a generation that has not been uploaded yet.
 *
 * Accumulated rather than replaced: several commits may land between two
 * frames, and the upload owes all of them. A NULL or empty region means the
 * source did not say, which is not the same as "nothing changed" -- it has to
 * become a full upload, because guessing the other way shows stale pixels.
 */
static void az_avk_note_source_damage(struct az_avk_buffer *entry,
		const pixman_region32_t *damage) {
	if (damage == NULL) {
		entry->pending_full = true;
		return;
	}
	pixman_region32_union(&entry->pending_damage, &entry->pending_damage,
		(pixman_region32_t *)damage);
}

/* The GPU-side image for a CPU buffer, on first sight. Its own function
 * because both the synchronous path and the commit path need it before they
 * can plan anything, and duplicating it is how the two would come to disagree
 * about is_shm -- which decides whether the entry keeps a staging buffer at
 * all. */
static bool az_avk_shm_image_create(struct az_avk_buffer *entry,
		uint32_t format) {
	entry->image = avk_upload_image_create(avk.device, format,
		(uint32_t)entry->buffer->width, (uint32_t)entry->buffer->height,
		AVK_IMAGE_OWNED);
	if (entry->image == NULL) {
		return false;
	}
	entry->is_shm = true;
	if (!avk.warned_shm) {
		avk.warned_shm = true;
		wlr_log(WLR_INFO, "AVK: a client is using CPU (SHM) buffers; these "
			"are copied to the GPU whenever the client changes them");
	}
	return true;
}

enum az_shm_plan_result {
	AZ_SHM_PLAN_FULL,      /* the whole buffer */
	AZ_SHM_PLAN_REGIONS,   /* only what the client said changed */
	AZ_SHM_PLAN_NOTHING,   /* the client reported no damage: believe it */
	AZ_SHM_PLAN_FAILED,
};

/*
 * What this generation owes the GPU, as a plan.
 *
 * Extracted so the synchronous path and the worker path decide identically.
 * They used to be one function, and the moment they were not, "the async path
 * ignores AZ_AVK_SOURCE_FULL" became a bug two break switches could not see.
 */
static enum az_shm_plan_result az_avk_shm_plan(struct az_avk_buffer *entry,
		uint32_t stride, struct avk_upload_plan *plan) {
	/*
	 * Whole buffer, or only the rectangles the client says it changed?
	 *
	 * Full whenever the answer is not certain: a first upload, a generation
	 * whose damage nobody reported, or more rectangles than the packed-copy
	 * path takes. One full upload for a generation is a correct answer and is
	 * still nothing like one per frame.
	 */
	bool want_full = entry->pending_full
		|| az_avk_env_flag("AZ_AVK_SOURCE_FULL");
	int rect_count = 0;
	const pixman_box32_t *boxes = want_full ? NULL
		: pixman_region32_rectangles(&entry->pending_damage, &rect_count);

	if (!want_full && rect_count == 0) {
		/* A generation whose client reported no damage at all changed no
		 * pixels. wlroots' own texture path copies nothing here too. */
		return AZ_SHM_PLAN_NOTHING;
	}

	if (!want_full && rect_count <= AZ_AVK_MAX_DAMAGE_RECTS) {
		struct avk_upload_rect rects[AZ_AVK_MAX_DAMAGE_RECTS];
		uint32_t n = 0;
		for (int i = 0; i < rect_count; i++) {
			/* Clamped to the buffer, always. wlroots clips buffer_damage to
			 * the committed buffer bounds, but this copy reads a client's
			 * mapping and a rectangle that escaped that would read past it. */
			int32_t x1 = boxes[i].x1 < 0 ? 0 : boxes[i].x1;
			int32_t y1 = boxes[i].y1 < 0 ? 0 : boxes[i].y1;
			int32_t x2 = boxes[i].x2 > entry->buffer->width
				? entry->buffer->width : boxes[i].x2;
			int32_t y2 = boxes[i].y2 > entry->buffer->height
				? entry->buffer->height : boxes[i].y2;
			if (x2 <= x1 || y2 <= y1) {
				continue;
			}
			if (az_avk_env_flag("AZ_AVK_OMIT_REGION") && i == rect_count - 1
					&& rect_count > 1) {
				/* Break test: drop one rectangle and leave the rest. */
				continue;
			}
			rects[n++] = (struct avk_upload_rect){
				.x = (uint32_t)x1, .y = (uint32_t)y1,
				.width = (uint32_t)(x2 - x1), .height = (uint32_t)(y2 - y1),
			};
		}
		if (n > 0 && avk_upload_plan_regions(plan, entry->image, stride,
				(uint32_t)entry->buffer->height, rects, n)) {
			return AZ_SHM_PLAN_REGIONS;
		}
		/* The partial path declined; fall through and do it whole rather than
		 * leaving the image holding a stale generation. */
	}

	if (!avk_upload_plan_full(plan, entry->image, stride,
			(uint32_t)entry->buffer->height)) {
		return AZ_SHM_PLAN_FAILED;
	}
	return AZ_SHM_PLAN_FULL;
}

/* The ordering that keeps an upload behind the frames still sampling the
 * image. One place, so the sync path and the worker path cannot differ -- and
 * so AZ_AVK_UNSAFE_REUSE strips it from both. */
static uint32_t az_avk_shm_wait(const struct az_avk_buffer *entry,
		VkSemaphoreSubmitInfo *wait) {
	*wait = (VkSemaphoreSubmitInfo){
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = avk.device->timeline,
		.value = entry->image->last_use,
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	};
	if (az_avk_env_flag("AZ_AVK_UNSAFE_REUSE")) {
		/* Break test: overwrite the image with no ordering against the frames
		 * still sampling it. The copy races the fragment shader reading the
		 * previous generation, which synchronisation validation reports as a
		 * write-after-read hazard -- and which, without validation, shows up
		 * as an occasional torn surface that looks like a client bug. */
		return 0;
	}
	return entry->image->last_use > 0 ? 1 : 0;
}

/*
 * Take the staging slot the next copy may write into, sized for `size`.
 *
 * The slot that is NOT the one the last copy used, so the host writes one
 * while the GPU may still be reading the other. See the comment on
 * az_avk_buffer.upload for what a single buffer did to the screen.
 *
 * Returns NULL if the slot could not be sized, in which case the caller does
 * what it already does when staging cannot be had: nothing, and try again on
 * the next commit. The slot is NOT committed to on failure -- upload_slot is
 * advanced by the caller once the copy is actually going to happen -- so a
 * failed attempt leaves the alternation where it was rather than skipping a
 * slot and landing back on the one the GPU is reading.
 */
static void az_avk_shm_staging_reap(void);

/*
 * ── STAGING IS SCRATCH SPACE, NOT STATE ───────────────────────────────────
 *
 * Give the slot back the moment its copy is submitted. Everything after that
 * belongs to the GPU, and the retire queue already knows how to wait for it --
 * which is also what puts the buffer in the warm cache for whoever needs one
 * next.
 *
 * It used to be held by the entry until its wl_buffer was destroyed, and that
 * is the wrong lifetime for the client this path exists for. Firefox presents
 * a NEW buffer per frame during activity and keeps the old ones, so every
 * frame was a new entry wanting its own 56MB, six of them inside 280ms on one
 * tag switch, each faulting its own pages: six 52ms copies where the pool
 * needed to hold two. Holding scratch space per buffer also held it for the
 * 57 buffers nobody was drawing.
 */
static void az_avk_shm_staging_release(struct az_avk_buffer *entry,
		uint32_t slot) {
	struct avk_upload *up = &entry->upload[slot];
	if (up->buffer == VK_NULL_HANDLE || avk.device == NULL) {
		return;
	}
	struct avk_upload *held = calloc(1, sizeof(*held));
	if (held == NULL) {
		/* Keep it on the entry rather than destroy it here: the GPU may still
		 * be reading, and the queue is what knows when it is not. */
		return;
	}
	AVK_LIVE_INC(avk.device, avk_uploads);
	*held = *up;
	memset(up, 0, sizeof(*up));
	avk.shm_staging_returned++;
	avk_retire_push(&avk.importer.retire, avk.device, held->last_use,
		avk_upload_retire, held);
}

static struct avk_upload *az_avk_shm_staging_take(struct az_avk_buffer *entry,
		VkDeviceSize size, uint32_t *slot_out) {
	uint32_t slot = entry->upload_slot ^ 1u;
	if (az_avk_env_flag("AZ_AVK_UNSAFE_STAGING")) {
		/* Break test: one staging buffer for every copy, which is what this
		 * code did before. The host memcpy then overwrites the bytes an
		 * in-flight vkCmdCopyBufferToImage2 is still reading, which
		 * synchronisation validation reports as a write-after-read hazard on
		 * the buffer -- and which, without validation, is the horizontal
		 * banding that looks like a client rendering bug. */
		slot = 0;
	}
	struct avk_upload *up = &entry->upload[slot];

	/*
	 * ── ASK THE IDLE ENTRIES BEFORE FAULTING A NEW ONE ────────────────────
	 *
	 * This slot has no buffer and the one wanted is big enough that creating
	 * it means a page fault per page -- 52ms for 56MB, on the thread that
	 * copies. Meanwhile a client that stopped drawing a second ago may be
	 * sitting on exactly such a buffer, doing nothing with it.
	 *
	 * On demand rather than only per frame, because per frame is too late in
	 * the case that matters: the allocation happens in a commit handler, and
	 * the frame that would have reaped comes after it. The reap pushes to the
	 * retire queue and avk_upload_staging_ensure() drains that queue on a
	 * cache miss, so the buffer is in the cache by the time it looks.
	 */
	if (up->buffer == VK_NULL_HANDLE && size >= (4u << 20)) {
		az_avk_shm_staging_reap();
	}

	/*
	 * Wait only if this slot is genuinely still in flight. Alternating makes
	 * that the two-submissions-ago case, which has normally long completed --
	 * so this is a poll that almost always answers immediately, not a fence
	 * wait on the event loop.
	 */
	if (up->last_use > 0
			&& avk_device_timeline_value(avk.device) < up->last_use) {
		uint64_t t0 = az_avk_now_ns();
		if (!avk_device_timeline_wait(avk.device, up->last_use, UINT64_MAX)) {
			return NULL;
		}
		uint64_t waited = az_avk_now_ns() - t0;
		avk.shm_staging_waits++;
		avk.shm_staging_wait_ns_sum += waited;
		if (waited > avk.shm_staging_wait_ns_max) {
			avk.shm_staging_wait_ns_max = waited;
		}
	}

	if (!avk_upload_staging_ensure(avk.device, up,
			avk.importer.upload_ring.retire, size)) {
		return NULL;
	}
	*slot_out = slot;
	return up;
}

/* Book-keeping for one completed upload, whichever path performed it. */
static void az_avk_shm_account(struct az_avk_buffer *entry,
		const struct avk_upload_plan *plan) {
	avk.shm_uploads++;
	avk.shm_upload_bytes += plan->bytes;
	entry->stat_upload_bytes += plan->bytes;
	if (plan->full) {
		avk.shm_full_uploads++;
		entry->stat_full_uploads++;
		avk.shm_damage_pixels +=
			(uint64_t)entry->buffer->width * (uint64_t)entry->buffer->height;
	} else {
		avk.shm_partial_uploads++;
		entry->stat_partial_uploads++;
		uint64_t damage_px = 0;
		for (uint32_t i = 0; i < plan->rect_count; i++) {
			damage_px += (uint64_t)plan->rects[i].width
				* (uint64_t)plan->rects[i].height;
		}
		avk.shm_damage_pixels += damage_px;
	}
	/* The image now holds different pixels behind the same identity -- see
	 * avk_image.content_seq. A partial upload counts: a wallpaper that changed
	 * only where it differs is still a new wallpaper. */
	entry->image->content_seq++;
}

/* Give back everything a job was holding on the client's behalf. The mapping is
 * NOT one of them: it belongs to the entry, not to the job, and outlives every
 * copy this buffer will ever be part of (az_shm_source.h). */
static void az_avk_shm_job_release(struct az_avk_buffer *entry) {
	wl_list_remove(&entry->job_link);
	wl_list_init(&entry->job_link);
	free(entry->job);
	entry->job = NULL;
	/* Last, because it may be the reference that keeps the buffer -- and this
	 * entry with it -- alive. */
	wlr_buffer_unlock(entry->buffer);
}

/*
 * Finish a copy that is in flight and put it on the GPU.
 *
 * `block` is the difference between "the worker says it is done" (the eventfd
 * drain, which must never wait) and "a frame needs these pixels now" (the
 * lookup, which has no choice). Returns true if the entry now holds
 * job_generation; false if nothing was settled.
 */
static bool az_avk_shm_job_settle(struct az_avk_buffer *entry, bool block) {
	if (entry->job == NULL) {
		return false;
	}
	if (!block && !avk_upload_job_done(entry->job)) {
		return false;
	}

	avk.shm_async_joins++;
	uint64_t waited = avk_upload_worker_finish(avk.upload_worker, entry->job);
	if (waited > 0) {
		avk.shm_async_join_waits++;
		avk.shm_async_join_ns_sum += waited;
		if (waited > avk.shm_async_join_ns_max) {
			avk.shm_async_join_ns_max = waited;
		}
		avk.frame_join_ns += waited;
		/*
		 * The wait is NOT recorded as a copy time.
		 *
		 * It used to be, and the consequence was that shm_copy_us_max and
		 * shm_async_join_us_max printed the same number and could never
		 * disagree -- so the one question they were read to answer, "is the
		 * copy slow or is the wait slow", had been defined away. The copy's
		 * own duration comes from the thread that performs it; see
		 * avk_upload_worker_timing() and shm_worker_pack_us_max.
		 */
	}

	VkSemaphoreSubmitInfo wait;
	uint32_t wait_count = az_avk_shm_wait(entry, &wait);
	/* The slot the WORKER filled, not the current one: another commit may have
	 * advanced upload_slot between the post and this submit. */
	bool ok = avk_upload_submit_packed(avk.device, &avk.importer.upload_ring,
		&entry->upload[entry->job_slot], entry->image, &entry->job->plan, &wait,
		wait_count) != 0;
	if (ok) {
		az_avk_shm_account(entry, &entry->job->plan);
		entry->uploaded_generation = entry->job_generation;
		az_avk_surface_note_good(entry);
	} else {
		/* The pixels are in staging and nothing carried them across. The
		 * generation is NOT marked uploaded and the next attempt is forced
		 * whole, because the damage that described it has already been
		 * consumed by the plan that failed. */
		entry->pending_full = true;
	}
	az_avk_shm_staging_release(entry, entry->job_slot);
	az_avk_shm_job_release(entry);
	return ok;
}

/*
 * ── GIVE BACK THE STAGING OF ENTRIES THAT HAVE STOPPED DRAWING ────────────
 *
 * Staging was released only when its wl_buffer was destroyed. That is fine for
 * a client that presents a fresh buffer every frame and drops the old one --
 * contrib/wlrepaint --churn does, which is why every headless measurement of
 * the warm-buffer cache looked perfect. Real clients keep their old buffers:
 * measured on a live desktop, 79 cached entries, 68 staging allocations, and
 * `retire_entries_live` sitting at 0 -- nothing had been released, so the cache
 * had nothing to hand out and every new buffer faulted a fresh 56MB of it.
 *
 * An entry that has not copied anything for a second is not about to. Its
 * staging goes back through the retire queue, which is what puts it in the
 * cache and what guarantees the GPU is finished with it. The entry keeps its
 * image and its damage; only the scratch space goes.
 *
 * A second is chosen against how clients actually rotate: a pool of two or
 * three buffers touches each one every few frames, so this can only reach the
 * ones that have genuinely stopped.
 */
#define AZ_AVK_STAGING_IDLE_NS 1000000000ull

static void az_avk_shm_staging_reap(void) {
	if (avk.device == NULL) {
		return;
	}
	uint64_t now = az_avk_now_ns();
	struct az_avk_buffer *entry;
	wl_list_for_each(entry, &avk.buffers, link) {
		if (!entry->is_shm || entry->job != NULL) {
			continue;
		}
		if (entry->upload_ns == 0
				|| now - entry->upload_ns < AZ_AVK_STAGING_IDLE_NS) {
			continue;
		}
		for (size_t s = 0; s < 2; s++) {
			struct avk_upload *up = &entry->upload[s];
			if (up->buffer == VK_NULL_HANDLE) {
				continue;
			}
			struct avk_upload *held = calloc(1, sizeof(*held));
			if (held == NULL) {
				/* Keep it on the entry rather than destroy it here: the GPU
				 * may still be reading, and that is what the queue is for. */
				continue;
			}
			AVK_LIVE_INC(avk.device, avk_uploads);
			*held = *up;
			memset(up, 0, sizeof(*up));
			avk.shm_staging_reaped++;
			avk_retire_push(&avk.importer.retire, avk.device, held->last_use,
				avk_upload_retire, held);
		}
		/* Stamped so a still-idle entry is not walked into every frame. */
		entry->upload_ns = now;
	}
}

/*
 * Hand this entry's outstanding damage to the worker.
 *
 * Called from the commit handler, where the client's content is guaranteed to
 * be the committed one and where -- until this change -- the memcpy happened.
 * Returns true if a job is now in flight.
 *
 * The pixels come from AVK's OWN mapping of the client's pool, not from
 * wlr_buffer_begin_data_ptr_access(): that access is a plain bool that other
 * code takes on the same buffer inside the same commit signal, and holding it
 * across this function's return aborted the compositor. See az_shm_source.h.
 *
 * A buffer that is not a wl_shm pool buffer -- a wp_single_pixel_buffer_v1, one
 * of the compositor's own -- has no fd to map, so it is simply not deferred.
 * That is the right answer on its merits as well: those are four bytes and a
 * few kilobytes respectively, and a worker thread has nothing to offer them.
 */
static bool az_avk_shm_job_start(struct az_avk_buffer *entry) {
	if (entry->job != NULL || entry->image == NULL || entry->failed) {
		return false;
	}
	if (!az_avk_shm_async_enabled()) {
		return false;
	}

	uint32_t format = 0;
	uint32_t stride = 0;
	const void *data = az_shm_source_get(&entry->shm_source, entry->buffer,
		&stride, &format);
	if (data == NULL) {
		/* Not a mappable wl_shm buffer. The synchronous path handles it,
		 * exactly as it did before any of this existed. */
		return false;
	}

	avk.shm_committed_pixels +=
		(uint64_t)entry->buffer->width * (uint64_t)entry->buffer->height;

	struct avk_upload_plan plan;
	enum az_shm_plan_result res = az_avk_shm_plan(entry, stride, &plan);
	if (res == AZ_SHM_PLAN_NOTHING) {
		avk.shm_upload_skips++;
		entry->uploaded_generation = entry->content_generation;
		return false;
	}
	if (res == AZ_SHM_PLAN_FAILED) {
		return false;
	}
	uint32_t slot = 0;
	struct avk_upload *up = az_avk_shm_staging_take(entry, plan.total, &slot);
	if (up == NULL) {
		return false;
	}

	struct avk_upload_job *job = calloc(1, sizeof(*job));
	if (job == NULL) {
		return false;
	}
	job->plan = plan;
	job->src = data;
	job->dst = up->mapped;
	job->user = entry;

	/*
	 * The damage is cleared HERE, not when the copy lands.
	 *
	 * The plan already carries it. A commit arriving while the job runs unions
	 * its own damage into an empty region, which is then exactly what is still
	 * owed -- and the alternative, clearing on completion, would throw that
	 * newer damage away and freeze the surface on the generation the job
	 * happened to catch.
	 */
	pixman_region32_clear(&entry->pending_damage);
	entry->pending_full = false;
	entry->job = job;
	entry->job_generation = entry->content_generation;
	/* Committed to only now that the copy is certainly happening; see
	 * az_avk_shm_staging_take() on why a failed attempt must not advance it. */
	entry->job_slot = slot;
	entry->upload_slot = slot;
	entry->upload_ns = az_avk_now_ns();
	/* Released in az_avk_shm_job_release(); see the comment on
	 * az_avk_buffer.job for why the lock is necessary and the data-ptr access
	 * is not. */
	wlr_buffer_lock(entry->buffer);
	wl_list_insert(&avk.shm_jobs, &entry->job_link);
	avk.shm_async_jobs++;
	avk.shm_async_bytes += plan.bytes;
	avk_upload_worker_post(avk.upload_worker, job);
	return true;
}

/*
 * Upload a CPU-backed buffer into its cached image, allocating the image on
 * first sight, ON THIS THREAD.
 *
 * The fallback and the BEFORE arm both. Everything the worker path does is
 * here too, in the same order, with the memcpy in the middle instead of on
 * another thread -- which is the point: the two paths share the plan, the
 * ordering and the accounting, so what differs between them is only which
 * thread pays for the bytes.
 *
 * The awkward case, and the reason this function can succeed without reading
 * anything: wlroots wraps a client's buffer in a wlr_client_buffer, uploads it
 * into a wlr_texture, and then lets the client have the original back. Once
 * that happens the wlr_client_buffer answers "not a dma-buf" AND "not
 * CPU-readable" -- the only surviving copy of the pixels is inside a
 * wlr_texture, which is precisely the object this renderer may not touch.
 *
 * A statically-drawn surface -- a wallpaper, a menu, an icon -- hits that on
 * its second frame and every frame after. Failing there would blank exactly
 * the content that never changes. So: upload while the source is readable,
 * and keep the image we already have when it is not. That is correct for
 * static content and stale-by-at-most-nothing for it, because content that
 * does not change cannot go stale.
 */
static bool az_avk_upload_shm(struct az_avk_buffer *entry) {
	/*
	 * Timed step by step because the whole of it came out at 30ms for a 42x41
	 * buffer -- seven kilobytes. That is not copying, it is waiting, and the
	 * candidates fail differently: mapping the client's memory, taking a
	 * staging slot (which can wait on the GPU), the copy itself (measured at
	 * 8us across 113 of them), and the submit.
	 */
	uint64_t t_map = 0, t_take = 0, t_pack = 0, t_submit = 0;
	uint64_t step_t0 = az_avk_now_ns();
	void *data = NULL;
	uint32_t format = 0;
	size_t stride = 0;
	if (!wlr_buffer_begin_data_ptr_access(entry->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		if (entry->image != NULL) {
			if (!avk.warned_shm_source_gone) {
				avk.warned_shm_source_gone = true;
				wlr_log(WLR_INFO, "AVK: wlroots has taken a CPU buffer back "
					"from us after uploading it to a texture; AVK keeps the "
					"copy it made, so static content stays correct and "
					"changing content would freeze");
			}
			return true;
		}
		/* Said out loud. A buffer that is neither a dma-buf nor CPU-readable
		 * and that we have never managed to copy has no route onto the GPU at
		 * all, and the whole point of this subsystem is that such a buffer
		 * produces a diagnosis rather than a blank window. */
		wlr_log(WLR_ERROR, "AVK: a %dx%d buffer is neither a dma-buf nor "
			"CPU-readable and was never copied; it cannot be drawn",
			entry->buffer->width, entry->buffer->height);
		return false;
	}

	bool ok = false;
	if (entry->image == NULL) {
		if (!az_avk_shm_image_create(entry, format)) {
			goto out;
		}
	}

	avk.shm_committed_pixels +=
		(uint64_t)entry->buffer->width * (uint64_t)entry->buffer->height;

	struct avk_upload_plan plan;
	enum az_shm_plan_result res = az_avk_shm_plan(entry, (uint32_t)stride,
		&plan);
	if (res == AZ_SHM_PLAN_NOTHING) {
		wlr_buffer_end_data_ptr_access(entry->buffer);
		avk.shm_upload_skips++;
		return true;
	}
	if (res == AZ_SHM_PLAN_FAILED) {
		goto out;
	}
	/* Alternating here too. The synchronous path packs and submits back to
	 * back, so its window is narrow, but it is the same host-write-over-a-
	 * GPU-read and it is the path a single-pixel or unmappable buffer always
	 * takes -- the two paths are not allowed to differ about this. */
	uint32_t slot = 0;
	t_map = az_avk_now_ns() - step_t0;
	step_t0 = az_avk_now_ns();
	struct avk_upload *up = az_avk_shm_staging_take(entry, plan.total, &slot);
	t_take = az_avk_now_ns() - step_t0;
	step_t0 = az_avk_now_ns();
	if (up == NULL) {
		goto out;
	}
	entry->upload_slot = slot;
	entry->upload_ns = az_avk_now_ns();

	/* THE BLOCK. Timed rather than described, because a claim about how long
	 * the event loop is held is worth nothing without the number. */
	uint64_t t0 = az_avk_now_ns();
	avk_upload_pack(&plan, data, up->mapped);
	az_avk_note_copy_ns(az_avk_now_ns() - t0);
	avk.shm_sync_copies++;
	t_pack = az_avk_now_ns() - step_t0;
	step_t0 = az_avk_now_ns();

	VkSemaphoreSubmitInfo wait;
	uint32_t wait_count = az_avk_shm_wait(entry, &wait);
	ok = avk_upload_submit_packed(avk.device, &avk.importer.upload_ring,
		up, entry->image, &plan, &wait, wait_count) != 0;
	t_submit = az_avk_now_ns() - step_t0;
	if (t_map + t_take + t_pack + t_submit > 5000000ull) {
		avk_log(AVK_ERROR, "slow sync upload of %dx%d: map %" PRIu64
			"us + take %" PRIu64 "us + pack %" PRIu64 "us + submit %" PRIu64
			"us", entry->buffer->width, entry->buffer->height,
			t_map / 1000, t_take / 1000, t_pack / 1000, t_submit / 1000);
	}
	if (ok) {
		az_avk_shm_account(entry, &plan);
		pixman_region32_clear(&entry->pending_damage);
		entry->pending_full = false;
		az_avk_surface_note_good(entry);
	}
	az_avk_shm_staging_release(entry, slot);

out:
	wlr_buffer_end_data_ptr_access(entry->buffer);
	return ok;
}

/*
 * Bring an entry's image up to its committed content, whatever it takes.
 *
 * The frame path's entry point, and the one place that decides between joining
 * a copy already in flight and doing one here. Both outcomes are possible on a
 * healthy system: a client that commits in the same dispatch round as the
 * frame gives the worker no time at all.
 */
/*
 * The NEWEST generation this surface has actually got onto the GPU, wherever it
 * lives -- `entry`'s own image, a sibling in the same pool, or the fallback the
 * surface kept.
 *
 * A client that allocates a FRESH wl_buffer per generation -- which GDK does,
 * at 56MB a time -- gives every commit a buffer with no image and no uploaded
 * generation, so "draw the previous generation instead of waiting" had nothing
 * to draw and waited anyway. That was half the remaining stalls: 56 frames took
 * the non-blocking path while 109 joins still blocked.
 *
 * The previous generation is not gone, though. It is on a sibling buffer in the
 * same surface's pool, already uploaded, and the same size -- so it is exactly
 * the frame-old-but-correct image the blocking path was waiting to replace.
 *
 * EVERY CANDIDATE IS RANKED BY GENERATION, INCLUDING THE ENTRY'S OWN, and that
 * is the whole point of this function rather than an implementation detail.
 * `entry` is a buffer the client RECYCLED: whatever image it still holds is
 * from the last time this particular buffer was used, which for an N-buffered
 * client is a full pool rotation back and not one frame. Preferring it because
 * it happened to be attached to the entry in hand -- which is what this code
 * did -- draws content OLDER than a sibling that is sitting right there and
 * newer. On a wallpaper surface the two generations are entirely different
 * pictures, so the previous wallpaper flickered back through after the new one
 * was already up. Anything that reintroduces a "use this one because it is
 * ours" shortcut brings that back.
 *
 * Dimensions are checked rather than assumed: a surface that resized has
 * siblings of the old size, and drawing one would be a stretched window rather
 * than a stale one.
 */
static struct avk_image *az_avk_shm_newest_image(struct az_avk_buffer *entry) {
	if (entry->buffer == NULL) {
		return NULL;
	}
	struct avk_image *best = NULL;
	uint64_t best_gen = 0;

	/* A candidate, not the answer. See above. */
	if (entry->image != NULL && entry->uploaded_generation > 0) {
		best = entry->image;
		best_gen = entry->uploaded_generation;
	}

	uint32_t seen = 0;
	if (entry->pool_head != NULL) {
		struct az_avk_buffer *sib;
		wl_list_for_each(sib, entry->pool_head, pool_link) {
			seen++;
			if (sib == entry || sib->image == NULL
					|| sib->uploaded_generation == 0
					|| sib->buffer == NULL) {
				continue;
			}
			if (sib->buffer->width != entry->buffer->width
					|| sib->buffer->height != entry->buffer->height) {
				continue;
			}
			if (sib->uploaded_generation > best_gen) {
				best_gen = sib->uploaded_generation;
				best = sib->image;
			}
		}
	} else if (best == NULL) {
		avk.shm_stale_no_pool++;
	}

	/*
	 * The surface's own fallback, which outlives the pool: a client that
	 * allocates a fresh buffer per generation and destroys the old one leaves a
	 * pool full of buffers that are all still being copied, and the generation
	 * that finished has already gone. The image it produced has not gone,
	 * because the surface kept it.
	 *
	 * Ranked like everything else, so it wins only when it is genuinely newer.
	 */
	uint64_t lg_gen = 0;
	struct avk_image *lg = az_avk_surface_last_good(entry, &lg_gen);
	if (lg != NULL && (best == NULL || lg_gen > best_gen)) {
		avk.shm_stale_from_surface++;
		return lg;
	}
	/* How big the pool was when the answer was no. A pool of one is a client
	 * whose old buffers are already gone; a pool of many that still yields
	 * nothing means the siblings are all unuploaded too, and the copies are
	 * simply not keeping up. */
	if (best == NULL) {
		avk.shm_stale_no_sibling++;
		avk.shm_stale_pool_size_sum += seen;
		avk.shm_stale_pool_samples++;
	}
	return best;
}

static struct avk_image *az_avk_shm_bring_current(struct az_avk_buffer *entry) {
	if (entry->job != NULL) {
		/*
		 * ── THE FRAME DOES NOT WAIT FOR A COPY ───────────────────────────
		 *
		 * Blocking here is what put the memcpy back on the critical path
		 * after it had been moved off it. One Firefox surface commits 56MB a
		 * generation; a 56MB block streams at about 1GB/s, so the frame that
		 * needed it waited ~50ms, and 155 such frames in one session came to
		 * 9.2 seconds of blocked event loop. libinput reported the same
		 * numbers back as input lag.
		 *
		 * If the surface has a generation on the GPU anywhere, drawing the
		 * NEWEST one is a far better answer than a compositor that stops for
		 * three vblanks. The copy lands on the worker's eventfd and the settle
		 * repaints -- see `drew_stale`.
		 *
		 * Newest, not nearest. An earlier version took whatever image the entry
		 * in hand already had, reasoning that it was "a frame old and
		 * correct-as-of-then". It is not a frame old: the entry is a RECYCLED
		 * client buffer, so its image is a full pool rotation behind, and that
		 * shortcut drew content older than a sibling standing right next to it.
		 * See az_avk_shm_newest_image().
		 *
		 * The image having NO content is the one case that must still wait:
		 * there is nothing to draw, and drawing nothing is a black window.
		 */
		if (!avk_upload_job_done(entry->job)
				&& !az_avk_env_flag("AZ_AVK_NO_STALE")) {
			/* AZ_AVK_NO_STALE: block instead of showing a previous
			 * generation. Diagnostic only -- it puts the copy back on the
			 * critical path, which is the whole thing this avoids. It exists
			 * so "did a capture catch a stale frame" is answerable. */
			struct avk_image *stale = az_avk_shm_newest_image(entry);
			if (stale != NULL) {
				entry->drew_stale = true;
				entry->stale_output = avk.frame_output;
				entry->stale_box_valid = false;
				avk.frame_stale_entry = entry;
				avk.shm_stale_frames++;
				return stale;
			}
			/* Nothing anywhere on this surface has ever reached the GPU. There
			 * is no previous frame to show, and showing nothing is a black
			 * window, so this one waits. */
		}
		az_avk_shm_job_settle(entry, true);
		if (entry->uploaded_generation == entry->content_generation) {
			return entry->image;
		}
		/* A newer generation landed while that one was in flight. Nothing is
		 * in flight for it, so it is this thread's to do. */
	}
	if (!az_avk_upload_shm(entry)) {
		return NULL;
	}
	entry->uploaded_generation = entry->content_generation;
	return entry->image;
}

/*
 * Settle every copy the worker has finished. Never waits.
 *
 * Driven by the worker's eventfd, so the submission happens in the dispatch
 * round the copy completed in and the client gets its buffer back without
 * waiting for a frame. Also called at the top of a frame, because an eventfd
 * that fires while the loop is busy elsewhere would otherwise leave the pixels
 * sitting in staging until the next wakeup.
 */
static void az_avk_shm_drain(void) {
	/*
	 * Rescanned from the head rather than walked, even with the _safe form of
	 * the macro.
	 *
	 * Settling a job gives back the wlr_buffer_lock() it was holding, and that
	 * unlock can destroy the buffer -- which destroys its entry, and could in
	 * principle destroy another entry with it. `_safe` protects the CURRENT
	 * element and the NEXT one; it does not protect a cursor from an element
	 * two along being freed. The list is one entry per surface with a copy in
	 * flight, so the rescan is over single digits.
	 */
	bool progress = true;
	while (progress) {
		progress = false;
		struct az_avk_buffer *entry;
		wl_list_for_each(entry, &avk.shm_jobs, job_link) {
			if (entry->job != NULL && avk_upload_job_done(entry->job)) {
				bool owed = entry->drew_stale;
				entry->drew_stale = false;
				/* Read before the settle; cleared after the repaint below. */
				az_avk_shm_job_settle(entry, false);
				if (owed) {
					/*
					 * A frame showed the previous generation and consumed the
					 * damage that described this one. Nothing else will ask
					 * for a repaint -- a client that committed once and went
					 * quiet would sit a generation stale forever -- so the
					 * repaint is asked for here.
					 *
					 * ONLY on the output that actually drew it. The first
					 * version damaged every monitor, on the grounds that the
					 * entry did not know which one it had reached. It does
					 * now, and the difference is measurable: a window
					 * spanning two outputs went from redrawing partially to
					 * redrawing fully, because the output that never showed
					 * the surface was being told its whole surface changed.
					 * avk-crossoutput-border-test.sh asserts partial > full
					 * and read 115 against 174.
					 *
					 * Still whole-output rather than the surface's own
					 * region: AVK has no path from a wlr_surface to its scene
					 * node, so the region is not available here. Bounded by
					 * how often a copy outruns a frame on that one output.
					 */
					Monitor *dm;
					uint32_t paid = 0;
					/* AZ_BREAK_STALE_ONE_OUTPUT: pay only the output
					 * recorded last, which is the defect this rule
					 * replaced. The break exists so the fixture can be
					 * seen to fail. */
					const bool one_only =
						az_avk_env_flag("AZ_BREAK_STALE_ONE_OUTPUT");
					wl_list_for_each(dm, &mons, link) {
						if (dm->scene_output == NULL || dm->wlr_output == NULL
								|| !dm->wlr_output->enabled) {
							continue;
						}
						if (entry->stale_box_valid) {
							/*
							 * EVERY OUTPUT THE RECTANGLE COVERS, which is not
							 * the same as "the output that drew it".
							 *
							 * `stale_output` is ONE pointer, and one entry can
							 * draw stale on several outputs in a single frame
							 * cycle -- a surface straddling a monitor edge does
							 * it every time. The later draw overwrote the
							 * earlier, so the first output's repaint debt was
							 * dropped and its pixels stayed as they were until
							 * something unrelated damaged them. A wallpaper
							 * covering both monitors came back on one of them
							 * and stayed until a window closed over it: the
							 * content was never wrong, it was simply never
							 * asked for again.
							 *
							 * Layout coords on both sides, so this intersection
							 * is the whole test for "did this output show it",
							 * and an output the surface never reached takes
							 * nothing.
							 */
							if (one_only && dm->wlr_output != entry->stale_output) {
								continue;
							}
							if (entry->stale_box.x >= dm->m.x + dm->m.width
									|| entry->stale_box.y
										>= dm->m.y + dm->m.height
									|| entry->stale_box.x
										+ entry->stale_box.width <= dm->m.x
									|| entry->stale_box.y
										+ entry->stale_box.height <= dm->m.y) {
								continue;
							}
							/*
							 * The rectangle it was drawn at, in this output's
							 * buffer pixels: layout coords less the output's
							 * origin, times its scale.
							 *
							 * Whole-output damage here cost a real regression
							 * twice. avk-crossoutput-border-test.sh caught it
							 * across outputs (partial 115 against full 174)
							 * and avk-rounded-persist-test.sh caught what was
							 * left on a single output (partial 171 against
							 * full 196) -- a late copy was making every frame
							 * a full redraw.
							 */
							double sc = dm->wlr_output->scale;
							struct wlr_box b = {
								.x = (int)((entry->stale_box.x - dm->m.x) * sc),
								.y = (int)((entry->stale_box.y - dm->m.y) * sc),
								.width = (int)(entry->stale_box.width * sc) + 1,
								.height = (int)(entry->stale_box.height * sc) + 1,
							};
							wlr_damage_ring_add_box(
								&dm->scene_output->damage_ring, &b);
						} else {
							/*
							 * No box was recorded -- the node had no layout
							 * coords, so there is no rectangle to intersect and
							 * no way to tell which outputs showed it. Whole
							 * output, and ONLY the one recorded: without a box
							 * this is the every-monitor damage that
							 * avk-crossoutput-border-test.sh exists to catch, so
							 * it stays narrow at the cost of being incomplete on
							 * the rare surface that has no coords AND spans two
							 * outputs.
							 */
							if (dm->wlr_output != entry->stale_output) {
								continue;
							}
							wlr_damage_ring_add_whole(
								&dm->scene_output->damage_ring);
						}
						wlr_output_schedule_frame(dm->wlr_output);
						paid++;
					}
					if (paid >= 2) {
						avk.shm_stale_multi_output_repaints++;
					}
					entry->stale_box_valid = false;
				}
				progress = true;
				break;
			}
		}
	}
}

/* Settle everything, waiting. For teardown and for the paths that must not
 * leave a client's buffer locked behind them. */
static void az_avk_shm_drain_blocking(void) {
	/* Always the head, for the reason in az_avk_shm_drain(). Terminates
	 * because settling removes that entry from this list. */
	while (!wl_list_empty(&avk.shm_jobs)) {
		struct wl_list *node = avk.shm_jobs.next;
		struct az_avk_buffer *entry = wl_container_of(node, entry, job_link);
		az_avk_shm_job_settle(entry, true);
		if (avk.shm_jobs.next == node) {
			/* Cannot happen -- settling with block=true always releases -- but
			 * a list this loop cannot shorten would hang the compositor at
			 * shutdown, and that is not a thing to leave to an argument. The
			 * comparison is of pointers only; `entry` may already be freed. */
			wlr_log(WLR_ERROR, "AVK: an in-flight SHM copy would not settle");
			break;
		}
	}
}

static int az_avk_upload_worker_readable(int fd, uint32_t mask, void *data) {
	(void)mask;
	(void)data;
	uint64_t count = 0;
	while (read(fd, &count, sizeof(count)) < 0 && errno == EINTR) {
		/* retry */
	}
	az_avk_shm_drain();
	return 0;
}

/*
 * Take this client buffer's acquire fence, once per frame.
 *
 * Explicit first: a client using wp_linux_drm_syncobj_v1 states an acquire
 * timeline point and deliberately attaches NO implicit fence to the dma-buf,
 * so reading the dma-buf's fences would find nothing and the wait would be
 * silently skipped -- the exact failure this protocol exists to make
 * impossible. Implicit second, for every client that does not use it.
 *
 * The fd is owned from here until the frame imports it.
 */
static void az_avk_collect_acquire(struct az_avk_buffer *entry) {
	if (entry == NULL || entry->is_shm || avk.frame_output == NULL) {
		return;
	}
	/* Once per frame per buffer: the same buffer resolves again on a second
	 * output and on every cache hit, and a sync_file may be waited on once. */
	if (entry->acquire_frame == avk.frames + 1) {
		return;
	}
	entry->acquire_frame = avk.frames + 1;

	int fence = -1;
	bool explicit_point = false;
	struct wlr_surface *surface = az_avk_surface_wlr(entry);
	if (surface != NULL) {
		struct wlr_linux_drm_syncobj_surface_v1_state *st =
			wlr_linux_drm_syncobj_v1_get_surface_state(surface);
		if (st != NULL) {
			/* Owed a release point whether or not it stated an acquire one. */
			if (avk.frame_surface_count < AZ_AVK_MAX_ACQUIRE) {
				avk.frame_surfaces[avk.frame_surface_count++] = surface;
			} else {
				avk.release_points_dropped++;
			}
		}
		if (st != NULL && st->acquire_timeline != NULL) {
			fence = wlr_drm_syncobj_timeline_export_sync_file(
				st->acquire_timeline, st->acquire_point);
			explicit_point = true;
		}
	}
	if (fence < 0 && !explicit_point) {
		struct wlr_dmabuf_attributes dmabuf;
		if (wlr_buffer_get_dmabuf(entry->buffer, &dmabuf)
				&& dmabuf.n_planes >= 1) {
			/* As a READER: we want whatever wrote it to be finished. Asking
			 * as a writer would also return other readers' fences, which we
			 * have no reason to wait for. */
			if (!avk_sync_dmabuf_fences(dmabuf.fd[0], DMA_BUF_SYNC_READ,
					&fence)) {
				fence = -1;
			}
		}
	}
	if (fence < 0) {
		/* Nothing to wait for: no fence on the buffer means nothing is
		 * writing it. Not an error and not counted as one. */
		return;
	}
	if (avk.frame_acquire_count >= AZ_AVK_MAX_ACQUIRE) {
		/* Said out loud rather than dropped quietly: past this point the
		 * frame is submitted without waiting for a client that is still
		 * drawing, which is the very thing this exists to prevent. */
		avk.acquire_dropped++;
		close(fence);
		return;
	}
	avk.frame_acquire_fds[avk.frame_acquire_count++] = fence;
	if (explicit_point) {
		avk.acquire_explicit++;
	} else {
		avk.acquire_implicit++;
	}
}

/*
 * The avk_image for a wlr_buffer, importing it on first sight.
 *
 * Returns NULL when the buffer cannot be represented on the GPU at all, having
 * said why. The caller drops the command rather than drawing black -- a black
 * rectangle where a window should be is the failure mode this whole subsystem
 * exists to remove, and reproducing it in the new path would be absurd.
 *
 * `async` says the caller is the COMMIT handler, which is the one caller that
 * is holding up the event loop and the one that may hand the copy to the
 * worker. Every other caller is the frame, which needs the pixels now.
 */
static struct avk_image *az_avk_image_for_buffer_ex(struct wlr_buffer *buffer,
		bool async) {
	struct wlr_addon *addon = wlr_addon_find(&buffer->addons, &avk,
		&az_avk_buffer_addon_impl);
	struct az_avk_buffer *entry = NULL;
	if (addon != NULL) {
		entry = wl_container_of(addon, entry, addon);
		if (entry->failed) {
			/* Remembered, so a buffer nothing can import costs one log line
			 * and one pointer comparison rather than a fresh diagnosis at
			 * every refresh. */
			return NULL;
		}
		avk.cache_hits++;
		if (entry->is_shm) {
			entry->stat_lookups++;
			/*
			 * Lookup, not upload. The copy happens only for a generation the
			 * GPU does not already hold -- see content_generation.
			 *
			 * Two break switches, one for each way this can be wrong:
			 *
			 *   AZ_AVK_UPLOAD_ON_LOOKUP=1 restores the unconditional upload.
			 *   Correct pixels, 924 GB a session.
			 *
			 *   AZ_AVK_CACHE_BY_IDENTITY=1 caches on the buffer pointer and
			 *   never re-uploads. Free, and permanently wrong the moment a
			 *   client reuses a wl_buffer -- which looks fine against every
			 *   toolkit that rotates a buffer pool, and is why
			 *   contrib/wlreuse exists.
			 */
			if (az_avk_env_flag("AZ_AVK_CACHE_BY_IDENTITY")) {
				avk.shm_upload_skips++;
				return entry->image;
			}
			if (entry->uploaded_generation == entry->content_generation
					&& entry->job == NULL
					&& !az_avk_env_flag("AZ_AVK_UPLOAD_ON_LOOKUP")) {
				avk.shm_upload_skips++;
				return entry->image;
			}
			/* May be a SIBLING's image -- the previous generation of the
			 * same surface -- when this buffer's copy is still in flight. */
			return az_avk_shm_bring_current(entry);
		}
		az_avk_collect_acquire(entry);
		return entry->image;
	}
	avk.cache_misses++;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return NULL;
	}
	entry->buffer = buffer;
	/* Empty means "in no pool"; az_avk_pool_add() and the destroy path both
	 * rely on being able to ask. */
	wl_list_init(&entry->pool_link);
	/* Likewise for the in-flight-copy list: a zeroed wl_list is not an empty
	 * one, and wl_list_remove() on it writes through a NULL. */
	wl_list_init(&entry->job_link);
	pixman_region32_init(&entry->pending_damage);
	/* Nothing is on the GPU yet, so the first upload is necessarily whole. */
	entry->pending_full = true;
	wlr_addon_init(&entry->addon, &buffer->addons, &avk,
		&az_avk_buffer_addon_impl);
	entry->src_width = (uint32_t)buffer->width;
	entry->src_height = (uint32_t)buffer->height;
	wl_list_insert(&avk.buffers, &entry->link);
	avk.client_images_cached++;

	uint64_t resolve_start_ns = az_avk_now_ns();
	struct az_buffer_source source;
	az_buffer_get_source(buffer, &source);
	switch (source.kind) {
	case AZ_BUFFER_DMABUF: {
		struct avk_dmabuf_attributes attribs;
		az_avk_attribs_from_wlr(&attribs, &source.dmabuf);
		/* The importer dup()s every fd it keeps, so from here on AVK's copy of
		 * the content survives the client destroying its wl_buffer. */
		entry->image = avk_dmabuf_import(&avk.importer, &attribs, false);
		break;
	}
	case AZ_BUFFER_DATA_PTR:
		/* Generation 1 is "the contents as they were when we first saw this
		 * buffer". Copying it is the one copy that is caused by a lookup, and
		 * it has to be: there is nothing on the GPU yet. */
		entry->content_generation = 1;
		if (async) {
			/*
			 * FIRST SIGHT IS THE HOT PATH FOR THE CLIENT THAT PRODUCED THIS
			 * BUG, and it took a perf trace to notice.
			 *
			 * A client that reuses one wl_buffer takes the "already ours"
			 * branch of the commit handler, which costs nothing; the copy then
			 * happens at frame time. A client that presents a buffer AVK has
			 * not seen -- which Qt does, continuously -- lands here instead,
			 * inside the commit handler, and paid for the whole 7.6 MB there.
			 * That is the call chain the profile showed:
			 * wl_signal_emit_mutable -> az_avk_surface_commit ->
			 * az_avk_image_for_buffer -> az_avk_upload_shm -> memcpy.
			 *
			 * So the async path has to cover it. The image is created empty
			 * and the copy is posted; the generation is deliberately NOT
			 * marked uploaded, so the frame that eventually samples this
			 * buffer settles the job first.
			 */
			void *data = NULL;
			uint32_t format = 0;
			size_t stride = 0;
			if (wlr_buffer_begin_data_ptr_access(buffer,
					WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format,
					&stride)) {
				bool made = az_avk_shm_image_create(entry, format);
				wlr_buffer_end_data_ptr_access(buffer);
				if (made && az_avk_shm_job_start(entry)) {
					break;
				}
			}
			/* Nothing was posted -- unreadable, unknown format, or the worker
			 * declined. Fall through to the copy on this thread rather than
			 * leaving an image with no contents. */
		}
		if (!az_avk_upload_shm(entry)) {
			entry->image = NULL;
		} else {
			entry->uploaded_generation = 1;
		}
		break;
	case AZ_BUFFER_NONE:
		wlr_log(WLR_ERROR, "AVK: a %dx%d buffer is neither a dma-buf nor "
			"CPU-readable; nothing can draw it", buffer->width,
			buffer->height);
		entry->image = NULL;
		break;
	}

	{
		/*
		 * WHICH BRANCH, WHEN IT IS SLOW. The walk's resolve was timed as a
		 * whole and came out owning a 38ms frame; standalone, creating the
		 * 59MB image it builds takes 0.02ms, so the cost is not the creation
		 * and the breakdown has to say what it is instead.
		 */
		uint64_t resolve_us = (az_avk_now_ns() - resolve_start_ns) / 1000;
		if (resolve_us > 5000) {
			avk_log(AVK_ERROR, "slow resolve: %s %dx%d took %" PRIu64
				"us (async=%d)",
				source.kind == AZ_BUFFER_DMABUF ? "dmabuf"
					: source.kind == AZ_BUFFER_DATA_PTR ? "shm" : "none",
				buffer->width, buffer->height, resolve_us, (int)async);
		}
	}

	if (entry->image == NULL) {
		avk.buffer_import_fails++;
		/* The entry stays, holding the failure. Destroying it would put the
		 * buffer back to "never seen", and the next frame would run the whole
		 * import and log the whole diagnosis again. */
		entry->failed = true;
		return NULL;
	}
	avk.buffer_imports++;
	/* A first-sight buffer needs its acquire fence as much as a cached one --
	 * more so, since a client's very first frame is the one most likely to be
	 * still in flight when the compositor reaches for it. */
	az_avk_collect_acquire(entry);
	return entry->image;
}

/* The frame path's spelling: it needs the pixels now, so it never defers. */
static struct avk_image *az_avk_image_for_buffer(struct wlr_buffer *buffer) {
	return az_avk_image_for_buffer_ex(buffer, false);
}

/* ── output targets ─────────────────────────────────────────────────────── */

/*
 * The swapchain AVK renders into, and its buffers' imported images.
 *
 * A swapchain of our own rather than the output's: wlr_output's swapchain is
 * created for, and reallocated by, wlr_scene's own build path, and sharing it
 * would mean the two paths silently fight over buffer lifetime the first time
 * a frame falls back.
 */
/*
 * Where a scan-out target is in its cycle.
 *
 * The states exist so that "may I render into this?" is a question with an
 * answer rather than an assumption. wlr_swapchain will only hand back a buffer
 * nobody holds a lock on, and until now that was the entire argument for
 * reusing it -- an argument that is true of the CPU's view and says nothing
 * about whether the display engine has finished reading the pixels. On an
 * implicitly synchronised path the kernel covered the difference. On an
 * explicitly synchronised one nothing does, and rendering into a buffer the
 * scanout engine is still reading produces tearing that looks like a damage
 * bug.
 */
enum az_avk_target_state {
	/* Never used, or the display engine has signalled it is done. */
	AZ_AVK_TARGET_FREE,
	/* A render was submitted into it but it never reached a commit -- a
	 * rejected output state, or a frame we abandoned. Safe to render into
	 * again: both submissions are on the same queue and therefore ordered. */
	AZ_AVK_TARGET_RENDERED,
	/* Handed to the display engine. Must not be rendered into until its
	 * release point is signalled. */
	AZ_AVK_TARGET_IN_FLIGHT,
};

struct az_avk_target {
	struct wlr_addon addon;
	struct avk_image *image;

	enum az_avk_target_state state;
	/* The point on the output's release timeline that frees this target, or 0
	 * when the release travels through the buffer's own dma-buf fences. */
	uint64_t release_point;
};

static void az_avk_target_addon_destroy(struct wlr_addon *addon) {
	struct az_avk_target *target = wl_container_of(addon, target, addon);
	if (target->image != NULL) {
		avk_retire_push(&avk.importer.retire, avk.device,
			target->image->last_use, avk_image_destroy, target->image);
		if (avk.output_targets > 0) {
			avk.output_targets--;
		}
	}
	wlr_addon_finish(&target->addon);
	free(target);
}

static const struct wlr_addon_interface az_avk_target_addon_impl = {
	.name = "az_avk_target",
	.destroy = az_avk_target_addon_destroy,
};

/*
 * How this output's finished frame reaches the display engine with a fence
 * attached to it.
 *
 * Two mechanisms, chosen from what the backend can actually do rather than
 * from what is nicer:
 *
 *   TIMELINE  The backend supports drm_syncobj timelines in an output commit.
 *             AVK exports its render completion as a sync_file, imports it at
 *             a point on a timeline, and hands the point to the output state.
 *             KMS waits on it in the kernel, and signals a second timeline
 *             when it has finished with the buffer. This is the good path: two
 *             fences, both directions covered, nothing polled.
 *
 *   DMABUF    The backend has no timeline support -- the headless backend has
 *             no DRM device at all, and a DRM backend on a kernel without
 *             DRM_CAP_SYNCOBJ_TIMELINE has no timelines either. The fence is
 *             instead attached to the target's own dma-buf reservation object,
 *             where any implicitly synchronised consumer will find it. The
 *             release direction comes back out of the same object.
 *
 * There is deliberately no third mechanism. If neither works the output is
 * declined and SceneFX renders it, because the alternative -- present anyway
 * and hope the driver is inserting fences on our behalf -- is exactly the
 * assumption this milestone exists to remove.
 */
enum az_avk_present_sync {
	AZ_AVK_PRESENT_SYNC_UNSET,
	AZ_AVK_PRESENT_SYNC_TIMELINE,
	AZ_AVK_PRESENT_SYNC_DMABUF,
	AZ_AVK_PRESENT_SYNC_BROKEN,
};

struct az_avk_output {
	struct wlr_swapchain *swapchain;
	/* WHAT THE SWAPCHAIN WAS ALLOCATED AT -- an attachment extent, and typed
	 * so that a presentation extent cannot be stored here by accident. This
	 * is the field a reallocation is decided against, so getting the space
	 * wrong here reallocates on every frame at best. */
	struct az_attachment_extent att;
	uint32_t drm_format;
	VkFormat vk_format;
	struct az_avk_renderer_slot *slot;

	/* The Vulkan end of the fence bridge. Per output rather than per device:
	 * two monitors run two independent frame cadences, and one shared export
	 * semaphore would have each one exporting the other's frame. */
	struct avk_sync sync;
	bool sync_ready;

	enum az_avk_present_sync present_sync;
	/* AVK -> display. A point is signalled by importing the frame's sync_file
	 * and handed to the output state as its wait point. */
	struct wlr_drm_syncobj_timeline *in_timeline;
	uint64_t in_point;
	/* display -> AVK. KMS signals a point when it has released the buffer. */
	struct wlr_drm_syncobj_timeline *out_timeline;
	uint64_t out_point;

	/* The renderer's running submit total as of this output's last frame,
	 * so a per-frame delta can be taken from it. */
	uint64_t last_submit_ns;

	/*
	 * M4I. THE MONITOR BACKGROUND BLUR RESULT CACHE, per output.
	 *
	 * Per output and never shared: two displays differ in resolution, scale,
	 * transform, format and -- under M5 -- colour domain, and every one of those
	 * is part of this image's identity. A cache keyed on anything less would
	 * hand a 1080p monitor the 4K monitor's blurred wallpaper.
	 *
	 * `generation` is the compositor's side: incremented whenever the scene
	 * marks the node dirty, never decremented, never cleared. The renderer
	 * holds its own copy of the number it last built at, and the cache is valid
	 * exactly while the two agree. See avk_blur_cache in avk_render.h for the
	 * rest of the validity contract -- geometry, parameters and format.
	 */
	uint64_t blur_cache_generation;
	struct avk_blur_cache blur_cache;

	/*
	 * M5/C6. THE PATH-B WORKING INTERMEDIATE, per output for the same reasons
	 * the blur cache is: it is the output's own extent, and it holds the
	 * PREVIOUS frame's composite outside this frame's damage, which is what
	 * makes the encode pass damage-scissorable at all.
	 *
	 * Empty on Path A, where there is no encode pass and composition goes
	 * straight into the scan-out buffer through its _SRGB view.
	 */
	struct avk_encode_intermediate encode_work;

	/*
	 * M6B/G2. THE OUTPUT'S MEASURED ENCODE CURVE, as a texture.
	 *
	 * Per output for the plainest possible reason: a display profile describes
	 * one display. Uploaded when the profile's serial moves and never on a
	 * frame path -- see avk_encode_lut.
	 */
	struct avk_encode_lut encode_lut;

	/*
	 * M6C. THE OUTPUT'S MEASURED CUBE, for the profiles that do not reduce to a
	 * matrix and a curve. Exactly one of this and `encode_lut` is ever
	 * populated for a given output, because exactly one of the two forms is
	 * derived -- but both slots exist unconditionally so that a profile
	 * CHANGING form (a cLUT file replacing a matrix-shaper one) is two serials
	 * moving rather than a union being reinterpreted mid-flight.
	 */
	struct avk_encode_clut encode_clut;

	/*
	 * M4F.2C.4c forensics. Per OUTPUT, because "frame 41" has to mean the same
	 * thing in the log and in the fixture, and the renderer's own frame counter
	 * is shared by every output using its VkFormat.
	 *
	 * `oracle_bufs` gives each scan-out buffer a stable index in first-seen
	 * order. There is no exposed slot number: wlr_damage_ring keys its history
	 * on the wlr_buffer itself, so buffer identity IS damage-history identity
	 * and one table serves for both.
	 */
	uint64_t frame_seq;
	/* Armed by `amsg dispatch capture_output`: write this output's next
	 * finished attachment to disk, then disarm. */
	bool capture_pending;
	/* The scene output's halo-damage record count as of this output's last
	 * frame, so "this frame took damage from the halo" is a delta rather than
	 * a guess about a region's extents. */
	uint64_t last_halo_records;
	struct wlr_buffer *oracle_bufs[8];
	size_t oracle_buf_len;
};

static struct az_avk_target *az_avk_target_for_buffer(
		struct wlr_buffer *buffer) {
	struct wlr_addon *addon = wlr_addon_find(&buffer->addons, &avk,
		&az_avk_target_addon_impl);
	if (addon != NULL) {
		struct az_avk_target *target = wl_container_of(addon, target, addon);
		return target;
	}

	struct wlr_dmabuf_attributes dmabuf;
	if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		wlr_log(WLR_ERROR, "AVK: the output swapchain handed back a buffer "
			"that is not a dma-buf; AVK cannot render into it");
		return NULL;
	}

	struct avk_dmabuf_attributes attribs;
	az_avk_attribs_from_wlr(&attribs, &dmabuf);
	/* for_render: a target needs COLOR_ATTACHMENT capability, which is a
	 * different question from whether it can be sampled and has a different
	 * answer on some modifiers. */
	struct avk_image *image = avk_dmabuf_import(&avk.importer, &attribs, true);
	if (image == NULL) {
		return NULL;
	}

	struct az_avk_target *target = calloc(1, sizeof(*target));
	if (target == NULL) {
		avk_image_destroy(avk.device, image);
		return NULL;
	}
	target->image = image;
	target->state = AZ_AVK_TARGET_FREE;
	wlr_addon_init(&target->addon, &buffer->addons, &avk,
		&az_avk_target_addon_impl);
	avk.output_targets++;
	return target;
}

static struct az_avk_renderer_slot *az_avk_renderer_for(VkFormat format) {
	struct az_avk_renderer_slot *free_slot = NULL;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		struct az_avk_renderer_slot *slot = &avk.renderers[i];
		if (slot->used && slot->format == format) {
			return slot;
		}
		if (!slot->used && free_slot == NULL) {
			free_slot = slot;
		}
	}
	if (free_slot == NULL) {
		wlr_log(WLR_ERROR, "AVK: more than %d output formats in use at once; "
			"no renderer available for VkFormat %d", AZ_AVK_MAX_FORMATS,
			format);
		return NULL;
	}
	if (!avk_renderer_init(&free_slot->renderer, avk.device, format)) {
		return NULL;
	}
	free_slot->used = true;
	free_slot->format = format;
	wlr_log(WLR_INFO, "AVK: renderer ready for VkFormat %d", format);
	return free_slot;
}

/*
 * The format the swapchain should allocate, as the intersection of three
 * opinions that all have to be satisfied at once:
 *
 *   - the output's, because the buffer has to be scanned out;
 *   - AVK's, because it has to be a colour attachment;
 *   - and the allocator's, which finds out by trying.
 *
 * DRM_FORMAT_MOD_INVALID is deliberately excluded. As a *target*, an implicit
 * modifier has no recovery path -- the copy rung reads pixels out of a buffer,
 * which is no use for one we are about to render into -- so allocating one
 * would produce a buffer AVK cannot attach.
 */
/*
 * M5/C3 input: can an output rendering into `fourcc` composite scene-linearly
 * through an _SRGB attachment view -- i.e. does Path A exist for it?
 *
 * FALSE WHEN AVK IS NOT THE RENDERER, and that is the right answer rather than
 * a missing one: Path A is an AVK path, so an output on the SceneFX renderer
 * does not have one to take.
 */
/* Is AVK the renderer this session? The C3 inputs that are only meaningful for
 * an AVK-driven output ask this rather than each finding their own way to it. */
static bool az_avk_is_active(void) {
	return avk.active;
}

/* Is the Vulkan validation layer actually loaded? The same value avk-stats
 * reports, reachable without parsing that response -- `get instance` answers
 * it alongside the identity, so a live gate can settle "who answered, and can
 * they see VUIDs at all" in ONE round trip. Two round trips is two chances for
 * the second to reach a different compositor than the first. */
static bool az_avk_validation_enabled(void) {
	return avk.instance != NULL && avk.instance->validation_enabled;
}

static bool az_avk_scanout_srgb_format_ok(uint32_t fourcc) {
	if (!avk.active) {
		return false;
	}
	return avk_format_table_scanout_srgb_format_ok(&avk.importer.table, fourcc);
}

static bool az_avk_pick_format(struct wlr_output *output, uint32_t fourcc,
		int width, int height, struct wlr_drm_format *out) {
	const struct avk_format_caps *caps =
		avk_format_table_find(&avk.importer.table, fourcc);
	if (caps == NULL) {
		wlr_log(WLR_ERROR, "AVK: no importable modifiers for format 0x%08x "
			"on this device", fourcc);
		return false;
	}

	/* NULL from wlr_output_get_primary_formats() means the backend has no
	 * format CONSTRAINT -- everything is allowed -- not that it supports
	 * nothing. Reading it the other way is how the headless backend, which
	 * constrains nothing at all, ends up looking like a backend that can
	 * scan out no formats whatsoever. */
	const struct wlr_drm_format_set *primary =
		wlr_output_get_primary_formats(output, WLR_BUFFER_CAP_DMABUF);
	if (primary != NULL && wlr_drm_format_set_get(primary, fourcc) == NULL) {
		wlr_log(WLR_ERROR, "AVK: output %s cannot scan out format 0x%08x",
			output->name, fourcc);
		return false;
	}

	*out = (struct wlr_drm_format){ .format = fourcc };
	for (uint32_t i = 0; i < caps->render_mod_count; i++) {
		const struct avk_modifier_caps *mc = &caps->render_mods[i];
		if (mc->modifier == DRM_FORMAT_MOD_INVALID) {
			continue;
		}
		if (primary != NULL &&
				!wlr_drm_format_set_has(primary, fourcc, mc->modifier)) {
			continue;
		}
		if ((int)mc->max_extent.width < width ||
				(int)mc->max_extent.height < height) {
			/* Not pedantry: compressed modifiers really do cap out below the
			 * resolutions people run, and a rejected commit at 4K is a much
			 * worse way to discover it. */
			continue;
		}
		uint64_t *grown = realloc(out->modifiers,
			(out->len + 1) * sizeof(*grown));
		if (grown == NULL) {
			wlr_drm_format_finish(out);
			return false;
		}
		out->modifiers = grown;
		out->modifiers[out->len++] = mc->modifier;
		out->capacity = out->len;
	}

	if (out->len == 0) {
		wlr_log(WLR_ERROR, "AVK: output %s and this GPU share no renderable "
			"modifier for format 0x%08x at %dx%d", output->name, fourcc,
			width, height);
		wlr_drm_format_finish(out);
		return false;
	}
	return true;
}

/* ── presentation synchronisation ───────────────────────────────────────── */

/*
 * Two switches, both of them for tests, both of them named for what they
 * break rather than what they enable.
 *
 * AZ_AVK_NO_PRESENT_SYNC=1 hands the frame over with no fence at all. It is
 * the break test for this whole file: with it set, `present_sync_none` climbs
 * and a test that claims to prove synchronisation had better start failing.
 *
 * AZ_AVK_NO_TIMELINE=1 pretends the backend has no drm_syncobj timeline
 * support, which forces the dma-buf reservation path. Without it that path is
 * only reachable on hardware nobody here has, and an untested fallback is a
 * fallback that does not work.
 */

/*
 * Decide, once per output, how a finished frame will carry its fence.
 *
 * Returns false when neither mechanism is available, in which case the output
 * is marked BROKEN and stays on the SceneFX path for the rest of the session.
 */
static bool az_avk_present_sync_prepare(struct az_avk_output *out,
		struct wlr_output *output) {
	if (out->present_sync == AZ_AVK_PRESENT_SYNC_BROKEN) {
		return false;
	}
	if (out->present_sync != AZ_AVK_PRESENT_SYNC_UNSET) {
		return true;
	}

	if (!out->sync_ready) {
		if (!avk_sync_init(&out->sync, avk.device, output->name)) {
			wlr_log(WLR_ERROR, "AVK: %s cannot be presented safely -- this "
				"device cannot turn a finished frame into a fence the display "
				"engine can wait on; staying on the SceneFX path",
				output->name);
			out->present_sync = AZ_AVK_PRESENT_SYNC_BROKEN;
			return false;
		}
		out->sync_ready = true;
	}

	int drm_fd = wlr_backend_get_drm_fd(output->backend);
	bool want_timeline = drm_fd >= 0 && output->backend->features.timeline
		&& !az_avk_env_flag("AZ_AVK_NO_TIMELINE");
	if (want_timeline) {
		out->in_timeline = wlr_drm_syncobj_timeline_create(drm_fd);
		out->out_timeline = wlr_drm_syncobj_timeline_create(drm_fd);
		if (out->in_timeline != NULL && out->out_timeline != NULL) {
			out->present_sync = AZ_AVK_PRESENT_SYNC_TIMELINE;
			wlr_log(WLR_INFO, "AVK: %s presents with drm_syncobj timelines; "
				"the display engine waits for the GPU in the kernel",
				output->name);
			return true;
		}
		wlr_drm_syncobj_timeline_unref(out->in_timeline);
		wlr_drm_syncobj_timeline_unref(out->out_timeline);
		out->in_timeline = NULL;
		out->out_timeline = NULL;
		wlr_log(WLR_ERROR, "AVK: %s supports timelines but they could not be "
			"created; falling back to dma-buf fences", output->name);
	}

	/* The dma-buf path cannot be probed without a real fence to attach, so it
	 * is assumed here and verified on the first frame -- see the handover.
	 * A kernel too old for the ioctl marks the output BROKEN there. */
	out->present_sync = AZ_AVK_PRESENT_SYNC_DMABUF;
	wlr_log(WLR_INFO, "AVK: %s has no drm_syncobj timeline support%s; the "
		"frame's fence travels on the target's dma-buf instead", output->name,
		az_avk_env_flag("AZ_AVK_NO_TIMELINE") ? " (forced by "
			"AZ_AVK_NO_TIMELINE)" : "");
	return true;
}

/*
 * Has the display engine finished with this target, and if not, what do we
 * wait on?
 *
 * Appends at most one wait to `waits`. Returns false only when the target is
 * genuinely unusable, which is the assertion the state machine exists for.
 */
static bool az_avk_target_acquire(struct az_avk_output *out,
		struct az_avk_target *target, struct wlr_buffer *buffer,
		VkSemaphoreSubmitInfo *waits, uint32_t *wait_count) {
	if (target->state == AZ_AVK_TARGET_FREE ||
			target->state == AZ_AVK_TARGET_RENDERED) {
		/* RENDERED is safe without a wait: the earlier submission and this one
		 * are on the same queue, and a queue orders its own work. */
		return true;
	}

	int fence = -1;
	if (out->present_sync == AZ_AVK_PRESENT_SYNC_TIMELINE) {
		bool signalled = false;
		if (wlr_drm_syncobj_timeline_check(out->out_timeline,
				target->release_point, 0, &signalled) && signalled) {
			target->state = AZ_AVK_TARGET_FREE;
			return true;
		}
		/* Not signalled. Either the display engine is still using it, or the
		 * commit that would have signalled it was rejected and the point will
		 * never materialise. Those need opposite responses, and asking is one
		 * ioctl. */
		bool available = false;
		if (!wlr_drm_syncobj_timeline_check(out->out_timeline,
				target->release_point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
				&available) || !available) {
			target->state = AZ_AVK_TARGET_FREE;
			return true;
		}
		fence = wlr_drm_syncobj_timeline_export_sync_file(out->out_timeline,
			target->release_point);
		if (fence < 0) {
			wlr_log(WLR_ERROR, "AVK: cannot export the release fence for a "
				"target still held by the display engine");
			avk.target_state_violations++;
			return false;
		}
	} else {
		/* The reservation object carries whatever anyone put on it -- the
		 * display engine's read, and also our own last write. Asking as a
		 * WRITER returns both, which is what we are about to become. Waiting
		 * on our own write is redundant, since one queue orders its own work,
		 * but it is one semaphore on a wait list rather than a stall and
		 * telling the two apart would cost more than it saves. */
		struct wlr_dmabuf_attributes dmabuf;
		if (!wlr_buffer_get_dmabuf(buffer, &dmabuf) || dmabuf.n_planes < 1) {
			avk.target_state_violations++;
			return false;
		}
		if (!avk_sync_dmabuf_fences(dmabuf.fd[0], DMA_BUF_SYNC_WRITE, &fence)) {
			avk.target_state_violations++;
			return false;
		}
		if (fence < 0) {
			/* No fences on the buffer: nobody is using it. */
			target->state = AZ_AVK_TARGET_FREE;
			return true;
		}
	}

	VkSemaphore sem = avk_sync_import_sync_file(&out->sync, fence);
	if (sem == VK_NULL_HANDLE) {
		avk.target_state_violations++;
		return false;
	}
	waits[*wait_count] = (VkSemaphoreSubmitInfo){
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = sem,
		/* The wait guards the colour attachment write, so that is the stage
		 * that has to block. Everything earlier in the pipeline may run. */
		.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	};
	(*wait_count)++;
	avk.presentation_waits++;
	target->state = AZ_AVK_TARGET_FREE;
	return true;
}

/*
 * Attach the frame's completion fence to the thing being presented.
 *
 * Called after the render has been submitted and before the output state is
 * committed. Returns false if the fence could not be attached, in which case
 * the caller must NOT present the frame: an unfenced buffer handed to a display
 * engine is a buffer that may be scanned out while the GPU is still writing it,
 * and the resulting tearing is indistinguishable from a damage-tracking bug.
 */
static bool az_avk_present_handover(struct az_avk_output *out,
		struct az_avk_target *target, struct wlr_buffer *buffer,
		struct wlr_output_state *state) {
	/* The export happens first and unconditionally, even in the break test.
	 * A binary semaphore signalled by one submission and then signalled again
	 * by the next, with nothing in between consuming the payload, is undefined
	 * behaviour -- so a break test that simply skipped the export would not be
	 * testing "no fence is handed on", it would be testing "the semaphore is
	 * misused", and it would fail for the wrong reason. Exporting and dropping
	 * the fence on the floor is exactly and only the missing handover. */
	int fence = -1;
	if (!avk_sync_export_sync_file(&out->sync, &fence)) {
		/* Not transient: the device either can export its completion or it
		 * cannot, and retrying every frame would produce one error line per
		 * vblank forever. */
		wlr_log(WLR_ERROR, "AVK: the frame's completion could not be exported "
			"as a fence; this output goes back to the SceneFX path");
		out->present_sync = AZ_AVK_PRESENT_SYNC_BROKEN;
		avk.present_sync_fails++;
		return false;
	}

	if (az_avk_env_flag("AZ_AVK_NO_PRESENT_SYNC")) {
		if (!avk.warned_no_present_sync) {
			avk.warned_no_present_sync = true;
			wlr_log(WLR_ERROR, "AVK: AZ_AVK_NO_PRESENT_SYNC=1 -- frames are "
				"being handed to the display with no fence attached. This is "
				"the break test for presentation synchronisation and it must "
				"never be set on a desktop you care about.");
		}
		if (fence >= 0) {
			close(fence);
		}
		avk.present_sync_none++;
		target->state = AZ_AVK_TARGET_RENDERED;
		return true;
	}

	if (out->present_sync == AZ_AVK_PRESENT_SYNC_TIMELINE) {
		if (fence >= 0) {
			out->in_point++;
			if (!wlr_drm_syncobj_timeline_import_sync_file(out->in_timeline,
					out->in_point, fence)) {
				wlr_log(WLR_ERROR, "AVK: the frame's fence could not be placed "
					"on the output's wait timeline");
				close(fence);
				avk.present_sync_fails++;
				return false;
			}
			close(fence);
			/*
			 * ── TELL EACH CLIENT WHEN THE READ ACTUALLY FINISHED ─────────
			 *
			 * in_timeline @ in_point is this frame's render completion --
			 * the moment the compositor stopped reading the client buffers
			 * it sampled. That is exactly the release point those clients
			 * are owed.
			 *
			 * Without this wlroots signals the release point at the client's
			 * NEXT COMMIT, gated by nothing: a client is told its buffer is
			 * free while the GPU is still reading it, and a client that
			 * believes the protocol immediately draws over the pixels being
			 * sampled. Advertising wp_linux_drm_syncobj_v1 and then doing
			 * that is worse than not advertising it, because the client
			 * stops attaching implicit fences once it sees the global.
			 */
			for (uint32_t i = 0; i < avk.frame_surface_count; i++) {
				struct wlr_linux_drm_syncobj_surface_v1_state *st =
					wlr_linux_drm_syncobj_v1_get_surface_state(
						avk.frame_surfaces[i]);
				if (st == NULL) {
					continue;
				}
				if (wlr_linux_drm_syncobj_v1_state_add_release_point(st,
						out->in_timeline, out->in_point,
						wl_display_get_event_loop(dpy))) {
					avk.release_points_set++;
				} else {
					avk.release_points_dropped++;
				}
			}
			avk.frame_surface_count = 0;
			wlr_output_state_set_wait_timeline(state, out->in_timeline,
				out->in_point);
		}
		/* The release direction is asked for whether or not there was anything
		 * to wait on: it is how the target gets back to FREE. */
		out->out_point++;
		wlr_output_state_set_signal_timeline(state, out->out_timeline,
			out->out_point);
		target->release_point = out->out_point;
		target->state = AZ_AVK_TARGET_IN_FLIGHT;
		avk.present_sync_timeline++;
		return true;
	}

	if (fence < 0) {
		/* Already complete. Nothing to attach, and nothing to wait for. */
		target->release_point = 0;
		target->state = AZ_AVK_TARGET_IN_FLIGHT;
		avk.present_sync_dmabuf++;
		return true;
	}

	struct wlr_dmabuf_attributes dmabuf;
	if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		close(fence);
		avk.present_sync_fails++;
		return false;
	}
	/* One fence, every plane: a multi-planar target is one image to the GPU
	 * and several reservation objects to the kernel, and a consumer that reads
	 * plane 1 without waiting is just as wrong as one that reads plane 0.
	 * avk_sync_dmabuf_attach consumes the fd, so each plane gets its own dup. */
	bool ok = true;
	for (int i = 0; i < dmabuf.n_planes; i++) {
		int dup = fcntl(fence, F_DUPFD_CLOEXEC, 0);
		if (dup < 0 || !avk_sync_dmabuf_attach(dmabuf.fd[i],
				DMA_BUF_SYNC_WRITE, dup)) {
			ok = false;
			break;
		}
	}
	close(fence);
	if (!ok) {
		wlr_log(WLR_ERROR, "AVK: the frame's fence could not be attached to "
			"the target dma-buf; this output cannot be presented safely and "
			"is going back to the SceneFX path");
		out->present_sync = AZ_AVK_PRESENT_SYNC_BROKEN;
		avk.present_sync_fails++;
		return false;
	}

	target->release_point = 0;
	target->state = AZ_AVK_TARGET_IN_FLIGHT;
	avk.present_sync_dmabuf++;
	return true;
}

/* Called when a monitor goes away. The swapchain's buffers carry the target
 * addons, so destroying it destroys those too -- which is the reason the
 * images are hung off the buffers rather than kept in a list here. */
static void az_avk_output_finish(struct az_avk_output *out) {
	if (out == NULL) {
		return;
	}
	if (out->swapchain != NULL) {
		wlr_swapchain_destroy(out->swapchain);
		out->swapchain = NULL;
	}
	/* Signal the timelines to their maximum before dropping them: anything
	 * still waiting on a point that will now never be reached would wait for
	 * the lifetime of the process. */
	if (out->in_timeline != NULL) {
		wlr_drm_syncobj_timeline_signal(out->in_timeline, UINT64_MAX);
		wlr_drm_syncobj_timeline_unref(out->in_timeline);
		out->in_timeline = NULL;
	}
	if (out->out_timeline != NULL) {
		wlr_drm_syncobj_timeline_signal(out->out_timeline, UINT64_MAX);
		wlr_drm_syncobj_timeline_unref(out->out_timeline);
		out->out_timeline = NULL;
	}
	if (out->sync_ready) {
		avk_sync_log_stats(&out->sync, "output");
		avk_sync_finish(&out->sync);
		out->sync_ready = false;
	}
	/*
	 * M4I. The cached background blur, handed to the renderer's retire queue so
	 * that a frame still sampling it when the monitor was unplugged keeps it
	 * alive. Destroying it here would be freeing an image a submitted command
	 * buffer is reading -- the exact VUID-vkDestroyImage-image-01000 that
	 * M4F.2C.4e's validation run caught on client textures.
	 */
	if (out->slot != NULL) {
		avk_blur_cache_finish(&out->blur_cache, out->slot->renderer.dev,
			&out->slot->renderer.retire);
		/* M5/C6. The Path-B working intermediate, released the same way and for
		 * the same reason. */
		avk_encode_intermediate_finish(&out->encode_work,
			out->slot->renderer.dev, &out->slot->renderer.retire);
		/* M6B/G2. And the measured curve, which a frame in flight is sampling
		 * for exactly as long as it is sampling the intermediate. */
		avk_encode_lut_finish(&out->encode_lut, out->slot->renderer.dev,
			&out->slot->renderer.retire);
		/* M6C. And the cube, on identical terms -- 2.1MB rather than 2KB, which
		 * is a reason to release it and not a reason to release it differently.
		 */
		avk_encode_clut_finish(&out->encode_clut, out->slot->renderer.dev,
			&out->slot->renderer.retire);
	}
	free(out);
}

/* ── the scene walker ───────────────────────────────────────────────────── */

struct az_avk_walk {
	struct avk_scene *scene;
	/* Output-buffer geometry: scale, transform and size, applied once here so
	 * every command is emitted in the pixels AVK will actually write. */
	float scale;
	enum wl_output_transform transform;
	/*
	 * THE PRESENTATION EXTENT, and it is typed so that the attachment extent
	 * cannot be assigned to it. This is the SOURCE space of the
	 * wlr_box_transform() below -- a node's geometry is in the orientation the
	 * user sees right up until that call -- and it is the pair that 90 and 270
	 * swap. Handing the attachment extent to a rotated output here is
	 * M4F.2C.4d: see src/render/az_extent.h.
	 */
	struct az_presentation_extent pres;
	/*
	 * THE ATTACHMENT EXTENT, and it is a different question from `pres`.
	 *
	 * Every `dst` box below has already been through wlr_box_transform(), so it
	 * is in ATTACHMENT coordinates -- and every cull that asks "is this box off
	 * the output" has to ask it against the attachment. Asking it against
	 * `pres` was wrong at 90, 270, flipped-90 and flipped-270, where the two
	 * extents are transposed: on an 800x600 output at 90 degrees a box at
	 * attachment x = 760 was compared against a 600-pixel limit and culled.
	 *
	 * It survived a pixel-exact eight-transform oracle because the cull only
	 * drops a box that STARTS past the limit, and every window in that fixture
	 * starts at the near edge. The cursor does not: parked in the bottom third
	 * of a 90-degree desktop it vanished completely, and that is how this was
	 * found.
	 */
	struct az_attachment_extent att;
	/* Layout coordinates of the output's top-left corner. */
	int ox, oy;
	/*
	 * ── THE SOURCE HALO, IN OUTPUT PIXELS ─────────────────────────────────
	 *
	 * How far outside this output's own bounds a command must still be KEPT,
	 * because a blur presented ON this output can sample that far past its
	 * edge. Presentation culling and source-reconstruction culling are
	 * different questions and this is the difference between them.
	 *
	 * An upper bound derived from the scene's kernel alone
	 * (avk_blur_support_bound), because the walk has to decide whether to keep
	 * a command before it has seen the blur node that will want it -- the exact
	 * per-node support needs the node's extent, which is discovered during the
	 * same traversal. It is used ONLY to widen retention; every capture and
	 * every damage region still uses the exact support.
	 *
	 * Zero when the scene has no blur, which is what keeps the direct path
	 * exactly as narrow as M4E left it.
	 */
	int halo;
	/*
	 * The scene's global blur kernel. Copied in rather than reached for through
	 * the node, because a blur node's own `strength` scales it
	 * (blur_data_apply_strength) and the walk should read the scene's value
	 * once, not once per node.
	 */
	struct blur_data blur;
	/*
	 * M4I. The OUTPUT's monitor-background-blur generation, by pointer, because
	 * the walk observes the dirty edge and the counter has to outlive the walk.
	 *
	 * Per output and not per scene: two monitors have two wallpapers, two
	 * extents and two caches, and one shared counter would have a wallpaper
	 * change on one display rebuild the other's cache -- correct, but at the
	 * cost of the one frame this whole mechanism exists to avoid.
	 */
	uint64_t *mon_blur_generation;
	/*
	 * M4I. THE ONE OPTIMIZED-BLUR NODE THAT BELONGS TO THIS OUTPUT.
	 *
	 * layers[LyrBlur] is a single global scene layer holding EVERY monitor's
	 * background node, and the walk is over the whole scene -- so without this,
	 * each output accepted whichever node it happened to walk last. On a two
	 * monitor layout that was the other display's: HEADLESS-2 built its cache
	 * at bounds (-1280,0), which is monitor 1's area expressed in monitor 2's
	 * pixels, and every backdrop on that output sampled the wrong background.
	 *
	 * Worse, and quieter: `ob->dirty` is an edge, cleared by the first walk that
	 * observes it. With both outputs reading both nodes, whichever rendered
	 * first consumed the flag and the other never incremented its generation --
	 * a wallpaper change that updates one display and leaves the other showing
	 * a stale blur indefinitely.
	 *
	 * Matched BY IDENTITY, not by geometry. A monitor's node is created at that
	 * monitor's position and size, so a box comparison would usually agree --
	 * and would silently pick the wrong node the moment two outputs shared an
	 * origin or a size, which is exactly the arrangement a mirrored or
	 * same-model pair produces.
	 */
	const struct wlr_scene_optimized_blur *mon_blur_node;
	/*
	 * M5/C2/ADR-003. The SCENE's reference luminance, cd/m2.
	 *
	 * Read once per frame rather than per surface, and carried here rather
	 * than read from `config` inside the adapter, because it is a SCENE
	 * property that must not vary between two surfaces of one frame: two
	 * windows resolved against different references would disagree about what
	 * scene 1.0 means, and a window straddling two outputs would split.
	 */
	float scene_ref_nits;
	/*
	 * M5. Composition for this frame happens in LINEAR LIGHT (Path A or B),
	 * so a colour that arrives as an sRGB code has to be decoded like any
	 * other source before it enters the blend.
	 *
	 * Read once per frame and carried, not recomputed per node: it is a
	 * property of the OUTPUT this walk is building for, and two nodes of one
	 * frame disagreeing about it would put half the desktop in one domain.
	 */
	bool linear_compose;
};

/*
 * ── M5: A CONFIG COLOUR IS AN ELECTRICAL VALUE ────────────────────────────
 *
 * A client's buffer is decoded because C7 gives it a luminance domain. A
 * BORDER's colour has no buffer and no domain: it is an sRGB hex triple out of
 * a config file, and until M5 nothing needed to say so, because composition
 * happened in the same encoding the config was written in.
 *
 * On a linear path it does need saying. Left alone, an electrical 0.5 enters
 * the blend as though it were a scene value and the encode re-encodes it:
 * srgb_ieotf(0.5) = 0.735, which is 187 where the config asked for 128.
 * Measured on the GPU before this existed -- 64 came out 137, 128 came out 188,
 * 192 came out 225. That is the whole of a desktop's chrome, not an edge case.
 *
 * ADR-004's rule, applied to the one other kind of source in the scene: what is
 * untagged is piecewise-sRGB BT.709. Config colours are exactly that, and
 * saying it here rather than in the renderer keeps colour policy on the
 * compositor side of the boundary -- a command still does not know which
 * attachment it lands in.
 *
 * RGB ONLY. Alpha is coverage, it is linear by definition and it has no
 * colorimetry (ADR-005). Passing it through a transfer function is the mistake
 * that makes every translucent panel the wrong opacity.
 */
/*
 * How many gradient stops the decode above will handle without allocating.
 * Generous against what a border theme uses (two to four); a gradient longer
 * than this takes the undecoded path rather than putting a malloc on the frame
 * path, and says so nowhere because it cannot happen with a config-driven
 * border -- if it ever does, the symptom is a gradient in the wrong domain,
 * which the border oracle sees.
 */
#define AZ_AVK_GRADIENT_STOPS_MAX 32

static inline void az_avk_scene_rgb(bool linear, const float in[4],
		float out[4]) {
	for (int i = 0; i < 3; i++) {
		out[i] = linear ? az_srgb_eotf(in[i]) : in[i];
	}
	out[3] = in[3];
}

/*
 * ── M5/C2: SCENEFX'S COLOUR FIELDS -> A LUMINANCE DOMAIN ──────────────────
 *
 * The adapter C2 said would be "three switch statements and belongs with the
 * scene walk". It is the only place in AVK that mentions a wlroots colour
 * enum; az_lum.h itself takes plain values so the resolver stays unit-testable
 * without a display.
 *
 * UNKNOWN VALUES COLLAPSE TO UNTAGGED rather than being rejected. A client
 * sending a transfer function from a protocol version this build predates must
 * still get pixels on the screen -- ADR-004 -- and az_lum_resolve() implements
 * what untagged means in one place.
 *
 * ── M12: THE PER-WINDOW RULES ARE LIVE, VIA A CLIENT LOOKUP ───────────────
 *
 * They were not. `sdr-white-scale` and `hdr-gain` were parsed, schema-
 * registered, documented, resolved onto the Client by APPLY_FLOAT_PROP -- and
 * read by nothing. Two configurable levers that did exactly nothing, which is
 * worse than an absent feature: the config accepted them and the picture never
 * changed.
 *
 * M5/C2 left this open and named two ways to close it, preferring a resolved
 * domain stored on the scene buffer. THAT IS THE ONE NOT TAKEN, and the reason
 * is subsurfaces. A window is not one buffer: a client with subsurfaces has
 * several, and a rule set on the one the compositor happens to know about
 * would leave the rest of the same window at a different luminance -- a
 * visibly split window, which is a worse failure than the gap it replaces.
 * Resolving from the buffer's surface goes through
 * wlr_surface_get_root_surface(), so every subsurface of a window answers with
 * that window's rule, for free.
 *
 * THE COST IS BOUNDED AND WAS CHECKED, not assumed. Per textured buffer per
 * frame: one addon lookup (wlr_scene_surface_try_from_buffer) and a handful of
 * pointer derefs with a shallow popup walk. No list is scanned -- the earlier
 * objection to a "client lookup in the walk" was a layering objection, and the
 * layering is what changed when the scene graph became ours.
 */
static struct az_lum_rules az_avk_rules_of(const struct wlr_scene_buffer *buf,
		const struct az_lum_source_desc *src, float scene_ref_nits) {
	struct wlr_surface *surface = NULL;
	if (buf != NULL) {
		struct wlr_scene_surface *ss =
			wlr_scene_surface_try_from_buffer((struct wlr_scene_buffer *)buf);
		if (ss != NULL) {
			surface = ss->surface;
		}
	}
	/* The precedence lives in az_lum_rules.h so the inspector reports what
	 * this applies rather than recomputing it. */
	return az_lum_rules_for_surface(surface, src, scene_ref_nits, NULL, NULL);
}

static struct az_lum_domain az_avk_lum_of(
		const struct wlr_scene_buffer *buf, float scene_ref_nits) {
	/* The translation lives in az_source_desc.h so the inspector reads the same
	 * one; see the note there for why it is not written twice. */
	struct az_lum_source_desc src = buf != NULL
		? az_source_desc_from_wlr(buf->transfer_function, buf->primaries,
			buf->max_cll)
		: (struct az_lum_source_desc){ .tagged = false };
	struct az_lum_rules rules = az_avk_rules_of(buf, &src, scene_ref_nits);
	return az_lum_resolve(&src, &rules, scene_ref_nits);
}

/*
 * ── M5/C6: THE OUTPUT'S COLOUR STATE -> THE ENCODE PASS'S CONSTANTS ───────
 *
 * The other adapter, and it is deliberately this thin. C3 derived every value
 * here when the output's state last changed (az_output_color_derive); nothing
 * is computed per frame, and nothing is decided here that is not already
 * decided there. What this function IS, is the one place a compositor type
 * (Monitor) meets a renderer type (avk_encode_params), which is the boundary
 * that keeps az_output_color.h free of Vulkan and avk_output_encode.h free of
 * wlroots.
 *
 * THE KNEE IS NOT IN az_output_color_state, and that is C6's decision rather
 * than an omission: ADR-009 fixes it at scene SDR white, and it rides on the
 * pass's push constants so that F5's question -- what a knee of 1.0 means
 * against an SDR ceiling of 1.0 -- is answerable by changing one number rather
 * than by arguing about a struct.
 */
static struct avk_encode_params az_avk_encode_params(const Monitor *m,
		struct avk_image *lut, const struct avk_encode_clut *clut) {
	struct avk_encode_params p = {0};
	const struct az_output_color_state *s = &m->color_state;
	for (int i = 0; i < 9; i++) {
		p.matrix[i] = s->matrix[i];
	}
	/* ADR-009: scene SDR white. Everything at or below it is untouched on every
	 * output, which is what makes the SDR round trip an exact identity. */
	p.knee = 1.0f;
	p.peak = s->peak_scene;
	/* PQ is ABSOLUTE -- its optical 1.0 is 10000 cd/m2 -- and the scene is
	 * relative. This is where the two meet, and it is applied AFTER the gamut
	 * matrix (ADR-008 step 4): before it would scale the values the matrix
	 * mixes, and the result is a plausible picture with the wrong white. */
	p.anchor = s->ref_nits / 10000.0f;
	p.dither_q = s->dither_q;
	/* The whole-output pass: this attachment IS the output raster, so the
	 * dither's anchor is the identity. The field exists so that a regional
	 * encode cannot be written without confronting the question. */
	p.origin_x = 0.0f;
	p.origin_y = 0.0f;
	/*
	 * M6B/G2. Three curves now, so the mapping is explicit rather than a
	 * predicate. A profiled SDR output encodes with the display's own measured
	 * curve; everything else keeps M5's two.
	 */
	p.tf = s->encode_tf == AZ_TF_PQ ? AVK_ENCODE_TF_PQ
		: s->encode_tf == AZ_TF_LUT1D ? AVK_ENCODE_TF_LUT1D
		: s->encode_tf == AZ_TF_CLUT3D ? AVK_ENCODE_TF_CLUT3D
		: AVK_ENCODE_TF_SRGB;
	/* Only for the variant that samples it. Left NULL for the analytic curves
	 * so that a caller passing one by mistake cannot leave a stale image
	 * descriptor bound to a pass that was never going to read it. */
	p.lut = p.tf == AVK_ENCODE_TF_LUT1D ? lut : NULL;
	/*
	 * M6C, on the same terms, and the dim travels WITH the image rather than
	 * being read from az_output_color_state: C3 knows there is a table and
	 * deliberately does not know how big it is, which is what keeps Vulkan out
	 * of that header. The renderer object that holds the cube is the only thing
	 * that can answer, and it is the same object the sample will come from.
	 */
	if (p.tf == AVK_ENCODE_TF_CLUT3D && clut != NULL) {
		p.clut = clut->image;
		p.clut_dim = clut->dim;
		p.clut_encoded_domain = clut->domain_break;
	}
	return p;
}

static enum avk_transform az_avk_transform(enum wl_output_transform t) {
	/* The two enumerations agree value for value -- both are the dihedral
	 * group of the square in the same order -- but AVK deliberately does not
	 * include a Wayland header, so the mapping is written out rather than
	 * cast. If wl_output_transform ever gains a member this becomes a compile
	 * error instead of a silently rotated desktop.
	 *
	 * They agree in MEANING as well, which until M4F.2C.4e they did not:
	 * transform_uv() resolved AVK_TRANSFORM_90 the way wl_output_transform
	 * defines 270 and vice versa, so this one-to-one mapping was handing the
	 * renderer a value it interpreted backwards. Every texture on a 90 or 270
	 * degree output came out rotated 180 degrees inside its own box. A
	 * value-for-value mapping between two enumerations is only safe while both
	 * sides mean the same thing by the value, and nothing here could check
	 * that -- the fixture that finally did is WLBGEFFECT_QUAD=1. */
	switch (t) {
	case WL_OUTPUT_TRANSFORM_NORMAL:      return AVK_TRANSFORM_NORMAL;
	case WL_OUTPUT_TRANSFORM_90:          return AVK_TRANSFORM_90;
	case WL_OUTPUT_TRANSFORM_180:         return AVK_TRANSFORM_180;
	case WL_OUTPUT_TRANSFORM_270:         return AVK_TRANSFORM_270;
	case WL_OUTPUT_TRANSFORM_FLIPPED:     return AVK_TRANSFORM_FLIPPED;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:  return AVK_TRANSFORM_FLIPPED_90;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180: return AVK_TRANSFORM_FLIPPED_180;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270: return AVK_TRANSFORM_FLIPPED_270;
	}
	return AVK_TRANSFORM_NORMAL;
}

/*
 * A node's logical box into the output buffer's pixels.
 *
 * Rounding matches wlr_scene's scale_box() exactly -- round the far edge and
 * subtract, rather than rounding the width. Rounding the width independently
 * makes adjacent windows on a fractional scale overlap by a pixel or leave a
 * seam, depending on where they land, which is the classic 1.25x-scaling
 * hairline.
 */
static void az_avk_box_to_output(const struct az_avk_walk *walk,
		int lx, int ly, int lw, int lh, struct wlr_box *out) {
	double x = (double)(lx - walk->ox);
	double y = (double)(ly - walk->oy);
	out->width = (int)(round((x + lw) * walk->scale) - round(x * walk->scale));
	out->height = (int)(round((y + lh) * walk->scale) - round(y * walk->scale));
	out->x = (int)round(x * walk->scale);
	out->y = (int)round(y * walk->scale);
	wlr_box_transform(out, out, wlr_output_transform_invert(walk->transform),
		walk->pres.width, walk->pres.height);
}

#include "az_corner_permute.h"

/*
 * SceneFX radii -> AVK command radii: logical units to output pixels, and
 * logical corners to physical ones.
 *
 * TWO conversions, and both have a way of being silently wrong.
 *
 * SCALE. Radii are logical, like the geometry they belong to, so they are
 * multiplied by the same output scale az_avk_box_to_output() applies. Doing it
 * anywhere else is how a radius ends up scaled twice (too round at 1.5) or not
 * at all (too square). One call site, next to the box, so the two cannot drift.
 *
 * ORIENTATION. wlr_box_transform() moves the box, so a rotated output puts the
 * logical top-left somewhere else physically -- and the shader works in output
 * pixels, where "top left" means the physical one. The permutation is
 * DERIVED rather than reasoned about: a 1x1 probe box is placed at each corner
 * of a unit square and pushed through the same transform, and where it lands
 * says which output corner that logical corner became. Working it out from the
 * Wayland rotation convention instead is a coin flip that renders correctly on
 * a normal output either way, so nothing would catch it.
 */
static void az_avk_corners_from_scenefx(const struct az_avk_walk *walk,
		struct fx_corner_radii in, float out[4]) {
	/* clockwise, matching struct fx_corner_radii and struct avk_cmd.corners */
	float logical[4] = {
		(float)in.top_left, (float)in.top_right,
		(float)in.bottom_right, (float)in.bottom_left,
	};
	if (logical[0] == 0.0f && logical[1] == 0.0f &&
			logical[2] == 0.0f && logical[3] == 0.0f) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return;
	}

	az_corner_permute(logical, wlr_output_transform_invert(walk->transform),
		out);
	for (int i = 0; i < 4; i++) {
		out[i] *= (float)walk->scale;
	}
}

/*
 * Cut a rounded region out of a command: the window's own footprint, removed
 * from the primitive drawn around it.
 *
 * ONE function because there are two callers and they must not drift. A
 * border is a filled rect with the client's interior taken out of it; a
 * shadow is a filled envelope with the same interior taken out of it. Same
 * geometry, and the same two halves -- an exact scissor subtraction for the
 * part a region can express, and the radii carried to the shader for the arcs
 * it cannot. The M4A wedge is what happens when only one of the two is done,
 * and it would have been a second, identical bug to fix here.
 *
 * `square_inner` restores that defect on demand; see the break switch at the
 * border call site.
 */
static void az_avk_clip_out_region(const struct az_avk_walk *walk,
		struct avk_cmd *cmd, const struct wlr_box *dst,
		const struct clipped_region *region, int lx, int ly, bool square_inner,
		const char *what) {
		struct wlr_box hole = region->area;
		struct wlr_box hole_out;
		az_avk_box_to_output(walk, lx + hole.x, ly + hole.y, hole.width,
			hole.height, &hole_out);
		cmd->inner = (struct avk_box){ hole_out.x, hole_out.y,
			hole_out.width, hole_out.height };
		cmd->has_inner = true;
		/* The SAME conversion the outer radii went through, so the two
		 * edges of the ring are scaled by the same factor and permuted by
		 * the same table. Rotating the outer corners and not the inner
		 * ones is a bug that renders perfectly on an unrotated output. */
		az_avk_corners_from_scenefx(walk, region->corners,
			cmd->inner_corners);
		if (square_inner) {
			cmd->inner_corners[0] = cmd->inner_corners[1] =
				cmd->inner_corners[2] = cmd->inner_corners[3] = 0.0f;
		}
		/*
		 * Shrink the scissor cut on each edge by 0.3 of the adjoining
		 * radius, which is SceneFX's rule (apply_clip_region in
		 * fx_pass.c), and ROUND THAT UP.
		 *
		 * The margin here is thinner than it looks. A box inset by k has
		 * its corner (r - k)*sqrt(2) from the arc's centre, so staying
		 * inside the hole needs k >= r*(1 - 1/sqrt2) = 0.2929r. The 0.3
		 * leaves 0.7% of slack -- and truncating the product spends far
		 * more than that: at r = 5, 0.3r is 1.5, truncation insets by 1,
		 * and the corner lands at 5.66 from a centre 5 away. OUTSIDE the
		 * hole, so pixman deletes a pixel the arc wanted, and the ring
		 * opens at exactly one pixel per corner.
		 *
		 * That is a subtractive scissor rounded the WRONG WAY, and it only
		 * shows where 0.3r is small or lands just above an integer: it
		 * passed radius 40 at scale 1 and failed radius 9, and failed
		 * radius 40 again at scale 1.5. Round up, then a further pixel for
		 * the antialiased band, which extends past the nominal arc and is
		 * just as much the shader's to paint.
		 */
		struct wlr_box cut_box = hole_out;
		const float *ic = cmd->inner_corners;
		if (ic[0] > 0.0f || ic[1] > 0.0f || ic[2] > 0.0f || ic[3] > 0.0f) {
			/* clockwise tl, tr, br, bl -> the edge each pair bounds */
			float top = ic[0] > ic[1] ? ic[0] : ic[1];
			float right = ic[1] > ic[2] ? ic[1] : ic[2];
			float bottom = ic[2] > ic[3] ? ic[2] : ic[3];
			float left = ic[3] > ic[0] ? ic[3] : ic[0];
			int in_l = (int)ceilf(left * 0.3f) + 1;
			int in_r = (int)ceilf(right * 0.3f) + 1;
			int in_t = (int)ceilf(top * 0.3f) + 1;
			int in_b = (int)ceilf(bottom * 0.3f) + 1;
			cut_box.x += in_l;
			cut_box.y += in_t;
			cut_box.width -= in_l + in_r;
			cut_box.height -= in_t + in_b;
		}
		/* AZ_AVK_BORDER_DEBUG=1 prints the annulus as the renderer
		 * actually receives it. Reading a border's geometry back out of a
		 * screenshot means inferring it through two antialiased edges and
		 * a classifier, which is how a wrong theory survives; this is the
		 * numbers themselves. Rate-limited to once a second. */
		static int border_debug = -1;
		if (border_debug < 0) {
			const char *env = getenv("AZ_AVK_BORDER_DEBUG");
			border_debug = env != NULL && env[0] == '1';
		}
		if (border_debug) {
			static time_t last;
			time_t now = time(NULL);
			if (now != last) {
				last = now;
				wlr_log(WLR_ERROR, "AVK %s: outer %d,%d %dx%d "
					"corners %.1f/%.1f/%.1f/%.1f | inner %d,%d %dx%d "
					"corners %.1f/%.1f/%.1f/%.1f | scale %.3f",
				what, dst->x, dst->y, dst->width, dst->height,
					cmd->corners[0], cmd->corners[1], cmd->corners[2],
					cmd->corners[3], hole_out.x, hole_out.y,
					hole_out.width, hole_out.height,
					cmd->inner_corners[0], cmd->inner_corners[1],
					cmd->inner_corners[2], cmd->inner_corners[3],
					walk->scale);
			}
		}
		pixman_region32_t clip;
		pixman_region32_init_rect(&clip, dst->x, dst->y,
		(unsigned)dst->width, (unsigned)dst->height);
		if (cut_box.width > 0 && cut_box.height > 0) {
			pixman_region32_t cut;
			pixman_region32_init_rect(&cut, cut_box.x, cut_box.y,
				(unsigned)cut_box.width, (unsigned)cut_box.height);
			pixman_region32_subtract(&clip, &clip, &cut);
			pixman_region32_fini(&cut);
		}
		avk_cmd_set_clip(cmd, &clip);
		pixman_region32_fini(&clip);
}

/*
 * A blur node's KEEP region: node-local pixman rectangles, in output pixels.
 *
 * THE OPPOSITE OF az_avk_clip_out_region ABOVE, and the difference is not a
 * detail of this function -- it is the semantics of the field. For a rect or a
 * shadow, `clipped_region` is the window's own footprint SUBTRACTED, which is
 * how a border becomes an annulus. For a BLUR node the reference INTERSECTS it
 * (types/scene/wlr_scene.c:2710 and :2727, both pixman_region32_intersect), so
 * it is the region the blur is ALLOWED to appear in. Same field name, same C
 * type, opposite operation; getting it backwards renders a plausible picture
 * with the blur exactly where it should not be.
 *
 * `clip_region` -- ext-background-effect-v1's client-supplied region -- is the
 * same intersection and takes precedence when set. It may hold ARBITRARILY MANY
 * rectangles and its shape is preserved: every rectangle is converted
 * independently and the result goes into the command's own clip, which the draw
 * loop already walks rectangle by rectangle as scissors. Nothing takes a
 * bounding box, and no shader learns about it.
 *
 * Each rectangle goes through az_avk_box_to_output(), the same conversion the
 * node's own box took -- so at a fractional scale the clip's edges land exactly
 * where the box's do. Adjacent rectangles share an edge coordinate and it rounds
 * identically for both, so a multi-rect region gains neither a seam nor an
 * overlap.
 *
 * `out` is always initialised and is always the caller's to fini. Returns false
 * when nothing survives, which the caller treats as "cull" -- and it decides
 * that BEFORE adding a command, so there is never a half-built command to
 * un-emit.
 */
static bool az_avk_blur_keep_region(const struct az_avk_walk *walk,
		const struct wlr_box *dst, const pixman_region32_t *keep, int lx,
		int ly, pixman_region32_t *out) {
	pixman_region32_init_rect(out, dst->x, dst->y, (unsigned)dst->width,
		(unsigned)dst->height);

	pixman_region32_t converted;
	pixman_region32_init(&converted);
	int n = 0;
	const pixman_box32_t *rects =
		pixman_region32_rectangles((pixman_region32_t *)keep, &n);
	for (int i = 0; i < n; i++) {
		struct wlr_box box;
		az_avk_box_to_output(walk, lx + rects[i].x1, ly + rects[i].y1,
			rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1, &box);
		if (box.width <= 0 || box.height <= 0) {
			continue;
		}
		pixman_region32_union_rect(&converted, &converted, box.x, box.y,
			(unsigned)box.width, (unsigned)box.height);
	}
	pixman_region32_intersect(out, out, &converted);
	pixman_region32_fini(&converted);

	return !pixman_region32_empty(out);
}

static void az_avk_walk_node(struct az_avk_walk *walk,
		struct wlr_scene_node *node, int lx, int ly);

static void az_avk_walk_children(struct az_avk_walk *walk,
		struct wlr_scene_tree *tree, int lx, int ly) {
	struct wlr_scene_node *child;
	/* Bottom to top: wlr_scene keeps children in paint order, and a painter's
	 * algorithm needs no occlusion information to be correct. Occlusion
	 * culling is an optimisation and is deliberately not in the first cut. */
	wl_list_for_each(child, &tree->children, link) {
		az_avk_walk_node(walk, child, lx + child->x, ly + child->y);
	}
}

/*
 * One frame's scene, as AVK sees it, at DEBUG.
 *
 * Enabled with AVK_SCENE_DUMP=<n>: dump frame n and stop. This exists because
 * twice now a rendering fault has been diagnosed by guessing at the scene when
 * the answer was one print away -- once a border rect covering its own window,
 * once a surface node that was simply not there. The scene walker is the only
 * place that knows what AVK was actually asked to draw.
 */
/*
 * Set by `amsg dispatch dump_scene`, cleared by the frame that honours it.
 *
 * A FRAME NUMBER IS NOT ENOUGH, and finding that out cost a run. AVK_SCENE_DUMP
 * names a frame, and a headless test cannot know which frame its window will be
 * on: too low and the dump fires before the client exists, too high and an idle
 * compositor never reaches it. Either way the dump is empty, which reads exactly
 * like "the walker emitted nothing" -- a completely different diagnosis.
 *
 * So a test asks for the dump when its scene is ready, and gets the NEXT frame.
 */
static bool az_avk_dump_armed = false;

static bool az_avk_dumping(void) {
	static int frame = -2;
	if (frame == -2) {
		const char *env = getenv("AVK_SCENE_DUMP");
		frame = env != NULL ? atoi(env) : -1;
	}
	return az_avk_dump_armed || (frame >= 0 && (uint64_t)frame == avk.frames);
}

static void az_avk_walk_node(struct az_avk_walk *walk,
		struct wlr_scene_node *node, int lx, int ly) {
	if (!node->enabled) {
		if (az_avk_dumping()) {
			wlr_log(WLR_ERROR, "scene: node=%p type=%d DISABLED at %d,%d",
				(void *)node, node->type, lx, ly);
		}
		return;
	}
	if (az_avk_dumping()) {
		int w = 0, h = 0;
		if (node->type == WLR_SCENE_NODE_RECT) {
			w = wlr_scene_rect_from_node(node)->width;
			h = wlr_scene_rect_from_node(node)->height;
		} else if (node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *b = wlr_scene_buffer_from_node(node);
			w = b->dst_width;
			h = b->dst_height;
			/*
			 * WHAT THE NODE IS, not just where it is.
			 *
			 * The line used to print position, destination size and the
			 * buffer pointer, and nothing else. Two buffer nodes at one
			 * origin with different buffer pointers were read as "a toplevel
			 * and its subsurface" -- an inference, never checked -- and a fix
			 * was written for subsurfaces that could not possibly have run,
			 * because the oversized node may not be a wlr_scene_surface at
			 * all. Whether a node carries a surface, and whether that surface
			 * is a subsurface, decides which code touches it; neither was
			 * visible here.
			 *
			 * `src` is printed with it: dst is what the node presents, src is
			 * what it samples, and a node presenting its buffer at natural
			 * size is exactly the case being hunted.
			 */
			struct wlr_scene_surface *ss =
				wlr_scene_surface_try_from_buffer(b);
			const char *kind = "plain";
			int sw = 0, sh = 0;
			if (ss != NULL && ss->surface != NULL) {
				sw = ss->surface->current.width;
				sh = ss->surface->current.height;
				kind = wlr_subsurface_try_from_wlr_surface(ss->surface)
						   ? "subsurface"
						   : (wlr_xdg_popup_try_from_wlr_surface(ss->surface)
								  ? "popup"
								  : "toplevel");
			}
			wlr_log(WLR_ERROR,
				"scene: node=%p BUFFER at %d,%d %dx%d buffer=%p "
				"kind=%s surface=%dx%d src=%.0fx%.0f",
				(void *)node, lx, ly, w, h, (void *)b->buffer, kind, sw, sh,
				b->src_box.width, b->src_box.height);
		}
		if (node->type != WLR_SCENE_NODE_BUFFER) {
			wlr_log(WLR_ERROR, "scene: node=%p type=%d at %d,%d %dx%d",
				(void *)node, node->type, lx, ly, w, h);
		}
	}

	switch (node->type) {
	case WLR_SCENE_NODE_TREE:
		az_avk_walk_children(walk, wlr_scene_tree_from_node(node), lx, ly);
		return;

	case WLR_SCENE_NODE_RECT: {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		struct wlr_box dst;
		az_avk_box_to_output(walk, lx, ly, rect->width, rect->height, &dst);
		if (dst.width <= 0 || dst.height <= 0) {
			return;
		}
		struct avk_cmd *cmd = avk_scene_add(walk->scene, AVK_CMD_RECT);
		if (cmd == NULL) {
			return;
		}
		cmd->dst = (struct avk_box){ dst.x, dst.y, dst.width, dst.height };
		az_avk_corners_from_scenefx(walk, rect->corners, cmd->corners);
		/*
		 * A window border is not four rectangles -- SceneFX draws it as ONE
		 * filled rect with the window's interior clipped OUT of it, which is
		 * how a rounded border gets a rounded inside edge.
		 *
		 * Ignoring that clip does not produce a subtly wrong border. It
		 * produces a filled rectangle over the whole window, and since the
		 * border sits above the surface in the scene, every window renders as
		 * a flat block of its border colour. That was the first thing AVK got
		 * visibly wrong on a real desktop, and it looked like a texture bug
		 * for far longer than it should have.
		 *
		 * THE CUT IS TWO THINGS, AND IT HAS TO BE BOTH.
		 *
		 * The interior is a ROUNDED rectangle, and a pixman region cannot be
		 * one. AVK used to subtract `clipped_region.area` as a plain box and
		 * ignore `clipped_region.corners` sitting beside it in the same
		 * struct. On a corner's diagonal that square hole removed border the
		 * outer arc had already curved away from, and nobody painted the
		 * wedge in between: 104 pixels of wallpaper per corner, present under
		 * partial damage and under a forced full redraw alike, because it was
		 * never a damage bug. That is the M4A artifact, classified and
		 * transferred here.
		 *
		 * So the region keeps the part it can express EXACTLY -- and, exactly
		 * as SceneFX does, it keeps a conservatively SHRUNKEN box so it can
		 * never drop a pixel the arcs want -- while the radii travel to the
		 * shader, which subtracts them analytically. When the radii are zero
		 * the shrink is zero, the region cut is the whole answer, and the
		 * shader does nothing.
		 */
		/* AVK_NO_BORDER_CLIP=1 skips the cut-out, for the same reason: the
		 * test that asserts a window is not covered by its own border needs a
		 * build where it fails. */
		static int no_border_clip = -1;
		if (no_border_clip < 0) {
			const char *env = getenv("AVK_NO_BORDER_CLIP");
			no_border_clip = env != NULL && env[0] == '1';
		}
		/*
		 * AZ_AVK_BORDER_SQUARE_INNER=1 restores the M4A defect precisely: the
		 * outer edge stays rounded, the inner cut goes back to a full square
		 * box, and the wedge comes back. It does NOT merely turn borders off
		 * -- a break that removes the feature proves nothing about the bug.
		 */
		static int square_inner = -1;
		if (square_inner < 0) {
			const char *env = getenv("AZ_AVK_BORDER_SQUARE_INNER");
			square_inner = env != NULL && env[0] == '1';
		}
		if (!no_border_clip && !wlr_box_empty(&rect->clipped_region.area)) {
			az_avk_clip_out_region(walk, cmd, &dst, &rect->clipped_region,
				lx, ly, square_inner, "border");
		}
		/*
		 * THE GRADIENT, SNAPSHOT AND NOT BORROWED.
		 *
		 * `gradient_colors` is owned by the scene node and freed the moment
		 * anything calls wlr_scene_rect_set_gradient() again -- which happens
		 * on every focus change, because the border's first stop is the
		 * (animated) border colour. The renderer runs after this walk, so a
		 * pointer into that array is a pointer into memory the compositor is
		 * free to release. avk_cmd_set_gradient() copies.
		 *
		 * WHAT THE FIELDS MEAN, traced rather than inferred (they are a fork
		 * extension and several read the opposite way to their names):
		 *
		 *   gradient_linear   1 = linear, 2 = conic. Not a boolean.
		 *   gradient_blend    banded (0) vs interpolated (1), and NOTHING
		 *                     else -- not smoothing, not premultiply.
		 *   gradient_degree   DEGREES; the reference shader calls radians().
		 *   gradient_origin   normalised 0..1 inside the rect's own box.
		 *   gradient_colors   4 floats per colour, PREMULTIPLIED, like every
		 *                     other wlr_scene_rect colour. Copied through
		 *                     unchanged: unlike cmd->color there is nothing to
		 *                     undo, because the shader writes them straight
		 *                     out under the same (ONE, 1-SRC_ALPHA) blend.
		 *
		 * There are no stop POSITIONS anywhere in the source. Spacing comes
		 * from the count alone.
		 */
		if (rect->has_gradient && rect->gradient_count > 0 &&
				rect->gradient_colors != NULL) {
			enum avk_gradient_type type = rect->gradient_linear == 2
				? AVK_GRADIENT_CONIC : AVK_GRADIENT_LINEAR;
			const float *stops = rect->gradient_colors;
			/*
			 * M5. The same decode the rect's own colour gets, because a
			 * gradient stop is the same kind of value from the same config --
			 * and a gradient whose ENDS were decoded while its stops were not
			 * is a border that changes colour along its length.
			 *
			 * These arrive PREMULTIPLIED, so the round trip is explicit:
			 * un-premultiply, decode, re-premultiply. A stack copy bounded by
			 * the same cap the gradient store already enforces; anything
			 * larger takes the undecoded path rather than allocating on the
			 * frame path.
			 */
			float decoded[AZ_AVK_GRADIENT_STOPS_MAX * 4];
			if (walk->linear_compose
					&& rect->gradient_count <= AZ_AVK_GRADIENT_STOPS_MAX) {
				for (int s = 0; s < rect->gradient_count; s++) {
					const float *in = &stops[s * 4];
					float *out = &decoded[s * 4];
					float a = in[3];
					for (int i = 0; i < 3; i++) {
						float straight = a > 0.0f ? in[i] / a : 0.0f;
						out[i] = az_srgb_eotf(straight) * a;
					}
					out[3] = a;
				}
				stops = decoded;
			}
			avk_cmd_set_gradient(walk->scene, cmd, type, rect->gradient_degree,
				rect->gradient_blend != 0, rect->gradient_origin,
				stops, (uint32_t)rect->gradient_count);
		}
		/* wlr_scene rect colours are PREMULTIPLIED -- scenefx writes them
		 * straight to the framebuffer under source-over blending. AVK's
		 * command takes a straight colour and premultiplies it itself, so the
		 * value has to be undone here. Getting this backwards darkens every
		 * translucent panel in the overview by exactly its own alpha, which
		 * looks like a theming choice rather than a bug. */
		float a = rect->color[3];
		float straight[4];
		for (int i = 0; i < 3; i++) {
			straight[i] = a > 0.0f ? rect->color[i] / a : 0.0f;
		}
		straight[3] = a;
		/* Un-premultiplied FIRST, then decoded. A transfer function applied to
		 * a premultiplied value is a curve applied to colour-times-coverage,
		 * which is not the colour and not the coverage. */
		az_avk_scene_rgb(walk->linear_compose, straight, cmd->color);
		return;
	}

	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
		if (buf->buffer == NULL) {
			return;
		}
		struct wlr_box dst;
		az_avk_box_to_output(walk, lx, ly, buf->dst_width, buf->dst_height,
			&dst);
		if (dst.width <= 0 || dst.height <= 0) {
			return;
		}
		/*
		 * Cull against THIS output before resolving anything.
		 *
		 * The walk starts at the scene root, so it visits every node on every
		 * output -- including a 4K wallpaper belonging to a monitor 3840
		 * pixels to the left. Resolving its buffer costs an import or a copy
		 * and produces a command the renderer then scissors away to nothing.
		 * Measured on the live desktop, that was each output frame resolving
		 * BOTH monitors' full-screen wallpapers.
		 *
		 * The test is the same one the renderer would apply anyway, moved
		 * ahead of the resolve rather than after it, so no command that would
		 * have drawn anything is lost.
		 */
		/*
		 * PRESENTATION CULLING, WIDENED BY THE SOURCE HALO.
		 *
		 * A texture entirely off this output cannot be presented here -- but it
		 * can still be SOURCE for a blur that is presented here, if it lies
		 * within one support of the edge. Dropping it would replace real scene
		 * content with the capture's edge-clamped colour and put a seam down
		 * every window that spans two monitors.
		 *
		 * The halo is narrow (tens of pixels), so this keeps the cull's whole
		 * point: the reason it exists is that resolving a 4K wallpaper
		 * belonging to a monitor 3840 pixels away costs an import per frame,
		 * and a wallpaper 3840 pixels away is still culled.
		 */
		if (!az_avk_env_flag("AZ_AVK_NO_OUTPUT_CULL") &&
				(dst.x >= walk->att.width + walk->halo
				|| dst.y >= walk->att.height + walk->halo
				|| dst.x + dst.width <= -walk->halo
				|| dst.y + dst.height <= -walk->halo)) {
			avk.nodes_output_culled_before_resolve++;
			return;
		}
		if (dst.x >= walk->att.width || dst.y >= walk->att.height
				|| dst.x + dst.width <= 0 || dst.y + dst.height <= 0) {
			/* Kept only because a blur here might read it. Counted separately:
			 * this is the price of the halo, and it is the number that says
			 * whether a narrow halo has quietly become "replay the neighbouring
			 * monitor". */
			avk.nodes_retained_for_halo++;
		}
		/*
		 * A surface's content must already be ours by now.
		 *
		 * If this is the first time AVK has seen a surface's buffer at DRAW
		 * time, the commit hook did not run, and the only reason it renders
		 * correctly is that the client has not yet reused or destroyed the
		 * buffer. That is the bug this milestone removed, so it is worth a
		 * loud complaint rather than a silent success.
		 */
		if (wlr_scene_surface_try_from_buffer(buf) != NULL &&
				wlr_addon_find(&buf->buffer->addons, &avk,
					&az_avk_buffer_addon_impl) == NULL) {
			avk.late_imports++;
			if (!avk.warned_late_import) {
				avk.warned_late_import = true;
				wlr_log(WLR_ERROR, "AVK: a surface's buffer reached the frame "
					"without having been taken at commit; AVK's ownership of "
					"client content is not what it should be");
			}
		}

		avk.buffer_resolve_attempts++;
		avk.frame_stale_entry = NULL;
		/* The one call in the walk that can do unbounded work: it imports a
		 * dma-buf or creates an image, and a 5128x2734 client buffer is a
		 * 56MB allocation. Timed so a slow walk says whether it was here. */
		uint64_t resolve_t0 = az_avk_now_ns();
		uint64_t creates_before = avk.cache_misses;
		struct avk_image *image = az_avk_image_for_buffer(buf->buffer);
		avk.frame_resolve_ns += az_avk_now_ns() - resolve_t0;
		avk.frame_resolve_creates +=
			avk.cache_misses - creates_before;
		if (avk.frame_stale_entry != NULL) {
			/* That resolve returned a PREVIOUS generation. The repaint it
			 * owes is this rectangle and not the output -- see
			 * az_avk_shm_drain(). Layout coordinates, converted per output
			 * when the debt is paid. */
			int nx = 0, ny = 0;
			if (wlr_scene_node_coords(&buf->node, &nx, &ny)) {
				avk.frame_stale_entry->stale_box = (struct wlr_box){
					.x = nx, .y = ny,
					.width = buf->dst_width, .height = buf->dst_height,
				};
				avk.frame_stale_entry->stale_box_valid =
					buf->dst_width > 0 && buf->dst_height > 0;
			}
			avk.frame_stale_entry = NULL;
		}
		if (image == NULL) {
			/* Already logged, in full, by the importer. Dropping the command
			 * leaves whatever is behind the window visible, which is at least
			 * honest about something having gone wrong. */
			return;
		}
		/*
		 * ── P3: ONE MARKER NODE, N TUMBLING FRAGMENTS ─────────────────────
		 *
		 * A shatter cloud is a single scene node holding the closing window's
		 * own buffer. Here it becomes one AVK_CMD_TEXTURE_QUAD per fragment,
		 * all sampling THAT SAME image through per-fragment source rects --
		 * no capture pass, no copy, and one buffer import rather than N.
		 *
		 * THE TRAJECTORY IS NOT EVALUATED HERE. The corners were computed on
		 * the CPU, at this output's own presentation instant, by
		 * shatter_next_tick (ADR-612 Model A: the CPU decides where, the GPU
		 * draws it). The walker reads them. If it recomputed them from a clock
		 * of its own, two outputs drawing the same frame could disagree about
		 * where a fragment is, which is the whole defect per-output
		 * presentation sampling exists to prevent.
		 */
		struct ShatterEmitter *shatter = shatter_emitter_for(buf);
		if (shatter != NULL) {
			for (int32_t i = 0; i < shatter->nfrags; i++) {
				if (shatter->frags[i].dropped) {
					/* Entered another monitor and was retired; see the
					 * trespass rule in animation/client.h. */
					continue;
				}
				const float *cor = &shatter->corners[i * 8];

				/*
				 * Layout pixels to output pixels, THROUGH THE EXISTING
				 * CONVERSION, one corner at a time as a 1x1 box.
				 *
				 * az_avk_box_to_output already handles the output's origin,
				 * its scale and all eight transforms, and is exercised by
				 * every other command in the tree. A second, quad-only
				 * conversion would be a second chance to get a rotated output
				 * wrong -- and that class of bug renders perfectly on the
				 * machine it was written on. The cost is that corners land on
				 * whole output pixels, which is the same quantisation every
				 * other scene node's position already has.
				 */
				float q[8];
				bool sane = true;
				for (int k = 0; k < 4; k++) {
					struct wlr_box pt;
					az_avk_box_to_output(walk, (int)lroundf(cor[k * 2]),
						(int)lroundf(cor[k * 2 + 1]), 1, 1, &pt);
					q[k * 2] = (float)pt.x;
					q[k * 2 + 1] = (float)pt.y;
					if (!isfinite(cor[k * 2]) || !isfinite(cor[k * 2 + 1])) {
						sane = false;
					}
				}
				if (!sane) {
					continue;
				}

				/* The fragment's own bounding box, which is what the scissor
				 * and the damage arithmetic read. A dst that did not contain
				 * all four corners would clip the fragment. */
				float bx0 = q[0], bx1 = q[0], by0 = q[1], by1 = q[1];
				for (int k = 1; k < 4; k++) {
					bx0 = fminf(bx0, q[k * 2]);
					bx1 = fmaxf(bx1, q[k * 2]);
					by0 = fminf(by0, q[k * 2 + 1]);
					by1 = fmaxf(by1, q[k * 2 + 1]);
				}
				struct avk_cmd *fc = avk_scene_add(walk->scene,
					AVK_CMD_TEXTURE_QUAD);
				if (fc == NULL) {
					return;
				}
				fc->dst = (struct avk_box){ (int)floorf(bx0), (int)floorf(by0),
					(int)ceilf(bx1) - (int)floorf(bx0) + 1,
					(int)ceilf(by1) - (int)floorf(by0) + 1 };
				fc->image = image;
				fc->opacity = shatter->opacity;
				fc->lum = az_avk_lum_of(buf, walk->scene_ref_nits);
				fc->src = (struct avk_fbox){ shatter->frags[i].sx,
					shatter->frags[i].sy, shatter->frags[i].sw,
					shatter->frags[i].sh };
				fc->transform = az_avk_transform(wlr_output_transform_compose(
					buf->transform, walk->transform));
				/* Always linear: a rotated fragment never samples 1:1, so
				 * nearest would crawl along its edges as it turns. */
				fc->filter_linear = true;
				memcpy(fc->quad, q, sizeof(q));
			}
			return;
		}

		struct avk_cmd *cmd = avk_scene_add(walk->scene, AVK_CMD_TEXTURE);
		if (cmd == NULL) {
			return;
		}
		cmd->dst = (struct avk_box){ dst.x, dst.y, dst.width, dst.height };
		cmd->image = image;
		cmd->opacity = buf->opacity;
		/* M5/C2. Carried, not yet consumed -- see avk_cmd.lum. */
		cmd->lum = az_avk_lum_of(buf, walk->scene_ref_nits);
		/* Rounding belongs to the DESTINATION geometry, not the source: a
		 * cropped or scaled client is still rounded at the window's corners,
		 * not at its texture's. */
		az_avk_corners_from_scenefx(walk, buf->corners, cmd->corners);
		if (wlr_fbox_empty(&buf->src_box)) {
			cmd->src = (struct avk_fbox){ 0, 0, image->extent.width,
				image->extent.height };
		} else {
			cmd->src = (struct avk_fbox){ buf->src_box.x, buf->src_box.y,
				buf->src_box.width, buf->src_box.height };
		}
		/* Same composition wlr_scene uses: undo the buffer's own transform,
		 * then apply the output's. */
		cmd->transform = az_avk_transform(wlr_output_transform_compose(
			wlr_output_transform_invert(buf->transform), walk->transform));
		cmd->filter_linear = buf->filter_mode == WLR_SCALE_FILTER_BILINEAR;
		return;
	}

	case WLR_SCENE_NODE_SHADOW: {
		struct wlr_scene_shadow *shadow = wlr_scene_shadow_from_node(node);
		struct wlr_box dst;
		az_avk_box_to_output(walk, lx, ly, shadow->width, shadow->height,
			&dst);
		if (dst.width <= 0 || dst.height <= 0) {
			return;
		}
		struct avk_cmd *cmd = avk_scene_add(walk->scene, AVK_CMD_SHADOW);
		if (cmd == NULL) {
			return;
		}
		cmd->dst = (struct avk_box){ dst.x, dst.y, dst.width, dst.height };

		/*
		 * THE NODE'S BOX IS THE ENVELOPE, NOT THE CASTER.
		 *
		 * client_draw_one_shadow() has already grown the box by
		 * (shadows_size + border) on every side and then applied the
		 * position offset, precisely so the falloff has somewhere to live.
		 * The renderer insets it again by blur_sigma to recover the caster.
		 * Nothing here computes a second envelope, and nothing should: the
		 * damage this node reports is its box, and a renderer that drew
		 * outside it would be drawing outside its own damage.
		 */
		az_avk_corners_from_scenefx(walk, shadow->corners, cmd->corners);

		/*
		 * SIGMA IS SCALED, and the reference does not scale it.
		 *
		 * REFERENCE BUG FIXED IN AVK: a shadow's blur sigma is converted to
		 * output pixels like the box and the radii it is measured against.
		 * SceneFX scales dst_box (scale_box in transform_output_box) and the
		 * corner radii (fx_corner_radii_scale) and passes blur_sigma through
		 * in logical units -- so on a fractional-scale output the falloff is
		 * measured against a box it no longer matches. It is not a subtle
		 * difference on this desk: DP-1 runs at 1.5 and HDMI-A-1 at 1.0, so
		 * the same window has a visibly tighter shadow on one monitor than
		 * the other, and changes shape as it is dragged across the seam.
		 *
		 * Scaled HERE, next to the box and the radii, so all three cannot
		 * drift apart -- the same argument az_avk_corners_from_scenefx makes
		 * for the radii.
		 */
		cmd->blur_sigma = shadow->blur_sigma * (float)walk->scale;

		/* Straight, not premultiplied: shadow.frag multiplies through by the
		 * coverage it computes, which is not known until then. Already
		 * straight, so the decode needs no un-premultiply. */
		az_avk_scene_rgb(walk->linear_compose, shadow->color, cmd->color);

		/*
		 * The interior cut-out -- the window's own footprint kept out of its
		 * own shadow -- travels exactly the road a border's does, and for the
		 * same reason: the region can express the box but not the arcs.
		 */
		if (!wlr_box_empty(&shadow->clipped_region.area)) {
			az_avk_clip_out_region(walk, cmd, &dst,
				&shadow->clipped_region, lx, ly, false, "shadow");
		}
		return;
	}

	case WLR_SCENE_NODE_BLUR: {
		/*
		 * A LIVE BACKGROUND BLUR, EMITTED IN PLACE.
		 *
		 * In place is the whole design: what a blur samples is the commands
		 * BEFORE it in this list, so its position here is its semantics. There
		 * is deliberately no second traversal collecting blur nodes and no
		 * effect list ordered separately -- either would be an ordering that can
		 * drift from the one the picture depends on. The command index this
		 * emission lands at IS the k the renderer replays [0, k) for; nothing
		 * derives k from a node count or a blur count.
		 */
		struct wlr_scene_blur *blur = wlr_scene_blur_from_node(node);
		avk.blur_nodes_seen++;

		/*
		 * The kernel, from the SCENE's global blur_data scaled by this node's
		 * own strength -- which is what blur_data_apply_strength() is for, and
		 * is how an opening window's blur ramps without a second code path.
		 */
		struct blur_data bd = blur_data_apply_strength(&walk->blur,
			blur->strength);

		/*
		 * ── CULLING ─────────────────────────────────────────────────────────
		 *
		 * Every test here answers "can this node put a pixel on this output at
		 * all". None of them is a heuristic and none is a quality trade: a blur
		 * that cannot contribute costs a prefix capture, a chain of transients
		 * and 2N draws, which is the most expensive nothing in the renderer.
		 *
		 * What is NOT culled: occlusion. A blur under an opaque window still
		 * runs, because deciding otherwise needs occlusion information the
		 * walker does not have, and the painter's algorithm is correct without
		 * it. That is an M4F.2B question.
		 */
		if (!is_scene_blur_enabled(&bd) || blur->alpha <= 0.0f) {
			avk.blur_nodes_culled++;
			return;
		}
		struct wlr_box dst;
		az_avk_box_to_output(walk, lx, ly, blur->width, blur->height, &dst);
		if (dst.width <= 0 || dst.height <= 0) {
			avk.blur_nodes_culled++;
			return;
		}
		/*
		 * Same widening, for the same reason one level up: an off-output blur
		 * node cannot present here, but its RESULT is part of the prefix a blur
		 * that IS presented here replays. Blur 2's source contains blur 1.
		 */
		if (!az_avk_env_flag("AZ_AVK_NO_OUTPUT_CULL") &&
				(dst.x >= walk->att.width + walk->halo
				|| dst.y >= walk->att.height + walk->halo
				|| dst.x + dst.width <= -walk->halo
				|| dst.y + dst.height <= -walk->halo)) {
			avk.blur_nodes_culled++;
			return;
		}

		/*
		 * THE KEEP REGION, DECIDED BEFORE THE COMMAND EXISTS. clip_region takes
		 * precedence over clipped_region, which is the reference's own order,
		 * and BOTH intersect -- see az_avk_blur_keep_region for why that is
		 * worth saying twice. An empty result is a cull, and doing it here means
		 * there is never a command to take back out of the list.
		 */
		pixman_region32_t clip;
		bool has_clip = false;
		if (blur->has_clip_region) {
			has_clip = true;
			if (!az_avk_blur_keep_region(walk, &dst, &blur->clip_region, lx, ly,
					&clip)) {
				pixman_region32_fini(&clip);
				avk.blur_nodes_culled++;
				return;
			}
		} else if (!wlr_box_empty(&blur->clipped_region.area)) {
			pixman_region32_t keep;
			pixman_region32_init_rect(&keep, blur->clipped_region.area.x,
				blur->clipped_region.area.y,
				(unsigned)blur->clipped_region.area.width,
				(unsigned)blur->clipped_region.area.height);
			has_clip = true;
			bool any = az_avk_blur_keep_region(walk, &dst, &keep, lx, ly, &clip);
			pixman_region32_fini(&keep);
			if (!any) {
				pixman_region32_fini(&clip);
				avk.blur_nodes_culled++;
				return;
			}
		}

		struct avk_cmd *cmd = avk_scene_add(walk->scene, AVK_CMD_BLUR);
		if (cmd == NULL) {
			if (has_clip) {
				pixman_region32_fini(&clip);
			}
			return;
		}
		if (has_clip) {
			avk_cmd_set_clip(cmd, &clip);
			pixman_region32_fini(&clip);
			avk.blur_nodes_clipped++;
		}
		cmd->dst = (struct avk_box){ dst.x, dst.y, dst.width, dst.height };
		cmd->has_blur = true;
		cmd->opacity = blur->alpha;
		cmd->blur_levels = bd.num_passes > 0 ? (uint32_t)bd.num_passes : 0;
		/*
		 * THE KERNEL, SCALED TO OUTPUT PIXELS -- once, here, beside the box,
		 * the radii and the soft edge.
		 *
		 * `radius` is a multiplier on a HALF-TEXEL sampling step, and the texel
		 * is a texel of the capture, which is in OUTPUT PIXELS. So an unscaled
		 * radius means a blur reaches the same number of DEVICE pixels on every
		 * output -- and therefore covers 1/1.5 as much of the picture on a
		 * 1.5x display as on a 1.0x one. The same window, dragged across a
		 * seam, would visibly sharpen.
		 *
		 * The reference does not scale it either (blur_data_calc_size() is
		 * 2^(passes+1) * radius and is applied to damage in BUFFER coordinates,
		 * fx_pass.c:1481), so this is a divergence and not a fix to a
		 * regression. It is the same policy M4D adopted for a shadow's sigma
		 * and M4F.2A.3 for edge_softness: a logical length is scaled exactly
		 * once, at the walker, and everything downstream is device pixels.
		 *
		 * `levels` is NOT scaled: it is a count of halvings, not a length, and
		 * there is no such thing as 1.5 of a level. Scaling the radius moves
		 * the reach continuously, which is what a fractional scale needs.
		 */
		cmd->blur_radius = bd.radius * (float)walk->scale;
		cmd->blur_brightness = bd.brightness;
		cmd->blur_contrast = bd.contrast;
		cmd->blur_saturation = bd.saturation;
		cmd->blur_noise = bd.noise;
		cmd->blur_apply_effects =
			blur_data_should_parameters_blur_effects(&bd);
		az_avk_corners_from_scenefx(walk, blur->corners, cmd->corners);

		/*
		 * EDGE SOFTNESS, SCALED TO OUTPUT PIXELS -- once, here, beside the box
		 * and the radii, exactly as a shadow's blur_sigma is.
		 *
		 * The reference scales this field (wlr_scene.c:2778) and does NOT scale
		 * a shadow's blur_sigma, so on a fractional-scale output a window's
		 * backdrop blur fades over 1.5x the distance the shadow drawn to match
		 * it does. asteroidz's producer hands both the SAME number
		 * (client.h:757), which is what makes the mismatch a real seam rather
		 * than a theoretical one. AVK scales both, so they stay in lockstep.
		 */
		cmd->blur_edge_softness = blur->edge_softness * (float)walk->scale;

		/*
		 * DARKEN, gated exactly as the reference gates it: only where
		 * has_sample_exclude identifies a shadow's backdrop. The clamp's premise
		 * is that the blur is replacing real backdrop, and that is the producer
		 * which guarantees it.
		 */
		if (blur->has_sample_exclude) {
			struct wlr_box ex;
			az_avk_box_to_output(walk, lx + blur->sample_exclude.x,
				ly + blur->sample_exclude.y, blur->sample_exclude.width,
				blur->sample_exclude.height, &ex);
			cmd->sample_exclude = (struct avk_box){ ex.x, ex.y, ex.width,
				ex.height };
			cmd->has_sample_exclude = true;
			cmd->blur_darken = blur->darken;
		}

		/*
		 * `should_only_blur_bottom_layer` is RECORDED AND NOT HONOURED, and the
		 * decision is the milestone's: the cached bottom-layer blur misses an
		 * adjacent tiled window's shadow falling into the gap, which shows as a
		 * bright strip. Every node takes the live path until the uncached
		 * renderer has been reviewed (M4F.2E). Counted so the cost of that
		 * decision is a number rather than a belief.
		 */
		cmd->blur_bottom_only = blur->should_only_blur_bottom_layer;
		if (blur->should_only_blur_bottom_layer) {
			avk.blur_nodes_forced_live++;
			if (!avk.warned_optimized_blur) {
				avk.warned_optimized_blur = true;
				wlr_log(WLR_INFO, "AVK: a blur node asks for the cached "
					"bottom-layer path; AVK renders every blur live (M4F.2E is "
					"not implemented)");
			}
		}
		avk.blur_nodes_emitted++;
		return;
	}

	case WLR_SCENE_NODE_OPTIMIZED_BLUR: {
		/*
		 * ── THE MONITOR BACKGROUND BLUR PRODUCER (M4I) ────────────────────
		 *
		 * Not a thing drawn on the screen: a declaration that everything below
		 * this point in the scene should be blurred ONCE and kept, so the nodes
		 * above can sample it instead of each reconstructing it.
		 *
		 * It emits no command. What it emits is a request, recorded beside the
		 * command list, whose only geometric content is WHERE THE SOURCE RANGE
		 * ENDS -- and that is simply how many commands exist right now, because
		 * the walk is in scene order.
		 */
		struct wlr_scene_optimized_blur *ob =
			wlr_scene_optimized_blur_from_node(node);
		/* NOT THIS OUTPUT'S NODE -- see az_avk_walk.mon_blur_node. Every
		 * monitor's node lives in one global layer, so this is reached once per
		 * monitor per frame and only one of them describes the background this
		 * output is rendering. Nothing is done with the others: not the bounds,
		 * and above all not the dirty edge, which belongs to whoever owns it. */
		/* AZ_BLUR_CACHE_WRONG_OUTPUT is the falsifier: it restores the
		 * last-node-wins behaviour, so an output builds its cache from whichever
		 * monitor's node the walk happened to reach last. A multi-output oracle
		 * that stays green with this on is an oracle that is not looking. */
		static int wrong_output = -1;
		if (wrong_output < 0) {
			wrong_output = az_avk_env_flag("AZ_BLUR_CACHE_WRONG_OUTPUT") ? 1 : 0;
			if (wrong_output) {
				wlr_log(WLR_ERROR, "M4I break: AZ_BLUR_CACHE_WRONG_OUTPUT -- an "
					"output may build its background cache from ANOTHER "
					"monitor's node; this renders a WRONG desktop");
			}
		}
		/* A NULL mon_blur_node rejects everything, deliberately: it means this
		 * monitor has no background node, and the only thing a foreign node
		 * could offer such an output is a neighbour's wallpaper. */
		if (!wrong_output && ob != walk->mon_blur_node) {
			return;
		}
		struct blur_data bd = walk->blur;
		if (!is_scene_blur_enabled(&bd)) {
			return;
		}
		struct wlr_box dst;
		az_avk_box_to_output(walk, lx, ly, ob->width, ob->height, &dst);
		if (dst.width <= 0 || dst.height <= 0) {
			return;
		}
		/*
		 * DIRTY EDGE -> GENERATION INCREMENT, and the flag is cleared here.
		 *
		 * Every producer of the signal funnels through wlr_scene_optimized_blur
		 * _mark_dirty(): a background layer commit, a blur-parameter change, a
		 * resize. Converting the edge to a counter at the single point where
		 * the node is observed means the renderer never has to know which of
		 * them fired, and a second change arriving while a rebuild is in flight
		 * increments again rather than being swallowed by an already-set bit.
		 *
		 * Cleared at OBSERVATION rather than on successful render, which is the
		 * opposite of what SceneFX does and is deliberate: the increment has
		 * already been recorded in the snapshot, so a frame that fails to build
		 * the cache leaves the renderer's cached generation behind the scene's
		 * and it rebuilds next frame anyway. Latching on success would need the
		 * renderer to reach back into the scene graph after submission, which
		 * is precisely the mutable-state-after-snapshot the design forbids.
		 */
		/*
		 * AZ_BLUR_CACHE_NO_DIRTY_EDGE is the falsifier for the SOURCE digest,
		 * and it models the SHIPPED defect rather than an invented one: the
		 * background changes and the notification never arrives, so the
		 * generation stays equal and only a fact about the source can catch it.
		 *
		 * NOT AZ_BLUR_CACHE_IGNORE_DIRTY, which is a different condition and
		 * cannot test this. That one lets the generation move and then
		 * suppresses the reason -- but avk_blur_cache_check() returns at the
		 * first disagreement, so it returns GENERATION and the SOURCE
		 * comparison below it is never evaluated at all. A fixture built on it
		 * reports inv_source == 0 and looks like the digest does nothing.
		 *
		 * The flag is still CLEARED. Leaving it set would make the next frame
		 * increment instead, which is a delayed notification rather than a lost
		 * one.
		 */
		static int no_dirty_edge = -1;
		if (no_dirty_edge < 0) {
			no_dirty_edge = az_avk_env_flag("AZ_BLUR_CACHE_NO_DIRTY_EDGE")
				? 1 : 0;
			if (no_dirty_edge) {
				wlr_log(WLR_ERROR, "M4I break: AZ_BLUR_CACHE_NO_DIRTY_EDGE -- "
					"the background dirty signal is DROPPED; only the source "
					"digest can now notice a wallpaper change");
			}
		}
		if (ob->dirty) {
			ob->dirty = false;
			if (!no_dirty_edge) {
				(*walk->mon_blur_generation)++;
			}
		}
		walk->scene->blur_cache.present = true;
		walk->scene->blur_cache.prefix_end = walk->scene->len;
		walk->scene->blur_cache.bounds = (struct avk_box){
			dst.x, dst.y, dst.width, dst.height };
		walk->scene->blur_cache.levels =
			bd.num_passes > 0 ? (uint32_t)bd.num_passes : 0;
		/* Scaled exactly as a live blur node's radius is, and for the same
		 * reason: a radius is a length in output pixels. Diverging here would
		 * make a cached backdrop and a live one disagree on a 1.5x display. */
		walk->scene->blur_cache.radius = bd.radius * (float)walk->scale;
		walk->scene->blur_cache.brightness = bd.brightness;
		walk->scene->blur_cache.contrast = bd.contrast;
		walk->scene->blur_cache.saturation = bd.saturation;
		walk->scene->blur_cache.noise = bd.noise;
		walk->scene->blur_cache.apply_effects =
			blur_data_should_parameters_blur_effects(&bd);
		walk->scene->blur_cache.generation = *walk->mon_blur_generation;
		return;
	}
	}
}

/* ── the frame ──────────────────────────────────────────────────────────── */

/*
 * Is this output one AVK can render correctly right now?
 *
 * Every check here is a thing M3 does not implement. Answering "no" costs a
 * frame on the SceneFX path; answering "yes" when it is not true costs a
 * visibly wrong desktop, which is the failure mode that makes a new renderer
 * untrustworthy for months.
 */
static bool az_avk_output_supported(Monitor *m,
		struct wlr_color_transform *color_transform, const char **why) {
	struct wlr_output *output = m->wlr_output;

	/* Every `false` below names itself. The caller turns a refusal into a
	 * fatal error by default, and "AVK refused this output" without the reason
	 * is the diagnostic this whole change exists to stop producing. */
	*why = "unknown";

	/* Decided once, on the first frame, and permanent: an output whose frames
	 * cannot be handed over with a fence attached is not one AVK may present,
	 * however well it composites. */
	if (m->avk != NULL && m->avk->present_sync == AZ_AVK_PRESENT_SYNC_BROKEN) {
		*why = "explicit present-sync is broken on this output (frames cannot "
			   "be handed over with a fence attached)";
		return false;
	}

	/*
	 * ── M5.6: HDR IS NO LONGER REFUSED OUTRIGHT ───────────────────────────
	 *
	 * This was one condition covering two unrelated things, and only one of
	 * them may be lifted. az_output_may_drive() is the decision and it is pure,
	 * so the table is a unit test rather than something only a real HDR panel
	 * can exercise; see az_output_color.h for why each branch is what it is.
	 *
	 * The interlock that matters: an HDR output is accepted ONLY when the C6
	 * encode pass will actually run for it. Its scan-out buffer is PQ-encoded
	 * BT.2020, so compositing into it without the pass writes scene-linear
	 * values as though they were already electrical -- which is much worse than
	 * the SceneFX fallback this replaces, and worse on exactly the content the
	 * user enabled HDR for.
	 */
	if (!az_output_may_drive(&m->color_state,
			output->image_description != NULL, color_transform != NULL,
			az_avk_encode_pass_enabled(NULL))) {
		if (!avk.warned_color_transform) {
			avk.warned_color_transform = true;
			if (color_transform != NULL
					|| m->color_state.path == AZ_OUTPUT_PATH_FALLBACK) {
				/*
				 * M6C. Both profile forms are carried now -- matrix-shaper as a
				 * 3x3 and 256 taps (G2), everything else as a 65-cube -- so the
				 * old wording ("cannot express as a matrix and a curve") would
				 * send someone looking for work that is done. Reaching here with
				 * a profile means NEITHER form could be built: the file did not
				 * parse, or the transform would not evaluate. That is the thing
				 * worth naming.
				 */
				wlr_log(WLR_INFO, "AVK: %s has a colour transform this renderer "
					"could reduce to neither a matrix-shaper nor a 3D table; "
					"refusing the output rather than showing uncharacterised "
					"colour on a display someone measured", output->name);
			} else {
				wlr_log(WLR_INFO, "AVK: %s is presenting HDR but the M5 output "
					"encode pass is not enabled (AZ_M5_PATH_B=1); refusing "
					"rather than writing scene values into a PQ buffer -- this "
					"output stays on the SceneFX path", output->name);
			}
		}
		*why = (color_transform != NULL
				|| m->color_state.path == AZ_OUTPUT_PATH_FALLBACK)
			? "a colour transform this renderer cannot express as a matrix and "
			  "a curve (a cLUT ICC profile)"
			: "the output presents HDR but the M5 encode pass is not enabled "
			  "(AZ_M5_PATH_B=1), so scene-linear values would be written into "
			  "a PQ buffer";
		return false;
	}

	if (m->scene_output != NULL && m->scene_output->zoom > 1.0f) {
		if (!avk.warned_zoom) {
			avk.warned_zoom = true;
			wlr_log(WLR_INFO, "AVK: output magnification is not implemented "
				"yet; zoomed frames stay on the SceneFX path");
		}
		*why = "output magnification (zoom > 1) is not implemented";
		return false;
	}

	/* A software cursor used to send the whole frame back to SceneFX, because
	 * the only handle wlroots offers for compositing one is
	 * wlr_output_add_software_cursors_to_render_pass() -- a wlr_render_pass,
	 * which is exactly what must not be on this path. AVK draws it itself now;
	 * see az_avk_emit_cursors(). */

	return true;
}

/*
 * The cursor, composited by AVK.
 *
 * WHAT THIS READS, AND WHAT IT DELIBERATELY DOES NOT
 *
 * Geometry comes from `wlr_output_cursor`, whose fields are all public and
 * already in output-buffer pixels: wlr_output_cursor_move() scales x/y by the
 * output scale, and output_cursor_set_texture() scales the size and hotspot
 * the same way. Reimplementing that arithmetic here would be a second copy of
 * it that could disagree with the hardware plane about where the pointer is.
 *
 * The IMAGE comes from asteroidz's own cursor state, because
 * `wlr_output_cursor.texture` is a wlr_texture belonging to the GLES
 * compatibility renderer and AVK may not touch it. That split -- wlroots owns
 * position, asteroidz owns the picture -- is the whole of §5.4g's design.
 *
 * WHY IT DRAWS AT MOST ONE
 *
 * asteroidz has exactly one wlr_cursor, so an output has exactly one cursor
 * from it. Drawing every entry in output->cursors would mean drawing OUR image
 * at somebody else's coordinates should another ever appear, which is worse
 * than drawing nothing and much harder to notice.
 *
 * DAMAGE IS NOT DONE HERE, ON PURPOSE
 *
 * When a software cursor moves, wlroots emits wlr_output.events.damage for the
 * region it left and the region it entered, and scenefx feeds that straight
 * into scene_output->damage_ring (wlr_scene.c:3008) -- the same ring this
 * frame's damage was rotated out of. So the old and new cursor rectangles are
 * already in the damage AVK is drawing, and adding more here would only make
 * the frame bigger. What that does require is that the mechanism keeps
 * working, which is what contrib/avk-cursor-test.sh's damage assertions check.
 */
static void az_avk_emit_cursors(struct az_avk_walk *walk,
		struct wlr_output *output) {
	/*
	 * BREAK SWITCH: emit no cursor at all.
	 *
	 * With AVK compositing, nothing else draws the cursor into the frame --
	 * SceneFX is not running for this output -- so setting this makes the
	 * pointer vanish. That is what distinguishes "AVK draws the cursor" from
	 * "a cursor appears and nobody checked who put it there", which was true
	 * of every frame before this function existed.
	 */
	if (az_avk_env_flag("AZ_AVK_NO_CURSOR")) {
		return;
	}
	struct wlr_output_cursor *oc;
	wl_list_for_each(oc, &output->cursors, link) {
		if (!oc->enabled || !oc->visible) {
			continue;
		}
		if (output->hardware_cursor == oc) {
			/* On the plane. Costs this path nothing, which is the point of
			 * preferring it. */
			if (avk.cursor_was_software) {
				avk.cursor_sw_to_hw++;
				avk.cursor_was_software = false;
			}
			avk.hardware_cursor_frames++;
			return;
		}
		if (!avk.cursor_was_software) {
			avk.cursor_hw_to_sw++;
			avk.cursor_was_software = true;
		}

		if (az_cursor.buffer == NULL) {
			/* wlroots believes there is a visible cursor and asteroidz has no
			 * picture for it. That is precisely the M3.5E regression, so it
			 * gets a counter rather than a silent skip. */
			avk.cursor_no_image++;
			return;
		}

		/*
		 * BREAK SWITCH: draw at the pointer instead of at the pointer minus
		 * the hotspot.
		 *
		 * A compositor that ignores the hotspot draws the complete image,
		 * correctly, one hotspot away from where it belongs -- so it passes
		 * every assertion that counts pixels and only fails one that asks
		 * where they are. This is that assertion's falsifier.
		 */
		bool no_hotspot = az_avk_env_flag("AZ_AVK_NO_CURSOR_HOTSPOT");
		struct wlr_box box = {
			.x = (int)oc->x - (no_hotspot ? 0 : oc->hotspot_x),
			.y = (int)oc->y - (no_hotspot ? 0 : oc->hotspot_y),
			.width = (int)oc->width,
			.height = (int)oc->height,
		};
		/*
		 * `walk->transform`, NOT `output->transform`, and the difference is a
		 * frame wide: on a modeset frame the walk is drawn for the PENDING
		 * transform and this was the one call site still reading the committed
		 * one. It would have mapped the cursor through the transform the output
		 * is leaving into the attachment it is arriving at -- a cursor in the
		 * wrong corner for exactly one frame, which is the hardest kind of
		 * artefact to catch and the easiest to dismiss. Found by the M4F.2C.4e
		 * variable-reuse audit; the pair is now the walk's own, once.
		 */
		wlr_box_transform(&box, &box,
			wlr_output_transform_invert(walk->transform),
			walk->pres.width, walk->pres.height);
		if (box.width <= 0 || box.height <= 0) {
			return;
		}
		if (box.x >= walk->att.width || box.y >= walk->att.height ||
				box.x + box.width <= 0 || box.y + box.height <= 0) {
			avk.cursor_culled++;
			return;
		}

		struct avk_image *image = az_avk_image_for_buffer(az_cursor.buffer);
		if (image == NULL) {
			avk.cursor_import_failures++;
			return;
		}

		/*
		 * The pixels come from az_cursor and the box comes from wlroots. They
		 * describe the same cursor only while az_cursor is its sole owner, so
		 * that is checked here rather than assumed.
		 *
		 * wlroots sizes an output cursor as buffer_size / buffer_scale *
		 * output_scale, and az_cursor supplied both the buffer and the scale
		 * -- so the two must agree exactly. They stop agreeing the moment
		 * something selects a cursor through wlr_cursor_set_xcursor(), which
		 * puts a per-output image at the output's native scale into
		 * wlr_output_cursor while az_cursor still holds the old one: the box
		 * describes wlroots' image and the pixels are asteroidz's.
		 *
		 * That shipped. Dragging a window showed no grab cursor and resizing
		 * one on the coarser of two outputs made the arrow bigger, because
		 * 36px at scale 1.5 is 24 output px and wlroots' own choice was 28.
		 * Nothing counted it, so nothing could fail on it.
		 */
		if (az_cursor.scale > 0.0f) {
			int expect_w = (int)lroundf((float)image->extent.width
				/ az_cursor.scale * output->scale);
			int expect_h = (int)lroundf((float)image->extent.height
				/ az_cursor.scale * output->scale);
			if ((int)oc->width != expect_w || (int)oc->height != expect_h) {
				avk.cursor_geometry_mismatch++;
			}
		}
		struct avk_cmd *cmd = avk_scene_add(walk->scene, AVK_CMD_TEXTURE);
		if (cmd == NULL) {
			return;
		}
		cmd->dst = (struct avk_box){ box.x, box.y, box.width, box.height };
		cmd->image = image;
		if (wlr_fbox_empty(&oc->src_box)) {
			cmd->src = (struct avk_fbox){ 0, 0, image->extent.width,
				image->extent.height };
		} else {
			cmd->src = (struct avk_fbox){ oc->src_box.x, oc->src_box.y,
				oc->src_box.width, oc->src_box.height };
		}
		cmd->transform = az_avk_transform(wlr_output_transform_compose(
			wlr_output_transform_invert(oc->transform), walk->transform));
		/*
		 * Nearest at 1:1. A cursor is small, high-contrast and mostly edges,
		 * and smoothing one that needs no scaling is how a crisp pointer turns
		 * into a blurry one on a fractional-scale output.
		 *
		 * ── AND THE COMPARISON HAS TO BE IN ONE SPACE ─────────────────────
		 *
		 * `box` has been through wlr_box_transform(), which SWAPS width and
		 * height at 90, 270, flipped-90 and flipped-270. `cmd->src` is the
		 * cursor image and never rotates. Comparing the two directly asked
		 * "is a 24x22 destination the same size as a 22x24 source" -- which is
		 * false for every non-square cursor, so every rotated output silently
		 * switched the pointer to bilinear filtering. The default arrow here
		 * is 22x24, and the resulting blur was 464 differing pixels against
		 * the same desktop on an unrotated output: the cursor in exactly the
		 * right place, drawn slightly wrong.
		 *
		 * Nothing could see it before M4F.2C.4e, because no test had ever
		 * looked at a cursor on a rotated output -- the transform oracle
		 * parked the pointer deliberately, and the headless backend puts every
		 * cursor on a plane that does not exist unless something forces
		 * software cursors.
		 *
		 * So compare the PRESENTATION-space size, which is what oc->width and
		 * oc->height already are, against the source. Both are unrotated.
		 */
		cmd->filter_linear = (int)oc->width != (int)cmd->src.width ||
			(int)oc->height != (int)cmd->src.height;
		avk.cursor_commands++;
		avk.software_cursor_frames++;
		avk.cursor_damage_pixels += (uint64_t)box.width * (uint64_t)box.height;
		if (box.x != avk.cursor_last_x || box.y != avk.cursor_last_y) {
			avk.cursor_moves++;
			avk.cursor_last_x = box.x;
			avk.cursor_last_y = box.y;
		}
		return;
	}
}

/*
 * ── DETERMINISTIC ATTACHMENT CAPTURE ──────────────────────────────────────
 *
 * TEST ONLY. Armed per output by `amsg dispatch capture_output`.
 *
 * grim captures NOTHING from a 90 or 270 degree output on this backend, so
 * every pixel assertion at those transforms was skipped -- which is how a
 * rotated frame can be wrong for an entire milestone with nobody able to look
 * at it. This writes the image AVK ACTUALLY RENDERED, read back off the GPU
 * from the same scan-out target that is about to be presented, in the
 * attachment's own orientation and extent, at any transform.
 *
 * The wait here is a TEST-ONLY wait on a frame that has already been submitted.
 * It is not on the render path: `capture_pending` is false in every normal
 * frame and the tap is not even declared.
 */
static void az_avk_capture_frame(struct az_avk_output *out, Monitor *m,
		struct avk_image *target, uint64_t timeline) {
	struct avk_renderer *r = &out->slot->renderer;
	if (!out->capture_pending || timeline == 0) {
		return;
	}
	out->capture_pending = false;
	if (!avk_device_timeline_wait(r->dev, timeline, 2000000000ULL)) {
		wlr_log(WLR_ERROR, "capture: %s's frame did not complete",
			m->wlr_output->name);
		return;
	}
	const char *dir = getenv("AZ_AVK_CAPTURE_DIR");
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.ppm",
		dir != NULL ? dir : "/tmp", m->wlr_output->name);
	/*
	 * BOTH EXTENTS, NAMED, beside the file: a capture whose orientation is
	 * inferred by the reader is a capture that can be compared against the
	 * wrong reference. `att` is what the swapchain was allocated from and what
	 * the copy must cover; `pres` is the shape the reader has to canonicalise
	 * into before comparing it with anything.
	 *
	 * `img` is the image that was actually copied. It is printed rather than
	 * assumed because the whole of M4F.2C.4d was an attachment whose extent did
	 * not match the frame it was drawn for, and the first instrument that could
	 * have said so in one line was this one.
	 */
	const struct az_attachment_extent att =
		az_output_attachment_extent(m->wlr_output, NULL);
	const struct az_presentation_extent pres =
		az_presentation_of(att, m->wlr_output->transform);
	const bool extent_ok = (uint32_t)att.width == target->extent.width &&
		(uint32_t)att.height == target->extent.height;
	wlr_log(WLR_ERROR, "capture: %s transform=%d attachment=%dx%d "
		"presentation=%dx%d image=%ux%u row_bytes=%u %s -> %s",
		m->wlr_output->name, (int)m->wlr_output->transform,
		att.width, att.height, pres.width, pres.height,
		target->extent.width, target->extent.height,
		target->extent.width * 4u,
		extent_ok ? "extent=OK" : "extent=MISMATCH", path);
	avk_oracle_write_ppm(&r->oracle, path, target->format);
	/* Disarmed on the renderer only once no output still wants a capture; the
	 * flag is per output and the oracle is per FORMAT, so two outputs sharing a
	 * renderer must both have written before the tap goes away. */
	bool any = false;
	Monitor *om;
	wl_list_for_each(om, &mons, link) {
		if (om->avk != NULL && om->avk->capture_pending) {
			any = true;
		}
	}
	if (!any) {
		avk_oracle_disarm_capture(&r->oracle);
	}
}

/*
 * ── THE FIRST-DIVERGENCE ORACLE, DRIVEN ───────────────────────────────────
 *
 * TEST ONLY, AZ_FRAME_ORACLE=1. Runs after the production frame has been
 * submitted and before the compositor has done anything else with the snapshot.
 *
 * The reference is the SAME scene, rendered whole. "Whole" means the whole
 * SOURCE bounds, not the whole output: a blur's dependency reaches into the
 * halo, so a reference damaged only over the presentation rectangle would leave
 * the halo's source damage empty and reconstruct less than the production frame
 * did. That reference would be the weaker of the two and would hide exactly the
 * class of defect being hunted.
 *
 * Nothing here consumes damage, rotates a ring, touches buffer age, mutates the
 * scene or schedules a frame. It renders a finished snapshot a second time into
 * an image nothing presents.
 */
static void az_avk_oracle_frame(struct az_avk_output *out, Monitor *m,
		struct avk_image *target, const struct avk_scene *scene,
		struct wlr_buffer *buffer, const pixman_region32_t *ring_damage,
		const pixman_region32_t *in_damage, uint64_t timeline) {
	struct avk_renderer *r = &out->slot->renderer;
	if (!r->oracle.enabled || timeline == 0) {
		return;
	}
	struct wlr_output *output = m->wlr_output;

	/* Buffer identity, in first-seen order. Also the damage-history identity:
	 * wlr_damage_ring keys on the wlr_buffer. */
	int slot_index = -1;
	for (size_t i = 0; i < out->oracle_buf_len; i++) {
		if (out->oracle_bufs[i] == buffer) {
			slot_index = (int)i;
			break;
		}
	}
	if (slot_index < 0
			&& out->oracle_buf_len < sizeof(out->oracle_bufs)
				/ sizeof(out->oracle_bufs[0])) {
		slot_index = (int)out->oracle_buf_len;
		out->oracle_bufs[out->oracle_buf_len++] = buffer;
	}

	/* The production submission must have landed before its taps are read, and
	 * before the reference render re-acquires the same client dma-bufs. A wait
	 * here is legal precisely because this path is not the frame path. */
	if (!avk_device_timeline_wait(r->dev, timeline, 2000000000ULL)) {
		wlr_log(WLR_ERROR, "oracle: production frame %" PRIu64 " did not "
			"complete", out->frame_seq);
		return;
	}

	struct avk_image *ref = avk_oracle_ref_target(&r->oracle, target->format,
		target->extent.width, target->extent.height);
	if (ref == NULL) {
		return;
	}

	/*
	 * The reference scene: the same commands, the same source bounds, damaged
	 * everywhere source can be reconstructed.
	 *
	 * A shallow copy is correct and the region is the only field that must not
	 * be shared -- pixman_region32_init() over the copied header leaves the
	 * original's data owned by `scene`, which is finished by its own caller.
	 */
	struct avk_scene full = *scene;
	pixman_region32_init(&full.damage);
	struct avk_box sb = scene->source_bounds;
	if (sb.width <= 0 || sb.height <= 0) {
		sb = (struct avk_box){ 0, 0, (int32_t)target->extent.width,
			(int32_t)target->extent.height };
	}
	pixman_region32_union_rect(&full.damage, &full.damage, sb.x, sb.y,
		(unsigned)sb.width, (unsigned)sb.height);

	/* Saved across the reference render: the caller has already taken its copy,
	 * but a renderer whose published damage described the reference frame would
	 * be a trap for the next reader of it. */
	pixman_region32_t saved_damage;
	pixman_region32_init(&saved_damage);
	pixman_region32_copy(&saved_damage, &r->frame_damage);

	avk_oracle_begin(&r->oracle, AVK_ORACLE_REFERENCE);
	uint64_t ref_timeline = avk_render_frame(r, ref, &full, NULL, 0, NULL, 0);
	pixman_region32_fini(&full.damage);
	pixman_region32_copy(&r->frame_damage, &saved_damage);
	pixman_region32_fini(&saved_damage);
	if (ref_timeline == 0
			|| !avk_device_timeline_wait(r->dev, ref_timeline, 2000000000ULL)) {
		wlr_log(WLR_ERROR, "oracle: reference frame %" PRIu64 " did not "
			"complete", out->frame_seq);
		return;
	}

	struct avk_box bbox = { 0, 0, 0, 0 };
	int worst = 0;
	struct avk_oracle_sample sample;
	int64_t wrong = avk_oracle_compare(&r->oracle, AVK_TAP_OUTPUT, 0, &bbox,
		&worst, &sample);
	if (wrong < 0) {
		wlr_log(WLR_ERROR, "oracle: frame %" PRIu64 " on %s has no comparable "
			"output tap (%" PRId64 "); dropped=%" PRIu64, out->frame_seq,
			output->name, wrong, r->oracle.dropped);
		return;
	}

	/*
	 * THE FORENSIC RECORD, and only around the first divergence.
	 *
	 * One line per frame would bury the answer in a hundred thousand lines of
	 * agreement. The frame BEFORE is kept by logging every frame's one-line
	 * summary at DEBUG and the divergent frame's full record at ERROR, and the
	 * two frames after by `tail`.
	 */
	bool first = wrong > 0 && r->oracle.divergent_frame < 0;
	if (first) {
		r->oracle.divergent_frame = (int64_t)out->frame_seq;
		r->oracle.tail = 2;
	}
	bool loud = first || r->oracle.tail > 0;
	if (!first && r->oracle.tail > 0) {
		r->oracle.tail--;
	}

	int ring_rects = 0;
	pixman_region32_rectangles((pixman_region32_t *)ring_damage, &ring_rects);
	pixman_box32_t ring_ext = *pixman_region32_extents(
		(pixman_region32_t *)ring_damage);
	int in_rects = 0;
	pixman_region32_rectangles((pixman_region32_t *)in_damage, &in_rects);
	pixman_box32_t in_ext = *pixman_region32_extents(
		(pixman_region32_t *)in_damage);
	int fd_rects = 0;
	pixman_region32_rectangles(&r->frame_damage, &fd_rects);
	pixman_box32_t fd_ext = *pixman_region32_extents(&r->frame_damage);

	wlr_log(loud ? WLR_ERROR : WLR_DEBUG,
		"oracle %s frame=%" PRIu64 " tf=%d raster=%ux%u buf=%d/%zu "
		"ring=%d,%d..%d,%d(%drects) in_damage=%d,%d..%d,%d(%drects) "
		"frame_damage=%d,%d..%d,%d(%drects) "
		"cmds=%zu halo_rec=%" PRIu64 " dropped=%" PRIu64 " invalidated=%" PRIu64
		" WRONG=%" PRId64 " bbox=%d,%d..%d,%d worst=%d%s",
		output->name, out->frame_seq, (int)output->transform,
		target->extent.width, target->extent.height, slot_index,
		out->oracle_buf_len, ring_ext.x1, ring_ext.y1, ring_ext.x2, ring_ext.y2,
		ring_rects, in_ext.x1, in_ext.y1, in_ext.x2, in_ext.y2,
		in_rects, fd_ext.x1, fd_ext.y1, fd_ext.x2, fd_ext.y2, fd_rects,
		scene->len, m->scene_output->halo_damage_records, r->oracle.dropped,
		r->oracle.invalidated, wrong,
		bbox.x, bbox.y, bbox.x + bbox.width,
		bbox.y + bbox.height, worst,
		first ? "  <== FIRST DIVERGENCE" : "");

	if (!loud) {
		return;
	}

	/*
	 * BOUNDARY 1 AND 2, PER BLUR, on the frames that matter.
	 *
	 * The prefix comparison is the one that decides the class. If a blur's
	 * reconstructed scene prefix already differs from the reference's, the
	 * defect is in segmented source reconstruction and nothing downstream needs
	 * looking at; if it matches and the blur result does not, the chain's own
	 * mapping owns it; if both match, the output tap's difference is damage,
	 * scissor or buffer history.
	 */
	if (sample.found) {
		/* ONE PIXEL, WITH BOTH VALUES. The bbox says where; this says what,
		 * which is what a trace of a single stale pixel needs. */
		wlr_log(WLR_ERROR, "oracle   px %d,%d production=%u,%u,%u,%u "
			"reference=%u,%u,%u,%u", sample.x, sample.y,
			sample.production[0], sample.production[1], sample.production[2],
			sample.production[3], sample.reference[0], sample.reference[1],
			sample.reference[2], sample.reference[3]);
	}

	for (size_t i = 0; i < scene->len; i++) {
		if (scene->cmds[i].type != AVK_CMD_BLUR) {
			continue;
		}
		bool prod = avk_oracle_has_tap(&r->oracle, AVK_ORACLE_PRODUCTION,
			AVK_TAP_PREFIX, i);
		bool ref = avk_oracle_has_tap(&r->oracle, AVK_ORACLE_REFERENCE,
			AVK_TAP_PREFIX, i);
		const struct avk_cmd *c = &scene->cmds[i];
		if (!prod || !ref) {
			/*
			 * SAID OUT LOUD, because the silence was the answer once.
			 *
			 * On the first divergent frame of the 180-degree defect, EVERY blur
			 * was absent from the production render -- the frame arrived with
			 * no damage, so no blur had a result region and none ran -- and the
			 * loop skipped all of them. A dump that prints nothing for that
			 * frame reads like "all boundaries agree", which is the opposite of
			 * what happened.
			 */
			wlr_log(WLR_ERROR, "oracle   blur[%zu] dst=%d,%d %dx%d "
				"ABSENT from %s render (it produced no result region)",
				i, c->dst.x, c->dst.y, c->dst.width, c->dst.height,
				!prod ? "the PRODUCTION" : "the REFERENCE");
			continue;
		}
		struct avk_box pbox = { 0, 0, 0, 0 }, bbox2 = { 0, 0, 0, 0 };
		int pworst = 0, bworst = 0;
		int64_t pwrong = avk_oracle_compare(&r->oracle, AVK_TAP_PREFIX, i,
			&pbox, &pworst, NULL);
		int64_t bwrong = avk_oracle_compare(&r->oracle, AVK_TAP_BLUR, i,
			&bbox2, &bworst, NULL);
		wlr_log(WLR_ERROR, "oracle   blur[%zu] dst=%d,%d %dx%d "
			"PREFIX wrong=%" PRId64 " bbox=%d,%d..%d,%d worst=%d | "
			"BLUR wrong=%" PRId64 " bbox=%d,%d..%d,%d worst=%d",
			i, c->dst.x, c->dst.y, c->dst.width, c->dst.height,
			pwrong, pbox.x, pbox.y, pbox.x + pbox.width,
			pbox.y + pbox.height, pworst,
			bwrong, bbox2.x, bbox2.y, bbox2.x + bbox2.width,
			bbox2.y + bbox2.height, bworst);
	}
}

/*
 * Build a frame with AVK, or return false and let the caller fall back.
 *
 * This is the function that replaces wlr_scene_output_build_state() in AVK
 * mode, and it is deliberately the only one: everything it does not do --
 * direct scanout, gamma LUTs, dma-buf feedback -- is absent rather than half
 * present, and listed in docs/vulkan-native-architecture.md.
 */
static bool az_avk_build_frame(Monitor *m, struct wlr_output_state *state,
		struct wlr_color_transform *color_transform) {
	if (!avk.active || m->scene_output == NULL) {
		return false;
	}
	const char *refused_why = NULL;
	if (!az_avk_output_supported(m, color_transform, &refused_why)) {
		avk.fallback_frames++;
		/*
		 * ── FALLING BACK TO SceneFX IS FATAL ─────────────────────────────
		 *
		 * Returning false here hands the output to wlr_scene_output_build_state()
		 * and the desktop keeps working, composited by SceneFX on wlroots' GLES
		 * renderer. That is precisely the behaviour being removed: the picture
		 * stays plausible, so the refusal is discovered -- if ever -- as a
		 * performance mystery or a colour mystery, long after the frame that
		 * caused it, with a renderer nobody was investigating in the loop.
		 *
		 * A refusal is a bug in AVK or an output AVK must learn to drive.
		 * Either way the useful outcome is a stack and a reason at the moment
		 * it happens, not a second renderer quietly covering for it. So this
		 * aborts, naming the output and the condition.
		 *
		 * There is deliberately NO environment variable to re-enable the
		 * fallback. An escape hatch is a fallback with an extra step: it gets
		 * set once to get past a bad login, stays set, and the next refusal is
		 * silent again -- which is the exact failure this replaces.
		 */
		wlr_log(WLR_ERROR,
			"AVK REFUSED %s: %s. This compositor does not composite with GL, "
			"so there is nothing to fall back to and this is fatal.",
			m->wlr_output != NULL ? m->wlr_output->name : "(output)",
			refused_why != NULL ? refused_why : "unknown");
		abort();
	}

	struct timespec frame_t0;
	clock_gettime(CLOCK_MONOTONIC, &frame_t0);
	/*
	 * ── WHICH PART OF `record` ────────────────────────────────────────────
	 *
	 * The frame-stall line splits a slow frame into record/join/commit, and
	 * for a 43ms frame with join=0 that says only "the renderer", which is
	 * 800 lines of it. These four stamps name the part: draining the client
	 * copies, acquiring the output target, walking the scene, and recording
	 * the passes. Each of the first three has been guessed at and each guess
	 * was wrong -- transient image creation runs at 2.2 per frame with no
	 * stalls at all, and m5_encode_compiles has been 1 all session.
	 */
	struct timespec ts_drain = frame_t0, ts_acq = frame_t0, ts_walk = frame_t0;
	avk.frame_resolve_ns = 0;
	avk.frame_resolve_creates = 0;
	/* Zeroed here so what accumulates below belongs to THIS frame. */
	avk.frame_join_ns = 0;
	avk.frame_output = m->wlr_output;
	/* Any fence left from a frame that bailed out before submitting is closed
	 * here: it belongs to a submission that never happened. */
	for (uint32_t i = 0; i < avk.frame_acquire_count; i++) {
		close(avk.frame_acquire_fds[i]);
	}
	avk.frame_acquire_count = 0;

	/*
	 * Anything the worker finished but the loop has not noticed yet.
	 *
	 * Belt to the eventfd's braces, and it earns its place: the eventfd only
	 * fires when the loop reaches poll(), and a busy dispatch round can put a
	 * frame in front of it. Settling here means this frame samples the newest
	 * pixels rather than joining the copy a few lines later.
	 */
	az_avk_shm_drain();
	clock_gettime(CLOCK_MONOTONIC, &ts_drain);

	struct wlr_output *output = m->wlr_output;
	struct az_avk_output *out = m->avk;
	if (out == NULL) {
		out = calloc(1, sizeof(*out));
		if (out == NULL) {
			return false;
		}
		m->avk = out;
	}

	/*
	 * ── TWO RASTERS, AND THEY ARE NOT THE SAME ONE ────────────────────────
	 *
	 * `att`   the ATTACHMENT: the buffer this frame is drawn into and the size
	 *         the backend scans out. It is the MODE, never the transformed
	 *         resolution. wlr_scene asserts exactly this (`buffer->width ==
	 *         resolution_width` in wlr_scene_output_build_state), and on a real
	 *         KMS output a framebuffer of the other shape is not a rotated
	 *         picture, it is a rejected commit.
	 *
	 * `pres`  the same raster in the orientation the user sees, and the space
	 *         every node's geometry is in immediately before
	 *         wlr_box_transform() maps it into the attachment. This is
	 *         wlr_scene's `trans_width/trans_height`, and the logical size of
	 *         the output is exactly this over the scale.
	 *
	 * They differ by a transpose at 90 and 270 degrees, and ONE PAIR OF `int
	 * width, height` USED TO HOLD BOTH. wlr_output_transformed_resolution()
	 * gave the transformed pair, the modeset branch gave the untransformed
	 * pair, and the result was used for the swapchain AND for the walker's
	 * transform. So a 90-degree output allocated a 600x800 attachment and then
	 * mapped its geometry into an 800x600 space: the right-hand 200 columns of
	 * every node were clipped away and the bottom 200 rows of the attachment
	 * were never written at all. Measured on an 800x600 output at 90 degrees:
	 * the two bottom corners of the capture are black and 455417 of 480000
	 * pixels differ from the same desktop at 0 degrees.
	 *
	 * Nothing caught it because nothing could LOOK at it -- grim captures
	 * nothing from a rotated output on this backend, so every pixel assertion
	 * at 90 and 270 degrees had been skipped with a stated reason since M4F.2C.3.
	 * `amsg dispatch capture_output` is what finally made the frame visible.
	 *
	 * The two are now DISTINCT TYPES (src/render/az_extent.h) rather than a
	 * naming convention, so `att = pres` does not compile and the conversion
	 * has to be named at every boundary it crosses.
	 */
	const struct az_attachment_extent att =
		az_output_attachment_extent(output, state);
	const enum wl_output_transform tr = az_output_pending_transform(output, state);
	const struct az_presentation_extent pres = az_presentation_of(att, tr);

	uint32_t fourcc = (state->committed & WLR_OUTPUT_STATE_RENDER_FORMAT)
		? state->render_format : output->render_format;
	if (!az_attachment_extent_valid(att)) {
		avk.fallback_frames++;
		return false;
	}

	if (out->swapchain == NULL || !az_attachment_extent_eq(out->att, att)
			|| out->drm_format != fourcc) {
		struct wlr_drm_format format;
		if (!az_avk_pick_format(output, fourcc, att.width, att.height, &format)) {
			avk.fallback_frames++;
			return false;
		}
		if (out->swapchain != NULL) {
			wlr_swapchain_destroy(out->swapchain);
		}
		/* THE ATTACHMENT EXTENT, and there is no other candidate: a swapchain
		 * allocated from the presentation extent is the M4F.2C.4d defect. */
		out->swapchain = wlr_swapchain_create(alloc, att.width, att.height,
			&format);
		wlr_drm_format_finish(&format);
		if (out->swapchain == NULL) {
			wlr_log(WLR_ERROR, "AVK: could not create a %dx%d swapchain for %s",
				att.width, att.height, output->name);
			avk.fallback_frames++;
			return false;
		}
		out->att = att;
		out->drm_format = fourcc;

		const struct avk_drm_format *vk_fmt =
			avk_drm_format_from_fourcc(fourcc);
		out->vk_format = vk_fmt != NULL ? vk_fmt->vk : VK_FORMAT_UNDEFINED;
		out->slot = az_avk_renderer_for(out->vk_format);
		if (out->slot == NULL) {
			avk.fallback_frames++;
			return false;
		}
		wlr_log(WLR_INFO, "AVK: %s now composited at %dx%d, format 0x%08x",
			output->name, att.width, att.height, fourcc);
	}
	if (out->slot == NULL) {
		avk.fallback_frames++;
		return false;
	}

	/*
	 * ── M5/C6. WHICH RENDERER COMPOSITES THIS OUTPUT ──────────────────────
	 *
	 * On Path B the scene does not composite into the scan-out buffer at all --
	 * it composites into a scene-linear FP16 intermediate, and one encode pass
	 * writes the scan-out buffer (ADR-008). Dynamic rendering bakes the colour
	 * attachment's format into every pipeline, so that is a DIFFERENT renderer:
	 * az_avk_renderer_for() already keys one per VkFormat.
	 *
	 * ADR-012 -- "the blur transient format follows the path" -- then needs no
	 * code of its own. The blur transients, the M4I cache and the prefix
	 * captures are all allocated in `renderer->format`, so they become FP16 by
	 * the same act, and the cache's format field invalidates it across a path
	 * change without a line being written for that either.
	 *
	 * AZ_M5_PATH_B=1      drive Path B wherever C3 chose it (10-bit, HDR).
	 * AZ_M5_PATH_B=force  additionally put a Path-A output on it. THE TEST
	 *                     INSTRUMENT: it is what lets the SDR round-trip gate
	 *                     run on the same 8-bit output as Path A, which is the
	 *                     only comparison in which "within a code" means
	 *                     anything. Not a setting -- Path A is strictly cheaper
	 *                     where it is available.
	 */
	bool path_b_forced = false;
	const bool path_b = az_avk_encode_pass_enabled(&path_b_forced)
		&& (path_b_forced
			|| m->color_state.path == AZ_OUTPUT_PATH_B_ENCODE);
	/*
	 * Path A is decided here too, and not down beside the render call where it
	 * used to be, because the SCENE WALK needs to know. A config colour is an
	 * electrical value and has to be decoded before it enters a linear blend
	 * (az_avk_scene_rgb), and the walk runs long before the frame is recorded.
	 */
	/* Unset means ON here (D5), so the env flag is read for its ABSENCE as
	 * well as its value -- az_avk_env_flag() answers "is it truthy", which is
	 * false for both "unset" and "=0" and cannot tell them apart. */
	const char *path_a_env = getenv("AZ_M5_PATH_A");
	const bool path_a = (path_a_env == NULL || strcmp(path_a_env, "0") != 0)
		&& m->color_state.path == AZ_OUTPUT_PATH_A_DIRECT_SRGB;
	const bool linear_compose = path_a || path_b;
	const VkFormat compose_format = path_b
		? VK_FORMAT_R16G16B16A16_SFLOAT : out->vk_format;
	if (out->slot->format != compose_format) {
		struct az_avk_renderer_slot *slot = az_avk_renderer_for(compose_format);
		if (slot == NULL) {
			/* No renderer for the working format: keep the one we have, which
			 * renders the pre-M5 picture rather than nothing. Path B is then
			 * declined below because the intermediate's format would not match
			 * the renderer's. */
			wlr_log(WLR_ERROR, "AVK: no renderer slot for the %s working "
				"format on %s", path_b ? "Path B" : "direct", output->name);
		} else {
			wlr_log(WLR_INFO, "AVK: %s composites through %s", output->name,
				path_b ? "a scene-linear FP16 intermediate (Path B)"
					: "the scan-out buffer directly");
			out->slot = slot;
		}
	}

	/*
	 * An output that has LEFT Path B gives its intermediate back.
	 *
	 * 66 MB on a 4K output, and nothing would ever ask for it again -- the
	 * frame path only reads it while `path_b` holds, so without this it sits
	 * allocated until the monitor is unplugged. Retired against its own last
	 * use, like everything else here, so a frame still sampling it when HDR was
	 * toggled off keeps it alive.
	 */
	if (!path_b && out->encode_work.image != NULL) {
		avk_encode_intermediate_finish(&out->encode_work,
			out->slot->renderer.dev, &out->slot->renderer.retire);
	}

	/* Before anything is rendered: can this frame be handed over with a fence
	 * at the end of it? Finding out afterwards would mean either presenting it
	 * unsynchronised or throwing away work already submitted. */
	if (!az_avk_present_sync_prepare(out, output)) {
		avk.fallback_frames++;
		return false;
	}

	struct wlr_buffer *buffer = wlr_swapchain_acquire(out->swapchain);
	if (buffer == NULL) {
		wlr_log(WLR_ERROR, "AVK: no free buffer in %s's swapchain",
			output->name);
		avk.fallback_frames++;
		return false;
	}
	struct az_avk_target *target_rec = az_avk_target_for_buffer(buffer);
	if (target_rec == NULL) {
		wlr_buffer_unlock(buffer);
		avk.fallback_frames++;
		return false;
	}
	struct avk_image *target = target_rec->image;

	/* Is the display engine still reading it? Plus one slot per client
	 * acquire fence collected during the walk below. */
	VkSemaphoreSubmitInfo waits[2 + AZ_AVK_MAX_ACQUIRE];
	uint32_t wait_count = 0;
	if (!az_avk_target_acquire(out, target_rec, buffer, waits, &wait_count)) {
		wlr_log(WLR_ERROR, "AVK: %s's swapchain handed back a target the "
			"display engine has not released", output->name);
		wlr_buffer_unlock(buffer);
		avk.fallback_frames++;
		return false;
	}

	/* ── the snapshot ───────────────────────────────────────────────────── */
	struct avk_scene scene;
	clock_gettime(CLOCK_MONOTONIC, &ts_acq);
	avk_scene_init(&scene);

	/*
	 * What has to be redrawn into THIS buffer.
	 *
	 * Not "what changed since the last frame" -- that would be right only for a
	 * swapchain one buffer deep. A target that last held frame N-3 needs
	 * everything that has changed since frame N-3, and the ring is what knows
	 * that, per buffer, because it is keyed on the wlr_buffer itself. A buffer
	 * it has never seen comes back fully damaged, which is exactly right for a
	 * target whose contents are undefined.
	 *
	 * It is the *scene's* ring, deliberately, and shared with the SceneFX path:
	 * a frame that falls back renders into a different buffer, and the ring
	 * accounts for both because it tracks buffers rather than frames. Keeping a
	 * second ring here would mean each path silently forgetting the other's
	 * frames.
	 *
	 * rotate_buffer MUTATES the ring -- it moves `current` into this buffer's
	 * entry -- so it happens after every check that can still decline the
	 * frame, and every failure below it has to trash the ring. Rotating and
	 * then not rendering is how a region gets acknowledged without ever being
	 * drawn, which shows up as a stale rectangle that survives until something
	 * else happens to damage it.
	 */
	/*
	 * Taken BEFORE the rotate, so it names damage this rotate is responsible
	 * for delivering. Read afterwards it would also catch records added between
	 * the rotate and here, which belong to the NEXT frame -- and a lost-damage
	 * check built on that would report a loss every time the scene was busy.
	 */
	uint64_t halo_recs_before = m->scene_output->halo_damage_records;

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	wlr_damage_ring_rotate_buffer(&m->scene_output->damage_ring, buffer,
		&damage);
	/*
	 * WHAT THE RING HANDED OVER, kept apart from what is rendered.
	 *
	 * `damage` is about to be replaced by the renderer's own post-blur figure,
	 * and AZ_AVK_FULL_DAMAGE / AZ_AVK_DAMAGE_HOLE may rewrite it before that.
	 * A forensic line that printed the variable at the end would be reporting
	 * three different quantities under one name -- which it did, and the
	 * resulting "no frame ever received out-of-bounds damage" was true of the
	 * post-clip figure and said nothing at all about the ring.
	 */
	pixman_region32_t ring_damage;
	pixman_region32_init(&ring_damage);
	pixman_region32_copy(&ring_damage, &damage);
	/*
	 * FRAMES THAT TOOK DAMAGE FROM OUTSIDE THEIR OWN OUTPUT, counted from the
	 * scene's own record of having routed some.
	 *
	 * This used to test the returned region's EXTENTS for a coordinate outside
	 * the output, and it was the fourth instrument in M4F able to describe work
	 * that was never done. It counted 4 frames on a run where the answer was
	 * zero: a wlr_damage_ring cannot return an out-of-bounds rectangle at all
	 * (rotate_buffer intersects with the buffer), so the condition could only
	 * ever fire on an EMPTY region, whose pixman extents are a degenerate box
	 * left over from the last intersect -- literally `-126,2..-126,2`, with a
	 * rectangle count of zero. The assertion that cross-output damage routing
	 * worked was passing on that.
	 *
	 * halo_damage_records is incremented by scene_output_damage() when it
	 * actually records some, so a delta across this frame is the real thing.
	 */
	/* THE LARGEST any output has recorded, not the last one's -- a
	 * last-writer-wins counter on a two-output desktop reports whichever
	 * monitor rendered most recently, which is the wrong one half the time.
	 * The invariant is zero, so a maximum states it exactly. */
	if (m->scene_output->ring_out_of_bounds > avk.damage_ring_out_of_bounds) {
		avk.damage_ring_out_of_bounds = m->scene_output->ring_out_of_bounds;
	}
	if (halo_recs_before != out->last_halo_records) {
		avk.blur_halo_damage_frames++;
		/*
		 * ── HALO DAMAGE THAT WENT IN AND DID NOT COME OUT ─────────────────
		 *
		 * THE INVARIANT: if the scene recorded damage for this output on
		 * behalf of a neighbour since this output last drew, this rotate must
		 * return something. It is the whole point of recording it.
		 *
		 * It was violated on EVERY record until M4F.2C.4c, and silently: a
		 * wlr_damage_ring intersects what it returns with the buffer
		 * rectangle, so the out-of-bounds rectangles the halo path used to
		 * record could not survive the trip. 26 records in, 0 rectangles out,
		 * and the output rendered a frame with nothing in it while a strip of
		 * its blur stayed stale.
		 *
		 * This is the counter that says so directly, rather than leaving it to
		 * a screenshot taken after the fact -- which only catches the loss when
		 * no later damage happens to cover the same pixels, about two runs in
		 * three. See AZ_SCENE_HALO_DAMAGE_RAW.
		 */
		if (!pixman_region32_not_empty(&damage)) {
			avk.blur_halo_damage_lost++;
		}
	}
	out->last_halo_records = halo_recs_before;
	if (az_avk_env_flag("AZ_AVK_FULL_DAMAGE")) {
		/* What M3b did on every frame. Kept as a switch because it is the
		 * reference a damage test compares against: the same scene, redrawn
		 * whole, is the only thing that can say whether the partially redrawn
		 * one is right. */
		pixman_region32_clear(&damage);
		/* THE ATTACHMENT, because damage is an attachment-space quantity all
		 * the way to KMS. Full damage expressed in the presentation extent
		 * would cover a transposed rectangle on a rotated output. */
		pixman_region32_union_rect(&damage, &damage, 0, 0,
			att.width, att.height);
	}
	/*
	 * AZ_AVK_DAMAGE_HOLE=x,y,w,h punches that rectangle OUT of every frame's
	 * damage, in output pixels. TEST ONLY.
	 *
	 * A persistence test needs a build that strands a region on purpose, or it
	 * cannot show that it would notice one -- and "no stale pixels" from a
	 * fixture that has never seen a stale pixel is not a result. The hole is
	 * subtracted AFTER the ring has been rotated, so the region counts as
	 * acknowledged and is never redrawn: precisely the shape of the bug, which
	 * is a region wrongly believed to be up to date rather than one that is
	 * merely skipped this frame.
	 */
	const char *hole = getenv("AZ_AVK_DAMAGE_HOLE");
	if (hole != NULL) {
		int hx, hy, hw, hh;
		if (sscanf(hole, "%d,%d,%d,%d", &hx, &hy, &hw, &hh) == 4 && hw > 0 &&
				hh > 0) {
			pixman_region32_t cut;
			pixman_region32_init_rect(&cut, hx, hy, (unsigned)hw, (unsigned)hh);
			pixman_region32_subtract(&damage, &damage, &cut);
			pixman_region32_fini(&cut);
			if (!avk.warned_damage_hole) {
				avk.warned_damage_hole = true;
				wlr_log(WLR_ERROR, "AZ_AVK_DAMAGE_HOLE=%s -- %dx%d at %d,%d will "
					"never be redrawn. This build is deliberately broken.",
					hole, hw, hh, hx, hy);
			}
		} else if (!avk.warned_damage_hole) {
			avk.warned_damage_hole = true;
			wlr_log(WLR_ERROR, "AZ_AVK_DAMAGE_HOLE=%s is not x,y,w,h -- ignored",
				hole);
		}
	}
	pixman_region32_copy(&scene.damage, &damage);

	/* Cleared black underneath everything. asteroidz's own root_bg rect
	 * normally covers the output, but a clear costs one scissored draw and
	 * means an uncovered region is black rather than the previous frame. */
	scene.has_clear = true;
	scene.clear_color[3] = 1.0f;

	struct az_avk_walk walk = {
		.scene = &scene,
		.scale = output->scale,
		/* The PENDING transform and the LOGICAL-orientation raster: these two
		 * are what wlr_box_transform() needs as its source space, and a
		 * modeset frame must use the state's transform rather than the
		 * committed one for the same reason it uses the state's mode. */
		.transform = tr,
		.pres = pres,
		.att = att,
		.ox = m->scene_output->x,
		.oy = m->scene_output->y,
		.blur = wlr_scene_get_blur_data(m->scene_output->scene),
		.mon_blur_generation = &out->blur_cache_generation,
		.mon_blur_node = m->blur,
		/* ADR-003: one reference for the whole scene, read once per frame. */
		.scene_ref_nits = config.sdr_reference_luminance,
		/* M5: whether this output composites in linear light, which decides
		 * what a config colour means. See az_avk_scene_rgb(). */
		.linear_compose = linear_compose,
	};

	/*
	 * ── THE SOURCE HALO ───────────────────────────────────────────────────
	 *
	 * How far past this output's edge a blur presented on it can reach for its
	 * source, in this output's own pixels. Computed ONCE per frame from the
	 * scene's kernel, because retention has to be decided before the walk finds
	 * the blur node that will want the pixels.
	 *
	 * The radius is scaled here for the same reason it is scaled at the node:
	 * `radius` is a multiplier on a half-texel step and the texels are output
	 * pixels. A node's own `strength` can only ever REDUCE the kernel
	 * (blur_data_apply_strength interpolates towards no blur at all), so the
	 * scene's value bounds every node's.
	 *
	 * Zero when the scene has no blur, and that is the direct path: no halo, no
	 * retained commands, no widened source bounds, nothing for M4E's frame to
	 * carry.
	 */
	/*
	 * `config.blur` AS WELL AS THE KERNEL, and it is not redundant.
	 *
	 * `effects { blur { enable 0 } }` stops asteroidz creating blur NODES --
	 * client_update_blur, layer_update_blur and popup_update_blur all consult
	 * it -- but leaves the SCENE's kernel at whatever `passes` and `radius`
	 * say, so is_scene_blur_enabled() stays true. Gating on the kernel alone
	 * measured a 71 px halo and 5 retained commands on a desktop that could not
	 * contain a single blur node, which is the direct path paying for a feature
	 * it does not use.
	 *
	 * The two together are exactly "can a blur node exist here": no node can be
	 * created without config.blur, and no node blurs anything without a kernel.
	 */
	if (config.blur && is_scene_blur_enabled(&walk.blur)) {
		struct avk_blur_params kernel = {
			.levels = walk.blur.num_passes > 0
				? (uint32_t)walk.blur.num_passes : 0,
			.radius = walk.blur.radius * (float)output->scale,
		};
		walk.halo = (int)avk_blur_support_bound(&kernel);
		/*
		 * Told to the SCENE too, so damage landing in the halo is recorded for
		 * this output instead of being clipped away. Set every frame rather
		 * than once: the kernel is configurable at runtime and the output's
		 * scale can change, and a halo that lagged either would silently record
		 * damage for the wrong distance.
		 */
		wlr_scene_output_set_blur_halo(m->scene_output, walk.halo);
		/* IN ATTACHMENT PIXELS, because every blur write region it is
		 * compared against is a transformed node box. Using the logical
		 * orientation here would widen the wrong axis on a rotated output. */
		scene.source_bounds = (struct avk_box){
			-walk.halo, -walk.halo,
			(int32_t)att.width + 2 * walk.halo,
			(int32_t)att.height + 2 * walk.halo,
		};
		/* The LARGEST halo in play, not the last output's. Two outputs at
		 * different scales compute different halos from the same kernel, and a
		 * last-writer-wins counter reports whichever monitor happened to render
		 * most recently -- which on a mixed-scale desktop is the wrong one half
		 * the time. */
		if ((uint64_t)walk.halo > avk.blur_halo_px) {
			avk.blur_halo_px = (uint64_t)walk.halo;
		}
	} else {
		/* No blur in the scene: no halo, and the fork goes back to clipping
		 * damage to the output exactly as it always did. This is the direct
		 * path, and it must cost nothing at all. */
		wlr_scene_output_set_blur_halo(m->scene_output, 0);
	}
	az_avk_walk_children(&walk, &m->scene_output->scene->tree, 0, 0);
	/*
	 * THE COMMAND STREAM, ONCE, AT DEBUG.
	 *
	 * The node dump above says what the walk SAW; this says what it EMITTED, and
	 * with the index each command landed at. That index is the k a blur's prefix
	 * is replayed for -- so "the blur is at 3 and the window's surface is at 5"
	 * is the scene-order claim stated as a fact about the stream rather than
	 * inferred from a picture. contrib/avk-blur-walker-test.sh asserts on these
	 * lines; a test that could only look at pixels could not tell a blur in the
	 * wrong PLACE from a blur with the wrong SOURCE.
	 */
	if (az_avk_dumping()) {
		for (size_t i = 0; i < scene.len; i++) {
			const struct avk_cmd *c = &scene.cmds[i];
			/* The clip's RECTANGLE COUNT, not just whether it exists. A
			 * multi-rectangle region that arrived as one rectangle has had its
			 * bounding box taken somewhere, and that is invisible in every
			 * other number here. */
			int nrects = 0;
			if (c->has_clip) {
				pixman_region32_rectangles(
					(pixman_region32_t *)&c->clip, &nrects);
			}
			/* M5: the resolved luminance domain, on the dump line rather than
			 * in a counter, because "which curve did THIS surface get" is a
			 * per-command question and a total cannot answer it. A PQ client
			 * whose domain came out sRGB is invisible in the totals -- every
			 * other surface on the desktop is legitimately sRGB and swamps it. */
			wlr_log(WLR_ERROR, "cmd[%zu] %s dst=%d,%d %dx%d clip=%drects%s"
				" lum[tf=%s prim=%s scale=%.3f peak=%.3f]", i,
				avk_cmd_kind_name(c->type),
				c->dst.x, c->dst.y, c->dst.width, c->dst.height, nrects,
				c->type == AVK_CMD_BLUR ? " (blur)" : "",
				az_tf_name(c->lum.tf),
				c->lum.primaries == AZ_PRIM_BT2020 ? "2020" : "709",
				(double)c->lum.scale, (double)c->lum.content_peak);
		}
		/* One frame, whichever way it was asked for. Every output renders its
		 * own frame, so this fires for the first one only -- which is what a
		 * caller reading "the command stream" wants. */
		az_avk_dump_armed = false;
	}
	/* Last, so it is above everything: layer-shell overlays, fullscreen
	 * windows, the lock screen and the overview alike. A cursor drawn under
	 * any of them is a cursor the user cannot find. */
	az_avk_emit_cursors(&walk, output);
	clock_gettime(CLOCK_MONOTONIC, &ts_walk);

	/* The extra signal is the frame's completion as something exportable: a
	 * timeline semaphore cannot become a sync_file, so a binary one rides
	 * alongside it purely to be turned into a file descriptor a moment later. */
	VkSemaphoreSubmitInfo signals[1] = {{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = avk_sync_signal_semaphore(&out->sync),
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	}};
	/*
	 * M4H.7. This output's deadline, refreshed every frame rather than cached:
	 * a mode set changes the refresh, and a budget captured once would go on
	 * scoring frames against a rate the display no longer runs at. refresh is
	 * in mHz. A zero or absent refresh leaves the budget at 0, which DISABLES
	 * the accounting rather than defaulting to a number -- an invented budget
	 * scores silently and is worse than no score.
	 */
	int32_t refresh_mhz = output != NULL && output->current_mode != NULL
		? output->current_mode->refresh : 0;
	avk_timestamps_set_output(&out->slot->renderer.timestamps,
		output != NULL ? output->name : NULL);
	avk_timestamps_set_budget(&out->slot->renderer.timestamps,
		refresh_mhz > 0 ? (uint64_t)(1000000000000.0 / (double)refresh_mhz) : 0);
	avk_oracle_begin(&out->slot->renderer.oracle, AVK_ORACLE_PRODUCTION);
	/*
	 * M4I. THIS OUTPUT'S background blur cache, lent to the renderer for the
	 * frame and taken back at the end of it.
	 *
	 * Lent rather than owned because a renderer is shared by every output with
	 * the same VkFormat, and this resource is per output -- leaving it attached
	 * would let DP-1's frame sample HDMI-A-1's blurred wallpaper, at the wrong
	 * resolution, on whichever output happened to render second. Cleared
	 * afterwards so that a code path which forgets to set it gets a NULL and
	 * takes the live path, rather than silently inheriting the last output's.
	 */
	out->slot->renderer.blur_cache = &out->blur_cache;
	/*
	 * ── M5 PATH A, LENT THE SAME WAY AND FOR THE SAME REASON ──────────────
	 *
	 * Which colour path an output takes is a property of that output, and a
	 * renderer is shared by every output with the same VkFormat -- so leaving
	 * these set would decode DP-1's sources on HDMI-A-1's frame the moment the
	 * two disagreed. Cleared afterwards so a path that forgets to set them
	 * renders the pre-M5 picture rather than inheriting the last output's.
	 *
	 * ── M6B/D5: ON BY DEFAULT, WHEREVER C3 CHOSE IT ──────────────────────
	 *
	 * It was opt-in for a milestone, on the stated grounds that it had not been
	 * watched live rather than that it was wrong -- and "not yet watched" is a
	 * condition that runs forever if nothing ends it. D5 ends it: Path A is the
	 * default for 8-bit SDR outputs, with the same flag shape as AZ_M5_PATH_B.
	 *
	 *   unset   on wherever C3 chose Path A
	 *   =0      off -- THE BISECT HANDLE, restoring pre-M5 blending exactly
	 *   =1      on (the old opt-in spelling, kept working)
	 *
	 * What Path A changes is WHERE the encode happens, not whether: composition
	 * blends scene-linearly through the scanout buffer's _SRGB view, so the
	 * hardware decodes on sample and encodes on write. Storage stays 8-bit
	 * sRGB-coded -- the same quantisation as before, neither better nor worse --
	 * and the round trip is closed at ZERO codes by the on-GPU gate.
	 *
	 * BLENDED pixels do change, and that is the feature rather than a
	 * regression: blending in linear light is what ADR-005 is for. It is also
	 * the one thing a fixture cannot call correct or incorrect on its own, which
	 * is why D5 gates the promotion on the operator watching it once.
	 */
	/*
	 * ── PATH B, LENT THE SAME WAY ─────────────────────────────────────────
	 *
	 * The intermediate and the encode constants are both per OUTPUT and the
	 * renderer is shared by every output using its VkFormat, so both are lent
	 * for the frame and taken back after it. A forgotten clear here would encode
	 * one monitor's frame with another's curve, peak and matrix -- which on two
	 * SDR outputs is invisible and on an SDR/HDR pair is the whole picture.
	 *
	 * `encode_full_frame` is set exactly when the intermediate was just created:
	 * it holds nothing, and the damage handed in describes the age of the
	 * SCAN-OUT buffer, which rotates through several while this does not.
	 */
	if (path_b) {
		struct avk_image *had = out->encode_work.image;
		struct avk_image *inter = avk_encode_intermediate_get(&out->encode_work,
			out->slot->renderer.dev, &out->slot->renderer.retire,
			VK_FORMAT_R16G16B16A16_SFLOAT,
			target->extent.width, target->extent.height);
		out->slot->renderer.encode_intermediate = inter;
		out->slot->renderer.encode_full_frame = inter != NULL && inter != had;
		/*
		 * M6B/G2. The measured curve, uploaded only when the profile's serial
		 * has moved -- so on every frame but the first after a profile change
		 * this is a comparison of two integers.
		 */
		struct avk_image *lut = NULL;
		if (m->color_state.encode_tf == AZ_TF_LUT1D) {
			lut = avk_encode_lut_get(&out->encode_lut,
				out->slot->renderer.dev, &avk.importer.upload_ring,
				&out->slot->renderer.retire, m->icc_shaper.curve,
				m->icc_serial);
			if (lut == NULL) {
				/*
				 * REFUSE THE FRAME rather than encode without the curve. C3 put
				 * this output on LUT1D, which took the profile away from
				 * SceneFX; encoding with the sRGB analytic instead would show
				 * uncharacterised colour on a calibrated display and look
				 * entirely plausible. Falling back hands the frame -- and the
				 * profile -- back to the renderer that still has it.
				 */
				avk.fallback_frames++;
				return false;
			}
		}
		/*
		 * M6C. The cube, on exactly the terms above: uploaded only when the
		 * profile's serial has moved, and its absence REFUSES THE FRAME rather
		 * than encoding without it.
		 *
		 * The refusal matters more here than it did for the curve, because there
		 * is no second renderer left to fall back to: encoding a calibrated
		 * display with the sRGB analytic would be plausible, permanent and
		 * silent, where a refused frame is loud.
		 */
		if (m->color_state.encode_tf == AZ_TF_CLUT3D) {
			if (m->icc_clut == NULL || avk_encode_clut_get(&out->encode_clut,
					out->slot->renderer.dev, &avk.importer.upload_ring,
					&out->slot->renderer.retire, m->icc_clut->rgb,
					m->icc_clut->dim, m->icc_serial) == NULL) {
				avk.fallback_frames++;
				return false;
			}
		}
		out->slot->renderer.encode_params = az_avk_encode_params(m, lut,
			&out->encode_clut);
	}
	/*
	 * DECODE IS ON FOR BOTH PATHS, and it has to be: the two paths differ only
	 * in WHERE the encode happens (a fixed-function attachment view against a
	 * shader pass), never in whether the sources were decoded. Decode without
	 * encode washes every window out; encode without decode darkens everything.
	 */
	out->slot->renderer.decode_enabled = path_a || path_b;
	out->slot->renderer.encode_srgb = path_a;
	/*
	 * ── WAIT FOR THE CLIENTS THIS FRAME IS ABOUT TO READ ─────────────────
	 *
	 * Imported here rather than when collected, because a sync_file import is
	 * consumed by the submission that waits on it: importing at collection
	 * time would tie a fence to a frame that might still bail out below.
	 *
	 * A failed import is NOT skipped quietly. Dropping the wait would submit
	 * a frame that reads a buffer the client is still drawing into, which is
	 * indistinguishable from the corruption this whole path exists to remove
	 * -- so it is counted and named.
	 */
	for (uint32_t i = 0; i < avk.frame_acquire_count; i++) {
		VkSemaphore sem = avk_sync_import_sync_file(&out->sync,
			avk.frame_acquire_fds[i]);
		if (sem == VK_NULL_HANDLE) {
			avk.acquire_import_fails++;
			continue;
		}
		waits[wait_count++] = (VkSemaphoreSubmitInfo){
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = sem,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};
		avk.acquire_waits++;
	}
	/* Owned by the import now (or closed by the failure above). */
	avk.frame_acquire_count = 0;

	uint64_t timeline = avk_render_frame(&out->slot->renderer, target, &scene,
		waits, wait_count, signals, 1);
	/*
	 * ── decode_enabled AND encode_srgb OUTLIVE THE PRODUCTION RENDER ──────
	 *
	 * They are cleared AFTER the oracle below, not here, because the oracle's
	 * reference render is part of this frame's work and has to composite under
	 * the same conditions. Cleared here, the reference frame ran with decode
	 * OFF and no _SRGB attachment while production ran with both ON -- so every
	 * pixel of every frame differed and the oracle reported 18 of 20 frames
	 * divergent, WRONG=480000, worst 42 codes on a flat background.
	 *
	 * The instrument was disagreeing with itself. It went unnoticed for as long
	 * as Path A was opt-in and appeared the moment M6B/D5 made it the default,
	 * which is the same way the Path A fixture's own control arm was caught.
	 *
	 * The other three below are still cleared here: the intermediate and the
	 * blur cache are per-output resources the reference render must NOT reuse,
	 * and encode_full_frame describes a scan-out buffer the reference is not.
	 */
	out->slot->renderer.encode_intermediate = NULL;
	out->slot->renderer.encode_full_frame = false;
	out->slot->renderer.blur_cache = NULL;
	/*
	 * WHAT THE FRAME REDREW, WHICH MAY BE MORE THAN WHAT WAS HANDED IN.
	 *
	 * A blur turns a small source change into a wider result change: M4F.2B's
	 * forward sweep grows the damage by every blur's output damage, and the
	 * region below is what the backend, the accounting and wlr_scene's own
	 * pending-damage bookkeeping are all told about. Reading `damage` here
	 * instead would report the pre-blur figure and leave a stale blurred fringe
	 * on screen until something unrelated damaged it.
	 *
	 * Taken before avk_scene_finish() only because it is convenient to keep the
	 * two together; the region lives on the renderer, not the scene.
	 */
	if (timeline != 0) {
		pixman_region32_copy(&damage, &out->slot->renderer.frame_damage);
	}
	/* After the frame's own damage has been taken, and before the snapshot is
	 * released -- the reference render needs the same immutable scene. */
	az_avk_capture_frame(out, m, target, timeline);
	az_avk_oracle_frame(out, m, target, &scene, buffer, &ring_damage,
		&scene.damage, timeline);
	/* Now: the reference render is done and nothing else this frame composites.
	 * A path that forgets to set them next frame renders the pre-M5 picture
	 * rather than inheriting this output's, which is the property the original
	 * placement was protecting. */
	out->slot->renderer.decode_enabled = false;
	out->slot->renderer.encode_srgb = false;
	out->frame_seq++;
	avk_scene_finish(&scene);
	if (timeline == 0) {
		/* The ring has already been rotated, so the damage this frame was
		 * going to draw is now recorded as drawn. Trashing it is the only
		 * honest recovery: the next frame redraws everything rather than
		 * inheriting a region nobody ever painted. */
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
		pixman_region32_fini(&ring_damage);
		pixman_region32_fini(&damage);
		wlr_buffer_unlock(buffer);
		avk.fallback_frames++;
		return false;
	}
	target_rec->state = AZ_AVK_TARGET_RENDERED;

	/* Submitted. Now give the display engine something to wait on. */
	if (!az_avk_present_handover(out, target_rec, buffer, state)) {
		/* The work is already in flight and will complete; the buffer just
		 * never becomes a frame. SceneFX renders this one into a different
		 * buffer, so nothing is torn and nothing is lost but the effort. */
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
		pixman_region32_fini(&ring_damage);
		pixman_region32_fini(&damage);
		wlr_buffer_unlock(buffer);
		avk.fallback_frames++;
		return false;
	}

	/* Retire whatever the GPU has finished with. Once per frame, never
	 * blocking -- one vkGetSemaphoreCounterValue and a walk of a short list. */
	/* The submit cost of THIS frame, taken as the delta of the renderer's
	 * running total, so the distribution can be built without the renderer
	 * needing to know what a histogram is. */
	uint64_t submit_ns_now = out->slot->renderer.stats.cpu_record_ns;
	if (submit_ns_now > out->last_submit_ns) {
		az_avk_hist_add(&avk.gpu_submit_hist,
			(submit_ns_now - out->last_submit_ns) / 1000, 5);
	}
	out->last_submit_ns = submit_ns_now;

	avk_renderer_collect(&out->slot->renderer);
	az_avk_shm_staging_reap();
	avk_dmabuf_importer_collect(&avk.importer);

	wlr_output_state_set_buffer(state, buffer);
	/* set_buffer locks it; the reference wlr_swapchain_acquire() gave us is
	 * ours to drop. */
	wlr_buffer_unlock(buffer);

	/*
	 * What the BACKEND is told changed, which is a slightly different question
	 * from what we just redrew.
	 *
	 * The minimal answer is "the region that differs from what is currently on
	 * screen", and wlr_scene keeps exactly that -- but in a WLR_PRIVATE field,
	 * and reaching into one of those to save a few pixels of blit is not a
	 * trade worth making.
	 *
	 * The redraw region is the honest superset. It is damage accumulated since
	 * THIS buffer was last presented, and the on-screen buffer was presented
	 * more recently, so it contains the minimal answer and over-reports by at
	 * most the last few frames' damage. Over-reporting output damage costs the
	 * backend a little extra work and is never wrong; under-reporting leaves
	 * stale pixels on screen.
	 *
	 * Containing it also matters for a second reason. wlr_scene subtracts the
	 * committed damage from its own pending region, and
	 * wlr_scene_output_needs_frame() is true while that region is non-empty --
	 * so a report that did not cover it would leave the compositor rendering
	 * frames forever on an idle desktop.
	 */
	wlr_output_state_set_damage(state, &damage);

	/* Damage accounting. Both totals accumulate over the run, so
	 * damage_pixels/output_pixels is the fraction of the desktop AVK actually
	 * redrew -- the single number that says whether damage tracking is doing
	 * anything at all. */
	uint64_t damage_px = 0;
	int rect_count = 0;
	const pixman_box32_t *rects = pixman_region32_rectangles(&damage,
		&rect_count);
	for (int i = 0; i < rect_count; i++) {
		damage_px += (uint64_t)(rects[i].x2 - rects[i].x1)
			* (uint64_t)(rects[i].y2 - rects[i].y1);
	}
	uint64_t output_px = (uint64_t)att.width * (uint64_t)att.height;
	avk.damage_pixels += damage_px;
	avk.output_pixels += output_px;
	if (damage_px >= output_px) {
		avk.full_redraw_frames++;
	} else {
		avk.partial_redraw_frames++;
	}
	if (rect_count > (int)avk.damage_rects_max) {
		avk.damage_rects_max = (uint64_t)rect_count;
	}
	pixman_region32_fini(&ring_damage);
	pixman_region32_fini(&damage);

	struct timespec frame_t1;
	clock_gettime(CLOCK_MONOTONIC, &frame_t1);
	uint64_t us = (uint64_t)((frame_t1.tv_sec - frame_t0.tv_sec) * 1000000
		+ (frame_t1.tv_nsec - frame_t0.tv_nsec) / 1000);
	avk.cpu_frame_us += us;
	avk.frame_record_us = us;
	if (us > avk.cpu_frame_us_max) {
		avk.cpu_frame_us_max = us;
	}
	if (us > 20000) {
		/* Same 20ms the slow-pack log uses: past three frames, this is why
		 * something visibly stuttered. */
#define AZ_US(a, b) ((uint64_t)(((b).tv_sec - (a).tv_sec) * 1000000 \
	+ ((b).tv_nsec - (a).tv_nsec) / 1000))
		avk_log(AVK_ERROR, "slow record on %s: %" PRIu64 "us = drain %" PRIu64
			"us + acquire %" PRIu64 "us + walk %" PRIu64 "us + passes %"
			PRIu64 "us", output != NULL && output->name != NULL
				? output->name : "(output)", us,
			AZ_US(frame_t0, ts_drain), AZ_US(ts_drain, ts_acq),
			AZ_US(ts_acq, ts_walk), AZ_US(ts_walk, frame_t1));
		avk_log(AVK_ERROR, "  of which resolving client buffers: %" PRIu64
			"us over %" PRIu64 " resolve(s) that built a new image",
			avk.frame_resolve_ns / 1000, avk.frame_resolve_creates);
#undef AZ_US
	}
	az_avk_hist_add(&avk.cpu_frame_hist, us, AZ_AVK_CPU_BUCKET_US);
	if (output_px > 0) {
		az_avk_hist_add(&avk.damage_permille_hist,
			damage_px * 1000 / output_px, 2);
	}

	avk.frames++;
	return true;
}

/* ── taking ownership at commit ─────────────────────────────────────────── */

/*
 * When AVK acquires the content, and why it cannot be later.
 *
 * A client may destroy its wl_buffer the moment it has committed it. The old
 * architecture survived that because wlroots had already copied the pixels
 * into a renderer-owned wlr_texture; AVK has no such copy, so it has to take
 * its own ownership at the one moment the content is guaranteed valid -- the
 * commit itself.
 *
 * "Ownership" here does not mean holding the wlr_buffer. It means holding
 * something that outlives it: a dma-buf import dup()s every file descriptor,
 * and a CPU buffer is copied into an AVK image. After that the client can do
 * whatever it likes with the original.
 *
 * The pattern this replaces -- wait for a render frame, then hope the buffer
 * is still readable -- is not merely fragile. It produced exactly one visible
 * symptom, a missing wallpaper, and it would have produced others at random
 * for any client that draws once and lets go.
 */
struct az_avk_surface {
	struct wlr_surface *surface;
	struct wl_listener commit;
	struct wl_listener destroy;
	/* Every buffer this surface has committed and that AVK still caches an
	 * image for. See az_avk_buffer.pool_link for why the list has to exist. */
	struct wl_list pool;      /* az_avk_buffer.pool_link */

	/*
	 * ── SOMETHING TO DRAW WHILE A COPY IS IN FLIGHT ──────────────────────
	 *
	 * The last image this surface actually got onto the GPU. A frame that
	 * finds this surface's newest buffer still being copied draws this rather
	 * than blocking for up to 51ms.
	 *
	 * Per SURFACE and not per buffer, because per buffer was not enough: a
	 * client that allocates a fresh buffer every generation and destroys the
	 * old one leaves a pool in which every entry is either brand new or still
	 * copying. Measured live, that was every remaining stall --
	 * shm_stale_no_sibling and shm_async_join_waits were both exactly 41, of
	 * which 37 ran past 30ms, against a pool three deep every time.
	 *
	 * `owned` is the ownership handover. While the buffer that produced this
	 * image is alive the image belongs to its entry and this is only a
	 * borrowed pointer; when that entry is destroyed it hands the image over
	 * instead of retiring it, and from then on this surface retires it. Two
	 * owners would be a double destroy of a driver object, which is a double
	 * free of the driver's host memory.
	 */
	struct avk_image *last_good;
	bool owns_last_good;
	uint32_t last_good_width, last_good_height;
	/* Which generation `last_good` holds, so it can be COMPARED against the
	 * other candidates rather than only used when they are absent. An image
	 * without its generation cannot be ranked, and an unrankable fallback is
	 * how a stale picture wins over a fresh one. */
	uint64_t last_good_generation;
};

/* The image to draw when this buffer's own generation is not ready, and which
 * generation it holds. NULL if the surface has never completed an upload, or
 * did so at another size -- drawing that would be a stretched window rather
 * than a stale one. */
static struct avk_image *az_avk_surface_last_good(struct az_avk_buffer *entry,
		uint64_t *generation) {
	struct az_avk_surface *as = entry->as;
	if (as == NULL || as->last_good == NULL || entry->buffer == NULL) {
		return NULL;
	}
	if (as->last_good_width != (uint32_t)entry->buffer->width
			|| as->last_good_height != (uint32_t)entry->buffer->height) {
		return NULL;
	}
	if (generation != NULL) {
		*generation = as->last_good_generation;
	}
	return as->last_good;
}

/* Record a completed upload as this surface's fallback. */
static void az_avk_surface_note_good(struct az_avk_buffer *entry) {
	struct az_avk_surface *as = entry->as;
	if (as == NULL || entry->image == NULL || entry->buffer == NULL) {
		return;
	}
	if (as->last_good == entry->image) {
		return;
	}
	/* Only if this surface owns the outgoing one. If a live entry still owns
	 * it, that entry retires it when it goes. */
	if (as->owns_last_good && as->last_good != NULL) {
		avk_retire_push(&avk.importer.retire, avk.device,
			as->last_good->last_use, avk_image_destroy, as->last_good);
	}
	as->last_good = entry->image;
	as->owns_last_good = false;
	as->last_good_width = (uint32_t)entry->buffer->width;
	as->last_good_height = (uint32_t)entry->buffer->height;
	as->last_good_generation = entry->uploaded_generation;
}

/*
 * The entry is going away. If it owns the surface's fallback image, the
 * surface takes it over and the caller must not retire it.
 */
static void az_avk_surface_adopt_image(struct az_avk_buffer *entry) {
	struct az_avk_surface *as = entry->as;
	if (as == NULL || entry->image == NULL || as->last_good != entry->image) {
		return;
	}
	as->owns_last_good = true;
	/* Cleared so az_avk_buffer_destroy()'s retire is skipped: exactly one
	 * owner, and it is now the surface. */
	entry->image = NULL;
}

static struct wlr_surface *az_avk_surface_wlr(struct az_avk_buffer *entry) {
	return entry->as != NULL ? entry->as->surface : NULL;
}

/* Give up the fallback. Called when the surface itself goes. */
static void az_avk_surface_drop_last_good(struct az_avk_surface *as) {
	if (as->owns_last_good && as->last_good != NULL) {
		avk_retire_push(&avk.importer.retire, avk.device,
			as->last_good->last_use, avk_image_destroy, as->last_good);
	}
	as->last_good = NULL;
	as->owns_last_good = false;
}

/*
 * Bring `entry` into `as`'s pool, moving it out of any pool it was in.
 *
 * A buffer normally belongs to one surface for its whole life, but nothing in
 * the protocol requires that, and a buffer sitting in two pools would be
 * corrupt in a way that only shows up much later.
 */
static void az_avk_pool_add(struct az_avk_surface *as,
		struct az_avk_buffer *entry) {
	if (!wl_list_empty(&entry->pool_link)) {
		if (entry->pool_link.next == &as->pool
				|| entry->pool_link.prev == &as->pool) {
			return;   /* already ours */
		}
		wl_list_remove(&entry->pool_link);
		wl_list_init(&entry->pool_link);
	}
	wl_list_insert(&as->pool, &entry->pool_link);
	entry->pool_head = &as->pool;
	entry->as = as;
}

/*
 * Give a commit's damage to every buffer in the pool.
 *
 * The committed buffer needs it because its pixels just changed. The others
 * need it because their cached images are now one commit further out of date,
 * and the next time one of them is committed the client will only report what
 * changed since THAT commit -- never the difference from the copy AVK holds.
 *
 * Each entry clears its own pending_damage when it uploads, so a buffer that
 * is committed every frame carries one frame's damage, and one that comes
 * round every fourth frame carries four frames' worth. That is precisely the
 * buffer-age accumulation the source side was missing.
 */
static void az_avk_pool_note_damage(struct az_avk_surface *as,
		const pixman_region32_t *damage) {
	struct az_avk_buffer *entry;
	wl_list_for_each(entry, &as->pool, pool_link) {
		if (entry->is_shm) {
			az_avk_note_source_damage(entry, damage);
		}
	}
}

static void az_avk_surface_commit_inner(struct az_avk_surface *as,
		struct wlr_buffer *buffer) {
	struct wlr_addon *addon = wlr_addon_find(&buffer->addons, &avk,
		&az_avk_buffer_addon_impl);
	if (addon != NULL) {
		/*
		 * Already ours -- but NOT necessarily unchanged.
		 *
		 * A client is entitled to reuse a wl_buffer: draw into it again,
		 * attach the same object, commit. wlroots' own test for "the texture
		 * needs re-uploading" is exactly this commit carrying a buffer
		 * (`invalid_buffer` in surface_commit_state), so it is the right test
		 * here too.
		 *
		 * This function used to return here and do nothing, and the only
		 * reason a reused buffer ever showed its new pixels was that the
		 * frame path re-uploaded unconditionally. Take that away without
		 * putting this in and a terminal freezes on whatever it drew first.
		 */
		struct az_avk_buffer *entry = wl_container_of(addon, entry, addon);
		az_avk_pool_add(as, entry);
		/*
		 * RE-ATTACHING A BUFFER MEANS ITS CONTENTS MAY HAVE MOVED, whatever
		 * kind of buffer it is -- see avk_image.content_seq.
		 *
		 * Outside the is_shm branch below on purpose. That branch is about
		 * whether AVK has to re-UPLOAD, which only the CPU path does; this is
		 * about whether anything caching a picture derived from the image can
		 * still trust it, and a GPU client that renders into a released dmabuf
		 * and re-commits it has changed every pixel without a single upload.
		 * Missing that would leave the M4I cache blurring the old contents,
		 * which is the defect this exists to close.
		 */
		if (entry->image != NULL) {
			entry->image->content_seq++;
		}
		if (entry->is_shm) {
			entry->content_generation++;
			entry->stat_generations++;
			avk.shm_commits++;
			/* WHICH pixels, straight from wlroots. surface->buffer_damage is
			 * already buffer-local, already clipped to the buffer, and already
			 * has surface-coordinate damage folded through the surface's
			 * scale, transform and viewport -- see surface_update_damage(),
			 * which runs before this signal is emitted. Recomputing any of
			 * that here would be a second implementation to keep in step.
			 *
			 * To the whole pool, not to this buffer alone: the damage is
			 * stated against the previous SURFACE content, so every buffer
			 * behind this surface is owed it. Giving it only to the committed
			 * buffer is correct exactly while a client reuses one buffer, and
			 * silently wrong the moment it rotates two -- which is what every
			 * Qt/KDE application does, and what made them flicker on their
			 * own recently-changed pixels while a single-buffer test client
			 * passed. */
			az_avk_pool_note_damage(as, &as->surface->buffer_damage);
			/*
			 * And START THE COPY, rather than doing it.
			 *
			 * This is the moment the client's mapping is guaranteed to hold
			 * the committed content -- wlr_compositor unlocks
			 * surface->current.buffer as soon as this signal returns -- so it
			 * is the moment the copy has to be *begun*. It is not the moment
			 * it has to be *finished*, and treating those as the same thing is
			 * what put a 6.5 MB memcpy on the event loop.
			 *
			 * Declining is normal and costs nothing: an entry with a copy
			 * already in flight keeps its damage and gets a second job when
			 * that one settles, and a build with no worker falls through to
			 * the frame path exactly as before.
			 */
			az_avk_shm_job_start(entry);
		}
		return;
	}
	if (az_avk_image_for_buffer_ex(buffer, true) != NULL) {
		addon = wlr_addon_find(&buffer->addons, &avk,
			&az_avk_buffer_addon_impl);
		if (addon != NULL) {
			struct az_avk_buffer *entry = wl_container_of(addon, entry, addon);
			entry->taken_at_commit = true;
			az_avk_pool_add(as, entry);
			if (entry->is_shm) {
				avk.shm_commits++;
			}
		}
		avk.commit_imports++;
	}
}

/*
 * ── THE MEASUREMENT ──────────────────────────────────────────────────────
 *
 * Call to return, around the whole handler, because that is the interval the
 * compositor is not in poll() on account of this client -- which is precisely
 * what libinput was complaining about, and which nothing in the compositor
 * could see. A fix without this number is a story.
 *
 * Timed here rather than inside the copy so it keeps counting whatever else
 * this handler grows: an instrument that measures only the thing it was built
 * for stops being an instrument the moment the cost moves.
 */
static void az_avk_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct az_avk_surface *as = wl_container_of(listener, as, commit);
	struct wlr_buffer *buffer = as->surface->current.buffer;
	if (buffer == NULL) {
		return;
	}
	uint64_t t0 = az_avk_now_ns();
	az_avk_surface_commit_inner(as, buffer);
	uint64_t ns = az_avk_now_ns() - t0;
	avk.shm_commit_samples++;
	avk.shm_commit_ns_sum += ns;
	if (ns > avk.shm_commit_ns_max) {
		avk.shm_commit_ns_max = ns;
	}
	if (ns > 5000000ULL) {
		avk.shm_commit_over_5ms++;
	}
}

/*
 * The same question, asked for buffers that are not client surfaces.
 *
 * asteroidz draws its own titlebar text, icons and animation frames into cairo
 * buffers and hands them to the scene. Those have no wl_surface and therefore
 * no commit, so the hook above never sees them -- and without a second source
 * of generations they would be uploaded once and then frozen.
 *
 * Every one of today's producers (text-node.c, ufo-node.c, asteroid-break.h)
 * happens to allocate a FRESH wlr_buffer per update and drop the old one,
 * which would make buffer identity a valid content version for them. That was
 * checked rather than assumed -- and it is still not what this hangs on,
 * because it is a property of three call sites that a fourth could quietly
 * break, with the symptom being a frozen visual and no error anywhere.
 * SceneFX announcing the change is a rule; surveying the producers is a
 * snapshot.
 *
 * Surfaces are skipped here: they are covered by the commit hook, which knows
 * about ownership as well as about content, and bumping in both places would
 * upload every client surface twice per commit.
 */
static void az_avk_scene_content_notify(
		const struct wlr_scene_buffer_content_event *event, void *user_data) {
	(void)user_data;
	struct wlr_addon *addon = wlr_addon_find(&event->buffer->addons, &avk,
		&az_avk_buffer_addon_impl);
	if (addon == NULL) {
		/* Never seen: the first lookup will import it and upload generation 1,
		 * which is the current content by definition. */
		return;
	}
	struct az_avk_buffer *entry = wl_container_of(addon, entry, addon);
	if (!entry->is_shm || entry->taken_at_commit) {
		return;
	}
	entry->content_generation++;
	entry->stat_generations++;
	avk.shm_commits++;
	az_avk_note_source_damage(entry, event->damage);
}

static void az_avk_surface_destroy(struct wl_listener *listener, void *data) {
	struct az_avk_surface *as = wl_container_of(listener, as, destroy);
	wl_list_remove(&as->commit.link);
	wl_list_remove(&as->destroy.link);
	/*
	 * Empty the pool before the head goes away. Buffers outlive the surface
	 * that committed them -- a client can destroy a surface and keep its
	 * wl_buffers -- and an entry still linked here would hold a pointer into
	 * freed memory until its own destroy walked it.
	 */
	struct az_avk_buffer *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &as->pool, pool_link) {
		wl_list_remove(&entry->pool_link);
		wl_list_init(&entry->pool_link);
		/* And the POINTERS to this surface, not just the link. Both outlive
		 * it otherwise: pool_head into a list head inside `as`, and `as`
		 * itself. Either one read afterwards is a use-after-free reached from
		 * the frame path, which is the worst place to find one. */
		entry->pool_head = NULL;
		entry->as = NULL;
	}
	az_avk_surface_drop_last_good(as);
	free(as);
}

static void az_avk_new_surface(struct wl_listener *listener, void *data) {
	if (!avk.active) {
		return;
	}
	struct wlr_surface *surface = data;
	struct az_avk_surface *as = calloc(1, sizeof(*as));
	if (as == NULL) {
		return;
	}
	as->surface = surface;
	wl_list_init(&as->pool);
	as->commit.notify = az_avk_surface_commit;
	wl_signal_add(&surface->events.commit, &as->commit);
	as->destroy.notify = az_avk_surface_destroy;
	wl_signal_add(&surface->events.destroy, &as->destroy);
}

static struct wl_listener az_avk_new_surface_listener = {
	.notify = az_avk_new_surface,
};
static bool az_avk_new_surface_attached = false;

/*
 * Detach from wlroots before the display goes away.
 *
 * wlr_compositor asserts that nothing is still listening to new_surface when
 * the display is destroyed, and it is right to: a listener outliving the
 * signal is a use-after-free waiting for the next client. This has to run
 * before wl_display_destroy(), which is earlier than az_avk_finish().
 */
static void az_avk_detach(void) {
	/*
	 * The upload worker's event source goes HERE, not in az_avk_finish().
	 *
	 * cleanup() calls this before wl_display_destroy() and az_avk_finish()
	 * after it, and wl_event_source_remove() on a loop that no longer exists
	 * is a use-after-free -- the kind that survives long enough to be blamed
	 * on whatever runs next. The copies are settled at the same time, while
	 * wlroots is still whole: each one holds a client buffer lock and an open
	 * data-ptr access, and giving those back after the display has gone would
	 * be giving them back to freed objects.
	 */
	az_avk_shm_drain_blocking();
	if (avk.upload_source != NULL) {
		wl_event_source_remove(avk.upload_source);
		avk.upload_source = NULL;
	}
	if (az_avk_new_surface_attached) {
		wl_list_remove(&az_avk_new_surface_listener.link);
		az_avk_new_surface_attached = false;
	}
	wlr_scene_set_buffer_content_observer(NULL, NULL);
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

static void az_avk_log_forward(enum avk_log_level level, const char *fmt,
		va_list args) {
	enum wlr_log_importance importance;
	switch (level) {
	case AVK_ERROR:
	case AVK_WARN:
		importance = WLR_ERROR;
		break;
	case AVK_INFO:
		importance = WLR_INFO;
		break;
	default:
		importance = WLR_DEBUG;
		break;
	}
	char msg[1024];
	vsnprintf(msg, sizeof(msg), fmt, args);
	/* "avk: " so a line in the compositor log says which subsystem produced
	 * it without the reader having to recognise the wording. */
	wlr_log(importance, "avk: %s", msg);
}

/*
 * Bring the engine up, or fail loudly.
 *
 * `drm_fd` is the node the compositor presents on. AVK is told which device to
 * use rather than choosing one: a compositor that renders on a different GPU
 * from the one it scans out on is a real configuration, and one that does so
 * by accident is undiagnosable after the fact.
 */
static bool az_avk_init(int drm_fd) {
	wl_list_init(&avk.buffers);
	avk_log_set_handler(az_avk_log_forward);

	avk.instance = avk_instance_create("asteroidz");
	if (avk.instance == NULL) {
		return false;
	}
	avk_instance_log_caps(avk.instance);

	avk.device = avk_device_create(avk.instance, drm_fd);
	if (avk.device == NULL) {
		avk_instance_destroy(avk.instance);
		avk.instance = NULL;
		return false;
	}
	avk_device_log_caps(avk.device);

	if (!avk_dmabuf_importer_init(&avk.importer, avk.device)) {
		avk_device_destroy(avk.device);
		avk_instance_destroy(avk.instance);
		avk.device = NULL;
		avk.instance = NULL;
		return false;
	}
	avk_format_table_log(&avk.importer.table);

	/* From here on, "this buffer has new pixels" is an event rather than an
	 * assumption made once per frame. */
	wlr_scene_set_buffer_content_observer(az_avk_scene_content_notify, NULL);

	/*
	 * ── THE COPY THREAD ──────────────────────────────────────────────────
	 *
	 * Optional in the strongest sense: if the thread or the eventfd cannot be
	 * had, or if the event source cannot be added, every copy simply happens
	 * on this thread and the compositor is exactly as correct (and exactly as
	 * laggy) as it was before. A latency optimisation that can fail a startup
	 * is not worth having.
	 *
	 * The eventfd is what makes a completed copy cheap to notice: the loop is
	 * in poll() anyway, so the submission lands in the same dispatch round
	 * instead of waiting for the next frame to come looking.
	 */
	wl_list_init(&avk.shm_jobs);
	/*
	 * BEFORE ANY CLIENT CAN CONNECT, and therefore before wlroots has had cause
	 * to install its own SIGBUS handler. That ordering is the whole argument
	 * for why the two compose: wlroots saves whatever handler it finds and
	 * chains to it for addresses it does not recognise, so it must find ours.
	 * Install it later and wlroots would eventually restore SIG_DFL over us.
	 * See az_shm_source.h.
	 */
	az_shm_guard_install();
	if (az_avk_env_flag("AZ_AVK_SYNC_UPLOAD")) {
		wlr_log(WLR_INFO, "AZ_AVK_SYNC_UPLOAD=1 -- client SHM buffers are "
			"copied on the event loop, which is what makes libinput report "
			"lag; this is the BEFORE arm of that measurement");
	} else {
		avk.upload_worker = avk_upload_worker_create();
		if (avk.upload_worker != NULL) {
			avk.upload_source = wl_event_loop_add_fd(
				wl_display_get_event_loop(dpy),
				avk_upload_worker_fd(avk.upload_worker), WL_EVENT_READABLE,
				az_avk_upload_worker_readable, NULL);
			if (avk.upload_source == NULL) {
				wlr_log(WLR_ERROR, "AVK: the upload worker's completion fd "
					"could not be watched; copying on the event loop instead");
				avk_upload_worker_destroy(avk.upload_worker);
				avk.upload_worker = NULL;
			}
		}
		if (avk.upload_worker == NULL) {
			wlr_log(WLR_ERROR, "AVK: no upload worker; client SHM buffers "
				"will be copied on the event loop");
		}
	}

	avk.active = true;
	return true;
}

/*
 * `amsg get avk-stats`.
 *
 * Counters only mattered at shutdown until now, which made every question
 * about a running desktop ("is the copy path hot?", "did damage actually
 * shrink?") unanswerable without killing the thing you were measuring.
 *
 * Anything not yet measured is reported as JSON null rather than 0. A zero is
 * a measurement; a null is an admission. Reporting "gpu_frame_us: 0" for a
 * timing this build does not collect would be the kind of number that gets
 * quoted back later as evidence.
 */
/* Renderer stats live per format slot; the IPC view wants the total. Summed
 * by offset so adding a counter does not mean adding a function. */
static uint64_t az_avk_renderer_stat_sum(size_t off) {
	uint64_t total = 0;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			total += *(const uint64_t *)((const char *)
				&avk.renderers[i].renderer.stats + off);
		}
	}
	return total;
}

/* The same, for the gradient store's own counters. A separate function rather
 * than a second offset base because the two structs are different types and
 * folding them together would mean passing which one as well as where. */
static uint64_t az_avk_gradient_stat_sum(size_t off) {
	uint64_t total = 0;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			total += *(const uint64_t *)((const char *)
				&avk.renderers[i].renderer.gradients.stats + off);
		}
	}
	return total;
}

/*
 * THE WORK ACTUALLY DONE, as opposed to the work priced above.
 *
 * The distinction is the entire finding of P4b. `blur_prefix_rebuild_pixels`
 * is accumulated for every blur slot WHETHER OR NOT ITS CHAIN RUNS -- the
 * comment at its increment site says so, because a skipped blur's saving is
 * only meaningful against what it would otherwise have cost. It is a price
 * list, not an invoice.
 *
 * `rebuilds` is the invoice: how many times the monitor background blur was
 * genuinely re-rendered. A tag slide that leaves this flat did no blur work at
 * all, however large the price list looks.
 */
static void az_avk_blur_cache_counts(uint64_t *rebuilds, uint64_t *hits) {
	uint64_t r = 0, h = 0;
	Monitor *mm;
	wl_list_for_each(mm, &mons, link) {
		if (mm->avk == NULL) {
			continue;
		}
		r += mm->avk->blur_cache.rebuilds;
		h += mm->avk->blur_cache.hits;
	}
	*rebuilds = r;
	*hits = h;
}

static cJSON *az_avk_stats_json(void) {
	cJSON *o = cJSON_CreateObject();
	if (o == NULL) {
		return NULL;
	}
	if (!avk.active) {
		cJSON_AddStringToObject(o, "backend", "scenefx");
		cJSON_AddBoolToObject(o, "active", false);
		return o;
	}

	const struct avk_caps *caps = &avk.device->caps;
	cJSON_AddStringToObject(o, "backend", "avk");
	cJSON_AddBoolToObject(o, "active", true);
	cJSON_AddStringToObject(o, "physical_device", caps->device_name);
	cJSON_AddStringToObject(o, "driver", caps->driver_name);
	char drm[64];
	snprintf(drm, sizeof(drm), "%" PRId64 ":%" PRId64,
		caps->drm_render_major, caps->drm_render_minor);
	cJSON_AddStringToObject(o, "drm_device", drm);
	cJSON_AddBoolToObject(o, "wl_compositor_has_renderer", false);

	/* composition */
	uint64_t surfaces = 0, rects = 0, submit_ns = 0, sync_waits = 0;
	uint64_t decode_draws = 0, srgb_segs = 0;
	uint64_t encode_draws = 0, encode_px = 0, encode_compiles = 0;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (!avk.renderers[i].used) {
			continue;
		}
		const struct avk_renderer_stats *st = &avk.renderers[i].renderer.stats;
		surfaces += st->surfaces;
		rects += st->rects;
		decode_draws += st->decode_draws;
		srgb_segs += st->srgb_attach_segments;
		encode_draws += st->encode_draws;
		encode_px += st->encode_px;
		encode_compiles += avk.renderers[i].renderer.encode.compiles;
		submit_ns += st->cpu_record_ns;
		sync_waits += st->cpu_sync_waits;
	}
	cJSON_AddNumberToObject(o, "frames", (double)avk.frames);
	cJSON_AddNumberToObject(o, "fallback_frames", (double)avk.fallback_frames);
	cJSON_AddNumberToObject(o, "surfaces", (double)surfaces);
	cJSON_AddNumberToObject(o, "rects", (double)rects);
	/* M5/C7. The two halves of Path A, counted separately -- because "both are
	 * on" has been an inference from a pixel error and needs to be a reading. */
	cJSON_AddNumberToObject(o, "m5_decode_draws", (double)decode_draws);
	cJSON_AddNumberToObject(o, "m5_srgb_attach_segments", (double)srgb_segs);
	/*
	 * M5/C6. Path B, as three separate readings rather than one.
	 *
	 * draws is damage rectangles and px their area, so a frame that "ran the
	 * encode" over four pixels is distinguishable from one that encoded the
	 * display. compiles must STOP MOVING after each output's first frame: a
	 * pipeline compile on the frame path is a stall of milliseconds, and this is
	 * the only instrument that separates "the variant was cached" from "it was
	 * built again", which no timing percentile can.
	 */
	cJSON_AddNumberToObject(o, "m5_encode_draws", (double)encode_draws);
	cJSON_AddNumberToObject(o, "m5_encode_px", (double)encode_px);
	cJSON_AddNumberToObject(o, "m5_encode_compiles", (double)encode_compiles);
	{
		static const char *dv[] = { "none", "srgb", "gamma22", "bt1886", "pq" };
		for (int i = 0; i < AVK_DECODE_COUNT; i++) {
			uint64_t n = 0;
			for (size_t k = 0; k < AZ_AVK_MAX_FORMATS; k++) {
				if (avk.renderers[k].used) {
					n += avk.renderers[k].renderer.stats.decode_by_variant[i];
				}
			}
			char key[48];
			snprintf(key, sizeof(key), "m5_decode_%s", dv[i]);
			cJSON_AddNumberToObject(o, key, (double)n);
		}
	}

	/* import */
	const struct avk_dmabuf_importer *imp = &avk.importer;
	cJSON_AddNumberToObject(o, "dmabuf_zero_copy",
		(double)(imp->imports_explicit + imp->imports_recovered));
	cJSON_AddNumberToObject(o, "dmabuf_explicit_modifier",
		(double)imp->imports_explicit);
	cJSON_AddNumberToObject(o, "dmabuf_implicit_fallback",
		(double)imp->imports_copied);
	cJSON_AddNumberToObject(o, "dmabuf_import_failures",
		(double)imp->imports_failed);
	cJSON_AddNumberToObject(o, "shm_upload_bytes",
		(double)avk.shm_upload_bytes);
	cJSON_AddNumberToObject(o, "shm_commits", (double)avk.shm_commits);
	cJSON_AddNumberToObject(o, "shm_full_uploads",
		(double)avk.shm_full_uploads);
	cJSON_AddNumberToObject(o, "shm_partial_uploads",
		(double)avk.shm_partial_uploads);
	cJSON_AddNumberToObject(o, "shm_upload_skips",
		(double)avk.shm_upload_skips);
	cJSON_AddNumberToObject(o, "shm_damage_pixels",
		(double)avk.shm_damage_pixels);
	cJSON_AddNumberToObject(o, "shm_committed_pixels",
		(double)avk.shm_committed_pixels);
	/*
	 * ── HOW LONG A CLIENT HELD THE EVENT LOOP ────────────────────────────
	 *
	 * shm_commit_us_max is the headline: the longest a single wl_surface
	 * commit kept the compositor out of poll(). shm_copy_us_max is the same
	 * question asked of the copy alone, wherever it ran, so that moving the
	 * block from one handler to another cannot look like fixing it.
	 *
	 * shm_async_* say whether the copy actually went anywhere: jobs handed to
	 * the worker, and how often a frame arrived before the worker had
	 * finished and had to wait for it. Zero jobs with a nonzero copy time is
	 * the synchronous arm; many jobs with join_waits near zero is the working
	 * state; many jobs with join_waits equal to the job count means the copy
	 * is being started too late to overlap with anything.
	 */
	cJSON_AddBoolToObject(o, "shm_async_upload", az_avk_shm_async_enabled());
	cJSON_AddNumberToObject(o, "shm_commit_us_max",
		(double)(avk.shm_commit_ns_max / 1000));
	cJSON_AddNumberToObject(o, "shm_commit_us_avg",
		(double)(avk.shm_commit_samples
			? avk.shm_commit_ns_sum / avk.shm_commit_samples / 1000 : 0));
	cJSON_AddNumberToObject(o, "shm_commit_samples",
		(double)avk.shm_commit_samples);
	cJSON_AddNumberToObject(o, "shm_commit_over_5ms",
		(double)avk.shm_commit_over_5ms);
	cJSON_AddNumberToObject(o, "shm_copy_us_max",
		(double)(avk.shm_copy_ns_max / 1000));
	cJSON_AddNumberToObject(o, "shm_copy_us_total",
		(double)(avk.shm_copy_ns_sum / 1000));
	cJSON_AddNumberToObject(o, "shm_copy_samples",
		(double)avk.shm_copy_samples);
	cJSON_AddNumberToObject(o, "shm_async_jobs", (double)avk.shm_async_jobs);
	cJSON_AddNumberToObject(o, "shm_async_bytes", (double)avk.shm_async_bytes);
	cJSON_AddNumberToObject(o, "shm_sync_copies", (double)avk.shm_sync_copies);
	cJSON_AddNumberToObject(o, "shm_async_joins", (double)avk.shm_async_joins);
	cJSON_AddNumberToObject(o, "shm_async_join_waits",
		(double)avk.shm_async_join_waits);
	cJSON_AddNumberToObject(o, "shm_async_join_us_max",
		(double)(avk.shm_async_join_ns_max / 1000));
	/* Zero is the working state: the alternating slot has always completed by
	 * the time the host wants it again. Nonzero means uploads are outrunning
	 * the transfer queue and the wait is the only thing between that and the
	 * horizontal banding double buffering was added to stop. */
	cJSON_AddNumberToObject(o, "shm_staging_waits",
		(double)avk.shm_staging_waits);
	cJSON_AddNumberToObject(o, "shm_staging_wait_us_max",
		(double)(avk.shm_staging_wait_ns_max / 1000));
	cJSON_AddNumberToObject(o, "shm_staging_reaped",
		(double)avk.shm_staging_reaped);
	cJSON_AddNumberToObject(o, "shm_staging_returned",
		(double)avk.shm_staging_returned);
	/*
	 * THE COUNTER THAT WAS NOT VISIBLE. The upload ring blocks when its next
	 * slot is still in flight, and it has always counted that -- but only
	 * logged it at AVK_DEBUG, which is filtered, so a grep for the message
	 * found nothing and the mechanism was crossed off the list twice. Nonzero
	 * means uploads are wrapping the ring inside a frame and paying for the
	 * GPU's backlog on the frame thread.
	 */
	cJSON_AddNumberToObject(o, "upload_ring_stalls",
		(double)avk.importer.upload_ring.stalls);
	cJSON_AddNumberToObject(o, "upload_ring_slots",
		(double)avk.importer.upload_ring.slot_count);
	/*
	 * WARM VERSUS FAULTED. A staging buffer costs a page fault per page the
	 * first time it is written, and for a 56MB buffer that is 52ms inside the
	 * copy -- 1.1GB/s where the same memcpy runs at 14GB/s once the pages
	 * exist. `staging_created` is how many allocations paid it;
	 * `staging_reused` is how many were served warm from the device's cache.
	 * A client that presents a fresh wl_buffer every frame drove the first of
	 * these once per frame before that cache existed.
	 */
	if (avk.device != NULL) {
		cJSON_AddNumberToObject(o, "staging_reused",
			(double)avk.device->staging_reused);
		cJSON_AddNumberToObject(o, "staging_reclaimed",
			(double)avk.device->staging_reclaimed);
		cJSON_AddNumberToObject(o, "staging_created",
			(double)avk.device->staging_created);
	}
	/* Explicit-sync participation, so "is this actually running" is a reading
	 * and not a belief. acquire_dropped and release_points_dropped must be 0:
	 * each one is a frame submitted, or a buffer released, without the
	 * ordering the protocol promises. */
	cJSON_AddNumberToObject(o, "acquire_waits", (double)avk.acquire_waits);
	cJSON_AddNumberToObject(o, "acquire_explicit",
		(double)avk.acquire_explicit);
	cJSON_AddNumberToObject(o, "acquire_implicit",
		(double)avk.acquire_implicit);
	cJSON_AddNumberToObject(o, "acquire_dropped", (double)avk.acquire_dropped);
	cJSON_AddNumberToObject(o, "acquire_import_fails",
		(double)avk.acquire_import_fails);
	cJSON_AddNumberToObject(o, "release_points_set",
		(double)avk.release_points_set);
	cJSON_AddNumberToObject(o, "release_points_dropped",
		(double)avk.release_points_dropped);
	cJSON_AddNumberToObject(o, "shm_stale_frames",
		(double)avk.shm_stale_frames);
	cJSON_AddNumberToObject(o, "shm_stale_from_surface",
		(double)avk.shm_stale_from_surface);
	cJSON_AddNumberToObject(o, "shm_stale_multi_output_repaints",
		(double)avk.shm_stale_multi_output_repaints);
	cJSON_AddNumberToObject(o, "shm_stale_no_pool",
		(double)avk.shm_stale_no_pool);
	cJSON_AddNumberToObject(o, "shm_stale_no_sibling",
		(double)avk.shm_stale_no_sibling);
	cJSON_AddNumberToObject(o, "shm_stale_pool_avg",
		(double)(avk.shm_stale_pool_samples
			? avk.shm_stale_pool_size_sum / avk.shm_stale_pool_samples : 0));
	{
		uint64_t packed = 0, stolen = 0;
		avk_upload_worker_counts(avk.upload_worker, &packed, &stolen);
		/* Jobs the posting thread had to pack itself because the worker had
		 * not started them. Not a failure -- it is strictly better than
		 * waiting for a busy worker -- but a rising number means one thread is
		 * no longer enough. */
		cJSON_AddNumberToObject(o, "shm_worker_packed", (double)packed);
		cJSON_AddNumberToObject(o, "shm_worker_stolen", (double)stolen);

		/* The copy's OWN duration, measured on the thread that performs it,
		 * and the rate it implies. This is the number that says whether a long
		 * join was a slow copy or a slow wake-up -- see the comment where the
		 * join wait is recorded. */
		uint64_t p_max = 0, p_sum = 0, p_n = 0, p_bytes = 0;
		avk_upload_worker_timing(avk.upload_worker, &p_max, &p_sum, &p_n,
			&p_bytes);
		cJSON_AddNumberToObject(o, "shm_worker_pack_us_max",
			(double)(p_max / 1000));
		cJSON_AddNumberToObject(o, "shm_worker_pack_us_avg",
			(double)(p_n ? p_sum / p_n / 1000 : 0));
		cJSON_AddNumberToObject(o, "shm_worker_pack_mb_s",
			p_sum ? (double)p_bytes * 1000.0 / (double)p_sum : 0);
	}
	cJSON_AddNumberToObject(o, "buffer_resolve_attempts",
		(double)avk.buffer_resolve_attempts);
	cJSON_AddNumberToObject(o, "nodes_output_culled_before_resolve",
		(double)avk.nodes_output_culled_before_resolve);
	cJSON_AddNumberToObject(o, "blur_nodes_seen", (double)avk.blur_nodes_seen);
	cJSON_AddNumberToObject(o, "blur_nodes_emitted",
		(double)avk.blur_nodes_emitted);
	cJSON_AddNumberToObject(o, "blur_nodes_culled",
		(double)avk.blur_nodes_culled);
	cJSON_AddNumberToObject(o, "nodes_retained_for_halo",
		(double)avk.nodes_retained_for_halo);
	cJSON_AddNumberToObject(o, "blur_halo_px", (double)avk.blur_halo_px);
	cJSON_AddNumberToObject(o, "blur_halo_damage_frames",
		(double)avk.blur_halo_damage_frames);
	cJSON_AddNumberToObject(o, "blur_halo_damage_lost",
		(double)avk.blur_halo_damage_lost);
	cJSON_AddNumberToObject(o, "damage_ring_out_of_bounds",
		(double)avk.damage_ring_out_of_bounds);
	{
		/* Recorded, from the scene side -- see halo_damage_records. Summed over
		 * every monitor, and reported beside the consumed count so the two ends
		 * of the mechanism can be told apart. */
		uint64_t recs = 0;
		Monitor *mm;
		wl_list_for_each(mm, &mons, link) {
			if (mm->scene_output != NULL) {
				recs += mm->scene_output->halo_damage_records;
			}
		}
		cJSON_AddNumberToObject(o, "blur_halo_damage_records", (double)recs);
	}
	cJSON_AddNumberToObject(o, "blur_nodes_forced_live",
		(double)avk.blur_nodes_forced_live);
	/*
	 * M4I. THE CACHE, SUMMED OVER OUTPUTS -- and the work avoided, not just the
	 * hit rate.
	 *
	 * A hit rate is unfalsifiable on its own: a cache serving a hundred cheap
	 * chains reports 100% and has saved nothing. The saved_* figures are in the
	 * same units the uncached path reports its cost in, so the claim "the cache
	 * removed this much work" is a subtraction rather than an inference.
	 */
	{
		uint64_t req = 0, hit = 0, reb = 0, inval = 0, bytes = 0;
		uint64_t hit_k[AVK_BLUR_CACHE_KINDS] = {0};
		uint64_t reb_k[AVK_BLUR_CACHE_KINDS] = {0};
		uint64_t s_draws = 0, s_px = 0, s_chains = 0, s_blur = 0;
		uint64_t i_gen = 0, i_geo = 0, i_par = 0, i_fmt = 0, i_new = 0,
		         i_forced = 0, i_src = 0;
		uint64_t gen_max = 0;
		uint32_t cache_w = 0, cache_h = 0;
		struct avk_blur_cache_inventory inv = {0};
		Monitor *mm;
		wl_list_for_each(mm, &mons, link) {
			if (mm->avk == NULL) {
				continue;
			}
			const struct avk_blur_cache *c = &mm->avk->blur_cache;
			avk_blur_cache_inventory(c, &inv);
			req += c->requests; hit += c->hits; reb += c->rebuilds;
			inval += c->invalidations; bytes += c->bytes;
			s_draws += c->saved_prefix_draws; s_px += c->saved_prefix_px;
			s_chains += c->saved_chains; s_blur += c->saved_blur_px;
			i_gen += c->inv_generation; i_geo += c->inv_geometry;
			i_par += c->inv_params; i_fmt += c->inv_format;
			i_new += c->inv_never_built; i_forced += c->inv_forced;
			i_src += c->inv_source;
			for (int k = 0; k < AVK_BLUR_CACHE_KINDS; k++) {
				hit_k[k] += c->hits_by_kind[k];
				reb_k[k] += c->rebuilds_by_kind[k];
			}
			if (mm->avk->blur_cache_generation > gen_max) {
				gen_max = mm->avk->blur_cache_generation;
			}
			if ((uint64_t)c->width * c->height
					> (uint64_t)cache_w * cache_h) {
				cache_w = c->width;
				cache_h = c->height;
			}
		}
		cJSON_AddNumberToObject(o, "blur_cache_requests", (double)req);
		cJSON_AddNumberToObject(o, "blur_cache_hits", (double)hit);
		cJSON_AddNumberToObject(o, "blur_cache_rebuilds", (double)reb);
		cJSON_AddNumberToObject(o, "blur_cache_invalidations", (double)inval);
		cJSON_AddNumberToObject(o, "blur_cache_bytes", (double)bytes);
		/*
		 * WHAT THE CACHE COSTS, in numbers that can disagree with each other.
		 *
		 * `bytes` alone could not be reconciled with anything: it read
		 * 39,387,136 with no extent, no image count and no format beside it, so
		 * it was not a measurement of anything. These four are separable --
		 * images x extent x format gives texel_bytes by hand, req_bytes is what
		 * VkMemoryRequirements asked for, and the gap between them is the
		 * driver's tiling and metadata rather than a mystery.
		 *
		 * The extent is the LARGEST across outputs, because the outputs differ
		 * (DP-1 3840x2160, HDMI-A-1 1920x1080) and a sum of two extents is not
		 * an extent. The byte figures ARE summed, because bytes do add.
		 */
		cJSON_AddNumberToObject(o, "blur_cache_width", (double)cache_w);
		cJSON_AddNumberToObject(o, "blur_cache_height", (double)cache_h);
		cJSON_AddNumberToObject(o, "blur_cache_images", (double)inv.images);
		cJSON_AddNumberToObject(o, "blur_cache_texel_bytes",
			(double)inv.texel_bytes);
		cJSON_AddNumberToObject(o, "blur_cache_req_bytes",
			(double)inv.req_bytes);
		cJSON_AddNumberToObject(o, "blur_cache_generation", (double)gen_max);
		/*
		 * ── PER OUTPUT, BECAUSE THE MAX ABOVE CAN HIDE A DEAD ONE ─────────
		 *
		 * `blur_cache_generation` is the largest generation across outputs, and
		 * a maximum is exactly the wrong summary for the failure that matters:
		 * one output's cache going stale while another's keeps invalidating
		 * leaves the reported number climbing healthily forever. That is not
		 * hypothetical -- the dirty edge is delivered to l->mon's node by
		 * layer_flush_blur_background(), so a wallpaper surface associated with
		 * the wrong monitor invalidates the wrong cache, and the aggregate says
		 * everything is fine.
		 *
		 * So each output states its own. Cheap, and it is the only view in
		 * which "DP-1 has not invalidated in six hours" is a readable fact.
		 */
		cJSON *per = cJSON_AddArrayToObject(o, "blur_cache_outputs");
		if (per != NULL) {
			wl_list_for_each(mm, &mons, link) {
				if (mm->avk == NULL) {
					continue;
				}
				const struct avk_blur_cache *c = &mm->avk->blur_cache;
				cJSON *e = cJSON_CreateObject();
				if (e == NULL) {
					continue;
				}
				cJSON_AddStringToObject(e, "name",
					mm->wlr_output != NULL ? mm->wlr_output->name : "?");
				cJSON_AddNumberToObject(e, "generation",
					(double)mm->avk->blur_cache_generation);
				cJSON_AddNumberToObject(e, "rebuilds", (double)c->rebuilds);
				cJSON_AddNumberToObject(e, "hits", (double)c->hits);
				cJSON_AddNumberToObject(e, "inv_generation",
					(double)c->inv_generation);
				cJSON_AddNumberToObject(e, "inv_source", (double)c->inv_source);
				/*
				 * ── PER KIND, BECAUSE THE TWO REBUILD INDEPENDENTLY ───────
				 *
				 * The same argument as the per-output split above, one level
				 * down. A kind is only rebuilt when a consumer of that kind was
				 * damaged, so the two run at different rates -- and a kind whose
				 * record has stopped advancing while the output's generation
				 * climbs is a cached picture that has gone stale. There is no
				 * aggregate in which that is visible: this is the reading.
				 */
				for (int k = 0; k < AVK_BLUR_CACHE_KINDS; k++) {
					const struct avk_blur_cache_image *ci = &c->img[k];
					const char *kn = avk_blur_cache_kind_name(
						(enum avk_blur_cache_kind)k);
					char key[48];
					snprintf(key, sizeof(key), "%s_built", kn);
					cJSON_AddBoolToObject(e, key, ci->valid);
					snprintf(key, sizeof(key), "%s_generation", kn);
					cJSON_AddNumberToObject(e, key, (double)ci->generation);
					snprintf(key, sizeof(key), "%s_source_hash", kn);
					/* As a string: a 64-bit digest does not survive a JSON
					 * number, and a silently-rounded hash that compares equal
					 * to a different one is the exact failure this reports. */
					char hx[24];
					snprintf(hx, sizeof(hx), "%016" PRIx64, ci->source_hash);
					cJSON_AddStringToObject(e, key, hx);
				}
				cJSON_AddItemToArray(per, e);
			}
		}
		cJSON_AddNumberToObject(o, "blur_cache_saved_prefix_draws",
			(double)s_draws);
		cJSON_AddNumberToObject(o, "blur_cache_saved_prefix_px", (double)s_px);
		cJSON_AddNumberToObject(o, "blur_cache_saved_chains", (double)s_chains);
		cJSON_AddNumberToObject(o, "blur_cache_saved_blur_px", (double)s_blur);
		cJSON_AddNumberToObject(o, "blur_cache_inv_generation", (double)i_gen);
		/*
		 * NON-ZERO MEANS A BACKGROUND CHANGED WITHOUT SAYING SO.
		 *
		 * Every ordinary change bumps the generation and is counted there, so
		 * this only moves when the source digest disagrees while the
		 * notification did not -- which is the stale-wallpaper defect, caught
		 * rather than rendered. It is a diagnosis, not a health metric: watch
		 * it, do not normalise it.
		 */
		cJSON_AddNumberToObject(o, "blur_cache_inv_source", (double)i_src);
		cJSON_AddNumberToObject(o, "blur_cache_inv_geometry", (double)i_geo);
		cJSON_AddNumberToObject(o, "blur_cache_inv_params", (double)i_par);
		cJSON_AddNumberToObject(o, "blur_cache_inv_format", (double)i_fmt);
		/*
		 * M5/C6. THE PATH-B INTERMEDIATE, PRICED BESIDE THE CACHE IT DOUBLES.
		 *
		 * Reported here rather than under the encode counters because the
		 * question it answers is the same one blur_cache_texel_bytes answers,
		 * and the two must be readable together: on Path B the M4I cache is FP16
		 * as well (ADR-012), so its bytes double AND this is added on top. For a
		 * 3840x2160 output that is 66.4 MB of intermediate beside 132.7 MB of
		 * cache, against 66.4 MB total on Path A. Stating it as one number
		 * somewhere else is how a memory figure becomes unreconcilable.
		 *
		 * Both texel and requirement bytes, for exactly the reason the cache
		 * carries both: one is arithmetic anyone can check, the other is what
		 * the driver asked for, and their difference is tiling rather than a
		 * mystery.
		 */
		{
			uint64_t enc_images = 0, enc_texel = 0, enc_req = 0;
			Monitor *me;
			wl_list_for_each(me, &mons, link) {
				if (me->avk == NULL || me->avk->encode_work.image == NULL) {
					continue;
				}
				const struct avk_image *img = me->avk->encode_work.image;
				enc_images++;
				enc_req += me->avk->encode_work.image_bytes;
				enc_texel += (uint64_t)img->extent.width * img->extent.height
					* avk_format_bytes_per_pixel(img->format);
			}
			cJSON_AddNumberToObject(o, "m5_intermediate_images",
				(double)enc_images);
			cJSON_AddNumberToObject(o, "m5_intermediate_texel_bytes",
				(double)enc_texel);
			cJSON_AddNumberToObject(o, "m5_intermediate_req_bytes",
				(double)enc_req);
		}
		cJSON_AddNumberToObject(o, "blur_cache_inv_never_built", (double)i_new);
		cJSON_AddNumberToObject(o, "blur_cache_inv_forced", (double)i_forced);
		/* PER KIND, because "the shadow backdrops are being served" must be a
		 * reading and not a subtraction. A total that is healthy while the dark
		 * image is never built describes a desktop whose shadows all fell back
		 * to the live path, which is the state this extension exists to end. */
		for (int k = 0; k < AVK_BLUR_CACHE_KINDS; k++) {
			char key[64];
			const char *nm = avk_blur_cache_kind_name(
				(enum avk_blur_cache_kind)k);
			snprintf(key, sizeof(key), "blur_cache_%s_hits", nm);
			cJSON_AddNumberToObject(o, key, (double)hit_k[k]);
			snprintf(key, sizeof(key), "blur_cache_%s_rebuilds", nm);
			cJSON_AddNumberToObject(o, key, (double)reb_k[k]);
		}
	}
	cJSON_AddNumberToObject(o, "blur_nodes_clipped",
		(double)avk.blur_nodes_clipped);
	cJSON_AddNumberToObject(o, "implicit_copy_bytes",
		(double)imp->copied_bytes);
	cJSON_AddNumberToObject(o, "implicit_copy_us", (double)imp->copied_us);

	/* ownership */
	cJSON_AddNumberToObject(o, "commit_imports", (double)avk.commit_imports);
	cJSON_AddNumberToObject(o, "late_imports", (double)avk.late_imports);
	cJSON_AddNumberToObject(o, "client_images_cached",
		(double)avk.client_images_cached);
	cJSON_AddNumberToObject(o, "client_image_cache_hits",
		(double)avk.cache_hits);
	cJSON_AddNumberToObject(o, "client_image_cache_misses",
		(double)avk.cache_misses);

	/* timing */
	cJSON_AddNumberToObject(o, "gpu_submit_us", (double)(submit_ns / 1000));
	cJSON_AddNumberToObject(o, "cpu_frame_us", (double)avk.cpu_frame_us);
	cJSON_AddNumberToObject(o, "cpu_frame_us_max",
		(double)avk.cpu_frame_us_max);
	cJSON_AddNumberToObject(o, "cpu_frame_us_p50",
		az_avk_hist_pct(&avk.cpu_frame_hist, 50.0, AZ_AVK_CPU_BUCKET_US));
	cJSON_AddNumberToObject(o, "cpu_frame_us_p95",
		az_avk_hist_pct(&avk.cpu_frame_hist, 95.0, AZ_AVK_CPU_BUCKET_US));
	cJSON_AddNumberToObject(o, "cpu_frame_us_p99",
		az_avk_hist_pct(&avk.cpu_frame_hist, 99.0, AZ_AVK_CPU_BUCKET_US));
	cJSON_AddNumberToObject(o, "gpu_submit_us_p95",
		az_avk_hist_pct(&avk.gpu_submit_hist, 95.0, 5));
	cJSON_AddNumberToObject(o, "gpu_submit_us_p99",
		az_avk_hist_pct(&avk.gpu_submit_hist, 99.0, 5));
	cJSON_AddNumberToObject(o, "damage_ratio_p50",
		az_avk_hist_pct(&avk.damage_permille_hist, 50.0, 2) / 1000.0);
	cJSON_AddNumberToObject(o, "damage_ratio_p95",
		az_avk_hist_pct(&avk.damage_permille_hist, 95.0, 2) / 1000.0);
	/*
	 * Renderer CPU time: what avk_render_frame() spends RECORDING and
	 * SUBMITTING, per frame, averaged. Named record_ so it cannot be read as
	 * GPU execution -- that is gpu_frame_us_avg further down, and the two are
	 * deliberately separate fields rather than one number that would understate
	 * a shader-bound frame and overstate a submission-bound one.
	 *
	 * This is the number M4E's direct-path comparison is made against: it is
	 * the part of the frame the graph could plausibly have made more expensive.
	 */
	uint64_t record_ns = az_avk_renderer_stat_sum(
		offsetof(struct avk_renderer_stats, cpu_record_ns));
	uint64_t record_frames = az_avk_renderer_stat_sum(
		offsetof(struct avk_renderer_stats, frames));
	if (record_frames > 0) {
		cJSON_AddNumberToObject(o, "record_us_avg",
			(double)record_ns / (double)record_frames / 1000.0);
	} else {
		cJSON_AddNullToObject(o, "record_us_avg");
	}
	/* And its distribution (M4F.2D), summed bucket-wise over renderers so the
	 * percentile is a percentile of the frames rather than an average of
	 * percentiles, which would be a percentile of nothing. */
	{
		struct avk_hist rec = {0};
		for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
			if (!avk.renderers[i].used) {
				continue;
			}
			const struct avk_hist *h =
				&avk.renderers[i].renderer.stats.record_hist;
			rec.count += h->count;
			rec.total += h->total;
			rec.overflow += h->overflow;
			rec.underflow += h->underflow;
			if (h->max > rec.max) {
				rec.max = h->max;
			}
			for (uint32_t b = 0; b < AVK_HIST_BUCKETS; b++) {
				rec.bucket[b] += h->bucket[b];
			}
		}
		cJSON_AddNumberToObject(o, "record_samples", (double)rec.count);
		/* THE MEAN IS NOT QUANTIZED. Percentiles are read out of 20us buckets,
		 * which is coarser than the whole quantity for a CPU record path that
		 * costs ~100us: every percentile difference between two builds comes
		 * out as +/- one bucket, i.e. +/-20us, whatever the real difference
		 * was. The mean is accumulated from raw nanoseconds and is exact, so
		 * it is the only field here that can resolve a small CPU delta. */
		cJSON_AddNumberToObject(o, "record_ns_avg",
			(double)avk_hist_mean(&rec));
		/* And the backpressure wait, which used to be inside record_ns. Two
		 * fields because they answer different questions: how expensive is a
		 * frame to build, and how often is the CPU ahead of the GPU. */
		{
			struct avk_hist rw = {0};
			for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
				if (!avk.renderers[i].used) {
					continue;
				}
				const struct avk_hist *h =
					&avk.renderers[i].renderer.stats.ring_wait_hist;
				rw.count += h->count;
				rw.total += h->total;
				rw.overflow += h->overflow;
				if (h->max > rw.max) { rw.max = h->max; }
				for (uint32_t b = 0; b < AVK_HIST_BUCKETS; b++) {
					rw.bucket[b] += h->bucket[b];
				}
			}
			cJSON_AddNumberToObject(o, "ring_wait_samples", (double)rw.count);
			cJSON_AddNumberToObject(o, "ring_wait_ns_avg",
				(double)avk_hist_mean(&rw));
			/* p50/p99 alongside p95: backpressure is bimodal -- most frames
			 * do not wait at all and the ones that do wait a whole frame --
			 * so a single percentile describes neither mode. */
			cJSON_AddNumberToObject(o, "ring_wait_ns_p50",
				(double)avk_hist_pct(&rw, 50.0));
			cJSON_AddNumberToObject(o, "ring_wait_ns_p95",
				(double)avk_hist_pct(&rw, 95.0));
			cJSON_AddNumberToObject(o, "ring_wait_ns_p99",
				(double)avk_hist_pct(&rw, 99.0));
			cJSON_AddNumberToObject(o, "ring_wait_ns_max", (double)rw.max);
			cJSON_AddNumberToObject(o, "ring_wait_overflow",
				(double)rw.overflow);
			cJSON_AddBoolToObject(o, "ring_wait_censored",
				avk_hist_censored(&rw));
		}
		if (rec.count > 0) {
			cJSON_AddNumberToObject(o, "record_ns_p50",
				(double)avk_hist_pct(&rec, 50.0));
			cJSON_AddNumberToObject(o, "record_ns_p95",
				(double)avk_hist_pct(&rec, 95.0));
			cJSON_AddNumberToObject(o, "record_ns_p99",
				(double)avk_hist_pct(&rec, 99.0));
			cJSON_AddNumberToObject(o, "record_ns_max", (double)rec.max);
			cJSON_AddBoolToObject(o, "record_censored", avk_hist_censored(&rec));
			cJSON_AddNumberToObject(o, "record_overflow", (double)rec.overflow);
			cJSON_AddNumberToObject(o, "record_underflow",
				(double)rec.underflow);
		} else {
			cJSON_AddNullToObject(o, "record_ns_p50");
		}
	}
	/* Superseded by gpu_frame_us_avg (M4D.P), which is a mean over completed
	 * timestamp samples. Kept null rather than removed: it was always null, and
	 * a consumer reading it has never had a number from it. */
	cJSON_AddNullToObject(o, "gpu_frame_us");

	/* damage */
	cJSON_AddNumberToObject(o, "damage_pixels", (double)avk.damage_pixels);
	cJSON_AddNumberToObject(o, "output_pixels", (double)avk.output_pixels);
	if (avk.output_pixels > 0) {
		cJSON_AddNumberToObject(o, "damage_ratio",
			(double)avk.damage_pixels / (double)avk.output_pixels);
	} else {
		cJSON_AddNullToObject(o, "damage_ratio");
	}
	cJSON_AddNumberToObject(o, "full_redraw_frames",
		(double)avk.full_redraw_frames);
	cJSON_AddNumberToObject(o, "partial_redraw_frames",
		(double)avk.partial_redraw_frames);
	cJSON_AddNumberToObject(o, "damage_rects_max",
		(double)avk.damage_rects_max);

	/* synchronisation and presentation */
	cJSON_AddNumberToObject(o, "cpu_sync_waits", (double)sync_waits);
	cJSON_AddNumberToObject(o, "presentation_waits",
		(double)avk.presentation_waits);
	cJSON_AddNumberToObject(o, "present_sync_timeline",
		(double)avk.present_sync_timeline);
	cJSON_AddNumberToObject(o, "present_sync_dmabuf",
		(double)avk.present_sync_dmabuf);
	cJSON_AddNumberToObject(o, "present_sync_none",
		(double)avk.present_sync_none);
	cJSON_AddNumberToObject(o, "present_sync_failures",
		(double)avk.present_sync_fails);
	cJSON_AddNumberToObject(o, "target_state_violations",
		(double)avk.target_state_violations);
	cJSON_AddNumberToObject(o, "output_targets_in_flight",
		(double)avk.output_targets);

	/*
	 * The largest SHM sources, so a pathological client can be identified
	 * instead of guessed at. Deliberately a debug view rather than a stable
	 * API: it is a list of whatever happens to be cached right now, ordered by
	 * bytes moved, and capped so the reply cannot grow without bound.
	 */
	cJSON *sources = cJSON_AddArrayToObject(o, "shm_sources");
	if (sources != NULL) {
		struct az_avk_buffer *entry;
		struct az_avk_buffer *top[AZ_AVK_STAT_SOURCES] = {0};
		size_t top_len = 0;
		wl_list_for_each(entry, &avk.buffers, link) {
			if (!entry->is_shm) {
				continue;
			}
			size_t at = top_len;
			while (at > 0 &&
					top[at - 1]->stat_upload_bytes < entry->stat_upload_bytes) {
				if (at < AZ_AVK_STAT_SOURCES) {
					top[at] = top[at - 1];
				}
				at--;
			}
			if (at < AZ_AVK_STAT_SOURCES) {
				top[at] = entry;
				if (top_len < AZ_AVK_STAT_SOURCES) {
					top_len++;
				}
			}
		}
		for (size_t i = 0; i < top_len; i++) {
			cJSON *src = cJSON_CreateObject();
			if (src == NULL) {
				break;
			}
			char id[32];
			snprintf(id, sizeof(id), "%p", (void *)top[i]->buffer);
			cJSON_AddStringToObject(src, "buffer", id);
			cJSON_AddNumberToObject(src, "width", top[i]->src_width);
			cJSON_AddNumberToObject(src, "height", top[i]->src_height);
			cJSON_AddBoolToObject(src, "from_surface_commit",
				top[i]->taken_at_commit);
			cJSON_AddNumberToObject(src, "generations",
				(double)top[i]->stat_generations);
			cJSON_AddNumberToObject(src, "full_uploads",
				(double)top[i]->stat_full_uploads);
			cJSON_AddNumberToObject(src, "partial_uploads",
				(double)top[i]->stat_partial_uploads);
			cJSON_AddNumberToObject(src, "upload_bytes",
				(double)top[i]->stat_upload_bytes);
			cJSON_AddNumberToObject(src, "frame_lookups",
				(double)top[i]->stat_lookups);
			cJSON_AddItemToArray(sources, src);
		}
	}

	/*
	 * M4H. FRAGMENT AREA BY PRIMITIVE CLASS.
	 *
	 * Every draw counter above answers "did the path run". These answer "how
	 * much of the screen did it run over", which is the only one of the two
	 * that can explain a frame time. `px_output` is the denominator, summed
	 * per composed segment, so px_total / px_output is the overdraw factor.
	 *
	 * px_shadow_env and px_border_outer are the same primitives measured
	 * WITHOUT their interior cut-out, so the pair states how much of the naive
	 * rectangle this renderer avoids -- and would catch a shadow whose
	 * envelope had come loose from its window the way M4G's blur node had.
	 */
#define AZ_AVK_PX(bucket, field) \
	cJSON_AddNumberToObject(o, "px_" #bucket "_" #field, \
		(double)az_avk_renderer_stat_sum(offsetof(struct avk_renderer_stats, \
			px_##bucket) + offsetof(struct avk_prim_px, field)))
#define AZ_AVK_PX_BUCKET(bucket) \
	AZ_AVK_PX(bucket, clear); \
	AZ_AVK_PX(bucket, content); \
	AZ_AVK_PX(bucket, shadow); \
	AZ_AVK_PX(bucket, shadow_env); \
	AZ_AVK_PX(bucket, border); \
	AZ_AVK_PX(bucket, border_outer); \
	AZ_AVK_PX(bucket, blur_comp); \
	AZ_AVK_PX(bucket, rect); \
	AZ_AVK_PX(bucket, gradient); \
	AZ_AVK_PX(bucket, target)
	AZ_AVK_PX_BUCKET(out);
	AZ_AVK_PX_BUCKET(prefix);
#undef AZ_AVK_PX_BUCKET
#undef AZ_AVK_PX
	cJSON_AddNumberToObject(o, "rounded_clip_draws",
		(double)az_avk_renderer_stat_sum(offsetof(struct avk_renderer_stats,
			rounded_clip_draws)));
	cJSON_AddNumberToObject(o, "rounded_asymmetric_draws",
		(double)az_avk_renderer_stat_sum(offsetof(struct avk_renderer_stats,
			rounded_asymmetric_draws)));
	/* M4B. Counters, not pixel accounting: a test asserts that the rounded and
	 * asymmetric border paths were actually TAKEN, so that "no gap" cannot be
	 * scored by a scene that drew no rounded border at all. */
	cJSON_AddNumberToObject(o, "border_draws",
		(double)az_avk_renderer_stat_sum(offsetof(struct avk_renderer_stats,
			border_draws)));
	cJSON_AddNumberToObject(o, "rounded_border_draws",
		(double)az_avk_renderer_stat_sum(offsetof(struct avk_renderer_stats,
			rounded_border_draws)));
	cJSON_AddNumberToObject(o, "asymmetric_border_draws",
		(double)az_avk_renderer_stat_sum(offsetof(struct avk_renderer_stats,
			asymmetric_border_draws)));
	/*
	 * M4D. The same argument as the border counters above, and the trap they
	 * exist to catch is sharper here: asteroidz shows shadows on FLOATING
	 * windows only, so a headless fixture that leaves its client tiled and
	 * then asserts on a shadow is asserting on nothing -- and would score just
	 * as well against a renderer that dropped every shadow node. Every
	 * shadow assertion checks this counter as a premise first.
	 */
	cJSON_AddNumberToObject(o, "shadow_draws",
		(double)az_avk_renderer_stat_sum(offsetof(struct avk_renderer_stats,
			shadow_draws)));
	cJSON_AddNumberToObject(o, "rounded_shadow_draws",
		(double)az_avk_renderer_stat_sum(offsetof(struct avk_renderer_stats,
			rounded_shadow_draws)));
	cJSON_AddNumberToObject(o, "asymmetric_shadow_draws",
		(double)az_avk_renderer_stat_sum(offsetof(struct avk_renderer_stats,
			asymmetric_shadow_draws)));
	/*
	 * M4C. Two groups, and the distinction between them is the point:
	 *
	 *   *_draws / colors_processed   what the frame ASKED FOR
	 *   buffer_*                     what the renderer had to ALLOCATE
	 *
	 * A steady gradient scene is expected to keep uploading -- the data is
	 * written into a per-frame slot -- while allocating nothing at all. Rolling
	 * "uploads" and "allocations" into one number would make a growing buffer
	 * indistinguishable from a busy one, which is the exact question these
	 * exist to answer.
	 */
#define AZ_GRAD_STAT(name) \
	cJSON_AddNumberToObject(o, "gradient_" #name, \
		(double)az_avk_gradient_stat_sum( \
			offsetof(struct avk_gradient_stats, name)))
	AZ_GRAD_STAT(draws);
	AZ_GRAD_STAT(linear_draws);
	AZ_GRAD_STAT(conic_draws);
	AZ_GRAD_STAT(colors_processed);
	AZ_GRAD_STAT(buffer_uploads);
	AZ_GRAD_STAT(buffer_upload_bytes);
	AZ_GRAD_STAT(buffer_reuses);
	AZ_GRAD_STAT(buffer_grows);
#undef AZ_GRAD_STAT

	/*
	 * M4D.P. GPU time, and deliberately reported as a MEAN of completed
	 * samples rather than as a total: a total would be summed over renderers
	 * whose frames overlap in time and would mean nothing.
	 *
	 * `gpu_frame_us_avg` is null, not zero, where the device cannot write
	 * timestamps. A test asserting on 0 there would be asserting that the
	 * frame took no time.
	 */
	uint64_t gpu_ns_total = 0, gpu_samples = 0, gpu_dropped = 0;
	bool gpu_supported = false;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (!avk.renderers[i].used) {
			continue;
		}
		const struct avk_timestamps *ts =
			&avk.renderers[i].renderer.timestamps;
		if (!ts->supported) {
			continue;
		}
		gpu_supported = true;
		gpu_ns_total += ts->gpu_frame_ns_total;
		gpu_samples += ts->samples;
		gpu_dropped += ts->dropped;
	}
	cJSON_AddBoolToObject(o, "gpu_timestamps", gpu_supported);
	if (gpu_supported && gpu_samples > 0) {
		cJSON_AddNumberToObject(o, "gpu_frame_us_avg",
			(double)gpu_ns_total / (double)gpu_samples / 1000.0);
	} else {
		cJSON_AddNullToObject(o, "gpu_frame_us_avg");
	}
	cJSON_AddNumberToObject(o, "gpu_samples", (double)gpu_samples);
	cJSON_AddNumberToObject(o, "gpu_dropped", (double)gpu_dropped);

	/*
	 * ── GPU DISTRIBUTIONS, AND WHAT EACH SPAN ACTUALLY CONTAINS (M4F.2D) ──
	 *
	 * A mean is the wrong statistic for a deadline, so every one of these
	 * carries p50/p95/p99 from a 20us-bucket histogram, quoted as the UPPER
	 * EDGE of the bucket the percentile falls in -- an over-estimate by at
	 * most one bucket, stated rather than hidden.
	 *
	 *   gpu_frame        FRAME_BEGIN -> FRAME_END: the whole frame on the GPU.
	 *   gpu_blur_total   BLUR_BEGIN -> BLUR_END: every prefix replay and every
	 *                    down/up chain. EXCLUDES the composite draws.
	 *   gpu_blur_prefix  the FIRST chain's prefix replay.
	 *   gpu_blur_down    the FIRST chain's downsample passes.
	 *   gpu_blur_up      the upsample passes, sampled ONLY on frames with
	 *                    exactly one chain, where that span is nothing else.
	 *
	 * gpu_blur_composite_ns DOES NOT EXIST and is not reported as zero. A
	 * blur's result is drawn by one call sitting at that blur's index inside
	 * the output segment, interleaved with ordinary content; a timestamp pair
	 * around it would measure the span of the segment that happens to contain
	 * it. Its cost is obtained by differencing two frames that differ only in
	 * whether the composite runs. See avk_timestamp.h.
	 */
	{
		struct avk_hist frame = {0}, total = {0}, prefix = {0}, down = {0},
			up = {0}, up0 = {0}, frameblur = {0}, pre = {0}, post = {0};
		for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
			if (!avk.renderers[i].used) {
				continue;
			}
			const struct avk_timestamps *ts =
				&avk.renderers[i].renderer.timestamps;
			if (!ts->supported) {
				continue;
			}
			/* Bucket-wise addition: two renderers' histograms are over the
			 * same buckets, so this is exact rather than an average of
			 * percentiles, which would not be a percentile of anything. */
			const struct avk_hist *src[9] = { &ts->gpu_frame_hist,
				&ts->blur_total_hist, &ts->blur_prefix_hist,
				&ts->blur_down_hist, &ts->blur_up_hist, &ts->blur_up0_hist,
				&ts->gpu_frame_blur_hist, &ts->blur_pre_hist,
				&ts->blur_post_hist };
			struct avk_hist *dst[9] = { &frame, &total, &prefix, &down, &up,
				&up0, &frameblur, &pre, &post };
			for (int k = 0; k < 9; k++) {
				dst[k]->count += src[k]->count;
				dst[k]->total += src[k]->total;
				dst[k]->overflow += src[k]->overflow;
				dst[k]->underflow += src[k]->underflow;
				if (src[k]->max > dst[k]->max) {
					dst[k]->max = src[k]->max;
				}
				for (uint32_t b = 0; b < AVK_HIST_BUCKETS; b++) {
					dst[k]->bucket[b] += src[k]->bucket[b];
				}
			}
		}
		/* gpu_frame is over ALL frames; gpu_frame_blur is the same samples
		 * restricted to frames that ran blur. On a sparse workload those are
		 * very different populations and only the second is comparable with
		 * gpu_blur_total. */
		/* WHAT THE CLASSIFIER DECIDED, counted at frame-build time. The
		 * histogram counts are what SURVIVED to readback; these are what was
		 * classified. A cohort test compares the two, and any gap is either a
		 * dropped result or a classification that did not travel with its
		 * frame. */
		{
			uint64_t cb = 0, ci = 0, cf = 0;
			bool wrong = false;
			for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
				if (!avk.renderers[i].used) {
					continue;
				}
				const struct avk_timestamps *t =
					&avk.renderers[i].renderer.timestamps;
				cb += t->cohort_blur_frames;
				ci += t->cohort_idle_frames;
				cf += t->frames_built;
				wrong = wrong || t->cohort_wrong;
			}
			cJSON_AddNumberToObject(o, "cohort_blur_frames", (double)cb);
			cJSON_AddNumberToObject(o, "cohort_idle_frames", (double)ci);
			cJSON_AddNumberToObject(o, "cohort_frames_built", (double)cf);
			cJSON_AddBoolToObject(o, "cohort_classifier_broken", wrong);
			{
				uint64_t st = 0;
				for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
					if (avk.renderers[i].used) {
						st += avk.renderers[i].renderer.timestamps.straddled;
					}
				}
				/* Results that arrived after the window they belong to ended.
				 * Expected to be small and non-zero -- it is the pipeline
				 * depth at the moment of the reset, not a fault. */
				cJSON_AddNumberToObject(o, "gpu_results_straddled", (double)st);
			}
		}
		static const char *names[9] = { "gpu_frame", "gpu_blur_total",
			"gpu_blur_prefix", "gpu_blur_down", "gpu_blur_up",
			"gpu_blur_up0", "gpu_frame_blur", "gpu_frame_preblur",
			"gpu_frame_postblur" };
		const struct avk_hist *hs[9] = { &frame, &total, &prefix, &down, &up,
			&up0, &frameblur, &pre, &post };
		for (int k = 0; k < 9; k++) {
			char key[64];
			snprintf(key, sizeof(key), "%s_samples", names[k]);
			cJSON_AddNumberToObject(o, key, (double)hs[k]->count);
			/* ALWAYS EMITTED, even with no samples. A consumer that checks
			 * "overflow == 0" must be able to tell "no overflow" from "the
			 * key is missing": a missing key reads as non-zero in every shell
			 * comparison, and a benchmark row was rejected for blur_OVERFLOW
			 * when what had actually happened was that no blur span was
			 * collected at all. Absence and saturation are different facts. */
			snprintf(key, sizeof(key), "%s_censored", names[k]);
			cJSON_AddBoolToObject(o, key, avk_hist_censored(hs[k]));
			snprintf(key, sizeof(key), "%s_overflow", names[k]);
			cJSON_AddNumberToObject(o, key, (double)hs[k]->overflow);
			snprintf(key, sizeof(key), "%s_underflow", names[k]);
			cJSON_AddNumberToObject(o, key, (double)hs[k]->underflow);
			if (hs[k]->count == 0) {
				/* Null, not zero: zero would assert that the work took no
				 * time, which is a different claim from "not measured". */
				snprintf(key, sizeof(key), "%s_ns_avg", names[k]);
				cJSON_AddNullToObject(o, key);
				continue;
			}
			snprintf(key, sizeof(key), "%s_ns_avg", names[k]);
			cJSON_AddNumberToObject(o, key, (double)avk_hist_mean(hs[k]));
			snprintf(key, sizeof(key), "%s_ns_p50", names[k]);
			cJSON_AddNumberToObject(o, key, (double)avk_hist_pct(hs[k], 50.0));
			snprintf(key, sizeof(key), "%s_ns_p95", names[k]);
			cJSON_AddNumberToObject(o, key, (double)avk_hist_pct(hs[k], 95.0));
			snprintf(key, sizeof(key), "%s_ns_p99", names[k]);
			cJSON_AddNumberToObject(o, key, (double)avk_hist_pct(hs[k], 99.0));
			snprintf(key, sizeof(key), "%s_ns_max", names[k]);
			cJSON_AddNumberToObject(o, key, (double)hs[k]->max);
		}
		cJSON_AddNullToObject(o, "gpu_blur_composite_ns");
	}

	/*
	 * M4E. The graph, as of the LAST frame each renderer built -- passes,
	 * resources, uses and barriers are per-frame counters, reset by
	 * avk_graph_reset(), so summing them over renderers describes the most
	 * recent frame of each and not a run.
	 *
	 * `graph_allocs` is the exception and the important one: it is cumulative
	 * and must STOP MOVING once the desktop settles. A number that keeps rising
	 * on a static scene means graph construction is allocating every frame,
	 * which is precisely what the flat-array design exists to prevent. Reading
	 * it twice a few seconds apart is the whole test.
	 */
	uint32_t g_passes = 0, g_resources = 0, g_uses = 0, g_barriers = 0;
	uint32_t g_transitions = 0, g_buffer_barriers = 0;
	uint64_t g_allocs = 0, g_build_ns = 0, g_frames = 0;
	uint32_t g_hist[64] = {0};
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (!avk.renderers[i].used) {
			continue;
		}
		const struct avk_graph_stats *gs =
			&avk.renderers[i].renderer.graph.stats;
		g_passes += gs->passes;
		g_resources += gs->resources;
		g_uses += gs->uses;
		g_barriers += gs->barriers;
		g_transitions += gs->image_transitions;
		g_buffer_barriers += gs->buffer_barriers;
		g_allocs += gs->allocs;
		g_build_ns += gs->build_ns;
		for (int h = 0; h < 64; h++) {
			g_hist[h] += gs->build_hist[h];
		}
		g_frames += gs->frames;
	}
	cJSON_AddNumberToObject(o, "graph_passes", (double)g_passes);
	cJSON_AddNumberToObject(o, "graph_resources", (double)g_resources);
	cJSON_AddNumberToObject(o, "graph_uses", (double)g_uses);
	cJSON_AddNumberToObject(o, "graph_barriers", (double)g_barriers);
	cJSON_AddNumberToObject(o, "graph_image_transitions",
		(double)g_transitions);
	/* Zero by construction -- see struct avk_graph_stats. Reported so the
	 * absence is a stated fact rather than an omission. */
	cJSON_AddNumberToObject(o, "graph_buffer_barriers",
		(double)g_buffer_barriers);
	cJSON_AddNumberToObject(o, "graph_allocs", (double)g_allocs);

	/*
	 * M4E.2. Zero across the board until M4F acquires from the pool; reported
	 * now so that when it stops being zero there is a before to compare with.
	 * `transient_creates` is the one to watch during a resize: it must stop
	 * rising once the sizes settle.
	 *
	 * TWO DIFFERENT TIME BASES, DELIBERATELY.
	 *
	 * acquires/reuses/retires are per-frame events and are cleared by
	 * az_avk_stats_reset(), so they describe the measurement window.
	 * creates/live/bytes/peak describe the POOL, which outlives any window --
	 * an allocation made before the reset is still allocated after it, and
	 * zeroing that would claim the compositor had allocated nothing.
	 *
	 * The distinction is not cosmetic. Comparing lifetime acquires between two
	 * separately launched compositors compares how many frames each happened
	 * to render before the window opened, not what the change under test did:
	 * an A/B asserting acquire equality read 264 against 256 for eight extra
	 * warm-up frames, on runs whose per-frame graph uses were identical.
	 */
	uint64_t t_acquires = 0, t_reuses = 0, t_creates = 0, t_retires = 0;
	uint64_t t_no_key = 0, t_in_use = 0, t_in_flight = 0;
	uint64_t t_unsafe = 0, t_bytes = 0, t_peak = 0;
	uint32_t t_live = 0;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (!avk.renderers[i].used) {
			continue;
		}
		const struct avk_transient_stats *ts =
			&avk.renderers[i].renderer.transients.stats;
		t_acquires += ts->acquires;
		t_reuses += ts->reuses;
		t_creates += ts->creates;
		t_no_key += ts->miss_no_key;
		t_in_use += ts->miss_in_use;
		t_in_flight += ts->miss_in_flight;
		t_retires += ts->retires;
		t_unsafe += ts->unsafe_reuses;
		t_bytes += ts->bytes;
		t_peak += ts->peak_bytes;
		t_live += ts->live;
	}
	cJSON_AddNumberToObject(o, "transient_acquires", (double)t_acquires);
	cJSON_AddNumberToObject(o, "transient_reuses", (double)t_reuses);
	cJSON_AddNumberToObject(o, "transient_creates", (double)t_creates);
	cJSON_AddNumberToObject(o, "transient_miss_no_key", (double)t_no_key);
	cJSON_AddNumberToObject(o, "transient_miss_in_use", (double)t_in_use);
	cJSON_AddNumberToObject(o, "transient_miss_in_flight", (double)t_in_flight);
	cJSON_AddNumberToObject(o, "transient_retires", (double)t_retires);
	/* MUST stay 0. The only path that can raise it is the M4E.4 break. */
	cJSON_AddNumberToObject(o, "transient_unsafe_reuses", (double)t_unsafe);
	cJSON_AddNumberToObject(o, "transient_live", (double)t_live);
	cJSON_AddNumberToObject(o, "transient_bytes", (double)t_bytes);
	cJSON_AddNumberToObject(o, "transient_peak_bytes", (double)t_peak);

	/*
	 * M4F. What the blur actually cost, RECORDED AND NOT YET OPTIMISED.
	 *
	 * `blur_prefix_commands` and `blur_prefix_pixels` are quadratic in the
	 * number of blur nodes by construction -- a blur at command index k replays
	 * k commands -- and that is exactly the number a cache would be bought with.
	 * It is measured before any decision about caching is taken (M4F.2D decides
	 * what M4F.2E has to optimise), so the decision is made against a
	 * measurement rather than against an expectation.
	 */
	uint64_t b_replays = 0, b_cmds = 0, b_px = 0;
	uint64_t b_chains = 0, b_passes = 0, b_transients = 0, b_skipped = 0;
	uint64_t b_draws = 0, b_soft = 0, b_darken = 0;
	/* M4F.2B. The six regions, as areas, plus what the two sweeps cost. */
	uint64_t d_src = 0, d_out = 0, d_rebuild = 0, d_copyable = 0;
	uint64_t f_rects = 0, f_before = 0, f_after = 0;
	uint64_t d_dep_full = 0, d_write_full = 0, d_cap_full = 0, d_saved = 0;
	uint64_t d_touched = 0, d_skipped = 0, d_fallbacks = 0, d_rects = 0;
	uint64_t d_inherited = 0, d_build_ns = 0;
	uint64_t d_halo = 0, d_cap_px = 0, d_res_px = 0, d_proc_px = 0;
	uint64_t role_chains[AVK_BLUR_ROLE_COUNT] = {0};
	uint64_t role_cap[AVK_BLUR_ROLE_COUNT] = {0};
	uint64_t role_rebuild[AVK_BLUR_ROLE_COUNT] = {0};
	uint64_t role_result[AVK_BLUR_ROLE_COUNT] = {0};
	uint64_t role_cmds[AVK_BLUR_ROLE_COUNT] = {0};
	uint64_t d_up0_px = 0;
	uint64_t d_req_px = 0;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (!avk.renderers[i].used) {
			continue;
		}
		const struct avk_renderer *r = &avk.renderers[i].renderer;
		b_replays += r->blur_prefix_replays;
		b_cmds += r->blur_prefix_commands;
		b_px += r->blur_prefix_pixels;
		b_chains += r->blur_stats.chains;
		b_passes += r->blur_stats.passes;
		b_transients += r->blur_stats.transients;
		b_skipped += r->blur_stats.skipped;
		b_draws += r->stats.blur_draws;
		b_soft += r->stats.blur_soft_draws;
		b_darken += r->stats.blur_darken_passes;
		d_src += r->blur_source_damage_pixels;
		d_out += r->blur_output_damage_pixels;
		d_rebuild += r->blur_prefix_rebuild_pixels;
		d_copyable += r->blur_prefix_copyable_pixels;
		f_rects += r->blur_fallback_rects;
		f_before += r->blur_fallback_area_before;
		f_after += r->blur_fallback_area_after;
		d_dep_full += r->blur_full_dependency_pixels;
		d_write_full += r->blur_full_write_pixels;
		d_cap_full += r->blur_full_capture_pixels;
		d_saved += r->blur_damage_saved_pixels;
		d_req_px += r->blur_required_work_pixels;
		d_touched += r->blur_damage_nodes_touched;
		d_skipped += r->blur_damage_nodes_skipped;
		d_fallbacks += r->blur_damage_fallbacks;
		d_inherited += r->blur_transitive_damage_pixels;
		d_build_ns += r->blur_damage_build_ns;
		d_halo += r->blur_halo_pixels;
		d_cap_px += r->blur_capture_pixels;
		d_res_px += r->blur_result_pixels;
		d_proc_px += r->blur_stats.processed_pixels;
		d_up0_px += r->blur_stats.up0_pixels;
		if (r->blur_damage_rects_max > d_rects) {
			d_rects = r->blur_damage_rects_max;
		}
		for (int k = 0; k < AVK_BLUR_ROLE_COUNT; k++) {
			role_chains[k] += r->blur_role_chains[k];
			role_cap[k] += r->blur_role_capture_px[k];
			role_rebuild[k] += r->blur_role_rebuild_px[k];
			role_result[k] += r->blur_role_result_px[k];
			role_cmds[k] += r->blur_role_prefix_cmds[k];
		}
	}
	/*
	 * M4I. THE SAME NUMBERS, PER ROLE, so "how much of this is the background
	 * blur being recomputed per window" is a reading rather than an inference.
	 *
	 * WINDOW_BACKDROP is the set that DECLARED its source to be the monitor
	 * background and got a private live chain anyway. Its share of the capture
	 * and rebuild totals is exactly the redundancy a monitor-wide result cache
	 * would remove, and it is stated here so the claim can be checked before any
	 * cache exists and again after.
	 */
	for (int k = 0; k < AVK_BLUR_ROLE_COUNT; k++) {
		char key[64];
		const char *nm = avk_blur_role_name((enum avk_blur_role)k);
		snprintf(key, sizeof(key), "blur_role_%s_chains", nm);
		cJSON_AddNumberToObject(o, key, (double)role_chains[k]);
		snprintf(key, sizeof(key), "blur_role_%s_capture_px", nm);
		cJSON_AddNumberToObject(o, key, (double)role_cap[k]);
		snprintf(key, sizeof(key), "blur_role_%s_rebuild_px", nm);
		cJSON_AddNumberToObject(o, key, (double)role_rebuild[k]);
		snprintf(key, sizeof(key), "blur_role_%s_result_px", nm);
		cJSON_AddNumberToObject(o, key, (double)role_result[k]);
		snprintf(key, sizeof(key), "blur_role_%s_prefix_cmds", nm);
		cJSON_AddNumberToObject(o, key, (double)role_cmds[k]);
	}
	cJSON_AddNumberToObject(o, "blur_prefix_replays", (double)b_replays);
	cJSON_AddNumberToObject(o, "blur_prefix_commands", (double)b_cmds);
	cJSON_AddNumberToObject(o, "blur_prefix_pixels", (double)b_px);
	cJSON_AddNumberToObject(o, "blur_chains", (double)b_chains);
	cJSON_AddNumberToObject(o, "blur_passes", (double)b_passes);
	cJSON_AddNumberToObject(o, "blur_transients", (double)b_transients);
	cJSON_AddNumberToObject(o, "blur_skipped", (double)b_skipped);
	/* At the DRAW, so these say what the material did rather than what the
	 * scene asked for -- two different claims, and only one of them is about
	 * the renderer. */
	cJSON_AddNumberToObject(o, "blur_draws", (double)b_draws);
	cJSON_AddNumberToObject(o, "blur_soft_edge_draws", (double)b_soft);
	cJSON_AddNumberToObject(o, "blur_darken_chains", (double)b_darken);
	/*
	 * M4F.2B. Each area is reported BESIDE the full-recompute area it replaces,
	 * so a ratio is available without a second instrument and "we saved
	 * nothing" is a number rather than the absence of one.
	 *
	 * blur_damage_nodes_touched + blur_damage_nodes_skipped is the number of
	 * blur commands the renderer considered -- so a fixture can tell "the blur
	 * did no work" from "there was no blur", which are different claims and
	 * only one of them is about damage.
	 */
	cJSON_AddNumberToObject(o, "blur_source_damage_pixels", (double)d_src);
	cJSON_AddNumberToObject(o, "blur_output_damage_pixels", (double)d_out);
	cJSON_AddNumberToObject(o, "blur_prefix_rebuild_pixels", (double)d_rebuild);
	cJSON_AddNumberToObject(o, "blur_prefix_copyable_pixels", (double)d_copyable);
	/*
	 * M4H.7. Frames that missed their output's deadline, and by how many
	 * refreshes. This is the closure metric: a percentile cannot say whether a
	 * frame missed, and gpu_frame_blur p95 moved from 2880 to 4860us across two
	 * identical live cohorts while p50 and p99 did not move at all. A count
	 * does not depend on how many animations happened to be in the sample.
	 */
	uint64_t ob_frames = 0, ob = 0, ob2 = 0, ob3 = 0;
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (!avk.renderers[i].used) {
			continue;
		}
		const struct avk_timestamps *t = &avk.renderers[i].renderer.timestamps;
		ob_frames += t->budget_frames;
		ob += t->over_budget;
		ob2 += t->over_budget_2x;
		ob3 += t->over_budget_3x;
	}
	cJSON_AddNumberToObject(o, "budget_frames", (double)ob_frames);
	cJSON_AddNumberToObject(o, "over_budget", (double)ob);
	cJSON_AddNumberToObject(o, "commit_us_max", (double)(avk.commit_ns_max / 1000));
	cJSON_AddNumberToObject(o, "commit_us_avg", (double)(avk.commit_samples
		? avk.commit_ns_sum / avk.commit_samples / 1000 : 0));
	cJSON_AddNumberToObject(o, "handler_us_max", (double)(avk.handler_ns_max / 1000));
	cJSON_AddNumberToObject(o, "handler_us_avg", (double)(avk.commit_samples
		? avk.handler_ns_sum / avk.commit_samples / 1000 : 0));
	cJSON_AddNumberToObject(o, "handler_over_10ms", (double)avk.handler_over_10ms);
	cJSON_AddNumberToObject(o, "handler_over_30ms", (double)avk.handler_over_30ms);
	cJSON_AddNumberToObject(o, "over_budget_2x", (double)ob2);
	cJSON_AddNumberToObject(o, "over_budget_3x", (double)ob3);
	cJSON_AddNumberToObject(o, "blur_full_dependency_pixels",
		(double)d_dep_full);
	cJSON_AddNumberToObject(o, "blur_full_write_pixels", (double)d_write_full);
	cJSON_AddNumberToObject(o, "blur_full_capture_pixels", (double)d_cap_full);
	cJSON_AddNumberToObject(o, "blur_damage_saved_pixels", (double)d_saved);
	cJSON_AddNumberToObject(o, "blur_damage_nodes_touched", (double)d_touched);
	cJSON_AddNumberToObject(o, "blur_damage_nodes_skipped", (double)d_skipped);
	cJSON_AddNumberToObject(o, "blur_damage_fallbacks", (double)d_fallbacks);
	/* And what those fallbacks cost. area_after/area_before is the fill the
	 * bounding-box collapse invented; rects is how fragmented the region was
	 * when it gave up. A fallback count on its own cannot distinguish a
	 * collapse that added 10% from one that added 20x. */
	cJSON_AddNumberToObject(o, "blur_fallback_rects", (double)f_rects);

	cJSON_AddNumberToObject(o, "blur_fallback_area_before", (double)f_before);
	cJSON_AddNumberToObject(o, "blur_fallback_area_after", (double)f_after);
	cJSON_AddNumberToObject(o, "blur_damage_rects_max", (double)d_rects);
	cJSON_AddNumberToObject(o, "blur_transitive_damage_pixels",
		(double)d_inherited);
	cJSON_AddNumberToObject(o, "blur_damage_build_ns", (double)d_build_ns);
	/* M4F.2C/.2D. blur_processed_pixels against blur_result_pixels is the size
	 * of the prize a per-level scissor would win; it is recorded now and
	 * decided on in M4F.2D. */
	cJSON_AddNumberToObject(o, "blur_halo_pixels", (double)d_halo);
	cJSON_AddNumberToObject(o, "blur_capture_pixels", (double)d_cap_px);
	cJSON_AddNumberToObject(o, "blur_result_pixels", (double)d_res_px);
	cJSON_AddNumberToObject(o, "blur_processed_pixels", (double)d_proc_px);
	/* MEASURED, not inferred. The up0 reduction was previously only available
	 * by differencing whole-chain totals, which mixes in four passes the
	 * prototype never touches. */
	cJSON_AddNumberToObject(o, "blur_up0_pixels", (double)d_up0_px);
	/* AND WHAT IT WOULD HAVE HAD TO PROCESS. The pair is the per-level scissor
	 * question stated as two numbers over the same frames; see
	 * blur_required_work_pixels. processed/required is a PIXEL-WORK upper
	 * bound on what regional execution could remove, not a GPU speedup. */
	cJSON_AddNumberToObject(o, "blur_required_work_pixels", (double)d_req_px);
	/*
	 * WHAT EACH CANDIDATE STRATEGY WOULD REMOVE, in pixel work, from the same
	 * derivation and the same frames. The question M4F.2D asks is not "how
	 * much could regional execution save" but "how much of that does the
	 * SIMPLEST implementation capture", and these are the numerators of that.
	 */
	{
		uint64_t up0 = 0, up01 = 0, upc = 0;
		struct avk_hist rb = {0};
		for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
			if (!avk.renderers[i].used) {
				continue;
			}
			const struct avk_renderer *r = &avk.renderers[i].renderer;
			up0 += r->blur_removable_up0_pixels;
			up01 += r->blur_removable_up01_pixels;
			upc += r->blur_removable_up_pixels;
			const struct avk_hist *h = &r->blur_region_build_hist;
			rb.count += h->count;
			rb.total += h->total;
			rb.overflow += h->overflow;
			rb.underflow += h->underflow;
			if (h->max > rb.max) { rb.max = h->max; }
			for (uint32_t b = 0; b < AVK_HIST_BUCKETS; b++) {
				rb.bucket[b] += h->bucket[b];
			}
		}
		{
			/* THE FRAME TOPOLOGY, so a reader can tell whether the up-phase
			 * and final-upsample timings were attributable at all: those
			 * spans are only meaningful on a frame with exactly one chain. */
			uint64_t maxslots = 0;
			for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
				if (avk.renderers[i].used
						&& avk.renderers[i].renderer.blur_max_slots > maxslots) {
					maxslots = avk.renderers[i].renderer.blur_max_slots;
				}
			}
			cJSON_AddNumberToObject(o, "blur_max_chains_per_frame",
				(double)maxslots);
		}
		cJSON_AddNumberToObject(o, "blur_removable_up0_pixels", (double)up0);
		cJSON_AddNumberToObject(o, "blur_removable_up01_pixels", (double)up01);
		cJSON_AddNumberToObject(o, "blur_removable_up_pixels", (double)upc);
		/* All levels: processed minus required, the full-regionalisation
		 * upper bound, printed here so a reader does not have to subtract. */
		cJSON_AddNumberToObject(o, "blur_removable_all_pixels",
			(double)(d_proc_px > d_req_px ? d_proc_px - d_req_px : 0));
		cJSON_AddNumberToObject(o, "blur_region_build_samples",
			(double)rb.count);
		if (rb.count > 0) {
			cJSON_AddNumberToObject(o, "blur_region_build_ns_avg",
				(double)avk_hist_mean(&rb));
			cJSON_AddNumberToObject(o, "blur_region_build_ns_p50",
				(double)avk_hist_pct(&rb, 50.0));
			cJSON_AddNumberToObject(o, "blur_region_build_ns_p95",
				(double)avk_hist_pct(&rb, 95.0));
			cJSON_AddNumberToObject(o, "blur_region_build_ns_p99",
				(double)avk_hist_pct(&rb, 99.0));
			cJSON_AddNumberToObject(o, "blur_region_build_ns_max",
				(double)rb.max);
			cJSON_AddNumberToObject(o, "blur_region_build_underflow",
				(double)rb.underflow);
		} else {
			cJSON_AddNullToObject(o, "blur_region_build_ns_p50");
		}
	}
	cJSON_AddNumberToObject(o, "graph_frames", (double)g_frames);
	if (g_frames > 0) {
		cJSON_AddNumberToObject(o, "graph_build_ns_avg",
			(double)g_build_ns / (double)g_frames);
		/*
		 * PERCENTILES, because the mean above is captured by preemption. A
		 * frame's graph work is a few microseconds; a scheduler slice landing
		 * inside it is charged to it, and on a loaded desktop the mean ends up
		 * describing the scheduler. p50 is what the graph costs.
		 */
		uint64_t seen = 0, want50 = g_frames / 2, want95 = g_frames * 95 / 100;
		uint64_t want99 = g_frames * 99 / 100;
		double p50 = 0, p95 = 0, p99 = 0;
		for (int h = 0; h < 64; h++) {
			uint64_t before = seen;
			seen += g_hist[h];
			double edge = (double)(h + 1) * 250.0;
			if (before <= want50 && seen > want50) { p50 = edge; }
			if (before <= want95 && seen > want95) { p95 = edge; }
			if (before <= want99 && seen > want99) { p99 = edge; }
		}
		cJSON_AddNumberToObject(o, "graph_build_ns_p50", p50);
		cJSON_AddNumberToObject(o, "graph_build_ns_p95", p95);
		cJSON_AddNumberToObject(o, "graph_build_ns_p99", p99);
	} else {
		cJSON_AddNullToObject(o, "graph_build_ns_avg");
		cJSON_AddNullToObject(o, "graph_build_ns_p50");
		cJSON_AddNullToObject(o, "graph_build_ns_p95");
		cJSON_AddNullToObject(o, "graph_build_ns_p99");
	}
	cJSON_AddNumberToObject(o, "software_cursor_frames",
		(double)avk.software_cursor_frames);
	cJSON_AddNumberToObject(o, "cursor_geometry_mismatch",
		(double)avk.cursor_geometry_mismatch);
	cJSON_AddNumberToObject(o, "hardware_cursor_frames",
		(double)avk.hardware_cursor_frames);
	cJSON_AddNumberToObject(o, "cursor_commands",
		(double)avk.cursor_commands);
	cJSON_AddNumberToObject(o, "cursor_no_image",
		(double)avk.cursor_no_image);
	cJSON_AddNumberToObject(o, "cursor_import_failures",
		(double)avk.cursor_import_failures);
	cJSON_AddNumberToObject(o, "cursor_culled", (double)avk.cursor_culled);
	cJSON_AddNumberToObject(o, "cursor_moves", (double)avk.cursor_moves);
	cJSON_AddNumberToObject(o, "cursor_damage_pixels",
		(double)avk.cursor_damage_pixels);
	cJSON_AddNumberToObject(o, "cursor_hw_to_sw", (double)avk.cursor_hw_to_sw);
	cJSON_AddNumberToObject(o, "cursor_sw_to_hw", (double)avk.cursor_sw_to_hw);
	cJSON_AddNumberToObject(o, "cursor_client_surface_sets",
		(double)az_cursor.sets_client);
	cJSON_AddNumberToObject(o, "cursor_shape_sets",
		(double)az_cursor.sets_shape);
	cJSON_AddNumberToObject(o, "cursor_xcursor_sets",
		(double)az_cursor.sets_xcursor);
	cJSON_AddNumberToObject(o, "cursor_unsets", (double)az_cursor.unsets);
	cJSON_AddNumberToObject(o, "cursor_forced_reimports",
		(double)az_cursor.forced_reimports);
	/*
	 * Counted since M3.5E stage 1 and never reported, which made it useless
	 * for the one question it answers: a client asked for a cursor surface
	 * and the surface had nothing to show, so the pointer was hidden rather
	 * than given the image the client meant. That is indistinguishable on
	 * screen from "the cursor never changes shape" -- the compositor's own
	 * default is put back on the next motion, so the pointer simply stays an
	 * arrow. Worth a number, because reading the code cannot tell you whether
	 * it is happening on a particular machine with a particular client.
	 */
	cJSON_AddNumberToObject(o, "cursor_client_no_buffer",
		(double)az_cursor.client_no_buffer);
	cJSON_AddNumberToObject(o, "cursor_stale_xcursor",
		(double)az_cursor.stale_xcursor);
	cJSON_AddNumberToObject(o, "cursor_mgr_generation",
		(double)az_cursor_mgr_generation);
	/*
	 * Ownership accounting. `lifecycle_violations` is the headline: every kind
	 * of double-owned or double-destroyed AVK resource increments it, and it
	 * is the counter a test asserts on, because the alternative is asserting
	 * on the absence of a glibc abort -- which is also what a code path that
	 * never ran produces.
	 *
	 * The live_* counts are a running census, not just a shutdown one. Read
	 * mid-session they say what AVK owns right now; read after teardown they
	 * would all be zero, but nothing can read them then, which is why
	 * avk_device_log_live_objects() also writes them to the log immediately
	 * before vkDestroyDevice.
	 */
	if (avk.device != NULL) {
		cJSON_AddNumberToObject(o, "lifecycle_violations",
			(double)avk.device->lifecycle_violations);
		cJSON_AddNumberToObject(o, "retire_duplicate_pushes",
			(double)(avk.importer.retire.duplicate_pushes));
		cJSON_AddNumberToObject(o, "retire_entries_live",
			(double)avk.device->live.retire_entries);
		cJSON_AddNumberToObject(o, "live_images", (double)avk.device->live.images);
		cJSON_AddNumberToObject(o, "live_image_views",
			(double)avk.device->live.image_views);
		cJSON_AddNumberToObject(o, "live_device_memory",
			(double)avk.device->live.device_memory);
		cJSON_AddNumberToObject(o, "live_buffers",
			(double)avk.device->live.buffers);
		cJSON_AddNumberToObject(o, "live_descriptor_pools",
			(double)avk.device->live.descriptor_pools);
		cJSON_AddNumberToObject(o, "live_command_pools",
			(double)avk.device->live.command_pools);
		cJSON_AddNumberToObject(o, "live_semaphores",
			(double)avk.device->live.semaphores);
		cJSON_AddNumberToObject(o, "live_avk_images",
			(double)avk.device->live.avk_images);
		cJSON_AddNumberToObject(o, "live_avk_uploads",
			(double)avk.device->live.avk_uploads);
	}
	/*
	 * Pointer focus traffic. Not renderer state, and it does not belong to
	 * AVK -- it is here because this is the channel that can be read from a
	 * running session, and because the question it answers is otherwise pure
	 * inference: a client whose hover state flickers while the pointer and
	 * the window are both stationary is either being told the pointer moved,
	 * or it is not. These two counters decide it.
	 */
	cJSON_AddNumberToObject(o, "pointer_enters", (double)az_pointer_enters);
	cJSON_AddNumberToObject(o, "pointer_focus_clears",
		(double)az_pointer_focus_clears);
	cJSON_AddNumberToObject(o, "pointer_motions", (double)az_pointer_motions);
	cJSON_AddNumberToObject(o, "pointer_notify_internal",
		(double)az_pointer_notify_internal);
	cJSON_AddBoolToObject(o, "cursor_force_software",
		az_cursor_force_software());
	/*
	 * The cursor image's OWN upload history, read straight off D.1's per-buffer
	 * record rather than counted again here.
	 *
	 * This is the whole "position changed != pixels changed" invariant in four
	 * numbers: a cursor dragged across the screen for thirty seconds must move
	 * the lookup count and leave commits, uploads and bytes exactly where they
	 * were. Counting it separately would have let the two drift apart, which is
	 * precisely the class of bug D.1 existed to remove.
	 */
	if (az_cursor.buffer != NULL) {
		struct wlr_addon *cursor_addon = wlr_addon_find(
			&az_cursor.buffer->addons, &avk, &az_avk_buffer_addon_impl);
		if (cursor_addon != NULL) {
			struct az_avk_buffer *ce = wl_container_of(cursor_addon, ce, addon);
			cJSON_AddNumberToObject(o, "cursor_source_commits",
				(double)ce->stat_generations);
			cJSON_AddNumberToObject(o, "cursor_source_uploads",
				(double)(ce->stat_full_uploads + ce->stat_partial_uploads));
			cJSON_AddNumberToObject(o, "cursor_source_upload_bytes",
				(double)ce->stat_upload_bytes);
			cJSON_AddNumberToObject(o, "cursor_source_upload_skips",
				(double)ce->stat_lookups);
		}
	}
	cJSON_AddNumberToObject(o, "validation_errors",
		(double)avk_validation_errors());
	/*
	 * WHETHER THAT COUNTER COULD HAVE MOVED AT ALL.
	 *
	 * `validation_errors` only ever increments from the validation layer's
	 * message callback, so with the layer absent it reads 0 no matter what the
	 * frame did -- and every fixture asserting `validation_errors == 0` without
	 * ASTEROIDZ_VK_DEBUG has therefore been asserting nothing. That is not a
	 * hypothetical: Path A spent a milestone attaching an _SRGB view to
	 * pipelines that declared the UNORM twin (VUID-...-08910, twenty times in
	 * one run), green in every headless fixture, and caught the first time the
	 * on-GPU unit test was run under the layer.
	 *
	 * So the premise is a field now. A fixture that asserts the count asserts
	 * this first, exactly as it would assert any other premise.
	 */
	cJSON_AddBoolToObject(o, "validation_enabled",
		avk.instance != NULL && avk.instance->validation_enabled);
	return o;
}

/*
 * Zero the counters without restarting.
 *
 * Live values -- how many images are cached, how many targets exist -- are
 * deliberately NOT reset: they describe the present, not an interval, and
 * zeroing them would make the next reading a lie until the caches turned over.
 */
/* M4I. Per-frame GPU tracing on every renderer that exists, at runtime. */
static void az_avk_set_frame_trace(bool on) {
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			avk_timestamps_set_trace(&avk.renderers[i].renderer.timestamps, on);
		}
	}
}

/*
 * M4I. Per-CHAIN tracing: one line per blur chain, with its role and geometry.
 *
 * Separate from the frame trace because they answer different questions and
 * cost different amounts. The frame trace is one line per frame; this is one
 * line per chain, and a busy desktop runs several per frame -- so it is armed
 * for a specific experiment and turned off again, never left on.
 */
static void az_avk_set_blur_cache(bool on) {
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			avk_render_set_blur_cache_enabled(&avk.renderers[i].renderer, on);
		}
	}
}

/* M4I instrument. -1 none, otherwise an avk_blur_cache_kind whose consumers are
 * treated as absent. Every renderer, because a monitor's cache belongs to
 * whichever format renderer drew it. */
static void az_avk_set_blur_cache_starve(int kind) {
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			avk_render_set_blur_cache_starve(&avk.renderers[i].renderer, kind);
		}
	}
}

static void az_avk_set_blur_chain_trace(bool on) {
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			avk.renderers[i].renderer.blur_chain_trace = on ? 1 : 0;
		}
	}
}

static void az_avk_stats_reset(void) {
	/*
	 * The event-loop blocking counters, zeroed with everything else.
	 *
	 * These were added to answer "did the shm copy come off the event loop",
	 * and were then left out of the reset -- so a reproduction started with
	 * handler_us_max already carrying the wallpaper's 6.2ms from session
	 * start. A maximum that survives the reset is not a maximum of the
	 * measurement window, and it is the one number a reader trusts most.
	 */
	avk.commit_ns_max = avk.commit_ns_sum = avk.commit_samples = 0;
	avk.handler_ns_max = avk.handler_ns_sum = 0;
	avk.handler_over_10ms = avk.handler_over_30ms = 0;
	/*
	 * M4I. The cache's INTERVAL counters, zeroed with everything else.
	 *
	 * They live on the output rather than the renderer, so they were missed by
	 * the loop below -- and the first run of the role fixture reported
	 * `rebuilds=1` beside `MONITOR_BACKGROUND chains 0.00/frame`, because one
	 * number covered the whole session and the other only the interval since
	 * the reset. Two counters describing two different windows of time is the
	 * exact shape of the cold/warm error this milestone has already made twice.
	 *
	 * `bytes`, `generation`, `valid` and the slots are NOT touched: they
	 * describe the present, not an interval, and zeroing them would make the
	 * next reading a lie until the cache happened to turn over.
	 */
	Monitor *rm;
	wl_list_for_each(rm, &mons, link) {
		if (rm->avk == NULL) {
			continue;
		}
		struct avk_blur_cache *c = &rm->avk->blur_cache;
		c->requests = c->hits = c->rebuilds = c->invalidations = 0;
		c->saved_prefix_draws = c->saved_prefix_px = 0;
		c->saved_chains = c->saved_blur_px = 0;
		c->inv_generation = c->inv_geometry = c->inv_params = 0;
		c->inv_format = c->inv_never_built = c->inv_forced = 0;
		/* inv_source with the rest of the reason table, which it was left out
		 * of. It is the one counter a fixture reads as an absolute ("no source
		 * disagreement happened in this interval"), so a monotonic value here
		 * carries a previous interval's diagnosis into this one's assertion. */
		c->inv_source = 0;
		memset(c->hits_by_kind, 0, sizeof(c->hits_by_kind));
		memset(c->rebuilds_by_kind, 0, sizeof(c->rebuilds_by_kind));
	}
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (!avk.renderers[i].used) {
			continue;
		}
		/* The per-frame half of the transient counters. See the two-time-bases
		 * note where these are reported: `creates` and the live/bytes/peak
		 * gauges are pool state and deliberately survive. */
		struct avk_transient_stats *ts =
			&avk.renderers[i].renderer.transients.stats;
		ts->acquires = 0;
		ts->reuses = 0;
		ts->retires = 0;
		/*
		 * THE GRADIENT DRAW COUNTERS, which were not being reset and are
		 * per-interval in every way that matters.
		 *
		 * `gradient_draws` beside `frames` is precisely the two-time-bases
		 * error the note above warns about: the border fixture read
		 * `frames=0 draws=24` and could not tell "it drew 24 times just now"
		 * from "it drew 24 times at startup and has been idle since". A
		 * livelock test built on that difference cannot be built on a counter
		 * that does not have it.
		 *
		 * The BUFFER gauges go too, for the same reason the transient pool
		 * keeps `creates`: uploads, reuses and grows are events in an
		 * interval, not state. Capacity and the slots themselves are state and
		 * are not touched.
		 */
		struct avk_gradient_stats *gs =
			&avk.renderers[i].renderer.gradients.stats;
		gs->draws = gs->linear_draws = gs->conic_draws = 0;
		gs->colors_processed = 0;
		gs->buffer_uploads = gs->buffer_upload_bytes = 0;
		gs->buffer_reuses = gs->buffer_grows = 0;
	}
	avk.frames = 0;
	avk.fallback_frames = 0;
	avk.buffer_imports = 0;
	avk.buffer_import_fails = 0;
	avk.shm_uploads = 0;
	avk.shm_upload_bytes = 0;
	avk.shm_commits = 0;
	avk.shm_full_uploads = 0;
	avk.shm_partial_uploads = 0;
	avk.shm_upload_skips = 0;
	avk.shm_damage_pixels = 0;
	avk.shm_committed_pixels = 0;
	avk.shm_commit_ns_max = 0;
	avk.shm_commit_ns_sum = 0;
	avk.shm_commit_samples = 0;
	avk.shm_commit_over_5ms = 0;
	avk.shm_copy_ns_max = 0;
	avk.shm_copy_ns_sum = 0;
	avk.shm_copy_samples = 0;
	avk.shm_async_jobs = 0;
	avk.shm_async_bytes = 0;
	avk.shm_sync_copies = 0;
	avk.shm_async_joins = 0;
	avk.shm_async_join_waits = 0;
	avk.shm_async_join_ns_max = 0;
	avk.shm_staging_waits = 0;
	avk.acquire_waits = avk.acquire_explicit = avk.acquire_implicit = 0;
	avk.acquire_dropped = avk.acquire_import_fails = 0;
	avk.release_points_set = avk.release_points_dropped = 0;
	avk.shm_stale_frames = 0;
	avk.shm_stale_from_surface = 0;
	avk.shm_stale_multi_output_repaints = 0;
	avk.shm_stale_no_pool = 0;
	avk.shm_stale_no_sibling = 0;
	avk.shm_stale_pool_size_sum = avk.shm_stale_pool_samples = 0;
	avk.shm_staging_wait_ns_max = avk.shm_staging_wait_ns_sum = 0;
	avk.shm_async_join_ns_sum = 0;
	avk.buffer_resolve_attempts = 0;
	avk.nodes_output_culled_before_resolve = 0;
	avk.blur_nodes_seen = 0;
	avk.blur_nodes_emitted = 0;
	avk.blur_nodes_culled = 0;
	avk.nodes_retained_for_halo = 0;
	avk.blur_halo_damage_frames = 0;
	avk.blur_halo_damage_lost = 0;
	avk.damage_ring_out_of_bounds = 0;
	avk.blur_halo_px = 0;
	avk.blur_nodes_forced_live = 0;
	avk.blur_nodes_clipped = 0;
	avk.commit_imports = 0;
	avk.late_imports = 0;
	avk.cache_hits = 0;
	avk.cache_misses = 0;
	avk.damage_pixels = 0;
	avk.output_pixels = 0;
	avk.full_redraw_frames = 0;
	avk.partial_redraw_frames = 0;
	avk.damage_rects_max = 0;
	avk.cpu_frame_us = 0;
	avk.cpu_frame_us_max = 0;
	avk.cpu_frame_hist = (struct az_avk_hist){0};
	avk.gpu_submit_hist = (struct az_avk_hist){0};
	avk.damage_permille_hist = (struct az_avk_hist){0};
	avk.software_cursor_frames = 0;
	avk.hardware_cursor_frames = 0;
	avk.cursor_geometry_mismatch = 0;
	avk.cursor_commands = 0;
	avk.cursor_no_image = 0;
	avk.cursor_import_failures = 0;
	avk.cursor_culled = 0;
	avk.cursor_moves = 0;
	avk.cursor_damage_pixels = 0;
	avk.cursor_hw_to_sw = 0;
	avk.cursor_sw_to_hw = 0;
	az_cursor.sets_client = 0;
	az_cursor.sets_shape = 0;
	az_cursor.sets_xcursor = 0;
	az_cursor.unsets = 0;
	az_cursor.forced_reimports = 0;
	avk.presentation_waits = 0;
	avk.present_sync_timeline = 0;
	avk.present_sync_dmabuf = 0;
	avk.present_sync_none = 0;
	avk.present_sync_fails = 0;
	avk.target_state_violations = 0;
	if (avk.active) {
		avk.importer.imports_explicit = 0;
		avk.importer.imports_recovered = 0;
		avk.importer.imports_copied = 0;
		avk.importer.imports_failed = 0;
		avk.importer.copied_bytes = 0;
		avk.importer.copied_us = 0;
		for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
			if (avk.renderers[i].used) {
				struct avk_renderer *r = &avk.renderers[i].renderer;
				r->stats = (struct avk_renderer_stats){0};
				/* Re-arm the M4H draw ledger from here. The segment counter
				 * is process-wide, so without this AZ_AVK_CMD_DUMP would be
				 * exhausted by startup frames long before a fixture reached
				 * the scene it wanted to see. "Reset the stats" already means
				 * "the measurement starts now" everywhere else. */
				r->dump_seg = 0;
				/* The blur accounting too. It lives beside the renderer stats
				 * rather than inside them, and a reset that zeroed one and not
				 * the other would report a per-frame cost against a frame count
				 * that had started again. */
				r->blur_prefix_replays = 0;
				r->blur_prefix_commands = 0;
				r->blur_prefix_pixels = 0;
				/* M4F.2B's areas too. Every one of these is a running total,
				 * and a ratio taken across a reset boundary would compare a
				 * partial numerator with a whole denominator -- which reads as
				 * a saving that is not there. */
				r->blur_source_damage_pixels = 0;
				r->blur_output_damage_pixels = 0;
				r->blur_prefix_rebuild_pixels = 0;
				r->blur_prefix_copyable_pixels = 0;
				/* The deadline itself survives -- it describes the output, not
				 * the measurement window -- but every count against it starts
				 * again, or "frames over budget" would be read against a frame
				 * total that had been reset underneath it. */
				r->timestamps.budget_frames = 0;
				r->timestamps.over_budget = 0;
				r->timestamps.over_budget_2x = 0;
				r->timestamps.over_budget_3x = 0;
				r->blur_fallback_rects = 0;
				r->blur_fallback_area_before = 0;
				r->blur_fallback_area_after = 0;
				r->blur_full_dependency_pixels = 0;
				r->blur_full_write_pixels = 0;
				r->blur_full_capture_pixels = 0;
				r->blur_damage_saved_pixels = 0;
				r->blur_damage_nodes_touched = 0;
				r->blur_damage_nodes_skipped = 0;
				r->blur_damage_fallbacks = 0;
				r->blur_damage_rects_max = 0;
				r->blur_transitive_damage_pixels = 0;
				r->blur_damage_build_ns = 0;
				r->blur_halo_pixels = 0;
				r->blur_capture_pixels = 0;
				r->blur_result_pixels = 0;
				r->blur_processed_pixels = 0;
				r->blur_required_work_pixels = 0;
				memset(r->blur_role_chains, 0,
					sizeof(r->blur_role_chains));
				memset(r->blur_role_capture_px, 0,
					sizeof(r->blur_role_capture_px));
				memset(r->blur_role_rebuild_px, 0,
					sizeof(r->blur_role_rebuild_px));
				memset(r->blur_role_result_px, 0,
					sizeof(r->blur_role_result_px));
				memset(r->blur_role_prefix_cmds, 0,
					sizeof(r->blur_role_prefix_cmds));
				r->blur_max_slots = 0;
				r->blur_removable_up0_pixels = 0;
				r->blur_removable_up01_pixels = 0;
				r->blur_removable_up_pixels = 0;
				r->blur_region_build_hist = (struct avk_hist){0};
				/*
				 * ── AND THE GPU HISTOGRAMS, WHICH SURVIVED A RESET ────────
				 *
				 * These live in `timestamps`, not in `stats`, so zeroing the
				 * renderer stats left them alone: a benchmark that reset and
				 * then measured 21 frames reported 31 blur_total samples,
				 * eleven of them from before the reset. Every GPU percentile
				 * in that window was a mixture of two workloads.
				 *
				 * The SLOTS are deliberately left alone: a frame submitted
				 * before the reset will still land in one, and dropping it
				 * would only trade contamination for a lost sample. What the
				 * reset owns is the accumulated distribution.
				 */
				struct avk_timestamps *ts = &r->timestamps;
				ts->gpu_frame_hist = (struct avk_hist){0};
				ts->blur_total_hist = (struct avk_hist){0};
				ts->blur_prefix_hist = (struct avk_hist){0};
				ts->blur_down_hist = (struct avk_hist){0};
				ts->blur_up_hist = (struct avk_hist){0};
				ts->blur_up0_hist = (struct avk_hist){0};
				ts->gpu_frame_blur_hist = (struct avk_hist){0};
				ts->blur_pre_hist = (struct avk_hist){0};
				ts->blur_post_hist = (struct avk_hist){0};
				ts->cohort_blur_frames = 0;
				ts->cohort_idle_frames = 0;
				ts->straddled = 0;
				/* Anything already in flight belongs to the window that just
				 * ended, and is dropped when it comes back. */
				avk_timestamps_new_generation(ts);
				ts->gpu_frame_ns_total = 0;
				ts->samples = 0;
				ts->dropped = 0;
				r->blur_stats = (struct avk_blur_stats){0};
			}
		}
	}
}

static void az_avk_log_stats(void) {
	if (!avk.active) {
		return;
	}
	wlr_log(WLR_INFO, "avk.frames=%" PRIu64 " avk.fallback_frames=%" PRIu64
		" avk.buffer_imports=%" PRIu64 " avk.buffer_import_fails=%" PRIu64
		" avk.shm_uploads=%" PRIu64, avk.frames, avk.fallback_frames,
		avk.buffer_imports, avk.buffer_import_fails, avk.shm_uploads);
	wlr_log(WLR_INFO, "avk.shm_commits=%" PRIu64 " avk.shm_full_uploads=%" PRIu64
		" avk.shm_partial_uploads=%" PRIu64 " avk.shm_upload_skips=%" PRIu64
		" avk.shm_upload_bytes=%" PRIu64, avk.shm_commits, avk.shm_full_uploads,
		avk.shm_partial_uploads, avk.shm_upload_skips, avk.shm_upload_bytes);
	wlr_log(WLR_INFO, "avk.shm_damage_pixels=%" PRIu64
		" avk.shm_committed_pixels=%" PRIu64
		" avk.buffer_resolve_attempts=%" PRIu64
		" avk.nodes_output_culled_before_resolve=%" PRIu64,
		avk.shm_damage_pixels, avk.shm_committed_pixels,
		avk.buffer_resolve_attempts, avk.nodes_output_culled_before_resolve);
	/* The block, in the log as well as over IPC: a session that felt laggy
	 * should be diagnosable from what it left behind rather than only from a
	 * counter someone thought to read while it was running. */
	wlr_log(WLR_INFO, "avk.shm_async_upload=%s avk.shm_commit_us_max=%" PRIu64
		" avk.shm_commit_us_avg=%" PRIu64 " avk.shm_commit_over_5ms=%" PRIu64
		" avk.shm_copy_us_max=%" PRIu64 " avk.shm_copy_us_total=%" PRIu64,
		az_avk_shm_async_enabled() ? "yes" : "no",
		avk.shm_commit_ns_max / 1000,
		avk.shm_commit_samples
			? avk.shm_commit_ns_sum / avk.shm_commit_samples / 1000 : 0,
		avk.shm_commit_over_5ms, avk.shm_copy_ns_max / 1000,
		avk.shm_copy_ns_sum / 1000);
	wlr_log(WLR_INFO, "avk.shm_async_jobs=%" PRIu64 " avk.shm_sync_copies=%"
		PRIu64 " avk.shm_async_join_waits=%" PRIu64
		" avk.shm_async_join_us_max=%" PRIu64, avk.shm_async_jobs,
		avk.shm_sync_copies, avk.shm_async_join_waits,
		avk.shm_async_join_ns_max / 1000);
	wlr_log(WLR_INFO, "avk.commit_imports=%" PRIu64 " avk.late_imports=%" PRIu64
		" avk.cache_hits=%" PRIu64 " avk.cache_misses=%" PRIu64,
		avk.commit_imports, avk.late_imports, avk.cache_hits,
		avk.cache_misses);
	wlr_log(WLR_INFO, "avk.damage_pixels=%" PRIu64 " avk.output_pixels=%" PRIu64
		" avk.full_redraw_frames=%" PRIu64 " avk.partial_redraw_frames=%" PRIu64
		" avk.damage_rects_max=%" PRIu64, avk.damage_pixels, avk.output_pixels,
		avk.full_redraw_frames, avk.partial_redraw_frames,
		avk.damage_rects_max);
	wlr_log(WLR_INFO, "avk.present_sync_timeline=%" PRIu64
		" avk.present_sync_dmabuf=%" PRIu64 " avk.present_sync_none=%" PRIu64
		" avk.present_sync_fails=%" PRIu64 " avk.presentation_waits=%" PRIu64
		" avk.target_state_violations=%" PRIu64, avk.present_sync_timeline,
		avk.present_sync_dmabuf, avk.present_sync_none, avk.present_sync_fails,
		avk.presentation_waits, avk.target_state_violations);
	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			avk_renderer_log_stats(&avk.renderers[i].renderer);
		}
	}
	avk_dmabuf_importer_log_stats(&avk.importer);
}

/*
 * Stop the GPU before anything AVK owns starts being destroyed.
 *
 * This is called from cleanup() BEFORE wlr_backend_destroy(), and the "before"
 * is the entire point. Destroying the outputs runs az_avk_output_destroy() ->
 * avk_sync_finish(), which calls vkDestroySemaphore on the per-output present
 * fence and its wait slots -- semaphores that the last frame's submission
 * still refers to. Vulkan requires that "all submitted batches that refer to
 * semaphore must have completed execution"
 * (VUID-vkDestroySemaphore-semaphore-05149), and nothing had made that true:
 * the only waits in teardown were inside az_avk_finish(), which runs long
 * afterwards.
 *
 * A driver asked to destroy an object a pending submission still lists does
 * not usually fail loudly. It frees the host allocation behind that object and
 * leaves the submission's reference dangling, and the damage surfaces later,
 * at some unrelated free() -- which is why every one of the four aborts this
 * was found through landed inside az_avk_finish(), in the frees that came
 * afterwards, rather than at the destruction that actually caused it.
 *
 * AZ_AVK_NO_TEARDOWN_IDLE=1 removes the wait. That is the break switch, and it
 * puts the compositor back on the code that crashed.
 */
static void az_avk_quiesce(void) {
	if (!avk.active) {
		return;
	}
	if (az_avk_env_flag("AZ_AVK_NO_TEARDOWN_IDLE")) {
		wlr_log(WLR_ERROR, "AZ_AVK_NO_TEARDOWN_IDLE=1 -- outputs will be torn "
			"down with GPU work still in flight");
		return;
	}
	avk_device_wait_idle(avk.device);
}

/*
 * Teardown markers.
 *
 * These exist because a teardown test that cannot prove teardown RAN is not a
 * teardown test. Ten headless "clean shutdown" cycles were once reported as
 * evidence when the compositor under test had no Vulkan in it at all: the
 * check was the absence of a crash, and absence of a crash is also what you
 * get from code that never executed.
 *
 * The stats line below is not a substitute. It is skipped when AVK is
 * inactive, so its absence is ambiguous between "cleanup() never ran", "this
 * binary has no AVK" and "AVK was never asked for" -- three different
 * failures that a harness must not confuse. BEGIN is printed before any
 * decision, END only after the last object is gone, and both name the state,
 * so a log answers the question directly.
 */
static void az_avk_finish(void) {
	wlr_log(WLR_INFO, "AVK_TEARDOWN_BEGIN active=%s", avk.active ? "yes" : "no");
	if (!avk.active) {
		wlr_log(WLR_INFO, "AVK_TEARDOWN_END reason=inactive");
		return;
	}
	az_avk_log_stats();

	/*
	 * The worker goes FIRST, and it goes by settling rather than by killing.
	 *
	 * Every job in flight owns a client buffer lock and a staging buffer, and
	 * it is reading a mapping the loop below is about to unmap. Stopping the
	 * thread without draining would leak the first two; destroying the entries
	 * without stopping the thread would pull the third out from under it.
	 */
	az_avk_shm_drain_blocking();
	if (avk.upload_worker != NULL) {
		avk_upload_worker_destroy(avk.upload_worker);
		avk.upload_worker = NULL;
	}

	/*
	 * Submissions stop, THEN the GPU is waited for, THEN anything is
	 * destroyed. In that order and once, at the top.
	 *
	 * It used to be three separate waits buried one each inside
	 * avk_renderer_finish(), avk_dmabuf_importer_finish() and
	 * avk_device_destroy(). Each was correct about its own resources and none
	 * of them said anything about the order between them -- so the loop below,
	 * which hands every client image to the retire queue, ran with no wait in
	 * front of it at all, and a caller wanting quiescence had no way to ask
	 * for it except by knowing which finish() happened to include one.
	 *
	 * A wait here is not a frame-path wait. avk.cpu_sync_waits stays zero:
	 * there is no frame after this. Submissions have already stopped --
	 * cleanup() clears allow_frame_scheduling before anything else, and the
	 * backend and the scene are already gone by the time this runs.
	 */
	avk_device_wait_idle(avk.device);

	/* The images first, then the queues that free them, then the device. Any
	 * other order either leaks or frees twice. */
	struct az_avk_buffer *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &avk.buffers, link) {
		az_avk_buffer_destroy(entry);
	}

	for (size_t i = 0; i < AZ_AVK_MAX_FORMATS; i++) {
		if (avk.renderers[i].used) {
			avk_renderer_finish(&avk.renderers[i].renderer);
			avk.renderers[i].used = false;
		}
	}
	avk_dmabuf_importer_finish(&avk.importer);
	avk_device_destroy(avk.device);
	avk_instance_destroy(avk.instance);
	avk.device = NULL;
	avk.instance = NULL;
	avk.active = false;
	wlr_log(WLR_INFO, "AVK_TEARDOWN_END reason=complete");
}

#endif /* AZ_AVK_H */
