CC = gcc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -lncursesw
PREFIX ?= /usr/local

SRCS = main.c readers.c drawing.c panels.c
OBJS = $(SRCS:.c=.o)

cutedash: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c cutedash.h
	$(CC) $(CFLAGS) -c $<

install: cutedash
	install -m 755 cutedash $(PREFIX)/bin/cutedash
	ln -sf $(PREFIX)/bin/cutedash $(PREFIX)/bin/stats

uninstall:
	rm -f $(PREFIX)/bin/cutedash $(PREFIX)/bin/stats

clean:
	rm -f cutedash $(OBJS)

.PHONY: install uninstall clean
