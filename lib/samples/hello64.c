/* HELLO64.EXE - the smallest native bdos program */
#include <stdio.h>

int main(int argc, char **argv)
{
	printf("Hello from a native 64-bit bdos program!\n");
	for (int i = 1; i < argc; i++)
		printf("  argument %d: %s\n", i, argv[i]);
	return 0;
}
