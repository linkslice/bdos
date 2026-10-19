/* lib.c - the tiny bit of libc the kernel needs */
#include "kernel.h"

void *memset(void *d, int c, size_t n)
{
	uint8_t *p = d;
	while (n--)
		*p++ = (uint8_t)c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	uint8_t *dp = d;
	const uint8_t *sp = s;
	while (n--)
		*dp++ = *sp++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	uint8_t *dp = d;
	const uint8_t *sp = s;
	if (dp < sp) {
		while (n--)
			*dp++ = *sp++;
	} else if (dp > sp) {
		dp += n;
		sp += n;
		while (n--)
			*--dp = *--sp;
	}
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (uint8_t)*a - (uint8_t)*b;
}

int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int strcasecmp(const char *a, const char *b)
{
	while (*a && toupper(*a) == toupper(*b))
		a++, b++;
	return toupper((uint8_t)*a) - toupper((uint8_t)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		int d = toupper((uint8_t)*a) - toupper((uint8_t)*b);
		if (d || !*a)
			return d;
	}
	return 0;
}

char *strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++))
		;
	return r;
}

char *strncpy0(char *d, const char *s, size_t n)
{
	size_t i = 0;
	if (!n)
		return d;
	for (; i + 1 < n && s[i]; i++)
		d[i] = s[i];
	d[i] = 0;
	return d;
}

/* ---- formatted output --------------------------------------------------- */

struct out {
	char *buf;
	size_t cap, len;
};

static void emit(struct out *o, char c)
{
	if (o->len + 1 < o->cap)
		o->buf[o->len] = c;
	o->len++;
}

static void emit_num(struct out *o, uint64_t v, int base, int neg, int width,
    int zero, int left, int upper)
{
	char tmp[24];
	int n = 0;
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	do {
		tmp[n++] = digits[v % base];
		v /= base;
	} while (v);
	int total = n + neg;
	if (!left && !zero)
		for (; total < width; total++)
			emit(o, ' ');
	if (neg)
		emit(o, '-');
	if (!left && zero)
		for (; total < width; total++)
			emit(o, '0');
	while (n)
		emit(o, tmp[--n]);
	if (left)
		for (; total < width; total++)
			emit(o, ' ');
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
	struct out o = { buf, cap, 0 };

	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			emit(&o, *fmt);
			continue;
		}
		fmt++;
		int left = 0, zero = 0, width = 0, lng = 0;
		for (;; fmt++) {
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				zero = 1;
			else
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				left = 1;
				width = -width;
			}
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l' || *fmt == 'z') {
			lng = 1;
			fmt++;
		}

		switch (*fmt) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			int len = (int)strlen(s);
			if (!left)
				for (int i = len; i < width; i++)
					emit(&o, ' ');
			while (*s)
				emit(&o, *s++);
			if (left)
				for (int i = len; i < width; i++)
					emit(&o, ' ');
			break;
		}
		case 'c':
			emit(&o, (char)va_arg(ap, int));
			break;
		case 'd':
		case 'i': {
			int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
			emit_num(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, v < 0,
			    width, zero, left, 0);
			break;
		}
		case 'u': {
			uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
			emit_num(&o, v, 10, 0, width, zero, left, 0);
			break;
		}
		case 'x':
		case 'X': {
			uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
			emit_num(&o, v, 16, 0, width, zero, left, *fmt == 'X');
			break;
		}
		case 'p':
			emit(&o, '0');
			emit(&o, 'x');
			emit_num(&o, (uint64_t)va_arg(ap, void *), 16, 0, 16, 1, 0, 0);
			break;
		case '%':
			emit(&o, '%');
			break;
		case 0:
			fmt--;
			break;
		default:
			emit(&o, '%');
			emit(&o, *fmt);
		}
	}
	if (cap)
		buf[o.len < cap ? o.len : cap - 1] = 0;
	return (int)o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return r;
}

void kprintf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	con_write(buf);
}

void fmt_commas(char *buf, size_t n, uint64_t v)
{
	char tmp[32];
	int len = 0, digits = 0;
	do {
		if (digits && digits % 3 == 0)
			tmp[len++] = ',';
		tmp[len++] = (char)('0' + v % 10);
		v /= 10;
		digits++;
	} while (v);
	size_t i = 0;
	while (len && i + 1 < n)
		buf[i++] = tmp[--len];
	buf[i] = 0;
}

_Noreturn void panic(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	con_write("\n*** bdos panic: ");
	con_write(buf);
	con_write("\n*** System halted.\n");
	for (;;)
		__asm__ volatile("cli; hlt");
}
