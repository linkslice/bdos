/*
 * arch.h - MIPS64 (Loongson 2E/2F: QEMU's Fuloong 2E, the Lemote Yeeloong).
 *
 * The kernel runs unmapped in KSEG0 (physical memory at 0xffffffff80000000),
 * one core, the n64 ABI. Device registers are reached uncached through
 * KSEG1, or XKPHYS for anything above 512 MB.
 */
#ifndef BDOS_ARCH_H
#define BDOS_ARCH_H

#include <stdint.h>

#define ARCH_NAME	"mips64"
#define PAGE_SIZE	4096UL
#define ARCH_KVA_OFFSET	0xffffffff80000000ULL	/* KSEG0 */
#define P2V(pa)		((void *)((uint64_t)(pa) + ARCH_KVA_OFFSET))
#define V2P(va)		((uint64_t)(va) - ARCH_KVA_OFFSET)

static inline uint32_t cp0_status(void)
{
	uint32_t v;
	__asm__ volatile("mfc0 %0, $12" : "=r"(v));
	return v;
}
static inline void cp0_set_status(uint32_t v)
{
	__asm__ volatile("mtc0 %0, $12; nop; nop; nop" : : "r"(v) : "memory");
}

static inline void cpu_pause(void) { __asm__ volatile("nop"); }
static inline void irq_enable(void) { cp0_set_status(cp0_status() | 1); }
static inline void irq_disable(void) { cp0_set_status(cp0_status() & ~1u); }
static inline void cpu_idle(void)
{
	irq_enable();
	__asm__ volatile("wait" ::: "memory");
}
static inline uint64_t irq_save(void)
{
	uint32_t s = cp0_status();
	cp0_set_status(s & ~1u);
	return s & 1;
}
static inline void irq_restore(uint64_t f)
{
	if (f & 1)
		irq_enable();
}
uint64_t rdtsc(void);			/* CP0 Count, widened to 64 bits */
static inline void mmio_barrier(void) { __asm__ volatile("sync" ::: "memory"); }

/* Saved by arch_switch: the stack (callee-saved registers are on it) and the FPU */
struct arch_context {
	uint64_t sp;
	uint64_t _pad;
	uint8_t fx[33 * 8];	/* $f0-$f31 and FCSR */
} __attribute__((aligned(16)));
static inline void arch_fpu_clean(void *fx)
{
	uint64_t *p = fx;
	for (int i = 0; i < 33; i++)
		p[i] = 0;
}

/* ---- one core ---- */
#define VEC_TIMER	0xF0
#define VEC_KICK	0xF1
#define VEC_HALT	0xF2
#define MAX_CPUS 1
struct cpu;
struct cpu *this_cpu(void);		/* the one core's */
void arch_cpu_local(struct cpu *c);
int arch_smp_init(void);
void arch_halt_others(void);
void arch_kick_cpu(int cpu);

void arch_init(const void *bootinfo);
void arch_switch(struct arch_context *from, struct arch_context *to);
void arch_task_setup(struct arch_context *ctx, void *stack_top, void (*entry)(void));
void arch_set_kstack(void *top);

/* ---- no user mode here: native bdos programs are x86-64 ---- */
#define USER_BASE	0x0000008000000000ULL
#define USER_TOP	0x00007FFFFFFFF000ULL
#define VM_WRITE	1
#define VM_EXEC		2
void *arch_as_new(void);
int arch_as_map(void *as, uint64_t va, uint64_t pa, int flags);
void *arch_map_mmio(uint64_t pa, uint64_t size);
uint64_t arch_as_lookup(void *as, uint64_t va);
uint64_t arch_as_unmap(void *as, uint64_t va);
void arch_as_free(void *as);
void arch_as_switch(void *as);
void arch_set_fs_base(uint64_t base);
_Noreturn void arch_enter_user(uint64_t rip, uint64_t rsp, uint64_t a0, uint64_t a1);
void arch_syscall_init(void);
struct sysframe {
	uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, rax;
	uint64_t rip, rflags, rsp;
};

_Noreturn void arch_reboot(void);
_Noreturn void arch_halt(void);
uint32_t arch_pci_read32(int bus, int slot, int func, uint8_t off);
void arch_pci_write32(int bus, int slot, int func, uint8_t off, uint32_t v);
void arch_rtc(int *y, int *mo, int *d, int *h, int *mi, int *s);
void arch_rtc_set(int y, int mo, int d, int h, int mi, int s);
void arch_cpu_name(char *out, int n);
void arch_speaker(uint32_t hz);

/* for the interpreters: s0-s8, sp, ra, gp and the callee-saved $f24-$f31 */
struct arch_jmpbuf {
	uint64_t regs[20];
};
int arch_setjmp(struct arch_jmpbuf *jb) __attribute__((returns_twice));
_Noreturn void arch_longjmp(struct arch_jmpbuf *jb, int v);

#define IRQ_TIMER	0
#define IRQ_KBD		1
#define IRQ_MOUSE	12
void irq_register(int irq, void (*fn)(void));

void serial_init(void);
void serial_putc(char c);
int serial_getc(void);
void ps2_init(void);
int ps2_mouse_present(void);

#endif
