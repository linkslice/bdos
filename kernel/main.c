/* main.c - bdos kernel entry */
#include "kernel.h"

const struct bdos_bootinfo *bootinfo;
extern char __bss_start[], __bss_end[];

void kmain(struct bdos_bootinfo *bi)
{
	/* The loader zeroes .bss, but don't depend on it. */
	memset(__bss_start, 0, (size_t)(__bss_end - __bss_start));

	bootinfo = bi;
	serial_init();
	if (bi->magic != BDOS_BOOT_MAGIC)
		panic("bad boot info from the loader");

	cpu_init();
	con_init(&bi->fb);
	mem_init(bi);
	kbd_init();
	rd_init();

	char total[32];
	fmt_commas(total, sizeof total, mem_total() / 1024);
	kprintf("%s %s (%s) - %s\n", BDOS_NAME, BDOS_VERSION, BDOS_ARCH, BDOS_LONGNAME);
	kprintf("%s K memory, %ux%u display\n\n", total, bi->fb.width, bi->fb.height);

	shell_run(bi);
}
