# bterm - a drawterm for bdos.
#
#   make          bterm with the desktop window (needs SDL2)
#   make text     bterm for text mode only (no SDL2 needed)
#
# SDL2:  macOS: brew install sdl2     Debian/Ubuntu: apt install libsdl2-dev

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null)

all: bterm

bterm: bterm.c
	@if [ -z "$(SDL_LIBS)" ]; then \
		echo "SDL2 not found: building text mode only (install SDL2 for the desktop)"; \
		$(CC) $(CFLAGS) -DNO_SDL -o $@ bterm.c; \
	else \
		$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ bterm.c $(SDL_LIBS); \
	fi

text: bterm.c
	$(CC) $(CFLAGS) -DNO_SDL -o bterm bterm.c

install: bterm
	install -m 755 bterm $(or $(PREFIX),/usr/local)/bin/bterm

clean:
	rm -f bterm

.PHONY: all text install clean
