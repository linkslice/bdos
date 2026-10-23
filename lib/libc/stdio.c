/* stdio.c - buffered files, printf and scanf */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <bdos.h>
#include "libc.h"

#define F_READ 1
#define F_WRITE 2
#define F_EOF 4
#define F_ERR 8
#define F_OWNBUF 16
#define F_STATIC 32

struct FILE {
	int fd;
	int flags;
	int mode;		/* _IOFBF, _IOLBF, _IONBF */
	unsigned char *buf;
	size_t size;
	size_t rpos, rend;	/* reading: buf[rpos..rend) */
	size_t wpos;		/* writing: buf[0..wpos) */
	int unget;		/* -1 or a pushed-back character */
	bdos_mutex lock;
	struct FILE *next;
};

static unsigned char outbuf[BUFSIZ], inbuf[BUFSIZ];
static FILE f_in = { 0, F_READ | F_STATIC, _IOLBF, inbuf, BUFSIZ, 0, 0, 0, -1, BDOS_MUTEX_INIT, NULL };
static FILE f_out = { 1, F_WRITE | F_STATIC, _IOLBF, outbuf, BUFSIZ, 0, 0, 0, -1, BDOS_MUTEX_INIT, &f_in };
static FILE f_err = { 2, F_WRITE | F_STATIC, _IONBF, NULL, 0, 0, 0, 0, -1, BDOS_MUTEX_INIT, &f_out };
FILE *stdin = &f_in, *stdout = &f_out, *stderr = &f_err;

static FILE *all = &f_err;
static bdos_mutex all_lock = BDOS_MUTEX_INIT;

static int flush_locked(FILE *f)
{
	size_t done = 0;
	while (done < f->wpos) {
		ssize_t w = write(f->fd, f->buf + done, f->wpos - done);
		if (w <= 0) {
			f->flags |= F_ERR;
			f->wpos = 0;
			return EOF;
		}
		done += (size_t)w;
	}
	f->wpos = 0;
	return 0;
}

/* give back what was read ahead so the fd's position is where the program thinks */
static void drop_readahead(FILE *f)
{
	if (f->rend > f->rpos || f->unget >= 0) {
		long back = (long)(f->rend - f->rpos) + (f->unget >= 0);
		if (f->fd > 2)
			lseek(f->fd, -back, SEEK_CUR);
	}
	f->rpos = f->rend = 0;
	f->unget = -1;
}

int fflush(FILE *f)
{
	if (!f) {
		bdos_lock(&all_lock);
		for (FILE *g = all; g; g = g->next)
			if (g->wpos) {
				bdos_lock(&g->lock);
				flush_locked(g);
				bdos_unlock(&g->lock);
			}
		bdos_unlock(&all_lock);
		return 0;
	}
	bdos_lock(&f->lock);
	int r = flush_locked(f);
	bdos_unlock(&f->lock);
	return r;
}

void __stdio_flush_all(void)
{
	fflush(NULL);
}

/* exit() flushes; this is for tcc -run, whose exit() runs destructors instead */
__attribute__((destructor)) static void flush_at_exit(void)
{
	fflush(NULL);
}

static int parse_mode(const char *m, int *fl)
{
	int o;
	switch (*m) {
	case 'r': o = O_RDONLY; *fl = F_READ; break;
	case 'w': o = O_WRONLY | O_CREAT | O_TRUNC; *fl = F_WRITE; break;
	case 'a': o = O_WRONLY | O_CREAT | O_APPEND; *fl = F_WRITE; break;
	default: return -1;
	}
	if (strchr(m, '+')) {
		o = (o & ~O_ACCMODE) | O_RDWR;
		*fl = F_READ | F_WRITE;
	}
	return o;
}

static FILE *new_file(int fd, int fl)
{
	FILE *f = calloc(1, sizeof *f);
	if (!f)
		return NULL;
	f->fd = fd;
	f->flags = fl | F_OWNBUF;
	f->mode = _IOFBF;
	f->unget = -1;
	f->size = BUFSIZ;
	f->buf = malloc(f->size);
	if (!f->buf) {
		free(f);
		return NULL;
	}
	bdos_lock(&all_lock);
	f->next = all;
	all = f;
	bdos_unlock(&all_lock);
	return f;
}

FILE *fopen(const char *path, const char *mode)
{
	int fl, o = parse_mode(mode, &fl);
	if (o < 0) {
		errno = EINVAL;
		return NULL;
	}
	int fd = open(path, o, 0666);
	if (fd < 0)
		return NULL;
	FILE *f = new_file(fd, fl);
	if (!f)
		close(fd);
	return f;
}

FILE *fdopen(int fd, const char *mode)
{
	int fl;
	if (parse_mode(mode, &fl) < 0) {
		errno = EINVAL;
		return NULL;
	}
	return new_file(fd, fl);
}

int fclose(FILE *f)
{
	int r = fflush(f);
	if (f->flags & F_STATIC)
		return r;
	bdos_lock(&all_lock);
	for (FILE **pp = &all; *pp; pp = &(*pp)->next)
		if (*pp == f) {
			*pp = f->next;
			break;
		}
	bdos_unlock(&all_lock);
	if (close(f->fd) < 0)
		r = EOF;
	if (f->flags & F_OWNBUF)
		free(f->buf);
	free(f);
	return r;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
	int fl, o = parse_mode(mode, &fl);
	if (o < 0 || !path) {
		errno = EINVAL;
		return NULL;
	}
	fflush(f);
	int fd = open(path, o, 0666);
	if (fd < 0)
		return NULL;
	if (f->fd > 2)
		close(f->fd);
	f->fd = fd;
	f->flags = (f->flags & (F_OWNBUF | F_STATIC)) | fl;
	f->rpos = f->rend = f->wpos = 0;
	f->unget = -1;
	return f;
}

static int fill(FILE *f)
{
	if (f == stdin)
		fflush(stdout);
	if (f->wpos)
		flush_locked(f);
	if (!f->buf) {
		static unsigned char one[1];
		ssize_t r = read(f->fd, one, 1);
		if (r <= 0) {
			f->flags |= r ? F_ERR : F_EOF;
			return EOF;
		}
		return one[0];
	}
	ssize_t r = read(f->fd, f->buf, f->size);
	if (r <= 0) {
		f->flags |= r ? F_ERR : F_EOF;
		return EOF;
	}
	f->rpos = 1;
	f->rend = (size_t)r;
	return f->buf[0];
}

static int getc_locked(FILE *f)
{
	if (f->unget >= 0) {
		int c = f->unget;
		f->unget = -1;
		return c;
	}
	if (f->rpos < f->rend)
		return f->buf[f->rpos++];
	if (f->flags & F_EOF)
		return EOF;
	return fill(f);
}

int fgetc(FILE *f)
{
	bdos_lock(&f->lock);
	int c = getc_locked(f);
	bdos_unlock(&f->lock);
	return c;
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
	if (c == EOF)
		return EOF;
	bdos_lock(&f->lock);
	if (f->rpos > 0 && f->rpos <= f->rend && f->unget < 0 && f->buf[f->rpos - 1] == (unsigned char)c)
		f->rpos--;
	else
		f->unget = (unsigned char)c;
	f->flags &= ~F_EOF;
	bdos_unlock(&f->lock);
	return c;
}

char *fgets(char *s, int n, FILE *f)
{
	int i = 0;
	bdos_lock(&f->lock);
	while (i < n - 1) {
		int c = getc_locked(f);
		if (c == EOF)
			break;
		s[i++] = (char)c;
		if (c == '\n')
			break;
	}
	bdos_unlock(&f->lock);
	if (!i)
		return NULL;
	s[i] = 0;
	return s;
}

static int write_locked(FILE *f, const void *p, size_t n)
{
	const unsigned char *s = p;
	if (f->rend > f->rpos || f->unget >= 0)
		drop_readahead(f);
	if (f->mode == _IONBF || !f->buf) {
		if (f->wpos && flush_locked(f))
			return EOF;
		size_t done = 0;
		while (done < n) {
			ssize_t w = write(f->fd, s + done, n - done);
			if (w <= 0) {
				f->flags |= F_ERR;
				return EOF;
			}
			done += (size_t)w;
		}
		return 0;
	}
	int nl = 0;
	while (n) {
		size_t room = f->size - f->wpos;
		if (!room) {
			if (flush_locked(f))
				return EOF;
			continue;
		}
		size_t k = n < room ? n : room;
		if (f->mode == _IOLBF && memchr(s, '\n', k))
			nl = 1;
		memcpy(f->buf + f->wpos, s, k);
		f->wpos += k;
		s += k;
		n -= k;
	}
	if (nl && flush_locked(f))
		return EOF;
	return 0;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
	if (!size || !n)
		return 0;
	bdos_lock(&f->lock);
	int r = write_locked(f, p, size * n);
	bdos_unlock(&f->lock);
	return r ? 0 : n;
}

size_t fread(void *p, size_t size, size_t n, FILE *f)
{
	size_t want = size * n, got = 0;
	unsigned char *d = p;
	if (!want)
		return 0;
	bdos_lock(&f->lock);
	if (f->unget >= 0) {
		d[got++] = (unsigned char)f->unget;
		f->unget = -1;
	}
	while (got < want) {
		if (f->rpos < f->rend) {
			size_t k = f->rend - f->rpos;
			if (k > want - got)
				k = want - got;
			memcpy(d + got, f->buf + f->rpos, k);
			f->rpos += k;
			got += k;
			continue;
		}
		if (f->flags & F_EOF)
			break;
		if (f == stdin)
			fflush(stdout);
		if (f->wpos)
			flush_locked(f);
		if (want - got >= f->size || !f->buf) {
			ssize_t r = read(f->fd, d + got, want - got);
			if (r <= 0) {
				f->flags |= r ? F_ERR : F_EOF;
				break;
			}
			got += (size_t)r;
			if (f->fd == 0)
				break;
			continue;
		}
		int c = fill(f);
		if (c == EOF)
			break;
		d[got++] = (unsigned char)c;
	}
	bdos_unlock(&f->lock);
	return got / size;
}

int fputc(int c, FILE *f)
{
	unsigned char ch = (unsigned char)c;
	return fwrite(&ch, 1, 1, f) ? ch : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
	size_t l = strlen(s);
	return !l || fwrite(s, l, 1, f) ? 0 : EOF;
}

int puts(const char *s)
{
	bdos_lock(&stdout->lock);
	int r = write_locked(stdout, s, strlen(s));
	if (!r)
		r = write_locked(stdout, "\n", 1);
	bdos_unlock(&stdout->lock);
	return r ? EOF : 0;
}

int fseek(FILE *f, long off, int whence)
{
	bdos_lock(&f->lock);
	if (f->wpos)
		flush_locked(f);
	if (whence == SEEK_CUR)
		off -= (long)(f->rend - f->rpos) + (f->unget >= 0);
	f->rpos = f->rend = 0;
	f->unget = -1;
	long r = lseek(f->fd, off, whence);
	if (r >= 0)
		f->flags &= ~F_EOF;
	bdos_unlock(&f->lock);
	return r < 0 ? -1 : 0;
}

long ftell(FILE *f)
{
	bdos_lock(&f->lock);
	long r = lseek(f->fd, 0, SEEK_CUR);
	if (r >= 0)
		r += (long)f->wpos - (long)(f->rend - f->rpos) - (f->unget >= 0);
	bdos_unlock(&f->lock);
	return r;
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); clearerr(f); }
int fgetpos(FILE *f, fpos_t *pos) { long r = ftell(f); if (r < 0) return -1; *pos = r; return 0; }
int fsetpos(FILE *f, const fpos_t *pos) { return fseek(f, *pos, SEEK_SET); }
int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE *f) { return f->fd; }

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
	fflush(f);
	f->mode = mode;
	if (mode == _IONBF)
		return 0;
	if (buf && size) {
		if (f->flags & F_OWNBUF)
			free(f->buf);
		f->flags &= ~F_OWNBUF;
		f->buf = (unsigned char *)buf;
		f->size = size;
	} else if (!f->buf) {
		f->buf = malloc(BUFSIZ);
		f->size = BUFSIZ;
		f->flags |= F_OWNBUF;
		if (!f->buf)
			f->mode = _IONBF;
	}
	return 0;
}

void setbuf(FILE *f, char *buf) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }

void perror(const char *s)
{
	if (s && *s)
		fprintf(stderr, "%s: %s\n", s, strerror(errno));
	else
		fprintf(stderr, "%s\n", strerror(errno));
}

static unsigned tmpcount;

char *tmpnam(char *s)
{
	static char own[L_tmpnam];
	if (!s)
		s = own;
	for (;;) {
		snprintf(s, L_tmpnam, "C:\\~TMP%05u.TMP", tmpcount++ % 100000);
		if (access(s, 0))
			return s;
	}
}

FILE *tmpfile(void)
{
	char name[L_tmpnam];
	return fopen(tmpnam(name), "w+");
}

int mkstemp(char *tmpl)
{
	size_t l = strlen(tmpl);
	if (l < 6 || strcmp(tmpl + l - 6, "XXXXXX")) {
		errno = EINVAL;
		return -1;
	}
	for (int tries = 0; tries < 1000; tries++) {
		unsigned v = (unsigned)bdos_us() ^ (tmpcount++ * 2654435761u);
		for (int i = 0; i < 6; i++) {
			tmpl[l - 6 + i] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[v % 36];
			v /= 36;
		}
		int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0 || errno != EEXIST)
			return fd;
	}
	return -1;
}

/* ---- printf ---- */

struct out {
	FILE *f;		/* or */
	char *s;
	size_t cap, len;	/* len counts everything, even what didn't fit */
	char tmp[256];
	size_t tlen;
};

static void out_flush(struct out *o)
{
	if (o->f && o->tlen) {
		write_locked(o->f, o->tmp, o->tlen);
		o->tlen = 0;
	}
}

static void out(struct out *o, const char *p, size_t n)
{
	if (o->f) {
		while (n) {
			size_t room = sizeof o->tmp - o->tlen;
			size_t k = n < room ? n : room;
			memcpy(o->tmp + o->tlen, p, k);
			o->tlen += k;
			p += k;
			n -= k;
			o->len += k;
			if (o->tlen == sizeof o->tmp)
				out_flush(o);
		}
		return;
	}
	for (size_t i = 0; i < n; i++, o->len++)
		if (o->len + 1 < o->cap)
			o->s[o->len] = p[i];
}

static void pad(struct out *o, char c, int n)
{
	char b[32];
	memset(b, c, sizeof b);
	while (n > 0) {
		int k = n < 32 ? n : 32;
		out(o, b, (size_t)k);
		n -= k;
	}
}

#define FL_MINUS 1
#define FL_PLUS 2
#define FL_SPACE 4
#define FL_HASH 8
#define FL_ZERO 16

/* the body (digits etc.) with its sign/prefix, padded to width */
static void emit(struct out *o, int fl, int width, const char *prefix, const char *body, size_t blen, int zeros)
{
	size_t pl = strlen(prefix);
	int total = (int)(pl + blen) + zeros;
	if (!(fl & FL_MINUS) && !(fl & FL_ZERO))
		pad(o, ' ', width - total);
	out(o, prefix, pl);
	if (!(fl & FL_MINUS) && (fl & FL_ZERO))
		pad(o, '0', width - total);
	pad(o, '0', zeros);
	out(o, body, blen);
	if (fl & FL_MINUS)
		pad(o, ' ', width - total);
}

static void fmt_int(struct out *o, unsigned long long v, int neg, int base, int upper, int fl, int width, int prec)
{
	char b[70];
	int n = 0;
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	while (v) {
		b[sizeof b - 1 - n++] = dig[v % (unsigned)base];
		v /= (unsigned)base;
	}
	char prefix[4] = "";
	int pi = 0;
	if (neg)
		prefix[pi++] = '-';
	else if (fl & FL_PLUS)
		prefix[pi++] = '+';
	else if (fl & FL_SPACE)
		prefix[pi++] = ' ';
	if ((fl & FL_HASH) && base == 16 && n) {
		prefix[pi++] = '0';
		prefix[pi++] = upper ? 'X' : 'x';
	}
	prefix[pi] = 0;
	int zeros = 0;
	if (prec >= 0) {
		fl &= ~FL_ZERO;
		if (n < prec)
			zeros = prec - n;
	} else if (!n) {
		zeros = 1;
	}
	if ((fl & FL_HASH) && base == 8 && !zeros && (!n || b[sizeof b - n] != '0'))
		zeros = 1;
	emit(o, fl, width, prefix, b + sizeof b - n, (size_t)n, zeros);
}

/* round the digit string d (n digits, value 0.d * 10^e) to keep digits; returns new n, may bump e */
static int round_digits(char *d, int n, int keep, int *e)
{
	if (keep >= n)
		return n;
	if (keep < 0) {
		d[0] = 0;
		return 0;
	}
	/* round half to even on the exact digits */
	int up;
	if (d[keep] > '5')
		up = 1;
	else if (d[keep] < '5')
		up = 0;
	else {
		up = 0;
		for (int i = keep + 1; i < n; i++)
			if (d[i] != '0') {
				up = 1;
				break;
			}
		if (!up)
			up = keep > 0 ? (d[keep - 1] - '0') & 1 : 0;
	}
	n = keep;
	if (up) {
		int i = n - 1;
		while (i >= 0 && d[i] == '9')
			d[i--] = '0';
		if (i >= 0)
			d[i]++;
		else {
			memmove(d + 1, d, (size_t)n);
			d[0] = '1';
			n++;
			(*e)++;
		}
		if (keep == 0 && i < 0) {
			/* rounded from nothing up to 1 */
		}
	}
	d[n] = 0;
	return n;
}

static void fmt_float(struct out *o, double v, int conv, int fl, int width, int prec)
{
	int upper = isupper(conv);
	conv = tolower(conv);
	char prefix[2] = "";
	if (signbit(v)) {
		prefix[0] = '-';
		v = -v;
	} else if (fl & FL_PLUS)
		prefix[0] = '+';
	else if (fl & FL_SPACE)
		prefix[0] = ' ';
	if (isnan(v) || isinf(v)) {
		const char *s = isnan(v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
		emit(o, fl & ~FL_ZERO, width, prefix, s, 3, 0);
		return;
	}
	if (prec < 0)
		prec = 6;
	char body[1200];
	size_t bl = 0;
	if (conv == 'a') {
		union { double d; uint64_t u; } u = { v };
		uint64_t mant = u.u & ((1ull << 52) - 1);
		int ex = (int)((u.u >> 52) & 0x7FF);
		int lead = ex ? 1 : 0;
		if (!ex && mant)
			ex = 1;
		ex = (ex || mant) ? ex - 1023 : 0;
		if (!lead && !mant)
			ex = 0;
		int p = prec == 6 && 0 ? 13 : prec;
		(void)p;
		char pfx[4] = { prefix[0], '0', upper ? 'X' : 'x', 0 };
		if (!prefix[0])
			memmove(pfx, pfx + 1, 3);
		const char *hx = upper ? "0123456789ABCDEF" : "0123456789abcdef";
		int nd = 13;
		while (nd > 0 && !((mant >> (4 * (13 - nd))) & 0xF))
			nd--;
		body[bl++] = (char)('0' + lead);
		if (nd || (fl & FL_HASH))
			body[bl++] = '.';
		for (int i = 0; i < nd; i++)
			body[bl++] = hx[(mant >> (48 - 4 * i)) & 0xF];
		bl += (size_t)snprintf(body + bl, sizeof body - bl, "%c%+d", upper ? 'P' : 'p', ex);
		emit(o, fl, width, pfx, body, bl, 0);
		return;
	}
	char d[1200];
	int e;
	int n = __dtoa_exact(v, d, sizeof d - 2, &e);
	int iszero = v == 0;
	if (conv == 'g') {
		int P = prec ? prec : 1;
		int e2 = e;
		char dd[1200];
		memcpy(dd, d, (size_t)n + 1);
		int nn = round_digits(dd, n, P, &e2);
		int X = iszero ? 0 : e2 - 1;
		if (P > X && X >= -4) {
			conv = 'f';
			prec = P - 1 - X;
		} else {
			conv = 'e';
			prec = P - 1;
		}
		(void)nn;
		if (!(fl & FL_HASH)) {
			/* produce, then strip trailing zeros */
			fl |= 0x100;
		}
	}
	if (conv == 'f') {
		int keep = e + prec;	/* digits kept */
		n = iszero ? 0 : round_digits(d, n, keep, &e);
		if (iszero)
			e = 1;
		/* integer part */
		if (e <= 0 || !n)
			body[bl++] = '0';
		else
			for (int i = 0; i < e; i++)
				body[bl++] = i < n ? d[i] : '0';
		if (prec || (fl & FL_HASH))
			body[bl++] = '.';
		for (int i = 0; i < prec; i++) {
			int idx = e + i;
			body[bl++] = (idx >= 0 && idx < n) ? d[idx] : '0';
		}
	} else {
		n = iszero ? 0 : round_digits(d, n, prec + 1, &e);
		body[bl++] = n ? d[0] : '0';
		if (prec || (fl & FL_HASH))
			body[bl++] = '.';
		for (int i = 1; i <= prec; i++)
			body[bl++] = i < n ? d[i] : '0';
		int x = iszero ? 0 : e - 1;
		if (fl & 0x100) {
			while (bl && body[bl - 1] == '0')
				bl--;
			if (bl && body[bl - 1] == '.')
				bl--;
			fl &= ~0x100;
		}
		body[bl++] = upper ? 'E' : 'e';
		body[bl++] = x < 0 ? '-' : '+';
		if (x < 0)
			x = -x;
		char eb[8];
		int en = 0;
		do {
			eb[en++] = (char)('0' + x % 10);
			x /= 10;
		} while (x);
		if (en < 2)
			eb[en++] = '0';
		while (en)
			body[bl++] = eb[--en];
	}
	if (fl & 0x100) {
		if (memchr(body, '.', bl)) {
			while (bl && body[bl - 1] == '0')
				bl--;
			if (bl && body[bl - 1] == '.')
				bl--;
		}
	}
	emit(o, fl & ~0x100, width, prefix, body, bl, 0);
}

static int vformat(struct out *o, const char *fmt, va_list ap)
{
	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			const char *q = p;
			while (*q && *q != '%')
				q++;
			out(o, p, (size_t)(q - p));
			p = q - 1;
			continue;
		}
		p++;
		int fl = 0;
		for (;; p++) {
			if (*p == '-') fl |= FL_MINUS;
			else if (*p == '+') fl |= FL_PLUS;
			else if (*p == ' ') fl |= FL_SPACE;
			else if (*p == '#') fl |= FL_HASH;
			else if (*p == '0') fl |= FL_ZERO;
			else break;
		}
		int width = 0, prec = -1;
		if (*p == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				fl |= FL_MINUS;
				width = -width;
			}
			p++;
		} else
			while (isdigit((unsigned char)*p))
				width = width * 10 + (*p++ - '0');
		if (*p == '.') {
			p++;
			prec = 0;
			if (*p == '*') {
				prec = va_arg(ap, int);
				if (prec < 0)
					prec = -1;
				p++;
			} else
				while (isdigit((unsigned char)*p))
					prec = prec * 10 + (*p++ - '0');
		}
		if (fl & FL_MINUS)
			fl &= ~FL_ZERO;
		int len = 0;	/* 1 hh, 2 h, 3 l, 4 ll, 5 z/t/j, 6 L */
		for (;; p++) {
			if (*p == 'h') len = len == 2 ? 1 : 2;
			else if (*p == 'l') len = len == 3 ? 4 : 3;
			else if (*p == 'z' || *p == 't' || *p == 'j' || *p == 'q') len = 5;
			else if (*p == 'L') len = 6;
			else break;
		}
		int c = *p;
		switch (c) {
		case 'd': case 'i': {
			long long v;
			if (len >= 3 && len != 6) v = len == 3 ? va_arg(ap, long) : va_arg(ap, long long);
			else v = va_arg(ap, int);
			if (len == 1) v = (signed char)v;
			else if (len == 2) v = (short)v;
			fmt_int(o, v < 0 ? -(unsigned long long)v : (unsigned long long)v, v < 0, 10, 0, fl, width, prec);
			break;
		}
		case 'u': case 'x': case 'X': case 'o': case 'b': {
			unsigned long long v;
			if (len >= 3 && len != 6) v = len == 3 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned long long);
			else v = va_arg(ap, unsigned);
			if (len == 1) v = (unsigned char)v;
			else if (len == 2) v = (unsigned short)v;
			fmt_int(o, v, 0, c == 'u' ? 10 : c == 'o' ? 8 : c == 'b' ? 2 : 16, c == 'X', fl & ~(FL_PLUS | FL_SPACE), width, prec);
			break;
		}
		case 'p': {
			void *v = va_arg(ap, void *);
			fmt_int(o, (uintptr_t)v, 0, 16, 0, (fl | FL_HASH) & ~(FL_PLUS | FL_SPACE), width, prec);
			break;
		}
		case 'c': {
			char ch = (char)va_arg(ap, int);
			emit(o, fl & ~FL_ZERO, width, "", &ch, 1, 0);
			break;
		}
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			size_t l = prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s);
			emit(o, fl & ~FL_ZERO, width, "", s, l, 0);
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
			double v = len == 6 ? (double)va_arg(ap, long double) : va_arg(ap, double);
			fmt_float(o, v, c, fl, width, prec);
			break;
		}
		case 'n': {
			void *v = va_arg(ap, void *);
			if (len == 1) *(signed char *)v = (signed char)o->len;
			else if (len == 2) *(short *)v = (short)o->len;
			else if (len >= 3 && len != 6) *(long *)v = (long)o->len;
			else *(int *)v = (int)o->len;
			break;
		}
		case '%':
			out(o, "%", 1);
			break;
		case 0:
			p--;
			break;
		default:
			out(o, "%", 1);
			out(o, p, 1);
		}
	}
	return (int)o->len;
}

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
	struct out o;
	o.f = NULL;
	o.s = s;
	o.cap = n;
	o.len = 0;
	o.tlen = 0;
	int r = vformat(&o, fmt, ap);
	if (n)
		s[o.len < n ? o.len : n - 1] = 0;
	return r;
}

int vsprintf(char *s, const char *fmt, va_list ap) { return vsnprintf(s, (size_t)INT_MAX, fmt, ap); }

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
	struct out o;
	o.f = f;
	o.s = NULL;
	o.cap = 0;
	o.len = 0;
	o.tlen = 0;
	bdos_lock(&f->lock);
	int r = vformat(&o, fmt, ap);
	out_flush(&o);
	int err = (f->flags & F_ERR) != 0;
	bdos_unlock(&f->lock);
	return err ? -1 : r;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vfprintf(stdout, fmt, ap);
	va_end(ap);
	return r;
}

int fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vfprintf(f, fmt, ap);
	va_end(ap);
	return r;
}

int sprintf(char *s, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vsprintf(s, fmt, ap);
	va_end(ap);
	return r;
}

int snprintf(char *s, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(s, n, fmt, ap);
	va_end(ap);
	return r;
}

int dprintf(int fd, const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	write(fd, b, strlen(b));
	return r;
}

/* ---- scanf ---- */

struct in {
	FILE *f;
	const char *s;
	int count;		/* characters consumed */
	int eof;
};

static int in_get(struct in *i)
{
	int c;
	if (i->f)
		c = getc_locked(i->f);
	else
		c = *i->s ? (unsigned char)*i->s++ : EOF;
	if (c == EOF)
		i->eof = 1;
	else
		i->count++;
	return c;
}

static void in_unget(struct in *i, int c)
{
	if (c == EOF)
		return;
	i->count--;
	if (i->f) {
		if (i->f->rpos > 0 && i->f->unget < 0 && i->f->rpos <= i->f->rend && i->f->buf[i->f->rpos - 1] == (unsigned char)c)
			i->f->rpos--;
		else
			i->f->unget = c;
	} else
		i->s--;
}

static void store_int(void *p, int len, unsigned long long v)
{
	switch (len) {
	case 1: *(char *)p = (char)v; break;
	case 2: *(short *)p = (short)v; break;
	case 3: case 5: *(long *)p = (long)v; break;
	case 4: *(long long *)p = (long long)v; break;
	default: *(int *)p = (int)v;
	}
}

static int vscan(struct in *in, const char *fmt, va_list ap)
{
	int assigned = 0, matched_any = 0;
	for (const char *p = fmt; *p; p++) {
		if (isspace((unsigned char)*p)) {
			int c;
			while (isspace(c = in_get(in)))
				;
			in_unget(in, c);
			continue;
		}
		if (*p != '%' || p[1] == '%') {
			if (*p == '%')
				p++;
			int c = in_get(in);
			if (*p == '%')
				while (isspace(c))
					c = in_get(in);
			if (c != (unsigned char)*p) {
				in_unget(in, c);
				goto done;
			}
			continue;
		}
		p++;
		int skip = 0;
		if (*p == '*') {
			skip = 1;
			p++;
		}
		int width = 0;
		while (isdigit((unsigned char)*p))
			width = width * 10 + (*p++ - '0');
		int len = 0;
		for (;; p++) {
			if (*p == 'h') len = len == 2 ? 1 : 2;
			else if (*p == 'l') len = len == 3 ? 4 : 3;
			else if (*p == 'z' || *p == 't' || *p == 'j') len = 5;
			else if (*p == 'L') len = 6;
			else break;
		}
		int conv = *p;
		if (!conv)
			break;
		if (conv == 'n') {
			if (!skip)
				store_int(va_arg(ap, void *), len, (unsigned long long)in->count);
			continue;
		}
		if (conv != 'c' && conv != '[') {
			int c;
			while (isspace(c = in_get(in)))
				;
			in_unget(in, c);
		}
		if (!width)
			width = conv == 'c' ? 1 : INT_MAX;
		char buf[512];
		int n = 0;
		if (conv == 'c' || conv == 's' || conv == '[') {
			char set[256];
			int negate = 0;
			if (conv == '[') {
				memset(set, 0, sizeof set);
				p++;
				if (*p == '^') {
					negate = 1;
					p++;
				}
				if (*p == ']') {
					set[']'] = 1;
					p++;
				}
				for (; *p && *p != ']'; p++) {
					if (p[1] == '-' && p[2] && p[2] != ']') {
						for (int ch = (unsigned char)p[0]; ch <= (unsigned char)p[2]; ch++)
							set[ch] = 1;
						p += 2;
					} else
						set[(unsigned char)*p] = 1;
				}
			}
			char *dst = skip ? NULL : va_arg(ap, char *);
			int c = 0;
			while (n < width) {
				c = in_get(in);
				if (c == EOF)
					break;
				if (conv == 's' && isspace(c))
					break;
				if (conv == '[' && !(set[c] ^ negate))
					break;
				if (dst)
					dst[n] = (char)c;
				n++;
			}
			if (n < width && c != EOF)
				in_unget(in, c);
			if (!n)
				goto done;
			if (dst && conv != 'c')
				dst[n] = 0;
			if (!skip)
				assigned++;
			matched_any = 1;
			continue;
		}
		/* numbers: gather the characters, then convert */
		int c = in_get(in);
		int isfloat = strchr("eEfFgGaA", conv) != NULL;
		int base = conv == 'd' || conv == 'u' ? 10 : conv == 'o' ? 8 : conv == 'x' || conv == 'X' || conv == 'p' ? 16 : 0;
		if ((c == '+' || c == '-') && n < width) {
			buf[n++] = (char)c;
			c = in_get(in);
		}
		if (isfloat) {
			int dot = 0, exp = 0, hex = 0;
			if (c == '0' && n < width) {
				buf[n++] = (char)c;
				c = in_get(in);
				if ((c == 'x' || c == 'X') && n < width) {
					hex = 1;
					buf[n++] = (char)c;
					c = in_get(in);
				}
			}
			while (n < width && n < (int)sizeof buf - 1 && c != EOF) {
				if ((hex ? isxdigit(c) : isdigit(c)) && !exp)
					;
				else if (isdigit(c) && exp)
					;
				else if (c == '.' && !dot && !exp)
					dot = 1;
				else if (!exp && (hex ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E'))) {
					exp = 1;
					buf[n++] = (char)c;
					c = in_get(in);
					if ((c == '+' || c == '-') && n < width) {
						buf[n++] = (char)c;
						c = in_get(in);
					}
					continue;
				} else if (!n || ((buf[0] == '+' || buf[0] == '-') && n == 1)) {
					/* inf / nan */
					if (tolower(c) == 'i' || tolower(c) == 'n') {
						while (n < width && n < 12 && isalpha(c)) {
							buf[n++] = (char)c;
							c = in_get(in);
						}
					}
					break;
				} else
					break;
				buf[n++] = (char)c;
				c = in_get(in);
			}
			in_unget(in, c);
			buf[n] = 0;
			char *end;
			double v = strtod(buf, &end);
			if (end == buf)
				goto done;
			if (!skip) {
				void *dst = va_arg(ap, void *);
				if (len == 3) *(double *)dst = v;
				else if (len == 6) *(long double *)dst = v;
				else *(float *)dst = (float)v;
				assigned++;
			}
			matched_any = 1;
			continue;
		}
		if (base == 0 || base == 16) {
			if (c == '0' && n < width) {
				buf[n++] = (char)c;
				c = in_get(in);
				if ((c == 'x' || c == 'X') && n < width) {
					buf[n++] = (char)c;
					c = in_get(in);
					base = 16;
				} else if (!base)
					base = 8;
			} else if (!base)
				base = 10;
		}
		while (n < width && n < (int)sizeof buf - 1 && c != EOF &&
		       (base == 16 ? isxdigit(c) : base == 8 ? (c >= '0' && c <= '7') : isdigit(c))) {
			buf[n++] = (char)c;
			c = in_get(in);
		}
		in_unget(in, c);
		buf[n] = 0;
		char *end;
		unsigned long long v = conv == 'd' || conv == 'i' ? (unsigned long long)strtoll(buf, &end, base) : strtoull(buf, &end, base);
		if (end == buf || (end[-1] == 'x' || end[-1] == 'X'))
			if (end == buf)
				goto done;
		if (!skip) {
			void *dst = va_arg(ap, void *);
			if (conv == 'p')
				*(void **)dst = (void *)(uintptr_t)v;
			else
				store_int(dst, len, v);
			assigned++;
		}
		matched_any = 1;
	}
done:
	if (!matched_any && in->eof && !assigned)
		return EOF;
	return assigned;
}

int vsscanf(const char *s, const char *fmt, va_list ap)
{
	struct in in = { NULL, s, 0, 0 };
	return vscan(&in, fmt, ap);
}

int vfscanf(FILE *f, const char *fmt, va_list ap)
{
	struct in in = { f, NULL, 0, 0 };
	bdos_lock(&f->lock);
	int r = vscan(&in, fmt, ap);
	bdos_unlock(&f->lock);
	return r;
}

int sscanf(const char *s, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vsscanf(s, fmt, ap);
	va_end(ap);
	return r;
}

int fscanf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vfscanf(f, fmt, ap);
	va_end(ap);
	return r;
}

int scanf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vfscanf(stdin, fmt, ap);
	va_end(ap);
	return r;
}
