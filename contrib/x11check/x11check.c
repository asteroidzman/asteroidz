/* x11check -- a raw XCB client whose whole job is to make scaling VISIBLE.
 *
 * ── WHY NOTHING ELSE IN contrib/ CAN DO THIS ─────────────────────────────
 *
 * Every other helper here is a Wayland client. A Wayland client cannot
 * exercise the XWayland path at all, and more importantly it negotiates its
 * own buffer scale with the compositor -- which is exactly the mechanism X11
 * clients do not have. The question "is this window's buffer reaching the
 * screen 1:1, or is the renderer magnifying it?" is only interesting for a
 * client that has no say in the matter.
 *
 * ── THE PATTERN IS THE INSTRUMENT ────────────────────────────────────────
 *
 * The window is filled with a ONE-PIXEL checkerboard: pixel (x,y) is white
 * when (x+y) is even and black when it is odd. That pattern is the highest
 * spatial frequency a raster can carry, which is what makes it a scaling
 * detector rather than a picture:
 *
 *   presented 1:1      every horizontal run of equal colour is exactly 1 px
 *                      long, and only two colours appear.
 *   upscaled, bilinear intermediate greys appear -- a magnified checkerboard
 *                      is a blur, which is the user-visible symptom.
 *   upscaled, nearest  no greys, but runs of length 2 appear where a source
 *                      column got duplicated (at 1.25x, one column in four).
 *
 * So a single measurement -- "are all runs length 1 and are there only two
 * colours" -- separates native presentation from BOTH kinds of upscaling.
 * A flat or low-frequency fill would be blind to both: magnifying a solid
 * colour produces the same solid colour, and a fixture built on one would
 * pass whatever the renderer did. (Same lesson as the shadow-blur glow: a
 * flat backdrop test is blind to the thing it was written for.)
 *
 * ── WHAT IT REPORTS ──────────────────────────────────────────────────────
 *
 * One line per event on stdout, line-buffered so a fixture can poll the log:
 *
 *   configure W H X Y   the size the COMPOSITOR asked for, in X11's own
 *                       units -- raw pixels. This is the cheapest possible
 *                       oracle for the configure-out boundary: it needs no
 *                       screenshot and no renderer at all.
 *   button X Y B        a press, in window-local X11 coordinates. The input
 *                       oracle: a click aimed at the middle of the window on
 *                       screen must arrive at the middle of the window here,
 *                       and a missing input transform moves it by the scale
 *                       factor.
 *   motion X Y          pointer motion, same coordinates.
 *   mapped W H          the window is up and painted at least once.
 *
 * Usage: x11check TITLE [HOLD_SECONDS] [WIDTH HEIGHT]
 */

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xcb/xcb.h>

static xcb_connection_t *conn;
static xcb_window_t win;
static xcb_gcontext_t gc;
static xcb_screen_t *screen;
static uint8_t depth;
static int win_w = 400, win_h = 300;

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Paint the checkerboard.
 *
 * xcb_put_image carries the pixels inside the request, so the whole image
 * cannot go in one call -- the server's maximum request length is finite and,
 * without BIG-REQUESTS, small. Sending it in row BANDS is not an optimisation
 * but a correctness requirement: an over-long request is dropped and the
 * window is left showing whatever the server had, which for a fixture means a
 * black rectangle that measures as "no scaling" for the wrong reason.
 *
 * Depth 24 is stored 32 bits per pixel with a 32-bit scanline pad on every X
 * server this runs on, so the stride is simply w*4 and no padding arithmetic
 * is needed. That assumption is asserted rather than assumed: a server that
 * disagreed would make every measurement below meaningless, and a wrong
 * picture that still measures is the failure mode this whole file exists to
 * avoid.
 */
static void paint(void) {
	if (win_w <= 0 || win_h <= 0) {
		return;
	}

	/* BREAK: X11CHECK_BREAK_NOPAINT=1 leaves the window at its background
	 * pixel instead of painting the checkerboard.
	 *
	 * It exists so a fixture's pixel gate can be shown to be about the
	 * CONTENT and not merely about a window being present. A black window
	 * has no greys, so a gate that only counted those would call it native
	 * and pass; this one also counts equal neighbours, of which a flat fill
	 * is entirely made, so it must go red. If it does not, the gate is
	 * measuring something other than what it claims. */
	if (getenv("X11CHECK_BREAK_NOPAINT")) {
		return;
	}

	const size_t stride = (size_t)win_w * 4;
	/* Cap a band at ~256 KiB of pixels, and at whatever the server will
	 * actually accept, whichever is smaller. */
	uint32_t max_units = xcb_get_maximum_request_length(conn);
	size_t max_bytes = (size_t)max_units * 4;
	if (max_bytes > 256 * 1024) {
		max_bytes = 256 * 1024;
	}
	/* Leave room for the request header itself. */
	if (max_bytes > 1024) {
		max_bytes -= 1024;
	}
	int band = (int)(max_bytes / stride);
	if (band < 1) {
		band = 1;
	}

	uint8_t *buf = malloc(stride * (size_t)band);
	if (!buf) {
		return;
	}

	for (int y0 = 0; y0 < win_h; y0 += band) {
		int rows = win_h - y0 < band ? win_h - y0 : band;
		for (int r = 0; r < rows; r++) {
			uint32_t *row = (uint32_t *)(buf + stride * (size_t)r);
			int y = y0 + r;
			for (int x = 0; x < win_w; x++) {
				/* WINDOW-LOCAL parity, not band-local: the phase has to be
				 * continuous across bands or the seam between two bands looks
				 * exactly like a duplicated column. */
				row[x] = ((x + y) & 1) ? 0x00000000u : 0x00ffffffu;
			}
		}
		xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, win, gc, (uint16_t)win_w,
					  (uint16_t)rows, 0, (int16_t)y0, 0, depth,
					  (uint32_t)(stride * (size_t)rows), buf);
	}

	free(buf);
	xcb_flush(conn);
}

int main(int argc, char **argv) {
	const char *title = argc > 1 ? argv[1] : "x11check";
	double hold = argc > 2 ? atof(argv[2]) : 30.0;
	if (argc > 4) {
		win_w = atoi(argv[3]);
		win_h = atoi(argv[4]);
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	int scr_n = 0;
	conn = xcb_connect(NULL, &scr_n);
	if (!conn || xcb_connection_has_error(conn)) {
		fprintf(stderr, "x11check: cannot connect to DISPLAY=%s\n",
				getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
		return 1;
	}
	const xcb_setup_t *setup = xcb_get_setup(conn);
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < scr_n; i++) {
		xcb_screen_next(&it);
	}
	screen = it.data;
	depth = screen->root_depth;

	/* The stride assumption above, checked against what the server says. */
	bool ok_format = false;
	xcb_format_iterator_t fit = xcb_setup_pixmap_formats_iterator(setup);
	for (; fit.rem; xcb_format_next(&fit)) {
		if (fit.data->depth == depth) {
			ok_format = fit.data->bits_per_pixel == 32 &&
						fit.data->scanline_pad == 32;
			break;
		}
	}
	if (!ok_format) {
		fprintf(stderr,
				"x11check: server stores depth %u at something other than 32 "
				"bpp / 32-bit pad; the checkerboard would be misaligned and "
				"every measurement made from it would be wrong\n",
				depth);
		return 1;
	}

	/* The size of the X screen as this client sees it. Reported, never
	 * asserted on here: it is what an X11 app that measures the display for
	 * itself reads, and it comes from Xwayland's own view of the wl_outputs
	 * rather than from anything the compositor configures directly. It used to
	 * be the logical desktop, which is what made a window sized in device
	 * pixels overflow the screen it lived on; it is the device-pixel desktop
	 * now. The 1.25-screen and 1.25-screen-clamped arms of
	 * contrib/xw-scale-test.sh assert both, off this line. */
	printf("screen %d %d\n", screen->width_in_pixels,
		   screen->height_in_pixels);

	win = xcb_generate_id(conn);
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	uint32_t vals[2] = {
		screen->black_pixel,
		XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
			XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
			XCB_EVENT_MASK_POINTER_MOTION,
	};
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 0, 0,
					  (uint16_t)win_w, (uint16_t)win_h, 0,
					  XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask,
					  vals);

	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
						XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
	/* WM_CLASS is "instance\0class\0". Both are "x11check" so a window rule
	 * can match either half. */
	char cls[64];
	int cls_n = snprintf(cls, sizeof(cls), "x11check") + 1;
	cls_n += snprintf(cls + cls_n, sizeof(cls) - (size_t)cls_n, "x11check") + 1;
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS,
						XCB_ATOM_STRING, 8, (uint32_t)cls_n, cls);

	gc = xcb_generate_id(conn);
	uint32_t gcvals[2] = {screen->white_pixel, screen->black_pixel};
	xcb_create_gc(conn, gc, win, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gcvals);

	xcb_map_window(conn, win);
	xcb_flush(conn);

	double deadline = now_s() + hold;
	bool announced = false;
	int fd = xcb_get_file_descriptor(conn);

	while (now_s() < deadline) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		double left = deadline - now_s();
		int timeout = left > 0.2 ? 200 : 1;
		if (poll(&pfd, 1, timeout) < 0) {
			break;
		}

		xcb_generic_event_t *ev;
		while ((ev = xcb_poll_for_event(conn))) {
			switch (ev->response_type & ~0x80) {
			case XCB_EXPOSE:
				paint();
				if (!announced) {
					printf("mapped %d %d\n", win_w, win_h);
					announced = true;
				}
				break;
			case XCB_CONFIGURE_NOTIFY: {
				xcb_configure_notify_event_t *cn =
					(xcb_configure_notify_event_t *)ev;
				if (cn->window != win) {
					break;
				}
				bool resized = cn->width != win_w || cn->height != win_h;
				win_w = cn->width;
				win_h = cn->height;
				printf("configure %d %d %d %d\n", win_w, win_h, cn->x, cn->y);
				if (resized) {
					paint();
				}
				break;
			}
			case XCB_BUTTON_PRESS: {
				xcb_button_press_event_t *b = (xcb_button_press_event_t *)ev;
				printf("button %d %d %u\n", b->event_x, b->event_y, b->detail);
				break;
			}
			case XCB_MOTION_NOTIFY: {
				xcb_motion_notify_event_t *m = (xcb_motion_notify_event_t *)ev;
				printf("motion %d %d\n", m->event_x, m->event_y);
				break;
			}
			default:
				break;
			}
			free(ev);
		}
		if (xcb_connection_has_error(conn)) {
			break;
		}
	}

	xcb_disconnect(conn);
	return 0;
}
