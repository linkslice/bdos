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
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (uint8_t)*a - (uint8_t)*b;
		if (!*a)
			return 0;
	}
	return 0;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

char *strrchr(const char *s, int c)
{
	const char *r = NULL;
	for (;; s++) {
		if (*s == (char)c)
			r = s;
		if (!*s)
			return (char *)r;
	}
}

char *strstr(const char *h, const char *n)
{
	size_t l = strlen(n);
	for (; *h; h++)
		if (!strncmp(h, n, l))
			return (char *)h;
	return l ? NULL : (char *)h;
}

char *strcat0(char *d, const char *s, size_t n)
{
	size_t l = strlen(d);
	if (l < n)
		strncpy0(d + l, s, n - l);
	return d;
}

long strtol(const char *s, const char **end, int base)
{
	long v = 0;
	int neg = 0;
	while (isspace(*s))
		s++;
	if (*s == '-' || *s == '+')
		neg = (*s++ == '-');
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0) {
		base = 10;
	}
	for (;; s++) {
		int d;
		if (isdigit(*s))
			d = *s - '0';
		else if (isalpha(*s))
			d = toupper(*s) - 'A' + 10;
		else
			break;
		if (d >= base)
			break;
		v = v * base + d;
	}
	if (end)
		*end = s;
	return neg ? -v : v;
}

/* '*' matches any run, '?' one character, case-insensitive. DOS-style
 * "*.*" also matches names without a dot. */
int wildmatch(const char *p, const char *s)
{
	if (!strcmp(p, "*.*"))
		return 1;
	const char *star = NULL, *ss = NULL;
	while (*s) {
		if (*p == '?' || (*p && toupper(*p) == toupper(*s))) {
			p++;
			s++;
		} else if (*p == '*') {
			star = p++;
			ss = s;
		} else if (star) {
			p = star + 1;
			s = ++ss;
		} else {
			return 0;
		}
	}
	while (*p == '*')
		p++;
	return !*p;
}

/* CRC-32C (Castagnoli), bitwise; metadata blocks are small. */
uint32_t crc32c(uint32_t crc, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0x82F63B78 & (0 - (crc & 1)));
	}
	return ~crc;
}

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
	irq_disable();
	console_panic_mode();
	con_write("\n*** bdos panic: ");
	con_write(buf);
	/* frame-pointer backtrace: resolve with addr2line -e build/KERNEL.SYS */
	uint64_t *fp = __builtin_frame_address(0);
	con_write("\n*** backtrace:");
	for (int i = 0; i < 16 && fp && (uint64_t)fp > 0x100000; i++) {
		char a[24];
		snprintf(a, sizeof a, " %lx", fp[1]);
		con_write(a);
		fp = (uint64_t *)fp[0];
	}
	con_write("\n*** System halted.\n");
	arch_halt();
}
