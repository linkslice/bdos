#ifndef _LOCALE_H
#define _LOCALE_H
#define LC_ALL 0
#define LC_CTYPE 1
#define LC_NUMERIC 2
struct lconv { char *decimal_point; };
char *setlocale(int cat, const char *loc);
struct lconv *localeconv(void);
#endif
