/*
 * net.c - BDNET: discovery and reliable connections over raw Ethernet.
 *
 * Frame: Ethernet header, then
 *   u16 magic 0xBD05, u8 type, u8 flags, u32 conn, u32 seq, u32 ack, u16 len,
 *   payload (up to 1400 bytes).
 * Connections number their DATA frames and acknowledge cumulatively. An
 * ACK may carry an 8-byte bitmap of the 64 frames after the one it asks for
 * that already arrived (a selective ack), so the sender resends just the
 * gaps: straight away when the bitmap shows one, or when the measured
 * round trip says a frame is overdue. Receivers keep frames that arrive
 * early. Peers that don't send bitmaps (older bdos and bterm) still work:
 * their frames are simply resent the old way, all of them.
 *
 * A DGRAM frame is a message of its own, outside the stream: [u8 type]
 * then the payload, never resent and never waited for, and it doesn't queue
 * behind the stream either. Sound goes this way -- late is as bad as lost,
 * and a lost bit of it mustn't hold up the rest. Older peers ignore it.
 */
#include "../kernel.h"
#include "../vfs.h"
#include "net.h"

#define ETHERTYPE	0x88B5
#define MAGIC		0xBD05
#define MAX_PAYLOAD	1400
#define WINDOW		64	/* frames in flight */
#define RTO_MIN		40	/* ms: the retransmit timer, between these */
#define RTO_MAX		2000
#define RTO_INIT	300
#define RX_AHEAD	64	/* early frames kept (one ACK bitmap's worth) */
#define MAX_CONNS	32

enum { F_WHO = 1, F_IAM, F_SYN, F_SYNACK, F_DATA, F_ACK, F_FIN, F_RST, F_DGRAM };

struct hdr {
	uint8_t dst[6], src[6];
	uint16_t ethertype;	/* big endian */
	uint16_t magic;
	uint8_t type, flags;
	uint32_t conn, seq, ack;
	uint16_t len;
} __attribute__((packed));

struct txframe {
	uint32_t seq;
	uint16_t len;
	uint8_t sacked;		/* the peer has it, though earlier ones are missing */
	uint8_t resent;		/* sent more than once: no round-trip sample from it */
	uint64_t sent_at;
	uint8_t data[MAX_PAYLOAD];
};

struct rxframe {
	uint16_t len;		/* 0: empty */
	uint8_t data[MAX_PAYLOAD];
};

struct conn {
	int used, state;	/* 0 free, 1 connecting, 2 open, 3 closed */
	uint32_t id;
	uint8_t peer[6];
	char peer_name[32];
	int svc;
	int initiator;
	/* sending */
	uint8_t *out;		/* bytes not yet put in frames: out[out_off .. out_len) */
	size_t out_off, out_len, out_cap;
	struct txframe win[WINDOW];
	int nwin;		/* frames in flight */
	uint32_t next_seq;	/* seq of the next new frame */
	uint64_t last_send;
	int retries;
	int srtt, rttvar, rto;	/* ms, Jacobson/Karels */
	uint64_t last_fast;	/* when we last resent a gap the bitmap showed */
	/* what the link did, for NETSTAT and tuning */
	uint64_t st_frames, st_bytes, st_resent;
	/* receiving */
	uint32_t expect;	/* next seq we want */
	struct rxframe *ahead;	/* early frames: expect+1 .. expect+RX_AHEAD */
	uint8_t *in;		/* stream bytes not yet parsed into messages */
	size_t in_len, in_cap;
	struct netmsg *q, *q_tail;
	uint64_t last_heard;
	void *user;
	/* SYN payload kept for retransmission */
	uint8_t hello[256];
	size_t hlen;
};

static struct netif *nif;
static struct conn conns[MAX_CONNS];
static char hostname[32];
static service_fn services[128];
static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* name cache, filled by IAM frames */
struct seen {
	char name[32];
	uint8_t mac[6];
	uint64_t when;
};
static struct seen seen[32];
static void (*view_cb)(const char *, const uint8_t *, void *);
static void *view_arg;

static uint16_t be16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }

void net_register(struct netif *n)
{
	if (!nif)
		nif = n;
}

int net_present(void) { return nif != NULL; }
const char *net_adapter(void) { return nif ? nif->name : ""; }
int net_link(void) { return nif && nif->link_up ? nif->link_up(nif) : nif != NULL; }
const char *net_hostname(void) { return hostname; }

void net_mac(uint8_t mac[6])
{
	if (nif)
		memcpy(mac, nif->mac, 6);
	else
		memset(mac, 0, 6);
}

void net_set_hostname(const char *name)
{
	size_t i = 0;
	for (; name[i] && i < sizeof hostname - 1; i++)
		hostname[i] = (char)toupper(name[i]);
	hostname[i] = 0;
}

static void send_frame(const uint8_t *dst, int type, uint32_t conn, uint32_t seq, uint32_t ack,
    const void *payload, size_t len)
{
	if (!nif)
		return;
	uint8_t buf[sizeof(struct hdr) + MAX_PAYLOAD];
	struct hdr *h = (struct hdr *)buf;
	memcpy(h->dst, dst, 6);
	memcpy(h->src, nif->mac, 6);
	h->ethertype = be16(ETHERTYPE);
	h->magic = MAGIC;
	h->type = (uint8_t)type;
	h->flags = 0;
	h->conn = conn;
	h->seq = seq;
	h->ack = ack;
	h->len = (uint16_t)len;
	if (len)
		memcpy(buf + sizeof *h, payload, len);
	if (ip_is_pseudo(dst))
		ip_bdnet_send(dst, buf, sizeof *h + len);	/* a peer reached over UDP */
	else
		nif->send(nif, buf, sizeof *h + len);
}

/* ---- names ---------------------------------------------------------------------- */

static void remember(const char *name, const uint8_t *mac)
{
	int slot = 0;
	uint64_t oldest = ~0ULL;
	for (int i = 0; i < (int)NELEM(seen); i++) {
		if (!strcasecmp(seen[i].name, name)) {
			slot = i;
			break;
		}
		if (seen[i].when < oldest) {
			oldest = seen[i].when;
			slot = i;
		}
	}
	strncpy0(seen[slot].name, name, sizeof seen[slot].name);
	memcpy(seen[slot].mac, mac, 6);
	seen[slot].when = uptime_ms() + 1;
}

int net_resolve(const char *name, uint8_t mac[6])
{
	if (!nif)
		return E_NOTSUP;
	if (!strcasecmp(name, hostname)) {
		memcpy(mac, nif->mac, 6);
		return 0;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		for (int i = 0; i < (int)NELEM(seen); i++)
			if (seen[i].when && !strcasecmp(seen[i].name, name)) {
				memcpy(mac, seen[i].mac, 6);
				return 0;
			}
		send_frame(bcast, F_WHO, 0, 0, 0, name, strlen(name) + 1);
		uint64_t until = uptime_ms() + 400;
		while (uptime_ms() < until) {
			for (int i = 0; i < (int)NELEM(seen); i++)
				if (seen[i].when && !strcasecmp(seen[i].name, name)) {
					memcpy(mac, seen[i].mac, 6);
					return 0;
				}
			task_idle();
		}
	}
	return E_NOENT;
}

void net_view(int ms, void (*cb)(const char *, const uint8_t *, void *), void *arg)
{
	if (!nif)
		return;
	cb(hostname, nif->mac, arg);	/* we don't hear our own broadcast */
	view_cb = cb;
	view_arg = arg;
	send_frame(bcast, F_WHO, 0, 0, 0, "*", 2);
	uint64_t until = uptime_ms() + (uint64_t)ms;
	while (uptime_ms() < until)
		task_idle();
	view_cb = NULL;
}

/* ---- connections ------------------------------------------------------------------ */

static struct conn *find_conn(const uint8_t *peer, uint32_t id)
{
	for (int i = 0; i < MAX_CONNS; i++)
		if (conns[i].used && conns[i].id == id && !memcmp(conns[i].peer, peer, 6))
			return &conns[i];
	return NULL;
}

static struct conn *new_conn(void)
{
	for (int i = 0; i < MAX_CONNS; i++)
		if (!conns[i].used) {
			struct conn *c = &conns[i];
			memset(c, 0, sizeof *c);
			c->used = 1;
			c->rto = RTO_INIT;
			return c;
		}
	return NULL;
}

static void free_conn(struct conn *c)
{
	struct netmsg *m = c->q;
	while (m) {
		struct netmsg *n = m->next;
		kfree(m);
		m = n;
	}
	kfree(c->out);
	kfree(c->in);
	kfree(c->ahead);
	c->used = 0;
}

/* Parse complete messages out of the receive stream. */
static void parse_stream(struct conn *c)
{
	while (c->in_len >= 5) {
		uint32_t len;
		memcpy(&len, c->in, 4);
		if (c->in_len < 5 + (size_t)len)
			return;
		struct netmsg *m = kmalloc(sizeof *m + len + 1);
		if (!m)
			return;
		m->next = NULL;
		m->type = c->in[4];
		m->len = len;
		memcpy(m->data, c->in + 5, len);
		m->data[len] = 0;
		memmove(c->in, c->in + 5 + len, c->in_len - 5 - len);
		c->in_len -= 5 + len;
		if (c->q_tail)
			c->q_tail->next = m;
		else
			c->q = m;
		c->q_tail = m;
	}
}

/* a message for the queue, as the stream's are */
static void queue_msg(struct conn *c, int type, const uint8_t *data, size_t len)
{
	struct netmsg *m = kmalloc(sizeof *m + len + 1);
	if (!m)
		return;
	m->next = NULL;
	m->type = type;
	m->len = (uint32_t)len;
	memcpy(m->data, data, len);
	m->data[len] = 0;
	if (c->q_tail)
		c->q_tail->next = m;
	else
		c->q = m;
	c->q_tail = m;
}

/* a datagram: sent now, once, ahead of anything the stream has waiting */
int conn_send_dgram(struct conn *c, int type, const void *a, size_t alen, const void *b, size_t blen)
{
	if (!c || c->state != 2 || 1 + alen + blen > MAX_PAYLOAD)
		return E_INVAL;
	uint8_t buf[MAX_PAYLOAD];
	buf[0] = (uint8_t)type;
	memcpy(buf + 1, a, alen);
	if (blen)
		memcpy(buf + 1 + alen, b, blen);
	send_frame(c->peer, F_DGRAM, c->id, 0, 0, buf, 1 + alen + blen);
	return 0;
}

static void pump_out(struct conn *c)
{
	/* fill the window with new frames */
	while (c->state == 2 && c->nwin < WINDOW && c->out_len > c->out_off) {
		struct txframe *f = &c->win[c->nwin++];
		size_t n = MIN(c->out_len - c->out_off, (size_t)MAX_PAYLOAD);
		f->seq = c->next_seq++;
		f->len = (uint16_t)n;
		f->sacked = f->resent = 0;
		memcpy(f->data, c->out + c->out_off, n);
		c->out_off += n;
		send_frame(c->peer, F_DATA, c->id, f->seq, c->expect, f->data, n);
		f->sent_at = c->last_send = uptime_ms();
		c->st_frames++;
		c->st_bytes += n;
	}
	if (c->out_off == c->out_len)
		c->out_off = c->out_len = 0;
	else if (c->out_off > 256 * 1024) {	/* now and then, move what's left down */
		memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
		c->out_len -= c->out_off;
		c->out_off = 0;
	}
}

static void resend(struct conn *c, struct txframe *f)
{
	send_frame(c->peer, F_DATA, c->id, f->seq, c->expect, f->data, f->len);
	f->sent_at = c->last_send = uptime_ms();
	f->resent = 1;
	c->st_resent++;
}

static void rtt_sample(struct conn *c, int ms)
{
	if (ms < 1)
		ms = 1;
	if (!c->srtt) {
		c->srtt = ms;
		c->rttvar = ms / 2;
	} else {
		int err = ms - c->srtt;
		c->srtt += err / 8;
		c->rttvar += ((err < 0 ? -err : err) - c->rttvar) / 4;
	}
	c->rto = MAX(RTO_MIN, MIN(RTO_MAX, c->srtt + 4 * MAX(c->rttvar, 2)));
}

/* ack: the peer has everything before it; sack (if have_sack): bit i set
 * means it also has ack+1+i */
static void handle_ack2(struct conn *c, uint32_t ack, uint64_t sack, int have_sack)
{
	uint64_t now = uptime_ms();
	/* drop frames the peer has: everything with seq < ack */
	int drop = 0;
	while (drop < c->nwin && (int32_t)(c->win[drop].seq - ack) < 0)
		drop++;
	if (drop) {
		struct txframe *last = &c->win[drop - 1];
		if (!last->resent)
			rtt_sample(c, (int)(now - last->sent_at));
		memmove(c->win, c->win + drop, sizeof(struct txframe) * (size_t)(c->nwin - drop));
		c->nwin -= drop;
		c->retries = 0;
		c->rto = MAX(RTO_MIN, MIN(RTO_MAX, c->srtt ? c->srtt + 4 * MAX(c->rttvar, 2) : RTO_INIT));
	}
	if (!have_sack || !sack || !c->nwin)
		return;
	/* the bitmap: mark what arrived, and resend the gaps below the highest
	 * that did, at most once a round trip */
	int highest = -1;
	for (int i = 0; i < c->nwin; i++) {
		int32_t d = (int32_t)(c->win[i].seq - ack) - 1;
		if (d >= 0 && d < 64 && (sack >> d) & 1) {
			c->win[i].sacked = 1;
			highest = i;
		}
	}
	uint64_t gap_age = (uint64_t)MAX(c->srtt, 5);
	if (highest > 0 && now - c->last_fast >= gap_age) {
		for (int i = 0; i < highest; i++)
			if (!c->win[i].sacked && now - c->win[i].sent_at >= gap_age)
				resend(c, &c->win[i]);
		c->last_fast = now;
	}
}

static void handle_ack(struct conn *c, uint32_t ack)
{
	handle_ack2(c, ack, 0, 0);
}

/* our ACK: what we have, and a bitmap of what we hold beyond it */
static void send_ack(struct conn *c, const uint8_t *dst)
{
	uint64_t sack = 0;
	if (c->ahead)
		for (int i = 0; i < RX_AHEAD; i++)
			if (c->ahead[i].len)
				sack |= 1ull << i;
	send_frame(dst, F_ACK, c->id, 0, c->expect, &sack, sizeof sack);
}

/* the frame we wanted: in it goes, and any early ones that now follow on */
static int take_in(struct conn *c, const uint8_t *p, size_t len)
{
	if (c->in_len + len > c->in_cap) {
		size_t nc = MAX(c->in_cap * 2, c->in_len + len + 4096);
		uint8_t *nb = krealloc(c->in, nc);
		if (!nb)
			return -1;
		c->in = nb;
		c->in_cap = nc;
	}
	memcpy(c->in + c->in_len, p, len);
	c->in_len += len;
	c->expect++;
	return 0;
}

static void spawn_service(struct conn *c);

static void handle_frame(const uint8_t *buf, int len)
{
	if (len < (int)sizeof(struct hdr))
		return;
	const struct hdr *h = (const struct hdr *)buf;
	if (be16(h->ethertype) != ETHERTYPE || h->magic != MAGIC)
		return;
	if (h->len > len - (int)sizeof *h)
		return;
	const uint8_t *p = buf + sizeof *h;
	struct conn *c;

	switch (h->type) {
	case F_WHO:
		if (h->len && ((p[0] == '*' && !p[1]) || !strncasecmp((const char *)p, hostname, h->len)))
			send_frame(h->src, F_IAM, 0, 0, 0, hostname, strlen(hostname) + 1);
		break;
	case F_IAM: {
		char name[32];
		strncpy0(name, (const char *)p, MIN((size_t)h->len + 1, sizeof name));
		remember(name, h->src);
		if (view_cb)
			view_cb(name, h->src, view_arg);
		break;
	}
	case F_SYN:
		c = find_conn(h->src, h->conn);
		if (!c) {
			if (!h->len || !services[p[0] & 127]) {
				send_frame(h->src, F_RST, h->conn, 0, 0, NULL, 0);
				break;
			}
			c = new_conn();
			if (!c) {
				send_frame(h->src, F_RST, h->conn, 0, 0, NULL, 0);
				break;
			}
			c->id = h->conn;
			memcpy(c->peer, h->src, 6);
			c->svc = p[0];
			c->state = 2;
			c->last_heard = uptime_ms();
			c->hlen = MIN((size_t)h->len - 1, sizeof c->hello);
			memcpy(c->hello, p + 1, c->hlen);
			/* the peer's name, if we know it */
			for (int i = 0; i < (int)NELEM(seen); i++)
				if (seen[i].when && !memcmp(seen[i].mac, h->src, 6))
					strncpy0(c->peer_name, seen[i].name, sizeof c->peer_name);
			if (!c->peer_name[0])	/* bterm over UDP: its address */
				ip_peer_addr(h->src, c->peer_name, sizeof c->peer_name);
			spawn_service(c);
		}
		send_frame(h->src, F_SYNACK, h->conn, 0, 0, NULL, 0);
		break;
	case F_SYNACK:
		c = find_conn(h->src, h->conn);
		if (c && c->state == 1) {
			c->state = 2;
			c->last_heard = uptime_ms();
		}
		break;
	case F_DATA:
		c = find_conn(h->src, h->conn);
		if (!c) {
			send_frame(h->src, F_RST, h->conn, 0, 0, NULL, 0);
			break;
		}
		c->last_heard = uptime_ms();
		handle_ack(c, h->ack);
		if (h->seq == c->expect) {
			if (take_in(c, p, h->len))
				break;
			/* early frames that follow on */
			while (c->ahead && c->ahead[0].len) {
				struct rxframe f0 = c->ahead[0];
				memmove(c->ahead, c->ahead + 1, sizeof(struct rxframe) * (RX_AHEAD - 1));
				c->ahead[RX_AHEAD - 1].len = 0;
				if (take_in(c, f0.data, f0.len))
					break;
			}
			if (c->ahead && !c->ahead[0].len) {
				/* the slots are relative to expect: shift past the gap's slot */
				memmove(c->ahead, c->ahead + 1, sizeof(struct rxframe) * (RX_AHEAD - 1));
				c->ahead[RX_AHEAD - 1].len = 0;
			}
			parse_stream(c);
		} else if ((int32_t)(h->seq - c->expect) > 0 && (int32_t)(h->seq - c->expect) <= RX_AHEAD) {
			/* early: keep it for when the gap is filled */
			if (!c->ahead)
				c->ahead = kzalloc(sizeof(struct rxframe) * RX_AHEAD);
			int k = (int)(h->seq - c->expect) - 1;
			if (c->ahead && h->len) {
				c->ahead[k].len = h->len;
				memcpy(c->ahead[k].data, p, h->len);
			}
		}
		send_ack(c, h->src);
		break;
	case F_ACK:
		c = find_conn(h->src, h->conn);
		if (c) {
			c->last_heard = uptime_ms();
			uint64_t sack = 0;
			if (h->len >= 8)
				memcpy(&sack, p, 8);
			handle_ack2(c, h->ack, sack, h->len >= 8);
		}
		break;
	case F_DGRAM:
		c = find_conn(h->src, h->conn);
		if (c && c->state == 2 && h->len >= 1) {
			c->last_heard = uptime_ms();
			queue_msg(c, p[0], p + 1, h->len - 1u);
		}
		break;
	case F_FIN:
	case F_RST:
		c = find_conn(h->src, h->conn);
		if (c && c->state != 3) {
			c->state = 3;
			if (h->type == F_FIN)
				send_frame(h->src, F_RST, h->conn, 0, 0, NULL, 0);
		}
		break;
	}
}

/* BDNET frames that arrived some other way (UDP, from ip.c) */
void bdnet_input(const uint8_t *buf, int len)
{
	handle_frame(buf, len);
}

static void timers(void)
{
	uint64_t now = uptime_ms();
	for (int i = 0; i < MAX_CONNS; i++) {
		struct conn *c = &conns[i];
		if (!c->used)
			continue;
		if (c->state == 1 && now - c->last_send > 300) {	/* resend SYN */
			uint8_t syn[1 + sizeof c->hello];
			syn[0] = (uint8_t)c->svc;
			memcpy(syn + 1, c->hello, c->hlen);
			send_frame(c->peer, F_SYN, c->id, 0, 0, syn, 1 + c->hlen);
			c->last_send = now;
			if (++c->retries > 15)
				c->state = 3;
		}
		if (c->state != 2)
			continue;
		pump_out(c);
		if (c->nwin && now - c->win[0].sent_at > (uint64_t)c->rto) {
			/* overdue: resend what the peer hasn't said it has, and wait
			 * longer next time (the link may just be slow) */
			for (int k = 0; k < c->nwin; k++)
				if (!c->win[k].sacked && now - c->win[k].sent_at > (uint64_t)c->rto)
					resend(c, &c->win[k]);
			c->rto = MIN(RTO_MAX, c->rto * 2);
			if (++c->retries > 12 && now - c->last_heard > 10000)
				c->state = 3;	/* ten seconds of silence: the peer is gone */
		}
	}
}

/* The network task: receive, dispatch, retransmit. */
static void netd(void *arg)
{
	(void)arg;
	uint8_t *buf = kmalloc(2048);
	for (;;) {
		int busy = 0, n;
		while ((n = nif->recv(nif, buf, 2048)) != 0) {
			if (n > 0 && !ip_input(buf, n))
				handle_frame(buf, n);
			busy = 1;
		}
		timers();
		ip_timers();
		if (busy)
			task_yield();
		else
			task_idle();
	}
}

struct conn *conn_connect(const char *host, int svc, const void *hello, size_t hlen, int *err)
{
	if (!nif) {
		*err = E_NOTSUP;
		return NULL;
	}
	uint8_t mac[6];
	int r = net_resolve(host, mac);
	if (r) {
		*err = r;
		return NULL;
	}
	struct conn *c = new_conn();
	if (!c) {
		*err = E_NOMEM;
		return NULL;
	}
	c->id = (uint32_t)(rdtsc() ^ (uptime_ms() << 16)) | 1;
	memcpy(c->peer, mac, 6);
	strncpy0(c->peer_name, host, sizeof c->peer_name);
	for (char *p = c->peer_name; *p; p++)
		*p = (char)toupper(*p);
	c->svc = svc;
	c->initiator = 1;
	c->state = 1;
	c->hlen = MIN(hlen, sizeof c->hello);
	memcpy(c->hello, hello, c->hlen);
	uint8_t syn[1 + sizeof c->hello];
	syn[0] = (uint8_t)svc;
	memcpy(syn + 1, c->hello, c->hlen);
	send_frame(mac, F_SYN, c->id, 0, 0, syn, 1 + c->hlen);
	c->last_send = uptime_ms();
	while (c->state == 1)
		task_idle();
	if (c->state != 2) {
		free_conn(c);
		*err = E_IO;
		return NULL;
	}
	return c;
}

int conn_send2(struct conn *c, int type, const void *a, size_t alen, const void *b, size_t blen)
{
	if (c->state != 2)
		return E_IO;
	size_t need = c->out_len + 5 + alen + blen;
	if (need > c->out_cap) {
		size_t nc = MAX(c->out_cap * 2, need + 4096);
		uint8_t *nb = krealloc(c->out, nc);
		if (!nb)
			return E_NOMEM;
		c->out = nb;
		c->out_cap = nc;
	}
	uint32_t len = (uint32_t)(alen + blen);
	memcpy(c->out + c->out_len, &len, 4);
	c->out[c->out_len + 4] = (uint8_t)type;
	if (alen)
		memcpy(c->out + c->out_len + 5, a, alen);
	if (blen)
		memcpy(c->out + c->out_len + 5 + alen, b, blen);
	c->out_len += 5 + alen + blen;
	pump_out(c);
	/* don't let one sender run unboundedly ahead of the wire */
	while (c->state == 2 && c->out_len - c->out_off > 512 * 1024)
		task_idle();
	return 0;
}

/* bytes queued or in flight: a sender with choices (the screen) waits for
 * this to come down rather than piling more on */
size_t conn_backlog(struct conn *c)
{
	return c->state == 2 ? c->out_len - c->out_off + (size_t)c->nwin * MAX_PAYLOAD : 0;
}

/* NETSTAT: the BDNET connections, how they're doing */
void bdnet_list(void (*fn)(const char *line, void *arg), void *arg)
{
	for (int i = 0; i < MAX_CONNS; i++) {
		struct conn *c = &conns[i];
		if (!c->used || c->state != 2)
			continue;
		char line[160];
		snprintf(line, sizeof line, "  BDNET  %-8s %s \\\\%-16s  round trip %d ms, %llu KB sent, %llu of %llu frames resent",
		    c->svc == 'T' ? "shell" : c->svc == 'D' ? "desktop" : c->svc == 'F' ? "files" : "other", c->initiator ? "to  " : "from",
		    c->peer_name[0] ? c->peer_name : "?", c->srtt, (unsigned long long)(c->st_bytes / 1024),
		    (unsigned long long)c->st_resent, (unsigned long long)c->st_frames);
		fn(line, arg);
	}
}

/* frames sent, bytes sent, frames resent, round trip (ms) */
void conn_stats(struct conn *c, uint64_t *frames, uint64_t *bytes, uint64_t *resent, int *rtt)
{
	*frames = c->st_frames;
	*bytes = c->st_bytes;
	*resent = c->st_resent;
	*rtt = c->srtt;
}

int conn_send(struct conn *c, int type, const void *data, size_t len)
{
	return conn_send2(c, type, data, len, NULL, 0);
}

struct netmsg *conn_take(struct conn *c, uint32_t mask)
{
	struct netmsg **pp = &c->q, *prev = NULL;
	while (*pp) {
		struct netmsg *m = *pp;
		if (mask & (1u << (m->type & 31))) {
			*pp = m->next;
			if (c->q_tail == m)
				c->q_tail = prev;
			m->next = NULL;
			return m;
		}
		prev = m;
		pp = &m->next;
	}
	return NULL;
}

struct netmsg *conn_wait(struct conn *c, uint32_t mask, int ms)
{
	uint64_t until = uptime_ms() + (uint64_t)(ms < 0 ? 0 : ms);
	for (;;) {
		struct netmsg *m = conn_take(c, mask);
		if (m)
			return m;
		if (c->state != 2)
			return NULL;
		if (ms >= 0 && uptime_ms() >= until)
			return NULL;
		task_idle();
	}
}

int conn_alive(struct conn *c)
{
	return c->state == 2;
}

void conn_close(struct conn *c)
{
	if (c->state == 2) {
		/* let queued data drain briefly, then say goodbye */
		uint64_t until = uptime_ms() + 1000;
		while ((c->out_len || c->nwin) && c->state == 2 && uptime_ms() < until)
			task_idle();
		send_frame(c->peer, F_FIN, c->id, 0, 0, NULL, 0);
	}
	free_conn(c);
}

const char *conn_peer(struct conn *c) { return c->peer_name[0] ? c->peer_name : "?"; }
void *conn_user(struct conn *c) { return c->user; }
void conn_set_user(struct conn *c, void *u) { c->user = u; }

void net_service(int svc, service_fn fn)
{
	services[svc & 127] = fn;
}

static void service_task(void *arg)
{
	struct conn *c = arg;
	services[c->svc & 127](c, c->hello, c->hlen);
}

static void spawn_service(struct conn *c)
{
	char name[TASK_NAME_MAX];
	snprintf(name, sizeof name, "net-%c-%s", c->svc, c->peer_name[0] ? c->peer_name : "peer");
	/* services start with no console of their own and C:\ as home */
	struct task *t = task_spawn(name, service_task, c, NULL);
	if (t) {
		t->con = NULL;
		t->desk = NULL;
		strcpy(t->cwd, "C:\\");
	}
}

/* ---- setup ------------------------------------------------------------------------ */

void net_init(void)
{
	extern void e1000_init(void);
	extern void virtio_net_init(void);
	e1000_init();
	if (!nif)
		virtio_net_init();
	if (!nif)
		return;
	/* hostname: C:\BDOS\HOSTNAME.TXT, else BDOS-xxxxxx from the MAC */
	int err;
	size_t n;
	char *name = vfs_slurp("C:\\BDOS\\HOSTNAME.TXT", &n, &err);
	if (name) {
		char *e = name;
		while (*e && *e != '\r' && *e != '\n' && *e != ' ')
			e++;
		*e = 0;
		if (*name)
			net_set_hostname(name);
		kfree(name);
	}
	if (!hostname[0])
		snprintf(hostname, sizeof hostname, "BDOS-%02X%02X%02X", nif->mac[3], nif->mac[4],
		    nif->mac[5]);
	ip_init(nif);
	task_spawn("netd", netd, NULL, NULL);
	kprintf("Network: %s %02x:%02x:%02x:%02x:%02x:%02x, this machine is \\\\%s\n", nif->name,
	    nif->mac[0], nif->mac[1], nif->mac[2], nif->mac[3], nif->mac[4], nif->mac[5], hostname);
}
