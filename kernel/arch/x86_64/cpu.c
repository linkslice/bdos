/*
 * cpu.c - x86-64 CPU setup: GDT, IDT, page tables, PAT, 8259 PIC,
 * 8254 PIT timer, local APIC pass-through, task contexts, reboot, RTC.
 */
#include "../../kernel.h"

/* ---- GDT ---------------------------------------------------------------- */

struct desc_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

#define KCODE 0x08
#define KDATA 0x10
/* user selectors, in the order SYSRET wants: 0x18 32-bit code (unused),
 * 0x20 data, 0x28 64-bit code; then the TSS */
#define TSS_SEL 0x30

struct tss {
	uint32_t res0;
	uint64_t rsp0, rsp1, rsp2;
	uint64_t res1;
	uint64_t ist[7];
	uint64_t res2;
	uint16_t res3;
	uint16_t iomap;
} __attribute__((packed));

/* each core's own GDT and TSS (the TSS says which stack interrupts from
 * user mode use, and that's per core) */
struct arch_cpu {
	uint64_t gdt[8];
	struct tss tss;
	uint8_t df_stack[8192] __attribute__((aligned(16)));	/* double faults */
};
static struct arch_cpu arch_cpus[MAX_CPUS];

static void gdt_load(struct arch_cpu *a)
{
	a->gdt[0] = 0;
	a->gdt[1] = 0x00AF9A000000FFFFULL;	/* 0x08: 64-bit code, ring 0 */
	a->gdt[2] = 0x00CF92000000FFFFULL;	/* 0x10: data, ring 0 */
	a->gdt[3] = 0x00CFFA000000FFFFULL;	/* 0x18: 32-bit code, ring 3 */
	a->gdt[4] = 0x00CFF2000000FFFFULL;	/* 0x20: data, ring 3 */
	a->gdt[5] = 0x00AFFA000000FFFFULL;	/* 0x28: 64-bit code, ring 3 */
	uint64_t base = (uint64_t)&a->tss, limit = sizeof a->tss - 1;
	a->gdt[6] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
	    (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
	a->gdt[7] = base >> 32;
	a->tss.iomap = sizeof a->tss;		/* no I/O permission map */
	a->tss.ist[0] = (uint64_t)(a->df_stack + sizeof a->df_stack);
	struct desc_ptr gp = { sizeof a->gdt - 1, (uint64_t)a->gdt };
	__asm__ volatile(
	    "lgdt %0\n\t"
	    "pushq %1\n\t"
	    "leaq 1f(%%rip), %%rax\n\t"
	    "pushq %%rax\n\t"
	    "lretq\n"
	    "1:\n\t"
	    "mov %2, %%ax\n\t"
	    "mov %%ax, %%ds\n\t"
	    "mov %%ax, %%es\n\t"
	    "mov %%ax, %%ss\n\t"
	    "xor %%eax, %%eax\n\t"
	    "mov %%ax, %%fs\n\t"
	    "mov %%ax, %%gs\n\t"
	    :
	    : "m"(gp), "i"(KCODE), "i"(KDATA)
	    : "rax", "memory");
	__asm__ volatile("ltr %w0" : : "r"(TSS_SEL));
}

void arch_cpu_local(struct cpu *c)
{
	struct arch_cpu *a = &arch_cpus[c->id];
	c->self = c;
	c->arch = a;
	gdt_load(a);
	wrmsr(0xC0000101, (uint64_t)c);		/* GS base */
	wrmsr(0xC0000102, 0);			/* kernel GS base (for swapgs) */
}

void arch_set_kstack(void *top)
{
	struct cpu *c = this_cpu();
	struct arch_cpu *a = c->arch;
	a->tss.rsp0 = (uint64_t)top;
	c->kstack = (uint64_t)top;
}

uint64_t *arch_kernel_pml4(void);

/* ---- IDT and trap dispatch ---------------------------------------------- */

struct idt_entry {
	uint16_t off_lo;
	uint16_t sel;
	uint8_t ist;
	uint8_t type;
	uint16_t off_mid;
	uint32_t off_hi;
	uint32_t zero;
} __attribute__((packed));

struct trapframe {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t vector, error;
	uint64_t rip, cs, rflags, rsp, ss;
};

static struct idt_entry idt[256];
extern const uint64_t isr_table[48];
extern const uint64_t isr_spurious, isr_lapic[3];
static void (*irq_handlers[16])(void);

static void idt_set(int vec, uint64_t addr)
{
	idt[vec].off_lo = addr & 0xFFFF;
	idt[vec].sel = KCODE;
	idt[vec].ist = vec == 8 ? 1 : 0;	/* double fault: a known-good stack */
	idt[vec].type = 0x8E;	/* present, ring 0, interrupt gate */
	idt[vec].off_mid = (addr >> 16) & 0xFFFF;
	idt[vec].off_hi = addr >> 32;
}

static const char *const exc_names[32] = {
	"Divide error", "Debug", "NMI", "Breakpoint", "Overflow",
	"Bound range exceeded", "Invalid opcode", "Device not available",
	"Double fault", "Coprocessor segment overrun", "Invalid TSS",
	"Segment not present", "Stack fault", "General protection fault",
	"Page fault", "Reserved", "x87 floating point", "Alignment check",
	"Machine check", "SIMD floating point", "Virtualization",
	"Control protection", "Reserved", "Reserved", "Reserved", "Reserved",
	"Reserved", "Reserved", "Hypervisor injection", "VMM communication",
	"Security", "Reserved",
};

#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1

void lapic_eoi(void);
void (*user_trap)(void *tf);	/* native programs' faults */
void proc_check(void);

void trap_dispatch(struct trapframe *tf)
{
	uint64_t v = tf->vector;

	if (v == VEC_TIMER) {			/* this core's own timer */
		lapic_eoi();
		sched_tick();
		sched_preempt_check();
		if (tf->cs & 3)
			proc_check();
		return;
	}
	if (v == VEC_KICK) {			/* just a wake-up */
		lapic_eoi();
		sched_preempt_check();
		return;
	}
	if (v == VEC_HALT)			/* someone panicked */
		arch_halt();

	if (v >= 32 && v < 48) {
		int irq = (int)v - 32;
		/* Spurious IRQ7/15: the PIC raised and withdrew; no EOI for the line. */
		if (irq == 7 || irq == 15) {
			outb(irq == 7 ? PIC1_CMD : PIC2_CMD, 0x0B);
			uint8_t isr = inb(irq == 7 ? PIC1_CMD : PIC2_CMD);
			if (!(isr & 0x80)) {
				if (irq == 15)
					outb(PIC1_CMD, 0x20);
				return;
			}
		}
		if (irq_handlers[irq])
			irq_handlers[irq]();
		if (irq >= 8)
			outb(PIC2_CMD, 0x20);
		outb(PIC1_CMD, 0x20);
		if (irq != IRQ_TIMER)
			sched_kick();		/* something happened: pollers, look */
		sched_preempt_check();
		if (tf->cs & 3)
			proc_check();
		return;
	}
	if ((tf->cs & 3) && user_trap) {	/* a native program's fault */
		user_trap(tf);
		return;
	}
	if (v == 255)
		return;	/* LAPIC spurious: no EOI */

	uint64_t cr2;
	__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
	console_panic_mode();
	kprintf("\n*** %s (exception %lu, error %lx) in task '%s'\n",
	    exc_names[v & 31], v, tf->error, task_current() ? task_current()->name : "-");
	kprintf("    RIP=%p RSP=%p CR2=%p\n", (void *)tf->rip, (void *)tf->rsp, (void *)cr2);
	kprintf("    RAX=%p RBX=%p RCX=%p RDX=%p\n", (void *)tf->rax, (void *)tf->rbx,
	    (void *)tf->rcx, (void *)tf->rdx);
	kprintf("    RSI=%p RDI=%p RBP=%p\n", (void *)tf->rsi, (void *)tf->rdi, (void *)tf->rbp);
	panic("unhandled CPU exception");
}

void irq_register(int irq, void (*fn)(void))
{
	irq_handlers[irq] = fn;
	uint16_t port = irq < 8 ? PIC1_DAT : PIC2_DAT;
	outb(port, inb(port) & ~(1 << (irq & 7)));
	if (irq >= 8)
		outb(PIC1_DAT, inb(PIC1_DAT) & ~(1 << 2));	/* cascade */
}

/* ---- Paging ------------------------------------------------------------- */

/*
 * Identity map everything with 2 MiB pages: RAM write-back, the
 * framebuffer write-combining, and everything else (MMIO) uncached.
 * Tables come from .bss so we never stand on firmware memory.
 */
#define MAX_GB 64
#define PTE_P	(1ULL << 0)
#define PTE_W	(1ULL << 1)
#define PTE_PWT	(1ULL << 3)
#define PTE_PCD	(1ULL << 4)
#define PTE_PS	(1ULL << 7)

static uint64_t pml4[512] __attribute__((aligned(4096)));

uint64_t *arch_kernel_pml4(void)
{
	return pml4;
}
static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t pds[MAX_GB][512] __attribute__((aligned(4096)));

static void pat_init(void)
{
	/* PAT entry 1 becomes write-combining (default is write-through). */
	uint64_t pat = rdmsr(0x277);
	pat = (pat & ~(0xFFULL << 8)) | (0x01ULL << 8);
	wrmsr(0x277, pat);
}

static int range_is_ram(const struct bdos_bootinfo *bi, uint64_t lo, uint64_t hi)
{
	for (uint32_t i = 0; i < bi->nranges; i++) {
		const struct bdos_memrange *r = &bi->ranges[i];
		uint64_t a = r->base, b = r->base + r->pages * 4096;
		if (r->type == BDOS_MEM_RESERVED)
			continue;
		if (a < hi && b > lo)
			return 1;
	}
	return 0;
}

/* MMIO above what the identity map covers (64-bit BARs, often far up): mapped
 * into a window at 508 GB, 2 MB pages, uncached */
#define MMIO_WINDOW_GB 508
static uint64_t mmio_pd[512] __attribute__((aligned(4096)));
static int mmio_next;
static uint64_t identity_top;

void *arch_map_mmio(uint64_t pa, uint64_t size)
{
	if (pa + size <= identity_top)
		return (void *)pa;
	uint64_t base = pa & ~((2ULL << 20) - 1), end = pa + size;
	int n = (int)((end - base + (2ULL << 20) - 1) >> 21);
	if (mmio_next + n > 512)
		return NULL;
	/* already mapped? */
	for (int i = 0; i + n <= mmio_next; i++)
		if ((mmio_pd[i] & ~0xFFFULL & ~((2ULL << 20) - 1)) == base)
			return (void *)(((uint64_t)MMIO_WINDOW_GB << 30) + ((uint64_t)i << 21) + (pa - base));
	int at = mmio_next;
	for (int i = 0; i < n; i++)
		mmio_pd[at + i] = (base + ((uint64_t)i << 21)) | PTE_P | PTE_W | PTE_PS | PTE_PCD | PTE_PWT;
	mmio_next += n;
	pdpt[MMIO_WINDOW_GB] = (uint64_t)mmio_pd | PTE_P | PTE_W;
	uint64_t cr3;
	__asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
	return (void *)(((uint64_t)MMIO_WINDOW_GB << 30) + ((uint64_t)at << 21) + (pa - base));
}

static void paging_init(const struct bdos_bootinfo *bi)
{
	pat_init();

	uint64_t top = 4ULL << 30;	/* always cover the 32-bit MMIO hole */
	for (uint32_t i = 0; i < bi->nranges; i++) {
		uint64_t end = bi->ranges[i].base + bi->ranges[i].pages * 4096;
		if (bi->ranges[i].type != BDOS_MEM_RESERVED && end > top)
			top = end;
	}
	uint64_t fb_end = bi->fb.base + bi->fb.size;
	if (fb_end > top)
		top = fb_end;
	int gbs = (int)((top + (1ULL << 30) - 1) >> 30);
	if (gbs > MAX_GB)
		gbs = MAX_GB;

	for (int g = 0; g < gbs; g++) {
		for (int i = 0; i < 512; i++) {
			uint64_t pa = ((uint64_t)g << 30) | ((uint64_t)i << 21);
			uint64_t flags = PTE_P | PTE_W | PTE_PS;
			if (pa < fb_end && pa + (2 << 20) > bi->fb.base)
				flags |= PTE_PWT;		/* PAT index 1: WC */
			else if (!range_is_ram(bi, pa, pa + (2 << 20)))
				flags |= PTE_PCD | PTE_PWT;	/* uncached MMIO */
			pds[g][i] = pa | flags;
		}
		pdpt[g] = (uint64_t)pds[g] | PTE_P | PTE_W;
	}
	identity_top = (uint64_t)gbs << 30;
	pml4[0] = (uint64_t)pdpt | PTE_P | PTE_W;
	__asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)pml4) : "memory");
}

/* ---- Interrupt controllers and timer ------------------------------------ */

static void pic_init(void)
{
	outb(PIC1_CMD, 0x11); io_wait();	/* ICW1: init, expect ICW4 */
	outb(PIC2_CMD, 0x11); io_wait();
	outb(PIC1_DAT, 32); io_wait();		/* ICW2: vector offsets */
	outb(PIC2_DAT, 40); io_wait();
	outb(PIC1_DAT, 4); io_wait();		/* ICW3: slave on IRQ2 */
	outb(PIC2_DAT, 2); io_wait();
	outb(PIC1_DAT, 0x01); io_wait();	/* ICW4: 8086 mode */
	outb(PIC2_DAT, 0x01); io_wait();
	outb(PIC1_DAT, 0xFF);			/* everything masked for now */
	outb(PIC2_DAT, 0xFF);
}

/*
 * The firmware leaves the local APIC on. Route the 8259's output through
 * LINT0 as ExtINT so legacy IRQs reach the CPU, and enable the APIC with
 * a spurious vector of 255.
 */
static void lapic_passthrough(void)
{
	uint64_t base = rdmsr(0x1B);
	if (!(base & (1 << 11)))
		return;		/* APIC disabled: the PIC drives INTR directly */
	volatile uint32_t *lapic = (volatile uint32_t *)(base & ~0xFFFULL);
	lapic[0xF0 / 4] = 0x100 | 0xFF;		/* enable, spurious vector 255 */
	lapic[0x350 / 4] = 0x700;		/* LINT0: ExtINT, unmasked */
	lapic[0x360 / 4] = 0x400;		/* LINT1: NMI */
	lapic[0x320 / 4] = 0x10000;		/* LAPIC timer masked */
	lapic[0x80 / 4] = 0;			/* task priority: accept all */
}

#define PIT_HZ 1193182
#define TICK_HZ 1000

static void timer_irq(void)
{
	timer_tick();
	sched_tick();
}

static void pit_init(void)
{
	uint16_t div = PIT_HZ / TICK_HZ;
	outb(0x43, 0x34);	/* channel 0, lo/hi, rate generator */
	outb(0x40, div & 0xFF);
	outb(0x40, div >> 8);
	irq_register(IRQ_TIMER, timer_irq);
}

/* SSE for the language interpreters (the rest of the kernel never uses it) */
static void fpu_init(void)
{
	uint64_t cr0, cr4;
	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
	cr0 &= ~(1ULL << 2);	/* EM off: no x87 emulation */
	cr0 |= 1ULL << 1;	/* MP on */
	cr0 &= ~(1ULL << 3);	/* TS off */
	__asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= (1ULL << 9) | (1ULL << 10);	/* OSFXSR, OSXMMEXCPT */
	__asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
	__asm__ volatile("fninit");
	uint32_t mxcsr = 0x1F80;	/* all exceptions masked, round to nearest */
	__asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

static void idt_load(void)
{
	struct desc_ptr ip = { sizeof idt - 1, (uint64_t)idt };
	__asm__ volatile("lidt %0" : : "m"(ip));
}

/* what every extra core needs before it can run tasks */
void arch_ap_setup(struct cpu *c)
{
	arch_cpu_local(c);
	idt_load();
	pat_init();
	fpu_init();
	arch_syscall_init();
}

uint64_t arch_cr3(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr3, %0" : "=r"(v));
	return v;
}

void arch_init(const void *bootinfo_ptr)
{
	const struct bdos_bootinfo *bi = bootinfo_ptr;
	cpus[0].id = 0;
	arch_cpu_local(&cpus[0]);
	fpu_init();
	for (int i = 0; i < 48; i++)
		idt_set(i, isr_table[i]);
	idt_set(255, isr_spurious);
	idt_set(VEC_TIMER, isr_lapic[0]);
	idt_set(VEC_KICK, isr_lapic[1]);
	idt_set(VEC_HALT, isr_lapic[2]);
	idt_load();
	paging_init(bi);
	pic_init();
	lapic_passthrough();
	pit_init();
}

/* ---- Tasks -------------------------------------------------------------- */

extern void arch_task_trampoline(void);

void arch_task_setup(struct arch_context *ctx, void *stack_top, void (*entry)(void))
{
	/* After arch_switch's ret, rsp is 16-aligned in the trampoline, so its
	 * call leaves rsp = 8 mod 16 at entry, as the SysV ABI requires. */
	uint64_t *sp = (uint64_t *)((uint64_t)stack_top & ~15ULL);
	*--sp = (uint64_t)arch_task_trampoline;	/* return address */
	*--sp = 0;				/* rbp */
	*--sp = 0;				/* rbx */
	*--sp = (uint64_t)entry;		/* r12 */
	*--sp = 0;				/* r13 */
	*--sp = 0;				/* r14 */
	*--sp = 0;				/* r15 */
	*--sp = 0x002;				/* rflags: interrupts off until trampoline */
	ctx->rsp = (uint64_t)sp;
}

/* ---- Reboot and clock --------------------------------------------------- */

_Noreturn void arch_halt(void)
{
	for (;;)
		__asm__ volatile("cli; hlt");
}

uint32_t arch_pci_read32(int bus, int slot, int func, uint8_t off)
{
	outl(0xCF8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)slot << 11 |
	    (uint32_t)func << 8 | (off & 0xFC));
	return inl(0xCFC);
}

void arch_pci_write32(int bus, int slot, int func, uint8_t off, uint32_t v)
{
	outl(0xCF8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)slot << 11 |
	    (uint32_t)func << 8 | (off & 0xFC));
	outl(0xCFC, v);
}

_Noreturn void arch_reboot(void)
{
	irq_disable();
	for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
		cpu_pause();
	outb(0x64, 0xFE);		/* keyboard controller reset line */
	outb(0xCF9, 0x06);		/* PCI reset control register */
	struct desc_ptr none = { 0, 0 };
	__asm__ volatile("lidt %0; int3" : : "m"(none));	/* triple fault */
	for (;;)
		__asm__ volatile("hlt");
}

static uint8_t cmos(uint8_t reg)
{
	outb(0x70, reg);
	return inb(0x71);
}

static void cmos_write(uint8_t reg, uint8_t v)
{
	outb(0x70, reg);
	outb(0x71, v);
}

static uint8_t to_bcd(int v, int is_bcd)
{
	return (uint8_t)(is_bcd ? ((v / 10) << 4) | (v % 10) : v);
}

static int bcd(uint8_t v, int is_bcd)
{
	return is_bcd ? (v & 0x0F) + (v >> 4) * 10 : v;
}

/* the CMOS index port is one for everyone: desktops' clocks read it from any core */
static struct spinlock cmos_lock = SPINLOCK_INIT;

void arch_rtc(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
	uint64_t f = spin_lock_irqsave(&cmos_lock);
	while (cmos(0x0A) & 0x80)
		cpu_pause();
	uint8_t regb = cmos(0x0B);
	int b = !(regb & 0x04);
	*s = bcd(cmos(0x00), b);
	*mi = bcd(cmos(0x02), b);
	uint8_t hr = cmos(0x04);
	*h = bcd(hr & 0x7F, b);
	if (!(regb & 0x02) && (hr & 0x80))
		*h = (*h % 12) + 12;
	*d = bcd(cmos(0x07), b);
	*mo = bcd(cmos(0x08), b);
	*y = 2000 + bcd(cmos(0x09), b);
	spin_unlock_irqrestore(&cmos_lock, f);
}

void arch_rtc_set(int y, int mo, int d, int h, int mi, int s)
{
	uint64_t f = spin_lock_irqsave(&cmos_lock);
	while (cmos(0x0A) & 0x80)
		cpu_pause();
	uint8_t regb = cmos(0x0B);
	int b = !(regb & 0x04);
	cmos_write(0x0B, regb | 0x80);		/* hold updates while we write */
	cmos_write(0x00, to_bcd(s, b));
	cmos_write(0x02, to_bcd(mi, b));
	uint8_t hr;
	if (regb & 0x02) {
		hr = to_bcd(h, b);
	} else {			/* 12-hour mode: bit 7 is pm */
		int h12 = h % 12 ? h % 12 : 12;
		hr = (uint8_t)(to_bcd(h12, b) | (h >= 12 ? 0x80 : 0));
	}
	cmos_write(0x04, hr);
	cmos_write(0x07, to_bcd(d, b));
	cmos_write(0x08, to_bcd(mo, b));
	cmos_write(0x09, to_bcd(y % 100, b));
	cmos_write(0x0B, regb & 0x7F);
	spin_unlock_irqrestore(&cmos_lock, f);
}

void arch_cpu_name(char *out, int n)
{
	uint32_t r[12];
	uint32_t a, bx, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(bx), "=c"(c), "=d"(d) : "a"(0x80000000u));
	if (a < 0x80000004u) {
		strncpy0(out, "x86-64 processor", (size_t)n);
		return;
	}
	for (uint32_t i = 0; i < 3; i++)
		__asm__ volatile("cpuid" : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]), "=c"(r[i * 4 + 2]), "=d"(r[i * 4 + 3])
		    : "a"(0x80000002u + i));
	char name[49];
	memcpy(name, r, 48);
	name[48] = 0;
	char *p = name;
	while (*p == ' ')
		p++;
	strncpy0(out, p, (size_t)n);
}

/* ---- the speaker ---------------------------------------------------------- */

void arch_speaker(uint32_t hz)
{
	uint8_t v = inb(0x61);
	if (!hz) {
		outb(0x61, v & 0xFC);
		return;
	}
	uint32_t div = PIT_HZ / hz;
	if (div > 0xFFFF)
		div = 0xFFFF;
	outb(0x43, 0xB6);		/* channel 2, lo/hi, square wave */
	outb(0x42, (uint8_t)div);
	outb(0x42, (uint8_t)(div >> 8));
	outb(0x61, v | 3);
}
