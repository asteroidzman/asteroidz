# wlcm -- a wp-color-management client that prints the preferred image
# description a compositor sets for its surface (see wlcm.c for why the
# assertion cannot be made from inside the compositor).
CFLAGS ?= -O2 -Wall -Wextra
WP := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
CM := $(WP)/staging/color-management/color-management-v1.xml
XDG := $(WP)/stable/xdg-shell/xdg-shell.xml

all: wlcm

color-management-v1-client-protocol.h: $(CM)
	wayland-scanner client-header $< $@
color-management-v1-protocol.c: $(CM)
	wayland-scanner private-code $< $@
xdg-shell-client-protocol.h: $(XDG)
	wayland-scanner client-header $< $@
xdg-shell-protocol.c: $(XDG)
	wayland-scanner private-code $< $@

wlcm: wlcm.c color-management-v1-protocol.c xdg-shell-protocol.c \
      color-management-v1-client-protocol.h xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlcm.c color-management-v1-protocol.c \
		xdg-shell-protocol.c $$(pkg-config --cflags --libs wayland-client) -lrt

clean:
	rm -f wlcm *-protocol.c *-client-protocol.h

.PHONY: all clean
