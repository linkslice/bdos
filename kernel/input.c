/*
 * input.c - queues between interrupt-driven input drivers and whoever
 * owns the physical keyboard and mouse (the screen console or the
 * desktop). Single producer (IRQ) and single consumer, so a ring with
 * interrupts briefly off on the consumer side is enough.
 */
#include "kernel.h"

#define KQ 128
#define MQ 128

static volatile int keys[KQ];
static volatile unsigned khead, ktail;
static struct mouse_event mice[MQ];
static volatile unsigned mhead, mtail;

void input_key(int key)
{
	unsigned next = (khead + 1) % KQ;
	if (next == ktail)
		return;		/* full: drop */
	keys[khead] = key;
	khead = next;
}

int input_getkey(void)
{
	uint64_t f = irq_save();
	int k = -1;
	if (ktail != khead) {
		k = keys[ktail];
		ktail = (ktail + 1) % KQ;
	}
	irq_restore(f);
	return k;
}

void input_mouse(int dx, int dy, int buttons)
{
	/* Coalesce motion with the newest queued event if buttons match. */
	if (mhead != mtail) {
		unsigned last = (mhead + MQ - 1) % MQ;
		if (mice[last].buttons == buttons) {
			mice[last].dx += dx;
			mice[last].dy += dy;
			return;
		}
	}
	unsigned next = (mhead + 1) % MQ;
	if (next == mtail)
		return;
	mice[mhead].dx = dx;
	mice[mhead].dy = dy;
	mice[mhead].buttons = buttons;
	mhead = next;
}

int input_getmouse(struct mouse_event *ev)
{
	uint64_t f = irq_save();
	int got = 0;
	if (mtail != mhead) {
		*ev = mice[mtail];
		mtail = (mtail + 1) % MQ;
		got = 1;
	}
	irq_restore(f);
	return got;
}
