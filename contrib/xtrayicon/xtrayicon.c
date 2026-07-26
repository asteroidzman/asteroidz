// xtrayicon — a minimal XEmbed tray icon client, for testing asteroidz-xembed.
//
// Every real XEmbed tray client is a whole application (Discord, Steam), which
// makes the bridge awkward to test: you cannot ask Discord to dock on demand
// inside a nested compositor. This does exactly the one thing the protocol
// asks of a client and nothing else -- create a window, find the owner of
// _NET_SYSTEM_TRAY_S<screen>, ask it to dock, then paint.
//
// It paints a SOLID KNOWN COLOUR so a test can assert on the pixels that come
// out the other end: the bridge captures the window and re-exports it as a
// StatusNotifierItem pixmap, and a test that only checks "an item appeared"
// would pass even if the capture produced garbage.
//
// Usage: xtrayicon [rrggbb] [seconds]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

#define SYSTEM_TRAY_REQUEST_DOCK 0

static xcb_atom_t intern(xcb_connection_t *c, const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
		c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
	free(r);
	return a;
}

int main(int argc, char **argv) {
	uint32_t colour = 0x00cc44; // default: a green nothing else in the bar uses
	if (argc > 1)
		colour = (uint32_t)strtoul(argv[1], NULL, 16);
	int seconds = argc > 2 ? atoi(argv[2]) : 30;

	int screen_num = 0;
	xcb_connection_t *c = xcb_connect(NULL, &screen_num);
	if (!c || xcb_connection_has_error(c)) {
		fprintf(stderr, "xtrayicon: cannot connect to X\n");
		return 1;
	}
	const xcb_setup_t *setup = xcb_get_setup(c);
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < screen_num; i++)
		xcb_screen_next(&it);
	xcb_screen_t *screen = it.data;

	char sel_name[64];
	snprintf(sel_name, sizeof(sel_name), "_NET_SYSTEM_TRAY_S%d", screen_num);
	xcb_atom_t sel = intern(c, sel_name);
	xcb_atom_t opcode = intern(c, "_NET_SYSTEM_TRAY_OPCODE");
	xcb_atom_t xembed_info = intern(c, "_XEMBED_INFO");

	xcb_get_selection_owner_reply_t *owner =
		xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, sel), NULL);
	xcb_window_t tray = owner ? owner->owner : XCB_NONE;
	free(owner);
	if (tray == XCB_NONE) {
		fprintf(stderr, "xtrayicon: no %s owner -- no tray is running\n",
				sel_name);
		return 2;
	}

	xcb_window_t win = xcb_generate_id(c);
	uint32_t values[] = {colour, XCB_EVENT_MASK_EXPOSURE |
									XCB_EVENT_MASK_STRUCTURE_NOTIFY};
	xcb_create_window(c, XCB_COPY_FROM_PARENT, win, screen->root, 0, 0, 24, 24,
					  0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
					  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);

	// _XEMBED_INFO is mandatory: a tray that finds it missing is entitled to
	// refuse the dock, and some do.
	uint32_t info[2] = {0 /* version */, 1 /* XEMBED_MAPPED */};
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, xembed_info, xembed_info,
						32, 2, info);

	xcb_client_message_event_t ev = {0};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = tray;
	ev.type = opcode;
	ev.data.data32[0] = XCB_CURRENT_TIME;
	ev.data.data32[1] = SYSTEM_TRAY_REQUEST_DOCK;
	ev.data.data32[2] = win;
	xcb_send_event(c, 0, tray, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
	xcb_flush(c);

	printf("xtrayicon: asked tray 0x%x to dock window 0x%x (colour %06x)\n",
		   tray, win, colour);
	fflush(stdout);

	// Repaint on expose; the bridge captures on damage, so the window must
	// actually produce content rather than just exist.
	xcb_gcontext_t gc = xcb_generate_id(c);
	uint32_t gcv[] = {colour};
	xcb_create_gc(c, gc, win, XCB_GC_FOREGROUND, gcv);

	for (int elapsed = 0; elapsed < seconds; elapsed++) {
		xcb_generic_event_t *e;
		while ((e = xcb_poll_for_event(c))) {
			if ((e->response_type & 0x7f) == XCB_EXPOSE) {
				xcb_rectangle_t r = {0, 0, 24, 24};
				xcb_poly_fill_rectangle(c, win, gc, 1, &r);
				xcb_flush(c);
			}
			free(e);
		}
		// keep drawing: a tray may map us late, and the first expose can
		// arrive before the reparent completes
		xcb_rectangle_t r = {0, 0, 24, 24};
		xcb_poly_fill_rectangle(c, win, gc, 1, &r);
		xcb_flush(c);
		sleep(1);
	}
	xcb_disconnect(c);
	return 0;
}
