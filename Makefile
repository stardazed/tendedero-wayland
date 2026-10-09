CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
PKGS     = gtk+-3.0 gtk-layer-shell-0
PREFIX  ?= /usr/local

tendedero: tendedero.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -o $@ $< $$(pkg-config --libs $(PKGS)) -lm

install: tendedero
	install -Dm755 tendedero $(DESTDIR)$(PREFIX)/bin/tendedero

clean:
	rm -f tendedero
.PHONY: install clean
