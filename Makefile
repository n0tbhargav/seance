VT      ?= vt
CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += $(shell pkg-config --cflags gtk+-3.0) -I$(VT)/include
LDLIBS  := $(VT)/lib/libghostty-vt.a $(shell pkg-config --libs gtk+-3.0) -lutil -lrt -lm

seance: src/main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
clean:
	rm -f seance
