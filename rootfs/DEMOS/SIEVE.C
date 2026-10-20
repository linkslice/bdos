/* SIEVE.C - primes below 1000, the C way */
#include <stdio.h>
#include <string.h>

#define N 1000

char composite[N];

int main(void)
{
	int i, j, count = 0;

	memset(composite, 0, sizeof composite);
	for (i = 2; i * i < N; i++)
		if (!composite[i])
			for (j = i * i; j < N; j += i)
				composite[j] = 1;
	for (i = 2; i < N; i++)
		if (!composite[i]) {
			printf("%5d", i);
			if (++count % 12 == 0)
				putchar('\n');
		}
	printf("\n%d primes below %d\n", count, N);
	return 0;
}
