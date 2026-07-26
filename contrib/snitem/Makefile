CFLAGS ?= -O2 -Wall -Wextra
snitem: snitem.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs libsystemd)
clean:
	rm -f snitem
.PHONY: clean
