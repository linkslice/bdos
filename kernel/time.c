/* time.c - the 1 kHz tick, and a microsecond clock from the CPU's counter */
#include "kernel.h"

static volatile uint64_t ticks;
static uint64_t cyc0, cyc_per_ms;

void timer_tick(void)
{
	ticks++;
}

uint64_t uptime_ms(void)
{
	return ticks;
}

/* measure the cycle counter against the tick (call with interrupts on) */
void time_calibrate(void)
{
	uint64_t t0 = ticks;
	while (ticks == t0)
		cpu_pause();
	uint64_t a = rdtsc();
	t0 = ticks;
	while (ticks < t0 + 20)
		cpu_pause();
	cyc_per_ms = (rdtsc() - a) / 20;
	if (!cyc_per_ms)
		cyc_per_ms = 1000000;
	cyc0 = a - cyc_per_ms * t0;
}

uint64_t uptime_us(void)
{
	if (!cyc_per_ms)
		return ticks * 1000;
	return (rdtsc() - cyc0) * 1000 / cyc_per_ms;
}

void delay_us(uint64_t us)
{
	uint64_t end = uptime_us() + us;
	while (uptime_us() < end)
		cpu_pause();
}

void sleep_ms(uint64_t ms)
{
	uint64_t until = ticks + ms;
	while (ticks < until)
		task_idle();
}
