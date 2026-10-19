/*
 * kbd.c - PS/2 keyboard (scan code set 1, polled) plus serial input.
 *
 * No interrupts yet: getkey() polls the i8042 and COM1 in a loop.
 * Modern USB-only machines reach this through the firmware's legacy
 * PS/2 emulation; a real xHCI driver comes later.
 */
#include "kernel.h"

#define KBD_DATA	0x60
#define KBD_STATUS	0x64

static int shift, caps, ctrl, extended;

static const char map_lo[128] = {
	0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
	'\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
	0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
	0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
	'*', 0, ' ',
};

static const char map_hi[128] = {
	0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
	'\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
	0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
	0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
	'*', 0, ' ',
};

void kbd_init(void)
{
	/* Drain anything the firmware left in the output buffer. */
	for (int i = 0; i < 64 && (inb(KBD_STATUS) & 1); i++)
		(void)inb(KBD_DATA);
}

int kbd_getc(void)
{
	uint8_t st = inb(KBD_STATUS);
	if (!(st & 1))
		return -1;
	uint8_t sc = inb(KBD_DATA);
	if (st & 0x20)		/* mouse byte, not ours */
		return -1;

	if (sc == 0xE0) {
		extended = 1;
		return -1;
	}
	int release = sc & 0x80;
	sc &= 0x7F;

	if (extended) {
		extended = 0;
		if (release)
			return -1;
		switch (sc) {
		case 0x48: return KEY_UP;
		case 0x50: return KEY_DOWN;
		case 0x4B: return KEY_LEFT;
		case 0x4D: return KEY_RIGHT;
		case 0x47: return KEY_HOME;
		case 0x4F: return KEY_END;
		case 0x53: return KEY_DEL;
		case 0x1C: return '\n';		/* keypad enter */
		case 0x35: return '/';		/* keypad slash */
		case 0x1D: ctrl = 1; return -1;	/* right ctrl */
		}
		return -1;
	}

	switch (sc) {
	case 0x2A:
	case 0x36:
		shift = !release;
		return -1;
	case 0x1D:
		ctrl = !release;
		return -1;
	case 0x3A:
		if (!release)
			caps = !caps;
		return -1;
	}
	if (release)
		return -1;

	char c = shift ? map_hi[sc] : map_lo[sc];
	if (!c)
		return -1;
	if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
		c ^= 0x20;
	if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
		return toupper(c) - '@';	/* ^C = 3 and friends */
	return c;
}

/* Serial input: translate CR, DEL and ANSI arrow sequences. */
static int esc_state;

static int serial_key(void)
{
	int c = serial_getc();
	if (c < 0)
		return -1;
	if (esc_state == 1) {
		esc_state = (c == '[') ? 2 : 0;
		return -1;
	}
	if (esc_state == 2) {
		esc_state = 0;
		switch (c) {
		case 'A': return KEY_UP;
		case 'B': return KEY_DOWN;
		case 'C': return KEY_RIGHT;
		case 'D': return KEY_LEFT;
		case 'H': return KEY_HOME;
		case 'F': return KEY_END;
		}
		return -1;
	}
	if (c == 27) {
		esc_state = 1;
		return -1;
	}
	/* Enter is CR, LF or CRLF depending on the other end. */
	static int last_cr;
	if (c == '\n' && last_cr) {
		last_cr = 0;
		return -1;
	}
	last_cr = (c == '\r');
	if (c == '\r' || c == '\n')
		return '\n';
	if (c == 0x7F)
		return '\b';
	return c;
}

int getkey(void)
{
	for (;;) {
		int c = kbd_getc();
		if (c >= 0)
			return c;
		c = serial_key();
		if (c >= 0)
			return c;
		cpu_pause();
	}
}
