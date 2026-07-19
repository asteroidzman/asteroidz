# wllayer — tiny wlr-layer-shell-unstable-v1 client for headless testing
# (see wllayer.c for what this exercises that kitty/wlvptr/wlvkbd can't).
#
# wlr-layer-shell-unstable-v1.xml's get_popup request types its arg as
# xdg_popup, so the generated interface table references xdg_popup_interface
# as extern data even though wllayer never calls get_popup -- needs
# xdg-shell's generated code linked in too, just to satisfy that symbol.
CFLAGS ?= -O2 -Wall -Wextra
PROTO := wlr-layer-shell-unstable-v1
XDG := xdg-shell

all: wllayer

$(PROTO)-client-protocol.h: $(PROTO).xml
	wayland-scanner client-header $< $@

$(PROTO)-protocol.c: $(PROTO).xml
	wayland-scanner private-code $< $@

$(XDG)-client-protocol.h: $(XDG).xml
	wayland-scanner client-header $< $@

$(XDG)-protocol.c: $(XDG).xml
	wayland-scanner private-code $< $@

wllayer: wllayer.c $(PROTO)-protocol.c $(PROTO)-client-protocol.h $(XDG)-protocol.c $(XDG)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wllayer.c $(PROTO)-protocol.c $(XDG)-protocol.c $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wllayer $(PROTO)-protocol.c $(PROTO)-client-protocol.h $(XDG)-protocol.c $(XDG)-client-protocol.h

.PHONY: all clean
