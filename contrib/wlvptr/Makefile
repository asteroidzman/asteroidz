# wlvptr — tiny wlr-virtual-pointer-unstable-v1 client for headless testing
# (see wlvptr.c for why this exists instead of ydotool).
CFLAGS ?= -O2 -Wall -Wextra
PROTO := wlr-virtual-pointer-unstable-v1

all: wlvptr

$(PROTO)-client-protocol.h: $(PROTO).xml
	wayland-scanner client-header $< $@

$(PROTO)-protocol.c: $(PROTO).xml
	wayland-scanner private-code $< $@

wlvptr: wlvptr.c $(PROTO)-protocol.c $(PROTO)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlvptr.c $(PROTO)-protocol.c $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlvptr $(PROTO)-protocol.c $(PROTO)-client-protocol.h

.PHONY: all clean
