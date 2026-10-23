/* SPIN.C - keep one core busy for a while, to watch in Pulse.
 *   SPIN            10 seconds
 *   SPIN 30         30 seconds
 * START it a few times (START \DEMOS\SPIN.C) and every core lights up. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
	long secs = argc > 1 ? atoi(argv[1]) : 10;
	long until = clock() + secs * 1000, n = 0, primes = 0, i;

	while (clock() < until) {
		n++;
		for (i = 2; i * i <= n; i++)
			if (n % i == 0)
				break;
		if (i * i > n)
			primes++;
	}
	printf("SPIN: %ld primes below %ld in %ld s\n", primes, n, secs);
	return 0;
}
