/*
 * input.c - queues between interrupt-driven input drivers and whoever
 * owns the physical keyboard and mouse (the screen console or the
 * desktop). Single producer (IRQ) and single consumer, so a ring with
 * interrupts briefly off on the consumer side is enough.
 */
#include "kernel.h"
#include "settings.h"

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

/*
 * Key repeat is done here rather than by the keyboard, so the delay and rate
 * settings work the same on every keyboard (and under emulators, which tend
 * to pass the host's own repeats through). Hardware repeats of a key that's
 * already down are ignored.
 */
static volatile int held_id = -1, held_key;
static volatile uint64_t next_repeat;
static int repeat_delay = 500, repeat_rate = 10;

void input_key_press(int id, int key)
{
	if (id == held_id)
		return;
	held_id = id;
	held_key = key;
	next_repeat = uptime_ms() + (uint64_t)repeat_delay;
	input_key(key);
}

void input_key_release(int id)
{
	if (id == held_id)
		held_id = -1;
}

int input_getkey(void)
{
	uint64_t f = irq_save();
	if (held_id >= 0 && ktail == khead && uptime_ms() >= next_repeat) {
		input_key(held_key);
		next_repeat = uptime_ms() + (uint64_t)(1000 / MAX(1, repeat_rate));
	}
	int k = -1;
	if (ktail != khead) {
		k = keys[ktail];
		ktail = (ktail + 1) % KQ;
	}
	irq_restore(f);
	return k;
}

/* pointer speed 1-10 (5 = as the mouse reports) with a little acceleration
 * for the fast settings; remainders carry over so slow moves aren't lost */
static int mouse_speed = 5, mouse_swap;
static int rem_x, rem_y;

static int scale(int d, int *rem)
{
	int num = mouse_speed;
	if (mouse_speed > 5 && (d > 6 || d < -6))
		num += mouse_speed - 5;		/* accelerate quick flicks */
	int v = d * num + *rem;
	int out = v / 5;
	*rem = v - out * 5;
	return out;
}

void input_mouse(int dx, int dy, int buttons)
{
	dx = scale(dx, &rem_x);
	dy = scale(dy, &rem_y);
	if (mouse_swap)
		buttons = (buttons & ~3) | ((buttons & 1) << 1) | ((buttons & 2) >> 1);
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

static void input_settings(const char *key)
{
	if (!*key || !strncmp(key, "mouse.", 6)) {
		mouse_speed = (int)MAX(1, MIN(10, setting_int("mouse.speed")));
		mouse_swap = setting_int("mouse.swap") != 0;
	}
	if (!*key || !strncmp(key, "kbd.", 4)) {
		repeat_delay = (int)MAX(150, MIN(2000, setting_int("kbd.delay")));
		repeat_rate = (int)MAX(1, MIN(40, setting_int("kbd.rate")));
	}
}

void input_settings_init(void)
{
	setting_watch(input_settings);
}
