#
# evmapd - An input event remapping daemon for Linux
#
# Copyright (c) 2007 Theodoros V. Kalamatianos <nyb@users.sourceforge.net>
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 as published by
# the Free Software Foundation.
#

prefix := /usr/local
sbindir := $(prefix)/bin

DEBUG :=
CFLAGS := -O2 -Wall $(DEBUG)



# Yes, I am lazy...
VER := $(shell head -n 1 NEWS | cut -d : -f 1)



all: evmapd

evmapd: evmapd.c NEWS
	$(CC) $(CFLAGS) -lcfg+ -DVERSION=\"$(VER)\" $< -o $@

install: all
	install -D -m755 evmapd $(sbindir)/evmapd

clean:
	rm -f *.o evmapd
