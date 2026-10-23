/*
 * hw.c - the PC around the CPU: two 8259 interrupt controllers, the 8254
 * timer, the 8042 keyboard controller, the CMOS clock, the speaker and the
 * A20 gate. Time is wall-clock time, so a game that times itself off the
 * timer runs at the right speed whatever pace the CPU is going at.
 */
#include "dos.h"
#include "../gate.h"

#define PIT_HZ 1193182ULL


/* ---- the 8259s ------------------------------------------------------------------ */

void pic_raise(struct dosbox *b, int irq)
{
	b->pic_irr[irq >> 3] |= (uint8_t)(1 << (irq & 7));
}

static int highest(uint8_t bits)
{
	for (int i = 0; i < 8; i++)
		if (bits & (1 << i))
			return i;
	return 8;
}

/* would pic_pending() have something? (without acknowledging it) */
int pic_waiting(struct dosbox *b)
{
	uint8_t slave_req = b->pic_irr[1] & (uint8_t)~b->pic_imr[1];
	int s = highest(slave_req);
	uint8_t req = b->pic_irr[0] & (uint8_t)~b->pic_imr[0];
	if (s < 8 && s < highest(b->pic_isr[1]))
		req |= 4;
	int m = highest(req);
	return m < 8 && m < highest(b->pic_isr[0]);
}

/* acknowledge the most important pending interrupt; returns its vector */
int pic_pending(struct dosbox *b)
{
	uint8_t slave_req = b->pic_irr[1] & (uint8_t)~b->pic_imr[1];
	int s = highest(slave_req);
	if (s < 8 && s < highest(b->pic_isr[1]))
		b->pic_irr[0] |= 4;		/* the slave asks on line 2 */
	uint8_t req = b->pic_irr[0] & (uint8_t)~b->pic_imr[0];
	int m = highest(req);
	if (m >= 8 || m >= highest(b->pic_isr[0]))
		return -1;
	b->pic_isr[0] |= (uint8_t)(1 << m);
	b->pic_irr[0] &= (uint8_t)~(1 << m);
	if (m == 2 && s < 8) {
		b->pic_isr[1] |= (uint8_t)(1 << s);
		b->pic_irr[1] &= (uint8_t)~(1 << s);
		return b->pic_base[1] + s;
	}
	return b->pic_base[0] + m;
}

/*
 * pic_init[p] counts the initialisation words still to come after ICW1:
 * bit 8 set while waiting for ICW2 (the vector base), then ICW3 unless
 * single mode, then ICW4 if ICW1 asked for it.
 */
static void pic_out(struct dosbox *b, int p, int cmd, uint8_t v)
{
	if (cmd) {
		if (v & 0x10) {			/* ICW1 */
			int words = 1 + !(v & 2) + (v & 1);	/* ICW2 [ICW3] [ICW4] */
			b->pic_init[p] = 0x100 | words;
			b->pic_imr[p] = 0;
			b->pic_isr[p] = 0;
			b->pic_irr[p] = 0;
			b->pic_read_isr[p] = 0;
			return;
		}
		if (v & 0x08) {			/* OCW3 */
			if (v & 2)
				b->pic_read_isr[p] = v & 1;
			return;
		}
		if ((v & 0xE0) == 0x20) {		/* non-specific EOI */
			int i = highest(b->pic_isr[p]);
			if (i < 8)
				b->pic_isr[p] &= (uint8_t)~(1 << i);
		} else if ((v & 0xE0) == 0x60) {	/* specific EOI */
			b->pic_isr[p] &= (uint8_t)~(1 << (v & 7));
		}
		return;
	}
	if (b->pic_init[p]) {
		if (b->pic_init[p] & 0x100)
			b->pic_base[p] = v & 0xF8;
		int left = (b->pic_init[p] & 0xFF) - 1;
		b->pic_init[p] = left > 0 ? left : 0;
		return;
	}
	b->pic_imr[p] = v;
}

/* ---- the 8254 --------------------------------------------------------------------- */

static uint64_t pit_elapsed(struct dosbox *b, int ch, uint64_t now)
{
	return (now - b->pit[ch].start_us) * PIT_HZ / 1000000ULL;
}

static uint16_t pit_count(struct dosbox *b, int ch)
{
	struct pit_ch *p = &b->pit[ch];
	uint64_t e = pit_elapsed(b, ch, dos_now(b));
	uint32_t r = p->reload;
	switch (p->mode) {
	case 0: case 1: case 4: case 5:
		return (uint16_t)(e >= r ? (uint32_t)(0x10000 - ((e - r) & 0xFFFF)) : r - (uint32_t)e);
	case 3:
		return (uint16_t)(r - (uint32_t)((e * 2) % r));
	default:
		return (uint16_t)(r - (uint32_t)(e % r));
	}
}

static int pit_output(struct dosbox *b, int ch)
{
	struct pit_ch *p = &b->pit[ch];
	uint64_t e = pit_elapsed(b, ch, dos_now(b));
	if (p->mode == 3)
		return (e % p->reload) < p->reload / 2;
	if (p->mode == 0)
		return e >= p->reload;
	return (e % p->reload) != p->reload - 1;
}

uint64_t pit_irq0_period_us(struct dosbox *b)
{
	uint64_t us = (uint64_t)b->pit[0].reload * 1000000ULL / PIT_HZ;
	return us ? us : 1;
}

void pit_tick(struct dosbox *b, uint64_t now)
{
	struct pit_ch *p = &b->pit[0];
	if (p->mode == 0 || p->mode == 1 || p->mode == 4 || p->mode == 5) {
		/* one-shot: one interrupt when the count runs out */
		if (b->irq0_next_us && now >= b->irq0_next_us) {
			pic_raise(b, 0);
			b->irq0_next_us = 0;
		}
		return;
	}
	uint64_t period = pit_irq0_period_us(b);
	if (!b->irq0_next_us)
		b->irq0_next_us = now + period;
	if (now >= b->irq0_next_us) {
		pic_raise(b, 0);
		b->irq0_next_us += period;
		if (now > b->irq0_next_us + period * 4)
			b->irq0_next_us = now + period;	/* fell behind: don't try to catch up */
	}
}

static void pit_load(struct dosbox *b, int ch, uint16_t v)
{
	struct pit_ch *p = &b->pit[ch];
	p->reload = v ? v : 0x10000;
	p->start_us = dos_now(b);
	if (ch == 0) {
		uint64_t period = pit_irq0_period_us(b);
		b->irq0_next_us = p->start_us + period;
	}
	if (ch == 2)
		speaker_update(b);
}

static void pit_out(struct dosbox *b, uint16_t port, uint8_t v)
{
	if (port == 0x43) {
		int ch = v >> 6;
		if (ch == 3) {			/* read-back: latch the counts asked for */
			for (int i = 0; i < 3; i++)
				if ((v & (2 << i)) && !(v & 0x20)) {
					b->pit[i].latch = pit_count(b, i);
					b->pit[i].latched = 1;
					b->pit[i].read_hi = 0;
				}
			return;
		}
		struct pit_ch *p = &b->pit[ch];
		int access = (v >> 4) & 3;
		if (!access) {			/* counter latch */
			p->latch = pit_count(b, ch);
			p->latched = 1;
			p->read_hi = 0;
			return;
		}
		p->access = access;
		p->mode = (v >> 1) & 7;
		if (p->mode > 5)
			p->mode -= 4;
		p->write_hi = 0;
		p->read_hi = 0;
		return;
	}
	int ch = port - 0x40;
	struct pit_ch *p = &b->pit[ch];
	switch (p->access) {
	case 1: pit_load(b, ch, v); break;
	case 2: pit_load(b, ch, (uint16_t)(v << 8)); break;
	default:
		if (!p->write_hi) {
			p->partial = v;
			p->write_hi = 1;
		} else {
			pit_load(b, ch, (uint16_t)(p->partial | v << 8));
			p->write_hi = 0;
		}
	}
}

static uint8_t pit_in(struct dosbox *b, uint16_t port)
{
	int ch = port - 0x40;
	if (ch == 3)
		return 0xFF;
	struct pit_ch *p = &b->pit[ch];
	uint16_t v = p->latched ? p->latch : pit_count(b, ch);
	uint8_t out;
	switch (p->access) {
	case 1: out = (uint8_t)v; p->latched = 0; break;
	case 2: out = (uint8_t)(v >> 8); p->latched = 0; break;
	default:
		if (!p->read_hi) {
			out = (uint8_t)v;
			p->read_hi = 1;
		} else {
			out = (uint8_t)(v >> 8);
			p->read_hi = 0;
			p->latched = 0;
		}
	}
	return out;
}

/* ---- the speaker: the real one ---------------------------------------------------- */

void speaker_update(struct dosbox *b)
{
	int hz = 0;
	if ((b->port61 & 3) == 3 && b->pit[2].reload > 16)
		hz = (int)(PIT_HZ / b->pit[2].reload);
	if (hz < 20 || hz > 20000)
		hz = 0;
	if (hz != b->speaker_hz) {
		b->speaker_hz = hz;
		bkl_enter();
		arch_speaker((uint32_t)hz);
		bkl_leave();
	}
}

/* ---- the 8042 ------------------------------------------------------------------------ */

static void set_a20(struct dosbox *b, int on)
{
	b->cpu.a20 = on ? 0xFFFFFFFF : 0xFFEFFFFF;
}

void kbd_push(struct dosbox *b, uint8_t sc)
{
	int next = (b->kbh + 1) % (int)sizeof b->kbq;
	if (next == b->kbt)
		return;
	b->kbq[b->kbh] = sc;
	b->kbh = next;
}

void kbd_pump(struct dosbox *b)
{
	if ((b->kbc_status & 1) || b->kbh == b->kbt)
		return;
	b->port60 = b->kbq[b->kbt];
	b->kbt = (b->kbt + 1) % (int)sizeof b->kbq;
	b->kbc_status |= 1;
	if (!(b->kbc_cmdbyte & 0x10) && (b->kbc_cmdbyte & 1))
		pic_raise(b, 1);
}

static void kbc_reply(struct dosbox *b, uint8_t v)
{
	/* controller replies go in front of the keys */
	b->kbt = (b->kbt + (int)sizeof b->kbq - 1) % (int)sizeof b->kbq;
	b->kbq[b->kbt] = v;
}

static void kbc_out(struct dosbox *b, uint16_t port, uint8_t v)
{
	if (port == 0x64) {
		b->kbc_cmd = 0;
		switch (v) {
		case 0x20: kbc_reply(b, b->kbc_cmdbyte); break;
		case 0x60: case 0xD1: case 0xD2: b->kbc_cmd = v; break;
		case 0xAA: kbc_reply(b, 0x55); break;
		case 0xAB: kbc_reply(b, 0x00); break;
		case 0xAD: b->kbc_cmdbyte |= 0x10; break;
		case 0xAE: b->kbc_cmdbyte &= (uint8_t)~0x10; break;
		case 0xC0: kbc_reply(b, 0xFF); break;
		case 0xD0: kbc_reply(b, (uint8_t)(0x01 | ((b->cpu.a20 & 0x100000) ? 2 : 0))); break;
		case 0xDD: set_a20(b, 0); break;
		case 0xDF: set_a20(b, 1); break;
		}
		return;
	}
	/* port 60h */
	if (b->kbc_cmd == 0x60) {
		b->kbc_cmdbyte = v;
		b->kbc_cmd = 0;
		return;
	}
	if (b->kbc_cmd == 0xD1) {
		set_a20(b, (v & 2) != 0);
		b->kbc_cmd = 0;
		return;
	}
	if (b->kbc_cmd == 0xD2) {
		kbd_push(b, v);
		b->kbc_cmd = 0;
		return;
	}
	if (b->kb_wait_cmd) {		/* the argument of ED/F3/F0 */
		b->kb_wait_cmd = 0;
		kbc_reply(b, 0xFA);
		return;
	}
	switch (v) {
	case 0xED: case 0xF3: case 0xF0:
		b->kb_wait_cmd = 1;
		kbc_reply(b, 0xFA);
		break;
	case 0xEE: kbc_reply(b, 0xEE); break;
	case 0xF2:
		kbc_reply(b, 0x41);
		kbc_reply(b, 0xAB);
		kbc_reply(b, 0xFA);
		break;
	case 0xFF:
		kbc_reply(b, 0xAA);
		kbc_reply(b, 0xFA);
		break;
	default:
		kbc_reply(b, 0xFA);
	}
}

/* ---- CMOS ------------------------------------------------------------------------------ */

static uint8_t bcd(int v)
{
	return (uint8_t)((v / 10) << 4 | (v % 10));
}

static uint8_t cmos_in(struct dosbox *b)
{
	int y, mo, d, h, mi, s;
	uint32_t ext_kb = b->cpu.mem_size > 0x100000 ? (b->cpu.mem_size - 0x100000) / 1024 : 0;
	if (ext_kb > 0xFFFF)
		ext_kb = 0xFFFF;
	switch (b->cmos_idx & 0x7F) {
	case 0x00: arch_rtc(&y, &mo, &d, &h, &mi, &s); return bcd(s);
	case 0x02: arch_rtc(&y, &mo, &d, &h, &mi, &s); return bcd(mi);
	case 0x04: arch_rtc(&y, &mo, &d, &h, &mi, &s); return bcd(h);
	case 0x06: return (uint8_t)((rtc_epoch() / 86400 + 4) % 7 + 1);
	case 0x07: arch_rtc(&y, &mo, &d, &h, &mi, &s); return bcd(d);
	case 0x08: arch_rtc(&y, &mo, &d, &h, &mi, &s); return bcd(mo);
	case 0x09: arch_rtc(&y, &mo, &d, &h, &mi, &s); return bcd(y % 100);
	case 0x32: arch_rtc(&y, &mo, &d, &h, &mi, &s); return bcd(y / 100);
	case 0x0A: return 0x26;
	case 0x0B: return 0x02;
	case 0x0D: return 0x80;
	case 0x10: return 0x40;			/* A: 1.44 MB */
	case 0x14: return 0x0F;			/* equipment: FPU, VGA */
	case 0x15: return 0x80;			/* 640 KB */
	case 0x16: return 0x02;
	case 0x17: case 0x30: return (uint8_t)ext_kb;
	case 0x18: case 0x31: return (uint8_t)(ext_kb >> 8);
	}
	return 0;
}

/* ---- dispatch -------------------------------------------------------------------------- */

uint32_t hw_in(struct x86 *c, uint16_t port, int size)
{
	struct dosbox *b = BOX(c);
	if (size > 1) {		/* wide accesses: byte by byte, as an ISA bus would */
		uint32_t v = 0;
		for (int i = 0; i < size; i++)
			v |= hw_in(c, (uint16_t)(port + i), 1) << (8 * i);
		return v;
	}
	switch (port) {
	case 0x20: case 0xA0: {
		int p = port == 0xA0;
		return b->pic_read_isr[p] ? b->pic_isr[p] : b->pic_irr[p];
	}
	case 0x21: case 0xA1:
		return b->pic_imr[port == 0xA1];
	case 0x40: case 0x41: case 0x42: case 0x43:
		return pit_in(b, port);
	case 0x60:
		b->kbc_status &= (uint8_t)~1;
		return b->port60;
	case 0x61: {
		uint8_t v = b->port61 & 0x0F;
		if ((dos_now(b) / 15) & 1)
			v |= 0x10;		/* memory refresh: programs time short delays with it */
		if (pit_output(b, 2))
			v |= 0x20;
		return v;
	}
	case 0x64:
		return (uint8_t)(b->kbc_status | 0x14);
	case 0x71:
		return cmos_in(b);
	case 0x92:
		return (b->cpu.a20 & 0x100000) ? 2 : 0;
	case 0x201:
		return 0xF0;		/* no joystick: the axes never time out */
	}
	if (port >= 0x3B0 && port <= 0x3DF)
		return vga_in(b, port);
	if ((port >= 0x220 && port <= 0x22F))
		return sb_in(b, port);
	if (port >= 0x388 && port <= 0x38B)
		return opl_in(b, port);
	if (port <= 0x0F || (port >= 0x80 && port <= 0x8F) || (port >= 0xC0 && port <= 0xDF))
		return dma_in(b, port);
	return 0xFF;
}

void hw_out(struct x86 *c, uint16_t port, uint32_t v, int size)
{
	struct dosbox *b = BOX(c);
	if (size > 1) {
		/* VGA index/data pairs written as one word: index then data */
		for (int i = 0; i < size; i++)
			hw_out(c, (uint16_t)(port + i), (v >> (8 * i)) & 0xFF, 1);
		return;
	}
	uint8_t x = (uint8_t)v;
	switch (port) {
	case 0x20: case 0x21: case 0xA0: case 0xA1:
		pic_out(b, port >= 0xA0, !(port & 1), x);
		return;
	case 0x40: case 0x41: case 0x42: case 0x43:
		pit_out(b, port, x);
		return;
	case 0x60: case 0x64:
		kbc_out(b, port, x);
		return;
	case 0x61:
		b->port61 = x;
		speaker_update(b);
		return;
	case 0x70:
		b->cmos_idx = x;
		return;
	case 0x71:
		return;			/* the guest doesn't get to set the real clock */
	case 0x92:
		set_a20(b, (x & 2) != 0);
		return;
	}
	if (port >= 0x3B0 && port <= 0x3DF) {
		vga_out(b, port, x);
		return;
	}
	if (port >= 0x220 && port <= 0x22F) {
		sb_out(b, port, x);
		return;
	}
	if (port >= 0x388 && port <= 0x38B) {
		opl_out(b, port, x);
		return;
	}
	if (port <= 0x0F || (port >= 0x80 && port <= 0x8F) || (port >= 0xC0 && port <= 0xDF))
		dma_out(b, port, x);
}

void hw_reset(struct dosbox *b)
{
	b->pic_base[0] = 0x08;
	b->pic_base[1] = 0x70;
	b->pic_imr[0] = 0x00;
	b->pic_imr[1] = 0x00;
	for (int i = 0; i < 3; i++) {
		b->pit[i].reload = 0x10000;
		b->pit[i].mode = 3;
		b->pit[i].access = 3;
		b->pit[i].start_us = dos_now(b);
	}
	b->pit[1].reload = 18;
	b->pit[2].reload = 1331;
	b->irq0_next_us = 0;
	b->kbc_cmdbyte = 0x45;
	b->kbc_status = 0;
	b->port61 = 0;
	sb_reset(b);
}
