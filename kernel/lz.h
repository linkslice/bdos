/* lz.h - LZ4-block-format compression for screen updates */
#ifndef BDOS_LZ_H
#define BDOS_LZ_H
#include <stddef.h>
#include <stdint.h>
#define LZ_TABLE_WORDS (1 << 13)	/* the compressor's scratch: this many uint32_t */
size_t lz_bound(size_t n);
/* 0 if out is too small; table is LZ_TABLE_WORDS words of scratch */
size_t lz_compress(const uint8_t *in, size_t n, uint8_t *out, size_t cap, uint32_t *table);
/* bytes produced, or -1 if it's not a valid block */
long lz_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
#endif
