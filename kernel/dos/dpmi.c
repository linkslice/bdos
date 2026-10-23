/*
 * dpmi.c - a DPMI 0.9 host, so protected-mode DOS programs run: DOS/4GW
 * games, DJGPP programs, anything that asks INT 2Fh 1687h for a host.
 *
 * The client runs at ring 3 with IOPL 3, in a flat world without paging:
 * a linear address is a guest physical address. Its descriptors live in an
 * LDT we keep, and every IDT entry points at a little stub in the ROM
 * (F1 F6 n IRET) that lands in dpmi_hle(), which decides what that
 * interrupt means: the client's own handler, an exception handler, INT 31h,
 * or a trip down to real mode.
 *
 * Real mode is real here: to reflect an interrupt or call real-mode code
 * the CPU really leaves protected mode, and comes back through
 * HLE_DPMI_RET when the real-mode code returns. Each trip pushes a frame
 * (the other mode's whole CPU state) so they nest.
 *
 * Everything the host keeps - LDT, IDT, its stack, this file's struct -
 * sits in one block of the guest's extended memory, so a saved state of a
 * protected-mode program carries its DPMI host along.
 */
#include "dos.h"
#include "../gate.h"

#define LDT_ENTRIES	8192
#define HOST_LDT	0x00000
#define HOST_IDT	0x10000
#define HOST_GDT	0x10800
#define HOST_STACK	0x11000		/* the locked stack: callbacks, exceptions, IRQs from real mode */
#define HOST_STACK_SZ	0x4000
#define HOST_STRUCT	0x15000
#define HOST_SIZE	0x1C000

/* the ROM (offsets in F000) */
#define ROM_PM_DEF	0x1400		/* 256 x "F1 FA n CF": the default action, for chaining */
#define ROM_PM_EXC	0x1800		/* 32 x "F1 F7 n CB": default exception handlers */
#define ROM_EXC_RET	0x1880		/* exception handlers RETF here */
#define ROM_PM_RET	0x1890		/* protected-mode handlers called from real mode IRET here */
#define ROM_RM_CB	0x18A0		/* 16 x "F1 F8 i 90": real-mode callbacks */
#define ROM_RM_TO_PM	0x1900		/* 256 x "F1 FC n CF": real-mode vectors hooked for protected mode */
#define ROM_RAW_TO_PM	0x1D00
#define ROM_RAW_TO_RM	0x1D10
#define ROM_SAVE	0x1D20		/* the state save/restore routine: nothing to do */
#define RM_STACK_PARAS	0x200		/* 8 KB of real-mode stack: the host data the client gives us */

/* our own descriptors (the first 16 are for 000Dh) */
#define IDX_CODE16	16
#define IDX_CODE32	17
#define IDX_STACK	18
#define IDX_CBALIAS	19		/* 4 of them: DS for real-mode callbacks */
#define IDX_FIRST_FREE	23

#define MAX_FRAMES	24
#define MAX_CB		16
#define MAX_SEGSEL	64

enum { FR_INT, FR_IRQ, FR_CALL, FR_CALLBACK, FR_RM2PM };

struct cpustate {
	uint32_t r[8], eip, flags, cr0;
	struct xseg s[6];
	int cpl;
	uint32_t idt_base, idt_limit;
};

struct frame {
	int kind;
	struct cpustate saved;		/* the mode we came from */
	uint32_t regs_lin;		/* FR_CALL / FR_CALLBACK: the real-mode call structure */
};

struct far32 {
	uint16_t sel;
	uint32_t off;
};

struct dpmi {
	uint32_t lin;			/* where all this is */
	int handle;			/* its ext block */
	int bits32;
	uint16_t psp;
	uint16_t env_seg;		/* what PSP:2Ch was before we made it a selector */
	uint16_t rm_stack_seg;
	uint8_t ldt_used[LDT_ENTRIES];
	struct far32 pm_vec[256];	/* sel 0: ours (the default) */
	struct far32 exc_vec[32];
	uint32_t rm_orig[256];		/* real-mode vectors we hooked, as they were */
	uint8_t rm_hooked[256];
	uint8_t mem[MAX_EXT];		/* ext handles that are DPMI blocks */
	struct {
		int used;
		struct far32 proc, regs;
	} cb[MAX_CB];
	struct {
		uint16_t seg, sel;
	} segsel[MAX_SEGSEL];
	int nsegsel;
	uint32_t a20;			/* the gate as the program had it */
	struct frame fr[MAX_FRAMES];
	int depth;
};

_Static_assert(sizeof(struct dpmi) <= HOST_SIZE - HOST_STRUCT, "struct dpmi must fit the host block");

static struct dpmi *D(struct dosbox *b)
{
	return b->dpmi_lin ? (struct dpmi *)(b->cpu.mem + b->dpmi_lin + HOST_STRUCT) : NULL;
}

static uint16_t sel_of(int idx) { return (uint16_t)(idx << 3 | 4 | 3); }
static int idx_of(uint16_t sel) { return sel >> 3; }

/* ---- the ROM -------------------------------------------------------------------------- */

void dpmi_rom(struct dosbox *b)
{
	for (int n = 0; n < 256; n++) {
		uint32_t a = sl(BIOS_SEG, (uint16_t)(ROM_PM_STUBS + n * 4));
		mbw(b, a, 0xF1), mbw(b, a + 1, HLE_PM_INT), mbw(b, a + 2, (uint8_t)n), mbw(b, a + 3, 0xCF);
		a = sl(BIOS_SEG, (uint16_t)(ROM_PM_DEF + n * 4));
		mbw(b, a, 0xF1), mbw(b, a + 1, HLE_PM_DEF), mbw(b, a + 2, (uint8_t)n), mbw(b, a + 3, 0xCF);
		a = sl(BIOS_SEG, (uint16_t)(ROM_RM_TO_PM + n * 4));
		mbw(b, a, 0xF1), mbw(b, a + 1, HLE_RM_TO_PM), mbw(b, a + 2, (uint8_t)n), mbw(b, a + 3, 0xCF);
	}
	for (int n = 0; n < 32; n++) {
		uint32_t a = sl(BIOS_SEG, (uint16_t)(ROM_PM_EXC + n * 4));
		mbw(b, a, 0xF1), mbw(b, a + 1, HLE_PM_EXC), mbw(b, a + 2, (uint8_t)n), mbw(b, a + 3, 0xCB);
	}
	for (int i = 0; i < MAX_CB; i++) {
		uint32_t a = sl(BIOS_SEG, (uint16_t)(ROM_RM_CB + i * 4));
		mbw(b, a, 0xF1), mbw(b, a + 1, HLE_RM_CB), mbw(b, a + 2, (uint8_t)i), mbw(b, a + 3, 0x90);
	}
	uint32_t a = sl(BIOS_SEG, ROM_EXC_RET);
	mbw(b, a, 0xF1), mbw(b, a + 1, HLE_EXC_RET);
	a = sl(BIOS_SEG, ROM_PM_RET);
	mbw(b, a, 0xF1), mbw(b, a + 1, HLE_DPMI_RET);
	a = sl(BIOS_SEG, ROM_RAW_TO_PM);
	mbw(b, a, 0xF1), mbw(b, a + 1, HLE_RAW_TO_PM);
	a = sl(BIOS_SEG, ROM_RAW_TO_RM);
	mbw(b, a, 0xF1), mbw(b, a + 1, HLE_RAW_TO_RM);
	mbw(b, sl(BIOS_SEG, ROM_SAVE), 0xCB);	/* RETF (RETFD in a 32-bit segment) */
}

/* ---- descriptors ---------------------------------------------------------------------- */

static uint32_t desc_lin(struct dpmi *d, int idx) { return d->lin + HOST_LDT + (uint32_t)idx * 8; }

static void desc_set(struct dosbox *b, struct dpmi *d, int idx, uint32_t base, uint32_t limit, uint8_t access, int big)
{
	uint32_t g = 0;
	if (limit > 0xFFFFF) {
		limit >>= 12;
		g = 0x800000;
	}
	uint32_t lo = (base << 16) | (limit & 0xFFFF);
	uint32_t hi = (base & 0xFF000000) | ((base >> 16) & 0xFF) | (limit & 0xF0000) | g |
	    (big ? 0x400000 : 0) | (uint32_t)access << 8;
	mdw(b, desc_lin(d, idx), lo);
	mdw(b, desc_lin(d, idx) + 4, hi);
}

static uint32_t desc_base(struct dosbox *b, struct dpmi *d, int idx)
{
	uint32_t lo = md(b, desc_lin(d, idx)), hi = md(b, desc_lin(d, idx) + 4);
	return (lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000);
}

static int desc_alloc(struct dosbox *b, struct dpmi *d, int n)
{
	if (n < 1)
		return -1;
	for (int i = IDX_FIRST_FREE; i + n <= LDT_ENTRIES; i++) {
		int ok = 1;
		for (int k = 0; k < n && ok; k++)
			ok = !d->ldt_used[i + k];
		if (!ok)
			continue;
		for (int k = 0; k < n; k++) {
			d->ldt_used[i + k] = 1;
			desc_set(b, d, i + k, 0, 0, 0xF2, d->bits32);
		}
		return i;
	}
	return -1;
}

static int sel_ok(struct dpmi *d, uint16_t sel)
{
	return (sel & 7) == 7 && idx_of(sel) < LDT_ENTRIES && d->ldt_used[idx_of(sel)] && idx_of(sel) >= 0 &&
	    (idx_of(sel) < IDX_CODE16 || idx_of(sel) >= IDX_FIRST_FREE);
}

static uint16_t seg_selector(struct dosbox *b, struct dpmi *d, uint16_t seg, int code)
{
	if (!code)
		for (int i = 0; i < d->nsegsel; i++)
			if (d->segsel[i].seg == seg)
				return d->segsel[i].sel;
	int idx = desc_alloc(b, d, 1);
	if (idx < 0)
		return 0;
	desc_set(b, d, idx, (uint32_t)seg << 4, 0xFFFF, code ? 0xFA : 0xF2, 0);
	if (!code && d->nsegsel < MAX_SEGSEL) {
		d->segsel[d->nsegsel].seg = seg;
		d->segsel[d->nsegsel++].sel = sel_of(idx);
	}
	return sel_of(idx);
}

/* segment registers holding sel see its new descriptor (a real host reloads them on the way out) */
static void reload(struct x86 *c, uint16_t sel)
{
	for (int s = S_ES; s <= S_GS; s++)
		if (s != S_CS && (c->s[s].sel | 3) == (sel | 3) && (sel & ~3u))
			x86_load_seg(c, s, c->s[s].sel);
}

/* ---- CPU state and the two modes ------------------------------------------------------ */

static void save(struct x86 *c, struct cpustate *s)
{
	memcpy(s->r, c->r, sizeof s->r);
	s->eip = c->eip;
	s->flags = x86_flags(c);
	s->cr0 = c->cr0;
	memcpy(s->s, c->s, sizeof s->s);
	s->cpl = c->cpl;
	s->idt_base = c->idt.base;
	s->idt_limit = c->idt.limit;
}

static void restore(struct x86 *c, const struct cpustate *s)
{
	memcpy(c->r, s->r, sizeof c->r);
	c->eip = s->eip;
	c->cr0 = s->cr0;
	x86_set_flags(c, s->flags, 0x3F7FD5);
	memcpy(c->s, s->s, sizeof c->s);
	c->cpl = s->cpl;
	c->idt.base = s->idt_base;
	c->idt.limit = s->idt_limit;
}

static void rm_seg(struct x86 *c, int s, uint16_t seg)
{
	x86_load_seg_real(c, s, seg);
	c->s[s].limit = 0xFFFF;
	c->s[s].big = 0;
	c->s[s].access = s == S_CS ? 0x9B : 0x93;
}

static void to_real(struct x86 *c)
{
	c->cr0 &= ~CR0_PE;
	c->cpl = 0;
	c->idt.base = 0;
	c->idt.limit = 0x3FF;
	x86_set_flags(c, 0, FL_VM | FL_NT | FL_TF);
}

static void to_pm(struct dosbox *b, struct dpmi *d)
{
	struct x86 *c = &b->cpu;
	c->cr0 |= CR0_PE;
	c->cpl = 3;
	c->idt.base = d->lin + HOST_IDT;
	c->idt.limit = 0x7FF;
	c->ldt.base = d->lin + HOST_LDT;
	c->ldt.limit = LDT_ENTRIES * 8 - 1;
	c->ldt.sel = 0;
	c->gdt.base = d->lin + HOST_GDT;
	c->gdt.limit = 7;
	x86_set_flags(c, FL_IOPL, FL_IOPL | FL_VM | FL_NT | FL_TF);
}

static uint16_t code_sel(struct dpmi *d) { return sel_of(d->bits32 ? IDX_CODE32 : IDX_CODE16); }

/* jump to sel:off in protected mode (CPL stays 3) */
static void pm_jump(struct x86 *c, uint16_t sel, uint32_t off)
{
	x86_load_seg(c, S_CS, sel);
	c->cpl = 3;
	c->s[S_CS].sel |= 3;
	c->eip = c->s[S_CS].big ? off : off & 0xFFFF;
}

static void pm_seg(struct x86 *c, int s, uint16_t sel)
{
	x86_load_seg(c, s, sel);
}

static int sz(struct dpmi *d) { return d->bits32 ? 4 : 2; }

static void push_sz(struct x86 *c, struct dpmi *d, uint32_t v)
{
	if (d->bits32)
		x86_push32(c, v);
	else
		x86_push16(c, (uint16_t)v);
}

static uint32_t sp_lin(struct x86 *c)
{
	return c->s[S_SS].base + (c->s[S_SS].big ? c->r[R_ESP] : c->r[R_ESP] & 0xFFFF);
}

static uint32_t rd_sz(struct x86 *c, uint32_t a, int n) { return n == 4 ? x86_rd32(c, a) : x86_rd16(c, a); }
/* the locked stack: below whatever's already on it */
static uint32_t lstack_esp(struct x86 *c, struct dpmi *d)
{
	uint16_t ls = sel_of(IDX_STACK);
	if ((c->cr0 & CR0_PE) && (c->s[S_SS].sel | 3) == ls)
		return (c->r[R_ESP] - 32) & ~3u;
	for (int i = d->depth - 1; i >= 0; i--) {
		struct cpustate *s = &d->fr[i].saved;
		if ((s->cr0 & CR0_PE) && (s->s[S_SS].sel | 3) == ls)
			return (s->r[R_ESP] - 32) & ~3u;
	}
	return HOST_STACK_SZ - 16;
}

/* the real-mode stack for a trip down: below whatever's already on it */
static uint16_t rstack_sp(struct x86 *c, struct dpmi *d)
{
	if (!(c->cr0 & CR0_PE) && SEG(c, S_SS) == d->rm_stack_seg)
		return (uint16_t)((c->r[R_ESP] - 32) & ~1u);
	for (int i = d->depth - 1; i >= 0; i--) {
		struct cpustate *s = &d->fr[i].saved;
		if (!(s->cr0 & CR0_PE) && s->s[S_SS].sel == d->rm_stack_seg)
			return (uint16_t)((s->r[R_ESP] - 32) & ~1u);
	}
	return RM_STACK_PARAS * 16 - 16;
}

static struct frame *push_frame(struct dosbox *b, struct dpmi *d, int kind)
{
	if (d->depth >= MAX_FRAMES) {
		snprintf(b->errbuf, sizeof b->errbuf, "DPMI: too many nested mode switches");
		b->errmsg = b->errbuf;
		b->cpu.state = XS_STOP;
		b->running = 0;
		return NULL;
	}
	struct frame *f = &d->fr[d->depth++];
	f->kind = kind;
	save(&b->cpu, &f->saved);
	f->regs_lin = 0;
	return f;
}

/* where an interrupt to real-mode vector n really goes (our hooks pass it on) */
static uint32_t rm_target(struct dosbox *b, struct dpmi *d, int n)
{
	uint32_t v = md(b, (uint32_t)n * 4);
	if (d->rm_hooked[n] && v == ((uint32_t)BIOS_SEG << 16 | (uint32_t)(ROM_RM_TO_PM + n * 4)))
		v = d->rm_orig[n];
	return v;
}

/* ---- going down to real mode ---------------------------------------------------------- */

/* real-mode stack set up, then CS:IP = target with an IRET (or RETF) frame back to us */
static void rm_enter(struct dosbox *b, struct dpmi *d, uint32_t target, int iret_frame, uint32_t rm_flags)
{
	struct x86 *c = &b->cpu;
	uint16_t sp = rstack_sp(c, d);
	to_real(c);
	rm_seg(c, S_SS, d->rm_stack_seg);
	c->r[R_ESP] = sp;
	if (iret_frame)
		x86_push16(c, (uint16_t)rm_flags);
	x86_push16(c, BIOS_SEG);
	x86_push16(c, ROM_DPMI_RET);
	x86_set_flags(c, rm_flags & ~(FL_IF | FL_TF), 0xFFFF);
	rm_seg(c, S_CS, (uint16_t)(target >> 16));
	c->eip = target & 0xFFFF;
}

/* an interrupt (INT n or an IRQ) with no protected-mode handler: run the real-mode one */
static void reflect(struct dosbox *b, struct dpmi *d, int n, int hw)
{
	struct x86 *c = &b->cpu;
	if (!push_frame(b, d, hw ? FR_IRQ : FR_INT))
		return;
	uint32_t flags = x86_flags(c);
	/* DS and ES mean nothing down there: point them at the stack */
	rm_enter(b, d, rm_target(b, d, n), 1, flags);
	rm_seg(c, S_DS, d->rm_stack_seg);
	rm_seg(c, S_ES, d->rm_stack_seg);
	rm_seg(c, S_FS, 0);
	rm_seg(c, S_GS, 0);
}

/* the real-mode call structure */
enum { RC_EDI = 0, RC_ESI = 4, RC_EBP = 8, RC_EBX = 16, RC_EDX = 20, RC_ECX = 24, RC_EAX = 28,
       RC_FLAGS = 32, RC_ES = 34, RC_DS = 36, RC_FS = 38, RC_GS = 40, RC_IP = 42, RC_CS = 44, RC_SP = 46, RC_SS = 48 };

static void regs_to_struct(struct dosbox *b, uint32_t a)
{
	struct x86 *c = &b->cpu;
	mdw(b, a + RC_EDI, c->r[R_EDI]);
	mdw(b, a + RC_ESI, c->r[R_ESI]);
	mdw(b, a + RC_EBP, c->r[R_EBP]);
	mdw(b, a + 12, 0);
	mdw(b, a + RC_EBX, c->r[R_EBX]);
	mdw(b, a + RC_EDX, c->r[R_EDX]);
	mdw(b, a + RC_ECX, c->r[R_ECX]);
	mdw(b, a + RC_EAX, c->r[R_EAX]);
	mww(b, a + RC_FLAGS, (uint16_t)x86_flags(c));
	mww(b, a + RC_ES, SEG(c, S_ES));
	mww(b, a + RC_DS, SEG(c, S_DS));
	mww(b, a + RC_FS, SEG(c, S_FS));
	mww(b, a + RC_GS, SEG(c, S_GS));
}

static void struct_to_regs(struct dosbox *b, uint32_t a)
{
	struct x86 *c = &b->cpu;
	c->r[R_EDI] = md(b, a + RC_EDI);
	c->r[R_ESI] = md(b, a + RC_ESI);
	c->r[R_EBP] = md(b, a + RC_EBP);
	c->r[R_EBX] = md(b, a + RC_EBX);
	c->r[R_EDX] = md(b, a + RC_EDX);
	c->r[R_ECX] = md(b, a + RC_ECX);
	c->r[R_EAX] = md(b, a + RC_EAX);
	rm_seg(c, S_ES, mw(b, a + RC_ES));
	rm_seg(c, S_DS, mw(b, a + RC_DS));
	rm_seg(c, S_FS, mw(b, a + RC_FS));
	rm_seg(c, S_GS, mw(b, a + RC_GS));
}

/* 0300h / 0301h / 0302h */
static void call_rm(struct dosbox *b, struct dpmi *d, int how)
{
	struct x86 *c = &b->cpu;
	uint32_t a = c->s[S_ES].base + (d->bits32 ? c->r[R_EDI] : DI(c));
	int words = CX(c);
	int vec = BL(c);
	/* parameters come off the client's stack, above the INT 31h frame */
	uint32_t from = sp_lin(c) + (uint32_t)sz(d) * 3;
	uint32_t pm_if = rd_sz(c, sp_lin(c) + (uint32_t)sz(d) * 2, sz(d)) & FL_IF;
	struct frame *f = push_frame(b, d, FR_CALL);
	if (!f)
		return;
	f->regs_lin = a;
	uint16_t ss = mw(b, a + RC_SS), sp = mw(b, a + RC_SP);
	uint16_t hsp = rstack_sp(c, d);
	to_real(c);
	if (ss || sp) {
		rm_seg(c, S_SS, ss);
		c->r[R_ESP] = sp;
	} else {
		rm_seg(c, S_SS, d->rm_stack_seg);
		c->r[R_ESP] = hsp;
	}
	for (int i = words - 1; i >= 0; i--)
		x86_push16(c, x86_rd16(c, from + (uint32_t)i * 2));
	uint32_t fl = (mw(b, a + RC_FLAGS) & 0x0FD5) | pm_if | 2;
	uint32_t target;
	if (how == 0)
		target = rm_target(b, d, vec);
	else
		target = (uint32_t)mw(b, a + RC_CS) << 16 | mw(b, a + RC_IP);
	if (how != 1)
		x86_push16(c, (uint16_t)fl);
	x86_push16(c, BIOS_SEG);
	x86_push16(c, ROM_DPMI_RET);
	struct_to_regs(b, a);
	x86_set_flags(c, how == 0 ? fl & ~(FL_IF | FL_TF) : fl, 0xFFFF);
	rm_seg(c, S_CS, (uint16_t)(target >> 16));
	c->eip = target & 0xFFFF;
}

/* ---- coming up to protected mode ------------------------------------------------------ */

/* call a protected-mode handler as an interrupt, on the locked stack, returning to ROM_PM_RET */
static void pm_call_handler(struct dosbox *b, struct dpmi *d, struct far32 h)
{
	struct x86 *c = &b->cpu;
	uint32_t esp = lstack_esp(c, d);
	uint32_t fl = x86_flags(c) & ~(FL_VM | FL_IF | FL_TF | FL_NT);
	to_pm(b, d);
	pm_seg(c, S_SS, sel_of(IDX_STACK));
	c->r[R_ESP] = esp;
	for (int s = S_ES; s <= S_GS; s++)
		if (s != S_CS && s != S_SS)
			pm_seg(c, s, 0);
	push_sz(c, d, fl | FL_IOPL);
	push_sz(c, d, code_sel(d));
	push_sz(c, d, ROM_PM_RET);
	x86_set_flags(c, FL_IOPL, FL_IOPL | FL_IF | FL_TF);
	pm_jump(c, h.sel, h.off);
}

/* ---- terminating ---------------------------------------------------------------------- */

static void pm_crash(struct dosbox *b, const char *what)
{
	struct x86 *c = &b->cpu;
	snprintf(b->errbuf, sizeof b->errbuf, "%s at %04X:%08X", what, SEG(c, S_CS), c->eip);
	b->errmsg = b->errbuf;
	/* down to real mode and out through DOS */
	struct dpmi *d = D(b);
	if (d) {
		to_real(c);
		rm_seg(c, S_SS, d->rm_stack_seg);
		c->r[R_ESP] = RM_STACK_PARAS * 16 - 16;
		rm_seg(c, S_DS, d->rm_stack_seg);
		rm_seg(c, S_ES, d->rm_stack_seg);
	}
	dos_terminate(b, 255, 0);
}

void dpmi_terminating(struct dosbox *b, uint16_t psp)
{
	struct dpmi *d = D(b);
	if (!d || d->psp != psp)
		return;
	/* put back the real-mode vectors we took over */
	for (int n = 0; n < 256; n++)
		if (d->rm_hooked[n] && md(b, (uint32_t)n * 4) == ((uint32_t)BIOS_SEG << 16 | (uint32_t)(ROM_RM_TO_PM + n * 4)))
			mdw(b, (uint32_t)n * 4, d->rm_orig[n]);
	if (d->env_seg)
		mww(b, sl(psp, 0x2C), d->env_seg);
	dpmi_free(b);
}

void dpmi_free(struct dosbox *b)
{
	struct dpmi *d = D(b);
	if (!d)
		return;
	for (int i = 0; i < MAX_EXT; i++)
		if (d->mem[i])
			ext_free(b, i + 1);
	int h = d->handle;
	b->cpu.a20 = d->a20;
	b->dpmi_lin = 0;
	ext_free(b, h);
	struct x86 *c = &b->cpu;
	if (c->cr0 & CR0_PE) {
		to_real(c);
		for (int s = S_ES; s <= S_GS; s++)
			rm_seg(c, s, 0);
	}
	c->idt.base = 0;
	c->idt.limit = 0x3FF;
}

/* ---- detection and the initial switch ------------------------------------------------- */

int dpmi_detect(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	set16(c, R_EAX, 0);
	set16(c, R_EBX, 1);			/* 32-bit programs supported */
	setlo(c, R_ECX, (uint8_t)(c->cpu_level >= 4 ? 4 : 3));
	set16(c, R_EDX, 0x005A);		/* 0.90 */
	set16(c, R_ESI, RM_STACK_PARAS);
	x86_load_seg_real(c, S_ES, BIOS_SEG);
	set16(c, R_EDI, ROM_DPMI_SW);
	return 0;
}

static void enter_pm_first(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	if (b->dpmi_lin) {
		x86_set_flags(c, FL_CF, FL_CF);	/* one client at a time */
		set16(c, R_EAX, 0x8011);
		return;
	}
	int handle;
	uint32_t lin = ext_alloc(b, HOST_SIZE, &handle);
	if (!lin) {
		x86_set_flags(c, FL_CF, FL_CF);
		set16(c, R_EAX, 0x8012);
		return;
	}
	memset(c->mem + lin, 0, HOST_SIZE);
	b->dpmi_lin = lin;
	struct dpmi *d = D(b);
	d->lin = lin;
	d->handle = handle;
	d->bits32 = AX(c) & 1;
	d->psp = b->psp;
	d->rm_stack_seg = SEG(c, S_ES);
	d->a20 = c->a20;
	c->a20 = ~0u;		/* all of memory, as any DPMI host gives */
	/* the IDT: every vector to its stub */
	uint16_t cs = code_sel(d);
	for (int n = 0; n < 256; n++) {
		uint32_t off = ROM_PM_STUBS + (uint32_t)n * 4;
		uint32_t a = lin + HOST_IDT + (uint32_t)n * 8;
		mdw(b, a, (uint32_t)cs << 16 | (off & 0xFFFF));
		mdw(b, a + 4, (off & 0xFFFF0000) | 0x8000 | 3 << 13 | (uint32_t)(d->bits32 ? 0xE : 0x6) << 8);
	}
	for (int i = IDX_CODE16; i < IDX_FIRST_FREE; i++)
		d->ldt_used[i] = 1;
	desc_set(b, d, IDX_CODE16, 0xF0000, 0xFFFF, 0xFA, 0);
	desc_set(b, d, IDX_CODE32, 0xF0000, 0xFFFF, 0xFA, 1);
	desc_set(b, d, IDX_STACK, lin + HOST_STACK, HOST_STACK_SZ - 1, 0xF2, d->bits32);
	for (int i = 0; i < 4; i++)
		desc_set(b, d, IDX_CBALIAS + i, 0, 0xFFFF, 0xF2, 0);
	/* the far call that got us here: we return from it in protected mode */
	uint16_t ip = x86_pop16(c), rcs = x86_pop16(c);
	uint16_t ds = SEG(c, S_DS), ss = SEG(c, S_SS);
	uint16_t sel_cs = seg_selector(b, d, rcs, 1);
	uint16_t sel_ds = seg_selector(b, d, ds, 0);
	uint16_t sel_ss = seg_selector(b, d, ss, 0);
	int pidx = desc_alloc(b, d, 1);
	desc_set(b, d, pidx, (uint32_t)b->psp << 4, 0xFF, 0xF2, 0);
	/* the environment becomes a selector */
	uint16_t env = mw(b, sl(b->psp, 0x2C));
	if (env) {
		uint32_t len = (uint32_t)mw(b, sl((uint16_t)(env - 1), 3)) * 16;
		int eidx = desc_alloc(b, d, 1);
		desc_set(b, d, eidx, (uint32_t)env << 4, len ? len - 1 : 0xFFFF, 0xF2, 0);
		d->env_seg = env;
		mww(b, sl(b->psp, 0x2C), sel_of(eidx));
	}
	uint32_t sp = c->r[R_ESP] & 0xFFFF;
	to_pm(b, d);
	pm_seg(c, S_DS, sel_ds);
	pm_seg(c, S_SS, sel_ss);
	pm_seg(c, S_ES, sel_of(pidx));
	pm_seg(c, S_FS, 0);
	pm_seg(c, S_GS, 0);
	c->r[R_ESP] = sp;
	pm_jump(c, sel_cs, ip);
	x86_set_flags(c, 0, FL_CF);
}

/* ---- INT 31h -------------------------------------------------------------------------- */

static void ret_cf(struct x86 *c, struct dpmi *d, int on)
{
	/* in the stacked EFLAGS the stub's IRET restores */
	uint32_t a = sp_lin(c) + (uint32_t)sz(d) * 2;
	uint16_t v = x86_rd16(c, a);
	x86_wr16(c, a, on ? (uint16_t)(v | FL_CF) : (uint16_t)(v & ~FL_CF));
}

static void fail(struct x86 *c, struct dpmi *d, uint16_t code)
{
	set16(c, R_EAX, code);
	ret_cf(c, d, 1);
}

static uint32_t esdi(struct x86 *c, struct dpmi *d)
{
	return c->s[S_ES].base + (d->bits32 ? c->r[R_EDI] : DI(c));
}

static uint32_t dx32(struct x86 *c, struct dpmi *d) { return d->bits32 ? c->r[R_EDX] : DX(c); }

static int is_default_vec(struct dpmi *d, struct far32 v, int n)
{
	return (v.sel | 3) == (code_sel(d) | 3) && v.off == (uint32_t)(ROM_PM_DEF + n * 4);
}

static int irq_vector(struct dosbox *b, int n)
{
	return (n >= b->pic_base[0] && n < b->pic_base[0] + 8) || (n >= b->pic_base[1] && n < b->pic_base[1] + 8);
}

/* a protected-mode handler for an IRQ (or 1Ch/23h/24h) gets the real-mode ones too */
static void hook_rm(struct dosbox *b, struct dpmi *d, int n, int on)
{
	uint32_t stub = (uint32_t)BIOS_SEG << 16 | (uint32_t)(ROM_RM_TO_PM + n * 4);
	if (on && !d->rm_hooked[n]) {
		d->rm_orig[n] = md(b, (uint32_t)n * 4);
		d->rm_hooked[n] = 1;
		mdw(b, (uint32_t)n * 4, stub);
	} else if (!on && d->rm_hooked[n]) {
		if (md(b, (uint32_t)n * 4) == stub)
			mdw(b, (uint32_t)n * 4, d->rm_orig[n]);
		d->rm_hooked[n] = 0;
	}
}

static int find_mem(struct dpmi *d, uint32_t h)
{
	return h >= 1 && h <= MAX_EXT && d->mem[h - 1] ? (int)h : 0;
}

void dpmi_int31(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	struct dpmi *d = D(b);
	ret_cf(c, d, 0);
	switch (AX(c)) {
	case 0x0000: {		/* allocate descriptors */
		int idx = desc_alloc(b, d, CX(c));
		if (idx < 0)
			return fail(c, d, 0x8011);
		set16(c, R_EAX, sel_of(idx));
		return;
	}
	case 0x0001: {		/* free a descriptor */
		uint16_t sel = BX(c);
		if (!sel_ok(d, sel))
			return fail(c, d, 0x8022);
		d->ldt_used[idx_of(sel)] = 0;
		desc_set(b, d, idx_of(sel), 0, 0, 0, 0);
		for (int i = 0; i < d->nsegsel; i++)
			if ((d->segsel[i].sel | 3) == (sel | 3))
				d->segsel[i] = d->segsel[--d->nsegsel];
		for (int s = S_ES; s <= S_GS; s++)
			if (s != S_CS && s != S_SS && (c->s[s].sel | 3) == (sel | 3))
				pm_seg(c, s, 0);
		return;
	}
	case 0x0002: {		/* segment to descriptor */
		uint16_t sel = seg_selector(b, d, BX(c), 0);
		if (!sel)
			return fail(c, d, 0x8011);
		set16(c, R_EAX, sel);
		return;
	}
	case 0x0003: set16(c, R_EAX, 8); return;
	case 0x0004: case 0x0005: return;	/* lock / unlock selector memory */
	case 0x0006: {		/* get base */
		if (!sel_ok(d, BX(c)))
			return fail(c, d, 0x8022);
		uint32_t base = desc_base(b, d, idx_of(BX(c)));
		set16(c, R_ECX, (uint16_t)(base >> 16));
		set16(c, R_EDX, (uint16_t)base);
		return;
	}
	case 0x0007: {		/* set base */
		if (!sel_ok(d, BX(c)))
			return fail(c, d, 0x8022);
		uint32_t a = desc_lin(d, idx_of(BX(c)));
		uint32_t base = (uint32_t)CX(c) << 16 | DX(c);
		uint32_t lo = md(b, a), hi = md(b, a + 4);
		lo = (lo & 0xFFFF) | base << 16;
		hi = (hi & 0x00FFFF00) | (base & 0xFF000000) | ((base >> 16) & 0xFF);
		mdw(b, a, lo);
		mdw(b, a + 4, hi);
		reload(c, BX(c));
		return;
	}
	case 0x0008: {		/* set limit */
		if (!sel_ok(d, BX(c)))
			return fail(c, d, 0x8022);
		uint32_t limit = (uint32_t)CX(c) << 16 | DX(c);
		if (limit > 0xFFFFF && (limit & 0xFFF) != 0xFFF)
			return fail(c, d, 0x8021);
		uint32_t a = desc_lin(d, idx_of(BX(c)));
		uint32_t lo = md(b, a), hi = md(b, a + 4);
		hi &= ~(0xF0000u | 0x800000u);
		if (limit > 0xFFFFF) {
			limit >>= 12;
			hi |= 0x800000;
		}
		lo = (lo & 0xFFFF0000) | (limit & 0xFFFF);
		hi |= limit & 0xF0000;
		mdw(b, a, lo);
		mdw(b, a + 4, hi);
		reload(c, BX(c));
		return;
	}
	case 0x0009: {		/* set access rights */
		if (!sel_ok(d, BX(c)))
			return fail(c, d, 0x8022);
		uint8_t acc = CL(c), ext = CH(c);
		if (((acc >> 5) & 3) != 3 || !(acc & 0x10))
			return fail(c, d, 0x8021);
		uint32_t a = desc_lin(d, idx_of(BX(c)));
		uint32_t hi = md(b, a + 4);
		hi = (hi & ~0x00F0FF00u) | (uint32_t)acc << 8 | (uint32_t)(ext & 0xD0) << 16;
		mdw(b, a + 4, hi);
		reload(c, BX(c));
		return;
	}
	case 0x000A: {		/* a data alias of a code descriptor */
		if (!sel_ok(d, BX(c)))
			return fail(c, d, 0x8022);
		int idx = desc_alloc(b, d, 1);
		if (idx < 0)
			return fail(c, d, 0x8011);
		uint32_t from = desc_lin(d, idx_of(BX(c))), to = desc_lin(d, idx);
		mdw(b, to, md(b, from));
		mdw(b, to + 4, (md(b, from + 4) & ~0x0E00u) | 0x0200);	/* data, writable */
		set16(c, R_EAX, sel_of(idx));
		return;
	}
	case 0x000B: {		/* get descriptor */
		if (!sel_ok(d, BX(c)) && !(BX(c) >> 3 >= IDX_CODE16 && BX(c) >> 3 < IDX_FIRST_FREE && (BX(c) & 4)))
			return fail(c, d, 0x8022);
		uint32_t a = desc_lin(d, idx_of(BX(c))), to = esdi(c, d);
		x86_wr32(c, to, md(b, a));
		x86_wr32(c, to + 4, md(b, a + 4));
		return;
	}
	case 0x000C: {		/* set descriptor */
		if (!sel_ok(d, BX(c)))
			return fail(c, d, 0x8022);
		uint32_t from = esdi(c, d);
		uint32_t lo = x86_rd32(c, from), hi = x86_rd32(c, from + 4);
		if ((hi & 0x8000) && (((hi >> 13) & 3) != 3 || !(hi & 0x1000)))
			return fail(c, d, 0x8021);
		uint32_t a = desc_lin(d, idx_of(BX(c)));
		mdw(b, a, lo);
		mdw(b, a + 4, hi);
		reload(c, BX(c));
		return;
	}
	case 0x000D: {		/* a specific descriptor (the first 16) */
		int idx = idx_of(BX(c));
		if (idx >= 16 || (BX(c) & 7) != 7)
			return fail(c, d, 0x8022);
		if (d->ldt_used[idx])
			return fail(c, d, 0x8011);
		d->ldt_used[idx] = 1;
		desc_set(b, d, idx, 0, 0, 0xF2, d->bits32);
		return;
	}
	case 0x0100: {		/* DOS memory */
		uint16_t largest = 0, paras = BX(c);
		uint16_t seg = mcb_alloc(b, paras, d->psp, &largest);
		if (!seg) {
			set16(c, R_EBX, largest);
			return fail(c, d, 0x0008);
		}
		int n = (paras + 0xFFF) / 0x1000;
		if (!n)
			n = 1;
		int idx = desc_alloc(b, d, n);
		if (idx < 0) {
			mcb_free(b, seg);
			return fail(c, d, 0x8011);
		}
		uint32_t bytes = (uint32_t)paras * 16;
		for (int i = 0; i < n; i++) {
			uint32_t left = bytes - (uint32_t)i * 0x10000;
			desc_set(b, d, idx + i, ((uint32_t)seg << 4) + (uint32_t)i * 0x10000,
			    i == 0 ? (bytes ? bytes - 1 : 0) : MIN(left, 0x10000u) - 1, 0xF2, 0);
		}
		set16(c, R_EAX, seg);
		set16(c, R_EDX, sel_of(idx));
		return;
	}
	case 0x0101: case 0x0102: {
		uint16_t sel = DX(c);
		if (!sel_ok(d, sel))
			return fail(c, d, 0x8022);
		uint16_t seg = (uint16_t)(desc_base(b, d, idx_of(sel)) >> 4);
		uint16_t paras = mw(b, sl((uint16_t)(seg - 1), 3));
		int n = (paras + 0xFFF) / 0x1000;
		if (!n)
			n = 1;
		if (AX(c) == 0x0101) {
			mcb_free(b, seg);
			for (int i = 0; i < n; i++) {
				d->ldt_used[idx_of(sel) + i] = 0;
				desc_set(b, d, idx_of(sel) + i, 0, 0, 0, 0);
			}
			return;
		}
		uint16_t largest = 0;
		int nn = (BX(c) + 0xFFF) / 0x1000;
		if (nn > n)
			for (int i = n; i < nn; i++)
				if (idx_of(sel) + i >= LDT_ENTRIES || d->ldt_used[idx_of(sel) + i])
					return fail(c, d, 0x8011);
		if (mcb_resize(b, seg, BX(c), &largest)) {
			set16(c, R_EBX, largest);
			return fail(c, d, 0x0008);
		}
		uint32_t bytes = (uint32_t)BX(c) * 16;
		for (int i = 0; i < MAX(n, nn); i++) {
			int idx = idx_of(sel) + i;
			if (i >= nn) {
				d->ldt_used[idx] = 0;
				desc_set(b, d, idx, 0, 0, 0, 0);
				continue;
			}
			d->ldt_used[idx] = 1;
			uint32_t left = bytes - (uint32_t)i * 0x10000;
			desc_set(b, d, idx, ((uint32_t)seg << 4) + (uint32_t)i * 0x10000,
			    i == 0 ? (bytes ? bytes - 1 : 0) : MIN(left, 0x10000u) - 1, 0xF2, 0);
		}
		return;
	}
	case 0x0200: {		/* real-mode vector */
		uint32_t v = md(b, (uint32_t)BL(c) * 4);
		if (d->rm_hooked[BL(c)] && v == ((uint32_t)BIOS_SEG << 16 | (uint32_t)(ROM_RM_TO_PM + BL(c) * 4)))
			v = d->rm_orig[BL(c)];
		set16(c, R_ECX, (uint16_t)(v >> 16));
		set16(c, R_EDX, (uint16_t)v);
		return;
	}
	case 0x0201: {
		uint32_t v = (uint32_t)CX(c) << 16 | DX(c);
		if (d->rm_hooked[BL(c)])
			d->rm_orig[BL(c)] = v;	/* still ours on top, passing on to theirs */
		else
			mdw(b, (uint32_t)BL(c) * 4, v);
		return;
	}
	case 0x0202: {		/* exception handler */
		if (BL(c) >= 32)
			return fail(c, d, 0x8021);
		struct far32 v = d->exc_vec[BL(c)];
		if (!v.sel) {
			v.sel = code_sel(d);
			v.off = ROM_PM_EXC + (uint32_t)BL(c) * 4;
		}
		set16(c, R_ECX, v.sel);
		if (d->bits32)
			c->r[R_EDX] = v.off;
		else
			set16(c, R_EDX, (uint16_t)v.off);
		return;
	}
	case 0x0203: {
		if (BL(c) >= 32)
			return fail(c, d, 0x8021);
		struct far32 v = { CX(c), dx32(c, d) };
		if ((v.sel | 3) == (code_sel(d) | 3) && v.off == ROM_PM_EXC + (uint32_t)BL(c) * 4)
			v.sel = 0;
		d->exc_vec[BL(c)] = v;
		return;
	}
	case 0x0204: {		/* protected-mode vector */
		struct far32 v = d->pm_vec[BL(c)];
		if (!v.sel) {
			v.sel = code_sel(d);
			v.off = ROM_PM_DEF + (uint32_t)BL(c) * 4;
		}
		set16(c, R_ECX, v.sel);
		if (d->bits32)
			c->r[R_EDX] = v.off;
		else
			set16(c, R_EDX, (uint16_t)v.off);
		return;
	}
	case 0x0205: {
		int n = BL(c);
		struct far32 v = { CX(c), dx32(c, d) };
		if (is_default_vec(d, v, n))
			v.sel = 0;
		d->pm_vec[n] = v;
		if (irq_vector(b, n) || n == 0x1C || n == 0x23 || n == 0x24)
			hook_rm(b, d, n, v.sel != 0);
		return;
	}
	case 0x0300: case 0x0301: case 0x0302:
		call_rm(b, d, AX(c) - 0x0300);
		return;
	case 0x0303: {		/* allocate a real-mode callback */
		for (int i = 0; i < MAX_CB; i++)
			if (!d->cb[i].used) {
				d->cb[i].used = 1;
				d->cb[i].proc.sel = SEG(c, S_DS);
				d->cb[i].proc.off = d->bits32 ? c->r[R_ESI] : SI(c);
				d->cb[i].regs.sel = SEG(c, S_ES);
				d->cb[i].regs.off = d->bits32 ? c->r[R_EDI] : DI(c);
				set16(c, R_ECX, BIOS_SEG);
				set16(c, R_EDX, (uint16_t)(ROM_RM_CB + i * 4));
				return;
			}
		return fail(c, d, 0x8015);
	}
	case 0x0304: {
		int i = (DX(c) - ROM_RM_CB) / 4;
		if (CX(c) != BIOS_SEG || i < 0 || i >= MAX_CB || !d->cb[i].used)
			return fail(c, d, 0x8024);
		d->cb[i].used = 0;
		return;
	}
	case 0x0305:		/* state save/restore: nothing to save */
		set16(c, R_EAX, 0);
		set16(c, R_EBX, BIOS_SEG);
		set16(c, R_ECX, ROM_SAVE);
		set16(c, R_ESI, code_sel(d));
		c->r[R_EDI] = ROM_SAVE;
		return;
	case 0x0306:		/* raw mode switch */
		set16(c, R_EBX, BIOS_SEG);
		set16(c, R_ECX, ROM_RAW_TO_PM);
		set16(c, R_ESI, code_sel(d));
		c->r[R_EDI] = ROM_RAW_TO_RM;
		return;
	case 0x0400:
		set16(c, R_EAX, 0x005A);
		set16(c, R_EBX, 0x0003);	/* 32-bit host; interrupts go back to real mode */
		setlo(c, R_ECX, (uint8_t)(c->cpu_level >= 4 ? 4 : 3));
		sethi(c, R_EDX, b->pic_base[0]);
		setlo(c, R_EDX, b->pic_base[1]);
		return;
	case 0x0500: {		/* free memory */
		uint32_t big = 0, total = ext_free_kb(b, &big);
		uint32_t a = esdi(c, d);
		for (int i = 0; i < 48; i += 4)
			x86_wr32(c, a + (uint32_t)i, 0xFFFFFFFF);
		x86_wr32(c, a, big * 1024);
		x86_wr32(c, a + 4, big / 4);
		x86_wr32(c, a + 8, big / 4);
		x86_wr32(c, a + 0x0C, c->mem_size / 4096);
		x86_wr32(c, a + 0x10, total / 4);
		x86_wr32(c, a + 0x14, total / 4);
		x86_wr32(c, a + 0x18, c->mem_size / 4096);
		x86_wr32(c, a + 0x1C, total / 4);
		x86_wr32(c, a + 0x20, 0);
		return;
	}
	case 0x0501: {		/* allocate a memory block */
		uint32_t size = (uint32_t)BX(c) << 16 | CX(c);
		int h;
		uint32_t lin = size ? ext_alloc(b, size, &h) : 0;
		if (!lin)
			return fail(c, d, 0x8012);
		d->mem[h - 1] = 1;
		set16(c, R_EBX, (uint16_t)(lin >> 16));
		set16(c, R_ECX, (uint16_t)lin);
		set16(c, R_ESI, 0);
		set16(c, R_EDI, (uint16_t)h);
		return;
	}
	case 0x0502: {
		int h = find_mem(d, (uint32_t)SI(c) << 16 | DI(c));
		if (!h)
			return fail(c, d, 0x8023);
		d->mem[h - 1] = 0;
		ext_free(b, h);
		return;
	}
	case 0x0503: {		/* resize: may move */
		int h = find_mem(d, (uint32_t)SI(c) << 16 | DI(c));
		uint32_t size = (uint32_t)BX(c) << 16 | CX(c);
		if (!h)
			return fail(c, d, 0x8023);
		if (!size)
			return fail(c, d, 0x8021);
		uint32_t old_base = b->xms[h - 1].base, old_size = b->xms[h - 1].size;
		b->xms[h - 1].used = 0;
		int nh;
		uint32_t nb = ext_alloc(b, size, &nh);
		if (!nb) {
			b->xms[h - 1].used = 1;
			return fail(c, d, 0x8012);
		}
		b->xms[nh - 1].used = 0;
		b->xms[h - 1].used = 1;
		b->xms[h - 1].base = nb;
		b->xms[h - 1].size = b->xms[nh - 1].size;
		if (nb != old_base)
			memmove(c->mem + nb, c->mem + old_base, MIN(old_size, b->xms[h - 1].size));
		set16(c, R_EBX, (uint16_t)(nb >> 16));
		set16(c, R_ECX, (uint16_t)nb);
		return;
	}
	case 0x0600: case 0x0601: case 0x0602: case 0x0603: case 0x0702: case 0x0703:
		return;
	case 0x0604:
		set16(c, R_EBX, 0);
		set16(c, R_ECX, 4096);
		return;
	case 0x0800: {		/* physical to linear: we have no paging */
		uint32_t phys = (uint32_t)BX(c) << 16 | CX(c);
		if (phys < 0x100000 && phys + ((uint32_t)SI(c) << 16 | DI(c)) > 0x100000)
			return fail(c, d, 0x8021);
		return;	/* BX:CX already the answer */
	}
	case 0x0801:
		return;
	case 0x0900: case 0x0901: case 0x0902: {	/* the virtual interrupt flag: the stacked IF */
		uint32_t a = sp_lin(c) + (uint32_t)sz(d) * 2;
		uint16_t f = x86_rd16(c, a);
		setlo(c, R_EAX, (f & FL_IF) ? 1 : 0);
		if (AX(c) == 0x0900)
			x86_wr16(c, a, (uint16_t)(f & ~FL_IF));
		else if (AX(c) == 0x0901)
			x86_wr16(c, a, (uint16_t)(f | FL_IF));
		return;
	}
	case 0x0E00:
		set16(c, R_EAX, (uint16_t)((c->cpu_level >= 4 ? 4 : 3) << 4 | 0x05));	/* MP, FPU present */
		return;
	case 0x0E01:
		return;
	default:
		return fail(c, d, 0x8001);
	}
}

/* ---- the default action for interrupt n, in protected mode ------------------------------ */

static void pm_default(struct dosbox *b, struct dpmi *d, int n, int kind)
{
	struct x86 *c = &b->cpu;
	if (kind == 1 || kind == 3) {
		/* software interrupts (3: chained to us by the client) */
		if (n == 0x31) {
			dpmi_int31(b);
			return;
		}
		if (n == 0x21 && AH(c) == 0x25) {		/* set vector: means the PM one here */
			struct far32 v = { SEG(c, S_DS), dx32(c, d) };
			if (is_default_vec(d, v, AL(c)))
				v.sel = 0;
			d->pm_vec[AL(c)] = v;
			if (irq_vector(b, AL(c)) || AL(c) == 0x1C || AL(c) == 0x23 || AL(c) == 0x24)
				hook_rm(b, d, AL(c), v.sel != 0);
			return;
		}
		if (n == 0x21 && AH(c) == 0x35) {
			struct far32 v = d->pm_vec[AL(c)];
			if (!v.sel) {
				v.sel = code_sel(d);
				v.off = ROM_PM_DEF + (uint32_t)AL(c) * 4;
			}
			pm_seg(c, S_ES, v.sel);
			if (d->bits32)
				c->r[R_EBX] = v.off;
			else
				set16(c, R_EBX, (uint16_t)v.off);
			return;
		}
		if (n == 0x2F && AX(c) == 0x1686) {	/* in protected mode: yes */
			set16(c, R_EAX, 0);
			return;
		}
	}
	reflect(b, d, n, kind == 2);
}

/* the exception frame a DPMI exception handler expects, on the locked stack */
static void call_exception(struct dosbox *b, struct dpmi *d, int n, struct far32 h)
{
	struct x86 *c = &b->cpu;
	int w = sz(d);
	/* what the CPU pushed: [err] eip cs eflags */
	uint32_t a = sp_lin(c);
	uint32_t err = 0;
	if (c->fault_has_err) {
		err = rd_sz(c, a, w);
		a += (uint32_t)w;
	}
	uint32_t eip = rd_sz(c, a, w), cs = rd_sz(c, a + (uint32_t)w, w) & 0xFFFF;
	uint32_t fl = rd_sz(c, a + (uint32_t)w * 2, w);
	uint32_t esp = c->r[R_ESP] + (uint32_t)w * (3 + (c->fault_has_err ? 1 : 0));
	uint16_t ss = SEG(c, S_SS);
	uint32_t lesp = lstack_esp(c, d);
	pm_seg(c, S_SS, sel_of(IDX_STACK));
	c->r[R_ESP] = lesp;
	push_sz(c, d, ss);
	push_sz(c, d, esp);
	push_sz(c, d, fl);
	push_sz(c, d, cs);
	push_sz(c, d, eip);
	push_sz(c, d, err);
	push_sz(c, d, code_sel(d));
	push_sz(c, d, ROM_EXC_RET);
	x86_set_flags(c, 0, FL_IF | FL_TF);
	pm_jump(c, h.sel, h.off);
}

static const char *exc_name(int n)
{
	switch (n) {
	case 0: return "Divide error";
	case 6: return "Invalid instruction";
	case 12: return "Stack fault";
	case 13: return "General protection fault";
	case 14: return "Page fault";
	default: return "Exception";
	}
}

/* ---- the HLE entry -------------------------------------------------------------------- */

int dpmi_hle(struct x86 *c, int n)
{
	struct dosbox *b = BOX(c);
	struct dpmi *d = D(b);
	if (n == HLE_DPMI_SW) {
		if (c->cr0 & CR0_PE)
			return 0;
		enter_pm_first(b);
		return 0;
	}
	if (!d)
		return 0;
	switch (n) {
	case HLE_PM_INT: {		/* every IDT entry lands here */
		int v = x86_rd8(c, c->s[S_CS].base + c->eip);
		c->eip++;
		int kind = c->int_kind;
		if (kind == 0 && v < 32) {
			if (d->exc_vec[v].sel) {
				call_exception(b, d, v, d->exc_vec[v]);
				return 0;
			}
			if (v < 8 && d->pm_vec[v].sel && !c->fault_has_err) {
				pm_jump(c, d->pm_vec[v].sel, d->pm_vec[v].off);
				return 0;
			}
			pm_crash(b, exc_name(v));
			return 0;
		}
		if (d->pm_vec[v].sel) {
			/* the CPU's frame is already there: just go */
			pm_jump(c, d->pm_vec[v].sel, d->pm_vec[v].off);
			return 0;
		}
		pm_default(b, d, v, kind);
		return 0;
	}
	case HLE_PM_DEF: {		/* the client chained to the default */
		int v = x86_rd8(c, c->s[S_CS].base + c->eip);
		c->eip++;
		pm_default(b, d, v, irq_vector(b, v) && v != 0x31 && v != 0x21 && v != 0x2F ? 2 : 3);
		return 0;
	}
	case HLE_PM_EXC: {		/* the default exception handler, called as one */
		int v = x86_rd8(c, c->s[S_CS].base + c->eip);
		/* the frame: ret eip/cs (ours, unused), err, eip, cs ... */
		int w = sz(d);
		uint32_t a = sp_lin(c) + (uint32_t)w * 3;
		c->eip = rd_sz(c, a, w);
		x86_load_seg(c, S_CS, (uint16_t)rd_sz(c, a + (uint32_t)w, w));
		pm_crash(b, exc_name(v));
		return 0;
	}
	case HLE_EXC_RET: {
		int w = sz(d);
		uint32_t a = sp_lin(c) + (uint32_t)w;
		uint32_t eip = rd_sz(c, a, w), cs = rd_sz(c, a + (uint32_t)w, w) & 0xFFFF;
		uint32_t fl = rd_sz(c, a + (uint32_t)w * 2, w), esp = rd_sz(c, a + (uint32_t)w * 3, w);
		uint16_t ss = (uint16_t)rd_sz(c, a + (uint32_t)w * 4, w);
		pm_seg(c, S_SS, ss);
		c->r[R_ESP] = esp;
		x86_set_flags(c, fl | FL_IOPL, 0x3F7FD5 & ~FL_VM);
		pm_jump(c, (uint16_t)cs, eip);
		return 0;
	}
	case HLE_DPMI_RET: {		/* a trip to the other mode is over */
		if (!d->depth)
			return 0;
		struct frame *f = &d->fr[--d->depth];
		switch (f->kind) {
		case FR_INT: {
			/* results: the registers, and the arithmetic flags into the stacked EFLAGS */
			uint32_t r[8];
			memcpy(r, c->r, sizeof r);
			uint32_t fl = x86_flags(c);
			restore(c, &f->saved);
			for (int i = 0; i < 8; i++)
				if (i != R_ESP)
					c->r[i] = r[i];
			uint32_t a = sp_lin(c) + (uint32_t)sz(d) * 2;
			uint16_t sf = x86_rd16(c, a);
			x86_wr16(c, a, (uint16_t)((sf & ~0x08D5) | (fl & 0x08D5)));
			return 0;
		}
		case FR_IRQ:
		case FR_RM2PM:
			restore(c, &f->saved);
			return 0;
		case FR_CALL: {
			regs_to_struct(b, f->regs_lin);
			mww(b, f->regs_lin + RC_FLAGS, (uint16_t)x86_flags(c));
			restore(c, &f->saved);
			return 0;
		}
		case FR_CALLBACK: {
			/* the client left the real-mode registers to return with at ES:(E)DI */
			uint32_t a = esdi(c, d);
			restore(c, &f->saved);
			struct_to_regs(b, a);
			x86_set_flags(c, mw(b, a + RC_FLAGS), 0x0FD5);
			rm_seg(c, S_SS, mw(b, a + RC_SS));
			c->r[R_ESP] = mw(b, a + RC_SP);
			rm_seg(c, S_CS, mw(b, a + RC_CS));
			c->eip = mw(b, a + RC_IP);
			return 0;
		}
		}
		return 0;
	}
	case HLE_RM_CB: {		/* real mode called a callback */
		int i = x86_rd8(c, c->s[S_CS].base + c->eip);
		if (i >= MAX_CB || !d->cb[i].used)
			return 0;
		struct frame *f = push_frame(b, d, FR_CALLBACK);
		if (!f)
			return 0;
		/* the registers into the client's structure */
		uint32_t a = desc_base(b, d, idx_of(d->cb[i].regs.sel)) + d->cb[i].regs.off;
		f->regs_lin = a;
		regs_to_struct(b, a);
		mww(b, a + RC_IP, (uint16_t)c->eip);
		mww(b, a + RC_CS, SEG(c, S_CS));
		mww(b, a + RC_SP, (uint16_t)c->r[R_ESP]);
		mww(b, a + RC_SS, SEG(c, S_SS));
		/* DS:ESI = the real-mode stack */
		int alias = IDX_CBALIAS + (d->depth & 3);
		desc_set(b, d, alias, (uint32_t)SEG(c, S_SS) << 4, 0xFFFF, 0xF2, 0);
		uint32_t rsp = c->r[R_ESP] & 0xFFFF;
		uint32_t esp = lstack_esp(c, d);
		uint32_t fl = x86_flags(c);
		to_pm(b, d);
		pm_seg(c, S_SS, sel_of(IDX_STACK));
		c->r[R_ESP] = esp;
		pm_seg(c, S_DS, sel_of(alias));
		c->r[R_ESI] = rsp;
		pm_seg(c, S_ES, d->cb[i].regs.sel);
		c->r[R_EDI] = d->cb[i].regs.off;
		pm_seg(c, S_FS, 0);
		pm_seg(c, S_GS, 0);
		push_sz(c, d, (fl & ~(FL_IF | FL_TF | FL_VM)) | FL_IOPL);
		push_sz(c, d, code_sel(d));
		push_sz(c, d, ROM_PM_RET);
		x86_set_flags(c, FL_IOPL, FL_IOPL | FL_IF | FL_TF);
		pm_jump(c, d->cb[i].proc.sel, d->cb[i].proc.off);
		return 0;
	}
	case HLE_RM_TO_PM: {		/* a real-mode interrupt the client handles in protected mode */
		int v = x86_rd8(c, c->s[S_CS].base + c->eip);
		c->eip++;
		if (!d->pm_vec[v].sel) {
			/* no longer: pass it on to what was there */
			uint32_t t = d->rm_orig[v];
			rm_seg(c, S_CS, (uint16_t)(t >> 16));
			c->eip = t & 0xFFFF;
			return 0;
		}
		if (!push_frame(b, d, FR_RM2PM))
			return 0;
		pm_call_handler(b, d, d->pm_vec[v]);
		return 0;
	}
	case HLE_RAW_TO_PM: {		/* AX=DS CX=ES DX=SS (E)BX=(E)SP SI=CS (E)DI=(E)IP */
		uint16_t ds = AX(c), es = CX(c), ss = DX(c), cs = SI(c);
		uint32_t esp = c->r[R_EBX], eip = c->r[R_EDI];
		to_pm(b, d);
		pm_seg(c, S_DS, ds);
		pm_seg(c, S_ES, es);
		pm_seg(c, S_SS, ss);
		pm_seg(c, S_FS, 0);
		pm_seg(c, S_GS, 0);
		c->r[R_ESP] = esp;
		pm_jump(c, cs, eip);
		return 0;
	}
	case HLE_RAW_TO_RM: {
		uint16_t ds = AX(c), es = CX(c), ss = DX(c), cs = SI(c);
		uint16_t sp = BX(c), ip = DI(c);
		to_real(c);
		rm_seg(c, S_DS, ds);
		rm_seg(c, S_ES, es);
		rm_seg(c, S_SS, ss);
		rm_seg(c, S_FS, 0);
		rm_seg(c, S_GS, 0);
		c->r[R_ESP] = sp;
		rm_seg(c, S_CS, cs);
		c->eip = ip;
		return 0;
	}
	}
	return 0;
}
