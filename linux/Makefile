CFLAGS ?= -O2 -g -Wall -Wextra
PKGS = sdl3 libavcodec libavutil gl egl libdrm freetype2 fontconfig

SRCS = macglass.c video.c render.c overlay.c

macglass: $(SRCS) macglass.h ../protocol.h
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(shell pkg-config --cflags --libs $(PKGS)) -lpthread

# Installs for the current user: the viewer, its user service, the open-or-focus launcher and a
# desktop entry.
PREFIX ?= $(HOME)/.local
install: macglass
	install -Dm755 macglass $(PREFIX)/bin/macglass
	install -Dm755 macglass-open $(PREFIX)/bin/macglass-open
	install -Dm644 macglass.service $(HOME)/.config/systemd/user/macglass.service
	install -Dm644 macglass.desktop $(PREFIX)/share/applications/macglass.desktop
	systemctl --user daemon-reload

clean:
	rm -f macglass

.PHONY: clean install
