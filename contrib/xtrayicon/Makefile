# xtrayicon — minimal XEmbed tray client for testing asteroidz-xembed
# (see xtrayicon.c for why a purpose-built client rather than a real app).
CFLAGS ?= -O2 -Wall -Wextra

all: xtrayicon

xtrayicon: xtrayicon.c
	$(CC) $(CFLAGS) -o $@ $< $$(pkg-config --cflags --libs xcb)

clean:
	rm -f xtrayicon

.PHONY: all clean
