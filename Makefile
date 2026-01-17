# kj-youtube-dl - GTK3 GUI wrapper for yt-dlp
# See LICENSE file for copyright and license details.

include src/config.mk

SRC = src/main.c
OBJ = src/main.o

all: bin/kj-youtube-dl

src/main.o: src/main.c
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

bin/kj-youtube-dl: $(OBJ)
	mkdir -p bin
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)

clean:
	rm -f bin/kj-youtube-dl $(OBJ)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f bin/kj-youtube-dl $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/kj-youtube-dl
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	cp -f kj-youtube-dl.desktop $(DESTDIR)$(PREFIX)/share/applications

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/kj-youtube-dl
	rm -f $(DESTDIR)$(PREFIX)/share/applications/kj-youtube-dl.desktop

.PHONY: all clean install uninstall
