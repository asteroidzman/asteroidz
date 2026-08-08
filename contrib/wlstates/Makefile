# wlstates — xdg-shell client that reports its configured state array
# (see wlstates.c for the class of bug this exists to make assertable).
CFLAGS ?= -O2 -Wall -Wextra
XDG := xdg-shell

all: wlstates

$(XDG)-client-protocol.h: $(XDG).xml
	wayland-scanner client-header $< $@

$(XDG)-protocol.c: $(XDG).xml
	wayland-scanner private-code $< $@

wlstates: wlstates.c $(XDG)-protocol.c $(XDG)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlstates.c $(XDG)-protocol.c $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlstates $(XDG)-protocol.c $(XDG)-client-protocol.h

.PHONY: all clean
