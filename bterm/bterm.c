/*
 * bterm - a drawterm for bdos.
 *
 *   bterm [options] host[:port]
 *
 * Opens a bdos machine's desktop in a window (or, with -t, a shell in this
 * terminal), and offers the directory you started from as drive T: over
 * there - the way drawterm's /mnt/term works with Plan 9.
 *
 * It speaks BDNET, the protocol bdos machines use between themselves,
 * carried in UDP (port 17979) so it works from any Linux or macOS machine,
 * through QEMU's user networking, and across routers.
 *
 * Build: make (needs SDL2 for the desktop; `make text` builds -t only).
 */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifndef NO_SDL
#include <SDL.h>
#endif

#define BTERM_VERSION "1.0"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"	/* names are cut short on purpose */
#endif
#define UNUSED __attribute__((unused))

/* ---- the protocol: must match kernel/net/net.c and remote.c ------------------------------ */

#define ETHERTYPE	0x88B5
#define MAGIC		0xBD05
#define MAX_PAYLOAD	1400
#define WINDOW		32
#define RTO_MS		200
#define DEFAULT_PORT	17979

enum { F_WHO = 1, F_IAM, F_SYN, F_SYNACK, F_DATA, F_ACK, F_FIN, F_RST };

struct hdr {
	uint8_t dst[6], src[6];
	uint16_t ethertype;	/* big endian */
	uint16_t magic;
	uint8_t type, flags;
	uint32_t conn, seq, ack;
	uint16_t len;
} __attribute__((packed));

enum { SVC_TTY = 'T', SVC_DESK = 'D', SVC_FILE = 'F' };
enum { M_KEY = 1, M_OUT, M_CTRL, M_EV, M_PIX, M_FREQ, M_FREP, M_BYE };
enum { C_CLEAR = 1, C_GOTO, C_ATTR, C_CURSOR };
enum { F_STAT = 1, F_LIST, F_READ, F_WRITE, F_CREATE, F_TRUNC, F_REMOVE, F_RENAME, F_SETATTR, F_INFO };

#define HELLO_SHARE	1
#define HELLO_POINTER	2

struct hello {
	int32_t w, h;
	uint8_t flags;
	char host[32];
	char share[128];
} __attribute__((packed));

struct freq {
	uint32_t id;
	uint8_t op, arg;
	uint16_t pad;
	uint64_t off;
	uint32_t n;
} __attribute__((packed));

struct frep {
	uint32_t id;
	int32_t result;
} __attribute__((packed));

#define VFS_NAME_MAX 234
enum { VFS_FILE = 1, VFS_DIR = 2 };
#define A_RO		0x01
#define A_HIDDEN	0x02
#define A_DIR		0x10
#define A_ARCHIVE	0x20

struct wire_stat {
	uint8_t type, attr;
	uint64_t size, mtime;
	char short_name[13];
	char name[VFS_NAME_MAX];
} __attribute__((packed));

enum {
	E_NOENT = -2, E_EXIST = -3, E_NOTDIR = -4, E_ISDIR = -5, E_NOSPC = -6, E_NOTEMPTY = -7,
	E_INVAL = -8, E_IO = -9, E_NAMETOOLONG = -11, E_ACCES = -12, E_NOMEM = -13, E_NOTSUP = -21,
};

/* bdos key codes */
enum {
	KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
	KEY_DEL, KEY_INS, KEY_SHIFT_TAB, KEY_F1 = 0x120, KEY_ALT = 0x1000,
};
enum { EV_KEY = 1, EV_MOVE, EV_DOWN, EV_UP };

/* ---- little helpers -------------------------------------------------------------------- */

static int verbose;

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "bterm: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		die("out of memory");
	return q;
}

/* ---- the connection: BDNET over UDP --------------------------------------------------- */

static int sock = -1;
static struct sockaddr_in peer;
static char password[64];

struct txframe {
	uint32_t seq;
	uint16_t len;
	uint8_t data[MAX_PAYLOAD];
};

static struct {
	int state;		/* 1 connecting, 2 open, 3 closed */
	uint32_t id;
	uint8_t syn[1 + sizeof(struct hello) + 64];
	size_t synlen;
	uint8_t *out;
	size_t out_len, out_cap;
	struct txframe win[WINDOW];
	int nwin;
	uint32_t next_seq, expect;
	uint64_t last_send;
	int retries;
	uint8_t *in;
	size_t in_len, in_cap;
	const char *why;	/* why it closed */
} c;

static void udp_out(int type, uint32_t seq, uint32_t ack, const void *payload, size_t len)
{
	uint8_t buf[6 + 64 + sizeof(struct hdr) + MAX_PAYLOAD];
	size_t pl = strlen(password);
	memcpy(buf, "BDNU", 4);
	buf[4] = 1;
	buf[5] = (uint8_t)pl;
	memcpy(buf + 6, password, pl);
	struct hdr h;
	memset(&h, 0, sizeof h);
	h.ethertype = htons(ETHERTYPE);
	h.magic = MAGIC;
	h.type = (uint8_t)type;
	h.conn = c.id;
	h.seq = seq;
	h.ack = ack;
	h.len = (uint16_t)len;
	memcpy(buf + 6 + pl, &h, sizeof h);
	if (len)
		memcpy(buf + 6 + pl + sizeof h, payload, len);
	sendto(sock, buf, 6 + pl + sizeof h + len, 0, (struct sockaddr *)&peer, sizeof peer);
}

static void pump_out(void)
{
	while (c.state == 2 && c.nwin < WINDOW && c.out_len) {
		struct txframe *f = &c.win[c.nwin++];
		size_t n = c.out_len < MAX_PAYLOAD ? c.out_len : MAX_PAYLOAD;
		f->seq = c.next_seq++;
		f->len = (uint16_t)n;
		memcpy(f->data, c.out, n);
		memmove(c.out, c.out + n, c.out_len - n);
		c.out_len -= n;
		udp_out(F_DATA, f->seq, c.expect, f->data, n);
		c.last_send = now_ms();
	}
}

static void send_msg2(int type, const void *a, size_t alen, const void *b, size_t blen)
{
	if (c.state != 2)
		return;
	size_t need = c.out_len + 5 + alen + blen;
	if (need > c.out_cap) {
		c.out_cap = need * 2 + 4096;
		c.out = xrealloc(c.out, c.out_cap);
	}
	uint32_t len = (uint32_t)(alen + blen);
	memcpy(c.out + c.out_len, &len, 4);
	c.out[c.out_len + 4] = (uint8_t)type;
	if (alen)
		memcpy(c.out + c.out_len + 5, a, alen);
	if (blen)
		memcpy(c.out + c.out_len + 5 + alen, b, blen);
	c.out_len += 5 + alen + blen;
	pump_out();
}

static void send_msg(int type, const void *a, size_t alen)
{
	send_msg2(type, a, alen, NULL, 0);
}

static void handle_ack(uint32_t ack)
{
	int drop = 0;
	while (drop < c.nwin && (int32_t)(c.win[drop].seq - ack) < 0)
		drop++;
	if (drop) {
		memmove(c.win, c.win + drop, sizeof(struct txframe) * (size_t)(c.nwin - drop));
		c.nwin -= drop;
		c.retries = 0;
		c.last_send = now_ms();
	}
}

static void on_message(int type, const uint8_t *data, uint32_t len);

static void parse_stream(void)
{
	size_t off = 0;
	while (c.in_len - off >= 5) {
		uint32_t len;
		memcpy(&len, c.in + off, 4);
		if (c.in_len - off < 5 + (size_t)len)
			break;
		int type = c.in[off + 4];
		/* hand over a copy with a terminating NUL, like bdos does */
		uint8_t *m = malloc((size_t)len + 1);
		if (!m)
			die("out of memory");
		memcpy(m, c.in + off + 5, len);
		m[len] = 0;
		off += 5 + len;
		on_message(type, m, len);
		free(m);
		if (c.state != 2)
			break;
	}
	memmove(c.in, c.in + off, c.in_len - off);
	c.in_len -= off;
}

static void net_input(void)
{
	uint8_t buf[2048];
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		ssize_t n = recvfrom(sock, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		if (n < 0)
			return;
		if (n < 6 || memcmp(buf, "BDNU", 4))
			continue;
		if (buf[4] == 0xFF) {
			c.state = 3;
			c.why = password[0] ? "the password was wrong" : "that machine wants a password (use -p)";
			return;
		}
		size_t skip = 6 + buf[5];
		if ((size_t)n < skip + sizeof(struct hdr))
			continue;
		struct hdr h;
		memcpy(&h, buf + skip, sizeof h);
		const uint8_t *p = buf + skip + sizeof h;
		if (ntohs(h.ethertype) != ETHERTYPE || h.magic != MAGIC || h.len > n - skip - sizeof h)
			continue;
		if (h.conn != c.id)
			continue;
		switch (h.type) {
		case F_SYNACK:
			if (c.state == 1) {
				c.state = 2;
				c.retries = 0;
			}
			break;
		case F_DATA:
			handle_ack(h.ack);
			if (h.seq == c.expect) {
				if (c.in_len + h.len > c.in_cap) {
					c.in_cap = (c.in_len + h.len) * 2 + 4096;
					c.in = xrealloc(c.in, c.in_cap);
				}
				memcpy(c.in + c.in_len, p, h.len);
				c.in_len += h.len;
				c.expect++;
				udp_out(F_ACK, 0, c.expect, NULL, 0);
				parse_stream();
			} else {
				udp_out(F_ACK, 0, c.expect, NULL, 0);
			}
			break;
		case F_ACK:
			handle_ack(h.ack);
			break;
		case F_FIN:
		case F_RST:
			if (c.state != 3) {
				c.state = 3;
				c.why = "the other end hung up";
				if (h.type == F_FIN)
					udp_out(F_RST, 0, 0, NULL, 0);
			}
			break;
		}
	}
}

static void net_timers(void)
{
	uint64_t t = now_ms();
	if (c.state == 1 && t - c.last_send > 300) {
		udp_out(F_SYN, 0, 0, c.syn, c.synlen);
		c.last_send = t;
		if (++c.retries > 15) {
			c.state = 3;
			c.why = "no answer (is net.udp on, and the port forwarded?)";
		}
	}
	if (c.state != 2)
		return;
	pump_out();
	if (c.nwin && t - c.last_send > RTO_MS) {
		for (int k = 0; k < c.nwin; k++)
			udp_out(F_DATA, c.win[k].seq, c.expect, c.win[k].data, c.win[k].len);
		c.last_send = t;
		if (++c.retries > 50) {
			c.state = 3;
			c.why = "the connection went quiet";
		}
	}
}

/* wait for the network up to ms, handling whatever arrives */
static void net_poll(int ms, int extra_fd, int *extra_ready)
{
	struct pollfd p[2] = { { sock, POLLIN, 0 }, { extra_fd, POLLIN, 0 } };
	int n = poll(p, extra_fd >= 0 ? 2 : 1, ms);
	if (extra_ready)
		*extra_ready = n > 0 && extra_fd >= 0 && (p[1].revents & (POLLIN | POLLHUP));
	if (n > 0 && (p[0].revents & POLLIN))
		net_input();
	net_timers();
}

static void conn_open(int svc, const struct hello *h)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	srandom((unsigned)(tv.tv_usec ^ tv.tv_sec ^ getpid()));
	c.id = ((uint32_t)random() << 1 | (uint32_t)random()) | 1;
	c.syn[0] = (uint8_t)svc;
	memcpy(c.syn + 1, h, sizeof *h);
	c.synlen = 1 + sizeof *h;
	c.state = 1;
	udp_out(F_SYN, 0, 0, c.syn, c.synlen);
	c.last_send = now_ms();
	while (c.state == 1)
		net_poll(50, -1, NULL);
	if (c.state != 2)
		die("can't connect: %s", c.why ? c.why : "no answer");
}

static void conn_close(void)
{
	if (c.state == 2) {
		send_msg(M_BYE, NULL, 0);
		uint64_t until = now_ms() + 1500;
		while ((c.out_len || c.nwin) && c.state == 2 && now_ms() < until)
			net_poll(20, -1, NULL);
		udp_out(F_FIN, 0, 0, NULL, 0);
	}
}

static void resolve(const char *arg, struct sockaddr_in *sa)
{
	char host[256];
	snprintf(host, sizeof host, "%s", arg);
	int port = DEFAULT_PORT;
	char *colon = strrchr(host, ':');
	if (colon) {
		*colon = 0;
		port = atoi(colon + 1);
	}
	char *h = host;
	while (*h == '\\' || *h == '/')
		h++;
	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	int e = getaddrinfo(h, NULL, &hints, &res);
	if (e)
		die("%s: %s", h, gai_strerror(e));
	memcpy(sa, res->ai_addr, sizeof *sa);
	sa->sin_port = htons((uint16_t)port);
	freeaddrinfo(res);
}

/* ---- T: - serving a local directory -------------------------------------------------------- */

static char share[PATH_MAX];
static int sharing = 1;

static int err_code(int e)
{
	switch (e) {
	case ENOENT: return E_NOENT;
	case EEXIST: return E_EXIST;
	case ENOTDIR: return E_NOTDIR;
	case EISDIR: return E_ISDIR;
	case ENOSPC: return E_NOSPC;
	case ENOTEMPTY: return E_NOTEMPTY;
	case EINVAL: return E_INVAL;
	case ENAMETOOLONG: return E_NAMETOOLONG;
	case EACCES: case EPERM: case EROFS: return E_ACCES;
	case ENOMEM: return E_NOMEM;
	}
	return E_IO;
}

/*
 * Map a bdos path ("GAMES\\Lander.bas") to a host path under the share.
 * DOS names aren't case-sensitive and people type them in capitals, so
 * each part is matched without regard to case when there's no exact match.
 */
static int map_path(const char *rel, char *out, size_t n)
{
	snprintf(out, n, "%s", share);
	const char *p = rel;
	while (*p) {
		while (*p == '/' || *p == '\\')
			p++;
		if (!*p)
			break;
		char part[NAME_MAX + 1];
		size_t l = 0;
		while (*p && *p != '/' && *p != '\\' && l + 1 < sizeof part)
			part[l++] = *p++;
		part[l] = 0;
		if (!strcmp(part, "..") || !strcmp(part, "."))
			return E_ACCES;	/* stay inside what was shared */
		size_t ol = strlen(out);
		if (ol + 1 + l + 1 > n)
			return E_NAMETOOLONG;
		char try[PATH_MAX];
		snprintf(try, sizeof try, "%s/%s", out, part);
		struct stat st;
		if (lstat(try, &st) != 0) {
			DIR *d = opendir(out);
			if (d) {
				struct dirent *de;
				while ((de = readdir(d)))
					if (!strcasecmp(de->d_name, part)) {
						snprintf(try, sizeof try, "%s/%s", out, de->d_name);
						break;
					}
				closedir(d);
			}
		}
		snprintf(out, n, "%s", try);
	}
	return 0;
}

static void short_name(const char *name, char *out)
{
	/* an 8.3 alias, near enough for DIR's short-name column */
	const char *dot = strrchr(name, '.');
	if (dot == name)
		dot = NULL;
	size_t base = dot ? (size_t)(dot - name) : strlen(name);
	char b[9] = "", e[4] = "";
	size_t bl = 0, el = 0;
	for (size_t i = 0; i < base && bl < 8; i++)
		if (isalnum((unsigned char)name[i]) || strchr("_-$~!#%&", name[i]))
			b[bl++] = (char)toupper((unsigned char)name[i]);
	b[bl] = 0;
	if (dot)
		for (const char *q = dot + 1; *q && el < 3; q++)
			if (isalnum((unsigned char)*q))
				e[el++] = (char)toupper((unsigned char)*q);
	e[el] = 0;
	if (base > 8 && bl >= 6) {
		b[6] = '~';
		b[7] = '1';
		b[8] = 0;
	}
	snprintf(out, 13, el ? "%s.%s" : "%s", b, e);
}

static long local_offset(time_t t)
{
	struct tm lt;
	localtime_r(&t, &lt);
#if defined(__APPLE__) || defined(__linux__)
	return lt.tm_gmtoff;
#else
	return 0;
#endif
}

static void fill_stat(const char *name, const struct stat *st, struct wire_stat *w)
{
	memset(w, 0, sizeof *w);
	w->type = S_ISDIR(st->st_mode) ? VFS_DIR : VFS_FILE;
	w->attr = S_ISDIR(st->st_mode) ? A_DIR : A_ARCHIVE;
	if (!(st->st_mode & S_IWUSR))
		w->attr |= A_RO;
	if (name[0] == '.')
		w->attr |= A_HIDDEN;
	w->size = S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size;
	w->mtime = (uint64_t)(st->st_mtime + local_offset(st->st_mtime));	/* bdos keeps local time */
	snprintf(w->name, sizeof w->name, "%s", name);
	short_name(name, w->short_name);
}

static void reply(uint32_t id, int32_t result, const void *data, size_t len)
{
	struct frep r = { id, result };
	send_msg2(M_FREP, &r, sizeof r, data, len);
}

static void serve_file(const uint8_t *data, uint32_t len)
{
	struct freq q;
	if (len < sizeof q)
		return;
	memcpy(&q, data, sizeof q);
	if (!sharing) {
		reply(q.id, E_ACCES, NULL, 0);
		return;
	}
	const char *path = (const char *)data + sizeof q;
	size_t plen = strnlen(path, len - sizeof q);
	const uint8_t *extra = (const uint8_t *)path + plen + 1;
	size_t elen = len > sizeof q + plen + 1 ? len - sizeof q - plen - 1 : 0;
	char abs[PATH_MAX], abs2[PATH_MAX];
	int r = map_path(path, abs, sizeof abs);
	if (r) {
		reply(q.id, r, NULL, 0);
		return;
	}
	if (verbose)
		fprintf(stderr, "bterm: T: op %d %s\r\n", q.op, abs);
	struct stat st;
	switch (q.op) {
	case F_STAT: {
		if (stat(abs, &st)) {
			reply(q.id, err_code(errno), NULL, 0);
			return;
		}
		struct wire_stat w;
		const char *base = strrchr(abs, '/');
		fill_stat(*path ? (base ? base + 1 : abs) : "", &st, &w);
		reply(q.id, 0, &w, sizeof w);
		return;
	}
	case F_LIST: {
		DIR *d = opendir(abs);
		if (!d) {
			reply(q.id, err_code(errno), NULL, 0);
			return;
		}
		struct wire_stat *ws = NULL;
		int count = 0;
		struct dirent *de;
		while ((de = readdir(d))) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			char full[PATH_MAX];
			snprintf(full, sizeof full, "%s/%s", abs, de->d_name);
			if (stat(full, &st))
				continue;
			if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
				continue;	/* sockets, devices... */
			ws = xrealloc(ws, sizeof *ws * (size_t)(count + 1));
			fill_stat(de->d_name, &st, &ws[count++]);
		}
		closedir(d);
		reply(q.id, count, ws, sizeof *ws * (size_t)count);
		free(ws);
		return;
	}
	case F_READ: {
		uint32_t n = q.n > 64 * 1024 ? 64 * 1024 : q.n;
		int fd = open(abs, O_RDONLY);
		if (fd < 0) {
			reply(q.id, err_code(errno), NULL, 0);
			return;
		}
		uint8_t *buf = malloc(n ? n : 1);
		ssize_t got = buf ? pread(fd, buf, n, (off_t)q.off) : -1;
		int e = errno;
		close(fd);
		reply(q.id, got < 0 ? err_code(e) : (int32_t)got, buf, got > 0 ? (size_t)got : 0);
		free(buf);
		return;
	}
	case F_WRITE: {
		int fd = open(abs, O_WRONLY | O_CREAT, 0644);
		if (fd < 0) {
			reply(q.id, err_code(errno), NULL, 0);
			return;
		}
		ssize_t w = pwrite(fd, extra, elen, (off_t)q.off);
		int e = errno;
		close(fd);
		reply(q.id, w < 0 ? err_code(e) : (int32_t)w, NULL, 0);
		return;
	}
	case F_CREATE:
		if (q.arg == VFS_DIR) {
			r = mkdir(abs, 0755) ? err_code(errno) : 0;
		} else {
			int fd = open(abs, O_WRONLY | O_CREAT, 0644);
			r = fd < 0 ? err_code(errno) : 0;
			if (fd >= 0) {
				if (!fstat(fd, &st) && st.st_size)
					r = E_EXIST;	/* create means new, as on bdos */
				close(fd);
			}
		}
		break;
	case F_TRUNC:
		r = truncate(abs, (off_t)q.off) ? err_code(errno) : 0;
		break;
	case F_REMOVE:
		if (stat(abs, &st))
			r = err_code(errno);
		else
			r = (S_ISDIR(st.st_mode) ? rmdir(abs) : unlink(abs)) ? err_code(errno) : 0;
		break;
	case F_RENAME:
		r = map_path(elen ? (const char *)extra : "", abs2, sizeof abs2);
		if (!r)
			r = rename(abs, abs2) ? err_code(errno) : 0;
		break;
	case F_SETATTR:
		if (stat(abs, &st))
			r = err_code(errno);
		else
			r = chmod(abs, (q.arg & A_RO) ? (st.st_mode & ~0222u) : (st.st_mode | S_IWUSR)) ?
			    err_code(errno) : 0;
		break;
	case F_INFO: {
		uint8_t info[48];
		memset(info, 0, sizeof info);
		const char *base = strrchr(share, '/');
		snprintf((char *)info, 32, "%s", base && base[1] ? base + 1 : share);
		struct statvfs sv;
		uint64_t fr = 0, tot = 0;
		if (!statvfs(share, &sv)) {
			fr = (uint64_t)sv.f_bavail * sv.f_frsize;
			tot = (uint64_t)sv.f_blocks * sv.f_frsize;
		}
		memcpy(info + 32, &fr, 8);
		memcpy(info + 40, &tot, 8);
		reply(q.id, 0, info, sizeof info);
		return;
	}
	default:
		r = E_NOTSUP;
	}
	reply(q.id, r, NULL, 0);
}

/* ---- text mode: a bdos shell in this terminal ------------------------------------------------ */

static struct termios saved_tio;
static int raw_on;
static int text_mode;

static void raw_off(void)
{
	if (raw_on) {
		tcsetattr(0, TCSAFLUSH, &saved_tio);
		raw_on = 0;
		fputs("\033[0m\033[?25h", stdout);
		fflush(stdout);
	}
}

static void raw_mode(void)
{
	if (!isatty(0))
		return;
	tcgetattr(0, &saved_tio);
	struct termios t = saved_tio;
	t.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	t.c_oflag &= ~(unsigned)OPOST;
	t.c_cflag |= CS8;
	t.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSAFLUSH, &t);
	raw_on = 1;
	atexit(raw_off);
}

/* code page 437's top half, so EDIT's boxes and bdos's lines look right */
static const char *const cp437[128] = {
	"Ç", "ü", "é", "â", "ä", "à", "å", "ç", "ê", "ë", "è", "ï", "î", "ì", "Ä", "Å",
	"É", "æ", "Æ", "ô", "ö", "ò", "û", "ù", "ÿ", "Ö", "Ü", "¢", "£", "¥", "₧", "ƒ",
	"á", "í", "ó", "ú", "ñ", "Ñ", "ª", "º", "¿", "⌐", "¬", "½", "¼", "¡", "«", "»",
	"░", "▒", "▓", "│", "┤", "╡", "╢", "╖", "╕", "╣", "║", "╗", "╝", "╜", "╛", "┐",
	"└", "┴", "┬", "├", "─", "┼", "╞", "╟", "╚", "╔", "╩", "╦", "╠", "═", "╬", "╧",
	"╨", "╤", "╥", "╙", "╘", "╒", "╓", "╫", "╪", "┘", "┌", "█", "▄", "▌", "▐", "▀",
	"α", "ß", "Γ", "π", "Σ", "σ", "µ", "τ", "Φ", "Θ", "Ω", "δ", "∞", "φ", "ε", "∩",
	"≡", "±", "≥", "≤", "⌠", "⌡", "÷", "≈", "°", "∙", "·", "√", "ⁿ", "²", "■", " ",
};

static void tty_out(const uint8_t *s, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		uint8_t ch = s[i];
		if (ch == '\n')
			fputs("\r\n", stdout);
		else if (ch >= 0x80)
			fputs(cp437[ch - 0x80], stdout);
		else
			fputc(ch, stdout);
	}
	fflush(stdout);
}

static void tty_ctrl(const int32_t *v)
{
	static const int ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };	/* CGA order -> ANSI order */
	switch (v[0]) {
	case C_CLEAR:
		fputs("\033[0m\033[2J\033[H", stdout);
		break;
	case C_GOTO:
		printf("\033[%d;%dH", v[2] + 1, v[1] + 1);
		break;
	case C_ATTR: {
		int fg = v[1] & 15, bg = v[2] & 15;
		if (fg == 7 && bg == 0)
			fputs("\033[0m", stdout);	/* the usual: let the terminal's colours show */
		else
			printf("\033[0;%d;%dm", (fg >= 8 ? 90 : 30) + ansi[fg & 7], (bg >= 8 ? 100 : 40) + ansi[bg & 7]);
		break;
	}
	case C_CURSOR:
		fputs(v[1] ? "\033[?25h" : "\033[?25l", stdout);
		break;
	}
	fflush(stdout);
}

static void send_key(int k)
{
	int32_t key = k;
	send_msg(M_KEY, &key, sizeof key);
}

/* Turn terminal input (with its escape sequences) into bdos keys. */
static int tty_keys(const uint8_t *b, size_t n, int *hangup)
{
	static const struct { const char *seq; int key; } seqs[] = {
		{ "[A", KEY_UP }, { "[B", KEY_DOWN }, { "[C", KEY_RIGHT }, { "[D", KEY_LEFT },
		{ "OA", KEY_UP }, { "OB", KEY_DOWN }, { "OC", KEY_RIGHT }, { "OD", KEY_LEFT },
		{ "[H", KEY_HOME }, { "[F", KEY_END }, { "OH", KEY_HOME }, { "OF", KEY_END },
		{ "[1~", KEY_HOME }, { "[4~", KEY_END }, { "[7~", KEY_HOME }, { "[8~", KEY_END },
		{ "[5~", KEY_PGUP }, { "[6~", KEY_PGDN }, { "[3~", KEY_DEL }, { "[2~", KEY_INS },
		{ "[Z", KEY_SHIFT_TAB },
		{ "OP", KEY_F1 }, { "OQ", KEY_F1 + 1 }, { "OR", KEY_F1 + 2 }, { "OS", KEY_F1 + 3 },
		{ "[11~", KEY_F1 }, { "[12~", KEY_F1 + 1 }, { "[13~", KEY_F1 + 2 }, { "[14~", KEY_F1 + 3 },
		{ "[15~", KEY_F1 + 4 }, { "[17~", KEY_F1 + 5 }, { "[18~", KEY_F1 + 6 }, { "[19~", KEY_F1 + 7 },
		{ "[20~", KEY_F1 + 8 }, { "[21~", KEY_F1 + 9 }, { "[23~", KEY_F1 + 10 }, { "[24~", KEY_F1 + 11 },
	};
	size_t i = 0;
	while (i < n) {
		uint8_t ch = b[i];
		if (ch == 0x1D) {	/* Ctrl-]: hang up, like telnet */
			*hangup = 1;
			return 0;
		}
		if (ch == 27 && i + 1 < n) {
			int matched = 0;
			for (size_t k = 0; k < sizeof seqs / sizeof seqs[0] && !matched; k++) {
				size_t l = strlen(seqs[k].seq);
				if (i + 1 + l <= n && !memcmp(b + i + 1, seqs[k].seq, l)) {
					send_key(seqs[k].key);
					i += 1 + l;
					matched = 1;
				}
			}
			if (matched)
				continue;
			if (isalnum(b[i + 1])) {	/* Alt+letter arrives as ESC letter */
				send_key(KEY_ALT | tolower(b[i + 1]));
				i += 2;
				continue;
			}
		}
		if (ch == '\r' || ch == '\n')
			send_key('\n');
		else if (ch == 0x7F || ch == 8)
			send_key('\b');
		else if (ch >= 0xC0) {	/* UTF-8: bdos's keyboard is ASCII, skip it */
			i++;
			while (i < n && (b[i] & 0xC0) == 0x80)
				i++;
			continue;
		} else
			send_key(ch);
		i++;
	}
	return 0;
}

/* ---- desktop mode ---------------------------------------------------------------------------- */

static int fb_w = 1024, fb_h = 768;
static uint32_t *fb;
static int fb_dirty;

static void draw_pixels(const uint8_t *d, uint32_t len)
{
	if (len < 16 || !fb)
		return;
	int32_t r[4];
	memcpy(r, d, 16);
	const uint8_t *p = d + 16;
	size_t nruns = (len - 16) / 6;
	int x = 0, y = 0;
	for (size_t i = 0; i < nruns; i++) {
		uint16_t n;
		uint32_t rgb;
		memcpy(&n, p + i * 6, 2);
		memcpy(&rgb, p + i * 6 + 2, 4);
		while (n--) {
			int sx = r[0] + x, sy = r[1] + y;
			if (sx >= 0 && sy >= 0 && sx < fb_w && sy < fb_h)
				fb[(size_t)sy * (size_t)fb_w + (size_t)sx] = 0xFF000000u | rgb;
			if (++x >= r[2]) {
				x = 0;
				y++;
			}
		}
	}
	fb_dirty = 1;
}

static void send_event(int type, int x, int y, int buttons, int key)
{
	int32_t v[5] = { type, x, y, buttons, key };
	send_msg(M_EV, v, sizeof v);
}

UNUSED static void save_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f || !fb)
		return;
	fprintf(f, "P6\n%d %d\n255\n", fb_w, fb_h);
	for (int i = 0; i < fb_w * fb_h; i++) {
		uint8_t rgb[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

/* ---- messages from bdos ----------------------------------------------------------------------- */

static int done;

static void on_message(int type, const uint8_t *data, uint32_t len)
{
	switch (type) {
	case M_OUT:
		tty_out(data, len);
		break;
	case M_CTRL:
		if (len >= 12) {
			int32_t v[3];
			memcpy(v, data, sizeof v);
			tty_ctrl(v);
		}
		break;
	case M_PIX:
		draw_pixels(data, len);
		break;
	case M_FREQ:
		serve_file(data, len);
		break;
	case M_BYE:
		done = 1;
		break;
	}
}

/* ---- scripted input, for tests and demos ------------------------------------------------------- */

static const char *type_script;
static double type_after = 1.5, quit_after;
static const char *snap_path;
static char click_script[256];	/* "x,y[,2];x,y..." - 2 = double-click */

/* "dir\r" -> keys; \r and \n mean Enter, \e Esc, \t Tab */
static void type_keys(const char *s, void (*key)(int))
{
	for (; *s; s++) {
		int k = (unsigned char)*s;
		if (*s == '\\' && s[1]) {
			s++;
			k = *s == 'r' || *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'e' ? 27 : *s;
		} else if (k == '\r') {
			k = '\n';
		}
		key(k);
	}
}

static void key_tty(int k) { send_key(k); }
UNUSED static void key_desk(int k) { send_event(EV_KEY, 0, 0, 0, k); }

/* ---- listing machines -------------------------------------------------------------------------- */

static void list_machines(const char *where)
{
	struct sockaddr_in to;
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port = htons(DEFAULT_PORT);
	if (where)
		resolve(where, &to);
	else
		to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	int on = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
	peer = to;
	c.id = 0;
	udp_out(F_WHO, 0, 0, "*", 2);
	uint64_t until = now_ms() + 1500;
	int found = 0;
	while (now_ms() < until) {
		struct pollfd p = { sock, POLLIN, 0 };
		if (poll(&p, 1, 100) <= 0)
			continue;
		uint8_t buf[2048];
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		ssize_t n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 6 || memcmp(buf, "BDNU", 4) || buf[4] != 1)
			continue;
		size_t skip = 6 + buf[5];
		if ((size_t)n < skip + sizeof(struct hdr))
			continue;
		struct hdr h;
		memcpy(&h, buf + skip, sizeof h);
		if (h.magic != MAGIC || h.type != F_IAM)
			continue;
		char name[33];
		snprintf(name, sizeof name, "%.*s", (int)(n - skip - sizeof h), (const char *)buf + skip + sizeof h);
		printf("\\\\%-20s %s:%d\n", name, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
		found++;
	}
	if (!found)
		printf("No bdos machines answered.\n");
}

/* ---- main --------------------------------------------------------------------------------- */

static void usage(void)
{
	fprintf(stderr,
	    "bterm %s - connect to a bdos machine\n\n"
	    "usage: bterm [options] host[:port]\n"
	    "       bterm -l [host]          list bdos machines on the network\n\n"
	    "  -t            text mode: a bdos shell in this terminal (Ctrl-] hangs up)\n"
	    "  -g WxH        desktop size (default 1024x768)\n"
	    "  -s dir        share dir as T: (default: the current directory)\n"
	    "  -n            don't share anything as T:\n"
	    "  -p password   the machine's net.password, if it has one ($BTERM_PASSWORD)\n"
	    "  -z N          scale the window N times (default: fit the screen)\n"
	    "  -v            say what T: is being asked for\n\n"
	    "The port defaults to %d. Under QEMU, forward it to the guest:\n"
	    "  -nic user,model=e1000,hostfwd=udp:127.0.0.1:%d-:%d   then   bterm localhost\n",
	    BTERM_VERSION, DEFAULT_PORT, DEFAULT_PORT, DEFAULT_PORT);
	exit(2);
}

int main(int argc, char **argv)
{
	int listing = 0, zoom = 0;
	const char *share_arg = NULL;
	if (getenv("BTERM_PASSWORD"))
		snprintf(password, sizeof password, "%s", getenv("BTERM_PASSWORD"));
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		const char *a = argv[i];
		const char *val = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "-t")) text_mode = 1;
		else if (!strcmp(a, "-n")) sharing = 0;
		else if (!strcmp(a, "-v")) verbose = 1;
		else if (!strcmp(a, "-l")) listing = 1;
		else if (!strcmp(a, "-g") && val) {
			if (sscanf(val, "%dx%d", &fb_w, &fb_h) != 2)
				usage();
			i++;
		} else if (!strcmp(a, "-s") && val) { share_arg = val; i++; }
		else if (!strcmp(a, "-p") && val) { snprintf(password, sizeof password, "%s", val); i++; }
		else if (!strcmp(a, "-z") && val) { zoom = atoi(val); i++; }
		/* for tests and demos */
		else if (!strcmp(a, "--type") && val) { type_script = val; i++; }
		else if (!strcmp(a, "--type-after") && val) { type_after = atof(val); i++; }
		else if (!strcmp(a, "--quit-after") && val) { quit_after = atof(val); i++; }
		else if (!strcmp(a, "--snap") && val) { snap_path = val; i++; }
		else if (!strcmp(a, "--click") && val) { snprintf(click_script, sizeof click_script, "%s", val); i++; }
		else if (!strcmp(a, "--version")) { printf("bterm %s\n", BTERM_VERSION); return 0; }
		else usage();
	}
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		die("socket: %s", strerror(errno));
	if (listing) {
		list_machines(i < argc ? argv[i] : NULL);
		return 0;
	}
	if (i != argc - 1)
		usage();
	resolve(argv[i], &peer);
	signal(SIGPIPE, SIG_IGN);

	if (!realpath(share_arg ? share_arg : ".", share))
		die("%s: %s", share_arg ? share_arg : ".", strerror(errno));
	struct hello h;
	memset(&h, 0, sizeof h);
	char hn[256] = "bterm";
	gethostname(hn, sizeof hn);
	char *dot = strchr(hn, '.');
	if (dot)
		*dot = 0;
	for (size_t k = 0; k < sizeof h.host - 1 && hn[k]; k++)
		h.host[k] = (char)toupper((unsigned char)hn[k]);
	h.flags = sharing ? HELLO_SHARE : 0;
	snprintf(h.share, sizeof h.share, "%s", share);
	fb_w = fb_w < 320 ? 320 : fb_w > 1600 ? 1600 : fb_w;
	fb_h = fb_h < 200 ? 200 : fb_h > 1200 ? 1200 : fb_h;

	if (text_mode) {
		struct winsize ws;
		h.w = 80;
		h.h = 25;
		if (isatty(1) && !ioctl(1, TIOCGWINSZ, &ws) && ws.ws_col && ws.ws_row) {
			h.w = ws.ws_col;
			h.h = ws.ws_row;
		}
		conn_open(SVC_TTY, &h);
		raw_mode();
		uint64_t start = now_ms();
		int typed = !type_script, hangup = 0;
		while (!done && !hangup && c.state == 2) {
			int ready = 0;
			net_poll(20, 0, &ready);
			if (ready) {
				uint8_t b[256];
				ssize_t n = read(0, b, sizeof b);
				if (n > 0)
					tty_keys(b, (size_t)n, &hangup);
				else if (n == 0 && !isatty(0) && !type_script)
					break;	/* stdin closed */
			}
			double t = (double)(now_ms() - start) / 1000.0;
			if (!typed && t >= type_after) {
				type_keys(type_script, key_tty);
				typed = 1;
			}
			if (quit_after > 0 && t >= quit_after)
				break;
		}
		conn_close();
		raw_off();
		fprintf(stderr, "\r\nbterm: connection closed%s%s.\r\n", c.why && !done ? ": " : "",
		    c.why && !done ? c.why : "");
		return 0;
	}

#ifdef NO_SDL
	(void)zoom;
	die("this bterm was built without SDL2: only -t (text mode) works");
#else
	h.w = fb_w;
	h.h = fb_h;
	fb = calloc((size_t)fb_w * (size_t)fb_h, 4);
	if (!fb)
		die("out of memory");
	for (int k = 0; k < fb_w * fb_h; k++)
		fb[k] = 0xFF202830u;
	if (SDL_Init(SDL_INIT_VIDEO))
		die("SDL: %s", SDL_GetError());
	SDL_DisplayMode dm;
	int ww = fb_w, wh = fb_h;
	if (zoom > 0) {
		ww *= zoom;
		wh *= zoom;
	} else if (!SDL_GetDesktopDisplayMode(0, &dm) && (ww > dm.w - 40 || wh > dm.h - 80)) {
		double s = (double)(dm.w - 40) / ww < (double)(dm.h - 80) / wh ?
		    (double)(dm.w - 40) / ww : (double)(dm.h - 80) / wh;
		ww = (int)(ww * s);
		wh = (int)(wh * s);
	}
	char title[256];
	snprintf(title, sizeof title, "bterm - %s (connecting)", argv[i]);
	SDL_Window *win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
	    SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!win)
		die("SDL: %s", SDL_GetError());
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren)
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	if (!ren)
		die("SDL: %s", SDL_GetError());
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	SDL_RenderSetLogicalSize(ren, fb_w, fb_h);
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, fb_w, fb_h);
	if (!tex)
		die("SDL: %s", SDL_GetError());
	SDL_UpdateTexture(tex, NULL, fb, fb_w * 4);
	SDL_RenderClear(ren);
	SDL_RenderCopy(ren, tex, NULL, NULL);
	SDL_RenderPresent(ren);

	conn_open(SVC_DESK, &h);
	snprintf(title, sizeof title, "bterm - %s%s%s", argv[i], sharing ? " - T: is " : "", sharing ? share : "");
	SDL_SetWindowTitle(win, title);
	SDL_StartTextInput();

	uint64_t start = now_ms(), last_present = 0;
	int typed = !type_script, buttons = 0, swallow_text = 0;
	int mx = -1, my = -1, moved = 0;
	while (!done && c.state == 2) {
		net_poll(fb_dirty ? 0 : 8, -1, NULL);
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT:
				done = 1;
				break;
			case SDL_MOUSEMOTION:
				mx = e.motion.x;
				my = e.motion.y;
				moved = 1;
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP: {
				int bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 :
				    e.button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
				if (!bit)
					break;
				if (moved) {
					send_event(EV_MOVE, mx, my, buttons, 0);
					moved = 0;
				}
				if (e.type == SDL_MOUSEBUTTONDOWN)
					buttons |= bit;
				else
					buttons &= ~bit;
				mx = e.button.x;
				my = e.button.y;
				send_event(e.type == SDL_MOUSEBUTTONDOWN ? EV_DOWN : EV_UP, mx, my, buttons, 0);
				break;
			}
			case SDL_KEYDOWN: {
				SDL_Keycode s = e.key.keysym.sym;
				uint16_t mod = e.key.keysym.mod;
				int k = 0;
				swallow_text = 0;
				switch (s) {
				case SDLK_RETURN: case SDLK_KP_ENTER: k = '\n'; break;
				case SDLK_BACKSPACE: k = '\b'; break;
				case SDLK_TAB: k = (mod & KMOD_SHIFT) ? KEY_SHIFT_TAB : '\t'; break;
				case SDLK_ESCAPE: k = 27; break;
				case SDLK_UP: k = KEY_UP; break;
				case SDLK_DOWN: k = KEY_DOWN; break;
				case SDLK_LEFT: k = KEY_LEFT; break;
				case SDLK_RIGHT: k = KEY_RIGHT; break;
				case SDLK_HOME: k = KEY_HOME; break;
				case SDLK_END: k = KEY_END; break;
				case SDLK_PAGEUP: k = KEY_PGUP; break;
				case SDLK_PAGEDOWN: k = KEY_PGDN; break;
				case SDLK_DELETE: k = KEY_DEL; break;
				case SDLK_INSERT: k = KEY_INS; break;
				default:
					if (s >= SDLK_F1 && s <= SDLK_F12)
						k = KEY_F1 + (int)(s - SDLK_F1);
					else if ((mod & KMOD_CTRL) && s >= 'a' && s <= 'z')
						k = (int)(s - 'a' + 1);		/* ^C and friends */
					else if ((mod & (KMOD_ALT | KMOD_GUI)) && s < 128 && isalnum((int)s))
						k = KEY_ALT | (int)s;	/* Alt (or Cmd on a Mac) + letter */
				}
				if (k) {
					send_event(EV_KEY, 0, 0, buttons, k);
					swallow_text = (mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) != 0;
				}
				break;
			}
			case SDL_TEXTINPUT:
				if (swallow_text) {
					swallow_text = 0;
					break;
				}
				for (const char *t = e.text.text; *t; t++)
					if ((unsigned char)*t >= 0x20 && (unsigned char)*t < 0x7F)
						send_event(EV_KEY, 0, 0, buttons, *t);
				break;
			case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
					fb_dirty = 1;
				break;
			}
		}
		if (moved) {	/* one move per pass is plenty */
			send_event(EV_MOVE, mx, my, buttons, 0);
			moved = 0;
		}
		double t = (double)(now_ms() - start) / 1000.0;
		if (!typed && t >= type_after) {
			type_keys(type_script, key_desk);
			typed = 1;
		}
		static int clicked;
		if (!clicked && click_script[0] && t >= type_after + 1.5) {
			/* clicks go out exactly as SDL mouse events would */
			for (char *q = click_script; q && *q; ) {
				int cx, cy, n = 1;
				if (sscanf(q, "%d,%d,%d", &cx, &cy, &n) < 2)
					break;
				send_event(EV_MOVE, cx, cy, 0, 0);
				for (int k = 0; k < n; k++) {
					send_event(EV_DOWN, cx, cy, 1, 0);
					send_event(EV_UP, cx, cy, 0, 0);
				}
				q = strchr(q, ';');
				if (q)
					q++;
			}
			clicked = 1;
		}
		if (quit_after > 0 && t >= quit_after)
			break;
		if (fb_dirty && now_ms() - last_present >= 15) {
			SDL_UpdateTexture(tex, NULL, fb, fb_w * 4);
			SDL_RenderClear(ren);
			SDL_RenderCopy(ren, tex, NULL, NULL);
			SDL_RenderPresent(ren);
			fb_dirty = 0;
			last_present = now_ms();
		}
	}
	if (snap_path)
		save_ppm(snap_path);
	conn_close();
	if (c.why && !done)
		fprintf(stderr, "bterm: connection closed: %s\n", c.why);
	SDL_Quit();
	return 0;
#endif
}
