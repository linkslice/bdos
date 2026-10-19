/* stubs.c - placeholders for subsystems still being built */
#include "kernel.h"

int basic_run_file(const char *p, int argc, char **argv)
{
	(void)argc; (void)argv;
	kprintf("BASIC isn't built in yet (%s)\n", p);
	return 1;
}

int basic_immediate(void)
{
	kprintf("BASIC isn't built in yet\n");
	return 1;
}

int com_run(const char *p, int argc, char **argv)
{
	(void)argc; (void)argv;
	kprintf("The 8086 emulator isn't built in yet (%s)\n", p);
	return 1;
}

int cint_run_file(const char *p, int argc, char **argv)
{
	(void)argc; (void)argv;
	kprintf("The C interpreter isn't built in yet (%s)\n", p);
	return 1;
}

int desktop_run(const char *remote)
{
	(void)remote;
	kprintf("The desktop isn't built in yet\n");
	return 1;
}

struct console *remote_console_open(const char *host, int *err)
{
	(void)host;
	*err = -21;
	return NULL;
}
