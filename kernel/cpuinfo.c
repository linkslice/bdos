/*
 * cpuinfo.c - CPU$, the cores as a device.
 *
 *   TYPE CPU$            one line per core: its load and whether it's on
 *   ECHO off 2 > CPU$    park core 2 (it stops taking work); "on 2" undoes it
 *
 * Pulse draws the same numbers; BASIC and C programs can read them too.
 */
#include "kernel.h"
#include "vfs.h"

static long cpu_read(struct chardev *d, void *buf, size_t n)
{
	(void)d;
	char info[4096], name[64];
	arch_cpu_name(name, sizeof name);
	int online = 0, total = 0;
	for (int i = 0; i < ncpus; i++)
		online += cpus[i].online && !cpus[i].parked;
	for (int i = 0; i < ncpus; i++)
		total += cpus[i].online ? cpus[i].load : 0;
	int len = snprintf(info, sizeof info, "cores %d on %d load %d \"%s\"\n", ncpus, online,
	    ncpus ? total / ncpus : 0, name);
	for (int i = 0; i < ncpus && len < (int)sizeof info - 64; i++)
		len += snprintf(info + len, sizeof info - (size_t)len, "cpu %d load %d %s\n", i,
		    cpus[i].load, !cpus[i].online ? "offline" : cpus[i].parked ? "off" : "on");
	size_t c = MIN(n, (size_t)len);
	memcpy(buf, info, c);
	return (long)c;
}

static long cpu_write(struct chardev *d, const void *buf, size_t n)
{
	(void)d;
	char line[64];
	size_t l = MIN(n, sizeof line - 1);
	memcpy(line, buf, l);
	line[l] = 0;
	const char *p = line;
	while (*p == ' ')
		p++;
	int off = !strncasecmp(p, "off", 3) || !strncasecmp(p, "park", 4);
	int on = !strncasecmp(p, "on", 2) || !strncasecmp(p, "unpark", 6);
	if (!off && !on)
		return E_INVAL;
	while (*p && *p != ' ')
		p++;
	int id = (int)strtol(p, NULL, 10);
	if (cpu_park(id, off))
		return E_INVAL;
	return (long)n;
}

static struct chardev cpu_dev = { "CPU$", cpu_read, cpu_write, NULL, 1 };

void cpuinfo_init(void)
{
	dev_register(&cpu_dev);
}
