/* libc.h - shared between the C library's files */
#ifndef LIBC_INT_H
#define LIBC_INT_H
#include <stdint.h>
#include <stddef.h>

/* exact decimal digits of a finite double >= 0: digits (no leading zeros,
 * "0" for zero) and the decimal exponent such that value = 0.DIGITS * 10^exp10 */
int __dtoa_exact(double v, char *digits, int cap, int *exp10);
double __strtod(const char *s, char **end);
#endif
