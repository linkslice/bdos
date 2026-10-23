/* main.c - bdos kernel entry */
#include "kernel.h"
#include "vfs.h"
#include "sysapi.h"

const struct bdos_bootinfo *bootinfo;
extern char __bss_start[], __bss_end[];

/* Quick checks that the foundations hold before anything relies on them. */
static volatile int pingpong;

static void pong(void *arg)
{
	(void)arg;
	for (int i = 0; i < 3; i++) {
		pingpong++;
		task_yield();
	}
}

static void selftest(void)
{
	/* heap: every size class and a multi-page block */
	void *p[12];
	uint64_t before = heap_in_use();
	for (int i = 0; i < 12; i++) {
		p[i] = kmalloc((size_t)16 << i);
		if (!p[i])
			panic("selftest: kmalloc(%d) failed", 16 << i);
		memset(p[i], 0xA5, (size_t)16 << i);
	}
	for (int i = 0; i < 12; i++)
		kfree(p[i]);
	if (heap_in_use() != before)
		panic("selftest: heap leak (%lu bytes)", heap_in_use() - before);

	/* tasks: two contexts taking turns */
	struct task *t = task_spawn("selftest", pong, NULL, NULL);
	for (int i = 0; i < 3; i++)
		task_yield();
	task_wait(t);
	if (pingpong != 3)
		panic("selftest: task switching (%d)", pingpong);

	/* timer interrupts */
	uint64_t t0 = uptime_ms();
	sleep_ms(20);
	if (uptime_ms() - t0 < 20)
		panic("selftest: timer not ticking");
}

void kernel_late_init(void)
{
	sysapi_init();
	extern void cpuinfo_init(void), proc_init(void);
	cpuinfo_init();
	proc_init();
	storage_init();
	extern void settings_init(void);
	settings_init();
	input_settings_init();
	extern void desktop_init(void);
	desktop_init();
	extern void audio_init(void), xhci_init(void);
	xhci_init();
	audio_init();
	extern void net_init(void), remote_init(void), inet_init(void);
	remote_init();
	inet_init();
	net_init();
	extern void ntp_init(void);
	ntp_init();
}

void kmain(struct bdos_bootinfo *bi)
{
	memset(__bss_start, 0, (size_t)(__bss_end - __bss_start));
	bootinfo = bi;
	serial_init();
	if (bi->magic != BDOS_BOOT_MAGIC)
		panic("bad boot info from the loader");

	arch_init(bi);		/* our GDT, IDT, page tables, PIC, timer */
	screen_init(&bi->fb);
	pmm_init(bi);
	task_init();
	irq_enable();
	ps2_init();
	selftest();
	time_calibrate();
	int cores = arch_smp_init();

	char total[32];
	fmt_commas(total, sizeof total, pmm_total() / 1024);
	kprintf("%s %s (%s) - %s\n", BDOS_NAME, BDOS_VERSION, ARCH_NAME, BDOS_LONGNAME);
	if (bi->fb.base)
		kprintf("%s K memory, %d core%s, %ux%u display%s\n\n", total, cores, cores == 1 ? "" : "s",
		    bi->fb.width, bi->fb.height, ps2_mouse_present() ? ", mouse" : "");
	else
		kprintf("%s K memory, %d core%s, serial console\n\n", total, cores, cores == 1 ? "" : "s");

	kernel_late_init();

	/* no screen (a MIPS box on its serial port): the shell lives on COM1 */
	struct console *home = bi->fb.base ? screen_console() : serial_console();
	struct task *sh = task_spawn("COMMAND", shell_task, NULL, home);
	for (;;) {
		task_wait(sh);
		/* EXIT from the top-level shell starts a fresh one, like DOS. */
		sh = task_spawn("COMMAND", shell_task, NULL, home);
	}
}
