/*
 * lz.c - a small LZ77 in the LZ4 block format, for screen updates over the
 * network: rows of a window repeat each other, so they squash well, and
 * both ends are quick about it.
 *
 * A sequence is a token (literal count << 4 | match length - 4), longer
 * counts in following bytes (255, 255, ..., rest), the literals, then a
 * two-byte little-endian offset back into what's been produced. The block
 * ends after a sequence's literals.
 */
#include "kernel.h"
#include "lz.h"

#define HBITS 13

static uint32_t rd32(const uint8_t *p)
{
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static uint8_t *put_len(uint8_t *op, size_t n)
{
	while (n >= 255) {
		*op++ = 255;
		n -= 255;
	}
	*op++ = (uint8_t)n;
	return op;
}

size_t lz_bound(size_t n)
{
	return n + n / 255 + 16;
}

size_t lz_compress(const uint8_t *in, size_t n, uint8_t *out, size_t cap, uint32_t *table)
{
	if (cap < lz_bound(n))
		return 0;
	memset(table, 0, sizeof(uint32_t) << HBITS);
	uint8_t *op = out;
	size_t i = 0, anchor = 0;
	while (n >= 8 && i + 8 <= n) {
		uint32_t v = rd32(in + i), h = (v * 2654435761u) >> (32 - HBITS);
		uint32_t ref = table[h];
		table[h] = (uint32_t)i + 1;
		if (ref && i - (ref - 1) <= 65535 && rd32(in + ref - 1) == v) {
			size_t m = ref - 1, len = 4;
			while (i + len < n && in[m + len] == in[i + len])
				len++;
			size_t lit = i - anchor;
			uint8_t *tok = op++;
			*tok = (uint8_t)((lit >= 15 ? 15 : lit) << 4 | (len - 4 >= 15 ? 15 : len - 4));
			if (lit >= 15)
				op = put_len(op, lit - 15);
			memcpy(op, in + anchor, lit);
			op += lit;
			size_t off = i - m;
			*op++ = (uint8_t)off;
			*op++ = (uint8_t)(off >> 8);
			if (len - 4 >= 15)
				op = put_len(op, len - 4 - 15);
			i += len;
			anchor = i;
		} else {
			i++;
		}
	}
	size_t lit = n - anchor;
	*op++ = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
	if (lit >= 15)
		op = put_len(op, lit - 15);
	memcpy(op, in + anchor, lit);
	op += lit;
	return (size_t)(op - out);
}

long lz_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	const uint8_t *ip = in, *end = in + n;
	uint8_t *op = out, *oend = out + cap;
	while (ip < end) {
		unsigned tok = *ip++;
		size_t lit = tok >> 4;
		if (lit == 15) {
			unsigned b;
			do {
				if (ip >= end)
					return -1;
				b = *ip++;
				lit += b;
			} while (b == 255);
		}
		if ((size_t)(end - ip) < lit || (size_t)(oend - op) < lit)
			return -1;
		memcpy(op, ip, lit);
		op += lit;
		ip += lit;
		if (ip >= end)
			break;
		if (end - ip < 2)
			return -1;
		size_t off = (size_t)ip[0] | (size_t)ip[1] << 8;
		ip += 2;
		size_t len = tok & 15;
		if (len == 15) {
			unsigned b;
			do {
				if (ip >= end)
					return -1;
				b = *ip++;
				len += b;
			} while (b == 255);
		}
		len += 4;
		if (!off || off > (size_t)(op - out) || (size_t)(oend - op) < len)
			return -1;
		const uint8_t *src = op - off;
		while (len--)
			*op++ = *src++;
	}
	return (long)(op - out);
}
