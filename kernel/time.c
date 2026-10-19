/* time.c - the 1 kHz tick */
#include "kernel.h"

static volatile uint64_t ticks;

void timer_tick(void)
{
	ticks++;
}

uint64_t uptime_ms(void)
{
	return ticks;
}

void sleep_ms(uint64_t ms)
{
	uint64_t until = ticks + ms;
	while (ticks < until)
		task_idle();
}
