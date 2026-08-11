# wlrotate — a wl_shm client that ROTATES a pool of buffers, the way every
# real toolkit does.
#
# See wlrotate.c: wlreuse holds one buffer for its whole life, which makes a
# commit's damage and that buffer's own divergence the same region. Rotate two
# and they are not, and a renderer caching one image per buffer has to
# accumulate damage across the commits it missed. Nothing else in the tree can
# produce that condition.
CFLAGS ?= -O2 -Wall -Wextra
XDG := xdg-shell

all: wlrotate

$(XDG)-client-protocol.h: $(XDG).xml
	wayland-scanner client-header $< $@

$(XDG)-protocol.c: $(XDG).xml
	wayland-scanner private-code $< $@

wlrotate: wlrotate.c $(XDG)-protocol.c $(XDG)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlrotate.c $(XDG)-protocol.c $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlrotate $(XDG)-protocol.c $(XDG)-client-protocol.h

.PHONY: all clean
