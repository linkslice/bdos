/*
 * input.c - queues between interrupt-driven input drivers and whoever
 * owns the physical keyboard and mouse (the screen console or the
 * desktop). Single producer (IRQ) and single consumer, so a ring with
 * interrupts briefly off on the consumer side is enough.
 */
#include "kernel.h"
#include "settings.h"

static struct spinlock lock = SPINLOCK_INIT;

#define KQ 128
#define MQ 128

static volatile int keys[KQ];
static volatile unsigned khead, ktail;
static struct mouse_event mice[MQ];
static volatile unsigned mhead, mtail;

/* the keyboard's own bytes, for programs that want key-up as well as key-down */
#define RQ 64
static volatile uint8_t raws[RQ];
static volatile unsigned rhead, rtail;

void input_raw(uint8_t sc)
{
	uint64_t f = spin_lock_irqsave(&lock);
	unsigned next = (rhead + 1) % RQ;
	if (next == rtail)
		rtail = (rtail + 1) % RQ;	/* full: forget the oldest */
	raws[rhead] = sc;
	rhead = next;
	spin_unlock_irqrestore(&lock, f);
}

int input_getraw(void)
{
	uint64_t f = spin_lock_irqsave(&lock);
	int r = -1;
	if (rtail != rhead) {
		r = raws[rtail];
		rtail = (rtail + 1) % RQ;
	}
	spin_unlock_irqrestore(&lock, f);
	return r;
}

static void key_put(int key)
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
void input_key(int key)
{
	uint64_t f = spin_lock_irqsave(&lock);
	key_put(key);
	spin_unlock_irqrestore(&lock, f);
	sched_kick();
}

static volatile int held_id = -1, held_key;
static volatile uint64_t next_repeat;
static int repeat_delay = 500, repeat_rate = 10;

void input_key_press(int id, int key)
{
	if (id == held_id)
		return;
	uint64_t f = spin_lock_irqsave(&lock);
	held_id = id;
	held_key = key;
	next_repeat = uptime_ms() + (uint64_t)repeat_delay;
	key_put(key);
	spin_unlock_irqrestore(&lock, f);
}

void input_key_release(int id)
{
	if (id == held_id)
		held_id = -1;
}

int input_getkey(void)
{
	uint64_t f = spin_lock_irqsave(&lock);
	if (held_id >= 0 && ktail == khead && uptime_ms() >= next_repeat) {
		key_put(held_key);
		next_repeat = uptime_ms() + (uint64_t)(1000 / MAX(1, repeat_rate));
	}
	int k = -1;
	if (ktail != khead) {
		k = keys[ktail];
		ktail = (ktail + 1) % KQ;
	}
	spin_unlock_irqrestore(&lock, f);
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

/* absolute devices: the newest position wins, so just overwrite a queued
 * absolute event with the same buttons */
static int abs_seen;

int input_mouse_is_abs(void)
{
	return abs_seen;
}

static void mouse_abs_put(int x, int y, int buttons);

void input_mouse_abs(int x, int y, int buttons)
{
	uint64_t f = spin_lock_irqsave(&lock);
	mouse_abs_put(x, y, buttons);
	spin_unlock_irqrestore(&lock, f);
}

static void mouse_abs_put(int x, int y, int buttons)
{
	abs_seen = 1;
	if (mouse_swap)
		buttons = (buttons & ~3) | ((buttons & 1) << 1) | ((buttons & 2) >> 1);
	if (mhead != mtail) {
		unsigned last = (mhead + MQ - 1) % MQ;
		if (mice[last].abs && !mice[last].wheel && mice[last].buttons == buttons) {
			mice[last].ax = x;
			mice[last].ay = y;
			return;
		}
	}
	unsigned next = (mhead + 1) % MQ;
	if (next == mtail)
		return;
	mice[mhead] = (struct mouse_event){ 0, 0, buttons, 1, x, y, 0 };
	mhead = next;
}

static void mouse_put(int dx, int dy, int buttons);

void input_mouse(int dx, int dy, int buttons)
{
	uint64_t f = spin_lock_irqsave(&lock);
	mouse_put(dx, dy, buttons);
	spin_unlock_irqrestore(&lock, f);
}

static void mouse_put(int dx, int dy, int buttons)
{
	dx = scale(dx, &rem_x);
	dy = scale(dy, &rem_y);
	if (mouse_swap)
		buttons = (buttons & ~3) | ((buttons & 1) << 1) | ((buttons & 2) >> 1);
	/* Coalesce motion with the newest queued event if buttons match. */
	if (mhead != mtail) {
		unsigned last = (mhead + MQ - 1) % MQ;
		if (!mice[last].abs && !mice[last].wheel && mice[last].buttons == buttons) {
			mice[last].dx += dx;
			mice[last].dy += dy;
			return;
		}
	}
	unsigned next = (mhead + 1) % MQ;
	if (next == mtail)
		return;
	mice[mhead] = (struct mouse_event){ dx, dy, buttons, 0, 0, 0, 0 };
	mhead = next;
}

void input_wheel(int clicks)
{
	if (!clicks)
		return;
	uint64_t f = spin_lock_irqsave(&lock);
	unsigned next = (mhead + 1) % MQ;
	if (next != mtail) {
		/* where the pointer is, as the newest event has it, with the wheel */
		struct mouse_event e = { 0, 0, 0, 0, 0, 0, clicks };
		if (mhead != mtail) {
			struct mouse_event *last = &mice[(mhead + MQ - 1) % MQ];
			e.buttons = last->buttons;
			e.abs = last->abs;
			e.ax = last->ax;
			e.ay = last->ay;
		} else {
			e.buttons = -1;		/* "unchanged": the reader keeps its own */
		}
		mice[mhead] = e;
		mhead = next;
	}
	spin_unlock_irqrestore(&lock, f);
}

int input_getmouse(struct mouse_event *ev)
{
	uint64_t f = spin_lock_irqsave(&lock);
	int got = 0;
	if (mtail != mhead) {
		*ev = mice[mtail];
		mtail = (mtail + 1) % MQ;
		got = 1;
	}
	spin_unlock_irqrestore(&lock, f);
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
