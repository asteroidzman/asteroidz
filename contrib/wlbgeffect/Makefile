# wlbgeffect — an xdg-shell client that supplies a real
# ext-background-effect-v1 blur region (see wlbgeffect.c for why nothing else
# in contrib/ can exercise the walker's node-local clip conversion).
CFLAGS ?= -O2 -Wall -Wextra
XDG := xdg-shell
BGE := ext-background-effect-v1
BGE_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/staging/ext-background-effect/$(BGE).xml

all: wlbgeffect

$(XDG)-client-protocol.h: $(XDG).xml
	wayland-scanner client-header $< $@

$(XDG)-protocol.c: $(XDG).xml
	wayland-scanner private-code $< $@

$(BGE)-client-protocol.h:
	wayland-scanner client-header $(BGE_XML) $@

$(BGE)-protocol.c:
	wayland-scanner private-code $(BGE_XML) $@

wlbgeffect: wlbgeffect.c $(XDG)-protocol.c $(XDG)-client-protocol.h \
            $(BGE)-protocol.c $(BGE)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlbgeffect.c $(XDG)-protocol.c $(BGE)-protocol.c \
		$$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlbgeffect $(XDG)-protocol.c $(XDG)-client-protocol.h \
		$(BGE)-protocol.c $(BGE)-client-protocol.h

.PHONY: all clean
