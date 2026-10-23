/*
 * tcp.c - TCP and a DNS resolver, on ip.c's IPv4.
 *
 * Plain, careful TCP: the three-way handshake both ways, a 64 KB window
 * each way, retransmission by going back to the first unacknowledged byte
 * with a doubling timeout, the MSS option, and the whole close dance
 * (FIN_WAIT, CLOSE_WAIT, a short TIME_WAIT). No SACK, no window scaling,
 * no congestion control beyond the timeout backing off - bdos talks to a
 * LAN or a QEMU NAT, not the open sea.
 *
 * It all runs under the big kernel lock: netd delivers segments and runs
 * the timers, and callers block in task_idle() between looks.
 */
#include "../kernel.h"
#include "../settings.h"
#include "../vfs.h"
#include "net.h"

#define P_TCP		6
#define TCP_BUF		65536
#define TCP_MSS		1460
#define RTO_MIN		300
#define RTO_MAX		8000
#define MAX_RETRIES	12
#define TIME_WAIT_MS	2000

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum { CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTAB, FIN_WAIT1, FIN_WAIT2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT };
static const char *const state_names[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT1",
					   "FIN_WAIT2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };

struct tcp {
	struct tcp *next;
	int state;
	uint32_t rip;
	uint16_t lport, rport;
	uint32_t iss, snd_una, snd_nxt, snd_wnd;
	uint32_t rcv_nxt;
	uint16_t mss;
	uint8_t *sbuf;		/* from snd_una on: sent-but-unacked, then unsent */
	uint32_t slen;
	uint8_t *rbuf;		/* a ring of what's arrived and not been read */
	uint32_t rhead, rlen;
	int fin_wanted;		/* close() asked: FIN after the data */
	int fin_sent, fin_rcvd;
	uint32_t fin_seq;
	uint64_t rto_at;	/* 0: nothing in flight */
	int rto, retries;
	uint64_t tw_until;
	int err;
	int user_closed;	/* nobody holds it: free once it's done */
	struct tcp *listener;	/* SYN_RCVD/ESTAB children waiting for accept() */
	int accepted;
	uint32_t last_wnd;	/* the window we last advertised */
};

static struct tcp *conns;
static uint16_t next_port = 49152;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* sequence-number comparisons, modulo 2^32 */
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static uint32_t new_iss(void)
{
	static uint32_t salt = 0x6B64F00D;
	salt = salt * 1103515245u + 12345u;
	return (uint32_t)uptime_us() * 4u + salt;
}

static uint16_t ephemeral(void)
{
	for (;;) {
		uint16_t p = next_port++;
		if (next_port < 49152)
			next_port = 49152;
		int used = 0;
		for (struct tcp *t = conns; t; t = t->next)
			used |= t->lport == p;
		if (!used)
			return p;
	}
}

static uint32_t rwnd(struct tcp *t)
{
	uint32_t w = TCP_BUF - t->rlen;
	return MIN(w, 65535u);
}

/* ---- segments out ------------------------------------------------------------------------ */

static void send_seg(struct tcp *t, uint32_t seq, int flags, const uint8_t *data, size_t len)
{
	uint8_t seg[20 + 4 + TCP_MSS];
	int hl = 20;
	memset(seg, 0, 24);
	wr16(seg, t->lport);
	wr16(seg + 2, t->rport);
	wr32(seg + 4, seq);
	wr32(seg + 8, (flags & F_ACK) ? t->rcv_nxt : 0);
	if (flags & F_SYN) {
		seg[20] = 2;		/* MSS */
		seg[21] = 4;
		wr16(seg + 22, TCP_MSS);
		hl = 24;
	}
	seg[12] = (uint8_t)(hl / 4) << 4;
	seg[13] = (uint8_t)flags;
	t->last_wnd = rwnd(t);
	wr16(seg + 14, (uint16_t)t->last_wnd);
	if (len)
		memcpy(seg + hl, data, len);
	wr16(seg + 16, ip_csum_pseudo(ip_local(), t->rip, P_TCP, seg, (size_t)hl + len));
	ip_output(t->rip, P_TCP, seg, (size_t)hl + len);
}

/* a reset for a segment nobody wants */
static void send_rst(uint32_t src, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, int with_ack)
{
	uint8_t seg[20];
	memset(seg, 0, 20);
	wr16(seg, dport);
	wr16(seg + 2, sport);
	wr32(seg + 4, seq);
	wr32(seg + 8, ack);
	seg[12] = 5 << 4;
	seg[13] = (uint8_t)(F_RST | (with_ack ? F_ACK : 0));
	wr16(seg + 16, ip_csum_pseudo(ip_local(), src, P_TCP, seg, 20));
	ip_output(src, P_TCP, seg, 20);
}

static void arm(struct tcp *t)
{
	if (!t->rto_at)
		t->rto_at = uptime_ms() + (uint64_t)t->rto;
}

/* send what the window allows, then the FIN if it's time */
static void output(struct tcp *t)
{
	if (t->state != ESTAB && t->state != CLOSE_WAIT && t->state != FIN_WAIT1 && t->state != LAST_ACK &&
	    t->state != CLOSING)
		return;
	uint32_t wnd = t->snd_wnd ? t->snd_wnd : 1;	/* a zero window gets probed a byte at a time */
	for (;;) {
		uint32_t off = t->snd_nxt - t->snd_una;
		if (t->fin_sent && off)
			off--;		/* the FIN's sequence number isn't data */
		if (off >= t->slen || off >= wnd)
			break;
		uint32_t n = MIN(t->slen - off, MIN(wnd - off, (uint32_t)t->mss));
		send_seg(t, t->snd_una + off, F_ACK | F_PSH, t->sbuf + off, n);
		t->snd_nxt = t->snd_una + off + n;
		arm(t);
	}
	if (t->fin_wanted && !t->fin_sent && t->snd_nxt - t->snd_una == t->slen) {
		t->fin_seq = t->snd_nxt;
		send_seg(t, t->snd_nxt, F_ACK | F_FIN, NULL, 0);
		t->snd_nxt++;
		t->fin_sent = 1;
		arm(t);
	}
}

/* ---- the life of a connection ------------------------------------------------------------- */

static struct tcp *tcb_new(void)
{
	struct tcp *t = kzalloc(sizeof *t);
	if (!t)
		return NULL;
	t->sbuf = kmalloc(TCP_BUF);
	t->rbuf = kmalloc(TCP_BUF);
	if (!t->sbuf || !t->rbuf) {
		kfree(t->sbuf);
		kfree(t->rbuf);
		kfree(t);
		return NULL;
	}
	t->mss = 536;
	t->rto = 1000;
	t->next = conns;
	conns = t;
	return t;
}

static void tcb_free(struct tcp *t)
{
	for (struct tcp **pp = &conns; *pp; pp = &(*pp)->next)
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	kfree(t->sbuf);
	kfree(t->rbuf);
	kfree(t);
}

static void dead(struct tcp *t, int err)
{
	t->state = CLOSED;
	if (!t->err)
		t->err = err;
	t->rto_at = 0;
}

/* the ack covered our FIN */
static void fin_acked(struct tcp *t)
{
	switch (t->state) {
	case FIN_WAIT1: t->state = FIN_WAIT2; break;
	case CLOSING: t->state = TIME_WAIT; t->tw_until = uptime_ms() + TIME_WAIT_MS; break;
	case LAST_ACK: dead(t, 0); break;
	}
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t n)
{
	if (n < 20)
		return;
	if (ip_csum_pseudo(src, dst, P_TCP, p, n) != 0)
		return;
	uint16_t sport = rd16(p), dport = rd16(p + 2);
	uint32_t seq = rd32(p + 4), ack = rd32(p + 8);
	int hl = (p[12] >> 4) * 4, flags = p[13];
	uint32_t win = rd16(p + 14);
	if (hl < 20 || (size_t)hl > n)
		return;
	const uint8_t *data = p + hl;
	uint32_t len = (uint32_t)(n - (size_t)hl);
	uint16_t mss = 536;
	for (int i = 20; i + 1 < hl;) {
		if (p[i] == 0)
			break;
		if (p[i] == 1) {
			i++;
			continue;
		}
		if (p[i] == 2 && p[i + 1] == 4 && i + 3 < hl)
			mss = rd16(p + i + 2);
		if (p[i + 1] < 2)
			break;
		i += p[i + 1];
	}

	struct tcp *t = NULL, *l = NULL;
	for (struct tcp *c = conns; c; c = c->next) {
		if (c->lport != dport)
			continue;
		if (c->state == LISTEN)
			l = c;
		else if (c->rip == src && c->rport == sport && c->state != CLOSED)
			t = c;
	}
	if (!t) {
		if (l && (flags & F_SYN) && !(flags & (F_ACK | F_RST))) {
			/* a new connection for a listener */
			struct tcp *c = tcb_new();
			if (!c)
				return;
			c->rip = src;
			c->rport = sport;
			c->lport = dport;
			c->listener = l;
			c->iss = new_iss();
			c->snd_una = c->iss;
			c->snd_nxt = c->iss + 1;
			c->rcv_nxt = seq + 1;
			c->snd_wnd = win;
			c->mss = MIN(mss, (uint16_t)TCP_MSS);
			c->state = SYN_RCVD;
			send_seg(c, c->iss, F_SYN | F_ACK, NULL, 0);
			arm(c);
			return;
		}
		if (!(flags & F_RST))
			send_rst(src, sport, dport, (flags & F_ACK) ? ack : 0,
			    seq + len + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0), !(flags & F_ACK));
		return;
	}

	if (t->state == SYN_SENT) {
		if ((flags & F_ACK) && ack != t->iss + 1) {
			if (!(flags & F_RST))
				send_rst(src, sport, dport, ack, 0, 0);
			return;
		}
		if (flags & F_RST) {
			if (flags & F_ACK)
				dead(t, E_REFUSED);
			return;
		}
		if ((flags & F_SYN) && (flags & F_ACK)) {
			t->rcv_nxt = seq + 1;
			t->snd_una = ack;
			t->snd_wnd = win;
			t->mss = MIN(mss, (uint16_t)TCP_MSS);
			t->state = ESTAB;
			t->rto_at = 0;
			t->retries = 0;
			send_seg(t, t->snd_nxt, F_ACK, NULL, 0);
			output(t);
		}
		return;
	}

	/* acceptable at all? (a zero window still takes ACKs and FINs at rcv_nxt) */
	if (flags & F_RST) {
		if (seq == t->rcv_nxt || (seq_le(t->rcv_nxt, seq) && seq_lt(seq, t->rcv_nxt + rwnd(t) + 1)))
			dead(t, E_RESET);
		return;
	}
	if (flags & F_SYN) {
		if (t->state == SYN_RCVD && seq + 1 == t->rcv_nxt) {
			send_seg(t, t->iss, F_SYN | F_ACK, NULL, 0);	/* our SYN-ACK was lost */
			return;
		}
		send_seg(t, t->snd_nxt, F_ACK, NULL, 0);
		return;
	}
	if (!(flags & F_ACK))
		return;

	/* the ACK */
	if (t->state == SYN_RCVD) {
		if (ack != t->iss + 1) {
			send_rst(src, sport, dport, ack, 0, 0);
			return;
		}
		t->snd_una = ack;
		t->state = ESTAB;
		t->rto_at = 0;
		t->retries = 0;
	}
	if (seq_lt(t->snd_una, ack) && seq_le(ack, t->snd_nxt)) {
		uint32_t acked = ack - t->snd_una;
		int fin_done = t->fin_sent && seq_lt(t->fin_seq, ack);
		uint32_t bytes = acked - (fin_done ? 1 : 0);
		bytes = MIN(bytes, t->slen);
		memmove(t->sbuf, t->sbuf + bytes, t->slen - bytes);
		t->slen -= bytes;
		t->snd_una = ack;
		t->retries = 0;
		t->rto = MAX(RTO_MIN, t->rto / 2);
		t->rto_at = t->snd_una == t->snd_nxt ? 0 : uptime_ms() + (uint64_t)t->rto;
		if (fin_done)
			fin_acked(t);
	}
	if (seq_le(t->snd_una, ack))
		t->snd_wnd = win;

	/* data */
	int need_ack = 0;
	if (len && (t->state == ESTAB || t->state == FIN_WAIT1 || t->state == FIN_WAIT2)) {
		if (seq == t->rcv_nxt) {
			uint32_t take = MIN(len, TCP_BUF - t->rlen);
			for (uint32_t i = 0; i < take; i++)
				t->rbuf[(t->rhead + t->rlen + i) % TCP_BUF] = data[i];
			t->rlen += take;
			t->rcv_nxt += take;
			if (take < len)
				flags &= ~F_FIN;	/* we didn't get to the FIN */
		} else {
			flags &= ~F_FIN;		/* out of order: wait for the gap */
		}
		need_ack = 1;
	} else if (len) {
		need_ack = 1;
	}
	/* their FIN, in order */
	if ((flags & F_FIN) && seq + len == t->rcv_nxt && !t->fin_rcvd) {
		t->rcv_nxt++;
		t->fin_rcvd = 1;
		need_ack = 1;
		switch (t->state) {
		case SYN_RCVD:
		case ESTAB: t->state = CLOSE_WAIT; break;
		case FIN_WAIT1:
			t->state = CLOSING;
			if (t->snd_una == t->snd_nxt)
				fin_acked(t);
			break;
		case FIN_WAIT2: t->state = TIME_WAIT; t->tw_until = uptime_ms() + TIME_WAIT_MS; break;
		}
	} else if ((flags & F_FIN) && t->fin_rcvd) {
		need_ack = 1;		/* a retransmitted FIN: ack it again */
	}
	if (need_ack)
		send_seg(t, t->snd_nxt, F_ACK, NULL, 0);
	output(t);
}

void tcp_timers(void)
{
	uint64_t now = uptime_ms();
	for (struct tcp *t = conns, *next; t; t = next) {
		next = t->next;
		if (t->state == TIME_WAIT && now >= t->tw_until)
			dead(t, 0);
		if (t->state == CLOSED) {
			if (t->user_closed)
				tcb_free(t);
			continue;
		}
		if (!t->rto_at || now < t->rto_at)
			continue;
		if (++t->retries > MAX_RETRIES) {
			dead(t, E_TIMEDOUT);
			continue;
		}
		t->rto = MIN(t->rto * 2, RTO_MAX);
		t->rto_at = now + (uint64_t)t->rto;
		if (t->state == SYN_SENT) {
			send_seg(t, t->iss, F_SYN, NULL, 0);
		} else if (t->state == SYN_RCVD) {
			send_seg(t, t->iss, F_SYN | F_ACK, NULL, 0);
		} else {
			/* go back to the first byte they haven't acked (the FIN follows the data again) */
			t->snd_nxt = t->snd_una;
			t->fin_sent = 0;
			t->rto_at = 0;
			output(t);
		}
	}
}

/* ---- the calls -------------------------------------------------------------------------------- */

static int ready(void)
{
	return ip_local() != 0;
}

struct tcp *tcp_connect(uint32_t ip, uint16_t port, int timeout_ms, int *err)
{
	if (!ready()) {
		*err = E_NONET;
		return NULL;
	}
	struct tcp *t = tcb_new();
	if (!t) {
		*err = E_NOMEM;
		return NULL;
	}
	t->rip = ip;
	t->rport = port;
	t->lport = ephemeral();
	t->iss = new_iss();
	t->snd_una = t->iss;
	t->snd_nxt = t->iss + 1;
	t->state = SYN_SENT;
	t->rto = 500;
	send_seg(t, t->iss, F_SYN, NULL, 0);	/* may wait on ARP: the timer resends */
	arm(t);
	uint64_t until = uptime_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 20000);
	while (t->state == SYN_SENT && uptime_ms() < until)
		task_idle();
	if (t->state != ESTAB && t->state != CLOSE_WAIT) {
		*err = t->err ? t->err : E_TIMEDOUT;
		if (t->state != CLOSED)
			send_rst(t->rip, t->rport, t->lport, t->snd_nxt, 0, 0);
		dead(t, *err);
		t->user_closed = 1;
		return NULL;
	}
	return t;
}

struct tcp *tcp_listen(uint16_t port, int *err)
{
	for (struct tcp *c = conns; c; c = c->next)
		if (c->lport == port && c->state == LISTEN) {
			*err = E_EXIST;
			return NULL;
		}
	struct tcp *t = tcb_new();
	if (!t) {
		*err = E_NOMEM;
		return NULL;
	}
	t->lport = port;
	t->state = LISTEN;
	return t;
}

struct tcp *tcp_accept(struct tcp *l, int timeout_ms)
{
	uint64_t until = timeout_ms < 0 ? ~0ULL : uptime_ms() + (uint64_t)timeout_ms;
	for (;;) {
		for (struct tcp *c = conns; c; c = c->next)
			if (c->listener == l && !c->accepted && c->state >= ESTAB) {
				c->accepted = 1;
				c->listener = NULL;
				return c;
			}
		if (uptime_ms() >= until || l->state != LISTEN)
			return NULL;
		task_idle();
	}
}

long tcp_send(struct tcp *t, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	size_t done = 0;
	while (done < n) {
		if (t->state != ESTAB && t->state != CLOSE_WAIT)
			return done ? (long)done : (t->err ? t->err : E_RESET);
		if (t->fin_wanted)
			return E_BADF;
		uint32_t room = TCP_BUF - t->slen;
		if (!room) {
			task_idle();
			continue;
		}
		uint32_t k = (uint32_t)MIN(n - done, (size_t)room);
		memcpy(t->sbuf + t->slen, p + done, k);
		t->slen += k;
		done += k;
		output(t);
	}
	return (long)done;
}

int tcp_readable(struct tcp *t)
{
	if (t->rlen)
		return (int)t->rlen;
	if (t->fin_rcvd || t->state == CLOSED)
		return -1;
	return 0;
}

long tcp_recv(struct tcp *t, void *buf, size_t n, int timeout_ms)
{
	uint64_t until = timeout_ms < 0 ? ~0ULL : uptime_ms() + (uint64_t)timeout_ms;
	while (!t->rlen) {
		if (t->fin_rcvd)
			return 0;
		if (t->state == CLOSED)
			return t->err ? t->err : 0;
		if (uptime_ms() >= until)
			return E_TIMEDOUT;
		task_idle();
	}
	uint32_t k = (uint32_t)MIN(n, (size_t)t->rlen);
	uint8_t *d = buf;
	for (uint32_t i = 0; i < k; i++)
		d[i] = t->rbuf[(t->rhead + i) % TCP_BUF];
	t->rhead = (t->rhead + k) % TCP_BUF;
	t->rlen -= k;
	/* the window opened a lot: say so */
	if (rwnd(t) >= t->last_wnd + 8192 || (t->last_wnd < t->mss && rwnd(t) >= t->mss))
		if (t->state == ESTAB || t->state == FIN_WAIT1 || t->state == FIN_WAIT2)
			send_seg(t, t->snd_nxt, F_ACK, NULL, 0);
	return (long)k;
}

void tcp_close(struct tcp *t)
{
	if (!t)
		return;
	switch (t->state) {
	case LISTEN:
		/* and the connections nobody accepted */
		for (struct tcp *c = conns; c; c = c->next)
			if (c->listener == t && !c->accepted) {
				send_rst(c->rip, c->rport, c->lport, c->snd_nxt, 0, 0);
				dead(c, E_RESET);
				c->user_closed = 1;
			}
		dead(t, 0);
		break;
	case SYN_SENT:
	case SYN_RCVD:
		dead(t, 0);
		break;
	case ESTAB:
		t->state = FIN_WAIT1;
		t->fin_wanted = 1;
		output(t);
		break;
	case CLOSE_WAIT:
		t->state = LAST_ACK;
		t->fin_wanted = 1;
		output(t);
		break;
	}
	t->user_closed = 1;
}

int tcp_peer(struct tcp *t, uint32_t *ip, uint16_t *port)
{
	*ip = t->rip;
	*port = t->rport;
	return 0;
}

void tcp_list(void (*cb)(const char *line, void *arg), void *arg)
{
	char line[96], a[16], l[16];
	for (struct tcp *t = conns; t; t = t->next) {
		ip_fmt(ip_local(), l);
		ip_fmt(t->rip, a);
		if (t->state == LISTEN)
			snprintf(line, sizeof line, "  TCP  %s:%u  listening", l, t->lport);
		else
			snprintf(line, sizeof line, "  TCP  %s:%u  %s:%u  %s", l, t->lport, a, t->rport, state_names[t->state]);
		cb(line, arg);
	}
}

/* ---- DNS --------------------------------------------------------------------------------------- */

static struct {
	char name[64];
	uint32_t ip;
	uint64_t until;
} dns_cache[16];
static int dns_next;

static volatile int dns_got;
static uint32_t dns_answer;
static uint16_t dns_id;

static void dns_input(uint32_t src, uint16_t sport, const uint8_t *d, size_t n, void *arg)
{
	(void)src;
	(void)sport;
	(void)arg;
	if (n < 12 || rd16(d) != dns_id || !(d[2] & 0x80))
		return;
	if ((d[3] & 15) != 0) {		/* NXDOMAIN or another failure */
		dns_answer = 0;
		dns_got = 1;
		return;
	}
	int qd = rd16(d + 4), an = rd16(d + 6);
	size_t i = 12;
	/* skip a name, compressed or not */
	#define SKIP_NAME() do { while (i < n && d[i]) { if ((d[i] & 0xC0) == 0xC0) { i++; break; } i += (size_t)d[i] + 1; } i++; } while (0)
	for (int q = 0; q < qd && i < n; q++) {
		SKIP_NAME();
		i += 4;
	}
	for (int a = 0; a < an && i + 10 <= n; a++) {
		SKIP_NAME();
		if (i + 10 > n)
			break;
		uint16_t type = rd16(d + i), cls = rd16(d + i + 2), rdlen = rd16(d + i + 8);
		i += 10;
		if (type == 1 && cls == 1 && rdlen == 4 && i + 4 <= n) {
			dns_answer = rd32(d + i);
			dns_got = 1;
			return;
		}
		i += rdlen;
	}
	#undef SKIP_NAME
	dns_answer = 0;
	dns_got = 1;
}

int dns_resolve(const char *name, uint32_t *ip)
{
	if (!ip_parse(name, ip))
		return 0;
	uint64_t now = uptime_ms();
	for (int i = 0; i < 16; i++)
		if (dns_cache[i].ip && !strcasecmp(dns_cache[i].name, name) && now < dns_cache[i].until) {
			*ip = dns_cache[i].ip;
			return 0;
		}
	if (!ready())
		return E_NONET;
	uint32_t server = ip_dns();
	if (!server)
		return E_NOHOST;
	/* the query */
	uint8_t q[300];
	size_t n = 12;
	memset(q, 0, 12);
	dns_id = (uint16_t)(uptime_us() ^ 0xB005);
	wr16(q, dns_id);
	q[2] = 0x01;		/* recursion desired */
	wr16(q + 4, 1);
	const char *p = name;
	while (*p) {
		const char *dot = strchr(p, '.');
		size_t l = dot ? (size_t)(dot - p) : strlen(p);
		if (l == 0 || l > 63 || n + l + 6 > sizeof q)
			return E_NOHOST;
		q[n++] = (uint8_t)l;
		memcpy(q + n, p, l);
		n += l;
		p += l + (dot ? 1 : 0);
	}
	q[n++] = 0;
	wr16(q + n, 1);		/* A */
	wr16(q + n + 2, 1);	/* IN */
	n += 4;
	uint16_t port = (uint16_t)(50000 + (dns_id % 10000));
	if (udp_bind(port, dns_input, NULL))
		return E_NOMEM;
	int r = E_TIMEDOUT;
	for (int attempt = 0; attempt < 3 && r == E_TIMEDOUT; attempt++) {
		dns_got = 0;
		ip_udp_send(server, port, 53, q, n);
		uint64_t until = uptime_ms() + 1500;
		while (!dns_got && uptime_ms() < until)
			task_idle();
		if (dns_got)
			r = dns_answer ? 0 : E_NOHOST;
	}
	udp_unbind(port);
	if (r)
		return r;
	*ip = dns_answer;
	strncpy0(dns_cache[dns_next].name, name, sizeof dns_cache[0].name);
	dns_cache[dns_next].ip = dns_answer;
	dns_cache[dns_next].until = uptime_ms() + 300000;
	dns_next = (dns_next + 1) % 16;
	return 0;
}
