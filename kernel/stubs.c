/* stubs.c - placeholders for subsystems still being built */
#include "kernel.h"

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

int remote_desktop(const char *host)
{
	kprintf("Networking isn't built in yet (%s)\n", host);
	return 1;
}

int console_is_remote(struct console *c)
{
	(void)c;
	return 0;
}

int remote_session_desktop(struct console *c)
{
	(void)c;
	return 1;
}

struct console *remote_console_open(const char *host, int *err)
{
	(void)host;
	*err = -21;
	return NULL;
}
