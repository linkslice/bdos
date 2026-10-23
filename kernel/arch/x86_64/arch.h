/*
 * arch.h - x86-64 specifics. Everything outside kernel/arch/ goes through
 * these names, so a port (MIPS64 Yeeloong) supplies its own arch.h.
 */
#ifndef BDOS_ARCH_H
#define BDOS_ARCH_H

#include <stdint.h>

#define ARCH_NAME	"x86-64"
#define PAGE_SIZE	4096UL
#define ARCH_KVA_OFFSET	0ULL		/* physical memory is identity mapped */
#define P2V(pa)		((void *)(uint64_t)(pa))
#define V2P(va)		((uint64_t)(va))

static inline void outb(uint16_t port, uint8_t v)
{
	__asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
	uint8_t v;
	__asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}
static inline void outw(uint16_t port, uint16_t v)
{
	__asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint16_t inw(uint16_t port)
{
	uint16_t v;
	__asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}
static inline void outl(uint16_t port, uint32_t v)
{
	__asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t inl(uint16_t port)
{
	uint32_t v;
	__asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}
static inline void io_wait(void)
{
	outb(0x80, 0);
}
static inline void cpu_pause(void)
{
	__asm__ volatile("pause");
}
static inline void irq_enable(void)
{
	__asm__ volatile("sti" ::: "memory");
}
static inline void irq_disable(void)
{
	__asm__ volatile("cli" ::: "memory");
}
/* Wait for the next interrupt. sti;hlt is atomic, so no wakeup is lost. */
static inline void cpu_idle(void)
{
	__asm__ volatile("sti; hlt" ::: "memory");
}
static inline uint64_t irq_save(void)
{
	uint64_t f;
	__asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
	return f;
}
static inline void irq_restore(uint64_t f)
{
	__asm__ volatile("pushq %0; popfq" : : "r"(f) : "memory", "cc");
}
static inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v)
{
	__asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}
static inline void mmio_barrier(void)
{
	__asm__ volatile("mfence" ::: "memory");
}

/* Saved by arch_switch(): the stack (callee-saved registers are on it) and
 * the x87/SSE state, since a task can be preempted mid-calculation. */
struct arch_context {
	uint64_t rsp;
	uint64_t _pad;
	uint8_t fx[512];	/* fxsave image */
} __attribute__((aligned(16)));
static inline void arch_fpu_clean(void *fx)
{
	__asm__ volatile("fxsave %0" : "=m"(*(uint8_t (*)[512])fx));
}

/* ---- multiprocessor support -------------------------------------------- */
#define VEC_TIMER	0xF0	/* each core's local APIC timer */
#define VEC_KICK	0xF1	/* wake a halted core */
#define VEC_HALT	0xF2	/* stop: another core panicked */

#define MAX_CPUS 64
struct cpu;
/* The current core's struct cpu: GS points at it, and its first word is itself. */
static inline struct cpu *this_cpu(void)
{
	struct cpu *c;
	__asm__ volatile("mov %%gs:0, %0" : "=r"(c));
	return c;
}
void arch_cpu_local(struct cpu *c);	/* this core's GS, GDT and TSS */
int arch_smp_init(void);		/* start the other cores; returns how many run */
void arch_halt_others(void);		/* panic: stop every other core */
void arch_kick_cpu(int cpu);		/* wake a halted core early */

void arch_init(const void *bootinfo);	/* GDT, IDT, paging, PIC, timer */
void arch_switch(struct arch_context *from, struct arch_context *to);
void arch_task_setup(struct arch_context *ctx, void *stack_top,
    void (*entry)(void));
void arch_set_kstack(void *top);	/* where interrupts from user mode land */

/* ---- user mode: address spaces, ring 3, system calls ---------------------- */
#define USER_BASE	0x0000008000000000ULL	/* 512 GB: everything below is the kernel's */
#define USER_TOP	0x00007FFFFFFFF000ULL
#define VM_WRITE	1
#define VM_EXEC		2
void *arch_as_new(void);			/* a new address space, kernel included */
int arch_as_map(void *as, uint64_t va, uint64_t pa, int flags);	/* one 4 KB page */
void *arch_map_mmio(uint64_t pa, uint64_t size);	/* device registers, wherever they are */
uint64_t arch_as_lookup(void *as, uint64_t va);	/* its physical page, or 0 */
uint64_t arch_as_unmap(void *as, uint64_t va);	/* returns the page it held */
void arch_as_free(void *as);			/* page tables and every page mapped */
void arch_as_switch(void *as);			/* NULL: the kernel's own */
void arch_set_fs_base(uint64_t base);
_Noreturn void arch_enter_user(uint64_t rip, uint64_t rsp, uint64_t a0, uint64_t a1);
void arch_syscall_init(void);			/* on every core */

struct sysframe {
	uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, rax;
	uint64_t rip, rflags, rsp;
};
_Noreturn void arch_reboot(void);
_Noreturn void arch_halt(void);		/* interrupts off, stop for good */
/* PCI configuration space (mechanism #1 on PCs; the Bonito bridge on a Yeeloong) */
uint32_t arch_pci_read32(int bus, int slot, int func, uint8_t off);
void arch_pci_write32(int bus, int slot, int func, uint8_t off, uint32_t v);
void arch_rtc(int *y, int *mo, int *d, int *h, int *mi, int *s);
void arch_rtc_set(int y, int mo, int d, int h, int mi, int s);
void arch_cpu_name(char *out, int n);
void arch_speaker(uint32_t hz);		/* the PC speaker: a tone, or 0 for quiet */	/* "Intel(R) Core(TM)..." or similar */

/* Non-local exits for the language interpreters */
struct arch_jmpbuf {
	uint64_t regs[8];
};
int arch_setjmp(struct arch_jmpbuf *jb) __attribute__((returns_twice));
_Noreturn void arch_longjmp(struct arch_jmpbuf *jb, int v);

/* Interrupt lines (legacy PC numbering on x86) */
#define IRQ_TIMER	0
#define IRQ_KBD		1
#define IRQ_MOUSE	12
void irq_register(int irq, void (*fn)(void));

/* Console-adjacent devices supplied by the architecture */
void serial_init(void);
void serial_putc(char c);
int serial_getc(void);
void ps2_init(void);
int ps2_mouse_present(void);

#endif
