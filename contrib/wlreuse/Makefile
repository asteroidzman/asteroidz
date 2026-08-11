# wlreuse — a wl_shm client that reuses ONE buffer for every commit.
#
# See wlreuse.c for why: it is the only client in the tree that makes buffer
# identity a lie, which is what a renderer-side pixel cache keyed on the
# buffer pointer needs in order to be caught.
CFLAGS ?= -O2 -Wall -Wextra
XDG := xdg-shell

all: wlreuse

$(XDG)-client-protocol.h: $(XDG).xml
	wayland-scanner client-header $< $@

$(XDG)-protocol.c: $(XDG).xml
	wayland-scanner private-code $< $@

wlreuse: wlreuse.c $(XDG)-protocol.c $(XDG)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlreuse.c $(XDG)-protocol.c $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlreuse $(XDG)-protocol.c $(XDG)-client-protocol.h

.PHONY: all clean
