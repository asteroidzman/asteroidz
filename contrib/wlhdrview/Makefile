# wlhdrview — show an HDR still as it was encoded.
#
# See wlhdrview.c: every viewer on this machine tone maps a PQ picture to SDR
# before displaying it, so "does the screenshot look right" was a question
# nothing could answer. This describes the surface to the compositor using the
# file's own colour and mastering metadata, and lets the real HDR path run.
#
# libheif is a dependency of THIS TOOL and not of the compositor. Decoding
# HEVC is not something a compositor should grow the ability to do, and a
# contrib client is where a decoder belongs.
CFLAGS ?= -O2 -Wall -Wextra
PROTOS := color-management-v1 xdg-shell
HEADERS := $(PROTOS:=-client-protocol.h)
SOURCES := $(PROTOS:=-protocol.c)

all: wlhdrview

%-client-protocol.h: %.xml
	wayland-scanner client-header $< $@

%-protocol.c: %.xml
	wayland-scanner private-code $< $@

wlhdrview: wlhdrview.c $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ wlhdrview.c $(SOURCES) \
		$$(pkg-config --cflags --libs wayland-client libheif)

clean:
	rm -f wlhdrview $(SOURCES) $(HEADERS)

.PHONY: all clean
