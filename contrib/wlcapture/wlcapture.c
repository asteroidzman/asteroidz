/*
 * wlcapture -- an ext-image-copy-capture client that consumes frames and says
 * what it was offered.
 *
 * M14's audit named this the blocker: "verifying it needs a capture client
 * consuming frames, and none exists in contrib/". contrib/wlshot is a
 * wlr-screencopy client and captures one frame; screencopy is the older
 * protocol and its session carries no format negotiation to read. The question
 * M14 exists to answer -- what a capture client can actually get off an HDR
 * output, and how fast -- is a property of the ext-image-copy-capture SESSION,
 * which announces its buffer size and every shm and dmabuf format it will
 * accept before a single frame is copied.
 *
 * So this prints the offer first and captures second. On an HDR output the
 * offer is the measurement: if the only shm format is 8-bit XRGB8888 then
 * "portal capture forces the output down to SDR" is not a portal defect, it is
 * what the compositor advertises, and M14's stage taps are what change it.
 *
 * ── WHAT IT DOES NOT DO, DELIBERATELY ────────────────────────────────────
 *
 * shm only. dmabuf capture is a later M14 increment and needs a GBM device,
 * format-modifier negotiation and an import path; none of that is needed to
 * read the offer or to measure the rate, and a client that does both badly
 * would be worse than one that does one honestly. The dmabuf formats ARE
 * printed -- reading them costs nothing and they are half the answer.
 *
 * It also does not encode. hdr-record.sh already owns the ffmpeg half; what it
 * lacks is a source that produces more than one frame per second.
 *
 * ── THE RATE IS THE POINT ────────────────────────────────────────────────
 *
 * hdr-record.sh records HDR at 1 fps because screenshot_ui freezes the output
 * and does a full readback per call. This asks for frames back-to-back and
 * reports the interval between `ready` events, so "capture is a slideshow" or
 * "capture keeps up" stops being a matter of impression.
 *
 * Usage:
 *   wlcapture [--output NAME] [--frames N] [--out DIR] [--timeout MS]
 *             [--cursor] [--quiet]
 *
 *   --output NAME   which wl_output to capture (default: the first announced)
 *   --frames N      how many frames to capture (default 30; 0 = offer only)
 *   --out DIR       write each frame as DIR/frame_%06u.raw plus one .txt
 *                   sidecar naming the format, size, stride and transform
 *   --timeout MS    give up on a frame after this long (default 2000). A
 *                   capture completes when the output next presents, so an
 *                   idle desktop legitimately produces none.
 *   --cursor        ask for the cursor to be painted into the capture
 *
 * Exit: 0 all requested frames arrived, 1 a frame failed or the session
 * stopped, 2 the compositor does not offer the protocol at all.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

#define MAX_FORMATS 64

/* wl_shm spells ARGB8888 and XRGB8888 as 0 and 1; every other value in the
 * enum is the DRM fourcc itself. A table that forgets that reports the two
 * most common formats as garbage, which is exactly the kind of wrong answer
 * this tool exists to avoid producing. */
#define WL_SHM_ARGB8888 0
#define WL_SHM_XRGB8888 1

static uint32_t shm_to_fourcc(uint32_t f) {
	if (f == WL_SHM_ARGB8888)
		return 0x34325241; /* AR24 */
	if (f == WL_SHM_XRGB8888)
		return 0x34325258; /* XR24 */
	return f;
}

static const char *fourcc_name(uint32_t f) {
	static const struct {
		uint32_t cc;
		const char *name;
		int bpc;
	} table[] = {
		{0x34325241, "ARGB8888", 8},      {0x34325258, "XRGB8888", 8},
		{0x34324241, "ABGR8888", 8},      {0x34324258, "XBGR8888", 8},
		{0x30335241, "ARGB2101010", 10},  {0x30335258, "XRGB2101010", 10},
		{0x30334241, "ABGR2101010", 10},  {0x30334258, "XBGR2101010", 10},
		{0x38344241, "ABGR16161616", 16}, {0x38344258, "XBGR16161616", 16},
		{0x48344241, "ABGR16161616F", 16},{0x48344258, "XBGR16161616F", 16},
		{0x36314752, "RGB565", 5},
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (table[i].cc == f)
			return table[i].name;
	}
	return NULL;
}

static int fourcc_bpc(uint32_t f) {
	switch (f) {
	case 0x30335241: case 0x30335258:
	case 0x30334241: case 0x30334258:
		return 10;
	case 0x38344241: case 0x38344258:
	case 0x48344241: case 0x48344258:
		return 16;
	default:
		return 8;
	}
}

static const char *transform_name(uint32_t t) {
	static const char *names[] = {"normal", "90", "180", "270",
		"flipped", "flipped-90", "flipped-180", "flipped-270"};
	return t < 8 ? names[t] : "?";
}

static const char *failure_name(uint32_t reason) {
	switch (reason) {
	case 0: return "unknown";
	case 1: return "buffer-constraints";
	case 2: return "stopped";
	default: return "?";
	}
}

struct output_entry {
	struct wl_output *output;
	char *name;
	struct wl_list link;
};

static struct wl_shm *shm;
static struct ext_output_image_capture_source_manager_v1 *source_mgr;
static struct ext_image_copy_capture_manager_v1 *capture_mgr;
static struct wl_list outputs;

/* the session's announced offer */
static uint32_t buf_width, buf_height;
static uint32_t shm_formats[MAX_FORMATS];
static size_t shm_format_len;
static uint32_t dmabuf_formats[MAX_FORMATS];
static size_t dmabuf_format_len;
static volatile bool offer_done;
static volatile bool session_stopped;

/* the current frame */
static volatile bool frame_ready, frame_failed;
static uint32_t frame_fail_reason;
static uint32_t frame_transform;
static uint64_t frame_pts_ns;
static bool frame_pts_seen;

static int64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/*
 * Wait for one of the flags, or give up.
 *
 * The timeout is not defensive tidiness. A capture frame completes when the
 * output next PRESENTS, so on a compositor with nothing to redraw there is no
 * frame to copy and `ready` never arrives -- a plain wl_display_dispatch loop
 * then blocks forever on an idle desktop, which is indistinguishable from a
 * broken compositor and is how the first run of this tool appeared to hang.
 * "No frame within N ms" is a result, and the instrument has to be able to
 * report it.
 *
 * Returns true if a flag went true, false on timeout or connection loss.
 */
static bool wait_flags(struct wl_display *dpy, volatile bool *a,
		volatile bool *b, int timeout_ms) {
	int64_t deadline = now_ns() + (int64_t)timeout_ms * 1000000;
	while (!(a && *a) && !(b && *b)) {
		/* Anything already queued must be dispatched before sleeping on the
		 * fd, or a flag that arrived with the previous read is not seen until
		 * the next unrelated event wakes the poll. */
		if (wl_display_dispatch_pending(dpy) < 0)
			return false;
		if ((a && *a) || (b && *b))
			return true;
		if (wl_display_flush(dpy) < 0 && errno != EAGAIN)
			return false;

		int64_t left_ns = deadline - now_ns();
		if (left_ns <= 0)
			return false;
		struct pollfd pfd = {
			.fd = wl_display_get_fd(dpy),
			.events = POLLIN,
		};
		int n = poll(&pfd, 1, (int)(left_ns / 1000000) + 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (n == 0)
			return false;
		if (wl_display_dispatch(dpy) < 0)
			return false;
	}
	return true;
}

/* ── output bookkeeping ─────────────────────────────────────────────────── */

static void output_name(void *data, struct wl_output *o, const char *name) {
	(void)o;
	struct output_entry *e = data;
	free(e->name);
	e->name = strdup(name);
}
static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t sub, const char *make,
		const char *model, int32_t tr) {
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
	(void)make; (void)model; (void)tr;
}
static void output_mode(void *d, struct wl_output *o, uint32_t flags,
		int32_t w, int32_t h, int32_t refresh) {
	(void)d; (void)o; (void)flags; (void)w; (void)h; (void)refresh;
}
static void output_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t s) {
	(void)d; (void)o; (void)s;
}
static void output_description(void *d, struct wl_output *o, const char *desc) {
	(void)d; (void)o; (void)desc;
}
static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data;
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface,
			ext_output_image_capture_source_manager_v1_interface.name) == 0) {
		source_mgr = wl_registry_bind(registry, name,
			&ext_output_image_capture_source_manager_v1_interface, 1);
	} else if (strcmp(interface,
			ext_image_copy_capture_manager_v1_interface.name) == 0) {
		capture_mgr = wl_registry_bind(registry, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output_entry *e = calloc(1, sizeof(*e));
		if (!e)
			return;
		e->output = wl_registry_bind(registry, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_list_insert(outputs.prev, &e->link);
		if (version >= 4)
			wl_output_add_listener(e->output, &output_listener, e);
	}
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) {
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ── the session's offer ────────────────────────────────────────────────── */

static void session_buffer_size(void *data,
		struct ext_image_copy_capture_session_v1 *s,
		uint32_t width, uint32_t height) {
	(void)data; (void)s;
	buf_width = width;
	buf_height = height;
}
static void session_shm_format(void *data,
		struct ext_image_copy_capture_session_v1 *s, uint32_t format) {
	(void)data; (void)s;
	if (shm_format_len < MAX_FORMATS)
		shm_formats[shm_format_len++] = format;
}
static void session_dmabuf_device(void *data,
		struct ext_image_copy_capture_session_v1 *s, struct wl_array *dev) {
	(void)data; (void)s; (void)dev;
}
static void session_dmabuf_format(void *data,
		struct ext_image_copy_capture_session_v1 *s, uint32_t format,
		struct wl_array *modifiers) {
	(void)data; (void)s; (void)modifiers;
	if (dmabuf_format_len < MAX_FORMATS)
		dmabuf_formats[dmabuf_format_len++] = format;
}
static void session_done(void *data,
		struct ext_image_copy_capture_session_v1 *s) {
	(void)data; (void)s;
	offer_done = true;
}
static void session_stopped_cb(void *data,
		struct ext_image_copy_capture_session_v1 *s) {
	(void)data; (void)s;
	session_stopped = true;
}
static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_buffer_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done,
	.stopped = session_stopped_cb,
};

/* ── one frame ──────────────────────────────────────────────────────────── */

static void frame_transform_cb(void *data,
		struct ext_image_copy_capture_frame_v1 *f, uint32_t transform) {
	(void)data; (void)f;
	frame_transform = transform;
}
static void frame_damage_cb(void *data,
		struct ext_image_copy_capture_frame_v1 *f,
		int32_t x, int32_t y, int32_t w, int32_t h) {
	(void)data; (void)f; (void)x; (void)y; (void)w; (void)h;
}
static void frame_presentation_time(void *data,
		struct ext_image_copy_capture_frame_v1 *f,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	(void)data; (void)f;
	uint64_t sec = ((uint64_t)tv_sec_hi << 32) | tv_sec_lo;
	frame_pts_ns = sec * 1000000000ull + tv_nsec;
	frame_pts_seen = true;
}
static void frame_ready_cb(void *data,
		struct ext_image_copy_capture_frame_v1 *f) {
	(void)data; (void)f;
	frame_ready = true;
}
static void frame_failed_cb(void *data,
		struct ext_image_copy_capture_frame_v1 *f, uint32_t reason) {
	(void)data; (void)f;
	frame_failed = true;
	frame_fail_reason = reason;
}
static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform_cb,
	.damage = frame_damage_cb,
	.presentation_time = frame_presentation_time,
	.ready = frame_ready_cb,
	.failed = frame_failed_cb,
};

/* ── shm buffer ─────────────────────────────────────────────────────────── */

static int anon_fd(size_t size) {
	int fd = memfd_create("wlcapture", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv) {
	const char *want_output = NULL;
	const char *outdir = NULL;
	long frames = 30;
	int frame_timeout_ms = 2000;
	bool cursor = false, quiet = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--output") && i + 1 < argc)
			want_output = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--out") && i + 1 < argc)
			outdir = argv[++i];
		else if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
			frame_timeout_ms = (int)strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--cursor"))
			cursor = true;
		else if (!strcmp(argv[i], "--quiet"))
			quiet = true;
		else {
			fprintf(stderr, "usage: %s [--output NAME] [--frames N] "
				"[--out DIR] [--timeout MS] [--cursor] [--quiet]\n", argv[0]);
			return 2;
		}
	}

	wl_list_init(&outputs);
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "wlcapture: cannot connect to a Wayland display\n");
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(dpy); /* globals */
	wl_display_roundtrip(dpy); /* wl_output names */

	if (!shm || !source_mgr || !capture_mgr) {
		fprintf(stderr, "wlcapture: compositor does not offer %s\n",
			!capture_mgr ? "ext_image_copy_capture_manager_v1"
			: !source_mgr ? "ext_output_image_capture_source_manager_v1"
			: "wl_shm");
		return 2;
	}

	struct output_entry *chosen = NULL, *e;
	wl_list_for_each(e, &outputs, link) {
		if (!want_output || (e->name && !strcmp(e->name, want_output))) {
			chosen = e;
			break;
		}
	}
	if (!chosen) {
		fprintf(stderr, "wlcapture: no output %s\n",
			want_output ? want_output : "at all");
		return 2;
	}

	struct ext_image_capture_source_v1 *source =
		ext_output_image_capture_source_manager_v1_create_source(
			source_mgr, chosen->output);
	struct ext_image_copy_capture_session_v1 *session =
		ext_image_copy_capture_manager_v1_create_session(capture_mgr, source,
			cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS
			       : 0);
	ext_image_copy_capture_session_v1_add_listener(session, &session_listener,
		NULL);

	/* The offer arrives as a burst terminated by `done`. Nothing may be
	 * assumed about it before that -- a session that has sent buffer_size and
	 * two formats is not finished sending formats. */
	if (!wait_flags(dpy, &offer_done, &session_stopped, frame_timeout_ms)) {
		fprintf(stderr, "wlcapture: no offer within %dms\n", frame_timeout_ms);
		return 2;
	}
	if (session_stopped) {
		fprintf(stderr, "wlcapture: session stopped before offering\n");
		return 1;
	}

	printf("output      %s\n", chosen->name ? chosen->name : "(unnamed)");
	printf("buffer      %ux%u\n", buf_width, buf_height);
	printf("shm formats %zu\n", shm_format_len);
	int best_bpc = 0;
	for (size_t i = 0; i < shm_format_len; i++) {
		uint32_t cc = shm_to_fourcc(shm_formats[i]);
		const char *n = fourcc_name(cc);
		int bpc = fourcc_bpc(cc);
		if (bpc > best_bpc)
			best_bpc = bpc;
		printf("  shm       %-14s %u bpc%s\n", n ? n : "(unknown)", bpc,
			n ? "" : " -- fourcc not in this tool's table");
	}
	printf("dmabuf formats %zu\n", dmabuf_format_len);
	for (size_t i = 0; i < dmabuf_format_len; i++) {
		const char *n = fourcc_name(dmabuf_formats[i]);
		printf("  dmabuf    %-14s %u bpc\n", n ? n : "(unknown)",
			fourcc_bpc(dmabuf_formats[i]));
	}
	/* The headline. An 8-bit-only shm offer is the whole of "portal capture
	 * forces the output down to SDR", stated by the compositor itself. */
	printf("shm depth   %d bpc max\n", best_bpc);

	if (frames <= 0) {
		ext_image_copy_capture_session_v1_destroy(session);
		ext_image_capture_source_v1_destroy(source);
		wl_display_disconnect(dpy);
		return 0;
	}
	if (shm_format_len == 0) {
		fprintf(stderr, "wlcapture: no shm format offered; "
			"dmabuf capture is not implemented here\n");
		return 1;
	}

	/* Prefer the deepest format offered: the point is to find out whether
	 * anything beyond 8 bpc survives the round trip. */
	uint32_t pick = shm_formats[0];
	for (size_t i = 0; i < shm_format_len; i++) {
		if (fourcc_bpc(shm_to_fourcc(shm_formats[i]))
				> fourcc_bpc(shm_to_fourcc(pick)))
			pick = shm_formats[i];
	}
	uint32_t pick_cc = shm_to_fourcc(pick);
	/* Every format this tool knows is 4 bytes per pixel except RGB565. */
	uint32_t bpp = (pick_cc == 0x36314752) ? 2 : 4;
	size_t stride = (size_t)buf_width * bpp;
	size_t size = stride * buf_height;

	int fd = anon_fd(size);
	if (fd < 0) {
		fprintf(stderr, "wlcapture: memfd: %s\n", strerror(errno));
		return 1;
	}
	void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "wlcapture: mmap: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
		(int32_t)buf_width, (int32_t)buf_height, (int32_t)stride, pick);
	wl_shm_pool_destroy(pool);
	close(fd);

	printf("capturing   %ld frames into %s\n", frames,
		fourcc_name(pick_cc) ? fourcc_name(pick_cc) : "(unknown)");

	int64_t first_ready = 0, prev_ready = 0;
	int64_t worst_gap = 0;
	long ok = 0;
	int rc = 0;

	for (long i = 0; i < frames; i++) {
		frame_ready = frame_failed = frame_pts_seen = false;
		frame_fail_reason = 0;

		struct ext_image_copy_capture_frame_v1 *frame =
			ext_image_copy_capture_session_v1_create_frame(session);
		ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener,
			NULL);
		ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
		ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0,
			(int32_t)buf_width, (int32_t)buf_height);
		ext_image_copy_capture_frame_v1_capture(frame);

		int64_t asked = now_ns();
		bool got_event = wait_flags(dpy, &frame_ready, &frame_failed,
			frame_timeout_ms);
		int64_t got = now_ns();
		if (!got_event) {
			fprintf(stderr, "wlcapture: no frame within %dms at frame %ld -- "
				"the output presented nothing to copy\n",
				frame_timeout_ms, i);
			ext_image_copy_capture_frame_v1_destroy(frame);
			rc = 1;
			goto done;
		}

		if (frame_failed) {
			fprintf(stderr, "wlcapture: frame %ld failed: %s\n", i,
				failure_name(frame_fail_reason));
			ext_image_copy_capture_frame_v1_destroy(frame);
			rc = 1;
			goto done;
		}
		if (session_stopped) {
			fprintf(stderr, "wlcapture: session stopped at frame %ld\n", i);
			ext_image_copy_capture_frame_v1_destroy(frame);
			rc = 1;
			goto done;
		}

		if (!first_ready)
			first_ready = got;
		int64_t gap = prev_ready ? got - prev_ready : 0;
		if (gap > worst_gap)
			worst_gap = gap;
		prev_ready = got;
		ok++;

		if (!quiet) {
			printf("frame %-4ld wait %6.2fms  gap %6.2fms  transform %-11s "
				"pts %s\n", i, (double)(got - asked) / 1e6,
				(double)gap / 1e6, transform_name(frame_transform),
				frame_pts_seen ? "yes" : "MISSING");
		}

		if (outdir) {
			char path[512];
			snprintf(path, sizeof(path), "%s/frame_%06ld.raw", outdir, i);
			FILE *f = fopen(path, "wb");
			if (f) {
				fwrite(pixels, size, 1, f);
				fclose(f);
			} else {
				fprintf(stderr, "wlcapture: cannot write %s: %s\n", path,
					strerror(errno));
				rc = 1;
			}
		}
		ext_image_copy_capture_frame_v1_destroy(frame);
	}

done:
	if (outdir && ok > 0) {
		char path[512];
		snprintf(path, sizeof(path), "%s/capture.txt", outdir);
		FILE *f = fopen(path, "w");
		if (f) {
			fprintf(f, "output %s\nwidth %u\nheight %u\nstride %zu\n"
				"format %s\nfourcc 0x%08x\nbpc %d\ntransform %s\nframes %ld\n",
				chosen->name ? chosen->name : "(unnamed)", buf_width,
				buf_height, stride,
				fourcc_name(pick_cc) ? fourcc_name(pick_cc) : "unknown",
				pick_cc, fourcc_bpc(pick_cc), transform_name(frame_transform),
				ok);
			fclose(f);
		}
	}

	if (ok > 1) {
		double span_s = (double)(prev_ready - first_ready) / 1e9;
		printf("captured    %ld frames in %.2fs = %.1f fps "
			"(worst gap %.2fms)\n", ok, span_s,
			span_s > 0 ? (double)(ok - 1) / span_s : 0.0,
			(double)worst_gap / 1e6);
	} else {
		printf("captured    %ld frames\n", ok);
	}

	wl_buffer_destroy(buffer);
	munmap(pixels, size);
	ext_image_copy_capture_session_v1_destroy(session);
	ext_image_capture_source_v1_destroy(source);
	wl_display_disconnect(dpy);
	return rc;
}
