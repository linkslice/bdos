/*
 * x87.c - the guest's floating point, run on the real x87.
 *
 * bdos runs on an x86-64, which still has an x87 inside with the same
 * 80-bit registers and rounding as a 486's. So rather than emulate it,
 * each guest FPU instruction is executed for real: its opcode and ModRM
 * bytes go into a tiny thunk ("D8 C1 C3": FADD ST,ST(1); RET) which is
 * called. Memory operands are rewritten to [RAX] and pointed at a buffer
 * that's copied from and back to guest memory. The guest's FPU state just
 * stays in the x87 while the DOS program runs; task switches save it with
 * everything else.
 *
 * Two things differ from handing the chip over completely: exceptions stay
 * masked on the real chip (an unmasked one would fault in the kernel) while
 * the guest sees the control word it set; and the 16-bit layouts of FSTENV
 * and FSAVE are converted to and from the 32-bit ones a 64-bit CPU writes.
 */
#include "x86.h"

#ifndef __x86_64__
/* Elsewhere there's no x87 to borrow: the box has no coprocessor, as most
 * 386s didn't. Programs that probe for one (FNINIT, FNSTSW) find nothing. */
void x87_reset(struct x86 *c) { c->fpu.cw = 0x037F; c->fpu.used = 0; }
void x87_enter(struct x86 *c) { (void)c; }
void x87_leave(struct x86 *c) { (void)c; }
void x87_free(struct x86 *c) { (void)c; }
void x87_exec(struct x86 *c, uint8_t op) { (void)c; (void)op; }
#else

#define THUNK_SIZE 16

static uint64_t call_thunk(uint8_t *code, void *mem)
{
	uint64_t rax = (uint64_t)mem;
	__asm__ volatile("call *%1" : "+a"(rax) : "r"(code) : "memory", "cc", "rcx", "rdx");
	return rax;
}

/* the guest asked for cw; the chip gets it with every exception masked */
static void set_host_cw(uint16_t cw)
{
	uint16_t h = cw | 0x3F;
	__asm__ volatile("fldcw %0" : : "m"(h));
}

void x87_reset(struct x86 *c)
{
	c->fpu.cw = 0x037F;
	c->fpu.used = 0;
	c->fpu.last = 0xFFFF;
}

void x87_enter(struct x86 *c)
{
	if (c->fpu.used)
		__asm__ volatile("frstor %0" : : "m"(c->fpu.env));
	else
		__asm__ volatile("fninit");
	set_host_cw(c->fpu.cw);
}

void x87_leave(struct x86 *c)
{
	__asm__ volatile("fnsave %0" : "=m"(c->fpu.env));
	c->fpu.used = 1;
	__asm__ volatile("fninit");
}

void x87_free(struct x86 *c)
{
	kfree(c->fpu.thunk);
	c->fpu.thunk = NULL;
}

static uint64_t run(struct x86 *c, uint8_t op, uint8_t modrm, void *mem)
{
	if (!c->fpu.thunk) {
		c->fpu.thunk = kmalloc(THUNK_SIZE);
		if (!c->fpu.thunk)
			return 0;
		c->fpu.last = 0xFFFF;
	}
	uint16_t want = (uint16_t)(op | modrm << 8);
	if (c->fpu.last != want) {
		c->fpu.thunk[0] = op;
		c->fpu.thunk[1] = modrm;
		c->fpu.thunk[2] = 0xC3;		/* RET */
		c->fpu.last = want;
	}
	return call_thunk(c->fpu.thunk, mem);
}

/* operand bytes and direction of each memory form: >0 read, <0 write */
static int mem_size(uint8_t op, int reg, int osz)
{
	switch (op) {
	case 0xD8: return 4;
	case 0xD9:
		switch (reg) {
		case 0: return 4;
		case 2: case 3: return -4;
		case 4: return osz ? 28 : 14;
		case 5: return 2;
		case 6: return osz ? -28 : -14;
		case 7: return -2;
		}
		return 0;
	case 0xDA: return 4;
	case 0xDB:
		switch (reg) {
		case 0: return 4;
		case 2: case 3: return -4;		/* (FISTTP is SSE3: not here) */
		case 5: return 10;
		case 7: return -10;
		}
		return 0;
	case 0xDC: return 8;
	case 0xDD:
		switch (reg) {
		case 0: return 8;
		case 2: case 3: return -8;
		case 4: return osz ? 108 : 94;
		case 6: return osz ? -108 : -94;
		case 7: return -2;
		}
		return 0;
	case 0xDE: return 2;
	case 0xDF:
		switch (reg) {
		case 0: return 2;
		case 2: case 3: return -2;
		case 4: return 10;
		case 5: return 8;
		case 6: return -10;
		case 7: return -8;
		}
		return 0;
	}
	return 0;
}

static uint32_t ea_linear(struct x86 *c, uint32_t n)
{
	uint32_t off = c->ea + n;
	if (!c->asz)
		off &= 0xFFFF;
	return c->s[c->ea_seg].base + off;
}

/* 16-bit environment (14 bytes) <-> the 32-bit one (28 bytes) */
static void env16_to_32(const uint8_t *e16, uint8_t *e32)
{
	memset(e32, 0, 28);
	for (int i = 0; i < 3; i++) {
		e32[i * 4] = e16[i * 2];
		e32[i * 4 + 1] = e16[i * 2 + 1];
	}
	e32[9] = e32[10] = e32[11] = 0;
}

static void env32_to_16(const uint8_t *e32, uint8_t *e16)
{
	memset(e16, 0, 14);
	for (int i = 0; i < 3; i++) {
		e16[i * 2] = e32[i * 4];
		e16[i * 2 + 1] = e32[i * 4 + 1];
	}
}

/* register forms a 486 has (the host must never see anything else) */
static int reg_form_ok(uint8_t op, uint8_t m)
{
	int reg = (m >> 3) & 7;
	switch (op) {
	case 0xD8: return 1;
	case 0xD9:
		if (reg <= 1) return 1;			/* FLD, FXCH */
		if (reg == 2) return m == 0xD0;		/* FNOP */
		if (reg == 3) return 0;
		if (reg == 4) return m == 0xE0 || m == 0xE1 || m == 0xE4 || m == 0xE5;
		if (reg == 5) return m != 0xEF;
		return 1;				/* F0-FF: all defined */
	case 0xDA: return m == 0xE9;			/* FUCOMPP */
	case 0xDC: return reg != 2 && reg != 3;
	case 0xDD: return reg != 1 && reg != 7;
	case 0xDE: return reg != 2 && (reg != 3 || m == 0xD9);
	case 0xDF: return reg == 0;			/* FFREEP */
	}
	return 0;
}

void x87_exec(struct x86 *c, uint8_t op)
{
	uint8_t modrm = (uint8_t)(c->mod << 6 | c->reg << 3 | c->rm);
	if (c->mod == 3) {
		/* FCMOV, FCOMI, FUCOMI: P6 additions a 486 doesn't have */
		if (op == 0xDA && c->reg < 4)
			goto ud;
		if (op == 0xDB) {
			if (c->reg != 4 || modrm > 0xE4)
				goto ud;
			if (modrm == 0xE3) {		/* FNINIT */
				c->fpu.cw = 0x037F;
				__asm__ volatile("fninit");
				set_host_cw(c->fpu.cw);
			} else if (modrm == 0xE2) {	/* FNCLEX */
				__asm__ volatile("fnclex");
			}
			return;				/* FENI FDISI FSETPM: nothing */
		}
		if (op == 0xDF && c->reg == 4) {	/* FNSTSW AX */
			if (modrm != 0xE0)
				goto ud;
			uint16_t sw;
			__asm__ volatile("fnstsw %0" : "=m"(sw));
			c->r[R_EAX] = (c->r[R_EAX] & 0xFFFF0000) | sw;
			return;
		}
		if (!reg_form_ok(op, modrm))
			goto ud;
		run(c, op, modrm, NULL);
		return;
	}
	int n = mem_size(op, c->reg, c->osz);
	if (!n)
		goto ud;
	uint8_t buf[112];
	uint8_t m = (uint8_t)(c->reg << 3);	/* [RAX] */
	if (op == 0xD9 && c->reg == 5) {	/* FLDCW */
		c->fpu.cw = x86_rd16(c, ea_linear(c, 0));
		set_host_cw(c->fpu.cw);
		return;
	}
	if (op == 0xD9 && c->reg == 7) {	/* FNSTCW */
		x86_wr16(c, ea_linear(c, 0), c->fpu.cw);
		return;
	}
	int env = (op == 0xD9 && (c->reg == 4 || c->reg == 6)) || (op == 0xDD && (c->reg == 4 || c->reg == 6));
	if (n > 0) {
		int len = n;
		for (int i = 0; i < len; i++)
			buf[i] = x86_rd8(c, ea_linear(c, (uint32_t)i));
		if (env) {
			uint8_t tmp[108];
			int regs = op == 0xDD;
			if (!c->osz) {
				env16_to_32(buf, tmp);
				if (regs)
					memcpy(tmp + 28, buf + 14, 80);
			} else {
				memcpy(tmp, buf, (size_t)len);
			}
			c->fpu.cw = (uint16_t)(tmp[0] | tmp[1] << 8);
			tmp[0] |= 0x3F;
			memcpy(buf, tmp, regs ? 108 : 28);
		}
		run(c, op, m, buf);
		return;
	}
	run(c, op, m, buf);
	int len = -n;
	if (env) {
		int regs = op == 0xDD;
		buf[0] = (uint8_t)c->fpu.cw;
		buf[1] = (uint8_t)(c->fpu.cw >> 8);
		if (!c->osz) {
			uint8_t tmp[108];
			memcpy(tmp, buf, 108);
			env32_to_16(tmp, buf);
			if (regs)
				memcpy(buf + 14, tmp + 28, 80);
		}
		if (regs)		/* FNSAVE leaves the FPU reinitialised */
			set_host_cw(c->fpu.cw = 0x037F);
	}
	for (int i = 0; i < len; i++)
		x86_wr8(c, ea_linear(c, (uint32_t)i), buf[i]);
	return;
ud:
	/* an instruction a 486 doesn't have: #UD */
	c->eip = c->ip0;
	c->fault_vec = 6;
	c->fault_has_err = 0;
	arch_longjmp(&c->jb, 1);
}

#endif /* __x86_64__ */
