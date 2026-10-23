/*
 * start.c - the start and the end of a program. Its own file so that
 * tcc -run, whose runmain.o brings its own exit() and atexit(), can link
 * the rest of the library without clashing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <bdos.h>
#include "../../include/bdos_sys.h"

/* ---- the start and the end ---- */

#define MAX_ATEXIT 32
static void (*atexit_fns[MAX_ATEXIT])(void);
static int natexit;

int atexit(void (*fn)(void))
{
	if (natexit >= MAX_ATEXIT)
		return -1;
	atexit_fns[natexit++] = fn;
	return 0;
}

void __stdio_flush_all(void);

_Noreturn void exit(int code)
{
	while (natexit)
		atexit_fns[--natexit]();
	__stdio_flush_all();
	_exit(code);
}

int main(int argc, char **argv);

_Noreturn void __libc_start(long argc, char **argv)
{
	exit(main((int)argc, argv));
}

