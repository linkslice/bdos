#ifndef _MATH_H
#define _MATH_H
#define M_PI 3.14159265358979323846
#define M_E 2.7182818284590452354
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_SQRT2 1.41421356237309504880
#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#ifdef __TINYC__
/* tcc has no classification built-ins: functions instead (as doubles) */
int __isnan(double x);
int __isinf(double x);
int __isfinite(double x);
int __signbit(double x);
#define INFINITY (__builtin_huge_valf())
#define NAN (-(0.0f / 0.0f))	/* the x86 makes a negative NaN */
#define isnan(x) __isnan(x)
#define isinf(x) __isinf(x)
#define isfinite(x) __isfinite(x)
#define signbit(x) __signbit(x)
#else
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#endif
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fmod(double x, double y);
double ldexp(double x, int e);
double frexp(double x, int *e);
double modf(double x, double *ip);
double hypot(double x, double y);
double cbrt(double x);
double fmin(double a, double b);
double fmax(double a, double b);
long lround(double x);
float sqrtf(float x);
float sinf(float x);
float cosf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float powf(float x, float y);
float atan2f(float y, float x);
long double ldexpl(long double x, int e);
long double frexpl(long double x, int *e);
long double fabsl(long double x);
long double sqrtl(long double x);
#endif
