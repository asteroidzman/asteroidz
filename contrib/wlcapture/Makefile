# wlcapture — an ext-image-copy-capture client, for M14.
#
# See wlcapture.c: M14's audit found there is no client in contrib/ that
# consumes capture frames, so nothing could measure what a capture session is
# offered or how fast it delivers. wlshot is wlr-screencopy and captures one
# frame; the format negotiation this needs to read only exists on the newer
# protocol's session.
CFLAGS ?= -O2 -Wall -Wextra
PROTOS := ext-image-copy-capture-v1 ext-image-capture-source-v1 ext-foreign-toplevel-list-v1
HEADERS := $(PROTOS:=-client-protocol.h)
SOURCES := $(PROTOS:=-protocol.c)

all: wlcapture

%-client-protocol.h: %.xml
	wayland-scanner client-header $< $@

%-protocol.c: %.xml
	wayland-scanner private-code $< $@

wlcapture: wlcapture.c $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ wlcapture.c $(SOURCES) $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlcapture $(SOURCES) $(HEADERS)

.PHONY: all clean
