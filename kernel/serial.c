/*
 * serial.c - COM1 at 115200 8N1.
 *
 * Everything printed on screen is mirrored here, and keys typed on the
 * serial line are accepted as keyboard input. That makes headless testing
 * (and a future CTTY COM1) easy.
 */
#include "kernel.h"

#define COM1 0x3F8

static int present;

void serial_init(void)
{
	outb(COM1 + 1, 0x00);	/* no interrupts */
	outb(COM1 + 3, 0x80);	/* DLAB on */
	outb(COM1 + 0, 0x01);	/* divisor 1 = 115200 baud */
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03);	/* 8N1, DLAB off */
	outb(COM1 + 2, 0xC7);	/* FIFO on, cleared, 14-byte threshold */
	outb(COM1 + 4, 0x0B);	/* DTR, RTS, OUT2 */

	/* Loopback self-test: is there really a UART here? */
	outb(COM1 + 4, 0x1E);
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
	for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
		cpu_pause();
	outb(COM1, (uint8_t)c);
}

int serial_getc(void)
{
	if (!present || !(inb(COM1 + 5) & 0x01))
		return -1;
	return inb(COM1);
}
