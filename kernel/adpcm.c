/* adpcm.c - IMA ADPCM blocks (see adpcm.h) */
#include "adpcm.h"

static const int16_t steps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
	73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
	449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
	9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767,
};
static const int8_t index_adj[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* one nibble's worth: the decoder's arithmetic, which the encoder follows too */
static void step(struct adpcm_ch *c, int code)
{
	int st = steps[c->index];
	int diff = st >> 3;
	if (code & 4) diff += st;
	if (code & 2) diff += st >> 1;
	if (code & 1) diff += st >> 2;
	c->pred = clamp16(code & 8 ? c->pred - diff : c->pred + diff);
	c->index += index_adj[code];
	c->index = c->index < 0 ? 0 : c->index > 88 ? 88 : c->index;
}

static int encode_one(struct adpcm_ch *c, int sample)
{
	int st = steps[c->index], diff = sample - c->pred, code = 0;
	if (diff < 0) {
		code = 8;
		diff = -diff;
	}
	if (diff >= st) { code |= 4; diff -= st; }
	if (diff >= st >> 1) { code |= 2; diff -= st >> 1; }
	if (diff >= st >> 2) code |= 1;
	step(c, code);
	return code;
}

size_t adpcm_encode(struct adpcm_ch st[2], const int16_t *pcm, uint32_t frames, uint8_t *out)
{
	if (!frames)
		return 0;
	for (int ch = 0; ch < 2; ch++) {
		st[ch].pred = pcm[ch];
		int16_t first = (int16_t)st[ch].pred;
		out[ch * 4] = (uint8_t)first;
		out[ch * 4 + 1] = (uint8_t)((uint16_t)first >> 8);
		out[ch * 4 + 2] = (uint8_t)st[ch].index;
		out[ch * 4 + 3] = 0;
	}
	uint8_t *p = out + ADPCM_HDR;
	for (uint32_t i = 1; i < frames; i++) {
		int l = encode_one(&st[0], pcm[i * 2]);
		int r = encode_one(&st[1], pcm[i * 2 + 1]);
		*p++ = (uint8_t)(l | r << 4);
	}
	return (size_t)(p - out);
}

uint32_t adpcm_decode(const uint8_t *in, size_t len, int16_t *pcm, uint32_t max_frames)
{
	if (len < ADPCM_HDR || !max_frames)
		return 0;
	struct adpcm_ch c[2];
	for (int ch = 0; ch < 2; ch++) {
		c[ch].pred = (int16_t)(in[ch * 4] | in[ch * 4 + 1] << 8);
		c[ch].index = in[ch * 4 + 2] > 88 ? 88 : in[ch * 4 + 2];
		pcm[ch] = (int16_t)c[ch].pred;
	}
	uint32_t frames = 1 + (uint32_t)(len - ADPCM_HDR);
	if (frames > max_frames)
		frames = max_frames;
	for (uint32_t i = 1; i < frames; i++) {
		uint8_t b = in[ADPCM_HDR + i - 1];
		step(&c[0], b & 15);
		step(&c[1], b >> 4);
		pcm[i * 2] = (int16_t)c[0].pred;
		pcm[i * 2 + 1] = (int16_t)c[1].pred;
	}
	return frames;
}
