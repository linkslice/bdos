/*
 * inet.c - Internet programs on bdos's TCP: TELNET (a client with an ANSI
 * terminal, CP437 and all, for BBSes and Unix boxes), a telnet server that
 * hands out bdos shells, FETCH (HTTP GET to a file), NSLOOKUP and NETSTAT.
 */
#include "../kernel.h"
#include "../settings.h"
#include "../vfs.h"
#include "net.h"

int console_is_remote(struct console *c);

#define IAC	255
#define DONT	254
#define DO	253
#define WONT	252
#define WILL	251
#define SB	250
#define SE	240
#define OPT_ECHO	1
#define OPT_SGA		3
#define OPT_TTYPE	24
#define OPT_NAWS	31

/* CP437 0x80-0xFF and the 0x01-0x1F glyphs, as Unicode */
static const uint16_t cp437_hi[128] = {
	0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,
	0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9, 0xFF, 0xD6, 0xDC, 0xA2, 0xA3, 0xA5, 0x20A7, 0x192,
	0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1, 0xAA, 0xBA, 0xBF, 0x2310, 0xAC, 0xBD, 0xBC, 0xA1, 0xAB, 0xBB,
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
	0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
	0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
	0x3B1, 0xDF, 0x393, 0x3C0, 0x3A3, 0x3C3, 0xB5, 0x3C4, 0x3A6, 0x398, 0x3A9, 0x3B4, 0x221E, 0x3C6, 0x3B5, 0x2229,
	0x2261, 0xB1, 0x2265, 0x2264, 0x2320, 0x2321, 0xF7, 0x2248, 0xB0, 0x2219, 0xB7, 0x221A, 0x207F, 0xB2, 0x25A0, 0xA0,
};
static const uint16_t cp437_lo[32] = {
	0x20, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, 0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
	0x25BA, 0x25C4, 0x2195, 0x203C, 0xB6, 0xA7, 0x25AC, 0x21A8, 0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
};

static int utf8(uint32_t u, char *out)
{
	if (u < 0x80) {
		out[0] = (char)u;
		return 1;
	}
	if (u < 0x800) {
		out[0] = (char)(0xC0 | u >> 6);
		out[1] = (char)(0x80 | (u & 0x3F));
		return 2;
	}
	out[0] = (char)(0xE0 | u >> 12);
	out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
	out[2] = (char)(0x80 | (u & 0x3F));
	return 3;
}

/* ANSI colour order to CGA's, both ways */
static const int ansi_to_cga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/* ======================================================================================
 * TELNET: the client
 * ====================================================================================== */

struct term {
	int cols, rows, x, y, sx, sy;
	int fg, bg, bold, reverse;
	int state;		/* 0 text, 1 after ESC, 2 in CSI */
	int params[8], np, priv;
	/* telnet */
	int tstate, sbopt, sblen;
	uint8_t sb[64];
	uint8_t done_opt[256];
	struct tcp *tcp;
};

static void t_send(struct term *t, const void *p, size_t n)
{
	tcp_send(t->tcp, p, n);
}

static void t_opt(struct term *t, int verb, int opt)
{
	uint8_t m[3] = { IAC, (uint8_t)verb, (uint8_t)opt };
	t_send(t, m, 3);
}

static void t_attr(struct term *t)
{
	int fg = t->fg | (t->bold ? 8 : 0), bg = t->bg;
	if (t->reverse) {
		int k = fg & 7;
		fg = bg;
		bg = k;
	}
	con_attr(fg, bg);
}

static void t_goto(struct term *t)
{
	t->x = MAX(0, MIN(t->x, t->cols - 1));
	t->y = MAX(0, MIN(t->y, t->rows - 1));
	con_gotoxy(t->x, t->y);
}

static void t_glyph(struct term *t, int ch)
{
	if (t->x >= t->cols) {
		con_putc('\n');
		t->x = 0;
		if (t->y < t->rows - 1)
			t->y++;
	}
	if (ch >= 0x20 && ch < 0x7F)
		con_putc((char)ch);
	else
		con_rawchar(ch);
	t->x++;
}

static void t_csi(struct term *t, int final)
{
	int p0 = t->np > 0 ? t->params[0] : 0, p1 = t->np > 1 ? t->params[1] : 0;
	switch (final) {
	case 'H': case 'f':
		t->y = (p0 ? p0 : 1) - 1;
		t->x = (p1 ? p1 : 1) - 1;
		t_goto(t);
		break;
	case 'A': t->y -= p0 ? p0 : 1; t_goto(t); break;
	case 'B': t->y += p0 ? p0 : 1; t_goto(t); break;
	case 'C': t->x += p0 ? p0 : 1; t_goto(t); break;
	case 'D': t->x -= p0 ? p0 : 1; t_goto(t); break;
	case 'G': t->x = (p0 ? p0 : 1) - 1; t_goto(t); break;
	case 'd': t->y = (p0 ? p0 : 1) - 1; t_goto(t); break;
	case 'J':
		if (p0 == 2 || p0 == 3) {
			con_clear();
			t->x = t->y = 0;
			t_goto(t);
		} else if (p0 == 0) {
			/* to the end of the screen */
			int x = t->x, y = t->y;
			for (int yy = y; yy < t->rows; yy++) {
				con_gotoxy(yy == y ? x : 0, yy);
				for (int xx = yy == y ? x : 0; xx < t->cols - (yy == t->rows - 1); xx++)
					con_rawchar(' ');
			}
			t_goto(t);
		}
		break;
	case 'K': {
		int from = p0 == 0 ? t->x : 0, to = p0 == 1 ? t->x + 1 : t->cols - (t->y == t->rows - 1);
		con_gotoxy(from, t->y);
		for (int xx = from; xx < to; xx++)
			con_rawchar(' ');
		t_goto(t);
		break;
	}
	case 'm':
		if (!t->np)
			t->params[t->np++] = 0;
		for (int i = 0; i < t->np; i++) {
			int v = t->params[i];
			if (v == 0) {
				t->fg = 7;
				t->bg = 0;
				t->bold = t->reverse = 0;
			} else if (v == 1) {
				t->bold = 1;
			} else if (v == 22) {
				t->bold = 0;
			} else if (v == 7) {
				t->reverse = 1;
			} else if (v == 27) {
				t->reverse = 0;
			} else if (v >= 30 && v <= 37) {
				t->fg = ansi_to_cga[v - 30];
			} else if (v == 39) {
				t->fg = 7;
			} else if (v >= 40 && v <= 47) {
				t->bg = ansi_to_cga[v - 40];
			} else if (v == 49) {
				t->bg = 0;
			} else if (v >= 90 && v <= 97) {
				t->fg = ansi_to_cga[v - 90] | 8;
			}
		}
		t_attr(t);
		break;
	case 's': t->sx = t->x; t->sy = t->y; break;
	case 'u': t->x = t->sx; t->y = t->sy; t_goto(t); break;
	case 'h': case 'l':
		if (t->priv && p0 == 25)
			con_cursor(final == 'h');
		break;
	case 'n':
		if (p0 == 6) {		/* where's the cursor? */
			char r[24];
			int n = snprintf(r, sizeof r, "\033[%d;%dR", t->y + 1, t->x + 1);
			t_send(t, r, (size_t)n);
		}
		break;
	}
}

/* one byte of what the server sent, after telnet's own layer */
static void t_byte(struct term *t, int ch)
{
	if (t->state == 1) {
		if (ch == '[') {
			t->state = 2;
			t->np = 0;
			t->priv = 0;
			memset(t->params, 0, sizeof t->params);
			return;
		}
		t->state = 0;
		return;		/* ESC 7 / ESC 8 / charset picks: not worth it */
	}
	if (t->state == 2) {
		if (ch == '?') {
			t->priv = 1;
			return;
		}
		if (ch >= '0' && ch <= '9') {
			if (!t->np)
				t->np = 1;
			t->params[t->np - 1] = t->params[t->np - 1] * 10 + (ch - '0');
			return;
		}
		if (ch == ';') {
			if (!t->np)
				t->np = 1;
			if (t->np < 8)
				t->np++;
			return;
		}
		t->state = 0;
		t_csi(t, ch);
		return;
	}
	switch (ch) {
	case 27: t->state = 1; return;
	case 0: case 7: return;
	case '\r': t->x = 0; t_goto(t); return;
	case '\n':
		if (t->y < t->rows - 1) {
			t->y++;
			t_goto(t);
		} else {
			int x = t->x;
			con_putc('\n');		/* scrolls */
			t->x = x;
			t_goto(t);
		}
		return;
	case '\b': if (t->x > 0) { t->x--; t_goto(t); } return;
	case '\t': {
		int n = 8 - (t->x & 7);
		while (n-- && t->x < t->cols)
			t_glyph(t, ' ');
		return;
	}
	}
	t_glyph(t, ch);
}

static void t_sb_done(struct term *t)
{
	if (t->sbopt == OPT_TTYPE && t->sblen >= 1 && t->sb[0] == 1) {	/* SEND */
		uint8_t m[] = { IAC, SB, OPT_TTYPE, 0, 'A', 'N', 'S', 'I', IAC, SE };
		t_send(t, m, sizeof m);
	}
}

static void t_naws(struct term *t)
{
	uint8_t m[] = { IAC, SB, OPT_NAWS, 0, (uint8_t)t->cols, 0, (uint8_t)t->rows, IAC, SE };
	t_send(t, m, sizeof m);
}

/* the telnet layer: options and IACs */
static void t_net(struct term *t, int ch)
{
	switch (t->tstate) {
	case 0:
		if (ch == IAC)
			t->tstate = 1;
		else
			t_byte(t, ch);
		return;
	case 1:
		if (ch == IAC) {
			t_byte(t, IAC);
			t->tstate = 0;
		} else if (ch == WILL || ch == WONT || ch == DO || ch == DONT) {
			t->tstate = ch;
		} else if (ch == SB) {
			t->tstate = 2;
		} else {
			t->tstate = 0;
		}
		return;
	case 2:
		t->sbopt = ch;
		t->sblen = 0;
		t->tstate = 3;
		return;
	case 3:
		if (ch == IAC)
			t->tstate = 4;
		else if (t->sblen < (int)sizeof t->sb)
			t->sb[t->sblen++] = (uint8_t)ch;
		return;
	case 4:
		if (ch == SE) {
			t_sb_done(t);
			t->tstate = 0;
		} else {
			if (t->sblen < (int)sizeof t->sb)
				t->sb[t->sblen++] = (uint8_t)ch;
			t->tstate = 3;
		}
		return;
	default: {
		int verb = t->tstate;
		t->tstate = 0;
		if (t->done_opt[ch] & (1 << (verb - WILL)))
			return;		/* answered already: no loops */
		t->done_opt[ch] |= (uint8_t)(1 << (verb - WILL));
		if (verb == WILL)
			t_opt(t, (ch == OPT_ECHO || ch == OPT_SGA) ? DO : DONT, ch);
		else if (verb == DO) {
			if (ch == OPT_SGA || ch == OPT_TTYPE || ch == OPT_NAWS) {
				t_opt(t, WILL, ch);
				if (ch == OPT_NAWS)
					t_naws(t);
			} else {
				t_opt(t, WONT, ch);
			}
		}
		return;
	}
	}
}

static void t_key(struct term *t, int k)
{
	const char *s = NULL;
	char b[8];
	switch (k) {
	case KEY_UP: s = "\033[A"; break;
	case KEY_DOWN: s = "\033[B"; break;
	case KEY_RIGHT: s = "\033[C"; break;
	case KEY_LEFT: s = "\033[D"; break;
	case KEY_HOME: s = "\033[H"; break;
	case KEY_END: s = "\033[F"; break;
	case KEY_INS: s = "\033[2~"; break;
	case KEY_DEL: s = "\033[3~"; break;
	case KEY_PGUP: s = "\033[5~"; break;
	case KEY_PGDN: s = "\033[6~"; break;
	case KEY_SHIFT_TAB: s = "\033[Z"; break;
	case '\r': case '\n': s = "\r\n"; break;
	default:
		if (k >= KEY_F1 && k < KEY_F1 + 4) {
			snprintf(b, sizeof b, "\033O%c", 'P' + (k - KEY_F1));
			s = b;
		} else if (k >= 0 && k < 256) {
			uint8_t c[2] = { (uint8_t)k, IAC };
			t_send(t, c, k == IAC ? 2 : 1);
		}
	}
	if (s)
		t_send(t, s, strlen(s));
}

int telnet_run(const char *host, int port)
{
	uint32_t ip;
	int e = dns_resolve(host, &ip);
	if (e) {
		kprintf("%s: %s\n", host, vfs_strerror(e));
		return 1;
	}
	char a[16];
	ip_fmt(ip, a);
	kprintf("Trying %s...\n", a);
	struct tcp *tc = tcp_connect(ip, (uint16_t)port, 15000, &e);
	if (!tc) {
		kprintf("Couldn't connect to %s port %d: %s\n", host, port, vfs_strerror(e));
		return 1;
	}
	kprintf("Connected to %s. Ctrl+] closes the connection.\n", host);
	struct term *t = kzalloc(sizeof *t);
	if (!t) {
		tcp_close(tc);
		return 1;
	}
	t->tcp = tc;
	con_size(&t->cols, &t->rows);
	t->fg = 7;
	t->rows = MAX(t->rows, 2);
	t->cols = MAX(t->cols, 2);
	t->y = t->rows - 1;	/* where the banner left us, more or less */
	uint8_t buf[1024];
	int closed_here = 0;
	for (;;) {
		int busy = 0;
		int n = tcp_readable(tc);
		if (n < 0)
			break;
		if (n > 0) {
			long got = tcp_recv(tc, buf, sizeof buf, 0);
			for (long i = 0; i < got; i++)
				t_net(t, buf[i]);
			busy = 1;
		}
		int k = con_pollkey();
		if (k == 0x1D) {	/* Ctrl+] */
			closed_here = 1;
			break;
		}
		if (k == KEY_EOF && console_is_remote(task_current()->con))
			break;		/* a CTTY session that hung up */
		if (k >= 0) {
			t_key(t, k);
			busy = 1;
		}
		if (!busy)
			task_idle();
	}
	tcp_close(tc);
	con_attr(7, 0);
	con_cursor(1);
	kprintf("\nConnection %s.\n", closed_here ? "closed" : "closed by the other end");
	kfree(t);
	return 0;
}

/* ======================================================================================
 * The telnet server: a bdos shell for every caller
 * ====================================================================================== */

struct tcon {
	struct console c;
	struct tcp *tcp;
	int cols, rows;
	int tstate, sbopt, sblen;
	uint8_t sb[16];
	uint8_t in[64];
	int nin;
	int last_cr;
	int dead;
};

static void tc_out(struct tcon *t, const void *p, size_t n)
{
	if (!t->dead && tcp_send(t->tcp, p, n) < 0)
		t->dead = 1;
}

static void tc_write(struct console *c, const char *s, size_t n)
{
	struct tcon *t = c->priv;
	char buf[512];
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		uint8_t ch = (uint8_t)s[i];
		if (o + 4 > sizeof buf) {
			tc_out(t, buf, o);
			o = 0;
		}
		if (ch == '\n') {
			buf[o++] = '\r';
			buf[o++] = '\n';
		} else if (ch >= 0x80) {
			o += (size_t)utf8(cp437_hi[ch - 0x80], buf + o);
		} else {
			buf[o++] = (char)ch;
		}
	}
	tc_out(t, buf, o);
}

static void tc_rawchar(struct console *c, int ch)
{
	struct tcon *t = c->priv;
	char b[4];
	uint8_t u = (uint8_t)ch;
	int n = u >= 0x80 ? utf8(cp437_hi[u - 0x80], b) : u < 0x20 ? utf8(cp437_lo[u], b) : utf8(u == 0x7F ? 0x2302 : u, b);
	tc_out(t, b, (size_t)n);
}

static void tc_printf(struct tcon *t, const char *fmt, ...)
{
	char b[64];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	tc_out(t, b, (size_t)n);
}

static void tc_clear(struct console *c) { tc_printf(c->priv, "\033[0m\033[2J\033[H"); }
static void tc_cursor(struct console *c, int on) { tc_printf(c->priv, on ? "\033[?25h" : "\033[?25l"); }
static void tc_gotoxy(struct console *c, int x, int y) { tc_printf(c->priv, "\033[%d;%dH", y + 1, x + 1); }

static void tc_attr(struct console *c, int fg, int bg)
{
	tc_printf(c->priv, "\033[0;%s%d;%dm", (fg & 8) ? "1;" : "", 30 + ansi_to_cga[fg & 7], 40 + ansi_to_cga[bg & 7]);
}

static void tc_size(struct console *c, int *w, int *h)
{
	struct tcon *t = c->priv;
	*w = t->cols;
	*h = t->rows;
}

/* bytes from the client, through the telnet layer */
static int tc_byte(struct tcon *t, int ch)
{
	switch (t->tstate) {
	case 0:
		if (ch == IAC) {
			t->tstate = 1;
			return -1;
		}
		return ch;
	case 1:
		if (ch == IAC) {
			t->tstate = 0;
			return IAC;
		}
		t->tstate = ch == SB ? 2 : (ch >= WILL && ch <= DONT) ? 5 : 0;
		return -1;
	case 2:
		t->sbopt = ch;
		t->sblen = 0;
		t->tstate = 3;
		return -1;
	case 3:
		if (ch == IAC)
			t->tstate = 4;
		else if (t->sblen < (int)sizeof t->sb)
			t->sb[t->sblen++] = (uint8_t)ch;
		return -1;
	case 4:
		t->tstate = ch == SE ? 0 : 3;
		if (ch == SE && t->sbopt == OPT_NAWS && t->sblen >= 4) {
			int w = t->sb[0] << 8 | t->sb[1], h = t->sb[2] << 8 | t->sb[3];
			if (w >= 20 && w <= 250)
				t->cols = w;
			if (h >= 5 && h <= 120)
				t->rows = h;
		}
		return -1;
	default:		/* the option after WILL/WONT/DO/DONT: we said what we wanted already */
		t->tstate = 0;
		return -1;
	}
}

static int tc_fill(struct tcon *t, int wait_ms)
{
	if (t->dead)
		return -1;
	int r = tcp_readable(t->tcp);
	if (r < 0) {
		t->dead = 1;
		return -1;
	}
	if (!r && wait_ms) {
		uint8_t b;
		long got = tcp_recv(t->tcp, &b, 1, wait_ms);
		if (got == 1) {
			int ch = tc_byte(t, b);
			if (ch >= 0 && t->nin < (int)sizeof t->in)
				t->in[t->nin++] = (uint8_t)ch;
		}
		return 0;
	}
	while (r > 0 && t->nin < (int)sizeof t->in) {
		uint8_t b;
		if (tcp_recv(t->tcp, &b, 1, 0) != 1)
			break;
		r--;
		int ch = tc_byte(t, b);
		if (ch >= 0)
			t->in[t->nin++] = (uint8_t)ch;
	}
	return 0;
}

static int tc_take(struct tcon *t, int n)
{
	int ch = t->in[0];
	memmove(t->in, t->in + n, (size_t)(t->nin - n));
	t->nin -= n;
	return ch;
}

static int tc_getkey(struct console *c)
{
	struct tcon *t = c->priv;
	if (tc_fill(t, 0) < 0 && !t->nin)
		return KEY_EOF;
	if (!t->nin)
		return -1;
	int ch = t->in[0];
	if (ch == 27) {
		if (t->nin < 3)
			tc_fill(t, 40);		/* the rest of a sequence is on its way */
		if (t->nin >= 3 && (t->in[1] == '[' || t->in[1] == 'O')) {
			int f = t->in[2];
			static const struct { char f; int key; } simple[] = {
				{ 'A', KEY_UP }, { 'B', KEY_DOWN }, { 'C', KEY_RIGHT }, { 'D', KEY_LEFT },
				{ 'H', KEY_HOME }, { 'F', KEY_END }, { 'Z', KEY_SHIFT_TAB },
				{ 'P', KEY_F1 }, { 'Q', KEY_F1 + 1 }, { 'R', KEY_F1 + 2 }, { 'S', KEY_F1 + 3 },
			};
			for (size_t i = 0; i < NELEM(simple); i++)
				if (f == simple[i].f) {
					tc_take(t, 3);
					return simple[i].key;
				}
			if (f >= '1' && f <= '6' && t->nin >= 4 && t->in[3] == '~') {
				static const int tilde[] = { KEY_HOME, KEY_INS, KEY_DEL, KEY_END, KEY_PGUP, KEY_PGDN };
				tc_take(t, 4);
				return tilde[f - '1'];
			}
		}
		tc_take(t, 1);
		return 27;
	}
	tc_take(t, 1);
	if (ch == '\n' && t->last_cr) {		/* CR LF is one Enter */
		t->last_cr = 0;
		return tc_getkey(c);
	}
	if (ch == 0 && t->last_cr) {		/* and so is CR NUL */
		t->last_cr = 0;
		return tc_getkey(c);
	}
	t->last_cr = ch == '\r';
	if (ch == '\r')
		return '\n';		/* Enter, as every bdos console says it */
	if (ch == 0x7F)
		return '\b';
	return ch;
}

static void telnet_session(void *arg)
{
	struct tcp *tcp = arg;
	struct tcon *t = kzalloc(sizeof *t);
	if (!t) {
		tcp_close(tcp);
		return;
	}
	t->tcp = tcp;
	t->cols = 80;
	t->rows = 25;
	t->c.name = "TELNET";
	t->c.write = tc_write;
	t->c.getkey = tc_getkey;
	t->c.clear = tc_clear;
	t->c.cursor = tc_cursor;
	t->c.size = tc_size;
	t->c.gotoxy = tc_gotoxy;
	t->c.attr = tc_attr;
	t->c.rawchar = tc_rawchar;
	t->c.priv = t;
	/* we echo; character at a time; tell us the window size */
	uint8_t hello[] = { IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA, IAC, DO, OPT_NAWS };
	tc_out(t, hello, sizeof hello);
	struct task *me = task_current();
	me->con = &t->c;
	strcpy(me->cwd, "C:\\");
	/* let the client's answers (and NAWS) arrive */
	for (int i = 0; i < 10; i++)
		tc_fill(t, 30);
	t->nin = 0;
	const char *pw = setting("net.password");
	int ok = 1;
	if (pw && *pw) {
		ok = 0;
		for (int tries = 0; tries < 3 && !ok && !t->dead; tries++) {
			kprintf("Password: ");
			char line[64];
			int n = 0;
			for (;;) {
				int k = con_getkey();
				if (k == KEY_EOF || t->dead)
					break;
				if (k == '\n' || k == '\r')
					break;
				if (k == '\b') {
					if (n)
						n--;
					continue;
				}
				if (k >= 32 && k < 127 && n < (int)sizeof line - 1)
					line[n++] = (char)k;
			}
			line[n] = 0;
			kprintf("\n");
			ok = !strcmp(line, pw);
			if (!ok && !t->dead)
				kprintf("Wrong password.\n");
		}
	}
	if (ok && !t->dead) {
		kprintf("\n%s %s on \\\\%s (%d core%s). EXIT to hang up.\n\n", BDOS_NAME, BDOS_VERSION, net_hostname(),
		    ncpus, ncpus == 1 ? "" : "s");
		shell_task(NULL);
	}
	me->con = NULL;
	tcp_close(tcp);
	kfree(t);
}

static void telnetd(void *arg)
{
	(void)arg;
	struct tcp *l = NULL;
	for (;;) {
		int want = setting_int("net.telnet") != 0 && ip_local();
		if (want && !l) {
			int e;
			l = tcp_listen(23, &e);
		} else if (!want && l) {
			tcp_close(l);
			l = NULL;
		}
		if (!l) {
			sleep_ms(1000);
			continue;
		}
		struct tcp *c = tcp_accept(l, 1000);
		if (c && !task_spawn("telnet", telnet_session, c, NULL))
			tcp_close(c);
	}
}

void inet_init(void)
{
	task_spawn("telnetd", telnetd, NULL, NULL);
}

/* ======================================================================================
 * FETCH: HTTP GET
 * ====================================================================================== */

static int parse_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn)
{
	if (!strncasecmp(url, "https://", 8)) {
		kprintf("FETCH speaks plain http:// only (no TLS here).\n");
		return -1;
	}
	if (!strncasecmp(url, "http://", 7))
		url += 7;
	const char *slash = strchr(url, '/');
	size_t hl = slash ? (size_t)(slash - url) : strlen(url);
	if (!hl || hl >= hn)
		return -1;
	memcpy(host, url, hl);
	host[hl] = 0;
	*port = 80;
	char *colon = strchr(host, ':');
	if (colon) {
		*colon = 0;
		*port = (int)strtol(colon + 1, NULL, 10);
		if (*port <= 0 || *port > 65535)
			return -1;
	}
	strncpy0(path, slash ? slash : "/", pn);
	return 0;
}

int fetch_run(const char *url_in, const char *out, int quiet)
{
	char url[512];
	strncpy0(url, url_in, sizeof url);
	for (int hop = 0; hop < 4; hop++) {
		char host[128], path[384];
		int port;
		if (parse_url(url, host, sizeof host, &port, path, sizeof path)) {
			kprintf("Not a URL I can fetch: %s\n", url);
			return 1;
		}
		uint32_t ip;
		int e = dns_resolve(host, &ip);
		if (e) {
			kprintf("%s: %s\n", host, vfs_strerror(e));
			return 1;
		}
		struct tcp *t = tcp_connect(ip, (uint16_t)port, 15000, &e);
		if (!t) {
			kprintf("Couldn't connect to %s: %s\n", host, vfs_strerror(e));
			return 1;
		}
		char req[700];
		int n = snprintf(req, sizeof req,
		    "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s/%s\r\nAccept: */*\r\nConnection: close\r\n\r\n",
		    path, host, BDOS_NAME, BDOS_VERSION);
		tcp_send(t, req, (size_t)n);
		/* the head: up to the blank line */
		char head[4096];
		size_t hl = 0;
		int body_at = -1;
		uint8_t buf[4096];
		long got = 0;
		while (body_at < 0) {
			got = tcp_recv(t, buf, sizeof buf, 20000);
			if (got <= 0)
				break;
			for (long i = 0; i < got && body_at < 0; i++) {
				if (hl < sizeof head - 1)
					head[hl++] = (char)buf[i];
				head[hl] = 0;
				if (hl >= 4 && !memcmp(head + hl - 4, "\r\n\r\n", 4)) {
					body_at = (int)i + 1;
				}
			}
		}
		if (body_at < 0) {
			kprintf("No answer from %s.\n", host);
			tcp_close(t);
			return 1;
		}
		int status = 0;
		char *sp = strchr(head, ' ');
		if (sp)
			status = (int)strtol(sp + 1, NULL, 10);
		if (status >= 300 && status < 400) {
			char *loc = NULL;
			for (char *line = head; line && *line; line = strstr(line, "\r\n") ? strstr(line, "\r\n") + 2 : NULL)
				if (!strncasecmp(line, "Location:", 9)) {
					loc = line + 9;
					break;
				}
			if (loc) {
				while (*loc == ' ')
					loc++;
				char next[512];
				size_t k = 0;
				while (loc[k] && loc[k] != '\r' && k < sizeof next - 1)
					next[k] = loc[k], k++;
				next[k] = 0;
				if (next[0] == '/')
					snprintf(url, sizeof url, "http://%s:%d%s", host, port, next);
				else
					strncpy0(url, next, sizeof url);
				if (!quiet)
					kprintf("Redirected to %s\n", url);
				tcp_close(t);
				continue;
			}
		}
		if (status != 200 && !quiet) {
			char *cr = strchr(head, '\r');
			kprintf("%s says: %.*s\n", host, cr ? (int)(cr - head) : (int)strlen(head), head);
		}
		struct vfile *f = NULL;
		if (out) {
			char abs[VFS_PATH_MAX];
			if (resolve_path(out, abs, sizeof abs) || !(f = vfs_open(abs, O_WRITE | O_CREATE | O_TRUNC, &e))) {
				kprintf("Can't write %s: %s\n", out, vfs_strerror(e));
				tcp_close(t);
				return 1;
			}
		}
		uint64_t total = 0, t0 = uptime_ms();
		long rest = got - body_at;
		const uint8_t *p = buf + body_at;
		for (;;) {
			if (rest > 0) {
				if (f)
					vfs_write(f, p, (size_t)rest);
				else
					con_writen((const char *)p, (size_t)rest);
				total += (uint64_t)rest;
			}
			if (con_pollkey() == 3) {
				kprintf("^C\n");
				break;
			}
			rest = tcp_recv(t, buf, sizeof buf, 30000);
			if (rest <= 0)
				break;
			p = buf;
		}
		tcp_close(t);
		if (f) {
			vfs_close(f);
			uint64_t ms = uptime_ms() - t0;
			if (!quiet)
				kprintf("%lu bytes to %s in %lu.%01lu s\n", total, out, ms / 1000, ms / 100 % 10);
		}
		return status == 200 ? 0 : 1;
	}
	kprintf("Too many redirects.\n");
	return 1;
}

/* ======================================================================================
 * NSLOOKUP, NETSTAT
 * ====================================================================================== */

int nslookup_run(const char *name)
{
	char s[16];
	ip_fmt(ip_dns(), s);
	kprintf("Server:  %s\n", ip_dns() ? s : "(none)");
	uint32_t ip;
	int e = dns_resolve(name, &ip);
	if (e) {
		kprintf("%s: %s\n", name, vfs_strerror(e));
		return 1;
	}
	ip_fmt(ip, s);
	kprintf("Name:    %s\nAddress: %s\n", name, s);
	return 0;
}

static void ns_line(const char *line, void *arg)
{
	(*(int *)arg)++;
	kprintf("%s\n", line);
}

int netstat_run(void)
{
	int n = 0;
	kprintf("Active connections:\n");
	tcp_list(ns_line, &n);
	if (!n)
		kprintf("  (none)\n");
	return 0;
}
