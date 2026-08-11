# wlshot — a screencopy client that can capture the cursor.
#
# See wlshot.c: grim always asks for overlay_cursor = 0, so every cursor test
# would pass against a compositor that draws no cursor at all. Asking for the
# cursor also forces software cursors for the duration of the capture, which is
# the mechanism M3.5E's software path is exercised through.
CFLAGS ?= -O2 -Wall -Wextra
PROTO := wlr-screencopy-unstable-v1

all: wlshot

$(PROTO)-client-protocol.h: $(PROTO).xml
	wayland-scanner client-header $< $@

$(PROTO)-protocol.c: $(PROTO).xml
	wayland-scanner private-code $< $@

wlshot: wlshot.c $(PROTO)-protocol.c $(PROTO)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlshot.c $(PROTO)-protocol.c $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlshot $(PROTO)-protocol.c $(PROTO)-client-protocol.h

.PHONY: all clean
