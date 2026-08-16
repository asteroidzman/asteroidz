#ifndef AZ_SHM_SOURCE_H
#define AZ_SHM_SOURCE_H

/*
 * ── READING A CLIENT'S wl_shm PIXELS FROM ANOTHER THREAD ──────────────────
 *
 * A wl_shm buffer's pixels have to be copied into staging before the GPU can
 * see them, and AVK does that copy on a worker thread (see
 * vulkan/image/avk_upload_worker.h). The copy therefore outlives the commit
 * handler that started it, and something has to keep the client's memory
 * mapped, stable and safe to touch for that whole interval.
 *
 * THE OBVIOUS ANSWER IS WRONG, AND IT ABORTED A LIVE SESSION.
 *
 * wlr_buffer_begin_data_ptr_access() looks like exactly the right tool: it
 * hands back the mapping and, for wl_shm, it also pins the mapping and installs
 * wlroots' SIGBUS handler for as long as it is held (types/wlr_shm.c --
 * mapping_consider_destroy() keeps a dropped mapping alive if and only if some
 * accessor still references it). Holding it across the commit handler's return
 * gives the worker everything it needs.
 *
 * It also asserts. `buffer->accessing_data_ptr` is a PLAIN BOOL, not a counter
 * (types/buffer/buffer.c:85): one accessor per buffer, process-wide, no
 * nesting. And there are other accessors, reached from code we do not control:
 *
 *   - wlr_buffer_is_opaque() opens any buffer whose format it cannot name --
 *     which is every buffer that answers neither get_dmabuf nor get_shm, i.e.
 *     wp_single_pixel_buffer_v1. SceneFX calls it from
 *     scene_buffer_set_buffer(), inside the SAME wl_surface commit signal that
 *     started our job. kitty's GLFW binds wp_single_pixel_buffer_manager_v1,
 *     so this fired on the first kitty window and killed the compositor.
 *
 *   - wlr_output_cursor_set_buffer() calls wlr_texture_from_buffer(), and every
 *     renderer's shm path opens the buffer to upload it. A client's wl_shm
 *     cursor surface goes through AVK's commit handler like any other surface,
 *     so a deferred cursor copy would collide with the cursor upload -- on a
 *     real backend, and not headlessly, which is the worst place for it.
 *
 * Excluding those two by name would be a list to keep in step with two upstream
 * projects. So AVK does not hold wlroots' access at all. It maps the client's
 * pool file ITSELF, from the fd wlr_buffer_get_shm() reports, and owns that
 * mapping outright:
 *
 *   - it is a second, independent mmap of the same file, so wl_shm_pool.resize
 *     replacing wlroots' mapping does not touch it (a pool may only GROW --
 *     wlr_shm.c refuses a shrink -- so the pages under our mapping keep their
 *     contents and their offsets);
 *   - it survives the client destroying the pool and closing the fd, because an
 *     mmap holds its own reference to the file;
 *   - nothing else in the process knows about it, so nothing else can free it
 *     or assert about it.
 *
 * WHAT WE GIVE UP BY NOT HOLDING WLROOTS' ACCESS is wlroots' SIGBUS handler,
 * and that is not optional: a client is free to ftruncate() its own pool file
 * smaller, and reading the vanished pages would fault. wlroots survives that;
 * so must this. The recovery below is wlroots' own recipe (types/wlr_shm.c
 * handle_sigbus) applied to our mappings: map anonymous zeroes over the hole
 * and let the copy finish reading zeroes.
 *
 * The handler is installed ONCE, at AVK start-up, before any client can
 * connect, and never removed. That ordering is what makes it compose with
 * wlroots': wlroots installs its own handler lazily on its first access and
 * saves whatever was there as `prev_action`, so it saves ours and chains to it
 * for any address it does not recognise -- which is exactly the set of
 * addresses that are ours. Installing later would let wlroots restore SIG_DFL
 * over us when its own accessor count reached zero.
 *
 * The registration list is walked from a signal handler, so it is an _Atomic
 * chain -- and, more usefully, entries are only ever added on first deferred
 * use and removed when the wlr_buffer is destroyed, which cannot happen while a
 * copy is in flight (the job holds a wlr_buffer_lock, and the destroy path
 * finishes the job first). A mapping is therefore never unregistered out from
 * under a faulting worker.
 */

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

/* AVK's private view of one client wl_shm buffer's backing file. Embedded in
 * az_avk_buffer, so its lifetime is the wlr_buffer's. */
struct az_shm_source {
	void *base;        /* our mmap of the pool file from offset 0 */
	size_t size;       /* its length; also what the SIGBUS guard covers */
	size_t offset;     /* where THIS buffer's first row starts inside base */
	uint32_t stride;
	uint32_t format;
	struct az_shm_source *_Atomic next;   /* az_shm_sources, when registered */
	bool registered;
};

/* Needs to be lock-free: it is read from a signal handler. */
static struct az_shm_source *_Atomic az_shm_sources = NULL;
static struct sigaction az_shm_prev_sigbus;
static bool az_shm_guard_installed = false;

static void az_shm_reraise(int sig, siginfo_t *info, void *context) {
	if (az_shm_prev_sigbus.sa_flags & SA_SIGINFO) {
		az_shm_prev_sigbus.sa_sigaction(sig, info, context);
		return;
	}
	if (az_shm_prev_sigbus.sa_handler == SIG_IGN) {
		return;
	}
	if (az_shm_prev_sigbus.sa_handler == SIG_DFL) {
		/* Restore the default and let it happen, rather than returning to the
		 * faulting instruction and looping forever on the same address. */
		struct sigaction dfl = { .sa_handler = SIG_DFL };
		sigaction(sig, &dfl, NULL);
		raise(sig);
		return;
	}
	az_shm_prev_sigbus.sa_handler(sig);
}

static void az_shm_handle_sigbus(int sig, siginfo_t *info, void *context) {
	uintptr_t addr = (uintptr_t)info->si_addr;
	for (struct az_shm_source *s = atomic_load(&az_shm_sources); s != NULL;
			s = atomic_load(&s->next)) {
		uintptr_t start = (uintptr_t)s->base;
		if (addr < start || addr >= start + s->size) {
			continue;
		}
		/* The client truncated its pool under us. Replace the hole with
		 * anonymous zeroes so the read that faulted can complete; the copy
		 * finishes with black where the pixels went, which is what wlroots
		 * does and is the only outcome that is not a crash. mmap() is not on
		 * the async-signal-safe list, and wlroots says the same about its own
		 * handler -- there is no safe call that does this job. */
		if (mmap(s->base, s->size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) != MAP_FAILED) {
			return;
		}
		break;
	}
	az_shm_reraise(sig, info, context);
}

/*
 * Install the guard. Call once, before any client can connect -- see the header
 * comment for why the ordering against wlroots' own handler matters.
 */
static void az_shm_guard_install(void) {
	if (az_shm_guard_installed) {
		return;
	}
	struct sigaction action = {
		.sa_sigaction = az_shm_handle_sigbus,
		.sa_flags = SA_SIGINFO | SA_NODEFER,
	};
	if (sigaction(SIGBUS, &action, &az_shm_prev_sigbus) != 0) {
		wlr_log_errno(WLR_ERROR, "AVK: could not install a SIGBUS handler; "
			"client shm copies will stay on the event loop");
		return;
	}
	az_shm_guard_installed = true;
}

static bool az_shm_guard_ready(void) {
	return az_shm_guard_installed;
}

/*
 * Map a wl_shm buffer's backing file privately to AVK, once per buffer.
 *
 * Returns the address of the buffer's first row, or NULL if this is not a
 * wl_shm buffer (a single-pixel buffer, a dma-buf, one of the compositor's own
 * buffer types) or the mapping could not be made. NULL is not an error: the
 * caller copies on this thread instead, exactly as it did before any of this
 * existed.
 */
static const void *az_shm_source_get(struct az_shm_source *src,
		struct wlr_buffer *buffer, uint32_t *stride, uint32_t *format) {
	if (src->base != NULL) {
		*stride = src->stride;
		*format = src->format;
		return (const uint8_t *)src->base + src->offset;
	}
	if (!az_shm_guard_ready()) {
		return NULL;
	}

	struct wlr_shm_attributes attrs;
	if (!wlr_buffer_get_shm(buffer, &attrs)) {
		return NULL;
	}
	if (attrs.stride <= 0 || attrs.offset < 0 || buffer->height <= 0) {
		return NULL;
	}
	/* From 0 rather than from `offset`: mmap's offset must be page-aligned and
	 * wl_shm's need not be. The extra pages cost address space and nothing
	 * else. wlroots has already checked this span fits the pool. */
	size_t size = (size_t)attrs.offset
		+ (size_t)attrs.stride * (size_t)buffer->height;
	void *base = mmap(NULL, size, PROT_READ, MAP_SHARED, attrs.fd, 0);
	if (base == MAP_FAILED) {
		wlr_log_errno(WLR_DEBUG, "AVK: cannot map a client's shm pool; its "
			"copies will stay on the event loop");
		return NULL;
	}

	src->base = base;
	src->size = size;
	src->offset = (size_t)attrs.offset;
	src->stride = (uint32_t)attrs.stride;
	src->format = attrs.format;

	/* Registered before the address is ever handed out, so a fault can never
	 * reach an address the handler does not know about. */
	struct az_shm_source *head = atomic_load(&az_shm_sources);
	do {
		atomic_store(&src->next, head);
	} while (!atomic_compare_exchange_weak(&az_shm_sources, &head, src));
	src->registered = true;

	*stride = src->stride;
	*format = src->format;
	return (const uint8_t *)src->base + src->offset;
}

/* Give the mapping back. The caller must know no copy is reading it -- see the
 * header comment; az_avk_buffer_destroy() finishes any job first. */
static void az_shm_source_finish(struct az_shm_source *src) {
	if (src->registered) {
		struct az_shm_source *_Atomic *link = &az_shm_sources;
		for (;;) {
			struct az_shm_source *cur = atomic_load(link);
			if (cur == NULL) {
				break;
			}
			if (cur == src) {
				atomic_store(link, atomic_load(&src->next));
				break;
			}
			link = &cur->next;
		}
		src->registered = false;
	}
	if (src->base != NULL) {
		munmap(src->base, src->size);
		src->base = NULL;
		src->size = 0;
	}
}

#endif /* AZ_SHM_SOURCE_H */
