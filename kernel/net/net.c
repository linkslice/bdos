/*
 * net.c - BDNET: discovery and reliable connections over raw Ethernet.
 *
 * Frame: Ethernet header, then
 *   u16 magic 0xBD05, u8 type, u8 flags, u32 conn, u32 seq, u32 ack, u16 len,
 *   payload (up to 1400 bytes).
 * Connections number their DATA frames and acknowledge cumulatively; the
 * sender keeps up to WINDOW frames in flight and resends them all if an
 * acknowledgement is late (go-back-N). On a LAN that's plenty.
 */
#include "../kernel.h"
#include "../vfs.h"
#include "net.h"

#define ETHERTYPE	0x88B5
#define MAGIC		0xBD05
#define MAX_PAYLOAD	1400
#define WINDOW		32
#define RTO_MS		200
#define MAX_CONNS	32

enum { F_WHO = 1, F_IAM, F_SYN, F_SYNACK, F_DATA, F_ACK, F_FIN, F_RST };

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
	uint8_t *out;		/* bytes not yet put in frames */
	size_t out_len, out_cap;
	struct txframe win[WINDOW];
	int nwin;		/* frames in flight */
	uint32_t next_seq;	/* seq of the next new frame */
	uint64_t last_send;
	int retries;
	/* receiving */
	uint32_t expect;	/* next seq we want */
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

static void pump_out(struct conn *c)
{
	/* fill the window with new frames */
	while (c->state == 2 && c->nwin < WINDOW && c->out_len) {
		struct txframe *f = &c->win[c->nwin++];
		size_t n = MIN(c->out_len, (size_t)MAX_PAYLOAD);
		f->seq = c->next_seq++;
		f->len = (uint16_t)n;
		memcpy(f->data, c->out, n);
		memmove(c->out, c->out + n, c->out_len - n);
		c->out_len -= n;
		send_frame(c->peer, F_DATA, c->id, f->seq, c->expect, f->data, n);
		c->last_send = uptime_ms();
	}
}

static void handle_ack(struct conn *c, uint32_t ack)
{
	/* drop frames the peer has: everything with seq < ack */
	int drop = 0;
	while (drop < c->nwin && (int32_t)(c->win[drop].seq - ack) < 0)
		drop++;
	if (drop) {
		memmove(c->win, c->win + drop, sizeof(struct txframe) * (size_t)(c->nwin - drop));
		c->nwin -= drop;
		c->retries = 0;
		c->last_send = uptime_ms();
	}
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
			if (c->in_len + h->len > c->in_cap) {
				size_t nc = MAX(c->in_cap * 2, c->in_len + h->len + 4096);
				uint8_t *nb = krealloc(c->in, nc);
				if (!nb)
					break;
				c->in = nb;
				c->in_cap = nc;
			}
			memcpy(c->in + c->in_len, p, h->len);
			c->in_len += h->len;
			c->expect++;
			parse_stream(c);
		}
		send_frame(h->src, F_ACK, h->conn, 0, c->expect, NULL, 0);
		break;
	case F_ACK:
		c = find_conn(h->src, h->conn);
		if (c) {
			c->last_heard = uptime_ms();
			handle_ack(c, h->ack);
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
		if (c->nwin && now - c->last_send > RTO_MS) {
			/* go back N: resend everything in flight */
			for (int k = 0; k < c->nwin; k++)
				send_frame(c->peer, F_DATA, c->id, c->win[k].seq, c->expect,
				    c->win[k].data, c->win[k].len);
			c->last_send = now;
			if (++c->retries > 50)
				c->state = 3;	/* ~10 s of silence: the peer is gone */
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
			if (n > 0)
				handle_frame(buf, n);
			busy = 1;
		}
		timers();
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
	while (c->state == 2 && c->out_len > 512 * 1024)
		task_idle();
	return 0;
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
	e1000_init();
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
	task_spawn("netd", netd, NULL, NULL);
	kprintf("Network: %s %02x:%02x:%02x:%02x:%02x:%02x, this machine is \\\\%s\n", nif->name,
	    nif->mac[0], nif->mac[1], nif->mac[2], nif->mac[3], nif->mac[4], nif->mac[5], hostname);
}
