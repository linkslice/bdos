/*
 * x86.h - an interpreted 386/486: real mode, 16- and 32-bit code, protected
 * mode (descriptors, privilege levels, interrupt gates), and the x87.
 *
 * It knows nothing about DOS. Whoever runs it (dosbox.c) supplies the
 * memory, the I/O ports, and a "high-level emulation" hook: the opcode F1
 * followed by a byte, executed from ROM (F0000-FFFFF), calls hle() instead
 * of trapping. That's how the BIOS and DOS services written in C get called
 * from the little machine-code stubs the interrupt vectors point at.
 */
#ifndef BDOS_X86_H
#define BDOS_X86_H

#include "../kernel.h"

enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };

#define FL_CF	0x0001
#define FL_PF	0x0004
#define FL_AF	0x0010
#define FL_ZF	0x0040
#define FL_SF	0x0080
#define FL_TF	0x0100
#define FL_IF	0x0200
#define FL_DF	0x0400
#define FL_OF	0x0800
#define FL_IOPL	0x3000
#define FL_NT	0x4000
#define FL_RF	0x10000
#define FL_VM	0x20000
#define FL_AC	0x40000
#define FL_ID	0x200000
#define FL_ARITH (FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF)

#define CR0_PE	0x00000001
#define CR0_PG	0x80000000

struct xseg {
	uint16_t sel;
	uint32_t base;
	uint32_t limit;		/* in bytes, already scaled by G */
	uint8_t access;		/* the descriptor's access byte */
	uint8_t big;		/* D/B: 32-bit code or stack */
};

struct x87 {
	uint8_t env[108];	/* the guest's FPU, as an FNSAVE image, when not live */
	uint16_t cw;		/* the control word the guest asked for */
	int used;
	uint8_t *thunk;		/* where guest FPU instructions run (x87.c) */
	uint16_t last;		/* the instruction the thunk holds now */
};

struct x86;

struct x86_env {
	uint32_t (*in)(struct x86 *c, uint16_t port, int size);
	void (*out)(struct x86 *c, uint16_t port, uint32_t v, int size);
	/* A0000-BFFFF: video memory */
	uint8_t (*vga_rd)(struct x86 *c, uint32_t a);
	void (*vga_wr)(struct x86 *c, uint32_t a, uint8_t v);
	/* F1 nn from ROM. Return 0 to carry on, 1 to come back to it later
	 * (it re-executes, after interrupts get their chance) */
	int (*hle)(struct x86 *c, int n);
};

enum { XS_RUN, XS_BUDGET, XS_HALT, XS_WAIT, XS_STOP, XS_TRIPLE };

struct x86 {
	uint32_t r[8];
	uint32_t eip;
	uint32_t flags;
	/* lazily computed arithmetic flags */
	int lf_op, lf_sz;
	uint32_t lf_a, lf_b, lf_res, lf_cin;
	struct xseg s[6];
	uint32_t cr0, cr2, cr3, cr4;
	uint32_t dr[8];
	struct { uint32_t base; uint32_t limit; } gdt, idt;
	struct xseg ldt, tr;
	int cpl;
	/* memory */
	uint8_t *mem;
	uint32_t mem_size;
	uint32_t a20;		/* 0xFFEFFFFF with the A20 gate off, else ~0 */
	uint32_t vga_lo, vga_hi;	/* the window the env handles (empty = none) */
	/* the instruction being decoded */
	uint32_t ip0;
	int osz, asz, seg, rep, lock;
	int mod, reg, rm;
	uint32_t ea;
	int ea_seg;
	/* running */
	const struct x86_env *env;
	void *box;
	int state;		/* why x86_run stopped */
	int hle_vec;
	uint64_t icount;
	int intr_shadow;	/* after STI / MOV SS: one more instruction first */
	struct arch_jmpbuf jb;
	int fault_vec;
	uint32_t fault_err;
	int fault_has_err;
	int faulting;		/* a fault while delivering one: double fault */
	/* dos.trace: where the last instructions were, for a crash report */
	int trace;
	unsigned hist_n;
	struct { uint16_t cs; uint32_t ip; } hist[64];
	struct x87 fpu;
	int cpu_level;		/* 3 = 386, 4 = 486 */
	/* what the last protected-mode interrupt was, for a DPMI host to sort
	 * IRQs from exceptions on the same vector: 0 exception, 1 INT n, 2 IRQ */
	int int_kind;
	int hw_int;
	/* an interrupt is waiting for IF: stop as soon as it's set, so the
	 * owner can deliver it (else a loop that always has IF clear where
	 * the budget runs out would never take one) */
	int irq_waiting;
	int vram_touched;	/* the program wrote text memory itself */
};

void x86_reset(struct x86 *c);
/* run up to budget instructions; returns how many ran, and c->state says why it stopped */
long x86_run(struct x86 *c, long budget);
/* deliver a hardware interrupt now (the caller checks IF) */
void x86_interrupt(struct x86 *c, int vec);
int x86_irq_ok(struct x86 *c);		/* IF set and not in an STI shadow */
void x86_load_seg_real(struct x86 *c, int s, uint16_t sel);
uint32_t x86_flags(struct x86 *c);	/* all flags, computed */
void x86_set_flags(struct x86 *c, uint32_t f, uint32_t mask);
int x86_load_seg(struct x86 *c, int s, uint16_t sel);	/* protected-mode load; 0 or fault */

/* guest memory, by linear address (no VGA window handling) */
static inline uint8_t x86_peek8(struct x86 *c, uint32_t a)
{
	a &= c->a20;
	return a < c->mem_size ? c->mem[a] : 0xFF;
}
static inline void x86_poke8(struct x86 *c, uint32_t a, uint8_t v)
{
	a &= c->a20;
	if (a < c->mem_size)
		c->mem[a] = v;
}
uint8_t x86_rd8(struct x86 *c, uint32_t lin);
uint16_t x86_rd16(struct x86 *c, uint32_t lin);
uint32_t x86_rd32(struct x86 *c, uint32_t lin);
void x86_wr8(struct x86 *c, uint32_t lin, uint8_t v);
void x86_wr16(struct x86 *c, uint32_t lin, uint16_t v);
void x86_wr32(struct x86 *c, uint32_t lin, uint32_t v);
void x86_push16(struct x86 *c, uint16_t v);
uint16_t x86_pop16(struct x86 *c);
void x86_push32(struct x86 *c, uint32_t v);
uint32_t x86_pop32(struct x86 *c);

/* the x87 (x87.c) */
void x87_reset(struct x86 *c);
void x87_exec(struct x86 *c, uint8_t op);	/* D8-DF; modrm already decoded */
void x87_enter(struct x86 *c);			/* make the guest's FPU live */
void x87_leave(struct x86 *c);			/* and put it away */
void x87_free(struct x86 *c);

#endif
