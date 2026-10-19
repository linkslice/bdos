/*
 * fmath.c - just enough libm for BASIC and C programs. Accurate to about
 * 1e-15 relative, which is far beyond what BASIC prints.
 */
#include "fmath.h"

#define PI_D 3.14159265358979323846

double f_fabs(double x) { return x < 0 ? -x : x; }

double f_floor(double x)
{
	if (x >= 9.2e18 || x <= -9.2e18 || x != x)
		return x;
	long long i = (long long)x;
	double d = (double)i;
	return (d > x) ? d - 1 : d;
}

double f_ceil(double x)
{
	double f = f_floor(x);
	return f == x ? f : f + 1;
}

double f_trunc(double x)
{
	return x < 0 ? -f_floor(-x) : f_floor(x);
}

double f_fmod(double a, double b)
{
	if (b == 0)
		return 0;
	return a - f_trunc(a / b) * b;
}

double f_sqrt(double x)
{
	if (x <= 0)
		return 0;
	double r;
	__asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
	return r;
}

/* sin on [-pi/4, pi/4] and cos likewise, by Taylor series */
static double ksin(double x)
{
	double x2 = x * x, term = x, sum = x;
	for (int n = 1; n < 12; n++) {
		term *= -x2 / ((2 * n) * (2 * n + 1));
		sum += term;
	}
	return sum;
}

static double kcos(double x)
{
	double x2 = x * x, term = 1, sum = 1;
	for (int n = 1; n < 12; n++) {
		term *= -x2 / ((2 * n - 1) * (2 * n));
		sum += term;
	}
	return sum;
}

static double reduce(double x, int *quad)
{
	double q = f_floor(x / (PI_D / 2) + 0.5);
	*quad = (int)((long long)q & 3);
	return x - q * (PI_D / 2);
}

double f_sin(double x)
{
	int q;
	double r = reduce(x, &q);
	switch (q) {
	case 0: return ksin(r);
	case 1: return kcos(r);
	case 2: return -ksin(r);
	default: return -kcos(r);
	}
}

double f_cos(double x)
{
	int q;
	double r = reduce(x, &q);
	switch (q) {
	case 0: return kcos(r);
	case 1: return -ksin(r);
	case 2: return -kcos(r);
	default: return ksin(r);
	}
}

double f_tan(double x)
{
	double c = f_cos(x);
	return c == 0 ? 1e308 : f_sin(x) / c;
}

double f_atan(double x)
{
	int neg = x < 0, inv = 0;
	if (neg)
		x = -x;
	if (x > 1) {
		x = 1 / x;
		inv = 1;
	}
	/* atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))), twice, then the series */
	x = x / (1 + f_sqrt(1 + x * x));
	x = x / (1 + f_sqrt(1 + x * x));
	double x2 = x * x, term = x, sum = x;
	for (int n = 1; n < 20; n++) {
		term *= -x2;
		sum += term / (2 * n + 1);
	}
	sum *= 4;
	if (inv)
		sum = PI_D / 2 - sum;
	return neg ? -sum : sum;
}

double f_atan2(double y, double x)
{
	if (x > 0)
		return f_atan(y / x);
	if (x < 0)
		return f_atan(y / x) + (y >= 0 ? PI_D : -PI_D);
	return y > 0 ? PI_D / 2 : (y < 0 ? -PI_D / 2 : 0);
}

double f_asin(double x)
{
	if (x >= 1) return PI_D / 2;
	if (x <= -1) return -PI_D / 2;
	return f_atan(x / f_sqrt(1 - x * x));
}

double f_acos(double x)
{
	return PI_D / 2 - f_asin(x);
}

double f_exp(double x)
{
	if (x > 709)
		return 1e308;
	if (x < -745)
		return 0;
	/* e^x = 2^k * e^r, |r| <= ln2/2 */
	double k = f_floor(x / 0.69314718055994530942 + 0.5);
	double r = x - k * 0.69314718055994530942;
	double term = 1, sum = 1;
	for (int n = 1; n < 20; n++) {
		term *= r / n;
		sum += term;
	}
	long long ki = (long long)k;
	while (ki > 0) {
		sum *= 2;
		ki--;
	}
	while (ki < 0) {
		sum /= 2;
		ki++;
	}
	return sum;
}

double f_log(double x)
{
	if (x <= 0)
		return -1e308;
	int k = 0;
	while (x > 2) {
		x /= 2;
		k++;
	}
	while (x < 0.5) {
		x *= 2;
		k--;
	}
	/* ln(x) = 2 atanh((x-1)/(x+1)) */
	double y = (x - 1) / (x + 1), y2 = y * y, term = y, sum = 0;
	for (int n = 0; n < 40; n++) {
		sum += term / (2 * n + 1);
		term *= y2;
	}
	return 2 * sum + k * 0.69314718055994530942;
}

double f_log10(double x)
{
	return f_log(x) / 2.30258509299404568402;
}

double f_pow(double a, double b)
{
	if (b == 0)
		return 1;
	if (b == f_floor(b) && f_fabs(b) < 1e9) {
		long long n = (long long)(b < 0 ? -b : b);
		double r = 1, p = a;
		while (n) {
			if (n & 1)
				r *= p;
			p *= p;
			n >>= 1;
		}
		return b < 0 ? 1 / r : r;
	}
	if (a <= 0)
		return 0;
	return f_exp(b * f_log(a));
}

/*
 * Format a number the way BBC BASIC does: integers plainly, otherwise up
 * to `sig` significant digits with trailing zeros removed, switching to
 * E notation for very large or small values.
 */
int f_format(double v, char *out, int n, int sig)
{
	char buf[64];
	int o = 0;
	if (v != v) {
		const char *s = "NaN";
		while (*s && o < n - 1)
			out[o++] = *s++;
		out[o] = 0;
		return o;
	}
	if (v < 0) {
		buf[o++] = '-';
		v = -v;
	}
	if (v == f_floor(v) && v < 1e15) {
		char d[24];
		int nd = 0;
		unsigned long long u = (unsigned long long)v;
		do {
			d[nd++] = (char)('0' + u % 10);
			u /= 10;
		} while (u);
		while (nd)
			buf[o++] = d[--nd];
		buf[o] = 0;
	} else {
		int e = 0;
		if (v != 0) {
			while (v >= 10) {
				v /= 10;
				e++;
			}
			while (v < 1) {
				v *= 10;
				e--;
			}
		}
		/* round to `sig` digits */
		double r = 0.5;
		for (int i = 1; i < sig; i++)
			r /= 10;
		v += r;
		if (v >= 10) {
			v /= 10;
			e++;
		}
		char dig[24];
		for (int i = 0; i < sig; i++) {
			int d = (int)v;
			dig[i] = (char)('0' + d);
			v = (v - d) * 10;
		}
		int last = sig - 1;
		while (last > 0 && dig[last] == '0')
			last--;
		if (e >= -4 && e < sig) {
			if (e < 0) {
				buf[o++] = '0';
				buf[o++] = '.';
				for (int i = -1; i > e; i--)
					buf[o++] = '0';
				for (int i = 0; i <= last; i++)
					buf[o++] = dig[i];
			} else {
				for (int i = 0; i <= MAXI(last, e); i++) {
					buf[o++] = i <= last ? dig[i] : '0';
					if (i == e && i < last)
						buf[o++] = '.';
				}
			}
		} else {
			buf[o++] = dig[0];
			if (last > 0) {
				buf[o++] = '.';
				for (int i = 1; i <= last; i++)
					buf[o++] = dig[i];
			}
			buf[o++] = 'E';
			if (e < 0) {
				buf[o++] = '-';
				e = -e;
			}
			char ed[8];
			int ne = 0;
			do {
				ed[ne++] = (char)('0' + e % 10);
				e /= 10;
			} while (e);
			while (ne)
				buf[o++] = ed[--ne];
		}
		buf[o] = 0;
	}
	int i = 0;
	for (; buf[i] && i < n - 1; i++)
		out[i] = buf[i];
	out[i] = 0;
	return i;
}

double f_parse(const char *s, const char **end)
{
	double v = 0;
	int neg = 0;
	if (*s == '-' || *s == '+')
		neg = (*s++ == '-');
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	if (*s == '.') {
		double scale = 0.1;
		s++;
		while (*s >= '0' && *s <= '9') {
			v += (*s++ - '0') * scale;
			scale /= 10;
		}
	}
	if ((*s == 'e' || *s == 'E') &&
	    ((s[1] >= '0' && s[1] <= '9') || ((s[1] == '-' || s[1] == '+') && s[2] >= '0' && s[2] <= '9'))) {
		s++;
		int eneg = 0, e = 0;
		if (*s == '-' || *s == '+')
			eneg = (*s++ == '-');
		while (*s >= '0' && *s <= '9')
			e = e * 10 + (*s++ - '0');
		v *= f_pow(10, eneg ? -e : e);
	}
	if (end)
		*end = s;
	return neg ? -v : v;
}
