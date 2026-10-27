# launchel
VERSION = 0.1
PREFIX = /usr/local

PKGS = x11 xft fontconfig
CFLAGS += -std=c99 -Wall -Os -D_DEFAULT_SOURCE -D_GNU_SOURCE -DVERSION=\"$(VERSION)\" `pkg-config --cflags $(PKGS)`
LDLIBS += `pkg-config --libs $(PKGS)`

launchel: launchel.c
	$(CC) $(CFLAGS) -o $@ launchel.c $(LDFLAGS) $(LDLIBS)

install: launchel
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 launchel $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/launchel

clean:
	rm -f launchel

.PHONY: install uninstall clean
