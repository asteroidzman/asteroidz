# wlbgeffect — an xdg-shell client that supplies a real
# ext-background-effect-v1 blur region (see wlbgeffect.c for why nothing else
# in contrib/ can exercise the walker's node-local clip conversion).
CFLAGS ?= -O2 -Wall -Wextra
XDG := xdg-shell
DEC := xdg-decoration-unstable-v1
BGE := ext-background-effect-v1
FROG := frog-color-management-v1
FROG_XML := ../../protocols/$(FROG).xml
CM := color-management-v1
CM_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/staging/color-management/$(CM).xml
BGE_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/staging/ext-background-effect/$(BGE).xml
DEC_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/unstable/xdg-decoration/$(DEC).xml

all: wlbgeffect

$(XDG)-client-protocol.h: $(XDG).xml
	wayland-scanner client-header $< $@

$(XDG)-protocol.c: $(XDG).xml
	wayland-scanner private-code $< $@

$(BGE)-client-protocol.h:
	wayland-scanner client-header $(BGE_XML) $@

$(CM)-client-protocol.h:
	wayland-scanner client-header $(CM_XML) $@

$(CM)-protocol.c:
	wayland-scanner private-code $(CM_XML) $@

$(DEC)-client-protocol.h:
	wayland-scanner client-header $(DEC_XML) $@

$(DEC)-protocol.c:
	wayland-scanner private-code $(DEC_XML) $@

$(BGE)-protocol.c:
	wayland-scanner private-code $(BGE_XML) $@

$(FROG)-client-protocol.h:
	wayland-scanner client-header $(FROG_XML) $@

$(FROG)-protocol.c:
	wayland-scanner private-code $(FROG_XML) $@

wlbgeffect: wlbgeffect.c $(XDG)-protocol.c $(XDG)-client-protocol.h \
            $(BGE)-protocol.c $(BGE)-client-protocol.h \
            $(DEC)-protocol.c $(DEC)-client-protocol.h \
            $(FROG)-protocol.c $(FROG)-client-protocol.h \
            $(CM)-protocol.c $(CM)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlbgeffect.c $(XDG)-protocol.c $(BGE)-protocol.c \
		$(DEC)-protocol.c $(FROG)-protocol.c $(CM)-protocol.c \
		$$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlbgeffect $(XDG)-protocol.c $(XDG)-client-protocol.h \
		$(BGE)-protocol.c $(BGE)-client-protocol.h \
		$(DEC)-protocol.c $(DEC)-client-protocol.h \
		$(CM)-protocol.c $(CM)-client-protocol.h \
		$(FROG)-protocol.c $(FROG)-client-protocol.h

.PHONY: all clean
