/*
 * PRIMES.EXE - count primes with one thread per core, to show off SMP.
 *
 *   PRIMES [limit] [threads]
 *
 * Watch Pulse while it runs: every core lights up.
 */
#include <stdio.h>
#include <stdlib.h>
#include <bdos.h>

static long limit = 20000000;
static int nthreads;
static long counts[64];

static int is_prime(long n)
{
	if (n < 2)
		return 0;
	if (n % 2 == 0)
		return n == 2;
	for (long d = 3; d * d <= n; d += 2)
		if (n % d == 0)
			return 0;
	return 1;
}

static int worker(void *arg)
{
	long id = (long)arg, c = 0;
	/* interleave in blocks so every thread gets a fair share of big numbers */
	for (long base = id * 1000; base < limit; base += (long)nthreads * 1000)
		for (long n = base; n < base + 1000 && n < limit; n++)
			c += is_prime(n);
	counts[id] = c;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1)
		limit = atol(argv[1]);
	nthreads = argc > 2 ? atoi(argv[2]) : bdos_cores();
	if (nthreads < 1)
		nthreads = 1;
	if (nthreads > 64)
		nthreads = 64;
	printf("Counting primes below %ld on %d thread%s (%d core%s)...\n", limit, nthreads,
	       nthreads == 1 ? "" : "s", bdos_cores(), bdos_cores() == 1 ? "" : "s");
	long t0 = bdos_ms();
	int tid[64];
	for (int i = 0; i < nthreads; i++)
		tid[i] = bdos_thread(worker, (void *)(long)i);
	long total = 0;
	for (int i = 0; i < nthreads; i++) {
		bdos_join(tid[i]);
		total += counts[i];
	}
	long ms = bdos_ms() - t0;
	printf("%ld primes in %ld.%03ld seconds\n", total, ms / 1000, ms % 1000);
	return 0;
}
