/*
 * hda.c - Intel High Definition Audio: the sound in every PC since 2004,
 * and QEMU's intel-hda.
 *
 * Bring the controller out of reset, talk to the codec over the CORB and
 * RIRB rings, find a path from a DAC to an output pin (line out, speaker
 * or headphones), and play one output stream: 48 kHz, 16-bit stereo, from
 * a ring of four buffers that a kernel task keeps topped up from the mixer.
 */
#include "../kernel.h"
#include "../audio.h"
#include "drv.h"

#define GCAP		0x00
#define GCTL		0x08
#define STATESTS	0x0E
#define INTCTL		0x20
#define CORBLBASE	0x40
#define CORBUBASE	0x44
#define CORBWP		0x48
#define CORBRP		0x4A
#define CORBCTL		0x4C
#define CORBSIZE	0x4E
#define RIRBLBASE	0x50
#define RIRBUBASE	0x54
#define RIRBWP		0x58
#define RINTCNT		0x5A
#define RIRBCTL		0x5C
#define RIRBSTS		0x5D
#define RIRBSIZE	0x5E

/* stream descriptor registers, from its base */
#define SD_CTL		0x00
#define SD_STS		0x03
#define SD_LPIB		0x04
#define SD_CBL		0x08
#define SD_LVI		0x0C
#define SD_FMT		0x12
#define SD_BDPL		0x18
#define SD_BDPU		0x1C

#define NBUF		4
#define BUF_FRAMES	1024		/* 21 ms each */
#define BUF_BYTES	(BUF_FRAMES * 4)

static volatile uint8_t *mmio;
static uint32_t *corb;
static uint64_t *rirb;
static int corb_wp, rirb_rp, codec;
static volatile uint8_t *sd;	/* the output stream descriptor */
static uint8_t *dma;		/* NBUF buffers */
static uint64_t *bdl;
static int dac, pin;

static uint16_t r16(int o) { return *(volatile uint16_t *)(mmio + o); }
static uint32_t r32(int o) { return *(volatile uint32_t *)(mmio + o); }
static void w8(int o, uint8_t v) { mmio[o] = v; }
static void w16(int o, uint16_t v) { *(volatile uint16_t *)(mmio + o) = v; }
static void w32(int o, uint32_t v) { *(volatile uint32_t *)(mmio + o) = v; }

static int wait_for(int (*cond)(void), int ms)
{
	for (int i = 0; i < ms * 10; i++) {
		if (cond())
			return 0;
		delay_us(100);
	}
	return -1;
}

static int in_reset(void) { return !(r32(GCTL) & 1); }
static int out_of_reset(void) { return (r32(GCTL) & 1) != 0; }
static int rirb_has(void) { return (r16(RIRBWP) & 0xFF) != rirb_rp; }

/* a verb to the codec, and its answer */
static int verb(int nid, uint32_t v, uint32_t *resp)
{
	uint32_t cmd = (uint32_t)codec << 28 | (uint32_t)nid << 20 | (v & 0xFFFFF);
	corb_wp = (corb_wp + 1) % 256;
	corb[corb_wp] = cmd;
	mmio_barrier();
	w16(CORBWP, (uint16_t)corb_wp);
	if (wait_for(rirb_has, 50))
		return -1;
	rirb_rp = (rirb_rp + 1) % 256;
	w8(RIRBSTS, 0x05);	/* acknowledge, or some controllers stop answering */
	if (resp)
		*resp = (uint32_t)rirb[rirb_rp];
	return 0;
}

static uint32_t param(int nid, int p)
{
	uint32_t r = 0;
	verb(nid, 0xF0000 | (uint32_t)p, &r);
	return r;
}

/* verbs with a 12-bit id and 8-bit payload, or a 4-bit id and 16-bit payload */
static void set12(int nid, int id, int payload) { verb(nid, (uint32_t)id << 8 | (uint32_t)(payload & 0xFF), NULL); }
static void set4(int nid, int id, int payload) { verb(nid, (uint32_t)id << 16 | (uint32_t)(payload & 0xFFFF), NULL); }

static int widget_type(int nid) { return (int)(param(nid, 0x09) >> 20) & 0xF; }

/* an amplifier at its 0 dB step, unmuted (out: the output amp, else input amp idx) */
static void amp_on(int nid, int out, int idx)
{
	uint32_t caps = param(nid, out ? 0x12 : 0x0D);
	int gain = (int)(caps & 0x7F);
	if (!caps)
		gain = 0x7F;		/* no amp caps here: the widget's own are the group's */
	set4(nid, 0x3, (out ? 0xB000 : 0x7000) | idx << 8 | gain);
}

static int conn_list(int nid, int *out, int max)
{
	uint32_t len = param(nid, 0x0E);
	int n = (int)(len & 0x7F), lng = (len >> 7) & 1, got = 0;
	for (int i = 0; i < n && got < max; i += lng ? 2 : 4) {
		uint32_t r = 0;
		verb(nid, 0xF0200 | (uint32_t)i, &r);
		for (int k = 0; k < (lng ? 2 : 4) && i + k < n && got < max; k++)
			out[got++] = lng ? (int)((r >> (16 * k)) & 0x7FFF) : (int)((r >> (8 * k)) & 0x7F);
	}
	return got;
}

/* from a widget towards a DAC: unmute and power everything on the way */
static int path_to_dac(int nid, int depth)
{
	int type = widget_type(nid);
	if (type == 0)
		return nid;
	if (depth > 5)
		return 0;
	int conns[16];
	int n = conn_list(nid, conns, 16);
	for (int i = 0; i < n; i++) {
		int d = path_to_dac(conns[i], depth + 1);
		if (d) {
			set12(nid, 0x701, i);		/* select this input */
			set12(nid, 0x705, 0);		/* power D0 */
			/* unmute: input amp i, both sides, and the output amp */
			amp_on(nid, 0, i);
			amp_on(nid, 1, 0);
			return d;
		}
	}
	return 0;
}

static void find_output(int afg, int first, int count)
{
	/* the pins that can drive something: line out, speaker, headphones in that order */
	static const int prefer[] = { 0x0, 0x1, 0x2 };
	for (int pass = 0; pass < 4 && !dac; pass++)
		for (int nid = first; nid < first + count && !dac; nid++) {
			if (widget_type(nid) != 4)
				continue;
			if (!(param(nid, 0x0C) & 0x10))	/* output capable */
				continue;
			uint32_t cfg = 0;
			verb(nid, 0xF1C00, &cfg);
			int conn = (int)(cfg >> 30), dev = (int)(cfg >> 20) & 0xF;
			if (pass < 3 ? (conn == 1 || dev != prefer[pass]) : 0)
				continue;
			int d = path_to_dac(nid, 0);
			if (!d)
				continue;
			dac = d;
			pin = nid;
			set12(nid, 0x705, 0);
			set12(nid, 0x707, 0xC0);		/* out enable, headphone amp */
			set12(nid, 0x70C, 0x02);		/* EAPD on */
			amp_on(nid, 1, 0);
		}
	(void)afg;
}

static void fill(int k)
{
	audio_mix((int16_t *)(dma + k * BUF_BYTES), BUF_FRAMES);
}

static void hda_task(void *arg)
{
	(void)arg;
	int next = 2;		/* 0 and 1 are filled before it starts */
	for (;;) {
		uint32_t pos = *(volatile uint32_t *)(sd + SD_LPIB);
		int playing = (int)(pos / BUF_BYTES) % NBUF;
		/* keep two buffers ahead of the one playing */
		while (next != (playing + 3) % NBUF && next != playing) {
			fill(next);
			next = (next + 1) % NBUF;
		}
		sleep_ms(5);
	}
}

int hda_init(void)
{
	struct pci_dev *d = pci_find_class(0x04, 0x03, -1, 0);
	if (!d)
		return 0;
	pci_enable(d);
	mmio = pci_map_bar(d, 0, 0x4000);
	if (!mmio)
		return 0;
	/* reset */
	w32(GCTL, r32(GCTL) & ~1u);
	if (wait_for(in_reset, 100))
		return 0;
	delay_us(200);
	w32(GCTL, r32(GCTL) | 1);
	if (wait_for(out_of_reset, 100))
		return 0;
	delay_us(1000);		/* codecs need 521 us to show up */
	uint16_t codecs = r16(STATESTS);
	if (!codecs)
		return 0;
	codec = __builtin_ctz(codecs);
	w32(INTCTL, 0);
	/* the command rings: 256 entries each */
	uint8_t *mem = pmm_alloc(1 + (NBUF * BUF_BYTES + 4095) / 4096 + 1);
	if (!mem)
		return 0;
	corb = (uint32_t *)mem;			/* 1 KB */
	rirb = (uint64_t *)(mem + 1024);	/* 2 KB */
	bdl = (uint64_t *)(mem + 3072);		/* 128-byte aligned */
	dma = mem + 4096;
	w8(CORBCTL, 0);
	w8(RIRBCTL, 0);
	delay_us(100);
	w32(CORBLBASE, (uint32_t)(uint64_t)corb);
	w32(CORBUBASE, (uint32_t)((uint64_t)corb >> 32));
	w8(CORBSIZE, 2);
	w16(CORBRP, 0x8000);
	delay_us(100);
	w16(CORBRP, 0);
	w16(CORBWP, 0);
	corb_wp = 0;
	w32(RIRBLBASE, (uint32_t)(uint64_t)rirb);
	w32(RIRBUBASE, (uint32_t)((uint64_t)rirb >> 32));
	w8(RIRBSIZE, 2);
	w16(RIRBWP, 0x8000);
	w16(RINTCNT, 0xFF);
	rirb_rp = 0;
	w8(CORBCTL, 2);
	w8(RIRBCTL, 2);
	delay_us(100);
	/* the audio function group, then its widgets */
	uint32_t sub = param(0, 0x04);
	int first = (int)(sub >> 16) & 0xFF, count = (int)sub & 0xFF, afg = 0;
	for (int n = first; n < first + count; n++)
		if ((param(n, 0x05) & 0xFF) == 1)
			afg = n;
	if (!afg) {
		kprintf("Sound: HD Audio codec %d has no audio function\n", codec);
		return 0;
	}
	set12(afg, 0x705, 0);
	sub = param(afg, 0x04);
	find_output(afg, (int)(sub >> 16) & 0xFF, (int)sub & 0xFF);
	if (!dac) {
		kprintf("Sound: HD Audio codec %d: no way out to a speaker found\n", codec);
		return 0;
	}
	/* the first output stream */
	int iss = (r16(GCAP) >> 8) & 0xF;
	sd = mmio + 0x80 + iss * 0x20;
	*(volatile uint8_t *)(sd + SD_CTL) = 0;
	*(volatile uint8_t *)(sd + SD_CTL) = 1;		/* stream reset */
	delay_us(100);
	*(volatile uint8_t *)(sd + SD_CTL) = 0;
	delay_us(100);
	for (int k = 0; k < NBUF; k++) {
		uint64_t a = (uint64_t)(dma + k * BUF_BYTES);
		bdl[k * 2] = a;
		bdl[k * 2 + 1] = BUF_BYTES;	/* length; no interrupt (we poll) */
	}
	*(volatile uint32_t *)(sd + SD_BDPL) = (uint32_t)(uint64_t)bdl;
	*(volatile uint32_t *)(sd + SD_BDPU) = (uint32_t)((uint64_t)bdl >> 32);
	*(volatile uint32_t *)(sd + SD_CBL) = NBUF * BUF_BYTES;
	*(volatile uint16_t *)(sd + SD_LVI) = NBUF - 1;
	*(volatile uint16_t *)(sd + SD_FMT) = 0x0011;	/* 48 kHz, 16-bit, stereo */
	/* the DAC plays stream 1 */
	set4(dac, 0x2, 0x0011);
	set12(dac, 0x706, 0x10);
	set12(dac, 0x705, 0);
	amp_on(dac, 1, 0);
	fill(0);
	fill(1);
	memset(dma + 2 * BUF_BYTES, 0, 2 * BUF_BYTES);
	*(volatile uint8_t *)(sd + SD_CTL + 2) = 0x10;	/* stream tag 1 */
	*(volatile uint8_t *)(sd + SD_CTL) = 0x02;		/* run */
	task_spawn("hda", hda_task, NULL, NULL);
	kprintf("Sound: HD Audio (codec %d, DAC %d, pin %d)\n", codec, dac, pin);
	return 1;
}
