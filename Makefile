# kj-youtube-dl - GTK3 GUI wrapper for yt-dlp
# See LICENSE file for copyright and license details.

include config.mk

SRC = main.c
OBJ = $(SRC:.c=.o)

all: kj-youtube-dl

.c.o:
	$(CC) $(CFLAGS) $(INCS) -c $<

kj-youtube-dl: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)

clean:
	rm -f kj-youtube-dl $(OBJ)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f kj-youtube-dl $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/kj-youtube-dl
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	cp -f kj-youtube-dl.desktop $(DESTDIR)$(PREFIX)/share/applications

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/kj-youtube-dl
	rm -f $(DESTDIR)$(PREFIX)/share/applications/kj-youtube-dl.desktop

.PHONY: all clean install uninstall
