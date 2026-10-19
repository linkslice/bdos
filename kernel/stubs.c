/* stubs.c - placeholders for subsystems still being built */
#include "kernel.h"

int cint_run_file(const char *p, int argc, char **argv)
{
	(void)argc; (void)argv;
	kprintf("The C interpreter isn't built in yet (%s)\n", p);
	return 1;
}

