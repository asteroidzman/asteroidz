/*
 * wlshot — a screencopy client that can capture the CURSOR.
 *
 * WHY grim IS NOT ENOUGH
 *
 * grim asks wlr-screencopy for a frame with overlay_cursor = 0, so what it
 * writes out is the composited scene without any cursor at all. Every cursor
 * test in this suite would therefore pass against a compositor that draws no
 * cursor whatsoever, which is precisely the bug M3.5E exists to fix.
 *
 * WHY THIS MATTERS MORE ON A HEADLESS OUTPUT
 *
 * The headless backend implements set_cursor() as `return true;` and does
 * nothing (backend/headless/output.c:82). So a headless compositor believes it
 * has a working hardware cursor plane, puts every cursor on it, and the cursor
 * is never composited into the output buffer — it goes to a plane that does
 * not exist. A headless screenshot is structurally blind to cursors.
 *
 * overlay_cursor is what breaks that open, and the way it does it is exactly
 * the mechanism this milestone needs: wlr_screencopy_v1.c:443 calls
 *
 *     wlr_output_lock_software_cursors(output, true)
 *
 * which disables the hardware cursor for the duration of the capture and
 * forces the cursor to be composited into the frame. So asking for a cursor in
 * the capture IS the request to exercise the software cursor path — no debug
 * environment variable, no test-only branch in the compositor, and the same
 * code path a real screen recorder takes.
 *
 * Usage:
 *     wlshot [--output NAME] [--cursor] [--no-cursor] OUT.png
 *
 * Writes a PNG. Defaults to the first output and to NOT including the cursor,
 * so it behaves like grim unless asked otherwise.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

static struct wl_shm *shm;
static struct zwlr_screencopy_manager_v1 *screencopy;

struct output_entry {
	struct wl_output *output;
	char *name;
	struct output_entry *next;
};
static struct output_entry *outputs;

static const char *want_output;
static bool want_cursor;

static struct {
	uint32_t format, width, height, stride;
	bool have_buffer_info;
	bool done, failed;
	void *data;
	size_t size;
	struct wl_buffer *buffer;
} frame;

static void output_name(void *data, struct wl_output *o, const char *name) {
	(void)o;
	struct output_entry *entry = data;
	free(entry->name);
	entry->name = strdup(name);
}
static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t sub, const char *make,
		const char *model, int32_t transform) {
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
	(void)make; (void)model; (void)transform;
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
			zwlr_screencopy_manager_v1_interface.name) == 0) {
		screencopy = wl_registry_bind(registry, name,
			&zwlr_screencopy_manager_v1_interface, version < 3 ? version : 3);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output_entry *entry = calloc(1, sizeof(*entry));
		if (entry == NULL) {
			return;
		}
		entry->output = wl_registry_bind(registry, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_output_add_listener(entry->output, &output_listener, entry);
		entry->next = outputs;
		outputs = entry;
	}
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) {
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static int anon_file(size_t size) {
	int fd = memfd_create("wlshot", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	return fd;
}

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *f,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	(void)data;
	frame.format = format;
	frame.width = width;
	frame.height = height;
	frame.stride = stride;
	frame.have_buffer_info = true;

	frame.size = (size_t)stride * height;
	int fd = anon_file(frame.size);
	if (fd < 0) {
		frame.failed = true;
		return;
	}
	frame.data = mmap(NULL, frame.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		fd, 0);
	if (frame.data == MAP_FAILED) {
		close(fd);
		frame.failed = true;
		return;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)frame.size);
	frame.buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width,
		(int32_t)height, (int32_t)stride, format);
	wl_shm_pool_destroy(pool);
	close(fd);

	zwlr_screencopy_frame_v1_copy(f, frame.buffer);
}
static void frame_flags(void *d, struct zwlr_screencopy_frame_v1 *f,
		uint32_t flags) {
	(void)d; (void)f; (void)flags;
}
static void frame_ready(void *d, struct zwlr_screencopy_frame_v1 *f,
		uint32_t hi, uint32_t lo, uint32_t nsec) {
	(void)d; (void)f; (void)hi; (void)lo; (void)nsec;
	frame.done = true;
}
static void frame_failed(void *d, struct zwlr_screencopy_frame_v1 *f) {
	(void)d; (void)f;
	frame.failed = true;
	frame.done = true;
}
static void frame_damage(void *d, struct zwlr_screencopy_frame_v1 *f,
		uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	(void)d; (void)f; (void)x; (void)y; (void)w; (void)h;
}
static void frame_linux_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f,
		uint32_t format, uint32_t width, uint32_t height) {
	(void)d; (void)f; (void)format; (void)width; (void)height;
}
static void frame_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) {
	(void)d; (void)f;
}
static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
	.damage = frame_damage,
	.linux_dmabuf = frame_linux_dmabuf,
	.buffer_done = frame_buffer_done,
};

/*
 * A minimal PNG writer.
 *
 * Deliberately no libpng dependency: this runs in the headless harness, and
 * one uncompressed-deflate encoder is a smaller price than another build
 * dependency for every contributor. "Stored" deflate blocks make the file
 * large and the code short, which is the right trade for a test artefact.
 */
static uint32_t crc_table[256];
static void crc_init(void) {
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;
		for (int k = 0; k < 8; k++) {
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		}
		crc_table[n] = c;
	}
}
static uint32_t crc_update(uint32_t crc, const uint8_t *buf, size_t len) {
	for (size_t i = 0; i < len; i++) {
		crc = crc_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
	}
	return crc;
}
static void put_be32(uint8_t *p, uint32_t v) {
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void write_chunk(FILE *f, const char *type, const uint8_t *data,
		size_t len) {
	uint8_t hdr[4];
	put_be32(hdr, (uint32_t)len);
	fwrite(hdr, 1, 4, f);
	fwrite(type, 1, 4, f);
	if (len > 0) {
		fwrite(data, 1, len, f);
	}
	uint32_t crc = crc_update(0xffffffffu, (const uint8_t *)type, 4);
	if (len > 0) {
		crc = crc_update(crc, data, len);
	}
	put_be32(hdr, crc ^ 0xffffffffu);
	fwrite(hdr, 1, 4, f);
}

static bool write_png(const char *path, const uint8_t *rgb, uint32_t width,
		uint32_t height) {
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		return false;
	}
	crc_init();
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);

	uint8_t ihdr[13];
	put_be32(ihdr, width);
	put_be32(ihdr + 4, height);
	ihdr[8] = 8;    /* bit depth */
	ihdr[9] = 2;    /* colour type: truecolour */
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	write_chunk(f, "IHDR", ihdr, sizeof(ihdr));

	/* raw = per-row filter byte 0 followed by RGB triples */
	size_t row = (size_t)width * 3 + 1;
	size_t raw_len = row * height;
	uint8_t *raw = malloc(raw_len);
	if (raw == NULL) {
		fclose(f);
		return false;
	}
	for (uint32_t y = 0; y < height; y++) {
		raw[y * row] = 0;
		memcpy(raw + y * row + 1, rgb + (size_t)y * width * 3,
			(size_t)width * 3);
	}

	/* zlib stream: 2-byte header, stored deflate blocks, adler32 */
	size_t max_block = 65535;
	size_t blocks = (raw_len + max_block - 1) / max_block;
	size_t z_len = 2 + blocks * 5 + raw_len + 4;
	uint8_t *z = malloc(z_len);
	if (z == NULL) {
		free(raw);
		fclose(f);
		return false;
	}
	size_t zi = 0;
	z[zi++] = 0x78; z[zi++] = 0x01;
	size_t off = 0;
	while (off < raw_len) {
		size_t n = raw_len - off < max_block ? raw_len - off : max_block;
		z[zi++] = (off + n >= raw_len) ? 1 : 0;
		z[zi++] = n & 0xff;
		z[zi++] = (n >> 8) & 0xff;
		z[zi++] = (~n) & 0xff;
		z[zi++] = ((~n) >> 8) & 0xff;
		memcpy(z + zi, raw + off, n);
		zi += n;
		off += n;
	}
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < raw_len; i++) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	put_be32(z + zi, (b << 16) | a);
	zi += 4;

	write_chunk(f, "IDAT", z, zi);
	write_chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(z);
	free(raw);
	return true;
}

int main(int argc, char **argv) {
	const char *out_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
			want_output = argv[++i];
		} else if (strcmp(argv[i], "--cursor") == 0) {
			want_cursor = true;
		} else if (strcmp(argv[i], "--no-cursor") == 0) {
			want_cursor = false;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "wlshot: unknown option '%s'\n", argv[i]);
			return 1;
		} else {
			out_path = argv[i];
		}
	}
	if (out_path == NULL) {
		fprintf(stderr, "usage: wlshot [--output NAME] [--cursor] OUT.png\n");
		return 1;
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "wlshot: cannot connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);   /* output names */

	if (shm == NULL || screencopy == NULL) {
		fprintf(stderr, "wlshot: compositor is missing wl_shm or "
			"wlr-screencopy\n");
		return 1;
	}

	struct wl_output *target = NULL;
	for (struct output_entry *e = outputs; e != NULL; e = e->next) {
		if (want_output == NULL ||
				(e->name != NULL && strcmp(e->name, want_output) == 0)) {
			target = e->output;
			break;
		}
	}
	if (target == NULL) {
		fprintf(stderr, "wlshot: no output named '%s'\n",
			want_output ? want_output : "(any)");
		return 1;
	}

	struct zwlr_screencopy_frame_v1 *f =
		zwlr_screencopy_manager_v1_capture_output(screencopy,
			want_cursor ? 1 : 0, target);
	zwlr_screencopy_frame_v1_add_listener(f, &frame_listener, NULL);

	while (!frame.done && wl_display_dispatch(display) != -1) {
		/* wait */
	}
	if (frame.failed || frame.data == NULL) {
		fprintf(stderr, "wlshot: the capture failed\n");
		return 1;
	}

	/* Convert to RGB. Only the two formats wlroots actually hands out for shm
	 * captures are handled; anything else is an error rather than a guess. */
	uint8_t *rgb = malloc((size_t)frame.width * frame.height * 3);
	if (rgb == NULL) {
		return 1;
	}
	bool bgr;
	switch (frame.format) {
	case WL_SHM_FORMAT_XRGB8888:
	case WL_SHM_FORMAT_ARGB8888:
		bgr = true;    /* little-endian: byte order is B,G,R,A */
		break;
	case WL_SHM_FORMAT_XBGR8888:
	case WL_SHM_FORMAT_ABGR8888:
		bgr = false;
		break;
	default:
		fprintf(stderr, "wlshot: unhandled shm format 0x%08x\n", frame.format);
		return 1;
	}
	for (uint32_t y = 0; y < frame.height; y++) {
		const uint8_t *src = (const uint8_t *)frame.data + y * frame.stride;
		uint8_t *dst = rgb + (size_t)y * frame.width * 3;
		for (uint32_t x = 0; x < frame.width; x++) {
			dst[x * 3 + 0] = src[x * 4 + (bgr ? 2 : 0)];
			dst[x * 3 + 1] = src[x * 4 + 1];
			dst[x * 3 + 2] = src[x * 4 + (bgr ? 0 : 2)];
		}
	}

	if (!write_png(out_path, rgb, frame.width, frame.height)) {
		fprintf(stderr, "wlshot: cannot write '%s'\n", out_path);
		return 1;
	}
	free(rgb);
	wl_display_disconnect(display);
	return 0;
}
