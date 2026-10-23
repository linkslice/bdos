/*
 * num.c - numbers to and from text, exactly: double to its exact decimal
 * digits for printf, correctly rounded strtod, and the integer parsers.
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include "libc.h"

/* ---- big unsigned integers, base 2^32, little-endian ---- */

#define BIG_LIMBS 130		/* 4160 bits: 10^1100 and 2^1100 with room */

typedef struct {
	int n;
	uint32_t d[BIG_LIMBS];
} big;

static void big_set(big *a, uint64_t v)
{
	a->n = 0;
	while (v) {
		a->d[a->n++] = (uint32_t)v;
		v >>= 32;
	}
}

static void big_muladd(big *a, uint32_t m, uint32_t add)
{
	uint64_t c = add;
	for (int i = 0; i < a->n; i++) {
		uint64_t t = (uint64_t)a->d[i] * m + c;
		a->d[i] = (uint32_t)t;
		c = t >> 32;
	}
	if (c && a->n < BIG_LIMBS)
		a->d[a->n++] = (uint32_t)c;
}

static uint32_t big_divsmall(big *a, uint32_t m)	/* -> remainder */
{
	uint64_t r = 0;
	for (int i = a->n - 1; i >= 0; i--) {
		uint64_t t = (r << 32) | a->d[i];
		a->d[i] = (uint32_t)(t / m);
		r = t % m;
	}
	while (a->n && !a->d[a->n - 1])
		a->n--;
	return (uint32_t)r;
}

static void big_shl(big *a, int s)
{
	if (!a->n)
		return;
	int w = s / 32, b = s % 32;
	int n = a->n + w + 1;
	if (n > BIG_LIMBS)
		n = BIG_LIMBS;
	for (int i = n - 1; i >= 0; i--) {
		uint64_t hi = (i - w >= 0 && i - w < a->n) ? a->d[i - w] : 0;
		uint64_t lo = (b && i - w - 1 >= 0 && i - w - 1 < a->n) ? a->d[i - w - 1] : 0;
		a->d[i] = (uint32_t)((hi << b) | (b ? lo >> (32 - b) : 0));
	}
	a->n = n;
	while (a->n && !a->d[a->n - 1])
		a->n--;
}

static int big_bits(const big *a)
{
	if (!a->n)
		return 0;
	return (a->n - 1) * 32 + (32 - __builtin_clz(a->d[a->n - 1]));
}

static int big_bit(const big *a, int i)
{
	return i / 32 < a->n ? (a->d[i / 32] >> (i % 32)) & 1 : 0;
}

static int big_cmp(const big *a, const big *b)
{
	if (a->n != b->n)
		return a->n < b->n ? -1 : 1;
	for (int i = a->n - 1; i >= 0; i--)
		if (a->d[i] != b->d[i])
			return a->d[i] < b->d[i] ? -1 : 1;
	return 0;
}

static void big_sub(big *a, const big *b)	/* a -= b, a >= b */
{
	int64_t c = 0;
	for (int i = 0; i < a->n; i++) {
		int64_t t = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) + c;
		a->d[i] = (uint32_t)t;
		c = t < 0 ? -1 : 0;
	}
	while (a->n && !a->d[a->n - 1])
		a->n--;
}

static void big_shl1_or(big *a, int bit)
{
	uint32_t c = (uint32_t)bit;
	for (int i = 0; i < a->n; i++) {
		uint32_t t = a->d[i];
		a->d[i] = (t << 1) | c;
		c = t >> 31;
	}
	if (c && a->n < BIG_LIMBS)
		a->d[a->n++] = c;
}

/* ---- double -> exact decimal ---- */

int __dtoa_exact(double v, char *digits, int cap, int *exp10)
{
	union { double d; uint64_t u; } u = { v };
	uint64_t mant = u.u & ((1ull << 52) - 1);
	int e = (int)((u.u >> 52) & 0x7FF);
	if (!e && !mant) {
		digits[0] = '0';
		digits[1] = 0;
		*exp10 = 1;
		return 1;
	}
	if (e)
		mant |= 1ull << 52;
	else
		e = 1;
	e -= 1075;		/* value = mant * 2^e */
	big local;
	big *b = &local;
	big_set(b, mant);
	int dexp = 0;
	if (e >= 0) {
		big_shl(b, e);
	} else {
		/* mant / 2^-e = mant * 5^-e / 10^-e */
		for (int i = 0; i < -e; i++)
			big_muladd(b, 5, 0);
		dexp = e;
	}
	/* b's decimal digits, from the bottom */
	char tmp[1200];
	int n = 0;
	while (b->n) {
		uint32_t r = big_divsmall(b, 1000000000u);
		for (int i = 0; i < 9; i++) {
			tmp[n++] = (char)('0' + r % 10);
			r /= 10;
		}
	}
	while (n > 1 && tmp[n - 1] == '0')
		n--;
	int len = n;
	/* trailing zeros don't matter; strip them */
	int lo = 0;
	while (lo < len - 1 && tmp[lo] == '0')
		lo++;
	int out = 0;
	for (int i = len - 1; i >= lo && out < cap - 1; i--)
		digits[out++] = tmp[i];
	digits[out] = 0;
	*exp10 = len + dexp;
	return out;
}

/* ---- text -> double, correctly rounded ---- */

/* (q, and anything nonzero below it if sticky) * 2^e2 to the nearest double */
static double make_double(uint64_t q, int e2, int sticky)
{
	if (!q)
		return 0.0;
	int lz = __builtin_clzll(q);
	q <<= lz;
	e2 -= lz;
	int top = e2 + 63;		/* the exponent of q's top bit */
	if (top > 1023)
		return HUGE_VAL;
	int keep = top < -1022 ? top + 1075 : 53;
	const uint64_t half = 1ull << 63;
	if (keep < 0)
		return 0.0;
	if (keep == 0)
		return (q > half || (q == half && sticky)) ? ldexp(1.0, -1074) : 0.0;
	int drop = 64 - keep;
	uint64_t m = q >> drop;
	uint64_t rest = q << (64 - drop);
	if (rest > half || (rest == half && (sticky || (m & 1))))
		m++;
	if (keep < 53)
		return ldexp((double)m, -1074);
	int e = top - 52;
	if (m >> 53) {
		m >>= 1;
		e++;
	}
	if (e + 52 > 1023)
		return HUGE_VAL;
	return ldexp((double)m, e);
}

#define MAX_DIGITS 800

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

double __strtod(const char *s, char **end)
{
	const char *p = s;
	while (isspace((unsigned char)*p))
		p++;
	int neg = 0;
	if (*p == '+' || *p == '-')
		neg = *p++ == '-';
	if (!strncasecmp(p, "inf", 3)) {
		p += 3;
		if (!strncasecmp(p, "inity", 5))
			p += 5;
		if (end)
			*end = (char *)p;
		return neg ? -HUGE_VAL : HUGE_VAL;
	}
	if (!strncasecmp(p, "nan", 3)) {
		p += 3;
		if (*p == '(') {
			const char *q = p + 1;
			while (isalnum((unsigned char)*q) || *q == '_')
				q++;
			if (*q == ')')
				p = q + 1;
		}
		if (end)
			*end = (char *)p;
		return neg ? -NAN : NAN;
	}
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
	    (hexval((unsigned char)p[2]) >= 0 || (p[2] == '.' && hexval((unsigned char)p[3]) >= 0))) {
		p += 2;
		uint64_t m = 0;
		int e2 = 0, sticky = 0, any = 0, dot = 0;
		for (;; p++) {
			if (*p == '.' && !dot) {
				dot = 1;
				continue;
			}
			int h = hexval((unsigned char)*p);
			if (h < 0)
				break;
			any = 1;
			if (m >> 59) {
				sticky |= h != 0;
				if (!dot)
					e2 += 4;
			} else {
				m = m << 4 | (uint64_t)h;
				if (dot)
					e2 -= 4;
			}
		}
		(void)any;
		if (*p == 'p' || *p == 'P') {
			const char *q = p + 1;
			int en = 0;
			if (*q == '+' || *q == '-')
				en = *q++ == '-';
			if (isdigit((unsigned char)*q)) {
				long ev = 0;
				while (isdigit((unsigned char)*q)) {
					if (ev < 100000)
						ev = ev * 10 + (*q - '0');
					q++;
				}
				e2 += (int)(en ? -ev : ev);
				p = q;
			}
		}
		if (end)
			*end = (char *)p;
		double r = make_double(m, e2, sticky);
		if (r == HUGE_VAL || (r == 0 && m))
			errno = ERANGE;
		return neg ? -r : r;
	}
	/* decimal */
	char dig[MAX_DIGITS + 2];
	int nd = 0, any = 0, dot = 0;
	long dexp = 0;
	int truncated = 0;
	for (;; p++) {
		if (*p == '.' && !dot) {
			dot = 1;
			continue;
		}
		if (!isdigit((unsigned char)*p))
			break;
		any = 1;
		if (!nd && *p == '0') {
			if (dot)
				dexp--;
			continue;
		}
		if (nd < MAX_DIGITS) {
			dig[nd++] = *p;
			if (dot)
				dexp--;
		} else {
			truncated |= *p != '0';
			if (!dot)
				dexp++;
		}
	}
	if (!any) {
		if (end)
			*end = (char *)s;
		return 0.0;
	}
	if (*p == 'e' || *p == 'E') {
		const char *q = p + 1;
		int en = 0;
		if (*q == '+' || *q == '-')
			en = *q++ == '-';
		if (isdigit((unsigned char)*q)) {
			long ev = 0;
			while (isdigit((unsigned char)*q)) {
				if (ev < 100000)
					ev = ev * 10 + (*q - '0');
				q++;
			}
			dexp += en ? -ev : ev;
			p = q;
		}
	}
	if (end)
		*end = (char *)p;
	if (truncated)
		dig[nd++] = '1', dexp--;
	if (!nd)
		return neg ? -0.0 : 0.0;
	/* value = DIG * 10^dexp */
	if (nd + dexp > 310) {
		errno = ERANGE;
		return neg ? -HUGE_VAL : HUGE_VAL;
	}
	if (nd + dexp < -330) {
		errno = ERANGE;
		return neg ? -0.0 : 0.0;
	}
	big n;
	big_set(&n, 0);
	for (int i = 0; i < nd; i++) {
		if (!n.n)
			big_set(&n, (uint64_t)(dig[i] - '0'));
		else
			big_muladd(&n, 10, (uint32_t)(dig[i] - '0'));
	}
	double r;
	if (dexp >= 0) {
		for (long i = 0; i < dexp; i++)
			big_muladd(&n, 10, 0);
		int bits = big_bits(&n);
		uint64_t q = 0;
		int sticky = 0, sh = bits > 64 ? bits - 64 : 0;
		for (int i = bits - 1; i >= sh; i--)
			q = q << 1 | (uint64_t)big_bit(&n, i);
		for (int i = 0; i < sh && !sticky; i++)
			sticky = big_bit(&n, i);
		r = make_double(q, sh, sticky);
	} else {
		big dv;
		big_set(&dv, 1);
		for (long i = 0; i < -dexp; i++)
			big_muladd(&dv, 10, 0);
		int s = big_bits(&dv) - big_bits(&n) + 62;
		if (s < 0)
			s = 0;
		/* q = (n << s) / dv by restoring division, one numerator bit at a time */
		int nb = big_bits(&n);
		big rem;
		big_set(&rem, 0);
		uint64_t q = 0;
		for (int i = nb - 1 + s; i >= 0; i--) {
			int bit = i >= s ? big_bit(&n, i - s) : 0;
			if (!rem.n)
				big_set(&rem, (uint64_t)bit);
			else
				big_shl1_or(&rem, bit);
			q <<= 1;
			if (big_cmp(&rem, &dv) >= 0) {
				big_sub(&rem, &dv);
				q |= 1;
			}
		}
		r = make_double(q, -s, rem.n != 0);
	}
	if (r == HUGE_VAL || r == 0)
		errno = ERANGE;
	return neg ? -r : r;
}

double strtod(const char *s, char **end) { return __strtod(s, end); }
float strtof(const char *s, char **end) { return (float)__strtod(s, end); }
long double strtold(const char *s, char **end) { return __strtod(s, end); }
double atof(const char *s) { return __strtod(s, NULL); }

/* ---- integers ---- */

static unsigned long long parse_u(const char *s, char **end, int base, int *neg, int *overflow)
{
	const char *p = s;
	while (isspace((unsigned char)*p))
		p++;
	*neg = 0;
	*overflow = 0;
	if (*p == '+' || *p == '-')
		*neg = *p++ == '-';
	if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && hexval((unsigned char)p[2]) >= 0) {
		p += 2;
		base = 16;
	} else if (base == 0 && p[0] == '0') {
		base = 8;
	} else if (base == 0) {
		base = 10;
	}
	unsigned long long v = 0;
	int any = 0;
	for (;; p++) {
		int d = hexval((unsigned char)*p);
		if (d < 0 && isalpha((unsigned char)*p))
			d = tolower((unsigned char)*p) - 'a' + 10;
		if (d < 0 || d >= base)
			break;
		any = 1;
		if (v > (~0ull - (unsigned)d) / (unsigned)base)
			*overflow = 1;
		else
			v = v * (unsigned)base + (unsigned)d;
	}
	if (end)
		*end = (char *)(any ? p : s);
	return v;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
	int neg, ov;
	unsigned long long v = parse_u(s, end, base, &neg, &ov);
	if (ov) {
		errno = ERANGE;
		return ~0ull;
	}
	return neg ? -v : v;
}

unsigned long strtoul(const char *s, char **end, int base) { return strtoull(s, end, base); }

long long strtoll(const char *s, char **end, int base)
{
	int neg, ov;
	unsigned long long v = parse_u(s, end, base, &neg, &ov);
	if (ov || (!neg && v > (unsigned long long)LLONG_MAX) || (neg && v > (unsigned long long)LLONG_MAX + 1)) {
		errno = ERANGE;
		return neg ? LLONG_MIN : LLONG_MAX;
	}
	return neg ? (long long)-v : (long long)v;
}

long strtol(const char *s, char **end, int base) { return strtoll(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }
