# wlcursor — a client that sets its own cursor image.
#
# See wlcursor.c: it is the only client in the tree that calls
# wl_pointer.set_cursor, which is the one cursor source that broke under AVK's
# NULL-renderer compositor and the one nothing else could catch.
CFLAGS ?= -O2 -Wall -Wextra
XDG := xdg-shell

all: wlcursor

$(XDG)-client-protocol.h: $(XDG).xml
	wayland-scanner client-header $< $@

$(XDG)-protocol.c: $(XDG).xml
	wayland-scanner private-code $< $@

wlcursor: wlcursor.c $(XDG)-protocol.c $(XDG)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlcursor.c $(XDG)-protocol.c $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlcursor $(XDG)-protocol.c $(XDG)-client-protocol.h

.PHONY: all clean
