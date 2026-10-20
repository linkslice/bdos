/* CERR.C - a runtime error: the interpreter must report it, not crash */
#include <stdio.h>
int main(void)
{
	int *p = NULL;
	printf("before\n");
	*p = 1;
	printf("after\n");
	return 0;
}
