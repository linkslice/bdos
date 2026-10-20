/*
 * arch.h - x86-64 specifics. Everything outside kernel/arch/ goes through
 * these names, so a port (MIPS64 Yeeloong) supplies its own arch.h.
 */
#ifndef BDOS_ARCH_H
#define BDOS_ARCH_H

#include <stdint.h>

#define ARCH_NAME	"x86-64"
#define PAGE_SIZE	4096UL

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

/* Saved by arch_switch(); everything else is caller-saved in SysV. */
struct arch_context {
	uint64_t rsp;
};

void arch_init(const void *bootinfo);	/* GDT, IDT, paging, PIC, timer */
void arch_switch(struct arch_context *from, struct arch_context *to);
void arch_task_setup(struct arch_context *ctx, void *stack_top,
    void (*entry)(void));
_Noreturn void arch_reboot(void);
_Noreturn void arch_halt(void);		/* interrupts off, stop for good */
/* PCI configuration space (mechanism #1 on PCs; the Bonito bridge on a Yeeloong) */
uint32_t arch_pci_read32(int bus, int slot, int func, uint8_t off);
void arch_pci_write32(int bus, int slot, int func, uint8_t off, uint32_t v);
void arch_rtc(int *y, int *mo, int *d, int *h, int *mi, int *s);

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
