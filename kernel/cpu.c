/*
 * cpu.c - our own GDT and IDT, replacing the firmware's.
 *
 * The firmware's tables live in boot-services memory, which bdos counts
 * as free, so we install our own before anything could reuse that RAM.
 * Paging still uses the firmware's identity map for now.
 */
#include "kernel.h"

struct gdt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

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

#define KCODE 0x08
#define KDATA 0x10

static const uint64_t gdt[] = {
	0,
	0x00AF9A000000FFFFULL,	/* 0x08: 64-bit code, ring 0 */
	0x00CF92000000FFFFULL,	/* 0x10: data, ring 0 */
};

static struct idt_entry idt[256];
extern const uint64_t isr_table[32];

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

void isr_dispatch(struct trapframe *tf)
{
	uint64_t cr2;
	__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
	kprintf("\n*** %s (exception %lu, error %lx)\n",
	    exc_names[tf->vector & 31], tf->vector, tf->error);
	kprintf("    RIP=%p RSP=%p CR2=%p\n", (void *)tf->rip, (void *)tf->rsp, (void *)cr2);
	kprintf("    RAX=%p RBX=%p RCX=%p RDX=%p\n", (void *)tf->rax, (void *)tf->rbx,
	    (void *)tf->rcx, (void *)tf->rdx);
	panic("unhandled CPU exception");
}

void cpu_init(void)
{
	struct gdt_ptr gp = { sizeof gdt - 1, (uint64_t)gdt };
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

	for (int i = 0; i < 32; i++) {
		uint64_t a = isr_table[i];
		idt[i].off_lo = a & 0xFFFF;
		idt[i].sel = KCODE;
		idt[i].ist = 0;
		idt[i].type = 0x8E;	/* present, ring 0, interrupt gate */
		idt[i].off_mid = (a >> 16) & 0xFFFF;
		idt[i].off_hi = a >> 32;
	}
	struct gdt_ptr ip = { sizeof idt - 1, (uint64_t)idt };
	__asm__ volatile("lidt %0" : : "m"(ip));
}

_Noreturn void reboot(void)
{
	/* Ask the keyboard controller to pulse the reset line. */
	for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
		cpu_pause();
	outb(0x64, 0xFE);
	/* If that didn't work, the reset control register at 0xCF9. */
	outb(0xCF9, 0x06);
	/* Last resort: triple fault with an empty IDT. */
	struct gdt_ptr none = { 0, 0 };
	__asm__ volatile("lidt %0; int3" : : "m"(none));
	for (;;)
		__asm__ volatile("hlt");
}
