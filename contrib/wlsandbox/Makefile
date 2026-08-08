# wlsandbox — security-context-v1 client that reports what a sandboxed client
# is allowed to see (see wlsandbox.c for the class of bug this exists to make
# assertable).
CFLAGS ?= -O2 -Wall -Wextra
SEC := security-context-v1
SEC_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/staging/security-context/$(SEC).xml

all: wlsandbox

$(SEC)-client-protocol.h: $(SEC_XML)
	wayland-scanner client-header $< $@

$(SEC)-protocol.c: $(SEC_XML)
	wayland-scanner private-code $< $@

wlsandbox: wlsandbox.c $(SEC)-protocol.c $(SEC)-client-protocol.h
	$(CC) $(CFLAGS) -o $@ wlsandbox.c $(SEC)-protocol.c $$(pkg-config --cflags --libs wayland-client)

clean:
	rm -f wlsandbox $(SEC)-protocol.c $(SEC)-client-protocol.h

.PHONY: all clean
