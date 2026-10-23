/*
 * SPIN64.EXE - keep every core busy for a while (or until Ctrl+C), the
 * native cousin of \DEMOS\SPIN.C. Good company for Pulse.
 *
 *   SPIN64 [seconds] [threads]
 */
#include <stdio.h>
#include <stdlib.h>
#include <bdos.h>

static volatile long stop_at;
static volatile unsigned long work[64];

static int spin(void *arg)
{
	long i = (long)arg;
	while (bdos_ms() < stop_at)
		for (int k = 0; k < 100000; k++)
			work[i]++;
	return 0;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 10;
	int n = argc > 2 ? atoi(argv[2]) : bdos_cores();
	if (n < 1) n = 1;
	if (n > 64) n = 64;
	printf("Spinning %d thread%s for %d seconds (Ctrl+C stops)...\n", n, n == 1 ? "" : "s", secs);
	stop_at = bdos_ms() + secs * 1000L;
	int tid[64];
	for (int i = 0; i < n; i++)
		tid[i] = bdos_thread(spin, (void *)(long)i);
	for (int i = 0; i < n; i++)
		bdos_join(tid[i]);
	printf("Done.\n");
	return 0;
}
