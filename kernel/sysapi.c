/* sysapi.c - the SYS call registry and the OS_ calls every program gets */
#include "kernel.h"
#include "sysapi.h"
#include "vfs.h"

static const struct sys_entry *table[128];
static int ntable;

void sys_register(const struct sys_entry *e)
{
	if (ntable < (int)NELEM(table))
		table[ntable++] = e;
}

const struct sys_entry *sys_find(const char *name)
{
	for (int i = 0; i < ntable; i++)
		if (!strcasecmp(table[i]->name, name))
			return table[i];
	return NULL;
}

int sys_call(const char *name, int argc, struct sysval *argv, struct sysval *ret)
{
	const struct sys_entry *e = sys_find(name);
	if (!e)
		return -1;
	memset(ret, 0, sizeof *ret);
	return e->fn(argc, argv, ret);
}

void sys_list(void (*cb)(const struct sys_entry *e, void *arg), void *arg)
{
	for (int i = 0; i < ntable; i++)
		cb(table[i], arg);
}

/* ---- OS_ calls ---- */

static int os_write(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	for (int i = 0; i < argc; i++) {
		if (a[i].is_str)
			con_write(a[i].s);
		else
			kprintf("%lld", a[i].n);
	}
	return 0;
}

static int os_newline(int argc, struct sysval *a, struct sysval *r)
{
	(void)argc; (void)a; (void)r;
	con_putc('\n');
	return 0;
}

static int os_readc(int argc, struct sysval *a, struct sysval *r)
{
	(void)argc; (void)a;
	r->n = con_getkey();
	return 0;
}

static int os_command(int argc, struct sysval *a, struct sysval *r)
{
	if (argc < 1 || !a[0].is_str)
		return -2;
	r->n = shell_exec(a[0].s);
	return 0;
}

static int os_time(int argc, struct sysval *a, struct sysval *r)
{
	(void)argc; (void)a;
	r->n = (long long)uptime_ms();
	return 0;
}

static int os_yield(int argc, struct sysval *a, struct sysval *r)
{
	(void)argc; (void)a; (void)r;
	task_yield();
	return 0;
}

static int os_sleep(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	sleep_ms(argc ? (uint64_t)MAX(0, sys_int(&a[0])) : 0);
	return 0;
}

static int os_getenv(int argc, struct sysval *a, struct sysval *r)
{
	(void)argc; (void)a;
	r->is_str = 1;
	strncpy0(r->s, task_current()->cwd, sizeof r->s);
	return 0;
}

static int os_screen(int argc, struct sysval *a, struct sysval *r)
{
	(void)argc; (void)a;
	int w, h;
	con_size(&w, &h);
	r->n = (long long)w << 16 | (h & 0xFFFF);
	return 0;
}

static int os_gotoxy(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	if (argc < 2)
		return -2;
	con_gotoxy((int)sys_int(&a[0]), (int)sys_int(&a[1]));
	return 0;
}

static int os_colour(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	con_attr(argc > 0 ? (int)sys_int(&a[0]) : 7, argc > 1 ? (int)sys_int(&a[1]) : 0);
	return 0;
}

static const struct sys_entry os_calls[] = {
	{ "OS_Write",    os_write,   "OS_Write text...     - print without a newline" },
	{ "OS_NewLine",  os_newline, "OS_NewLine           - print a newline" },
	{ "OS_ReadC",    os_readc,   "OS_ReadC             - wait for a key, return its code" },
	{ "OS_Command",  os_command, "OS_Command cmd       - run a shell command" },
	{ "OS_Time",     os_time,    "OS_Time              - milliseconds since boot" },
	{ "OS_Yield",    os_yield,   "OS_Yield             - let other tasks run" },
	{ "OS_Sleep",    os_sleep,   "OS_Sleep ms          - pause" },
	{ "OS_CurrentDir", os_getenv, "OS_CurrentDir        - the current directory" },
	{ "OS_ScreenSize", os_screen, "OS_ScreenSize        - columns<<16 | rows" },
	{ "OS_GotoXY",   os_gotoxy,  "OS_GotoXY x, y       - move the text cursor" },
	{ "OS_Colour",   os_colour,  "OS_Colour fg, bg     - text colours 0-15" },
};

void sysapi_init(void)
{
	for (size_t i = 0; i < NELEM(os_calls); i++)
		sys_register(&os_calls[i]);
}
