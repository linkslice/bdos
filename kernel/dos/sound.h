/*
 * sound.h - the sound hardware a DOS box has: a Sound Blaster 16 (DSP,
 * mixer), the 8237 DMA controllers that feed it, and an OPL2 (AdLib) FM
 * synthesizer. All of it lives in struct dosbox, so saved states keep it.
 */
#ifndef BDOS_DOS_SOUND_H
#define BDOS_DOS_SOUND_H

#include <stdint.h>

#define SB_IRQ		7
#define SB_DMA8		1
#define SB_DMA16	5

struct dma_ch {
	uint16_t base_addr, cur_addr;	/* words for channels 4-7 */
	uint16_t base_count, cur_count;	/* transfers - 1 */
	uint8_t page;
	uint8_t mode;
	uint8_t masked;
	uint8_t tc;			/* reached terminal count */
};

struct sb {
	/* the DSP */
	int resetting;
	uint8_t outq[64];
	int out_head, out_len;
	uint8_t cmd;
	int need, got;
	uint8_t args[4];
	uint32_t rate;			/* samples per second, per channel */
	int dma_on, dma16, autoinit, stereo, is_signed, paused, exit_auto;
	uint32_t block_len;		/* samples per block (a stereo pair counts 2) */
	uint32_t block_left;
	uint16_t block8;		/* 48h's block size, bytes */
	uint8_t irq_pending;		/* bit 0: 8-bit, bit 1: 16-bit */
	int speaker;
	uint8_t test_reg;
	int silence_left;		/* 80h: samples of nothing, then an IRQ */
	uint8_t direct;			/* 10h: the DAC's level */
	uint64_t direct_until;		/* direct output plays till then */
	uint8_t mixer_idx;
	uint8_t mixer[256];
	uint64_t last_us;
	uint64_t carry;			/* fractional samples */
	int stream;			/* audio stream + 1 */
	uint32_t stream_rate;
};

struct opl_op {
	uint32_t phase, inc;
	int state;			/* 0 off, 1 attack, 2 decay, 3 sustain, 4 release */
	uint32_t env;			/* attenuation, 0..511, 16.16 */
	int32_t out, prev;
	uint16_t att_static;		/* TL and KSL, in 0.1875 dB steps */
	uint8_t eff_ar, eff_dr, eff_rr;
	uint16_t sl;
	uint8_t wave;
};

struct opl {
	uint8_t reg[256];
	uint8_t idx;
	struct opl_op op[18];
	uint8_t status;
	uint64_t t1_start, t2_start;
	int t1_on, t2_on;
	int reads;			/* status reads since the timers were started */
	uint32_t lfo;			/* tremolo and vibrato phase */
	uint32_t noise;
	uint64_t last_us, carry;
	uint64_t quiet_since;
	int stream;			/* audio stream + 1 */
};

struct dosbox;
void sb_reset(struct dosbox *b);
void sb_poll(struct dosbox *b, uint64_t now_us);
void sb_free(struct dosbox *b);
uint32_t sb_in(struct dosbox *b, uint16_t port);
void sb_out(struct dosbox *b, uint16_t port, uint8_t v);
uint32_t dma_in(struct dosbox *b, uint16_t port);
void dma_out(struct dosbox *b, uint16_t port, uint8_t v);
uint32_t opl_in(struct dosbox *b, uint16_t port);
void opl_out(struct dosbox *b, uint16_t port, uint8_t v);

#endif
