/* math.c - the maths library, on SSE2 and the x87 */
#include <math.h>
#include <stdint.h>
#include <errno.h>

typedef union { double d; uint64_t u; } du;

int __isnan(double x) { du v = { x }; return (v.u & ~(1ull << 63)) > 0x7FF0000000000000ull; }
int __isinf(double x) { du v = { x }; return (v.u & ~(1ull << 63)) == 0x7FF0000000000000ull; }
int __isfinite(double x) { du v = { x }; return (v.u & 0x7FF0000000000000ull) != 0x7FF0000000000000ull; }
int __signbit(double x) { du v = { x }; return (int)(v.u >> 63); }

double fabs(double x) { du v = { x }; v.u &= ~(1ull << 63); return v.d; }
float fabsf(float x) { return (float)fabs(x); }
long double fabsl(long double x) { return x < 0 ? -x : x; }

double sqrt(double x)
{
	double r;
	__asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
	return r;
}

float sqrtf(float x)
{
	float r;
	__asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
	return r;
}

long double sqrtl(long double x)
{
	__asm__("fsqrt" : "+t"(x));
	return x;
}

/* x87 helpers: the value goes through memory as an 80-bit number */
#define X87_1(name, insn)                                       \
	static long double name(long double x)                  \
	{                                                       \
		__asm__(insn : "+t"(x));                        \
		return x;                                       \
	}

X87_1(x_sin, "fsin")
X87_1(x_cos, "fcos")
X87_1(x_rndint, "frndint")

static long double x_reduce(long double x)
{
	/* fsin/fcos want |x| < 2^63; reduce by 2*pi with fprem1 first */
	if (fabsl(x) < 1e18L)
		return x;
	long double twopi = 6.283185307179586476925286766559L;
	__asm__("1: fprem1\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b" : "+t"(x) : "u"(twopi) : "ax");
	return x;
}

double sin(double x)
{
	if (isinf(x) || isnan(x))
		return x - x;
	return (double)x_sin(x_reduce(x));
}

double cos(double x)
{
	if (isinf(x) || isnan(x))
		return x - x;
	return (double)x_cos(x_reduce(x));
}

double tan(double x)
{
	if (isinf(x) || isnan(x))
		return x - x;
	long double v = x_reduce(x), one;
	__asm__("fptan" : "=t"(one), "=u"(v) : "0"(v));
	return (double)v;
}

float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }

static long double x_atan2(long double y, long double x)
{
	long double r;
	__asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
	return r;
}

double atan2(double y, double x) { return (double)x_atan2(y, x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
double atan(double x) { return (double)x_atan2(x, 1.0L); }

double asin(double x)
{
	if (x > 1 || x < -1) {
		errno = EDOM;
		return NAN;
	}
	return (double)x_atan2(x, sqrtl(1.0L - (long double)x * x));
}

double acos(double x)
{
	if (x > 1 || x < -1) {
		errno = EDOM;
		return NAN;
	}
	return (double)x_atan2(sqrtl(1.0L - (long double)x * x), x);
}

/* y * log2(x) */
static long double x_yl2x(long double y, long double x)
{
	long double r;
	__asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
	return r;
}

/* 2^x for any finite x */
static long double x_exp2(long double x)
{
	if (x > 16384)
		return HUGE_VALL;
	if (x < -16500)
		return 0;
	long double i = x_rndint(x), f = x - i, r;
	__asm__("f2xm1" : "=t"(r) : "0"(f));
	r += 1;
	__asm__("fscale" : "=t"(r) : "0"(r), "u"(i));
	return r;
}

double exp2(double x) { if (isnan(x)) return x; return (double)x_exp2(x); }

double exp(double x)
{
	if (isnan(x))
		return x;
	double r = (double)x_exp2((long double)x * 1.4426950408889634073599246810019L);
	if (isinf(r) && !isinf(x))
		errno = ERANGE;
	return r;
}

double log2(double x)
{
	if (x < 0 || isnan(x)) {
		if (x < 0)
			errno = EDOM;
		return NAN;
	}
	if (x == 0) {
		errno = ERANGE;
		return -HUGE_VAL;
	}
	return (double)x_yl2x(1.0L, x);
}

double log(double x)
{
	if (x <= 0 || isnan(x))
		return log2(x);
	return (double)x_yl2x(0.69314718055994530941723212145818L, x);
}

double log10(double x)
{
	if (x <= 0 || isnan(x))
		return log2(x);
	return (double)x_yl2x(0.30102999566398119521373889472449L, x);
}

double pow(double x, double y)
{
	if (y == 0)
		return 1;
	if (x == 1)
		return 1;
	if (isnan(x) || isnan(y))
		return x + y;
	if (x == 0) {
		int odd = y == trunc(y) && fmod(fabs(y), 2) == 1;
		if (y < 0)
			return odd && signbit(x) ? -HUGE_VAL : HUGE_VAL;
		return odd ? x : 0.0;
	}
	int neg = 0;
	if (x < 0) {
		if (y != trunc(y)) {
			errno = EDOM;
			return NAN;
		}
		neg = fmod(fabs(y), 2) == 1;
		x = -x;
	}
	long double r;
	if (y == trunc(y) && fabs(y) <= 64) {
		/* exact for small integer powers */
		long n = (long)fabs(y);
		long double b = x, a = 1;
		while (n) {
			if (n & 1)
				a *= b;
			b *= b;
			n >>= 1;
		}
		r = y < 0 ? 1 / a : a;
	} else {
		r = x_exp2(x_yl2x(y, x));
	}
	double d = (double)r;
	if (isinf(d))
		errno = ERANGE;
	return neg ? -d : d;
}

float powf(float x, float y) { return (float)pow(x, y); }

static double round_mode(double x, int mode)
{
	/* mode: the x87 rounding control, 0 nearest-even, 1 down, 2 up, 3 truncate */
	if (isnan(x) || isinf(x) || fabs(x) >= 4503599627370496.0)
		return x;
	uint16_t cw, ncw;
	long double v = x;
	__asm__ volatile("fnstcw %0" : "=m"(cw));
	ncw = (uint16_t)((cw & ~0x0C00) | (mode << 10));
	__asm__ volatile("fldcw %0" : : "m"(ncw));
	v = x_rndint(v);
	__asm__ volatile("fldcw %0" : : "m"(cw));
	double r = (double)v;
	if (r == 0)
		return x < 0 ? -0.0 : 0.0;
	return r;
}

double floor(double x) { return round_mode(x, 1); }
double ceil(double x) { return round_mode(x, 2); }
double trunc(double x) { return round_mode(x, 3); }
double round(double x)
{
	double t = trunc(x);
	if (fabs(x - t) >= 0.5)
		t += x < 0 ? -1 : 1;
	return t;
}
long lround(double x) { return (long)round(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }

double fmod(double x, double y)
{
	if (y == 0 || isinf(x) || isnan(x) || isnan(y)) {
		if (y == 0)
			errno = EDOM;
		return (x * y) / (x * y);
	}
	if (isinf(y))
		return x;
	long double a = x, b = y;
	__asm__("1: fprem\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b" : "+t"(a) : "u"(b) : "ax");
	return (double)a;
}

double ldexp(double x, int n)
{
	/* as musl's scalbn: scale in steps that stay exact, round once at the end */
	du u;
	if (n > 1023) {
		x *= 0x1p1023;
		n -= 1023;
		if (n > 1023) {
			x *= 0x1p1023;
			n -= 1023;
			if (n > 1023)
				n = 1023;
		}
	} else if (n < -1022) {
		x *= 0x1p-1022 * 0x1p53;
		n += 1022 - 53;
		if (n < -1022) {
			x *= 0x1p-1022 * 0x1p53;
			n += 1022 - 53;
			if (n < -1022)
				n = -1022;
		}
	}
	u.u = (uint64_t)(0x3ff + n) << 52;
	return x * u.d;
}

long double ldexpl(long double x, int e)
{
	long double r;
	long double ee = e;
	__asm__("fscale" : "=t"(r) : "0"(x), "u"(ee));
	return r;
}

double frexp(double x, int *e)
{
	du v = { x };
	int ex = (int)((v.u >> 52) & 0x7FF);
	if (!ex) {
		if (x == 0) {
			*e = 0;
			return x;
		}
		x *= 0x1p64;
		v.d = x;
		ex = (int)((v.u >> 52) & 0x7FF) - 64;
	} else if (ex == 0x7FF) {
		*e = 0;
		return x;
	}
	*e = ex - 1022;
	v.u = (v.u & ~(0x7FFull << 52)) | (1022ull << 52);
	return v.d;
}

long double frexpl(long double x, int *e)
{
	int ee;
	double m = frexp((double)x, &ee);
	(void)m;
	*e = ee;
	return ldexpl(x, -ee);
}

double modf(double x, double *ip)
{
	double t = trunc(x);
	*ip = t;
	return isinf(x) ? 0.0 * x : x - t;
}

double hypot(double x, double y) { return (double)sqrtl((long double)x * x + (long double)y * y); }

double cbrt(double x)
{
	if (x == 0 || isnan(x) || isinf(x))
		return x;
	double r = (double)x_exp2(x_yl2x(1.0L / 3, fabs(x)));
	r = r - (r * r * r - fabs(x)) / (3 * r * r);	/* one Newton step */
	return x < 0 ? -r : r;
}

double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x)
{
	if (x > 20) return 1;
	if (x < -20) return -1;
	double e = exp(2 * x);
	return (e - 1) / (e + 1);
}

double fmin(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a < b ? a : b; }
double fmax(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a > b ? a : b; }
