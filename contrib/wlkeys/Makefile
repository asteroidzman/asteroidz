# wlkeys — xdg-shell client that reports keyboard events back out
# (see wlkeys.c for the class of bug this exists to make assertable).
CFLAGS ?= -O2 -Wall -Wextra
XDG := xdg-shell

all: wlkeys

$(XDG)-client-protocol.h: $(XDG).xml
	wayland-scanner client-header $< $@

$(XDG)-protocol.c: $(XDG).xml
	wayland-scanner private-code $< $@

wlkeys: wlkeys.c $(XDG)-protocol.c $(XDG)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlkeys.c $(XDG)-protocol.c $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlkeys $(XDG)-protocol.c $(XDG)-client-protocol.h

.PHONY: all clean
