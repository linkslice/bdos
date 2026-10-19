/* fmath.h - floating point helpers for the language interpreters */
#ifndef BDOS_FMATH_H
#define BDOS_FMATH_H

#define MAXI(a, b) ((a) > (b) ? (a) : (b))

double f_fabs(double x);
double f_floor(double x);
double f_ceil(double x);
double f_trunc(double x);
double f_fmod(double a, double b);
double f_sqrt(double x);
double f_sin(double x);
double f_cos(double x);
double f_tan(double x);
double f_atan(double x);
double f_atan2(double y, double x);
double f_asin(double x);
double f_acos(double x);
double f_exp(double x);
double f_log(double x);
double f_log10(double x);
double f_pow(double a, double b);
int f_format(double v, char *out, int n, int sig);
double f_parse(const char *s, const char **end);

#endif
