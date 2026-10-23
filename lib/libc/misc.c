/* misc.c - the rest of stdlib.h, and time.h */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }
div_t div(int a, int b) { div_t r = { a / b, a % b }; return r; }
ldiv_t ldiv(long a, long b) { ldiv_t r = { a / b, a % b }; return r; }
lldiv_t lldiv(long long a, long long b) { lldiv_t r = { a / b, a % b }; return r; }

static unsigned long long seed = 1;
void srand(unsigned s) { seed = s; }
int rand(void)
{
	seed = seed * 6364136223846793005ull + 1442695040888963407ull;
	return (int)(seed >> 33);
}

static void swap(char *a, char *b, size_t n)
{
	while (n--) {
		char t = *a;
		*a++ = *b;
		*b++ = t;
	}
}

/* introsort-lite: quicksort with median of three, insertion sort for small runs */
static void qs(char *b, size_t n, size_t sz, int (*cmp)(const void *, const void *), int depth)
{
	while (n > 12) {
		if (!depth--) {
			/* heap sort fallback */
			for (size_t i = n / 2; i-- > 0;) {
				size_t r = i;
				for (;;) {
					size_t c = 2 * r + 1;
					if (c >= n) break;
					if (c + 1 < n && cmp(b + c * sz, b + (c + 1) * sz) < 0) c++;
					if (cmp(b + r * sz, b + c * sz) >= 0) break;
					swap(b + r * sz, b + c * sz, sz);
					r = c;
				}
			}
			for (size_t end = n - 1; end > 0; end--) {
				swap(b, b + end * sz, sz);
				size_t r = 0;
				for (;;) {
					size_t c = 2 * r + 1;
					if (c >= end) break;
					if (c + 1 < end && cmp(b + c * sz, b + (c + 1) * sz) < 0) c++;
					if (cmp(b + r * sz, b + c * sz) >= 0) break;
					swap(b + r * sz, b + c * sz, sz);
					r = c;
				}
			}
			return;
		}
		char *m = b + (n / 2) * sz, *l = b + (n - 1) * sz;
		if (cmp(m, b) < 0) swap(m, b, sz);
		if (cmp(l, m) < 0) {
			swap(l, m, sz);
			if (cmp(m, b) < 0) swap(m, b, sz);
		}
		swap(m, b + sz, sz);	/* pivot at b[1] */
		char *p = b + sz;
		size_t i = 1, j = n - 1;
		for (;;) {
			do i++; while (cmp(b + i * sz, p) < 0);
			do j--; while (cmp(b + j * sz, p) > 0);
			if (i >= j) break;
			swap(b + i * sz, b + j * sz, sz);
		}
		swap(p, b + j * sz, sz);
		/* recurse into the smaller side */
		if (j < n - j - 1) {
			qs(b, j, sz, cmp, depth);
			b += (j + 1) * sz;
			n -= j + 1;
		} else {
			qs(b + (j + 1) * sz, n - j - 1, sz, cmp, depth);
			n = j;
		}
	}
	for (size_t i = 1; i < n; i++)
		for (size_t k = i; k > 0 && cmp(b + (k - 1) * sz, b + k * sz) > 0; k--)
			swap(b + (k - 1) * sz, b + k * sz, sz);
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
	int depth = 0;
	for (size_t m = n; m; m >>= 1)
		depth += 2;
	qs(base, n, size, cmp, depth);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
	const char *b = base;
	while (n) {
		const char *m = b + (n / 2) * size;
		int c = cmp(key, m);
		if (!c)
			return (void *)m;
		if (c > 0) {
			b = m + size;
			n -= n / 2 + 1;
		} else
			n /= 2;
	}
	return NULL;
}

char *realpath(const char *path, char *resolved)
{
	/* bdos paths: drive-relative or not, the shell resolves; do it simply */
	static char own[256];
	char *out = resolved ? resolved : own;
	if ((path[0] && path[1] == ':') || path[0] == '\\' || path[0] == '/') {
		if (path[0] == '\\' || path[0] == '/') {
			char cwd[256];
			getcwd(cwd, sizeof cwd);
			snprintf(out, 256, "%.2s%s", cwd, path);
		} else
			snprintf(out, 256, "%s", path);
	} else {
		char cwd[256];
		getcwd(cwd, sizeof cwd);
		size_t l = strlen(cwd);
		snprintf(out, 256, "%s%s%s", cwd, l && cwd[l - 1] == '\\' ? "" : "\\", path);
	}
	for (char *p = out; *p; p++)
		if (*p == '/')
			*p = '\\';
	if (access(out, 0)) {
		errno = ENOENT;
		return NULL;
	}
	return resolved ? resolved : strdup(out);
}

/* ---- time ---- */

double difftime(time_t a, time_t b) { return (double)(a - b); }

static int is_leap(long y) { return (y % 4 == 0 && y % 100) || y % 400 == 0; }
static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

struct tm *gmtime_r(const time_t *t, struct tm *r)
{
	long s = *t;
	long days = s / 86400, rem = s % 86400;
	if (rem < 0) {
		rem += 86400;
		days--;
	}
	r->tm_hour = (int)(rem / 3600);
	r->tm_min = (int)(rem % 3600 / 60);
	r->tm_sec = (int)(rem % 60);
	r->tm_wday = (int)((days + 4) % 7);
	if (r->tm_wday < 0)
		r->tm_wday += 7;
	long y = 1970;
	for (;;) {
		long yd = is_leap(y) ? 366 : 365;
		if (days < 0) {
			y--;
			days += is_leap(y) ? 366 : 365;
		} else if (days >= yd) {
			days -= yd;
			y++;
		} else
			break;
	}
	r->tm_year = (int)(y - 1900);
	r->tm_yday = (int)days;
	int m = 0;
	for (;; m++) {
		int md = mdays[m] + (m == 1 && is_leap(y));
		if (days < md)
			break;
		days -= md;
	}
	r->tm_mon = m;
	r->tm_mday = (int)days + 1;
	r->tm_isdst = 0;
	return r;
}

/* bdos keeps local time in the clock, so local and UTC are the same here */
struct tm *localtime_r(const time_t *t, struct tm *r) { return gmtime_r(t, r); }
struct tm *gmtime(const time_t *t) { static struct tm r; return gmtime_r(t, &r); }
struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t mktime(struct tm *tm)
{
	long y = tm->tm_year + 1900L, m = tm->tm_mon;
	y += m / 12;
	m %= 12;
	if (m < 0) {
		m += 12;
		y--;
	}
	long days = 0;
	for (long yy = 1970; yy < y; yy++)
		days += is_leap(yy) ? 366 : 365;
	for (long yy = y; yy < 1970; yy++)
		days -= is_leap(yy) ? 366 : 365;
	for (int i = 0; i < m; i++)
		days += mdays[i] + (i == 1 && is_leap(y));
	days += tm->tm_mday - 1;
	time_t t = days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
	gmtime_r(&t, tm);
	return t;
}

static const char *wday_name[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *mon_name[] = { "January", "February", "March", "April", "May", "June", "July",
				  "August", "September", "October", "November", "December" };

char *asctime(const struct tm *tm)
{
	static char b[32];
	snprintf(b, sizeof b, "%.3s %.3s %2d %02d:%02d:%02d %d\n", wday_name[tm->tm_wday % 7],
		 mon_name[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
	return b;
}

char *ctime(const time_t *t) { return asctime(localtime(t)); }

size_t strftime(char *s, size_t n, const char *fmt, const struct tm *tm)
{
	size_t o = 0;
	char b[64];
	for (const char *p = fmt; *p; p++) {
		const char *add = b;
		if (*p != '%') {
			b[0] = *p;
			b[1] = 0;
		} else {
			switch (*++p) {
			case 'a': snprintf(b, sizeof b, "%.3s", wday_name[tm->tm_wday % 7]); break;
			case 'A': add = wday_name[tm->tm_wday % 7]; break;
			case 'b': case 'h': snprintf(b, sizeof b, "%.3s", mon_name[tm->tm_mon % 12]); break;
			case 'B': add = mon_name[tm->tm_mon % 12]; break;
			case 'c': snprintf(b, sizeof b, "%.24s", asctime(tm)); break;
			case 'd': snprintf(b, sizeof b, "%02d", tm->tm_mday); break;
			case 'e': snprintf(b, sizeof b, "%2d", tm->tm_mday); break;
			case 'H': snprintf(b, sizeof b, "%02d", tm->tm_hour); break;
			case 'I': snprintf(b, sizeof b, "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
			case 'j': snprintf(b, sizeof b, "%03d", tm->tm_yday + 1); break;
			case 'm': snprintf(b, sizeof b, "%02d", tm->tm_mon + 1); break;
			case 'M': snprintf(b, sizeof b, "%02d", tm->tm_min); break;
			case 'p': add = tm->tm_hour < 12 ? "AM" : "PM"; break;
			case 'S': snprintf(b, sizeof b, "%02d", tm->tm_sec); break;
			case 'y': snprintf(b, sizeof b, "%02d", tm->tm_year % 100); break;
			case 'Y': snprintf(b, sizeof b, "%d", tm->tm_year + 1900); break;
			case 'w': snprintf(b, sizeof b, "%d", tm->tm_wday); break;
			case 'x': snprintf(b, sizeof b, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); break;
			case 'X': case 'T': snprintf(b, sizeof b, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
			case 'D': snprintf(b, sizeof b, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); break;
			case 'F': snprintf(b, sizeof b, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
			case 'R': snprintf(b, sizeof b, "%02d:%02d", tm->tm_hour, tm->tm_min); break;
			case 'Z': add = ""; break;
			case 'n': add = "\n"; break;
			case 't': add = "\t"; break;
			case '%': add = "%"; break;
			default: return 0;
			}
		}
		size_t l = strlen(add);
		if (o + l >= n)
			return 0;
		memcpy(s + o, add, l);
		o += l;
	}
	if (o < n)
		s[o] = 0;
	return o;
}
