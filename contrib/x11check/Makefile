# x11check — the XWayland scaling oracle's client half. See x11check.c.
CFLAGS ?= -O2 -Wall -Wextra

all: x11check

x11check: x11check.c
	$(CC) $(CFLAGS) -o $@ x11check.c $$(pkg-config --cflags --libs xcb)

clean:
	rm -f x11check

.PHONY: all clean
