# kj-youtube-dl Makefile

PREFIX ?= $(HOME)/.local

CC = cc
CFLAGS = -std=c99 -pedantic -Wall -Wextra -O2 `pkg-config --cflags gtk+-3.0`
LDFLAGS =
LIBS = `pkg-config --libs gtk+-3.0`

SRC = src/main.c
OBJ = src/main.o

all: kj-youtube-dl

src/main.o: src/main.c src/config.h
	$(CC) $(CFLAGS) -c $< -o $@

kj-youtube-dl: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)

clean:
	rm -f kj-youtube-dl $(OBJ)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -Dm755 kj-youtube-dl $(DESTDIR)$(PREFIX)/bin/kj-youtube-dl
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	install -Dm644 kj-youtube-dl.desktop $(DESTDIR)$(PREFIX)/share/applications/kj-youtube-dl.desktop
	mkdir -p $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -Dm644 kj-youtube-dl.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/kj-youtube-dl.svg

remove:
	rm -f $(DESTDIR)$(PREFIX)/bin/kj-youtube-dl
	rm -f $(DESTDIR)$(PREFIX)/share/applications/kj-youtube-dl.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/kj-youtube-dl.svg

.PHONY: all clean install remove
