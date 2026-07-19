# wlvkbd — tiny zwp_virtual_keyboard_unstable_v1 client for headless testing
# (see wlvkbd.c for why this exists; pairs with contrib/wlvptr).
CFLAGS ?= -O2 -Wall -Wextra
PROTO := virtual-keyboard-unstable-v1

all: wlvkbd

$(PROTO)-client-protocol.h: $(PROTO).xml
	wayland-scanner client-header $< $@

$(PROTO)-protocol.c: $(PROTO).xml
	wayland-scanner private-code $< $@

wlvkbd: wlvkbd.c $(PROTO)-protocol.c $(PROTO)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlvkbd.c $(PROTO)-protocol.c $$(pkg-config --cflags --libs wayland-client xkbcommon)

clean:
	rm -f wlvkbd $(PROTO)-protocol.c $(PROTO)-client-protocol.h

.PHONY: all clean
