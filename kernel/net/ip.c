/*
 * ip.c - just enough TCP/IP-land for bdos: ARP, IPv4, ICMP echo, UDP and a
 * DHCP client.
 *
 * Its main job is to carry BDNET over UDP (port 17979), so machines that
 * don't speak raw BDNET Ethernet - bterm on Linux or macOS, or a bdos box on
 * the other side of a router - can open CTTY, WIN and T: sessions. Each UDP
 * peer gets a made-up "pseudo MAC" (02:BD:55:44:50:nn), so the BDNET code
 * above never needs to know which wire a connection travels on.
 *
 * UDP payload:  "BDNU" ver(1) passlen(1) password  BDNET-frame
 * A bad password is answered with "BDNU" 0xFF 0 and nothing else.
 */
#include "../kernel.h"
#include "../settings.h"
#include "net.h"

#define ET_IP		0x0800
#define ET_ARP		0x0806
#define P_ICMP		1
#define P_UDP		17
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

static struct netif *nif;
static uint32_t my_ip, my_mask, my_gw, my_dns, dhcp_server;
static int use_dhcp, udp_on = 1;
static uint16_t bdnet_port = 17979;
static char password[32];
static uint16_t ip_id = 1;

/* ---- byte order and checksums ---------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static uint16_t csum(const uint8_t *p, size_t n)
{
	uint32_t s = 0;
	for (size_t i = 0; i + 1 < n; i += 2)
		s += (uint32_t)(p[i] << 8 | p[i + 1]);
	if (n & 1)
		s += (uint32_t)p[n - 1] << 8;
	while (s >> 16)
		s = (s & 0xFFFF) + (s >> 16);
	return (uint16_t)~s;
}

void ip_fmt(uint32_t ip, char *out)
{
	snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

int ip_parse(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		if (!isdigit((unsigned char)*s))
			return -1;
		long part = 0;
		while (isdigit((unsigned char)*s))
			part = part * 10 + (*s++ - '0');
		if (part > 255)
			return -1;
		v = v << 8 | (uint32_t)part;
		if (i < 3 && *s++ != '.')
			return -1;
	}
	if (*s && *s != ' ' && *s != ':')
		return -1;
	*out = v;
	return 0;
}

/* ---- ARP ------------------------------------------------------------------------------- */

static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static struct arp_ent {
	uint32_t ip;
	uint8_t mac[6];
	uint64_t asked;		/* when we last asked (0: known) */
	int valid;
} arp[16];

static void eth_send(const uint8_t *dst, uint16_t type, const uint8_t *payload, size_t len)
{
	uint8_t buf[1600];
	if (!nif || len > sizeof buf - 14)
		return;
	memcpy(buf, dst, 6);
	memcpy(buf + 6, nif->mac, 6);
	wr16(buf + 12, type);
	memcpy(buf + 14, payload, len);
	size_t n = 14 + len;
	if (n < 60) {		/* pad to the Ethernet minimum */
		memset(buf + n, 0, 60 - n);
		n = 60;
	}
	nif->send(nif, buf, n);
}

static void arp_send(int op, const uint8_t *tmac, uint32_t tip, const uint8_t *dst)
{
	uint8_t p[28];
	wr16(p, 1);		/* Ethernet */
	wr16(p + 2, ET_IP);
	p[4] = 6;
	p[5] = 4;
	wr16(p + 6, (uint16_t)op);
	memcpy(p + 8, nif->mac, 6);
	wr32(p + 14, my_ip);
	memcpy(p + 18, tmac, 6);
	wr32(p + 24, tip);
	eth_send(dst, ET_ARP, p, sizeof p);
}

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
	if (!ip || ip == 0xFFFFFFFFu)
		return;
	int slot = -1;
	for (int i = 0; i < (int)NELEM(arp); i++)
		if (arp[i].ip == ip) {
			slot = i;
			break;
		}
	if (slot < 0)
		for (int i = 0; i < (int)NELEM(arp); i++)
			if (!arp[i].ip || !arp[i].valid) {
				slot = i;
				break;
			}
	if (slot < 0)
		slot = (int)(uptime_ms() % NELEM(arp));
	arp[slot].ip = ip;
	memcpy(arp[slot].mac, mac, 6);
	arp[slot].valid = 1;
	arp[slot].asked = 0;
}

/* the MAC for ip, or -1 after sending a request (the caller drops the packet;
 * everything above us retries) */
static int arp_lookup(uint32_t ip, uint8_t *mac)
{
	for (int i = 0; i < (int)NELEM(arp); i++)
		if (arp[i].ip == ip && arp[i].valid) {
			memcpy(mac, arp[i].mac, 6);
			return 0;
		}
	struct arp_ent *e = NULL;
	for (int i = 0; i < (int)NELEM(arp) && !e; i++)
		if (arp[i].ip == ip)
			e = &arp[i];
	for (int i = 0; i < (int)NELEM(arp) && !e; i++)
		if (!arp[i].ip)
			e = &arp[i];
	if (!e)
		e = &arp[uptime_ms() % NELEM(arp)];
	uint64_t now = uptime_ms();
	if (e->ip != ip || !e->asked || now - e->asked > 250) {
		e->ip = ip;
		e->valid = 0;
		e->asked = now | 1;
		static const uint8_t zero[6];
		arp_send(1, zero, ip, bcast_mac);
	}
	return -1;
}

/* ---- IPv4 ------------------------------------------------------------------------------- */

static int on_link(uint32_t ip)
{
	return my_mask && (ip & my_mask) == (my_ip & my_mask);
}

static int ip_send(uint32_t dst, int proto, const uint8_t *payload, size_t len)
{
	uint8_t pkt[1500];
	if (!nif || len > sizeof pkt - 20)
		return -1;
	uint8_t mac[6];
	if (dst == 0xFFFFFFFFu || (my_mask && dst == (my_ip | ~my_mask)))
		memcpy(mac, bcast_mac, 6);
	else if (arp_lookup(on_link(dst) || !my_gw ? dst : my_gw, mac))
		return -1;
	memset(pkt, 0, 20);
	pkt[0] = 0x45;
	wr16(pkt + 2, (uint16_t)(20 + len));
	wr16(pkt + 4, ip_id++);
	pkt[8] = 64;
	pkt[9] = (uint8_t)proto;
	wr32(pkt + 12, my_ip);
	wr32(pkt + 16, dst);
	wr16(pkt + 10, csum(pkt, 20));
	memcpy(pkt + 20, payload, len);
	eth_send(mac, ET_IP, pkt, 20 + len);
	return 0;
}

static int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len)
{
	uint8_t u[1480];
	if (len > sizeof u - 8)
		return -1;
	wr16(u, sport);
	wr16(u + 2, dport);
	wr16(u + 4, (uint16_t)(8 + len));
	wr16(u + 6, 0);		/* no checksum: allowed for IPv4 */
	memcpy(u + 8, data, len);
	return ip_send(dst, P_UDP, u, 8 + len);
}

/* ---- ping ------------------------------------------------------------------------------- */

static volatile uint16_t ping_seq_seen;
static volatile int ping_got;
static volatile uint32_t ping_from;

int ip_ping(uint32_t dst, int seq, int timeout_ms)
{
	if (!my_ip)
		return -2;
	uint8_t p[40];
	memset(p, 0, sizeof p);
	p[0] = 8;	/* echo request */
	wr16(p + 4, 0xBD05);
	wr16(p + 6, (uint16_t)seq);
	memcpy(p + 8, "bdos ping, bdos pong.....", 25);
	wr16(p + 2, csum(p, sizeof p));
	ping_got = 0;
	uint64_t start = uptime_ms(), until = start + (uint64_t)timeout_ms, next_try = 0;
	while (uptime_ms() < until) {
		if (uptime_ms() >= next_try) {	/* the first one may wait on ARP */
			int sent = ip_send(dst, P_ICMP, p, sizeof p) == 0;
			next_try = uptime_ms() + (sent ? 1000 : 20);
		}
		if (ping_got && ping_seq_seen == (uint16_t)seq && ping_from == dst)
			return (int)(uptime_ms() - start);
		task_idle();
	}
	return -1;
}

/* ---- BDNET over UDP ------------------------------------------------------------------------ */

static struct upeer {
	uint32_t ip;
	uint16_t port;
	uint64_t seen;
} upeers[16];

static const uint8_t pseudo_prefix[5] = { 0x02, 0xBD, 0x55, 0x44, 0x50 };

int ip_is_pseudo(const uint8_t *mac)
{
	return !memcmp(mac, pseudo_prefix, 5) && mac[5] < NELEM(upeers);
}

int ip_bdnet_send(const uint8_t *mac, const void *frame, size_t len)
{
	struct upeer *p = &upeers[mac[5]];
	if (!p->ip)
		return -1;
	uint8_t buf[1500];
	if (len + 6 > sizeof buf)
		return -1;
	memcpy(buf, "BDNU", 4);
	buf[4] = 1;
	buf[5] = 0;
	memcpy(buf + 6, frame, len);
	return udp_send(p->ip, bdnet_port, p->port, buf, len + 6);
}

static void bdnet_udp_input(uint32_t src, uint16_t sport, const uint8_t *d, size_t n)
{
	if (!udp_on || n < 6 || memcmp(d, "BDNU", 4) || d[4] != 1)
		return;
	size_t pl = d[5];
	if (6 + pl > n)
		return;
	if (password[0] && (pl != strlen(password) || memcmp(d + 6, password, pl))) {
		uint8_t no[6] = { 'B', 'D', 'N', 'U', 0xFF, 0 };
		udp_send(src, bdnet_port, sport, no, sizeof no);
		return;
	}
	const uint8_t *frame = d + 6 + pl;
	size_t flen = n - 6 - pl;
	if (flen < 14 || flen > 1600)
		return;
	/* find or make this peer's pseudo MAC */
	int slot = -1;
	uint64_t oldest = ~0ULL;
	int old = 0;
	for (int i = 0; i < (int)NELEM(upeers); i++) {
		if (upeers[i].ip == src && upeers[i].port == sport) {
			slot = i;
			break;
		}
		if (upeers[i].seen < oldest) {
			oldest = upeers[i].seen;
			old = i;
		}
	}
	if (slot < 0) {
		slot = old;
		upeers[slot].ip = src;
		upeers[slot].port = sport;
	}
	upeers[slot].seen = uptime_ms() + 1;
	uint8_t copy[1600];
	memcpy(copy, frame, flen);
	memcpy(copy, nif->mac, 6);			/* dst: us */
	memcpy(copy + 6, pseudo_prefix, 5);		/* src: the peer's pseudo MAC */
	copy[11] = (uint8_t)slot;
	bdnet_input(copy, (int)flen);
}

int ip_peer_addr(const uint8_t *mac, char *out, size_t n)
{
	if (!ip_is_pseudo(mac) || !upeers[mac[5]].ip)
		return -1;
	char a[16];
	ip_fmt(upeers[mac[5]].ip, a);
	snprintf(out, n, "%s:%u", a, upeers[mac[5]].port);
	return 0;
}

/* ---- DHCP --------------------------------------------------------------------------------- */

enum { D_OFF, D_SELECTING, D_REQUESTING, D_BOUND };
static int dhcp_state;
static uint32_t dhcp_xid, offered_ip;
static uint64_t dhcp_next, lease_end, renew_at;
static int dhcp_tries;

static void dhcp_send(int type)
{
	uint8_t p[300];
	memset(p, 0, sizeof p);
	p[0] = 1;
	p[1] = 1;
	p[2] = 6;
	wr32(p + 4, dhcp_xid);
	wr16(p + 10, 0x8000);	/* please broadcast the answer: we have no address yet */
	if (type == 7)
		wr32(p + 12, my_ip);
	memcpy(p + 28, nif->mac, 6);
	wr32(p + 236, 0x63825363);
	uint8_t *o = p + 240;
	*o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
	if (type == 3) {
		*o++ = 50; *o++ = 4; wr32(o, offered_ip); o += 4;
		*o++ = 54; *o++ = 4; wr32(o, dhcp_server); o += 4;
	}
	if (type == 7) {
		*o++ = 54; *o++ = 4; wr32(o, dhcp_server); o += 4;
	}
	const char *h = net_hostname();
	size_t hl = MIN(strlen(h), (size_t)32);
	if (hl) {
		*o++ = 12; *o++ = (uint8_t)hl;
		memcpy(o, h, hl);
		o += hl;
	}
	*o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
	*o++ = 255;
	uint32_t save = my_ip;
	if (type != 7)
		my_ip = 0;	/* DISCOVER and REQUEST come from 0.0.0.0 */
	udp_send(type == 7 ? dhcp_server : 0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, p,
	    (size_t)(o - p) < 300 ? 300 : (size_t)(o - p));
	my_ip = save;
}

static void dhcp_start(void)
{
	dhcp_state = D_SELECTING;
	dhcp_xid = (uint32_t)(rdtsc() ^ (uptime_ms() << 12));
	dhcp_tries = 0;
	dhcp_next = 0;
}

static void dhcp_input(const uint8_t *p, size_t n)
{
	if (n < 240 || p[0] != 2 || rd32(p + 4) != dhcp_xid || rd32(p + 236) != 0x63825363)
		return;
	int type = 0;
	uint32_t mask = 0, gw = 0, dns = 0, server = 0, lease = 3600;
	for (size_t i = 240; i < n && p[i] != 255; ) {
		uint8_t opt = p[i];
		if (opt == 0) {
			i++;
			continue;
		}
		if (i + 1 >= n)
			break;
		uint8_t l = p[i + 1];
		const uint8_t *v = p + i + 2;
		if (i + 2 + l > n)
			break;
		if (opt == 53 && l >= 1) type = v[0];
		if (opt == 1 && l >= 4) mask = rd32(v);
		if (opt == 3 && l >= 4) gw = rd32(v);
		if (opt == 6 && l >= 4) dns = rd32(v);
		if (opt == 51 && l >= 4) lease = rd32(v);
		if (opt == 54 && l >= 4) server = rd32(v);
		i += 2 + l;
	}
	uint32_t yiaddr = rd32(p + 16);
	if (type == 2 && dhcp_state == D_SELECTING) {		/* OFFER */
		offered_ip = yiaddr;
		dhcp_server = server;
		dhcp_state = D_REQUESTING;
		dhcp_tries = 0;
		dhcp_next = 0;
	} else if (type == 5 && (dhcp_state == D_REQUESTING || dhcp_state == D_BOUND)) {	/* ACK */
		my_ip = yiaddr;
		my_mask = mask ? mask : 0xFFFFFF00u;
		my_gw = gw;
		my_dns = dns;
		if (server)
			dhcp_server = server;
		dhcp_state = D_BOUND;
		if (lease < 60)
			lease = 60;
		lease_end = uptime_ms() + (uint64_t)lease * 1000;
		renew_at = uptime_ms() + (uint64_t)lease * 500;
	} else if (type == 6) {		/* NAK: start over */
		my_ip = 0;
		dhcp_start();
	}
}

/* ---- input ---------------------------------------------------------------------------------- */

static void icmp_input(uint32_t src, const uint8_t *p, size_t n)
{
	if (n < 8)
		return;
	if (p[0] == 8) {		/* echo request: answer it */
		uint8_t r[1480];
		if (n > sizeof r)
			return;
		memcpy(r, p, n);
		r[0] = 0;
		r[2] = r[3] = 0;
		wr16(r + 2, csum(r, n));
		ip_send(src, P_ICMP, r, n);
	} else if (p[0] == 0 && rd16(p + 4) == 0xBD05) {
		ping_seq_seen = rd16(p + 6);
		ping_from = src;
		ping_got = 1;
	}
}

int ip_input(const uint8_t *f, int len)
{
	if (len < 14 || !nif)
		return 0;
	uint16_t type = rd16(f + 12);
	if (type == ET_ARP) {
		if (len < 14 + 28)
			return 1;
		const uint8_t *a = f + 14;
		int op = rd16(a + 6);
		uint32_t sip = rd32(a + 14), tip = rd32(a + 24);
		if (my_ip && (tip == my_ip || on_link(sip)))
			arp_learn(sip, a + 8);
		if (op == 1 && my_ip && tip == my_ip)
			arp_send(2, a + 8, sip, a + 8);
		return 1;
	}
	if (type != ET_IP)
		return 0;
	const uint8_t *ip = f + 14;
	int n = len - 14;
	if (n < 20 || (ip[0] >> 4) != 4)
		return 1;
	int ihl = (ip[0] & 15) * 4;
	int tot = rd16(ip + 2);
	if (ihl < 20 || tot > n || tot < ihl)
		return 1;
	if (rd16(ip + 6) & 0x3FFF)
		return 1;		/* fragments: not for us */
	uint32_t src = rd32(ip + 12), dst = rd32(ip + 16);
	int for_us = dst == my_ip || dst == 0xFFFFFFFFu || (my_mask && dst == (my_ip | ~my_mask)) ||
	    (!my_ip && dhcp_state != D_OFF);
	if (!for_us)
		return 1;
	if (my_ip && on_link(src))
		arp_learn(src, f + 6);
	const uint8_t *p = ip + ihl;
	size_t pl = (size_t)(tot - ihl);
	if (ip[9] == P_ICMP) {
		icmp_input(src, p, pl);
	} else if (ip[9] == P_UDP && pl >= 8) {
		uint16_t sport = rd16(p), dport = rd16(p + 2), ul = rd16(p + 4);
		if (ul < 8 || ul > pl)
			return 1;
		if (dport == DHCP_CLIENT_PORT)
			dhcp_input(p + 8, ul - 8);
		else if (dport == bdnet_port && my_ip)
			bdnet_udp_input(src, sport, p + 8, ul - 8);
	}
	return 1;
}

/* ---- timers, settings ------------------------------------------------------------------------- */

void ip_timers(void)
{
	if (!nif || !use_dhcp)
		return;
	uint64_t now = uptime_ms();
	if (dhcp_state == D_BOUND) {
		if (now >= lease_end) {
			my_ip = 0;
			dhcp_start();
		} else if (now >= renew_at) {
			dhcp_state = D_REQUESTING;
			offered_ip = my_ip;
			dhcp_tries = 0;
			dhcp_next = 0;
		}
		return;
	}
	if (dhcp_state == D_OFF || now < dhcp_next)
		return;
	dhcp_send(dhcp_state == D_SELECTING ? 1 : 3);
	dhcp_tries++;
	if (dhcp_state == D_REQUESTING && dhcp_tries > 4)
		dhcp_start();		/* the server went quiet: start over */
	/* 1, 2, 4, 8 ... up to 30 s between tries */
	uint64_t wait = 1000ULL << MIN(dhcp_tries, 5);
	dhcp_next = now + MIN(wait, 30000ULL);
}

static void apply_settings(const char *key)
{
	if (*key && strncmp(key, "net.", 4))
		return;
	udp_on = setting_int("net.udp") != 0;
	long port = setting_int("net.port");
	bdnet_port = (uint16_t)(port > 0 && port < 65536 ? port : 17979);
	strncpy0(password, setting("net.password"), sizeof password);
	if (*key && strcmp(key, "net.ip") && strcmp(key, "net.mask") && strcmp(key, "net.gateway") &&
	    strcmp(key, "net.dns"))
		return;
	const char *mode = setting("net.ip");
	uint32_t a;
	if (!strcasecmp(mode, "dhcp") || ip_parse(mode, &a)) {
		if (!use_dhcp || !*key) {
			use_dhcp = 1;
			my_ip = 0;
			dhcp_start();
		}
		return;
	}
	use_dhcp = 0;
	dhcp_state = D_OFF;
	my_ip = a;
	uint32_t v;
	my_mask = ip_parse(setting("net.mask"), &v) ? 0xFFFFFF00u : v;
	my_gw = ip_parse(setting("net.gateway"), &v) ? 0 : v;
	my_dns = ip_parse(setting("net.dns"), &v) ? 0 : v;
	memset(arp, 0, sizeof arp);
}

void ip_get(struct ip_info *info)
{
	memset(info, 0, sizeof *info);
	info->ip = my_ip;
	info->mask = my_mask;
	info->gw = my_gw;
	info->dns = my_dns;
	info->server = dhcp_server;
	info->dhcp = use_dhcp;
	info->bound = use_dhcp ? dhcp_state == D_BOUND : my_ip != 0;
	info->lease_left = dhcp_state == D_BOUND && lease_end > uptime_ms() ? (lease_end - uptime_ms()) / 1000 : 0;
	info->udp = udp_on;
	info->port = bdnet_port;
	info->password = password[0] != 0;
}

void ip_renew(void)
{
	if (!use_dhcp)
		return;
	my_ip = 0;
	dhcp_start();
}

void ip_release(void)
{
	if (use_dhcp && dhcp_state == D_BOUND && dhcp_server)
		dhcp_send(7);
	my_ip = 0;
	dhcp_state = D_OFF;
}

void ip_init(struct netif *n)
{
	nif = n;
	setting_watch(apply_settings);
}
