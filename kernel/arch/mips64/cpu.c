/*
 * cpu.c - the MIPS64 arch layer: from PMON's hand-off to a running kernel.
 *
 * Loongson 2E (QEMU's Fuloong 2E) and 2F (the Lemote Yeeloong) both put a
 * PC's ISA devices - the 16550 serial port, the MC146818 clock, the i8042
 * - in the Bonito bridge's I/O window at physical 0x1fd00000, so ports work
 * as on a PC, just memory-mapped. The 1 kHz tick comes from CP0
 * Count/Compare, measured against the real-time clock at boot.
 */
#include "../../kernel.h"
#include <bootinfo.h>

#define ISA_IO		0xffffffffbfd00000ULL	/* KSEG1: uncached */
#define ST_IE		0x00000001
#define ST_EXL		0x00000002
#define ST_KX		0x00000080
#define ST_IM7		0x00008000		/* the CP0 timer */
#define ST_FR		0x04000000
#define ST_CU0		0x10000000
#define ST_CU1		0x20000000
#define ST_BEV		0x00400000

static inline uint8_t inb(uint16_t port) { return *(volatile uint8_t *)(ISA_IO + port); }
static inline void outb(uint16_t port, uint8_t v) { *(volatile uint8_t *)(ISA_IO + port) = v; mmio_barrier(); }

extern struct cpu cpus[];
struct cpu *this_cpu(void) { return &cpus[0]; }
static struct bdos_bootinfo boot __attribute__((section(".data.boot")));	/* kmain clears .bss */
static uint32_t count_per_ms;


void kmain(struct bdos_bootinfo *bi);
void timer_tick(void);
void sched_tick(void);
void sched_preempt_check(void);
void sched_kick(void);
void task_exit(void);
extern char exc_stub[], exc_stub_end[], __kernel_end[], _start[];
void arch_task_trampoline(void);

/* ---- the counter -------------------------------------------------------------------- */

static inline uint32_t count(void)
{
	uint32_t v;
	__asm__ volatile("mfc0 %0, $9" : "=r"(v));
	return v;
}

static inline void set_compare(uint32_t v)
{
	__asm__ volatile("mtc0 %0, $11; nop; nop" : : "r"(v));
}

/* Count is 32 bits and wraps in seconds: widen it (single core) */
uint64_t rdtsc(void)
{
	static uint64_t hi;
	static uint32_t last;
	uint64_t f = irq_save();
	uint32_t c = count();
	if (c < last)
		hi += 1ULL << 32;
	last = c;
	uint64_t v = hi | c;
	irq_restore(f);
	return v;
}

/* ---- serial: the 16550 at 3F8h ---------------------------------------------------- */

void serial_init(void)
{
	outb(0x3F8 + 1, 0x00);
	outb(0x3F8 + 3, 0x80);
	outb(0x3F8 + 0, 0x01);	/* 115200 */
	outb(0x3F8 + 1, 0x00);
	outb(0x3F8 + 3, 0x03);
	outb(0x3F8 + 2, 0xC7);
	outb(0x3F8 + 4, 0x03);
}

void serial_putc(char c)
{
	for (int i = 0; i < 100000 && !(inb(0x3F8 + 5) & 0x20); i++)
		;
	outb(0x3F8, (uint8_t)c);
}

int serial_getc(void)
{
	return (inb(0x3F8 + 5) & 1) ? inb(0x3F8) : -1;
}

/* ---- the clock ------------------------------------------------------------------------- */

static uint8_t cmos(int r)
{
	outb(0x70, (uint8_t)r);
	return inb(0x71);
}

static int bcd(int v, int binary) { return binary ? v : (v >> 4) * 10 + (v & 15); }

void arch_rtc(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
	while (cmos(0x0A) & 0x80)
		;
	int b = cmos(0x0B) & 4;
	*s = bcd(cmos(0), b);
	*mi = bcd(cmos(2), b);
	int hr = cmos(4);
	*h = bcd(hr & 0x7F, b);
	*d = bcd(cmos(7), b);
	*mo = bcd(cmos(8), b);
	*y = 2000 + bcd(cmos(9), b);
}

void arch_rtc_set(int y, int mo, int d, int h, int mi, int s)
{
	(void)y; (void)mo; (void)d; (void)h; (void)mi; (void)s;
}

/* how fast Count runs: count across one RTC second */
static uint32_t measure_count(void)
{
	int s0 = cmos(0);
	uint32_t guard = count();
	while (cmos(0) == s0 && count() - guard < 0x7FFFFFFF)
		;
	uint32_t a = count();
	int s1 = cmos(0);
	while (cmos(0) == s1 && count() - a < 0x7FFFFFFF)
		;
	return count() - a;
}

/* ---- exceptions and interrupts ---------------------------------------------------------- */

struct frame {
	uint64_t r[32];
	uint64_t hi, lo, epc, status, cause, badva;
};

static const char *const exc_names[32] = {
	"Interrupt", "TLB modified", "TLB miss (load)", "TLB miss (store)", "Address error (load)",
	"Address error (store)", "Bus error (fetch)", "Bus error (data)", "Syscall", "Breakpoint",
	"Reserved instruction", "Coprocessor unusable", "Overflow", "Trap", NULL, "Floating point",
};

void mips_trap(struct frame *f)
{
	int code = (int)(f->cause >> 2) & 31;
	if (code == 0) {
		uint32_t pending = (uint32_t)(f->cause & f->status & 0xFF00);
		if (pending & ST_IM7) {
			set_compare(count() + count_per_ms);
			timer_tick();
			sched_tick();
		}
		if (pending & ~ST_IM7)
			sched_kick();
		sched_preempt_check();
		return;
	}
	console_panic_mode();
	kprintf("\n*** %s (exception %d) in task '%s'\n", exc_names[code] ? exc_names[code] : "Exception", code,
	    task_current() ? task_current()->name : "-");
	kprintf("    EPC=%p BadVAddr=%p RA=%p SP=%p\n", (void *)f->epc, (void *)f->badva, (void *)f->r[31],
	    (void *)f->r[29]);
	panic("unhandled CPU exception");
}

void irq_register(int irq, void (*fn)(void))
{
	(void)irq;
	(void)fn;	/* the 8259s behind the bridge aren't routed yet: drivers poll */
}

void arch_init(const void *bi)
{
	(void)bi;
	/* the exception vectors (BEV off): TLB refill, XTLB refill, cache error, general */
	static const uint64_t vec[] = { 0xffffffff80000000ULL, 0xffffffff80000080ULL, 0xffffffff80000180ULL };
	size_t n = (size_t)(exc_stub_end - exc_stub);
	for (size_t i = 0; i < NELEM(vec); i++)
		memcpy((void *)vec[i], exc_stub, n);
	/* the caches: write the vectors back and drop stale lines (Loongson 2: 32-byte lines, 64 KB) */
	for (uint64_t a = 0xffffffff80000000ULL; a < 0xffffffff80000400ULL; a += 32)
		__asm__ volatile("cache 0x19, 0(%0); cache 0x10, 0(%0)" : : "r"(a) : "memory");
	cp0_set_status(ST_CU0 | ST_CU1 | ST_FR | ST_KX | ST_IM7);
	__asm__ volatile("ctc1 $zero, $31");
	count_per_ms = measure_count() / 1000;
	if (count_per_ms < 1000)
		count_per_ms = 50000;
	set_compare(count() + count_per_ms);
}

/* ---- tasks ---------------------------------------------------------------------------------- */

void arch_task_setup(struct arch_context *ctx, void *stack_top, void (*entry)(void))
{
	uint64_t *sp = (uint64_t *)(((uint64_t)stack_top & ~15ULL) - 112);
	memset(sp, 0, 112);
	sp[0] = (uint64_t)entry;			/* s0 */
	sp[9] = (uint64_t)arch_task_trampoline;		/* ra */
	sp[11] = 0;					/* Status: interrupts off till task_start */
	ctx->sp = (uint64_t)sp;
}

void arch_cpu_local(struct cpu *c) { (void)c; }
int arch_smp_init(void) { return 1; }
void arch_halt_others(void) {}
void arch_kick_cpu(int cpu) { (void)cpu; }
void arch_set_kstack(void *top) { (void)top; }

void arch_cpu_name(char *out, int n)
{
	uint32_t prid;
	__asm__ volatile("mfc0 %0, $15" : "=r"(prid));
	const char *name = (prid & 0xFFFF) == 0x6302 ? "Loongson 2E" : (prid & 0xFFFF) == 0x6303 ? "Loongson 2F"
	    : "MIPS64";
	snprintf(out, (size_t)n, "%s (PRId %04x), Count at %u MHz", name, prid & 0xFFFF,
	    count_per_ms / 1000);
}

void arch_speaker(uint32_t hz) { (void)hz; }

/* ---- no user mode ---------------------------------------------------------------------------- */

void *arch_as_new(void) { return NULL; }
int arch_as_map(void *as, uint64_t va, uint64_t pa, int flags) { (void)as; (void)va; (void)pa; (void)flags; return -1; }
uint64_t arch_as_lookup(void *as, uint64_t va) { (void)as; (void)va; return 0; }
uint64_t arch_as_unmap(void *as, uint64_t va) { (void)as; (void)va; return 0; }
void arch_as_free(void *as) { (void)as; }
void arch_as_switch(void *as) { (void)as; }
void arch_set_fs_base(uint64_t base) { (void)base; }
void arch_syscall_init(void) {}
_Noreturn void arch_enter_user(uint64_t rip, uint64_t rsp, uint64_t a0, uint64_t a1)
{
	(void)rip; (void)rsp; (void)a0; (void)a1;
	panic("no user mode on MIPS");
}

void *arch_map_mmio(uint64_t pa, uint64_t size)
{
	(void)size;
	return (void *)(0x9000000000000000ULL | pa);	/* XKPHYS, uncached */
}

/* ---- PCI: the Bonito's configuration window ----------------------------------------------- */

#define BONITO_REG(o)	(*(volatile uint32_t *)(0xffffffffbfe00000ULL + (o)))
#define BONITO_PCIMAP_CFG	0x118
#define PCI_CFG_SPACE	0xffffffffbfe80000ULL

/* Not switched on yet: the slot-to-IDSEL wiring differs between boards and
 * hasn't been checked, and a wrong guess is a bus error. */
static int pci_on;

uint32_t arch_pci_read32(int bus, int slot, int func, uint8_t off)
{
	if (!pci_on || bus || slot < 1 || slot > 20)
		return 0xFFFFFFFF;
	/* type 0: an IDSEL line per slot, through the 64 KB window */
	uint32_t addr = (1u << (slot + 11)) | (uint32_t)func << 8 | (off & 0xFC);
	BONITO_REG(BONITO_PCIMAP_CFG) = addr >> 16;
	mmio_barrier();
	return *(volatile uint32_t *)(PCI_CFG_SPACE + (addr & 0xFFFF));
}

void arch_pci_write32(int bus, int slot, int func, uint8_t off, uint32_t v)
{
	if (bus || slot < 1 || slot > 20)
		return;
	uint32_t addr = (1u << (slot + 11)) | (uint32_t)func << 8 | (off & 0xFC);
	BONITO_REG(BONITO_PCIMAP_CFG) = addr >> 16;
	mmio_barrier();
	*(volatile uint32_t *)(PCI_CFG_SPACE + (addr & 0xFFFF)) = v;
}

/* ---- the keyboard: the i8042 (Yeeloong: the EC's), polled for now ------------------------- */

void ps2_init(void) {}
int ps2_mouse_present(void) { return 0; }
int input_mods(void) { return 0; }
void keyboard_scancode(uint8_t sc) { (void)sc; }

/* ---- stopping ---------------------------------------------------------------------------------- */

_Noreturn void arch_halt(void)
{
	irq_disable();
	for (;;)
		__asm__ volatile("wait");
}

_Noreturn void arch_reboot(void)
{
	/* the 8042's reset line, where there is one */
	outb(0x64, 0xFE);
	arch_halt();
}

/* ---- from PMON to kmain -------------------------------------------------------------------- */

static uint64_t parse_num(const char *s)
{
	uint64_t v = 0;
	int base = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	for (;; s++) {
		int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10
		    : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : 99;
		if (d >= base)
			return v;
		v = v * (uint64_t)base + (uint64_t)d;
	}
}

/* look for name=value in a string of words */
static int find(const char *s, const char *name, uint64_t *out)
{
	size_t n = strlen(name);
	for (const char *p = s; *p; p++)
		if ((p == s || p[-1] == ' ') && !strncmp(p, name, n) && p[n] == '=') {
			*out = parse_num(p + n + 1);
			return 1;
		}
	return 0;
}

void mips_start(long argc, int32_t *argv, int32_t *envp)
{
	serial_init();
	uint64_t mem_mb = 256, rd_start = 0, rd_size = 0, v;
	for (long i = 0; i < argc && argv; i++) {
		const char *a = (const char *)(intptr_t)argv[i];
		if (find(a, "rd_start", &v))
			rd_start = v;
		if (find(a, "rd_size", &v))
			rd_size = v;
	}
	for (int i = 0; envp && envp[i] && i < 64; i++) {
		const char *e = (const char *)(intptr_t)envp[i];
		if (find(e, "memsize", &v))
			mem_mb = v;
		if (find(e, "rd_start", &v))
			rd_start = v;
		if (find(e, "rd_size", &v))
			rd_size = v;
	}
	if (mem_mb > 256)
		mem_mb = 256;		/* what KSEG0 reaches below the I/O hole */
	boot.magic = BDOS_BOOT_MAGIC;
	uint64_t kend = (V2P(__kernel_end) + 0xFFFF) & ~0xFFFFULL;
	uint64_t rd_pa = rd_start ? (rd_start >= ARCH_KVA_OFFSET ? V2P(rd_start) : rd_start & 0x1FFFFFFF) : 0;
	int n = 0;
	boot.ranges[n++] = (struct bdos_memrange){ 0, kend / PAGE_SIZE, BDOS_MEM_LOADER, 0 };
	if (rd_pa && rd_size) {
		uint64_t a = rd_pa & ~0xFFFULL, b = (rd_pa + rd_size + 0xFFF) & ~0xFFFULL;
		if (a > kend)
			boot.ranges[n++] = (struct bdos_memrange){ kend, (a - kend) / PAGE_SIZE, BDOS_MEM_FREE, 0 };
		boot.ranges[n++] = (struct bdos_memrange){ a, (b - a) / PAGE_SIZE, BDOS_MEM_LOADER, 0 };
		boot.ranges[n++] = (struct bdos_memrange){ b, ((mem_mb << 20) - b) / PAGE_SIZE, BDOS_MEM_FREE, 0 };
		boot.rootfs_base = rd_pa;
		boot.rootfs_size = rd_size;
	} else {
		boot.ranges[n++] = (struct bdos_memrange){ kend, ((mem_mb << 20) - kend) / PAGE_SIZE, BDOS_MEM_FREE, 0 };
	}
	boot.nranges = (uint32_t)n;
	boot.kernel_base = V2P(_start);
	boot.kernel_size = V2P(__kernel_end) - V2P(_start);
	kmain(&boot);
}
void (*user_trap)(void *tf);	/* native programs: not on MIPS */
