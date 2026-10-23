/* string.c - string.h, strings.h, ctype.h */
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

void *memcpy(void *d, const void *s, size_t n)
{
	void *r = d;
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return r;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dd = d;
	const unsigned char *ss = s;
	if (dd == ss || !n)
		return d;
	if (dd < ss || dd >= ss + n)
		return memcpy(d, s, n);
	while (n--)
		dd[n] = ss[n];
	return d;
}

void *memset(void *d, int c, size_t n)
{
	void *r = d;
	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return r;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t n)
{
	size_t i = 0;
	while (i < n && s[i])
		i++;
	return i;
}

char *strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++))
		;
	return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++)
		d[i] = s[i];
	for (; i < n; i++)
		d[i] = 0;
	return d;
}

char *strcat(char *d, const char *s)
{
	strcpy(d + strlen(d), s);
	return d;
}

char *strncat(char *d, const char *s, size_t n)
{
	char *p = d + strlen(d);
	while (n-- && *s)
		*p++ = *s++;
	*p = 0;
	return d;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (!*a)
			return 0;
	}
	return 0;
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

size_t strxfrm(char *d, const char *s, size_t n)
{
	size_t l = strlen(s);
	if (n) {
		strncpy(d, s, n - 1);
		d[n - 1] = 0;
	}
	return l;
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
	if (!l)
		return (char *)h;
	for (; *h; h++)
		if (*h == *n && !strncmp(h, n, l))
			return (char *)h;
	return NULL;
}

char *strdup(const char *s)
{
	size_t l = strlen(s) + 1;
	char *d = malloc(l);
	return d ? memcpy(d, s, l) : NULL;
}

char *strndup(const char *s, size_t n)
{
	size_t l = strnlen(s, n);
	char *d = malloc(l + 1);
	if (!d)
		return NULL;
	memcpy(d, s, l);
	d[l] = 0;
	return d;
}

size_t strspn(const char *s, const char *a)
{
	size_t i = 0;
	while (s[i] && strchr(a, s[i]))
		i++;
	return i;
}

size_t strcspn(const char *s, const char *r)
{
	size_t i = 0;
	while (s[i] && !strchr(r, s[i]))
		i++;
	return i;
}

char *strpbrk(const char *s, const char *a)
{
	s += strcspn(s, a);
	return *s ? (char *)s : NULL;
}

char *strtok_r(char *s, const char *d, char **save)
{
	if (!s)
		s = *save;
	if (!s)
		return NULL;
	s += strspn(s, d);
	if (!*s) {
		*save = NULL;
		return NULL;
	}
	char *e = s + strcspn(s, d);
	if (*e)
		*e++ = 0;
	else
		e = NULL;
	*save = e;
	return s;
}

char *strtok(char *s, const char *d)
{
	static char *save;
	return strtok_r(s, d, &save);
}

int strcasecmp(const char *a, const char *b)
{
	while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
		a++, b++;
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
		if (x != y)
			return x - y;
		if (!x)
			return 0;
	}
	return 0;
}

char *strerror(int e)
{
	switch (e) {
	case 0: return "No error";
	case EPERM: return "Not permitted";
	case ENOENT: return "No such file or directory";
	case EIO: return "Input/output error";
	case EBADF: return "Bad file handle";
	case ENOMEM: return "Not enough memory";
	case EACCES: return "Access denied";
	case EEXIST: return "File exists";
	case ENOTDIR: return "Not a directory";
	case EISDIR: return "Is a directory";
	case EINVAL: return "Invalid argument";
	case EMFILE: return "Too many open files";
	case ENOSPC: return "Disk full";
	case ERANGE: return "Result out of range";
	case EDOM: return "Argument out of domain";
	case ENAMETOOLONG: return "Name too long";
	case ENOSYS: return "Not supported";
	case ENOTEMPTY: return "Directory not empty";
	case ETIMEDOUT: return "Timed out";
	default: return "Unknown error";
	}
}

/* ---- ctype (ASCII; CP437 high characters are neither letters nor digits) ---- */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isalpha(int c) { return islower(c) || isupper(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isascii(int c) { return c >= 0 && c < 128; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
