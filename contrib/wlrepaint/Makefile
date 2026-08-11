# wlrepaint — a wl_shm client that repaints its WHOLE surface every generation.
#
# See wlrepaint.c: a persistence test is only as good as the change it tests
# against, and neither a settled terminal nor wlrotate's 24x24 marks change
# enough of the screen to make a stale region detectable. This one alternates
# every pixel between two four-colour checkers whose parity is recoverable from
# a single screenshot.
#
# xdg-decoration is linked in for --ssd. A client that never binds it is CSD by
# default (src/fetch/client.h: client_wants_ssd), so it gets NO server border --
# and a border test run against such a client measures an empty ring and blames
# the renderer.
CFLAGS ?= -O2 -Wall -Wextra
PROTOS := xdg-shell xdg-decoration-unstable-v1
HEADERS := $(PROTOS:=-client-protocol.h)
SOURCES := $(PROTOS:=-protocol.c)

all: wlrepaint

%-client-protocol.h: %.xml
	wayland-scanner client-header $< $@

%-protocol.c: %.xml
	wayland-scanner private-code $< $@

wlrepaint: wlrepaint.c $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ wlrepaint.c $(SOURCES) $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlrepaint $(SOURCES) $(HEADERS)

.PHONY: all clean
