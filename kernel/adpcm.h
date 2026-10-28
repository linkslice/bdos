/*
 * adpcm.h - IMA ADPCM, 4 bits a sample, for sound over the network.
 *
 * A block is stereo: for each channel its first sample as it is and the
 * step index, then a byte per frame after the first (left in the low
 * nibble, right in the high). Every block decodes on its own, so one that's
 * lost costs only itself. bterm has the same code.
 */
#ifndef BDOS_ADPCM_H
#define BDOS_ADPCM_H
#include <stdint.h>
#include <stddef.h>

struct adpcm_ch { int pred, index; };

#define ADPCM_HDR	8	/* two of: i16 first sample, u8 index, u8 0 */
static inline size_t adpcm_block_bytes(uint32_t frames) { return ADPCM_HDR + (frames ? frames - 1 : 0); }

/* frames of interleaved stereo in, a block out; st carries the step size
 * from block to block (for quality: each block still stands alone) */
size_t adpcm_encode(struct adpcm_ch st[2], const int16_t *pcm, uint32_t frames, uint8_t *out);
/* a block in, frames of interleaved stereo out; returns the frames, 0 if bad */
uint32_t adpcm_decode(const uint8_t *in, size_t len, int16_t *pcm, uint32_t max_frames);

#endif
