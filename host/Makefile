# seance host (GTK3 + libghostty core). Layout: $(PREFIX)/{bin/seance, lib/libghostty.so, share/{ghostty,terminfo}}
PREFIX  ?= /opt/seance
CORE    ?= ../full
GHOSTTY ?= ../ghostty
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -I$(GHOSTTY)/include $(shell pkg-config --cflags gtk+-3.0)
LDLIBS  := $(shell pkg-config --libs gtk+-3.0) -lEGL -ldl -rdynamic

seance-host: host.c
	$(CC) $(CFLAGS) -o $@ $< -L$(CORE)/lib -l:ghostty-internal.so $(LDLIBS) -Wl,-rpath,'$$ORIGIN/../full/lib'

# Installed binary uses rpath $ORIGIN/../lib
seance: host.c
	$(CC) $(CFLAGS) -o $@ $< -L$(CORE)/lib -l:ghostty-internal.so $(LDLIBS) -Wl,-rpath,'$$ORIGIN/../lib'

install: seance seancectl
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/share
	install -m 0755 seance $(DESTDIR)$(PREFIX)/bin/seance
	install -m 0755 seancectl $(DESTDIR)$(PREFIX)/bin/seancectl
	install -m 0755 $(CORE)/lib/ghostty-internal.so $(DESTDIR)$(PREFIX)/lib/libghostty.so
	install -d $(DESTDIR)/usr/share/applications
	install -m 0644 ../packaging/seance.desktop $(DESTDIR)/usr/share/applications/seance.desktop
	install -d $(DESTDIR)$(PREFIX)/share/licenses/seance
	install -m 0644 ../LICENSE ../NOTICE.md ../licenses/* $(DESTDIR)$(PREFIX)/share/licenses/seance/
	cp -R $(CORE)/share/ghostty $(DESTDIR)$(PREFIX)/share/ghostty
	cp -R $(CORE)/share/terminfo $(DESTDIR)$(PREFIX)/share/terminfo
	rm -f $(DESTDIR)$(PREFIX)/share/terminfo/ghostty.terminfo

seancectl: seancectl.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f seance seance-host seancectl
