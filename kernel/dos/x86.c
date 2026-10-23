/*
 * x86.c - the 386/486 interpreter. See x86.h.
 *
 * Decoding follows the real thing: prefixes, then the opcode, with operand
 * and address size coming from the code segment's D bit and the 66/67
 * prefixes. Arithmetic flags are computed lazily: the last flag-setting
 * operation is remembered, and the six flags are only worked out when
 * something reads them (a jump, PUSHF, ADC...). Faults unwind the
 * instruction with a longjmp and restart it through the interrupt table.
 */
#include "x86.h"

/* ---- lazy flags ------------------------------------------------------------ */

enum { LF_NONE, LF_ADD, LF_ADC, LF_SUB, LF_SBB, LF_LOGIC, LF_INC, LF_DEC };

static inline uint32_t szmask(int sz)
{
	return sz == 1 ? 0xFF : sz == 2 ? 0xFFFF : 0xFFFFFFFF;
}

static inline uint32_t szsign(int sz)
{
	return sz == 1 ? 0x80 : sz == 2 ? 0x8000 : 0x80000000;
}

static const uint8_t parity_tab[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
	P6(1), P6(0), P6(0), P6(1)
#undef P2
#undef P4
#undef P6
};

static int lf_cf(struct x86 *c)
{
	uint32_t m = szmask(c->lf_sz);
	uint64_t a = c->lf_a & m, b = c->lf_b & m;
	switch (c->lf_op) {
	case LF_ADD: return (a + b) > m;
	case LF_ADC: return (a + b + c->lf_cin) > m;
	case LF_SUB: return a < b;
	case LF_SBB: return a < b + c->lf_cin;
	case LF_LOGIC: return 0;
	default: return (c->flags & FL_CF) != 0;	/* INC/DEC keep it */
	}
}

static int lf_of(struct x86 *c)
{
	uint32_t s = szsign(c->lf_sz), a = c->lf_a, b = c->lf_b, r = c->lf_res;
	switch (c->lf_op) {
	case LF_ADD: case LF_ADC: case LF_INC:
		return ((a ^ r) & (b ^ r) & s) != 0;
	case LF_SUB: case LF_SBB: case LF_DEC:
		return ((a ^ b) & (a ^ r) & s) != 0;
	default:
		return 0;
	}
}

static int lf_af(struct x86 *c)
{
	if (c->lf_op == LF_LOGIC)
		return 0;
	return ((c->lf_a ^ c->lf_b ^ c->lf_res) & 0x10) != 0;
}

static inline int get_cf(struct x86 *c) { return c->lf_op ? lf_cf(c) : (c->flags & FL_CF) != 0; }
static inline int get_zf(struct x86 *c) { return c->lf_op ? (c->lf_res & szmask(c->lf_sz)) == 0 : (c->flags & FL_ZF) != 0; }
static inline int get_sf(struct x86 *c) { return c->lf_op ? (c->lf_res & szsign(c->lf_sz)) != 0 : (c->flags & FL_SF) != 0; }
static inline int get_of(struct x86 *c) { return c->lf_op ? lf_of(c) : (c->flags & FL_OF) != 0; }
static inline int get_pf(struct x86 *c) { return c->lf_op ? parity_tab[c->lf_res & 0xFF] : (c->flags & FL_PF) != 0; }
static inline int get_af(struct x86 *c) { return c->lf_op ? lf_af(c) : (c->flags & FL_AF) != 0; }

/* fold the lazy flags into c->flags */
static void flags_fix(struct x86 *c)
{
	if (!c->lf_op)
		return;
	uint32_t f = c->flags & ~FL_ARITH;
	f |= get_cf(c) ? FL_CF : 0;
	f |= get_pf(c) ? FL_PF : 0;
	f |= get_af(c) ? FL_AF : 0;
	f |= get_zf(c) ? FL_ZF : 0;
	f |= get_sf(c) ? FL_SF : 0;
	f |= get_of(c) ? FL_OF : 0;
	c->flags = f;
	c->lf_op = LF_NONE;
}

uint32_t x86_flags(struct x86 *c)
{
	flags_fix(c);
	return c->flags | 2;
}

void x86_set_flags(struct x86 *c, uint32_t f, uint32_t mask)
{
	flags_fix(c);
	c->flags = (c->flags & ~mask) | (f & mask);
}

static inline void setf(struct x86 *c, uint32_t f, int on)
{
	flags_fix(c);
	if (on)
		c->flags |= f;
	else
		c->flags &= ~f;
}

/* after an operation whose flags were set by hand: ZF SF PF from a result */
static void szp(struct x86 *c, uint32_t v, int sz)
{
	flags_fix(c);
	v &= szmask(sz);
	c->flags &= ~(FL_ZF | FL_SF | FL_PF);
	if (!v)
		c->flags |= FL_ZF;
	if (v & szsign(sz))
		c->flags |= FL_SF;
	if (parity_tab[v & 0xFF])
		c->flags |= FL_PF;
}

static inline void lazy(struct x86 *c, int op, uint32_t a, uint32_t b, uint32_t res, int sz)
{
	if (op == LF_INC || op == LF_DEC) {
		/* CF survives INC/DEC: settle it before the lazy state moves on */
		int cf = get_cf(c);
		flags_fix(c);
		c->flags = (c->flags & ~FL_CF) | (cf ? FL_CF : 0);
	}
	c->lf_op = op;
	c->lf_a = a;
	c->lf_b = b;
	c->lf_res = res;
	c->lf_sz = sz;
}

/* ---- faults ------------------------------------------------------------------ */

static _Noreturn void fault(struct x86 *c, int vec, int has_err, uint32_t err)
{
	c->fault_vec = vec;
	c->fault_has_err = has_err;
	c->fault_err = err;
	arch_longjmp(&c->jb, 1);
}

#define GP(e) fault(c, 13, 1, (e))
#define UD() fault(c, 6, 0, 0)

static inline int pmode(struct x86 *c)
{
	return (c->cr0 & CR0_PE) && !(c->flags & FL_VM);
}

/* ---- memory ---------------------------------------------------------------- */

static inline int in_vga(struct x86 *c, uint32_t a)
{
	return a >= c->vga_lo && a < c->vga_hi;
}

uint8_t x86_rd8(struct x86 *c, uint32_t a)
{
	a &= c->a20;
	if (in_vga(c, a))
		return c->env->vga_rd(c, a);
	return a < c->mem_size ? c->mem[a] : 0xFF;
}

void x86_wr8(struct x86 *c, uint32_t a, uint8_t v)
{
	a &= c->a20;
	if (in_vga(c, a)) {
		c->env->vga_wr(c, a, v);
		return;
	}
	if (a >= 0xF0000 && a < 0x100000)
		return;		/* ROM */
	if (a >= 0xB0000 && a < 0xC0000)
		c->vram_touched = 1;
	if (a < c->mem_size)
		c->mem[a] = v;
}

uint16_t x86_rd16(struct x86 *c, uint32_t a)
{
	uint32_t m = a & c->a20;
	if (m + 1 < c->mem_size && !in_vga(c, m) && ((m + 1) & c->a20) == m + 1) {
		uint16_t v;
		memcpy(&v, c->mem + m, 2);
		return v;
	}
	return (uint16_t)(x86_rd8(c, a) | x86_rd8(c, a + 1) << 8);
}

uint32_t x86_rd32(struct x86 *c, uint32_t a)
{
	uint32_t m = a & c->a20;
	if (m + 3 < c->mem_size && !in_vga(c, m) && ((m + 3) & c->a20) == m + 3) {
		uint32_t v;
		memcpy(&v, c->mem + m, 4);
		return v;
	}
	return (uint32_t)x86_rd16(c, a) | (uint32_t)x86_rd16(c, a + 2) << 16;
}

void x86_wr16(struct x86 *c, uint32_t a, uint16_t v)
{
	uint32_t m = a & c->a20;
	if (m + 1 < c->mem_size && m < 0xA0000 && ((m + 1) & c->a20) == m + 1) {
		memcpy(c->mem + m, &v, 2);
		return;
	}
	x86_wr8(c, a, (uint8_t)v);
	x86_wr8(c, a + 1, (uint8_t)(v >> 8));
}

void x86_wr32(struct x86 *c, uint32_t a, uint32_t v)
{
	uint32_t m = a & c->a20;
	if (m + 3 < c->mem_size && (m + 3 < 0xA0000 || m >= 0x100000) && ((m + 3) & c->a20) == m + 3) {
		memcpy(c->mem + m, &v, 4);
		return;
	}
	x86_wr16(c, a, (uint16_t)v);
	x86_wr16(c, a + 2, (uint16_t)(v >> 16));
}

/* a segment:offset, with the offset wrapped to the address size */
static inline uint32_t lin(struct x86 *c, int s, uint32_t off)
{
	return c->s[s].base + off;
}

static inline uint32_t rd(struct x86 *c, uint32_t a, int sz)
{
	return sz == 1 ? x86_rd8(c, a) : sz == 2 ? x86_rd16(c, a) : x86_rd32(c, a);
}

static inline void wr(struct x86 *c, uint32_t a, uint32_t v, int sz)
{
	if (sz == 1)
		x86_wr8(c, a, (uint8_t)v);
	else if (sz == 2)
		x86_wr16(c, a, (uint16_t)v);
	else
		x86_wr32(c, a, v);
}

/* ---- instruction fetch ---------------------------------------------------------- */

static inline uint8_t fetch8(struct x86 *c)
{
	uint32_t a = (c->s[S_CS].base + c->eip) & c->a20;
	c->eip++;
	if (!c->s[S_CS].big)
		c->eip &= 0xFFFF;
	return a < c->mem_size ? c->mem[a] : x86_rd8(c, a);
}

static inline uint16_t fetch16(struct x86 *c)
{
	uint16_t lo = fetch8(c);
	return (uint16_t)(lo | fetch8(c) << 8);
}

static inline uint32_t fetch32(struct x86 *c)
{
	uint32_t lo = fetch16(c);
	return lo | (uint32_t)fetch16(c) << 16;
}

static inline uint32_t fetchsz(struct x86 *c, int sz)
{
	return sz == 1 ? fetch8(c) : sz == 2 ? fetch16(c) : fetch32(c);
}

/* ---- registers ------------------------------------------------------------------- */

static inline uint32_t getr(struct x86 *c, int i, int sz)
{
	if (sz == 4)
		return c->r[i];
	if (sz == 2)
		return c->r[i] & 0xFFFF;
	return i < 4 ? c->r[i] & 0xFF : (c->r[i - 4] >> 8) & 0xFF;
}

static inline void setr(struct x86 *c, int i, uint32_t v, int sz)
{
	if (sz == 4)
		c->r[i] = v;
	else if (sz == 2)
		c->r[i] = (c->r[i] & 0xFFFF0000) | (v & 0xFFFF);
	else if (i < 4)
		c->r[i] = (c->r[i] & 0xFFFFFF00) | (v & 0xFF);
	else
		c->r[i - 4] = (c->r[i - 4] & 0xFFFF00FF) | (v & 0xFF) << 8;
}

/* ---- the stack ----------------------------------------------------------------- */

static inline uint32_t sp_get(struct x86 *c)
{
	return c->s[S_SS].big ? c->r[R_ESP] : c->r[R_ESP] & 0xFFFF;
}

static inline void sp_add(struct x86 *c, int32_t d)
{
	if (c->s[S_SS].big)
		c->r[R_ESP] += (uint32_t)d;
	else
		c->r[R_ESP] = (c->r[R_ESP] & 0xFFFF0000) | ((c->r[R_ESP] + (uint32_t)d) & 0xFFFF);
}

static void push(struct x86 *c, uint32_t v, int sz)
{
	sp_add(c, -sz);
	wr(c, lin(c, S_SS, sp_get(c)), v, sz);
}

static uint32_t pop(struct x86 *c, int sz)
{
	uint32_t v = rd(c, lin(c, S_SS, sp_get(c)), sz);
	sp_add(c, sz);
	return v;
}

void x86_push16(struct x86 *c, uint16_t v) { push(c, v, 2); }
uint16_t x86_pop16(struct x86 *c) { return (uint16_t)pop(c, 2); }
void x86_push32(struct x86 *c, uint32_t v) { push(c, v, 4); }
uint32_t x86_pop32(struct x86 *c) { return pop(c, 4); }

/* ---- segments ------------------------------------------------------------------- */

void x86_load_seg_real(struct x86 *c, int s, uint16_t sel)
{
	struct xseg *g = &c->s[s];
	g->sel = sel;
	g->base = (uint32_t)sel << 4;
	if (!(c->cr0 & CR0_PE) || (c->flags & FL_VM)) {
		/* real mode keeps the limit and size (that's how "unreal mode" works) */
		if (c->flags & FL_VM) {
			g->limit = 0xFFFF;
			g->big = 0;
		}
		if (s == S_CS && !(c->cr0 & CR0_PE))
			g->big = 0;
	}
}

static int read_desc(struct x86 *c, uint16_t sel, uint32_t *lo, uint32_t *hi)
{
	uint32_t base, limit;
	if (sel & 4) {
		base = c->ldt.base;
		limit = c->ldt.limit;
	} else {
		base = c->gdt.base;
		limit = c->gdt.limit;
	}
	uint32_t off = sel & ~7u;
	if (off + 7 > limit)
		return -1;
	*lo = x86_rd32(c, base + off);
	*hi = x86_rd32(c, base + off + 4);
	return 0;
}

static void desc_to_seg(struct xseg *g, uint16_t sel, uint32_t lo, uint32_t hi)
{
	g->sel = sel;
	g->base = (lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000);
	uint32_t limit = (lo & 0xFFFF) | (hi & 0xF0000);
	if (hi & 0x800000)
		limit = (limit << 12) | 0xFFF;
	g->limit = limit;
	g->access = (uint8_t)(hi >> 8);
	g->big = (hi & 0x400000) != 0;
}

static void set_accessed(struct x86 *c, uint16_t sel)
{
	uint32_t base = (sel & 4) ? c->ldt.base : c->gdt.base;
	uint32_t a = base + (sel & ~7u) + 5;
	x86_wr8(c, a, x86_rd8(c, a) | 1);
}

/* load a segment register in protected mode, with the checks that matter */
int x86_load_seg(struct x86 *c, int s, uint16_t sel)
{
	if (!pmode(c)) {
		x86_load_seg_real(c, s, sel);
		return 0;
	}
	struct xseg *g = &c->s[s];
	if ((sel & ~3u) == 0) {
		if (s == S_SS)
			GP(0);
		memset(g, 0, sizeof *g);
		g->sel = sel;
		return 0;
	}
	uint32_t lo, hi;
	if (read_desc(c, sel, &lo, &hi))
		GP(sel & ~3u);
	uint8_t acc = (uint8_t)(hi >> 8);
	if (!(acc & 0x10))		/* a system descriptor */
		GP(sel & ~3u);
	if (s == S_SS) {
		if ((acc & 0x08) || !(acc & 0x02))	/* code, or read-only data */
			GP(sel & ~3u);
		if (!(acc & 0x80))
			fault(c, 12, 1, sel & ~3u);
	} else if (s != S_CS) {
		if ((acc & 0x0A) == 0x08)		/* execute-only code */
			GP(sel & ~3u);
		if (!(acc & 0x80))
			fault(c, 11, 1, sel & ~3u);
	} else if (!(acc & 0x80)) {
		fault(c, 11, 1, sel & ~3u);
	}
	desc_to_seg(g, sel, lo, hi);
	if (!(acc & 1))
		set_accessed(c, sel);
	return 0;
}

static void load_cs(struct x86 *c, uint16_t sel, uint32_t eip)
{
	if (pmode(c)) {
		x86_load_seg(c, S_CS, sel);
		c->cpl = sel & 3;
		c->s[S_CS].sel = (uint16_t)((sel & ~3u) | (uint32_t)c->cpl);
	} else {
		x86_load_seg_real(c, S_CS, sel);
		if (c->flags & FL_VM)
			c->cpl = 3;
	}
	c->eip = c->s[S_CS].big ? eip : eip & 0xFFFF;
}

/* ---- ModRM ---------------------------------------------------------------------- */

static void modrm(struct x86 *c)
{
	uint8_t b = fetch8(c);
	c->mod = b >> 6;
	c->reg = (b >> 3) & 7;
	c->rm = b & 7;
	if (c->mod == 3)
		return;
	uint32_t off = 0;
	int seg = S_DS;
	if (!c->asz) {
		switch (c->rm) {
		case 0: off = c->r[R_EBX] + c->r[R_ESI]; break;
		case 1: off = c->r[R_EBX] + c->r[R_EDI]; break;
		case 2: off = c->r[R_EBP] + c->r[R_ESI]; seg = S_SS; break;
		case 3: off = c->r[R_EBP] + c->r[R_EDI]; seg = S_SS; break;
		case 4: off = c->r[R_ESI]; break;
		case 5: off = c->r[R_EDI]; break;
		case 6:
			if (c->mod == 0)
				off = fetch16(c);
			else {
				off = c->r[R_EBP];
				seg = S_SS;
			}
			break;
		case 7: off = c->r[R_EBX]; break;
		}
		if (c->mod == 1)
			off += (uint32_t)(int32_t)(int8_t)fetch8(c);
		else if (c->mod == 2)
			off += fetch16(c);
		off &= 0xFFFF;
	} else {
		if (c->rm == 4) {
			uint8_t sib = fetch8(c);
			int scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
			if (base == 5 && c->mod == 0) {
				off = fetch32(c);
			} else {
				off = c->r[base];
				if (base == R_ESP || base == R_EBP)
					seg = S_SS;
			}
			if (index != 4)
				off += c->r[index] << scale;
		} else if (c->rm == 5 && c->mod == 0) {
			off = fetch32(c);
		} else {
			off = c->r[c->rm];
			if (c->rm == R_EBP)
				seg = S_SS;
		}
		if (c->mod == 1)
			off += (uint32_t)(int32_t)(int8_t)fetch8(c);
		else if (c->mod == 2)
			off += fetch32(c);
	}
	if (c->seg >= 0)
		seg = c->seg;
	c->ea_seg = seg;
	c->ea = off;
}

static inline uint32_t ea_lin(struct x86 *c)
{
	return c->s[c->ea_seg].base + c->ea;
}

static inline uint32_t get_rm(struct x86 *c, int sz)
{
	return c->mod == 3 ? getr(c, c->rm, sz) : rd(c, ea_lin(c), sz);
}

static inline void set_rm(struct x86 *c, uint32_t v, int sz)
{
	if (c->mod == 3)
		setr(c, c->rm, v, sz);
	else
		wr(c, ea_lin(c), v, sz);
}

/* the second half of a memory operand (far pointers, LGDT...) */
static inline uint32_t ea_plus(struct x86 *c, uint32_t n)
{
	uint32_t off = c->ea + n;
	if (!c->asz)
		off &= 0xFFFF;
	return c->s[c->ea_seg].base + off;
}

/* ---- arithmetic -------------------------------------------------------------- */

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP */
static uint32_t alu(struct x86 *c, int op, uint32_t a, uint32_t b, int sz)
{
	uint32_t m = szmask(sz), r;
	a &= m;
	b &= m;
	switch (op) {
	case 0: r = a + b; lazy(c, LF_ADD, a, b, r, sz); break;
	case 2: {
		uint32_t cin = (uint32_t)get_cf(c);
		r = a + b + cin;
		lazy(c, LF_ADC, a, b, r, sz);
		c->lf_cin = cin;
		break;
	}
	case 3: {
		uint32_t cin = (uint32_t)get_cf(c);
		r = a - b - cin;
		lazy(c, LF_SBB, a, b, r, sz);
		c->lf_cin = cin;
		break;
	}
	case 5: case 7: r = a - b; lazy(c, LF_SUB, a, b, r, sz); break;
	case 1: r = a | b; lazy(c, LF_LOGIC, a, b, r, sz); break;
	case 4: r = a & b; lazy(c, LF_LOGIC, a, b, r, sz); break;
	default: r = a ^ b; lazy(c, LF_LOGIC, a, b, r, sz); break;
	}
	return r & m;
}

static uint32_t shift(struct x86 *c, int op, uint32_t v, int count, int sz)
{
	int bits = sz * 8;
	uint32_t m = szmask(sz), sign = szsign(sz);
	count &= 0x1F;
	if (!count)
		return v & m;
	flags_fix(c);
	uint64_t x = v & m;
	int cf = (c->flags & FL_CF) != 0, of = 0;
	switch (op) {
	case 0: {	/* ROL */
		int n = count % bits;
		x = ((x << n) | (x >> (bits - n))) & m;
		cf = (int)(x & 1);
		of = ((x & sign) != 0) != cf;
		c->flags = (c->flags & ~(FL_CF | FL_OF)) | (cf ? FL_CF : 0) | (of ? FL_OF : 0);
		return (uint32_t)x;
	}
	case 1: {	/* ROR */
		int n = count % bits;
		x = ((x >> n) | (x << (bits - n))) & m;
		cf = (x & sign) != 0;
		of = ((x ^ (x << 1)) & sign) != 0;
		c->flags = (c->flags & ~(FL_CF | FL_OF)) | (cf ? FL_CF : 0) | (of ? FL_OF : 0);
		return (uint32_t)x;
	}
	case 2: {	/* RCL */
		int n = count % (bits + 1);
		for (int i = 0; i < n; i++) {
			int out = (x & sign) != 0;
			x = ((x << 1) | (uint64_t)cf) & m;
			cf = out;
		}
		of = ((x & sign) != 0) != cf;
		c->flags = (c->flags & ~(FL_CF | FL_OF)) | (cf ? FL_CF : 0) | (of ? FL_OF : 0);
		return (uint32_t)x;
	}
	case 3: {	/* RCR */
		int n = count % (bits + 1);
		of = ((x & sign) != 0) != cf;
		for (int i = 0; i < n; i++) {
			int out = (int)(x & 1);
			x = (x >> 1) | (cf ? sign : 0);
			cf = out;
		}
		c->flags = (c->flags & ~(FL_CF | FL_OF)) | (cf ? FL_CF : 0) | (of ? FL_OF : 0);
		return (uint32_t)x;
	}
	case 4: case 6:	/* SHL */
		cf = count <= bits ? (int)((x >> (bits - count)) & 1) : 0;
		x = (x << count) & m;
		of = ((x & sign) != 0) != cf;
		break;
	case 5:		/* SHR */
		cf = (int)((x >> (count - 1)) & 1);
		of = (v & sign) != 0;
		x >>= count;
		break;
	case 7: {	/* SAR */
		int64_t sx = (int64_t)x;
		if (x & sign)
			sx -= (int64_t)m + 1;
		cf = (int)((sx >> (count - 1 < 63 ? count - 1 : 63)) & 1);
		sx >>= count;
		x = (uint64_t)sx & m;
		of = 0;
		break;
	}
	}
	c->flags = (c->flags & ~(FL_CF | FL_OF | FL_AF)) | (cf ? FL_CF : 0) | (of ? FL_OF : 0);
	szp(c, (uint32_t)x, sz);
	return (uint32_t)x;
}

/* SHLD/SHRD */
static uint32_t dshift(struct x86 *c, int left, uint32_t dst, uint32_t src, int count, int sz)
{
	count &= 0x1F;
	if (!count)
		return dst;
	int bits = sz * 8;
	uint64_t d = dst & szmask(sz), s = src & szmask(sz), r;
	int cf;
	if (sz == 2 && count > 16) {	/* undefined; do something sane */
		count &= 15;
		if (!count)
			return dst;
	}
	if (left) {
		uint64_t both = (d << bits) | s;
		cf = (int)((d >> (bits - count)) & 1);
		r = (both << count) >> bits;
	} else {
		uint64_t both = (s << bits) | d;
		cf = (int)((d >> (count - 1)) & 1);
		r = both >> count;
	}
	r &= szmask(sz);
	flags_fix(c);
	int of = ((r ^ d) & szsign(sz)) != 0;
	c->flags = (c->flags & ~(FL_CF | FL_OF)) | (cf ? FL_CF : 0) | (of ? FL_OF : 0);
	szp(c, (uint32_t)r, sz);
	return (uint32_t)r;
}

static int cond(struct x86 *c, int cc)
{
	int r;
	switch (cc >> 1) {
	case 0: r = get_of(c); break;
	case 1: r = get_cf(c); break;
	case 2: r = get_zf(c); break;
	case 3: r = get_cf(c) || get_zf(c); break;
	case 4: r = get_sf(c); break;
	case 5: r = get_pf(c); break;
	case 6: r = get_sf(c) != get_of(c); break;
	default: r = get_zf(c) || (get_sf(c) != get_of(c)); break;
	}
	return (cc & 1) ? !r : r;
}

/* ---- interrupts and exceptions ---------------------------------------------------- */

static void real_int(struct x86 *c, int vec, uint32_t ret_eip)
{
	uint32_t f = x86_flags(c);
	push(c, f, 2);
	push(c, c->s[S_CS].sel, 2);
	push(c, ret_eip, 2);
	c->flags &= ~(FL_IF | FL_TF | FL_AC);
	uint32_t a = c->idt.base + (uint32_t)vec * 4;
	uint16_t off = x86_rd16(c, a), seg = x86_rd16(c, a + 2);
	x86_load_seg_real(c, S_CS, seg);
	c->eip = off;
}

/* protected mode: through an interrupt or trap gate */
static void pm_int(struct x86 *c, int vec, uint32_t ret_eip, int has_err, uint32_t err, int soft)
{
	if ((uint32_t)vec * 8 + 7 > c->idt.limit)
		GP((uint32_t)vec * 8 + 2);
	uint32_t a = c->idt.base + (uint32_t)vec * 8;
	uint32_t lo = x86_rd32(c, a), hi = x86_rd32(c, a + 4);
	int type = (hi >> 8) & 0x1F, dpl = (hi >> 13) & 3;
	if (!(hi & 0x8000))
		fault(c, 11, 1, (uint32_t)vec * 8 + 2);
	if (soft && dpl < c->cpl)
		GP((uint32_t)vec * 8 + 2);
	c->int_kind = soft ? 1 : c->hw_int ? 2 : 0;
	int gate32 = (type & 8) != 0;
	int trap = (type & 7) == 7;
	if ((type & 7) != 6 && (type & 7) != 7)
		GP((uint32_t)vec * 8 + 2);	/* task gates aren't supported */
	uint16_t sel = (uint16_t)(lo >> 16);
	uint32_t off = (lo & 0xFFFF) | (hi & 0xFFFF0000);
	if (!gate32)
		off &= 0xFFFF;
	uint32_t dlo, dhi;
	if ((sel & ~3u) == 0 || read_desc(c, sel, &dlo, &dhi))
		GP(sel & ~3u);
	int new_cpl = (dhi >> 13) & 3;
	if ((dhi & 0x400) && new_cpl > c->cpl)	/* conforming: stays at our level */
		new_cpl = c->cpl;
	uint32_t f = x86_flags(c);
	int sz = gate32 ? 4 : 2;
	if (new_cpl < c->cpl) {
		/* to an inner ring: switch to its stack from the TSS */
		uint16_t old_ss = c->s[S_SS].sel;
		uint32_t old_esp = c->r[R_ESP];
		uint32_t tss = c->tr.base;
		uint32_t nesp;
		uint16_t nss;
		if ((c->tr.access & 0x0F) == 0x09 || (c->tr.access & 0x0F) == 0x0B) {
			nesp = x86_rd32(c, tss + 4 + (uint32_t)new_cpl * 8);
			nss = x86_rd16(c, tss + 8 + (uint32_t)new_cpl * 8);
		} else {
			nesp = x86_rd16(c, tss + 2 + (uint32_t)new_cpl * 4);
			nss = x86_rd16(c, tss + 4 + (uint32_t)new_cpl * 4);
		}
		int save_cpl = c->cpl;
		c->cpl = new_cpl;
		x86_load_seg(c, S_SS, nss);
		c->s[S_SS].sel = (uint16_t)((nss & ~3u) | (uint32_t)new_cpl);
		c->r[R_ESP] = nesp;
		(void)save_cpl;
		push(c, old_ss, sz);
		push(c, old_esp, sz);
	}
	push(c, f, sz);
	push(c, c->s[S_CS].sel, sz);
	push(c, ret_eip, sz);
	if (has_err)
		push(c, err, sz);
	c->flags &= ~(FL_TF | FL_NT | FL_RF | FL_VM);
	if (!trap)
		c->flags &= ~FL_IF;
	struct xseg cs;
	desc_to_seg(&cs, sel, dlo, dhi);
	c->s[S_CS] = cs;
	c->cpl = new_cpl;
	c->s[S_CS].sel = (uint16_t)((sel & ~3u) | (uint32_t)new_cpl);
	c->eip = off;
}

static void do_int(struct x86 *c, int vec, uint32_t ret_eip, int has_err, uint32_t err, int soft)
{
	if (pmode(c))
		pm_int(c, vec, ret_eip, has_err, err, soft);
	else
		real_int(c, vec, ret_eip);
}

void x86_interrupt(struct x86 *c, int vec)
{
	c->state = XS_RUN;
	if (arch_setjmp(&c->jb)) {
		/* the delivery itself faulted: give up on it */
		c->hw_int = 0;
		c->state = XS_TRIPLE;
		return;
	}
	c->hw_int = 1;
	do_int(c, vec, c->eip, 0, 0, 0);
	c->hw_int = 0;
}

int x86_irq_ok(struct x86 *c)
{
	return (c->flags & FL_IF) && !c->intr_shadow;
}

/* the exception that interrupted an instruction: restart it through the IDT */
static void deliver_fault(struct x86 *c)
{
	int vec = c->fault_vec;
	if (c->faulting) {
		c->state = XS_TRIPLE;
		return;
	}
	c->faulting = 1;
	if (arch_setjmp(&c->jb)) {
		c->faulting = 0;
		c->state = XS_TRIPLE;	/* a fault while delivering a fault */
		return;
	}
	c->eip = c->ip0;
	do_int(c, vec, c->ip0, pmode(c) && c->fault_has_err, c->fault_err, 0);
	c->faulting = 0;
}

/* IRET */
static void do_iret(struct x86 *c, int sz)
{
	if (!pmode(c)) {
		uint32_t ip = pop(c, sz), cs = pop(c, sz), f = pop(c, sz);
		x86_load_seg_real(c, S_CS, (uint16_t)cs);
		c->eip = sz == 4 ? ip : ip & 0xFFFF;
		uint32_t mask = sz == 4 ? 0x257FD5 : 0x7FD5;
		if (c->flags & FL_VM)
			mask &= ~FL_IOPL;
		x86_set_flags(c, f, mask);
		return;
	}
	uint32_t ip = pop(c, sz), cs = pop(c, sz) & 0xFFFF, f = pop(c, sz);
	int rpl = cs & 3;
	uint32_t mask = sz == 4 ? 0x257FD5 : 0x7FD5;
	if (c->cpl > 0)
		mask &= ~FL_IOPL;
	if (c->cpl > (int)((c->flags & FL_IOPL) >> 12))
		mask &= ~FL_IF;
	if (rpl > c->cpl) {
		/* back to an outer ring: its stack comes off ours */
		uint32_t esp = pop(c, sz), ss = pop(c, sz) & 0xFFFF;
		int old = c->cpl;
		c->cpl = rpl;
		load_cs(c, (uint16_t)cs, ip);
		x86_load_seg(c, S_SS, (uint16_t)ss);
		c->r[R_ESP] = sz == 4 ? esp : (c->r[R_ESP] & 0xFFFF0000) | (esp & 0xFFFF);
		/* data segments the outer ring can't see become null */
		for (int s = S_ES; s <= S_GS; s++) {
			if (s == S_CS || s == S_SS)
				continue;
			int dpl = (c->s[s].access >> 5) & 3;
			int conforming_code = (c->s[s].access & 0x0C) == 0x0C;
			if (c->s[s].sel & ~3u && dpl < rpl && !conforming_code)
				memset(&c->s[s], 0, sizeof c->s[s]);
		}
		(void)old;
	} else {
		load_cs(c, (uint16_t)cs, ip);
	}
	x86_set_flags(c, f, mask);
}

/* far RET (with n bytes of parameters) */
static void do_retf(struct x86 *c, int sz, uint16_t n)
{
	uint32_t ip = pop(c, sz), cs = pop(c, sz) & 0xFFFF;
	if (pmode(c) && (int)(cs & 3) > c->cpl) {
		sp_add(c, n);
		uint32_t esp = pop(c, sz), ss = pop(c, sz) & 0xFFFF;
		c->cpl = cs & 3;
		load_cs(c, (uint16_t)cs, ip);
		x86_load_seg(c, S_SS, (uint16_t)ss);
		c->r[R_ESP] = sz == 4 ? esp : (c->r[R_ESP] & 0xFFFF0000) | (esp & 0xFFFF);
		sp_add(c, n);
		return;
	}
	load_cs(c, (uint16_t)cs, ip);
	sp_add(c, n);
}

/* far JMP/CALL: direct to a code segment (call gates aren't supported) */
static void far_jump(struct x86 *c, uint16_t sel, uint32_t off, int call, int sz)
{
	if (call) {
		push(c, c->s[S_CS].sel, sz);
		push(c, c->eip, sz);
	}
	if (pmode(c)) {
		uint32_t lo, hi;
		if ((sel & ~3u) == 0 || read_desc(c, sel, &lo, &hi))
			GP(sel & ~3u);
		if (!(hi & 0x1000))
			GP(sel & ~3u);	/* gates: not supported */
		int dpl = (hi >> 13) & 3;
		struct xseg cs;
		desc_to_seg(&cs, sel, lo, hi);
		c->s[S_CS] = cs;
		if (!(hi & 0x400))
			c->cpl = c->cpl;	/* non-conforming: same level */
		(void)dpl;
		c->s[S_CS].sel = (uint16_t)((sel & ~3u) | (uint32_t)c->cpl);
		c->eip = cs.big ? off : off & 0xFFFF;
		return;
	}
	x86_load_seg_real(c, S_CS, sel);
	c->eip = sz == 4 ? off : off & 0xFFFF;
}

/* ---- string instructions ------------------------------------------------------- */

static inline uint32_t si_get(struct x86 *c) { return c->asz ? c->r[R_ESI] : c->r[R_ESI] & 0xFFFF; }
static inline uint32_t di_get(struct x86 *c) { return c->asz ? c->r[R_EDI] : c->r[R_EDI] & 0xFFFF; }
static inline uint32_t cx_get(struct x86 *c) { return c->asz ? c->r[R_ECX] : c->r[R_ECX] & 0xFFFF; }

static inline void idx_add(struct x86 *c, int r, int32_t d)
{
	if (c->asz)
		c->r[r] += (uint32_t)d;
	else
		c->r[r] = (c->r[r] & 0xFFFF0000) | ((c->r[r] + (uint32_t)d) & 0xFFFF);
}

static inline void cx_set(struct x86 *c, uint32_t v)
{
	if (c->asz)
		c->r[R_ECX] = v;
	else
		c->r[R_ECX] = (c->r[R_ECX] & 0xFFFF0000) | (v & 0xFFFF);
}

/* long REP runs give other things a turn: the instruction restarts */
#define REP_CHUNK 4096

static void string_op(struct x86 *c, uint8_t op, int sz)
{
	int32_t d = (c->flags & FL_DF) ? -sz : sz;
	int seg = c->seg >= 0 ? c->seg : S_DS;
	int base = op & ~1;
	if (op == 0x6C || op == 0x6D || op == 0x6E || op == 0x6F)
		base = op;
	if (c->rep && !cx_get(c))
		return;
	int n = 0;
	for (;;) {
		switch (base) {
		case 0xA4:	/* MOVS */
			wr(c, lin(c, S_ES, di_get(c)), rd(c, lin(c, seg, si_get(c)), sz), sz);
			idx_add(c, R_ESI, d);
			idx_add(c, R_EDI, d);
			break;
		case 0xA6: {	/* CMPS */
			uint32_t a = rd(c, lin(c, seg, si_get(c)), sz);
			uint32_t b = rd(c, lin(c, S_ES, di_get(c)), sz);
			alu(c, 7, a, b, sz);
			idx_add(c, R_ESI, d);
			idx_add(c, R_EDI, d);
			break;
		}
		case 0xAA:	/* STOS */
			wr(c, lin(c, S_ES, di_get(c)), getr(c, R_EAX, sz), sz);
			idx_add(c, R_EDI, d);
			break;
		case 0xAC:	/* LODS */
			setr(c, R_EAX, rd(c, lin(c, seg, si_get(c)), sz), sz);
			idx_add(c, R_ESI, d);
			break;
		case 0xAE: {	/* SCAS */
			uint32_t b = rd(c, lin(c, S_ES, di_get(c)), sz);
			alu(c, 7, getr(c, R_EAX, sz), b, sz);
			idx_add(c, R_EDI, d);
			break;
		}
		case 0x6C: case 0x6D:	/* INS */
			wr(c, lin(c, S_ES, di_get(c)), c->env->in(c, (uint16_t)c->r[R_EDX], sz), sz);
			idx_add(c, R_EDI, d);
			break;
		case 0x6E: case 0x6F:	/* OUTS */
			c->env->out(c, (uint16_t)c->r[R_EDX], rd(c, lin(c, seg, si_get(c)), sz), sz);
			idx_add(c, R_ESI, d);
			break;
		}
		if (!c->rep)
			return;
		uint32_t cx = cx_get(c) - 1;
		cx_set(c, cx);
		if (!cx)
			return;
		if (base == 0xA6 || base == 0xAE) {
			int zf = get_zf(c);
			if ((c->rep == 1 && !zf) || (c->rep == 2 && zf))
				return;
		}
		if (++n >= REP_CHUNK) {
			c->eip = c->ip0;	/* carry on next time round */
			c->icount += (uint64_t)n / 4;
			return;
		}
	}
}

/* ---- the 0F page ------------------------------------------------------------------ */

static void bit_op(struct x86 *c, int which, uint32_t bitoff_reg, int from_imm, int sz)
{
	/* BT/BTS/BTR/BTC: with a register bit offset, memory operands can reach
	 * beyond the addressed word */
	uint32_t bits = (uint32_t)sz * 8;
	if (c->mod == 3 || from_imm) {
		uint32_t v = get_rm(c, sz);
		uint32_t b = bitoff_reg & (bits - 1);
		int bit = (v >> b) & 1;
		setf(c, FL_CF, bit);
		if (which == 1) v |= 1u << b;
		else if (which == 2) v &= ~(1u << b);
		else if (which == 3) v ^= 1u << b;
		if (which)
			set_rm(c, v, sz);
		return;
	}
	int32_t off = sz == 2 ? (int16_t)bitoff_reg : (int32_t)bitoff_reg;
	int32_t byte = off >> 3;
	uint32_t a = ea_lin(c) + (uint32_t)byte;
	uint8_t v = x86_rd8(c, a), b = (uint8_t)(off & 7);
	setf(c, FL_CF, (v >> b) & 1);
	if (which == 1) v |= (uint8_t)(1 << b);
	else if (which == 2) v &= (uint8_t)~(1 << b);
	else if (which == 3) v ^= (uint8_t)(1 << b);
	if (which)
		x86_wr8(c, a, v);
}

static void cpuid(struct x86 *c)
{
	uint32_t leaf = c->r[R_EAX];
	if (leaf == 0) {
		c->r[R_EAX] = 1;
		c->r[R_EBX] = 0x756E6547;	/* "GenuineIntel" */
		c->r[R_EDX] = 0x49656E69;
		c->r[R_ECX] = 0x6C65746E;
	} else {
		c->r[R_EAX] = 0x0000045B;	/* a 486DX2: family 4, model 5 */
		c->r[R_EBX] = 0;
		c->r[R_ECX] = 0;
		c->r[R_EDX] = 0x00000001;	/* FPU on chip */
	}
}

static void op_0f(struct x86 *c)
{
	int sz = c->osz ? 4 : 2;
	uint8_t op = fetch8(c);
	if (op >= 0x80 && op <= 0x8F) {		/* Jcc near */
		int32_t d = c->osz ? (int32_t)fetch32(c) : (int16_t)fetch16(c);
		if (cond(c, op & 0xF)) {
			c->eip += (uint32_t)d;
			if (!c->osz)
				c->eip &= 0xFFFF;
		}
		return;
	}
	if (op >= 0x90 && op <= 0x9F) {		/* SETcc */
		modrm(c);
		set_rm(c, (uint32_t)cond(c, op & 0xF), 1);
		return;
	}
	if (op >= 0xC8 && op <= 0xCF) {		/* BSWAP */
		uint32_t v = c->r[op & 7];
		c->r[op & 7] = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
		return;
	}
	switch (op) {
	case 0x00:			/* SLDT STR LLDT LTR VERR VERW */
		modrm(c);
		if (!pmode(c))
			UD();
		switch (c->reg) {
		case 0: set_rm(c, c->ldt.sel, 2); break;
		case 1: set_rm(c, c->tr.sel, 2); break;
		case 2: {
			if (c->cpl)
				GP(0);
			uint16_t sel = (uint16_t)get_rm(c, 2);
			if (!(sel & ~3u)) {
				memset(&c->ldt, 0, sizeof c->ldt);
				break;
			}
			uint32_t lo, hi;
			if ((sel & 4) || read_desc(c, sel, &lo, &hi))
				GP(sel & ~3u);
			desc_to_seg(&c->ldt, sel, lo, hi);
			break;
		}
		case 3: {
			if (c->cpl)
				GP(0);
			uint16_t sel = (uint16_t)get_rm(c, 2);
			uint32_t lo, hi;
			if ((sel & 4) || read_desc(c, sel, &lo, &hi))
				GP(sel & ~3u);
			desc_to_seg(&c->tr, sel, lo, hi);
			x86_wr8(c, c->gdt.base + (sel & ~7u) + 5, (uint8_t)((hi >> 8) | 2));	/* busy */
			break;
		}
		case 4: case 5: {
			uint16_t sel = (uint16_t)get_rm(c, 2);
			uint32_t lo, hi;
			int okay = (sel & ~3u) && !read_desc(c, sel, &lo, &hi) && (hi & 0x1000);
			if (okay && c->reg == 4)
				okay = !(hi & 0x800) || (hi & 0x200);	/* data, or readable code */
			if (okay && c->reg == 5)
				okay = !(hi & 0x800) && (hi & 0x200);	/* writable data */
			setf(c, FL_ZF, okay);
			break;
		}
		default: UD();
		}
		return;
	case 0x01:			/* SGDT SIDT LGDT LIDT SMSW LMSW INVLPG */
		modrm(c);
		switch (c->reg) {
		case 0: case 1: {
			if (c->mod == 3)
				UD();
			uint32_t base = c->reg ? c->idt.base : c->gdt.base;
			uint32_t limit = c->reg ? c->idt.limit : c->gdt.limit;
			x86_wr16(c, ea_lin(c), (uint16_t)limit);
			x86_wr32(c, ea_plus(c, 2), c->osz ? base : (base & 0xFFFFFF) | 0xFF000000);
			break;
		}
		case 2: case 3: {
			if (c->mod == 3)
				UD();
			if (pmode(c) && c->cpl)
				GP(0);
			uint32_t limit = x86_rd16(c, ea_lin(c));
			uint32_t base = x86_rd32(c, ea_plus(c, 2));
			if (!c->osz)
				base &= 0xFFFFFF;
			if (c->reg == 2) {
				c->gdt.base = base;
				c->gdt.limit = limit;
			} else {
				c->idt.base = base;
				c->idt.limit = limit;
			}
			break;
		}
		case 4:
			set_rm(c, c->cr0 & 0xFFFF, 2);
			break;
		case 6: {
			if (pmode(c) && c->cpl)
				GP(0);
			uint32_t v = get_rm(c, 2);
			c->cr0 = (c->cr0 & ~0xEu) | (v & 0xF) | (c->cr0 & CR0_PE);	/* can't leave PM this way */
			break;
		}
		case 7:
			break;	/* INVLPG */
		default: UD();
		}
		return;
	case 0x02: case 0x03: {		/* LAR LSL */
		modrm(c);
		if (!pmode(c))
			UD();
		uint16_t sel = (uint16_t)get_rm(c, 2);
		uint32_t lo, hi;
		if (!(sel & ~3u) || read_desc(c, sel, &lo, &hi)) {
			setf(c, FL_ZF, 0);
			return;
		}
		if (op == 0x02) {
			setr(c, c->reg, hi & 0x00F0FF00, sz);
		} else {
			uint32_t limit = (lo & 0xFFFF) | (hi & 0xF0000);
			if (hi & 0x800000)
				limit = (limit << 12) | 0xFFF;
			setr(c, c->reg, limit, sz);
		}
		setf(c, FL_ZF, 1);
		return;
	}
	case 0x06: c->cr0 &= ~8u; return;			/* CLTS */
	case 0x08: case 0x09: return;				/* INVD WBINVD */
	case 0x0B: UD();					/* UD2 */
	case 0x20: case 0x22: {				/* MOV CRn */
		modrm(c);
		if (pmode(c) && c->cpl)
			GP(0);
		uint32_t *cr = c->reg == 0 ? &c->cr0 : c->reg == 2 ? &c->cr2 : c->reg == 3 ? &c->cr3 : &c->cr4;
		if (op == 0x20) {
			c->r[c->rm] = *cr;
		} else {
			uint32_t v = c->r[c->rm];
			if (c->reg == 0) {
				int was_pm = (c->cr0 & CR0_PE) != 0;
				c->cr0 = v | 0x10;	/* ET: a 387-class FPU */
				if (was_pm && !(v & CR0_PE))
					c->cpl = 0;
			} else {
				*cr = v;
			}
		}
		return;
	}
	case 0x21: case 0x23:				/* MOV DRn */
		modrm(c);
		if (op == 0x21)
			c->r[c->rm] = c->dr[c->reg];
		else
			c->dr[c->reg] = c->r[c->rm];
		return;
	case 0x24: case 0x26:				/* MOV TRn: nothing there */
		modrm(c);
		if (op == 0x24)
			c->r[c->rm] = 0;
		return;
	case 0x31: {					/* RDTSC */
		uint64_t t = c->icount * 4;
		c->r[R_EAX] = (uint32_t)t;
		c->r[R_EDX] = (uint32_t)(t >> 32);
		return;
	}
	case 0xA0: push(c, c->s[S_FS].sel, sz); return;
	case 0xA1: x86_load_seg(c, S_FS, (uint16_t)pop(c, sz)); return;
	case 0xA8: push(c, c->s[S_GS].sel, sz); return;
	case 0xA9: x86_load_seg(c, S_GS, (uint16_t)pop(c, sz)); return;
	case 0xA2:
		if (c->cpu_level < 4)
			UD();
		cpuid(c);
		return;
	case 0xA3: modrm(c); bit_op(c, 0, getr(c, c->reg, sz), 0, sz); return;
	case 0xAB: modrm(c); bit_op(c, 1, getr(c, c->reg, sz), 0, sz); return;
	case 0xB3: modrm(c); bit_op(c, 2, getr(c, c->reg, sz), 0, sz); return;
	case 0xBB: modrm(c); bit_op(c, 3, getr(c, c->reg, sz), 0, sz); return;
	case 0xBA: {
		modrm(c);
		uint8_t imm = fetch8(c);
		if (c->reg < 4)
			UD();
		bit_op(c, c->reg - 4, imm, 1, sz);
		return;
	}
	case 0xA4: case 0xA5: case 0xAC: case 0xAD: {	/* SHLD SHRD */
		modrm(c);
		int count = (op & 1) ? (int)(c->r[R_ECX] & 0xFF) : fetch8(c);
		uint32_t r = dshift(c, op < 0xA8, get_rm(c, sz), getr(c, c->reg, sz), count, sz);
		set_rm(c, r, sz);
		return;
	}
	case 0xAF: {					/* IMUL r, r/m */
		modrm(c);
		int64_t a = sz == 2 ? (int16_t)getr(c, c->reg, 2) : (int32_t)c->r[c->reg];
		int64_t b = sz == 2 ? (int16_t)get_rm(c, 2) : (int32_t)get_rm(c, 4);
		int64_t r = a * b;
		setr(c, c->reg, (uint32_t)r, sz);
		int over = sz == 2 ? r != (int16_t)r : r != (int32_t)r;
		setf(c, FL_CF, over);
		setf(c, FL_OF, over);
		return;
	}
	case 0xB0: case 0xB1: {				/* CMPXCHG */
		if (c->cpu_level < 4)
			UD();
		modrm(c);
		int s = op & 1 ? sz : 1;
		uint32_t dst = get_rm(c, s), acc = getr(c, R_EAX, s);
		alu(c, 7, acc, dst, s);
		if (acc == dst)
			set_rm(c, getr(c, c->reg, s), s);
		else {
			set_rm(c, dst, s);
			setr(c, R_EAX, dst, s);
		}
		return;
	}
	case 0xC0: case 0xC1: {				/* XADD */
		if (c->cpu_level < 4)
			UD();
		modrm(c);
		int s = op & 1 ? sz : 1;
		uint32_t dst = get_rm(c, s), src = getr(c, c->reg, s);
		uint32_t sum = alu(c, 0, dst, src, s);
		setr(c, c->reg, dst, s);
		set_rm(c, sum, s);
		return;
	}
	case 0xB2: case 0xB4: case 0xB5: {		/* LSS LFS LGS */
		modrm(c);
		if (c->mod == 3)
			UD();
		uint32_t off = rd(c, ea_lin(c), sz);
		uint16_t sel = x86_rd16(c, ea_plus(c, (uint32_t)sz));
		x86_load_seg(c, op == 0xB2 ? S_SS : op == 0xB4 ? S_FS : S_GS, sel);
		setr(c, c->reg, off, sz);
		if (op == 0xB2)
			c->intr_shadow = 1;
		return;
	}
	case 0xB6: case 0xB7: case 0xBE: case 0xBF: {	/* MOVZX MOVSX */
		modrm(c);
		uint32_t v;
		if (op & 1)
			v = get_rm(c, 2), v = (op & 8) ? (uint32_t)(int32_t)(int16_t)v : v;
		else
			v = get_rm(c, 1), v = (op & 8) ? (uint32_t)(int32_t)(int8_t)v : v;
		setr(c, c->reg, v, sz);
		return;
	}
	case 0xBC: case 0xBD: {				/* BSF BSR */
		modrm(c);
		uint32_t v = get_rm(c, sz);
		if (!v) {
			setf(c, FL_ZF, 1);
			return;
		}
		int i;
		if (op == 0xBC)
			for (i = 0; !(v & (1u << i)); i++)
				;
		else
			for (i = sz * 8 - 1; !(v & (1u << i)); i--)
				;
		setr(c, c->reg, (uint32_t)i, sz);
		setf(c, FL_ZF, 0);
		return;
	}
	}
	UD();
}

/* ---- one instruction ---------------------------------------------------------------- */

static int io_allowed(struct x86 *c)
{
	return !pmode(c) || c->cpl <= (int)((c->flags & FL_IOPL) >> 12);
}

static void step(struct x86 *c)
{
	c->ip0 = c->eip;
	if (c->trace) {
		unsigned i = c->hist_n++ % 64;
		c->hist[i].cs = c->s[S_CS].sel;
		c->hist[i].ip = c->eip;
	}
	c->seg = -1;
	c->rep = 0;
	c->lock = 0;
	int def32 = c->s[S_CS].big;
	c->osz = def32;
	c->asz = def32;
	uint8_t op;
	for (int n = 0;; n++) {
		op = fetch8(c);
		switch (op) {
		case 0x26: c->seg = S_ES; continue;
		case 0x2E: c->seg = S_CS; continue;
		case 0x36: c->seg = S_SS; continue;
		case 0x3E: c->seg = S_DS; continue;
		case 0x64: c->seg = S_FS; continue;
		case 0x65: c->seg = S_GS; continue;
		case 0x66: c->osz = !def32; continue;
		case 0x67: c->asz = !def32; continue;
		case 0xF0: c->lock = 1; continue;
		case 0xF2: c->rep = 2; continue;
		case 0xF3: c->rep = 1; continue;
		}
		if (n > 14)
			GP(0);
		break;
	}
	int sz = c->osz ? 4 : 2;

	if (op < 0x40 && (op & 7) < 6) {	/* the ALU block */
		int aop = op >> 3;
		switch (op & 7) {
		case 0: { modrm(c); uint32_t r = alu(c, aop, get_rm(c, 1), getr(c, c->reg, 1), 1); if (aop != 7) set_rm(c, r, 1); return; }
		case 1: { modrm(c); uint32_t r = alu(c, aop, get_rm(c, sz), getr(c, c->reg, sz), sz); if (aop != 7) set_rm(c, r, sz); return; }
		case 2: { modrm(c); uint32_t r = alu(c, aop, getr(c, c->reg, 1), get_rm(c, 1), 1); if (aop != 7) setr(c, c->reg, r, 1); return; }
		case 3: { modrm(c); uint32_t r = alu(c, aop, getr(c, c->reg, sz), get_rm(c, sz), sz); if (aop != 7) setr(c, c->reg, r, sz); return; }
		case 4: { uint32_t r = alu(c, aop, getr(c, R_EAX, 1), fetch8(c), 1); if (aop != 7) setr(c, R_EAX, r, 1); return; }
		case 5: { uint32_t r = alu(c, aop, getr(c, R_EAX, sz), fetchsz(c, sz), sz); if (aop != 7) setr(c, R_EAX, r, sz); return; }
		}
	}

	switch (op) {
	case 0x06: push(c, c->s[S_ES].sel, sz); break;
	case 0x07: x86_load_seg(c, S_ES, (uint16_t)pop(c, sz)); break;
	case 0x0E: push(c, c->s[S_CS].sel, sz); break;
	case 0x0F: op_0f(c); break;
	case 0x16: push(c, c->s[S_SS].sel, sz); break;
	case 0x17: x86_load_seg(c, S_SS, (uint16_t)pop(c, sz)); c->intr_shadow = 1; break;
	case 0x1E: push(c, c->s[S_DS].sel, sz); break;
	case 0x1F: x86_load_seg(c, S_DS, (uint16_t)pop(c, sz)); break;
	case 0x27: case 0x2F: {	/* DAA DAS */
		uint8_t al = (uint8_t)c->r[R_EAX], oal = al;
		int cf = get_cf(c), af = get_af(c);
		flags_fix(c);
		if ((al & 0xF) > 9 || af) {
			al = (uint8_t)(op == 0x27 ? al + 6 : al - 6);
			af = 1;
		} else {
			af = 0;
		}
		if (oal > 0x99 || cf) {
			al = (uint8_t)(op == 0x27 ? al + 0x60 : al - 0x60);
			cf = 1;
		} else {
			cf = 0;
		}
		setr(c, R_EAX, al, 1);
		c->flags = (c->flags & ~(FL_CF | FL_AF)) | (cf ? FL_CF : 0) | (af ? FL_AF : 0);
		szp(c, al, 1);
		break;
	}
	case 0x37: case 0x3F: {	/* AAA AAS */
		int af = get_af(c);
		flags_fix(c);
		uint8_t al = (uint8_t)c->r[R_EAX];
		if ((al & 0xF) > 9 || af) {
			uint16_t ax = (uint16_t)c->r[R_EAX];
			if (op == 0x37)
				ax = (uint16_t)(ax + 0x106);
			else
				ax = (uint16_t)(((ax & 0xFF) - 6) & 0xFF) | (uint16_t)((((ax >> 8) - 1) & 0xFF) << 8);
			setr(c, R_EAX, ax, 2);
			c->flags |= FL_AF | FL_CF;
		} else {
			c->flags &= ~(FL_AF | FL_CF);
		}
		setr(c, R_EAX, c->r[R_EAX] & 0xFF0F, 2);
		break;
	}
	case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
		uint32_t a = getr(c, op & 7, sz), r = a + 1;
		lazy(c, LF_INC, a, 1, r, sz);
		setr(c, op & 7, r, sz);
		break;
	}
	case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
		uint32_t a = getr(c, op & 7, sz), r = a - 1;
		lazy(c, LF_DEC, a, 1, r, sz);
		setr(c, op & 7, r, sz);
		break;
	}
	case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57:
		push(c, getr(c, op & 7, sz), sz);
		break;
	case 0x54: {	/* PUSH SP pushes the value before the push (286 on) */
		uint32_t v = getr(c, R_ESP, sz);
		push(c, v, sz);
		break;
	}
	case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
		uint32_t v = pop(c, sz);
		setr(c, op & 7, v, sz);
		break;
	}
	case 0x60: {	/* PUSHA */
		uint32_t sp = getr(c, R_ESP, sz);
		for (int i = 0; i < 8; i++)
			push(c, i == R_ESP ? sp : getr(c, i, sz), sz);
		break;
	}
	case 0x61:	/* POPA */
		for (int i = 7; i >= 0; i--) {
			uint32_t v = pop(c, sz);
			if (i != R_ESP)
				setr(c, i, v, sz);
		}
		break;
	case 0x62:	/* BOUND */
		modrm(c);
		if (c->mod == 3)
			UD();
		{
			int32_t v = sz == 2 ? (int16_t)getr(c, c->reg, 2) : (int32_t)c->r[c->reg];
			int32_t lo = sz == 2 ? (int16_t)x86_rd16(c, ea_lin(c)) : (int32_t)x86_rd32(c, ea_lin(c));
			int32_t hi = sz == 2 ? (int16_t)x86_rd16(c, ea_plus(c, 2)) : (int32_t)x86_rd32(c, ea_plus(c, 4));
			if (v < lo || v > hi)
				fault(c, 5, 0, 0);
		}
		break;
	case 0x63: {	/* ARPL */
		modrm(c);
		if (!pmode(c))
			UD();
		uint16_t d = (uint16_t)get_rm(c, 2), s = (uint16_t)getr(c, c->reg, 2);
		if ((d & 3) < (s & 3)) {
			set_rm(c, (d & ~3u) | (s & 3), 2);
			setf(c, FL_ZF, 1);
		} else {
			setf(c, FL_ZF, 0);
		}
		break;
	}
	case 0x68: push(c, fetchsz(c, sz), sz); break;
	case 0x6A: push(c, (uint32_t)(int32_t)(int8_t)fetch8(c), sz); break;
	case 0x69: case 0x6B: {	/* IMUL r, r/m, imm */
		modrm(c);
		int64_t a = sz == 2 ? (int16_t)get_rm(c, 2) : (int32_t)get_rm(c, 4);
		int64_t b = op == 0x6B ? (int8_t)fetch8(c) : sz == 2 ? (int16_t)fetch16(c) : (int32_t)fetch32(c);
		int64_t r = a * b;
		setr(c, c->reg, (uint32_t)r, sz);
		int over = sz == 2 ? r != (int16_t)r : r != (int32_t)r;
		setf(c, FL_CF, over);
		setf(c, FL_OF, over);
		break;
	}
	case 0x6C: case 0x6D: case 0x6E: case 0x6F:	/* INS OUTS */
		if (!io_allowed(c))
			GP(0);
		string_op(c, op, op & 1 ? sz : 1);
		break;
	case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
	case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
		int8_t d = (int8_t)fetch8(c);
		if (cond(c, op & 0xF)) {
			c->eip += (uint32_t)(int32_t)d;
			if (!c->osz)
				c->eip &= 0xFFFF;
		}
		break;
	}
	case 0x80: case 0x82: {
		modrm(c);
		uint32_t r = alu(c, c->reg, get_rm(c, 1), fetch8(c), 1);
		if (c->reg != 7)
			set_rm(c, r, 1);
		break;
	}
	case 0x81: case 0x83: {
		modrm(c);
		uint32_t imm = op == 0x81 ? fetchsz(c, sz) : (uint32_t)(int32_t)(int8_t)fetch8(c);
		uint32_t r = alu(c, c->reg, get_rm(c, sz), imm, sz);
		if (c->reg != 7)
			set_rm(c, r, sz);
		break;
	}
	case 0x84: modrm(c); alu(c, 4, get_rm(c, 1), getr(c, c->reg, 1), 1); break;
	case 0x85: modrm(c); alu(c, 4, get_rm(c, sz), getr(c, c->reg, sz), sz); break;
	case 0x86: case 0x87: {
		modrm(c);
		int s = op & 1 ? sz : 1;
		uint32_t a = get_rm(c, s), b = getr(c, c->reg, s);
		set_rm(c, b, s);
		setr(c, c->reg, a, s);
		break;
	}
	case 0x88: modrm(c); set_rm(c, getr(c, c->reg, 1), 1); break;
	case 0x89: modrm(c); set_rm(c, getr(c, c->reg, sz), sz); break;
	case 0x8A: modrm(c); setr(c, c->reg, get_rm(c, 1), 1); break;
	case 0x8B: modrm(c); setr(c, c->reg, get_rm(c, sz), sz); break;
	case 0x8C:
		modrm(c);
		if (c->reg > 5)
			UD();
		if (c->mod == 3)
			setr(c, c->rm, c->s[c->reg].sel, sz);
		else
			set_rm(c, c->s[c->reg].sel, 2);
		break;
	case 0x8D:
		modrm(c);
		if (c->mod == 3)
			UD();
		setr(c, c->reg, c->ea, sz);
		break;
	case 0x8E:
		modrm(c);
		if (c->reg == S_CS || c->reg > 5)
			UD();
		x86_load_seg(c, c->reg, (uint16_t)get_rm(c, 2));
		if (c->reg == S_SS)
			c->intr_shadow = 1;
		break;
	case 0x8F: {
		/* POP r/m: ESP is incremented before the address is worked out */
		uint32_t v = pop(c, sz);
		modrm(c);
		set_rm(c, v, sz);
		break;
	}
	case 0x90:
		if (c->rep == 1)
			c->icount += 0;	/* PAUSE */
		break;
	case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
		uint32_t t = getr(c, R_EAX, sz);
		setr(c, R_EAX, getr(c, op & 7, sz), sz);
		setr(c, op & 7, t, sz);
		break;
	}
	case 0x98:
		if (sz == 4)
			c->r[R_EAX] = (uint32_t)(int32_t)(int16_t)c->r[R_EAX];
		else
			setr(c, R_EAX, (uint32_t)(int16_t)(int8_t)c->r[R_EAX], 2);
		break;
	case 0x99:
		if (sz == 4)
			c->r[R_EDX] = (c->r[R_EAX] & 0x80000000) ? 0xFFFFFFFF : 0;
		else
			setr(c, R_EDX, (c->r[R_EAX] & 0x8000) ? 0xFFFF : 0, 2);
		break;
	case 0x9A: {
		uint32_t off = fetchsz(c, sz);
		uint16_t sel = fetch16(c);
		far_jump(c, sel, off, 1, sz);
		break;
	}
	case 0x9B:	/* FWAIT */
		break;
	case 0x9C: {	/* PUSHF */
		uint32_t f = x86_flags(c);
		if (sz == 4)
			push(c, f & 0x00FCFFFF, 4);
		else
			push(c, f & 0xFFFF, 2);
		break;
	}
	case 0x9D: {	/* POPF */
		uint32_t f = pop(c, sz);
		uint32_t mask = sz == 4 ? 0x257FD5 : 0x7FD5;
		if (c->cpu_level < 4)
			mask &= ~(FL_AC | FL_ID);
		if (pmode(c)) {
			if (c->cpl > 0)
				mask &= ~FL_IOPL;
			if (c->cpl > (int)((c->flags & FL_IOPL) >> 12))
				mask &= ~FL_IF;
		}
		x86_set_flags(c, f, mask);
		break;
	}
	case 0x9E:	/* SAHF */
		x86_set_flags(c, (c->r[R_EAX] >> 8) & 0xD5, 0xD5);
		break;
	case 0x9F:	/* LAHF */
		setr(c, 4, x86_flags(c) & 0xFF, 1);
		break;
	case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
		uint32_t off = c->asz ? fetch32(c) : fetch16(c);
		uint32_t a = lin(c, c->seg >= 0 ? c->seg : S_DS, off);
		int s = op & 1 ? sz : 1;
		if (op < 0xA2)
			setr(c, R_EAX, rd(c, a, s), s);
		else
			wr(c, a, getr(c, R_EAX, s), s);
		break;
	}
	case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
	case 0xAC: case 0xAD: case 0xAE: case 0xAF:
		string_op(c, op, op & 1 ? sz : 1);
		break;
	case 0xA8: alu(c, 4, getr(c, R_EAX, 1), fetch8(c), 1); break;
	case 0xA9: alu(c, 4, getr(c, R_EAX, sz), fetchsz(c, sz), sz); break;
	case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
		setr(c, op & 7, fetch8(c), 1);
		break;
	case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
		setr(c, op & 7, fetchsz(c, sz), sz);
		break;
	case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
		modrm(c);
		int s = op & 1 ? sz : 1;
		int count = (op <= 0xC1) ? fetch8(c) : (op >= 0xD2 ? (int)(c->r[R_ECX] & 0xFF) : 1);
		set_rm(c, shift(c, c->reg, get_rm(c, s), count, s), s);
		break;
	}
	case 0xC2: {
		uint16_t n = fetch16(c);
		uint32_t ip = pop(c, sz);
		c->eip = sz == 4 ? ip : ip & 0xFFFF;
		sp_add(c, n);
		break;
	}
	case 0xC3: {
		uint32_t ip = pop(c, sz);
		c->eip = sz == 4 ? ip : ip & 0xFFFF;
		break;
	}
	case 0xC4: case 0xC5: {	/* LES LDS */
		modrm(c);
		if (c->mod == 3)
			UD();
		uint32_t off = rd(c, ea_lin(c), sz);
		uint16_t sel = x86_rd16(c, ea_plus(c, (uint32_t)sz));
		x86_load_seg(c, op == 0xC4 ? S_ES : S_DS, sel);
		setr(c, c->reg, off, sz);
		break;
	}
	case 0xC6: modrm(c); set_rm(c, fetch8(c), 1); break;
	case 0xC7: modrm(c); set_rm(c, fetchsz(c, sz), sz); break;
	case 0xC8: {	/* ENTER */
		uint16_t size = fetch16(c);
		uint8_t level = fetch8(c) & 31;
		push(c, getr(c, R_EBP, sz), sz);
		uint32_t frame = sp_get(c);
		for (int i = 1; i < level; i++) {
			uint32_t bp = getr(c, R_EBP, c->s[S_SS].big ? 4 : 2) - (uint32_t)sz;
			setr(c, R_EBP, bp, c->s[S_SS].big ? 4 : 2);
			push(c, rd(c, lin(c, S_SS, bp), sz), sz);
		}
		if (level)
			push(c, frame, sz);
		setr(c, R_EBP, frame, sz);
		sp_add(c, -(int32_t)size);
		break;
	}
	case 0xC9:	/* LEAVE */
		if (c->s[S_SS].big)
			c->r[R_ESP] = c->r[R_EBP];
		else
			setr(c, R_ESP, c->r[R_EBP] & 0xFFFF, 2);
		setr(c, R_EBP, pop(c, sz), sz);
		break;
	case 0xCA: do_retf(c, sz, fetch16(c)); break;
	case 0xCB: do_retf(c, sz, 0); break;
	case 0xCC: do_int(c, 3, c->eip, 0, 0, 1); break;
	case 0xCD: {
		uint8_t n = fetch8(c);
		if ((c->flags & FL_VM) && ((c->flags & FL_IOPL) >> 12) < 3)
			GP(0);
		do_int(c, n, c->eip, 0, 0, 1);
		break;
	}
	case 0xCE:
		if (get_of(c))
			do_int(c, 4, c->eip, 0, 0, 1);
		break;
	case 0xCF: do_iret(c, sz); break;
	case 0xD4: {	/* AAM */
		uint8_t base = fetch8(c);
		if (!base)
			fault(c, 0, 0, 0);
		uint8_t al = (uint8_t)c->r[R_EAX];
		setr(c, R_EAX, (uint32_t)(al / base) << 8 | (al % base), 2);
		szp(c, al % base, 1);
		break;
	}
	case 0xD5: {	/* AAD */
		uint8_t base = fetch8(c);
		uint8_t al = (uint8_t)(c->r[R_EAX] + ((c->r[R_EAX] >> 8) & 0xFF) * base);
		setr(c, R_EAX, al, 2);
		szp(c, al, 1);
		break;
	}
	case 0xD6: setr(c, R_EAX, get_cf(c) ? 0xFF : 0, 1); break;	/* SALC */
	case 0xD7: {	/* XLAT */
		uint32_t off = (c->asz ? c->r[R_EBX] : c->r[R_EBX] & 0xFFFF) + (c->r[R_EAX] & 0xFF);
		if (!c->asz)
			off &= 0xFFFF;
		setr(c, R_EAX, x86_rd8(c, lin(c, c->seg >= 0 ? c->seg : S_DS, off)), 1);
		break;
	}
	case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
		if (c->cr0 & 0x4)	/* EM: no FPU */
			fault(c, 7, 0, 0);
		modrm(c);
		x87_exec(c, op);
		break;
	case 0xE0: case 0xE1: case 0xE2: {	/* LOOPNZ LOOPZ LOOP */
		int8_t d = (int8_t)fetch8(c);
		uint32_t cx = cx_get(c) - 1;
		cx_set(c, cx);
		int go = cx != 0;
		if (op == 0xE0)
			go = go && !get_zf(c);
		else if (op == 0xE1)
			go = go && get_zf(c);
		if (go) {
			c->eip += (uint32_t)(int32_t)d;
			if (!c->osz)
				c->eip &= 0xFFFF;
		}
		break;
	}
	case 0xE3: {
		int8_t d = (int8_t)fetch8(c);
		if (!cx_get(c)) {
			c->eip += (uint32_t)(int32_t)d;
			if (!c->osz)
				c->eip &= 0xFFFF;
		}
		break;
	}
	case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF: {
		uint16_t port = op < 0xE8 ? fetch8(c) : (uint16_t)c->r[R_EDX];
		if (!io_allowed(c))
			GP(0);
		int s = op & 1 ? sz : 1;
		if (op & 2)
			c->env->out(c, port, getr(c, R_EAX, s), s);
		else
			setr(c, R_EAX, c->env->in(c, port, s), s);
		break;
	}
	case 0xE8: {
		int32_t d = c->osz ? (int32_t)fetch32(c) : (int16_t)fetch16(c);
		push(c, c->eip, sz);
		c->eip += (uint32_t)d;
		if (!c->osz)
			c->eip &= 0xFFFF;
		break;
	}
	case 0xE9: {
		int32_t d = c->osz ? (int32_t)fetch32(c) : (int16_t)fetch16(c);
		c->eip += (uint32_t)d;
		if (!c->osz)
			c->eip &= 0xFFFF;
		break;
	}
	case 0xEA: {
		uint32_t off = fetchsz(c, sz);
		uint16_t sel = fetch16(c);
		far_jump(c, sel, off, 0, sz);
		break;
	}
	case 0xEB: {
		int8_t d = (int8_t)fetch8(c);
		c->eip += (uint32_t)(int32_t)d;
		if (!c->osz)
			c->eip &= 0xFFFF;
		break;
	}
	case 0xF1: {	/* in ROM: a call into the C BIOS/DOS */
		uint32_t at = (c->s[S_CS].base + c->ip0) & c->a20;
		if (at >= 0xF0000 && at < 0x100000) {
			int n = fetch8(c);
			c->hle_vec = n;
			if (c->env->hle(c, n)) {
				c->eip = c->ip0;
				c->state = XS_WAIT;
			}
			break;
		}
		do_int(c, 1, c->eip, 0, 0, 0);
		break;
	}
	case 0xF4:	/* HLT */
		if (pmode(c) && c->cpl)
			GP(0);
		c->state = XS_HALT;
		break;
	case 0xF5: setf(c, FL_CF, !get_cf(c)); break;
	case 0xF6: case 0xF7: {
		modrm(c);
		int s = op & 1 ? sz : 1;
		switch (c->reg) {
		case 0: case 1:
			alu(c, 4, get_rm(c, s), fetchsz(c, s), s);
			break;
		case 2:
			set_rm(c, ~get_rm(c, s), s);
			break;
		case 3: {
			uint32_t v = get_rm(c, s);
			set_rm(c, alu(c, 5, 0, v, s), s);
			break;
		}
		case 4: {	/* MUL */
			uint64_t a = getr(c, R_EAX, s), b = get_rm(c, s), r = a * b;
			int hi;
			if (s == 1) {
				setr(c, R_EAX, (uint32_t)r, 2);
				hi = (r >> 8) != 0;
			} else if (s == 2) {
				setr(c, R_EAX, (uint32_t)r, 2);
				setr(c, R_EDX, (uint32_t)(r >> 16), 2);
				hi = (r >> 16) != 0;
			} else {
				c->r[R_EAX] = (uint32_t)r;
				c->r[R_EDX] = (uint32_t)(r >> 32);
				hi = (r >> 32) != 0;
			}
			szp(c, (uint32_t)r, s);
			setf(c, FL_CF, hi);
			setf(c, FL_OF, hi);
			break;
		}
		case 5: {	/* IMUL */
			int64_t a, b, r;
			int over;
			if (s == 1) {
				a = (int8_t)c->r[R_EAX];
				b = (int8_t)get_rm(c, 1);
				r = a * b;
				setr(c, R_EAX, (uint32_t)r, 2);
				over = r != (int8_t)r;
			} else if (s == 2) {
				a = (int16_t)c->r[R_EAX];
				b = (int16_t)get_rm(c, 2);
				r = a * b;
				setr(c, R_EAX, (uint32_t)r, 2);
				setr(c, R_EDX, (uint32_t)(r >> 16), 2);
				over = r != (int16_t)r;
			} else {
				a = (int32_t)c->r[R_EAX];
				b = (int32_t)get_rm(c, 4);
				r = a * b;
				c->r[R_EAX] = (uint32_t)r;
				c->r[R_EDX] = (uint32_t)((uint64_t)r >> 32);
				over = r != (int32_t)r;
			}
			szp(c, (uint32_t)r, s);
			setf(c, FL_CF, over);
			setf(c, FL_OF, over);
			break;
		}
		case 6: {	/* DIV */
			uint64_t d = get_rm(c, s);
			if (!d)
				fault(c, 0, 0, 0);
			if (s == 1) {
				uint32_t n = c->r[R_EAX] & 0xFFFF;
				if (n / d > 0xFF)
					fault(c, 0, 0, 0);
				setr(c, R_EAX, (uint32_t)((n % d) << 8 | (n / d)), 2);
			} else if (s == 2) {
				uint32_t n = (c->r[R_EDX] & 0xFFFF) << 16 | (c->r[R_EAX] & 0xFFFF);
				if (n / d > 0xFFFF)
					fault(c, 0, 0, 0);
				setr(c, R_EAX, (uint32_t)(n / d), 2);
				setr(c, R_EDX, (uint32_t)(n % d), 2);
			} else {
				uint64_t n = (uint64_t)c->r[R_EDX] << 32 | c->r[R_EAX];
				if (n / d > 0xFFFFFFFFULL)
					fault(c, 0, 0, 0);
				c->r[R_EAX] = (uint32_t)(n / d);
				c->r[R_EDX] = (uint32_t)(n % d);
			}
			break;
		}
		case 7: {	/* IDIV */
			if (s == 1) {
				int32_t n = (int16_t)c->r[R_EAX], d = (int8_t)get_rm(c, 1);
				if (!d || n / d > 127 || n / d < -128)
					fault(c, 0, 0, 0);
				setr(c, R_EAX, (uint32_t)(((n % d) & 0xFF) << 8 | ((n / d) & 0xFF)), 2);
			} else if (s == 2) {
				int32_t n = (int32_t)((c->r[R_EDX] & 0xFFFF) << 16 | (c->r[R_EAX] & 0xFFFF));
				int32_t d = (int16_t)get_rm(c, 2);
				if (!d || n / d > 32767 || n / d < -32768)
					fault(c, 0, 0, 0);
				setr(c, R_EAX, (uint32_t)(n / d), 2);
				setr(c, R_EDX, (uint32_t)(n % d), 2);
			} else {
				int64_t n = (int64_t)((uint64_t)c->r[R_EDX] << 32 | c->r[R_EAX]);
				int64_t d = (int32_t)get_rm(c, 4);
				if (!d || (n == INT64_MIN && d == -1) || n / d > 2147483647LL || n / d < -2147483648LL)
					fault(c, 0, 0, 0);
				c->r[R_EAX] = (uint32_t)(n / d);
				c->r[R_EDX] = (uint32_t)(n % d);
			}
			break;
		}
		}
		break;
	}
	case 0xF8: setf(c, FL_CF, 0); break;
	case 0xF9: setf(c, FL_CF, 1); break;
	case 0xFA:
		if (pmode(c) && c->cpl > (int)((c->flags & FL_IOPL) >> 12))
			GP(0);
		c->flags &= ~FL_IF;
		break;
	case 0xFB:
		if (pmode(c) && c->cpl > (int)((c->flags & FL_IOPL) >> 12))
			GP(0);
		if (!(c->flags & FL_IF))
			c->intr_shadow = 1;
		c->flags |= FL_IF;
		break;
	case 0xFC: c->flags &= ~FL_DF; break;
	case 0xFD: c->flags |= FL_DF; break;
	case 0xFE: {
		modrm(c);
		uint32_t a = get_rm(c, 1), r;
		if (c->reg == 0) {
			r = a + 1;
			lazy(c, LF_INC, a, 1, r, 1);
		} else if (c->reg == 1) {
			r = a - 1;
			lazy(c, LF_DEC, a, 1, r, 1);
		} else {
			UD();
		}
		set_rm(c, r, 1);
		break;
	}
	case 0xFF:
		modrm(c);
		switch (c->reg) {
		case 0: case 1: {
			uint32_t a = get_rm(c, sz), r = c->reg ? a - 1 : a + 1;
			lazy(c, c->reg ? LF_DEC : LF_INC, a, 1, r, sz);
			set_rm(c, r, sz);
			break;
		}
		case 2: {
			uint32_t t = get_rm(c, sz);
			push(c, c->eip, sz);
			c->eip = sz == 4 ? t : t & 0xFFFF;
			break;
		}
		case 3: case 5: {
			if (c->mod == 3)
				UD();
			uint32_t off = rd(c, ea_lin(c), sz);
			uint16_t sel = x86_rd16(c, ea_plus(c, (uint32_t)sz));
			far_jump(c, sel, off, c->reg == 3, sz);
			break;
		}
		case 4: {
			uint32_t t = get_rm(c, sz);
			c->eip = sz == 4 ? t : t & 0xFFFF;
			break;
		}
		case 6: push(c, get_rm(c, sz), sz); break;
		default: UD();
		}
		break;
	default:
		UD();
	}
}

long x86_run(struct x86 *c, long budget)
{
	volatile long done = 0;
	c->state = XS_RUN;
	for (;;) {
		if (!arch_setjmp(&c->jb)) {
			while (c->state == XS_RUN) {
				if (done >= budget) {
					c->state = XS_BUDGET;
					break;
				}
				c->intr_shadow = 0;
				step(c);
				done++;
				c->icount++;
				if (c->irq_waiting && (c->flags & FL_IF) && !c->intr_shadow) {
					c->state = XS_BUDGET;
					break;
				}
			}
			return done;
		}
		/* an instruction faulted: deliver it (or give up) and carry on */
		deliver_fault(c);
		if (c->state == XS_TRIPLE)
			return done;
	}
}

void x86_reset(struct x86 *c)
{
	memset(c->r, 0, sizeof c->r);
	c->flags = 2;
	c->lf_op = LF_NONE;
	c->cr0 = 0x10;
	c->cpl = 0;
	for (int s = 0; s < 6; s++) {
		memset(&c->s[s], 0, sizeof c->s[s]);
		c->s[s].limit = 0xFFFF;
		c->s[s].access = 0x93;
	}
	c->s[S_CS].access = 0x9B;
	c->idt.base = 0;
	c->idt.limit = 0x3FF;
	c->gdt.base = 0;
	c->gdt.limit = 0xFFFF;
	c->a20 = 0xFFEFFFFF;
	c->cpu_level = 4;
	x87_reset(c);
}
