/*
 * serial.c - COM1 at 115200 8N1, polled.
 *
 * The boot console mirrors to COM1 and accepts input from it, which is
 * how the test suite drives bdos headless. CTTY COM1 builds on this.
 */
#include "../../kernel.h"

#define COM1 0x3F8

static int present;
static int stalled;	/* nobody's draining the line: drop rather than wait */

void serial_init(void)
{
	outb(COM1 + 1, 0x00);	/* no interrupts */
	outb(COM1 + 3, 0x80);	/* DLAB on */
	outb(COM1 + 0, 0x01);	/* divisor 1 = 115200 baud */
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03);	/* 8N1, DLAB off */
	outb(COM1 + 2, 0xC7);	/* FIFO on, cleared */
	outb(COM1 + 4, 0x0B);

	outb(COM1 + 4, 0x1E);	/* loopback self-test */
	outb(COM1 + 0, 0xAE);
	present = (inb(COM1 + 0) == 0xAE);
	outb(COM1 + 4, 0x0F);
}

void serial_putc(char c)
{
	if (!present)
		return;
	if (c == '\n')
		serial_putc('\r');
	int ready = inb(COM1 + 5) & 0x20;
	if (!ready && !stalled) {
		for (int i = 0; i < 20000 && !(ready = inb(COM1 + 5) & 0x20); i++)
			cpu_pause();
		if (!ready)
			stalled = 1;
	}
	if (ready) {
		stalled = 0;
		outb(COM1, (uint8_t)c);
	}
}

int serial_getc(void)
{
	if (!present || !(inb(COM1 + 5) & 0x01))
		return -1;
	return inb(COM1);
}
