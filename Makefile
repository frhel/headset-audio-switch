.POSIX:
CFLAGS  = -O2 -s -Wall -Wextra -Werror
LDLIBS  = -lpulse
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

headset-audio-switch: headset-audio-switch.c
	$(CC) $(CFLAGS) -o $@ headset-audio-switch.c $(LDLIBS)

install: headset-audio-switch
	install -Dm755 headset-audio-switch $(DESTDIR)$(BINDIR)/headset-audio-switch
	install -Dm644 99-steelseries-arctis7.rules $(DESTDIR)/etc/udev/rules.d/99-steelseries-arctis7.rules

clean:
	rm -f headset-audio-switch

.PHONY: install clean
