# wlsync — the only client in this tree that speaks wp_linux_drm_syncobj_v1.
#
# See wlsync.c for why: acquire and release points are invisible in a
# screenshot, so the client that drives them has to be built for the purpose.
CFLAGS ?= -O2 -Wall -Wextra
PROTO := xdg-shell linux-dmabuf-v1 linux-drm-syncobj-v1
HDRS  := $(PROTO:%=%-client-protocol.h)
SRCS  := $(PROTO:%=%-protocol.c)

all: wlsync

%-client-protocol.h: %.xml
	wayland-scanner client-header $< $@

%-protocol.c: %.xml
	wayland-scanner private-code $< $@

wlsync: wlsync.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ wlsync.c $(SRCS) \
		$$(pkg-config --cflags --libs wayland-client gbm libdrm)

clean:
	rm -f wlsync $(SRCS) $(HDRS)

.PHONY: all clean
