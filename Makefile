CC = gcc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -lncursesw
PREFIX ?= /usr/local

cutedash: cutedash.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: cutedash
	install -m 755 cutedash $(PREFIX)/bin/cutedash
	ln -sf $(PREFIX)/bin/cutedash $(PREFIX)/bin/stats

uninstall:
	rm -f $(PREFIX)/bin/cutedash $(PREFIX)/bin/stats

clean:
	rm -f cutedash

.PHONY: install uninstall clean
