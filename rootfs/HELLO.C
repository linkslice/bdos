/* HELLO.C - C programs run straight from source: just type HELLO.C
 *
 * No compiler, no linker. Change this file in EDIT and run it again.
 */
#include <stdio.h>

int main(int argc, char **argv)
{
	int i;

	printf("Hello from C on bdos!\n");
	for (i = 1; i <= 5; i++)
		printf("%d squared is %2d\n", i, i * i);
	if (argc > 1)
		printf("You said: %s\n", argv[1]);
	return 0;
}
