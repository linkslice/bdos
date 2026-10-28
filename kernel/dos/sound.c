/*
 * sound.c - a Sound Blaster 16, the 8237 DMA controllers, and an OPL2.
 *
 * The DSP plays what DMA hands it at the rate the program set, in step
 * with the wall clock (like the real card), and raises IRQ 7 at the end
 * of every block. The FM synthesizer is an OPL2 done the way the chip
 * does it - log-sine and exponent tables, 9-bit envelopes, phase
 * modulation with feedback - at 48 kHz. Both turn into streams for the
 * mixer (audio.c), which the sound card plays.
 */
#include "dos.h"
#include "../audio.h"
#include "opl_tables.h"

/* ======================================================================================
 * The DMA controllers
 * ====================================================================================== */

static const uint8_t page_port[8] = { 0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A };

uint32_t dma_in(struct dosbox *b, uint16_t port)
{
	if (port <= 0x0F || (port >= 0xC0 && port <= 0xDF)) {
		int hi = port >= 0xC0;
		int reg = hi ? (port - 0xC0) / 2 : port;
		if (reg < 8) {
			struct dma_ch *ch = &b->dma[(hi ? 4 : 0) + reg / 2];
			uint16_t v = (reg & 1) ? ch->cur_count : ch->cur_addr;
			int ff = b->dma_ff[hi];
			b->dma_ff[hi] ^= 1;
			return ff ? v >> 8 : v & 0xFF;
		}
		if (reg == 8) {		/* status: terminal counts (cleared by reading) */
			uint8_t st = 0;
			for (int i = 0; i < 4; i++) {
				struct dma_ch *ch = &b->dma[(hi ? 4 : 0) + i];
				if (ch->tc)
					st |= (uint8_t)(1 << i);
				ch->tc = 0;
			}
			return st;
		}
		return 0xFF;
	}
	for (int i = 0; i < 8; i++)
		if (port == page_port[i])
			return b->dma[i].page;
	return 0xFF;
}

void dma_out(struct dosbox *b, uint16_t port, uint8_t v)
{
	if (port <= 0x0F || (port >= 0xC0 && port <= 0xDF)) {
		int hi = port >= 0xC0;
		int reg = hi ? (port - 0xC0) / 2 : port;
		int base = hi ? 4 : 0;
		if (reg < 8) {
			struct dma_ch *ch = &b->dma[base + reg / 2];
			uint16_t *p = (reg & 1) ? &ch->base_count : &ch->base_addr;
			if (b->dma_ff[hi])
				*p = (uint16_t)((*p & 0x00FF) | v << 8);
			else
				*p = (uint16_t)((*p & 0xFF00) | v);
			b->dma_ff[hi] ^= 1;
			if (reg & 1)
				ch->cur_count = ch->base_count;
			else
				ch->cur_addr = ch->base_addr;
			return;
		}
		switch (reg) {
		case 0x0A:		/* one mask bit */
			b->dma[base + (v & 3)].masked = (v >> 2) & 1;
			return;
		case 0x0B:		/* mode */
			b->dma[base + (v & 3)].mode = v;
			return;
		case 0x0C:		/* clear the flip-flop */
			b->dma_ff[hi] = 0;
			return;
		case 0x0D:		/* master clear */
			b->dma_ff[hi] = 0;
			for (int i = 0; i < 4; i++)
				b->dma[base + i].masked = 1;
			return;
		case 0x0E:		/* clear all masks */
			for (int i = 0; i < 4; i++)
				b->dma[base + i].masked = 0;
			return;
		case 0x0F:		/* all mask bits */
			for (int i = 0; i < 4; i++)
				b->dma[base + i].masked = (v >> i) & 1;
			return;
		}
		return;
	}
	for (int i = 0; i < 8; i++)
		if (port == page_port[i])
			b->dma[i].page = v;
}

/* read one transfer (a byte, or a word on channels 4-7) from memory; -1 if the channel won't */
static int dma_read(struct dosbox *b, int c)
{
	struct dma_ch *ch = &b->dma[c];
	if (ch->masked)
		return -1;
	uint32_t a;
	int v;
	if (c >= 4) {
		a = ((uint32_t)(ch->page & 0xFE) << 16) | (uint32_t)ch->cur_addr << 1;
		v = x86_peek8(&b->cpu, a) | x86_peek8(&b->cpu, a + 1) << 8;
	} else {
		a = (uint32_t)ch->page << 16 | ch->cur_addr;
		v = x86_peek8(&b->cpu, a);
	}
	if (ch->mode & 0x20)
		ch->cur_addr--;
	else
		ch->cur_addr++;
	if (ch->cur_count-- == 0) {
		ch->tc = 1;
		if (ch->mode & 0x10) {		/* auto-initialize */
			ch->cur_addr = ch->base_addr;
			ch->cur_count = ch->base_count;
		} else {
			ch->masked = 1;
			ch->cur_count = 0xFFFF;
		}
	}
	return v;
}

/* ======================================================================================
 * The Sound Blaster 16 DSP and mixer
 * ====================================================================================== */

static void out_byte(struct sb *s, uint8_t v)
{
	if (s->out_len < (int)sizeof s->outq)
		s->outq[(s->out_head + s->out_len++) % sizeof s->outq] = v;
}

static void mixer_reset(struct sb *s)
{
	memset(s->mixer, 0, sizeof s->mixer);
	s->mixer[0x22] = 0xEE;		/* SB Pro master */
	s->mixer[0x04] = 0xEE;		/* SB Pro voice */
	s->mixer[0x26] = 0xEE;		/* SB Pro FM */
	s->mixer[0x30] = s->mixer[0x31] = 0xF8;	/* SB16 master */
	s->mixer[0x32] = s->mixer[0x33] = 0xF8;	/* voice */
	s->mixer[0x34] = s->mixer[0x35] = 0xF8;	/* FM */
	s->mixer[0x80] = 0x04;		/* IRQ 7 */
	s->mixer[0x81] = 0x22;		/* DMA 1 and 5 */
}

void sb_reset(struct dosbox *b)
{
	struct sb *s = &b->sb;
	int stream = s->stream;
	uint8_t keep_mixer[256];
	memcpy(keep_mixer, s->mixer, sizeof keep_mixer);
	int had_mixer = s->mixer[0x80] != 0;
	memset(s, 0, sizeof *s);
	s->stream = stream;
	if (had_mixer)
		memcpy(s->mixer, keep_mixer, sizeof keep_mixer);
	else
		mixer_reset(s);
	s->rate = 22050;
	s->block8 = 0x7FF;
	s->speaker = 0;
	for (int i = 0; i < 8; i++) {
		b->dma[i].masked = 1;
	}
	b->dma[4].masked = 0;		/* the cascade */
	for (int i = 0; i < 18; i++)
		if (!b->opl.op[i].state)
			b->opl.op[i].env = 511u << 16;
}

/* the level a mixer pair gives: 0..256 */
static int mixer_level(struct sb *s, int reg)
{
	int l = s->mixer[reg] >> 3, r = s->mixer[reg + 1] >> 3;
	return (l + r) * 256 / 62;
}

static void apply_volume(struct dosbox *b)
{
	struct sb *s = &b->sb;
	int master = mixer_level(s, 0x30);
	if (s->stream)
		audio_set_volume(s->stream - 1, mixer_level(s, 0x32) * master >> 8);
	if (b->opl.stream)
		audio_set_volume(b->opl.stream - 1, mixer_level(s, 0x34) * master >> 8);
}

static void raise_irq(struct dosbox *b, int bit16)
{
	b->sb.irq_pending |= bit16 ? 2 : 1;
	pic_raise(b, SB_IRQ);
}

static int args_for(uint8_t cmd)
{
	if (cmd >= 0xB0 && cmd <= 0xCF)
		return 3;
	switch (cmd) {
	case 0x10: case 0x40: case 0xE0: case 0xE4: case 0x38:
		return 1;
	case 0x14: case 0x16: case 0x17: case 0x24: case 0x41: case 0x42: case 0x48: case 0x80:
	case 0x74: case 0x75: case 0x76: case 0x77:
		return 2;
	}
	return 0;
}

static void start_dma(struct dosbox *b, int is16, int autoinit, uint32_t samples, int stereo, int is_signed)
{
	struct sb *s = &b->sb;
	s->dma_on = 1;
	s->dma16 = is16;
	s->autoinit = autoinit;
	s->stereo = stereo;
	s->is_signed = is_signed;
	s->paused = 0;
	s->exit_auto = 0;
	s->block_len = samples ? samples : 1;
	s->block_left = s->block_len;
	s->last_us = uptime_us();
	s->carry = 0;
}

static void render_direct(struct dosbox *b, uint64_t now);

static void dsp_command(struct dosbox *b)
{
	struct sb *s = &b->sb;
	uint8_t *a = s->args;
	uint16_t len = (uint16_t)(a[0] | a[1] << 8);
	switch (s->cmd) {
	case 0x10:		/* direct DAC */
		render_direct(b, uptime_us());
		s->direct = a[0];
		s->direct_until = uptime_us() + 100000;
		break;
	case 0x14: case 0x16: case 0x17:	/* 8-bit single-cycle DMA (16/17: ADPCM, played as PCM) */
		start_dma(b, 0, 0, (uint32_t)len + 1, (s->mixer[0x0E] & 2) != 0, 0);
		break;
	case 0x1C: case 0x1F:	/* 8-bit auto-init */
		start_dma(b, 0, 1, (uint32_t)s->block8 + 1, (s->mixer[0x0E] & 2) != 0, 0);
		break;
	case 0x90: case 0x91:	/* high speed 8-bit */
		start_dma(b, 0, s->cmd == 0x90, (uint32_t)s->block8 + 1, (s->mixer[0x0E] & 2) != 0, 0);
		break;
	case 0x40: {		/* time constant */
		uint32_t r = 1000000 / (uint32_t)(256 - a[0]);
		s->rate = (s->mixer[0x0E] & 2) ? r / 2 : r;
		break;
	}
	case 0x41: case 0x42:
		s->rate = (uint32_t)(a[0] << 8 | a[1]);
		break;
	case 0x48:
		s->block8 = len;
		break;
	case 0x80:		/* a block of silence */
		s->silence_left = len + 1;
		s->last_us = uptime_us();
		s->carry = 0;
		break;
	case 0xD0: case 0xD5: s->paused = 1; break;
	case 0xD4: case 0xD6: s->paused = 0; s->last_us = uptime_us(); break;
	case 0xD1: s->speaker = 1; break;
	case 0xD3: s->speaker = 0; break;
	case 0xD8: out_byte(s, s->speaker ? 0xFF : 0x00); break;
	case 0xD9: case 0xDA: s->exit_auto = 1; break;
	case 0xE0: out_byte(s, (uint8_t)~a[0]); break;
	case 0xE1: out_byte(s, 4); out_byte(s, 5); break;		/* DSP 4.05: a Sound Blaster 16 */
	case 0xE3: {
		const char *c = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";
		while (*c)
			out_byte(s, (uint8_t)*c++);
		out_byte(s, 0);
		break;
	}
	case 0xE4: s->test_reg = a[0]; break;
	case 0xE8: out_byte(s, s->test_reg); break;
	case 0xF2: raise_irq(b, 0); break;
	case 0xF3: raise_irq(b, 1); break;
	case 0xF8: out_byte(s, 0); break;
	case 0x20: out_byte(s, 0x80); break;	/* direct ADC: silence */
	default:
		if (s->cmd >= 0xB0 && s->cmd <= 0xCF && !(s->cmd & 0x08)) {
			/* SB16 DMA: B0-B7 16-bit, C0-C7 8-bit; bit 2 auto-init; then mode, length */
			uint16_t n = (uint16_t)(a[1] | a[2] << 8);
			start_dma(b, s->cmd < 0xC0, (s->cmd & 4) != 0, (uint32_t)n + 1, (a[0] & 0x20) != 0,
			    (a[0] & 0x10) != 0);
		}
		break;
	}
}

static void dsp_write(struct dosbox *b, uint8_t v)
{
	struct sb *s = &b->sb;
	if (s->need > s->got) {
		s->args[s->got++] = v;
		if (s->got == s->need) {
			s->need = s->got = 0;
			dsp_command(b);
		}
		return;
	}
	s->cmd = v;
	s->need = args_for(v);
	s->got = 0;
	if (!s->need)
		dsp_command(b);
}

uint32_t opl_in(struct dosbox *b, uint16_t port);
void opl_out(struct dosbox *b, uint16_t port, uint8_t v);

uint32_t sb_in(struct dosbox *b, uint16_t port)
{
	struct sb *s = &b->sb;
	switch (port & 0xF) {
	case 0x0: case 0x1: case 0x2: case 0x3: case 0x8: case 0x9:
		return opl_in(b, port);
	case 0x5:
		if (s->mixer_idx == 0x82)
			return s->irq_pending | 0x40;
		return s->mixer[s->mixer_idx];
	case 0xA: {		/* read data */
		if (!s->out_len)
			return 0xFF;
		uint8_t v = s->outq[s->out_head];
		s->out_head = (s->out_head + 1) % (int)sizeof s->outq;
		s->out_len--;
		return v;
	}
	case 0xC:		/* write status: always ready */
		return 0x7F;
	case 0xE:		/* read status; acknowledges the 8-bit IRQ */
		s->irq_pending &= (uint8_t)~1;
		return s->out_len ? 0xFF : 0x7F;
	case 0xF:		/* acknowledges the 16-bit IRQ */
		s->irq_pending &= (uint8_t)~2;
		return 0xFF;
	}
	return 0xFF;
}

void sb_out(struct dosbox *b, uint16_t port, uint8_t v)
{
	struct sb *s = &b->sb;
	switch (port & 0xF) {
	case 0x0: case 0x1: case 0x2: case 0x3: case 0x8: case 0x9:
		opl_out(b, port, v);
		return;
	case 0x4:
		s->mixer_idx = v;
		return;
	case 0x5:
		if (s->mixer_idx == 0x00) {
			mixer_reset(s);
		} else if (s->mixer_idx == 0x80 || s->mixer_idx == 0x81 || s->mixer_idx == 0x82) {
			/* the IRQ and DMA settings are fixed here */
		} else {
			s->mixer[s->mixer_idx] = v;
			/* SB Pro registers set the SB16 ones they stand for */
			if (s->mixer_idx == 0x22) {
				s->mixer[0x30] = (uint8_t)((v & 0xF0) | 8);
				s->mixer[0x31] = (uint8_t)((v << 4) | 8);
			} else if (s->mixer_idx == 0x04) {
				s->mixer[0x32] = (uint8_t)((v & 0xF0) | 8);
				s->mixer[0x33] = (uint8_t)((v << 4) | 8);
			} else if (s->mixer_idx == 0x26) {
				s->mixer[0x34] = (uint8_t)((v & 0xF0) | 8);
				s->mixer[0x35] = (uint8_t)((v << 4) | 8);
			}
		}
		apply_volume(b);
		return;
	case 0x6:		/* reset: 1 then 0, and the DSP says AAh */
		if (v & 1) {
			s->resetting = 1;
		} else if (s->resetting) {
			int stream = s->stream;
			uint8_t mix[256];
			memcpy(mix, s->mixer, sizeof mix);
			memset(s, 0, sizeof *s);
			memcpy(s->mixer, mix, sizeof mix);
			s->stream = stream;
			s->rate = 22050;
			s->block8 = 0x7FF;
			out_byte(s, 0xAA);
		}
		return;
	case 0xC:
		dsp_write(b, v);
		return;
	}
}

static int sb_stream(struct dosbox *b, uint32_t rate)
{
	struct sb *s = &b->sb;
	if (!s->stream) {
		int id = audio_open(rate);
		if (id < 0)
			return -1;
		s->stream = id + 1;
		s->stream_rate = rate;
		apply_volume(b);
	} else if (s->stream_rate != rate) {
		audio_set_rate(s->stream - 1, rate);
		s->stream_rate = rate;
	}
	return s->stream - 1;
}

/* direct DAC output (10h): the level held since the last write */
static void render_direct(struct dosbox *b, uint64_t now)
{
	struct sb *s = &b->sb;
	if (s->dma_on || now > s->direct_until + 100000 || !s->direct_until) {
		s->last_us = now;
		s->carry = 0;
		return;
	}
	uint64_t span = now - s->last_us;
	s->last_us = now;
	if (span > 50000)
		span = 50000;
	s->carry += span * 22050;
	uint32_t n = (uint32_t)(s->carry / 1000000);
	s->carry -= (uint64_t)n * 1000000;
	int id = sb_stream(b, 22050);
	if (id < 0 || !n)
		return;
	int16_t buf[256];
	int16_t v = (int16_t)((s->direct - 128) << 8);
	for (uint32_t i = 0; i < MIN(n, 256u); i++)
		buf[i] = v;
	audio_write(id, buf, MIN(n, 256u), 1);
}

/* DMA playback: as many samples as the time since last time is worth */
static void render_dma(struct dosbox *b, uint64_t now)
{
	struct sb *s = &b->sb;
	if (s->silence_left > 0) {
		uint64_t span = now - s->last_us;
		s->last_us = now;
		s->carry += span * s->rate;
		uint32_t n = (uint32_t)(s->carry / 1000000);
		s->carry -= (uint64_t)n * 1000000;
		s->silence_left -= (int)MIN(n, (uint32_t)s->silence_left);
		if (!s->silence_left)
			raise_irq(b, 0);
		return;
	}
	if (!s->dma_on || s->paused) {
		s->last_us = now;
		s->carry = 0;
		return;
	}
	uint32_t rate = MAX(s->rate, 4000u);
	uint64_t span = now - s->last_us;
	if (span > 100000)
		span = 100000;		/* we were held up: don't try to catch up a long gap */
	s->last_us = now;
	s->carry += span * rate;
	uint32_t frames = (uint32_t)(s->carry / 1000000);
	s->carry -= (uint64_t)frames * 1000000;
	int ch = s->stereo ? 2 : 1;
	int dmac = s->dma16 ? SB_DMA16 : SB_DMA8;
	int16_t buf[2048];
	uint32_t n = 0;
	int id = sb_stream(b, rate);
	while (frames--) {
		for (int k = 0; k < ch; k++) {
			int v = dma_read(b, dmac);
			if (v < 0) {
				frames = 0;	/* the DMA channel's off: the DSP waits */
				break;
			}
			int16_t smp;
			if (s->dma16)
				smp = s->is_signed ? (int16_t)v : (int16_t)(v - 32768);
			else
				smp = s->is_signed ? (int16_t)((int8_t)v << 8) : (int16_t)((v - 128) << 8);
			buf[n++] = smp;
			if (s->block_left)
				s->block_left--;
		}
		if (!s->block_left) {
			raise_irq(b, s->dma16);
			if (s->autoinit && !s->exit_auto) {
				s->block_left = s->block_len;
			} else {
				s->dma_on = 0;
				break;
			}
		}
		if (n + 2 > NELEM(buf)) {
			if (id >= 0)
				audio_write(id, buf, n / (uint32_t)ch, ch);
			n = 0;
		}
	}
	if (n && id >= 0)
		audio_write(id, buf, n / (uint32_t)ch, ch);
}

/* ======================================================================================
 * The OPL2
 * ====================================================================================== */

/* operator register offsets, and the two operators of each channel */
static const int8_t slot_of_offset[32] = { 0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1,
					    12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static const uint8_t ch_slot[9] = { 0, 1, 2, 6, 7, 8, 12, 13, 14 };	/* modulators; carriers are +3 */
static const uint8_t slot_offset[18] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21 };
static const uint8_t mult2[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
static const uint8_t ksl_rom[16] = { 0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64 };
static const uint8_t ksl_shift[4] = { 8, 1, 2, 0 };

static int slot_channel(int slot)
{
	for (int c = 0; c < 9; c++)
		if (ch_slot[c] == slot || ch_slot[c] + 3 == slot)
			return c;
	return 0;
}

/* everything an operator needs from the registers */
static void opl_update(struct opl *o, int slot)
{
	struct opl_op *op = &o->op[slot];
	int off = slot_offset[slot], c = slot_channel(slot);
	uint8_t r20 = o->reg[0x20 + off], r40 = o->reg[0x40 + off], r60 = o->reg[0x60 + off],
		r80 = o->reg[0x80 + off];
	int fnum = o->reg[0xA0 + c] | (o->reg[0xB0 + c] & 3) << 8, block = (o->reg[0xB0 + c] >> 2) & 7;
	/* phase step at the chip's 49716 Hz, scaled to our 48 kHz (10-bit phase = acc >> 10) */
	uint32_t inc = ((uint32_t)fnum << block) * mult2[r20 & 15] / 2;
	op->inc = (uint32_t)(((uint64_t)inc * 67879) >> 16);
	int ksl = (ksl_rom[fnum >> 6] << 2) - ((8 - block) << 5);
	if (ksl < 0)
		ksl = 0;
	op->att_static = (uint16_t)(((r40 & 0x3F) << 2) + (ksl >> ksl_shift[r40 >> 6]));
	/* key scaling of the rates */
	int nts = (o->reg[0x08] >> 6) & 1;
	int rof = block << 1 | ((fnum >> (nts ? 8 : 9)) & 1);
	if (!(r20 & 0x10))
		rof >>= 2;
	int ar = r60 >> 4, dr = r60 & 15, rr = r80 & 15;
	op->eff_ar = (uint8_t)(ar ? MIN(63, ar * 4 + rof) : 0);
	op->eff_dr = (uint8_t)(dr ? MIN(63, dr * 4 + rof) : 0);
	op->eff_rr = (uint8_t)(rr ? MIN(63, rr * 4 + rof) : 0);
	int sl = r80 >> 4;
	op->sl = (uint16_t)(sl == 15 ? 31 << 4 : sl << 4);	/* 3 dB steps, 15 is 93 dB */
	op->wave = (o->reg[0x01] & 0x20) ? o->reg[0xE0 + off] & 3 : 0;
}

static void key(struct opl *o, int slot, int on)
{
	struct opl_op *op = &o->op[slot];
	if (on && (op->state == 0 || op->state == 4)) {
		op->state = 1;
		op->phase = 0;
	} else if (!on && op->state && op->state != 4) {
		op->state = 4;
	}
}

static int32_t opl_wave(int phase, int env, int wave)
{
	phase &= 0x3FF;
	int neg = phase & 0x200, level;
	switch (wave) {
	case 1:			/* half sine */
		if (neg)
			return 0;
		break;
	case 2:			/* both halves positive */
		neg = 0;
		break;
	case 3:			/* quarters */
		if (phase & 0x100)
			return 0;
		neg = 0;
		break;
	}
	level = (phase & 0x100) ? opl_logsin[(phase & 0xFF) ^ 0xFF] : opl_logsin[phase & 0xFF];
	level += env << 3;
	if (level > 0x1FFF)
		level = 0x1FFF;
	int32_t v = ((opl_exp[(level & 0xFF) ^ 0xFF] + 1024) << 1) >> (level >> 8);
	return neg ? -v : v;
}

/* one operator, one sample: its output (about +-4096) */
static int32_t op_step(struct opl *o, int slot, int32_t mod, int trem, int vib_cents)
{
	struct opl_op *op = &o->op[slot];
	if (!op->state) {
		op->out = op->prev = 0;
		return 0;
	}
	/* the envelope */
	switch (op->state) {
	case 1:
		if (op->eff_ar >= 60) {
			op->env = 0;
		} else {
			uint32_t k = opl_atk[op->eff_ar];
			op->env -= (uint32_t)(((uint64_t)op->env * k) >> 16) + (k ? 1 : 0);
			if ((int32_t)op->env < 0)
				op->env = 0;
		}
		if (op->env < (1 << 16))
			op->env = 0, op->state = 2;
		break;
	case 2:
		op->env += opl_dec[op->eff_dr];
		if (op->env >= (uint32_t)op->sl << 16) {
			op->env = (uint32_t)op->sl << 16;
			op->state = 3;
		}
		break;
	case 3:
		if (!(o->reg[0x20 + slot_offset[slot]] & 0x20))	/* not sustained: keep going */
			op->env += opl_dec[op->eff_rr];
		break;
	case 4:
		op->env += opl_dec[op->eff_rr];
		break;
	}
	if (op->env >= 511u << 16) {
		op->env = 511u << 16;
		if (op->state >= 3)
			op->state = 0;
	}
	int att = (int)(op->env >> 16) + op->att_static;
	if (o->reg[0x20 + slot_offset[slot]] & 0x80)
		att += trem;
	if (att > 511)
		att = 511;
	uint32_t inc = op->inc;
	if ((o->reg[0x20 + slot_offset[slot]] & 0x40) && vib_cents)
		inc = (uint32_t)((int64_t)inc + ((int64_t)inc * vib_cents >> 16));
	op->phase += inc;
	int32_t v = opl_wave((int)(op->phase >> 10) + mod, att, op->wave);
	op->prev = op->out;
	op->out = v;
	return v;
}

static int opl_busy(struct opl *o)
{
	for (int i = 0; i < 18; i++)
		if (o->op[i].state)
			return 1;
	return 0;
}

static void opl_render(struct dosbox *b, int16_t *out, uint32_t frames)
{
	struct opl *o = &b->opl;
	int rhythm = (o->reg[0xBD] & 0x20) != 0;
	for (uint32_t i = 0; i < frames; i++) {
		/* LFOs: tremolo 3.7 Hz (1 or 4.8 dB), vibrato 6.1 Hz (7 or 14 cents) */
		o->lfo++;
		uint32_t tp = (o->lfo * 79) & 0xFFFFF;		/* 3.7 Hz over a 2^20 cycle */
		int tri = (int)(tp >> 12);			/* 0..255 */
		tri = tri < 128 ? tri : 255 - tri;		/* 0..127 */
		int trem = (o->reg[0xBD] & 0x80) ? tri * 26 / 127 : tri * 5 / 127;
		uint32_t vp = (o->lfo * 133) & 0xFFFFF;
		int vt = (int)(vp >> 12);
		int vtri = vt < 64 ? vt : vt < 192 ? 128 - vt : vt - 256;	/* -64..64 */
		int vib = vtri * ((o->reg[0xBD] & 0x40) ? 530 : 265) / 64;	/* 14/7 cents in 16.16 */
		o->noise = o->noise * 1103515245u + 12345u;
		int32_t mix = 0;
		for (int c = 0; c < 9; c++) {
			int m = ch_slot[c], car = m + 3;
			if (!o->op[m].state && !o->op[car].state)
				continue;
			if (rhythm && c >= 6)
				continue;
			uint8_t cfb = o->reg[0xC0 + c];
			int fb = (cfb >> 1) & 7;
			int32_t fbmod = fb ? (o->op[m].out + o->op[m].prev) >> (9 - fb) : 0;
			int32_t mo = op_step(o, m, fbmod, trem, vib);
			if (cfb & 1)
				mix += mo + op_step(o, car, 0, trem, vib);
			else
				mix += op_step(o, car, mo, trem, vib);
		}
		if (rhythm) {
			/* bass drum: channel 6 as usual; the others one operator each */
			uint8_t cfb = o->reg[0xC6];
			int fb = (cfb >> 1) & 7;
			int32_t fbmod = fb ? (o->op[12].out + o->op[12].prev) >> (9 - fb) : 0;
			int32_t mo = op_step(o, 12, fbmod, trem, vib);
			mix += 2 * ((cfb & 1) ? op_step(o, 15, 0, trem, vib) : op_step(o, 15, mo, trem, vib));
			int32_t nz = (int32_t)(o->noise >> 22) - 512;	/* a little noise for the cymbals */
			mix += 2 * op_step(o, 13, nz, trem, vib);	/* hi-hat */
			mix += 2 * op_step(o, 16, nz >> 1, trem, vib);	/* snare */
			mix += 2 * op_step(o, 14, 0, trem, vib);	/* tom */
			mix += 2 * op_step(o, 17, nz, trem, vib);	/* cymbal */
		}
		mix = mix * 3;
		out[i] = (int16_t)(mix > 32767 ? 32767 : mix < -32768 ? -32768 : mix);
	}
}

uint32_t opl_in(struct dosbox *b, uint16_t port)
{
	struct opl *o = &b->opl;
	if (port & 1)
		return 0xFF;
	/* the status register: the timers, worked out from the clock */
	uint64_t now = uptime_us();
	uint8_t mask = o->reg[0x04];
	/* a program waiting the 80 us a timer takes by reading this register a few
	 * dozen times gets less than a microsecond here: count the reads too */
	o->reads++;
	if (o->t1_on && (now - o->t1_start >= (uint64_t)(256 - o->reg[0x02]) * 80 || o->reads >= 16) && !(mask & 0x40))
		o->status |= 0x40;
	if (o->t2_on && (now - o->t2_start >= (uint64_t)(256 - o->reg[0x03]) * 320 || o->reads >= 16) && !(mask & 0x20))
		o->status |= 0x20;
	uint8_t st = o->status & 0x60;
	if (st)
		st |= 0x80;
	return st | 0x06;	/* 06: an OPL2, not an OPL3 */
}

void opl_out(struct dosbox *b, uint16_t port, uint8_t v)
{
	struct opl *o = &b->opl;
	if (!(port & 1)) {
		o->idx = v;
		return;
	}
	uint8_t r = o->idx;
	uint8_t was = o->reg[r];
	o->reg[r] = v;
	if (r == 0x04) {
		if (v & 0x80) {
			o->status = 0;
			o->reg[0x04] = was & 0x7F;
			return;
		}
		uint64_t now = uptime_us();
		o->reads = 0;
		if ((v & 1) && !o->t1_on)
			o->t1_start = now;
		if ((v & 2) && !o->t2_on)
			o->t2_start = now;
		o->t1_on = v & 1;
		o->t2_on = (v >> 1) & 1;
		return;
	}
	if ((r >= 0x20 && r < 0xA0) || (r >= 0xE0 && r < 0xF6)) {
		int s = slot_of_offset[r & 0x1F];
		if (s >= 0 && (r & 0x1F) < 0x16)
			opl_update(o, s);
		return;
	}
	if (r == 0x01) {
		for (int s = 0; s < 18; s++)
			opl_update(o, s);
		return;
	}
	if (r >= 0xA0 && r <= 0xB8 && (r & 0xF) <= 8) {
		int c = r & 0xF;
		opl_update(o, ch_slot[c]);
		opl_update(o, ch_slot[c] + 3);
		if (r >= 0xB0) {
			int on = (v >> 5) & 1, wason = (was >> 5) & 1;
			if (on != wason) {
				key(o, ch_slot[c], on);
				key(o, ch_slot[c] + 3, on);
			}
		}
		return;
	}
	if (r == 0xBD) {
		/* rhythm: BD (ch 6), SD (op 16), TT (op 14), CY (op 17), HH (op 13) */
		if (v & 0x20) {
			static const int bits[5][2] = { { 0x10, 12 }, { 0x08, 16 }, { 0x04, 14 }, { 0x02, 17 }, { 0x01, 13 } };
			for (int i = 0; i < 5; i++) {
				int on = (v & bits[i][0]) != 0, wason = (was & bits[i][0]) != 0 && (was & 0x20);
				if (on != wason) {
					key(o, bits[i][1], on);
					if (i == 0)
						key(o, 15, on);
				}
			}
		}
		return;
	}
}

static void render_opl(struct dosbox *b, uint64_t now)
{
	struct opl *o = &b->opl;
	uint64_t span = now - o->last_us;
	o->last_us = now;
	if (span > 100000)
		span = 100000;
	if (!opl_busy(o)) {
		o->carry = 0;
		return;
	}
	o->carry += span * AUDIO_RATE;
	uint32_t n = (uint32_t)(o->carry / 1000000);
	o->carry -= (uint64_t)n * 1000000;
	if (!o->stream) {
		int id = audio_open(AUDIO_RATE);
		if (id < 0)
			return;
		o->stream = id + 1;
		apply_volume(b);
	}
	int16_t buf[1024];
	while (n) {
		uint32_t k = MIN(n, (uint32_t)NELEM(buf));
		opl_render(b, buf, k);
		audio_write(o->stream - 1, buf, k, 1);
		n -= k;
	}
}

/* ======================================================================================
 * The box's loop calls this every time round
 * ====================================================================================== */

/*
 * The PC speaker, drawn: the cone is out while port 61h's data bit is set
 * and (with the gate on) the PIT's channel 2 is high -- a square wave at its
 * pitch, or whatever the program makes by flicking the data bit itself (the
 * way some games talk). The changes come with their times (hw.c), so even
 * the flicking comes out right. A DC blocker keeps a cone held out silent.
 */
#define SPK_AMP	7000

static void render_speaker(struct dosbox *b)
{
	struct spk *s = &b->spk;
	uint64_t now = dos_now(b);
	if (!s->last_us || now < s->last_us || now - s->last_us > 200000) {
		s->last_us = now;	/* the start, or held up: from now */
		s->nev = 0;
		s->bits = b->port61 & 3;
		s->hz = b->pit[2].reload > 16 ? (uint32_t)(PIT_HZ / b->pit[2].reload) : 0;
		return;
	}
	if (!s->nev && !(s->bits & 2) && s->y1 > -64 && s->y1 < 64) {
		s->last_us = now;	/* silent, and nothing's changed: nothing to draw */
		s->y1 = s->x1 = 0;
		return;
	}
	uint32_t n = (uint32_t)((now - s->last_us) * AUDIO_RATE / 1000000);
	if (!n)
		return;
	if (!s->stream) {
		int id = audio_open_named(AUDIO_RATE, "PC speaker");
		if (id < 0) {
			s->nev = 0;
			s->last_us = now;
			return;
		}
		s->stream = id + 1;
	}
	int16_t buf[512];
	uint32_t k = 0, e = 0;
	uint64_t t0 = s->last_us;
	uint32_t step = s->hz ? (uint32_t)(((uint64_t)s->hz << 32) / AUDIO_RATE) : 0;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t t = t0 + (uint64_t)i * 1000000 / AUDIO_RATE;
		while (e < (uint32_t)s->nev && s->ev[e].t <= t) {
			s->bits = s->ev[e].bits;
			if (s->ev[e].hz != s->hz) {
				s->hz = s->ev[e].hz;
				step = s->hz ? (uint32_t)(((uint64_t)s->hz << 32) / AUDIO_RATE) : 0;
			}
			e++;
		}
		int out;
		if (!(s->bits & 2))
			out = 0;
		else if ((s->bits & 1) && s->hz >= 20 && s->hz <= 20000) {
			s->phase += step;
			out = (s->phase & 0x80000000u) ? SPK_AMP : 0;
		} else {
			out = SPK_AMP;
		}
		int32_t y = out - s->x1 + ((s->y1 * 32604) >> 15);
		s->x1 = out;
		s->y1 = y;
		buf[k++] = (int16_t)MAX(-32768, MIN(32767, y));
		if (k == NELEM(buf)) {
			audio_write(s->stream - 1, buf, k, 1);
			k = 0;
		}
	}
	if (k)
		audio_write(s->stream - 1, buf, k, 1);
	/* changes still to come stay for next time */
	if (e)
		memmove(s->ev, s->ev + e, sizeof s->ev[0] * (size_t)(s->nev - (int)e));
	s->nev -= (int)e;
	s->last_us = t0 + (uint64_t)n * 1000000 / AUDIO_RATE;
}

void sb_poll(struct dosbox *b, uint64_t now)
{
	if (b->sb.dma_on || b->sb.silence_left)
		render_dma(b, now);
	else
		render_direct(b, now);
	render_opl(b, now);
	if (b->spk.mixer)
		render_speaker(b);
}

void sb_free(struct dosbox *b)
{
	if (b->sb.stream)
		audio_close(b->sb.stream - 1);
	if (b->opl.stream)
		audio_close(b->opl.stream - 1);
	if (b->spk.stream)
		audio_close(b->spk.stream - 1);
	b->sb.stream = b->opl.stream = b->spk.stream = 0;
}
